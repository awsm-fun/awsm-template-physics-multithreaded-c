// headless.h — stand-ins for the page and the renderer, for running the app
// natively (the headless run in main.c, and the tests).
//
// In the browser these records come from dom/main.js and the Rust renderer.
// Here they are written straight into the same bridge rings, so the app code
// cannot tell the difference.

#ifndef HEADLESS_H
#define HEADLESS_H

#include "bridge.h"

// Plays the renderer's part of loading the bundled scene (media/bundle): a
// SCENE_BEGIN, the table, its four rails and the ball as SCENE_COLLIDERs,
// then SCENE_READY.
void headless_send_scene(bridge_header *bridge);

// Plays the page's part of user input.
void headless_set_canvas(bridge_header *bridge, uint32_t width, uint32_t height);
void headless_key(bridge_header *bridge, uint32_t key, bool down); // event + held state
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
    uint32_t cues;                              // all BRIDGE_CUE records
    uint32_t cues_by_id[HEADLESS_CUE_KINDS];    // ... per cue id (u[0])
    float last_cue_value[HEADLESS_CUE_KINDS];   // f[1] of the latest cue with each id
    uint32_t last_spawn_slot;
    uint32_t last_spawn_node; // node hash of the most recent SPAWN
} headless_outbox;

// Empties the app → renderer and app → dom rings (nobody else reads them in a
// native run), adding what was in them to `outbox`.
void headless_drain(bridge_header *bridge, headless_outbox *outbox);

#endif // HEADLESS_H
