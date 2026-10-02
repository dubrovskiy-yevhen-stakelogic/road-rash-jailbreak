// The hazard objects (hazard.h), line by line from our own listings of RASHCDI.BIN (SHA-1
// 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06), RASHCDG.BIN (SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c)
// and SLUS_010.53 (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1). The addresses beside the statements
// are the original's instructions.
#include "game/sim/hazard.h"

#include <cstring>

#include "game/sim/bike_step.h"      // RotateRowPair SLUS 0x8002ED94
#include "game/sim/fixed.h"          // FixMul, FixDiv, RatAtan2
#include "game/sim/recover_walk.h"   // VecMat SLUS 0x8002EFF4
#include "game/sim/road_runtime.h"   // RouteFindLegView 0x8003B4B0, MulAddView 0x8002EAD8
#include "game/sim/traffic_bind.h"   // ModelBind SLUS 0x8002FAD4
#include "game/sim/traffic_drive.h"  // RoadStep SLUS 0x8003775C
#include "game/sim/vec.h"            // Blend16To32

namespace rr::sim {
namespace {

constexpr uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
constexpr int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
int32_t Neg(int32_t v) { return S(0u - U(v)); }
int32_t Add(int32_t a, int32_t b) { return S(U(a) + U(b)); }
int32_t Sub(int32_t a, int32_t b) { return S(U(a) - U(b)); }
int32_t Iabs(int32_t v) { return v < 0 ? Neg(v) : v; }

constexpr uint32_t kEnvGuest = 0x8006B8B8; // where the loader reads ENV.EN (the objects' source 0x8006BA50)

// The sign pattern every divide here uses: FixDiv of the magnitudes (a "positive" operand is > 0),
// negated when exactly one operand is not positive.
int32_t SDiv(int32_t a, int32_t b) {
    if (a > 0) return b > 0 ? S(FixDiv(U(a), U(b))) : Neg(S(FixDiv(U(a), U(Neg(b)))));
    return b > 0 ? Neg(S(FixDiv(U(Neg(a)), U(b)))) : S(FixDiv(U(Neg(a)), U(Neg(b))));
}
// The octagonal distance of two whole-unit magnitudes (the `sra/addu/xor` absolute values made by the
// callers), larger first after the swap.
int32_t Octagon(int32_t a, int32_t b) {
    if (a < b) {
        const int32_t t = a;
        a = b;
        b = t;
    }
    const int32_t m = Add(b, b >> 1);
    return Add(Add(Sub(Sub(a, a >> 5), a >> 7), m >> 2), m >> 6);
}
// The branch-free clamp the compiler emitted: s + ((s - lo) >> 31 & (lo - s)) + ((hi - s) >> 31 & (hi - s)),
// both halves from the UNCLAMPED s (0x800A1094..0x800A10C0, 0x800A1118..0x800A1160, 0x800A1480..0x800A14A8).
int32_t Clamp2(int32_t s, int32_t lo, int32_t hi) {
    const int32_t a = Add(s, S(U(Sub(s, lo) >> 31) & U(Sub(lo, s))));
    const int32_t b = S(U(Sub(hi, s) >> 31) & U(Sub(hi, s)));
    return Add(a, b);
}
uint32_t MulHi(uint32_t a, uint32_t b) { return static_cast<uint32_t>((static_cast<uint64_t>(a) * b) >> 32); }

// RASHCDG 0x80086C00 CameraAngleChase(angle, target, dt, rate) - the same body as camera.cpp's port.
void AngleChase(GuestRam& g, uint32_t angle, int32_t target, int32_t dt, int32_t rate) {
    const int32_t p0 = g.S32(angle);
    const int32_t d0 = Sub(target, p0);
    const int32_t wrap = (d0 < -2048) ? -4096 : 0;
    g.W32(angle, U((d0 >= 2049) ? Add(Add(p0, 4096), wrap) : Add(p0, wrap)));
    const int32_t p = g.S32(angle);
    const int32_t d = Sub(target, p);
    int32_t step = 0;
    if (rate != 0) step = FixMul(FixMul(rate, S(U(d) << 16)), dt) >> 16;
    int32_t a0;
    if (Iabs(step) >= 3) a0 = Add(step, p);
    else if (d > 0) a0 = Add(p, 2);
    else if (d < 0) a0 = Sub(p, 2);
    else a0 = p;
    const bool clamp = (a0 < target && !(p < target)) || (!(a0 < target) && !(target < p) && target < a0);
    g.W32(angle, U(clamp ? target : a0));
}

void Get9(GuestRam& g, uint32_t a, int16_t m[9]) {
    for (uint32_t k = 0; k < 9; ++k) m[k] = g.S16(a + 2u * k);
}
void Put9(GuestRam& g, uint32_t a, const int16_t m[9]) {
    for (uint32_t k = 0; k < 9; ++k) g.W16(a + 2u * k, static_cast<uint16_t>(m[k]));
}
void Get3(GuestRam& g, uint32_t a, int16_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) v[k] = g.S16(a + 2u * k);
}
void Put3(GuestRam& g, uint32_t a, const int16_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) g.W16(a + 2u * k, static_cast<uint16_t>(v[k]));
}
// GTE OP (sf 1, lm 0) with the rotation diagonal `d` and IR `ir`, stored at `out` (the inline
// `ctc2 $0/$2/$4; mtc2 $9..$11; cop2 0x178000C; mfc2` blocks).
void Op(GuestRam& g, uint32_t d, uint32_t ir, uint32_t out) {
    int16_t a[3], b[3], o[3];
    Get3(g, d, a);
    Get3(g, ir, b);
    OuterProduct(a, b, o);
    Put3(g, out, o);
}
bool Norm16(GuestRam& g, uint32_t v, const BikeTables& t) { // SLUS 0x8002E468
    int32_t sum = 0;
    return rc::GNormalize(g, v, t, sum);
}
int32_t EnvS8(const HazardLoaderData& d, size_t at, bool& ok) {
    if (at >= d.envSize) {
        ok = false;
        return 0;
    }
    return static_cast<int8_t>(d.env[at]);
}

} // namespace

// ============================================================================ RASHCDI 0x8006AD4C
bool HazardPick(GuestRam& g, const uint8_t* env, size_t envSize, int32_t& sel, int32_t& v0) {
    if (envSize < 0xCDu + 2u * 128u) return false;
    const int32_t count = static_cast<int8_t>(env[204]);
    auto pair = [&](int32_t s, int32_t k) -> int32_t {
        const size_t at = 205u + 2u * static_cast<size_t>(s) + static_cast<size_t>(k);
        return at < envSize ? static_cast<int8_t>(env[at]) : 0;
    };
    if (count == 0) {
        sel = 0;                                                               // 0x8006AD74
    } else {
        const uint32_t r = GuestRand(g);                                       // 0x8006AD84
        sel = S(r % U(count));                                                 // 0x8006AD94 divu
    }
    if (g.S8(kHzTable) >= 0) {                                                 // 0x8006ADA8
        int32_t n = 0;                                                         // the named sets
        for (uint32_t a = kHzTable; n < 2; a += 4u) {
            if (g.S8(a) < 0) break;
            ++n;
        }
        int32_t t0 = sel + 1;                                                  // 0x8006ADF0
        if (t0 == count) t0 = 0;
        while (t0 != sel) {                                                    // 0x8006AE00 / 0x8006AE94
            if (t0 < -128 || t0 > 127) return false; // OURS: the pair index stays in the byte range read
            int32_t matched = 0;
            for (int32_t k = 0; k < n; ++k) {                                  // 0x8006AE24
                const int32_t cls = g.S8(kHzTable + 4u * U(k));
                for (int32_t j = 0; j < 2; ++j)
                    if (cls == pair(t0, j)) {
                        ++matched;
                        break;
                    }
            }
            if (matched == n) {                                                // 0x8006AE6C
                sel = t0;
                break;
            }
            t0 = (t0 + 1 == count) ? 0 : t0 + 1;                               // 0x8006AE70..0x8006AE90
        }
    }
    if (sel < -128 || 2 * sel + 205 >= static_cast<int32_t>(envSize) || 2 * sel + 205 < 0) return false;
    const int32_t a1 = pair(sel, 0);                                           // 0x8006AEBC
    v0 = a1 < 1 ? 1 : (a1 > 5 ? 5 : a1);                                      // 0x8006AEC0..0x8006AEE8
    return !g.Faulted();
}

// ============================================================================ RASHCDI 0x8006AEF4
bool HazardSetup(GuestRam& g, const HazardLoaderData& d, uint32_t sp, RecoverCallees& c) {
    const uint32_t F = sp - 112u;
    const uint32_t list = F + 16u;
    bool ok = true;
    {   // a free class-table entry takes {9, -100, 0} (0x8006AF30..0x8006AF70)
        int32_t s1 = 0;
        uint32_t a = kHzTable;
        for (; s1 < 2; ++s1, a += 4u)
            if (g.S8(a) < 0) break;
        if (s1 < 2) {
            g.W8(a, 9);
            g.W8(a + 1u, 0x9C);
            g.W16(a + 2u, 0);
        }
    }
    g.W32(kHzObjectCount, 0);                                                  // 0x8006AF84
    uint32_t events = 0;
    if (!rc::Call(c, kHzMallocFn, {48u, 0u}, F, &events)) return false;       // 0x8006AF80
    g.W32(kHzEventsPtr, events);                                               // 0x8006AFA4
    g.W32(kHzEventCount, 0);                                                   // 0x8006AFAC
    for (int32_t s6 = 0; s6 < 2; ++s6) {
        const int32_t s5 = EnvS8(d, 205u + 2u * U(d.sel) + U(s6), ok);         // 0x8006AFC8
        if (!ok) return false;
        if (!(U(s5) < 6u)) break;                                              // 0x8006AFD4
        int32_t window = 0;                                                    // sp+40
        int32_t s1 = 0, s3 = -1, a1 = 0;
        uint32_t a0 = kHzTable;
        for (; s1 < 2; ++s1, a0 += 4u) {                                       // 0x8006AFF0
            const int32_t v = g.S8(a0);
            if (v < 0) break;
            if (v == s5) {                                                     // 0x8006B06C
                const int32_t slot = g.S8(a0 + 1u);
                window = g.S16(a0 + 2u);
                a1 = slot < 0 ? 1 : 0;
                s3 = Iabs(slot);
                break;
            }
        }
        if (a1 != 0) {                                                         // 0x8006B01C: a fixed object
            if (!(s3 < 6)) continue;
            const uint32_t n = g.U32(kHzObjectCount);
            g.W32(list + 4u * n, U(s3));                                       // 0x8006B048
            g.W8(kHzTable + 4u * U(s1) + 1u, static_cast<uint8_t>(n));         // 0x8006B054
            g.W32(kHzObjectCount, n + 1u);
            continue;
        }
        const int32_t eventCount = d.eventCount;                               // 0x8006B098
        for (int32_t s4 = 0; s4 < eventCount; ++s4) {                          // 0x8006B0C0
            const uint32_t s2 = g.U32(kHzEventsPtr) + 16u * g.U32(kHzEventCount);
            const uint8_t* s0 = eventCount != 0 && s4 < 3 ? d.events + 16 * s4 : nullptr; // 0x8006B0D4
            if (eventCount != 0 && s4 >= 3) return false; // OURS: the loader's BSS holds three templates
            auto e32 = [s0](int o) {
                int32_t v;
                std::memcpy(&v, s0 + o, 4);
                return v;
            };
            auto e16 = [s0](int o) {
                int16_t v;
                std::memcpy(&v, s0 + o, 2);
                return v;
            };
            g.W8(s2 + 8u, s5 != 0 ? 0u : 9u);                                  // 0x8006B0F4
            g.W8(s2 + 12u, 1);
            uint16_t w = static_cast<uint16_t>(window);                        // 0x8006B110
            if (s0 != nullptr && e16(14) >= 0) w = static_cast<uint16_t>(e16(14));
            g.W16(s2 + 14u, w);
            if (s0 != nullptr) {
                g.W32(s2, U(e32(0)));                                          // 0x8006B140
                const uint32_t r = GuestRand(g);                               // 0x8006B144
                const int32_t span = Sub(e32(8), e32(4)) >> 16;
                const uint32_t hi = U(span) == 0 ? r : r % U(span);            // divu: hi = dividend on zero
                g.W32(s2 + 4u, U(Add(e32(4) >> 16, S(hi))) << 16);             // 0x8006B168..0x8006B174
            } else {
                const uint32_t bike = g.U32(kHzBikePtr);                       // 0x8006B194
                g.W32(s2, g.U32(bike + 360u));
                g.W32(s2 + 4u, g.U32(bike + 368u));
            }
            const int32_t variants = EnvS8(d, 221u + U(s5), ok);               // 0x8006B1C0
            if (!ok) return false;
            if (s0 != nullptr && e16(12) >= 0) s3 = e16(12);                   // 0x8006B1CC
            if (s3 < 0 || !(s3 < variants)) {                                  // 0x8006B1E0
                const uint32_t r = GuestRand(g);
                s3 = S(U(variants) == 0 ? r : r % U(variants));                // 0x8006B1F8 divu
            }
            for (int32_t k = 0; k < 3; ++k) {                                  // 0x8006B21C
                const int64_t at = 227 + 30 * static_cast<int64_t>(s5) + 3 * static_cast<int64_t>(s3) + k;
                if (at < 0) return false;
                const int32_t obj = EnvS8(d, static_cast<size_t>(at), ok);
                if (!ok) return false;
                if (obj < 0) {                                                 // 0x8006B184
                    g.W8(s2 + 9u + U(k), 0xFF);
                    break;
                }
                g.W8(s2 + 9u + U(k), g.U8(kHzObjectCount));                    // 0x8006B240
                const uint32_t n = g.U32(kHzObjectCount);
                g.W32(list + 4u * n, U(obj));                                  // 0x8006B25C
                g.W32(kHzObjectCount, n + 1u);
            }
            g.W32(kHzEventCount, g.U32(kHzEventCount) + 1u);                   // 0x8006B27C
        }
    }
    uint32_t objs = 0;
    if (!rc::Call(c, kHzMallocFn, {1872u, 0u}, F, &objs)) return false;      // 0x8006B2A0
    g.W32(kHzObjectsPtr, objs);                                                // 0x8006B2BC
    int32_t s1 = 0;
    for (; s1 < g.S32(kHzObjectCount); ++s1) {                                 // 0x8006B2D4
        const int32_t v = g.S32(list + 4u * U(s1));
        const uint32_t dst = g.U32(kHzObjectsPtr) + 312u * U(s1);
        if (v < 40) {
            const int64_t at = 408 + 312 * static_cast<int64_t>(v);
            if (at < 0 || static_cast<size_t>(at) + 312u > d.envSize) return false; // OURS: the copy's source
            g.WriteBlock(dst, d.env + at, 312);                                // memcpy 0x8001E0B4 (words)
        } else {
            g.W8(dst, 0xFF);                                                   // 0x8006B324
        }
    }
    for (; s1 < 6; ++s1) g.W8(g.U32(kHzObjectsPtr) + 312u * U(s1), 0);         // 0x8006B364
    g.W32(kHzOut, 0);                                                          // 0x8006B390
    uint32_t recs = 0, models = 0;
    if (!rc::Call(c, kHzMallocFn, {840u, 0u}, F, &recs)) return false;        // 0x8006B38C
    g.W32(kHzRecordsPtr, recs);
    if (!rc::Call(c, kHzMallocFn, {72u, 0u}, F, &models)) return false;       // 0x8006B3A0
    g.W32(kHzModelsPtr, models);
    for (int32_t k = 2; k >= 0; --k) g.W16(g.U32(kHzRecordsPtr) + 280u * U(k) + 172u, 0); // 0x8006B3BC
    (void)kEnvGuest;
    return !g.Faulted();
}

// ============================================================================ SLUS leaves
void HazardRelease(GuestRam& g, uint32_t rec) {
    if (g.U16(rec + 172u) == 0) return;                                        // 0x80014008
    const uint32_t obj = g.U32(rec + 276u);
    g.W16(rec + 172u, 0);
    if (g.S8(obj) < 0) g.W8(obj, 0);                                           // 0x80014028
    g.W32(kHzOut, g.U32(kHzOut) - 1u);                                         // gp+1672
}

void Hermite(GuestRam& g, int32_t t, uint32_t h00, uint32_t h01, uint32_t h10, uint32_t h11) {
    const int32_t t2 = FixMul(t, t);                                           // 0x8002FA5C
    const int32_t t3 = FixMul(t2, t);
    const int32_t a = Sub(t3, t2);
    g.W32(h11, U(a));                                                          // 0x8002FA78
    const int32_t b = Sub(a, t2);
    g.W32(h10, U(b));
    const int32_t c = Sub(Neg(b), g.S32(h11));
    g.W32(h01, U(c));                                                          // 0x8002FA98
    g.W32(h00, U(Sub(0x10000, c)));
    g.W32(h10, U(Add(g.S32(h10), t)));                                         // 0x8002FAAC
}

void HermiteD(GuestRam& g, int32_t t, uint32_t d00, uint32_t d01, uint32_t d10, uint32_t d11) {
    const int32_t two = S(U(t) << 1);                                          // 0x8002F9B4
    const int32_t t2 = FixMul(t, t);
    const int32_t a = Sub(Add(S(U(t2) << 1), t2), two);
    g.W32(d11, U(a));                                                          // 0x8002F9DC
    g.W32(d10, U(Add(Sub(a, two), 0x10000)));
    const int32_t b = Sub(S(U(g.S32(d11)) << 1), two);                         // 0x8002F9EC
    g.W32(d00, U(b));
    g.W32(d01, U(Neg(b)));
}

// ============================================================================ RASHCDG 0x800A1318
void HazardPlace(GuestRam& g, uint32_t rec, uint32_t local, uint32_t rows, uint32_t sp) {
    const uint32_t F = sp - 48u;
    const int32_t x = g.S32(local), z = g.S32(local + 8u);
    g.W32(F + 16u, U(x));                                                      // 0x8002ECB8's fifth argument
    int16_t a[3], b[3];
    int32_t o[3];
    Get3(g, rows + 12u, a);
    Get3(g, rows, b);
    Blend16To32(a, b, o, z, x);                                                // 0x800A1354
    for (uint32_t k = 0; k < 3; ++k) g.W32(rec + 184u + 4u * k, U(o[k]));
    MulAddView(g, rec + 184u, rows + 6u, g.S32(local + 4u), rec + 184u);       // 0x800A1368
    g.W32(rec + 176u, 0xFFFFFFFFu);                                            // 0x800A137C
    for (uint32_t k = 0; k < 3; ++k) g.W32(rec + 184u + 4u * k, U(Add(g.S32(rec + 184u + 4u * k), g.S32(rec + 224u + 4u * k))));
}

// ============================================================================ RASHCDG 0x800A0A20
bool HazardSpawn(GuestRam& g, uint32_t cls, uint32_t mode, uint32_t pos, uint32_t rows, uint32_t obj, uint32_t sp,
                 const BikeTables& t, uint32_t& v0) {
    const uint32_t F = sp - 72u;
    v0 = 0;
    uint32_t s1 = g.U32(kHzRecordsPtr);
    int32_t a3 = 0;
    for (;;) {                                                                 // 0x800A0A64
        if (g.U16(s1 + 172u) == 0) break;
        ++a3;
        if (!(a3 < 3)) break;
        s1 += 280u;
    }
    if (!(a3 < 3)) return !g.Faulted();                                        // 0x800A0A8C
    g.W16(s1 + 172u, static_cast<uint16_t>(a3 + 189));                         // 0x800A0AA4
    g.W32(s1 + 4u, g.U32(kHzModelsPtr) + 24u * U(a3));
    uint32_t mv = 0;
    if (!ModelBind(g, s1, 6, cls, 0, mv)) return false;                        // 0x800A0AC0
    g.W32(s1 + 276u, obj);
    const uint32_t s2 = obj;
    if (g.S8(s2) < 0) {                                                        // 0x800A0AD4: two random keys
        g.W8(s2 + 3u, 2);
        const int32_t s3 = (GuestRand(g) & 1u) ? 1 : -1;                        // 0x800A0ADC
        uint32_t r = GuestRand(g);                                             // 0x800A0AF4
        const uint32_t q = MulHi(r, 0xAAAAAAABu) >> 17;
        g.W32(s2 + 144u, 0);
        g.W32(s2 + 116u, 0);
        g.W32(s2 + 88u, r - q * 196608u + 0x30000u);                           // 0x800A0B34
        r = GuestRand(g);                                                      // 0x800A0B30
        g.W32(s2 + 4u, (r & 0xFFFFu) + 0x8000u);
        const int32_t u1 = SDiv(FixMul(0x1A666, g.S32(s2 + 88u)), g.S32(s2 + 4u)); // 0x800A0B50..0x800A0BF8
        const int32_t t1 = S(U(s3) * U(u1));                                   // 0x800A0BFC
        const int32_t x1 = S(U(s3) * g.U32(s2 + 88u));                         // 0x800A0C10
        g.W32(s2 + 228u, U(t1));
        g.W32(s2 + 172u, 0);
        g.W32(s2 + 200u, 0);
        g.W32(s2 + 92u, 0);
        g.W32(s2 + 120u, 0);
        g.W32(s2 + 148u, U(x1));
        const int32_t u2 = SDiv(FixMul(0x1A666, x1), g.S32(s2 + 4u));          // 0x800A0C40..0x800A0CE8
        g.W32(s2 + 176u, U(Neg(s3)) * U(u2));                                  // 0x800A0CEC
        g.W32(s2 + 232u, 0);
        g.W32(s2 + 204u, 0);
        r = GuestRand(g);                                                      // 0x800A0D08
        const uint32_t q2 = MulHi(r >> 2, 0x08FB823Fu) >> 1;
        const uint32_t v = r - q2 * 228u + 227u;
        const uint32_t roll = U(Neg(s3)) * v;                                  // 0x800A0D48
        g.W32(s2 + 288u, roll);
        g.W32(s2 + 284u, roll);
    }
    for (uint32_t k = 0; k < 3; ++k) g.W32(s1 + 224u + 4u * k, g.U32(pos + 4u * k)); // 0x800A0D58
    if (rows != 0) {                                                           // 0x800A0D74
        rc::CopyHalfwords(g, 9, rows, s1 + 244u);                              // SLUS 0x8003FA18
        rc::CopyHalfwords(g, 9, s1 + 244u, s1 + 196u);
    } else {
        for (uint32_t k = 0; k < 9; ++k) {
            const uint16_t v = (k % 4u) == 0 ? 4096u : 0u;
            g.W16(s1 + 244u + 2u * k, v);
            g.W16(s1 + 196u + 2u * k, v);
        }
    }
    g.W8(s2 + 1u, static_cast<uint8_t>(mode));                                 // 0x800A0DA4 / 0x800A0DF4
    if (mode == 0) {                                                           // 0x800A0DF8
        g.W32(s1 + 236u, U(RatAtan2(g.S16(s1 + 256u), g.S16(s1 + 260u), t.atan)));
        int32_t as = 0;
        if (!Asin(S(U(g.S16(s1 + 258u)) << 4), t.asin, as)) return false;
        g.W32(s1 + 240u, U(Neg(as)));
    }
    g.W8(s1 + 262u, 0xFF);                                                     // 0x800A0E30
    g.W32(s1 + 268u, 0);
    g.W32(s1 + 272u, 0);
    g.W32(s1 + 264u, 0);
    const uint32_t o = g.U32(s1 + 276u);
    g.W32(F + 16u, g.U32(o + 88u));                                            // the first key, 0x800A0E48
    g.W32(F + 20u, g.U32(o + 116u));
    g.W32(F + 24u, g.U32(o + 144u));
    HazardPlace(g, s1, F + 16u, s1 + 196u, F);                                 // 0x800A0E70
    g.W32(s1 + 176u, 0xFFFFFFFFu);
    g.W32(kHzOut, g.U32(kHzOut) + 1u);                                         // 0x800A0E94
    v0 = s1;
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800A0EC4
bool HazardEvent(GuestRam& g, uint32_t ev, uint32_t pos, uint32_t rows, uint32_t sp, const BikeTables& t) {
    const uint32_t F = sp - 56u;
    int32_t n = 0;
    while (n < 3 && g.S8(ev + 9u + U(n)) >= 0) ++n;                            // 0x800A0EF4
    for (int32_t k = 0; k < n; ++k) {                                          // 0x800A0F24
        const int32_t idx = g.S8(ev + 9u + U(k));
        const uint32_t obj = g.U32(kHzObjectsPtr) + U(idx) * 312u;
        g.W32(F + 16u, obj);
        uint32_t v0 = 0;
        if (!HazardSpawn(g, U(static_cast<int32_t>(g.S8(ev + 8u))), g.S16(ev + 14u) != 0 ? 1u : 0u, pos, rows, obj, F, t,
                         v0))
            return false;                                                      // 0x800A0F60
    }
    g.W8(ev + 12u, 0);                                                         // 0x800A0F74
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800A0F9C
void HazardRandomize(GuestRam& g, uint32_t t, uint32_t sp) {
    const uint32_t F = sp - 72u;
    const uint32_t keep[6] = {g.U32(t + 92u), g.U32(t + 120u), g.U32(t + 148u), g.U32(t + 176u), g.U32(t + 204u),
                              g.U32(t + 232u)};
    g.W32(F + 16u, keep[0]);                                                   // 0x800A0FC0..0x800A0FFC
    g.W32(F + 20u, keep[1]);
    g.W32(F + 24u, keep[2]);
    g.W32(F + 32u, keep[3]);
    g.W32(F + 36u, keep[4]);
    g.W32(F + 40u, keep[5]);
    uint32_t s3, v1;
    if (g.U32(t + 92u) != 0) {                                                 // 0x800A1008
        s3 = 2;
        v1 = 0;
    } else {
        s3 = 0;
        v1 = 2;
    }
    const uint32_t s2 = t + 28u * v1;
    int32_t s0 = Iabs(g.S32(s2 + 92u));                                        // 0x800A1038
    if (g.S32(t + 88u) > 0) {                                                  // 0x800A1040
        const uint32_t r = GuestRand(g);
        const uint32_t q = MulHi(r, 0x35558AABu) >> 14;
        s0 = Add(s0, S(r - q * 78642u + 0xFFFF6667u));                         // 0x800A1090
        s0 = Clamp2(s0, 0x30000, 0x140000);                                    // 0x800A1094..0x800A10C0
    }
    const uint32_t r = GuestRand(g);                                           // 0x800A10C4
    const uint32_t q = MulHi(r, 0x35558AABu) >> 14;
    int32_t a1 = Add(g.S32(t + 120u), S(r - q * 78642u + 0xFFFF6667u));        // 0x800A1114
    a1 = Clamp2(a1, -0x20000, 0x30000);                                        // 0x800A1118..0x800A1160
    const uint32_t a2 = t + 28u * s3;
    g.W32(a2 + 92u, U(g.S32(a2 + 88u) < 0 ? s0 : Neg(s0)));                    // 0x800A115C..0x800A116C
    g.W32(t + 120u, U(a1));
    g.W32(s2 + 92u, 0);                                                        // 0x800A1174
    const int32_t k = g.S32(s2 + 172u) >= 0 ? S(0xFFFE599Au) : 0x1A666;        // 0x800A1178..0x800A118C
    g.W32(s2 + 176u, U(SDiv(FixMul(k, s0), g.S32(t + 4u))));                   // 0x800A1190..0x800A1298
    g.W32(t + 28u * s3 + 176u, 0);                                             // 0x800A12AC
    g.W32(t + 204u, 0);
    g.W32(t + 88u, g.U32(F + 16u));                                            // 0x800A12B4..0x800A12F8
    g.W32(t + 116u, g.U32(F + 20u));
    g.W32(t + 144u, g.U32(F + 24u));
    g.W32(t + 172u, g.U32(F + 32u));
    g.W32(t + 200u, g.U32(F + 36u));
    g.W32(t + 228u, g.U32(F + 40u));
}

// ============================================================================ RASHCDG 0x800A13C4
bool HazardPass(GuestRam& g, int32_t dt, uint32_t sp, const BikeTables& t) {
    const uint32_t F = sp - 200u;
    const uint32_t bike = g.U32(kHzBikePtr);                                   // 0x800A13D0
    g.W32(sp, U(dt));                                                          // 0x800A1400: a0's home
    g.W32(F + 120u, bike);
    g.W16(F + 140u, 0);
    if (g.U32(kHzOut) == 0) {                                                  // 0x800A1408
        if (g.S16(bike + 320u) != 0 && g.S32(kHzEventCount) > 0) {             // 0x800A1418 / 0x800A1428
            for (int32_t i = 0; i < g.S32(kHzEventCount); ++i) {               // 0x800A1430
                g.W32(F + 144u, U(i));
                const uint32_t s2 = g.U32(kHzEventsPtr) + 16u * U(i);
                if (g.S8(s2 + 12u) == 0) continue;
                if (g.U32(s2) != g.U32(bike + 360u)) continue;                 // 0x800A1464
                const int32_t a2 = g.S16(s2 + 14u);
                const int32_t s0 = Clamp2(S(U(a2) << 16), 0xA0000, 0xC80000);  // 0x800A1480..0x800A14A8
                const int32_t s1 = Sub(g.S32(s2 + 4u), g.S32(bike + 368u));
                if (!(Iabs(s1) < s0)) continue;                                // 0x800A14BC
                if (g.U8(g.U32(bike + 1084u)) & 0x80u) continue;               // 0x800A14D8: wrong way
                uint32_t pos, rows;
                if (a2 == 0) {                                                 // 0x800A159C: on the bike
                    pos = bike + 504u;
                    rows = bike + 516u;
                } else {
                    GuestCopyWords(g, F + 24u, bike + 328u, 32u);              // 0x800A14F0
                    uint32_t sl = 0;
                    if (!RoadStep(g, 0, F + 24u, s1 >= 0 ? s0 : Neg(s0), F, sl)) return false; // 0x800A1508
                    pos = g.U32(F + 36u) + 20u;
                    const uint32_t leg = RouteFindLegView(g, g.U32(bike + 428u), g.U16(bike + 360u)); // 0x800A1520
                    const uint32_t slice = g.U32(F + 36u);
                    uint16_t x = g.U16(slice + 14u), z = g.U16(slice + 18u);
                    g.W16(F + 70u, 0);
                    g.W16(F + 68u, x);
                    g.W16(F + 72u, z);
                    if (leg != 0 && g.S32(leg + 4u) < 0) {                     // 0x800A1548
                        x = static_cast<uint16_t>(0u - x);
                        z = static_cast<uint16_t>(0u - z);
                        g.W16(F + 68u, x);
                        g.W16(F + 72u, z);
                    }
                    rows = F + 56u;
                    g.W16(F + 58u, 0);                                         // 0x800A157C..0x800A1598
                    g.W16(F + 62u, 0);
                    g.W16(F + 64u, 4096);
                    g.W16(F + 66u, 0);
                    g.W16(F + 56u, g.U16(F + 72u));
                    g.W16(F + 60u, static_cast<uint16_t>(0u - g.U16(F + 68u)));
                }
                g.W32(F + 132u, rows);
                if (!HazardEvent(g, s2, pos, rows, F, t)) return false;        // 0x800A15B8
                break;
            }
        }
        if (g.U32(kHzOut) == 0) return !g.Faulted();                           // 0x800A15E8
    }
    for (uint32_t i = 0; i < 3; ++i) {                                         // 0x800A15F4
        g.W32(F + 144u, i);
        const uint32_t s5 = g.U32(kHzRecordsPtr) + 280u * i;
        if (g.U16(s5 + 172u) == 0) continue;
        const int32_t dx = Sub(g.S32(s5 + 184u), g.S32(bike + 184u));
        const int32_t dz = Sub(g.S32(s5 + 192u), g.S32(bike + 192u));
        const int32_t dist = Octagon(Iabs(dx >> 16), Iabs(dz >> 16));          // 0x800A1638..0x800A169C
        const bool far = g.U32(s5 + 180u) == 0 ? !(dist < 601) : !(dist < 301);
        if (far && (g.U8(s5 + 9u) & 1u) == 0) {                                // 0x800A16CC
            HazardRelease(g, s5);                                              // SLUS 0x80014000
            continue;
        }
        const uint32_t tm = g.U32(s5 + 276u);
        g.W32(F + 124u, tm);
        if (g.U8(tm + 3u) < 2u) continue;                                      // 0x800A1704
        if (!(g.S32(s5 + 264u) < S(g.U8(tm + 3u)))) {                          // 0x800A171C: past the last key
            MulAddView(g, s5 + 184u, s5 + 214u, FixMul(g.S32(s5 + 220u), dt), s5 + 184u); // 0x800A1744
            g.W32(s5 + 176u, 0xFFFFFFFFu);
            continue;
        }
        if (g.S8(tm) < 0 && g.U32(s5 + 272u) == 0) HazardRandomize(g, tm, F);  // 0x800A1780
        uint32_t base, m;                                                      // sp+132 / sp+128
        if (g.S8(tm + 1u) != 0) {                                              // 0x800A1798
            base = s5 + 244u;
            m = F + 56u;
        } else {                                                               // 0x800A17B0: rides with the bike
            for (uint32_t k = 0; k < 3; ++k) g.W32(s5 + 224u + 4u * k, g.U32(bike + 504u + 4u * k));
            base = F + 56u;
            const int32_t yaw = RatAtan2(g.S16(bike + 450u), g.S16(bike + 454u), t.atan); // 0x800A17E8
            AngleChase(g, s5 + 236u, yaw, dt, 0x70000);                        // 0x800A17FC
            m = s5 + 244u;
            int32_t as = 0;
            if (!Asin(S(U(g.S16(bike + 452u)) << 4), t.asin, as)) return false; // 0x800A1818
            AngleChase(g, s5 + 240u, Neg(as), dt, 0x70000);                    // 0x800A182C
            g.W16(F + 96u, static_cast<uint16_t>(0u - g.U16(s5 + 240u)));
            g.W16(F + 100u, 0);
            g.W16(F + 98u, static_cast<uint16_t>(0u - g.U16(s5 + 236u)));
            int16_t ang[3], rm[9];
            Get3(g, F + 96u, ang);
            RotMatrix(ang, rm, t.sincos);                                      // SLUS 0x8004D2A4
            Put9(g, base, rm);
        }
        g.W32(F + 132u, base);
        g.W32(F + 128u, m);
        // the key's clock (0x800A185C..0x800A1958)
        int32_t k = g.S32(s5 + 264u);
        g.W32(F + 148u, U(k));
        int32_t s6 = g.S32(tm + 4u + 4u * U(k));
        const int32_t now = Add(g.S32(s5 + 272u), dt);
        int32_t local = Sub(now, g.S32(s5 + 268u));
        g.W32(s5 + 272u, U(now));
        if (!(local < s6)) {                                                   // 0x800A189C
            const int32_t nk = g.S32(s5 + 264u) + 1;
            g.W32(s5 + 264u, U(nk));
            const int32_t cnt = g.U8(tm + 3u);
            if (nk < cnt - 1) {                                                // 0x800A18CC
                k = nk;
                g.W32(F + 148u, U(k));
                const int32_t start = Add(g.S32(s5 + 268u), s6);
                local = Sub(g.S32(s5 + 272u), start);
                g.W32(s5 + 268u, U(start));
                s6 = g.S32(tm + 4u + 4u * U(k));
            } else {
                if (g.S8(tm) > 0) {                                            // 0x800A18DC: hold the last key
                    g.W32(s5 + 264u, U(cnt));
                    g.W32(s5 + 220u, g.U32(tm + 256u + 4u * U(cnt - 1)));
                } else {                                                       // 0x800A1908: loop
                    g.W32(s5 + 268u, 0);
                    g.W32(s5 + 272u, 0);
                    g.W32(s5 + 264u, 0);
                    g.W8(s5 + 262u, 0xFF);
                }
                local = s6;
            }
        }
        int32_t u = SDiv(local, s6);                                           // 0x800A195C..0x800A19B8
        g.W32(F + 136u, U(u));
        const uint32_t H = F + 104u;                                           // h00 h01 h10 h11
        if (g.S8(tm) < 0) {                                                    // 0x800A19CC: the random leg
            const uint32_t a0 = g.U32(tm + 172u) != 0 ? 0u : 1u;
            const uint32_t v1 = a0 == 0 ? 1u : 0u;
            const uint32_t s0 = tm + 28u * a0;
            const int32_t r = FixMul(u, Sub(g.S32(tm + 28u * v1 + 176u), g.S32(s0 + 172u)));
            g.W32(s5 + 220u, U(Add(g.S32(s0 + 172u), r)));                     // 0x800A1A3C
        } else {                                                               // 0x800A1A40: the time curve
            const uint32_t s1 = tm + 4u * U(k + 1);
            const uint32_t s2 = tm + 4u * U(k);
            const int32_t r = FixMul(u, Sub(g.S32(s1 + 256u), g.S32(s2 + 256u)));
            g.W32(s5 + 220u, U(Add(g.S32(s2 + 256u), r)));                     // 0x800A1A8C
            g.W32(F + 16u, H + 12u);
            Hermite(g, u, H, H + 4u, H + 8u, H + 12u);                         // 0x800A1A90
            const int32_t a = FixMul(g.S32(s2 + 60u), g.S32(H));
            const int32_t b = FixMul(g.S32(s1 + 60u), g.S32(H + 4u));
            const int32_t c = FixMul(g.S32(s2 + 32u), g.S32(H + 8u));
            const int32_t d = FixMul(g.S32(s1 + 32u), g.S32(H + 12u));
            const int32_t e = FixMul(s6, Add(c, d));
            const int32_t s3 = Add(Add(a, b), e);                              // 0x800A1AF0
            g.W32(F + 136u, U(s3));
            const int32_t t0 = g.S32(s2 + 60u);
            s6 = Sub(g.S32(s1 + 60u), t0);                                     // 0x800A1B00
            u = SDiv(Sub(s3, t0), s6);                                         // 0x800A1AF8..0x800A1B50
            g.W32(F + 136u, U(u));
        }
        u = g.S32(F + 136u);
        g.W32(F + 16u, H + 12u);
        Hermite(g, u, H, H + 4u, H + 8u, H + 12u);                             // 0x800A1B6C
        for (uint32_t j = 0; j < 3; ++j) {                                     // 0x800A1B88: the position
            const uint32_t s0 = tm + 4u * U(k) + 28u * j;
            const uint32_t s2 = tm + 4u * U(k + 1) + 28u * j;
            const int32_t a = FixMul(g.S32(s0 + 88u), g.S32(H));
            const int32_t b = FixMul(g.S32(s2 + 88u), g.S32(H + 4u));
            const int32_t c = FixMul(g.S32(s0 + 172u), g.S32(H + 8u));
            const int32_t d = FixMul(g.S32(s2 + 172u), g.S32(H + 12u));
            const int32_t e = FixMul(s6, Add(c, d));
            g.W32(F + 80u + 4u * j, U(Add(Add(a, b), e)));                     // 0x800A1C04
        }
        HazardPlace(g, s5, F + 80u, g.U32(F + 132u), F);                       // 0x800A1C0C
        g.W32(F + 16u, H + 12u);
        HermiteD(g, g.S32(F + 136u), H, H + 4u, H + 8u, H + 12u);              // 0x800A1C28
        for (uint32_t j = 0; j < 3; ++j) {                                     // 0x800A1C38: the tangent
            const uint32_t s1 = tm + 4u * U(k) + 28u * j;
            const uint32_t s2 = tm + 4u * U(k + 1) + 28u * j;
            g.W32(F + 152u, F + 80u + 4u * j);
            const int32_t s3 = FixMul(g.S32(s1 + 172u), g.S32(H + 8u));
            const int32_t s4 = FixMul(g.S32(s2 + 172u), g.S32(H + 12u));
            const int32_t s0 = Add(FixMul(g.S32(s1 + 88u), g.S32(H)), FixMul(g.S32(s2 + 88u), g.S32(H + 4u)));
            g.W32(F + 80u + 4u * j, U(Add(Add(s3, SDiv(s0, s6)), s4)));        // 0x800A1CB4..0x800A1DA8
        }
        {   // SLUS 0x8002E14C Normalize32(sp+80)
            int32_t v[3] = {g.S32(F + 80u), g.S32(F + 84u), g.S32(F + 88u)};
            Normalize32(v, t.rsqrt);
            for (uint32_t j = 0; j < 3; ++j) g.W32(F + 80u + 4u * j, U(v[j]));
        }
        m = g.U32(F + 128u);
        g.W16(m + 12u, static_cast<uint16_t>(g.S32(F + 80u) >> 4));            // 0x800A1DCC..0x800A1DEC: row 2
        g.W16(m + 14u, static_cast<uint16_t>(g.S32(F + 84u) >> 4));
        g.W16(m + 16u, static_cast<uint16_t>(g.S32(F + 88u) >> 4));
        if (g.S8(tm + 2u) != 0) {                                              // 0x800A1E00: level the heading
            g.W16(F + 140u, g.U16(m + 14u));
            g.W16(m + 14u, 0);
            if (!Norm16(g, m + 12u, t)) return false;                          // 0x800A1E18
        }
        if (!(Iabs(g.S32(F + 80u)) < 656 && Iabs(g.S32(F + 88u)) < 656)) {     // 0x800A1E34 / 0x800A1E54
            const int32_t side = g.S8(s5 + 262u);
            const bool neg = side < 0;
            if (g.S8(tm) < 2 || neg) {                                         // 0x800A1E74
                g.W16(m + 2u, 0);                                              // 0x800A1E84: row 0 level
                g.W16(m + 0u, g.U16(m + 16u));
                const uint16_t a0 = g.U16(m + 0u);
                const uint8_t v1 = g.S16(m + 0u) > 0 ? 1u : 0u;
                const uint16_t v0 = static_cast<uint16_t>(0u - g.U16(m + 12u));
                g.W16(m + 4u, v0);
                const uint8_t a1 = static_cast<int16_t>(v0) > 0 ? 1u : 0u;
                if (!neg && g.S8(s5 + 262u) != v1 && g.S8(s5 + 263u) != a1) { // 0x800A1EBC..0x800A1EF8
                    g.W16(m + 0u, static_cast<uint16_t>(0u - a0));
                    g.W16(m + 2u, static_cast<uint16_t>(0u - g.U16(m + 2u)));
                    g.W16(m + 4u, static_cast<uint16_t>(0u - g.U16(m + 4u)));
                } else {
                    g.W8(s5 + 262u, v1);                                       // 0x800A1EFC
                    g.W8(s5 + 263u, a1);
                }
                if (!Norm16(g, m, t)) return false;                            // 0x800A1F08
                Op(g, m + 12u, m, m + 6u);                                     // 0x800A1F54: row 1
                const uint32_t n1 = tm + 4u * U(k + 1), n0 = tm + 4u * U(k);
                const int32_t v = FixMul(g.S32(F + 136u), S(U(Sub(g.S32(n1 + 284u), g.S32(n0 + 284u))) << 16));
                RotateRowPair(g, m + 6u, m, Add(g.S32(n0 + 284u), v >> 16), t.sincos); // 0x800A1FB8: the roll
            } else {                                                           // 0x800A1FC8
                Op(g, m + 12u, m, m + 6u);
                if (!Norm16(g, m + 6u, t)) return false;                       // 0x800A202C
                Op(g, m + 6u, m + 12u, m);                                     // 0x800A206C
            }
            int16_t a[9], b[9], o[9];
            Get9(g, m, a);
            Get9(g, g.U32(F + 132u), b);
            MulMatrix0(a, b, o);                                               // SLUS 0x8003FA40
            Put9(g, s5 + 196u, o);
        }
        if (g.S8(tm + 2u) != 0) {                                              // 0x800A20A4
            g.W16(m + 14u, g.U16(F + 140u));
            VecMat(g, m + 12u, g.U32(F + 132u), s5 + 214u);                    // SLUS 0x8002EFF4
        } else {
            g.W16(s5 + 214u, g.U16(s5 + 208u));                                // 0x800A20D8
            g.W16(s5 + 216u, g.U16(s5 + 210u));
            g.W16(s5 + 218u, g.U16(s5 + 212u));
        }
        if (g.Faulted()) return false;
    }
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800A2138
bool HazardDraw(GuestRam& g, uint32_t p, uint32_t sp, RecoverCallees& c) {
    const uint32_t F = sp - 40u;
    for (uint32_t i = 0; i < 3; ++i) {                                         // 0x800A216C
        const uint32_t t0 = g.U32(kHzRecordsPtr) + 280u * i;
        if (g.U16(t0 + 172u) == 0) continue;
        if (g.S32(t0 + 176u) == -1) {                                          // 0x800A2194: find a cell
            uint32_t t8 = 0xFFFFFFFFu, t7 = 0xFFFFFFFFu;
            bool near = false;
            if (S(p) < 2) {
                for (int32_t t6 = S(p); t6 < 2 && !near; ++t6) {               // 0x800A21E0
                    const uint32_t a3 = g.U32(0x8005B268u + 4u * U(t6));
                    const int32_t dz = Sub(g.S32(t0 + 192u), g.S32(a3 + 192u));
                    const int32_t dx = Sub(g.S32(t0 + 184u), g.S32(a3 + 184u));
                    const int32_t d = Octagon(Iabs(dz >> 16), Iabs(dx >> 16));
                    if (U(d) < 120u) {                                         // 0x800A2258: the bike's own cell
                        t8 = g.U32(a3 + 176u);
                        near = true;
                        break;
                    }
                    for (int32_t k = g.S32(0x8005B568u + 4u * U(t6)) - 1; k >= 0; --k) { // 0x800A227C
                        const uint32_t v1 = g.U32(0x800D9B80u + 48u * U(t6) + 4u * U(k));
                        if (v1 == 0xFFFFFFFFu) continue;
                        const uint32_t t1 = 0x800D87ECu + 112u * v1;
                        const uint32_t body = g.U32(t1);
                        const int32_t a = Sub(g.S16(t0 + 194u), g.S32(body + 16u) >> 6);
                        const int32_t b = Sub(g.S16(t0 + 186u), g.S32(body + 8u) >> 6);
                        const int32_t e = Octagon(Iabs(a), Iabs(b));
                        if (U(e) < t7) {                                       // 0x800A2314 sltu
                            t7 = U(e);
                            t8 = g.U32(t1 + 4u);
                        }
                    }
                }
            }
            g.W32(t0 + 176u, t8);                                              // 0x800A234C / 0x800A2394
        }
        g.W32(t0 + 12u, U(g.S32(t0 + 184u) >> 10));                            // 0x800A2350
        g.W32(t0 + 20u, U(g.S32(t0 + 192u) >> 10));
        g.W32(t0 + 16u, U(g.S32(t0 + 188u) >> 10));
        const uint32_t a0 = g.S8(t0 + 72u) == 3 ? t0 + 104u : g.U32(t0 + 4u) + 4u; // 0x800A236C
        static const uint32_t kTo[9] = {0, 6, 12, 2, 8, 14, 4, 10, 16};         // the rows transposed
        for (uint32_t k = 0; k < 9; ++k) g.W16(a0 + kTo[k], g.U16(t0 + 196u + 2u * k));
        if (g.Faulted()) return false;
        if (!rc::Call(c, kHzModelVisibleFn, {t0, p}, F)) return false;         // 0x800A2410
    }
    return !g.Faulted();
}

} // namespace rr::sim
