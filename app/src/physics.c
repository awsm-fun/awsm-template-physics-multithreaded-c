// physics.c — see physics.h.

#include "physics.h"

#include <stdio.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/console.h>
#endif

// Sides used to approximate a round cylinder or cone with a convex hull.
constexpr int ROUND_HULL_SIDES = 16;

// Prints one line of Box3D's own output. Box3D may call this from a solver
// thread. In the browser, stdio from another thread is handed to the app
// thread to print, which only happens while that thread is waiting inside
// the step; emscripten_err() writes to the console from whichever thread
// calls it, so a message cannot get stuck behind a crash.
static void report(const char *message) {
#ifdef __EMSCRIPTEN__
    emscripten_err(message);
#else
    fprintf(stderr, "%s\n", message);
#endif
}

// Box3D calls this when one of its internal checks fails (Debug builds).
// Returning non-zero lets Box3D stop right there, which is what we want:
// carrying on after a failed assert corrupts the world.
static int on_box3d_assert(const char *condition, const char *file, int line) {
    char message[512];
    snprintf(message, sizeof message, "BOX3D ASSERT: %s (%s:%d)",
             condition != nullptr ? condition : "?", file != nullptr ? file : "?", line);
    report(message);
    return 1;
}

// Box3D's warnings (it has a few even in Release builds).
static void on_box3d_log(const char *message) {
    char line[512];
    snprintf(line, sizeof line, "box3d: %s", message != nullptr ? message : "?");
    report(line);
}

b3WorldId physics_create_world(float gravity, float hit_event_threshold, uint32_t worker_count) {
    b3SetAssertFcn(on_box3d_assert);
    b3SetLogFcn(on_box3d_log);

    // Box3D's definition structs carry a hidden validity cookie, so always
    // start from b3Default*Def() and change fields; never build one by hand.
    b3WorldDef def = b3DefaultWorldDef();
    def.gravity = (b3Vec3){0.0f, -gravity, 0.0f};
    def.hitEventThreshold = hit_event_threshold;
    // No task callbacks are set, so with more than one worker Box3D uses its
    // own scheduler and threads.
    def.workerCount = worker_count < 1u ? 1u : worker_count;
    return b3CreateWorld(&def);
}

bool physics_add_collider_shape(b3BodyId body, const b3ShapeDef *def, uint32_t shape,
                                const float dims[3], b3ShapeId *out_shape) {
    b3ShapeId id;
    switch (shape) {
    case BRIDGE_SHAPE_BOX: {
        // dims: half extents x, y, z
        b3BoxHull box = b3MakeBoxHull(dims[0], dims[1], dims[2]);
        id = b3CreateHullShape(body, def, &box.base);
        break;
    }
    case BRIDGE_SHAPE_SPHERE: {
        // dims: radius
        b3Sphere sphere = {.center = {0.0f, 0.0f, 0.0f}, .radius = dims[0]};
        id = b3CreateSphereShape(body, def, &sphere);
        break;
    }
    case BRIDGE_SHAPE_CAPSULE: {
        // dims: half height (of the straight part), radius
        b3Capsule capsule = {
            .center1 = {0.0f, -dims[0], 0.0f},
            .center2 = {0.0f, dims[0], 0.0f},
            .radius = dims[1],
        };
        id = b3CreateCapsuleShape(body, def, &capsule);
        break;
    }
    // Box3D has no cylinder or cone primitive; it builds convex hulls for
    // them. Those are heap-allocated, and the world keeps its own copy when
    // the shape is created, so ours is freed straight away.
    case BRIDGE_SHAPE_CYLINDER: {
        // dims: half height, radius. The hull spans y = offset .. offset +
        // height, so an offset of -half height centres it.
        b3HullData *hull = b3CreateCylinder(2.0f * dims[0], dims[1], -dims[0], ROUND_HULL_SIDES);
        id = b3CreateHullShape(body, def, hull);
        b3DestroyHull(hull);
        break;
    }
    case BRIDGE_SHAPE_CONE: {
        // dims: half height, base radius. An approximation: Box3D's cone
        // spans y = 0 .. height (base at the body's origin, not centred) and
        // needs a top radius above zero, so the tip is cut at 5% of the base.
        b3HullData *hull = b3CreateCone(2.0f * dims[0], dims[1], dims[1] * 0.05f, ROUND_HULL_SIDES);
        id = b3CreateHullShape(body, def, hull);
        b3DestroyHull(hull);
        break;
    }
    default:
        return false;
    }
    if (out_shape != nullptr) {
        *out_shape = id;
    }
    return true;
}
