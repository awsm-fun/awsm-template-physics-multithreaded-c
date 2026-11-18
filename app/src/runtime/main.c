// RUNTIME: shared by every game; your game goes in app/src/game/ and dom/game.js
//
// main.c — native entry point: a headless run, no browser involved.
//
// It drives the app exactly the way the browser does (frame times feeding the
// fixed-step clock, input batched per page frame), with headless.c standing
// in for the renderer and the page, and your game's game_headless_* functions
// (app/src/game/headless.c) giving the scene and a script of input. Handy as a
// quick sanity check, under a debugger, or with sanitizers:
//
//   ./app/build/debug/app        # 5 simulated seconds
//   ./app/build/debug/app 20     # 20 simulated seconds
//
// The browser entry point is web_main.c.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <stdint.h>

#include "game/game.h"
#include "runtime/bridge.h"
#include "runtime/fixed_step.h"
#include "runtime/headless.h"
#include "runtime/runtime.h"

constexpr double FRAME_MS = 1000.0 / 60.0; // pretend display refresh

int main(int argc, char *argv[]) {
    double seconds = argc > 1 ? atof(argv[1]) : 5.0;
    if (!(seconds > 0.0)) {
        fprintf(stderr, "usage: %s [seconds]\n", argv[0]);
        return EXIT_FAILURE;
    }

    bridge_header *bridge = bridge_create((float)APP_STEP_HZ);
    if (bridge == nullptr) {
        fprintf(stderr, "error: could not allocate the bridge block\n");
        return EXIT_FAILURE;
    }
    static runtime_state runtime;      // static: it is large, no need to put it on the stack
    runtime_init(&runtime, bridge, 1); // 1: step Box3D on this thread only
    fixed_step clock_state = {0};
    headless_outbox outbox = {0};

    headless_set_canvas(bridge, 1600, 900);
    game_headless_scene(bridge, false);

    uint32_t steps = 0;
    uint32_t tick = bridge_frame_tick(bridge);
    for (double now = 0.0; now <= seconds * 1000.0; now += FRAME_MS) {
        game_headless_input(bridge, now);

        // What the page does at the end of its frame, what the renderer does
        // at the start of its frame, then what the app loop in web_main.c
        // does when it wakes. (This frame's input is not old enough for this
        // frame's burst; it goes in the next one, as in the browser.)
        uint32_t now_us = (uint32_t)llround(now * 1000.0);
        headless_frame_mark(bridge, now_us);
        bridge_signal_frame_at(bridge, now_us);
        tick = bridge_wait_frame(bridge, tick);
        runtime_frame_input(&runtime, now_us, FRAME_MS);
        steps += fixed_step_advance(&clock_state, &runtime, now);
        headless_drain(bridge, &outbox);
    }

    printf("headless run: %.1f s simulated, %u frames, %u steps (%d Hz)\n", seconds, tick, steps,
           APP_STEP_HZ);
    game_headless_report(&runtime);
    printf("  sent: %u bind, %u spawn, %u despawn, %u cues\n", outbox.binds, outbox.spawns,
           outbox.despawns, outbox.cues);

    bool ok = runtime_scene_ready(&runtime);
    runtime_destroy(&runtime);
    bridge_destroy(bridge);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
