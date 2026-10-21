#include "room_grab.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace framepip {
namespace {

constexpr double kTouchDepth = 0.08;   // m in front of / behind the PiP that counts as touching it
constexpr double kTouchMargin = 0.04;  // m beyond its edges that still counts
constexpr double kReach = 8;           // m: farthest a pointing controller picks it up from

struct Vec {
  double x, y, z;
};
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator*(Vec a, double k) { return {a.x * k, a.y * k, a.z * k}; }
double Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec Cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double Length(Vec a) { return std::sqrt(Dot(a, a)); }
Vec Normalized(Vec a) { return a * (1 / std::max(Length(a), 1e-9)); }

Vec Column(const vr::HmdMatrix34_t &m, int c) { return {m.m[0][c], m.m[1][c], m.m[2][c]}; }
Vec Position(const vr::HmdMatrix34_t &m) { return Column(m, 3); }

vr::HmdMatrix34_t FromColumns(Vec x, Vec y, Vec z, Vec p) {
  vr::HmdMatrix34_t m{};
  const Vec cols[4] = {x, y, z, p};
  for (int c = 0; c < 4; c++) {
    m.m[0][c] = (float)cols[c].x;
    m.m[1][c] = (float)cols[c].y;
    m.m[2][c] = (float)cols[c].z;
  }
  return m;
}

// Does a controller at `hand` (standing space) point at or touch the window?
bool OnWindow(const vr::HmdMatrix34_t &hand, const RoomGrab::Window &w) {
  const vr::HmdMatrix34_t local = Multiply(InverseRigid(w.pose), hand);  // the hand in the window's frame
  const Vec o = Position(local), dir = Column(local, 2) * -1;           // controllers point along -Z
  const double hw = w.width / 2, hh = w.height / 2;
  if (std::fabs(o.z) < kTouchDepth && std::fabs(o.x) < hw + kTouchMargin && std::fabs(o.y) < hh + kTouchMargin)
    return true;
  if (std::fabs(dir.z) < 1e-6) return false;
  const double t = -o.z / dir.z;
  if (t <= 0 || t > kReach) return false;
  const Vec hit = o + dir * t;
  return std::fabs(hit.x) <= hw && std::fabs(hit.y) <= hh;
}

}  // namespace

vr::HmdMatrix34_t Multiply(const vr::HmdMatrix34_t &a, const vr::HmdMatrix34_t &b) {
  vr::HmdMatrix34_t r{};
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 4; j++) {
      double v = j == 3 ? a.m[i][3] : 0;
      for (int k = 0; k < 3; k++) v += (double)a.m[i][k] * b.m[k][j];
      r.m[i][j] = (float)v;
    }
  return r;
}

vr::HmdMatrix34_t InverseRigid(const vr::HmdMatrix34_t &m) {
  vr::HmdMatrix34_t r{};
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) r.m[i][j] = m.m[j][i];
  for (int i = 0; i < 3; i++) r.m[i][3] = -(r.m[i][0] * m.m[0][3] + r.m[i][1] * m.m[1][3] + r.m[i][2] * m.m[2][3]);
  return r;
}

vr::HmdMatrix34_t InFrontOf(const vr::HmdMatrix34_t &head, double distance) {
  const Vec eye = Position(head), gaze = Normalized(Column(head, 2) * -1);
  const Vec p = eye + gaze * distance;
  const Vec z = gaze * -1;  // the PiP faces back at the eye
  Vec x = Cross({0, 1, 0}, z);
  x = Length(x) > 1e-3 ? Normalized(x) : Normalized(Column(head, 0));  // looking straight up or down
  const Vec y = Cross(z, x);
  return FromColumns(x, y, z, p);
}

void RoomGrab::Buzz(const Input &input, int hand) {
  vr::VRInput()->TriggerHapticVibrationAction(input.haptic, 0, 0.025f, 160, 0.6f, input.hands[hand]);
}

void RoomGrab::Hold(int hand, const Window &w) {
  holder_ = hand;
  offset_ = Multiply(InverseRigid(hands_[hand].pose), w.pose);
  widthAtHold_ = w.width;
}

RoomGrab::Change RoomGrab::Update(const Input &input, bool grabbable, Window *w, double minWidth, double maxWidth) {
  if (!input.Ready()) return Change::None;
  vr::IVRInput *in = vr::VRInput();

  bool pressed[2] = {}, released[2] = {};
  for (int h = 0; h < 2; h++) {
    Hand &hand = hands_[h];
    vr::InputDigitalActionData_t grip{};
    const bool down = in->GetDigitalActionData(input.grab, &grip, sizeof grip, input.hands[h]) ==
                          vr::VRInputError_None &&
                      grip.bActive && grip.bState;
    pressed[h] = down && !hand.gripping;
    released[h] = !down && hand.gripping;
    hand.gripping = down;
    vr::InputPoseActionData_t pose{};
    hand.tracked = in->GetPoseActionDataForNextFrame(input.aim, vr::TrackingUniverseStanding, &pose, sizeof pose,
                                                     input.hands[h]) == vr::VRInputError_None &&
                   pose.bActive && pose.pose.bPoseIsValid;
    if (hand.tracked) hand.pose = pose.pose.mDeviceToAbsoluteTracking;
    hand.onWindow = grabbable && hand.tracked && OnWindow(hand.pose, *w);
  }

  Change change = Change::None;
  if (!grabbable) {
    if (holder_ >= 0) change = Change::Released;
    holder_ = resizer_ = -1;
    aimed_ = false;
    return change;
  }

  for (int h = 0; h < 2; h++) {
    if (released[h] && h == resizer_) resizer_ = -1;
    if (released[h] && h == holder_) {
      if (resizer_ >= 0 && hands_[resizer_].tracked) {  // the other hand carries on holding it
        Hold(resizer_, *w);
        resizer_ = -1;
      } else {
        holder_ = resizer_ = -1;
        change = Change::Released;
        Buzz(input, h);
      }
    }
    if (!pressed[h] || !hands_[h].tracked) continue;
    if (holder_ < 0 && hands_[h].onWindow) {
      Hold(h, *w);
      Buzz(input, h);
    } else if (holder_ >= 0 && h != holder_ && resizer_ < 0 && hands_[holder_].tracked) {
      resizer_ = h;
      resizeDistance_ = Length(Position(hands_[h].pose) - Position(hands_[holder_].pose));
      resizeWidth_ = w->width;
      Buzz(input, h);
    }
  }

  if (holder_ >= 0 && hands_[holder_].tracked) {
    if (resizer_ >= 0 && hands_[resizer_].tracked && resizeDistance_ > 0.02) {
      const double aspect = w->height / w->width;
      const double spacing = Length(Position(hands_[resizer_].pose) - Position(hands_[holder_].pose));
      w->width = std::clamp(resizeWidth_ * spacing / resizeDistance_, minWidth, maxWidth);
      w->height = w->width * aspect;
    }
    // follow the hand; when resized, scale about the hand so the part under it stays there
    vr::HmdMatrix34_t offset = offset_;
    const double k = w->width / widthAtHold_;
    for (int i = 0; i < 3; i++) offset.m[i][3] = (float)(offset_.m[i][3] * k);
    w->pose = Multiply(hands_[holder_].pose, offset);
    if (change == Change::None) change = Change::Moved;
  }
  aimed_ = holder_ >= 0 || hands_[0].onWindow || hands_[1].onWindow;
  return change;
}

}  // namespace framepip
