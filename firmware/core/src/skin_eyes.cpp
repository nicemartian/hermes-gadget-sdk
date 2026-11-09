#include "hg/skin.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

#include "hg/ui.hpp"

namespace hg::skin {
namespace {

constexpr float kPi = 3.14159265f;
// Everything but the ring stays inside this radius, and the ring's band outside it, so each
// can be cleared and redrawn without touching the other.
constexpr int kInner = 191;
constexpr int kGlow = 24;  // how far an eye's glow reaches
constexpr int kTextTop = 288;  // the face stays above this row, the captions below it

struct Rgb {
  uint8_t r, g, b;
};
constexpr Rgb kGold{255, 196, 107}, kCyan{107, 228, 255}, kViolet{165, 139, 255};
constexpr Rgb kFg{236, 232, 241}, kMuted{143, 136, 156}, kWhite{255, 255, 255};

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

float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// A rounded box (a capsule when its radius is half its short side) with a soft glow around it.
// `glow` 0 draws only the shape.
void glow_box(Surface& s, float cx, float cy, float w, float h, Rgb c, float alpha, float glow) {
  const float hw = w / 2, hh = h / 2, rad = std::min(hw, hh);
  const int x0 = static_cast<int>(std::floor(cx - hw - glow)), x1 = static_cast<int>(std::ceil(cx + hw + glow));
  const int y0 = static_cast<int>(std::floor(cy - hh - glow)), y1 = static_cast<int>(std::ceil(cy + hh + glow));
  for (int y = y0; y <= y1; ++y) {
    const float qy = std::fabs(y + 0.5f - cy) - (hh - rad);
    for (int x = x0; x <= x1; ++x) {
      const float qx = std::fabs(x + 0.5f - cx) - (hw - rad);
      const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
      const float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - rad;
      float a = clamp01(0.5f - d);
      if (a < 1 && glow > 0 && d < glow) {
        const float g = 1 - std::max(d, 0.0f) / glow;
        a += (1 - a) * g * g * 0.42f;
      }
      if (a > 0) s.add(x, y, c, static_cast<int>(a * alpha * 255));
    }
  }
}

// A line with round ends, `width` wide.
void capsule_line(Surface& s, float ax, float ay, float bx, float by, float width, Rgb c, float alpha) {
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

void disc(Surface& s, float cx, float cy, float r, Rgb c, float alpha, float glow = 0) {
  glow_box(s, cx, cy, 2 * r, 2 * r, c, alpha, glow);
}

// --- text ---

char32_t next_cp(std::string_view s, size_t& i) {
  const auto b = static_cast<unsigned char>(s[i++]);
  if (b < 0x80) return b;
  int n = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
  char32_t cp = b & (0x3F >> n);
  while (n-- > 0 && i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
  return cp;
}

const Glyph* find(const Font& f, char32_t cp) {
  int lo = 0, hi = f.count - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    if (f.glyphs[mid].cp == cp) return &f.glyphs[mid];
    if (f.glyphs[mid].cp < cp) lo = mid + 1;
    else hi = mid - 1;
  }
  return cp == '?' ? nullptr : find(f, '?');
}

int text_width(const Font& f, std::string_view t) {
  int w = 0;
  for (size_t i = 0; i < t.size();)
    if (const Glyph* g = find(f, next_cp(t, i))) w += g->adv;
  return w;
}

void draw_text(Surface& s, const Font& f, int x, int y, std::string_view t, Rgb c, float alpha) {
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
std::string fit(const Font& f, std::string_view t, int max_w) {
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
std::vector<std::string> wrap(const Font& f, std::string_view t, int max_w) {
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

uint32_t mix(uint32_t h, uint32_t v) {
  h ^= v + 0x9E3779B9u + (h << 6) + (h >> 2);
  return h;
}
uint32_t mix(uint32_t h, std::string_view s) {
  for (unsigned char ch : s) h = mix(h, ch);
  return mix(h, static_cast<uint32_t>(s.size()));
}

// A stand-in for the voice's level while Hermes speaks (the model carries none): 0..1.
float voice(float t) {
  const float v = 0.5f + 0.28f * std::sin(t * 7.1f) + 0.18f * std::sin(t * 13.7f + 1) + 0.12f * std::sin(t * 23.3f + 2);
  const float env = std::sin(t * 1.7f);
  return clamp01(v * (0.6f + 0.4f * env * env));
}

struct Target {
  Rgb acc;
  float ew, eh, gap, ey;
};

Target target_for(const UiModel& m) {
  switch (m.screen) {
    case Screen::Listening: return {kCyan, 52, 88, 118, 200};
    case Screen::Thinking: return {kViolet, 44, 60, 108, 196};
    case Screen::Responding:
      if (m.speaking) return {kGold, 50, 52, 116, 176};
      return {kGold, 46, 66, 112, 200};
    default: return {kGold, 46, 74, 112, 205};
  }
}

}  // namespace

bool Eyes::wants(const UiModel& m) {
  if (!m.hero || !m.yes.empty() || !m.no.empty() || m.qr || m.timer_permille >= 0) return false;
  switch (m.screen) {
    case Screen::Ready:
    case Screen::Listening:
    case Screen::Thinking:
    case Screen::Responding: return true;
    default: return false;
  }
}

std::vector<Rows> Eyes::draw(const UiModel& m, uint16_t* fb, int w, int h, bool swap, bool full) {
  Surface s{fb, w, h, swap};
  const float C = w / 2.0f;
  const int R = std::min(w, h) / 2;
  std::vector<Rows> dirty;
  auto mark = [&dirty, h](int y0, int y1) {
    y0 = std::max(0, y0);
    y1 = std::min(h, y1);
    if (y0 < y1) dirty.push_back({y0, y1});
  };

  if (band_w_ != w) {  // the ring band's runs per row, between kInner + 1 and the glass edge
    band_w_ = w;
    band_.assign(static_cast<size_t>(h) * 4, 0);
    for (int y = 0; y < h; ++y) {
      const float dy = y + 0.5f - h / 2.0f;
      const float out2 = static_cast<float>(R) * R - dy * dy;
      if (out2 <= 0) continue;
      const int xo = static_cast<int>(std::sqrt(out2));
      const float in2 = static_cast<float>(kInner + 1) * (kInner + 1) - dy * dy;
      const int xi = in2 > 0 ? static_cast<int>(std::ceil(std::sqrt(in2))) : 0;
      int16_t* b = &band_[static_cast<size_t>(y) * 4];
      b[0] = static_cast<int16_t>(C - xo);
      b[1] = static_cast<int16_t>(C - xi);
      b[2] = static_cast<int16_t>(C + xi);
      b[3] = static_cast<int16_t>(C + xo + 1);
    }
  }
  auto clear_band = [&](int y0, int y1) {
    for (int y = std::max(0, y0); y < std::min(h, y1); ++y) {
      const int16_t* b = &band_[static_cast<size_t>(y) * 4];
      s.clear_run(y, b[0], b[1]);
      s.clear_run(y, b[2], b[3]);
    }
  };
  auto clear_inner = [&](int y0, int y1) {  // rows of the disc inside the ring band
    for (int y = std::max(0, y0); y < std::min(h, y1); ++y) {
      const int16_t* b = &band_[static_cast<size_t>(y) * 4];
      if (b[3] > b[0]) s.clear_run(y, b[1], b[2]);
    }
  };

  // --- time and easing ---
  const bool first = full || !started_;
  if (first) {  // whatever was on the panel goes
    std::memset(fb, 0, static_cast<size_t>(w) * h * 2);
    mark(0, h);
  }
  const uint32_t steps = first ? 0 : std::min<uint32_t>(m.fine - last_frame_, 30);
  last_frame_ = m.fine;
  started_ = true;
  const uint32_t now_ms = m.fine * frame_ms_;
  const float t = now_ms / 1000.0f;
  const float dt = steps * frame_ms_ / 1000.0f;
  const bool move = m.animate;
  const float k = first || !move ? 1.0f : 1.0f - std::exp(-dt * 11.0f);
  const Target tg = target_for(m);
  const bool speaking = m.screen == Screen::Responding && m.speaking;

  ew_ += (tg.ew - ew_) * k;
  eh_ += (tg.eh - eh_) * k;
  gap_ += (tg.gap - gap_) * k;
  ey_ += (tg.ey - ey_) * k;
  mouth_ += ((speaking ? 1.0f : 0.0f) - mouth_) * k;
  const float target_acc[3] = {float(tg.acc.r), float(tg.acc.g), float(tg.acc.b)};
  for (int i = 0; i < 3; ++i) acc_[i] += (target_acc[i] - acc_[i]) * k;
  const float level_to = m.screen == Screen::Listening ? m.level / 100.0f : 0.0f;
  level_ += (level_to - level_) * (first || !move ? 1.0f : 1.0f - std::exp(-dt * 18.0f));

  auto rnd = [this] {  // 0..1
    rng_ = rng_ * 1664525u + 1013904223u;
    return (rng_ >> 8) / 16777216.0f;
  };
  if (move) {
    if (static_cast<int32_t>(now_ms - next_gaze_ms_) >= 0) {
      next_gaze_ms_ = now_ms + 1200 + static_cast<uint32_t>(rnd() * 2200);
      if (m.screen == Screen::Thinking) {
        gaze_x_ = -14 + rnd() * 28;
        gaze_y_ = -16;
      } else if (m.screen == Screen::Ready) {
        gaze_x_ = (rnd() - 0.5f) * 36;
        gaze_y_ = (rnd() - 0.5f) * 16;
      }
    }
    if (m.screen == Screen::Listening || m.screen == Screen::Responding) gaze_x_ = gaze_y_ = 0;
    if (static_cast<int32_t>(now_ms - next_blink_ms_) >= 0) {
      blink_at_ms_ = now_ms;
      blinking_ = true;
      next_blink_ms_ = now_ms + 2400 + static_cast<uint32_t>(rnd() * 3000);
    }
  } else {
    gaze_x_ = gaze_y_ = 0;
    blinking_ = false;
  }
  const float gk = first || !move ? 1.0f : 1.0f - std::exp(-dt * 7.0f);
  gx_ += (gaze_x_ - gx_) * gk;
  gy_ += (gaze_y_ - gy_) * gk;
  float blink = 1;
  if (blinking_) {
    const uint32_t bt = now_ms - blink_at_ms_;
    if (bt < 160) blink = std::fabs(static_cast<float>(bt) - 80) / 80 * 0.9f + 0.1f;
    else blinking_ = false;
  }
  const Rgb acc{static_cast<uint8_t>(acc_[0] + 0.5f), static_cast<uint8_t>(acc_[1] + 0.5f), static_cast<uint8_t>(acc_[2] + 0.5f)};

  // --- face: the eyes and, while speaking, the mouth ---
  {
    auto q = [](float v) { return static_cast<uint32_t>(static_cast<int>(std::lround(v * 2)) + 4096); };
    uint32_t key = 17;
    for (float v : {ew_, eh_ * blink, gap_, ey_ + gy_, gx_}) key = mix(key, q(v));
    key = mix(key, (uint32_t(acc.r) << 16) | (uint32_t(acc.g) << 8) | acc.b);
    key = mix(key, q(mouth_ * 40));
    if (mouth_ > 0.03f) key = mix(key, m.fine);
    if (first || key != face_key_) {
      face_key_ = key;
      const float eh = std::max(6.0f, eh_ * blink), cy = ey_ + gy_;
      Box box;
      box.x0 = static_cast<int>(C - gap_ / 2 + gx_ - ew_ / 2 - kGlow - 2);
      box.x1 = static_cast<int>(C + gap_ / 2 + gx_ + ew_ / 2 + kGlow + 3);
      box.y0 = static_cast<int>(cy - eh_ / 2 - kGlow - 2);  // the open eye's height: a blink does not shrink the box
      box.y1 = static_cast<int>(cy + eh_ / 2 + kGlow + 3);
      const float mouth_y = 254;
      if (mouth_ > 0.03f) box.y1 = std::max(box.y1, static_cast<int>(mouth_y + 20 + 10));
      const Box was = face_box_;
      const int y0 = was.empty() ? box.y0 : std::min(was.y0, box.y0), y1 = was.empty() ? box.y1 : std::max(was.y1, box.y1);
      clear_inner(y0, std::min(y1, kTextTop));
      for (int side : {-1, 1}) glow_box(s, C + side * gap_ / 2 + gx_, cy, ew_, eh, acc, 1.0f, kGlow);
      if (mouth_ > 0.03f) {
        const int n = 9;
        const float bw = 7, bg = 7, total = n * bw + (n - 1) * bg;
        for (int i = 0; i < n; ++i) {
          const float env = 1 - std::fabs(i - (n - 1) / 2.0f) / ((n - 1) / 2.0f) * 0.55f;
          const float bh = 6 + 34 * (move ? voice(t + i * 0.33f) : 0.4f) * env * mouth_;
          glow_box(s, C - total / 2 + i * (bw + bg) + bw / 2, mouth_y, bw, bh, acc, mouth_, 6);
        }
      }
      face_box_ = box;
      mark(y0, std::min(y1, kTextTop));
    }
  }

  // --- captions: the headline, the detail lines and the hint ---
  {
    uint32_t key = mix(mix(mix(23, m.headline), m.detail), m.hint);
    key = mix(mix(key, static_cast<uint32_t>(m.screen)), (uint32_t(m.caption_lines) << 1) | speaking);
    if (first || key != text_key_) {
      text_key_ = key;
      const int top = kTextTop, bottom = 404;
      clear_inner(top, bottom);
      auto chord = [&](int y0, int y1) {  // the width free of the ring over rows [y0, y1)
        int width = w;
        for (int y : {y0, y1 - 1}) {
          const float dy = std::fabs(y + 0.5f - h / 2.0f);
          const float c2 = static_cast<float>(kInner - 2) * (kInner - 2) - dy * dy;
          width = std::min(width, c2 > 0 ? static_cast<int>(2 * std::sqrt(c2)) : 0);
        }
        return width;
      };
      auto centred = [&](const Font& f, int y, const std::string& text, Rgb c, float a) {
        const std::string line = fit(f, text, chord(y, y + f.line));
        draw_text(s, f, static_cast<int>(C) - text_width(f, line) / 2, y, line, c, a);
      };
      int y = kTextTop + 2;
      centred(kHead, y, m.headline, kFg, 0.96f);
      y += kHead.line + 1;
      const int max_lines = std::max<int>(1, std::min<int>(m.caption_lines, 2));
      std::string detail = m.detail;
      if (speaking) std::replace(detail.begin(), detail.end(), '\n', ' ');
      if (detail == m.headline) detail.clear();  // a status that only repeats the headline
      std::vector<std::string> lines = wrap(kBody, detail, chord(y, y + max_lines * kBody.line));
      if (static_cast<int>(lines.size()) > max_lines) {
        if (speaking) {  // the reply's latest lines
          lines.erase(lines.begin(), lines.end() - max_lines);
        } else {
          lines.resize(static_cast<size_t>(max_lines));
          lines.back() += "\xE2\x80\xA6";
        }
      }
      for (const std::string& line : lines) {
        centred(kBody, y, line, speaking ? kFg : kMuted, speaking ? 0.96f : 1.0f);
        y += kBody.line;
      }
      if (!m.hint.empty()) centred(kHint, 374, m.hint, kMuted, 0.8f);
      mark(top, bottom);
    }
  }

  // --- the ring at the glass edge ---
  {
    Ring ring = Ring::Still;
    if (move) {
      if (m.screen == Screen::Listening) ring = Ring::Pulse;
      else if (m.screen == Screen::Thinking || (m.screen == Screen::Responding && !speaking)) ring = Ring::Comet;
      else if (speaking) ring = Ring::Ticks;
    }
    uint32_t key = mix(29, static_cast<uint32_t>(ring));
    key = mix(key, (uint32_t(acc.r) << 16) | (uint32_t(acc.g) << 8) | acc.b);
    if (ring == Ring::Pulse) key = mix(key, static_cast<uint32_t>(level_ * 200));
    if (ring == Ring::Comet || ring == Ring::Ticks) key = mix(key, m.fine);
    if (first || key != ring_key_) {
      const bool same_comet = !first && ring == Ring::Comet && ring_ == Ring::Comet;
      ring_key_ = key;
      ring_ = ring;
      const float cy = h / 2.0f;
      if (ring == Ring::Comet) {
        const float head = t * 3.2f, rr = R - 12.0f;
        Box box{w, h, 0, 0};
        auto grow = [&box](float x, float y, float r) {
          box.x0 = std::min(box.x0, static_cast<int>(x - r - 1));
          box.x1 = std::max(box.x1, static_cast<int>(x + r + 2));
          box.y0 = std::min(box.y0, static_cast<int>(y - r - 1));
          box.y1 = std::max(box.y1, static_cast<int>(y + r + 2));
        };
        const int n = 44;
        for (int i = 0; i <= n; ++i) grow(C + std::cos(head - i * 1.7f / n) * rr, cy + std::sin(head - i * 1.7f / n) * rr, 16);
        const int y0 = same_comet ? std::min(box.y0, comet_box_.y0) : 0, y1 = same_comet ? std::max(box.y1, comet_box_.y1) : h;
        clear_band(y0, y1);
        for (int i = n; i >= 1; --i) {
          const float f = static_cast<float>(i) / n, a = head - f * 1.7f;
          disc(s, C + std::cos(a) * rr, cy + std::sin(a) * rr, 3.6f * (1 - f * 0.6f), acc, (1 - f) * (1 - f));
        }
        disc(s, C + std::cos(head) * rr, cy + std::sin(head) * rr, 4.5f, acc, 1.0f, 12);
        disc(s, C + std::cos(head) * rr, cy + std::sin(head) * rr, 3.0f, kWhite, 0.8f);
        comet_box_ = box;
        mark(y0, y1);
      } else {
        clear_band(0, h);
        if (ring == Ring::Still) {
          const float rr = R - 8.0f;
          for (int y = 0; y < h; ++y) {
            const int16_t* b = &band_[static_cast<size_t>(y) * 4];
            for (int run = 0; run < 2; ++run)
              for (int x = b[run * 2]; x < b[run * 2 + 1]; ++x) {
                const float d = std::fabs(std::hypot(x + 0.5f - C, y + 0.5f - cy) - rr) - 1.0f;
                const float a = clamp01(0.5f - d);
                if (a > 0) s.add(x, y, acc, static_cast<int>(a * 0.3f * 255));
              }
          }
        } else if (ring == Ring::Pulse) {
          const float rr = R - 14.0f, half = (4 + 8 * level_) / 2, glow = 8 + 12 * level_, alpha = 0.6f + 0.4f * level_;
          for (int y = 0; y < h; ++y) {
            const int16_t* b = &band_[static_cast<size_t>(y) * 4];
            for (int run = 0; run < 2; ++run)
              for (int x = b[run * 2]; x < b[run * 2 + 1]; ++x) {
                const float d = std::fabs(std::hypot(x + 0.5f - C, y + 0.5f - cy) - rr) - half;
                if (d >= glow) continue;
                float a = clamp01(0.5f - d);
                const float g = 1 - std::max(d, 0.0f) / glow;
                a += (1 - a) * g * g * 0.5f;
                s.add(x, y, acc, static_cast<int>(a * alpha * 255));
              }
          }
        } else if (ring == Ring::Ticks) {
          const int n = 96;
          const float ro = R - 4.0f;
          for (int i = 0; i < n; ++i) {
            const float a = i * 2 * kPi / n - kPi / 2;
            const float wob = std::sin(i * 0.4f + t * 2);
            const float len = 4 + 18 * voice(t + i * 0.21f) * (0.5f + 0.5f * wob * wob);
            const float ca = std::cos(a), sa = std::sin(a);
            capsule_line(s, C + ca * ro, cy + sa * ro, C + ca * (ro - len), cy + sa * (ro - len), 3.0f, acc, 0.35f + 0.65f * len / 22);
          }
        }
        mark(0, h);
      }
    }
  }

  std::sort(dirty.begin(), dirty.end(), [](const Rows& a, const Rows& b) { return a.y0 < b.y0; });
  std::vector<Rows> merged;
  for (const Rows& r : dirty) {
    if (!merged.empty() && r.y0 <= merged.back().y1) merged.back().y1 = std::max(merged.back().y1, r.y1);
    else merged.push_back(r);
  }
  return merged;
}

}  // namespace hg::skin
