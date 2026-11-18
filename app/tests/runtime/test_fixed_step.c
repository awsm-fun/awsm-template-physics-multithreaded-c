// RUNTIME: shared by every game; your game goes in app/src/game/ and dom/game.js
//
// Tests for fixed_step.c: frame times in, whole steps out, each snapshot
// stamped with the moment its step stands for. No scene is ever described,
// so of your game's hooks only game_init runs: this holds whatever the game
// is.

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include "game/game.h"
#include "runtime/bridge.h"
#include "runtime/check.h"
#include "runtime/fixed_step.h"
#include "runtime/runtime.h"

// Snapshot `step`, as the renderer would find it: step N is in ring entry
// N & (count - 1).
static bridge_snapshot_header *snapshot_of(bridge_header *bridge, uint32_t step) {
    unsigned char *base = (unsigned char *)bridge + bridge->snapshot_offset;
    return (bridge_snapshot_header *)(void *)(base + (step & (bridge->snapshot_count - 1u)) *
                                                         bridge->snapshot_stride);
}

static void test_snapshot_times_follow_the_accumulator(void) {
    // Each snapshot carries the moment its step stands for: the newest one
    // of a burst `now - leftover`, the ones before it a step apart. Ten
    // seconds of 60 Hz frames must not let that drift against the frame
    // times by even one microsecond per frame.
    bridge_header *bridge = bridge_create((float)APP_STEP_HZ);
    runtime_state *runtime = calloc(1, sizeof *runtime); // large: not on the stack
    runtime_init(runtime, bridge, 1);
    fixed_step clock_state = {0};
    const double start_ms = 4'294'000.0; // the clock wraps (at 2^32 µs) during the run
    const double frame_ms = 1000.0 / 60.0;
    const double step_us = 1e6 / (double)APP_STEP_HZ;
    (void)fixed_step_advance(&clock_state, runtime, start_ms); // starts the clock
    for (int frame = 1; frame <= 600; ++frame) {
        double now_ms = start_ms + frame * frame_ms;
        uint32_t steps = fixed_step_advance(&clock_state, runtime, now_ms);
        CHECK(steps >= 3 && steps <= 5);
        uint32_t now_us = (uint32_t)llround(now_ms * 1000.0);
        uint32_t newest = snapshot_of(bridge, atomic_load(&bridge->latest_step))->time_us;
        // `now - leftover`: behind the frame time by less than one step.
        int32_t behind = (int32_t)(now_us - newest);
        CHECK(behind >= 0 && behind <= (int32_t)(step_us + 1.0));
        CHECK_NEAR((double)behind, clock_state.accumulator_ms * 1000.0, 1.0);
        // The step before it, exactly one step earlier.
        uint32_t previous = snapshot_of(bridge, runtime->step - 1u)->time_us;
        CHECK_NEAR((double)(int32_t)(newest - previous), step_us, 1.0);
    }
    CHECK(runtime->step >= 2399 && runtime->step <= 2400); // 10 s at 240 Hz
    runtime_destroy(runtime);
    free(runtime);
    bridge_destroy(bridge);
}

int main(void) {
    test_snapshot_times_follow_the_accumulator();
    return check_report();
}
