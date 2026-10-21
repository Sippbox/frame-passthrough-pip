// The settings panel shown in the SteamVR dashboard: layout, drawing, animation and mouse handling.
#pragma once

#include <string>

#include "autostart.hpp"
#include "canvas.hpp"
#include "settings.hpp"

namespace framepip {

struct PanelStatus {
  enum Level { Ok, Waiting, Error };
  Level level = Waiting;
  std::string text;
  Autostart::State autostart = Autostart::State::Unknown;
  std::string version;
};

class Panel {
 public:
  static constexpr int kWidth = 1280, kHeight = 800;

  // Loads the panel's pictures from shareDir (the Local space graphics). False (with *error) if one is missing;
  // the panel then draws without it.
  bool LoadArt(const std::string &shareDir, std::string *error);

  // Things the panel asks the app to do (beyond editing Settings).
  enum class Request { None, Quit, ToggleAutostart, BringHere, OpenBindings };
  enum class Page { Main, Settings };

  // Mouse coordinates are panel pixels with the origin top-left. Each returns true when settings changed.
  bool MouseDown(float x, float y, Settings *s);
  bool MouseMove(float x, float y, Settings *s);
  // *commit: the interaction finished and the settings should be saved.
  bool MouseUp(float x, float y, Settings *s, bool *commit);
  // The laser left the panel.
  void MouseLeave();

  // Advances the animations by dt seconds toward what s and status show. True while something is still moving.
  bool Tick(double dt, const Settings &s, const PanelStatus &status);
  void Draw(Canvas *c, const Settings &s, const PanelStatus &status) const;
  void SetPage(Page p);

  // The pending request, if any; cleared by taking it.
  Request TakeRequest() {
    const Request r = request_;
    request_ = Request::None;
    return r;
  }
  // True once after something that changes only the panel itself (page switch, hover highlight).
  bool TakeRedraw() {
    const bool r = redraw_;
    redraw_ = false;
    return r;
  }

 private:
  enum class Target { None, View, Yaw, Pitch, Size, Opacity, Hue, Strength, Count };
  enum class Card { None, Position, Appearance, Startup };
  enum Button { kCloseButton, kResetButton, kBringButton, kBindingsButton, kButtonCount };
  static constexpr int kSwatchCount = 7;

  // A value easing exponentially toward its target.
  struct Anim {
    float v = 0, target = 0;
    bool Step(float dt, float rate);  // true while still moving
  };

  bool Apply(Target t, float x, float y, Settings *s) const;
  void Hover(float x, float y);
  void DrawHeader(Canvas *c, const Settings &s, const PanelStatus &status) const;
  void DrawMain(Canvas *c, const Settings &s) const;
  void DrawViewPosition(Canvas *c, const Settings &s, const Settings &shown) const;
  void DrawRoomPosition(Canvas *c) const;
  void DrawSettings(Canvas *c, const Settings &s, const PanelStatus &status) const;

  Page page_ = Page::Main;
  Target drag_ = Target::None;
  Card hover_ = Card::None;  // highlighted card (under the laser, or being dragged in)
  bool clicked_ = false;  // a settings button was pressed since the last MouseUp
  bool redraw_ = false;
  Request request_ = Request::None;

  Image roomUnlocked_, roomLocked_;  // the Local space graphic, as big as its well

  // animation state
  bool animInit_ = false;
  float pulse_ = 0;                  // phase of the "waiting" status dot, seconds
  Anim cards_[4];                    // highlight per Card
  Anim tab_;                         // 0 = PiP tab, 1 = Settings tab
  Anim pageFade_{1, 1};              // the page's content fading in after a switch
  Anim showToggle_, autoToggle_;     // switch knob positions
  Anim lockToggle_, rememberToggle_;
  Anim mode_;                        // 0 = follow view, 1 = in the room (segmented control highlight)
  Anim knobs_[(int)Target::Count];   // slider knob size while dragged
  Anim presses_[kButtonCount];       // button flash after a click
  Anim swatches_[kSwatchCount];      // selection ring per swatch
  Anim yaw_, pitch_, size_, roomWidth_, opacity_, hue_, strength_;  // displayed values gliding to the settings
};

}  // namespace framepip
