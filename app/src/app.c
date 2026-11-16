// app.c — the demo: balls on a table, simulated by Box3D.
//
// Every fixed step (app_step) does four things, in order:
//
//   1. read what the renderer says about the scene  (renderer → app ring)
//   2. read what the user did                        (dom → app ring + held keys)
//   3. move the world                                (simulate: one Box3D step)
//   4. publish the result                            (snapshot + command/cue rings)
//
// The scene's collider nodes become the Box3D world: the table and its rails
// are static bodies, the authored ball is a dynamic sphere driven by the
// keyboard, and every click drops another ball. Box3D reports what touched
// what; those reports become the sound cues the page plays.

#include "app.h"

#include <stdio.h>

#include "physics.h"

// ── Tuning. Every rate is per second, so none of these depend on APP_STEP_HZ.

constexpr float GRAVITY = 9.81f; // m/s², downwards
// Box3D runs its solver this many times inside one b3World_Step. Its advice
// is 4 at 60 steps a second; we already step at 240, so 1 gives the same
// solver rate. Raise it only if contacts look soft (the cost is linear).
constexpr int SOLVER_SUB_STEPS = 1;

constexpr float DROP_HEIGHT = 2.0f; // balls appear this far above where they belong
constexpr float FALL_LIMIT = -2.0f; // a ball below this height fell off: put it back
// A click near the edge still lands on the felt: this far (plus the ball's
// radius) inside the table's outline, clear of the rails.
constexpr float DROP_MARGIN = 0.25f;

// How bouncy and grippy things are, and how quickly a ball loses speed.
constexpr float TABLE_RESTITUTION = 0.5f;
constexpr float TABLE_FRICTION = 0.9f;
constexpr float BALL_RESTITUTION = 0.6f;
constexpr float BALL_FRICTION = 0.9f;
constexpr float BALL_LINEAR_DAMPING = 0.1f;
constexpr float BALL_ANGULAR_DAMPING = 0.4f;

// The player's ball. Pushes are scaled by its mass, so it handles the same
// whatever its size.
constexpr float MOVE_ACCEL = 27.0f; // m/s² while a direction key is held
constexpr float MAX_SPEED = 6.0f;   // m/s, cap on horizontal speed from the keys
constexpr float JUMP_SPEED = 3.2f;  // m/s upwards when Space is pressed

// Touch: a swipe throws the player's ball.
constexpr float TAP_SLOP_PX = 12.0f;     // a touch that travels less is a tap
constexpr uint32_t FLING_WINDOW_US = 120'000; // speed is measured over the end of the swipe
constexpr float FLING_GAIN = 0.004f;     // swipe speed in px/s → ball speed in m/s
constexpr float FLING_MIN_SPEED = 0.25f; // slower swipes are ignored
constexpr float FLING_MAX_SPEED = 9.0f;  // faster ones are capped

// Sound cues.
constexpr float HIT_MIN_SPEED = 0.8f;   // m/s; slower contacts are not "hits" (Box3D enforces it)
constexpr float LAND_FULL_SPEED = 6.5f; // m/s; a landing this hard is at full volume
constexpr float MIN_IMPACT_INTENSITY = 0.12f;
constexpr float ROLL_SILENT_BELOW = 0.04f; // a roll slower than this (0..1) counts as still
// Rates in steps, derived from the per-second values.
constexpr uint32_t IMPACT_COOLDOWN_STEPS = (uint32_t)(0.13 * (double)APP_STEP_HZ + 0.5);
constexpr uint32_t ROLL_CUE_EVERY_STEPS = (uint32_t)((double)APP_STEP_HZ / 20.0 + 0.5);

constexpr float CAMERA_TARGET_LIFT = 0.4f; // the camera aims this far above the felt

// Fallbacks for a scene without a recognisable table or ball. They match the
// bundled scene, so the demo still behaves if a collider is renamed.
constexpr float DEFAULT_HALF_WIDTH = 3.0f;
constexpr float DEFAULT_HALF_DEPTH = 2.0f;
constexpr float DEFAULT_BALL_RADIUS = 0.3f;

// Scene names the app looks for. They cross the bridge as bridge_hash().
static const char *const BALL_NODE = "Ball";                   // top-level group of the ball
static const char *const PLAYER_VARIANT = "Ball_Player_Red";   // material variant for slot 0
static const char *const TABLE_COLLIDER = "Tabletop_Collider"; // the felt's collider box

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

// ── Talking to the other threads ─────────────────────────────────────────

static void send_command(app_state *app, uint32_t type, uint32_t slot, uint32_t node,
                         uint32_t variant) {
    bridge_record rec = {.type = type, .step = app->step};
    rec.u[0] = slot;
    rec.u[1] = node;
    rec.u[2] = variant;
    if (!bridge_ring_push(bridge_get_ring(app->bridge, BRIDGE_RING_APP_TO_RENDERER), &rec)) {
        fprintf(stderr, "app: command ring full, a command was dropped\n");
    }
}

// Tells the page something happened, so it can play a sound or show it. See
// APP_CUE_* in app.h for what `value` and `where` mean for each cue.
static void send_cue(app_state *app, uint32_t cue, float value, vec3 where) {
    bridge_record rec = {.type = BRIDGE_CUE, .step = app->step};
    rec.u[0] = cue;
    rec.f[1] = value;
    rec.f[2] = where.x;
    rec.f[3] = where.y;
    rec.f[4] = where.z;
    // If the ring is full the page is not draining it (a hidden tab, say);
    // losing a sound is fine, so the result is not checked.
    (void)bridge_ring_push(bridge_get_ring(app->bridge, BRIDGE_RING_APP_TO_DOM), &rec);
}

// ── Balls ────────────────────────────────────────────────────────────────

static app_ball *player(app_state *app) {
    return &app->balls[APP_PLAYER_SLOT];
}

// Creates the Box3D body of a ball and fills in its slot.
static void create_ball(app_state *app, int slot, vec3 position) {
    bool is_player = slot == APP_PLAYER_SLOT;

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
    b3BodyId body = b3CreateBody(app->world, &body_def);

    b3ShapeDef shape_def = b3DefaultShapeDef();
    shape_def.density = 1.0f;
    shape_def.baseMaterial.friction = BALL_FRICTION;
    shape_def.baseMaterial.restitution = BALL_RESTITUTION;
    shape_def.userData = role_to_user_data(ROLE_BALL);
    // Hit events (a contact faster than HIT_MIN_SPEED) make the impact
    // sounds, for every ball. Begin/end-touch events are only needed for the
    // player's ball, to know whether it is on the table. Box3D reports a
    // touch if EITHER shape asks for it, so asking here covers player-vs-table
    // and the table's shapes must not ask too (they would report every ball).
    shape_def.enableHitEvents = true;
    shape_def.enableContactEvents = is_player;
    b3Sphere sphere = {.center = {0.0f, 0.0f, 0.0f}, .radius = app->scene.ball_radius};
    b3ShapeId shape = b3CreateSphereShape(body, &shape_def, &sphere);

    app->balls[slot] = (app_ball){
        .active = true,
        .teleported = true,
        .body = body,
        .spawn = position,
        .position = position,
        .rotation = quat_identity(),
    };
    if (is_player) {
        app->player_shape = shape;
        app->ball_mass = b3Body_GetMass(body);
    }
}

// Puts a ball back where it first appeared, at rest.
static void put_back(app_state *app, app_ball *ball) {
    b3Body_SetTransform(ball->body, to_b3(ball->spawn), b3Quat_identity);
    b3Body_SetLinearVelocity(ball->body, b3Vec3_zero);
    b3Body_SetAngularVelocity(ball->body, b3Vec3_zero);
    ball->teleported = true;
    if (ball == player(app)) {
        app->touching_count = 0; // it is in mid-air now
    }
}

// R: clear every dropped ball and put the player's ball back at the start.
static void reset(app_state *app) {
    for (int slot = APP_PLAYER_SLOT + 1; slot < APP_MAX_BALLS; ++slot) {
        app_ball *ball = &app->balls[slot];
        if (ball->active) {
            b3DestroyBody(ball->body);
            ball->active = false;
            send_command(app, BRIDGE_CMD_DESPAWN, (uint32_t)slot, 0, 0);
        }
    }
    put_back(app, player(app));
}

// A click or tap: find where it lands on the table and drop a new ball there.
static void drop_ball_at(app_state *app, float ndc_x, float ndc_y) {
    const app_scene *s = &app->scene;

    float aspect = bridge_canvas_aspect(bridge_get_input(app->bridge));
    vec3 hit;
    if (!ray_hit_plane_y(camera_ray(&app->camera, aspect, ndc_x, ndc_y), s->table_top, &hit)) {
        return; // clicked the sky
    }

    int slot = -1;
    for (int i = APP_PLAYER_SLOT + 1; i < APP_MAX_BALLS; ++i) {
        if (!app->balls[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        printf("app: all %d balls are in play; press R to clear them\n", APP_MAX_BALLS);
        return;
    }

    // Clicks beside the table still land on the felt, inside the rails.
    float margin = s->ball_radius + DROP_MARGIN;
    vec3 position = {
        clampf(hit.x, s->min_x + margin, s->max_x - margin),
        s->ball_start.y + DROP_HEIGHT,
        clampf(hit.z, s->min_z + margin, s->max_z - margin),
    };
    create_ball(app, slot, position);
    // Variant 0: the copy keeps the ball's original material (not the red one).
    send_command(app, BRIDGE_CMD_SPAWN, (uint32_t)slot, bridge_hash(BALL_NODE), 0);
}

// ── Scene description (renderer → app) ───────────────────────────────────
//
// The renderer sends SCENE_BEGIN, one SCENE_COLLIDER per collider node, then
// SCENE_READY. Colliders are collected first and turned into the physics
// world on READY, since they can arrive in any order.

// Empties the world: the table and every ball. The world itself is kept.
static void destroy_bodies(app_state *app) {
    for (int i = 0; i < app->static_count; ++i) {
        b3DestroyBody(app->statics[i]);
    }
    app->static_count = 0;
    for (int i = 0; i < APP_MAX_BALLS; ++i) {
        if (app->balls[i].active) {
            b3DestroyBody(app->balls[i].body);
            app->balls[i].active = false;
        }
    }
    app->touching_count = 0;
    app->scene.ready = false;
}

static void scene_begin(app_state *app) {
    // The renderer is describing its scene from a clean baseline: anything we
    // bound or spawned before is gone on its side, so forget it here too.
    destroy_bodies(app);
    app->collider_count = 0;
}

static void scene_collider(app_state *app, const bridge_record *rec) {
    if (app->collider_count < APP_MAX_SCENE_COLLIDERS) {
        app->colliders[app->collider_count++] = *rec;
    } else {
        fprintf(stderr, "app: more than %d colliders in the scene; one was ignored\n",
                APP_MAX_SCENE_COLLIDERS);
    }
}

static vec3 collider_position(const bridge_record *rec) {
    return vec3_make(rec->f[6], rec->f[7], rec->f[8]);
}

static bool is_ball_collider(const bridge_record *rec) {
    return rec->u[2] == BRIDGE_SHAPE_SPHERE && rec->u[1] == bridge_hash(BALL_NODE);
}

// Which collected collider is the table top: the box with the expected name,
// or else the box with the largest footprint. -1 if there are no boxes.
static int find_table(const app_state *app) {
    uint32_t wanted = bridge_hash(TABLE_COLLIDER);
    int best = -1;
    float best_area = 0.0f;
    for (int i = 0; i < app->collider_count; ++i) {
        const bridge_record *rec = &app->colliders[i];
        if (rec->u[2] != BRIDGE_SHAPE_BOX) {
            continue;
        }
        if (rec->u[0] == wanted) {
            return i;
        }
        float area = rec->f[3] * rec->f[5];
        if (area > best_area) {
            best_area = area;
            best = i;
        }
    }
    return best;
}

// The table top's height and outline, from its collider box.
static void measure_table(app_scene *s, const bridge_record *table) {
    vec3 center = collider_position(table);
    quat rotation = {table->f[9], table->f[10], table->f[11], table->f[12]};
    // The box's world axis-aligned bounds: rotate each half-extent axis and
    // add up how far each reaches along x, y and z.
    vec3 ax = quat_rotate(rotation, vec3_make(table->f[3], 0.0f, 0.0f));
    vec3 ay = quat_rotate(rotation, vec3_make(0.0f, table->f[4], 0.0f));
    vec3 az = quat_rotate(rotation, vec3_make(0.0f, 0.0f, table->f[5]));
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

// One static body per collider, placed where the scene has it.
static void create_static(app_state *app, const bridge_record *rec, uint8_t role) {
    b3BodyDef body_def = b3DefaultBodyDef(); // static is the default type
    body_def.position = (b3Vec3){rec->f[6], rec->f[7], rec->f[8]};
    body_def.rotation = (b3Quat){.v = {rec->f[9], rec->f[10], rec->f[11]}, .s = rec->f[12]};
    b3BodyId body = b3CreateBody(app->world, &body_def);

    b3ShapeDef shape_def = b3DefaultShapeDef();
    shape_def.baseMaterial.friction = TABLE_FRICTION;
    shape_def.baseMaterial.restitution = TABLE_RESTITUTION;
    shape_def.userData = role_to_user_data(role);
    shape_def.enableContactEvents = false; // see create_ball()
    if (!physics_add_collider_shape(body, &shape_def, rec->u[2], &rec->f[3], nullptr)) {
        fprintf(stderr, "app: collider shape %u is not supported; skipped\n", rec->u[2]);
        b3DestroyBody(body);
        return;
    }
    app->statics[app->static_count++] = body;
}

static void scene_ready(app_state *app) {
    app_scene *s = &app->scene;
    // Normally a no-op, since SCENE_BEGIN already emptied the world. It makes
    // a READY that arrives without its BEGIN rebuild the world instead of
    // adding a second copy of everything to it.
    destroy_bodies(app);

    int table = find_table(app);
    if (table >= 0) {
        measure_table(s, &app->colliders[table]);
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
    const bridge_record *ball = nullptr;
    for (int i = 0; i < app->collider_count; ++i) {
        const bridge_record *rec = &app->colliders[i];
        if (is_ball_collider(rec)) {
            ball = rec;
        } else {
            create_static(app, rec, i == table ? ROLE_FLOOR : ROLE_WALL);
        }
    }

    if (ball != nullptr && ball->f[3] > 0.0f) {
        s->ball_radius = ball->f[3];
        s->ball_start = collider_position(ball);
    } else {
        fprintf(stderr, "app: no sphere collider under \"%s\"; using defaults\n", BALL_NODE);
        s->ball_radius = DEFAULT_BALL_RADIUS;
        s->ball_start = vec3_make((s->min_x + s->max_x) * 0.5f, s->table_top + DEFAULT_BALL_RADIUS,
                                  (s->min_z + s->max_z) * 0.5f);
    }

    app->camera.target = vec3_make((s->min_x + s->max_x) * 0.5f, s->table_top + CAMERA_TARGET_LIFT,
                                   (s->min_z + s->max_z) * 0.5f);
    send_cue(app, APP_CUE_CAMERA, app->camera.yaw, vec3_make(0.0f, 0.0f, 0.0f));
    app->camera_yaw_sent = app->camera.yaw;
    s->ready = true;

    // The authored ball becomes the player's: slot 0 drives it, in red. It
    // starts above its authored spot and drops in.
    send_command(app, BRIDGE_CMD_BIND, APP_PLAYER_SLOT, bridge_hash(BALL_NODE),
                 bridge_hash(PLAYER_VARIANT));
    create_ball(app, APP_PLAYER_SLOT, vec3_add(s->ball_start, vec3_make(0.0f, DROP_HEIGHT, 0.0f)));

    printf("app: scene ready: %d static bodies, table %.2f x %.2f m with its top at y=%.2f, "
           "ball radius %.2f m\n",
           app->static_count, (double)(s->max_x - s->min_x), (double)(s->max_z - s->min_z),
           (double)s->table_top, (double)s->ball_radius);
}

static void read_scene(app_state *app) {
    bridge_ring *ring = bridge_get_ring(app->bridge, BRIDGE_RING_RENDERER_TO_APP);
    bridge_record rec;
    while (bridge_ring_pop(ring, &rec)) {
        switch (rec.type) {
        case BRIDGE_SCENE_BEGIN:
            scene_begin(app);
            break;
        case BRIDGE_SCENE_COLLIDER:
            scene_collider(app, &rec);
            break;
        case BRIDGE_SCENE_READY:
            scene_ready(app);
            break;
        default:
            break; // a record type this app does not know: skip it
        }
    }
}

// ── User input (dom → app) ───────────────────────────────────────────────
//
// Two kinds: discrete EVENTS from the ring (a key went down, a button came
// up), handled here, and continuous STATE (which keys are held right now),
// read with bridge_key_held() in steer_player().
//
// The pointer does different things per device:
//   mouse   left click drops a ball; dragging with the right button orbits
//   touch   a tap drops a ball; a swipe throws the player's ball
//   wheel   zooms

// Turns a direction given relative to the view (x = to the right on screen,
// z = towards the viewer) into a world direction on the ground.
static vec3 view_to_ground(const app_state *app, float x, float z) {
    vec3 right = camera_ground_right(&app->camera);
    vec3 forward = camera_ground_forward(&app->camera);
    return vec3_sub(vec3_scale(right, x), vec3_scale(forward, z));
}

static void jump(app_state *app) {
    b3Body_ApplyLinearImpulseToCenter(player(app)->body,
                                      (b3Vec3){0.0f, JUMP_SPEED * app->ball_mass, 0.0f}, true);
}

static void swipe_add_sample(app_swipe *swipe, uint32_t time_us) {
    // Keep only the end of the swipe: drop samples older than the window,
    // but always keep one from before it to measure from.
    while (swipe->sample_count > 1 && time_us - swipe->samples[1].time_us > FLING_WINDOW_US) {
        swipe->sample_count -= 1;
        for (int i = 0; i < swipe->sample_count; ++i) {
            swipe->samples[i] = swipe->samples[i + 1];
        }
    }
    if (swipe->sample_count == APP_MAX_SWIPE_SAMPLES) {
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
static void fling(app_state *app, uint32_t time_us) {
    const app_swipe *swipe = &app->swipe;
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
    vec3 velocity = view_to_ground(app, vx, vz);
    if (speed > FLING_MAX_SPEED) {
        velocity = vec3_scale(velocity, FLING_MAX_SPEED / speed);
    }
    b3BodyId body = player(app)->body;
    velocity.y = b3Body_GetLinearVelocity(body).y;
    b3Body_SetLinearVelocity(body, to_b3(velocity));
}

static void pointer_down(app_state *app, const bridge_record *rec) {
    if (rec->u[3] == BRIDGE_POINTER_TOUCH) {
        app->swipe = (app_swipe){.active = true};
        swipe_add_sample(&app->swipe, rec->u[4]);
    } else if (rec->u[0] == 2) {
        app->orbiting = true;
    }
}

static void pointer_move(app_state *app, const bridge_record *rec) {
    if (app->swipe.active) {
        app->swipe.x += rec->f[0];
        app->swipe.y += rec->f[1];
        swipe_add_sample(&app->swipe, rec->u[6]);
    } else if (app->orbiting && (rec->u[4] & 2u) != 0) { // 2: the right button is held
        camera_orbit(&app->camera, rec->f[0], rec->f[1]);
    }
}

static void pointer_up(app_state *app, const bridge_record *rec) {
    uint32_t button = rec->u[0];
    bool was_swipe = app->swipe.active;
    app->swipe.active = false;
    // The browser reports one press and one release per pointer, however many
    // buttons were involved: the release is of whichever button came up last.
    // So any release ends the orbit, not only the right button's.
    app->orbiting = false;
    // A press the browser cancelled is released, and nothing else. Only the
    // left button (which is also what a finger reports) does anything.
    if (button != 0 || !app->scene.ready) {
        return;
    }
    if (was_swipe && hypotf(app->swipe.x, app->swipe.y) >= TAP_SLOP_PX) {
        fling(app, rec->u[4]);
    } else {
        drop_ball_at(app, rec->f[1], rec->f[2]);
    }
}

static void read_user_input(app_state *app) {
    bridge_ring *ring = bridge_get_ring(app->bridge, BRIDGE_RING_DOM_TO_APP);
    bridge_record rec;
    while (bridge_ring_pop(ring, &rec)) {
        switch (rec.type) {
        case BRIDGE_EV_KEY: {
            bool down = rec.u[1] != 0;
            if (!down || !app->scene.ready) {
                break;
            }
            if (rec.u[0] == BRIDGE_KEY_SPACE) {
                jump(app);
            } else if (rec.u[0] == BRIDGE_KEY_R) {
                reset(app);
            }
            break;
        }
        case BRIDGE_EV_POINTER_DOWN:
            pointer_down(app, &rec);
            break;
        case BRIDGE_EV_POINTER_MOVE:
            pointer_move(app, &rec);
            break;
        case BRIDGE_EV_POINTER_UP:
            pointer_up(app, &rec);
            break;
        case BRIDGE_EV_WHEEL:
            camera_zoom(&app->camera, rec.f[0]);
            break;
        default:
            break; // a record type this app does not know: skip it
        }
    }
}

// Held direction keys push the player's ball, relative to the view: "up" is
// always away from the camera, whatever angle it is orbiting at.
static void steer_player(app_state *app, float dt) {
    bridge_input *input = bridge_get_input(app->bridge);
    float x = 0.0f; // to the right on screen
    float z = 0.0f; // towards the viewer
    if (bridge_key_held(input, BRIDGE_KEY_W) || bridge_key_held(input, BRIDGE_KEY_UP)) {
        z -= 1.0f;
    }
    if (bridge_key_held(input, BRIDGE_KEY_S) || bridge_key_held(input, BRIDGE_KEY_DOWN)) {
        z += 1.0f;
    }
    if (bridge_key_held(input, BRIDGE_KEY_D) || bridge_key_held(input, BRIDGE_KEY_RIGHT)) {
        x += 1.0f;
    }
    if (bridge_key_held(input, BRIDGE_KEY_A) || bridge_key_held(input, BRIDGE_KEY_LEFT)) {
        x -= 1.0f;
    }
    if (x == 0.0f && z == 0.0f) {
        return;
    }

    // An impulse of mass × acceleration × dt adds the same speed per second
    // at any step rate and for any ball size.
    b3BodyId body = player(app)->body;
    vec3 push = vec3_scale(vec3_normalize(view_to_ground(app, x, z)),
                           MOVE_ACCEL * dt * app->ball_mass);
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

// ── What Box3D reports after a step ──────────────────────────────────────

// If `event` involves the player's ball, returns true and the other shape.
static bool touches_player(const app_state *app, b3ShapeId a, b3ShapeId b, b3ShapeId *other) {
    if (B3_ID_EQUALS(a, app->player_shape)) {
        *other = b;
        return true;
    }
    if (B3_ID_EQUALS(b, app->player_shape)) {
        *other = a;
        return true;
    }
    return false;
}

static void touching_add(app_state *app, b3ShapeId shape) {
    for (int i = 0; i < app->touching_count; ++i) {
        if (app->touching[i].shape_index == shape.index1) {
            return;
        }
    }
    if (app->touching_count < APP_MAX_TOUCHING) {
        app->touching[app->touching_count++] =
            (app_touch){.shape_index = shape.index1, .role = shape_role(shape)};
    }
}

static void touching_remove(app_state *app, b3ShapeId shape) {
    for (int i = 0; i < app->touching_count; ++i) {
        if (app->touching[i].shape_index == shape.index1) {
            app->touching[i] = app->touching[--app->touching_count];
            return;
        }
    }
}

// Resting on something other than a rail (the table, or another ball).
static bool player_grounded(const app_state *app) {
    for (int i = 0; i < app->touching_count; ++i) {
        if (app->touching[i].role != ROLE_WALL) {
            return true;
        }
    }
    return false;
}

// Sends an impact cue unless the same kind sounded a moment ago.
static void impact_cue(app_state *app, uint32_t cue, uint32_t *last_step, float speed,
                       float full_speed, b3Vec3 where) {
    if (app->step - *last_step < IMPACT_COOLDOWN_STEPS) {
        return;
    }
    *last_step = app->step;
    float intensity = clampf(speed / full_speed, MIN_IMPACT_INTENSITY, 1.0f);
    send_cue(app, cue, intensity, vec3_make(where.x, where.y, where.z));
}

static void read_contact_events(app_state *app) {
    b3ContactEvents events = b3World_GetContactEvents(app->world);

    // Begin/end touch, reported for the player's ball only: keeps the list
    // of what it is resting on.
    b3ShapeId other;
    for (int i = 0; i < events.beginCount; ++i) {
        const b3ContactBeginTouchEvent *ev = &events.beginEvents[i];
        if (touches_player(app, ev->shapeIdA, ev->shapeIdB, &other)) {
            touching_add(app, other);
        }
    }
    for (int i = 0; i < events.endCount; ++i) {
        const b3ContactEndTouchEvent *ev = &events.endEvents[i];
        if (touches_player(app, ev->shapeIdA, ev->shapeIdB, &other)) {
            touching_remove(app, other);
        }
    }

    // Hits, reported for every ball, with where and how fast: a rail knock, a
    // landing thud, or two balls clacking. Each kind has its own cooldown, so
    // a knock and a clack in the same instant both sound.
    for (int i = 0; i < events.hitCount; ++i) {
        const b3ContactHitEvent *ev = &events.hitEvents[i];
        uint8_t role_a = shape_role(ev->shapeIdA);
        uint8_t role_b = shape_role(ev->shapeIdB);
        if (role_a == ROLE_WALL || role_b == ROLE_WALL) {
            impact_cue(app, APP_CUE_WALL, &app->last_wall_cue_step, ev->approachSpeed, MAX_SPEED,
                       ev->point);
        } else if (role_a == ROLE_FLOOR || role_b == ROLE_FLOOR) {
            impact_cue(app, APP_CUE_FLOOR, &app->last_floor_cue_step, ev->approachSpeed,
                       LAND_FULL_SPEED, ev->point);
        } else {
            impact_cue(app, APP_CUE_BALL, &app->last_ball_cue_step, ev->approachSpeed, MAX_SPEED,
                       ev->point);
        }
    }
}

// The continuous cue behind the rolling sound: how fast the player's ball is
// moving across the table, 0..1. Silent in the air and when nearly still.
static void send_roll_cue(app_state *app) {
    const app_ball *ball = player(app);
    float speed = 0.0f;
    if (player_grounded(app)) {
        b3Vec3 v = b3Body_GetLinearVelocity(ball->body);
        speed = clampf(hypotf(v.x, v.z) / MAX_SPEED, 0.0f, 1.0f);
        if (speed < ROLL_SILENT_BELOW) {
            speed = 0.0f;
        }
    }
    send_cue(app, APP_CUE_ROLL, speed, ball->position);
}

// ── The step ─────────────────────────────────────────────────────────────

static void simulate(app_state *app, float dt) {
    // ── Your game logic goes here. ──────────────────────────────────────────
    // Everything the world needs is in `app`; read input with bridge_key_held()
    // or in read_user_input(), talk back with send_command() / send_cue().
    steer_player(app, dt);

    b3World_Step(app->world, dt, SOLVER_SUB_STEPS);

    // Copy the new poses out, and rescue any ball that got over a rail and
    // fell off the table.
    for (int i = 0; i < APP_MAX_BALLS; ++i) {
        app_ball *ball = &app->balls[i];
        if (!ball->active) {
            continue;
        }
        if (b3Body_GetPosition(ball->body).y < FALL_LIMIT) {
            put_back(app, ball);
        }
        b3Vec3 p = b3Body_GetPosition(ball->body);
        b3Quat q = b3Body_GetRotation(ball->body);
        ball->position = vec3_make(p.x, p.y, p.z);
        ball->rotation = (quat){q.v.x, q.v.y, q.v.z, q.s};
    }

    read_contact_events(app);
    if (app->step % ROLL_CUE_EVERY_STEPS == 0) {
        send_roll_cue(app);
    }
}

// ── Publishing ───────────────────────────────────────────────────────────

static void publish_snapshot(app_state *app) {
    bridge_snapshot_header *snapshot = bridge_snapshot_begin(app->bridge, app->step);
    snapshot->camera = camera_to_bridge(&app->camera);

    bridge_pose *poses = bridge_snapshot_poses(snapshot);
    uint32_t pose_count = 0;
    uint32_t active = 0;
    for (int i = 0; i < APP_MAX_BALLS; ++i) {
        app_ball *ball = &app->balls[i];
        uint32_t flags = 0;
        if (ball->active) {
            flags = (uint32_t)BRIDGE_POSE_ACTIVE;
            if (ball->teleported) {
                flags |= (uint32_t)BRIDGE_POSE_TELEPORT;
            }
            pose_count = (uint32_t)i + 1u;
            active += 1;
        }
        poses[i] = (bridge_pose){
            .position = {ball->position.x, ball->position.y, ball->position.z},
            .rotation = {ball->rotation.x, ball->rotation.y, ball->rotation.z, ball->rotation.w},
            .flags = flags,
        };
        ball->teleported = false;
    }
    snapshot->pose_count = pose_count;
    bridge_snapshot_publish(app->bridge, snapshot);

    atomic_store_explicit(&bridge_get_stats(app->bridge)->active_poses, active,
                          memory_order_relaxed);
}

// The page keeps the sound's left/right matching the view, so it needs to
// know when the camera turns.
static void send_camera_cue_if_turned(app_state *app) {
    if (app->scene.ready && app->camera.yaw != app->camera_yaw_sent) {
        send_cue(app, APP_CUE_CAMERA, app->camera.yaw, vec3_make(0.0f, 0.0f, 0.0f));
        app->camera_yaw_sent = app->camera.yaw;
    }
}

// ── Public API ───────────────────────────────────────────────────────────

void app_init(app_state *app, bridge_header *bridge, uint32_t solver_threads) {
    *app = (app_state){.bridge = bridge};
    app->camera = camera_default(vec3_make(0.0f, CAMERA_TARGET_LIFT, 0.0f));
    // One world for the app's whole life. A new scene description replaces
    // its bodies, not the world, which also keeps the solver threads (owned
    // by the world) running throughout.
    app->world = physics_create_world(GRAVITY, HIT_MIN_SPEED, solver_threads);
    if (!b3World_IsValid(app->world)) {
        fprintf(stderr, "app: Box3D could not create the world; nothing will move\n");
    }
}

void app_destroy(app_state *app) {
    if (b3World_IsValid(app->world)) {
        b3DestroyWorld(app->world);
    }
    app->world = b3_nullWorldId;
    app->scene.ready = false;
}

void app_step(app_state *app, float dt) {
    app->step += 1;

    read_scene(app);
    read_user_input(app);
    camera_step(&app->camera);
    // Before the scene is known there is nothing to simulate, but the
    // snapshot still goes out: it carries the camera the renderer draws with.
    if (app->scene.ready) {
        simulate(app, dt);
    }
    send_camera_cue_if_turned(app);
    publish_snapshot(app);
}

int app_active_balls(const app_state *app) {
    int count = 0;
    for (int i = 0; i < APP_MAX_BALLS; ++i) {
        count += app->balls[i].active ? 1 : 0;
    }
    return count;
}
