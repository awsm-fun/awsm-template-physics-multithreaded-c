// main.c — native entry point: a headless run, no browser involved.
//
// It drives the app exactly the way the browser does (frame times feeding the
// fixed-step clock, input batched per page frame), with headless.c standing
// in for the renderer and the page. Handy as a quick sanity check, under a debugger, or with
// sanitizers:
//
//   ./app/build/debug/app        # 5 simulated seconds
//   ./app/build/debug/app 20     # 20 simulated seconds
//
// The browser entry point is web_main.c.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "app.h"
#include "fixed_step.h"
#include "headless.h"

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
    static app_state app;      // static: it is large, no need to put it on the stack
    app_init(&app, bridge, 1); // 1: step Box3D on this thread only
    fixed_step clock_state = {0};
    headless_outbox outbox = {0};

    headless_set_canvas(bridge, 1600, 900);
    headless_send_scene(bridge);

    // A little script of user input, at fixed points in simulated time.
    bool clicked = false;
    bool rolling = false;
    bool stopped = false;
    bool jumped = false;

    uint32_t steps = 0;
    uint32_t tick = bridge_frame_tick(bridge);
    for (double now = 0.0; now <= seconds * 1000.0; now += FRAME_MS) {
        if (!clicked && now >= 1000.0) {
            headless_click(bridge, 0.25f, 0.0f); // drop a ball right of centre
            clicked = true;
        }
        if (!rolling && now >= 2000.0) {
            headless_key(bridge, BRIDGE_KEY_D, true);
            rolling = true;
        }
        if (!stopped && now >= 2500.0) {
            headless_key(bridge, BRIDGE_KEY_D, false);
            stopped = true;
        }
        if (!jumped && now >= 3000.0) {
            headless_key(bridge, BRIDGE_KEY_SPACE, true);
            headless_key(bridge, BRIDGE_KEY_SPACE, false);
            jumped = true;
        }

        // What the page does at the end of its frame, what the renderer does
        // at the start of its frame, then what the app loop in web_main.c
        // does when it wakes. (This frame's input is not old enough for this
        // frame's burst; it goes in the next one, as in the browser.)
        uint32_t now_us = (uint32_t)llround(now * 1000.0);
        headless_frame_mark(bridge, now_us);
        bridge_signal_frame_at(bridge, now_us);
        tick = bridge_wait_frame(bridge, tick);
        app_frame_input(&app, now_us, FRAME_MS);
        steps += fixed_step_advance(&clock_state, &app, now);
        headless_drain(bridge, &outbox);
    }

    const app_ball *ball = &app.balls[APP_PLAYER_SLOT];
    printf("headless run: %.1f s simulated, %u frames, %u steps (%d Hz)\n", seconds, tick, steps,
           APP_STEP_HZ);
    printf("  scene ready: %s, active balls: %d\n", app.scene.ready ? "yes" : "no",
           app_active_balls(&app));
    b3Vec3 velocity = b3Body_GetLinearVelocity(ball->body);
    printf("  player ball at (%.3f, %.3f, %.3f), speed %.3f m/s\n", (double)ball->position.x,
           (double)ball->position.y, (double)ball->position.z, (double)b3Length(velocity));
    printf("  sent: %u bind, %u spawn, %u despawn, %u cues\n", outbox.binds, outbox.spawns,
           outbox.despawns, outbox.cues);

    bool ok = app.scene.ready;
    app_destroy(&app);
    bridge_destroy(bridge);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
