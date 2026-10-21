// driver_framepip: a device-less SteamVR driver, loaded into the Steam Frame's own vrserver, that lends the framepip
// app the tracking cameras' capture buffers (protocol: src/common/fd_protocol.hpp).
//
// It lends narrowly:
//   - only to the installed framepip binary: the caller's /proc/<pid>/exe must be <install root>/bin/framepip, the
//     install root being where this library itself lives;
//   - only capture buffers of tracking cameras, which it finds itself (XRService's log names each camera's node,
//     VIDIOC_QUERYBUF on that node lists the buffers). The app never names fds.
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include "fd_protocol.hpp"
#include "openvr_driver.h"
#include "xrservice.hpp"

namespace {

using framepip::BufferReply;
using framepip::BufferRequest;
using framepip::LendStatus;

void Log(const char *fmt, ...) {
  if (!vr::VRDriverLog()) return;
  char line[512] = "";
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + strlen(line), sizeof line - strlen(line), fmt, ap);
  va_end(ap);
  vr::VRDriverLog()->Log(line);
}

// This library sits at <root>/driver/framepip/bin/linuxarm64/driver_framepip.so; the app at <root>/bin/framepip.
std::string InstalledAppPath() {
  Dl_info info{};
  if (!dladdr((void *)&InstalledAppPath, &info) || !info.dli_fname) return "";
  std::string root = info.dli_fname;
  for (const char *expected : {"driver_framepip.so", "linuxarm64", "bin", "framepip", "driver"}) {
    const size_t slash = root.rfind('/');
    if (slash == std::string::npos || root.compare(slash + 1, std::string::npos, expected) != 0) return "";
    root.resize(slash);
  }
  char real[PATH_MAX];
  return realpath((root + "/bin/framepip").c_str(), real) ? real : "";
}

// The executable of the process on the other end of a unix socket.
std::string PeerExecutable(int conn) {
  ucred cred{};
  socklen_t len = sizeof cred;
  if (getsockopt(conn, SOL_SOCKET, SO_PEERCRED, &cred, &len) || cred.uid != getuid()) return "";
  char exe[PATH_MAX];
  const ssize_t n = readlink(("/proc/" + std::to_string(cred.pid) + "/exe").c_str(), exe, sizeof exe);
  return n > 0 ? std::string(exe, (size_t)n) : "";
}

void Send(int conn, const BufferReply &reply, const std::vector<int> &fds) {
  iovec io{(void *)&reply, sizeof reply};
  msghdr msg{};
  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * framepip::kMaxLentFds)];
  if (!fds.empty()) {
    msg.msg_control = control;
    msg.msg_controllen = CMSG_SPACE(sizeof(int) * fds.size());
    cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
    memcpy(CMSG_DATA(c), fds.data(), sizeof(int) * fds.size());
  }
  sendmsg(conn, &msg, MSG_NOSIGNAL);
}

// Builds the reply for one request; on success *fds holds our duplicates of XRService's buffers (caller closes them).
BufferReply Lend(const BufferRequest &req, std::vector<int> *fds) {
  BufferReply reply;
  if (req.magic != framepip::kProtocolMagic || req.version != framepip::kProtocolVersion || !req.cameraMask ||
      (req.cameraMask >> framepip::kMaxCameras)) {
    reply.status = LendStatus::BadRequest;
    return reply;
  }
  const int xr = framepip::FindXrService(getpid());
  if (!xr) {
    reply.status = LendStatus::NoXrService;
    return reply;
  }
  reply.xrPid = xr;
  const std::vector<std::string> nodes = framepip::XrServiceCameraNodes(xr);
  for (int index = 0; index < framepip::kMaxCameras; index++) {
    if (!(req.cameraMask >> index & 1)) continue;
    if (index >= (int)nodes.size() || nodes[index].empty() || nodes[index].size() >= sizeof reply.cameras[0].node) {
      reply.status = LendStatus::NoCameraNodes;
      return reply;
    }
    const std::vector<int> xrFds = framepip::QueueBufferFds(nodes[index], framepip::kMaxBuffersPerCamera);
    if (xrFds.empty()) {
      reply.status = LendStatus::NoBuffers;
      return reply;
    }
    framepip::LentCamera &cam = reply.cameras[reply.cameraCount++];
    cam.index = index;
    cam.bufferCount = (int32_t)xrFds.size();
    std::copy(xrFds.begin(), xrFds.end(), cam.xrFds);
    strcpy(cam.node, nodes[index].c_str());
  }
  const int pidfd = (int)syscall(SYS_pidfd_open, xr, 0);
  reply.status = LendStatus::Ok;
  for (int k = 0; k < reply.cameraCount && reply.status == LendStatus::Ok; k++)
    for (int b = 0; b < reply.cameras[k].bufferCount; b++) {
      const int dup = pidfd < 0 ? -1 : (int)syscall(SYS_pidfd_getfd, pidfd, reply.cameras[k].xrFds[b], 0);
      if (dup < 0) {
        reply.status = LendStatus::DuplicateFailed;
        break;
      }
      fds->push_back(dup);
    }
  if (pidfd >= 0) close(pidfd);
  if (reply.status != LendStatus::Ok) {
    for (int fd : *fds) close(fd);
    fds->clear();
  }
  return reply;
}

class Lender {
 public:
  bool Start(const std::string &app) {
    app_ = app;
    mkdir(framepip::RuntimeDir().c_str(), 0700);
    path_ = framepip::LendSocketPath();
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path_.size() >= sizeof addr.sun_path) return false;
    strcpy(addr.sun_path, path_.c_str());
    unlink(path_.c_str());
    listen_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    wake_ = eventfd(0, EFD_CLOEXEC);
    epoll_ = epoll_create1(EPOLL_CLOEXEC);
    if (listen_ < 0 || wake_ < 0 || epoll_ < 0 || bind(listen_, (sockaddr *)&addr, sizeof addr) || listen(listen_, 4)) {
      Log("can't listen on %s: %s", path_.c_str(), strerror(errno));
      Stop();
      return false;
    }
    chmod(path_.c_str(), 0600);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listen_;
    epoll_ctl(epoll_, EPOLL_CTL_ADD, listen_, &ev);
    ev.data.fd = wake_;
    epoll_ctl(epoll_, EPOLL_CTL_ADD, wake_, &ev);
    thread_ = std::thread([this] { Loop(); });
    return true;
  }

  void Stop() {
    if (thread_.joinable()) {
      const uint64_t one = 1;
      (void)!write(wake_, &one, sizeof one);
      thread_.join();
    }
    for (int *fd : {&listen_, &wake_, &epoll_})
      if (*fd >= 0) close(*fd), *fd = -1;
    if (!path_.empty()) unlink(path_.c_str());
  }

  const std::string &Path() const { return path_; }

 private:
  void Loop() {
    for (;;) {
      epoll_event ev;
      if (epoll_wait(epoll_, &ev, 1, -1) != 1) continue;
      if (ev.data.fd == wake_) return;
      const int conn = accept4(listen_, nullptr, nullptr, SOCK_CLOEXEC);
      if (conn < 0) continue;
      Serve(conn);
      close(conn);
    }
  }

  void Serve(int conn) {
    if (PeerExecutable(conn) != app_) {
      BufferReply refusal;
      refusal.status = LendStatus::NotAllowed;
      Send(conn, refusal, {});
      Log("refused camera buffers to a process that isn't %s", app_.c_str());
      return;
    }
    const timeval timeout{2, 0};  // one message, or give up
    setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    BufferRequest req;
    if (recv(conn, &req, sizeof req, 0) != (ssize_t)sizeof req) return;
    std::vector<int> fds;
    const BufferReply reply = Lend(req, &fds);
    Send(conn, reply, fds);
    for (int fd : fds) close(fd);
  }

  std::string app_, path_;
  int listen_ = -1, wake_ = -1, epoll_ = -1;
  std::thread thread_;
};

class Provider : public vr::IServerTrackedDeviceProvider {
 public:
  vr::EVRInitError Init(vr::IVRDriverContext *ctx) override {
    VR_INIT_SERVER_DRIVER_CONTEXT(ctx);
    const std::string app = InstalledAppPath();
    if (app.empty()) Log("not inside a framepip install (no bin/framepip beside the driver); not lending");
    else if (lender_.Start(app)) Log("lending tracking-camera buffers to %s on %s", app.c_str(), lender_.Path().c_str());
    return vr::VRInitError_None;
  }
  void Cleanup() override {
    lender_.Stop();
    VR_CLEANUP_SERVER_DRIVER_CONTEXT();
  }
  const char *const *GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
  void RunFrame() override {}
  bool ShouldBlockStandbyMode() override { return false; }
  void EnterStandby() override {}
  void LeaveStandby() override {}

 private:
  Lender lender_;
};

Provider g_provider;

}  // namespace

extern "C" __attribute__((visibility("default"))) void *HmdDriverFactory(const char *iface, int *code) {
  if (!strcmp(iface, vr::IServerTrackedDeviceProvider_Version)) return &g_provider;
  if (code) *code = vr::VRInitError_Init_InterfaceNotFound;
  return nullptr;
}
