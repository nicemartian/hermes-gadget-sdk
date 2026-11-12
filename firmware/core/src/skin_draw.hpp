// Drawing for the Eyes skin's own screens (skin_eyes.cpp, skin_views.cpp): light added to a
// black RGB565 panel with 8-bit coverage, soft shapes, and the skin's fonts. Internal to core.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "hg/skin.hpp"

namespace hg::skin {

struct Rgb {
  uint8_t r, g, b;
};
inline constexpr Rgb kGold{255, 196, 107}, kCyan{107, 228, 255}, kViolet{165, 139, 255};
inline constexpr Rgb kFg{236, 232, 241}, kMuted{143, 136, 156}, kWhite{255, 255, 255};

struct Surface {
  uint16_t* fb;
  int w, h;
  bool swap;

  uint16_t get(int i) const {
    const uint16_t v = fb[i];
    return swap ? static_cast<uint16_t>((v >> 8) | (v << 8)) : v;
  }
  void put(int i, uint16_t v) { fb[i] = swap ? static_cast<uint16_t>((v >> 8) | (v << 8)) : v; }
  // Adds `c` at coverage a (0..255) to the pixel, saturating: light over a black panel.
  void add(int x, int y, Rgb c, int a) {
    if (a <= 0 || x < 0 || y < 0 || x >= w || y >= h) return;
    const int i = y * w + x;
    const uint16_t v = get(i);
    const int r = std::min(31, (v >> 11) + ((c.r * a) >> 11));
    const int g = std::min(63, ((v >> 5) & 63) + ((c.g * a) >> 10));
    const int b = std::min(31, (v & 31) + ((c.b * a) >> 11));
    put(i, static_cast<uint16_t>((r << 11) | (g << 5) | b));
  }
  void clear_run(int y, int x0, int x1) {
    if (y < 0 || y >= h) return;
    x0 = std::max(0, x0);
    x1 = std::min(w, x1);
    if (x0 < x1) std::memset(fb + y * w + x0, 0, static_cast<size_t>(x1 - x0) * 2);
  }
};

inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// A square root from float multiplies only (one Newton step on the bit-trick estimate): the
// ESP32-S3's FPU has no square root, and sqrtf in software was most of a frame. Within 0.2%,
// under a twentieth of a pixel at the distances drawn here.
inline float fast_sqrt(float v) {
  if (v <= 0) return 0;
  uint32_t i;
  std::memcpy(&i, &v, 4);
  i = 0x5f3759dfu - (i >> 1);
  float r;
  std::memcpy(&r, &i, 4);
  r *= 1.5f - 0.5f * v * r * r;
  return v * r;
}

// A rounded box (a capsule when its radius is half its short side) with a soft glow around it.
// `glow` 0 draws only the shape.
inline void glow_box(Surface& s, float cx, float cy, float w, float h, Rgb c, float alpha, float glow) {
  const float hw = w / 2, hh = h / 2, rad = std::min(hw, hh);
  const int x0 = std::max(0, static_cast<int>(std::floor(cx - hw - glow))), x1 = std::min(s.w - 1, static_cast<int>(std::ceil(cx + hw + glow)));
  const int y0 = std::max(0, static_cast<int>(std::floor(cy - hh - glow))), y1 = std::min(s.h - 1, static_cast<int>(std::ceil(cy + hh + glow)));
  const float inv_glow = glow > 0 ? 1 / glow : 0;
  const int full = static_cast<int>(alpha * 255);
  for (int y = y0; y <= y1; ++y) {
    const float qy = std::fabs(y + 0.5f - cy) - (hh - rad);
    const float oy = std::max(qy, 0.0f);
    for (int x = x0; x <= x1; ++x) {
      const float qx = std::fabs(x + 0.5f - cx) - (hw - rad);
      // The distance to the edge. Only the rounded corners need the root: beside a straight
      // side it is the distance along one axis.
      float d;
      if (qx > 0 && qy > 0) {
        const float d2 = qx * qx + oy * oy, reach = rad + glow;
        if (d2 >= reach * reach) continue;
        d = fast_sqrt(d2) - rad;
      } else {
        d = std::max(qx, qy) - rad;
      }
      if (d <= -0.5f) {  // inside
        s.add(x, y, c, full);
        continue;
      }
      float a = d < 0.5f ? 0.5f - d : 0.0f;
      if (d < glow) {
        const float g = 1 - std::max(d, 0.0f) * inv_glow;
        a += (1 - a) * g * g * 0.42f;
      }
      if (a > 0) s.add(x, y, c, static_cast<int>(a * alpha * 255));
    }
  }
}

// A line with round ends, `width` wide.
inline void capsule_line(Surface& s, float ax, float ay, float bx, float by, float width, Rgb c, float alpha) {
  const float r = width / 2;
  const int x0 = static_cast<int>(std::floor(std::min(ax, bx) - r - 1)), x1 = static_cast<int>(std::ceil(std::max(ax, bx) + r + 1));
  const int y0 = static_cast<int>(std::floor(std::min(ay, by) - r - 1)), y1 = static_cast<int>(std::ceil(std::max(ay, by) + r + 1));
  const float dx = bx - ax, dy = by - ay, len2 = std::max(dx * dx + dy * dy, 1e-6f);
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) {
      const float px = x + 0.5f - ax, py = y + 0.5f - ay;
      const float t = clamp01((px * dx + py * dy) / len2);
      const float ex = px - t * dx, ey = py - t * dy;
      const float a = clamp01(0.5f - (std::sqrt(ex * ex + ey * ey) - r));
      if (a > 0) s.add(x, y, c, static_cast<int>(a * alpha * 255));
    }
}

inline void disc(Surface& s, float cx, float cy, float r, Rgb c, float alpha, float glow = 0) {
  glow_box(s, cx, cy, 2 * r, 2 * r, c, alpha, glow);
}

// --- text ---

inline char32_t next_cp(std::string_view s, size_t& i) {
  const auto b = static_cast<unsigned char>(s[i++]);
  if (b < 0x80) return b;
  int n = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
  char32_t cp = b & (0x3F >> n);
  while (n-- > 0 && i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
  return cp;
}

inline const Glyph* find(const Font& f, char32_t cp) {
  const Glyph* g = glyph(f, cp);
  return g || cp == '?' ? g : glyph(f, '?');
}

inline int text_width(const Font& f, std::string_view t) {
  int w = 0;
  for (size_t i = 0; i < t.size();)
    if (const Glyph* g = find(f, next_cp(t, i))) w += g->adv;
  return w;
}

inline void draw_text(Surface& s, const Font& f, int x, int y, std::string_view t, Rgb c, float alpha) {
  for (size_t i = 0; i < t.size();) {
    const Glyph* g = find(f, next_cp(t, i));
    if (!g) continue;
    const int stride = (g->w + 1) / 2;
    for (int gy = 0; gy < g->h; ++gy) {
      const uint8_t* row = f.bits + g->off + gy * stride;
      for (int gx = 0; gx < g->w; ++gx) {
        const int a = gx & 1 ? row[gx / 2] & 15 : row[gx / 2] >> 4;
        if (a) s.add(x + g->x + gx, y + g->y + gy, c, static_cast<int>(a * 17 * alpha));
      }
    }
    x += g->adv;
  }
}

// `t` cut to `max_w` pixels, with an ellipsis when it was cut.
inline std::string fit(const Font& f, std::string_view t, int max_w) {
  if (text_width(f, t) <= max_w) return std::string(t);
  const std::string dots = "\xE2\x80\xA6";
  std::string out;
  for (size_t i = 0; i < t.size();) {
    const size_t start = i;
    next_cp(t, i);
    const std::string_view grown = t.substr(0, i);
    if (text_width(f, grown) + text_width(f, dots) > max_w) {
      out = std::string(t.substr(0, start));
      break;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out + dots;
}

// Greedy word wrap by pixel width; '\n' breaks a line.
inline std::vector<std::string> wrap(const Font& f, std::string_view t, int max_w) {
  std::vector<std::string> lines;
  std::string line;
  size_t i = 0;
  auto flush = [&] {
    lines.push_back(line);
    line.clear();
  };
  while (i <= t.size()) {
    if (i == t.size() || t[i] == '\n') {
      flush();
      ++i;
      continue;
    }
    if (t[i] == ' ') {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < t.size() && t[j] != ' ' && t[j] != '\n') ++j;
    const std::string word(t.substr(i, j - i));
    const std::string joined = line.empty() ? word : line + " " + word;
    if (text_width(f, joined) <= max_w || line.empty()) {
      line = joined;
    } else {
      flush();
      line = word;
    }
    i = j;
  }
  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  return lines;
}

inline uint32_t mix(uint32_t h, uint32_t v) {
  h ^= v + 0x9E3779B9u + (h << 6) + (h >> 2);
  return h;
}
inline uint32_t mix(uint32_t h, std::string_view s) {
  for (unsigned char ch : s) h = mix(h, ch);
  return mix(h, static_cast<uint32_t>(s.size()));
}

}  // namespace hg::skin
