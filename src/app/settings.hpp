// User settings, persisted in ~/.config/framepip/settings.ini.
#pragma once

#include <string>

namespace framepip {

struct Settings {
  bool enabled = true;
  double yawDeg = 24;      // PiP centre, degrees right of straight ahead
  double pitchDeg = -18;   // PiP centre, degrees above straight ahead
  double sizeDeg = 26;     // PiP width as seen from the eye, degrees
  double opacity = 1.0;    // 0.1 .. 1
  double tintHue = 120;    // degrees on the colour wheel
  double tintStrength = 0; // 0 = no tint .. 1 = fully tinted

  // Room placement: instead of following the view, the PiP stays put in the room (standing space) and can be
  // picked up with a controller.
  bool roomMode = false;
  double roomWidth = kRoomWidthDefault;  // metres
  bool roomLocked = false;       // grabbing disabled
  bool rememberRoomPose = false; // restore roomPose at startup (otherwise the PiP starts in front of you)
  bool hasRoomPose = false;
  float roomPose[3][4] = {};     // PiP pose in standing space (OpenVR 3x4, rows)

  // The PiP's centre is limited so the whole PiP stays within +-kYawMax / +-kPitchMax of straight ahead.
  static constexpr double kYawMax = 40, kPitchMax = 35;
  static constexpr double kSizeMin = 10, kSizeMax = 60;
  static constexpr double kRoomWidthMin = 0.15, kRoomWidthMax = 2.5, kRoomWidthDefault = 0.6;
  static constexpr double kOpacityMin = 0.1, kOpacityMax = 1;
  static constexpr double kAspect = 0.75;  // PiP height / width

  double YawLimit() const { return kYawMax - sizeDeg / 2; }
  double PitchLimit() const { return kPitchMax - sizeDeg * kAspect / 2; }
  void Clamp();
  // RGB multiplier for the overlay: white blended toward the tint hue by tintStrength.
  void TintColor(float *r, float *g, float *b) const;
};

std::string SettingsPath();
Settings LoadSettings();
bool SaveSettings(const Settings &s);

// Full-saturation, full-value colour for a hue in degrees.
void HueToRgb(double hueDeg, float *r, float *g, float *b);

}  // namespace framepip
