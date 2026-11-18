// YOUR GAME: start here
//
// test_game.c — tests for the game, with no browser: the runtime's headless.c
// plays the page and the renderer by writing the records they would write
// into the bridge rings, the runtime steps, and the tests read back what was
// published and sent. It is also a worked example of how to test your own
// game.

#include <box3d/box3d.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include "game/game.h"
#include "runtime/bridge.h"
#include "runtime/camera.h"
#include "runtime/check.h"
#include "runtime/headless.h"
#include "runtime/math3d.h"
#include "runtime/runtime.h"

constexpr float DT = 1.0f / (float)APP_STEP_HZ;

typedef struct fixture {
    bridge_header *bridge;
    runtime_state runtime;
    headless_outbox outbox;
} fixture;

static fixture *fixture_new(void) {
    fixture *f = calloc(1, sizeof *f);
    f->bridge = bridge_create((float)APP_STEP_HZ);
    runtime_init(&f->runtime, f->bridge, 1);
    headless_set_canvas(f->bridge, 1600, 900);
    return f;
}

static void fixture_free(fixture *f) {
    runtime_destroy(&f->runtime);
    bridge_destroy(f->bridge);
    free(f);
}

static void run(fixture *f, float seconds) {
    int steps = (int)(seconds * (float)APP_STEP_HZ + 0.5f);
    for (int i = 0; i < steps; ++i) {
        runtime_step(&f->runtime, DT);
        headless_drain(f->bridge, &f->outbox);
    }
}

// The newest published snapshot.
static bridge_snapshot_header *latest(fixture *f) {
    uint32_t step = atomic_load(&f->bridge->latest_step);
    unsigned char *base = (unsigned char *)f->bridge + f->bridge->snapshot_offset;
    return (bridge_snapshot_header *)(void *)(base + (step & (f->bridge->snapshot_count - 1u)) *
                                                         f->bridge->snapshot_stride);
}

static void test_publishes_before_scene(void) {
    fixture *f = fixture_new();
    run(f, 0.1f);
    CHECK(!f->runtime.scene_ready);
    CHECK(atomic_load(&f->bridge->latest_step) == f->runtime.step);
    bridge_snapshot_header *snap = latest(f);
    CHECK(snap->pose_count == 0);
    CHECK(snap->camera.fov_y > 0.0f); // a usable camera from the very first step
    CHECK(snap->camera.far > snap->camera.near);
    fixture_free(f);
}

static vec3 velocity_of(const runtime_body *ball) {
    b3Vec3 v = b3Body_GetLinearVelocity(ball->body);
    return vec3_make(v.x, v.y, v.z);
}

static void test_scene_ready_builds_the_world(void) {
    fixture *f = fixture_new();
    game_headless_scene(f->bridge, false);
    run(f, DT);

    CHECK(f->runtime.scene_ready);
    CHECK(f->outbox.binds == 1);
    CHECK(f->runtime.static_count == 5); // the table top and four rails
    CHECK_NEAR(f->runtime.game.scene.ball_radius, 0.3f, 1e-6);
    CHECK_NEAR(f->runtime.game.scene.table_top, 0.0f, 1e-6);
    CHECK_NEAR(f->runtime.game.scene.max_x, 3.0f, 1e-5);
    CHECK_NEAR(f->runtime.game.scene.min_z, -2.0f, 1e-5);
    CHECK(f->runtime.game.ball_mass > 0.0f);

    bridge_snapshot_header *snap = latest(f);
    CHECK(snap->pose_count == 1);
    bridge_pose *pose = bridge_snapshot_poses(snap);
    CHECK((pose[0].flags & BRIDGE_POSE_ACTIVE) != 0);
    CHECK((pose[0].flags & BRIDGE_POSE_TELEPORT) != 0); // it just appeared
    CHECK(pose[0].position[1] > 2.0f);                  // above the table, about to drop in
    // The page was told which way the camera faces.
    CHECK(f->outbox.cues_by_id[GAME_CUE_CAMERA] == 1);
    fixture_free(f);
}

static void test_ball_settles_on_table(void) {
    fixture *f = fixture_new();
    game_headless_scene(f->bridge, false);
    run(f, 6.0f);

    const runtime_body *ball = runtime_get_body(&f->runtime, GAME_PLAYER_SLOT);
    CHECK_NEAR(ball->position.y, 0.3f, 0.02); // resting: table top + radius
    CHECK_NEAR(vec3_length(velocity_of(ball)), 0.0f, 0.05);
    CHECK(f->outbox.cues_by_id[GAME_CUE_FLOOR] > 0); // the landing was heard
    CHECK(f->outbox.cues_by_id[GAME_CUE_WALL] == 0);
    // The roll cue arrives about 20 times a second, and says "still" by now.
    CHECK(f->outbox.cues_by_id[GAME_CUE_ROLL] > 100);
    CHECK(f->outbox.last_cue_value[GAME_CUE_ROLL] == 0.0f);

    bridge_pose *pose = bridge_snapshot_poses(latest(f));
    CHECK((pose[0].flags & BRIDGE_POSE_TELEPORT) == 0);
    CHECK_NEAR(pose[0].position[1], 0.3f, 0.02);
    fixture_free(f);
}

static void test_keys_roll_the_ball_and_rails_hold(void) {
    fixture *f = fixture_new();
    game_headless_scene(f->bridge, false);
    run(f, 3.0f);

    // Default camera looks from +Z towards -Z, so D (right) is +X.
    headless_key(f->bridge, BRIDGE_KEY_D, true);
    run(f, 0.5f);
    CHECK(f->outbox.last_cue_value[GAME_CUE_ROLL] > 0.1f); // rolling, and audibly so
    run(f, 2.5f);
    headless_key(f->bridge, BRIDGE_KEY_D, false);

    const runtime_body *ball = runtime_get_body(&f->runtime, GAME_PLAYER_SLOT);
    CHECK(ball->position.x > 1.0f);
    CHECK(ball->position.x < 2.9f);                 // the east rail's inner face is at 2.9
    CHECK(ball->rotation.w < 0.9999f);              // it rolled, so it turned
    CHECK(f->outbox.cues_by_id[GAME_CUE_WALL] > 0); // it knocked against the rail
    fixture_free(f);
}

static void test_space_jumps(void) {
    fixture *f = fixture_new();
    game_headless_scene(f->bridge, false);
    run(f, 4.0f);

    headless_key(f->bridge, BRIDGE_KEY_SPACE, true);
    headless_key(f->bridge, BRIDGE_KEY_SPACE, false);
    run(f, 0.2f);
    CHECK(runtime_get_body(&f->runtime, GAME_PLAYER_SLOT)->position.y > 0.5f);
    fixture_free(f);
}

static void test_click_drops_a_ball_and_reset_clears(void) {
    fixture *f = fixture_new();
    game_headless_scene(f->bridge, false);
    run(f, 1.0f);

    headless_click(f->bridge, 0.25f, 0.0f); // lands at about (2.1, -0.2): right of centre
    run(f, DT);
    CHECK(f->outbox.spawns == 1);
    CHECK(f->outbox.last_spawn_slot == GAME_FIRST_DROP_SLOT);
    CHECK(f->outbox.last_spawn_node == bridge_hash("Ball"));
    CHECK(runtime_active_bodies(&f->runtime) == 2);

    const runtime_body *dropped = runtime_get_body(&f->runtime, GAME_FIRST_DROP_SLOT);
    CHECK(dropped->position.x > 0.0f); // right of centre, where the click was
    CHECK(dropped->position.x <= f->runtime.game.scene.max_x);

    // It falls and comes to rest on the felt like the player's ball.
    run(f, 5.0f);
    CHECK_NEAR(dropped->position.y, 0.3f, 0.02);

    // Dragging with the right button orbits the camera; it drops nothing.
    float yaw_before = f->runtime.camera.yaw;
    uint32_t camera_cues = f->outbox.cues_by_id[GAME_CUE_CAMERA];
    headless_right_drag(f->bridge, 40.0f, 0.0f);
    run(f, DT);
    CHECK(f->outbox.spawns == 1);
    CHECK(f->runtime.camera.yaw != yaw_before);
    CHECK(f->outbox.cues_by_id[GAME_CUE_CAMERA] == camera_cues + 1);
    CHECK(f->outbox.last_cue_value[GAME_CUE_CAMERA] == f->runtime.camera.yaw);

    // Enter: the dropped ball goes, the player's ball returns to the start.
    headless_key(f->bridge, BRIDGE_KEY_ENTER, true);
    headless_key(f->bridge, BRIDGE_KEY_ENTER, false);
    run(f, DT);
    CHECK(f->outbox.despawns == 1);
    CHECK(runtime_active_bodies(&f->runtime) == 1);
    bridge_snapshot_header *snap = latest(f);
    CHECK(snap->pose_count == 1);
    CHECK((bridge_snapshot_poses(snap)[0].flags & BRIDGE_POSE_TELEPORT) != 0);
    fixture_free(f);
}

static void test_chorded_buttons_do_not_leave_the_camera_orbiting(void) {
    // Right down, left down, right up, left up: the browser reports one
    // press (right) and one release (left). The orbit must end with it.
    fixture *f = fixture_new();
    game_headless_scene(f->bridge, false);
    run(f, 0.5f);
    bridge_ring *ring = bridge_get_ring(f->bridge, BRIDGE_RING_DOM_TO_APP);
    bridge_record down = {.type = BRIDGE_EV_POINTER_DOWN};
    down.u[0] = 2;
    bridge_record up = {.type = BRIDGE_EV_POINTER_UP};
    up.u[0] = 0;
    up.f[2] = 0.99f; // released over the sky, so nothing drops
    CHECK(bridge_ring_push(ring, &down));
    CHECK(bridge_ring_push(ring, &up));
    run(f, DT);
    CHECK(!f->runtime.game.orbiting);
    fixture_free(f);
}

static void test_a_pile_on_several_threads(void) {
    // The browser steps Box3D on up to five threads; do the same here, with
    // enough balls in contact to give every thread work.
    fixture *f = calloc(1, sizeof *f);
    f->bridge = bridge_create((float)APP_STEP_HZ);
    runtime_init(&f->runtime, f->bridge, 4);
    headless_set_canvas(f->bridge, 1600, 900);
    game_headless_scene(f->bridge, false);
    run(f, 0.5f);
    for (int i = 0; i < 60; ++i) {
        headless_click(f->bridge, -0.2f + 0.05f * (float)(i % 9), -0.3f + 0.04f * (float)(i % 7));
        run(f, 0.05f);
    }
    run(f, 2.0f);
    CHECK(runtime_active_bodies(&f->runtime) == 61);
    for (int i = 0; i < GAME_SLOT_COUNT; ++i) {
        const runtime_body *ball = runtime_get_body(&f->runtime, (uint32_t)i);
        if (ball->active) {
            CHECK(ball->position.y > -2.0f && ball->position.y < 20.0f); // nothing blew up
        }
    }
    fixture_free(f);
}

static void test_two_balls_clack(void) {
    fixture *f = fixture_new();
    game_headless_scene(f->bridge, false);
    run(f, 4.0f); // the player's ball is resting at the centre

    // Click where the table's centre is on screen (the canvas is 1600 x 900).
    vec3 ndc = mat4_transform_point(camera_view_projection(&f->runtime.camera, 1600.0f / 900.0f),
                                    vec3_make(0.0f, 0.0f, 0.0f));
    headless_click(f->bridge, ndc.x, ndc.y);
    run(f, DT);
    CHECK(runtime_active_bodies(&f->runtime) == 2);
    CHECK_NEAR(runtime_get_body(&f->runtime, GAME_FIRST_DROP_SLOT)->position.x, 0.0f, 0.2);
    CHECK_NEAR(runtime_get_body(&f->runtime, GAME_FIRST_DROP_SLOT)->position.z, 0.0f, 0.2);
    run(f, 2.0f); // it lands on the player's ball
    CHECK(f->outbox.cues_by_id[GAME_CUE_BALL] > 0);
    fixture_free(f);
}

static void test_touch_swipe_flings_and_tap_drops(void) {
    fixture *f = fixture_new();
    game_headless_scene(f->bridge, false);
    run(f, 4.0f);
    const runtime_body *ball = runtime_get_body(&f->runtime, GAME_PLAYER_SLOT);

    // A quick swipe to the right: 200 px in 100 ms = 2000 px/s = 8 m/s.
    headless_swipe(f->bridge, 200.0f, 0.0f, 100);
    run(f, DT);
    CHECK(f->outbox.spawns == 0); // a swipe is not a tap
    vec3 v = velocity_of(ball);
    CHECK(v.x > 6.0f && v.x < 9.01f); // thrown to the right (+X at the default view)
    CHECK_NEAR(v.z, 0.0f, 0.2);

    // A touch that barely moves is a tap: it drops a ball.
    headless_swipe(f->bridge, 3.0f, 2.0f, 80);
    run(f, DT);
    CHECK(f->outbox.spawns == 1);
    fixture_free(f);
}

static void test_ball_that_falls_off_is_put_back(void) {
    fixture *f = fixture_new();
    game_headless_scene(f->bridge, false);
    run(f, 1.0f);
    const runtime_body *ball = runtime_get_body(&f->runtime, GAME_PLAYER_SLOT);
    b3Body_SetTransform(ball->body, (b3Vec3){10.0f, -1.9f, 0.0f}, b3Quat_identity); // off the table
    run(f, 0.5f);
    CHECK(ball->position.y > 0.0f);
    CHECK_NEAR(ball->position.x, 0.0f, 0.01);
    fixture_free(f);
}

static void test_scene_can_be_described_again(void) {
    // An app that outlives a renderer attach gets the scene a second time and
    // must rebuild its world from it without leaking the first.
    fixture *f = fixture_new();
    game_headless_scene(f->bridge, false);
    run(f, 1.0f);
    headless_click(f->bridge, 0.25f, 0.0f);
    run(f, 1.0f);
    game_headless_scene(f->bridge, false);
    run(f, 1.0f);
    CHECK(f->runtime.scene_ready);
    CHECK(f->runtime.static_count == 5);
    CHECK(runtime_active_bodies(&f->runtime) == 1);
    CHECK(f->outbox.binds == 2);
    fixture_free(f);
}

static void test_r_reloads_the_scene(void) {
    fixture *f = fixture_new();
    game_headless_scene(f->bridge, false);
    run(f, 1.0f);
    headless_click(f->bridge, 0.25f, 0.0f);
    headless_right_drag(f->bridge, 40.0f, 0.0f);
    run(f, 1.0f);
    CHECK(runtime_active_bodies(&f->runtime) == 2);
    float yaw_default = camera_default(vec3_make(0.0f, 0.0f, 0.0f)).yaw;
    CHECK(f->runtime.camera.yaw != yaw_default);

    // R: one RELOAD_SCENE goes out and the world is emptied at once.
    headless_key(f->bridge, BRIDGE_KEY_R, true);
    headless_key(f->bridge, BRIDGE_KEY_R, false);
    run(f, 0.1f);
    CHECK(f->outbox.reloads == 1);
    CHECK(!f->runtime.scene_ready);
    CHECK(runtime_active_bodies(&f->runtime) == 0);
    CHECK(f->runtime.static_count == 0);
    CHECK(latest(f)->pose_count == 0); // nothing is posed meanwhile

    // A second R before the scene is back does not ask twice, and clicks
    // drop nothing into a world that is not there.
    headless_key(f->bridge, BRIDGE_KEY_R, true);
    headless_key(f->bridge, BRIDGE_KEY_R, false);
    headless_click(f->bridge, 0.25f, 0.0f);
    run(f, 0.1f);
    CHECK(f->outbox.reloads == 1);
    CHECK(f->outbox.spawns == 1);

    // The renderer describes the fresh scene: everything starts over, as
    // after the first load (the dropped ball is gone, the camera is back).
    game_headless_scene(f->bridge, false);
    run(f, 1.0f);
    CHECK(f->runtime.scene_ready);
    CHECK(f->runtime.static_count == 5);
    CHECK(runtime_active_bodies(&f->runtime) == 1);
    CHECK(f->outbox.binds == 2);
    CHECK(f->runtime.camera.yaw == yaw_default);
    fixture_free(f);
}

static void test_hot_swap(void) {
    // A new app instance attaches to a scene a previous instance has driven.
    fixture *f = fixture_new();
    game_headless_scene(f->bridge, true);
    run(f, 0.1f);
#if GAME_RELOAD_SCENE_ON_HOT_SWAP
    // It does not take the scene over; it asks for a clean one, once.
    CHECK(!f->runtime.scene_ready);
    CHECK(f->outbox.reloads == 1);
    CHECK(f->outbox.binds == 0);
    game_headless_scene(f->bridge, false);
    run(f, 0.1f);
    CHECK(f->outbox.reloads == 1);
#else
    CHECK(f->outbox.reloads == 0);
#endif
    CHECK(f->runtime.scene_ready);
    CHECK(f->runtime.static_count == 5);
    CHECK(f->outbox.binds == 1);
    fixture_free(f);
}

static void test_click_on_sky_does_nothing(void) {
    fixture *f = fixture_new();
    game_headless_scene(f->bridge, false);
    run(f, 0.5f);
    headless_click(f->bridge, 0.0f, 0.99f); // top of the screen: above the horizon
    run(f, DT);
    CHECK(f->outbox.spawns == 0);
    fixture_free(f);
}

// ── Input batching (one page frame per burst) ─────────────────────────────

static void test_input_is_applied_one_page_frame_at_a_time(void) {
    fixture *f = fixture_new();
    game_headless_scene(f->bridge, false);
    run(f, 3.0f); // the ball comes to rest
    const double period_ms = 1000.0 / 60.0;
    const uint32_t frame_us = 16'667;
    const uint32_t t0 = 0xFFFFFFFFu - 20'000u; // the frame clock wraps between frames

    // Page frame A: W goes down. Page frame B, one frame later: a Space tap.
    headless_key(f->bridge, BRIDGE_KEY_W, true);
    headless_frame_mark(f->bridge, t0);
    headless_key(f->bridge, BRIDGE_KEY_SPACE, true);
    headless_key(f->bridge, BRIDGE_KEY_SPACE, false);
    headless_frame_mark(f->bridge, t0 + frame_us);

    // The burst for the frame one after A: A's mark is old enough, B's is not
    // (it is this frame's own).
    runtime_frame_input(&f->runtime, t0 + frame_us, period_ms);
    CHECK(f->runtime.input.count == 5);
    CHECK(f->runtime.input.released == 2); // W down, mark A
    runtime_step(&f->runtime, DT);
    CHECK(runtime_key_held(&f->runtime, BRIDGE_KEY_W));
    CHECK(fabsf(velocity_of(runtime_get_body(&f->runtime, GAME_PLAYER_SLOT)).y) <
          0.5f); // no jump yet
    // The other steps of the burst apply nothing more.
    runtime_step(&f->runtime, DT);
    CHECK(f->runtime.input.count == 3);

    // Two frames with no steps (a display faster than the step rate): B is
    // released, and waits for the next step that runs.
    runtime_frame_input(&f->runtime, t0 + 2u * frame_us, period_ms);
    CHECK(f->runtime.input.released == 3);
    runtime_frame_input(&f->runtime, t0 + 3u * frame_us, period_ms);
    CHECK(f->runtime.input.released == 3);
    runtime_step(&f->runtime, DT);
    CHECK(velocity_of(runtime_get_body(&f->runtime, GAME_PLAYER_SLOT)).y > 1.0f); // jumped
    CHECK(f->runtime.input.count == 0);
    CHECK(runtime_key_held(&f->runtime, BRIDGE_KEY_W)); // still held: no W-up was sent
    fixture_free(f);
}

static void test_input_without_marks_is_applied_at_once(void) {
    fixture *f = fixture_new();
    game_headless_scene(f->bridge, false);
    run(f, 3.0f);
    const double period_ms = 1000.0 / 60.0;

    // A page that sends no marks (an older one): everything goes at once.
    headless_key(f->bridge, BRIDGE_KEY_SPACE, true);
    runtime_frame_input(&f->runtime, 1'000'000u, period_ms);
    CHECK(f->runtime.input.released == 1);
    runtime_step(&f->runtime, DT);
    CHECK(velocity_of(runtime_get_body(&f->runtime, GAME_PLAYER_SLOT)).y > 1.0f);

    // A mark that never comes (lost, or a page stalled half way through a
    // frame): what did arrive waits for it, but not longer than
    // RUNTIME_MARK_TIMEOUT_US.
    headless_frame_mark(f->bridge, 1'000'000u);
    runtime_frame_input(&f->runtime, 1'016'667u, period_ms);
    CHECK(f->runtime.input.released == 1); // the mark itself
    headless_key(f->bridge, BRIDGE_KEY_SPACE, false);
    runtime_frame_input(&f->runtime, 1'033'334u, period_ms);
    CHECK(f->runtime.input.count == 2);
    CHECK(f->runtime.input.released == 1); // the key-up waits for its frame's mark
    runtime_frame_input(&f->runtime, 1'033'334u + RUNTIME_MARK_TIMEOUT_US, period_ms);
    CHECK(f->runtime.input.released == 1); // not yet
    runtime_frame_input(&f->runtime, 1'033'334u + RUNTIME_MARK_TIMEOUT_US + 1u, period_ms);
    CHECK(f->runtime.input.released == 2);
    runtime_step(&f->runtime, DT);
    CHECK(f->runtime.input.count == 0);
    fixture_free(f);
}

static void test_a_tap_shorter_than_a_frame_still_counts(void) {
    // Down and up in the same frame's input: held for that one step, then not.
    fixture *f = fixture_new();
    game_headless_scene(f->bridge, false);
    run(f, 3.0f);
    CHECK_NEAR(velocity_of(runtime_get_body(&f->runtime, GAME_PLAYER_SLOT)).x, 0.0f, 0.01);
    headless_key(f->bridge, BRIDGE_KEY_D, true);
    headless_key(f->bridge, BRIDGE_KEY_D, false);
    runtime_step(&f->runtime, DT);
    float pushed = velocity_of(runtime_get_body(&f->runtime, GAME_PLAYER_SLOT)).x;
    CHECK(pushed > 0.03f); // D pushes to the right with the camera at yaw 0
    CHECK(!runtime_key_held(&f->runtime, BRIDGE_KEY_D));
    runtime_step(&f->runtime, DT);
    CHECK(velocity_of(runtime_get_body(&f->runtime, GAME_PLAYER_SLOT)).x <=
          pushed + 1e-4f); // one push only
    fixture_free(f);
}

int main(void) {
    test_publishes_before_scene();
    test_scene_ready_builds_the_world();
    test_ball_settles_on_table();
    test_keys_roll_the_ball_and_rails_hold();
    test_space_jumps();
    test_click_drops_a_ball_and_reset_clears();
    test_chorded_buttons_do_not_leave_the_camera_orbiting();
    test_input_is_applied_one_page_frame_at_a_time();
    test_input_without_marks_is_applied_at_once();
    test_a_tap_shorter_than_a_frame_still_counts();
    test_a_pile_on_several_threads();
    test_two_balls_clack();
    test_touch_swipe_flings_and_tap_drops();
    test_ball_that_falls_off_is_put_back();
    test_scene_can_be_described_again();
    test_r_reloads_the_scene();
    test_hot_swap();
    test_click_on_sky_does_nothing();
    return check_report();
}
