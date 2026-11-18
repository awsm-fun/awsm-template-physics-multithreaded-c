// RUNTIME: shared by every game; your game goes in app/src/game/ and dom/game.js
//
// fixed_step.c — see fixed_step.h.

#include "runtime/fixed_step.h"

#include <stdint.h>

#include "game/game.h"
#include "runtime/bridge.h"
#include "runtime/runtime.h"

constexpr int MAX_GAP_WHOLE_MS = 100;
constexpr double MAX_GAP_MS = MAX_GAP_WHOLE_MS;
constexpr double STEP_MS = 1000.0 / (double)APP_STEP_HZ;

// The renderer reads the two steps around the moment it draws while the app
// writes its next burst into the same ring, so a burst must never reach back
// to them: the ring has to hold the largest burst plus those two.
static_assert(MAX_GAP_WHOLE_MS * APP_STEP_HZ / 1000 + 1 + 2 <= (int)BRIDGE_SNAPSHOT_COUNT,
              "the snapshot ring is too short for the largest burst of steps");

uint32_t fixed_step_advance(fixed_step *fs, runtime_state *runtime, double now_ms) {
    const float step_seconds = (float)(STEP_MS / 1000.0);
    // Enough steps to cover MAX_GAP_MS, rounded up.
    const uint32_t max_steps = (uint32_t)(MAX_GAP_MS / STEP_MS) + 1u;

    if (!fs->started) {
        fs->started = true;
        fs->last_ms = now_ms;
        return 0;
    }

    double gap = now_ms - fs->last_ms;
    fs->last_ms = now_ms;
    if (gap < 0.0) {
        gap = 0.0;
    }
    if (gap > MAX_GAP_MS) {
        gap = MAX_GAP_MS;
    }
    fs->accumulator_ms += gap;

    uint32_t steps = 0;
    while (fs->accumulator_ms >= STEP_MS && steps < max_steps) {
        fs->accumulator_ms -= STEP_MS;
        // The moment this step stands for: `now` minus the time still left
        // over after it. Worked out from the accumulator every time, never
        // by adding STEP_MS to the previous step's time, which would drift.
        runtime->step_time_ms = now_ms - fs->accumulator_ms;
        runtime_step(runtime, step_seconds);
        steps += 1;
    }
    return steps;
}
