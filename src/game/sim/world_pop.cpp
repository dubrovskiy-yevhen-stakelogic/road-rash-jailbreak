// The cell walker, the roadside props and the collision volumes (world_pop.h), line by line from our
// own listings of RASHCDG.BIN (SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c) and SLUS_010.53
// (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1). The addresses beside the statements are the
// original's instructions.
#include "game/sim/world_pop.h"

#include "game/sim/ai.h"          // AiProject 0x800B6AAC, SqrtGte SLUS 0x8004CF74
#include "game/sim/cell_draw.h"   // PositionCell SLUS 0x80030410
#include "game/sim/fixed.h"       // FixMul, RatAtan2
#include "game/sim/police.h"      // FindFreeCop 0x80095848
#include "game/sim/population.h"  // WindowPred, RoadGate, CursorSeat, RoadWindow
#include "game/sim/traffic_bind.h"   // ModelBind, PoolRelease
#include "game/sim/traffic_leaves.h" // Budget, CarSetup
#include "game/sim/vec.h"

namespace rr::sim {
namespace {

constexpr uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }
constexpr int32_t S(uint32_t v) { return static_cast<int32_t>(v); }
int32_t Iabs(int32_t v) { return v < 0 ? S(0u - U(v)) : v; }

uint32_t GameState(GuestRam& g) { return g.U32(kWpGameStatePtr); }
uint32_t Players(GuestRam& g) { return g.U32(GameState(g) + 48u); }
bool TwoPlayerMode(GuestRam& g) { return (g.U8(GameState(g) + 4u) & 0x10u) != 0; }
uint32_t PlayerBike(GuestRam& g, uint32_t p) { return g.U32(kWpPlayerBikes + 4u * p); }

void Read32(GuestRam& g, uint32_t a, int32_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) v[k] = g.S32(a + 4u * k);
}
void Write32(GuestRam& g, uint32_t a, const int32_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) g.W32(a + 4u * k, U(v[k]));
}
void Read16(GuestRam& g, uint32_t a, int16_t v[3]) {
    for (uint32_t k = 0; k < 3; ++k) v[k] = g.S16(a + 2u * k);
}
// RASHCDG 0x800B6AAC AiProject(p, axis, q) over guest addresses (ai.h).
int32_t GProject(GuestRam& g, uint32_t p, uint32_t axis, uint32_t q) {
    int32_t a[3], o[3];
    int16_t n[3];
    Read32(g, p, a);
    Read16(g, axis, n);
    Read32(g, q, o);
    return AiProject(a, n, o);
}
// SLUS 0x8002EAD8 MulAdd(base, dir, t, out) (vec.h); `out` may be `base`.
void GMulAdd(GuestRam& g, uint32_t base, uint32_t dir, int32_t t, uint32_t out) {
    int32_t b[3], o[3];
    int16_t d[3];
    Read32(g, base, b);
    Read16(g, dir, d);
    MulAdd(b, d, t, o);
    Write32(g, out, o);
}
// SLUS 0x8002E570 MulAdd32(base, dir, t, out) (vec.h).
void GMulAdd32(GuestRam& g, uint32_t base, uint32_t dir, int32_t t, uint32_t out) {
    int32_t b[3], d[3], o[3];
    Read32(g, base, b);
    Read32(g, dir, d);
    MulAdd32(b, d, t, o);
    Write32(g, out, o);
}
// SLUS 0x8002EE50 Scale(t, dir, out) (vec.h).
void GScale(GuestRam& g, int32_t t, uint32_t dir, uint32_t out) {
    int16_t d[3];
    int32_t o[3];
    Read16(g, dir, d);
    Scale(t, d, o);
    Write32(g, out, o);
}
// SLUS 0x8002ECB8 Blend16To32(a, b, out, wa, [sp+16] wb) (vec.h).
void GBlend(GuestRam& g, uint32_t a, uint32_t b, uint32_t out, int32_t wa, int32_t wb) {
    int16_t x[3], y[3];
    int32_t o[3];
    Read16(g, a, x);
    Read16(g, b, y);
    Blend16To32(x, y, o, wa, wb);
    Write32(g, out, o);
}
// SLUS 0x80020018 RatAtan2(a0, a1) with its table 0x8005285C read out of the image.
int32_t GAtan(GuestRam& g, int32_t a0, int32_t a1) {
    int32_t table[18];
    for (uint32_t k = 0; k < 18; ++k) table[k] = g.S32(0x8005285Cu + 4u * k);
    return RatAtan2(a0, a1, table);
}
// The heading words the props and volumes keep: h = RatAtan2(row2.x << 4, row2.z << 4), then
// cos << 4 and sin << 4 from the sine table (h & 0xFFF).
void Heading(GuestRam& g, uint32_t xz, uint32_t zz, uint32_t out) {
    const int32_t h = GAtan(g, S(U(g.S16(xz)) << 4), S(U(g.S16(zz)) << 4));
    g.W32(out, U(h));
    const uint32_t i = (U(h) & 0xFFFu) << 2;
    g.W32(out + 4u, U(g.S16(kWpSinCos + (i | 2u))) << 4);   // cos
    g.W32(out + 8u, U(g.S16(kWpSinCos + ((g.U32(out) & 0xFFFu) << 2))) << 4); // sin, the word re-read
}
// SLUS 0x8003B8F4 RouteBindingValid(p), a leaf over 0x8003B4B0 (race.h).
int32_t RouteValid(GuestRam& g, uint32_t p) {
    if (p == 0) return 0;
    const uint32_t o = g.U32(p + 256u);
    if (o == 0) return 0;
    const uint32_t w = g.U32(p + 188u);
    if ((w >> 16) == 1u) return (w & 0xFFFFu) == g.U32(o) ? 1 : 0;
    return RouteFindLegView(g, o, w & 0xFFFFu) != 0 ? 1 : 0;
}
// The octagonal distance of the whole-unit halves (the volume lists' metric, 0x8008DA20 / 0x8008DD20).
int32_t Octagon(int32_t dx, int32_t dz) {
    int32_t a = Iabs(dx), b = Iabs(dz);
    if (a < b) {
        const int32_t t = a;
        a = b;
        b = t;
    }
    const int32_t m = b + (b >> 1);
    return ((a - (a >> 5)) - (a >> 7)) + (m >> 2) + (m >> 6);
}
uint32_t View(uint32_t p) { return kWpViews + 1132u * p; }

// A seam call (recover.h rc::Call).
bool Seam(RecoverCallees& c, uint32_t fn, std::initializer_list<uint32_t> a, uint32_t sp, uint32_t* v0 = nullptr) {
    return rc::Call(c, fn, a, sp, v0);
}

} // namespace

// ============================================================================ SLUS leaves

uint32_t CellSlotFor(GuestRam& g, uint32_t id, uint32_t p) {
    const uint32_t first = kWpCellSlots + 1344u * p;                         // 0x80013204..0x80013220
    const uint32_t end = first + 1344u;
    for (uint32_t s = first; s < end; s += 112u) {
        const uint32_t w0 = g.U32(s), w2 = g.U32(s + 8u), w1 = g.U32(s + 4u);
        if (w0 == 0xFFFFFFFFu || w2 == 0xFFFFFFFFu || w1 == 0) continue;     // 0x80013248..0x80013264
        if (w2 == id) return s;                                               // 0x8001326C
    }
    return 0;
}

uint32_t OtherSlot(GuestRam& g, uint32_t id, uint32_t p) {
    if (!(g.U8(g.U32(g.gp() + 1644u) + 4u) & 0x10u)) return 0;               // 0x80013360..0x80013378
    return CellSlotFor(g, id, p == 0 ? 1u : 0u);                              // sltiu a1,a1,1
}

int32_t ResidentPieces(GuestRam& g, uint32_t out, int32_t max) {
    int32_t n = 0;
    if (out == 0) return 0;
    const int32_t last = g.S32(kWpPiecesLast);
    if (last < 0) return 0;
    uint32_t a = kWpPieces;
    for (int32_t i = 0; i <= last; ++i, a += 16u) {                            // 0x8003C458..0x8003C484
        const uint32_t v = g.U32(a);
        if (v == 0xFFFFFFFFu) continue;
        if (!(n < max)) return n;
        g.W32(out, v);
        out += 4u;
        ++n;
    }
    return n;
}

uint32_t PieceChild(GuestRam& g, uint32_t key, uint32_t piece) {
    const uint32_t b = RoadBttRecord(g, S(piece));                            // 0x8003C4AC
    if (b == 0) return 0;
    const uint32_t r = g.U32(b + 12u);
    if (r == 0) return 0;
    const int32_t n = g.S16(r + 30u);
    uint32_t c = g.U32(r + 72u);
    for (int32_t i = 0; i < n; ++i, c += 20u)                                 // 0x8003C4E0..0x8003C504
        if (g.U32(c) == key) return c;
    return 0;
}

uint32_t JunctionSlice(GuestRam& g, uint32_t cursor, uint32_t pos) {
    if (cursor == 0) return 0;                                                // 0x8003C774
    const uint32_t sub = g.U32(cursor + 8u);
    const int32_t k = S(U(static_cast<int32_t>(g.S16(sub + 8u))) + U(static_cast<int32_t>(g.S16(sub + 10u))));
    uint32_t s1 = g.U32(cursor + 12u);
    const uint32_t s2 = g.U32(g.U32(cursor) + 52u) + U(k) * 52u - 52u;        // 0x8003C790..0x8003C7B4
    if (s1 == s2) return s1;                                                  // 0x8003C7B8
    uint32_t s0 = s1 + 52u;
    if (GProject(g, pos, s1 + 66u, s1 + 72u) <= 0) return s1;                 // 0x8003C7C8
    for (;;) {
        const int32_t v1 = g.S16(s2);                                         // 0x8003C7DC
        if (!(g.S16(s1) < v1)) return s1;
        s1 = s0;                                                              // 0x8003C7F0
        if (g.S16(s0) < v1) s0 = s1 + 52u;
        if (!(GProject(g, pos, s0 + 14u, s0 + 20u) > 0)) return s1;           // 0x8003C810
        if (g.Faulted()) return s1;
    }
}

int32_t VolumeMask(GuestRam& g, uint32_t kind, uint32_t h, uint32_t p) {
    if (!(((h - 1u) & 0xFFFFu) < 223u)) return 0;                             // 0x800132AC..0x800132C0
    const uint32_t vol = g.U32(kWpPool6Ptr) + 280u * (h & 31u);
    const int32_t r = WindowPred(g, kind, vol + 12u, p, 0, 1);                // 0x800132FC
    uint8_t b = g.U8(vol + 3u);
    const uint8_t m = static_cast<uint8_t>(1u << (p & 31u));
    if (b & m) {
        if (r != 0) return 0;                                                 // 0x8001331C
        b = static_cast<uint8_t>(b & (1u << ((p ^ 1u) & 31u)));
    } else {
        if (r == 0) return 0;                                                 // 0x80013330
        b = static_cast<uint8_t>(b | m);
    }
    g.W8(vol + 3u, b);                                                        // 0x80013338
    return 1;
}

void Class50(GuestRam& g, uint32_t rec, uint32_t cellId, uint32_t p) {
    const uint32_t r = kWpClass50 + 28u * p;
    g.W32(r + 0u, g.U32(rec + 8u));
    g.W32(r + 4u, g.U32(rec + 20u));
    g.W32(r + 8u, g.U32(rec + 24u));
    g.W32(r + 12u, g.U32(rec + 28u));
    g.W16(r + 16u, g.U16(rec + 14u));
    g.W16(r + 18u, g.U16(rec + 16u));
    const uint16_t z = g.U16(rec + 18u);
    g.W32(r + 24u, cellId);
    g.W16(r + 22u, 1);
    g.W16(r + 20u, z);
}

void MatrixQuat(GuestRam& g, uint32_t m, uint32_t q, const BikeTables& t) {
    const int32_t next[3] = {g.S32(kWpQuatNext), g.S32(kWpQuatNext + 4u), g.S32(kWpQuatNext + 8u)}; // 0x800716E8
    const int32_t a0 = g.S16(m), v1 = g.S16(m + 8u), a1 = g.S16(m + 16u);
    const int32_t tr = S(U(a0 + v1 + a1) << 4);                               // 0x80071710..0x80071718
    auto el = [&](int32_t r, int32_t c) { return static_cast<int32_t>(g.S16(m + 2u * U(3 * r + c))); };
    if (tr > 0) {
        const int32_t s = SqrtGte(S(U(tr) + 0x10000u), t.sqrt);               // 0x80071728
        g.W16(q + 6u, static_cast<uint16_t>(U(s) >> 1));                      // 0x8007173C
        const int32_t k = rc::Recip(S(U(s) << 3));
        g.W16(q + 0u, static_cast<uint16_t>(S(U(el(1, 2) - el(2, 1)) * U(k)) >> 14)); // 0x80071784..
        g.W16(q + 2u, static_cast<uint16_t>(S(U(el(2, 0) - el(0, 2)) * U(k)) >> 14));
        g.W16(q + 4u, static_cast<uint16_t>(S(U(el(0, 1) - el(1, 0)) * U(k)) >> 14));
        return;
    }
    uint32_t i = a0 < v1 ? 1u : 0u;                                           // 0x800717E8
    if (g.S16(m + 8u * i) < a1) i = 2;                                        // 0x800717F4..0x80071808
    const int32_t j = next[i], k2 = next[j];
    const int32_t d = S((U(el(S(i), S(i))) - U(el(j, j) + el(k2, k2))) << 4);  // 0x80071834..0x80071850
    const int32_t s = SqrtGte(S(U(d) + 0x10000u), t.sqrt);
    g.W16(q + 2u * i, static_cast<uint16_t>(U(s) >> 1));                      // 0x80071874
    const int32_t k = rc::Recip(S(U(s) << 3));
    g.W16(q + 6u, static_cast<uint16_t>(S(U(el(j, k2) - el(k2, j)) * U(k)) >> 14)); // 0x800718E4..0x80071914
    g.W16(q + 2u * U(j), static_cast<uint16_t>(S(U(el(S(i), j) + el(j, S(i))) * U(k)) >> 14));
    g.W16(q + 2u * U(k2), static_cast<uint16_t>(S(U(el(S(i), k2) + el(k2, S(i))) * U(k)) >> 14));
}

// ============================================================================ the cell walker

int32_t IdInList(GuestRam& g, uint32_t id, uint32_t list, int32_t n) {
    for (int32_t i = 0; i < n; ++i, list += 4u)
        if (g.U32(list) == id) return 1;
    return 0;
}

int32_t CellFar(GuestRam& g, uint32_t ext, uint32_t p) {
    if (ext == 0) return 1;                                                   // 0x8009F054
    const uint32_t b = PlayerBike(g, p);
    if (g.U16(b + 362u) == 1u) return 0;                                      // 0x8009F078..0x8009F08C
    for (int32_t i = 0; i < 4; ++i, ext += 12u) {
        const int32_t road = g.S32(ext);
        if (road < 0) return 0;                                               // 0x8009F09C
        if (U(road) != g.U16(b + 360u)) continue;
        const int32_t along = g.S32(b + 368u) >> 10;
        if (!(Iabs(S(g.U32(ext + 4u) - U(along))) > 19200)) continue;         // 0x8009F0B4..0x8009F0D4
        if (Iabs(S(g.U32(ext + 8u) - U(along))) > 19200) return 1;            // 0x8009F0DC..0x8009F0F8
    }
    return 0;
}

int32_t PieceInCell(GuestRam& g, uint32_t key, uint32_t ext) {
    if (S(key) < 0 || ext == 0) return 0;                                     // 0x8009F144 / 0x8009F14C
    const uint32_t b = RoadBttRecord(g, S(key));
    if (b == 0) return 0;
    const uint32_t r = g.U32(b + 12u);
    if (r == 0) return 0;
    const int32_t n = g.S16(r + 30u);
    uint32_t c = g.U32(r + 72u);
    if (n <= 0 || c == 0) return 0;                                           // 0x8009F184 / 0x8009F18C
    for (int32_t i = 0; i < n; ++i, c += 20u) {
        if (c == 0) return 0;                                                 // 0x8009F19C
        uint32_t e = ext;
        for (int32_t j = 0; j < 4; ++j, e += 12u) {
            const int32_t road = g.S32(e);
            if (road < 0) break;                                              // 0x8009F1B4
            const uint32_t childRoad = g.U32(c + 4u);
            if (childRoad != U(road)) continue;                               // 0x8009F1CC
            const uint32_t span = g.S16(r + 16u) == 1 ? RoadFindPiece(g, r, S(childRoad)) : g.U32(r + 44u);
            if (span == 0) continue;
            if (g.S32(e + 8u) < (g.S32(span + 24u) >> 10)) continue;          // 0x8009F204..0x8009F214
            if ((g.S32(span + 28u) >> 10) < g.S32(e + 4u)) continue;          // 0x8009F21C..0x8009F22C
            return 1;
        }
        if (g.Faulted()) return 0;
    }
    return 0;
}

int32_t CellPieces(GuestRam& g, uint32_t ext, uint32_t out, int32_t max, uint32_t sp) {
    const uint32_t F = sp - 72u;
    int32_t n = 0;
    if (ext == 0 || out == 0) return 0;                                       // 0x8009EFBC / 0x8009EFC4
    const int32_t k = ResidentPieces(g, F + 16u, 6);                          // 0x8009EFCC
    for (int32_t i = 0; i < k; ++i) {
        const uint32_t key = g.U32(F + 16u + 4u * U(i));
        if ((PieceInCell(g, key, ext) & 0xFF) == 0) continue;                 // 0x8009EFEC
        if (!(n < max)) continue;
        g.W32(out, g.U32(F + 16u + 4u * U(i)));                               // 0x8009F008..0x8009F010
        out += 4u;
        ++n;
    }
    return n;
}

int32_t CellsAround(GuestRam& g, uint32_t out, uint32_t p, uint32_t sp) {
    const uint32_t F = sp - 336u;
    const uint32_t bike = PlayerBike(g, p);
    const uint32_t rider = g.U32(bike + 852u);
    const uint32_t s1 = g.U32(rider + 604u) < 3u ? bike : rider;              // 0x8009FB1C..0x8009FB30
    int32_t n = 0;
    const uint32_t own = g.U32(s1 + 176u);
    if (own != 0xFFFFFFFFu) {                                                 // 0x8009FB3C
        g.W32(out, own);
        n = 1;
    }
    if (g.U16(s1 + 362u) != 0) return n;                                      // 0x8009FB4C
    const int32_t d = RoadEndNode(g, s1 + 360u, F + 296u, 1);                 // 0x8009FB64
    int32_t s0 = 0x960000;                                                    // 150.0
    uint32_t w = out + 4u * U(n);
    for (int32_t k = 0; k < 2; ++k, s0 += 0x640000) {                         // + 100.0
        if (d < s0) continue;                                                 // 0x8009FB80
        g.W16(F + 16u, 192);                                                  // 0x8009FB90
        g.W32(F + 204u, g.U32(s1 + 360u));
        g.W32(F + 208u, g.U32(s1 + 364u));
        const uint32_t along = g.U32(s1 + 368u);
        g.W32(F + 212u, along);
        g.W32(F + 212u, g.S32(s1 + 364u) > 0 ? along + U(s0) : along - U(s0));  // 0x8009FBC0..0x8009FBCC
        const int32_t cell = PositionCell(g, F + 16u, 0);                     // 0x8009FBD4
        g.W32(F + 20u, U(cell));
        if (IdInList(g, U(cell), out, n) != 0) continue;                      // 0x8009FBE8
        g.W32(w, g.U32(F + 20u));                                             // 0x8009FBF8..0x8009FC00
        w += 4u;
        ++n;
        if (g.Faulted()) return n;
    }
    return n;
}

int32_t RecordJunction(GuestRam& g, uint32_t rec, uint32_t list, int32_t n) {
    for (int32_t i = 0; i < n; ++i, list += 4u) {
        const uint32_t c = PieceChild(g, g.U32(rec + 8u), g.U32(list));       // 0x8009F2C0
        if (c == 0) continue;
        int32_t a1 = g.S32(rec + 32u);
        int32_t s0;
        if (g.U32(c + 12u) != 0) {                                            // 0x8009F2D4..0x8009F300
            a1 = S(0u - U(a1));
            s0 = S(((g.U32(c + 8u) + g.U32(c + 16u)) << 10) - g.U32(rec + 36u));
        } else {
            s0 = S((g.U32(c + 8u) << 10) + g.U32(rec + 36u));                 // 0x8009F304..0x8009F310
        }
        if (s0 >= 0 && !(S((g.U32(c + 8u) + g.U32(c + 16u)) << 10) < s0)) {  // 0x8009F314..0x8009F334
            const uint16_t id = g.U16(c + 4u);                                // 0x8009F388
            g.W32(rec + 36u, U(s0));
            g.W32(rec + 32u, U(a1));
            g.W32(rec + 8u, id);
            return 1;
        }
        const uint32_t road = GraphRoad(g, g.U32(c + 4u));                    // 0x8009F340
        if (road == 0) continue;
        const uint32_t node = (s0 < 0 ? g.U32(road + 8u) : g.U32(road + 12u)) & 0xFFFFu;
        g.W32(rec + 8u, node | 0x10000u);                                     // 0x8009F370..0x8009F384
        g.W32(rec + 36u, 0);
        g.W32(rec + 32u, 0);
    }
    return 0;
}

int32_t RecordWindow(GuestRam& g, uint32_t rec, uint32_t list, int32_t n, uint32_t p, uint32_t sp) {
    const uint32_t F = sp - 56u;
    uint32_t gate = 0;
    if (g.S16(rec + 6u) >= 0) {                                               // 0x8009C518
        const uint32_t b = RoadBttRecord(g, g.S16(rec + 6u));                 // 0x8009C520
        if (b == 0) return 0;
        gate = g.U32(b + 12u);
    } else {
        if ((g.U32(rec + 8u) & 0xFFFE0000u) != 0 && RecordJunction(g, rec, list, n) == 0) return 0; // 0x8009C53C..
        g.W32(F + 24u, g.U32(rec + 8u));                                      // 0x8009C560
        g.W32(F + 28u, U(static_cast<int32_t>(g.S16(rec + 60u))));
        g.W32(F + 32u, g.U32(rec + 36u));
        gate = RoadGate(g, 0, F + 24u);                                       // 0x8009C580
    }
    if (gate == 0) return 0;                                                  // 0x8009C58C
    if (g.S16(rec + 6u) < 0) g.W16(rec + 6u, g.U16(gate));                    // 0x8009C594..0x8009C5AC
    return WindowPred(g, g.U16(rec), rec + 20u, p, 0, 0);                     // 0x8009C5BC
}

void RecordStamp(GuestRam& g, uint32_t slot, uint32_t kind, uint32_t idx, uint32_t value) {
    if (!(g.U8(GameState(g) + 4u) & 0x10u)) return;                           // 0x8009C428..0x8009C434
    if (slot == 0) return;                                                    // 0x8009C43C
    const uint32_t r0 = g.U32(g.U32(slot + 4u) + 32u);                        // 0x8009C444..0x8009C44C
    const uint32_t i = idx & 0xFFu;
    uint32_t rec = 0;
    switch (kind) {                                                           // jump table 0x8005B8BC
    case 0: rec = g.U32(r0 + 52u) + 64u * i; break;
    case 2: rec = g.U32(r0 + 40u) + 76u * i; break;
    case 3: rec = g.U32(r0 + 36u) + 68u * i; break;
    case 4: rec = g.U32(r0 + 44u) + 64u * i; break;
    case 6: rec = g.U32(r0 + 48u) + 88u * i; break;
    default: return;
    }
    g.W16(rec + 4u, static_cast<uint16_t>(value));
}

int32_t PropKind(GuestRam& g, int32_t cls) {
    const int32_t idx = g.S16(kWpPropClass);
    if (idx == -1) return -1;                                                 // 0x8009C5F0
    const uint32_t reg = kWpRegistry + 16u * U(idx);
    if (!(cls < static_cast<int32_t>(g.U8(reg + 4u)))) return -1;              // 0x8009C60C
    const uint32_t dod = g.U32(g.U32(reg + 8u) + 12u * U(cls));
    return static_cast<int32_t>((g.U16(dod + 14u) & 0xF80u) >> 7);
}

int32_t VolumeRoom(GuestRam& g, uint32_t p) {
    if (Players(g) != 2u) return 1;                                           // 0x8008D9F8
    return g.S32(kWpPool6Ctrl + 16u + 4u * p) < 16 ? 1 : 0;
}

bool RecordSpawn(GuestRam& g, uint32_t rec, uint32_t cellId, uint32_t p, uint32_t sp, const BikeTables& t,
                 RecoverCallees& c, uint32_t& v0) {
    const uint32_t F = sp - 40u;
    uint32_t s2 = 0;
    v0 = 0;
    if (g.S16(rec + 4u) > 0) return !g.Faulted();                             // 0x8009C678
    const uint32_t kind = g.U16(rec);
    switch (kind) {                                                           // jump table 0x8005B8DC
    case 0: {                                                                 // 0x8009C6B0 (dead: no kind-0 record on disc)
        if (g.S32(0x800D86F4u) > 0) break;
        const uint32_t r = GuestRand(g);                                      // 0x8009C6C4
        const uint32_t m = (r - 11u * (r / 11u)) << 16;
        if (!(S(m) < g.S32(rec + 48u))) break;
        const uint32_t e = FindFreeCop(g);                                    // 0x8009C708
        if (e == 0) break;
        const uint32_t key = g.U32(rec + 8u);
        if ((key >> 16) != 0) break;                                          // 0x8009C728
        g.W32(e + 360u, key);
        g.W32(e + 368u, g.U32(rec + 36u));
        g.W32(e + 364u, U(static_cast<int32_t>(g.S16(rec + 60u))));
        s2 = 1;
        g.W32(e + 344u, g.U32(rec + 32u));
        if (!Seam(c, kWpResetBikeFn, {e}, F)) return false;                    // 0x8009C754
        const uint32_t r2 = g.U32(e + 852u);
        g.W16(e + 320u, 1);
        g.W32(e + 924u, 0);
        g.W32(r2 + 604u, 1);
        if (!Seam(c, kWpTransitionFn, {e, 0u}, F)) return false;               // 0x8009C778
        break;
    }
    case 2: {                                                                 // 0x8009C788: a pedestrian
        if (Budget(g, 2) == 0) break;
        const uint32_t b = PlayerBike(g, p);
        if (g.U32(rec + 8u) == g.U32(b + 360u) && g.U32(GameState(g) + 16u) != 0 &&
            Iabs(S(g.U32(b + 368u) - g.U32(rec + 36u))) <= 0)                 // 0x8009C7A8..0x8009C7F0
            break;
        uint32_t r = 0;
        if (!Seam(c, kWpPedSpawnFn, {rec, 1u, b}, F, &r)) return false;       // 0x8009C800
        if (r != 0) s2 = 1;
        break;
    }
    case 6: {                                                                 // 0x8009C810: a collision volume
        if (Budget(g, 6) == 0) break;
        if (VolumeRoom(g, p) == 0) break;
        uint32_t e = 0;
        if (!VolumeSpawn(g, rec, PlayerBike(g, p), F, t, e)) return false;    // 0x8009C840
        if (e == 0) break;
        s2 = g.U16(e);
        break;
    }
    case 4: {                                                                 // 0x8009C85C
        const uint32_t cls = g.U16(rec + 2u);
        if (cls == 50u) {
            if (!(g.U8(GameState(g) + 4u) & 0x10u)) break;
            Class50(g, rec, cellId, p);                                       // 0x8009C88C
            s2 = 1;
            break;
        }
        if (cls == 9u || cls == 0u) {                                         // 0x8009C8AC: the hazard objects
            if (g.U8(GameState(g) + 4u) & 0x10u) break;
            if (!(g.S32(kWpHazardCount) < 3)) break;
            uint32_t a0 = kWpHazardTable;
            for (int32_t k = 0; k < 2; ++k, a0 += 4u) {
                const int32_t v1 = g.S8(a0);
                if (v1 < 0) break;                                            // 0x8009C8F0
                if (U(v1) != g.U16(rec + 2u)) continue;
                int32_t slot = Iabs(g.S8(a0 + 1u));                           // 0x8009C908..0x8009C920
                if (!(slot < 6)) {
                    slot = g.S32(kWpHazardNext);                              // 0x8009C930
                    if (!(slot < 6)) break;
                    uint32_t o = g.U32(kWpHazardPool) + 312u * U(slot);
                    for (;;) {
                        if (g.S8(o) == 0) {                                   // 0x8009C960
                            g.W8(o, 0xFF);                                    // 0x8009C9D0
                            break;
                        }
                        ++slot;
                        if (!(slot < 6)) break;
                        o += 312u;
                    }
                    if (!(slot < 6)) break;                                   // 0x8009C980
                }
                s2 = 1;                                                       // 0x8009C994
                const uint32_t obj = g.U32(kWpHazardPool) + 312u * U(slot);
                if (!Seam(c, kWpHazardSpawnFn, {g.U16(rec + 2u), 1u, rec + 20u, 0u, obj}, F)) return false; // 0x8009C9BC
                break;
            }
            break;
        }
        const int32_t k = PropKind(g, S(cls));                                // 0x8009C9F0
        if (k < 0) break;
        const uint32_t b = PlayerBike(g, p);
        uint32_t e = 0;
        if (U(k) < 6u) {
            if (Budget(g, 4) == 0) break;
            if (!PropAlloc4(g, rec, b, F, t, c, e)) return false;             // 0x8009CA30
        } else {
            if (Budget(g, 5) == 0) break;
            if (!PropAlloc5(g, rec, b, F, t, c, e)) return false;             // 0x8009CA50
        }
        if (e != 0) s2 = 1;
        break;
    }
    default: break;                                                           // 1, 3, 5: 0x8009CA64
    }
    if (s2 != 0) g.W16(rec + 4u, static_cast<uint16_t>(s2));                  // 0x8009CA6C
    v0 = s2;
    return !g.Faulted();
}

bool CellRecords(GuestRam& g, uint32_t id, uint32_t r0, uint32_t ext, int32_t onRoute, uint32_t p, uint32_t sp,
                 const BikeTables& t, RecoverCallees& c) {
    const uint32_t F = sp - 80u;
    if (r0 == 0) return !g.Faulted();                                         // 0x8009CAC4
    if (CellFar(g, ext, p) != 0) return !g.Faulted();                         // 0x8009CAD0
    const uint32_t s7 = OtherSlot(g, id, p);                                  // 0x8009CAE0
    const int32_t s6 = CellPieces(g, ext, F + 16u, 6, F);                     // 0x8009CAF4
    if (s6 <= 0) return !g.Faulted();
    const uint32_t cell = g.U32(r0 + 12u);
    uint32_t v = 0;
    // kind 2, the pedestrians (one player only)
    if (Players(g) == 1u && g.U16(r0 + 2u) != 0) {                            // 0x8009CB10..0x8009CB28
        for (uint32_t i = 0; i < g.U16(r0 + 2u); ++i) {
            const uint32_t rec = g.U32(r0 + 40u) + 76u * i;
            if (g.S16(rec + 4u) > 0) continue;
            if (RecordWindow(g, rec, F + 16u, s6, p, F) == 0) continue;
            if (!RecordSpawn(g, g.U32(r0 + 40u) + 76u * i, g.U32(r0 + 12u), p, F, t, c, v)) return false;
            if (g.Faulted()) return false;
        }
    }
    // kind 3 (empty on the disc)
    for (uint32_t i = 0; i < g.U16(r0); ++i) {                                // 0x8009CB88
        const uint32_t rec = g.U32(r0 + 36u) + 68u * i;
        if (g.S16(rec + 4u) > 0) continue;
        if (RecordWindow(g, rec, F + 16u, s6, p, F) == 0) continue;
        if (!RecordSpawn(g, g.U32(r0 + 36u) + 68u * i, g.U32(r0 + 12u), p, F, t, c, v)) return false;
        if ((v & 0xFFFFu) != 0) RecordStamp(g, s7, 3, i & 0xFFu, v & 0xFFFFu);
        if (g.Faulted()) return false;
    }
    // kind 6, the collision volumes
    for (uint32_t i = 0; i < g.U16(r0 + 6u); ++i) {                           // 0x8009CC0C
        const uint32_t rec = g.U32(r0 + 48u) + 88u * i;
        if (g.S16(rec + 4u) > 0) {                                            // 0x8009CC44
            if (!(g.U8(GameState(g) + 4u) & 0x10u)) continue;                 // 0x8009CCF8..0x8009CD0C
            if (VolumeMask(g, 6, g.U16(rec + 4u), p) == 0) continue;
            VolumeLists(g);                                                   // 0x8009CD28
            continue;
        }
        if (RecordWindow(g, rec, F + 16u, s6, p, F) == 0) continue;           // 0x8009CC58
        if (!RecordSpawn(g, g.U32(r0 + 48u) + 88u * i, g.U32(r0 + 12u), p, F, t, c, v)) return false;
        if (((v - 1u) & 0xFFFFu) < 223u) {                                    // 0x8009CC7C..0x8009CC88
            const uint32_t vol = g.U32(kWpPool6Ptr) + 280u * (v & 31u);
            if (vol != 0) {
                g.W8(vol + 2u, static_cast<uint8_t>(i));                      // 0x8009CCBC
                g.W8(vol + 3u, static_cast<uint8_t>(1u << (p & 31u)));
                g.W32(vol + 4u, g.U32(r0 + 12u));
            }
            RecordStamp(g, s7, 6, i & 0xFFu, v & 0xFFFFu);                    // 0x8009CCD0
            VolumeLists(g);                                                   // 0x8009CD28
        } else if (!VolumeEvict(g, g.U32(r0 + 48u) + 88u * i, p)) {           // 0x8009CCE8
            return false;
        }
        if (g.Faulted()) return false;
    }
    // kind 4, the props (and class 50)
    for (uint32_t i = 0; i < g.U16(r0 + 4u); ++i) {                           // 0x8009CD48
        const uint32_t rec = g.U32(r0 + 44u) + 64u * i;
        if (g.S16(rec + 4u) > 0) continue;
        const uint32_t cls = g.U16(rec + 2u);
        if (onRoute == 0 && cls == 30u) continue;                             // 0x8009CD7C..0x8009CD8C
        if (cls == 50u && !(g.U8(GameState(g) + 4u) & 0x10u)) continue;       // 0x8009CD9C..0x8009CDB8
        bool direct = false;
        if (cls == 50u) {
            if ((g.U32(rec + 8u) & 0xFFFE0000u) != 0 && RecordJunction(g, rec, F + 16u, s6) == 0) continue; // 0x8009CDDC
            direct = g.U16(rec + 2u) == 50u;                                  // 0x8009CDEC
        }
        if (!direct && RecordWindow(g, g.U32(r0 + 44u) + 64u * i, F + 16u, s6, p, F) == 0) continue;
        if (!RecordSpawn(g, g.U32(r0 + 44u) + 64u * i, g.U32(r0 + 12u), p, F, t, c, v)) return false; // 0x8009CE28
        if ((v & 0xFFFFu) != 0 && g.U16(rec + 2u) != 50u) RecordStamp(g, s7, 4, i & 0xFFu, v & 0xFFFFu);
        if (g.Faulted()) return false;
    }
    // kind 0 (empty on the disc)
    for (uint32_t i = 0; i < g.U16(r0 + 8u); ++i) {                           // 0x8009CE6C
        const uint32_t rec = g.U32(r0 + 52u) + 64u * i;
        if (g.S16(rec + 4u) > 0) continue;
        if (RecordWindow(g, rec, F + 16u, s6, p, F) == 0) continue;
        if (!RecordSpawn(g, g.U32(r0 + 52u) + 64u * i, g.U32(r0 + 12u), p, F, t, c, v)) return false;
        if ((v & 0xFFFFu) != 0) RecordStamp(g, s7, 0, i & 0xFFu, v & 0xFFFFu);
        if (g.Faulted()) return false;
    }
    (void)cell;
    return !g.Faulted();
}

bool CellWalker(GuestRam& g, uint32_t sp, const BikeTables& t, RecoverCallees& c) {
    const uint32_t F = sp - 72u;
    for (uint32_t p = 0; S(p) < S(Players(g)); ++p) {                         // 0x8009C334..0x8009C3EC
        const int32_t n = CellsAround(g, F + 24u, p, F);                      // 0x8009C354
        const int32_t onRoute = RouteValid(g, PlayerBike(g, p) + 172u);       // 0x8009C368
        for (int32_t k = 0; k < n; ++k) {
            const uint32_t id = g.U32(F + 24u + 4u * U(k));
            const uint32_t slot = CellSlotFor(g, id, p);                      // 0x8009C380
            if (slot == 0 || g.U32(slot + 8u) != g.U32(F + 24u + 4u * U(k))) continue;
            g.W32(F + 16u, p);                                                // 0x8009C3B4
            if (!CellRecords(g, g.U32(F + 24u + 4u * U(k)), g.U32(g.U32(slot + 4u) + 32u), g.U32(slot + 76u), onRoute,
                             p, F, t, c))
                return false;
            if (g.Faulted()) return false;
        }
    }
    return !g.Faulted();
}

// ============================================================================ props

bool PropRows(GuestRam& g, uint32_t rec, uint32_t e, const BikeTables& t) {
    const uint32_t f = g.U16(rec + 12u);
    if (!(f & 1u)) {                                                          // the identity
        g.W16(e + 432u, 0x1000);
        g.W16(e + 434u, 0);
        g.W16(e + 436u, 0);
        g.W16(e + 438u, 0);
        g.W16(e + 440u, 0x1000);
        g.W16(e + 442u, 0);
        g.W16(e + 444u, 0);
        g.W16(e + 446u, 0);
        g.W16(e + 448u, 0x1000);
    } else {                                                                  // the slice's frame
        const uint32_t sl = g.U32(e + 340u);
        for (uint32_t k = 0; k < 8; ++k) g.W16(e + 432u + 2u * k, g.U16(sl + 2u + 2u * k));
        const uint16_t z = g.U16(sl + 18u);
        g.W16(e + 438u, static_cast<uint16_t>(0u - g.U16(e + 438u)));
        g.W16(e + 442u, static_cast<uint16_t>(0u - g.U16(e + 442u)));
        g.W16(e + 448u, z);
        g.W16(e + 440u, static_cast<uint16_t>(0u - g.U16(e + 440u)));
    }
    if (!(g.U16(rec + 12u) & 2u)) return !g.Faulted();                        // 0x800A3FF0..0x800A3FFC
    g.W16(e + 444u, g.U16(rec + 14u));
    g.W16(e + 446u, g.U16(rec + 16u));
    g.W16(e + 448u, g.U16(rec + 18u));
    if (g.U16(rec + 12u) & 1u) {                                              // 0x800A403C: OP(up diag, n), normalised
        rc::GteOp(g, e + 438u, e + 444u, e + 432u);
        int32_t sum = 0;
        if (!rc::GNormalize(g, e + 432u, t, sum)) return false;               // 0x800A4098
    } else {
        g.W16(e + 432u, g.U16(rec + 18u));                                    // 0x800A40A8
        g.W16(e + 436u, static_cast<uint16_t>(0u - g.U16(rec + 14u)));
    }
    return !g.Faulted();
}

bool PropInit(GuestRam& g, uint32_t rec, uint32_t e, uint32_t pool, uint32_t bike, uint32_t sp,
              const BikeTables& t, RecoverCallees& c, uint32_t& v0) {
    const uint32_t F = sp - 96u;
    RoadRuntimeNative road;
    uint32_t fail = 1;
    g.W32(F + 16u, g.U32(rec + 8u));                                          // 0x800A0804..0x800A0828
    g.W32(F + 24u, g.U32(rec + 36u));
    g.W32(F + 20u, U(static_cast<int32_t>(g.S16(rec + 60u))));
    const uint32_t gate = RoadGate(g, 0, F + 16u);                            // 0x800A0824
    if (gate != 0 && CursorSeat(g, gate, F + 16u, F + 32u, F) != 0) {          // 0x800A083C
        if (g.U16(F + 18u) == 1u) JunctionSlice(g, F + 32u, rec + 20u);        // 0x800A0860
        GuestCopyWords(g, e + 328u, F + 32u, 32u);                            // 0x800A0874
        g.W32(e + 184u, g.U32(rec + 20u));
        g.W32(e + 188u, g.U32(rec + 24u));
        g.W32(e + 192u, g.U32(rec + 28u));
        RoadProjectView(g, e + 184u, g.U32(e + 340u), e + 344u, e + 348u, F); // 0x800A08A4
        uint32_t mv = 0;
        if (!ModelBind(g, e, 6, g.U16(rec + 2u), 0, mv)) return false;        // 0x800A08B8
        CarSetup(g, e, g.U16(rec + 2u), F);                                   // 0x800A08C4
        if (!PropRows(g, rec, e, t)) return false;                            // 0x800A08D0
        RoadPosition(g, e + 444u, e + 328u, e + 360u, F);                     // 0x800A08E0
        g.W32(e + 364u, g.U32(F + 20u));
        RouteBind(g, e + 172u, 0, bike, F, road);                             // 0x800A08FC
        g.W32(e + 324u, U(ProgressScalar(g, e + 172u, F)));                   // 0x800A0904
        g.W32(e + 180u, g.U16(rec + 2u));
        g.W16(e + 322u, g.U16(rec + 62u));
        const uint32_t live = RoadWindow(g, e + 172u, F);                     // 0x800A0924
        g.W16(e + 320u, static_cast<uint16_t>(live));
        if ((live & 0xFFFFu) != 0) {
            RoadClass(g, e, 1, 0, -1, F, road);                               // 0x800A0944
            fail = 0;
            const uint32_t sl = g.U32(e + 340u);
            const int32_t d = GProject(g, e + 184u, sl + 8u, sl + 20u);       // 0x800A095C
            GMulAdd(g, e + 184u, g.U32(e + 340u) + 8u, S(0u - U(d)), e + 184u);   // 0x800A0974
            if (!Seam(c, kWpBuildObbFn, {e}, F)) return false;                // 0x800A097C
            Heading(g, e + 444u, e + 448u, e + 292u);                         // 0x800A0990..0x800A09E0
            g.W32(e + 176u, 0xFFFFFFFFu);
        }
    }
    if (g.Faulted()) return false;
    if (fail != 0 && !PoolRelease(g, e + 172u, S(pool))) return false;       // 0x800A09EC
    v0 = fail ^ 1u;
    return !g.Faulted();
}

namespace {
// The shared store record of pools 4 / 5 (0x800A2718..0x800A2788 / 0x800A2518..0x800A2588): the lowest
// free one of the 18 goes to e+4, the low-free index moves past the next used ones.
void TakeStore(GuestRam& g, uint32_t e) {
    const int32_t a2 = g.S32(kWpStoreLow);
    int32_t t0 = a2 + 1;
    uint32_t r = kWpStore + 24u * U(t0);
    while (t0 < 18 && g.U32(r) != 0) {
        ++t0;
        r += 24u;
    }
    g.W32(kWpStoreLow, U(t0));
    g.W32(e + 4u, kWpStore + 24u * U(a2));
}
} // namespace

bool PropAlloc4(GuestRam& g, uint32_t rec, uint32_t bike, uint32_t sp, const BikeTables& t, RecoverCallees& c,
                uint32_t& v0) {
    const uint32_t F = sp - 32u;
    v0 = 0;
    const int32_t next = g.S32(kWpPool4Ctrl + 4u), high = g.S32(kWpPool4Ctrl + 8u);
    if (!(next < high) && g.U32(kWpStoreFree) < 596u) return !g.Faulted();    // 0x800A2658..0x800A2674
    const int32_t t1 = next;
    int32_t a0 = t1 + 1;
    uint32_t a1 = g.U32(kWpPool4Ctrl + 12u) + 596u * U(a0);
    while (a0 < high + 1 && g.U16(a1 + 172u) != 0) {                          // 0x800A26B8
        ++a0;
        a1 += 596u;
    }
    g.W32(kWpPool4Ctrl + 4u, U(a0));
    const uint32_t e = g.U32(kWpPool4Ctrl + 12u) + 596u * U(t1);
    g.W16(e + 172u, static_cast<uint16_t>(t1 + 128));                        // 0x800A2710
    TakeStore(g, e);
    g.W32(kWpPool4Ctrl, g.U32(kWpPool4Ctrl) + 1u);                            // 0x800A2794
    if (g.S32(kWpPool4Ctrl + 8u) < t1) {
        g.W32(kWpPool4Ctrl + 8u, U(t1));
        g.W32(kWpStoreFree, g.U32(kWpStoreFree) - 596u);
    }
    if (e == 0) return !g.Faulted();
    uint32_t ok = 0;
    if (!PropInit(g, rec, e, 4, bike, F, t, c, ok)) return false;             // 0x800A27C4
    if (ok == 0) return !g.Faulted();
    int32_t mass = S(U(FixMul(g.S32(e + 312u), FixMul(g.S32(e + 308u), g.S32(e + 304u)))) << 9); // 0x800A27D4..0x800A27F0
    g.W32(e + 316u, U(mass));
    if (0x800000 < mass) mass = 0x800000;
    g.W32(e + 316u, U(mass));
    g.W32(e + 504u, g.U32(e + 184u));
    g.W32(e + 508u, g.U32(e + 188u));
    g.W32(e + 512u, g.U32(e + 192u));
    rc::CopyHalfwords(g, 9, e + 432u, e + 516u);                              // 0x800A2834
    g.W32(e + 480u, 0);
    g.W32(e + 592u, 0);
    g.W16(e + 450u, g.U16(e + 444u));
    g.W16(e + 452u, g.U16(e + 446u));
    g.W16(e + 454u, g.U16(e + 448u));
    RoadRuntimeNative road;
    RoadsideRun(g, e, 1, -1, F, road);                                        // 0x800A2864
    MatrixQuat(g, e + 432u, e + 560u, t);                                     // 0x800A2870
    v0 = e;
    return !g.Faulted();
}

bool PropAlloc5(GuestRam& g, uint32_t rec, uint32_t bike, uint32_t sp, const BikeTables& t, RecoverCallees& c,
                uint32_t& v0) {
    const uint32_t F = sp - 24u;
    v0 = 0;
    const int32_t next = g.S32(kWpPool5Ctrl + 4u), high = g.S32(kWpPool5Ctrl + 8u);
    if (!(next < high) && g.U32(kWpStoreFree) < 452u) return !g.Faulted();    // 0x800A246C..0x800A2488
    const int32_t t1 = next;
    int32_t a0 = t1 + 1;
    uint32_t a1 = g.U32(kWpPool5Ctrl + 12u) - 452u * U(a0);
    while (a0 < high + 1 && g.U16(a1 + 172u) != 0) {                          // 0x800A24C4
        ++a0;
        a1 -= 452u;
    }
    g.W32(kWpPool5Ctrl + 4u, U(a0));
    const uint32_t e = g.U32(kWpPool5Ctrl + 12u) - 452u * U(t1);
    g.W16(e + 172u, static_cast<uint16_t>(t1 + 160));                        // 0x800A2514
    TakeStore(g, e);
    g.W32(kWpPool5Ctrl, g.U32(kWpPool5Ctrl) + 1u);
    if (g.S32(kWpPool5Ctrl + 8u) < t1) {
        g.W32(kWpPool5Ctrl + 8u, U(t1));
        g.W32(kWpStoreFree, g.U32(kWpStoreFree) - 452u);
    }
    if (e == 0) return !g.Faulted();
    uint32_t ok = 0;
    if (!PropInit(g, rec, e, 5, bike, F, t, c, ok)) return false;             // 0x800A25C8
    if (ok == 0) return !g.Faulted();
    int32_t mass = S(U(FixMul(g.S32(e + 312u), FixMul(g.S32(e + 308u), g.S32(e + 304u)))) << 10); // 0x800A25D8..0x800A25F4
    g.W32(e + 316u, U(mass));
    if (0x1000000 < mass) mass = 0x1000000;
    g.W32(e + 316u, U(mass));
    v0 = e;
    return !g.Faulted();
}

bool PropSettle(GuestRam& g, uint32_t e, uint32_t sp, RecoverCallees& c) {
    const uint32_t F = sp - 72u;
    uint32_t s1 = 0, s2 = 0;
    int32_t v1 = 0;
    if (g.U32(e + 388u) & 1u) {                                               // 0x800A3AA0: off the road
        s2 = F + 24u;
        s1 = F + 40u;
        uint32_t r = 0;
        if (!Seam(c, kWpGroundQueryFn, {e, 0u, s2, s1, g.U32(e + 536u)}, F, &r)) return false; // 0x800A3ACC
        v1 = S(r);
        g.W32(e + 536u, r);
    } else {
        g.W8(e + 534u, g.U32(e + 372u) != 0 ? g.U8(e + 394u) : static_cast<uint8_t>(1)); // 0x800A3AE0..0x800A3B00
        g.W32(e + 536u, 0);
    }
    bool slope;
    if (v1 > 0) {                                                             // 0x800A3B08
        slope = true;
    } else {
        const uint32_t sl = g.U32(e + 340u);
        s2 = sl + 20u;
        s1 = sl + 8u;
        slope = v1 >= 0;                                                      // 0x800A3B1C
        if (!slope && g.U32(e + 480u) == 0) {                                 // 0x800A3B24
            g.W32(e + 480u, 0x1C9C4u);
            GScale(g, 0x1C9C4, e + 450u, e + 456u);                           // 0x800A3B48
        }
    }
    if (slope && g.S16(s1 + 2u) < -614) {                                     // 0x800A3B58: the normal is steep enough
        g.W16(e + 522u, static_cast<uint16_t>(0u - g.U16(s1)));
        g.W16(e + 524u, static_cast<uint16_t>(0u - g.U16(s1 + 2u)));
        g.W16(e + 526u, static_cast<uint16_t>(0u - g.U16(s1 + 4u)));
    } else {                                                                  // 0x800A3B6C
        g.W16(F + 40u, static_cast<uint16_t>(0u - g.U16(e + 522u)));
        g.W16(F + 42u, static_cast<uint16_t>(0u - g.U16(e + 524u)));
        g.W16(F + 44u, static_cast<uint16_t>(0u - g.U16(e + 526u)));
        s2 = e + 504u;
    }
    int32_t d[3];
    for (uint32_t k = 0; k < 3; ++k) {                                        // 0x800A3BD0..0x800A3C08
        d[k] = S(g.U32(s2 + 4u * k) - g.U32(e + 184u + 4u * k));
        g.W32(F + 24u + 4u * k, U(d[k]));
    }
    int32_t sum = 0;
    for (uint32_t k = 0; k < 3; ++k) {
        const int64_t prod = static_cast<int64_t>(d[k]) * static_cast<int64_t>(S(U(static_cast<int32_t>(g.S16(e + 522u + 2u * k))) << 4));
        sum = S(U(sum) + static_cast<uint32_t>(static_cast<uint64_t>(prod) >> 16));
    }
    GMulAdd(g, e + 184u, e + 522u, sum, e + 504u);                            // 0x800A3C9C
    return !g.Faulted();
}

bool PropPass(GuestRam& g, int32_t dt, uint32_t sp, RecoverCallees& c) {
    const uint32_t F = sp - 48u;
    const uint32_t rec = kWpPoolTable + 64u;                                  // pool 4
    int32_t n = g.S32(g.U32(rec + 12u));
    uint32_t e = g.U32(rec);
    RoadRuntimeNative road;
    for (; n >= 0; --n, e += g.U32(rec + 4u)) {                               // 0x800A28D8..0x800A2A38
        if (g.U16(e + 172u) == 0) continue;
        if (g.S16(e + 320u) != 0 && g.S32(e + 480u) > 0) {                    // 0x800A28E8..0x800A2900
            const uint32_t s2 = (g.U32(e + 592u) >> 1) & 1u;
            const uint32_t base = e + 504u + (s2 != 0 ? 76u : 0u);
            g.W32(e + 472u, g.U32(e + 188u));                                 // 0x800A2938..0x800A2944
            g.W32(e + 476u, g.U32(e + 192u));
            g.W32(e + 468u, g.U32(e + 184u));
            GMulAdd32(g, base, e + 456u, dt, base);                           // 0x800A2940
            if (s2 != 0) {
                GMulAdd(g, e + 580u, e + 438u, g.S32(e + 312u) >> 1, e + 184u);   // 0x800A295C
            } else {
                g.W32(e + 184u, g.U32(e + 504u));
                g.W32(e + 188u, g.U32(e + 508u));
                g.W32(e + 192u, g.U32(e + 512u));
            }
            const uint32_t s1 = RoadRebindBody(g, e, F, road);               // 0x800A2984
            bool obb = false;
            if (s1 != 0 || s2 != 0) {
                if (!PropSettle(g, e, F, c)) return false;                    // 0x800A29A0
                obb = s2 != 0;
            }
            if (!obb) {
                if (s1 != 0) {                                                // 0x800A29B0
                    g.W32(e + 184u, g.U32(e + 504u));
                    g.W32(e + 188u, g.U32(e + 508u));
                    g.W32(e + 192u, g.U32(e + 512u));
                    g.W32(e + 592u, g.U32(e + 592u) | 0x80u);
                }
                if (g.U32(e + 592u) & 4u)                                     // 0x800A29DC
                    GMulAdd(g, e + 504u, e + 522u, S(0u - g.U32(e + 308u)), e + 184u);
            }
            if (!Seam(c, kWpBuildObbFn, {e}, F)) return false;                // 0x800A2A04
        }
        const uint32_t live = RoadWindow(g, e + 172u, F);                     // 0x800A2A10
        g.W16(e + 320u, static_cast<uint16_t>(live));
        if ((live & 0xFFFFu) == 0 && !PoolRelease(g, e + 172u, 4)) return false; // 0x800A2A28
        if (g.Faulted()) return false;
    }
    return !g.Faulted();
}

bool Pool5Pass(GuestRam& g, uint32_t sp) {
    const uint32_t F = sp - 40u;
    const uint32_t rec = kWpPoolTable + 80u;                                  // pool 5
    int32_t n = g.S32(g.U32(rec + 12u));
    uint32_t e = g.U32(rec);
    for (; n >= 0; --n, e += g.U32(rec + 4u)) {
        const uint32_t h = g.U16(e + 172u);
        if (h == 0 || (h >> 5) != 5u) continue;
        const uint32_t live = RoadWindow(g, e + 172u, F);
        g.W16(e + 320u, static_cast<uint16_t>(live));
        if ((live & 0xFFFFu) == 0 && !PoolRelease(g, e + 172u, 5)) return false;
        if (g.Faulted()) return false;
    }
    return !g.Faulted();
}

// ============================================================================ collision volumes

bool VolumeSpawn(GuestRam& g, uint32_t rec, uint32_t bike, uint32_t sp, const BikeTables& t, uint32_t& v0) {
    const uint32_t F = sp - 112u;
    v0 = 0;
    RoadRuntimeNative road;
    g.W32(F + 24u, g.U32(rec + 8u));                                          // 0x8009BB70..0x8009BB94
    g.W32(F + 32u, g.U32(rec + 36u));
    g.W32(F + 28u, U(static_cast<int32_t>(g.S16(rec + 60u))));
    const uint32_t gate = RoadGate(g, 0, F + 24u);                            // 0x8009BB90
    if (gate == 0) return !g.Faulted();
    uint32_t e = 0;
    {                                                                         // 0x8009BBA4..0x8009BC6C: allocate
        const uint32_t base = g.U32(kWpPool6Ctrl + 28u);
        const int32_t next = g.S32(kWpPool6Ctrl + 4u), cap = g.S32(kWpPool6Cap);
        if (base != 0 && next < cap) {
            int32_t t0 = next + 1;
            uint32_t a1 = base + 280u * U(t0);
            while (t0 < cap && g.U16(a1) != 0) {
                ++t0;
                a1 += 280u;
            }
            g.W32(kWpPool6Ctrl + 4u, U(t0));
            e = g.U32(kWpPool6Ctrl + 28u) + 280u * U(next);
            g.W16(e, static_cast<uint16_t>(next + 192));
            g.W32(kWpPool6Ctrl, g.U32(kWpPool6Ctrl) + 1u);
            if (g.S32(kWpPool6Ctrl + 8u) < next) g.W32(kWpPool6Ctrl + 8u, U(next));
        }
    }
    if (e == 0) return !g.Faulted();
    g.W16(e + 150u, g.U16(rec + 62u));                                        // 0x8009BC98
    if (CursorSeat(g, gate, F + 24u, F + 40u, F) == 0) {                      // 0x8009BC94
        if (!PoolRelease(g, e, 6)) return false;                              // 0x8009BCA8
        return !g.Faulted();
    }
    if (g.U16(F + 26u) == 1u) JunctionSlice(g, F + 40u, rec + 20u);            // 0x8009BCCC
    GuestCopyWords(g, e + 156u, F + 40u, 32u);                                // 0x8009BCE0
    const uint32_t slice = g.U32(e + 168u);
    g.W32(e + 12u, g.U32(rec + 20u));
    g.W32(e + 16u, g.U32(rec + 24u));
    g.W32(e + 20u, g.U32(rec + 28u));
    RoadProjectView(g, e + 12u, slice, e + 172u, e + 176u, F);                // 0x8009BD10
    g.W16(F + 76u, 0);
    g.W16(F + 74u, 0);
    g.W16(F + 72u, 0);
    RoadPosition(g, F + 72u, e + 156u, e + 188u, F);                          // 0x8009BD2C
    g.W32(e + 192u, g.U32(F + 28u));
    RouteBind(g, e, 0, bike, F, road);                                        // 0x8009BD44
    g.W32(e + 152u, U(ProgressScalar(g, e, F)));                              // 0x8009BD4C
    g.W32(e + 12u, g.U32(rec + 20u));
    g.W32(e + 16u, g.U32(rec + 24u));
    g.W32(e + 20u, g.U32(rec + 28u));
    g.W32(e + 8u, g.U16(rec + 2u));
    // the frame: row0 +260, row1 +266, row2 +272
    if (g.U16(rec + 12u) & 1u) {                                              // 0x8009BD9C: the slice's
        g.W16(e + 260u, g.U16(slice + 14u));
        g.W16(e + 262u, g.U16(slice + 16u));
        g.W16(e + 264u, g.U16(slice + 18u));
        g.W16(e + 266u, g.U16(slice + 8u));
        g.W16(e + 268u, g.U16(slice + 10u));
        g.W16(e + 270u, g.U16(slice + 12u));
        g.W16(e + 272u, g.U16(slice + 2u));
        g.W16(e + 274u, g.U16(slice + 4u));
        g.W16(e + 276u, g.U16(slice + 6u));
        g.W16(e + 268u, static_cast<uint16_t>(0u - g.U16(e + 268u)));
        g.W16(e + 266u, static_cast<uint16_t>(0u - g.U16(e + 266u)));
        g.W16(e + 270u, static_cast<uint16_t>(0u - g.U16(e + 270u)));
        if (g.S32(e + 172u) < 0) {                                            // 0x8009BE24
            for (uint32_t k = 0; k < 3; ++k) g.W16(e + 260u + 2u * k, static_cast<uint16_t>(0u - g.U16(e + 260u + 2u * k)));
        } else {
            for (uint32_t k = 0; k < 3; ++k) g.W16(e + 272u + 2u * k, static_cast<uint16_t>(0u - g.U16(e + 272u + 2u * k)));
        }
    } else {                                                                  // 0x8009BE7C: the identity
        g.W16(e + 260u, 0x1000);
        g.W16(e + 262u, 0);
        g.W16(e + 264u, 0);
        g.W16(e + 266u, 0);
        g.W16(e + 268u, 0x1000);
        g.W16(e + 270u, 0);
        g.W16(e + 272u, 0);
        g.W16(e + 274u, 0);
        g.W16(e + 276u, 0x1000);
    }
    if (g.U32(e + 8u) == 1u) {                                                // 0x8009BEB0: a pole's square footprint
        g.W32(e + 132u, g.U32(rec + 64u));
        g.W32(e + 136u, g.U32(rec + 64u));
        g.W32(e + 140u, g.U32(rec + 80u));
        g.W32(e + 84u, g.U32(rec + 76u));
        g.W32(e + 88u, g.U32(rec + 68u));
        GBlend(g, e + 260u, e + 272u, e + 24u, S(0u - g.U32(e + 132u)), S(0u - g.U32(e + 136u))); // 0x8009BEF8
        for (uint32_t k = 0; k < 3; ++k) g.W32(e + 48u + 4u * k, 0u - g.U32(e + 24u + 4u * k));
        GBlend(g, e + 260u, e + 272u, e + 36u, g.S32(e + 132u), S(0u - g.U32(e + 136u)));        // 0x8009BF4C
        for (uint32_t k = 0; k < 3; ++k) g.W32(e + 60u + 4u * k, 0u - g.U32(e + 36u + 4u * k));
        for (uint32_t k = 0; k < 4; ++k)                                      // 0x8009BF94..0x8009BFD4
            for (uint32_t j = 0; j < 3; ++j)
                g.W32(e + 24u + 12u * k + 4u * j, g.U32(e + 24u + 12u * k + 4u * j) + g.U32(e + 12u + 4u * j));
    } else {                                                                  // 0x8009BFE4: a box
        g.W32(e + 132u, U(S(g.U32(rec + 76u) - g.U32(rec + 64u)) >> 1));
        g.W32(e + 140u, g.U32(rec + 80u) - g.U32(rec + 68u));
        g.W32(e + 136u, U(S(g.U32(rec + 84u) - g.U32(rec + 72u)) >> 1));
        if (g.U16(rec + 12u) & 2u) {                                          // 0x8009C03C
            g.W16(e + 272u, g.U16(rec + 14u));
            g.W16(e + 274u, g.U16(rec + 16u));
            g.W16(e + 276u, g.U16(rec + 18u));
            if (g.U16(rec + 12u) & 1u) {
                rc::GteOp(g, e + 266u, e + 272u, e + 260u);                   // 0x8009C074..0x8009C0CC
                int32_t sum = 0;
                if (!rc::GNormalize(g, e + 260u, t, sum)) return false;       // 0x8009C0D0
            } else {
                g.W16(e + 260u, g.U16(rec + 18u));                            // 0x8009C0E0
                g.W16(e + 264u, static_cast<uint16_t>(0u - g.U16(rec + 14u)));
            }
        }
        GMulAdd(g, e + 12u, e + 266u, 0x8000, e + 12u);                       // 0x8009C108: up by half a unit
        const int32_t hx = g.S32(e + 132u), hz = g.S32(e + 136u);
        GBlend(g, e + 260u, e + 272u, e + 24u, S(0u - U(hx)), S(0u - U(hz))); // 0x8009C144
        GBlend(g, e + 260u, e + 272u, e + 36u, g.S32(e + 132u), S(0u - g.U32(e + 136u)));
        GBlend(g, e + 260u, e + 272u, e + 48u, g.S32(e + 132u), g.S32(e + 136u));
        GBlend(g, e + 260u, e + 272u, e + 60u, S(0u - g.U32(e + 132u)), g.S32(e + 136u));
        for (uint32_t k = 0; k < 4; ++k) {                                    // 0x8009C1AC..0x8009C208: base, then top
            for (uint32_t j = 0; j < 3; ++j)
                g.W32(e + 24u + 12u * k + 4u * j, g.U32(e + 24u + 12u * k + 4u * j) + g.U32(e + 12u + 4u * j));
            GMulAdd(g, e + 24u + 12u * k, e + 266u, S(0xFFFF8000u - g.U32(e + 140u)), e + 72u + 12u * k);
        }
    }
    Heading(g, e + 272u, e + 276u, e + 120u);                                 // 0x8009C210..0x8009C270
    int32_t m = S(U(FixMul(g.S32(e + 140u), FixMul(g.S32(e + 136u), g.S32(e + 132u)))) << 2); // 0x8009C26C..0x8009C280
    g.W32(e + 144u, U(m));
    if (0x3F333 < m) m = 0x3F333;
    g.W32(e + 144u, U(m) << 9);
    g.W32(e + 4u, 0xFFFFFFFFu);
    const uint32_t live = RoadWindow(g, e, F);                                // 0x8009C2B4
    g.W16(e + 148u, static_cast<uint16_t>(live));
    if ((live & 0xFFFFu) == 0) {
        if (!PoolRelease(g, e, 6)) return false;                              // 0x8009C2D0
        return !g.Faulted();
    }
    v0 = e;
    return !g.Faulted();
}

void VolumeUnstamp(GuestRam& g, uint32_t vol, uint32_t p) {
    if (vol == 0) return;
    const uint32_t slot = CellSlotFor(g, g.U32(vol + 4u), p);                 // 0x8009F3E8
    if (slot == 0 || g.U32(slot + 8u) != g.U32(vol + 4u)) return;
    const uint32_t r0 = g.U32(g.U32(slot + 4u) + 32u);
    g.W16(g.U32(r0 + 48u) + 88u * g.U8(vol + 2u) + 4u, 0);                    // 0x8009F410..0x8009F438
}

void ListsClear(GuestRam& g, uint32_t p) {
    const uint32_t b = kWpVolumeLists + 32u * p;
    uint32_t a = g.U32(b + 4u);
    for (int32_t i = 0; i < g.S32(b); ++i, a += 8u) {                         // 0x8008DCCC..0x8008DCE4
        g.W16(a + 2u, 0xFFFF);
        g.W32(a + 4u, 0xFFFFFFFFu);
    }
    for (uint32_t k = 0; k < 3; ++k) {
        g.W32(b + 8u + 4u * k, 0);
        g.W32(b + 20u + 4u * k, 0xFFFFFFFFu);
    }
}

void ListAppend(GuestRam& g, uint32_t p, uint32_t band, uint32_t idx, uint32_t dist) {
    const uint32_t b = kWpVolumeLists + 32u * p;
    const uint32_t t0 = b + 4u * band;
    int32_t a1 = g.S32(t0 + 20u);
    g.W32(t0 + 8u, g.U32(t0 + 8u) + 1u);                                      // 0x8008DE34..0x8008DE44
    if (a1 == -1) {
        g.W32(t0 + 20u, idx);
    } else {
        const uint32_t list = g.U32(b + 4u);
        int32_t n = g.S16(list + 8u * U(a1) + 2u);
        while (n != -1) {                                                     // 0x8008DE6C..0x8008DE80
            a1 = n;
            n = g.S16(list + 8u * U(a1) + 2u);
        }
        g.W16(g.U32(b + 4u) + 8u * U(a1) + 2u, static_cast<uint16_t>(idx));
    }
    g.W32(g.U32(b + 4u) + 8u * idx + 4u, dist);                               // 0x8008DEC8
}

void VolumeFile(GuestRam& g, uint32_t p, uint32_t vol, uint32_t idx) {
    const uint32_t v = View(p);
    const int32_t d = Octagon(S(U(static_cast<int32_t>(g.S16(v + 186u))) - U(static_cast<int32_t>(g.S16(vol + 14u)))),
                              S(U(static_cast<int32_t>(g.S16(v + 194u))) - U(static_cast<int32_t>(g.S16(vol + 22u)))));
    const int32_t r = g.S32(kWpVolumeRadius);
    uint32_t band = 0;
    if (!(d < (r >> 18))) band = d < (r >> 16) ? 1u : 2u;                     // 0x8008DDD8..0x8008DDF0
    ListAppend(g, p, band, idx, U(d));                                        // 0x8008DE00
}

uint32_t VolumeTake(GuestRam& g, uint32_t p, uint32_t band) {
    const uint32_t b = kWpVolumeLists + 32u * p;
    const uint32_t a2 = b + 4u * band;
    uint32_t v1 = 224;
    if (g.S32(a2 + 8u) <= 0) return v1;                                       // 0x8008DEEC
    const int32_t a1 = g.S32(a2 + 20u);
    if (a1 != -1) {                                                           // 0x8008DEFC
        const uint32_t e = g.U32(b + 4u) + 8u * U(a1);
        g.W32(a2 + 20u, U(static_cast<int32_t>(g.S16(e + 2u))));
        g.W16(g.U32(b + 4u) + 8u * U(a1) + 2u, 0xFFFF);
        g.W32(g.U32(b + 4u) + 8u * U(a1) + 4u, 0);
        v1 = g.U16(g.U32(kWpPool6Ptr) + 280u * U(a1));
    }
    g.W32(a2 + 8u, g.U32(a2 + 8u) - 1u);                                      // 0x8008DF68
    return v1;
}

void VolumeLists(GuestRam& g) {
    g.W32(kWpPool6Ctrl + 16u, 0);                                             // 0x8008D8C4..0x8008D8CC
    g.W32(kWpPool6Ctrl + 20u, 0);
    g.W32(kWpPool6Ctrl + 24u, 0);
    for (uint32_t p = 0; S(p) < S(Players(g)); ++p) ListsClear(g, p);         // 0x8008D8E4
    const uint32_t rec = kWpPoolTable + 96u;                                  // pool 6
    int32_t s1 = g.S32(g.U32(rec + 12u));
    uint32_t s0 = g.U32(rec);
    for (; s1 >= 0; --s1, s0 += g.U32(rec + 4u)) {                            // 0x8008D930..0x8008D9C4
        if (g.U16(s0) == 0) continue;
        if (g.U8(s0 + 3u) & 1u) {
            g.W32(kWpPool6Ctrl + 16u, g.U32(kWpPool6Ctrl + 16u) + 1u);
            VolumeFile(g, 0, s0, U(s1));
        }
        if (g.U8(s0 + 3u) & 2u) {
            g.W32(kWpPool6Ctrl + 20u, g.U32(kWpPool6Ctrl + 20u) + 1u);
            VolumeFile(g, 1, s0, U(s1));
        }
        if (g.U8(s0 + 3u) == 3u) g.W32(kWpPool6Ctrl + 24u, g.U32(kWpPool6Ctrl + 24u) + 1u);
        if (g.Faulted()) return;
    }
}

bool VolumeEvict(GuestRam& g, uint32_t rec, uint32_t p) {
    const uint32_t v = View(p);
    const int32_t d = Octagon(S(U(static_cast<int32_t>(g.S16(v + 186u))) - U(static_cast<int32_t>(g.S16(rec + 22u)))),
                              S(U(static_cast<int32_t>(g.S16(v + 194u))) - U(static_cast<int32_t>(g.S16(rec + 30u)))));
    const int32_t r = g.S32(kWpVolumeRadius);
    int32_t band = -1;
    if (d < (r >> 18)) band = g.S32(kWpVolumeLists + 32u * p + 16u) > 0 ? 2 : 1; // 0x8008DAE0..0x8008DB10
    else if (d < (r >> 16)) band = 2;
    uint32_t h = 224;
    if (band != -1) h = VolumeTake(g, p, U(band));                            // 0x8008DB30
    if (!(((h - 1u) & 0xFFFFu) < 223u)) return !g.Faulted();                  // 0x8008DB3C..0x8008DB48
    const uint32_t vol = g.U32(kWpPool6Ptr) + 280u * (h & 31u);
    if (vol == 0) return !g.Faulted();
    VolumeUnstamp(g, vol, p);                                                 // 0x8008DB78
    if (!PoolRelease(g, vol, 6)) return false;                                // 0x8008DB84
    VolumeLists(g);                                                           // 0x8008DB8C
    return !g.Faulted();
}

bool VolumePass(GuestRam& g, uint32_t sp) {
    const uint32_t F = sp - 48u;
    const uint32_t rec = kWpPoolTable + 96u;
    int32_t n = g.S32(g.U32(rec + 12u));
    uint32_t e = g.U32(rec);
    for (; n >= 0; --n, e += g.U32(rec + 4u)) {                               // 0x8009AB60..
        if (g.U16(e) == 0) continue;
        const uint32_t live = RoadWindow(g, e, F);
        g.W16(e + 148u, static_cast<uint16_t>(live));
        if ((live & 0xFFFFu) == 0) {
            for (uint32_t p = 0; S(p) < S(Players(g)); ++p) VolumeUnstamp(g, e, p);
            if (!PoolRelease(g, e, 6)) return false;
        } else {
            for (uint32_t p = 0; S(p) < S(Players(g)); ++p) VolumeMask(g, 6, g.U16(e), p);
        }
        if (g.Faulted()) return false;
    }
    VolumeLists(g);
    return !g.Faulted();
}

} // namespace rr::sim
