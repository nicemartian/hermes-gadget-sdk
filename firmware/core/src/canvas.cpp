#include "hg/canvas.hpp"

#include "hg/skin.hpp"

#include <algorithm>
#include <cmath>

#include "hg/font.hpp"
#include "hg/font_cjk.hpp"

namespace hg {

void Canvas::set_clip_rows(int y0, int y1) {
  clip_y0_ = std::max(0, y0);
  clip_y1_ = std::min(h_, y1);
}

void Canvas::pixel(int x, int y, uint16_t color) {
  if (x < 0 || x >= w_ || y < clip_y0_ || y >= clip_y1_ || y >= h_) return;
  if (y < base_y_ || y >= base_y_ + rows_) return;
  px_[(y - base_y_) * stride_ + x] = encode(color);
}

void Canvas::fill_rect(int x, int y, int w, int h, uint16_t color) {
  int x0 = std::max(0, x), x1 = std::min(w_, x + w);
  int y0 = std::max(clip_y0_, y), y1 = std::min(std::min(h_, clip_y1_), y + h);
  y0 = std::max(y0, base_y_);
  y1 = std::min(y1, base_y_ + rows_);
  if (x0 >= x1 || y0 >= y1) return;
  uint16_t c = encode(color);
  for (int yy = y0; yy < y1; ++yy) {
    std::fill(px_ + (yy - base_y_) * stride_ + x0, px_ + (yy - base_y_) * stride_ + x1, c);
  }
}

void Canvas::rect(int x, int y, int w, int h, uint16_t color) {
  fill_rect(x, y, w, 1, color);
  fill_rect(x, y + h - 1, w, 1, color);
  fill_rect(x, y, 1, h, color);
  fill_rect(x + w - 1, y, 1, h, color);
}

void Canvas::fill_round_rect(int x, int y, int w, int h, int r, uint16_t color) {
  r = std::min(r, std::min(w, h) / 2);
  if (r <= 0) {
    fill_rect(x, y, w, h, color);
    return;
  }
  fill_rect(x, y + r, w, h - 2 * r, color);
  const bool smooth = skin::text_on();
  for (int i = 0; i < r; ++i) {
    if (smooth) {
      // The quarter circle's edge at this row's middle: whole pixels filled, the last one mixed.
      const float dy = static_cast<float>(r - i) - 0.5f;
      const float edge = static_cast<float>(r) - std::sqrt(static_cast<float>(r * r) - dy * dy);
      const int inset = static_cast<int>(std::ceil(edge));
      const int a = static_cast<int>((static_cast<float>(inset) - edge) * 15.0f + 0.5f);
      for (int row : {y + i, y + h - 1 - i}) {
        fill_rect(x + inset, row, w - 2 * inset, 1, color);
        if (inset > 0) {
          blend(x + inset - 1, row, color, a);
          blend(x + w - inset, row, color, a);
        }
      }
      continue;
    }
    // Inset of row i from the top/bottom edge for a quarter circle of radius r.
    int dy = r - i;
    int inset = 0;
    while (inset < r && (r - inset) * (r - inset) + dy * dy > r * r) ++inset;
    fill_rect(x + inset, y + i, w - 2 * inset, 1, color);
    fill_rect(x + inset, y + h - 1 - i, w - 2 * inset, 1, color);
  }
}

void Canvas::fill_circle(int cx, int cy, int r, uint16_t color) {
  if (skin::text_on()) {
    // Coverage from the distance to the edge, half a pixel either side of radius r + 0.5.
    const float R = static_cast<float>(r) + 0.5f;
    for (int dy = -r - 1; dy <= r + 1; ++dy) {
      for (int dx = -r - 1; dx <= r + 1; ++dx) {
        const float d = R - std::sqrt(static_cast<float>(dx * dx + dy * dy));
        if (d <= -0.5f) continue;
        blend(cx + dx, cy + dy, color, d >= 0.5f ? 15 : static_cast<int>((d + 0.5f) * 15.0f + 0.5f));
      }
    }
    return;
  }
  for (int dy = -r; dy <= r; ++dy) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= r * r) ++dx;
    fill_rect(cx - dx, cy + dy, 2 * dx + 1, 1, color);
  }
}

void Canvas::glow_box(int cx, int cy, int w, int h, int r, int glow, uint16_t color) {
  const float hw = static_cast<float>(w) / 2, hh = static_cast<float>(h) / 2;
  const float rr = std::min(static_cast<float>(r), std::min(hw, hh));
  const float g = static_cast<float>(std::max(1, glow));
  for (int y = cy - h / 2 - glow - 1; y <= cy + h / 2 + glow + 1; ++y) {
    for (int x = cx - w / 2 - glow - 1; x <= cx + w / 2 + glow + 1; ++x) {
      // Signed distance to the rounded box: negative inside.
      const float qx = std::fabs(static_cast<float>(x - cx)) - (hw - rr);
      const float qy = std::fabs(static_cast<float>(y - cy)) - (hh - rr);
      const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
      const float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - rr;
      if (d >= g) continue;
      int a = 15;
      if (d > -0.5f) {
        const float edge = d < 0.5f ? 0.5f - d : 0.0f;  // the box's own edge
        const float t = 1.0f - std::max(d, 0.0f) / g;   // the halo, fading out
        a = static_cast<int>(std::min(1.0f, edge + 0.34f * t * t * t) * 15.0f + 0.5f);
      }
      blend(x, y, color, a);
    }
  }
}

void Canvas::ring(int cx, int cy, int r, int thickness, uint16_t color) {
  int inner = std::max(0, r - thickness);
  for (int dy = -r; dy <= r; ++dy) {
    for (int dx = -r; dx <= r; ++dx) {
      int d2 = dx * dx + dy * dy;
      if (d2 <= r * r && d2 > inner * inner) pixel(cx + dx, cy + dy, color);
    }
  }
}

void Canvas::arc(int cx, int cy, int r, int thickness, int dir, uint16_t color) {
  int inner = std::max(0, r - thickness);
  for (int dy = -r; dy <= r; ++dy) {
    int ady = dy < 0 ? -dy : dy;
    for (int dx = -r; dx <= r; ++dx) {
      int d2 = dx * dx + dy * dy;
      if (d2 > r * r || d2 <= inner * inner) continue;
      if (dir * dx * 10 < ady * 7) continue;  // outside the +-55 degree cone
      pixel(cx + dx, cy + dy, color);
    }
  }
}

void Canvas::bitmap1(int x, int y, int w, int h, const uint8_t* bits, uint16_t color) {
  const int stride = (w + 7) / 8;
  const uint16_t c = encode(color);
  int y0, y1;
  if (row_window(y0, y1)) return;
  y0 = std::max(y0, y);
  y1 = std::min(y1, y + h);
  for (int yy = y0; yy < y1; ++yy) {
    const uint8_t* row = bits + (yy - y) * stride;
    for (int col = 0; col < w; ++col) {
      int xx = x + col;
      if (xx < 0 || xx >= w_) continue;
      if ((row[col >> 3] >> (7 - (col & 7))) & 1) px_[(yy - base_y_) * stride_ + xx] = c;
    }
  }
}

void Canvas::blend(int x, int y, uint16_t color, int a) {
  if (a <= 0 || x < 0 || x >= w_ || y < clip_y0_ || y >= clip_y1_ || y >= h_ || y < base_y_ || y >= base_y_ + rows_) return;
  uint16_t& px = px_[(y - base_y_) * stride_ + x];
  if (a >= 15) {
    px = encode(color);
    return;
  }
  const uint16_t d = encode(px);  // the swap is its own inverse
  const int r = ((d >> 11) * (15 - a) + (color >> 11) * a + 7) / 15;
  const int g = (((d >> 5) & 63) * (15 - a) + ((color >> 5) & 63) * a + 7) / 15;
  const int b = ((d & 31) * (15 - a) + (color & 31) * a + 7) / 15;
  px = encode(static_cast<uint16_t>((r << 11) | (g << 5) | b));
}

int Canvas::text(int x, int y, std::string_view s, int scale, uint16_t color) {
  if (scale < 1) scale = 1;
  const skin::Font* f = skin::text_on() ? skin::font_for(scale) : nullptr;
  if (!f) return text_pixel(x, y, s, scale, color);
  // The capitals' middle sits where the pixel glyph's does: 3.5 rows down its 7.
  const int top = y + font::kAsciiYOffset * scale + (font::kGlyphHeight * scale) / 2 - f->mid;
  for (size_t i = 0; i < s.size();) {
    const size_t start = i;
    const char32_t cp = font::next(s, i);
    const skin::Glyph* g = skin::glyph(*f, cp);
    if (!g) {  // no smooth glyph (CJK, a letter at the digits' size): the pixel font's
      x = text_pixel(x, y, s.substr(start, i - start), scale, color);
      continue;
    }
    const int stride = (g->w + 1) / 2;
    for (int gy = 0; gy < g->h; ++gy) {
      const uint8_t* row = f->bits + g->off + gy * stride;
      for (int gx = 0; gx < g->w; ++gx) blend(x + g->x + gx, top + g->y + gy, color, gx & 1 ? row[gx / 2] & 15 : row[gx / 2] >> 4);
    }
    x += g->adv;
  }
  return x;
}

int Canvas::text_pixel(int x, int y, std::string_view s, int scale, uint16_t color) {
  for (size_t i = 0; i < s.size();) {
    const char32_t cp = font::next(s, i);
    if (font::columns(cp) == 2) {
      // Wide characters are 12x12, filling the whole line pitch.
      if (const uint8_t* g = font::cjk_glyph(cp)) {
        for (int bit = 0; bit < font::kCjkSize * font::kCjkSize; ++bit) {
          if ((g[bit >> 3] >> (7 - (bit & 7))) & 1) {
            fill_rect(x + (bit % font::kCjkSize) * scale, y + (bit / font::kCjkSize) * scale, scale, scale, color);
          }
        }
      } else {
        text(x + 3 * scale, y, "?", scale, color);  // no glyph: a '?' centred in the two columns
      }
      x += 2 * font::kCellWidth * scale;
      continue;
    }
    const char* g = font::glyph(cp);
    const int y0 = y + font::kAsciiYOffset * scale;
    for (int row = 0; row < font::kGlyphHeight; ++row) {
      for (int col = 0; col < font::kGlyphWidth; ++col) {
        if (g[row * font::kGlyphWidth + col] == '#') {
          fill_rect(x + col * scale, y0 + row * scale, scale, scale, color);
        }
      }
    }
    x += font::kCellWidth * scale;
  }
  return x;
}

int Canvas::text_width(std::string_view s, int scale) {
  if (s.empty()) return 0;
  if (const skin::Font* f = skin::text_on() ? skin::font_for(scale < 1 ? 1 : scale) : nullptr) {
    int w = 0;
    for (size_t i = 0; i < s.size();) {
      const char32_t cp = font::next(s, i);
      const skin::Glyph* g = skin::glyph(*f, cp);
      w += g ? g->adv : font::columns(cp) * font::kCellWidth * scale;
    }
    return w;
  }
  return static_cast<int>(font::length(s)) * font::kCellWidth * scale - scale;
}

int Canvas::line_height(int scale) { return font::kCellHeight * scale; }

void Canvas::blit(int x, int y, int w, int h, const uint16_t* src) {
  for (int row = 0; row < h; ++row) {
    int yy = y + row;
    if (yy < clip_y0_ || yy >= clip_y1_ || yy >= h_ || yy < 0) continue;
    if (yy < base_y_ || yy >= base_y_ + rows_) continue;
    for (int col = 0; col < w; ++col) {
      int xx = x + col;
      if (xx < 0 || xx >= w_) continue;
      px_[(yy - base_y_) * stride_ + xx] = encode(src[row * w + col]);
    }
  }
}

void Canvas::picture(int x, int y, int size, const uint16_t* src, int src_px, const uint16_t* band, int band_y,
                     int band_rows) {
  for (int row = 0; row < size; ++row) {
    int yy = y + row;
    if (yy < clip_y0_ || yy >= clip_y1_ || yy >= h_ || yy < 0) continue;
    if (yy < base_y_ || yy >= base_y_ + rows_) continue;
    const int src_row = row * src_px / size, in_band = src_row - band_y;
    const uint16_t* line = in_band >= 0 && in_band < band_rows ? band + in_band * src_px : src + src_row * src_px;
    for (int col = 0; col < size; ++col) {
      int xx = x + col;
      uint16_t c = line[col * src_px / size];
      if (xx < 0 || xx >= w_ || !c) continue;
      px_[(yy - base_y_) * stride_ + xx] = encode(c);
    }
  }
}

// Widths are in code points (font::length): a UTF-8 letter is one column.
namespace {

// A wrap unit: a run of narrow characters, or one wide character, which may break anywhere.
struct Unit {
  std::string_view text;
  size_t cols;
  bool space_before;  // separated from the previous unit by a space
};

std::vector<Unit> split_units(std::string_view para) {
  std::vector<Unit> units;
  bool space = false;
  size_t i = 0;
  while (i < para.size()) {
    if (para[i] == ' ') {
      space = true;
      ++i;
      continue;
    }
    const size_t start = i;
    size_t j = i;
    if (font::columns(font::next(para, j)) == 2) {
      units.push_back({para.substr(start, j - start), 2, space});
      i = j;
    } else {
      size_t cols = 0;
      while (i < para.size() && para[i] != ' ') {
        size_t k = i;
        if (font::columns(font::next(para, k)) == 2) break;
        ++cols;
        i = k;
      }
      units.push_back({para.substr(start, i - start), cols, space});
    }
    space = false;
  }
  return units;
}

}  // namespace

namespace {

// wrap_text for the smooth fonts: the same greedy wrap, measured by their widths
// (skin::text_units) instead of counted columns.
std::vector<std::string> wrap_smooth(std::string_view text, int max_chars) {
  std::vector<std::string> lines;
  const int max = std::max(1, max_chars) * 63;  // a little under 64: other scales round differently
  const int space = skin::text_units(" ");
  size_t i = 0;
  while (i <= text.size()) {
    const size_t nl = text.find('\n', i);
    std::string_view para = text.substr(i, nl == std::string_view::npos ? std::string_view::npos : nl - i);
    std::string line;
    int used = 0;
    size_t p = 0;
    while (p < para.size()) {
      if (para[p] == ' ') {
        ++p;
        continue;
      }
      size_t q = p;
      while (q < para.size() && para[q] != ' ') ++q;
      std::string_view word = para.substr(p, q - p);
      p = q;
      int units = skin::text_units(word);
      while (units > max) {  // a word longer than a line is cut where the line ends
        if (!line.empty()) {
          lines.push_back(line);
          line.clear();
          used = 0;
        }
        size_t cut = 0;
        int fit = 0;
        while (cut < word.size()) {
          size_t next = cut;
          font::next(word, next);
          const int u = skin::text_units(word.substr(cut, next - cut));
          if (fit + u > max && cut) break;
          fit += u;
          cut = next;
        }
        lines.emplace_back(word.substr(0, cut));
        word.remove_prefix(cut);
        units = skin::text_units(word);
      }
      if (word.empty()) continue;
      const int gap = line.empty() ? 0 : space;
      if (used + gap + units > max && !line.empty()) {
        lines.push_back(line);
        line.clear();
        used = 0;
      } else if (gap) {
        line.push_back(' ');
        used += gap;
      }
      line.append(word);
      used += units;
    }
    lines.push_back(line);
    if (nl == std::string_view::npos) break;
    i = nl + 1;
  }
  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  return lines;
}

}  // namespace

// Widths are in display columns (font::length): narrow letters take one, wide characters two.
std::vector<std::string> wrap_text(std::string_view text, int max_chars) {
  if (skin::text_on()) return wrap_smooth(text, max_chars);
  std::vector<std::string> lines;
  const size_t max = static_cast<size_t>(max_chars < 1 ? 1 : max_chars);
  size_t i = 0;
  while (i <= text.size()) {
    size_t nl = text.find('\n', i);
    std::string_view para = text.substr(i, nl == std::string_view::npos ? std::string_view::npos : nl - i);
    std::string line;
    size_t line_cols = 0;
    for (const Unit& u : split_units(para)) {
      std::string_view word = u.text;
      size_t cols = u.cols;
      // A narrow word longer than a line is cut at the line width.
      while (cols > max) {
        if (!line.empty()) {
          lines.push_back(line);
          line.clear();
          line_cols = 0;
        }
        const size_t cut = font::prefix(word, max);
        lines.emplace_back(word.substr(0, cut));
        word.remove_prefix(cut);
        cols = font::length(word);
      }
      if (word.empty()) continue;
      // CJK text has no spaces: only narrow words are joined with one.
      const size_t gap = (!line.empty() && u.space_before) ? 1 : 0;
      if (line_cols + gap + cols > max && !line.empty()) {
        lines.push_back(line);
        line.clear();
        line_cols = 0;
      } else if (gap) {
        line.push_back(' ');
        ++line_cols;
      }
      line.append(word);
      line_cols += cols;
    }
    lines.push_back(line);
    if (nl == std::string_view::npos) break;
    i = nl + 1;
  }
  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  return lines;
}

}  // namespace hg
