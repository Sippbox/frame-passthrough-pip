#include "input.hpp"

namespace framepip {

bool Input::Init(const std::string &manifestPath, std::string *error) {
  vr::IVRInput *in = vr::VRInput();
  if (!in) {
    *error = "SteamVR Input unavailable";
    return false;
  }
  vr::EVRInputError e = in->SetActionManifestPath(manifestPath.c_str());
  if (e == vr::VRInputError_None) e = in->GetActionSetHandle("/actions/pip", &set_);
  if (e == vr::VRInputError_None) e = in->GetActionHandle("/actions/pip/in/grab", &grab);
  if (e == vr::VRInputError_None) e = in->GetActionHandle("/actions/pip/in/aim", &aim);
  if (e == vr::VRInputError_None) e = in->GetActionHandle("/actions/pip/out/haptic", &haptic);
  if (e == vr::VRInputError_None) e = in->GetActionHandle("/actions/pip/in/toggle", &toggle);
  if (e == vr::VRInputError_None) e = in->GetInputSourceHandle("/user/hand/left", &hands[0]);
  if (e == vr::VRInputError_None) e = in->GetInputSourceHandle("/user/hand/right", &hands[1]);
  if (e != vr::VRInputError_None) {
    *error = "SteamVR Input error " + std::to_string((int)e) + " loading " + manifestPath;
    return false;
  }
  ready_ = true;
  return true;
}

void Input::Update() {
  if (!ready_) return;
  vr::IVRInput *in = vr::VRInput();
  vr::VRActiveActionSet_t active{};
  active.ulActionSet = set_;
  in->UpdateActionState(&active, sizeof active, 1);
  vr::InputDigitalActionData_t t{};
  const bool down = in->GetDigitalActionData(toggle, &t, sizeof t, vr::k_ulInvalidInputValueHandle) ==
                        vr::VRInputError_None &&
                    t.bActive && t.bState;
  if (down && !toggleDown_) toggled_ = true;
  toggleDown_ = down;
}

bool Input::OpenBindings(std::string *error) {
  if (!ready_) {
    *error = "SteamVR Input unavailable";
    return false;
  }
  const vr::EVRInputError e = vr::VRInput()->OpenBindingUI(nullptr, set_, vr::k_ulInvalidInputValueHandle, false);
  if (e == vr::VRInputError_None) return true;
  *error = "SteamVR Input error " + std::to_string((int)e) + " opening the binding editor";
  return false;
}

}  // namespace framepip
