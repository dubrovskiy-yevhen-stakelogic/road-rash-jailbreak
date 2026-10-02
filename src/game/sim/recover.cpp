#include "game/sim/recover.h"

#include "game/sim/vec.h"

namespace rr::sim {

BikeTables RecoverTables(uint8_t* ram, GuestRam& g) {
    auto at = [ram](uint32_t a) -> uint8_t* {
        if (a < 0x80000000u || a >= 0x80200000u) return nullptr;
        return ram + (a - 0x80000000u);
    };
    BikeTables t;
    t.sincos = reinterpret_cast<const int16_t*>(at(kRcSinCos));
    t.asin = reinterpret_cast<const uint16_t*>(at(0x800527E0u));
    t.atan = reinterpret_cast<const int32_t*>(at(0x8005285Cu));
    t.rsqrt = reinterpret_cast<const uint16_t*>(at(g.U32(g.gp() + 2260u)));
    t.sqrt = reinterpret_cast<const int16_t*>(at(0x800560CCu));
    return t;
}

namespace rc {

bool IsPlayerRider(GuestRam& g, uint32_t h) {
    const uint32_t gs = g.U32(kRcGameStatePtr);
    return (h >> 5) == 1u && S(h & 0x1Fu) < g.S32(gs + 48u);
}

bool IsPlayerBike(GuestRam& g, uint32_t h) {
    const uint32_t gs = g.U32(kRcGameStatePtr);
    return h < g.U32(gs + 48u);
}

void GteOp(GuestRam& g, uint32_t d, uint32_t ir, uint32_t out) {
    int16_t dv[3], iv[3], o[3];
    for (uint32_t k = 0; k < 3; ++k) {
        dv[k] = g.S16(d + 2u * k);
        iv[k] = g.S16(ir + 2u * k);
    }
    OuterProduct(dv, iv, o);
    for (uint32_t k = 0; k < 3; ++k) g.W16(out + 2u * k, static_cast<uint16_t>(o[k]));
}

bool GNormalize(GuestRam& g, uint32_t v, const BikeTables& t, int32_t& sum) {
    int16_t x[3];
    for (uint32_t k = 0; k < 3; ++k) x[k] = g.S16(v + 2u * k);    // SQR (sf 0) then two TRAPPING adds: MAC1 + MAC2, then + MAC3 (0x8002E4A0 / 0x8002E4A4).
    const int64_t s12 = static_cast<int64_t>(x[0]) * x[0] + static_cast<int64_t>(x[1]) * x[1];
    if (s12 > INT32_MAX) return false;
    const int64_t s = s12 + static_cast<int64_t>(x[2]) * x[2];
    if (s > INT32_MAX) return false;
    if (!Normalize(x, t.rsqrt)) return false;
    for (uint32_t k = 0; k < 3; ++k) g.W16(v + 2u * k, static_cast<uint16_t>(x[k]));
    sum = static_cast<int32_t>(s);
    return true;
}

void CopyHalfwords(GuestRam& g, int32_t n, uint32_t src, uint32_t dst) {
    for (; n > 0; --n) {
        g.W16(dst, g.U16(src));
        src += 2u;
        dst += 2u;
    }
}

int32_t Recip(int32_t x) {
    const int32_t m = x < 0 ? S(0u - U(x)) : x;
    const uint32_t d = U((m >> 1) + (S(U(m) - 2u) >> 31));
    const uint32_t q = d == 0u ? 0xFFFFFFFFu : 0x80000000u / d;
    return x < 0 ? S(0u - q) : S(q);
}

} // namespace rc
} // namespace rr::sim
