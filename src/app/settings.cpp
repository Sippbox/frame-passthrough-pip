#include "settings.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace framepip {

void Settings::Clamp() {
  sizeDeg = std::clamp(sizeDeg, kSizeMin, kSizeMax);
  roomWidth = std::clamp(roomWidth, kRoomWidthMin, kRoomWidthMax);
  yawDeg = std::clamp(yawDeg, -YawLimit(), YawLimit());
  pitchDeg = std::clamp(pitchDeg, -PitchLimit(), PitchLimit());
  opacity = std::clamp(opacity, kOpacityMin, kOpacityMax);
  tintHue = std::fmod(std::fmod(tintHue, 360.0) + 360.0, 360.0);
  tintStrength = std::clamp(tintStrength, 0.0, 1.0);
}

void HueToRgb(double hueDeg, float *r, float *g, float *b) {
  const double h = std::fmod(std::fmod(hueDeg, 360.0) + 360.0, 360.0) / 60.0;
  const double x = 1 - std::fabs(std::fmod(h, 2.0) - 1);
  double rr = 0, gg = 0, bb = 0;
  switch ((int)h) {
    case 0: rr = 1, gg = x; break;
    case 1: rr = x, gg = 1; break;
    case 2: gg = 1, bb = x; break;
    case 3: gg = x, bb = 1; break;
    case 4: rr = x, bb = 1; break;
    default: rr = 1, bb = x; break;
  }
  *r = (float)rr, *g = (float)gg, *b = (float)bb;
}

void Settings::TintColor(float *r, float *g, float *b) const {
  float hr, hg, hb;
  HueToRgb(tintHue, &hr, &hg, &hb);
  const float s = (float)tintStrength;
  *r = 1 + (hr - 1) * s;
  *g = 1 + (hg - 1) * s;
  *b = 1 + (hb - 1) * s;
}

std::string SettingsPath() {
  const char *xdg = getenv("XDG_CONFIG_HOME");
  const char *home = getenv("HOME");
  std::string base = xdg && *xdg ? xdg : std::string(home ? home : "/tmp") + "/.config";
  return base + "/framepip/settings.ini";
}

Settings LoadSettings() {
  Settings s;
  std::ifstream f(SettingsPath());
  std::string line;
  while (std::getline(f, line)) {
    const size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    auto trim = [](std::string &t) {
      t.erase(0, t.find_first_not_of(" \t"));
      t.erase(t.find_last_not_of(" \t\r") + 1);
    };
    trim(key);
    trim(value);
    char *end = nullptr;
    const double v = strtod(value.c_str(), &end);
    const bool num = end && end != value.c_str() && std::isfinite(v);  // nan/inf would poison the overlay pose
    if (key == "enabled") s.enabled = value == "true" || value == "1";
    else if (key == "room_mode") s.roomMode = value == "true" || value == "1";
    else if (key == "room_locked") s.roomLocked = value == "true" || value == "1";
    else if (key == "remember_room_pose") s.rememberRoomPose = value == "true" || value == "1";
    else if (key == "room_pose") {
      float m[12];
      int n = 0;
      std::istringstream in(value);
      for (std::string part; n < 12 && std::getline(in, part, ',');) {
        char *e = nullptr;
        const double x = strtod(part.c_str(), &e);
        if (e == part.c_str() || !std::isfinite(x) || std::fabs(x) > 1000) break;
        m[n++] = (float)x;
      }
      if (n == 12) {
        memcpy(s.roomPose, m, sizeof m);
        s.hasRoomPose = true;
      }
    } else if (!num) continue;
    else if (key == "yaw") s.yawDeg = v;
    else if (key == "pitch") s.pitchDeg = v;
    else if (key == "size") s.sizeDeg = v;
    else if (key == "opacity") s.opacity = v;
    else if (key == "tint_hue") s.tintHue = v;
    else if (key == "tint_strength") s.tintStrength = v;
    else if (key == "room_width") s.roomWidth = v;
  }
  s.Clamp();
  return s;
}

bool SaveSettings(const Settings &s) {
  const std::string path = SettingsPath();
  const std::string dir = path.substr(0, path.rfind('/'));
  mkdir(dir.substr(0, dir.rfind('/')).c_str(), 0755);
  mkdir(dir.c_str(), 0755);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return false;
    f << "# Passthrough PiP settings (edited from the SteamVR dashboard)\n";
    f << "enabled = " << (s.enabled ? "true" : "false") << "\n";
    f << "yaw = " << s.yawDeg << "\n";
    f << "pitch = " << s.pitchDeg << "\n";
    f << "size = " << s.sizeDeg << "\n";
    f << "opacity = " << s.opacity << "\n";
    f << "tint_hue = " << s.tintHue << "\n";
    f << "tint_strength = " << s.tintStrength << "\n";
    f << "room_mode = " << (s.roomMode ? "true" : "false") << "\n";
    f << "room_width = " << s.roomWidth << "\n";
    f << "room_locked = " << (s.roomLocked ? "true" : "false") << "\n";
    f << "remember_room_pose = " << (s.rememberRoomPose ? "true" : "false") << "\n";
    if (s.rememberRoomPose && s.hasRoomPose) {  // otherwise each session starts with the PiP in front of you
      f << "room_pose = ";
      for (int i = 0; i < 12; i++) f << (i ? "," : "") << s.roomPose[i / 4][i % 4];
      f << "\n";
    }
    if (!f.good()) return false;
  }
  return rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace framepip
