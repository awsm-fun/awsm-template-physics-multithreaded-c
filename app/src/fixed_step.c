// fixed_step.c — see fixed_step.h.

#include "fixed_step.h"

constexpr double MAX_GAP_MS = 100.0;
constexpr double STEP_MS = 1000.0 / (double)APP_STEP_HZ;

uint32_t fixed_step_advance(fixed_step *fs, app_state *app, double now_ms) {
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
        app_step(app, step_seconds);
        fs->accumulator_ms -= STEP_MS;
        steps += 1;
    }
    if (fs->accumulator_ms >= STEP_MS) {
        fs->accumulator_ms = 0.0; // hit the cap: drop the backlog
    }
    return steps;
}
