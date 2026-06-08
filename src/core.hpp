#pragma once

#include <algorithm>
#include <cmath>

namespace cloud_render {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Vec3 operator+(Vec3 a, Vec3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 operator-(Vec3 a, Vec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 operator*(Vec3 a, float s)
{
    return {a.x * s, a.y * s, a.z * s};
}

inline Vec3 operator/(Vec3 a, float s)
{
    return {a.x / s, a.y / s, a.z / s};
}

inline float dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 cross(Vec3 a, Vec3 b)
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

inline float length(Vec3 v)
{
    return std::sqrt(dot(v, v));
}

inline Vec3 normalize(Vec3 v)
{
    const float len = length(v);
    if (len <= 1.0e-8f) {
        return {0.0f, 0.0f, 1.0f};
    }
    return v / len;
}

inline constexpr Vec3 kWorldUp = {0.0f, 0.0f, 1.0f};

inline float maxComponent(Vec3 v)
{
    return std::max({v.x, v.y, v.z});
}

inline float component(Vec3 v, int axis)
{
    switch (axis) {
    case 0:
        return v.x;
    case 1:
        return v.y;
    default:
        return v.z;
    }
}

inline void setComponent(Vec3& v, int axis, float value)
{
    switch (axis) {
    case 0:
        v.x = value;
        break;
    case 1:
        v.y = value;
        break;
    default:
        v.z = value;
        break;
    }
}

inline const char* axisName(int axis)
{
    switch (axis) {
    case 0:
        return "X";
    case 1:
        return "Y";
    default:
        return "Z";
    }
}

inline float volumeMoveScale(Vec3 worldMin, Vec3 worldMax)
{
    const Vec3 extent = worldMax - worldMin;
    return std::max(maxComponent(extent), 1.0f);
}

} // namespace cloud_render
