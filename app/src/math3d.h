// math3d.h — just enough vector / quaternion / matrix maths for the app.
//
// Conventions (the same as the renderer's):
//   * right-handed, +Y up; a camera looks down its own -Z
//   * quaternions are (x, y, z, w) and rotate with q * v * q⁻¹
//   * mat4 is column-major: element (row r, column c) is m[c * 4 + r]
//   * clip-space depth runs 0..1 (WebGPU), not -1..1 (OpenGL)
//
// Everything is `static inline`, so there is no .c file to keep in sync.

#ifndef MATH3D_H
#define MATH3D_H

#include <math.h>

#define MATH3D_PI 3.14159265358979323846f

typedef struct vec3 {
    float x, y, z;
} vec3;

typedef struct quat {
    float x, y, z, w;
} quat;

typedef struct mat4 {
    float m[16];
} mat4;

// ── Scalars ───────────────────────────────────────────────────────────────

static inline float clampf(float value, float lo, float hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

// ── vec3 ─────────────────────────────────────────────────────────────────

static inline vec3 vec3_make(float x, float y, float z) {
    return (vec3){x, y, z};
}

static inline vec3 vec3_add(vec3 a, vec3 b) {
    return (vec3){a.x + b.x, a.y + b.y, a.z + b.z};
}

static inline vec3 vec3_sub(vec3 a, vec3 b) {
    return (vec3){a.x - b.x, a.y - b.y, a.z - b.z};
}

static inline vec3 vec3_scale(vec3 v, float s) {
    return (vec3){v.x * s, v.y * s, v.z * s};
}

static inline float vec3_dot(vec3 a, vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline vec3 vec3_cross(vec3 a, vec3 b) {
    return (vec3){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

static inline float vec3_length(vec3 v) {
    return sqrtf(vec3_dot(v, v));
}

// Unit vector in the direction of `v`, or `v` unchanged if it is (nearly) zero.
static inline vec3 vec3_normalize(vec3 v) {
    float len = vec3_length(v);
    return len > 1e-12f ? vec3_scale(v, 1.0f / len) : v;
}

// ── quat ─────────────────────────────────────────────────────────────────

static inline quat quat_identity(void) {
    return (quat){0.0f, 0.0f, 0.0f, 1.0f};
}

// Rotation of `angle` radians about the unit vector `axis`.
static inline quat quat_from_axis_angle(vec3 axis, float angle) {
    float s = sinf(angle * 0.5f);
    return (quat){axis.x * s, axis.y * s, axis.z * s, cosf(angle * 0.5f)};
}

// a * b: rotate by b first, then by a.
static inline quat quat_mul(quat a, quat b) {
    return (quat){
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

// Repeatedly multiplying rotations lets rounding creep in; renormalise.
static inline quat quat_normalize(quat q) {
    float len = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len < 1e-12f) {
        return quat_identity();
    }
    float inv = 1.0f / len;
    return (quat){q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

// Rotates `v` by the unit quaternion `q`.
static inline vec3 quat_rotate(quat q, vec3 v) {
    vec3 u = {q.x, q.y, q.z};
    vec3 t = vec3_scale(vec3_cross(u, v), 2.0f);
    return vec3_add(vec3_add(v, vec3_scale(t, q.w)), vec3_cross(u, t));
}

// ── mat4 ─────────────────────────────────────────────────────────────────

// View matrix for a camera at `eye` looking at `target`.
static inline mat4 mat4_look_at(vec3 eye, vec3 target, vec3 up) {
    vec3 f = vec3_normalize(vec3_sub(target, eye)); // forward
    vec3 s = vec3_normalize(vec3_cross(f, up));     // right
    vec3 u = vec3_cross(s, f);                      // true up
    return (mat4){{
        s.x, u.x, -f.x, 0.0f,
        s.y, u.y, -f.y, 0.0f,
        s.z, u.z, -f.z, 0.0f,
        -vec3_dot(s, eye), -vec3_dot(u, eye), vec3_dot(f, eye), 1.0f,
    }};
}

// Perspective projection; maps view depth near..far to clip depth 0..1.
static inline mat4 mat4_perspective(float fov_y, float aspect, float near, float far) {
    float h = 1.0f / tanf(fov_y * 0.5f);
    float w = h / aspect;
    float r = far / (near - far);
    return (mat4){{
        w, 0.0f, 0.0f, 0.0f,
        0.0f, h, 0.0f, 0.0f,
        0.0f, 0.0f, r, -1.0f,
        0.0f, 0.0f, r * near, 0.0f,
    }};
}

// a * b: apply b first, then a.
static inline mat4 mat4_mul(mat4 a, mat4 b) {
    mat4 out;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[k * 4 + r] * b.m[c * 4 + k];
            }
            out.m[c * 4 + r] = sum;
        }
    }
    return out;
}

// Transforms a point (w = 1) and divides by the resulting w.
static inline vec3 mat4_transform_point(mat4 m, vec3 p) {
    const float *a = m.m;
    float x = a[0] * p.x + a[4] * p.y + a[8] * p.z + a[12];
    float y = a[1] * p.x + a[5] * p.y + a[9] * p.z + a[13];
    float z = a[2] * p.x + a[6] * p.y + a[10] * p.z + a[14];
    float w = a[3] * p.x + a[7] * p.y + a[11] * p.z + a[15];
    float inv = w != 0.0f ? 1.0f / w : 0.0f;
    return (vec3){x * inv, y * inv, z * inv};
}

#endif // MATH3D_H
