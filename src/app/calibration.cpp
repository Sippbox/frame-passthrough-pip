#include "calibration.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>

#include "nlohmann/json.hpp"

namespace framepip {
namespace {

using nlohmann::json;

Vec3 ReadVec3(const json &j) { return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()}; }

Vec3 Cross(const Vec3 &a, const Vec3 &b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

double Dot(const Vec3 &a, const Vec3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

Vec3 Normalized(const Vec3 &v) {
  const double n = std::sqrt(Dot(v, v));
  return n > 0 ? Vec3{v[0] / n, v[1] / n, v[2] / n} : v;
}

}  // namespace

std::vector<CameraModel> LoadCalibration(const std::string &path, std::string *error) {
  std::vector<CameraModel> out;
  try {
    std::ifstream f(path);
    if (!f) {
      *error = "can't read " + path;
      return out;
    }
    const json root = json::parse(f);
    // "cad" places the head and the cameras in one CAD frame. Head axes in that frame -> a change of basis.
    const json &cad = root.at("cad");
    const Vec3 hx = Normalized(ReadVec3(cad.at("__head").at("plus_x")));
    const Vec3 hz = Normalized(ReadVec3(cad.at("__head").at("plus_z")));
    const Vec3 hy = Cross(hz, hx);
    auto toHead = [&](const Vec3 &v) { return Vec3{Dot(v, hx), Dot(v, hy), Dot(v, hz)}; };

    const json &cams = root.at("cameras");
    const json &cadCams = cad.at("cameras");
    for (size_t i = 0; i < cams.size() && i < cadCams.size(); i++) {
      const json &c = cams[i];
      const json *kb = nullptr;
      for (const json &in : c.at("intrinsics"))
        if (in.value("cameraModel", "") == "kb") kb = &in;
      if (!kb) continue;
      CameraModel m;
      m.index = (int)i;
      m.name = c.value("sourceCamera", "");
      m.width = c.at("width").get<int>();
      m.height = c.at("height").get<int>();
      m.fx = kb->at("fx").get<double>();
      m.fy = kb->at("fy").get<double>();
      m.cx = kb->at("cx").get<double>();
      m.cy = kb->at("cy").get<double>();
      for (int k = 0; k < 4; k++) m.k[k] = kb->value("k" + std::to_string(k + 1), 0.0);
      const double circle = c.value("imageCircleDiameterInDeg", 170.0);
      m.maxTheta = std::min(circle / 2 - 2, 88.0) * M_PI / 180;
      // camera axes in CAD; y = z x x makes (x right, y down, z forward) right-handed
      const Vec3 x = Normalized(ReadVec3(cadCams[i].at("plus_x")));
      const Vec3 z = Normalized(ReadVec3(cadCams[i].at("plus_z")));
      const Vec3 y = Cross(z, x);
      m.axisX = toHead(x);
      m.axisY = toHead(y);
      m.axisZ = toHead(z);
      out.push_back(m);
    }
    if (out.empty()) *error = "no usable cameras in " + path;
  } catch (const std::exception &e) {
    *error = std::string("bad calibration: ") + e.what();
    out.clear();
  }
  return out;
}

std::vector<CameraModel> ForwardCameras(const std::vector<CameraModel> &all) {
  std::vector<CameraModel> sorted = all;
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const CameraModel &a, const CameraModel &b) { return a.width * a.height > b.width * b.height; });
  if (sorted.size() > 2) sorted.resize(2);
  std::sort(sorted.begin(), sorted.end(), [](const CameraModel &a, const CameraModel &b) { return a.index < b.index; });
  return sorted;
}

}  // namespace framepip
