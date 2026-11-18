// app.h — the application: all state and logic, independent of the platform.
//
// Nothing here knows about the browser. The app talks to the outside world
// only through the bridge block, so the same code runs in the wasm worker and
// in native tests. web_main.c (browser) and main.c (native) just call
// app_init() once and then app_step() at a fixed rate.
//
// The demo is balls on a table, simulated by Box3D: the scene's colliders
// become the physics world, the authored ball is the player's, and clicks
// drop more.

#ifndef APP_H
#define APP_H

#include <box3d/box3d.h>

#include "bridge.h"
#include "camera.h"
#include "math3d.h"

// Fixed simulation rate. Written into bridge_header.step_hz, which is where the
// renderer reads it from. Raising it means more (smaller) steps per frame, not
// a faster game: every rate in app.c is per second.
enum { APP_STEP_HZ = 240 };

// What happens to the scene when you save a C file and `task dev` swaps a new
// build of the app into the running page:
//
//   1  the new app asks the renderer to load the scene again
//      (app_reload_scene), so every save starts from the scene bundle,
//      whatever the previous build did to the world. Costs a scene load
//      (textures included) per save.
//   0  the new app takes over the scene as the renderer has it. Instant, and
//      fine for this demo: the renderer undoes every bind and spawn of the
//      previous app when a new one attaches. Anything a future command might
//      change beyond that would still be there.
#define APP_RELOAD_SCENE_ON_HOT_SWAP 1

// Ids carried in BRIDGE_CUE records (u[0]). The page turns them into sound;
// dom/main.js has a matching table.
//
// FLOOR, WALL and BALL are impacts:  f[1] = intensity 0..1, f[2..4] = where.
// ROLL is continuous, about 20 times a second: f[1] = how fast the player's
//   ball is rolling on the table, 0..1 (0 = still or airborne), f[2..4] = where
//   the ball is.
// CAMERA: f[1] = the camera's yaw in radians, sent whenever it changes, so
//   the page can keep left/right in the sound matching the view.
enum {
    APP_CUE_FLOOR = 1,  // a ball landed on the table
    APP_CUE_WALL = 2,   // a ball hit a rail
    APP_CUE_BALL = 3,   // two balls hit each other
    APP_CUE_ROLL = 4,   // the player's ball is rolling (continuous)
    APP_CUE_CAMERA = 5, // the camera turned
};

// Pose slots: one per ball. Slot 0 is always the player's ball; the rest are
// balls dropped by clicking.
enum {
    APP_PLAYER_SLOT = 0,
    APP_MAX_DROPPED_BALLS = 200,
    APP_MAX_BALLS = APP_MAX_DROPPED_BALLS + 1,
};
static_assert((unsigned)APP_MAX_BALLS <= BRIDGE_POSE_CAPACITY, "more balls than pose slots");

typedef struct app_ball {
    bool active;
    // Moved discontinuously since the last snapshot (spawned or put back), so
    // the renderer must not interpolate into the new position.
    bool teleported;
    b3BodyId body;
    vec3 spawn; // where it is put back if it falls off the table
    // Copied out of Box3D after every step, for publishing.
    vec3 position; // centre
    quat rotation;
} app_ball;

// What the renderer told us about the scene (see BRIDGE_SCENE_*).
typedef struct app_scene {
    bool ready; // BRIDGE_SCENE_READY seen; nothing is simulated before it

    // The table top: its height and its outline, in world space. Clicks drop
    // balls inside it.
    float table_top;
    float min_x, max_x, min_z, max_z;

    float ball_radius;
    vec3 ball_start; // where the authored ball sits
} app_scene;

enum {
    APP_MAX_SCENE_COLLIDERS = 64,
    // How many other shapes the player's ball can be touching at once and
    // still be tracked (see app_state.touching).
    APP_MAX_TOUCHING = 16,
    // Recent positions of a finger on the screen, for measuring a swipe.
    APP_MAX_SWIPE_SAMPLES = 64,
    // Input records taken off the dom → app ring but not applied yet (see
    // app_input). Twice the ring, so emptying a full ring always fits.
    APP_INPUT_QUEUE = 2 * BRIDGE_RING_CAPACITY,
};

// Input that has waited this long (microseconds, frame clock) for its
// FRAME_MARK is applied without it: a lost mark, or a page stalled half way
// through a frame, should not hold back what was sent.
constexpr uint32_t APP_MARK_TIMEOUT_US = 100'000;

// The dom → app input, batched one page frame at a time (see
// BRIDGE_EV_FRAME_MARK in bridge.h). Records are taken off the ring into
// `queue` as they arrive; app_frame_input() releases the ones the coming
// burst may apply, and the first step of the burst applies them.
typedef struct app_input {
    // app_frame_input() has been called: the browser loop. Without it (native
    // runs and most tests) each step applies whatever is in the ring, as it
    // always did.
    bool framed;
    bool seen_mark; // a FRAME_MARK has ever arrived
    int count;      // records in queue
    int released;   // queue[0, released) may be applied
    // Each record's `step` field is overwritten here with the frame time at
    // which it was taken off the ring (the page leaves it 0), for the timeout.
    bridge_record queue[APP_INPUT_QUEUE];
} app_input;

// One thing the player's ball is touching.
typedef struct app_touch {
    int32_t shape_index; // Box3D's index for the other shape
    uint8_t role;        // what that shape is (ROLE_* in app.c)
} app_touch;

// A finger on the screen that may turn out to be a swipe. Positions are CSS
// pixels relative to where it touched down.
typedef struct app_swipe {
    bool active;
    float x, y;
    int sample_count;
    struct {
        uint32_t time_us;
        float x, y;
    } samples[APP_MAX_SWIPE_SAMPLES];
} app_swipe;

typedef struct app_state {
    bridge_header *bridge;
    uint32_t step; // the last step published; the first is 1
    // The moment the current step stands for, in milliseconds on the clock
    // fixed_step_advance() was given. Published with the step, as frame-clock
    // microseconds (bridge_snapshot_header.time_us).
    double step_time_ms;

    app_scene scene;
    orbit_camera camera;
    float camera_yaw_sent; // the yaw last sent as an APP_CUE_CAMERA

    // ── Physics ──
    b3WorldId world;
    b3BodyId statics[APP_MAX_SCENE_COLLIDERS]; // the table and its rails
    int static_count;
    app_ball balls[APP_MAX_BALLS];
    b3ShapeId player_shape;
    float ball_mass;
    // Shapes the player's ball is touching right now, kept up to date from
    // Box3D's begin/end-touch events. Answers "is it on the table?".
    app_touch touching[APP_MAX_TOUCHING];
    int touching_count;
    // The step each kind of impact last made a sound, so a ball grinding
    // against a rail does not rattle like a machine gun.
    uint32_t last_floor_cue_step;
    uint32_t last_wall_cue_step;
    uint32_t last_ball_cue_step;

    // ── Input ──
    app_input input;
    // Keys held, by BRIDGE_KEY_* bit, as the EV_KEY records applied so far
    // say. `keys_tapped` also has every key that went down in this step's
    // input, even if it came back up in the same batch: a tap shorter than a
    // frame still moves the ball for a step.
    uint32_t keys_down[2];
    uint32_t keys_tapped[2];

    // ── Pointer gestures in progress ──
    bool orbiting; // the right mouse button is held: moves orbit the camera
    app_swipe swipe;

    // Scene records collected between SCENE_BEGIN and SCENE_READY.
    bridge_record colliders[APP_MAX_SCENE_COLLIDERS];
    int collider_count;
    // app_reload_scene() was called: what the renderer still says about the
    // old scene is ignored until the description of the fresh one begins.
    bool reload_pending;
} app_state;

// Sets up `app` to talk through `bridge` (which must outlive it) and creates
// its physics world. `solver_threads` is how many threads Box3D steps the
// world on, counting the caller: 1 keeps everything on the calling thread.
void app_init(app_state *app, bridge_header *bridge, uint32_t solver_threads);

// Destroys the physics world (and with it, any solver threads).
void app_destroy(app_state *app);

// Advances the world by one fixed step of `dt` seconds and publishes the
// result as a snapshot. The first step after app_frame_input() applies the
// input it released.
void app_step(app_state *app, float dt);

// Called once per frame, before that frame's steps, with the frame's time on
// the frame clock and the display's frame period. Takes new input off the
// ring and releases what this frame's burst should apply: everything up to
// the newest FRAME_MARK at or before `frame_time_us - period / 2`. Records
// after it wait for a later frame; released records a burst of zero steps
// did not get to wait for the next step that runs.
void app_frame_input(app_state *app, uint32_t frame_time_us, double period_ms);

// Whether key `key` (BRIDGE_KEY_*) counts as held in the current step.
bool app_key_held(const app_state *app, uint32_t key);

// Asks the renderer to destroy everything in the scene and load it again from
// the bundle, without touching the GPU device or reloading the page
// (BRIDGE_CMD_RELOAD_SCENE). The world is emptied at once and nothing is
// simulated until the renderer has described the fresh scene; the app then
// starts over exactly as after the first load. Call it from anywhere in the
// app: a "restart" key (R in the demo), a game-over screen, the next level.
void app_reload_scene(app_state *app);

// Number of balls currently in play (including the player's).
int app_active_balls(const app_state *app);

#endif // APP_H
