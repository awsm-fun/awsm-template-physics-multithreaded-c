// RUNTIME: shared by every game; your game goes in app/src/game/ and dom/game.js
//
// runtime.h — runs your game, one fixed step at a time. It owns the Box3D
// world, the camera and the pose slots, and is the only code that talks to the
// other two threads for your game.
//
// Nothing here knows about the browser, or about what your game is. The
// runtime talks to the outside world only through the bridge block, so the
// same code runs in the page's worker (web_main.c), natively with no browser
// at all (main.c), and in the tests (app/tests/).
//
// Every fixed step (runtime_step) does four things, in order:
//
//   1. read what the renderer says about the scene    (renderer → app ring)
//        → game_scene_begin, game_scene_ready
//   2. read what the user did                          (dom → app ring)
//        → game_key, game_pointer
//   3. let the game move the world, step Box3D, copy the poses out
//        → game_update, then the world step, then game_contact, game_hit,
//          game_after_physics
//   4. publish the result                              (a snapshot: camera + poses)
//
// ── The contract with your game (app/src/game/) ────────────────────────────
//
// game.h provides game_state, APP_STEP_HZ, GAME_SLOT_COUNT, the world settings
// GAME_GRAVITY, GAME_HIT_MIN_SPEED and GAME_SOLVER_SUB_STEPS, and
// GAME_RELOAD_SCENE_ON_HOT_SWAP; game.c provides the game_* hooks declared
// below. The game calls back only the runtime_* functions in "The API your
// game calls", plus Box3D's own b3* API on the bodies it creates.
//
// Bodies and pose slots: the game creates Box3D bodies in runtime_world()
// and hands each one that should be drawn to the runtime with
// runtime_bind_body() or runtime_spawn_body(), naming the scene node that
// shows it. After every world step the runtime copies each such body's pose
// into its slot, and publishes every slot. So the game never sees a ring or a
// snapshot, and the runtime never needs to know what a body means.

#ifndef RUNTIME_H
#define RUNTIME_H

#include <box3d/box3d.h>
#include <stdint.h>

#include "game/game.h"
#include "runtime/bridge.h"
#include "runtime/camera.h"
#include "runtime/math3d.h"

static_assert((unsigned)GAME_SLOT_COUNT <= BRIDGE_POSE_CAPACITY, "more slots than pose slots");

enum {
    // Collider nodes a scene may have; more are logged and ignored.
    RUNTIME_MAX_COLLIDERS = 64,
    // Cue ids runtime_send_cue_throttled() keeps a cooldown for (0 .. this-1).
    RUNTIME_CUE_IDS = 32,
    // Input records taken off the dom → app ring but not applied yet (see
    // runtime_input). Twice the ring, so emptying a full ring always fits.
    RUNTIME_INPUT_QUEUE = 2 * BRIDGE_RING_CAPACITY,
};

// Input that has waited this long (microseconds, frame clock) for its
// FRAME_MARK is applied without it: a lost mark, or a page stalled half way
// through a frame, should not hold back what was sent.
constexpr uint32_t RUNTIME_MARK_TIMEOUT_US = 100'000;

// ── What the game is handed ────────────────────────────────────────────────

// One collider node of the scene, as the renderer reported it, in world space
// (node scale already folded into `dims`).
typedef struct runtime_collider {
    uint32_t name;      // bridge_hash() of the collider node's own name
    uint32_t top_level; // bridge_hash() of its top-level ancestor's name
    uint32_t shape;     // BRIDGE_SHAPE_*
    float dims[3];      // the shape's dimensions (see BRIDGE_SHAPE_* in bridge.h)
    vec3 position;
    quat rotation;
} runtime_collider;

// A pose slot: one body the renderer draws.
typedef struct runtime_body {
    bool active; // the slot has a body
    // Moved discontinuously since the last snapshot (just created, or
    // teleported), so the renderer must not glide into the new pose.
    bool teleported;
    b3BodyId body;
    // Copied out of Box3D after every world step (and by a teleport).
    vec3 position;
    quat rotation;
} runtime_body;

// What a game_pointer() call is about.
typedef enum runtime_pointer_kind {
    RUNTIME_POINTER_DOWN,  // a button went down (or a finger touched)
    RUNTIME_POINTER_MOVE,  // the pointer moved while pressed
    RUNTIME_POINTER_UP,    // the button came up (or the browser cancelled the press)
    RUNTIME_POINTER_WHEEL, // the wheel turned
} runtime_pointer_kind;

// One pointer event from the page. Which fields are set depends on `kind`.
typedef struct runtime_pointer {
    runtime_pointer_kind kind;
    // DOWN, UP: 0 left (and any touch or pen contact), 1 middle, 2 right.
    // UP: BRIDGE_BUTTON_CANCELLED for a press the browser cancelled.
    uint32_t button;
    uint32_t buttons;   // MOVE: the buttons held (1 left, 2 right, 4 middle)
    uint32_t device;    // DOWN, MOVE, UP: BRIDGE_POINTER_MOUSE, _TOUCH or _PEN
    float ndc_x, ndc_y; // DOWN, MOVE, UP: where, in normalized device coordinates
    // MOVE: how far it moved, CSS pixels. WHEEL: dy is the wheel's deltaY in
    // CSS pixels (positive = towards the user).
    float dx, dy;
    // DOWN, MOVE, UP: the browser's event time in microseconds, wrapped to 32
    // bits. Only differences mean anything; subtract as uint32_t.
    uint32_t time_us;
} runtime_pointer;

// ── State ──────────────────────────────────────────────────────────────────

// The dom → app input, batched one page frame at a time (see
// BRIDGE_EV_FRAME_MARK in bridge.h). Records are taken off the ring into
// `queue` as they arrive; runtime_frame_input() releases the ones the coming
// burst may apply, and the first step of the burst applies them.
typedef struct runtime_input {
    // runtime_frame_input() has been called: the browser loop. Without it
    // (most tests) each step applies whatever is in the ring.
    bool framed;
    bool seen_mark; // a FRAME_MARK has ever arrived
    int count;      // records in queue
    int released;   // queue[0, released) may be applied
    // Each record's `step` field is overwritten here with the frame time at
    // which it was taken off the ring (the page leaves it 0), for the timeout.
    bridge_record queue[RUNTIME_INPUT_QUEUE];
} runtime_input;

typedef struct runtime_state {
    bridge_header *bridge;
    uint32_t step; // the last step run; the first is 1
    // The moment the current step stands for, in milliseconds on the clock
    // fixed_step_advance() was given. Published with the step, as frame-clock
    // microseconds (bridge_snapshot_header.time_us).
    double step_time_ms;

    // ── The scene ──
    // Set once the renderer has described the scene. The game is not stepped
    // before.
    bool scene_ready;
    // runtime_reload_scene() was called: what the renderer still says about
    // the old scene is ignored until the description of the fresh one begins.
    bool reload_pending;
    // Collected between SCENE_BEGIN and SCENE_READY.
    runtime_collider colliders[RUNTIME_MAX_COLLIDERS];
    int collider_count;

    // The view the renderer draws, published with every step.
    orbit_camera camera;

    // ── Physics ──
    // One world for the app's whole life. A new scene replaces its bodies,
    // not the world, which also keeps the solver threads (owned by the world)
    // running throughout.
    b3WorldId world;
    b3BodyId statics[RUNTIME_MAX_COLLIDERS]; // runtime_create_collider_body()
    int static_count;
    runtime_body bodies[GAME_SLOT_COUNT]; // by pose slot
    // The step each cue id was last sent at by runtime_send_cue_throttled().
    uint32_t last_cue_step[RUNTIME_CUE_IDS];

    // ── Input ──
    runtime_input input;
    // Keys held, by BRIDGE_KEY_* bit, as the EV_KEY records applied so far
    // say. `keys_tapped` also has every key that went down in this step's
    // input, even if it came back up in the same batch: a tap shorter than a
    // frame still counts as held for one step.
    uint32_t keys_down[2];
    uint32_t keys_tapped[2];

    // ── Timing, for the page's stats panel ──
    // The run loop sets the two flags while the panel has that breakdown open,
    // and only then: `profile` times the phases of the step, `profile_physics`
    // passes on the stages of Box3D's step.
    bool profile;
    bool profile_physics;
    uint64_t lap_ticks; // where the phase being timed began
    // Time inside Box3D's step since the run loop last took it (always
    // measured: the panel's physics headline).
    double physics_ms;

    // Your game's own state. The runtime only hands it to the hooks.
    game_state game;
} runtime_state;

// ── Running the runtime (web_main.c, main.c, fixed_step.c and the tests) ────

// Sets `runtime` up to talk through `bridge` (which must outlive it), creates
// the Box3D world, then calls game_init(). `solver_threads` is how many
// threads Box3D steps the world on, counting the caller: 1 keeps everything
// on the calling thread.
void runtime_init(runtime_state *runtime, bridge_header *bridge, uint32_t solver_threads);

// Destroys the Box3D world (and with it, any solver threads).
void runtime_destroy(runtime_state *runtime);

// Runs one fixed step of `dt` seconds and publishes the result as a snapshot.
// The first step after runtime_frame_input() applies the input it released.
void runtime_step(runtime_state *runtime, float dt);

// Called once per frame, before that frame's steps, with the frame's time on
// the frame clock and the display's frame period. Takes new input off the
// ring and releases what this frame's burst should apply: everything up to
// the newest FRAME_MARK at or before `frame_time_us - period / 2`. Records
// after it wait for a later frame; released records a burst of zero steps did
// not get to wait for the next step that runs.
void runtime_frame_input(runtime_state *runtime, uint32_t frame_time_us, double period_ms);

// ═══ Your game provides (app/src/game/game.c) ══════════════════════════════

// Once, when the app starts (and so after every hot swap): set up `game` and
// the camera. The world exists but is empty; no scene yet.
void game_init(runtime_state *runtime, game_state *game);

// A scene description is starting (after the first load, after every reload,
// and when a hot-swapped app attaches): the world begins again from nothing.
// The runtime has already destroyed every body; reset your game state here.
void game_scene_begin(runtime_state *runtime, game_state *game);

// The scene is described: its colliders are in runtime_get_collider(). Build
// your world from them (runtime_create_collider_body, runtime_bind_body, …).
// The game starts running.
void game_scene_ready(runtime_state *runtime, game_state *game);

// Key `key` (BRIDGE_KEY_*) went down or up. Only called while the scene is
// ready. For one-off actions; keys that act for as long as they are held are
// polled with runtime_key_held() in game_update().
void game_key(runtime_state *runtime, game_state *game, uint32_t key, bool down);

// A pointer or wheel event. Called whether or not the scene is ready (so the
// camera can be turned while it loads): check runtime_scene_ready() before
// touching the world.
void game_pointer(runtime_state *runtime, game_state *game, const runtime_pointer *pointer);

// One fixed step of `dt` seconds, before the world steps, while the scene is
// ready: your game loop. Push bodies around here.
void game_update(runtime_state *runtime, game_state *game, float dt);

// After the world step: shapes `a` and `b` started (`touching`) or stopped
// touching. Box3D reports this only for shapes created with
// `enableContactEvents`, and an ending touch may name a shape that has since
// been destroyed.
void game_contact(runtime_state *runtime, game_state *game, b3ShapeId a, b3ShapeId b,
                  bool touching);

// After the world step: shapes `a` and `b` hit each other at `speed` m/s
// (faster than GAME_HIT_MIN_SPEED), at `where`. Only for shapes created with
// `enableHitEvents`.
void game_hit(runtime_state *runtime, game_state *game, b3ShapeId a, b3ShapeId b, float speed,
              vec3 where);

// The end of the step, after the contacts and hits: every body's pose is up
// to date (runtime_get_body). Read the results and send cues here.
void game_after_physics(runtime_state *runtime, game_state *game);

// ═══ The API your game calls ═══════════════════════════════════════════════

// ── The scene ──

// The scene's colliders, valid from game_scene_ready() on: `index` from 0 to
// runtime_collider_count() - 1.
int runtime_collider_count(const runtime_state *runtime);
const runtime_collider *runtime_get_collider(const runtime_state *runtime, int index);

// Whether the scene is described and the game is running.
bool runtime_scene_ready(const runtime_state *runtime);

// Asks the renderer to destroy everything in the scene and load it again from
// the bundle, without touching the GPU device or reloading the page
// (BRIDGE_CMD_RELOAD_SCENE). Every body is destroyed at once, and the game is
// not stepped until the renderer has described the fresh scene; it then
// starts over, from game_scene_begin(), exactly as after the first load. Call
// it from anywhere in the game: a "restart" key (R in the demo), a game-over
// screen, the next level.
void runtime_reload_scene(runtime_state *runtime);

// The step being run, counting from 1. Steps are 1 / APP_STEP_HZ apart.
uint32_t runtime_current_step(const runtime_state *runtime);

// ── Input ──

// Whether key `key` (BRIDGE_KEY_*) counts as held in the current step.
bool runtime_key_held(const runtime_state *runtime, uint32_t key);

// The ray from the camera through a point on the canvas, given in normalized
// device coordinates (a runtime_pointer's ndc_x, ndc_y): what is under the
// pointer, as the picture shows it.
ray runtime_pointer_ray(const runtime_state *runtime, float ndc_x, float ndc_y);

// ── The camera ──

// The camera the renderer draws from. Change it at any time; every step
// publishes it as it is then.
orbit_camera *runtime_get_camera(runtime_state *runtime);

// ── Bodies ──

// The Box3D world. Create bodies in it with the b3* API, then hand those to
// be drawn to runtime_bind_body() or runtime_spawn_body().
b3WorldId runtime_world(const runtime_state *runtime);

// A static body where `collider` is in the scene, with the collider's shape
// made from `shape_def` (material, user data, events). The runtime destroys
// it with the scene. Returns b3_nullBodyId for a shape it cannot make.
b3BodyId runtime_create_collider_body(runtime_state *runtime, const runtime_collider *collider,
                                      const b3ShapeDef *shape_def);

// Hands `body` to pose slot `slot`: from now on it is drawn as scene node
// `node` itself (bind), with material variant `variant` (nullptr: keep its
// material).
void runtime_bind_body(runtime_state *runtime, uint32_t slot, b3BodyId body, const char *node,
                       const char *variant);

// The same, drawn as a new copy of scene node `node` (spawn). With `variant`
// nullptr the copy has the node's original material.
void runtime_spawn_body(runtime_state *runtime, uint32_t slot, b3BodyId body, const char *node,
                        const char *variant);

// Destroys slot `slot`'s body and takes it off the screen.
void runtime_destroy_body(runtime_state *runtime, uint32_t slot);

// Slot `slot`: whether it has a body, the body, and its pose.
const runtime_body *runtime_get_body(const runtime_state *runtime, uint32_t slot);

// Moves slot `slot`'s body to `position` and `rotation` at once, at rest, and
// tells the renderer it jumped rather than moved.
void runtime_teleport_body(runtime_state *runtime, uint32_t slot, vec3 position, quat rotation);

// How many slots have a body.
int runtime_active_bodies(const runtime_state *runtime);

// ── Cues (sound and UI, for the page) ──

// Tells the page something happened: `cue` is one of your GAME_CUE_* ids,
// `value` a number that goes with it (an intensity, a speed, an angle) and
// `where` a world position. dom/game.js gets all three.
void runtime_send_cue(runtime_state *runtime, uint32_t cue, float value, vec3 where);

// The same, unless cue `cue` was already sent less than `min_gap_s` seconds
// ago: a ball grinding along a rail knocks, it does not rattle. Returns
// whether it was sent. Cue ids from RUNTIME_CUE_IDS up are always sent.
bool runtime_send_cue_throttled(runtime_state *runtime, uint32_t cue, float value, vec3 where,
                                float min_gap_s);

#endif // RUNTIME_H
