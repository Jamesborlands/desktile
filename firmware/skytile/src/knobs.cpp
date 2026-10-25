// Two Adafruit #5880 I2C rotary encoders (seesaw firmware, product 4991) on the
// panel's I2C port (same bus as touch/backlight: SDA 15, SCL 16).
//   0x37 = TOP knob (Mode): turn = switch mode, press = cycle filter, hold = next hotspot,
//          hold 2 s = home
//   0x36 = BOTTOM knob (Select): turn = move selection, press = act, hold = favorite
// Both ship at 0x36. The top knob has solder jumper A0 bridged (0x37); software address
// changes don't stick on these boards, so there is no setup step.
// Polled from the main loop, the same task that reads touch, so the bus is never shared
// between tasks.
#include "knobs.h"

#include <Adafruit_seesaw.h>
#include <Arduino.h>

#include "app.h"

static const uint8_t ADDR_BOTTOM = 0x36, ADDR_TOP = 0x37;
static const uint8_t SS_SWITCH = 24;
static const uint32_t PRODUCT = 4991;
static const uint32_t POLL_MS = 25, RESCAN_MS = 3000, HOLD_MS = 600, LONG_MS = 2000;
static const int DIR = 1;  // flip to -1 if clockwise turns the wrong way

struct Knob {
  Adafruit_seesaw ss;
  uint8_t addr;
  bool present = false;
  int32_t last_pos = 0;
  bool down = false;
  uint32_t down_at = 0;
  bool held = false;  // hold already fired for this press
};
static Knob k_bottom, k_top;

static bool probe(Knob &k) {
  if (!k.ss.begin(k.addr)) return false;
  if (((k.ss.getVersion() >> 16) & 0xFFFF) != PRODUCT) return false;
  k.ss.pinMode(SS_SWITCH, INPUT_PULLUP);
  k.last_pos = k.ss.getEncoderPosition();
  k.down = false;
  k.held = false;
  Serial.printf("[knob] found at 0x%02X\n", k.addr);
  return true;
}

// The top knob has an address jumper bridged (A0 = 0x37, A1 = 0x38, ...): accept any of them.
static bool probe_top() {
  for (uint8_t a = ADDR_TOP; a <= 0x3D; a++) {
    k_top.addr = a;
    if (probe(k_top)) return true;
  }
  k_top.addr = ADDR_TOP;
  return false;
}

void knobs_init() {
  k_bottom.addr = ADDR_BOTTOM;
  k_top.addr = ADDR_TOP;
  k_bottom.present = probe(k_bottom);
  k_top.present = probe_top();
}

int knobs_state() { return (k_bottom.present ? KNOB_BOTTOM : 0) | (k_top.present ? KNOB_TOP : 0); }

// One knob: turns become steps, presses become press/hold events. With a long-hold
// event the hold fires on release (so a long hold doesn't also fire the short one);
// without one it fires as soon as the hold time is reached.
static void poll_one(Knob &k, InputEvt next, InputEvt prev, InputEvt press, InputEvt hold, int long_hold = -1) {
  int32_t pos = k.ss.getEncoderPosition();
  int32_t d = (pos - k.last_pos) * DIR;
  k.last_pos = pos;
  for (; d > 0; d--) app_input(next);
  for (; d < 0; d++) app_input(prev);

  bool down = !k.ss.digitalRead(SS_SWITCH);
  uint32_t now = millis();
  if (down && !k.down) { k.down_at = now; k.held = false; }
  uint32_t t = now - k.down_at;
  if (down && !k.held && long_hold < 0 && hold != press && t >= HOLD_MS) { k.held = true; app_input(hold); }
  if (down && !k.held && long_hold >= 0 && t >= LONG_MS) { k.held = true; app_input((InputEvt)long_hold); }
  if (!down && k.down && !k.held) app_input(long_hold >= 0 && t >= HOLD_MS ? hold : press);  // fires on release
  k.down = down;
}

void knobs_poll() {
  static uint32_t last_poll = 0, last_scan = 0;
  uint32_t now = millis();
  if (now - last_scan >= RESCAN_MS) {  // pick up knobs plugged in while running
    last_scan = now;
    if (!k_bottom.present) k_bottom.present = probe(k_bottom);
    if (!k_top.present) k_top.present = probe_top();
  }
  if (now - last_poll < POLL_MS) return;
  last_poll = now;
  if (k_bottom.present) poll_one(k_bottom, IN_SEL_NEXT, IN_SEL_PREV, IN_SEL_PRESS, IN_SEL_HOLD);
  if (k_top.present) poll_one(k_top, IN_MODE_NEXT, IN_MODE_PREV, IN_MODE_PRESS, IN_MODE_HOLD, IN_MODE_LONG);
}
