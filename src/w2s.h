// Coffeeclaw - world to screen
#pragma once

#include "common.h"

namespace claw {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kDeg2Rad = kPi / 180.0;

struct Basis {
    Vec3 fwd, right, up;
};

inline Basis MakeBasis(const Rotator& r) {
    const double sp = std::sin(r.pitch * kDeg2Rad), cp = std::cos(r.pitch * kDeg2Rad);
    const double sy = std::sin(r.yaw * kDeg2Rad), cy = std::cos(r.yaw * kDeg2Rad);
    const double sr = std::sin(r.roll * kDeg2Rad), cr = std::cos(r.roll * kDeg2Rad);

    Basis b;
    b.fwd = Vec3(cp * cy, cp * sy, sp);
    b.right = Vec3(sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp);
    b.up = Vec3(-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp);
    return b;
}

// Projects a world point. Returns false when the point sits behind the camera.
inline bool WorldToScreen(const Vec3& world, const Vec3& cam, const Basis& basis, double fovDeg,
                          int width, int height, float& sx, float& sy) {
    const Vec3 rel = world - cam;
    const double z = rel.Dot(basis.fwd);
    if (z <= 1.0) return false;
    const double x = rel.Dot(basis.right);
    const double y = rel.Dot(basis.up);

    const double tanHalfX = std::tan((fovDeg * 0.5) * kDeg2Rad);
    if (tanHalfX <= 1e-6) return false;
    const double tanHalfY = tanHalfX * ((double)height / (double)width);

    const double ndcX = x / (z * tanHalfX);
    const double ndcY = y / (z * tanHalfY);
    sx = (float)(width * 0.5 + ndcX * width * 0.5);
    sy = (float)(height * 0.5 - ndcY * height * 0.5);
    return true;
}

} // namespace claw
