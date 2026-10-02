#include "game/sim/input.h"

namespace rr::sim {
namespace {

inline int32_t Add32(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}
inline int32_t Sub32(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}
inline uint16_t LoadU16(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8));
}

// R3000A `div`: no exception. A zero divisor gives lo = (dividend >= 0) ? -1 : 1; the one other
// special case is INT32_MIN / -1, which gives lo = INT32_MIN.
inline int32_t MipsDiv(int32_t n, int32_t d) {
    if (d == 0) return (n >= 0) ? -1 : 1;
    if (d == -1 && n == static_cast<int32_t>(0x80000000)) return n;
    return n / d;
}

} // namespace

int32_t AxisCurve(uint8_t raw, const uint16_t* curve, const uint8_t* cfg) {
    // 0x8001CA58..0x8001CA68.
    const int32_t d = Sub32(static_cast<int32_t>(raw), 127);
    const int32_t t = (d >= 0) ? d : Sub32(1, d);

    // 0x8001CA6C..0x8001CA80: the dead zone and the segment width.
    const int32_t dead = static_cast<int32_t>(LoadU16(cfg + 176));
    if (!(dead < t)) return 0;                 // 0x8001CB2C
    const int32_t width = static_cast<int32_t>(LoadU16(cfg + 178));

    // 0x8001CA88..0x8001CAB8: walk at most four segments.
    int32_t edge = Add32(dead, width);
    const uint16_t* last = curve + 4;          // `t1 = a1 + 8`
    if (edge < t) {
        while (curve < last) {
            edge = Add32(edge, width);
            curve += 1;
            if (!(edge < t)) break;
        }
    }

    // 0x8001CABC..0x8001CAF4.
    const int32_t lo = static_cast<int32_t>(curve[0]);
    const int32_t span = Sub32(static_cast<int32_t>(curve[1]), lo);
    int32_t num;
    int32_t den = width;
    if (curve < last) {
        num = Sub32(t, Sub32(edge, width));
    } else {
        den = Sub32(width, Sub32(edge, 128));
        if (!(t < 128)) return (d >= 0) ? 0x10000 : -0x10000; // 0x8001CB18, saturated
        num = Add32(Sub32(t, 128), den);
    }

    // 0x8001CAF8..0x8001CB14: a 32-bit `mult`/`mflo` then a real `div`.
    const int32_t prod = static_cast<int32_t>(static_cast<uint32_t>(
        static_cast<uint64_t>(static_cast<int64_t>(num) * span) & 0xFFFFFFFFu));
    const int32_t v = Add32(lo, MipsDiv(prod, den));
    return (d >= 0) ? v : static_cast<int32_t>(0u - static_cast<uint32_t>(v));
}

} // namespace rr::sim
