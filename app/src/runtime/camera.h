// RUNTIME: shared by every game; your game goes in app/src/game/ and dom/game.js
//
// camera.h — an orbit camera: it circles a target point and always looks at it.
//
// The app owns the camera: the runtime keeps one (runtime_get_camera), your
// game decides how it moves, and each step it is copied into the snapshot
// (camera_to_bridge). The renderer draws from exactly that view, which is why
// the app can also turn a click into a world-space ray (camera_ray) and get
// the same answer the picture shows.

#ifndef CAMERA_H
#define CAMERA_H

#include "runtime/bridge.h"
#include "runtime/math3d.h"

typedef struct orbit_camera {
    vec3 target;  // world point the camera looks at and circles
    float yaw;    // radians around +Y; 0 = camera on the +Z side of the target
    float pitch;  // radians above the horizon
    float radius; // distance from the target
    float fov_y;  // vertical field of view, radians
    float near;
    float far;
} orbit_camera;

typedef struct ray {
    vec3 origin;
    vec3 direction; // unit length
} ray;

orbit_camera camera_default(vec3 target);

// Drag in CSS pixels: horizontal turns around the target, vertical tilts
// (dragging down lowers the camera).
void camera_orbit(orbit_camera *camera, float dx_px, float dy_px);

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
