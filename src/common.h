// Coffeeclaw - common definitions
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace claw {

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
    Vec3() = default;
    Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator*(double s) const { return Vec3(x * s, y * s, z * s); }
    double Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    double Length() const { return std::sqrt(Dot(*this)); }
};

struct Rotator {
    double pitch = 0.0; // degrees
    double yaw = 0.0;   // degrees
    double roll = 0.0;  // degrees
};

inline std::string Narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)need, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], need, nullptr, nullptr);
    return out;
}

struct Entity {
    std::wstring name;
    Vec3 world;
    double distanceM = 0.0;
    double heightCm = 100.0;
    float widthRatio = 0.62f;
    std::vector<Vec3> bones;
    std::vector<int32_t> boneParents;
};

struct Snapshot {
    std::vector<Entity> entities;
    bool attached = false;
    bool worldResolved = false;
    std::wstring status;
    Vec3 camera;
    Rotator cameraRot;
    double fov = 90.0;
};

} // namespace claw



