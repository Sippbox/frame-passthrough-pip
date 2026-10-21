#include "canvas.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "settings.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb/stb_image.h"

namespace framepip {

struct Canvas::Font {
  std::vector<unsigned char> data;
  stbtt_fontinfo info{};
};

namespace {

std::vector<unsigned char> ReadFile(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<unsigned char>(std::istreambuf_iterator<char>(f), {});
}

std::string FcMatch(const char *pattern) {
  std::string cmd = std::string("fc-match -f '%{file}' '") + pattern + "' 2>/dev/null";
  std::string out;
  if (FILE *p = popen(cmd.c_str(), "r")) {
    char buf[512];
    while (fgets(buf, sizeof buf, p)) out += buf;
    pclose(p);
  }
  return out;
}

// Decodes one UTF-8 code point at s[*i], advancing *i.
int NextCodepoint(const std::string &s, size_t *i) {
  const unsigned char c = s[(*i)++];
  if (c < 0x80) return c;
  int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
  int cp = c & (0x3F >> n);
  while (n-- > 0 && *i < s.size()) cp = (cp << 6) | (s[(*i)++] & 0x3F);
  return cp;
}

}  // namespace

Canvas::Canvas(int w, int h) : w_(w), h_(h), px_((size_t)w * h * 4, 0) {}

Canvas::~Canvas() {
  for (Font *f : fonts_) delete f;
}

bool Canvas::LoadFonts(const std::string &fontDir, std::string *error) {
  auto load = [](const std::vector<std::string> &paths) -> Font * {
    for (const std::string &p : paths) {
      if (p.empty()) continue;
      Font *f = new Font;
      f->data = ReadFile(p);
      if (!f->data.empty() && stbtt_InitFont(&f->info, f->data.data(), stbtt_GetFontOffsetForIndex(f->data.data(), 0)))
        return f;
      delete f;
    }
    return nullptr;
  };
  fonts_[0] = load({fontDir + "/AtkinsonHyperlegible-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                    FcMatch("sans")});
  fonts_[1] = load({fontDir + "/AtkinsonHyperlegible-Bold.ttf", "/usr/share/fonts/noto/NotoSans-Bold.ttf",
                    FcMatch("sans:bold")});
  fonts_[2] = load({fontDir + "/MaterialIcons-Regular.ttf"});
  if (!fonts_[0]) {
    *error = "no usable font found (looked for Atkinson Hyperlegible in " + fontDir + ")";
    return false;
  }
  if (!fonts_[2]) {
    *error = "Material Icons font missing from " + fontDir;
    return false;
  }
  return true;
}

void Canvas::Clear(Color c) {
  for (size_t i = 0; i < px_.size(); i += 4) {
    px_[i] = c.r;
    px_[i + 1] = c.g;
    px_[i + 2] = c.b;
    px_[i + 3] = c.a;
  }
}

Image LoadImage(const std::string &path, int w, int h, std::string *error) {
  Image out;
  int sw = 0, sh = 0, channels = 0;
  unsigned char *src = stbi_load(path.c_str(), &sw, &sh, &channels, 4);
  if (!src) {
    *error = "can't read " + path + ": " + stbi_failure_reason();
    return out;
  }
  out.w = w, out.h = h;
  out.rgba.resize((size_t)w * h * 4);
  // each output pixel averages the source pixels it covers, weighted by alpha so transparent pixels don't darken edges
  for (int y = 0; y < h; y++) {
    const int y0 = y * sh / h, y1 = std::max(y0 + 1, (y + 1) * sh / h);
    for (int x = 0; x < w; x++) {
      const int x0 = x * sw / w, x1 = std::max(x0 + 1, (x + 1) * sw / w);
      double r = 0, g = 0, b = 0, a = 0;
      for (int sy = y0; sy < y1; sy++)
        for (int sx = x0; sx < x1; sx++) {
          const unsigned char *p = src + ((size_t)sy * sw + sx) * 4;
          r += p[0] * p[3], g += p[1] * p[3], b += p[2] * p[3], a += p[3];
        }
      uint8_t *o = &out.rgba[((size_t)y * w + x) * 4];
      const int n = (x1 - x0) * (y1 - y0);
      if (a > 0) o[0] = (uint8_t)(r / a + 0.5), o[1] = (uint8_t)(g / a + 0.5), o[2] = (uint8_t)(b / a + 0.5);
      o[3] = (uint8_t)(a / n + 0.5);
    }
  }
  stbi_image_free(src);
  return out;
}

void Canvas::DrawImage(int x, int y, const Image &img, float opacity) {
  if (opacity <= 0) return;
  for (int iy = 0; iy < img.h; iy++)
    for (int ix = 0; ix < img.w; ix++) {
      const uint8_t *p = &img.rgba[((size_t)iy * img.w + ix) * 4];
      if (p[3]) Blend(x + ix, y + iy, {p[0], p[1], p[2], p[3]}, opacity);
    }
}

void Canvas::BlendToward(const std::vector<uint8_t> &snapshot, int x0, int y0, int x1, int y1, float amount) {
  if (snapshot.size() != px_.size() || amount <= 0) return;
  const int a = (int)(std::min(amount, 1.f) * 256);
  for (int y = std::max(0, y0); y < std::min(h_, y1); y++)
    for (int x = std::max(0, x0) * 4; x < std::min(w_, x1) * 4; x++) {
      const size_t i = (size_t)y * w_ * 4 + x;
      px_[i] = (uint8_t)(px_[i] + (((int)snapshot[i] - px_[i]) * a >> 8));
    }
}

void Canvas::Blend(int x, int y, Color c, float coverage) {
  if (x < 0 || y < 0 || x >= w_ || y >= h_ || coverage <= 0) return;
  uint8_t *p = &px_[((size_t)y * w_ + x) * 4];
  const int a = (int)(std::min(1.f, coverage) * c.a + 0.5f);
  if (a >= 255) {
    p[0] = c.r, p[1] = c.g, p[2] = c.b, p[3] = 255;
    return;
  }
  p[0] = (uint8_t)(p[0] + ((c.r - p[0]) * a + 127) / 255);
  p[1] = (uint8_t)(p[1] + ((c.g - p[1]) * a + 127) / 255);
  p[2] = (uint8_t)(p[2] + ((c.b - p[2]) * a + 127) / 255);
  p[3] = (uint8_t)(p[3] + ((255 - p[3]) * a + 127) / 255);
}

namespace {

struct RoundRect {
  float x0, y0, x1, y1, r;
  RoundRect(float x0_, float y0_, float x1_, float y1_, float radius)
      : x0(x0_), y0(y0_), x1(x1_), y1(y1_), r(std::min({radius, (x1_ - x0_) / 2, (y1_ - y0_) / 2})) {}

  // Signed distance from (px, py) to the edge; negative inside.
  float Distance(float px, float py) const {
    const float qx = std::fabs(px - (x0 + x1) / 2) - ((x1 - x0) / 2 - r);
    const float qy = std::fabs(py - (y0 + y1) / 2) - ((y1 - y0) / 2 - r);
    return std::hypot(std::max(qx, 0.f), std::max(qy, 0.f)) + std::min(std::max(qx, qy), 0.f) - r;
  }
  // Cheap test: at least `m` pixels inside, away from the rounded corners (no distance needed).
  bool DeepInside(float px, float py, float m) const {
    return (px >= x0 + m && px <= x1 - m && py >= y0 + std::max(r, m) && py <= y1 - std::max(r, m)) ||
           (py >= y0 + m && py <= y1 - m && px >= x0 + std::max(r, m) && px <= x1 - std::max(r, m));
  }
};

}  // namespace

void Canvas::FillRoundRect(float x0, float y0, float x1, float y1, float radius, Color c) {
  const RoundRect rr(x0, y0, x1, y1, radius);
  for (int y = std::max(0, (int)std::floor(y0)); y < std::min(h_, (int)std::ceil(y1)); y++)
    for (int x = std::max(0, (int)std::floor(x0)); x < std::min(w_, (int)std::ceil(x1)); x++) {
      const float px = x + 0.5f, py = y + 0.5f;
      Blend(x, y, c, rr.DeepInside(px, py, 1) ? 1.f : std::clamp(0.5f - rr.Distance(px, py), 0.f, 1.f));
    }
}

void Canvas::StrokeRoundRect(float x0, float y0, float x1, float y1, float radius, float width, Color c) {
  const RoundRect rr(x0, y0, x1, y1, radius);
  for (int y = std::max(0, (int)std::floor(y0)); y < std::min(h_, (int)std::ceil(y1)); y++)
    for (int x = std::max(0, (int)std::floor(x0)); x < std::min(w_, (int)std::ceil(x1)); x++) {
      const float px = x + 0.5f, py = y + 0.5f;
      if (rr.DeepInside(px, py, width + 1)) continue;
      const float d = rr.Distance(px, py);
      // inside the shape (d <= 0) and within `width` of its edge
      Blend(x, y, c, std::clamp(0.5f - d, 0.f, 1.f) * std::clamp(d + width + 0.5f, 0.f, 1.f));
    }
}

void Canvas::Glow(float x0, float y0, float x1, float y1, float radius, float spread, Color c) {
  const RoundRect rr(x0, y0, x1, y1, radius);
  for (int y = std::max(0, (int)std::floor(y0 - spread)); y < std::min(h_, (int)std::ceil(y1 + spread)); y++)
    for (int x = std::max(0, (int)std::floor(x0 - spread)); x < std::min(w_, (int)std::ceil(x1 + spread)); x++) {
      const float px = x + 0.5f, py = y + 0.5f;
      if (rr.DeepInside(px, py, 0)) continue;
      const float d = rr.Distance(px, py);
      if (d <= 0 || d >= spread) continue;
      const float t = 1 - d / spread;
      Blend(x, y, c, t * t);
    }
}

void Canvas::FillCircle(float cx, float cy, float r, Color c) { FillRoundRect(cx - r, cy - r, cx + r, cy + r, r, c); }

void Canvas::Line(float x0, float y0, float x1, float y1, float width, Color c) {
  const float h = width / 2, dx = x1 - x0, dy = y1 - y0, len2 = std::max(dx * dx + dy * dy, 1e-6f);
  for (int y = (int)std::floor(std::min(y0, y1) - h - 1); y < (int)std::ceil(std::max(y0, y1) + h + 1); y++)
    for (int x = (int)std::floor(std::min(x0, x1) - h - 1); x < (int)std::ceil(std::max(x0, x1) + h + 1); x++) {
      const float px = x + 0.5f - x0, py = y + 0.5f - y0;
      const float t = std::clamp((px * dx + py * dy) / len2, 0.f, 1.f);
      Blend(x, y, c, std::clamp(0.5f - (std::hypot(px - t * dx, py - t * dy) - h), 0.f, 1.f));
    }
}

void Canvas::Arc(float cx, float cy, float r, float width, float startDeg, float endDeg, Color c) {
  const float h = width / 2;
  for (int y = (int)std::floor(cy - r - h - 1); y < (int)std::ceil(cy + r + h + 1); y++)
    for (int x = (int)std::floor(cx - r - h - 1); x < (int)std::ceil(cx + r + h + 1); x++) {
      const float px = x + 0.5f - cx, py = y + 0.5f - cy;
      float a = std::atan2(py, px) * 180 / (float)M_PI;
      if (a < startDeg) a += 360;
      if (a > endDeg) continue;
      Blend(x, y, c, std::clamp(0.5f - (std::fabs(std::hypot(px, py) - r) - h), 0.f, 1.f));
    }
}

void Canvas::HueBar(float x0, float y0, float x1, float y1) {
  const float r = (y1 - y0) / 2;
  for (int x = (int)std::floor(x0); x < (int)std::ceil(x1); x++) {
    float fr, fg, fb;
    HueToRgb(360.0 * std::clamp((x + 0.5f - x0) / (x1 - x0), 0.f, 0.999f), &fr, &fg, &fb);
    const Color col{(uint8_t)(fr * 255), (uint8_t)(fg * 255), (uint8_t)(fb * 255)};
    for (int y = (int)std::floor(y0); y < (int)std::ceil(y1); y++) {
      const float qx = std::max({x0 + r - (x + 0.5f), (x + 0.5f) - (x1 - r), 0.f}), qy = std::fabs(y + 0.5f - (y0 + r));
      Blend(x, y, col, std::clamp(0.5f - (std::hypot(qx, qy) - r), 0.f, 1.f));
    }
  }
}

const Canvas::Glyph &Canvas::GetGlyph(int font, float size, int codepoint) {
  const auto key = std::make_tuple(font, (int)std::lround(size * 4), codepoint);
  auto it = glyphs_.find(key);
  if (it != glyphs_.end()) return it->second;
  Glyph g;
  const stbtt_fontinfo &info = fonts_[font]->info;
  const float scale = font == 2 ? stbtt_ScaleForMappingEmToPixels(&info, size) : stbtt_ScaleForPixelHeight(&info, size);
  unsigned char *bmp = stbtt_GetCodepointBitmap(&info, scale, scale, codepoint, &g.w, &g.h, &g.xoff, &g.yoff);
  if (bmp) {
    g.bitmap.assign(bmp, bmp + (size_t)g.w * g.h);
    stbtt_FreeBitmap(bmp, nullptr);
  }
  return glyphs_.emplace(key, std::move(g)).first->second;
}

float Canvas::TextWidth(float size, const std::string &s, bool bold) {
  const int font = bold && fonts_[1] ? 1 : 0;
  if (!fonts_[font]) return 0;
  const stbtt_fontinfo &info = fonts_[font]->info;
  const float scale = stbtt_ScaleForPixelHeight(&info, size);
  float w = 0;
  int prev = 0;
  for (size_t i = 0; i < s.size();) {
    const int cp = NextCodepoint(s, &i);
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&info, cp, &adv, &lsb);
    if (prev) w += scale * stbtt_GetCodepointKernAdvance(&info, prev, cp);
    w += scale * adv;
    prev = cp;
  }
  return w;
}

float Canvas::Text(float x, float y, float size, const std::string &s, Color c, bool bold, Align align) {
  const int font = bold && fonts_[1] ? 1 : 0;
  if (!fonts_[font]) return 0;
  const float width = TextWidth(size, s, bold);
  if (align == Align::Center) x -= width / 2;
  if (align == Align::Right) x -= width;
  const stbtt_fontinfo &info = fonts_[font]->info;
  const float scale = stbtt_ScaleForPixelHeight(&info, size);
  float pen = x;
  int prev = 0;
  for (size_t i = 0; i < s.size();) {
    const int cp = NextCodepoint(s, &i);
    if (prev) pen += scale * stbtt_GetCodepointKernAdvance(&info, prev, cp);
    const Glyph &g = GetGlyph(font, size, cp);
    const int gx = (int)std::lround(pen) + g.xoff, gy = (int)std::lround(y) + g.yoff;
    for (int yy = 0; yy < g.h; yy++)
      for (int xx = 0; xx < g.w; xx++) {
        const uint8_t a = g.bitmap[(size_t)yy * g.w + xx];
        if (a) Blend(gx + xx, gy + yy, c, a / 255.f);
      }
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&info, cp, &adv, &lsb);
    pen += scale * adv;
    prev = cp;
  }
  return width;
}

void Canvas::Icon(float cx, float cy, float size, int codepoint, Color c, float rotateDeg) {
  if (!fonts_[2]) return;
  // Material Icons draws each icon in the em square above the baseline: the bitmap's origin sits at (xoff, yoff)
  // from the square's bottom-left corner.
  const Glyph &g = GetGlyph(2, size, codepoint);
  if (g.bitmap.empty()) return;
  const float ox = cx - size / 2 + g.xoff, oy = cy + size / 2 + g.yoff;  // bitmap's top-left on the canvas
  const float rad = rotateDeg * (float)M_PI / 180, cs = std::cos(rad), sn = std::sin(rad);
  auto sample = [&](float u, float v) {  // bilinear coverage at bitmap coordinates (u, v)
    u -= 0.5f, v -= 0.5f;
    const int u0 = (int)std::floor(u), v0 = (int)std::floor(v);
    const float fu = u - u0, fv = v - v0;
    auto at = [&](int x, int y) { return x < 0 || y < 0 || x >= g.w || y >= g.h ? 0.f : g.bitmap[(size_t)y * g.w + x] / 255.f; };
    return (at(u0, v0) * (1 - fu) + at(u0 + 1, v0) * fu) * (1 - fv) + (at(u0, v0 + 1) * (1 - fu) + at(u0 + 1, v0 + 1) * fu) * fv;
  };
  const float reach = size * 0.75f;  // covers the square at any rotation
  for (int y = (int)std::floor(cy - reach); y < (int)std::ceil(cy + reach); y++)
    for (int x = (int)std::floor(cx - reach); x < (int)std::ceil(cx + reach); x++) {
      // turn the pixel centre back by the rotation, about the icon's centre, into bitmap coordinates
      const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
      const float sx = cx + dx * cs + dy * sn, sy = cy - dx * sn + dy * cs;
      const float a = sample(sx - ox, sy - oy);
      if (a > 0.003f) Blend(x, y, c, a);
    }
}

}  // namespace framepip
