#include "hg/skin.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

#include "hg/i18n.hpp"
#include "hg/ui.hpp"
#include "skin_draw.hpp"

namespace hg::skin {
namespace {

constexpr float kPi = 3.14159265f;
// Everything but the ring stays inside this radius, and the ring's band outside it, so each
// can be cleared and redrawn without touching the other.
constexpr int kInner = 191;
constexpr int kGlow = 24;  // how far an eye's glow reaches
constexpr int kTextTop = 288;  // the face stays above this row, the captions below it
// The resting face: its captions stay this long after they change, then fade; a long quiet
// makes the eyes sleepy; a cheer is a smile this long.
constexpr uint32_t kCaptionMs = 6000, kCaptionFadeMs = 600, kSleepyMs = 120000, kCheerMs = 2500;
// The clock and a running timer: the eyes sit small at the top and the digits take the middle.
// The face stays above kCompactTop there; on the other faces the marks at the top stay above
// kChipsBottom, clear of the eyes.
constexpr int kCompactTop = 176, kChipsTop = 88, kChipsBottom = 116;
constexpr Rgb kMint{120, 235, 165}, kPill{38, 34, 46};
// A running timer's Cancel, under its label.
constexpr int kCancelY = 362, kCancelW = 128, kCancelH = 46;

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

Target target_for(const UiModel& m, bool sleepy, bool attention, bool compact) {
  if (m.screen == Screen::Alarm) return {kMint, 54, 86, 120, 200};  // time is up: wide open
  if (compact) {
    if (sleepy) return {Rgb{150, 115, 63}, 38, 16, 88, 122};
    if (attention) return {kGold, 40, 58, 92, 116};
    if (!m.timer_text.empty()) return {kCyan, 36, 44, 88, 120};  // watching the digits
    return {kGold, 36, 52, 88, 118};
  }
  if (sleepy) return {Rgb{150, 115, 63}, 46, 26, 112, 214};  // lids half down, dimmer
  if (attention) return {kGold, 54, 86, 120, 200};          // wide open
  switch (m.screen) {
    case Screen::Listening: return {kCyan, 52, 88, 118, 200};
    case Screen::Thinking:
      if (!m.detail.empty()) return {kViolet, 48, 36, 108, 198};  // at work with a tool: a squint
      return {kViolet, 44, 60, 108, 196};
    case Screen::Responding:
      if (m.speaking) return {kGold, 50, 52, 116, 176};
      return {kGold, 46, 66, 112, 200};
    default: return {kGold, 46, 74, 112, 205};
  }
}

// A smiling eye: a shallow arc open downwards, with the same soft edge and glow as the capsule eyes.
void smile(Surface& s, float cx, float cy, float r, float width, Rgb c, float glow) {
  const float half = width / 2, reach = r + half + glow;
  const float slope = 0.7f, ex = r * 0.819f, ey = -r * 0.574f;  // the ends, 35 degrees above level
  for (int y = static_cast<int>(cy - reach); y <= static_cast<int>(cy + ey + half + glow) + 1; ++y)
    for (int x = static_cast<int>(cx - reach); x <= static_cast<int>(cx + reach) + 1; ++x) {
      const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
      float d;
      if (-dy >= slope * std::fabs(dx)) {
        d = std::fabs(fast_sqrt(dx * dx + dy * dy) - r) - half;
      } else {
        const float qx = std::fabs(dx) - ex, qy = dy - ey;
        d = fast_sqrt(qx * qx + qy * qy) - half;
      }
      if (d >= glow) continue;
      float a = clamp01(0.5f - d);
      if (a < 1) {
        const float g = 1 - std::max(d, 0.0f) / glow;
        a += (1 - a) * g * g * 0.42f;
      }
      s.add(x, y, c, static_cast<int>(a * 255));
    }
}

// "TUE NOV 10" as the stock clock writes it, in the skin's quieter voice: "Tue Nov 10".
std::string soften(std::string text) {
  bool inside = false;
  for (char& c : text) {
    const bool upper = c >= 'A' && c <= 'Z';
    if (upper && inside) c = static_cast<char>(c - 'A' + 'a');
    inside = upper || (c >= 'a' && c <= 'z');
  }
  return text;
}

Rgb tone_rgb(DashRow::Tone tone) {
  switch (tone) {
    case DashRow::Tone::Busy: return kViolet;
    case DashRow::Tone::Waiting:
    case DashRow::Tone::Warn: return kGold;
    case DashRow::Tone::Good: return Rgb{95, 224, 138};
    case DashRow::Tone::Bad: return Rgb{255, 107, 107};
    default: return kMuted;
  }
}

bool g_text = false;

}  // namespace

void set_text(bool on) { g_text = on; }
bool text_on() { return g_text; }

const Font* font_for(int scale) {
  switch (scale) {
    case 2: return &kText2;
    case 3: return &kText3;
    case 4: return &kText4;
    default: return scale >= 5 ? &kBig : nullptr;
  }
}

const Glyph* glyph(const Font& f, char32_t cp) {
  int lo = 0, hi = f.count - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    if (f.glyphs[mid].cp == cp) return &f.glyphs[mid];
    if (f.glyphs[mid].cp < cp) lo = mid + 1;
    else hi = mid - 1;
  }
  return nullptr;
}

int text_units(std::string_view s) {
  // kText2 is drawn where the pixel font's columns are 12 pixels wide.
  int units = 0;
  for (size_t i = 0; i < s.size();) {
    const Glyph* g = glyph(kText2, next_cp(s, i));
    units += g ? (g->adv * 64 + 11) / 12 : 64;
  }
  return units;
}

int Eyes::button_hit(const UiModel& m, int x, int y, int w, int h) {
  if (m.screen != Screen::Ready) return 0;
  x -= w / 2;
  y -= (h - 466) / 2;
  if (!m.timer_text.empty()) return std::abs(x) <= kCancelW / 2 + 12 && std::abs(y - kCancelY) <= kCancelH / 2 + 12 ? 2 : 0;
  // What is playing, under the name: a tap opens its screen.
  if (!m.music_title.empty() && m.attention.empty() && m.detail.empty() && std::abs(x) <= 150 && y >= 318 && y < 384) return 3;
  return 0;
}

bool Eyes::wants(const UiModel& m) {
  if (m.music_view) return false;  // now playing has its own screen (skin_views.cpp)
  // The clock, a timer that ran out and a running timer are faces too.
  if (m.screen == Screen::Clock || m.screen == Screen::Alarm) return true;
  if (!m.hero || !m.yes.empty() || !m.no.empty() || m.qr) return false;
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
  const bool seen_before = started_;
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
  const bool speaking = m.screen == Screen::Responding && m.speaking;
  // Moods of the resting face. What it says leaves after a few seconds, a long quiet makes it
  // sleepy, something waiting on the user opens the eyes wide, and good news is a short smile.
  const bool ready = m.screen == Screen::Ready;
  const bool clock = m.screen == Screen::Clock, alarm = m.screen == Screen::Alarm;
  const bool timing = ready && !m.timer_text.empty();
  const bool compact = clock || timing;
  const bool resting = ready || clock;
  const bool attention = resting && !m.attention.empty();
  const bool night = clock && (m.clock_hour >= 23 || m.clock_hour < 7);
  // Between the two layouts everything inside the ring goes, and comes back as the eyes arrive.
  const bool repaint = first || compact != compact_;
  if (!first && compact != compact_) {
    clear_inner(0, h);
    mark(0, h);
    face_box_ = Box{};
    eq_y_ = -1;
  }
  compact_ = compact;
  {
    uint32_t ck = mix(mix(mix(mix(mix(43, m.headline), m.detail), m.hint), m.attention), m.attention_detail);
    ck = mix(ck, static_cast<uint32_t>(m.screen));
    if (first || ck != calm_key_) {
      calm_key_ = ck;
      calm_since_ms_ = now_ms;
    }
  }
  const uint32_t calm_ms = now_ms - calm_since_ms_;
  const bool quiet = ready && !attention && move && calm_ms >= kCaptionMs;
  const bool sleepy = (quiet && calm_ms >= kSleepyMs && !timing) || (night && !attention);
  const float caption_alpha = !ready || attention || !move ? 1.0f : clamp01((static_cast<float>(kCaptionMs) - calm_ms) / kCaptionFadeMs);
  if (m.cheer != cheer_) {
    cheering_ = seen_before && move;
    cheer_at_ms_ = now_ms;
    cheer_ = m.cheer;
  }
  if (clock && hour_ != 255 && m.clock_hour != hour_ && !night && move) {  // a new hour: a smile
    cheering_ = true;
    cheer_at_ms_ = now_ms;
  }
  hour_ = clock ? m.clock_hour : 255;
  if (cheering_ && (now_ms - cheer_at_ms_ >= kCheerMs || !resting)) cheering_ = false;
  const Target tg = target_for(m, sleepy, attention, compact);

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
  if (m.glance != glance_) {  // a touch: the eyes look that way for a moment
    if (started_ && move && ready && !compact && seen_before) {
      gaze_x_ = m.glance_x * 0.2f;
      gaze_y_ = m.glance_y * 0.09f;
      next_gaze_ms_ = now_ms + 1100;
    }
    glance_ = m.glance;
  }
  if (move) {
    if (static_cast<int32_t>(now_ms - next_gaze_ms_) >= 0) {
      next_gaze_ms_ = now_ms + 1200 + static_cast<uint32_t>(rnd() * 2200);
      if (m.screen == Screen::Thinking) {
        gaze_x_ = -14 + rnd() * 28;
        gaze_y_ = -16;
      } else if (compact) {  // little room above the digits: the eyes look about, hardly up or down
        gaze_x_ = (rnd() - 0.5f) * 24;
        gaze_y_ = timing ? 3 : (rnd() - 0.5f) * 6;
      } else if (m.screen == Screen::Ready) {
        gaze_x_ = (rnd() - 0.5f) * 36;
        gaze_y_ = (rnd() - 0.5f) * 16;
      }
    }
    if (m.screen == Screen::Listening || m.screen == Screen::Responding || alarm) gaze_x_ = gaze_y_ = 0;
    if (compact) gaze_y_ = std::max(-3.0f, std::min(3.0f, gaze_y_));
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
  // What stays on the resting face for hours moves a pixel or two each minute, and the ring a
  // little in or out: nothing rests on the same pixels.
  const int drift_min = static_cast<int>(now_ms / 60000);
  const int drift_x = resting && !compact ? (drift_min * 7) % 5 - 2 : 0, drift_y = resting && !compact ? (drift_min * 5) % 3 - 1 : 0;
  // Music: the eyes nod along, upwards only so they stay clear of what is under them.
  const bool music = resting && !attention && !m.music_title.empty();
  const float bob = music && move && !sleepy ? -1.5f * (1 + std::sin(t * 5.4f)) : 0.0f;

  // --- face: the eyes and, while speaking, the mouth ---
  {
    auto q = [](float v) { return static_cast<uint32_t>(static_cast<int>(std::lround(v * 2)) + 4096); };
    uint32_t key = 17;
    for (float v : {ew_, eh_ * blink, gap_, ey_ + gy_ + bob, gx_}) key = mix(key, q(v));
    key = mix(key, (uint32_t(acc.r) << 16) | (uint32_t(acc.g) << 8) | acc.b);
    key = mix(key, q(mouth_ * 40));
    if (mouth_ > 0.03f) key = mix(key, m.fine);
    key = mix(key, cheering_);
    if (repaint || key != face_key_) {
      face_key_ = key;
      const float eh = std::max(6.0f, eh_ * blink), cy = ey_ + gy_ + bob;
      Box box;
      box.x0 = static_cast<int>(C - gap_ / 2 + gx_ - ew_ / 2 - kGlow - 2);
      box.x1 = static_cast<int>(C + gap_ / 2 + gx_ + ew_ / 2 + kGlow + 3);
      box.y0 = static_cast<int>(cy - eh_ / 2 - kGlow - 3);  // the open eye's height: a blink does not shrink the box
      box.y1 = static_cast<int>(cy + eh_ / 2 + kGlow + 3);
      const float mouth_y = 254;
      if (mouth_ > 0.03f) box.y1 = std::max(box.y1, static_cast<int>(mouth_y + 20 + 10));
      const Box was = face_box_;
      const int y0 = was.empty() ? box.y0 : std::min(was.y0, box.y0), y1 = was.empty() ? box.y1 : std::max(was.y1, box.y1);
      // Clear only the columns the old and the new face cover, inside the ring band.
      const int cx0 = was.empty() ? box.x0 : std::min(was.x0, box.x0), cx1 = was.empty() ? box.x1 : std::max(was.x1, box.x1);
      for (int y = std::max(0, y0); y < std::min(h, y1); ++y) {
        const int16_t* b = &band_[static_cast<size_t>(y) * 4];
        if (b[3] > b[0]) s.clear_run(y, std::max<int>(b[1], cx0), std::min<int>(b[2], cx1));
      }
      // One eye's coverage, computed when its size changes (a blink, a change of state) and
      // copied otherwise: the float work is not per frame, and the copy never reads the panel.
      const uint32_t shape = mix(mix(31, q(ew_)), q(eh));
      if (shape != eye_key_ || eye_.empty()) {
        eye_key_ = shape;
        eye_w_ = static_cast<int>(std::ceil(ew_)) + 2 * kGlow + 2;
        eye_h_ = static_cast<int>(std::ceil(eh)) + 2 * kGlow + 2;
        eye_.assign(static_cast<size_t>(eye_w_) * eye_h_, 0);
        const float hw = ew_ / 2, hh = eh / 2, rad = std::min(hw, hh), glow = kGlow;
        for (int y = 0; y < eye_h_; ++y) {
          const float qy = std::fabs(y + 0.5f - eye_h_ / 2.0f) - (hh - rad), oy = std::max(qy, 0.0f);
          for (int x = 0; x < eye_w_; ++x) {
            const float qx = std::fabs(x + 0.5f - eye_w_ / 2.0f) - (hw - rad);
            const float d = (qx > 0 && qy > 0 ? fast_sqrt(qx * qx + oy * oy) : std::max(qx, qy)) - rad;
            float a = clamp01(0.5f - d);
            if (a < 1 && d < glow) {
              const float g = 1 - std::max(d, 0.0f) / glow;
              a += (1 - a) * g * g * 0.42f;
            }
            eye_[static_cast<size_t>(y) * eye_w_ + x] = static_cast<uint8_t>(a * 255);
          }
        }
      }
      if (cheering_) {
        for (int side : {-1, 1}) {
          if (compact) smile(s, C + side * gap_ / 2 + gx_, cy + 20, 24, 10, acc, 10);
          else smile(s, C + side * gap_ / 2 + gx_, cy + 30, 34, 13, acc, 14);
        }
        face_box_ = box;
        mark(y0, y1);
      } else {
      uint16_t lut[256];
      for (int a = 0; a < 256; ++a) {
        const uint16_t v = static_cast<uint16_t>((((acc.r * a) >> 11) << 11) | (((acc.g * a) >> 10) << 5) | ((acc.b * a) >> 11));
        lut[a] = swap ? static_cast<uint16_t>((v >> 8) | (v << 8)) : v;
      }
      for (int side : {-1, 1}) {
        const int ex = static_cast<int>(std::lround(C + side * gap_ / 2 + gx_)) - eye_w_ / 2;
        const int ty = static_cast<int>(std::lround(cy)) - eye_h_ / 2;
        for (int y = std::max(0, -ty); y < eye_h_ && ty + y < h; ++y) {
          const uint8_t* src = &eye_[static_cast<size_t>(y) * eye_w_];
          uint16_t* dst = fb + static_cast<size_t>(ty + y) * w + ex;
          for (int x = std::max(0, -ex); x < eye_w_ && ex + x < w; ++x)
            if (src[x]) dst[x] = lut[src[x]];
        }
      }
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
      mark(y0, y1);
      }
    }
  }

  // --- words: captions, the clock or the timer, what is playing, and who else is at work ---
  {
    // They wait for the eyes: in the compact layout the face must be above the digits, in the
    // other clear of the marks at the top.
    const bool settled = compact ? ey_ + eh_ / 2 + kGlow + 6 <= kCompactTop + 0.5f : ey_ - eh_ / 2 - kGlow - 14 >= kChipsBottom;
    uint32_t key = mix(mix(mix(23, m.headline), m.detail), m.hint);
    key = mix(mix(key, static_cast<uint32_t>(m.screen)), (uint32_t(m.caption_lines) << 1) | speaking);
    // While the reply is spoken the face is alone on the glass: the words come after, on the answer view.
    key = mix(key, static_cast<uint32_t>(std::ceil(caption_alpha * 12)));
    key = mix(mix(key, m.attention), m.attention_detail);
    if (speaking) key = 0x5EA4;
    key = mix(key, (uint32_t(compact) << 2) | (uint32_t(settled) << 1) | night);
    key = mix(mix(mix(mix(key, m.clock_time), m.clock_date), m.timer_text), m.timer_label);
    key = mix(mix(key, m.music_title), m.music_artist);
    for (const UiModel::Chip& chip : m.chips) key = mix(mix(key, chip.label), static_cast<uint32_t>(chip.tone));
    key = mix(key, static_cast<uint32_t>((drift_x + 8) * 16 + drift_y + 8));
    const bool battery = resting && m.battery_mark >= 0;
    key = mix(key, battery ? 256u + static_cast<uint32_t>(m.battery_mark) + (m.battery_mark_low ? 128u : 0u) : 0u);
    const bool redraw = repaint || key != text_key_;
    if (redraw) {
      text_key_ = key;
      eq_y_ = -1;
      pulses_.clear();
      const int top = compact ? kCompactTop : kTextTop, bottom = 426;
      clear_inner(top, bottom);
      mark(top, bottom);
      if (!compact) {
        clear_inner(kChipsTop, kChipsBottom);
        mark(kChipsTop, kChipsBottom);
      }
      auto chord = [&](int y0, int y1) {  // the width free of the ring over rows [y0, y1)
        int width = w;
        for (int y : {y0, y1 - 1}) {
          const float dy = std::fabs(y + 0.5f - h / 2.0f);
          const float c2 = static_cast<float>(kInner - 2) * (kInner - 2) - dy * dy;
          width = std::min(width, c2 > 0 ? static_cast<int>(2 * std::sqrt(c2)) : 0);
        }
        return width;
      };
      auto centred = [&](const Font& f, int y, const std::string& text, Rgb c, float a, int dx = 0) {
        const std::string line = fit(f, text, chord(y, y + f.line) - 2 * std::abs(dx));
        draw_text(s, f, static_cast<int>(C) - text_width(f, line) / 2 + dx, y, line, c, a);
      };
      // What is playing, on one line: three small bars that move (below), then the words.
      auto playing = [&](int y, const std::string& text, Rgb c, float a) {
        const int lead = 26;
        const std::string line = fit(kText2, text, chord(y, y + kText2.line) - lead - 8);
        const int x0 = static_cast<int>(C) - (text_width(kText2, line) + lead) / 2;
        draw_text(s, kText2, x0 + lead, y, line, c, a);
        eq_x_ = x0;
        eq_y_ = y;
      };
      // Who else is at work: a dot in each one's colour and its name, as many as the row holds.
      auto chips = [&](int y) {
        const int dot = 16, between = 22, room = chord(y, y + kText2.line) - 8;
        size_t n = 0;
        int total = 0;
        for (; n < m.chips.size(); ++n) {
          const int width = dot + text_width(kText2, m.chips[n].label) + (n ? between : 0);
          if (total + width > room) break;
          total += width;
        }
        int x = static_cast<int>(C) - total / 2 + drift_x;
        for (size_t i = 0; i < n; ++i) {
          const UiModel::Chip& chip = m.chips[i];
          const bool idle = chip.tone == DashRow::Tone::Dim;
          if (chip.tone == DashRow::Tone::Warn && move) pulses_.push_back({x + 5, y + 14});  // drawn below, pulsing
          else glow_box(s, x + 5.0f, y + 14.0f, 9, 9, tone_rgb(chip.tone), idle ? 0.55f : 1.0f, idle ? 0 : 5);
          draw_text(s, kText2, x + dot, y, chip.label, idle ? kMuted : kFg, idle ? 0.7f : 0.9f);
          x += dot + text_width(kText2, chip.label) + between;
        }
      };
      const std::string song = m.music_artist.empty() ? m.music_title : m.music_title + " \xC2\xB7 " + m.music_artist;
      if (compact) {
        if (settled) {
          // A clock that is on for hours moves a few pixels each minute, as the stock one does.
          const int sx = clock ? (m.clock_minute * 7) % 9 - 4 : 0, sy = clock ? (m.clock_minute * 5) % 7 - 3 : 0;
          centred(kBig, 173 + sy, clock ? m.clock_time : m.timer_text, kFg, night ? 0.55f : 0.96f, sx);
          if (attention) {
            centred(kText2, 264, m.attention, kGold, 0.96f);
            centred(kText2, 296, m.attention_detail, kFg, 0.9f);
          } else {
            centred(kText2, 264 + sy, clock ? soften(m.clock_date) : m.timer_label, kMuted, night ? 0.6f : 0.95f, sx);
            if (music && !night) playing(298, song, kFg, 0.8f);
          }
          if (timing) {  // a button: Cancel
            glow_box(s, C, kCancelY, kCancelW, kCancelH, kPill, 1.0f, 0);
            const std::string word(i18n::text(m.lang, i18n::Msg::Cancel));
            draw_text(s, kText2, static_cast<int>(C) - text_width(kText2, word) / 2, kCancelY - kText2.line / 2, word, kFg, 0.96f);
          } else if (!m.chips.empty() && !night) {
            chips(336);
          }
        }
      } else {
        int y = kTextTop + 2;
        const float ca = caption_alpha;
        if (!speaking && ca > 0) centred(kText3, y, attention ? m.attention : m.headline, attention ? kGold : alarm ? kMint : kFg, 0.96f * ca);
        y += kText3.line + 1;
        const int max_lines = attention ? 2 : std::max<int>(1, std::min<int>(m.caption_lines, 2));
        std::string detail = speaking || ca <= 0 ? std::string() : attention ? m.attention_detail : m.detail;
        if (detail == m.headline) detail.clear();  // a status that only repeats the headline
        std::vector<std::string> lines = wrap(kText2, detail, chord(y, y + max_lines * kText2.line));
        if (static_cast<int>(lines.size()) > max_lines) {
          if (speaking) {  // the reply's latest lines
            lines.erase(lines.begin(), lines.end() - max_lines);
          } else {
            lines.resize(static_cast<size_t>(max_lines));
            lines.back() += "\xE2\x80\xA6";
          }
        }
        // What is playing stays under the name after the name has gone: it is why one looks.
        if (music && m.detail.empty()) {
          playing(y + drift_y, m.music_title, kFg, 0.85f);
          if (!m.music_artist.empty()) centred(kText2, y + drift_y + kText2.line, m.music_artist, kMuted, 0.9f, drift_x);
        }
        for (const std::string& line : lines) {
          centred(kText2, y, line, speaking || attention ? kFg : kMuted, speaking ? 0.96f : attention ? 0.9f : ca);
          y += kText2.line;
        }
        if (!m.hint.empty() && !speaking && !attention && ca > 0) centred(kText2, 372, m.hint, kMuted, 0.75f * ca);
        if (resting && settled && !m.chips.empty()) chips(kChipsTop + 2 + drift_y);
      }
      if (battery && (!compact || (settled && !timing))) {
        // A small battery at the foot of the face, only when it is getting low: red when it is.
        const Rgb c = m.battery_mark_low ? Rgb{255, 107, 107} : kMuted;
        // An outline 26 by 13 with small corners, what is left in it as a bar, and the cap.
        const float bx = C + drift_x - 1.0f, by = 413, hw = 13, hh = 6.5f, rad = 3.5f;
        for (int yy = static_cast<int>(by - hh) - 1; yy <= static_cast<int>(by + hh) + 1; ++yy)
          for (int xx = static_cast<int>(bx - hw) - 1; xx <= static_cast<int>(bx + hw) + 1; ++xx) {
            const float qx = std::fabs(xx + 0.5f - bx) - (hw - rad), qy = std::fabs(yy + 0.5f - by) - (hh - rad);
            const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
            const float d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - rad;
            const float a = clamp01(0.5f - d) * clamp01(d + 2.1f);  // a line 1.6 px wide, inside the edge
            if (a > 0) s.add(xx, yy, c, static_cast<int>(a * 230));
          }
        const int fill = std::max(3, 20 * m.battery_mark / 100), fx = static_cast<int>(bx - hw) + 3;
        for (int yy = static_cast<int>(by - hh) + 3; yy < static_cast<int>(by + hh) - 2; ++yy)
          for (int xx = fx; xx < fx + fill; ++xx) s.add(xx, yy, c, 230);
        for (int yy = static_cast<int>(by) - 2; yy <= static_cast<int>(by) + 2; ++yy)
          for (int xx = static_cast<int>(bx + hw) + 1; xx < static_cast<int>(bx + hw) + 3; ++xx) s.add(xx, yy, c, 230);
      }
    }
    // A mark whose owner waits on the user pulses: its dot alone is redrawn, a few steps a second.
    if (!pulses_.empty()) {
      const uint32_t step = static_cast<uint32_t>(t * 8);
      if (redraw || step != pulse_key_) {
        pulse_key_ = step;
        const float a = 0.55f + 0.45f * std::sin(step / 8.0f * 3.0f);
        for (const Pulse& p : pulses_) {
          for (int y = p.y - 10; y < p.y + 10; ++y) s.clear_run(y, p.x - 10, p.x + 10);
          glow_box(s, static_cast<float>(p.x), static_cast<float>(p.y), 9, 9, kGold, a, 5);
          mark(p.y - 10, p.y + 10);
        }
      }
    }
    // The bars beside what is playing: a few steps a second, in their own small box.
    if (eq_y_ >= 0) {
      const uint32_t step = move ? static_cast<uint32_t>(t * 9) : 0;
      if (redraw || step != eq_key_) {
        eq_key_ = step;
        const int y0 = eq_y_ + 3, y1 = eq_y_ + 23;
        if (!redraw)
          for (int y = y0; y < y1; ++y) s.clear_run(y, eq_x_ - 1, eq_x_ + 19);
        for (int i = 0; i < 3; ++i) {
          const float bh = move ? 5 + 13 * voice(step / 9.0f * 0.8f + i * 1.9f) : 7.0f + 4 * i;
          glow_box(s, eq_x_ + 3.0f + i * 6.5f, y1 - 1 - bh / 2, 4, bh, kGold, 0.9f, 0);
        }
        mark(y0, y1);
      }
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
    if (timing && m.timer_permille >= 0) ring = Ring::Arc;  // what is left of the timer
    uint32_t key = mix(29, static_cast<uint32_t>(ring));
    key = mix(key, (uint32_t(acc.r) << 16) | (uint32_t(acc.g) << 8) | acc.b);
    if (ring == Ring::Pulse) key = mix(key, static_cast<uint32_t>(level_ * 200));
    if (ring == Ring::Comet || ring == Ring::Ticks) key = mix(key, m.fine);
    // Something waits on the user: the still ring breathes, a few steps a second.
    const bool breathe = (attention || alarm) && ring == Ring::Still && move;
    const float breath = breathe ? 0.5f - 0.5f * std::cos(t * (alarm ? 7.0f : 2.2f)) : 0.0f;
    if (breathe) key = mix(key, static_cast<uint32_t>(breath * 10));
    if (ring == Ring::Arc) key = mix(key, static_cast<uint32_t>(m.timer_permille));
    key = mix(key, (static_cast<uint32_t>(drift_min % 3) << 1) | night);
    // The still ring over rows [y0, y1): all of it, or the rows the clock's dot has left.
    const float still_half = breathe ? 1.0f + 1.5f * breath : 1.0f;
    const float still_alpha = breathe ? 0.35f + 0.6f * breath : alarm ? 0.9f : night ? 0.16f : 0.3f;
    auto still = [&](int y0, int y1) {
      const float rr = R - 8.0f - (resting ? drift_min % 3 : 0), cy = h / 2.0f;
      for (int y = std::max(0, y0); y < std::min(h, y1); ++y) {
        const int16_t* b = &band_[static_cast<size_t>(y) * 4];
        for (int run = 0; run < 2; ++run)
          for (int x = b[run * 2]; x < b[run * 2 + 1]; ++x) {
            const float d = std::fabs(std::hypot(x + 0.5f - C, y + 0.5f - cy) - rr) - still_half;
            const float a = clamp01(0.5f - d);
            if (a > 0) s.add(x, y, acc, static_cast<int>(a * still_alpha * 255));
          }
      }
    };
    const bool ring_redrawn = first || key != ring_key_;
    if (ring_redrawn) {
      const bool same_comet = !first && ring == Ring::Comet && ring_ == Ring::Comet;
      ring_key_ = key;
      ring_ = ring;
      const float cy = h / 2.0f;
      if (ring == Ring::Comet) {
        const float head = t * (m.screen == Screen::Thinking && !m.detail.empty() ? 2.0f : 3.2f), rr = R - 12.0f;
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
          still(0, h);
        } else if (ring == Ring::Arc) {
          // A faint full circle, and over it what is left of the timer, clockwise from the top.
          const float rr = R - 9.0f, left = clamp01(m.timer_permille / 1000.0f) * 2 * kPi;
          for (int y = 0; y < h; ++y) {
            const int16_t* b = &band_[static_cast<size_t>(y) * 4];
            for (int run = 0; run < 2; ++run)
              for (int x = b[run * 2]; x < b[run * 2 + 1]; ++x) {
                const float px = x + 0.5f - C, py = y + 0.5f - cy;
                const float off = std::fabs(std::hypot(px, py) - rr);
                if (off > 4.5f) continue;
                float a = clamp01(0.5f - (off - 1.0f)) * 0.2f;
                float angle = std::atan2(px, -py);
                if (angle < 0) angle += 2 * kPi;
                if (angle <= left) a = std::max(a, clamp01(0.5f - (off - 2.5f)) * 0.95f);
                if (a > 0) s.add(x, y, acc, static_cast<int>(a * 255));
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
      // The clock's seconds: a dot that goes round the ring, moved by redrawing only its rows.
    const int sec = clock && ring == Ring::Still ? m.clock_second % 60 : -1;
    if (ring_redrawn || sec != dot_sec_) {
      if (!ring_redrawn && dot_y1_ > dot_y0_) {
        clear_band(dot_y0_, dot_y1_);
        still(dot_y0_, dot_y1_);
        mark(dot_y0_, dot_y1_);
      }
      dot_sec_ = sec;
      dot_y0_ = dot_y1_ = 0;
      if (sec >= 0) {
        const float at = sec * (2 * kPi / 60), rr = R - 8.0f - drift_min % 3;
        const float x = C + std::sin(at) * rr, y = h / 2.0f - std::cos(at) * rr;
        disc(s, x, y, 3.5f, kFg, night ? 0.5f : 0.95f, 4);
        dot_y0_ = static_cast<int>(y - 9);
        dot_y1_ = static_cast<int>(y + 10);
        mark(dot_y0_, dot_y1_);
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
