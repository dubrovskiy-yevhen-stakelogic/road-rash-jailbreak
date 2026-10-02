#include "game/sim/camera_collide.h"

#include "game/sim/bike.h"
#include "game/sim/ground.h"
#include "game/sim/resolvers.h"
#include "game/sim/vec.h"

namespace rr::sim {
namespace {

inline uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
inline int32_t S(uint32_t v) { return static_cast<int32_t>(v); }

void Read16(GuestRam& g, uint32_t a, int16_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) v[k] = g.S16(a + 2u * k);
}
void Read32(GuestRam& g, uint32_t a, int32_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) v[k] = g.S32(a + 4u * k);
}
void Write32(GuestRam& g, uint32_t a, const int32_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) g.W32(a + 4u * k, U(v[k]));
}

// `(handle >> 5 != 4) | ((handle & 31) < 30)`: not a view object (0x800B2F00..0x800B2F18).
bool NotView(uint16_t h) { return (h >> 5) != 4u || (h & 31u) < 30u; }

// RASHCDG 0x8008BA18 BuildObb(e) on the guest entity (bike.h); the crouch reads the owner's +0x25C
// for a pool-0 entity only.
// The entity is copied out and back (unchanged bytes are written back as they were).
bool GuestBuildObb(GuestRam& g, uint32_t e) {
    uint8_t buf[1132];
    g.ReadBlock(e, buf, sizeof(buf));
    if (g.Faulted()) return false;
    EntityView ev(buf);
    int32_t rev = 0;
    if ((g.U16(e + 0xACu) >> 5) == 0) {
        const uint32_t owner = g.U32(e + 0x354u);
        rev = g.S32(owner + 604u);
    }
    BuildObb(ev, rev);
    g.WriteBlock(e, buf, sizeof(buf));
    return !g.Faulted();
}

} // namespace

// ============================================================================ RASHCDG 0x800B2E64
bool PropVsShape(GuestRam& g, uint32_t e, uint32_t shape, uint32_t sp, CollisionCallees& c, uint32_t& v0) {
    const uint32_t F = sp - kPropVsShapeFrame;
    const uint32_t rows = shape + 260u, box = shape + 24u;
    g.W32(F + 16, F + 64);
    g.W32(F + 20, F + 68);
    uint32_t corner = 0;
    if (!FirstPointInsideBox(g, e + 196u, rows, box, 16u, F + 64, F + 68, F, corner)) return false; // 0x800B2EA4
    if (corner == 8u) {
        v0 = 0;
        return !g.Faulted();
    }
    g.W32(F + 16, F + 24);
    if (!FaceNormal(g, box, rows, g.U32(F + 64), F + 40, F + 24, F)) return false;               // 0x800B2ED4
    {
        int16_t n[3];
        int32_t o[3];
        Read16(g, F + 40, n);
        Scale(S(U(g.S32(F + 68)) + 8192u), n, o);                                                // 0x800B2EEC
        Write32(g, F + 48, o);
    }
    const uint16_t h = g.U16(e + 172u);
    ApplyImpulse(g, e, F + 48, NotView(h) ? 1 : 0);                                              // 0x800B2F14
    v0 = 1;
    if (!NotView(g.U16(e + 172u))) return !g.Faulted();                                          // 0x800B2F40
    if (g.U32(e + 592u) & 2u) {
        const uint32_t a[4] = {e, 0u, F + 40, 0xFFFFFFFFu};
        uint32_t r = 0;
        if (!c.Unported(coll::kPropTopple, a, 4, F, r)) return false;                            // 0x800B2F64
    } else {
        g.W32(e + 480u, 0);                                                                      // 0x800B2F74
    }
    v0 = 1;
    return !g.Faulted();
}

// ============================================================================ RASHCDG 0x800A421C
bool CameraCollide(GuestRam& g, uint32_t v, uint32_t sp, const BikeTables& t, CollisionCallees& c, uint32_t& v0) {
    const uint32_t F = sp - kCameraCollideFrame;
    g.W32(v + 548u, g.U32(v + 548u) & 0xFFFFF1FFu);                                              // 0x800A4250
    if (!GuestBuildObb(g, v)) return false;                                                      // 0x8008BA18
    if (!ViewTrack(g, v, 1, t)) return false;                                                    // 0x800A4258
    g.W32(F + 24, g.U32(v + 468u));                                                              // the saves
    g.W32(F + 28, g.U32(v + 472u));
    g.W32(F + 32, g.U32(v + 476u));
    g.W16(F + 56, g.U16(v + 450u));
    g.W16(F + 58, g.U16(v + 452u));
    g.W16(F + 60, g.U16(v + 454u));
    g.W32(F + 64, g.U32(v + 480u));
    if (!ViewTrack(g, v, 0, t)) return false;                                                    // 0x800A42B0
    for (int32_t i = 0; i < g.S32(0x800CCF90u); ++i) {                                           // the count re-read
        const uint32_t id = g.U8(0x800CCF88u + U(i)) & 31u;
        const uint32_t shape = g.U32(0x800CD6C4u) + 280u * id;
        uint32_t hit = 0;
        if (!PropVsShape(g, v, shape, F, c, hit)) return false;                                  // 0x800A4304
        if (hit != 0) {
            if (!ViewTrack(g, v, 1, t)) return false;                                            // 0x800A43C4
            break;
        }
    }
    uint32_t wall = 0;
    if (!WallContact(g, v, 0, F, t, c, wall)) return false;                                      // 0x800A432C
    g.W32(v + 468u, g.U32(F + 24));
    g.W32(v + 472u, g.U32(F + 28));
    g.W32(v + 476u, g.U32(F + 32));
    g.W16(v + 450u, g.U16(F + 56));
    g.W16(v + 452u, g.U16(F + 58));
    g.W32(v + 480u, g.U32(F + 64));
    g.W16(v + 454u, g.U16(F + 60));
    int32_t q = 0;                                                                                // v1
    uint32_t pt = 0, nrm = 0;                                                                     // s3, s2
    if (g.U32(v + 388u) & 1u) {                                                                  // off the road
        pt = F + 40;
        nrm = F + 56;
        g.W32(F + 16, g.U32(v + 536u));
        const GroundResult r = GroundQuery(g, v, 0, F + 40, F + 56, g.U32(v + 536u), t.rsqrt);  // 0x800A43AC
        if (r.declined) return false;
        q = S(r.value);
        g.W32(v + 536u, r.value);
    } else {
        if (g.U32(v + 372u) != 0) g.W8(v + 534u, g.U8(v + 394u));
        else g.W8(v + 534u, 1);
        g.W32(v + 536u, 0);
    }
    bool useOwn = false;
    if (!(q > 0)) {                                                                              // 0x800A43FC
        const uint32_t slice = g.U32(v + 340u);
        pt = slice + 20u;
        nrm = slice + 8u;
        if (q < 0) {
            if (g.U32(v + 480u) == 0) {                                                          // 0x800A4418
                g.W32(v + 480u, 0x1C9C4u);
                int16_t d[3];
                int32_t o[3];
                Read16(g, v + 450u, d);
                Scale(0x1C9C4, d, o);                                                            // 0x800A4440
                Write32(g, v + 456u, o);
            }
            useOwn = true;
        }
    }
    if (!useOwn) {                                                                               // 0x800A4450
        if (g.S16(nrm + 2u) < -614) {
            g.W16(v + 522u, g.U16(nrm + 0u));
            g.W16(v + 524u, g.U16(nrm + 2u));
            g.W16(v + 526u, g.U16(nrm + 4u));
            g.W32(v + 504u, g.U32(pt + 0u));
            g.W32(v + 508u, g.U32(pt + 4u));
            g.W32(v + 512u, g.U32(pt + 8u));
        } else {
            useOwn = true;
        }
    }
    if (useOwn) {
        pt = v + 504u;
        nrm = v + 522u;
    }
    g.W32(F + 16, 0);
    const uint32_t k = CornerMin(g, v + 196u, nrm, pt, F + 64, 0);                               // 0x800A44C8
    if (g.Faulted()) return false;
    if (k < 8u) {
        int32_t b[3], o[3];
        int16_t d[3];
        Read32(g, v + 184u, b);
        Read16(g, nrm, d);
        MulAdd(b, d, g.S32(F + 64), o);                                                          // 0x800A44EC
        Write32(g, v + 184u, o);
        v0 = 1;
    } else {
        v0 = wall;
    }
    return !g.Faulted();
}

} // namespace rr::sim
