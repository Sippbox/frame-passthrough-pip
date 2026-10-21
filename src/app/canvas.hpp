// A tiny anti-aliased software rasterizer (RGBA8) with TrueType text, for the dashboard panel.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace framepip {

struct Color {
  uint8_t r, g, b, a = 255;
};

enum class Align { Left, Center, Right };

// A straight-alpha RGBA8 image.
struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;
};

// Loads a PNG, scaled (by averaging) to w x h. An empty image, with *error set, if it can't be read.
Image LoadImage(const std::string &path, int w, int h, std::string *error);

class Canvas {
 public:
  Canvas(int w, int h);
  ~Canvas();
  Canvas(const Canvas &) = delete;
  Canvas &operator=(const Canvas &) = delete;

  // Loads Atkinson Hyperlegible (regular + bold) from fontDir, falling back to the system's Noto Sans or fontconfig's
  // sans, and the Material Icons font. False (with *error) if a font is missing; text or icons then don't draw.
  bool LoadFonts(const std::string &fontDir, std::string *error);

  int Width() const { return w_; }
  int Height() const { return h_; }
  const uint8_t *Pixels() const { return px_.data(); }
  // A copy of the current pixels, and blending a rectangle back toward such a copy (amount 1 = fully the copy).
  std::vector<uint8_t> Snapshot() const { return px_; }
  void BlendToward(const std::vector<uint8_t> &snapshot, int x0, int y0, int x1, int y1, float amount);

  void Clear(Color c);
  void FillRoundRect(float x0, float y0, float x1, float y1, float radius, Color c);
  // A border of `width` pixels just inside the rounded rectangle's edge.
  void StrokeRoundRect(float x0, float y0, float x1, float y1, float radius, float width, Color c);
  // A soft halo of `spread` pixels around the rounded rectangle, fading out from c.
  void Glow(float x0, float y0, float x1, float y1, float radius, float spread, Color c);
  void FillCircle(float cx, float cy, float r, Color c);
  // A line with round caps.
  void Line(float x0, float y0, float x1, float y1, float width, Color c);
  // An arc of a ring (angles in degrees, clockwise from 3 o'clock, start < end).
  void Arc(float cx, float cy, float r, float width, float startDeg, float endDeg, Color c);
  // A horizontal rainbow (hue 0..360 left to right) with rounded ends.
  void HueBar(float x0, float y0, float x1, float y1);
  // Draws UTF-8 text with its baseline at y; returns the advance width.
  float Text(float x, float y, float size, const std::string &s, Color c, bool bold = false, Align align = Align::Left);
  float TextWidth(float size, const std::string &s, bool bold = false);
  // A Material Icons glyph (by code point) filling a size x size box centred on (cx, cy), turned rotateDeg clockwise.
  void Icon(float cx, float cy, float size, int codepoint, Color c, float rotateDeg = 0);
  // Draws an image 1:1 with its top-left corner at (x, y), blended by its alpha times opacity.
  void DrawImage(int x, int y, const Image &img, float opacity = 1);

 private:
  struct Glyph {
    int w = 0, h = 0, xoff = 0, yoff = 0;
    std::vector<uint8_t> bitmap;
  };
  struct Font;
  void Blend(int x, int y, Color c, float coverage);
  const Glyph &GetGlyph(int font, float size, int codepoint);  // size: pixel height (text) or em size (icons)

  int w_, h_;
  std::vector<uint8_t> px_;
  Font *fonts_[3] = {};  // regular, bold, icons
  std::map<std::tuple<int, int, int>, Glyph> glyphs_;  // (font, size*4, codepoint)
};

}  // namespace framepip
