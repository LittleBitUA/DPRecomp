// [new_fix_25092026_glitch]
// Tests for keyboard driving (src/deadlyprem_keyboard_driving.h). Plain
// executable, exit code 0 = pass. Build target dp_keyboard_driving_test.
#include <cstdint>
#include <cstdio>

#include "../src/deadlyprem_keyboard_driving.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

struct Pad {
  int16_t ly;
  uint8_t lt, rt;
};

Pad Run(bool in_vehicle, int16_t ly, uint8_t lt = 0, uint8_t rt = 0, bool* changed = nullptr) {
  Pad p{ly, lt, rt};
  const bool c = dp::ApplyKeyboardDriving(in_vehicle, p.ly, p.lt, p.rt);
  if (changed) *changed = c;
  return p;
}

}  // namespace

int main() {
  // On foot nothing changes: W/S walk, Space/Ctrl stay the triggers.
  {
    bool changed = true;
    Pad p = Run(false, 32767, 0, 0, &changed);
    CHECK(!changed && p.ly == 32767 && p.lt == 0 && p.rt == 0);
    p = Run(false, -32767, 255, 0);
    CHECK(p.ly == -32767 && p.lt == 255);
  }
  // In the car: W = full gas (RT), stick Y cleared.
  {
    bool changed = false;
    Pad p = Run(true, 32767, 0, 0, &changed);
    CHECK(changed && p.ly == 0 && p.rt == 255 && p.lt == 0);
  }
  // S = full brake / reverse (LT); INT16_MIN clamps to 255.
  {
    Pad p = Run(true, -32767);
    CHECK(p.ly == 0 && p.lt == 255 && p.rt == 0);
    p = Run(true, -32768);
    CHECK(p.ly == 0 && p.lt == 255);
  }
  // The keyboard ramp (mnk_key_stick_ramp_ms) carries over: half stick = half trigger.
  {
    Pad p = Run(true, 16384);
    CHECK(p.rt >= 127 && p.rt <= 128 && p.ly == 0);
    p = Run(true, 1);
    CHECK(p.rt <= 1);
  }
  // Space / Ctrl still work and are never weakened.
  {
    Pad p = Run(true, 8000, 0, 255);
    CHECK(p.rt == 255);
    p = Run(true, 0, 200, 255);
    CHECK(p.lt == 200 && p.rt == 255);
    bool changed = true;
    Run(true, 0, 0, 0, &changed);
    CHECK(!changed);
  }
  // Space held = the keyboard layer's aim state: stick Y is mouse motion (with a
  // per-axis floor, 1 px down = -8989). Untouched, so no brake under full gas.
  // Critic finding 25.09: this used to give LT = 70 with RT = 255.
  {
    bool changed = true;
    Pad p = Run(true, -8989, 0, 255, &changed);
    CHECK(!changed && p.lt == 0 && p.rt == 255 && p.ly == -8989);
    p = Run(true, -32768, 0, 255, &changed);  // a big downward flick
    CHECK(!changed && p.lt == 0 && p.ly == -32768);
    p = Run(true, 20000, 0, 1, &changed);  // any nonzero RT
    CHECK(!changed && p.rt == 1 && p.ly == 20000);
  }
  // Ctrl adds no mouse motion to the stick, so S with Ctrl held still maps.
  {
    Pad p = Run(true, -32767, 120, 0);
    CHECK(p.lt == 255 && p.ly == 0 && p.rt == 0);
  }
  // W and Ctrl together: gas from W, brake from Ctrl (the game decides).
  {
    Pad p = Run(true, 32767, 255, 0);
    CHECK(p.rt == 255 && p.lt == 255 && p.ly == 0);
  }
  // Monotonic in the stick: more W never means less gas.
  {
    uint8_t last = 0;
    for (int v = 0; v <= 32767; v += 97) {
      Pad p = Run(true, static_cast<int16_t>(v));
      CHECK(p.rt >= last);
      last = p.rt;
    }
    CHECK(last >= 254);
  }
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("keyboard_driving_test: all passed\n");
  return 0;
}
