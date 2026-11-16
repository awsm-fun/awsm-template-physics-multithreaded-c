// camera.h — an orbit camera: it circles a target point and always looks at it.
//
// The app owns the camera. Each step it is copied into the snapshot
// (camera_to_bridge) and the renderer draws from exactly that view, which is
// why the app can also turn a click into a world-space ray (camera_ray) and
// get the same answer the picture shows.

#ifndef CAMERA_H
#define CAMERA_H

#include "bridge.h"
#include "math3d.h"

// How many steps one orbit movement is spread over (25 ms at 240 Hz). See
// camera_orbit.
constexpr int CAMERA_ORBIT_SPREAD_STEPS = 6;

typedef struct orbit_camera {
    vec3 target;  // world point the camera looks at and circles
    float yaw;    // radians around +Y; 0 = camera on the +Z side of the target
    float pitch;  // radians above the horizon
    float radius; // distance from the target
    float fov_y;  // vertical field of view, radians
    float near;
    float far;

    // Orbit still to be applied: slot (next + i) % CAMERA_ORBIT_SPREAD_STEPS
    // is what the i-th coming camera_step adds.
    float orbit_yaw[CAMERA_ORBIT_SPREAD_STEPS];
    float orbit_pitch[CAMERA_ORBIT_SPREAD_STEPS];
    int orbit_next;
} orbit_camera;

typedef struct ray {
    vec3 origin;
    vec3 direction; // unit length
} ray;

orbit_camera camera_default(vec3 target);

// Drag in CSS pixels: horizontal turns around the target, vertical tilts
// (dragging down lowers the camera).
//
// The turn is not applied at once: it is spread evenly over the next
// CAMERA_ORBIT_SPREAD_STEPS calls to camera_step, the first of them in the
// current step. Pointer movement arrives once per display frame, but the app
// runs its steps in bursts, one burst per frame, of 3, 4 or 5 steps at 60 Hz
// as the wake-up time wobbles. Applied at once, each movement would land on
// the first step of a burst, and the renderer, which plays the steps back at
// an even pace, would show some frames with no turn and the next with two:
// a visible judder whenever the camera orbits. Spread over more steps than a
// burst holds, consecutive movements overlap into an even turn. The cost is
// half the spread in latency, about 12 ms.
void camera_orbit(orbit_camera *camera, float dx_px, float dy_px);

// Applies this step's share of any orbit in progress (see camera_orbit). Call
// once per app step.
void camera_step(orbit_camera *camera);

// Wheel in CSS pixels: positive (towards the user) moves away.
void camera_zoom(orbit_camera *camera, float dy_px);

vec3 camera_eye(const orbit_camera *camera);

// Unit vectors along the ground plane: where "forward" and "right" point for
// the viewer, so keyboard movement can follow the view.
vec3 camera_ground_forward(const orbit_camera *camera);
vec3 camera_ground_right(const orbit_camera *camera);

// The view the renderer will draw.
bridge_camera camera_to_bridge(const orbit_camera *camera);

// projection * view, for a canvas of the given aspect ratio (width / height).
mat4 camera_view_projection(const orbit_camera *camera, float aspect);

// The ray from the eye through a point on screen given in normalized device
// coordinates (x right, y up, both -1..1).
ray camera_ray(const orbit_camera *camera, float aspect, float ndc_x, float ndc_y);

// Where `r` crosses the horizontal plane y = `height`. Returns false if it
// never does in front of the origin (e.g. a click on the sky).
bool ray_hit_plane_y(ray r, float height, vec3 *hit);

#endif // CAMERA_H
