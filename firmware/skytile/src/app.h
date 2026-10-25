#pragma once

// Logical inputs. Touch and (later) the two knobs both feed app_input().
enum InputEvt {
  IN_MODE_NEXT,   // top knob turn right
  IN_MODE_PREV,   // top knob turn left
  IN_MODE_PRESS,  // top knob press: next hotspot (San Diego, Hormuz, Dover...)
  IN_MODE_HOLD,   // top knob hold: home to San Diego
  IN_SEL_NEXT,    // bottom knob turn right
  IN_SEL_PREV,    // bottom knob turn left
  IN_SEL_PRESS,   // bottom knob press: open details
  IN_SEL_HOLD,    // bottom knob hold: toggle favorite
};

void app_init();
void app_loop();
void app_input(InputEvt e);
// Raw touch from the driver: n fingers (0-2) and their positions. Drives pinch zoom.
void app_touch(int n, int x0, int y0, int x1, int y1);
void app_demo_todos();      // debug over USB: show made-up to-dos (for screenshots)
void app_sleep_on(int mode);  // debug over USB: switch mode (0..3) and go to sleep now
