#include "tracking_cameras.hpp"

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "fd_protocol.hpp"

namespace framepip {
namespace {

constexpr double kStalledAfter = 0.25;   // s without a frame: check slowly
constexpr double kPausedAfter = 0.5;     // s without a frame: report paused
constexpr double kGroupWait = 0.025;     // s to wait for the other cameras' frame of the same moment
constexpr double kAfterLong = 0.006;     // s from a long exposure to just before the short one (~8.4 ms later)
constexpr double kAfterShort = 0.018;    // s from a short exposure to just before the next long one (21.6-25 ms)
constexpr int kPitchFramesNeeded = 4;    // textured frames that must agree before a row pitch is trusted

double Now() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

void SleepFor(double s) {
  if (s <= 0) return;
  timespec t{(time_t)s, (long)((s - (time_t)s) * 1e9)};
  nanosleep(&t, nullptr);
}

// CPU access to a dmabuf the camera writes behind the caches
void CpuAccess(int fd, bool begin) {
  dma_buf_sync sync{};
  sync.flags = DMA_BUF_SYNC_READ | (begin ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END);
  ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

}  // namespace

const char *CameraStatusText(CameraStatus s) {
  switch (s) {
    case CameraStatus::Starting: return "Starting cameras";
    case CameraStatus::NoTracking: return "Waiting for headset tracking";
    case CameraStatus::NoDriver: return "Camera driver not loaded - restart SteamVR";
    case CameraStatus::Failed: return "Cameras unavailable";
    case CameraStatus::Paused: return "Cameras paused (headset idle)";
    case CameraStatus::Streaming: return "Camera live";
  }
  return "";
}

// One tracking camera: its V4L2 node (for buffer state) and its lent buffers (for pixels).
class TrackingCameras::Camera {
 public:
  enum class Result { Nothing, NewFrame, Lost };

  Camera(const LentCamera &lent, const int *fds) : index_(lent.index), node_(lent.node) {
    for (int i = 0; i < lent.bufferCount; i++) buffers_.push_back(Buffer{lent.xrFds[i], fds[i]});
  }

  ~Camera() {
    for (Buffer &b : buffers_) {
      if (b.map) munmap(b.map, b.length);
      if (b.fd >= 0) close(b.fd);
    }
    if (node_fd_ >= 0) close(node_fd_);
  }

  // Opens the node and maps the buffers. Returns an error, or "" when ready.
  std::string Setup() {
    node_fd_ = open(node_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (node_fd_ < 0) return "can't open " + node_ + ": " + strerror(errno);
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(node_fd_, VIDIOC_G_FMT, &fmt)) return node_ + ": VIDIOC_G_FMT: " + strerror(errno);
    width_ = (int)fmt.fmt.pix_mp.width;
    height_ = (int)fmt.fmt.pix_mp.height;
    if (width_ <= 0 || height_ <= 0 || width_ > 4096 || height_ > 4096) return node_ + ": unexpected frame size";
    size_t smallest = SIZE_MAX;
    uint32_t newest = 0;
    for (size_t i = 0; i < buffers_.size(); i++) {
      v4l2_buffer b;
      v4l2_plane planes[VIDEO_MAX_PLANES];
      if (!Query((int)i, &b, planes) || planes[0].m.fd != buffers_[i].xrFd) return node_ + ": buffers changed";
      buffers_[i].length = planes[0].length;
      buffers_[i].map = (uint8_t *)mmap(nullptr, buffers_[i].length, PROT_READ, MAP_SHARED, buffers_[i].fd, 0);
      if (buffers_[i].map == MAP_FAILED) {
        buffers_[i].map = nullptr;
        return node_ + ": mmap: " + strerror(errno);
      }
      smallest = std::min(smallest, buffers_[i].length);
      newest = std::max(newest, b.sequence);
    }
    lastSequence_ = newest;  // only frames captured from now on
    if (!PlanPitches(fmt.fmt.pix_mp.plane_fmt[0].bytesperline, smallest)) return node_ + ": buffers too small";
    frame.width = width_;
    frame.height = height_;
    return "";
  }

  double NextCheck() const { return nextCheck_; }

  // Looks for new frames; keeps the newest long exposure in `frame` (setting `fresh`).
  Result Check(double now) {
    if (now < nextCheck_) return Result::Nothing;
    struct Ready {
      int buffer;
      uint32_t sequence;
      double timestamp;
    };
    std::vector<Ready> ready;
    for (size_t i = 0; i < buffers_.size(); i++) {
      v4l2_buffer b;
      v4l2_plane planes[VIDEO_MAX_PLANES];
      if (!Query((int)i, &b, planes) || planes[0].m.fd != buffers_[i].xrFd) return Result::Lost;  // XRService restarted
      // filled and handed to XRService (not queued for the camera), and newer than anything seen
      if (!(b.flags & V4L2_BUF_FLAG_QUEUED) && b.sequence > lastSequence_)
        ready.push_back({(int)i, b.sequence, b.timestamp.tv_sec + b.timestamp.tv_usec * 1e-6});
    }
    std::sort(ready.begin(), ready.end(), [](const Ready &a, const Ready &b) { return a.sequence < b.sequence; });

    bool accepted = false, newestWasLong = false;
    for (const Ready &r : ready) {
      lastSequence_ = r.sequence;
      lastSeen_ = now;
      const Buffer &buf = buffers_[r.buffer];
      CpuAccess(buf.fd, true);
      const float mean = Brightness(buf.map);
      newestWasLong = IsLongExposure(r.sequence, mean);
      if (!newestWasLong) {
        CpuAccess(buf.fd, false);
        continue;
      }
      if (!pitch_) {  // the row pitch isn't known yet: this frame only helps find it
        LearnPitch(buf.map);
        CpuAccess(buf.fd, false);
        continue;
      }
      scratch_.resize((size_t)width_ * height_);
      for (int y = 0; y < height_; y++) memcpy(&scratch_[(size_t)y * width_], buf.map + (size_t)y * pitch_, width_);
      CpuAccess(buf.fd, false);
      // the camera may have been given the buffer back while we copied: then the copy may be torn
      v4l2_buffer after;
      v4l2_plane planes[VIDEO_MAX_PLANES];
      if (!Query(r.buffer, &after, planes) || (after.flags & V4L2_BUF_FLAG_QUEUED) || after.sequence != r.sequence) continue;
      frame.pixels.swap(scratch_);
      frame.sequence = r.sequence;
      frame.timestamp = r.timestamp;
      frame.mean = mean;
      fresh = true;
      accepted = true;
    }
    if (accepted) lastArrival = now;
    // Wake just before the next frame of either kind, so the short exposures stay in view too: IsLongExposure
    // tells the two apart by comparing neighbouring frames.
    if (ready.empty())
      nextCheck_ = now + (lastSeen_ > 0 && now - lastSeen_ < kStalledAfter ? 0.001 : 0.02);
    else
      nextCheck_ = now + (newestWasLong ? kAfterLong : kAfterShort);
    return accepted ? Result::NewFrame : Result::Nothing;
  }

  int Index() const { return index_; }

  CameraImage frame;      // newest verified long exposure
  bool fresh = false;     // `frame` hasn't been published yet
  double lastArrival = 0; // when `frame` was taken (CLOCK_MONOTONIC s)

 private:
  struct Buffer {
    int xrFd;  // XRService's number for it, to notice when the buffers are replaced
    int fd;    // ours
    uint8_t *map = nullptr;
    size_t length = 0;
  };

  bool Query(int i, v4l2_buffer *b, v4l2_plane *planes) const {
    memset(b, 0, sizeof *b);
    memset(planes, 0, sizeof(v4l2_plane) * VIDEO_MAX_PLANES);
    b->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b->index = (uint32_t)i;
    b->m.planes = planes;
    b->length = VIDEO_MAX_PLANES;
    return ioctl(node_fd_, VIDIOC_QUERYBUF, b) == 0;
  }

  // The cameras strictly alternate a long exposure with a short one, so long exposures share one sequence parity.
  // Which parity is decided by brightness: a frame clearly brighter than the frame just before it is a long
  // exposure, a clearly darker one a short exposure. Frames too alike to tell (a pitch-dark room) follow the
  // parity last decided. XRService sometimes changes its exposure timing, which flips the parity; the next pair of
  // frames shows it straight away. The frame it changes on is half exposed: a long exposure much darker than the
  // recent ones is skipped, unless several come in a row (the room really did get darker).
  bool IsLongExposure(uint32_t sequence, float mean) {
    const bool followsPrevious = previousMean_ >= 0 && sequence == previousSequence_ + 1;
    const float before = previousMean_;
    previousSequence_ = sequence;
    previousMean_ = mean;
    bool isLong;
    if (followsPrevious && mean >= before * 1.5f + 2) {
      isLong = true;
      longParity_ = (int)(sequence & 1);
    } else if (followsPrevious && before >= mean * 1.5f + 2) {
      isLong = false;
      longParity_ = (int)(~sequence & 1);
    } else if (longParity_ >= 0) {
      isLong = (int)(sequence & 1) == longParity_;
    } else {
      return false;  // nothing to tell them apart by yet
    }
    if (!isLong) return false;
    if (recentLongMean_ > 0 && mean < 0.5f * recentLongMean_ && ++dimLongRun_ < 3) return false;
    dimLongRun_ = 0;
    recentLongMean_ = recentLongMean_ > 0 ? recentLongMean_ + (mean - recentLongMean_) * 0.2f : mean;
    return true;
  }

  float Brightness(const uint8_t *p) const {
    const int pitch = pitch_ ? pitch_ : pitchCandidates_.front();
    uint64_t sum = 0, count = 0;
    const int dy = std::max(1, height_ / 32), dx = std::max(1, width_ / 32);  // about 1000 samples
    for (int y = dy / 2; y < height_; y += dy)
      for (int x = dx / 2; x < width_; x += dx) sum += p[(size_t)y * pitch + x], count++;
    return count ? (float)sum / count : 0.f;
  }

  // The row pitch the cameras write isn't always V4L2's bytesperline (the forward cameras can write 1152-byte rows
  // while reporting 1056). Every plausible pitch is a candidate: what V4L2 says, the frame width, and every multiple
  // of 32 up to what the buffers can hold.
  bool PlanPitches(uint32_t reported, size_t bufferLength) {
    const int widest = (int)std::min<size_t>(bufferLength / height_, (size_t)width_ + 512);
    if (widest < width_) return false;
    std::vector<int> c = {width_};
    if ((int)reported >= width_ && (int)reported <= widest) c.push_back((int)reported);
    for (int p = (width_ + 31) / 32 * 32; p <= widest; p += 32) c.push_back(p);
    std::sort(c.begin(), c.end());
    c.erase(std::unique(c.begin(), c.end()), c.end());
    pitchCandidates_ = c;
    pitchScores_.assign(c.size(), 0);
    if (c.size() == 1) pitch_ = c[0];
    return true;
  }

  // Read at the right pitch, each row continues the one above it; at a wrong pitch rows are sheared and differ
  // more. Scores (mean difference between neighbouring rows) add up over textured frames; the lowest total wins.
  // Only rows every candidate reads inside the luma plane count (beyond it lies the chroma plane, flat in these
  // monochrome cameras, which would look deceptively smooth).
  void LearnPitch(const uint8_t *p) {
    const int widest = pitchCandidates_.back();
    const int rows = (int)((size_t)width_ * height_ / widest) - 1;
    if (rows < 8) {
      pitch_ = width_;
      return;
    }
    std::vector<double> scores;
    for (int pitch : pitchCandidates_) {
      uint64_t diff = 0, count = 0;
      for (int y = 0; y + 1 < rows; y += 3)
        for (int x = 0; x < width_; x += 3, count++) diff += std::abs(p[(size_t)y * pitch + x] - p[(size_t)(y + 1) * pitch + x]);
      scores.push_back(count ? (double)diff / count : 0);
    }
    std::vector<double> sorted = scores;
    std::sort(sorted.begin(), sorted.end());
    if (sorted.size() > 1 && !(sorted[1] > sorted[0] * 1.25 && sorted[1] - sorted[0] > 1.5)) return;  // too flat to tell
    for (size_t i = 0; i < scores.size(); i++) pitchScores_[i] += scores[i];
    if (++pitchFrames_ < kPitchFramesNeeded) return;
    const size_t best = std::min_element(pitchScores_.begin(), pitchScores_.end()) - pitchScores_.begin();
    pitch_ = pitchCandidates_[best];
    fprintf(stderr, "cameras: %s writes rows %d bytes apart\n", node_.c_str(), pitch_);
  }

  int index_;
  std::string node_;
  int node_fd_ = -1;
  int width_ = 0, height_ = 0;
  std::vector<Buffer> buffers_;
  uint32_t lastSequence_ = 0;
  double lastSeen_ = 0, nextCheck_ = 0;
  uint32_t previousSequence_ = 0;
  float previousMean_ = -1, recentLongMean_ = 0;
  int longParity_ = -1, dimLongRun_ = 0;
  int pitch_ = 0, pitchFrames_ = 0;
  std::vector<int> pitchCandidates_;
  std::vector<double> pitchScores_;
  std::vector<uint8_t> scratch_;
};

TrackingCameras::TrackingCameras(std::vector<int> indices) : indices_(std::move(indices)) {}

TrackingCameras::~TrackingCameras() { Stop(); }

void TrackingCameras::Start() {
  if (run_.exchange(true)) return;
  thread_ = std::thread([this] { Run(); });
}

void TrackingCameras::Stop() {
  if (!run_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
}

std::string TrackingCameras::Detail() const {
  std::lock_guard<std::mutex> l(mutex_);
  return detail_;
}

bool TrackingCameras::Latest(uint64_t *serial, std::vector<CameraImage> *out) {
  std::lock_guard<std::mutex> l(mutex_);
  if (publishedSerial_ == 0 || publishedSerial_ == *serial) return false;
  *out = published_;
  *serial = publishedSerial_;
  return true;
}

void TrackingCameras::SetStatus(CameraStatus s, const std::string &detail) {
  std::lock_guard<std::mutex> l(mutex_);
  if (status_.load() != s || detail_ != detail)
    fprintf(stderr, "cameras: %s%s%s\n", CameraStatusText(s), detail.empty() ? "" : ": ", detail.c_str());
  status_ = s;
  detail_ = detail;
}

void TrackingCameras::Disconnect() {
  cameras_.clear();
  if (xrPidFd_ >= 0) close(xrPidFd_);
  xrPidFd_ = -1;
}

// Borrows the cameras' buffers from driver_framepip and sets the cameras up.
bool TrackingCameras::Connect() {
  Disconnect();
  BufferRequest req;
  for (int index : indices_)
    if (index >= 0 && index < kMaxCameras) req.cameraMask |= 1u << index;

  int sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof addr.sun_path, "%s", LendSocketPath().c_str());
  if (sock < 0 || connect(sock, (sockaddr *)&addr, sizeof addr) || send(sock, &req, sizeof req, MSG_NOSIGNAL) != sizeof req) {
    if (sock >= 0) close(sock);
    SetStatus(CameraStatus::NoDriver, "");
    return false;
  }
  BufferReply reply;
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * kMaxLentFds)];
  iovec io{&reply, sizeof reply};
  msghdr msg{};
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof control;
  pollfd pf{sock, POLLIN, 0};
  const ssize_t got = poll(&pf, 1, 3000) == 1 ? recvmsg(sock, &msg, MSG_CMSG_CLOEXEC) : -1;
  close(sock);

  std::vector<int> fds;
  for (cmsghdr *c = got > 0 ? CMSG_FIRSTHDR(&msg) : nullptr; c; c = CMSG_NXTHDR(&msg, c))
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      const int n = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
      fds.insert(fds.end(), (int *)CMSG_DATA(c), (int *)CMSG_DATA(c) + n);
    }
  auto fail = [&](CameraStatus s, const std::string &why) {
    for (int fd : fds) close(fd);
    SetStatus(s, why);
    return false;
  };
  if (got != (ssize_t)sizeof reply || reply.magic != kProtocolMagic || reply.version != kProtocolVersion)
    return fail(CameraStatus::NoDriver, "no answer from the driver");
  switch (reply.status) {
    case LendStatus::Ok: break;
    case LendStatus::NoXrService:
    case LendStatus::NoCameraNodes:
    case LendStatus::NoBuffers: return fail(CameraStatus::NoTracking, LendStatusText(reply.status));
    default: return fail(CameraStatus::Failed, std::string("driver: ") + LendStatusText(reply.status));
  }
  size_t expected = 0;
  for (int k = 0; k < reply.cameraCount && k < kMaxCameras; k++) expected += (size_t)reply.cameras[k].bufferCount;
  if (reply.cameraCount != (int)indices_.size() || fds.size() != expected)
    return fail(CameraStatus::Failed, "driver lent an unexpected set of buffers");

  // each camera owns its fds from here on, so Disconnect() releases everything
  size_t next = 0;
  for (int index : indices_) {
    LentCamera *lent = nullptr;
    for (int k = 0; k < reply.cameraCount; k++)
      if (reply.cameras[k].index == index) lent = &reply.cameras[k];
    if (!lent) return fail(CameraStatus::Failed, "driver left out camera " + std::to_string(index));
    lent->node[sizeof lent->node - 1] = 0;
    cameras_.push_back(std::make_unique<Camera>(*lent, fds.data() + next));
    next += (size_t)lent->bufferCount;
  }
  for (size_t i = next; i < fds.size(); i++) close(fds[i]);
  for (auto &cam : cameras_) {
    const std::string err = cam->Setup();
    if (!err.empty()) {
      SetStatus(CameraStatus::Failed, err);
      return false;
    }
  }
  xrPidFd_ = (int)syscall(SYS_pidfd_open, reply.xrPid, 0);
  SetStatus(CameraStatus::Starting, "");
  return true;
}

void TrackingCameras::Run() {
  double retryAt = 0, connectedAt = 0, groupStarted = 0;
  while (run_) {
    const double now = Now();
    if (cameras_.empty()) {
      if (now < retryAt || !Connect()) {
        if (now >= retryAt) {
          Disconnect();
          retryAt = now + 2.0;
        }
        SleepFor(std::min(0.1, std::max(0.0, retryAt - now)));
        continue;
      }
      connectedAt = now;
    }
    pollfd pf{xrPidFd_, POLLIN, 0};
    if (xrPidFd_ >= 0 && poll(&pf, 1, 0) == 1) {  // XRService exited
      SetStatus(CameraStatus::NoTracking, "XRService restarted");
      Disconnect();
      retryAt = now + 1.0;
      continue;
    }
    bool lost = false;
    for (auto &cam : cameras_) lost = lost || cam->Check(now) == Camera::Result::Lost;
    if (lost) {
      SetStatus(CameraStatus::NoTracking, "camera buffers changed");
      Disconnect();
      retryAt = now + 1.0;
      continue;
    }

    // publish the cameras' frames together: when all have a new one, or shortly after the first did
    bool all = true, any = false, everyCameraHasOne = true;
    double newest = 0;
    for (auto &cam : cameras_) {
      all = all && cam->fresh;
      any = any || cam->fresh;
      everyCameraHasOne = everyCameraHasOne && cam->frame.sequence != 0;
      newest = std::max(newest, cam->lastArrival);
    }
    if (any && !groupStarted) groupStarted = now;
    if (everyCameraHasOne && (all || (any && now - groupStarted > kGroupWait))) {
      std::lock_guard<std::mutex> l(mutex_);
      published_.resize(cameras_.size());
      for (size_t k = 0; k < cameras_.size(); k++) {
        Camera &cam = *cameras_[k];
        if (!cam.fresh) continue;  // its last frame is already published
        CameraImage &out = published_[k];
        out.pixels.swap(cam.frame.pixels);  // no copy; the camera gets a spare buffer back
        out.width = cam.frame.width;
        out.height = cam.frame.height;
        out.sequence = cam.frame.sequence;
        out.timestamp = cam.frame.timestamp;
        out.mean = cam.frame.mean;
        cam.fresh = false;
      }
      publishedSerial_++;
      groupStarted = 0;
    }

    const bool streaming = newest > 0 && now - newest < kPausedAfter;
    if (streaming && status_ != CameraStatus::Streaming) SetStatus(CameraStatus::Streaming, "");
    if (!streaming && (status_ == CameraStatus::Streaming || (status_ == CameraStatus::Starting && now - connectedAt > 2.0)))
      SetStatus(CameraStatus::Paused, "");

    double wake = now + 0.02;
    for (auto &cam : cameras_) wake = std::min(wake, cam->NextCheck());
    SleepFor(wake - Now());
  }
  Disconnect();
}

}  // namespace framepip
