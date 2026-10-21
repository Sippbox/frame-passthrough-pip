// Reads the Steam Frame's tracking-camera frames without disturbing XRService, which owns the cameras.
//
// driver_framepip lends this process XRService's capture buffers (src/common/fd_protocol.hpp). Which buffer holds
// the newest frame comes from VIDIOC_QUERYBUF on each camera's own V4L2 node: any process may query a queue.
// The cameras alternate a long exposure (the image) with a short one (for controller LEDs, nearly black); only the
// long exposures are kept.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace framepip {

struct CameraImage {
  int width = 0, height = 0;
  std::vector<uint8_t> pixels;  // packed 8-bit luma, width * height
  uint32_t sequence = 0;
  double timestamp = 0;  // V4L2 timestamp, seconds
  float mean = 0;        // sampled mean luma, 0..255
};

enum class CameraStatus {
  Starting,     // nothing tried yet
  NoTracking,   // XRService isn't running, or its cameras aren't set up yet
  NoDriver,     // driver_framepip isn't answering: SteamVR hasn't loaded it
  Failed,       // the cameras or their buffers aren't what we expect
  Paused,       // set up, but no frames (headset asleep / not tracking)
  Streaming,
};

class TrackingCameras {
 public:
  // indices: XRService camera indices (= the factory calibration's camera indices) to read.
  explicit TrackingCameras(std::vector<int> indices);
  ~TrackingCameras();
  TrackingCameras(const TrackingCameras &) = delete;
  TrackingCameras &operator=(const TrackingCameras &) = delete;

  void Start();
  void Stop();

  CameraStatus Status() const { return status_.load(); }
  std::string Detail() const;

  // If a frame set newer than *serial has been published, copies it into *out (one image per index, in order),
  // updates *serial and returns true.
  bool Latest(uint64_t *serial, std::vector<CameraImage> *out);

 private:
  class Camera;
  void Run();
  bool Connect();
  void Disconnect();
  void SetStatus(CameraStatus s, const std::string &detail);

  std::vector<int> indices_;
  std::vector<std::unique_ptr<Camera>> cameras_;
  int xrPidFd_ = -1;  // readable once XRService exits

  std::thread thread_;
  std::atomic<bool> run_{false};
  std::atomic<CameraStatus> status_{CameraStatus::Starting};

  mutable std::mutex mutex_;  // guards everything below
  std::string detail_;
  std::vector<CameraImage> published_;
  uint64_t publishedSerial_ = 0;
};

const char *CameraStatusText(CameraStatus s);

}  // namespace framepip
