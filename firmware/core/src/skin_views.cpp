// The Eyes skin's whole-panel screens besides the face: a question with Yes and No buttons, a
// card, and the offline screen. One layout for all three: everything inside the round glass,
// the same fonts and colours as the face, and targets a fingertip tall. Drawn for a 466 px
// panel and centred on a larger one. Like the rest of the renderer they read no clock.
#include <algorithm>
#include <cmath>

#include "hg/canvas.hpp"
#include "hg/i18n.hpp"
#include "hg/ui.hpp"
#include "skin_draw.hpp"

namespace hg::skin::view {
namespace {

constexpr int kSize = 466, kC = kSize / 2;
constexpr int kReach = 200;  // text and buttons stay inside this radius: the edge ring is outside it
constexpr Rgb kPill{38, 34, 46}, kYes{30, 110, 68}, kNo{132, 40, 58}, kOff{30, 27, 36}, kRest{255, 107, 107};

// The glass's width over rows [y0, y1), less a margin each side.
int chord(int y0, int y1) {
  int width = kSize;
  for (int y : {y0, y1 - 1}) {
    const float dy = std::fabs(y + 0.5f - kC);
    const float c2 = static_cast<float>(kReach) * kReach - dy * dy;
    width = std::min(width, c2 > 0 ? static_cast<int>(2 * std::sqrt(c2)) : 0);
  }
  return std::max(0, width - 12);
}

// --- the question ---

constexpr int kAskBottom = 316, kAskPitch = 26;
constexpr int kButtonY = 326, kButtonH = 58, kButtonW = 122, kButtonGap = 10;

// Word wrap where each line has its own width (the glass narrows away from the middle). A word
// wider than its line breaks inside rather than being cut, and nothing is dropped: what the
// question shows is what gets answered.
std::vector<std::string> wrap_rows(const Font& f, std::string_view t, int top, int pitch) {
  std::vector<std::string> lines;
  std::string line;
  auto width = [&] { return chord(top + static_cast<int>(lines.size()) * pitch, top + static_cast<int>(lines.size()) * pitch + f.line); };
  auto flush = [&] {
    lines.push_back(line);
    line.clear();
  };
  size_t i = 0;
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
    std::string word(t.substr(i, j - i));
    i = j;
    const std::string joined = line.empty() ? word : line + " " + word;
    if (text_width(f, joined) <= width()) {
      line = joined;
      continue;
    }
    if (!line.empty()) flush();
    while (text_width(f, word) > width()) {  // break inside the word, a character at a time
      size_t cut = 0, k = 0;
      while (k < word.size()) {
        size_t next = k;
        next_cp(word, next);
        if (text_width(f, std::string_view(word).substr(0, next)) > width()) break;
        cut = k = next;
      }
      if (!cut) {  // not even one character fits: take one anyway
        cut = 0;
        next_cp(word, cut);
      }
      line = word.substr(0, cut);
      flush();
      word.erase(0, cut);
    }
    line = word;
  }
  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  return lines;
}

bool drawable(const Font& f, std::string_view t) {
  for (size_t i = 0; i < t.size();) {
    const char32_t cp = next_cp(t, i);
    if (cp != '\n' && !glyph(f, cp)) return false;
  }
  return true;
}

struct Ask {
  bool eyes = true;   // the small face above the title: gone when the text needs its rows
  bool whole = true;  // every character of the text is on the glass
  int title_y = 0, top = 0, pitch = kAskPitch;
  const Font* font = &kText2;  // a short question is set larger
  std::vector<std::string> lines;
};

Ask ask_layout(const UiModel& m) {
  Ask a;
  {
    std::vector<std::string> large = wrap_rows(kText3, m.detail, 132, kText3.line + 2);
    if (!large.empty() && large.size() <= 3 && drawable(kText3, m.detail)) {
      bool fits = true;
      const int pitch = kText3.line + 2, top = 122 + (kAskBottom - 122 - static_cast<int>(large.size()) * pitch) / 2;
      for (size_t i = 0; i < large.size(); ++i) {
        const int y = top + static_cast<int>(i) * pitch;
        if (text_width(kText3, large[i]) > chord(y, y + kText3.line)) fits = false;
      }
      if (fits) {
        a.title_y = 90, a.top = top, a.pitch = pitch, a.font = &kText3;
        a.lines = std::move(large);
        return a;
      }
    }
  }
  for (const bool eyes : {true, false}) {
    a.eyes = eyes;
    a.title_y = eyes ? 90 : 66;
    a.top = a.title_y + 32;
    a.lines = wrap_rows(kText2, m.detail, a.top, kAskPitch);
    const int rows = (kAskBottom - a.top) / kAskPitch;
    if (static_cast<int>(a.lines.size()) <= rows) break;
    if (!eyes) a.whole = false;
  }
  if (!drawable(kText2, m.detail)) a.whole = false;
  if (!a.whole) {  // its last row says so
    const size_t keep = static_cast<size_t>(std::max(1, (kAskBottom - a.top) / kAskPitch - 1));
    if (a.lines.size() > keep) a.lines.resize(keep);
  }
  // A short question sits in the middle of its rows, where every line still fits the glass there.
  const int rows = (kAskBottom - a.top) / kAskPitch, spare = rows - static_cast<int>(a.lines.size()) - (a.whole ? 0 : 1);
  if (spare > 0) {
    const int top = a.top + spare * kAskPitch / 2;
    bool fits = true;
    for (size_t i = 0; i < a.lines.size(); ++i) {
      const int y = top + static_cast<int>(i) * kAskPitch;
      if (text_width(kText2, a.lines[i]) > chord(y, y + kText2.line)) fits = false;
    }
    if (fits) a.top = top;
  }
  return a;
}

struct Box {
  int x, y, w, h;
  bool has(int px, int py, int pad) const { return px >= x - pad && px < x + w + pad && py >= y - pad && py < y + h + pad; }
};

Box yes_box(const UiModel& m) {
  if (m.no.empty()) return {kC - kButtonW / 2, kButtonY, kButtonW, kButtonH};
  return {kC - kButtonGap / 2 - kButtonW, kButtonY, kButtonW, kButtonH};
}
Box no_box() { return {kC + kButtonGap / 2, kButtonY, kButtonW, kButtonH}; }

// --- shared pieces ---

void centred(Surface& s, const Font& f, int y, const std::string& text, Rgb c, float a = 1.0f) {
  const std::string line = fit(f, text, chord(y, y + f.line));
  draw_text(s, f, kC - text_width(f, line) / 2, y, line, c, a);
}

void button(Surface& s, const Box& b, const std::string& label, Rgb fill, Rgb ink, const Font& f = kText3) {
  glow_box(s, b.x + b.w / 2.0f, b.y + b.h / 2.0f, static_cast<float>(b.w), static_cast<float>(b.h), fill, 1.0f, 0);
  const std::string t = fit(f, label, b.w - 16);
  draw_text(s, f, b.x + (b.w - text_width(f, t)) / 2, b.y + b.h / 2 - f.mid, t, ink, 1.0f);
}

// The way back, the same on every screen that has one: a pill at the top of the glass.
constexpr Box kBack{kC - 56, 38, 112, 40};

void back_pill(Surface& s, const UiModel& m, i18n::Msg label = i18n::Msg::Back) {
  const std::string word = i18n::text(m.lang, label);
  const bool back = label == i18n::Msg::Back;  // only the way back has the chevron
  const int tw = text_width(kText2, word), lead = back ? 14 : 0, total = lead + tw, x = kC - total / 2, cy = kBack.y + kBack.h / 2;
  glow_box(s, kC, kBack.y + kBack.h / 2.0f, static_cast<float>(std::max(kBack.w, total + 36)), static_cast<float>(kBack.h), kPill, 1.0f, 0);
  draw_text(s, kText2, x + lead, cy - kText2.mid, word, kFg, 0.92f);
  if (!back) return;
  capsule_line(s, x + 6.0f, cy - 6.0f, x + 0.5f, static_cast<float>(cy), 2.2f, kMuted, 1.0f);
  capsule_line(s, x + 0.5f, static_cast<float>(cy), x + 6.0f, cy + 6.0f, 2.2f, kMuted, 1.0f);
}

void small_eyes(Surface& s, int cy, Rgb c) {
  for (int side : {-1, 1}) glow_box(s, kC + side * 26.0f, static_cast<float>(cy), 20, 30, c, 1.0f, 10);
}

// --- the screens ---

void draw_ask(Surface& s, const UiModel& m) {
  const Ask a = ask_layout(m);
  if (a.eyes) small_eyes(s, 56, kGold);
  centred(s, kText2, a.title_y, m.headline, kGold);
  int y = a.top;
  for (const std::string& line : a.lines) {
    // The last line in brackets says where the question comes from: quieter than the question.
    const bool aside = &line == &a.lines.back() && a.lines.size() > 1 && line.size() > 2 && line.front() == '(' && line.back() == ')';
    draw_text(s, *a.font, kC - text_width(*a.font, line) / 2, y, line, aside ? kMuted : kFg, aside ? 1.0f : 0.96f);
    y += a.pitch;
  }
  if (!a.whole) centred(s, kText2, y, i18n::text(m.lang, i18n::Msg::TooLongToShow), kRest);
  if (!m.yes.empty()) {
    const bool on = a.whole;
    button(s, yes_box(m), i18n::text(m.lang, i18n::Msg::Yes), on ? kYes : kOff, on ? kWhite : kMuted);
  }
  if (!m.no.empty()) button(s, no_box(), i18n::text(m.lang, i18n::Msg::No), kNo, kWhite);
}

constexpr int kCardTop = 150, kCardBottom = 372, kCardPitch = 27, kCardWidth = 288;

void draw_card(Surface& s, const UiModel& m) {
  back_pill(s, m);
  centred(s, kText3, 92, m.headline, kFg);
  glow_box(s, kC, 137, 36, 3, kGold, 0.9f, 0);
  const int room = kCardBottom - kCardTop;
  if (m.hero) {  // a short card: as large as it stays whole
    for (const Font* f : {&kText3, &kText2}) {
      const int pitch = f->line + 2;
      std::vector<std::string> lines = wrap(*f, m.detail, kCardWidth + (f == &kText2 ? 0 : 24));
      if (f == &kText3 && static_cast<int>(lines.size()) > 4) continue;
      const int rows = std::min<int>(static_cast<int>(lines.size()), room / pitch);
      int y = kCardTop + (room - rows * pitch) / 2;
      for (int i = 0; i < rows; ++i, y += pitch) centred(s, *f, y, lines[static_cast<size_t>(i)], kFg, 0.96f);
      break;
    }
    return;
  }
  // A long card pages: the app counts the same lines (card_cols, card_rows).
  const std::vector<std::string> lines = wrap_text(m.body, card_cols());
  const int rows = card_rows(), total = static_cast<int>(lines.size());
  const int max_first = std::max(0, total - rows);
  const int first = m.scroll < 0 ? max_first : std::min(m.scroll, max_first);
  const int shown = std::min(rows, total - first);
  int y = kCardTop + (total > rows ? 0 : (room - shown * kCardPitch) / 2);
  for (int i = 0; i < shown; ++i, y += kCardPitch) centred(s, kText2, y, lines[static_cast<size_t>(first + i)], kFg, 0.96f);
  if (total > rows) {  // where this page is in the card
    const float track = 56, thumb = std::max(12.0f, track * rows / total);
    const float x0 = kC - track / 2, at = max_first ? static_cast<float>(first) / max_first : 0.0f;
    glow_box(s, kC, 388, track, 4, kPill, 1.0f, 0);
    glow_box(s, x0 + thumb / 2 + (track - thumb) * at, 388, thumb, 4, kMuted, 1.0f, 0);
  }
}

// The sessions and status tiles: one row each, set to the glass's width at its height.
constexpr int kDashTop = 98, kDashPitch = 38, kDashRows = 7;

Rgb tone_rgb(DashRow::Tone t) {
  switch (t) {
    case DashRow::Tone::Busy: return kViolet;
    case DashRow::Tone::Waiting:
    case DashRow::Tone::Warn: return kGold;
    case DashRow::Tone::Good: return Rgb{95, 224, 138};
    case DashRow::Tone::Bad: return kRest;
    default: return kMuted;
  }
}

void draw_dash(Surface& s, const UiModel& m) {
  back_pill(s, m, i18n::Msg::Settings);
  const int total = static_cast<int>(m.dash.size());
  const int max_first = std::max(0, total - kDashRows);
  const int first = std::max(0, std::min(m.scroll, max_first));
  const int shown = std::min(kDashRows, total - first);
  // Someone waits on the user: said first, above the rows.
  const bool waits = !m.attention.empty();
  if (waits) centred(s, kText2, 80, m.attention, kGold);
  int y = kDashTop + (waits ? 12 : 0) + (kDashRows - shown) * kDashPitch / 2;
  for (int i = 0; i < shown; ++i, y += kDashPitch) {
    const DashRow& row = m.dash[static_cast<size_t>(first + i)];
    const int ty = y + (kDashPitch - kText2.line) / 2, cy = y + kDashPitch / 2;
    if (row.heading) {
      centred(s, kText2, ty, row.label, kMuted, 0.8f);
      continue;
    }
    const int width = 276, left = kC - width / 2, right = kC + width / 2;  // one column: what the lowest row's glass holds
    if (row.sub) {  // which one: a session's name under the row that counts them
      draw_text(s, kText2, left + 24, ty, fit(kText2, row.label, width - 24), row.tone == DashRow::Tone::Warn ? kGold : kMuted, 0.95f);
      continue;
    }
    const Rgb tone = tone_rgb(row.tone);
    glow_box(s, left + 6.0f, static_cast<float>(cy), 10, 10, tone, 1.0f, 5);
    const std::string value = fit(kText2, row.value, width / 2);
    const int vw = text_width(kText2, value);
    const std::string label = fit(kText2, row.label, width - 24 - vw - (vw ? 12 : 0));
    draw_text(s, kText2, left + 24, ty, label, kFg, 0.96f);
    if (vw) draw_text(s, kText2, right - vw, ty, value, tone, 1.0f);
  }
  if (total > kDashRows) {
    const float track = 56, thumb = std::max(12.0f, track * kDashRows / total);
    const float at = max_first ? static_cast<float>(first) / max_first : 0.0f;
    glow_box(s, kC, 392, track, 4, kPill, 1.0f, 0);
    glow_box(s, kC - track / 2 + thumb / 2 + (track - thumb) * at, 392, thumb, 4, kMuted, 1.0f, 0);
  }
}

constexpr Box kAction{kC - 92, 330, 184, 52};

// Now playing: the track and who plays it, a button to pause with one each side to skip, and
// how far the track is as an arc at the glass edge.
constexpr int kMusicY = 336;
constexpr Rgb kPlay{95, 224, 138};

void draw_music(Surface& s, const UiModel& m) {
  back_pill(s, m);
  // The arc: a faint circle, and over it the part played, clockwise from the top.
  const float rr = kSize / 2 - 9.0f, played = m.music_permille < 0 ? -1.0f : clamp01(m.music_permille / 1000.0f) * 6.2831853f;
  for (int y = 0; y < kSize; ++y) {
    const float py = y + 0.5f - kC;
    const float out2 = (rr + 5) * (rr + 5) - py * py;
    if (out2 <= 0) continue;
    const float in2 = (rr - 5) * (rr - 5) - py * py;
    const int xo = static_cast<int>(std::sqrt(out2)) + 1, xi = in2 > 0 ? static_cast<int>(std::sqrt(in2)) : 0;
    for (int side : {-1, 1})
      for (int i = xi; i <= xo; ++i) {
        const int x = kC + side * i - (side < 0 ? 1 : 0);
        const float px = x + 0.5f - kC;
        const float off = std::fabs(std::sqrt(px * px + py * py) - rr);
        if (off > 4.5f) continue;
        float a = clamp01(0.5f - (off - 1.0f)) * 0.2f;
        float angle = std::atan2(px, -py);
        if (angle < 0) angle += 6.2831853f;
        if (angle <= played) a = std::max(a, clamp01(0.5f - (off - 2.5f)) * 0.95f);
        if (a > 0) s.add(x, y, kPlay, static_cast<int>(a * 255));
      }
  }
  // Pleased eyes: two small arcs.
  for (int side : {-1, 1}) {
    const float cx = kC + side * 34.0f, cy = 146, r = 17;
    float px = 0, py = 0;
    for (int i = 0; i <= 8; ++i) {
      const float a = (200 + i * 17.5f) * 3.14159265f / 180;
      const float x = cx + std::cos(a) * r, y = cy + std::sin(a) * r;
      if (i) capsule_line(s, px, py, x, y, 9.0f, kGold, 1.0f);
      px = x;
      py = y;
    }
  }
  centred(s, kText3, 192, fit(kText3, m.music_title, 330), kFg);
  centred(s, kText2, 236, fit(kText2, m.music_artist, 300), kMuted);
  // Pause, with the two bars; a pair of chevrons each side to skip.
  glow_box(s, kC, kMusicY, 72, 72, kPill, 1.0f, 0);
  for (int side : {-1, 1}) glow_box(s, kC + side * 8.0f, kMusicY, 8, 28, kFg, 0.96f, 0);
  for (int side : {-1, 1})
    for (int k = 0; k < 2; ++k) {
      const float tip = kC + side * (98.0f + k * 13), back = tip - side * 10.0f;
      capsule_line(s, back, kMusicY - 11.0f, tip, kMusicY, 3.0f, kMuted, 1.0f);
      capsule_line(s, tip, kMusicY, back, kMusicY + 11.0f, 3.0f, kMuted, 1.0f);
    }
  if (!m.music_length.empty()) centred(s, kText2, 386, m.music_at + " / " + m.music_length, kMuted, 0.8f);
}

// --- settings ---

// The list uses the whole glass: a row is as wide as the glass is at its height, rows fade into
// the edge at the top and bottom, and the selected row opens in place for its slider and text.
// List coordinates start at kSetTop and scroll by UiModel::settings_scroll.
constexpr int kSetTop = 84, kSetBottom = 418;  // the rows of the glass the list shows through
constexpr int kSetPad = 22, kSetEnd = 46;      // above the first row and below the last: clear of the fades
constexpr int kSetPitch = 48, kSetRow = 46;
constexpr int kSetSlider = 36, kSetText = 244, kSetValue = 150;
constexpr Rgb kTrack{58, 54, 62};

bool set_has(const UiModel& m, int i) { return i >= 0 && i < static_cast<int>(m.settings.size()); }

// The selected row's text: the item's body, less the first line where the row already shows it.
std::vector<std::string> set_desc(const UiModel& m) {
  if (!set_has(m, m.settings_sel)) return {};
  const SettingRow& row = m.settings[static_cast<size_t>(m.settings_sel)];
  std::string_view body = m.body;
  const size_t nl = body.find('\n');
  const bool shown = !row.value.empty() && text_width(kText2, row.value) <= kSetValue && body.substr(0, nl) == row.value;
  if (row.kind == SettingRow::Kind::Slider || shown) body = nl == std::string_view::npos ? std::string_view() : body.substr(nl + 1);
  return wrap(kText2, body, kSetText);
}

int set_extra(const UiModel& m) {
  if (!set_has(m, m.settings_sel)) return 0;
  const bool slider = m.settings[static_cast<size_t>(m.settings_sel)].kind == SettingRow::Kind::Slider;
  const int lines = static_cast<int>(set_desc(m).size());
  return (slider ? kSetSlider : 0) + lines * kText2.line + (slider || lines ? 10 : 0);
}

// The columns a row's text keeps to over panel rows [y0, y1).
struct Span {
  int l, r;
};
Span set_span(int y0, int y1) {
  const float dy = std::max(std::fabs(y0 + 0.5f - kC), std::fabs(y1 - 0.5f - kC));
  const float reach = kC - 18.0f, c2 = reach * reach - dy * dy;
  const int half = c2 > 0 ? static_cast<int>(std::sqrt(c2)) : 0;
  return {std::max(62, kC - half + 30), std::min(kSize - 62, kC + half - 30)};
}

int set_scroll(const UiModel& m) { return std::max(0, std::min(m.settings_scroll, set_max_scroll(m))); }

// The open slider's bar: its ends and its centre line, in panel coordinates.
struct Bar {
  int x0 = 0, x1 = 0, y = 0;
};
Bar set_bar(const UiModel& m) {
  if (!set_has(m, m.settings_sel) || m.settings[static_cast<size_t>(m.settings_sel)].kind != SettingRow::Kind::Slider) return {};
  const int top = kSetTop + set_row_top(m, m.settings_sel) - set_scroll(m);
  const Span sp = set_span(top + 4, top + set_row_h(m, m.settings_sel) - 4);
  return {sp.l + 8, sp.r - 8, top + kSetRow + 12};
}

void round_box(Surface& s, int x, int y, int w, int h, int r, Rgb c) {
  for (int yy = std::max(0, y); yy < std::min(s.h, y + h); ++yy) {
    const float py = yy + 0.5f, qy = std::max({y + r - py, py - (y + h - r), 0.0f});
    for (int xx = x; xx < x + w; ++xx) {
      const float px = xx + 0.5f, qx = std::max({x + r - px, px - (x + w - r), 0.0f});
      const float a = qx > 0 && qy > 0 ? clamp01(0.5f - (std::sqrt(qx * qx + qy * qy) - r)) : 1.0f;
      if (a > 0) s.add(xx, yy, c, static_cast<int>(a * 255));
    }
  }
}

// Rows [y0, y1) fade to black toward y0 (`rising`) or toward y1.
void fade_rows(Surface& s, int y0, int y1, bool rising) {
  for (int y = std::max(0, y0); y < std::min(s.h, y1); ++y) {
    const int a = (rising ? y - y0 : y1 - 1 - y) * 256 / (y1 - y0);
    for (int x = 0; x < kSize; ++x) {
      const int i = y * s.w + x;
      const uint16_t v = s.get(i);
      if (!v) continue;
      s.put(i, static_cast<uint16_t>((((v >> 11) * a >> 8) << 11) | ((((v >> 5) & 63) * a >> 8) << 5) | ((v & 31) * a >> 8)));
    }
  }
}

void draw_settings(Surface& s, const UiModel& m) {
  const int scroll = set_scroll(m), n = static_cast<int>(m.settings.size());
  const std::vector<std::string> desc = set_desc(m);
  for (int i = 0; i < n; ++i) {
    const int top = kSetTop + set_row_top(m, i) - scroll, h = set_row_h(m, i);
    if (top >= kSetBottom) break;
    if (top + h <= kSetTop) continue;
    const SettingRow& row = m.settings[static_cast<size_t>(i)];
    const bool sel = i == m.settings_sel;
    const int cy = top + kSetRow / 2, ty = cy - kText2.mid;
    const Span sp = sel ? set_span(top + 4, top + h - 4) : set_span(cy, cy + 1);
    if (sp.r - sp.l < 80) continue;  // at the very edge of the glass: under the fade anyway
    if (sel) round_box(s, sp.l - 16, top, sp.r - sp.l + 32, h, 18, kPill);
    int right = sp.r;  // where the label must stop
    switch (row.kind) {
      case SettingRow::Kind::Toggle: {
        const bool on = row.level != 0;
        glow_box(s, sp.r - 22.0f, static_cast<float>(cy), 44, 24, on ? kPlay : kTrack, on ? 0.9f : 1.0f, 0);
        disc(s, sp.r - 22.0f + (on ? 10 : -10), static_cast<float>(cy), 9, kWhite, on ? 1.0f : 0.75f);
        right = sp.r - 44;
        break;
      }
      case SettingRow::Kind::Action:
        capsule_line(s, sp.r - 8.0f, cy - 6.0f, sp.r - 2.0f, static_cast<float>(cy), 2.2f, kMuted, sel ? 1.0f : 0.7f);
        capsule_line(s, sp.r - 2.0f, static_cast<float>(cy), sp.r - 8.0f, cy + 6.0f, 2.2f, kMuted, sel ? 1.0f : 0.7f);
        right = sp.r - 10;
        break;
      default: {  // a value, or a slider's percentage
        const std::string v = fit(kText2, row.value, std::min(kSetValue, (sp.r - sp.l) / 2));
        const int vw = text_width(kText2, v);
        draw_text(s, kText2, sp.r - vw, ty, v, sel ? kGold : kMuted, 1.0f);
        right = sp.r - vw;
        break;
      }
    }
    draw_text(s, kText2, sp.l, ty, fit(kText2, row.label, right - 12 - sp.l), kFg, 0.96f);
    if (!sel) continue;
    int y = top + kSetRow;
    if (row.kind == SettingRow::Kind::Slider) {
      const Bar bar = set_bar(m);
      const int w = bar.x1 - bar.x0, fill = w * std::min<int>(100, row.level) / 100;
      glow_box(s, bar.x0 + w / 2.0f, static_cast<float>(bar.y), static_cast<float>(w), 5, kTrack, 1.0f, 0);
      if (fill > 2) glow_box(s, bar.x0 + fill / 2.0f, static_cast<float>(bar.y), static_cast<float>(fill), 5, kGold, 1.0f, 0);
      disc(s, static_cast<float>(bar.x0 + fill), static_cast<float>(bar.y), 10, kWhite, 1.0f);
      y += kSetSlider;
    }
    for (const std::string& line : desc) {
      draw_text(s, kText2, sp.l, y, line, kMuted, 1.0f);
      y += kText2.line;
    }
  }
  // The list meets the glass softly, and the way back sits over where it has gone.
  for (int y = 0; y < kSetTop; ++y) s.clear_run(y, 0, kSize);
  for (int y = kSetBottom; y < kSize; ++y) s.clear_run(y, 0, kSize);
  fade_rows(s, kSetTop, kSetTop + 22, true);
  fade_rows(s, kSetBottom - 36, kSetBottom, false);
  back_pill(s, m);
  const int max_scroll = set_max_scroll(m);
  if (max_scroll > 0) {  // where the list is, at the right edge
    const float track = 90, view = static_cast<float>(set_view_h()), thumb = std::max(18.0f, track * view / (view + max_scroll));
    const float x = kSize - 27.5f, at = static_cast<float>(scroll) / max_scroll;
    glow_box(s, x, kC, 3, track, kTrack, 1.0f, 0);
    glow_box(s, x, kC - track / 2 + thumb / 2 + (track - thumb) * at, 3, thumb, kMuted, 1.0f, 0);
  }
}

void draw_offline(Surface& s, const UiModel& m) {
  for (int side : {-1, 1}) glow_box(s, kC + side * 56.0f, 168, 44, 10, kRest, 0.85f, 16);  // resting, not broken
  centred(s, kText3, 216, m.headline, kFg);
  std::vector<std::string> lines = wrap(kText2, m.detail, 300);
  if (lines.size() > 2) {
    lines.resize(2);
    lines.back() += "\xE2\x80\xA6";
  }
  int y = 258;
  for (const std::string& line : lines) {
    centred(s, kText2, y, line, kMuted);
    y += kText2.line;
  }
  button(s, kAction, i18n::text(m.lang, i18n::Msg::Settings), kPill, kFg, kText2);
}

}  // namespace

bool wants(const UiModel& m) {
  if (m.qr || m.color_test) return false;
  if (m.music_view) return true;
  switch (m.screen) {
    case Screen::Prompt: return m.hero && !(m.yes.empty() && m.no.empty());
    case Screen::Card: return true;
    case Screen::Offline: return m.hero;
    case Screen::Dashboard: return true;
    case Screen::Settings: return !m.settings.empty();
    default: return false;
  }
}

uint32_t key(const UiModel& m) {
  uint32_t k = mix(mix(mix(41, m.headline), m.detail), m.body);
  k = mix(k, static_cast<uint32_t>(m.screen));
  k = mix(k, static_cast<uint32_t>(m.scroll));
  k = mix(k, static_cast<uint32_t>(m.lang));
  if (m.screen == Screen::Dashboard) k = mix(k, m.attention);
  if (m.music_view) {
    k = mix(mix(mix(mix(k, m.music_title), m.music_artist), m.music_at), m.music_length);
    k = mix(k, 7u + static_cast<uint32_t>(m.music_permille + 1));
  }
  if (m.screen == Screen::Dashboard)
    for (const DashRow& row : m.dash) k = mix(mix(mix(k, row.label), row.value), (static_cast<uint32_t>(row.tone) << 2) | (row.sub << 1) | row.heading);
  if (m.screen == Screen::Settings) {
    k = mix(mix(k, static_cast<uint32_t>(m.settings_sel)), static_cast<uint32_t>(m.settings_scroll));
    for (const SettingRow& row : m.settings) k = mix(mix(mix(k, row.label), row.value), (static_cast<uint32_t>(row.kind) << 8) | row.level);
  }
  return mix(k, (m.hero ? 1u : 0u) | (m.yes.empty() ? 0u : 2u) | (m.no.empty() ? 0u : 4u));
}

void draw(const UiModel& m, uint16_t* fb, int w, int h, bool swap) {
  std::memset(fb, 0, static_cast<size_t>(w) * h * 2);
  if (w < kSize || h < kSize) return;
  // The layout's 466 square, centred on the panel.
  Surface s{fb + (h - kSize) / 2 * w + (w - kSize) / 2, w, kSize, swap};
  if (m.music_view) {
    draw_music(s, m);
    return;
  }
  switch (m.screen) {
    case Screen::Prompt: draw_ask(s, m); break;
    case Screen::Card: draw_card(s, m); break;
    case Screen::Offline: draw_offline(s, m); break;
    case Screen::Dashboard: draw_dash(s, m); break;
    case Screen::Settings: draw_settings(s, m); break;
    default: break;
  }
}

Hit hit(const UiModel& m, int x, int y, int w, int h) {
  x -= (w - kSize) / 2;
  y -= (h - kSize) / 2;
  if (m.music_view) {
    if (kBack.has(x, y, 22)) return Hit::Close;
    if (std::abs(y - kMusicY) > 46) return Hit::None;
    if (std::abs(x - kC) <= 46) return Hit::Action;
    if (std::abs(x - kC) <= 150) return x < kC ? Hit::Previous : Hit::Next;
    return Hit::None;
  }
  switch (m.screen) {
    case Screen::Prompt:
      if (!m.yes.empty() && yes_box(m).has(x, y, 10) && (m.no.empty() || x < kC)) return Hit::Yes;
      if (!m.no.empty() && no_box().has(x, y, 10)) return Hit::No;
      return Hit::None;
    case Screen::Card: return kBack.has(x, y, 22) ? Hit::Back : Hit::None;
    case Screen::Offline: return kAction.has(x, y, 12) ? Hit::Action : Hit::None;
    case Screen::Dashboard: return kBack.has(x, y, 22) ? Hit::Action : Hit::None;
    default: return Hit::None;
  }
}

bool ask_whole(const UiModel& m) { return ask_layout(m).whole; }

int card_cols() { return kCardWidth / 12; }  // the body font's columns are 12 px (text_units)
int card_rows() { return (kCardBottom - kCardTop) / kCardPitch; }
int dash_rows() { return kDashRows; }

int set_view_h() { return kSetBottom - kSetTop; }

int set_row_h(const UiModel& m, int i) { return kSetRow + (i == m.settings_sel ? set_extra(m) : 0); }

int set_row_top(const UiModel& m, int i) {
  return kSetPad + i * kSetPitch + (m.settings_sel >= 0 && i > m.settings_sel ? set_extra(m) : 0);
}

int set_max_scroll(const UiModel& m) {
  const int n = static_cast<int>(m.settings.size());
  return n ? std::max(0, set_row_top(m, n - 1) + set_row_h(m, n - 1) + kSetEnd - set_view_h()) : 0;
}

// The least the list moves for the selected row to be whole and where the glass is wide.
int set_scroll_for(const UiModel& m, int scroll) {
  if (!set_has(m, m.settings_sel)) return 0;
  const int top = set_row_top(m, m.settings_sel) - kSetPad;
  const int bottom = set_row_top(m, m.settings_sel) + set_row_h(m, m.settings_sel) + kSetEnd;
  if (bottom - scroll > set_view_h()) scroll = bottom - set_view_h();
  if (top < scroll) scroll = top;
  return std::max(0, std::min(scroll, set_max_scroll(m)));
}

int set_row_hit(const UiModel& m, int x, int y, int w, int h) {
  x -= (w - kSize) / 2;
  y -= (h - kSize) / 2;
  if (m.screen != Screen::Settings || x < 0 || x >= kSize || y < kSetTop || y >= kSetBottom) return -1;
  y += set_scroll(m) - kSetTop;
  for (int i = 0; i < static_cast<int>(m.settings.size()); ++i) {
    const int top = set_row_top(m, i);
    if (y >= top && y < top + set_row_h(m, i)) return i;
  }
  return -1;
}

int set_slider_at(const UiModel& m, int x, int y, int w, int h, bool anywhere) {
  if (m.screen != Screen::Settings) return -1;
  const Bar bar = set_bar(m);
  if (bar.x1 <= bar.x0) return -1;
  x -= (w - kSize) / 2;
  y -= (h - kSize) / 2;
  // A fingertip around the bar, while it is on the glass; once a slide has begun, any height.
  const int reach = 24;
  if (!anywhere && (bar.y < kSetTop || bar.y >= kSetBottom || std::abs(y - bar.y) > reach || x < bar.x0 - reach || x >= bar.x1 + reach))
    return -1;
  return std::max(0, std::min(100, (x - bar.x0) * 100 / (bar.x1 - bar.x0)));
}

}  // namespace hg::skin::view
