// RUNTIME: shared by every game; your game goes in app/src/game/ and dom/game.js
//
// Prints the bridge block's layout, as this compiler sees it, as JSON.
//
// tools/check-bridge.mjs compares it with the two hand-written mirrors
// (renderer/src/bridge.rs and dom/runtime/bridge.js), so a field that moves
// here without its mirrors being updated fails `task test` instead of drawing
// garbage. When you add a field or a constant that a mirror uses, add it
// here too: a mirror may only name things this file prints.

#include <stddef.h>
#include <stdio.h>

#include "runtime/bridge.h"

static bool first_group = true;
static bool first_entry = true;

static void group(const char *name) {
    printf("%s\n  \"%s\": {", first_group ? "" : "\n  },", name);
    first_group = false;
    first_entry = true;
}

static void entry(const char *name, size_t value) {
    printf("%s\n    \"%s\": %zu", first_entry ? "" : ",", name, value);
    first_entry = false;
}

#define OFFSET(type, field) entry(#field, offsetof(type, field))

int main(void) {
    printf("{");

    group("handshake");
    entry("magic", BRIDGE_MAGIC);
    entry("version", BRIDGE_VERSION);

    group("header");
    OFFSET(bridge_header, magic);
    OFFSET(bridge_header, version);
    OFFSET(bridge_header, total_size);
    OFFSET(bridge_header, header_size);
    OFFSET(bridge_header, step_hz);
    OFFSET(bridge_header, frame_tick);
    OFFSET(bridge_header, latest_step);
    OFFSET(bridge_header, input_offset);
    OFFSET(bridge_header, input_size);
    OFFSET(bridge_header, stats_offset);
    OFFSET(bridge_header, stats_size);
    OFFSET(bridge_header, snapshot_offset);
    OFFSET(bridge_header, snapshot_count);
    OFFSET(bridge_header, snapshot_stride);
    OFFSET(bridge_header, snapshot_header_size);
    OFFSET(bridge_header, pose_capacity);
    OFFSET(bridge_header, pose_stride);
    OFFSET(bridge_header, ring_offset);
    OFFSET(bridge_header, ring_header_size);
    OFFSET(bridge_header, record_size);
    OFFSET(bridge_header, timing_offset);
    OFFSET(bridge_header, timing_capacity);
    OFFSET(bridge_header, timing_stride);
    OFFSET(bridge_header, frame_time_us);
    OFFSET(bridge_header, frame_time_valid);

    group("input");
    OFFSET(bridge_input, keys);
    OFFSET(bridge_input, canvas_width);
    OFFSET(bridge_input, canvas_height);
    OFFSET(bridge_input, pointer_ndc_x);
    OFFSET(bridge_input, pointer_ndc_y);
    OFFSET(bridge_input, pointer_buttons);

    group("stats");
    OFFSET(bridge_stats, step_work_us);
    OFFSET(bridge_stats, active_poses);
    OFFSET(bridge_stats, frame_count);
    OFFSET(bridge_stats, frame_work_us);
    OFFSET(bridge_stats, gpu_frame_us);
    OFFSET(bridge_stats, gpu_frame_count);
    OFFSET(bridge_stats, solver_threads);
    OFFSET(bridge_stats, frame_span_us);
    OFFSET(bridge_stats, app_late);
    OFFSET(bridge_stats, detail);
    OFFSET(bridge_stats, physics_step_us);

    group("timing");
    OFFSET(bridge_timing, kind);
    OFFSET(bridge_timing, total_ns);
    OFFSET(bridge_timing, count);
    OFFSET(bridge_timing, name);
    entry("name_size", sizeof(((bridge_timing *)0)->name));
    entry("app_slots", BRIDGE_TIMING_APP_SLOTS);

    group("timing_kind");
    entry("app_phase", BRIDGE_TIMING_APP_PHASE);
    entry("renderer_phase", BRIDGE_TIMING_RENDERER_PHASE);
    entry("renderer_pass", BRIDGE_TIMING_RENDERER_PASS);
    entry("gpu_pass", BRIDGE_TIMING_GPU_PASS);
    entry("renderer_prep", BRIDGE_TIMING_RENDERER_PREP);
    entry("renderer_submit", BRIDGE_TIMING_RENDERER_SUBMIT);
    entry("renderer_hook", BRIDGE_TIMING_RENDERER_HOOK);
    entry("physics_stage", BRIDGE_TIMING_PHYSICS_STAGE);
    entry("physics_solve", BRIDGE_TIMING_PHYSICS_SOLVE);

    group("detail");
    entry("app", BRIDGE_DETAIL_APP);
    entry("renderer", BRIDGE_DETAIL_RENDERER);
    entry("gpu", BRIDGE_DETAIL_GPU);
    entry("physics", BRIDGE_DETAIL_PHYSICS);

    group("snapshot");
    OFFSET(bridge_snapshot_header, version);
    OFFSET(bridge_snapshot_header, step);
    OFFSET(bridge_snapshot_header, pose_count);
    OFFSET(bridge_snapshot_header, time_us);
    OFFSET(bridge_snapshot_header, camera);

    group("camera");
    OFFSET(bridge_camera, eye);
    OFFSET(bridge_camera, target);
    OFFSET(bridge_camera, up);
    OFFSET(bridge_camera, fov_y);
    OFFSET(bridge_camera, near);
    OFFSET(bridge_camera, far);

    group("pose");
    OFFSET(bridge_pose, position);
    OFFSET(bridge_pose, rotation);
    OFFSET(bridge_pose, flags);

    group("ring");
    OFFSET(bridge_ring, head);
    OFFSET(bridge_ring, tail);
    OFFSET(bridge_ring, capacity);
    OFFSET(bridge_ring, dropped);

    group("record");
    OFFSET(bridge_record, type);
    OFFSET(bridge_record, step);
    entry("payload", offsetof(bridge_record, u));

    group("size");
    entry("header", sizeof(bridge_header));
    entry("input", sizeof(bridge_input));
    entry("stats", sizeof(bridge_stats));
    entry("camera", sizeof(bridge_camera));
    entry("snapshot_header", sizeof(bridge_snapshot_header));
    entry("pose", sizeof(bridge_pose));
    entry("ring", sizeof(bridge_ring));
    entry("record", sizeof(bridge_record));
    entry("timing", sizeof(bridge_timing));

    group("ring_index");
    entry("dom_to_app", BRIDGE_RING_DOM_TO_APP);
    entry("renderer_to_app", BRIDGE_RING_RENDERER_TO_APP);
    entry("app_to_renderer", BRIDGE_RING_APP_TO_RENDERER);
    entry("app_to_dom", BRIDGE_RING_APP_TO_DOM);

    group("record_type");
    entry("ev_key", BRIDGE_EV_KEY);
    entry("ev_pointer_down", BRIDGE_EV_POINTER_DOWN);
    entry("ev_pointer_up", BRIDGE_EV_POINTER_UP);
    entry("ev_pointer_move", BRIDGE_EV_POINTER_MOVE);
    entry("ev_wheel", BRIDGE_EV_WHEEL);
    entry("ev_frame_mark", BRIDGE_EV_FRAME_MARK);
    entry("scene_begin", BRIDGE_SCENE_BEGIN);
    entry("scene_collider", BRIDGE_SCENE_COLLIDER);
    entry("scene_ready", BRIDGE_SCENE_READY);
    entry("cmd_bind", BRIDGE_CMD_BIND);
    entry("cmd_spawn", BRIDGE_CMD_SPAWN);
    entry("cmd_despawn", BRIDGE_CMD_DESPAWN);
    entry("cmd_reload_scene", BRIDGE_CMD_RELOAD_SCENE);
    entry("cue", BRIDGE_CUE);

    group("shape");
    entry("box", BRIDGE_SHAPE_BOX);
    entry("sphere", BRIDGE_SHAPE_SPHERE);
    entry("capsule", BRIDGE_SHAPE_CAPSULE);
    entry("cylinder", BRIDGE_SHAPE_CYLINDER);
    entry("cone", BRIDGE_SHAPE_CONE);

    group("pose_flag");
    entry("active", BRIDGE_POSE_ACTIVE);
    entry("teleport", BRIDGE_POSE_TELEPORT);

    group("button");
    entry("cancelled", BRIDGE_BUTTON_CANCELLED);

    group("pointer");
    entry("mouse", BRIDGE_POINTER_MOUSE);
    entry("touch", BRIDGE_POINTER_TOUCH);
    entry("pen", BRIDGE_POINTER_PEN);

    group("key");
    entry("w", BRIDGE_KEY_W);
    entry("a", BRIDGE_KEY_A);
    entry("s", BRIDGE_KEY_S);
    entry("d", BRIDGE_KEY_D);
    entry("up", BRIDGE_KEY_UP);
    entry("down", BRIDGE_KEY_DOWN);
    entry("left", BRIDGE_KEY_LEFT);
    entry("right", BRIDGE_KEY_RIGHT);
    entry("space", BRIDGE_KEY_SPACE);
    entry("shift", BRIDGE_KEY_SHIFT);
    entry("enter", BRIDGE_KEY_ENTER);
    entry("escape", BRIDGE_KEY_ESCAPE);
    entry("r", BRIDGE_KEY_R);
    entry("count", BRIDGE_KEY_COUNT);

    // Not a layout: a test vector, so the check can also compare the other
    // implementations of bridge_hash().
    group("hash");
    entry("foobar", bridge_hash("foobar"));

    printf("\n  }\n}\n");
    return 0;
}
