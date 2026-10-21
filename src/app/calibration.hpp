// The Steam Frame's factory camera calibration (/persist/xrservice.json): per-camera Kannala-Brandt fisheye
// intrinsics and each camera's orientation relative to the head.
#pragma once

#include <array>
#include <string>
#include <vector>

namespace framepip {

using Vec3 = std::array<double, 3>;

struct CameraModel {
  int index = 0;  // XRService / calibration camera index
  std::string name;  // sourceCamera, e.g. "slam_left"
  int width = 0, height = 0;
  double fx = 0, fy = 0, cx = 0, cy = 0;
  double k[4] = {};
  double maxTheta = 0;  // radians from the optical axis that the lens images
  // camera axes (OpenCV convention: x right, y down, z forward) in OpenVR head space (x right, y up, z back)
  Vec3 axisX{}, axisY{}, axisZ{};
};

constexpr const char *kCalibrationPath = "/persist/xrservice.json";

// Loads every camera with a "kb" intrinsic model and a CAD orientation. Empty on failure; *error says why.
std::vector<CameraModel> LoadCalibration(const std::string &path, std::string *error);

// The cameras for the forward view: the two highest-resolution ones (the forward-facing passthrough pair).
std::vector<CameraModel> ForwardCameras(const std::vector<CameraModel> &all);

}  // namespace framepip
