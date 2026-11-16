// camera.c — see camera.h.

#include "camera.h"

constexpr float ORBIT_RADIANS_PER_PX = 0.005f;
// Just under straight down, so the view never flips over the top.
constexpr float PITCH_MAX = MATH3D_PI / 2.0f - 0.01f;
// Keeps the eye above the table's rim.
constexpr float PITCH_MIN = 0.15f;
constexpr float RADIUS_MIN = 2.0f;
constexpr float RADIUS_MAX = 25.0f;
// Fraction of the radius added per CSS pixel of wheel movement.
constexpr float ZOOM_PER_PX = 0.001f;

orbit_camera camera_default(vec3 target) {
    return (orbit_camera){
        .target = target,
        .yaw = 0.0f,
        .pitch = 0.46f,
        .radius = 9.0f,
        .fov_y = 55.0f * MATH3D_PI / 180.0f,
        .near = 0.1f,
        .far = 400.0f,
    };
}

void camera_orbit(orbit_camera *camera, float dx_px, float dy_px) {
    float yaw = -dx_px * ORBIT_RADIANS_PER_PX / (float)CAMERA_ORBIT_SPREAD_STEPS;
    float pitch = -dy_px * ORBIT_RADIANS_PER_PX / (float)CAMERA_ORBIT_SPREAD_STEPS;
    for (int i = 0; i < CAMERA_ORBIT_SPREAD_STEPS; ++i) {
        camera->orbit_yaw[i] += yaw;
        camera->orbit_pitch[i] += pitch;
    }
}

void camera_step(orbit_camera *camera) {
    int slot = camera->orbit_next;
    camera->yaw += camera->orbit_yaw[slot];
    camera->pitch = clampf(camera->pitch + camera->orbit_pitch[slot], PITCH_MIN, PITCH_MAX);
    camera->orbit_yaw[slot] = 0.0f;
    camera->orbit_pitch[slot] = 0.0f;
    camera->orbit_next = (slot + 1) % CAMERA_ORBIT_SPREAD_STEPS;
}

void camera_zoom(orbit_camera *camera, float dy_px) {
    camera->radius = clampf(camera->radius * (1.0f + dy_px * ZOOM_PER_PX), RADIUS_MIN, RADIUS_MAX);
}

vec3 camera_eye(const orbit_camera *camera) {
    float cp = cosf(camera->pitch);
    vec3 offset = {
        camera->radius * cp * sinf(camera->yaw),
        camera->radius * sinf(camera->pitch),
        camera->radius * cp * cosf(camera->yaw),
    };
    return vec3_add(camera->target, offset);
}

// The eye sits at (sin yaw, cos yaw) around the target, so looking at the
// target means facing (-sin yaw, -cos yaw) on the ground.
vec3 camera_ground_forward(const orbit_camera *camera) {
    return vec3_make(-sinf(camera->yaw), 0.0f, -cosf(camera->yaw));
}

vec3 camera_ground_right(const orbit_camera *camera) {
    return vec3_make(cosf(camera->yaw), 0.0f, -sinf(camera->yaw));
}

bridge_camera camera_to_bridge(const orbit_camera *camera) {
    vec3 eye = camera_eye(camera);
    return (bridge_camera){
        .eye = {eye.x, eye.y, eye.z},
        .target = {camera->target.x, camera->target.y, camera->target.z},
        .up = {0.0f, 1.0f, 0.0f},
        .fov_y = camera->fov_y,
        .near = camera->near,
        .far = camera->far,
    };
}

mat4 camera_view_projection(const orbit_camera *camera, float aspect) {
    mat4 view = mat4_look_at(camera_eye(camera), camera->target, vec3_make(0.0f, 1.0f, 0.0f));
    mat4 projection = mat4_perspective(camera->fov_y, aspect, camera->near, camera->far);
    return mat4_mul(projection, view);
}

ray camera_ray(const orbit_camera *camera, float aspect, float ndc_x, float ndc_y) {
    // Undo the perspective by hand: at distance 1 in front of the eye, the
    // screen spans ±tan(fov/2) vertically and ±tan(fov/2)·aspect horizontally.
    vec3 eye = camera_eye(camera);
    vec3 forward = vec3_normalize(vec3_sub(camera->target, eye));
    vec3 right = vec3_normalize(vec3_cross(forward, vec3_make(0.0f, 1.0f, 0.0f)));
    vec3 up = vec3_cross(right, forward);
    float half_h = tanf(camera->fov_y * 0.5f);
    vec3 direction = vec3_add(forward, vec3_add(vec3_scale(right, ndc_x * half_h * aspect),
                                                vec3_scale(up, ndc_y * half_h)));
    return (ray){.origin = eye, .direction = vec3_normalize(direction)};
}

bool ray_hit_plane_y(ray r, float height, vec3 *hit) {
    if (fabsf(r.direction.y) < 1e-6f) {
        return false; // parallel to the plane
    }
    float t = (height - r.origin.y) / r.direction.y;
    if (t <= 0.0f) {
        return false; // the plane is behind the eye
    }
    *hit = vec3_add(r.origin, vec3_scale(r.direction, t));
    return true;
}
