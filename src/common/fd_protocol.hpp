// How driver_framepip (inside vrserver) lends the framepip app the tracking cameras' capture buffers.
//
// XRService (the Steam Frame's tracking) owns the cameras and captures into dmabufs. The kernel only lets an
// ancestor duplicate another process's fds (Yama ptrace_scope 1), and XRService runs as a child of vrserver, so the
// driver loaded into vrserver does the duplicating. It decides itself which buffers it lends: the capture buffers
// of the camera indices asked for, found from XRService's own log and the cameras' V4L2 queues.
//
// Transport: a SOCK_SEQPACKET unix socket in the user's runtime directory. The app sends one BufferRequest; the
// driver answers with one BufferReply, plus (when status is Ok) the duplicated fds as SCM_RIGHTS: camera by camera
// in the order of BufferReply::cameras, each camera's buffers in V4L2 index order.
#pragma once

#include <unistd.h>

#include <cstdint>
#include <string>

namespace framepip {

constexpr uint32_t kProtocolMagic = 0x50495046;  // "FPIP"
constexpr uint32_t kProtocolVersion = 2;
constexpr int kMaxCameras = 4;
constexpr int kMaxBuffersPerCamera = 16;
constexpr int kMaxLentFds = kMaxCameras * kMaxBuffersPerCamera;

enum class LendStatus : int32_t {
  Ok = 0,
  BadRequest,       // wrong magic/version/size, or no cameras asked for
  NotAllowed,       // the caller isn't the installed framepip app
  NoXrService,      // vrserver has no XRService child
  NoCameraNodes,    // XRService hasn't logged a node for a requested camera
  NoBuffers,        // a requested camera has no capture buffers (not streaming yet)
  DuplicateFailed,  // pidfd_getfd failed
};

struct BufferRequest {
  uint32_t magic = kProtocolMagic;
  uint32_t version = kProtocolVersion;
  uint32_t cameraMask = 0;  // bit i: XRService camera index i
};

struct LentCamera {
  int32_t index = -1;                         // XRService camera index
  int32_t bufferCount = 0;                    // fds that follow for this camera
  int32_t xrFds[kMaxBuffersPerCamera] = {};   // XRService's own fd numbers, as VIDIOC_QUERYBUF reports them
  char node[32] = {};                         // the camera's V4L2 node, e.g. "/dev/video0"
};

struct BufferReply {
  uint32_t magic = kProtocolMagic;
  uint32_t version = kProtocolVersion;
  LendStatus status = LendStatus::BadRequest;
  int32_t xrPid = 0;
  int32_t cameraCount = 0;
  LentCamera cameras[kMaxCameras];
};

inline const char *LendStatusText(LendStatus s) {
  switch (s) {
    case LendStatus::Ok: return "ok";
    case LendStatus::BadRequest: return "bad request";
    case LendStatus::NotAllowed: return "only the installed framepip app may borrow camera buffers";
    case LendStatus::NoXrService: return "XRService isn't running";
    case LendStatus::NoCameraNodes: return "XRService hasn't logged its camera nodes";
    case LendStatus::NoBuffers: return "the cameras have no capture buffers yet";
    case LendStatus::DuplicateFailed: return "couldn't duplicate XRService's buffers";
  }
  return "unknown";
}

// Always /run/user/<uid>: Desktop Mode's nested Plasma session points XDG_RUNTIME_DIR somewhere else.
inline std::string RuntimeDir() { return "/run/user/" + std::to_string(getuid()) + "/framepip"; }
inline std::string LendSocketPath() { return RuntimeDir() + "/cameras.sock"; }

}  // namespace framepip
