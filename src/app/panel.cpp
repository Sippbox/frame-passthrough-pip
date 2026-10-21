#include "panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace framepip {
namespace {

struct Rect {
  float x0, y0, x1, y1;
  bool In(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};

// palette, from the Steam Frame logo: its ink navy (#0E141B) for the page, navy-tinted cards and lines, and its
// blue (#1A9FFF) for accents (card highlight, slider fills, switches, pressed buttons)
const Color kBg{14, 20, 27}, kCard{21, 29, 39}, kCardBorder{38, 49, 63}, kCardHot{26, 36, 48},
    kCardHotBorder{26, 159, 255}, kCardGlow{26, 159, 255, 60}, kWell{11, 16, 22}, kWellBorder{34, 44, 56},
    kTrack{42, 54, 68}, kText{245, 247, 250}, kSoft{201, 210, 220}, kMuted{142, 154, 168}, kAccent{26, 159, 255},
    kButton{23, 32, 43}, kButtonPressed{22, 58, 92}, kButtonBorder{47, 59, 74}, kButtonBorderPressed{26, 159, 255},
    kNavFill{17, 24, 33}, kTabActive{30, 44, 60}, kOk{74, 222, 128}, kWarn{251, 191, 36}, kErr{248, 113, 113},
    kKnobOn{245, 247, 250}, kKnobOff{201, 210, 220};

// animation rates (1/s): larger is snappier
constexpr float kFast = 22, kMedium = 12, kSlow = 7;

// header
const Rect kNav{372, 24, 712, 92};
const Rect kTabMain{380, 32, 520, 84}, kTabSettings{528, 32, 704, 84};
const Rect kToggle{960, 38, 1048, 82};
const Rect kClose{1072, 32, 1240, 88};
const Rect kContent{0, 150, 1280, 800};  // what fades in after a page switch
// main page
const Rect kLeftCard{40, 160, 628, 770}, kRightCard{652, 160, 1240, 770};
constexpr float kDegPx = 5.0f;  // view area scale
const Rect kView{134, 236, 134 + 2 * Settings::kYawMax * kDegPx, 236 + 2 * Settings::kPitchMax * kDegPx};
const Rect kReset{684, 700, 928, 752};
// position card: follow view / local space
const Rect kModeSeg{300, 178, 596, 222};
const Rect kRoomWell{72, 252, 596, 452};
const Rect kLockToggle{508, 606, 596, 650};
const Rect kBring{72, 690, 372, 742};
// settings page
const Rect kSettingsCard{40, 160, 1240, 770};
const Rect kAutostartToggle{1128, 244, 1216, 288};
const Rect kRememberToggle{1128, 364, 1216, 408};
const Rect kBindings{960, 478, 1216, 530};

struct Slider {
  float x0, x1, labelY, trackY;
  const char *label;
};
const Slider kYawSlider{96, 572, 646, 674, "Horizontal"};
const Slider kPitchSlider{96, 572, 714, 742, "Vertical"};
const Slider kSizeSlider{684, 1208, 268, 298, "Size"};
const Slider kOpacitySlider{684, 1208, 358, 388, "Opacity"};
const Slider kStrengthSlider{684, 1208, 618, 648, "Tint strength"};
const Rect kHueBar{684, 528, 1208, 556};

struct Swatch {
  double hue;  // < 0: no tint
  const char *name;
};
const Swatch kSwatches[] = {{-1, "None"}, {120, "Green"}, {40, "Amber"}, {185, "Cyan"}, {0, "Red"}, {275, "Violet"}, {220, "Blue"}};
constexpr float kSwatchY = 476, kSwatchX0 = 708, kSwatchStep = 74, kSwatchR = 22;

// magnetic presets: corners, edge midpoints and centre, as fractions of the current position limits
const double kAnchors[3] = {-0.9, 0, 0.9};

Rect SliderHit(const Slider &s) { return {s.x0 - 24, s.trackY - 28, s.x1 + 24, s.trackY + 28}; }

double SliderValue(const Slider &s, float x, double lo, double hi) {
  return lo + (hi - lo) * std::clamp((x - s.x0) / (s.x1 - s.x0), 0.f, 1.f);
}

float ViewX(double yaw) { return kView.x0 + (float)(yaw + Settings::kYawMax) * kDegPx; }
float ViewY(double pitch) { return kView.y0 + (float)(Settings::kPitchMax - pitch) * kDegPx; }

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

Color Mix(Color a, Color b, float t) {
  t = std::clamp(t, 0.f, 1.f);
  return {(uint8_t)Lerp(a.r, b.r, t), (uint8_t)Lerp(a.g, b.g, t), (uint8_t)Lerp(a.b, b.b, t), (uint8_t)Lerp(a.a, b.a, t)};
}

Color WithAlpha(Color c, float a) {
  c.a = (uint8_t)(c.a * std::clamp(a, 0.f, 1.f));
  return c;
}

Color HueColor(double hue) {
  float r, g, b;
  HueToRgb(hue, &r, &g, &b);
  return {(uint8_t)(r * 255), (uint8_t)(g * 255), (uint8_t)(b * 255)};
}

std::string Format(const char *fmt, double v) {
  char b[64];
  snprintf(b, sizeof b, fmt, v);
  return b;
}

std::string YawText(double yaw) {
  if (std::fabs(yaw) < 0.5) return "Centre";
  return Format("%.0f\xC2\xB0 ", std::fabs(yaw)) + (yaw > 0 ? "right" : "left");
}

std::string PitchText(double pitch) {
  if (std::fabs(pitch) < 0.5) return "Centre";
  return Format("%.0f\xC2\xB0 ", std::fabs(pitch)) + (pitch > 0 ? "up" : "down");
}

std::string WidthText(double metres) {
  return metres < 1 ? Format("%.0f cm", metres * 100) : Format("%.2f m", metres);
}

// --- components (t: animation level 0..1)

void DrawCard(Canvas *c, const Rect &r, float t) {
  if (t > 0.01f) c->Glow(r.x0, r.y0, r.x1, r.y1, 18, 16, WithAlpha(kCardGlow, t));
  c->FillRoundRect(r.x0, r.y0, r.x1, r.y1, 18, Mix(kCard, kCardHot, t));
  c->StrokeRoundRect(r.x0, r.y0, r.x1, r.y1, 18, Lerp(1.5f, 2.5f, t), Mix(kCardBorder, kCardHotBorder, t));
}

void DrawToggle(Canvas *c, const Rect &r, float t, bool usable) {
  const float h = (r.y1 - r.y0) / 2;
  c->FillRoundRect(r.x0, r.y0, r.x1, r.y1, h, usable ? Mix(kTrack, kAccent, t) : kButton);
  if (!usable) c->StrokeRoundRect(r.x0, r.y0, r.x1, r.y1, h, 1.5f, kCardBorder);
  c->FillCircle(Lerp(r.x0 + h, r.x1 - h, t), r.y0 + h, h - 5, usable ? Mix(kKnobOff, kKnobOn, t) : kTrack);
}

// Material Icons code points (resources/fonts/MaterialIcons-Regular.ttf)
constexpr int kIconPip = 0xe8aa;         // picture_in_picture
constexpr int kIconTune = 0xe429;        // tune
constexpr int kIconClose = 0xe5cd;       // close
constexpr int kIconRestart = 0xf053;     // restart_alt
constexpr int kIconToFront = 0xe883;     // flip_to_front
constexpr int kIconController = 0xea28;  // sports_esports
constexpr float kButtonIcon = 26;        // icon size in buttons and tabs

// An outlined button with an icon and a bold label after it; `press` flashes it, `spinDeg` turns the icon.
void DrawButton(Canvas *c, const Rect &r, const char *label, float press, int icon, float spinDeg = 0) {
  c->FillRoundRect(r.x0, r.y0, r.x1, r.y1, 12, Mix(kButton, kButtonPressed, press));
  c->StrokeRoundRect(r.x0, r.y0, r.x1, r.y1, 12, 1.5f, Mix(kButtonBorder, kButtonBorderPressed, press));
  const float textW = c->TextWidth(22, label, true), contentW = kButtonIcon + 10 + textW;
  const float x = (r.x0 + r.x1 - contentW) / 2, cy = (r.y0 + r.y1) / 2;
  c->Icon(x + kButtonIcon / 2, cy, kButtonIcon, icon, kText, spinDeg);
  c->Text(x + kButtonIcon + 10, cy + 8, 22, label, kText, true);
}

void DrawSlider(Canvas *c, const Slider &s, double t, const std::string &value, float grab) {
  c->Text(s.x0, s.labelY, 22, s.label, kMuted);
  c->Text(s.x1, s.labelY, 22, value, kText, true, Align::Right);
  c->FillRoundRect(s.x0, s.trackY - 4, s.x1, s.trackY + 4, 4, kTrack);
  const float kx = s.x0 + (s.x1 - s.x0) * (float)std::clamp(t, 0.0, 1.0);
  c->FillRoundRect(s.x0, s.trackY - 4, kx, s.trackY + 4, 4, kAccent);
  if (grab > 0.01f) c->FillCircle(kx, s.trackY, 13 + 12 * grab, WithAlpha(kText, 0.18f * grab));  // grab halo
  c->FillCircle(kx, s.trackY, 15 + 3 * grab, kBg);  // dark rim: keeps the white knob apart from the light fill
  c->FillCircle(kx, s.trackY, 13 + 3 * grab, kText);
}

}  // namespace

bool Panel::LoadArt(const std::string &shareDir, std::string *error) {
  const int w = (int)(kRoomWell.x1 - kRoomWell.x0), h = (int)(kRoomWell.y1 - kRoomWell.y0);
  std::string unlockedError, lockedError;
  roomUnlocked_ = LoadImage(shareDir + "/localspace-unlocked.png", w, h, &unlockedError);
  roomLocked_ = LoadImage(shareDir + "/localspace-locked.png", w, h, &lockedError);
  *error = unlockedError.empty() ? lockedError : unlockedError;
  return error->empty();
}

bool Panel::Anim::Step(float dt, float rate) {
  if (v == target) return false;
  v += (target - v) * (1 - std::exp(-dt * rate));
  if (std::fabs(target - v) < 0.002f * std::max(1.f, std::fabs(target))) v = target;
  return true;
}

bool Panel::Tick(double dtSeconds, const Settings &s, const PanelStatus &status) {
  const float dt = (float)std::min(dtSeconds, 0.1);
  // targets
  for (int i = 0; i < 4; i++) cards_[i].target = (int)hover_ == i && i != 0 ? 1.f : 0.f;
  tab_.target = page_ == Page::Settings ? 1.f : 0.f;
  showToggle_.target = s.enabled ? 1.f : 0.f;
  autoToggle_.target = status.autostart == Autostart::State::Enabled ? 1.f : 0.f;
  lockToggle_.target = s.roomLocked ? 1.f : 0.f;
  rememberToggle_.target = s.rememberRoomPose ? 1.f : 0.f;
  mode_.target = s.roomMode ? 1.f : 0.f;
  for (int i = 0; i < (int)Target::Count; i++) knobs_[i].target = (int)drag_ == i && i != 0 ? 1.f : 0.f;
  for (int i = 0; i < kSwatchCount; i++) {
    const bool none = kSwatches[i].hue < 0;
    swatches_[i].target =
        (none ? s.tintStrength < 0.005 : s.tintStrength >= 0.005 && std::fabs(s.tintHue - kSwatches[i].hue) < 0.5) ? 1.f : 0.f;
  }
  for (Anim &p : presses_) p.target = 0;
  yaw_.target = (float)s.yawDeg;
  pitch_.target = (float)s.pitchDeg;
  size_.target = (float)s.sizeDeg;
  roomWidth_.target = (float)s.roomWidth;
  opacity_.target = (float)s.opacity;
  hue_.target = (float)s.tintHue;
  strength_.target = (float)s.tintStrength;
  if (!animInit_) {  // first frame: start settled
    animInit_ = true;
    for (Anim *a : {&tab_, &showToggle_, &autoToggle_, &lockToggle_, &rememberToggle_, &mode_, &yaw_, &pitch_, &size_,
                    &roomWidth_, &opacity_, &hue_, &strength_})
      a->v = a->target;
    for (Anim &a : swatches_) a.v = a.target;
  }

  bool moving = false;
  for (Anim &a : cards_) moving |= a.Step(dt, kMedium);
  moving |= tab_.Step(dt, kMedium);
  moving |= pageFade_.Step(dt, kSlow);
  moving |= showToggle_.Step(dt, kMedium);
  moving |= autoToggle_.Step(dt, kMedium);
  moving |= lockToggle_.Step(dt, kMedium);
  moving |= rememberToggle_.Step(dt, kMedium);
  moving |= mode_.Step(dt, kMedium);
  for (Anim &a : knobs_) moving |= a.Step(dt, kFast);
  for (Anim &a : presses_) moving |= a.Step(dt, kSlow / 2);
  for (Anim &a : swatches_) moving |= a.Step(dt, kMedium);
  // displayed values glide to clicks (presets, reset) but follow drags closely
  const float follow = drag_ == Target::None ? kMedium : kFast * 2;
  for (Anim *a : {&yaw_, &pitch_, &size_, &roomWidth_, &opacity_, &hue_, &strength_}) moving |= a->Step(dt, follow);
  // a waiting status dot breathes
  if (status.level == PanelStatus::Waiting) {
    pulse_ += dt;
    moving = true;
  }
  return moving;
}

bool Panel::Apply(Target t, float x, float y, Settings *s) const {
  const Settings before = *s;
  switch (t) {
    case Target::View: {
      s->yawDeg = std::round((x - kView.x0) / kDegPx - Settings::kYawMax);
      s->pitchDeg = std::round(Settings::kPitchMax - (y - kView.y0) / kDegPx);
      for (double ax : kAnchors)
        for (double ay : kAnchors) {
          const double yaw = ax * s->YawLimit(), pitch = ay * s->PitchLimit();
          if (std::hypot(ViewX(yaw) - x, ViewY(pitch) - y) < 16) s->yawDeg = yaw, s->pitchDeg = pitch;
        }
      break;
    }
    case Target::Yaw: s->yawDeg = std::round(SliderValue(kYawSlider, x, -s->YawLimit(), s->YawLimit())); break;
    case Target::Pitch: s->pitchDeg = std::round(SliderValue(kPitchSlider, x, -s->PitchLimit(), s->PitchLimit())); break;
    case Target::Size:
      if (s->roomMode)
        s->roomWidth = std::round(100 * SliderValue(kSizeSlider, x, Settings::kRoomWidthMin, Settings::kRoomWidthMax)) / 100;
      else
        s->sizeDeg = std::round(SliderValue(kSizeSlider, x, Settings::kSizeMin, Settings::kSizeMax));
      break;
    case Target::Opacity:
      s->opacity = std::round(100 * SliderValue(kOpacitySlider, x, Settings::kOpacityMin, Settings::kOpacityMax)) / 100;
      break;
    case Target::Hue:
      s->tintHue = std::round(360 * std::clamp((x - kHueBar.x0) / (kHueBar.x1 - kHueBar.x0), 0.f, 0.997f));
      if (s->tintStrength < 0.05) s->tintStrength = 0.6;  // make the pick visible
      break;
    case Target::Strength: s->tintStrength = std::round(100 * SliderValue(kStrengthSlider, x, 0, 1)) / 100; break;
    case Target::None:
    case Target::Count: break;
  }
  s->Clamp();
  return s->yawDeg != before.yawDeg || s->pitchDeg != before.pitchDeg || s->sizeDeg != before.sizeDeg ||
         s->roomWidth != before.roomWidth || s->opacity != before.opacity || s->tintHue != before.tintHue ||
         s->tintStrength != before.tintStrength;
}

void Panel::SetPage(Page p) {
  if (p == page_) return;
  page_ = p;
  hover_ = Card::None;
  for (Anim &a : cards_) a.v = 0;  // the new page's cards start unlit
  pageFade_.v = 0;
  pageFade_.target = 1;
  redraw_ = true;
}

void Panel::Hover(float x, float y) {
  Card h = Card::None;
  if (page_ == Page::Settings) {
    if (kSettingsCard.In(x, y)) h = Card::Startup;
  } else if (drag_ == Target::View || drag_ == Target::Yaw || drag_ == Target::Pitch) {
    h = Card::Position;  // a drag keeps its card lit even when the laser strays
  } else if (drag_ != Target::None) {
    h = Card::Appearance;
  } else if (kLeftCard.In(x, y)) {
    h = Card::Position;
  } else if (kRightCard.In(x, y)) {
    h = Card::Appearance;
  }
  if (h != hover_) {
    hover_ = h;
    redraw_ = true;
  }
}

void Panel::MouseLeave() {
  if (hover_ != Card::None && drag_ == Target::None) {
    hover_ = Card::None;
    redraw_ = true;
  }
}

bool Panel::MouseDown(float x, float y, Settings *s) {
  drag_ = Target::None;
  if (kTabMain.In(x, y)) {
    SetPage(Page::Main);
    return false;
  }
  if (kTabSettings.In(x, y)) {
    SetPage(Page::Settings);
    return false;
  }
  if (kClose.In(x, y)) {
    presses_[kCloseButton].v = 1;
    request_ = Request::Quit;
    redraw_ = true;
    return false;
  }
  if (kToggle.In(x, y)) {
    s->enabled = !s->enabled;
    clicked_ = true;
    return true;
  }
  if (page_ == Page::Settings) {
    if (kAutostartToggle.In(x, y)) request_ = Request::ToggleAutostart;
    if (kBindings.In(x, y)) {
      presses_[kBindingsButton].v = 1;
      request_ = Request::OpenBindings;
      redraw_ = true;
      return false;
    }
    if (kRememberToggle.In(x, y)) {
      s->rememberRoomPose = !s->rememberRoomPose;
      clicked_ = true;
      return true;
    }
    return false;
  }
  if (kModeSeg.In(x, y)) {
    const bool room = x >= (kModeSeg.x0 + kModeSeg.x1) / 2;
    if (room == s->roomMode) return false;
    s->roomMode = room;
    clicked_ = true;
    Hover(x, y);
    return true;
  }
  if (kReset.In(x, y)) {
    // appearance and position go back to defaults; how and where the PiP is placed stays as chosen
    const Settings keep = *s;
    *s = Settings{};
    s->enabled = keep.enabled;
    s->roomMode = keep.roomMode;
    s->roomLocked = keep.roomLocked;
    s->rememberRoomPose = keep.rememberRoomPose;
    s->hasRoomPose = keep.hasRoomPose;
    std::copy(&keep.roomPose[0][0], &keep.roomPose[0][0] + 12, &s->roomPose[0][0]);
    if (s->roomMode) request_ = Request::BringHere;
    presses_[kResetButton].v = 1;
    clicked_ = true;
    return true;
  }
  if (s->roomMode && kBring.In(x, y)) {
    presses_[kBringButton].v = 1;
    request_ = Request::BringHere;
    redraw_ = true;
    return false;
  }
  if (s->roomMode && kLockToggle.In(x, y)) {
    s->roomLocked = !s->roomLocked;
    clicked_ = true;
    return true;
  }
  for (size_t i = 0; i < std::size(kSwatches); i++)
    if (std::hypot(x - (kSwatchX0 + i * kSwatchStep), y - kSwatchY) <= kSwatchR + 6) {
      if (kSwatches[i].hue < 0) {
        s->tintStrength = 0;
      } else {
        s->tintHue = kSwatches[i].hue;
        if (s->tintStrength < 0.05) s->tintStrength = 0.6;
      }
      clicked_ = true;
      return true;
    }
  const Rect viewHit{kView.x0 - 12, kView.y0 - 12, kView.x1 + 12, kView.y1 + 12};
  const Rect hueHit{kHueBar.x0 - 16, kHueBar.y0 - 16, kHueBar.x1 + 16, kHueBar.y1 + 16};
  if (!s->roomMode && viewHit.In(x, y)) drag_ = Target::View;
  else if (!s->roomMode && SliderHit(kYawSlider).In(x, y)) drag_ = Target::Yaw;
  else if (!s->roomMode && SliderHit(kPitchSlider).In(x, y)) drag_ = Target::Pitch;
  else if (SliderHit(kSizeSlider).In(x, y)) drag_ = Target::Size;
  else if (SliderHit(kOpacitySlider).In(x, y)) drag_ = Target::Opacity;
  else if (hueHit.In(x, y)) drag_ = Target::Hue;
  else if (SliderHit(kStrengthSlider).In(x, y)) drag_ = Target::Strength;
  Hover(x, y);
  return Apply(drag_, x, y, s);
}

bool Panel::MouseMove(float x, float y, Settings *s) {
  Hover(x, y);
  return drag_ != Target::None && Apply(drag_, x, y, s);
}

bool Panel::MouseUp(float x, float y, Settings *s, bool *commit) {
  const bool changed = drag_ != Target::None && Apply(drag_, x, y, s);
  *commit = drag_ != Target::None || clicked_;
  drag_ = Target::None;
  clicked_ = false;
  Hover(x, y);
  return changed;
}

void Panel::Draw(Canvas *c, const Settings &s, const PanelStatus &status) const {
  c->Clear(kBg);
  const bool fading = pageFade_.v < 1;
  std::vector<uint8_t> background;
  if (fading) background = c->Snapshot();
  if (page_ == Page::Settings) DrawSettings(c, s, status);
  else DrawMain(c, s);
  if (fading) c->BlendToward(background, (int)kContent.x0, (int)kContent.y0, (int)kContent.x1, (int)kContent.y1, 1 - pageFade_.v);
  DrawHeader(c, s, status);
}

void Panel::DrawHeader(Canvas *c, const Settings &s, const PanelStatus &status) const {
  c->Text(48, 66, 38, "Passthrough PiP", kText, true);
  c->Text(50, 96, 20, "by Sippbox", kMuted);
  const Color dot = status.level == PanelStatus::Ok ? kOk : status.level == PanelStatus::Waiting ? kWarn : kErr;
  if (status.level == PanelStatus::Waiting) {
    const float breathe = 0.5f + 0.5f * std::sin(pulse_ * 4.0f);
    c->FillCircle(56, 124, 6 + 6 * breathe, WithAlpha(dot, 0.35f * (1 - breathe)));
  }
  c->FillCircle(56, 124, 6, dot);
  c->Text(72, 131, 22, status.text, kMuted);

  // navigation pill, as on a website header; the active-tab highlight slides between tabs
  c->FillRoundRect(kNav.x0, kNav.y0, kNav.x1, kNav.y1, 16, kNavFill);
  c->StrokeRoundRect(kNav.x0, kNav.y0, kNav.x1, kNav.y1, 16, 1.5f, kCardBorder);
  const float t = tab_.v;
  c->FillRoundRect(Lerp(kTabMain.x0, kTabSettings.x0, t), kTabMain.y0, Lerp(kTabMain.x1, kTabSettings.x1, t), kTabMain.y1, 12,
                   kTabActive);
  const float ty = (kTabMain.y0 + kTabMain.y1) / 2;
  const Color mainCol = Mix(kText, kSoft, t), settingsCol = Mix(kSoft, kText, t);
  c->Icon(kTabMain.x0 + 34, ty, kButtonIcon, kIconPip, mainCol);
  c->Text(kTabMain.x0 + 60, ty + 8, 23, "PiP", mainCol, t < 0.5f);
  c->Icon(kTabSettings.x0 + 33, ty, kButtonIcon, kIconTune, settingsCol);
  c->Text(kTabSettings.x0 + 58, ty + 8, 23, "Settings", settingsCol, t >= 0.5f);

  c->Text(kToggle.x0 - 16, (kToggle.y0 + kToggle.y1) / 2 + 8, 23, "Show PiP", kText, true, Align::Right);
  DrawToggle(c, kToggle, showToggle_.v, true);
  (void)s;
  DrawButton(c, kClose, "Close app", presses_[kCloseButton].v, kIconClose);
}

void Panel::DrawMain(Canvas *c, const Settings &s) const {
  // the preview, knobs and swatches show the gliding values
  Settings shown = s;
  shown.yawDeg = yaw_.v, shown.pitchDeg = pitch_.v, shown.sizeDeg = size_.v, shown.roomWidth = roomWidth_.v;
  shown.opacity = opacity_.v, shown.tintHue = hue_.v, shown.tintStrength = strength_.v;

  // position card: a "Follow view | Local space" switch, then that mode's controls
  DrawCard(c, kLeftCard, cards_[(int)Card::Position].v);
  c->Text(72, 212, 30, "Position", kText, true);
  c->FillRoundRect(kModeSeg.x0, kModeSeg.y0, kModeSeg.x1, kModeSeg.y1, 14, kNavFill);
  c->StrokeRoundRect(kModeSeg.x0, kModeSeg.y0, kModeSeg.x1, kModeSeg.y1, 14, 1.5f, kCardBorder);
  const float half = (kModeSeg.x1 - kModeSeg.x0) / 2, t = mode_.v;
  c->FillRoundRect(kModeSeg.x0 + 4 + half * t, kModeSeg.y0 + 4, kModeSeg.x0 + half - 4 + half * t, kModeSeg.y1 - 4, 10,
                   kTabActive);
  const float segY = (kModeSeg.y0 + kModeSeg.y1) / 2 + 7;
  c->Text(kModeSeg.x0 + half / 2, segY, 20, "Follow view", Mix(kText, kSoft, t), t < 0.5f, Align::Center);
  c->Text(kModeSeg.x0 + half * 1.5f, segY, 20, "Local space", Mix(kSoft, kText, t), t >= 0.5f, Align::Center);
  if (s.roomMode) DrawRoomPosition(c);
  else DrawViewPosition(c, s, shown);

  // appearance card
  DrawCard(c, kRightCard, cards_[(int)Card::Appearance].v);
  c->Text(684, 212, 30, "Appearance", kText, true);
  if (s.roomMode)
    DrawSlider(c, kSizeSlider,
               (shown.roomWidth - Settings::kRoomWidthMin) / (Settings::kRoomWidthMax - Settings::kRoomWidthMin),
               WidthText(s.roomWidth), knobs_[(int)Target::Size].v);
  else
    DrawSlider(c, kSizeSlider, (shown.sizeDeg - Settings::kSizeMin) / (Settings::kSizeMax - Settings::kSizeMin),
               Format("%.0f\xC2\xB0", s.sizeDeg), knobs_[(int)Target::Size].v);
  DrawSlider(c, kOpacitySlider, (shown.opacity - Settings::kOpacityMin) / (Settings::kOpacityMax - Settings::kOpacityMin),
             Format("%.0f%%", s.opacity * 100), knobs_[(int)Target::Opacity].v);
  c->Text(684, 438, 22, "Tint", kMuted);
  for (int i = 0; i < kSwatchCount; i++) {
    const float x = kSwatchX0 + i * kSwatchStep, sel = swatches_[i].v;
    const bool none = kSwatches[i].hue < 0;
    if (sel > 0.01f) c->Arc(x, kSwatchY, kSwatchR + 1 + 4 * sel, 3, -180, 180, WithAlpha(kText, sel));
    c->FillCircle(x, kSwatchY, kSwatchR - 2 * sel, none ? kTrack : HueColor(kSwatches[i].hue));
    if (none) c->Line(x - 10, kSwatchY + 10, x + 10, kSwatchY - 10, 3, kErr);
  }
  c->HueBar(kHueBar.x0, kHueBar.y0, kHueBar.x1, kHueBar.y1);
  {
    const float kx = kHueBar.x0 + (kHueBar.x1 - kHueBar.x0) * (float)(shown.tintHue / 360);
    const float ky = (kHueBar.y0 + kHueBar.y1) / 2, grab = knobs_[(int)Target::Hue].v;
    if (grab > 0.01f) c->FillCircle(kx, ky, 17 + 10 * grab, WithAlpha(kText, 0.18f * grab));
    c->FillCircle(kx, ky, 17 + 3 * grab, kText);
    c->FillCircle(kx, ky, 12 + 3 * grab, HueColor(shown.tintHue));
  }
  DrawSlider(c, kStrengthSlider, shown.tintStrength, Format("%.0f%%", s.tintStrength * 100), knobs_[(int)Target::Strength].v);
  const float press = presses_[kResetButton].v;
  DrawButton(c, kReset, "Reset to defaults", press, kIconRestart, -360 * (1 - press));  // spins once when pressed
}

void Panel::DrawViewPosition(Canvas *c, const Settings &s, const Settings &shown) const {
  c->FillRoundRect(kView.x0, kView.y0, kView.x1, kView.y1, 14, kWell);
  c->StrokeRoundRect(kView.x0, kView.y0, kView.x1, kView.y1, 14, 1.5f, kWellBorder);
  c->Line(ViewX(0), kView.y0 + 12, ViewX(0), kView.y1 - 12, 1, kWellBorder);
  c->Line(kView.x0 + 12, ViewY(0), kView.x1 - 12, ViewY(0), 1, kWellBorder);
  for (double ax : kAnchors)
    for (double ay : kAnchors) c->FillCircle(ViewX(ax * shown.YawLimit()), ViewY(ay * shown.PitchLimit()), 5, kTrack);
  {
    float tr, tg, tb;
    shown.TintColor(&tr, &tg, &tb);
    const float w = (float)shown.sizeDeg * kDegPx, h = w * (float)Settings::kAspect;
    const float cx = ViewX(shown.yawDeg), cy = ViewY(shown.pitchDeg);
    const Color body{(uint8_t)(190 * tr), (uint8_t)(190 * tg), (uint8_t)(190 * tb), (uint8_t)(60 + 150 * shown.opacity)};
    const float grab = knobs_[(int)Target::View].v;
    if (grab > 0.01f) c->Glow(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, 6, 14, WithAlpha(kAccent, 0.45f * grab));
    c->FillRoundRect(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, 6, kWell);
    c->FillRoundRect(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, 6, body);
    c->StrokeRoundRect(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, 6, 2.5f, kAccent);
  }
  c->Text((kView.x0 + kView.x1) / 2, kView.y1 + 28, 20, "Drag to place the PiP in your view", kMuted, false, Align::Center);
  DrawSlider(c, kYawSlider, (shown.yawDeg / shown.YawLimit() + 1) / 2, YawText(s.yawDeg), knobs_[(int)Target::Yaw].v);
  DrawSlider(c, kPitchSlider, (shown.pitchDeg / shown.PitchLimit() + 1) / 2, PitchText(s.pitchDeg),
             knobs_[(int)Target::Pitch].v);
}

void Panel::DrawRoomPosition(Canvas *c) const {
  // the Local space graphic, cross-fading between its unlocked and locked versions with the Lock in place switch
  const Rect &w = kRoomWell;
  c->FillRoundRect(w.x0, w.y0, w.x1, w.y1, 14, kWell);
  c->StrokeRoundRect(w.x0, w.y0, w.x1, w.y1, 14, 1.5f, kWellBorder);
  const float locked = lockToggle_.v;
  if (locked < 1) c->DrawImage((int)w.x0, (int)w.y0, roomUnlocked_, 1 - locked);
  if (locked > 0) c->DrawImage((int)w.x0, (int)w.y0, roomLocked_, locked);

  c->Text(72, 488, 20, "Point a controller at the camera view (or touch it)", kMuted);
  c->Text(72, 516, 20, "and hold grip to move it. Grip with your other hand", kMuted);
  c->Text(72, 544, 20, "as well and pull apart to resize it.", kMuted);

  c->Text(72, 632, 22, "Lock in place", kText, true);
  c->Text(72, 660, 20, "Controllers can't move it while locked.", kMuted);
  DrawToggle(c, kLockToggle, lockToggle_.v, true);
  const float press = presses_[kBringButton].v;
  DrawButton(c, kBring, "Bring to front", press, kIconToFront);
}

void Panel::DrawSettings(Canvas *c, const Settings &s, const PanelStatus &status) const {
  DrawCard(c, kSettingsCard, cards_[(int)Card::Startup].v);
  c->Text(72, 212, 30, "General", kText, true);
  using A = Autostart::State;
  const A st = status.autostart;
  c->Text(72, 264, 24, "Start with SteamVR", kText, true);
  const char *desc = st == A::Unavailable ? "Only available when Passthrough PiP is installed with its AppImage."
                     : st == A::Busy || st == A::Unknown ? "Checking..."
                     : st == A::Enabled ? "Passthrough PiP opens by itself whenever SteamVR starts."
                                        : "Start Passthrough PiP from Launch Program when you want it.";
  c->Text(72, 296, 21, desc, kMuted);
  DrawToggle(c, kAutostartToggle, autoToggle_.v, st != A::Unavailable);
  c->Line(72, 330, 1208, 330, 1, kCardBorder);
  c->Text(72, 384, 24, "Remember local space position", kText, true);
  c->Text(72, 416, 21,
          s.rememberRoomPose ? "A PiP in local space is where you left it after SteamVR restarts."
                             : "A PiP in local space starts in front of you each time SteamVR starts.",
          kMuted);
  DrawToggle(c, kRememberToggle, rememberToggle_.v, true);
  c->Line(72, 450, 1208, 450, 1, kCardBorder);
  c->Text(72, 504, 24, "Controller bindings", kText, true);
  c->Text(72, 536, 21, "By default, double-tap View on the left controller to show or hide the PiP.", kMuted);
  DrawButton(c, kBindings, "Edit bindings", presses_[kBindingsButton].v, kIconController);
  c->Line(72, 570, 1208, 570, 1, kCardBorder);
  c->Text(72, 742, 20, "Passthrough PiP " + status.version, kMuted);
}

}  // namespace framepip
