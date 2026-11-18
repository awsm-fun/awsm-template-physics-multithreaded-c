// YOUR GAME: start here
//
// headless.c — your game without a browser: the scene as the renderer would
// describe it, the native run's script of input, and what that run reports
// (see "Your game provides" in app/src/runtime/headless.h). Used by
// `task app:run` and the tests; the browser build never calls it.

#include <box3d/box3d.h>
#include <stdint.h>
#include <stdio.h>

#include "game/game.h"
#include "runtime/bridge.h"
#include "runtime/headless.h"
#include "runtime/runtime.h"

void game_headless_scene(bridge_header *bridge, bool used) {
    headless_scene_begin(bridge, used);
    // The colliders of media/bundle/scene.toml, in world space.
    headless_collider(bridge, "Tabletop_Collider", "Table", BRIDGE_SHAPE_BOX,
                      (const float[3]){3.0f, 0.1f, 2.0f}, (const float[3]){0.0f, -0.1f, 0.0f});
    headless_collider(bridge, "Wall_South_Collider", "Table", BRIDGE_SHAPE_BOX,
                      (const float[3]){3.0f, 0.2f, 0.05f}, (const float[3]){0.0f, 0.2f, 1.95f});
    headless_collider(bridge, "Wall_North_Collider", "Table", BRIDGE_SHAPE_BOX,
                      (const float[3]){3.0f, 0.2f, 0.05f}, (const float[3]){0.0f, 0.2f, -1.95f});
    headless_collider(bridge, "Wall_East_Collider", "Table", BRIDGE_SHAPE_BOX,
                      (const float[3]){0.05f, 0.2f, 2.0f}, (const float[3]){2.95f, 0.2f, 0.0f});
    headless_collider(bridge, "Wall_West_Collider", "Table", BRIDGE_SHAPE_BOX,
                      (const float[3]){0.05f, 0.2f, 2.0f}, (const float[3]){-2.95f, 0.2f, 0.0f});
    headless_collider(bridge, "Ball_Collider", "Ball", BRIDGE_SHAPE_SPHERE,
                      (const float[3]){0.3f, 0.0f, 0.0f}, (const float[3]){0.0f, 0.3f, 0.0f});
    headless_scene_ready(bridge, 6);
}

// A little script of user input, at fixed points in simulated time: drop a
// ball, roll right for half a second, jump.
void game_headless_input(bridge_header *bridge, double now_ms) {
    static bool clicked = false;
    static bool rolling = false;
    static bool stopped = false;
    static bool jumped = false;
    if (!clicked && now_ms >= 1000.0) {
        headless_click(bridge, 0.25f, 0.0f); // drop a ball right of centre
        clicked = true;
    }
    if (!rolling && now_ms >= 2000.0) {
        headless_key(bridge, BRIDGE_KEY_D, true);
        rolling = true;
    }
    if (!stopped && now_ms >= 2500.0) {
        headless_key(bridge, BRIDGE_KEY_D, false);
        stopped = true;
    }
    if (!jumped && now_ms >= 3000.0) {
        headless_key(bridge, BRIDGE_KEY_SPACE, true);
        headless_key(bridge, BRIDGE_KEY_SPACE, false);
        jumped = true;
    }
}

void game_headless_report(const runtime_state *runtime) {
    const runtime_body *ball = runtime_get_body(runtime, GAME_PLAYER_SLOT);
    printf("  scene ready: %s, active balls: %d\n", runtime_scene_ready(runtime) ? "yes" : "no",
           runtime_active_bodies(runtime));
    if (!ball->active) {
        return;
    }
    b3Vec3 velocity = b3Body_GetLinearVelocity(ball->body);
    printf("  player ball at (%.3f, %.3f, %.3f), speed %.3f m/s\n", (double)ball->position.x,
           (double)ball->position.y, (double)ball->position.z, (double)b3Length(velocity));
}
