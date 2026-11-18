// RUNTIME: shared by every game; your game goes in app/src/game/ and dom/game.js
//
// runtime.c — the step loop around your game's hooks: reads the bridge rings,
// keeps the keys, the camera and the pose slots, steps the Box3D world and
// hands the game what it reported, publishes snapshots. See runtime.h for the
// contract.

#include "runtime/runtime.h"

#include <box3d/box3d.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "game/game.h"
#include "runtime/bridge.h"
#include "runtime/camera.h"
#include "runtime/math3d.h"
#include "runtime/physics.h"

static b3Vec3 to_b3(vec3 v) {
    return (b3Vec3){v.x, v.y, v.z};
}

// ── Talking to the other threads ───────────────────────────────────────────

static void send_command(runtime_state *runtime, uint32_t type, uint32_t slot, uint32_t node,
                         uint32_t variant) {
    bridge_record rec = {.type = type, .step = runtime->step};
    rec.u[0] = slot;
    rec.u[1] = node;
    rec.u[2] = variant;
    if (!bridge_ring_push(bridge_get_ring(runtime->bridge, BRIDGE_RING_APP_TO_RENDERER), &rec)) {
        fprintf(stderr, "app: command ring full, a command was dropped\n");
    }
}

void runtime_send_cue(runtime_state *runtime, uint32_t cue, float value, vec3 where) {
    bridge_record rec = {.type = BRIDGE_CUE, .step = runtime->step};
    rec.u[0] = cue;
    rec.f[1] = value;
    rec.f[2] = where.x;
    rec.f[3] = where.y;
    rec.f[4] = where.z;
    // If the ring is full the page is not draining it (a hidden tab, say);
    // losing a sound is fine, so the result is not checked.
    (void)bridge_ring_push(bridge_get_ring(runtime->bridge, BRIDGE_RING_APP_TO_DOM), &rec);
}

bool runtime_send_cue_throttled(runtime_state *runtime, uint32_t cue, float value, vec3 where,
                                float min_gap_s) {
    if (cue < RUNTIME_CUE_IDS) {
        uint32_t gap_steps = (uint32_t)((double)min_gap_s * (double)APP_STEP_HZ + 0.5);
        if (runtime->step - runtime->last_cue_step[cue] < gap_steps) {
            return false;
        }
        runtime->last_cue_step[cue] = runtime->step;
    }
    runtime_send_cue(runtime, cue, value, where);
    return true;
}

// ── Bodies ─────────────────────────────────────────────────────────────────

b3WorldId runtime_world(const runtime_state *runtime) {
    return runtime->world;
}

b3BodyId runtime_create_collider_body(runtime_state *runtime, const runtime_collider *collider,
                                      const b3ShapeDef *shape_def) {
    if (runtime->static_count == RUNTIME_MAX_COLLIDERS) {
        fprintf(stderr, "app: more than %d collider bodies; one was skipped\n",
                RUNTIME_MAX_COLLIDERS);
        return b3_nullBodyId;
    }
    b3BodyDef body_def = b3DefaultBodyDef(); // static is the default type
    body_def.position = to_b3(collider->position);
    body_def.rotation = (b3Quat){
        .v = {collider->rotation.x, collider->rotation.y, collider->rotation.z},
        .s = collider->rotation.w,
    };
    b3BodyId body = b3CreateBody(runtime->world, &body_def);
    if (!physics_add_collider_shape(body, shape_def, collider->shape, collider->dims, nullptr)) {
        fprintf(stderr, "app: collider shape %u is not supported; skipped\n", collider->shape);
        b3DestroyBody(body);
        return b3_nullBodyId;
    }
    runtime->statics[runtime->static_count++] = body;
    return body;
}

// Gives slot `slot` its body, and the renderer the command that draws it.
static void take_body(runtime_state *runtime, uint32_t command, uint32_t slot, b3BodyId body,
                      const char *node, const char *variant) {
    if (slot >= GAME_SLOT_COUNT) {
        fprintf(stderr, "app: slot %u is beyond GAME_SLOT_COUNT; the body is not drawn\n", slot);
        return;
    }
    b3Vec3 p = b3Body_GetPosition(body);
    b3Quat q = b3Body_GetRotation(body);
    runtime->bodies[slot] = (runtime_body){
        .active = true,
        .teleported = true,
        .body = body,
        .position = vec3_make(p.x, p.y, p.z),
        .rotation = (quat){q.v.x, q.v.y, q.v.z, q.s},
    };
    send_command(runtime, command, slot, bridge_hash(node),
                 variant != nullptr ? bridge_hash(variant) : 0u);
}

void runtime_bind_body(runtime_state *runtime, uint32_t slot, b3BodyId body, const char *node,
                       const char *variant) {
    take_body(runtime, BRIDGE_CMD_BIND, slot, body, node, variant);
}

void runtime_spawn_body(runtime_state *runtime, uint32_t slot, b3BodyId body, const char *node,
                        const char *variant) {
    take_body(runtime, BRIDGE_CMD_SPAWN, slot, body, node, variant);
}

void runtime_destroy_body(runtime_state *runtime, uint32_t slot) {
    if (slot >= GAME_SLOT_COUNT || !runtime->bodies[slot].active) {
        return;
    }
    b3DestroyBody(runtime->bodies[slot].body);
    runtime->bodies[slot].active = false;
    send_command(runtime, BRIDGE_CMD_DESPAWN, slot, 0, 0);
}

const runtime_body *runtime_get_body(const runtime_state *runtime, uint32_t slot) {
    static const runtime_body none = {0};
    return slot < GAME_SLOT_COUNT ? &runtime->bodies[slot] : &none;
}

void runtime_teleport_body(runtime_state *runtime, uint32_t slot, vec3 position, quat rotation) {
    if (slot >= GAME_SLOT_COUNT || !runtime->bodies[slot].active) {
        return;
    }
    runtime_body *slot_body = &runtime->bodies[slot];
    b3Body_SetTransform(slot_body->body, to_b3(position),
                        (b3Quat){.v = {rotation.x, rotation.y, rotation.z}, .s = rotation.w});
    b3Body_SetLinearVelocity(slot_body->body, b3Vec3_zero);
    b3Body_SetAngularVelocity(slot_body->body, b3Vec3_zero);
    slot_body->position = position;
    slot_body->rotation = rotation;
    slot_body->teleported = true;
}

int runtime_active_bodies(const runtime_state *runtime) {
    int count = 0;
    for (int i = 0; i < GAME_SLOT_COUNT; ++i) {
        count += runtime->bodies[i].active ? 1 : 0;
    }
    return count;
}

// Empties the world: every collider body and every slot. The world itself is
// kept. No DESPAWN goes out: this happens when the renderer has dropped (or is
// about to drop) everything on its side too.
static void destroy_bodies(runtime_state *runtime) {
    for (int i = 0; i < runtime->static_count; ++i) {
        b3DestroyBody(runtime->statics[i]);
    }
    runtime->static_count = 0;
    for (int i = 0; i < GAME_SLOT_COUNT; ++i) {
        if (runtime->bodies[i].active) {
            b3DestroyBody(runtime->bodies[i].body);
            runtime->bodies[i].active = false;
        }
    }
    runtime->scene_ready = false;
}

// ── 1. The scene (renderer → app) ──────────────────────────────────────────
//
// The renderer sends SCENE_BEGIN, one SCENE_COLLIDER per collider node, then
// SCENE_READY. Colliders are collected first and handed to the game on READY,
// since they can arrive in any order.

int runtime_collider_count(const runtime_state *runtime) {
    return runtime->collider_count;
}

const runtime_collider *runtime_get_collider(const runtime_state *runtime, int index) {
    return &runtime->colliders[index];
}

bool runtime_scene_ready(const runtime_state *runtime) {
    return runtime->scene_ready;
}

void runtime_reload_scene(runtime_state *runtime) {
    if (runtime->reload_pending) {
        return; // already asked
    }
    bridge_record rec = {.type = BRIDGE_CMD_RELOAD_SCENE, .step = runtime->step};
    if (!bridge_ring_push(bridge_get_ring(runtime->bridge, BRIDGE_RING_APP_TO_RENDERER), &rec)) {
        fprintf(stderr, "app: command ring full, the scene was not reloaded\n");
        return;
    }
    // The scene is going away: stop simulating it now, rather than play on
    // in a world the renderer is about to replace.
    destroy_bodies(runtime);
    runtime->collider_count = 0;
    runtime->reload_pending = true;
}

// A scene description is starting: the world begins again from nothing, as
// it was when the app started.
static void scene_begin(runtime_state *runtime, bool used_by_a_previous_app) {
    // The renderer describes its scene from a clean baseline: anything bound
    // or spawned before is gone on its side, so forget it here too.
    destroy_bodies(runtime);
    runtime->collider_count = 0;
    memset(runtime->last_cue_step, 0, sizeof runtime->last_cue_step);
    game_scene_begin(runtime, &runtime->game);

    if (!used_by_a_previous_app) {
        runtime->reload_pending = false; // this is the fresh scene
    } else if (GAME_RELOAD_SCENE_ON_HOT_SWAP) {
        // This app was hot-swapped into a scene another build has been
        // playing with. Skip its description and ask for a clean one.
        runtime_reload_scene(runtime);
    }
}

static void scene_collider(runtime_state *runtime, const bridge_record *rec) {
    if (runtime->collider_count == RUNTIME_MAX_COLLIDERS) {
        fprintf(stderr, "app: more than %d colliders in the scene; one was ignored\n",
                RUNTIME_MAX_COLLIDERS);
        return;
    }
    runtime->colliders[runtime->collider_count++] = (runtime_collider){
        .name = rec->u[0],
        .top_level = rec->u[1],
        .shape = rec->u[2],
        .dims = {rec->f[3], rec->f[4], rec->f[5]},
        .position = vec3_make(rec->f[6], rec->f[7], rec->f[8]),
        .rotation = {rec->f[9], rec->f[10], rec->f[11], rec->f[12]},
    };
}

static void scene_ready(runtime_state *runtime) {
    // Normally a no-op, since SCENE_BEGIN already emptied the world. It makes
    // a READY that arrives without its BEGIN rebuild the world instead of
    // adding a second copy of everything to it.
    destroy_bodies(runtime);
    runtime->scene_ready = true;
    game_scene_ready(runtime, &runtime->game);
}

static void read_scene(runtime_state *runtime) {
    bridge_ring *ring = bridge_get_ring(runtime->bridge, BRIDGE_RING_RENDERER_TO_APP);
    bridge_record rec;
    while (bridge_ring_pop(ring, &rec)) {
        switch (rec.type) {
        case BRIDGE_SCENE_BEGIN:
            scene_begin(runtime, rec.u[0] != 0);
            break;
        case BRIDGE_SCENE_COLLIDER:
            if (!runtime->reload_pending) {
                scene_collider(runtime, &rec);
            }
            break;
        case BRIDGE_SCENE_READY:
            if (!runtime->reload_pending) {
                scene_ready(runtime);
            }
            break;
        default:
            break; // a record type this app does not know: skip it
        }
    }
}

// ── 2. Input (dom → app) ───────────────────────────────────────────────────
//
// Everything arrives as records on the dom → app ring, in the order it
// happened: keys going down and up, pointer presses, moves and releases, the
// wheel. Which keys are held is worked out from those records too
// (runtime_key_held), rather than read from the page's live bridge_input
// bits, so that a step sees exactly the input that has been applied, and a
// tap shorter than a frame is not lost between two steps.
//
// In the browser the records are applied one page frame at a time, in the
// first step of each burst (runtime_frame_input; see BRIDGE_EV_FRAME_MARK in
// bridge.h for why).

static void key_event(runtime_state *runtime, uint32_t key, bool down) {
    if (key == BRIDGE_KEY_NONE || key >= BRIDGE_KEY_COUNT) {
        return;
    }
    uint32_t bit = 1u << (key % 32u);
    if (down) {
        runtime->keys_down[key / 32u] |= bit;
        runtime->keys_tapped[key / 32u] |= bit;
    } else {
        runtime->keys_down[key / 32u] &= ~bit;
    }
    // Held keys are remembered either way; the game only hears about presses
    // once there is a world for them to act on.
    if (runtime->scene_ready) {
        game_key(runtime, &runtime->game, key, down);
    }
}

static void apply_input(runtime_state *runtime, const bridge_record *rec) {
    runtime_pointer pointer;
    switch (rec->type) {
    case BRIDGE_EV_KEY:
        key_event(runtime, rec->u[0], rec->u[1] != 0);
        return;
    case BRIDGE_EV_POINTER_DOWN:
    case BRIDGE_EV_POINTER_UP:
        pointer = (runtime_pointer){
            .kind = rec->type == BRIDGE_EV_POINTER_DOWN ? RUNTIME_POINTER_DOWN : RUNTIME_POINTER_UP,
            .button = rec->u[0],
            .ndc_x = rec->f[1],
            .ndc_y = rec->f[2],
            .device = rec->u[3],
            .time_us = rec->u[4],
        };
        break;
    case BRIDGE_EV_POINTER_MOVE:
        pointer = (runtime_pointer){
            .kind = RUNTIME_POINTER_MOVE,
            .dx = rec->f[0],
            .dy = rec->f[1],
            .ndc_x = rec->f[2],
            .ndc_y = rec->f[3],
            .buttons = rec->u[4],
            .device = rec->u[5],
            .time_us = rec->u[6],
        };
        break;
    case BRIDGE_EV_WHEEL:
        pointer = (runtime_pointer){.kind = RUNTIME_POINTER_WHEEL, .dy = rec->f[0]};
        break;
    default:
        return; // a FRAME_MARK, or a record type this app does not know: skip it
    }
    game_pointer(runtime, &runtime->game, &pointer);
}

// Takes records off the ring into the queue while there is room, noting when.
static void pull_input(runtime_state *runtime, uint32_t frame_time_us) {
    runtime_input *in = &runtime->input;
    bridge_ring *ring = bridge_get_ring(runtime->bridge, BRIDGE_RING_DOM_TO_APP);
    while (in->count < RUNTIME_INPUT_QUEUE && bridge_ring_pop(ring, &in->queue[in->count])) {
        bridge_record *rec = &in->queue[in->count];
        rec->step = frame_time_us;
        if (rec->type == BRIDGE_EV_FRAME_MARK) {
            in->seen_mark = true;
        }
        in->count += 1;
    }
}

void runtime_frame_input(runtime_state *runtime, uint32_t frame_time_us, double period_ms) {
    runtime_input *in = &runtime->input;
    in->framed = true;
    pull_input(runtime, frame_time_us);

    // A page that sends no marks (an older one): apply everything now.
    if (!in->seen_mark) {
        in->released = in->count;
        return;
    }
    // Up to and including the newest mark at or before the cutoff. Marks come
    // in time order, so every mark after a late one is late too. Half a frame
    // of margin makes the choice robust to how the page's frame and this one
    // are phased: this frame's own mark is always left for the next burst,
    // and the previous frame's is always taken.
    uint32_t cutoff = frame_time_us - (uint32_t)llround(period_ms * 500.0);
    for (int i = in->released; i < in->count; ++i) {
        const bridge_record *rec = &in->queue[i];
        if (rec->type != BRIDGE_EV_FRAME_MARK) {
            continue;
        }
        if ((int32_t)(rec->u[0] - cutoff) > 0) {
            break;
        }
        in->released = i + 1;
    }
    // Input still waiting for its mark after RUNTIME_MARK_TIMEOUT_US: apply it
    // anyway (a lost mark, a page that stalled half way through a frame).
    if (in->released < in->count && (int32_t)(frame_time_us - in->queue[in->released].step) >
                                        (int32_t)RUNTIME_MARK_TIMEOUT_US) {
        in->released = in->count;
    }
}

static void read_input(runtime_state *runtime) {
    runtime_input *in = &runtime->input;
    if (!in->framed) {
        // No frame loop (most tests): apply whatever has arrived.
        bridge_ring *ring = bridge_get_ring(runtime->bridge, BRIDGE_RING_DOM_TO_APP);
        bridge_record rec;
        while (bridge_ring_pop(ring, &rec)) {
            apply_input(runtime, &rec);
        }
        return;
    }
    if (in->released == 0) {
        return; // a later step of the burst: its input went in the first one
    }
    for (int i = 0; i < in->released; ++i) {
        apply_input(runtime, &in->queue[i]);
    }
    in->count -= in->released;
    memmove(in->queue, in->queue + in->released, (size_t)in->count * sizeof in->queue[0]);
    in->released = 0;
}

bool runtime_key_held(const runtime_state *runtime, uint32_t key) {
    if (key >= BRIDGE_KEY_COUNT) {
        return false;
    }
    uint32_t word = runtime->keys_down[key / 32u] | runtime->keys_tapped[key / 32u];
    return ((word >> (key % 32u)) & 1u) != 0;
}

ray runtime_pointer_ray(const runtime_state *runtime, float ndc_x, float ndc_y) {
    float aspect = bridge_canvas_aspect(bridge_get_input(runtime->bridge));
    return camera_ray(&runtime->camera, aspect, ndc_x, ndc_y);
}

orbit_camera *runtime_get_camera(runtime_state *runtime) {
    return &runtime->camera;
}

uint32_t runtime_current_step(const runtime_state *runtime) {
    return runtime->step;
}

// ── Timing (for the stats panel) ───────────────────────────────────────────
//
// The panel shows the step in two parts: the app's own code (the runtime and
// your game), and Box3D's step, which runs on all the solver threads. Each has
// a breakdown, measured only while the panel has it open (runtime->profile,
// runtime->profile_physics). These are their entries in the bridge's timing
// table.
enum {
    // The step's own code, in the order it runs.
    PHASE_SCENE,
    PHASE_INPUT,
    PHASE_LOGIC,
    PHASE_RESULTS,
    PHASE_PUBLISH,
    // The stages of b3World_Step, and the parts of its solve stage, as
    // Box3D reports them (b3Profile).
    STAGE_PAIRS,
    STAGE_COLLIDE,
    STAGE_SOLVE,
    STAGE_SENSORS,
    SOLVE_SETUP,
    SOLVE_CONSTRAINTS,
    SOLVE_TRANSFORMS,
    SOLVE_REFIT,
    SOLVE_BULLETS,
    SOLVE_SLEEP,
    TIMING_ENTRIES,
};
static_assert(TIMING_ENTRIES <= BRIDGE_TIMING_APP_SLOTS, "more entries than the app's slots");

static void name_timings(bridge_header *bridge) {
    bridge_timing_name(bridge, PHASE_SCENE, BRIDGE_TIMING_APP_PHASE, "read scene");
    bridge_timing_name(bridge, PHASE_INPUT, BRIDGE_TIMING_APP_PHASE, "read input");
    bridge_timing_name(bridge, PHASE_LOGIC, BRIDGE_TIMING_APP_PHASE, "game logic");
    bridge_timing_name(bridge, PHASE_RESULTS, BRIDGE_TIMING_APP_PHASE, "poses + contact events");
    bridge_timing_name(bridge, PHASE_PUBLISH, BRIDGE_TIMING_APP_PHASE, "publish");
    bridge_timing_name(bridge, STAGE_PAIRS, BRIDGE_TIMING_PHYSICS_STAGE, "find pairs");
    bridge_timing_name(bridge, STAGE_COLLIDE, BRIDGE_TIMING_PHYSICS_STAGE, "collide");
    bridge_timing_name(bridge, STAGE_SOLVE, BRIDGE_TIMING_PHYSICS_STAGE, "solve");
    bridge_timing_name(bridge, STAGE_SENSORS, BRIDGE_TIMING_PHYSICS_STAGE, "sensors");
    bridge_timing_name(bridge, SOLVE_SETUP, BRIDGE_TIMING_PHYSICS_SOLVE, "set up");
    bridge_timing_name(bridge, SOLVE_CONSTRAINTS, BRIDGE_TIMING_PHYSICS_SOLVE, "constraints");
    bridge_timing_name(bridge, SOLVE_TRANSFORMS, BRIDGE_TIMING_PHYSICS_SOLVE, "update bodies");
    bridge_timing_name(bridge, SOLVE_REFIT, BRIDGE_TIMING_PHYSICS_SOLVE, "refit the tree");
    bridge_timing_name(bridge, SOLVE_BULLETS, BRIDGE_TIMING_PHYSICS_SOLVE, "fast bodies");
    bridge_timing_name(bridge, SOLVE_SLEEP, BRIDGE_TIMING_PHYSICS_SOLVE, "sleep");
}

// While profiling: charges the time since the last lap to `phase` and starts
// the next one. (Box3D's clock, since it is there on every platform.)
static void lap(runtime_state *runtime, uint32_t phase) {
    if (runtime->profile) {
        bridge_timing_add(runtime->bridge, phase,
                          (double)b3GetMillisecondsAndReset(&runtime->lap_ticks));
    }
}

// ── 3. The world: the game moves it, Box3D steps it ────────────────────────

// Steps the world, timing it for the panel's physics headline, and passes on
// Box3D's own account of the step when the panel asks for it.
static void step_world(runtime_state *runtime, float dt) {
    uint64_t start = b3GetTicks();
    b3World_Step(runtime->world, dt, GAME_SOLVER_SUB_STEPS);
    runtime->physics_ms += (double)b3GetMilliseconds(start);

    if (runtime->profile_physics) {
        b3Profile profile = b3World_GetProfile(runtime->world);
        bridge_header *bridge = runtime->bridge;
        bridge_timing_add(bridge, STAGE_PAIRS, (double)profile.pairs);
        bridge_timing_add(bridge, STAGE_COLLIDE, (double)profile.collide);
        bridge_timing_add(bridge, STAGE_SOLVE, (double)profile.solve);
        bridge_timing_add(bridge, STAGE_SENSORS, (double)profile.sensors);
        bridge_timing_add(bridge, SOLVE_SETUP, (double)profile.solverSetup);
        bridge_timing_add(bridge, SOLVE_CONSTRAINTS, (double)profile.constraints);
        bridge_timing_add(bridge, SOLVE_TRANSFORMS, (double)profile.transforms);
        bridge_timing_add(bridge, SOLVE_REFIT, (double)profile.refit);
        bridge_timing_add(bridge, SOLVE_BULLETS, (double)profile.bullets);
        bridge_timing_add(bridge, SOLVE_SLEEP, (double)profile.sleepIslands);
    }
}

// Copies every slot's pose out of Box3D, for the game and for publishing.
static void read_poses(runtime_state *runtime) {
    for (int i = 0; i < GAME_SLOT_COUNT; ++i) {
        runtime_body *slot = &runtime->bodies[i];
        if (!slot->active) {
            continue;
        }
        b3Vec3 p = b3Body_GetPosition(slot->body);
        b3Quat q = b3Body_GetRotation(slot->body);
        slot->position = vec3_make(p.x, p.y, p.z);
        slot->rotation = (quat){q.v.x, q.v.y, q.v.z, q.s};
    }
}

// Hands the game what Box3D reported about the step: touches beginning, then
// touches ending, then hits.
static void read_contact_events(runtime_state *runtime) {
    b3ContactEvents events = b3World_GetContactEvents(runtime->world);
    game_state *game = &runtime->game;
    for (int i = 0; i < events.beginCount; ++i) {
        const b3ContactBeginTouchEvent *ev = &events.beginEvents[i];
        game_contact(runtime, game, ev->shapeIdA, ev->shapeIdB, true);
    }
    for (int i = 0; i < events.endCount; ++i) {
        const b3ContactEndTouchEvent *ev = &events.endEvents[i];
        game_contact(runtime, game, ev->shapeIdA, ev->shapeIdB, false);
    }
    for (int i = 0; i < events.hitCount; ++i) {
        const b3ContactHitEvent *ev = &events.hitEvents[i];
        game_hit(runtime, game, ev->shapeIdA, ev->shapeIdB, ev->approachSpeed,
                 vec3_make(ev->point.x, ev->point.y, ev->point.z));
    }
}

static void simulate(runtime_state *runtime, float dt) {
    game_update(runtime, &runtime->game, dt);
    lap(runtime, PHASE_LOGIC);
    if (!runtime->scene_ready) {
        return; // the game asked for the scene to be reloaded
    }

    step_world(runtime, dt);
    runtime->lap_ticks = b3GetTicks(); // the step is the physics' time, not a phase

    read_poses(runtime);
    read_contact_events(runtime);
    game_after_physics(runtime, &runtime->game);
}

// ── 4. Publishing ──────────────────────────────────────────────────────────

static void publish_snapshot(runtime_state *runtime) {
    bridge_snapshot_header *snapshot = bridge_snapshot_begin(runtime->bridge, runtime->step);
    snapshot->camera = camera_to_bridge(&runtime->camera);
    // Frame-clock microseconds, wrapped; 0 is kept for "no time" (bridge.h).
    uint32_t time_us = (uint32_t)llround(runtime->step_time_ms * 1000.0);
    snapshot->time_us = time_us != 0 ? time_us : 1u;

    bridge_pose *poses = bridge_snapshot_poses(snapshot);
    uint32_t pose_count = 0;
    uint32_t active = 0;
    for (int i = 0; i < GAME_SLOT_COUNT; ++i) {
        runtime_body *slot = &runtime->bodies[i];
        uint32_t flags = 0;
        if (slot->active) {
            flags = (uint32_t)BRIDGE_POSE_ACTIVE;
            if (slot->teleported) {
                flags |= (uint32_t)BRIDGE_POSE_TELEPORT;
            }
            pose_count = (uint32_t)i + 1u;
            active += 1;
        }
        poses[i] = (bridge_pose){
            .position = {slot->position.x, slot->position.y, slot->position.z},
            .rotation = {slot->rotation.x, slot->rotation.y, slot->rotation.z, slot->rotation.w},
            .flags = flags,
        };
        slot->teleported = false;
    }
    snapshot->pose_count = pose_count;
    bridge_snapshot_publish(runtime->bridge, snapshot);

    atomic_store_explicit(&bridge_get_stats(runtime->bridge)->active_poses, active,
                          memory_order_relaxed);
}

// ── The step ───────────────────────────────────────────────────────────────

void runtime_init(runtime_state *runtime, bridge_header *bridge, uint32_t solver_threads) {
    // memset rather than assigning a compound literal: runtime_state is large
    // (the input queue alone is 32 KB) and a literal would be built on the
    // stack.
    memset(runtime, 0, sizeof *runtime);
    runtime->bridge = bridge;
    name_timings(bridge);
    runtime->world = physics_create_world(GAME_GRAVITY, GAME_HIT_MIN_SPEED, solver_threads);
    if (!b3World_IsValid(runtime->world)) {
        fprintf(stderr, "app: Box3D could not create the world; nothing will move\n");
    }
    game_init(runtime, &runtime->game);
}

void runtime_destroy(runtime_state *runtime) {
    if (b3World_IsValid(runtime->world)) {
        b3DestroyWorld(runtime->world);
    }
    runtime->world = b3_nullWorldId;
    runtime->scene_ready = false;
}

void runtime_step(runtime_state *runtime, float dt) {
    runtime->step += 1;
    if (runtime->profile) {
        runtime->lap_ticks = b3GetTicks();
    }

    read_scene(runtime);
    lap(runtime, PHASE_SCENE);
    read_input(runtime);
    lap(runtime, PHASE_INPUT);
    // Before the scene is known there is nothing to simulate, but the
    // snapshot still goes out: it carries the camera the renderer draws with.
    if (runtime->scene_ready) {
        simulate(runtime, dt);
    }
    lap(runtime, PHASE_RESULTS);
    publish_snapshot(runtime);
    // A tap counts for one step only.
    runtime->keys_tapped[0] = 0;
    runtime->keys_tapped[1] = 0;
    lap(runtime, PHASE_PUBLISH);
}
