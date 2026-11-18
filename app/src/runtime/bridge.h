// RUNTIME: shared by every game; your game goes in app/src/game/ and dom/game.js
//
// bridge.h — THE contract between the three threads.
//
//   dom/       main thread, vanilla JS      (reads + writes through typed-array views)
//   renderer/  render worker, Rust          (reads + writes through typed-array views)
//   app/       logic worker, C (this side)  (reads + writes through plain pointers)
//
// The "bridge block" is one contiguous region inside the APP module's linear
// memory. That memory is a shared WebAssembly.Memory, so its SharedArrayBuffer
// can be handed to the other two threads. A wasm module can only dereference
// its own memory, which is why the block lives here: the app (where iteration
// happens) touches it with zero copies, and the other threads go through
// views — the renderer does one bulk copy per frame, nothing more.
//
// This header is the single source of truth for the layout. The mirrors are
// `renderer/src/bridge.rs` and `dom/runtime/bridge.js`; nothing else in any of
// the three codebases should know these offsets. `task test` compares both
// mirrors with this header (app/tests/runtime/bridge_layout.c prints the
// layout as the compiler sees it, tools/check-bridge.mjs compares), so they
// cannot drift without a test failing.
//
// ── What is in the block ───────────────────────────────────────────────────
//
//   bridge_header      magic/version handshake, the frame-tick futex word, and
//                      a SECTION TABLE (offset/stride/capacity of everything
//                      below). Readers take offsets from the table, never from
//                      constants, so sections can grow or move without the
//                      renderer being rebuilt.
//   bridge_input       continuous input state (held keys, pointer, canvas size)
//                      dom → app
//   bridge_stats       counters for the on-page stats panel
//   timing table       named running totals for that panel's breakdowns (the
//                      app's step phases, Box3D's stages, the renderer's
//                      frame phases and passes), filled in only while the
//                      panel asks for them
//   snapshot ring      the app's published world state: one snapshot per fixed
//                      step (camera + a pose per slot, and the time the step
//                      stands for), newest BRIDGE_SNAPSHOT_COUNT steps kept.
//                      The renderer draws each frame between the two steps
//                      around one moment (see "Frame pacing" below).
//                      app → renderer
//   4 event rings      single-producer/single-consumer queues of fixed-size
//                      tagged records, for everything discrete:
//                        dom → app        key / pointer / wheel events
//                        renderer → app   what the loaded scene contains
//                        app → renderer   bind / spawn / despawn commands
//                        app → dom        cues (sound, UI)
//
// ── Rules that keep a stale renderer safe ─────────────────────────────────
//
//   1. Layout changes are ADDITIVE: add fields in place of `reserved` words,
//      append record types, grow capacities, add sections. Offsets of existing
//      fields never move. None of that needs a renderer rebuild. (A struct can
//      also grow at its end, since readers use the strides in the section
//      table; update its size guard below when you do that.)
//   2. Anything that changes the MEANING of existing data bumps
//      BRIDGE_VERSION. A mismatch is reported on the page instead of drawing
//      garbage.
//   3. Records are fixed-size and tagged, so a consumer skips types it does
//      not know.
//
// ── Concurrency ───────────────────────────────────────────────────────────
//
//   * Every field marked `_Atomic` is read/written with atomic ops by all
//     sides (JS: `Atomics.*` on an Int32Array/Uint32Array view).
//   * A snapshot is guarded by a seqlock (`version`): odd while the app is
//     writing it, even when stable. A reader copies it, then re-checks that
//     `version` is unchanged and even; otherwise it retries.
//   * Each ring has exactly one producer and one consumer. The producer owns
//     `head`, the consumer owns `tail`; both are free-running counters and the
//     slot index is `counter & (capacity - 1)`.
//
// ── Frame pacing ──────────────────────────────────────────────────────────
//
// One clock, the "frame clock", times everything: the browser's frame
// timestamp (performance.timeOrigin + the requestAnimationFrame time) in
// microseconds, wrapped to 32 bits. Every thread computes it the same way, so
// their readings compare directly. Compare two of them with a signed
// difference, `(int32_t)(a - b)`; they wrap every 71 minutes.
//
//   1. At the START of each frame the renderer stores the frame's time in
//      `frame_time_us` (and sets `frame_time_valid`), then bumps `frame_tick`.
//   2. The app wakes on the tick and runs as many fixed steps as the time
//      since the previous frame time pays for (Fix Your Timestep). Each
//      snapshot carries the frame-clock time its step stands for (`time_us`):
//      after a burst, the newest step stands for `frame_time - leftover`.
//   3. The NEXT frame draws the moment `previous frame_time - one step`,
//      interpolating between the two snapshots around it. That moment is
//      fixed before the app starts its next burst, so the burst cannot move
//      it, and it advances by exactly the time between frames: motion is
//      even whatever the step counts per burst. If the app has not published
//      that far yet, the renderer holds the newest snapshot and counts the
//      frame in `app_late`.
//   4. Input is batched per page frame: the page pushes a FRAME_MARK record
//      after each frame's input, and the app applies, in the first step of a
//      burst, the records up to the newest mark that is at least half a frame
//      older than the burst's frame time (see BRIDGE_EV_FRAME_MARK).
//
// All values are little-endian (wasm). All offsets are in bytes from the
// start of the block.

#ifndef BRIDGE_H
#define BRIDGE_H

// `_Atomic` is a keyword, so this header needs no <stdatomic.h>; the files
// that call atomic_load() and friends on these fields include it themselves.
#include <stddef.h>
#include <stdint.h>

// ── Handshake ──────────────────────────────────────────────────────────────

// 'AWSB' as little-endian bytes. First word of the block.
#define BRIDGE_MAGIC 0x42535741u

// Bump ONLY for a change that alters the meaning of existing data (rule 2).
#define BRIDGE_VERSION 1u

// ── Sizes (the app is free to change these; readers use the section table) ─

// Steps of history kept in the snapshot ring. The renderer reads the two steps
// around the moment it draws while the app writes its next burst, so this
// must exceed the largest burst (fixed_step.c) by at least two. Must be a
// power of two.
#define BRIDGE_SNAPSHOT_COUNT 32u

// Maximum number of simultaneously posed things (one pose "slot" each).
#define BRIDGE_POSE_CAPACITY 256u

// Records per event ring. Must be a power of two.
#define BRIDGE_RING_CAPACITY 256u

// ── Header ─────────────────────────────────────────────────────────────────

// Index into bridge_header.ring_offset.
enum {
    BRIDGE_RING_DOM_TO_APP = 0,
    BRIDGE_RING_RENDERER_TO_APP = 1,
    BRIDGE_RING_APP_TO_RENDERER = 2,
    BRIDGE_RING_APP_TO_DOM = 3,
    BRIDGE_RING_COUNT = 4,
};

typedef struct bridge_header {
    uint32_t magic;       //   0  BRIDGE_MAGIC
    uint32_t version;     //   4  BRIDGE_VERSION
    uint32_t total_size;  //   8  size of the whole block, bytes
    uint32_t header_size; //  12  sizeof(bridge_header) as built

    // Fixed simulation rate. The app advances in steps of 1/step_hz seconds;
    // the renderer converts frame time into steps with the same number.
    float step_hz; //  16

    // Bumped (and notified) by the renderer at the start of every frame,
    // after it has stored `frame_time_us`. The app blocks on this word
    // between frames, which keeps the simulation in phase with the display
    // instead of beating against it.
    _Atomic uint32_t frame_tick; //  20

    // Step index of the newest fully published snapshot. Written by the app
    // AFTER the snapshot is complete.
    _Atomic uint32_t latest_step; //  24

    uint32_t reserved0; //  28

    // Section table.
    uint32_t input_offset; //  32
    uint32_t input_size;   //  36
    uint32_t stats_offset; //  40
    uint32_t stats_size;   //  44

    uint32_t snapshot_offset;      //  48  first snapshot
    uint32_t snapshot_count;       //  52  ring length (power of two)
    uint32_t snapshot_stride;      //  56  bytes from one snapshot to the next
    uint32_t snapshot_header_size; //  60  bytes before a snapshot's first pose
    uint32_t pose_capacity;        //  64  poses per snapshot
    uint32_t pose_stride;          //  68  bytes per pose

    uint32_t ring_offset[BRIDGE_RING_COUNT]; //  72  each points at a bridge_ring
    uint32_t ring_header_size;               //  88  bytes before a ring's first record
    uint32_t record_size;                    //  92  bytes per record

    // Written by the renderer at the start of each frame, before it bumps
    // `frame_tick`: that frame's time on the frame clock (see "Frame pacing"
    // above), in microseconds, wrapped to 32 bits.
    _Atomic uint32_t frame_time_us; //  96
    // Set to 1 by the renderer once it has stored a frame time, and never
    // cleared. 0 means a renderer from before frame times: the app then keeps
    // time itself (on the same clock).
    _Atomic uint32_t frame_time_valid; // 100

    // Section table, continued. All three are 0 from an app built before the
    // timing table existed: there is no table then.
    uint32_t timing_offset;   // 104  the first bridge_timing
    uint32_t timing_capacity; // 108  entries in the table
    uint32_t timing_stride;   // 112  bytes per entry

    uint32_t reserved1[3]; // 116
} bridge_header;           // 128

// ── Input state (dom → app) ────────────────────────────────────────────────

// Keys the page reports (KEY_CODES in dom/game.js maps `KeyboardEvent.code` to
// these); what a key DOES is decided in the game. Values index bits in
// bridge_input.keys.
enum {
    BRIDGE_KEY_NONE = 0,
    BRIDGE_KEY_W = 1,
    BRIDGE_KEY_A = 2,
    BRIDGE_KEY_S = 3,
    BRIDGE_KEY_D = 4,
    BRIDGE_KEY_UP = 5,
    BRIDGE_KEY_DOWN = 6,
    BRIDGE_KEY_LEFT = 7,
    BRIDGE_KEY_RIGHT = 8,
    BRIDGE_KEY_SPACE = 9,
    BRIDGE_KEY_SHIFT = 10,
    BRIDGE_KEY_ENTER = 11,
    BRIDGE_KEY_ESCAPE = 12,
    BRIDGE_KEY_R = 13,
    BRIDGE_KEY_COUNT = 64, // room to append without changing the layout
};

typedef struct bridge_input {
    // Bit `k` of keys[k / 32] is set while key `k` is held.
    _Atomic uint32_t keys[2]; //  0

    // Canvas backing-store size in device pixels (for aspect ratio).
    _Atomic uint32_t canvas_width;  //  8
    _Atomic uint32_t canvas_height; // 12

    // Last pointer position in normalized device coordinates: x right, y up,
    // both -1..1. Stored as f32 bit patterns.
    _Atomic uint32_t pointer_ndc_x; // 16
    _Atomic uint32_t pointer_ndc_y; // 20

    // Bitmask of pressed pointer buttons, as in `PointerEvent.buttons`.
    _Atomic uint32_t pointer_buttons; // 24

    uint32_t reserved[9]; // 28
} bridge_input;           // 64

// ── Stats (for the HUD; any side may read) ─────────────────────────────────

typedef struct bridge_stats {
    // Written by the app.
    _Atomic uint32_t step_work_us; //  0  total microseconds spent stepping (wraps)
    _Atomic uint32_t active_poses; //  4  slots currently active

    // Written by the renderer.
    _Atomic uint32_t frame_count;   //  8  frames presented (wraps)
    _Atomic uint32_t frame_work_us; // 12  total microseconds of CPU per frame (wraps)

    // Written by the renderer, for the page's stats panel. Every field below
    // replaced a `reserved` word (rule 1), so an app built before they existed
    // simply leaves them at zero.
    //
    // GPU time per frame, from the renderer's timestamp queries: a running
    // total and how many frames it sums. The readback skips frames while one
    // is in flight, so the count trails frame_count. Both stay 0 on a GPU
    // without `timestamp-query`.
    _Atomic uint32_t gpu_frame_us;    // 16  total microseconds of GPU per sampled frame (wraps)
    _Atomic uint32_t gpu_frame_count; // 20  frames in gpu_frame_us (wraps)
    // Retired: written by renderers that had an adaptive "display cursor",
    // which frame times replaced. Current renderers leave them at 0; they
    // stay so that no later field moves (rule 1).
    _Atomic uint32_t sync_error_max; // 24  (retired) f32 bits, steps
    _Atomic uint32_t display_lag;    // 28  (retired) f32 bits, steps

    // Written by the app, once, at start-up.
    _Atomic uint32_t solver_threads; // 32  threads Box3D steps the world on, counting the app's

    // Written by the renderer: the time from each presented frame to the next,
    // summed, by the browser's frame timestamps. Over a window, this divided
    // by the display's refresh period says how many refreshes went by, and
    // the change in frame_count how many of them got a new frame; the page
    // shows the difference as missed frames.
    _Atomic uint32_t frame_span_us; // 36  total microseconds between frames (wraps)

    // Written by the renderer: frames drawn before the app had published the
    // moment they show, so they held the newest snapshot instead (the app was
    // late: its burst did not finish within a frame).
    _Atomic uint32_t app_late; // 40  frames (wraps)

    // Written by the PAGE: which breakdowns its stats panel is showing right
    // now (BRIDGE_DETAIL_* bits). The app and the renderer only spend time
    // measuring the detail that is asked for; this is not a running total.
    _Atomic uint32_t detail; // 44

    // Written by the app: the part of step_work_us spent inside Box3D's step
    // (b3World_Step, which runs on `solver_threads` threads). The page shows
    // it as the physics time, and the rest of step_work_us as the app's own.
    _Atomic uint32_t physics_step_us; // 48  total microseconds (wraps)

    uint32_t reserved[3]; // 52
} bridge_stats;           // 64

// bridge_stats.detail
enum {
    BRIDGE_DETAIL_APP = 1u << 0,      // the app's step phases
    BRIDGE_DETAIL_RENDERER = 1u << 1, // the renderer's frame phases and CPU time per pass
    BRIDGE_DETAIL_GPU = 1u << 2,      // GPU time per pass
    BRIDGE_DETAIL_PHYSICS = 1u << 3,  // the stages of Box3D's step
};

// ── Timing table (breakdowns for the stats panel) ──────────────────────────
//
// One named running total per entry. The app owns entries
// [0, BRIDGE_TIMING_APP_SLOTS), the renderer the rest; each side only writes
// its own. An owner writes `name` first and `kind` last, so a reader that
// sees a kind sees the whole name. The page shows the change in `total_ns`
// divided by the change in `count` between two samples.
//
// The capacity can grow freely (rule 1). BRIDGE_TIMING_APP_SLOTS is the one
// number here the renderer has compiled in: changing it needs a renderer
// rebuild, which the mirror check (tools/check-bridge.mjs) insists on.
//
// Capacity: the renderer claims an entry per name the first time it has a
// number for it, and names it cannot fit are left out of the panel. With
// every effect on it needs about 80 (4 phases, some 20 "prepare" and "submit"
// scopes, up to 25 passes on the CPU and as many on the GPU, a hook each), so
// 104 of its own leave room.

#define BRIDGE_TIMING_CAPACITY 128u
#define BRIDGE_TIMING_APP_SLOTS 24u

// bridge_timing.kind: what the entry measures, which is also where the page
// shows it.
enum {
    BRIDGE_TIMING_UNUSED = 0,
    BRIDGE_TIMING_APP_PHASE = 1,      // a part of the app's step            (per step)
    BRIDGE_TIMING_RENDERER_PHASE = 2, // a part of the renderer's frame      (per frame)
    BRIDGE_TIMING_RENDERER_PASS = 3,  // CPU time encoding one render pass   (per frame)
    BRIDGE_TIMING_GPU_PASS = 4,       // GPU time of one render pass         (per frame)
    // 5 is retired (what the renderer's sampled timers cost) and not reused.
    BRIDGE_TIMING_PHYSICS_STAGE = 6, // a stage of Box3D's step              (per step)
    BRIDGE_TIMING_PHYSICS_SOLVE = 7, // a part of its solve stage            (per step)
    // The rest of the renderer's render(), by what awsm-renderer says each of
    // its scopes is (profiling::cpu_scope_kind):
    BRIDGE_TIMING_RENDERER_PREP = 8,   // work before the first pass         (per frame)
    BRIDGE_TIMING_RENDERER_SUBMIT = 9, // finishing and submitting the frame (per frame)
    BRIDGE_TIMING_RENDERER_HOOK = 10,  // a render hook of the renderer's    (per frame)
};

typedef struct bridge_timing {
    _Atomic uint32_t kind;     //  0  BRIDGE_TIMING_*
    _Atomic uint32_t total_ns; //  4  running total, nanoseconds (wraps)
    _Atomic uint32_t count;    //  8  steps or frames that total covers (wraps)
    char name[52];             // 12  ASCII, padded with zero bytes
} bridge_timing;               // 64

// ── Snapshot ring (app → renderer) ─────────────────────────────────────────

// The view the renderer should draw from. The renderer supplies the aspect
// ratio and depth convention itself.
typedef struct bridge_camera {
    float eye[3];    //  0  world position
    float target[3]; // 12  world point looked at
    float up[3];     // 24
    float fov_y;     // 36  vertical field of view, radians
    float near;      // 40
    float far;       // 44
} bridge_camera;     // 48

// bridge_pose.flags
enum {
    // The slot holds a live pose in this snapshot. Inactive slots are ignored.
    BRIDGE_POSE_ACTIVE = 1u << 0,
    // The pose jumped this step: the renderer must not interpolate into it
    // from the previous step (it would sweep the object across the world).
    BRIDGE_POSE_TELEPORT = 1u << 1,
};

typedef struct bridge_pose {
    float position[3]; //  0  world space
    float rotation[4]; // 12  quaternion x, y, z, w
    uint32_t flags;    // 28  BRIDGE_POSE_*
} bridge_pose;         // 32

typedef struct bridge_snapshot_header {
    // Seqlock: odd while being written, even when stable.
    _Atomic uint32_t version; //  0
    // Which step this snapshot holds. The ring overwrites in place, so a
    // reader that wants step N must check it still says N.
    uint32_t step; //  4
    // Slots [0, pose_count) may be active; the rest are not.
    uint32_t pose_count; //  8
    // The moment this step stands for, on the frame clock (microseconds,
    // wrapped; see "Frame pacing" above). Never 0: the app writes 1 instead
    // of a time that wraps to exactly 0, so that 0 can keep meaning "an app
    // from before step times" (the renderer then just draws the newest step).
    uint32_t time_us;     // 12
    bridge_camera camera; // 16
} bridge_snapshot_header; // 64

// A snapshot is a bridge_snapshot_header followed by pose_capacity poses.
// Step N lives in ring index `N & (snapshot_count - 1)`.

// ── Event rings ────────────────────────────────────────────────────────────

typedef struct bridge_ring {
    _Atomic uint32_t head;    //  0  records ever written  (producer only)
    _Atomic uint32_t tail;    //  4  records ever consumed (consumer only)
    uint32_t capacity;        //  8  power of two
    _Atomic uint32_t dropped; // 12  records the producer discarded because the ring was full
    uint32_t reserved[12];    // 16
} bridge_ring;                // 64, followed by `capacity` records

// One event. `type` selects which of the layouts documented below applies;
// `u` and `f` are two views of the same 14 payload words.
typedef struct bridge_record {
    uint32_t type; //  0  BRIDGE_EV_* / BRIDGE_SCENE_* / BRIDGE_CMD_* / BRIDGE_CUE
    uint32_t step; //  4  app step when written (0 if the producer has no step)
    union {
        uint32_t u[14];
        float f[14];
    }; //  8
} bridge_record; // 64

// Names cross the bridge as 32-bit hashes, never as strings: bridge_hash() of
// the UTF-8 name (FNV-1a). Scene nodes and material variants are identified
// this way. 0 means "none".

// Record types. Payload word indices refer to `u[i]` / `f[i]`.
enum {
    // ── dom → app ──────────────────────────────────────────────────────────
    // A key changed.            u[0] = BRIDGE_KEY_*   u[1] = 1 down, 0 up
    BRIDGE_EV_KEY = 1,
    // A pointer button went down. u[0] = button (0 left, 1 middle, 2 right;
    //                                     a touch or pen contact is 0)
    //                             f[1], f[2] = position, NDC
    //                             u[3] = BRIDGE_POINTER_* (what kind of device)
    //                             u[4] = time, microseconds (see below)
    BRIDGE_EV_POINTER_DOWN = 2,
    // A pointer button went up.   same payload as POINTER_DOWN
    // u[0] = BRIDGE_BUTTON_CANCELLED means the press was cancelled by the
    // browser (e.g. a touch turned into a scroll): release it, but do not
    // treat it as a click.
    BRIDGE_EV_POINTER_UP = 3,
    // The pointer moved.          f[0], f[1] = delta in CSS pixels
    //                             f[2], f[3] = position, NDC
    //                             u[4] = buttons bitmask (1 left, 2 right, 4 middle)
    //                             u[5] = BRIDGE_POINTER_*
    //                             u[6] = time, microseconds
    // Pointer times are the browser's event timestamps in microseconds,
    // wrapped to 32 bits (about 71 minutes). Only differences between them
    // mean anything (e.g. how fast a swipe was); subtract as uint32_t.
    BRIDGE_EV_POINTER_MOVE = 4,
    // The wheel turned.           f[0] = deltaY in CSS pixels (positive = towards the user)
    BRIDGE_EV_WHEEL = 5,
    // The page finished a frame: every record before this one belongs to
    // that frame or an earlier one. Sent only after a frame that sent input
    // (a frame with none has nothing to close), so an idle page adds nothing
    // to the ring, even while the app is not stepping.
    //                             u[0] = the page frame's time on the frame
    //                                    clock (see "Frame pacing" above)
    // The app applies input one page frame at a time: in the first step of
    // the burst for frame time T, it applies the records up to and including
    // the newest mark at or before T - half a frame, and keeps the rest for a
    // later burst. That way every burst gets exactly one page frame of input,
    // whichever thread happens to run first. An app that sees no marks (an
    // older page) applies everything at once, and input left waiting for a
    // mark too long (RUNTIME_MARK_TIMEOUT_US in runtime.h) is applied without
    // it.
    BRIDGE_EV_FRAME_MARK = 6,

    // ── renderer → app ─────────────────────────────────────────────────────
    // Sent once per attach, after the scene is on screen, and again after
    // every scene reload (BRIDGE_CMD_RELOAD_SCENE). A BEGIN, then one
    // COLLIDER per collider node, then READY. Nothing should be simulated
    // before READY arrives.
    //   u[0]  0: the scene is exactly as loaded; no app has changed it.
    //         1: a previous app instance has driven this scene (the app was
    //            hot-swapped). The renderer has undone every bind and spawn,
    //            but the scene has been played with.
    BRIDGE_SCENE_BEGIN = 16,
    // One collider node, in WORLD space (node scale already folded into the
    // shape dimensions).
    //   u[0]  hash of the collider node's own name
    //   u[1]  hash of its top-level ancestor's name (e.g. "Ball")
    //   u[2]  BRIDGE_SHAPE_*
    //   f[3..5]   shape dimensions (see BRIDGE_SHAPE_*)
    //   f[6..8]   translation
    //   f[9..12]  rotation quaternion x, y, z, w
    BRIDGE_SCENE_COLLIDER = 17,
    //   u[0]  number of COLLIDER records sent
    BRIDGE_SCENE_READY = 18,

    // ── app → renderer ─────────────────────────────────────────────────────
    // Drive an existing scene node from a pose slot. The renderer keeps the
    // node's authored scale and takes position + rotation from the slot.
    //   u[0]  pose slot
    //   u[1]  hash of the node's name (its first renderable mesh is driven)
    //   u[2]  hash of a material variant to apply, or 0 to keep the material
    BRIDGE_CMD_BIND = 32,
    // Create a new instance that looks like an existing scene node, driven
    // from a pose slot. Same payload as BIND; u[1] names the node to copy.
    // With u[2] = 0 the instance gets the node's ORIGINAL material, even if a
    // BIND has since given the node itself a variant.
    BRIDGE_CMD_SPAWN = 33,
    // Remove whatever SPAWN created for this slot (or release a BIND).
    //   u[0]  pose slot
    BRIDGE_CMD_DESPAWN = 34,
    // Throw the whole scene away and load it again from the bundle. The GPU
    // device, the canvas, the page and its Settings (resolution,
    // anti-aliasing) stay; every object, bind and spawn goes. The page shows
    // its loading screen meanwhile, and then the renderer describes the fresh
    // scene (BEGIN with u[0] = 0, COLLIDER…, READY) exactly as after the
    // first load. Commands sent after this one apply to the new scene.
    // No payload.
    BRIDGE_CMD_RELOAD_SCENE = 35,

    // ── app → dom ──────────────────────────────────────────────────────────
    // Something happened that the page may want to voice or show.
    //   u[0]  cue id, defined by the game (GAME_CUE_* in game.h)
    //   f[1]  intensity, 0..1
    //   f[2..4]  world position
    BRIDGE_CUE = 48,
};

// "Button" value of a BRIDGE_EV_POINTER_UP for a cancelled press.
#define BRIDGE_BUTTON_CANCELLED 0xFFFFFFFFu

// What kind of device a pointer event came from.
enum {
    BRIDGE_POINTER_MOUSE = 0,
    BRIDGE_POINTER_TOUCH = 1,
    BRIDGE_POINTER_PEN = 2,
};

// BRIDGE_SCENE_COLLIDER shapes and what f[3..5] hold for each.
enum {
    BRIDGE_SHAPE_BOX = 1,      // half extents x, y, z
    BRIDGE_SHAPE_SPHERE = 2,   // radius, -, -
    BRIDGE_SHAPE_CAPSULE = 3,  // half height, radius, -
    BRIDGE_SHAPE_CYLINDER = 4, // half height, radius, -
    BRIDGE_SHAPE_CONE = 5,     // half height, radius, -
};

// FNV-1a, 32 bit. Mirrored in bridge.rs and bridge.js; all three must agree.
static inline uint32_t bridge_hash(const char *name) {
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p != 0; ++p) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

// ── Layout guards ──────────────────────────────────────────────────────────
// If one of these fires, a field was moved or resized. Either undo it, or
// (for a deliberate change of meaning) bump BRIDGE_VERSION and update
// renderer/src/bridge.rs and dom/runtime/bridge.js to match. (These guard
// this header against itself; `task test` checks the two mirrors against it.)

static_assert(sizeof(_Atomic uint32_t) == 4, "atomics must be plain 32-bit words");

static_assert(sizeof(bridge_header) == 128, "bridge_header size");
static_assert(offsetof(bridge_header, step_hz) == 16, "bridge_header.step_hz");
static_assert(offsetof(bridge_header, frame_tick) == 20, "bridge_header.frame_tick");
static_assert(offsetof(bridge_header, latest_step) == 24, "bridge_header.latest_step");
static_assert(offsetof(bridge_header, input_offset) == 32, "bridge_header.input_offset");
static_assert(offsetof(bridge_header, stats_offset) == 40, "bridge_header.stats_offset");
static_assert(offsetof(bridge_header, snapshot_offset) == 48, "bridge_header.snapshot_offset");
static_assert(offsetof(bridge_header, pose_capacity) == 64, "bridge_header.pose_capacity");
static_assert(offsetof(bridge_header, ring_offset) == 72, "bridge_header.ring_offset");
static_assert(offsetof(bridge_header, ring_header_size) == 88, "bridge_header.ring_header_size");
static_assert(offsetof(bridge_header, record_size) == 92, "bridge_header.record_size");
static_assert(offsetof(bridge_header, frame_time_us) == 96, "bridge_header.frame_time_us");
static_assert(offsetof(bridge_header, frame_time_valid) == 100, "bridge_header.frame_time_valid");
static_assert(offsetof(bridge_header, timing_offset) == 104, "bridge_header.timing_offset");

static_assert(sizeof(bridge_input) == 64, "bridge_input size");
static_assert(offsetof(bridge_input, canvas_width) == 8, "bridge_input.canvas_width");
static_assert(offsetof(bridge_input, pointer_ndc_x) == 16, "bridge_input.pointer_ndc_x");
static_assert(offsetof(bridge_input, pointer_buttons) == 24, "bridge_input.pointer_buttons");

static_assert(sizeof(bridge_stats) == 64, "bridge_stats size");
static_assert(offsetof(bridge_stats, frame_count) == 8, "bridge_stats.frame_count");
static_assert(offsetof(bridge_stats, gpu_frame_us) == 16, "bridge_stats.gpu_frame_us");
static_assert(offsetof(bridge_stats, sync_error_max) == 24, "bridge_stats.sync_error_max");
static_assert(offsetof(bridge_stats, display_lag) == 28, "bridge_stats.display_lag");
static_assert(offsetof(bridge_stats, solver_threads) == 32, "bridge_stats.solver_threads");
static_assert(offsetof(bridge_stats, frame_span_us) == 36, "bridge_stats.frame_span_us");
static_assert(offsetof(bridge_stats, app_late) == 40, "bridge_stats.app_late");
static_assert(offsetof(bridge_stats, detail) == 44, "bridge_stats.detail");
static_assert(offsetof(bridge_stats, physics_step_us) == 48, "bridge_stats.physics_step_us");

static_assert(sizeof(bridge_camera) == 48, "bridge_camera size");
static_assert(sizeof(bridge_pose) == 32, "bridge_pose size");
static_assert(offsetof(bridge_pose, rotation) == 12, "bridge_pose.rotation");
static_assert(offsetof(bridge_pose, flags) == 28, "bridge_pose.flags");

static_assert(sizeof(bridge_snapshot_header) == 64, "bridge_snapshot_header size");
static_assert(offsetof(bridge_snapshot_header, step) == 4, "bridge_snapshot_header.step");
static_assert(offsetof(bridge_snapshot_header, pose_count) == 8,
              "bridge_snapshot_header.pose_count");
static_assert(offsetof(bridge_snapshot_header, time_us) == 12, "bridge_snapshot_header.time_us");
static_assert(offsetof(bridge_snapshot_header, camera) == 16, "bridge_snapshot_header.camera");

static_assert(sizeof(bridge_ring) == 64, "bridge_ring size");
static_assert(offsetof(bridge_ring, capacity) == 8, "bridge_ring.capacity");
static_assert(offsetof(bridge_ring, dropped) == 12, "bridge_ring.dropped");

static_assert(sizeof(bridge_record) == 64, "bridge_record size");
static_assert(sizeof(bridge_timing) == 64, "bridge_timing size");
static_assert(offsetof(bridge_record, u) == 8, "bridge_record payload");

static_assert((BRIDGE_SNAPSHOT_COUNT & (BRIDGE_SNAPSHOT_COUNT - 1u)) == 0,
              "snapshot count must be a power of two");
static_assert((BRIDGE_RING_CAPACITY & (BRIDGE_RING_CAPACITY - 1u)) == 0,
              "ring capacity must be a power of two");

// ── C API (implemented in bridge.c) ────────────────────────────────────────
// Declared below this line by the app side. Everything above is the wire
// format and is what the other two threads mirror.
//
// Nothing below changes the layout; it is how C code on the app side touches
// the block. Functions that read `_Atomic` fields take non-const pointers on
// purpose: an atomic load through a const pointer is rejected by some
// compilers.

// Every section starts on a 64-byte boundary (one cache line), and so does the
// block itself.
#define BRIDGE_ALIGN 64u

// ── The block ──

// Bytes the block needs with the sizes above (a multiple of BRIDGE_ALIGN).
[[nodiscard]] size_t bridge_block_size(void);

// Lays the block out in `memory` (which must be BRIDGE_ALIGN-aligned and at
// least bridge_block_size() bytes): zeroes it, fills the header and section
// table, and writes `magic` last. Returns the header, or nullptr if `memory`
// is unusable.
[[nodiscard]] bridge_header *bridge_init(void *memory, size_t size, float step_hz);

// bridge_init() on a fresh aligned heap allocation. Free with bridge_destroy().
[[nodiscard]] bridge_header *bridge_create(float step_hz);
void bridge_destroy(bridge_header *bridge);

// ── Sections (all located through the section table) ──

bridge_input *bridge_get_input(bridge_header *bridge);
bridge_stats *bridge_get_stats(bridge_header *bridge);

// ── Timing table ──

// Which breakdowns the page's stats panel is showing (BRIDGE_DETAIL_* bits).
uint32_t bridge_detail(bridge_header *bridge);

// Names entry `slot` (below BRIDGE_TIMING_APP_SLOTS) and says what it
// measures (a BRIDGE_TIMING_* kind). Call once, before the first
// bridge_timing_add() for it.
void bridge_timing_name(bridge_header *bridge, uint32_t slot, uint32_t kind, const char *name);

// Adds one step's `milliseconds` to entry `slot`.
void bridge_timing_add(bridge_header *bridge, uint32_t slot, double milliseconds);
bridge_ring *bridge_get_ring(bridge_header *bridge, int which); // BRIDGE_RING_*

// ── Event rings ──
//
// A ring has one producer and one consumer, so these never block and never
// take a lock. Records are copied in and out by value.
//
//   bridge_record cue = {.type = BRIDGE_CUE, .step = step};
//   cue.u[0] = GAME_CUE_FLOOR;
//   cue.f[1] = 0.5f;
//   bridge_ring_push(bridge_get_ring(bridge, BRIDGE_RING_APP_TO_DOM), &cue);
//
//   bridge_record rec;
//   while (bridge_ring_pop(bridge_get_ring(bridge, BRIDGE_RING_DOM_TO_APP), &rec)) {
//       switch (rec.type) { ... default: break; } // skip types you don't know
//   }

// Producer side. Returns false (and counts it in `dropped`) when the ring is
// full; the record is discarded rather than waiting for the consumer.
bool bridge_ring_push(bridge_ring *ring, const bridge_record *record);

// Consumer side. Returns false when the ring is empty.
bool bridge_ring_pop(bridge_ring *ring, bridge_record *out);

// ── Snapshots (the app publishes one per fixed step) ──
//
//   bridge_snapshot_header *snap = bridge_snapshot_begin(bridge, step);
//   snap->camera = ...;
//   snap->pose_count = n;
//   bridge_pose *poses = bridge_snapshot_poses(snap);
//   ... fill poses[0 .. n-1] ...
//   bridge_snapshot_publish(bridge, snap);
//
// Steps start at 1, so `latest_step == 0` means nothing has been published.

// Claims the ring slot for `step` and marks it as being written (seqlock odd).
bridge_snapshot_header *bridge_snapshot_begin(bridge_header *bridge, uint32_t step);

// The pose array that follows a snapshot header (pose_capacity entries).
bridge_pose *bridge_snapshot_poses(bridge_snapshot_header *snapshot);

// Marks the snapshot stable (seqlock even), THEN sets `latest_step` to it.
void bridge_snapshot_publish(bridge_header *bridge, bridge_snapshot_header *snapshot);

// ── Input state (written by the page; read here) ──

bool bridge_key_held(bridge_input *input, uint32_t key); // key: BRIDGE_KEY_*
float bridge_pointer_ndc_x(bridge_input *input);
float bridge_pointer_ndc_y(bridge_input *input);
uint32_t bridge_pointer_buttons(bridge_input *input);
// Canvas width / height, or 1 while the page has not reported a size yet.
float bridge_canvas_aspect(bridge_input *input);

// ── Frame pacing ──

uint32_t bridge_frame_tick(bridge_header *bridge);

// The frame time the renderer stored with the latest tick (frame clock,
// microseconds). Returns false, leaving `*time_us` alone, if the renderer has
// never stored one (a renderer from before frame times).
bool bridge_frame_time(bridge_header *bridge, uint32_t *time_us);

// Sleeps until `frame_tick` differs from `last` (the renderer started a
// frame), then returns the new value. In the browser this is a real futex
// wait, so it must only be called on a worker. Native builds have no renderer:
// there it returns the current value immediately, without waiting.
uint32_t bridge_wait_frame(bridge_header *bridge, uint32_t last);

// The renderer's side of the handshake: bump `frame_tick` and wake the app.
// The real renderer does this from Rust; native tests use it as a stand-in.
void bridge_signal_frame(bridge_header *bridge);

// The same, with a frame time stored first (as the real renderer does).
void bridge_signal_frame_at(bridge_header *bridge, uint32_t time_us);

#endif // BRIDGE_H
