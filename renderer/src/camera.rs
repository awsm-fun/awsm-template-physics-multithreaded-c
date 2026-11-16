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

/// The camera to draw with, from the two snapshots a frame shows: the newer
/// one's, whole, not interpolated (see the comment where it is called), or
/// the older one's if the newer carries no usable camera.
///
/// Returns `None` when neither snapshot carries a usable camera (the app has
/// not published one yet); the caller then keeps the camera it already has.
pub fn newest_usable(
    older: Option<&bridge::Camera>,
    newer: Option<&bridge::Camera>,
) -> Option<bridge::Camera> {
    newer
        .filter(|camera| is_usable(camera))
        .or(older.filter(|camera| is_usable(camera)))
        .copied()
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
