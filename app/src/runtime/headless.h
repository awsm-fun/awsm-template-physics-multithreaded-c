// RUNTIME: shared by every game; your game goes in app/src/game/ and dom/game.js
//
// headless.h — stand-ins for the page and the renderer, for running the app
// natively (the headless run in main.c, and the tests).
//
// In the browser these records come from dom/runtime/main.js and the Rust
// renderer. Here they are written straight into the same bridge rings, so the
// runtime and your game cannot tell the difference.

#ifndef HEADLESS_H
#define HEADLESS_H

#include <stdint.h>

#include "runtime/bridge.h"
#include "runtime/runtime.h"

// Plays the renderer's part of describing a scene: a SCENE_BEGIN (`used`: a
// previous app instance has driven the scene, as a hot-swapped app is told),
// one SCENE_COLLIDER per collider, then a SCENE_READY.
void headless_scene_begin(bridge_header *bridge, bool used);
// One collider node, unrotated, at `position` in world space. `name` is the
// collider node's own name, `top_level` its top-level ancestor's; `shape` is a
// BRIDGE_SHAPE_* and `dims` its dimensions.
void headless_collider(bridge_header *bridge, const char *name, const char *top_level,
                       uint32_t shape, const float dims[3], const float position[3]);
void headless_scene_ready(bridge_header *bridge, uint32_t collider_count);

// Plays the page's part of user input.
void headless_set_canvas(bridge_header *bridge, uint32_t width, uint32_t height);
void headless_key(bridge_header *bridge, uint32_t key, bool down);    // event + held state
void headless_click(bridge_header *bridge, float ndc_x, float ndc_y); // left mouse button
// A right-button mouse drag by (dx, dy) CSS pixels.
void headless_right_drag(bridge_header *bridge, float dx_px, float dy_px);
// A finger swiping (dx, dy) CSS pixels at a steady speed over `duration_ms`.
void headless_swipe(bridge_header *bridge, float dx_px, float dy_px, uint32_t duration_ms);
// The end of a page frame: a FRAME_MARK at `time_us` on the frame clock.
void headless_frame_mark(bridge_header *bridge, uint32_t time_us);

// Counts of what the app sent out, collected by headless_drain().
enum { HEADLESS_CUE_KINDS = 8 };

typedef struct headless_outbox {
    uint32_t binds;
    uint32_t spawns;
    uint32_t despawns;
    uint32_t reloads;
    uint32_t cues;                            // all BRIDGE_CUE records
    uint32_t cues_by_id[HEADLESS_CUE_KINDS];  // ... per cue id (u[0])
    float last_cue_value[HEADLESS_CUE_KINDS]; // f[1] of the latest cue with each id
    uint32_t last_spawn_slot;
    uint32_t last_spawn_node; // node hash of the most recent SPAWN
} headless_outbox;

// Empties the app → renderer and app → dom rings (nobody else reads them in a
// native run), adding what was in them to `outbox`.
void headless_drain(bridge_header *bridge, headless_outbox *outbox);

// ═══ Your game provides (app/src/game/headless.c) ══════════════════════════

// Describes your scene the way the renderer would after loading
// media/bundle/ (with headless_scene_begin, headless_collider,
// headless_scene_ready): the native runs and the tests never load the bundle.
void game_headless_scene(bridge_header *bridge, bool used);

// The headless run's script of user input (main.c): called once per
// simulated page frame, at `now_ms` since the start, before that frame's mark.
void game_headless_input(bridge_header *bridge, double now_ms);

// What the headless run prints about your game at the end.
void game_headless_report(const runtime_state *runtime);

#endif // HEADLESS_H
