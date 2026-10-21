// Picking the PiP up with the controllers when it's placed in local space.
//
// Hold grip while a controller points at the PiP (or touches it) and the PiP follows that controller until grip
// is let go. Gripping with the other controller as well resizes it: pulling the controllers apart grows it.
#pragma once

#include "input.hpp"
#include "openvr.h"

namespace framepip {

// Rigid 3x4 transforms (OpenVR layout: rotation columns, translation in column 3).
vr::HmdMatrix34_t Multiply(const vr::HmdMatrix34_t &a, const vr::HmdMatrix34_t &b);
vr::HmdMatrix34_t InverseRigid(const vr::HmdMatrix34_t &m);
// A pose `distance` metres ahead of the head along its gaze, upright and facing the eye.
vr::HmdMatrix34_t InFrontOf(const vr::HmdMatrix34_t &head, double distance);

class RoomGrab {
 public:
  struct Window {
    vr::HmdMatrix34_t pose;  // standing space
    double width, height;    // metres
  };
  enum class Change { None, Moved, Released };

  // Call every loop, after input.Update(). grabbable: the PiP is in local space, shown and not locked. While held,
  // moves *w (and resizes it within [minWidth, maxWidth], keeping its aspect).
  Change Update(const Input &input, bool grabbable, Window *w, double minWidth, double maxWidth);

  // A controller points at or touches the PiP, or holds it.
  bool Aimed() const { return aimed_; }
  bool Holding() const { return holder_ >= 0; }

 private:
  struct Hand {
    bool gripping = false, tracked = false, onWindow = false;
    vr::HmdMatrix34_t pose{};
  };

  void Buzz(const Input &input, int hand);
  void Hold(int hand, const Window &w);

  Hand hands_[2];  // left, right
  int holder_ = -1;              // hand holding the PiP
  int resizer_ = -1;             // other hand, gripping to resize
  vr::HmdMatrix34_t offset_{};   // PiP pose relative to the holding hand, at widthAtHold_
  double widthAtHold_ = 0;
  double resizeDistance_ = 0, resizeWidth_ = 0;  // hand spacing and width when resizing began
  bool aimed_ = false;
};

}  // namespace framepip
