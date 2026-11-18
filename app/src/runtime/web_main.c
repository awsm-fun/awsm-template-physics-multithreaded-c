// RUNTIME: shared by every game; your game goes in app/src/game/ and dom/game.js
//
// web_main.c — browser entry point (wasm builds only; native uses main.c).
//
// app/web/worker.js loads this module in a dedicated worker and calls:
//
//   app_start()  once: creates the bridge block and returns its address, which
//                the worker hands to the page together with the memory.
//   app_run()    then: the frame-paced loop. It never returns.
//
// Because app_run() never returns, this worker never gets back to its JS
// event loop and cannot receive postMessage. That is intended: everything
// coming in arrives through the bridge block instead.
//
// printf still works on this thread while the loop runs: Emscripten turns
// each finished line into a synchronous console.log() call. Output is
// line-buffered, so end messages with '\n'. (Code running on Box3D's solver
// threads should not use stdio; see report() in physics.c.)
//
// EMSCRIPTEN_KEEPALIVE exports a function to JS as `module._name`.

#include <emscripten/em_macros.h>
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "game/game.h"
#include "runtime/bridge.h"
#include "runtime/fixed_step.h"
#include "runtime/runtime.h"

// Prototypes keep -Wmissing-prototypes quiet; JS is the only caller.
EMSCRIPTEN_KEEPALIVE uintptr_t app_start(void);
EMSCRIPTEN_KEEPALIVE void app_run(void);

// Box3D steps the world on several threads: this one plus up to this many
// more. Emscripten runs each extra thread in a web worker that shares this
// module's memory. Those workers are created while the module loads
// (-sPTHREAD_POOL_SIZE in CMakeLists.txt, which repeats the arithmetic of
// solver_thread_count() below; keep the two in step), because once
// app_run() starts this thread never returns to its event loop and could
// not create one.
constexpr int APP_MAX_SOLVER_THREADS = 4;
// Cores left alone for the threads that already exist: page, renderer, app.
constexpr int APP_OTHER_THREADS = 3;

// How many threads to step on, counting this one. On a machine with few
// cores this is 1, and Box3D simply runs everything here.
static uint32_t solver_thread_count(void) {
    int spare = emscripten_num_logical_cores() - APP_OTHER_THREADS;
    if (spare < 0) {
        spare = 0;
    }
    if (spare > APP_MAX_SOLVER_THREADS) {
        spare = APP_MAX_SOLVER_THREADS;
    }
    return (uint32_t)spare + 1u;
}

static bridge_header *bridge;
static runtime_state runtime;
static fixed_step stepper;

uintptr_t app_start(void) {
    if (bridge == nullptr) {
        bridge = bridge_create((float)APP_STEP_HZ);
        if (bridge == nullptr) {
            fprintf(stderr, "app: could not allocate the bridge block\n");
            return 0;
        }
        uint32_t threads = solver_thread_count();
        runtime_init(&runtime, bridge, threads);
        // For the page's stats panel, which shows what the physics runs on.
        atomic_store_explicit(&bridge_get_stats(bridge)->solver_threads, threads,
                              memory_order_relaxed);
        printf("app: bridge block ready (%u bytes), stepping at %d Hz, Box3D on %u thread%s\n",
               bridge->total_size, APP_STEP_HZ, threads, threads == 1 ? "" : "s");
    }
    return (uintptr_t)bridge;
}

// The frame clock (bridge.h, "Frame pacing") as this loop keeps it: a
// running count of microseconds that never wraps, whose low 32 bits are the
// frame-clock reading. Fed by the frame times the renderer stores with each
// tick; with a renderer from before frame times, read from the same clock
// directly (emscripten_get_now() is performance.timeOrigin + now() here).
typedef struct frame_clock {
    bool started;
    bool from_renderer; // the latest reading came from the renderer
    int64_t now_us;
    uint32_t last_us; // the latest reading, as it arrived (wrapped)
    double period_ms; // the display's frame period, estimated from the readings
    bool period_known;
} frame_clock;

static void clock_advance_to(frame_clock *clock, uint32_t reading_us) {
    if (!clock->started) {
        clock->started = true;
        clock->now_us = reading_us;
    } else {
        // Wrapping difference; a reading older than the last one (it cannot
        // happen with a single renderer) does not move the clock back.
        int32_t delta = (int32_t)(reading_us - (uint32_t)clock->now_us);
        if (delta > 0) {
            clock->now_us += delta;
        }
    }
}

static void clock_read(frame_clock *clock, bridge_header *block) {
    uint32_t reading_us;
    if (bridge_frame_time(block, &reading_us)) {
        if (clock->started && clock->from_renderer) {
            // Frame period: follows a shorter interval quickly (a faster
            // display, or the first estimate) and a longer one only slowly,
            // so a dropped frame does not double it.
            double interval_ms = (double)(int32_t)(reading_us - clock->last_us) / 1000.0;
            if (interval_ms > 0.0) {
                if (!clock->period_known) {
                    clock->period_ms = interval_ms;
                    clock->period_known = true;
                } else if (interval_ms < clock->period_ms) {
                    clock->period_ms += (interval_ms - clock->period_ms) * 0.5;
                } else if (interval_ms < clock->period_ms * 1.5) {
                    clock->period_ms += (interval_ms - clock->period_ms) * 0.05;
                }
            }
        }
        clock->from_renderer = true;
    } else {
        reading_us = (uint32_t)llround(emscripten_get_now() * 1000.0);
        clock->from_renderer = false;
    }
    clock_advance_to(clock, reading_us);
    clock->last_us = reading_us;
}

void app_run(void) {
    if (bridge == nullptr) {
        fprintf(stderr, "app: app_run() called before app_start()\n");
        return;
    }
    bridge_stats *stats = bridge_get_stats(bridge);

    // Wake once per frame, at its start, then run the steps the time since
    // the previous frame pays for. Timing them by the frame times the
    // renderer stores, rather than by when this thread happens to wake, keeps
    // the step count per frame steady (4, 4, 4 at 60 Hz) and lets the
    // renderer interpolate to an exact moment (bridge.h, "Frame pacing").
    frame_clock clock = {.period_ms = 1000.0 / 60.0};
    uint32_t tick = bridge_frame_tick(bridge);
    for (;;) {
        tick = bridge_wait_frame(bridge, tick);

        // Breakdowns only while the stats panel shows them.
        uint32_t detail = bridge_detail(bridge);
        runtime.profile = (detail & BRIDGE_DETAIL_APP) != 0;
        runtime.profile_physics = (detail & BRIDGE_DETAIL_PHYSICS) != 0;
        double start_ms = emscripten_get_now();
        clock_read(&clock, bridge);
        runtime_frame_input(&runtime, (uint32_t)clock.now_us, clock.period_ms);
        uint32_t steps = fixed_step_advance(&stepper, &runtime, (double)clock.now_us / 1000.0);
        if (steps > 0) {
            double work_us = (emscripten_get_now() - start_ms) * 1000.0;
            atomic_fetch_add_explicit(&stats->step_work_us, (uint32_t)(work_us + 0.5),
                                      memory_order_relaxed);
            atomic_fetch_add_explicit(&stats->physics_step_us,
                                      (uint32_t)(runtime.physics_ms * 1000.0 + 0.5),
                                      memory_order_relaxed);
            runtime.physics_ms = 0.0;
        }
    }
}
