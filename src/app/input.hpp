// framepip's SteamVR Input actions (share/input/actions.json):
//   grab, aim, haptic   picking the PiP up in local space (bound to grip and the pointing pose by default)
//   toggle              show / hide the PiP (double-tap View on the left controller by default)
#pragma once

#include <string>

#include "openvr.h"

namespace framepip {

class Input {
 public:
  // Loads the action manifest. False (with *error) when SteamVR Input can't be used.
  bool Init(const std::string &manifestPath, std::string *error);
  bool Ready() const { return ready_; }

  // Refreshes the action state; call before reading actions.
  void Update();

  // Opens SteamVR's controller binding editor for this app, in the headset.
  bool OpenBindings(std::string *error);

  // True once per press of the toggle action, after the Update that saw it.
  bool TakeToggle() {
    const bool t = toggled_;
    toggled_ = false;
    return t;
  }

  vr::VRActionHandle_t grab = vr::k_ulInvalidActionHandle, aim = vr::k_ulInvalidActionHandle,
                       haptic = vr::k_ulInvalidActionHandle, toggle = vr::k_ulInvalidActionHandle;
  vr::VRInputValueHandle_t hands[2] = {vr::k_ulInvalidInputValueHandle, vr::k_ulInvalidInputValueHandle};  // left, right

 private:
  bool ready_ = false;
  vr::VRActionSetHandle_t set_ = vr::k_ulInvalidActionSetHandle;
  bool toggleDown_ = false, toggled_ = false;
};

}  // namespace framepip
