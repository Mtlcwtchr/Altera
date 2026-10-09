#include "framework.hpp"
#include "terrain_core/dlss_math.hpp"
#include <array>

using namespace terrain_core;
namespace {
// Eye-relative perspective camera facing +Z, reversed infinite depth.
constexpr std::array<float, 16> camera{1,0,0,0, 0,1,0,0, 0,0,0,0.25f, 0,0,1,0};
std::array<float, 4> transform(const float m[16], const std::array<float, 4>& p) {
    std::array<float, 4> result{};
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) result[r] += m[r * 4 + c] * p[c];
    return result;
}
}
TEST(dlss_motion_stationary_camera_removes_jitter) {
    auto jittered = camera;
    constexpr float jitterX = 0.25f, jitterY = -0.3f, width = 1280, height = 720;
    for (int c = 0; c < 4; ++c) {
        jittered[c] -= 2 * jitterX / width * camera[12 + c];
        jittered[4 + c] += 2 * jitterY / height * camera[12 + c];
    }
    float reprojection[16];
    const double displacement[3]{};
    CHECK(dlss_math::reprojection(jittered.data(), camera.data(), displacement, reprojection));
    const std::array<float, 4> point{1,2,30,1};
    const auto now = transform(jittered.data(), point);
    const auto previous = transform(reprojection, now);
    const float motionX = (previous[0] / previous[3] - now[0] / now[3]) * 0.5f * width - jitterX;
    const float motionY = (previous[1] / previous[3] - now[1] / now[3]) * -0.5f * height - jitterY;
    CHECK(std::abs(motionX) < 1e-4f);
    CHECK(std::abs(motionY) < 1e-4f);
}
TEST(dlss_eye_translation_has_correct_direction_and_sky_is_infinite) {
    const double displacement[3] = {1,0,0};
    float reprojection[16];
    CHECK(dlss_math::reprojection(camera.data(), camera.data(), displacement, reprojection));
    const auto current = transform(camera.data(), {0,0,10,1});
    const auto previous = transform(reprojection, current);
    // Eye moves right: the world was to the right in the previous image.
    CHECK(std::abs(previous[0] / previous[3] - 0.1f) < 1e-6f);
    const auto sky = transform(reprojection, {0.4f,0.2f,0,1});
    CHECK(std::abs(sky[0] / sky[3] - 0.4f) < 1e-6f);
}
TEST(dlss_reprojection_rejects_singular_camera) {
    float singular[16]{}, output[16];
    const double eye[3]{};
    CHECK(!dlss_math::reprojection(singular, camera.data(), eye, output));
}
