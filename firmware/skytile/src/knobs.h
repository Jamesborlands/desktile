#pragma once

// Bits returned by knobs_state()
enum { KNOB_BOTTOM = 1, KNOB_TOP = 2 };

void knobs_init();      // probe both knobs (call after Wire is up)
void knobs_poll();      // call often from the main loop; sends app_input() events
int knobs_state();      // which knobs are connected (KNOB_* bits)
