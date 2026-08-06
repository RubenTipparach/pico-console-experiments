#pragma once

// Fixed point unit quaternions, for anything whose orientation is not just a
// heading.
//
// Euler angles are fine for a camera and for anything that only ever turns
// about one axis at a time. They are not fine for a body being rotated by
// forces in its OWN frame, which is what a thruster is, and the reason is not
// only the singularity everyone names. Feed body frame rates straight into
// Euler rates and the axes are simply wrong away from zero, because an Euler
// angle turns about a frame that the other angles have already moved. Measured
// on Tom Lander, whose hull is pitched and rolled by four pods: the error in
// the pitch axis equalled the roll angle, degree for degree, all the way out
// to 90 where a pod that should have yawed the hull pitched it instead.
//
// A quaternion has no privileged axis, so a body frame rate composes onto it
// correctly at every attitude, and there is no attitude at which a degree of
// freedom is lost.
//
// Q14 throughout: 16384 is 1.0. That is the same scale sin/cos tables in this
// repo return, it holds a unit quaternion to about six decimal digits of
// angle, and every product fits an int32 once shifted back. Products are
// formed in int64 and rounded to nearest rather than truncated, because
// truncation biases in one direction and an orientation integrated a hundred
// times a second accumulates a bias into a visible drift.

#include <cstdint>

namespace pse {

constexpr int32_t k_quat_one = 16384;   // Q14

struct Quat {
    int32_t w, x, y, z;
};

// The 3x3 rotation a quaternion stands for, row major, as the renderer wants
// it: out = m * v, with m[0..2] the first row.
struct Basis {
    float m[9];
};

inline Quat quat_identity() { return Quat{k_quat_one, 0, 0, 0}; }

// Hamilton product. `a` then `b` applied in a's own frame: post multiplying by
// a small rotation turns the body about ITS axes, which is the whole reason
// this file exists. Pre multiplying would turn it about the world's.
Quat quat_mul(const Quat& a, const Quat& b);

// Back to unit length. Call it every tick after integrating: the small angle
// step below is a first order approximation, and renormalising is what pays
// for that approximation being cheap.
Quat quat_normalize(const Quat& q);

Quat quat_conjugate(const Quat& q);

// Rotate a Q14 vector into the world.
void quat_rotate(const Quat& q, int32_t vx, int32_t vy, int32_t vz,
                 int32_t& ox, int32_t& oy, int32_t& oz);

// Rotate a Q14 vector the other way, world into the body's own frame. Where
// the world's up sits in body coordinates is exactly what a levelling
// controller needs, and it is a far better error signal than a pair of Euler
// angles because it stays meaningful at every attitude.
void quat_unrotate(const Quat& q, int32_t vx, int32_t vy, int32_t vz,
                   int32_t& ox, int32_t& oy, int32_t& oz);

// One tick of body frame angular velocity, composed onto q.
//
// rx, ry, rz are the rotation THIS TICK about the body's own x, y and z, in
// Q14 radians. The step is the first order q + (q * (0,r)) / 2, which for the
// fraction of a degree a tick actually turns is far inside the fixed point
// grid, and the normalise that follows removes what error there is. No
// trigonometry, so nothing here needs a table.
Quat quat_integrate(const Quat& q, int32_t rx, int32_t ry, int32_t rz);

Basis quat_basis(const Quat& q);

// A rotation of `radians` about an axis, which does not have to be unit
// length. Float, and deliberately: this is for placing something, not for
// integrating it, so it runs at setup rather than per tick, and the per tick
// path above stays free of trigonometry.
Quat quat_from_axis_angle(float ax, float ay, float az, float radians);

}  // namespace pse
