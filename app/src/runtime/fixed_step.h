// RUNTIME: shared by every game; your game goes in app/src/game/ and dom/game.js
//
// fixed_step.h — turns "time has passed" into a whole number of app steps.
//
// The simulation always advances in steps of exactly 1 / APP_STEP_HZ seconds,
// however unevenly the thread wakes up. Real time goes into an accumulator
// and comes out as as many whole steps as it pays for; the remainder carries
// over to the next call. A step is never split or stretched, so the
// simulation behaves the same on a 60 Hz and a 144 Hz display.

#ifndef FIXED_STEP_H
#define FIXED_STEP_H

#include <stdint.h>

#include "runtime/runtime.h"

typedef struct fixed_step {
    bool started;
    double last_ms;        // time of the previous call
    double accumulator_ms; // real time not yet turned into steps
} fixed_step;

// Runs runtime_step() as many times as the real time since the previous call has
// bought, and returns how many it ran. `now_ms` is any monotonic clock in
// milliseconds. The first call only starts the clock.
//
// Before each step it sets `runtime->step_time_ms` to the moment that step
// stands for on the same clock: `now_ms` minus the time left over after it.
// So the newest step of a burst stands for `now_ms - leftover`, which is what
// lets the renderer interpolate to an exact moment (see bridge.h, "Frame
// pacing").
//
// Two limits keep a slow moment from snowballing (the "spiral of death",
// where catching up takes so long that there is even more to catch up next
// time): a single gap longer than 100 ms (a background tab, a debugger
// pause) counts as 100 ms, and one call never runs more steps than 100 ms
// worth. Time beyond that is dropped: the world slows down rather than
// freezing the thread.
uint32_t fixed_step_advance(fixed_step *fs, runtime_state *runtime, double now_ms);

#endif // FIXED_STEP_H
