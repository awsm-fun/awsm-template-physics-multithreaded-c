// YOUR GAME: start here
//
// game.c — the demo: balls on a table, simulated by Box3D.
//
// The runtime calls the game_* hooks below (see game.h for the list); your
// game loop is game_update(). The scene's collider nodes become the Box3D
// world: the table and its rails are static bodies, the authored ball is a
// dynamic sphere driven by the keyboard, and every click drops another ball.
// Box3D reports what touched what (game_contact, game_hit); those reports
// become the sound cues the page plays.

#include "game/game.h"

#include <box3d/box3d.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "runtime/bridge.h"
#include "runtime/camera.h"
#include "runtime/math3d.h"
#include "runtime/runtime.h"

// Scene names the game looks for. They cross the bridge as bridge_hash().
static const char *const BALL_NODE = "Ball";                   // top-level group of the ball
static const char *const PLAYER_VARIANT = "Ball_Player_Red";   // material variant for slot 0
static const char *const TABLE_COLLIDER = "Tabletop_Collider"; // the felt's collider box

constexpr uint32_t ROLL_CUE_EVERY_STEPS =
    (uint32_t)((double)APP_STEP_HZ / (double)ROLL_CUES_PER_SECOND + 0.5);

// What a shape is, for deciding which sound a hit makes. Stored in each
// Box3D shape's userData.
enum {
    ROLE_FLOOR = 0, // the table top
    ROLE_WALL = 1,  // a rail
    ROLE_BALL = 2,
};

static void *role_to_user_data(uint8_t role) {
    return (void *)(uintptr_t)role;
}

static uint8_t shape_role(b3ShapeId shape) {
    return (uint8_t)(uintptr_t)b3Shape_GetUserData(shape);
}

static b3Vec3 to_b3(vec3 v) {
    return (b3Vec3){v.x, v.y, v.z};
}

// ── Balls ──────────────────────────────────────────────────────────────────

static b3BodyId player_body(const runtime_state *runtime) {
    return runtime_get_body(runtime, GAME_PLAYER_SLOT)->body;
}

// Creates the Box3D body of a ball, for slot `slot`.
static b3BodyId create_ball(runtime_state *runtime, game_state *game, uint32_t slot,
                            vec3 position) {
    bool is_player = slot == GAME_PLAYER_SLOT;

    b3BodyDef body_def = b3DefaultBodyDef();
    body_def.type = b3_dynamicBody;
    body_def.position = to_b3(position);
    body_def.linearDamping = BALL_LINEAR_DAMPING;
    body_def.angularDamping = BALL_ANGULAR_DAMPING;
    if (is_player) {
        // The player's ball is always awake, and uses continuous collision so
        // it cannot tunnel through the thin rails at speed. Dropped balls are
        // cheaper: they may fall asleep once they settle (a resting pile then
        // costs next to nothing) and skip the continuous check.
        body_def.enableSleep = false;
        body_def.isBullet = true;
    }
    b3BodyId body = b3CreateBody(runtime_world(runtime), &body_def);

    b3ShapeDef shape_def = b3DefaultShapeDef();
    shape_def.density = 1.0f;
    shape_def.baseMaterial.friction = BALL_FRICTION;
    shape_def.baseMaterial.restitution = BALL_RESTITUTION;
    shape_def.userData = role_to_user_data(ROLE_BALL);
    // Hit events (a contact faster than GAME_HIT_MIN_SPEED) make the impact
    // sounds, for every ball. Begin/end-touch events are only needed for the
    // player's ball, to know whether it is on the table. Box3D reports a
    // touch if EITHER shape asks for it, so asking here covers player-vs-table
    // and the table's shapes must not ask too (they would report every ball).
    shape_def.enableHitEvents = true;
    shape_def.enableContactEvents = is_player;
    b3Sphere sphere = {.center = {0.0f, 0.0f, 0.0f}, .radius = game->scene.ball_radius};
    b3ShapeId shape = b3CreateSphereShape(body, &shape_def, &sphere);

    game->spawn[slot] = position;
    if (is_player) {
        game->player_shape = shape;
        game->ball_mass = b3Body_GetMass(body);
    }
    return body;
}

// Puts a ball back where it first appeared, at rest.
static void put_back(runtime_state *runtime, game_state *game, uint32_t slot) {
    runtime_teleport_body(runtime, slot, game->spawn[slot], quat_identity());
    if (slot == GAME_PLAYER_SLOT) {
        game->touching_count = 0; // it is in mid-air now
    }
}

// Enter: clear every dropped ball and put the player's ball back at the
// start. (R goes further and reloads the whole scene: runtime_reload_scene.)
static void reset(runtime_state *runtime, game_state *game) {
    for (uint32_t slot = GAME_FIRST_DROP_SLOT; slot < GAME_SLOT_COUNT; ++slot) {
        runtime_destroy_body(runtime, slot);
    }
    put_back(runtime, game, GAME_PLAYER_SLOT);
}

// A click or tap: find where it lands on the table and drop a new ball there.
static void drop_ball_at(runtime_state *runtime, game_state *game, float ndc_x, float ndc_y) {
    const game_scene *s = &game->scene;

    vec3 hit;
    if (!ray_hit_plane_y(runtime_pointer_ray(runtime, ndc_x, ndc_y), s->table_top, &hit)) {
        return; // clicked the sky
    }

    uint32_t slot = GAME_SLOT_COUNT;
    for (uint32_t i = GAME_FIRST_DROP_SLOT; i < GAME_SLOT_COUNT; ++i) {
        if (!runtime_get_body(runtime, i)->active) {
            slot = i;
            break;
        }
    }
    if (slot == GAME_SLOT_COUNT) {
        printf("app: all %d balls are in play; press Enter to clear them\n", GAME_SLOT_COUNT);
        return;
    }

    // Clicks beside the table still land on the felt, inside the rails.
    float margin = s->ball_radius + DROP_MARGIN;
    vec3 position = {
        clampf(hit.x, s->min_x + margin, s->max_x - margin),
        s->ball_start.y + DROP_HEIGHT,
        clampf(hit.z, s->min_z + margin, s->max_z - margin),
    };
    // No variant: the copy keeps the ball's original material (not the red one).
    runtime_spawn_body(runtime, slot, create_ball(runtime, game, slot, position), BALL_NODE,
                       nullptr);
}

// ── The camera ─────────────────────────────────────────────────────────────

void game_init(runtime_state *runtime, game_state *game) {
    (void)game; // starts zeroed, which is all the demo needs
    *runtime_get_camera(runtime) = camera_default(vec3_make(0.0f, CAMERA_TARGET_LIFT, 0.0f));
}

// The page keeps the sound's left/right matching the view, so it needs to
// know when the camera turns.
static void send_camera_cue(runtime_state *runtime, game_state *game) {
    float yaw = runtime_get_camera(runtime)->yaw;
    runtime_send_cue(runtime, GAME_CUE_CAMERA, yaw, vec3_make(0.0f, 0.0f, 0.0f));
    game->camera_yaw_sent = yaw;
}

// ── The scene ──────────────────────────────────────────────────────────────

void game_scene_begin(runtime_state *runtime, game_state *game) {
    game->touching_count = 0;
    game->orbiting = false;
    game->swipe.active = false;
    *runtime_get_camera(runtime) = camera_default(vec3_make(0.0f, CAMERA_TARGET_LIFT, 0.0f));
}

static bool is_ball_collider(const runtime_collider *c) {
    return c->shape == BRIDGE_SHAPE_SPHERE && c->top_level == bridge_hash(BALL_NODE);
}

// Which collider is the table top: the box with the expected name, or else the
// box with the largest footprint. -1 if there are no boxes.
static int find_table(const runtime_state *runtime) {
    uint32_t wanted = bridge_hash(TABLE_COLLIDER);
    int best = -1;
    float best_area = 0.0f;
    for (int i = 0; i < runtime_collider_count(runtime); ++i) {
        const runtime_collider *c = runtime_get_collider(runtime, i);
        if (c->shape != BRIDGE_SHAPE_BOX) {
            continue;
        }
        if (c->name == wanted) {
            return i;
        }
        float area = c->dims[0] * c->dims[2];
        if (area > best_area) {
            best_area = area;
            best = i;
        }
    }
    return best;
}

// The table top's height and outline, from its collider box.
static void measure_table(game_scene *s, const runtime_collider *table) {
    vec3 center = table->position;
    // The box's world axis-aligned bounds: rotate each half-extent axis and
    // add up how far each reaches along x, y and z.
    vec3 ax = quat_rotate(table->rotation, vec3_make(table->dims[0], 0.0f, 0.0f));
    vec3 ay = quat_rotate(table->rotation, vec3_make(0.0f, table->dims[1], 0.0f));
    vec3 az = quat_rotate(table->rotation, vec3_make(0.0f, 0.0f, table->dims[2]));
    vec3 half = {
        fabsf(ax.x) + fabsf(ay.x) + fabsf(az.x),
        fabsf(ax.y) + fabsf(ay.y) + fabsf(az.y),
        fabsf(ax.z) + fabsf(ay.z) + fabsf(az.z),
    };
    s->table_top = center.y + half.y;
    s->min_x = center.x - half.x;
    s->max_x = center.x + half.x;
    s->min_z = center.z - half.z;
    s->max_z = center.z + half.z;
}

// One static body per collider, placed where the scene has it. Returns
// whether Box3D could make its shape.
static bool create_static(runtime_state *runtime, const runtime_collider *c, uint8_t role) {
    b3ShapeDef shape_def = b3DefaultShapeDef();
    shape_def.baseMaterial.friction = TABLE_FRICTION;
    shape_def.baseMaterial.restitution = TABLE_RESTITUTION;
    shape_def.userData = role_to_user_data(role);
    shape_def.enableContactEvents = false; // see create_ball()
    return B3_IS_NON_NULL(runtime_create_collider_body(runtime, c, &shape_def));
}

void game_scene_ready(runtime_state *runtime, game_state *game) {
    game_scene *s = &game->scene;
    game->touching_count = 0;

    int table = find_table(runtime);
    if (table >= 0) {
        measure_table(s, runtime_get_collider(runtime, table));
    } else {
        fprintf(stderr, "app: the scene has no box collider for the table; using defaults\n");
        s->table_top = 0.0f;
        s->min_x = -DEFAULT_HALF_WIDTH;
        s->max_x = DEFAULT_HALF_WIDTH;
        s->min_z = -DEFAULT_HALF_DEPTH;
        s->max_z = DEFAULT_HALF_DEPTH;
    }

    // Everything that is not the ball is fixed geometry: the table top is the
    // floor, anything else is a rail.
    const runtime_collider *ball = nullptr;
    int statics = 0;
    for (int i = 0; i < runtime_collider_count(runtime); ++i) {
        const runtime_collider *c = runtime_get_collider(runtime, i);
        if (is_ball_collider(c)) {
            ball = c;
        } else {
            statics += create_static(runtime, c, i == table ? ROLE_FLOOR : ROLE_WALL) ? 1 : 0;
        }
    }

    if (ball != nullptr && ball->dims[0] > 0.0f) {
        s->ball_radius = ball->dims[0];
        s->ball_start = ball->position;
    } else {
        fprintf(stderr, "app: no sphere collider under \"%s\"; using defaults\n", BALL_NODE);
        s->ball_radius = DEFAULT_BALL_RADIUS;
        s->ball_start = vec3_make((s->min_x + s->max_x) * 0.5f, s->table_top + DEFAULT_BALL_RADIUS,
                                  (s->min_z + s->max_z) * 0.5f);
    }

    runtime_get_camera(runtime)->target =
        vec3_make((s->min_x + s->max_x) * 0.5f, s->table_top + CAMERA_TARGET_LIFT,
                  (s->min_z + s->max_z) * 0.5f);
    send_camera_cue(runtime, game);

    // The authored ball becomes the player's: slot 0 drives it, in red. It
    // starts above its authored spot and drops in.
    vec3 start = vec3_add(s->ball_start, vec3_make(0.0f, DROP_HEIGHT, 0.0f));
    runtime_bind_body(runtime, GAME_PLAYER_SLOT,
                      create_ball(runtime, game, GAME_PLAYER_SLOT, start), BALL_NODE,
                      PLAYER_VARIANT);

    printf("app: scene ready: %d static bodies, table %.2f x %.2f m with its top at y=%.2f, "
           "ball radius %.2f m\n",
           statics, (double)(s->max_x - s->min_x), (double)(s->max_z - s->min_z),
           (double)s->table_top, (double)s->ball_radius);
}

// ── Input ──────────────────────────────────────────────────────────────────
//
// The pointer does different things per device:
//   mouse   left click drops a ball; dragging with the right button orbits
//   touch   a tap drops a ball; a swipe throws the player's ball
//   wheel   zooms

// Turns a direction given relative to the view (x = to the right on screen,
// z = towards the viewer) into a world direction on the ground.
static vec3 view_to_ground(runtime_state *runtime, float x, float z) {
    const orbit_camera *camera = runtime_get_camera(runtime);
    vec3 right = camera_ground_right(camera);
    vec3 forward = camera_ground_forward(camera);
    return vec3_sub(vec3_scale(right, x), vec3_scale(forward, z));
}

static void jump(runtime_state *runtime, const game_state *game) {
    b3Body_ApplyLinearImpulseToCenter(player_body(runtime),
                                      (b3Vec3){0.0f, JUMP_SPEED * game->ball_mass, 0.0f}, true);
}

static void swipe_add_sample(game_swipe *swipe, uint32_t time_us) {
    // Keep only the end of the swipe: drop samples older than the window,
    // but always keep one from before it to measure from.
    while (swipe->sample_count > 1 && time_us - swipe->samples[1].time_us > FLING_WINDOW_US) {
        swipe->sample_count -= 1;
        for (int i = 0; i < swipe->sample_count; ++i) {
            swipe->samples[i] = swipe->samples[i + 1];
        }
    }
    if (swipe->sample_count == GAME_MAX_SWIPE_SAMPLES) {
        return; // more samples than any real device sends; the anchor is kept
    }
    swipe->samples[swipe->sample_count].time_us = time_us;
    swipe->samples[swipe->sample_count].x = swipe->x;
    swipe->samples[swipe->sample_count].y = swipe->y;
    swipe->sample_count += 1;
}

// The finger lifted after travelling: if the end of the swipe was fast
// enough, the player's ball takes off in that direction at that speed. Its
// horizontal velocity is replaced, not added to: a throw should go where the
// finger went. Vertical velocity (gravity, a jump) is left alone.
static void fling(runtime_state *runtime, const game_state *game, uint32_t time_us) {
    const game_swipe *swipe = &game->swipe;
    // The oldest sample still inside the window, or failing that the anchor.
    int from = 0;
    for (int i = 0; i < swipe->sample_count; ++i) {
        if (time_us - swipe->samples[i].time_us <= FLING_WINDOW_US) {
            from = i;
            break;
        }
    }
    float seconds = (float)(time_us - swipe->samples[from].time_us) / 1e6f;
    if (seconds <= 0.0f) {
        return;
    }
    // Screen right is view x; screen down is towards the viewer, view z.
    float vx = (swipe->x - swipe->samples[from].x) / seconds * FLING_GAIN;
    float vz = (swipe->y - swipe->samples[from].y) / seconds * FLING_GAIN;
    float speed = hypotf(vx, vz);
    if (speed < FLING_MIN_SPEED) {
        return; // a slow drag is not a throw
    }
    vec3 velocity = view_to_ground(runtime, vx, vz);
    if (speed > FLING_MAX_SPEED) {
        velocity = vec3_scale(velocity, FLING_MAX_SPEED / speed);
    }
    b3BodyId body = player_body(runtime);
    velocity.y = b3Body_GetLinearVelocity(body).y;
    b3Body_SetLinearVelocity(body, to_b3(velocity));
}

static void pointer_down(game_state *game, const runtime_pointer *p) {
    if (p->device == BRIDGE_POINTER_TOUCH) {
        game->swipe = (game_swipe){.active = true};
        swipe_add_sample(&game->swipe, p->time_us);
    } else if (p->button == 2) {
        game->orbiting = true;
    }
}

static void pointer_move(runtime_state *runtime, game_state *game, const runtime_pointer *p) {
    if (game->swipe.active) {
        game->swipe.x += p->dx;
        game->swipe.y += p->dy;
        swipe_add_sample(&game->swipe, p->time_us);
    } else if (game->orbiting && (p->buttons & 2u) != 0) { // 2: the right button is held
        camera_orbit(runtime_get_camera(runtime), p->dx, p->dy);
    }
}

static void pointer_up(runtime_state *runtime, game_state *game, const runtime_pointer *p) {
    bool was_swipe = game->swipe.active;
    game->swipe.active = false;
    // The browser reports one press and one release per pointer, however many
    // buttons were involved: the release is of whichever button came up last.
    // So any release ends the orbit, not only the right button's.
    game->orbiting = false;
    // A press the browser cancelled is released, and nothing else. Only the
    // left button (which is also what a finger reports) does anything.
    if (p->button != 0 || !runtime_scene_ready(runtime)) {
        return;
    }
    if (was_swipe && hypotf(game->swipe.x, game->swipe.y) >= TAP_SLOP_PX) {
        fling(runtime, game, p->time_us);
    } else {
        drop_ball_at(runtime, game, p->ndc_x, p->ndc_y);
    }
}

void game_pointer(runtime_state *runtime, game_state *game, const runtime_pointer *pointer) {
    switch (pointer->kind) {
    case RUNTIME_POINTER_DOWN:
        pointer_down(game, pointer);
        break;
    case RUNTIME_POINTER_MOVE:
        pointer_move(runtime, game, pointer);
        break;
    case RUNTIME_POINTER_UP:
        pointer_up(runtime, game, pointer);
        break;
    case RUNTIME_POINTER_WHEEL:
        camera_zoom(runtime_get_camera(runtime), pointer->dy);
        break;
    }
}

void game_key(runtime_state *runtime, game_state *game, uint32_t key, bool down) {
    // One-off actions go here; keys that act for as long as they are held
    // are polled with runtime_key_held() in game_update().
    if (!down) {
        return;
    }
    if (key == BRIDGE_KEY_SPACE) {
        jump(runtime, game);
    } else if (key == BRIDGE_KEY_ENTER) {
        reset(runtime, game);
    } else if (key == BRIDGE_KEY_R) {
        runtime_reload_scene(runtime);
    }
}

// ── The step ───────────────────────────────────────────────────────────────

// Held direction keys push the player's ball, relative to the view: "up" is
// always away from the camera, whatever angle it is orbiting at.
static void steer_player(runtime_state *runtime, const game_state *game, float dt) {
    float x = 0.0f; // to the right on screen
    float z = 0.0f; // towards the viewer
    if (runtime_key_held(runtime, BRIDGE_KEY_W) || runtime_key_held(runtime, BRIDGE_KEY_UP)) {
        z -= 1.0f;
    }
    if (runtime_key_held(runtime, BRIDGE_KEY_S) || runtime_key_held(runtime, BRIDGE_KEY_DOWN)) {
        z += 1.0f;
    }
    if (runtime_key_held(runtime, BRIDGE_KEY_D) || runtime_key_held(runtime, BRIDGE_KEY_RIGHT)) {
        x += 1.0f;
    }
    if (runtime_key_held(runtime, BRIDGE_KEY_A) || runtime_key_held(runtime, BRIDGE_KEY_LEFT)) {
        x -= 1.0f;
    }
    if (x == 0.0f && z == 0.0f) {
        return;
    }

    // An impulse of mass × acceleration × dt adds the same speed per second
    // at any step rate and for any ball size.
    b3BodyId body = player_body(runtime);
    vec3 push = vec3_scale(vec3_normalize(view_to_ground(runtime, x, z)),
                           MOVE_ACCEL * dt * game->ball_mass);
    b3Body_ApplyLinearImpulseToCenter(body, to_b3(push), true);

    // Holding a key keeps adding speed; cap the horizontal part. Vertical
    // speed (gravity, a jump) is left alone.
    b3Vec3 v = b3Body_GetLinearVelocity(body);
    float speed = hypotf(v.x, v.z);
    if (speed > MAX_SPEED) {
        float k = MAX_SPEED / speed;
        b3Body_SetLinearVelocity(body, (b3Vec3){v.x * k, v.y, v.z * k});
    }
}

void game_update(runtime_state *runtime, game_state *game, float dt) {
    // ── Your game logic goes here. ─────────────────────────────────────────
    steer_player(runtime, game, dt);
}

// ── What Box3D reports after a step ────────────────────────────────────────

static void touching_add(game_state *game, b3ShapeId shape) {
    for (int i = 0; i < game->touching_count; ++i) {
        if (game->touching[i].shape_index == shape.index1) {
            return;
        }
    }
    if (game->touching_count < GAME_MAX_TOUCHING) {
        game->touching[game->touching_count++] =
            (game_touch){.shape_index = shape.index1, .role = shape_role(shape)};
    }
}

static void touching_remove(game_state *game, b3ShapeId shape) {
    for (int i = 0; i < game->touching_count; ++i) {
        if (game->touching[i].shape_index == shape.index1) {
            game->touching[i] = game->touching[--game->touching_count];
            return;
        }
    }
}

// Begin/end touch, reported for the player's ball only (see create_ball):
// keeps the list of what it is resting on.
void game_contact(runtime_state *runtime, game_state *game, b3ShapeId a, b3ShapeId b,
                  bool touching) {
    (void)runtime;
    b3ShapeId other;
    if (B3_ID_EQUALS(a, game->player_shape)) {
        other = b;
    } else if (B3_ID_EQUALS(b, game->player_shape)) {
        other = a;
    } else {
        return;
    }
    if (touching) {
        touching_add(game, other);
    } else {
        touching_remove(game, other);
    }
}

// Sends an impact cue unless the same kind sounded a moment ago.
static void impact_cue(runtime_state *runtime, uint32_t cue, float speed, float full_speed,
                       vec3 where) {
    float intensity = clampf(speed / full_speed, MIN_IMPACT_INTENSITY, 1.0f);
    (void)runtime_send_cue_throttled(runtime, cue, intensity, where, IMPACT_COOLDOWN_S);
}

// Hits, reported for every ball, with where and how fast: a rail knock, a
// landing thud, or two balls clacking. Each kind has its own cooldown, so a
// knock and a clack in the same instant both sound.
void game_hit(runtime_state *runtime, game_state *game, b3ShapeId a, b3ShapeId b, float speed,
              vec3 where) {
    (void)game;
    uint8_t role_a = shape_role(a);
    uint8_t role_b = shape_role(b);
    if (role_a == ROLE_WALL || role_b == ROLE_WALL) {
        impact_cue(runtime, GAME_CUE_WALL, speed, MAX_SPEED, where);
    } else if (role_a == ROLE_FLOOR || role_b == ROLE_FLOOR) {
        impact_cue(runtime, GAME_CUE_FLOOR, speed, LAND_FULL_SPEED, where);
    } else {
        impact_cue(runtime, GAME_CUE_BALL, speed, MAX_SPEED, where);
    }
}

// Resting on something other than a rail (the table, or another ball).
static bool player_grounded(const game_state *game) {
    for (int i = 0; i < game->touching_count; ++i) {
        if (game->touching[i].role != ROLE_WALL) {
            return true;
        }
    }
    return false;
}

// The continuous cue behind the rolling sound: how fast the player's ball is
// moving across the table, 0..1. Silent in the air and when nearly still.
static void send_roll_cue(runtime_state *runtime, const game_state *game) {
    const runtime_body *ball = runtime_get_body(runtime, GAME_PLAYER_SLOT);
    float speed = 0.0f;
    if (player_grounded(game)) {
        b3Vec3 v = b3Body_GetLinearVelocity(ball->body);
        speed = clampf(hypotf(v.x, v.z) / MAX_SPEED, 0.0f, 1.0f);
        if (speed < ROLL_SILENT_BELOW) {
            speed = 0.0f;
        }
    }
    runtime_send_cue(runtime, GAME_CUE_ROLL, speed, ball->position);
}

void game_after_physics(runtime_state *runtime, game_state *game) {
    // Rescue any ball that got over a rail and fell off the table.
    for (uint32_t slot = 0; slot < GAME_SLOT_COUNT; ++slot) {
        const runtime_body *ball = runtime_get_body(runtime, slot);
        if (ball->active && ball->position.y < FALL_LIMIT) {
            put_back(runtime, game, slot);
        }
    }
    if (runtime_current_step(runtime) % ROLL_CUE_EVERY_STEPS == 0) {
        send_roll_cue(runtime, game);
    }
    if (runtime_get_camera(runtime)->yaw != game->camera_yaw_sent) {
        send_camera_cue(runtime, game);
    }
}
