// physics.h — the small amount of glue between this app and Box3D.
//
// Box3D (https://github.com/erincatto/box3d, vendored unmodified under
// app/vendored/box3d) is used directly: app.c calls the b3* functions itself.
// This file only holds the pieces that are not specific to the demo:
// creating a world with the right threading setup, routing Box3D's asserts
// somewhere visible, and turning a collider the renderer described into a
// Box3D shape.

#ifndef PHYSICS_H
#define PHYSICS_H

#include <box3d/box3d.h>

#include "bridge.h"

// Creates a world with gravity along -Y.
//
// `hit_event_threshold` (m/s): contacts that approach faster than this are
// reported as hit events (see b3World_GetContactEvents).
//
// `worker_count`: how many threads the solver uses, including the calling
// one. 1 runs everything on the calling thread. More makes Box3D start
// `worker_count - 1` threads of its own, which then exist until the world is
// destroyed; see web_main.c for what that means in the browser.
b3WorldId physics_create_world(float gravity, float hit_event_threshold, uint32_t worker_count);

// Adds the shape of one BRIDGE_SCENE_COLLIDER to `body`, aligned with it and
// centred on it (capsules, cylinders and cones stand along the body's Y
// axis, as they do in the scene editor; the cone is the exception to
// "centred", see physics.c). `shape` is a BRIDGE_SHAPE_* and
// `dims` its three dimension words, f[3..5] of the record. Returns false for
// a shape kind this function does not know.
bool physics_add_collider_shape(b3BodyId body, const b3ShapeDef *def, uint32_t shape,
                                const float dims[3], b3ShapeId *out_shape);

#endif // PHYSICS_H
