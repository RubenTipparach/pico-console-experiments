#include <cmath>

#include "pse/quat.hpp"

namespace pse {
namespace {

// Q14 product, rounded to nearest. The int64 is not caution for its own sake:
// two Q14 values near 1.0 multiply to nearly 2^28, and quat_mul sums four of
// those before shifting, which is 2^30 and inside int32 only by luck. Round
// rather than truncate: a shift on a negative truncates toward minus infinity,
// so a truncating quaternion integrator drifts one way and only one way.
inline int32_t mul14(int32_t a, int32_t b) {
    return static_cast<int32_t>(
        (static_cast<int64_t>(a) * b + (1 << 13)) >> 14);
}

// Integer square root, for the normalise. No floats: this runs on a chip with
// no FPU, every tick, and sqrtf would be a software call each time.
uint32_t isqrt(uint64_t value) {
    if (value == 0) return 0;
    uint64_t x = value;
    uint64_t bit = 1ULL << 62;
    while (bit > x) bit >>= 2;
    uint64_t root = 0;
    while (bit != 0) {
        if (x >= root + bit) {
            x -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return static_cast<uint32_t>(root);
}

}  // namespace

Quat quat_mul(const Quat& a, const Quat& b) {
    Quat r;
    r.w = mul14(a.w, b.w) - mul14(a.x, b.x) - mul14(a.y, b.y) - mul14(a.z, b.z);
    r.x = mul14(a.w, b.x) + mul14(a.x, b.w) + mul14(a.y, b.z) - mul14(a.z, b.y);
    r.y = mul14(a.w, b.y) - mul14(a.x, b.z) + mul14(a.y, b.w) + mul14(a.z, b.x);
    r.z = mul14(a.w, b.z) + mul14(a.x, b.y) - mul14(a.y, b.x) + mul14(a.z, b.w);
    return r;
}

Quat quat_conjugate(const Quat& q) { return Quat{q.w, -q.x, -q.y, -q.z}; }

Quat quat_normalize(const Quat& q) {
    const int64_t sq = static_cast<int64_t>(q.w) * q.w +
                       static_cast<int64_t>(q.x) * q.x +
                       static_cast<int64_t>(q.y) * q.y +
                       static_cast<int64_t>(q.z) * q.z;
    if (sq <= 0) return quat_identity();
    // sq is Q28, so its root is Q14 and is the length directly.
    const int32_t len = static_cast<int32_t>(isqrt(static_cast<uint64_t>(sq)));
    if (len <= 0) return quat_identity();
    Quat r;
    r.w = static_cast<int32_t>(
        (static_cast<int64_t>(q.w) * k_quat_one + len / 2) / len);
    r.x = static_cast<int32_t>(
        (static_cast<int64_t>(q.x) * k_quat_one + len / 2) / len);
    r.y = static_cast<int32_t>(
        (static_cast<int64_t>(q.y) * k_quat_one + len / 2) / len);
    r.z = static_cast<int32_t>(
        (static_cast<int64_t>(q.z) * k_quat_one + len / 2) / len);
    return r;
}

void quat_rotate(const Quat& q, int32_t vx, int32_t vy, int32_t vz,
                 int32_t& ox, int32_t& oy, int32_t& oz) {
    // v + 2 * (w * (qv x v) + qv x (qv x v)). Two cross products beat building
    // the matrix when only one vector is being turned.
    const int32_t ux = mul14(q.y, vz) - mul14(q.z, vy);
    const int32_t uy = mul14(q.z, vx) - mul14(q.x, vz);
    const int32_t uz = mul14(q.x, vy) - mul14(q.y, vx);

    const int32_t wx = mul14(q.y, uz) - mul14(q.z, uy);
    const int32_t wy = mul14(q.z, ux) - mul14(q.x, uz);
    const int32_t wz = mul14(q.x, uy) - mul14(q.y, ux);

    ox = vx + 2 * (mul14(q.w, ux) + wx);
    oy = vy + 2 * (mul14(q.w, uy) + wy);
    oz = vz + 2 * (mul14(q.w, uz) + wz);
}

void quat_unrotate(const Quat& q, int32_t vx, int32_t vy, int32_t vz,
                   int32_t& ox, int32_t& oy, int32_t& oz) {
    quat_rotate(quat_conjugate(q), vx, vy, vz, ox, oy, oz);
}

Quat quat_integrate(const Quat& q, int32_t rx, int32_t ry, int32_t rz) {
    // The pure quaternion (0, r/2), composed on the RIGHT so the rates are
    // read in the body's own frame.
    const Quat half{0, rx / 2, ry / 2, rz / 2};
    const Quat d = quat_mul(q, half);
    return quat_normalize(Quat{q.w + d.w, q.x + d.x, q.y + d.y, q.z + d.z});
}

Quat quat_from_axis_angle(float ax, float ay, float az, float radians) {
    const float len = std::sqrt(ax * ax + ay * ay + az * az);
    if (len < 1e-6f) return quat_identity();
    ax /= len; ay /= len; az /= len;
    const float half = radians * 0.5f;
    const float s = std::sin(half);
    const float k = static_cast<float>(k_quat_one);
    return quat_normalize(Quat{static_cast<int32_t>(std::cos(half) * k),
                               static_cast<int32_t>(ax * s * k),
                               static_cast<int32_t>(ay * s * k),
                               static_cast<int32_t>(az * s * k)});
}

Basis quat_basis(const Quat& q) {
    const int32_t xx = mul14(q.x, q.x), yy = mul14(q.y, q.y);
    const int32_t zz = mul14(q.z, q.z);
    const int32_t xy = mul14(q.x, q.y), xz = mul14(q.x, q.z);
    const int32_t yz = mul14(q.y, q.z);
    const int32_t wx = mul14(q.w, q.x), wy = mul14(q.w, q.y);
    const int32_t wz = mul14(q.w, q.z);

    constexpr float k = 1.0f / static_cast<float>(k_quat_one);
    Basis b;
    b.m[0] = 1.0f - 2.0f * (yy + zz) * k;
    b.m[1] = 2.0f * (xy - wz) * k;
    b.m[2] = 2.0f * (xz + wy) * k;
    b.m[3] = 2.0f * (xy + wz) * k;
    b.m[4] = 1.0f - 2.0f * (xx + zz) * k;
    b.m[5] = 2.0f * (yz - wx) * k;
    b.m[6] = 2.0f * (xz - wy) * k;
    b.m[7] = 2.0f * (yz + wx) * k;
    b.m[8] = 1.0f - 2.0f * (xx + yy) * k;
    return b;
}

}  // namespace pse
