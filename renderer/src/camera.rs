//! Turning the app's `bridge_camera` into what the renderer wants.
//!
//! The app owns the camera: it publishes eye / target / up and the lens in
//! every snapshot. The renderer only adds what it alone knows — the aspect
//! ratio of the surface and its depth convention — and both of those are
//! applied inside `AwsmRenderer::set_camera`, not here.

use awsm_renderer::camera::CameraParams;
use glam::{Mat4, Vec3};

use crate::bridge;

/// The camera used until the app has published one: a view from above one
/// side of the origin, so the loaded scene is visible while the app starts.
pub const STARTUP: bridge::Camera = bridge::Camera {
    eye: [0.0, 4.4, 8.1],
    target: [0.0, 0.4, 0.0],
    up: [0.0, 1.0, 0.0],
    fov_y: 0.96,
    near: 0.1,
    far: 400.0,
};

/// The camera at a display position between two snapshots.
///
/// Returns `None` when neither snapshot carries a usable camera (the app has
/// not published one yet); the caller then keeps the camera it already has.
pub fn between(
    older: Option<&bridge::Camera>,
    newer: Option<&bridge::Camera>,
    alpha: f32,
) -> Option<bridge::Camera> {
    match (
        older.filter(|c| is_usable(c)),
        newer.filter(|c| is_usable(c)),
    ) {
        (Some(a), Some(b)) => Some(bridge::Camera {
            eye: lerp3(a.eye, b.eye, alpha),
            target: lerp3(a.target, b.target, alpha),
            up: lerp3(a.up, b.up, alpha),
            fov_y: lerp(a.fov_y, b.fov_y, alpha),
            near: lerp(a.near, b.near, alpha),
            far: lerp(a.far, b.far, alpha),
        })
        // Blending two valid cameras can still land on a degenerate one (an
        // `up` that flipped between the two steps passes through zero).
        .filter(is_usable)
        .or(Some(*a)),
        (Some(only), None) | (None, Some(only)) => Some(*only),
        (None, None) => None,
    }
}

/// The two halves `AwsmRenderer::set_camera` takes: a view matrix and the
/// projection parameters.
pub fn to_renderer(camera: &bridge::Camera) -> (Mat4, CameraParams) {
    let eye = Vec3::from_array(camera.eye);
    let target = Vec3::from_array(camera.target);
    let view = Mat4::look_at_rh(eye, target, Vec3::from_array(camera.up));
    let mut params = CameraParams::perspective(camera.fov_y, camera.near, camera.far);
    // Only matters if the scene enables depth of field: keep whatever the
    // camera looks at in focus.
    params.focus_distance = eye.distance(target);
    (view, params)
}

/// Whether a camera can be turned into finite matrices. A block that has
/// never been written is all zeroes, and a view matrix built from that is NaN.
fn is_usable(camera: &bridge::Camera) -> bool {
    let eye = Vec3::from_array(camera.eye);
    let target = Vec3::from_array(camera.target);
    let up = Vec3::from_array(camera.up);
    let forward = target - eye;

    let finite = eye.is_finite()
        && target.is_finite()
        && up.is_finite()
        && camera.fov_y.is_finite()
        && camera.near.is_finite()
        && camera.far.is_finite();

    finite
        && camera.fov_y > 0.0
        && camera.fov_y < std::f32::consts::PI
        && camera.near > 0.0
        && camera.far > camera.near
        // The eye must look somewhere, and not straight along `up`.
        && forward.cross(up).length_squared() > 1e-12
}

fn lerp(from: f32, to: f32, alpha: f32) -> f32 {
    from + (to - from) * alpha
}

fn lerp3(from: [f32; 3], to: [f32; 3], alpha: f32) -> [f32; 3] {
    Vec3::from_array(from)
        .lerp(Vec3::from_array(to), alpha)
        .to_array()
}
