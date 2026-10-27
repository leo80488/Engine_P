#pragma once

// LuaMathTypes: lightweight math wrappers for the Lua bindings, with the memory layout of
// XMFLOAT3 / XMFLOAT4 (reinterpret_cast safe). Pure scalar math on purpose (no
// DirectXMath include): ScriptSystem.h includes this header and must stay cheap.

#include <cmath>

struct LuaVec3
{
    float x = 0, y = 0, z = 0;
    LuaVec3() = default;
    LuaVec3(float inX, float inY, float inZ) : x(inX), y(inY), z(inZ) {}

    LuaVec3 operator+(const LuaVec3& o) const { return { x+o.x, y+o.y, z+o.z }; }
    LuaVec3 operator-(const LuaVec3& o) const { return { x-o.x, y-o.y, z-o.z }; }
    LuaVec3 operator-()                 const { return { -x, -y, -z }; }
    LuaVec3 operator*(float s)          const { return { x*s,   y*s,   z*s   }; }
    LuaVec3 operator/(float s)          const
    {
        return std::fabs(s) > 1e-12f ? LuaVec3{ x/s, y/s, z/s } : LuaVec3{};
    }
    bool    operator==(const LuaVec3& o) const { return x==o.x && y==o.y && z==o.z; }

    float   Dot(const LuaVec3& o)   const { return x*o.x + y*o.y + z*o.z; }
    LuaVec3 Cross(const LuaVec3& o) const
    {
        return { y*o.z - z*o.y, z*o.x - x*o.z, x*o.y - y*o.x };
    }
    float   LengthSq()              const { return x*x + y*y + z*z; }
    float   Length()                const { return std::sqrt(LengthSq()); }
    float   Distance(const LuaVec3& o) const { return (*this - o).Length(); }
    LuaVec3 Normalized() const
    {
        float l = Length();
        return l > 1e-6f ? LuaVec3{x/l, y/l, z/l} : LuaVec3{};
    }
    static LuaVec3 Lerp(const LuaVec3& a, const LuaVec3& b, float t)
    {
        return { a.x + (b.x - a.x) * t,
                 a.y + (b.y - a.y) * t,
                 a.z + (b.z - a.z) * t };
    }
};

struct LuaQuat
{
    float x = 0, y = 0, z = 0, w = 1;
    LuaQuat() = default;
    LuaQuat(float inX, float inY, float inZ, float inW) : x(inX), y(inY), z(inZ), w(inW) {}

    // Hamilton product q = this * o: rotates by o first, then by this (Rotate applies
    // q v q^-1). XMQuaternionMultiply(a, b) is the reverse spelling: it equals b * a here.
    LuaQuat operator*(const LuaQuat& o) const
    {
        return {
            w*o.x + x*o.w + y*o.z - z*o.y,
            w*o.y - x*o.z + y*o.w + z*o.x,
            w*o.z + x*o.y - y*o.x + z*o.w,
            w*o.w - x*o.x - y*o.y - z*o.z
        };
    }

    // Rotate a vector: v' = v + 2w cross(u, v) + 2 cross(u, cross(u, v)), u = (x, y, z).
    LuaVec3 Rotate(const LuaVec3& v) const
    {
        const LuaVec3 q{ x, y, z };
        const LuaVec3 t = q.Cross(v) * 2.f;
        return v + t * w + q.Cross(t);
    }

    float Dot(const LuaQuat& o) const { return x*o.x + y*o.y + z*o.z + w*o.w; }

    LuaQuat Normalized() const
    {
        const float l = std::sqrt(x*x + y*y + z*z + w*w);
        if (l < 1e-8f) return {};
        return { x/l, y/l, z/l, w/l };
    }

    LuaQuat Conjugate() const { return { -x, -y, -z, w }; }

    // Matches XMQuaternionRotationRollPitchYaw(pitch, yaw, roll), the convention
    // Engine.RotateTo uses. Angles in degrees (script-friendly).
    static LuaQuat FromEuler(float pitchDeg, float yawDeg, float rollDeg)
    {
        constexpr float kDeg2Rad = 0.017453292519943295f;
        const float p = pitchDeg * kDeg2Rad * 0.5f;
        const float yv = yawDeg  * kDeg2Rad * 0.5f;
        const float r = rollDeg  * kDeg2Rad * 0.5f;
        const float sp = std::sin(p),  cp = std::cos(p);
        const float sy = std::sin(yv), cy = std::cos(yv);
        const float sr = std::sin(r),  cr = std::cos(r);
        return {
            cr*sp*cy + sr*cp*sy,
            cr*cp*sy - sr*sp*cy,
            sr*cp*cy - cr*sp*sy,
            cr*cp*cy + sr*sp*sy
        };
    }

    // Axis does not need to be normalised. Angle in DEGREES.
    static LuaQuat AngleAxis(float angleDeg, const LuaVec3& axis)
    {
        constexpr float kDeg2Rad = 0.017453292519943295f;
        const LuaVec3 n = axis.Normalized();
        const float   h = angleDeg * kDeg2Rad * 0.5f;
        const float   s = std::sin(h);
        return { n.x * s, n.y * s, n.z * s, std::cos(h) };
    }

    static LuaQuat Slerp(const LuaQuat& a, const LuaQuat& bIn, float t)
    {
        if (t <= 0.f) return a;
        if (t >= 1.f) return bIn;
        LuaQuat b = bIn;
        float cosom = a.Dot(b);
        if (cosom < 0.f) { b = { -b.x, -b.y, -b.z, -b.w }; cosom = -cosom; }
        float k0, k1;
        if (cosom > 0.9995f)            // nearly parallel: nlerp
        {
            k0 = 1.f - t;
            k1 = t;
        }
        else
        {
            const float omega  = std::acos(cosom);
            const float sinom  = std::sin(omega);
            k0 = std::sin((1.f - t) * omega) / sinom;
            k1 = std::sin(t * omega) / sinom;
        }
        return LuaQuat{ a.x*k0 + b.x*k1, a.y*k0 + b.y*k1,
                        a.z*k0 + b.z*k1, a.w*k0 + b.w*k1 }.Normalized();
    }
};
