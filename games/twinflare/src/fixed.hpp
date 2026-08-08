#pragma once

#include <cstdint>

#include "tuning.hpp"

// Fixed point maths for the sim. The engine deliberately carries none of this:
// pse::Quat is Q14 quaternions and nothing else, and renderer3d.hpp notes that
// every game brings its own trig. So here is Twin Flare's.
//
// Angles are brads, 65536 to a full turn, which is worth the small strangeness
// of the unit: wrapping is what an int32 already does at the top of its range,
// so an angle can never drift out of domain and there is no fmod anywhere.

namespace twinflare {

// sin, in Q14. Interpolated between table entries, which costs one multiply
// and buys about eight times the accuracy of the raw table: without it a slow
// yaw visibly staircases, because a pod turning at 170 brads a tick sits on
// the same table entry for a second and a half.
int32_t fsin(int32_t brads);
inline int32_t fcos(int32_t brads) { return fsin(brads + k_turn / 4); }

// Arc tangent in brads, the full circle, arguments in the usual atan2 order.
// A heading measured from the +Z axis, which is what the track and the rivals
// want, is therefore fatan2(dx, dz). Never called in a per pixel path.
int32_t fatan2(int32_t y, int32_t x);

// Integer square root, for speeds. Newton, seeded from the bit length, which
// converges in about five iterations for the magnitudes here.
uint32_t fsqrt(uint32_t value);

// Length of a 2D vector, in the same fixed point as its inputs.
int32_t flength(int32_t x, int32_t z);

// a * b >> k_fp, keeping the intermediate in 64 bits. Two fp16 values multiply
// to fp32, which overflows an int32 the moment either exceeds about 32,768,
// and a track coordinate does that constantly.
inline int32_t fmul(int32_t a, int32_t b) {
    return static_cast<int32_t>((static_cast<int64_t>(a) * b) >> k_fp);
}

// a * numerator / 1000, for the thousandths multipliers the tuning header is
// full of. Kept in 64 bits for the same reason as fmul.
inline int32_t fscale(int32_t a, int32_t thousandths) {
    return static_cast<int32_t>((static_cast<int64_t>(a) * thousandths) / 1000);
}

// a * sin/cos, where the trig value is Q14.
inline int32_t ftrig(int32_t a, int32_t trig) {
    return static_cast<int32_t>((static_cast<int64_t>(a) * trig) >> k_trig_fp);
}

inline int32_t clamp32(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// The shortest way round from one angle to another, in brads. Free, because
// brads wrap with the integer.
inline int32_t angle_diff(int32_t to, int32_t from) {
    return static_cast<int32_t>(static_cast<int16_t>(
        static_cast<uint16_t>((to - from) & 0xFFFF)));
}

}  // namespace twinflare
