#pragma once

#include <algorithm>
#include <cmath>

#include <DirectXMath.h>

namespace cloud_render {

using Vec3 = DirectX::XMFLOAT3;
using Matrix4 = DirectX::XMFLOAT4X4;

inline DirectX::XMVECTOR loadVec3(Vec3 v)
{
    return DirectX::XMLoadFloat3(&v);
}

inline Vec3 storeVec3(DirectX::FXMVECTOR v)
{
    Vec3 result;
    DirectX::XMStoreFloat3(&result, v);
    return result;
}

inline Vec3 operator+(Vec3 a, Vec3 b)
{
    return storeVec3(DirectX::XMVectorAdd(loadVec3(a), loadVec3(b)));
}

inline Vec3 operator-(Vec3 a, Vec3 b)
{
    return storeVec3(DirectX::XMVectorSubtract(loadVec3(a), loadVec3(b)));
}

inline Vec3 operator*(Vec3 a, float s)
{
    return storeVec3(DirectX::XMVectorScale(loadVec3(a), s));
}

inline Vec3 operator/(Vec3 a, float s)
{
    return storeVec3(DirectX::XMVectorScale(loadVec3(a), 1.0f / s));
}

inline float dot(Vec3 a, Vec3 b)
{
    return DirectX::XMVectorGetX(DirectX::XMVector3Dot(loadVec3(a), loadVec3(b)));
}

inline Vec3 cross(Vec3 a, Vec3 b)
{
    return storeVec3(DirectX::XMVector3Cross(loadVec3(a), loadVec3(b)));
}

inline float length(Vec3 v)
{
    return DirectX::XMVectorGetX(DirectX::XMVector3Length(loadVec3(v)));
}

inline Vec3 normalize(Vec3 v)
{
    const DirectX::XMVECTOR vector = loadVec3(v);
    const float lengthSquared = DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(vector));
    if (lengthSquared <= 1.0e-16f) {
        return {0.0f, 0.0f, 1.0f};
    }
    return storeVec3(DirectX::XMVector3Normalize(vector));
}

inline const Vec3 kWorldUp = {0.0f, 0.0f, 1.0f};

inline Matrix4 identityMatrix4()
{
    Matrix4 matrix;
    DirectX::XMStoreFloat4x4(&matrix, DirectX::XMMatrixIdentity());
    return matrix;
}

inline Matrix4 zeroMatrix4()
{
    return {};
}

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
