// The Eyes skin: an anti-aliased, full-colour face for round AMOLED panels.
//
// It replaces the mascot screens (Ready, Listening, Thinking, Responding) on boards that
// turn it on (DeviceProfile::skin_eyes); every other screen keeps the stock renderer.
// Like the stock renderer it reads no clock: time is the model's frame counter times the
// frame period, so the same sequence of models always produces the same pixels.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hg {

struct UiModel;

namespace skin {

// A 4-bit coverage glyph: `w` x `h` pixels, rows padded to a byte, two pixels per byte
// (high nibble first), placed at (`x`, `y`) from the pen on the ascender line.
struct Glyph {
  uint16_t cp;
  uint8_t w, h;
  int8_t x, y;
  uint8_t adv;
  uint32_t off;
};

struct Font {
  const Glyph* glyphs;  // sorted by code point
  uint16_t count;
  const uint8_t* bits;
  uint8_t ascent, line;
  uint8_t mid;  // rows from the ascender line to the middle of a capital
};

// One font per text scale of the pixel font (2, 3, 4), and large digits for every scale above.
extern const Font kText2, kText3, kText4, kBig;

// Smooth text for the whole UI: while on, Canvas::text draws these fonts instead of the 5x7
// pixel font, and text is measured and wrapped by their widths. One display, so one switch.
void set_text(bool on);
bool text_on();
// The font for a text scale, or null where the pixel font stays (scale 1).
const Font* font_for(int scale);
const Glyph* glyph(const Font& f, char32_t cp);  // null when the font has none
// A string's width in 1/64 of a pixel-font column, whatever the scale: what fits in
// `cols` columns is at most cols * 64. Code points without a glyph count a whole column.
int text_units(std::string_view s);

struct Rows {
  int y0, y1;  // panel rows [y0, y1)
};

class Eyes {
 public:
  // Whether the skin draws this model. Screens it does not know stay with the stock renderer.
  static bool wants(const UiModel& m);
  void set_frame_ms(uint32_t ms) { frame_ms_ = ms ? ms : 1; }
  // Draws what changed since the last call into the panel's RGB565 framebuffer (`full`:
  // everything). Returns the rows to send, sorted and merged; empty when nothing changed.
  std::vector<Rows> draw(const UiModel& m, uint16_t* fb, int w, int h, bool swap, bool full);

 private:
  struct Box {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // [x0, x1) x [y0, y1); empty when y0 >= y1
    bool empty() const { return y0 >= y1 || x0 >= x1; }
  };
  enum class Ring : uint8_t { None, Still, Pulse, Comet, Ticks };

  uint32_t frame_ms_ = 100;
  bool started_ = false;
  uint32_t last_frame_ = 0;
  uint32_t rng_ = 0x2545F491u;
  // eased face state
  float ew_ = 46, eh_ = 74, gap_ = 112, ey_ = 205, gx_ = 0, gy_ = 0, mouth_ = 0, level_ = 0;
  float acc_[3] = {255, 196, 107};
  float gaze_x_ = 0, gaze_y_ = 0;
  uint32_t next_gaze_ms_ = 0, next_blink_ms_ = 2400, blink_at_ms_ = 0;
  bool blinking_ = false;
  // what is on the panel now
  uint32_t face_key_ = 0, ring_key_ = 0, text_key_ = 0;
  Box face_box_, comet_box_;
  Ring ring_ = Ring::None;
  // One eye's coverage (0..255 per pixel, glow included), kept until its size changes: moving
  // the gaze then only copies it.
  std::vector<uint8_t> eye_;
  int eye_w_ = 0, eye_h_ = 0;
  uint32_t eye_key_ = 0;
  std::vector<int16_t> band_;  // per row: the ring band's two pixel runs (x0, x1, x0, x1)
  int band_w_ = 0;
};

}  // namespace skin
}  // namespace hg
