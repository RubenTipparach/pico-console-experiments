#include "fixed.hpp"

namespace twinflare {
namespace {

// Built at compile time rather than typed, and rather than computed at boot.
// constexpr means it lands in flash as a const table, which is 514 bytes of
// the resource this machine has plenty of, instead of 514 bytes of the one it
// does not.
//
// A Taylor series is used because <cmath> is not constexpr and the table has
// to exist at compile time. Nine terms is enough: over a quarter turn the
// worst error is under one part in 30,000, which is below the Q14 step.
constexpr double series_sin(double x) {
    double term = x, sum = x;
    for (int n = 1; n < 10; ++n) {
        term *= -x * x / ((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

constexpr double k_pi = 3.14159265358979323846;

struct SinTable {
    int16_t v[257];
};

constexpr SinTable build_sin() {
    SinTable t{};
    for (int i = 0; i <= 256; ++i) {
        const double a = (k_pi / 2.0) * i / 256.0;
        double s = series_sin(a) * k_trig_one;
        // Round to nearest without <cmath>, which is not constexpr.
        t.v[i] = static_cast<int16_t>(s < 0 ? s - 0.5 : s + 0.5);
    }
    return t;
}

// The table itself, in flash. Nothing outside this file needs it: fsin is
// already an out of line call, so exposing the array would only invite a
// second consumer to index it with a different convention.
constexpr SinTable k_table = build_sin();

}  // namespace

int32_t fsin(int32_t brads) {
    // 16384 brads to a quarter turn, 256 table entries, so 64 brads a step and
    // the remainder is the interpolation weight.
    uint32_t a = static_cast<uint32_t>(brads) & 0xFFFF;
    const bool negate = a >= 0x8000;
    if (negate) a -= 0x8000;
    if (a >= 0x4000) a = 0x8000 - a;   // mirror the second quarter

    const uint32_t index = a >> 6;
    const uint32_t frac = a & 63;
    const int32_t lo = k_table.v[index];
    const int32_t hi = k_table.v[index + 1];
    const int32_t v = lo + ((hi - lo) * static_cast<int32_t>(frac) >> 6);
    return negate ? -v : v;
}

uint32_t fsqrt(uint32_t value) {
    if (value == 0) return 0;
    // Seed from the bit length: a power of two at half the exponent is within
    // a factor of root two, so Newton lands in about five passes and never
    // spins on a value it cannot represent.
    uint32_t bit = 0;
    for (uint32_t v = value; v; v >>= 2) bit += 1;
    uint32_t x = 1u << bit;
    for (int i = 0; i < 8; ++i) {
        const uint32_t next = (x + value / x) >> 1;
        if (next >= x) break;
        x = next;
    }
    return x;
}

int32_t flength(int32_t x, int32_t z) {
    // Squared in 64 bits, because two fp16 values of a few hundred units
    // square well past an int32.
    const int64_t sq = static_cast<int64_t>(x) * x + static_cast<int64_t>(z) * z;
    if (sq <= 0) return 0;
    // Shift down to a magnitude fsqrt can take, root it, shift the answer back
    // by half. Even shifts only, so the halving is exact.
    int shift = 0;
    int64_t v = sq;
    while (v > 0x7FFFFFFF) { v >>= 2; shift += 1; }
    return static_cast<int32_t>(fsqrt(static_cast<uint32_t>(v))) << shift;
}

int32_t fatan2(int32_t y, int32_t x) {
    if (x == 0 && y == 0) return 0;
    const int32_t ax = x < 0 ? -x : x;
    const int32_t ay = y < 0 ? -y : y;

    // One octant by the standard rational approximation, then reflected out.
    //
    //   atan(r) ~= r*pi/4 - r*(r-1)*(0.2447 + 0.0663*r)   for r in [0,1]
    //
    // in brads, where a quarter turn is 16384 and one radian is 10430:
    // pi/4 becomes 8192, 0.2447 becomes 2552, 0.0663 becomes 691. Worst error
    // is about 16 brads, a tenth of a degree.
    //
    // The version this replaced had the correction term algebraically wrong
    // and was out by nine and a half degrees, which nothing in the game would
    // have crashed on: the rivals would simply have driven at a slight angle
    // to the road forever.
    const int32_t lo = ay <= ax ? ay : ax;
    const int32_t hi = ay <= ax ? ax : ay;
    const int64_t r = (static_cast<int64_t>(lo) << 16) / (hi == 0 ? 1 : hi);
    const int64_t inner = 2552 + ((691 * r) >> 16);
    const int64_t bend = (((r * (r - 65536)) >> 16) * inner) >> 16;
    int32_t angle = static_cast<int32_t>(((r * 8192) >> 16) - bend);
    if (ay > ax) angle = 16384 - angle;

    if (x < 0) angle = 32768 - angle;
    if (y < 0) angle = -angle;
    return angle;
}

}  // namespace twinflare
