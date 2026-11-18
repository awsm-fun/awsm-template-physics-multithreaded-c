// RUNTIME: shared by every game; your game goes in app/src/game/ and dom/game.js
//
// Tests for camera.c: the orbit camera and click-to-ray picking.

#include "runtime/camera.h"
#include "runtime/check.h"
#include "runtime/math3d.h"

static void test_camera_ray_matches_projection(void) {
    // A point projected to the screen with the view-projection matrix must
    // come back from camera_ray() at the same place.
    orbit_camera camera = camera_default(vec3_make(0.0f, 0.4f, 0.0f));
    camera_orbit(&camera, 37.0f, 12.0f);
    float aspect = 16.0f / 9.0f;
    vec3 point = vec3_make(1.2f, 0.3f, -0.7f);

    vec3 ndc = mat4_transform_point(camera_view_projection(&camera, aspect), point);
    CHECK(ndc.z > 0.0f && ndc.z < 1.0f); // in front, inside near..far

    vec3 hit;
    CHECK(ray_hit_plane_y(camera_ray(&camera, aspect, ndc.x, ndc.y), point.y, &hit));
    CHECK_NEAR(hit.x, point.x, 1e-3);
    CHECK_NEAR(hit.z, point.z, 1e-3);

    // The centre of the screen is the camera target.
    vec3 centre = mat4_transform_point(camera_view_projection(&camera, aspect), camera.target);
    CHECK_NEAR(centre.x, 0.0f, 1e-5);
    CHECK_NEAR(centre.y, 0.0f, 1e-5);
}

int main(void) {
    test_camera_ray_matches_projection();
    return check_report();
}
