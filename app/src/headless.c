// headless.c — see headless.h.

#include "headless.h"

static void push(bridge_header *bridge, int ring, const bridge_record *rec) {
    (void)bridge_ring_push(bridge_get_ring(bridge, ring), rec);
}

// One SCENE_COLLIDER, unrotated, as the renderer would report it.
static void send_collider(bridge_header *bridge, const char *name, const char *top_level,
                          uint32_t shape, const float dims[3], const float position[3]) {
    bridge_record rec = {.type = BRIDGE_SCENE_COLLIDER};
    rec.u[0] = bridge_hash(name);
    rec.u[1] = bridge_hash(top_level);
    rec.u[2] = shape;
    for (int i = 0; i < 3; ++i) {
        rec.f[3 + i] = dims[i];
        rec.f[6 + i] = position[i];
    }
    rec.f[9] = 0.0f; // identity rotation
    rec.f[10] = 0.0f;
    rec.f[11] = 0.0f;
    rec.f[12] = 1.0f;
    push(bridge, BRIDGE_RING_RENDERER_TO_APP, &rec);
}

void headless_send_scene(bridge_header *bridge) {
    headless_send_scene_as(bridge, false);
}

void headless_send_scene_as(bridge_header *bridge, bool used) {
    bridge_record begin = {.type = BRIDGE_SCENE_BEGIN};
    begin.u[0] = used ? 1u : 0u;
    push(bridge, BRIDGE_RING_RENDERER_TO_APP, &begin);

    // The colliders of media/bundle/scene.toml, in world space.
    send_collider(bridge, "Tabletop_Collider", "Table", BRIDGE_SHAPE_BOX,
                  (const float[3]){3.0f, 0.1f, 2.0f}, (const float[3]){0.0f, -0.1f, 0.0f});
    send_collider(bridge, "Wall_South_Collider", "Table", BRIDGE_SHAPE_BOX,
                  (const float[3]){3.0f, 0.2f, 0.05f}, (const float[3]){0.0f, 0.2f, 1.95f});
    send_collider(bridge, "Wall_North_Collider", "Table", BRIDGE_SHAPE_BOX,
                  (const float[3]){3.0f, 0.2f, 0.05f}, (const float[3]){0.0f, 0.2f, -1.95f});
    send_collider(bridge, "Wall_East_Collider", "Table", BRIDGE_SHAPE_BOX,
                  (const float[3]){0.05f, 0.2f, 2.0f}, (const float[3]){2.95f, 0.2f, 0.0f});
    send_collider(bridge, "Wall_West_Collider", "Table", BRIDGE_SHAPE_BOX,
                  (const float[3]){0.05f, 0.2f, 2.0f}, (const float[3]){-2.95f, 0.2f, 0.0f});
    send_collider(bridge, "Ball_Collider", "Ball", BRIDGE_SHAPE_SPHERE,
                  (const float[3]){0.3f, 0.0f, 0.0f}, (const float[3]){0.0f, 0.3f, 0.0f});

    bridge_record ready = {.type = BRIDGE_SCENE_READY};
    ready.u[0] = 6; // colliders sent
    push(bridge, BRIDGE_RING_RENDERER_TO_APP, &ready);
}

void headless_set_canvas(bridge_header *bridge, uint32_t width, uint32_t height) {
    bridge_input *input = bridge_get_input(bridge);
    atomic_store(&input->canvas_width, width);
    atomic_store(&input->canvas_height, height);
}

void headless_key(bridge_header *bridge, uint32_t key, bool down) {
    bridge_input *input = bridge_get_input(bridge);
    uint32_t bit = 1u << (key % 32u);
    if (down) {
        atomic_fetch_or(&input->keys[key / 32u], bit);
    } else {
        atomic_fetch_and(&input->keys[key / 32u], ~bit);
    }
    bridge_record rec = {.type = BRIDGE_EV_KEY};
    rec.u[0] = key;
    rec.u[1] = down ? 1u : 0u;
    push(bridge, BRIDGE_RING_DOM_TO_APP, &rec);
}

void headless_click(bridge_header *bridge, float ndc_x, float ndc_y) {
    bridge_record down = {.type = BRIDGE_EV_POINTER_DOWN};
    down.u[0] = 0; // left button
    down.f[1] = ndc_x;
    down.f[2] = ndc_y;
    push(bridge, BRIDGE_RING_DOM_TO_APP, &down);

    bridge_record up = down;
    up.type = BRIDGE_EV_POINTER_UP;
    push(bridge, BRIDGE_RING_DOM_TO_APP, &up);
}

void headless_right_drag(bridge_header *bridge, float dx_px, float dy_px) {
    bridge_record down = {.type = BRIDGE_EV_POINTER_DOWN};
    down.u[0] = 2; // right button
    push(bridge, BRIDGE_RING_DOM_TO_APP, &down);

    bridge_record move = {.type = BRIDGE_EV_POINTER_MOVE};
    move.f[0] = dx_px;
    move.f[1] = dy_px;
    move.u[4] = 2; // buttons bitmask: right
    push(bridge, BRIDGE_RING_DOM_TO_APP, &move);

    bridge_record up = down;
    up.type = BRIDGE_EV_POINTER_UP;
    push(bridge, BRIDGE_RING_DOM_TO_APP, &up);
}

void headless_swipe(bridge_header *bridge, float dx_px, float dy_px, uint32_t duration_ms) {
    constexpr uint32_t MOVES = 10;
    constexpr uint32_t START_US = 5'000'000; // any time: only differences matter

    bridge_record down = {.type = BRIDGE_EV_POINTER_DOWN};
    down.u[3] = BRIDGE_POINTER_TOUCH;
    down.u[4] = START_US;
    push(bridge, BRIDGE_RING_DOM_TO_APP, &down);

    for (uint32_t i = 1; i <= MOVES; ++i) {
        bridge_record move = {.type = BRIDGE_EV_POINTER_MOVE};
        move.f[0] = dx_px / (float)MOVES;
        move.f[1] = dy_px / (float)MOVES;
        move.u[4] = 1;
        move.u[5] = BRIDGE_POINTER_TOUCH;
        move.u[6] = START_US + duration_ms * 1000u * i / MOVES;
        push(bridge, BRIDGE_RING_DOM_TO_APP, &move);
    }

    bridge_record up = down;
    up.type = BRIDGE_EV_POINTER_UP;
    up.u[4] = START_US + duration_ms * 1000u;
    push(bridge, BRIDGE_RING_DOM_TO_APP, &up);
}

void headless_frame_mark(bridge_header *bridge, uint32_t time_us) {
    bridge_record mark = {.type = BRIDGE_EV_FRAME_MARK};
    mark.u[0] = time_us;
    push(bridge, BRIDGE_RING_DOM_TO_APP, &mark);
}

void headless_drain(bridge_header *bridge, headless_outbox *outbox) {
    bridge_record rec;
    bridge_ring *commands = bridge_get_ring(bridge, BRIDGE_RING_APP_TO_RENDERER);
    while (bridge_ring_pop(commands, &rec)) {
        switch (rec.type) {
        case BRIDGE_CMD_BIND:
            outbox->binds += 1;
            break;
        case BRIDGE_CMD_SPAWN:
            outbox->spawns += 1;
            outbox->last_spawn_slot = rec.u[0];
            outbox->last_spawn_node = rec.u[1];
            break;
        case BRIDGE_CMD_DESPAWN:
            outbox->despawns += 1;
            break;
        case BRIDGE_CMD_RELOAD_SCENE:
            outbox->reloads += 1;
            break;
        default:
            break;
        }
    }
    bridge_ring *cues = bridge_get_ring(bridge, BRIDGE_RING_APP_TO_DOM);
    while (bridge_ring_pop(cues, &rec)) {
        if (rec.type == BRIDGE_CUE) {
            outbox->cues += 1;
            if (rec.u[0] < HEADLESS_CUE_KINDS) {
                outbox->cues_by_id[rec.u[0]] += 1;
                outbox->last_cue_value[rec.u[0]] = rec.f[1];
            }
        }
    }
}
