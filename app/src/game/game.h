// YOUR GAME: start here
//
// game.h — what the runtime needs to know about your game at compile time: its
// pose slots, its cues, its state, its step rate and its physics world.
// game.c is the logic.
//
// The demo: balls on a table, simulated by Box3D. The scene's colliders
// become the physics world, the authored ball is the player's (W/A/S/D or the
// arrows roll it, Space jumps, a swipe throws it), clicks drop more balls,
// Enter clears them and R loads the whole scene again. Box3D reports what
// touched what, and that becomes the sound cues the page plays.
//
// ── The contract with the runtime (app/src/runtime/runtime.h) ──────────────
//
// game.c defines these, and the runtime calls them; nothing else does:
//
//   game_init             once, when the app starts: set up the camera
//   game_scene_begin      a scene description begins: reset your state
//   game_scene_ready      the scene's colliders are in: build your world
//   game_key              a key went down or up (only while the scene is ready)
//   game_pointer          a pointer or wheel event (whether or not it is)
//   game_update           once per fixed step, before the world steps
//   game_contact          two shapes started or stopped touching
//   game_hit              two shapes hit each other
//   game_after_physics    the end of the step: poses are up to date
//
// and calls only these runtime_* functions back (never the bridge itself),
// plus Box3D's b3* API on its own bodies:
//
//   runtime_collider_count, runtime_get_collider   the scene's colliders
//   runtime_scene_ready       is the scene described
//   runtime_reload_scene      load the scene again from the bundle
//   runtime_current_step      the step being run, counting from 1
//   runtime_key_held          is a key down this step
//   runtime_pointer_ray       the ray under a point on the canvas
//   runtime_get_camera        the camera the renderer draws from
//   runtime_world             the Box3D world, to create bodies in
//   runtime_create_collider_body   a static body for a scene collider
//   runtime_bind_body, runtime_spawn_body   draw a body as a scene node, or a copy
//   runtime_destroy_body      destroy a slot's body and take it off screen
//   runtime_get_body          a slot's body and pose
//   runtime_teleport_body     move a slot's body at once, at rest
//   runtime_active_bodies     how many slots have a body
//   runtime_send_cue          tell the page something happened (GAME_CUE_*)
//   runtime_send_cue_throttled   the same, at most so often per cue
//
// The runtime copies every slot's pose out of Box3D after each world step and
// publishes it, with the camera, after game_after_physics returns.

#ifndef GAME_H
#define GAME_H

#include <box3d/box3d.h>
#include <stdint.h>

#include "runtime/math3d.h"

// Fixed simulation rate, steps per second. The renderer reads it from the
// bridge header. Raising it means more (smaller) steps per frame, not a faster
// game: every rate in game.c is per second.
enum { APP_STEP_HZ = 240 };

// What happens to the scene when you save a C file and `task dev` swaps a new
// build of the app into the running page:
//
//   1  the new app asks the renderer to load the scene again
//      (runtime_reload_scene), so every save starts from the scene bundle,
//      whatever the previous build did to the world. Costs a scene load
//      (textures included) per save.
//   0  the new app takes over the scene as the renderer has it. Instant, and
//      fine for this demo: the renderer undoes every bind and spawn of the
//      previous app when a new one attaches. Anything a future command might
//      change beyond that would still be there.
#define GAME_RELOAD_SCENE_ON_HOT_SWAP 1

// ── The physics world (the runtime creates it with these) ──────────────────

static constexpr float GAME_GRAVITY = 9.81f; // m/s², downwards
// Contacts that approach faster than this (m/s) are hits (game_hit).
// Slower ones make no sound.
static constexpr float GAME_HIT_MIN_SPEED = 0.8f;
// Box3D runs its solver this many times inside one world step. Its advice is 4
// at 60 steps a second; we already step at 240, so 1 gives the same solver
// rate. Raise it only if contacts look soft (the cost is linear).
static constexpr int GAME_SOLVER_SUB_STEPS = 1;

// ── Pose slots ─────────────────────────────────────────────────────────────
//
// One per ball. Slot 0 is always the player's ball; the slots from
// GAME_FIRST_DROP_SLOT on are balls dropped by clicking (Enter clears those).
enum {
    GAME_PLAYER_SLOT = 0,
    GAME_FIRST_DROP_SLOT = 1,
    GAME_MAX_DROPPED_BALLS = 200,
    GAME_SLOT_COUNT = GAME_FIRST_DROP_SLOT + GAME_MAX_DROPPED_BALLS,
};

// ── Cues for the page ──────────────────────────────────────────────────────
//
// Ids sent with runtime_send_cue(). dom/game.js has the matching table and
// decides what each one sounds like.
//
// FLOOR, WALL and BALL are impacts: value = intensity 0..1, where = the hit.
// ROLL is continuous, about 20 times a second: value = how fast the player's
//   ball is rolling on the table, 0..1 (0 = still or airborne), where = the
//   ball.
// CAMERA: value = the camera's yaw in radians, sent whenever it changes, so
//   the page can keep left/right in the sound matching the view.
enum {
    GAME_CUE_FLOOR = 1,  // a ball landed on the table
    GAME_CUE_WALL = 2,   // a ball hit a rail
    GAME_CUE_BALL = 3,   // two balls hit each other
    GAME_CUE_ROLL = 4,   // the player's ball is rolling (continuous)
    GAME_CUE_CAMERA = 5, // the camera turned
};

// ── Tuning. Every rate is per second, so none of these depend on APP_STEP_HZ.

static constexpr float DROP_HEIGHT = 2.0f; // balls appear this far above where they belong
static constexpr float FALL_LIMIT = -2.0f; // a ball below this height fell off: put it back
// A click near the edge still lands on the felt: this far (plus the ball's
// radius) inside the table's outline, clear of the rails.
static constexpr float DROP_MARGIN = 0.25f;

// How bouncy and grippy things are, and how quickly a ball loses speed.
static constexpr float TABLE_RESTITUTION = 0.5f;
static constexpr float TABLE_FRICTION = 0.9f;
static constexpr float BALL_RESTITUTION = 0.6f;
static constexpr float BALL_FRICTION = 0.9f;
static constexpr float BALL_LINEAR_DAMPING = 0.1f;
static constexpr float BALL_ANGULAR_DAMPING = 0.4f;

// The player's ball. Pushes are scaled by its mass, so it handles the same
// whatever its size.
static constexpr float MOVE_ACCEL = 27.0f; // m/s² while a direction key is held
static constexpr float MAX_SPEED = 6.0f;   // m/s, cap on horizontal speed from the keys
static constexpr float JUMP_SPEED = 3.2f;  // m/s upwards when Space is pressed

// Touch: a swipe throws the player's ball.
static constexpr float TAP_SLOP_PX = 12.0f;          // a touch that travels less is a tap
static constexpr uint32_t FLING_WINDOW_US = 120'000; // speed is measured over the end of the swipe
static constexpr float FLING_GAIN = 0.004f;          // swipe speed in px/s → ball speed in m/s
static constexpr float FLING_MIN_SPEED = 0.25f;      // slower swipes are ignored
static constexpr float FLING_MAX_SPEED = 9.0f;       // faster ones are capped

// Sound cues.
static constexpr float LAND_FULL_SPEED = 6.5f; // m/s; a landing this hard is at full volume
static constexpr float MIN_IMPACT_INTENSITY = 0.12f;
static constexpr float IMPACT_COOLDOWN_S = 0.13f; // per kind of impact
static constexpr float ROLL_SILENT_BELOW = 0.04f; // a roll slower than this (0..1) counts as still
static constexpr uint32_t ROLL_CUES_PER_SECOND = 20;

static constexpr float CAMERA_TARGET_LIFT = 0.4f; // the camera aims this far above the felt

// Fallbacks for a scene without a recognisable table or ball. They match the
// bundled scene, so the demo still behaves if a collider is renamed.
static constexpr float DEFAULT_HALF_WIDTH = 3.0f;
static constexpr float DEFAULT_HALF_DEPTH = 2.0f;
static constexpr float DEFAULT_BALL_RADIUS = 0.3f;

// ── State ──────────────────────────────────────────────────────────────────

enum {
    // How many other shapes the player's ball can be touching at once and
    // still be tracked (see game_state.touching).
    GAME_MAX_TOUCHING = 16,
    // Recent positions of a finger on the screen, for measuring a swipe.
    GAME_MAX_SWIPE_SAMPLES = 64,
};

// What the scene says about the table and the ball, worked out in
// game_scene_ready().
typedef struct game_scene {
    // The table top: its height and its outline, in world space. Clicks drop
    // balls inside it.
    float table_top;
    float min_x, max_x, min_z, max_z;

    float ball_radius;
    vec3 ball_start; // where the authored ball sits
} game_scene;

// One thing the player's ball is touching.
typedef struct game_touch {
    int32_t shape_index; // Box3D's index for the other shape
    uint8_t role;        // what that shape is (ROLE_* in game.c)
} game_touch;

// A finger on the screen that may turn out to be a swipe. Positions are CSS
// pixels relative to where it touched down.
typedef struct game_swipe {
    bool active;
    float x, y;
    int sample_count;
    struct {
        uint32_t time_us;
        float x, y;
    } samples[GAME_MAX_SWIPE_SAMPLES];
} game_swipe;

// Everything your game remembers between steps, apart from what the runtime
// keeps (bodies and their poses: runtime_get_body; the camera:
// runtime_get_camera). The runtime keeps one of these and hands it to every
// hook.
typedef struct game_state {
    game_scene scene;
    vec3 spawn[GAME_SLOT_COUNT]; // where each ball is put back if it falls off
    b3ShapeId player_shape;
    float ball_mass;
    // Shapes the player's ball is touching right now, kept up to date from
    // game_contact(). Answers "is it on the table?".
    game_touch touching[GAME_MAX_TOUCHING];
    int touching_count;

    float camera_yaw_sent; // the yaw last sent as a GAME_CUE_CAMERA

    // Pointer gestures in progress.
    bool orbiting; // the right mouse button is held: moves orbit the camera
    game_swipe swipe;
} game_state;

#endif // GAME_H
