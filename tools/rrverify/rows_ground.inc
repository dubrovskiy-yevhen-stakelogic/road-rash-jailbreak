// Bench rows for the ground query. Included by verify_physics.cpp
// inside its anonymous namespace, so everything defined above that point is visible here.
//
// The port is src\game\sim\ground.{h,cpp}: the two geometry leaves over host arrays, the lookup and
// the query over road_query.h's guest-address view (they walk the resident-cell slot table at
// 0x800D87E8 and the cell bodies behind it), and the view distance 0x8008DBCC.
//
// INPUT FAMILIES. The geometry is THIS machine's: rr-race holds ten resident cells in slots 0..9
// (scene_cell.md 1.1), and every lookup/query case runs on them, with the query's own inputs drawn
// around them:
//   * the reference point: a point inside a random primitive of a random sub-area's coarse or fine
//     group (so the walk lists can hit it), a region-3 sub-area polygon's centroid, one of its
//     vertices EXACTLY (the strict "on an edge is outside" rule), a captured bike's position, a
//     random point in a resident cell's box, or 500 units off the map;
//   * the hint: 0, a random word, or - half the time - one whose low twelve bits are the state
//     the lookup will reach (computed with the PORTED lookup on the case's own memory, which only
//     shapes the input; both sides then run from it), with a real region-4 record offset of that
//     key or a random one below 512, a random list and a random byte 0..127;
//   * entity +0x2C / +0x30 on both sides of 1280, the number of players 1 or 2 (game_state +0x30),
//     a captured or random heading at +0x1C2, `ref` = 0 (then e+0xB8) a sixth of the time, and the
//     normal buffer either a scratch buffer or the entity's own +0x10C, as the ground frame passes;
//   * the slot table itself permuted half the time (live cells moved into player 2's half 12..23,
//     empty slots moved in front), and, one case in eight each, a live cell's region 7 or region 6
//     pointer cleared or its sub-area count B zeroed - the three shapes a trace of the game does not
//     reach - and, one query case in five, the region-4 record count of the cell the
//     point lands in is lowered (to 0 at times), so the query's own search can exhaust.
// Named bounds, each a range the original's own callers stay in:
//   * the hint's record offset, when the hint's low bits match, is a real record start or below 512
//     and its byte index at most 127 - the original only ever sees hints it produced, and a wild
//     offset reads list bytes as primitive indices;
//   * PointInPoly's axis is -1..4 (its index arithmetic reads p[axis+1] otherwise) and n <= 16;
//   * region 3 is the shipped one, so every polygon has 4 or 6 vertices (the port declines odd or
//     larger counts rather than invent the stack the original would read).

namespace gq {
constexpr uint32_t kPip = 0x800B6E08, kNormal = 0x800B6844, kLookup = 0x800A8498,
                   kQuery = 0x800A7BF8, kView = 0x8008DBCC;
// Scratch-block layout (8 KiB).
constexpr uint32_t kPoint = 0x000;   // 12 bytes
constexpr uint32_t kHeading = 0x010; // 6
constexpr uint32_t kPrimOut = 0x018, kVertsOut = 0x01C, kOriginOut = 0x020, kStateOut = 0x030;
constexpr uint32_t kOutPt = 0x040;   // 12
constexpr uint32_t kOutN = 0x050;    // 6
constexpr uint32_t kPoly = 0x100;    // up to 16 vertices x 12
constexpr uint32_t kEntity = 0x400;  // 1096
constexpr uint32_t kSlotTableBytes = rr::sim::kCellSlotBytes * rr::sim::kCellSlotCount;
} // namespace gq

std::vector<uint16_t> GqRsqrt(Memory& m, uint32_t gp) {
    std::vector<uint16_t> t(2048);
    m.ReadBlock(m.PeekWord(gp + kRsqrtTableGpOffset), t.data(), t.size() * sizeof(uint16_t));
    return t;
}

uint16_t GqU16(Memory& m, uint32_t a) { return static_cast<uint16_t>(GetS16(m, a)); }

// The live slots of the machine's own cell table, as it stands in `m`.
std::vector<uint32_t> GqLiveSlots(Memory& m) {
    std::vector<uint32_t> out;
    for (uint32_t s = 0; s < rr::sim::kCellSlotCount; ++s)
        if (m.PeekWord(rr::sim::kCellSlotTable + rr::sim::kCellSlotBytes * s) != 0xFFFFFFFFu) out.push_back(s);
    return out;
}

uint32_t GqBody(Memory& m, uint32_t slot) {
    return m.PeekWord(rr::sim::kCellSlotTable + rr::sim::kCellSlotBytes * slot + 4u);
}

void GqOrigin(Memory& m, uint32_t body, int32_t o[3]) {
    for (uint32_t c = 0; c < 3; ++c) o[c] = static_cast<int32_t>(m.PeekWord(body + 8u + 4u * c) << 10);
}

void GqVertex(Memory& m, uint32_t body, uint32_t index, int32_t v[3]) {
    const uint32_t va = m.PeekWord(body + 0x34u) + 4u + 8u * index;
    for (uint32_t c = 0; c < 3; ++c)
        v[c] = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(GetS16(m, va + 2u * c))) << 10);
}

// The region-3 polygon of sub-area `sub`, in the lookup's stored (reversed) order, cell-local.
int GqPolygon(Memory& m, uint32_t body, uint32_t sub, int32_t poly[16][3]) {
    const uint32_t r3 = m.PeekWord(body + 0x2Cu);
    const int32_t start = GetS16(m, r3 + 2u * (sub + 1u));
    const int32_t cnt = GetS16(m, r3 + 2u * (sub + 2u)) - start;
    if (cnt <= 0 || cnt > 16) return 0;
    for (int32_t j = 0; j < cnt; ++j) {
        const int32_t vi = GetS16(m, r3 + 2u * static_cast<uint32_t>(start + j));
        GqVertex(m, body, static_cast<uint32_t>(vi), poly[cnt - 1 - j]);
    }
    return cnt;
}

// A point (world 16.16) the query can hit: inside a primitive of the sub-area's coarse or fine
// group. Returns false when the chosen cell has nothing to offer.
bool GqPrimitivePoint(CaseContext& c, uint32_t body, int32_t out[3]) {
    Memory& m = *c.mem;
    const uint32_t A = GqU16(m, body + 4u), B = GqU16(m, body + 6u);
    if (B == 0) return false;
    const uint32_t sub = c.rng->U32() % B;
    const bool fine = (c.rng->Next() & 1u) && m.PeekWord(body + 0x3Cu) != 0;
    const uint32_t g = fine ? A + B + sub : A + sub;
    const uint32_t grp = m.PeekWord(body + 0x28u) + 12u * g;
    const uint32_t base = fine ? m.PeekWord(body + 0x3Cu) : m.PeekWord(body + 0x38u);
    const uint32_t tri = m.PeekWord(grp + 4u), quad = m.PeekWord(grp + 8u);
    if (base == 0 || tri + quad == 0 || tri + quad > 4096) return false;
    const uint32_t k = c.rng->U32() % (tri + quad);
    const uint32_t prim = base + m.PeekWord(grp) + (k < tri ? 20u * k : 20u * tri + 24u * (k - tri));
    const uint32_t nv = k < tri ? 3u : 4u;
    const uint32_t ix = prim + (k < tri ? 14u : 16u);
    int64_t sum[3] = {0, 0, 0};
    for (uint32_t v = 0; v < nv; ++v) {
        int32_t p[3];
        GqVertex(m, body, GqU16(m, ix + 2u * v), p);
        for (int q = 0; q < 3; ++q) sum[q] += p[q];
    }
    int32_t o[3];
    GqOrigin(m, body, o);
    for (int q = 0; q < 3; ++q)
        out[q] = static_cast<int32_t>(static_cast<uint32_t>(o[q]) +
                                      static_cast<uint32_t>(static_cast<int32_t>(sum[q] / nv)));
    if (c.rng->Next() % 3u == 0)
        for (int q = 0; q < 3; ++q) out[q] += static_cast<int32_t>(c.rng->U32() % 0x40000u) - 0x20000;
    return true;
}

// Any point of the family (see the header).
void GqPoint(CaseContext& c, int32_t out[3]) {
    Memory& m = *c.mem;
    const std::vector<uint32_t> live = GqLiveSlots(m);
    const uint64_t roll = c.rng->Next() % 12u;
    if (live.empty() || roll >= 10) {                     // a captured bike, or 500 units off the map
        const Sample* s = c.sample ? c.sample : c.any;
        for (int q = 0; q < 3; ++q) out[q] = s->p[q] + ((roll == 11) ? (500 << 16) : 0);
        return;
    }
    const uint32_t body = GqBody(m, live[c.rng->U32() % live.size()]);
    if (roll < 6 && GqPrimitivePoint(c, body, out)) return;
    int32_t o[3];
    GqOrigin(m, body, o);
    const uint32_t B = GqU16(m, body + 6u);
    if (roll < 9 && B != 0) {
        int32_t poly[16][3];
        const int n = GqPolygon(m, body, c.rng->U32() % B, poly);
        if (n > 0) {
            if (roll == 8) {                                 // exactly on a vertex
                const int k = static_cast<int>(c.rng->U32() % static_cast<uint32_t>(n));
                for (int q = 0; q < 3; ++q) out[q] = o[q] + poly[k][q];
                return;
            }
            int64_t sum[3] = {0, 0, 0};
            for (int k = 0; k < n; ++k)
                for (int q = 0; q < 3; ++q) sum[q] += poly[k][q];
            for (int q = 0; q < 3; ++q) out[q] = o[q] + static_cast<int32_t>(sum[q] / n);
            return;
        }
    }
    // a random point within 60 units of the cell origin
    for (int q = 0; q < 3; ++q) out[q] = o[q] + static_cast<int32_t>(c.rng->U32() % (120u << 16)) - (60 << 16);
}

// The slot table, permuted half the time; one case in eight each clears a live cell's region 7,
// region 6 or its B. Every write lands in both machines (the setup runs on the guest's memory and
// the clone starts from it).
void GqStirSlots(CaseContext& c) {
    Memory& m = *c.mem;
    if (c.rng->Next() & 1u) {
        std::vector<uint8_t> table(gq::kSlotTableBytes);
        m.ReadBlock(rr::sim::kCellSlotTable, table.data(), table.size());
        uint32_t order[rr::sim::kCellSlotCount];
        for (uint32_t i = 0; i < rr::sim::kCellSlotCount; ++i) order[i] = i;
        for (uint32_t i = rr::sim::kCellSlotCount - 1; i > 0; --i) std::swap(order[i], order[c.rng->U32() % (i + 1)]);
        std::vector<uint8_t> out(table.size());
        for (uint32_t i = 0; i < rr::sim::kCellSlotCount; ++i)
            std::memcpy(out.data() + rr::sim::kCellSlotBytes * i, table.data() + rr::sim::kCellSlotBytes * order[i],
                        rr::sim::kCellSlotBytes);
        m.WriteBlock(rr::sim::kCellSlotTable, out.data(), out.size());
    }
    const std::vector<uint32_t> live = GqLiveSlots(m);
    if (live.empty()) return;
    const uint64_t roll = c.rng->Next() % 8u;
    const uint32_t body = GqBody(m, live[c.rng->U32() % live.size()]);
    if (roll == 0) m.PokeWord(body + 0x3Cu, 0);
    if (roll == 1) m.PokeWord(body + 0x38u, 0);
    if (roll == 2) WriteU16Bench(m, body + 6u, 0);

}

// The hint: a key-consistent one half the time (see the header), otherwise 0 / random / -1.
uint32_t GqHint(CaseContext& c, const int32_t point[3], uint32_t heading, uint32_t stateIn) {
    Memory& m = *c.mem;
    const uint64_t roll = c.rng->Next() % 8u;
    if (roll == 0) return 0;
    if (roll == 1) return 0xFFFFFFFFu;
    if (roll == 2) return c.rng->U32() & 0x00FFFFFFu; // a random word with a byte index of 0
    rr::sim::GuestRam g(m.ram().data(), c.gp);
    const rr::sim::CellLookupResult look = rr::sim::CellLookup(g, point, heading, stateIn);
    const uint32_t low = (look.counts != 0 && !look.declined) ? (look.state & 0xFFFu) : (c.rng->U32() & 0xFFFu);
    uint32_t off = c.rng->U32() % 512u;
    if (look.counts != 0 && !look.declined && (c.rng->Next() % 4u) != 0) {
        // the record whose key the lookup reached, as the query's own search would find it
        const uint32_t body = GqBody(m, look.state & 0x3Fu);
        const uint32_t r4 = m.PeekWord(body + 0x30u);
        const uint32_t fine = (look.state >> 11) & 1u;
        const uint32_t n = m.PeekByte(r4 + fine);
        uint32_t o = 2;
        for (uint32_t k = 0; k < n && o < 512u; ++k) {
            const uint8_t tag = m.PeekByte(r4 + o);
            if (static_cast<uint32_t>(tag >> 7) == fine && ((tag >> 4) & 7u) == ((look.state >> 6) & 7u) &&
                ((tag >> 2) & 3u) == ((look.state >> 9) & 3u)) {
                off = o;
                break;
            }
            o += 2u + m.PeekByte(r4 + o + 1u);
        }
        if (c.rng->Next() % 4u == 0) off = 0x3FF;         // "no cached record"
    }
    const uint32_t list = c.rng->U32() & 3u;
    const uint32_t pos = (c.rng->Next() % 4u == 0) ? 127u : (c.rng->U32() % 128u);
    return (pos << 24) | (off << 14) | (list << 12) | low | (c.rng->U32() & 0x80000000u);
}

void GqHeading(CaseContext& c, uint32_t at) {
    const Sample* s = c.sample;
    int16_t h[3];
    if (s && s->entity.size() >= 0x1C8 && (c.rng->Next() & 1u)) {
        for (int q = 0; q < 3; ++q)
            h[q] = static_cast<int16_t>(s->entity[0x1C2 + 2 * q] | (s->entity[0x1C3 + 2 * q] << 8));
    } else {
        for (int q = 0; q < 3; ++q) h[q] = c.rng->Unit();
    }
    PutVec3S16(*c.mem, at, h);
}

void AddGroundRows(std::vector<Row>& rows) {
    // ------------------------------------------------------------------ F2, RASHCDG 0x800B6E08
    rows.push_back(Row{
        "point_in_poly", "s32 PointInPoly(const s32 p[3], const s32 v[][3], s32 n, s32 axis)  [RASHCDG]",
        gq::kPip,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            int32_t poly[16][3] = {};
            int n = 0;
            int32_t p[3];
            const std::vector<uint32_t> live = GqLiveSlots(m);
            if (c.sample && !live.empty()) {
                // a real sub-area polygon, cell-local, and a point near it
                const uint32_t body = GqBody(m, live[c.rng->U32() % live.size()]);
                const uint32_t B = GqU16(m, body + 6u);
                if (B != 0) n = GqPolygon(m, body, c.rng->U32() % B, poly);
            }
            if (n == 0) {
                // a synthetic convex polygon: n points on an ellipse, in either winding
                n = 3 + static_cast<int>(c.rng->Next() % 6u);
                const double rx = static_cast<double>(1 + c.rng->U32() % 4000u) * 65536.0 / 64.0;
                const double rz = static_cast<double>(1 + c.rng->U32() % 4000u) * 65536.0 / 64.0;
                const int32_t cx = c.rng->World(), cz = c.rng->World(), cy = c.rng->World();
                const bool cw = (c.rng->Next() & 1u) != 0;
                for (int k = 0; k < n; ++k) {
                    const double a = (cw ? -1.0 : 1.0) * 6.283185307179586 * k / n;
                    poly[k][0] = cx + static_cast<int32_t>(rx * std::cos(a));
                    poly[k][1] = cy + static_cast<int32_t>(c.rng->U32() % 65536u);
                    poly[k][2] = cz + static_cast<int32_t>(rz * std::sin(a));
                }
            }
            const uint64_t roll = c.rng->Next() % 6u;
            const int k = static_cast<int>(c.rng->U32() % static_cast<uint32_t>(n));
            const int k2 = (k + 1) % n;
            for (int q = 0; q < 3; ++q) {
                int64_t sum = 0;
                for (int i = 0; i < n; ++i) sum += poly[i][q];
                const int32_t centroid = static_cast<int32_t>(sum / n);
                switch (roll) {
                    case 0: p[q] = poly[k][q]; break;                                        // a vertex
                    case 1: p[q] = static_cast<int32_t>((static_cast<int64_t>(poly[k][q]) + poly[k2][q]) / 2); break;
                    case 2: p[q] = centroid; break;
                    case 3: p[q] = centroid + static_cast<int32_t>(c.rng->U32() % 0x200000u) - 0x100000; break;
                    case 4: p[q] = c.rng->S32(); break;
                    default: p[q] = poly[k][q] + static_cast<int32_t>(c.rng->U32() % 64u) - 32; break;
                }
            }
            if (c.rng->Next() % 16u == 0) n = static_cast<int>(c.rng->Next() % 3u) - 1; // -1, 0, 1
            for (int i = 0; i < 16; ++i) PutVec3S32(m, c.scratch + gq::kPoly + 12u * static_cast<uint32_t>(i), poly[i]);
            PutVec3S32(m, c.scratch + gq::kPoint, p);
            args.a[0] = c.scratch + gq::kPoint;
            args.a[1] = c.scratch + gq::kPoly;
            args.a[2] = static_cast<uint32_t>(n);
            // axis 1 is the only one the game passes; the others are the function's own arithmetic
            args.a[3] = (c.rng->Next() % 3u == 0) ? static_cast<uint32_t>(static_cast<int32_t>(c.rng->Next() % 6u) - 1) : 1u;
        },
        [](Memory& m, uint32_t, uint32_t, const Args& args) {
            int32_t p[3], poly[16][3] = {};
            GetVec3S32(m, args.a[0], p);
            const int32_t n = static_cast<int32_t>(args.a[2]);
            for (int32_t i = 0; i < n && i < 16; ++i) GetVec3S32(m, args.a[1] + 12u * static_cast<uint32_t>(i), poly[i]);
            return static_cast<uint32_t>(rr::sim::PointInPoly(p, poly, n, static_cast<int32_t>(args.a[3])));
        }});

    // ------------------------------------------------------------------ F3, RASHCDG 0x800B6844
    rows.push_back(Row{
        "tri_normal", "void TriNormal(const s32 v[][3], s32 n, s16 out[3])  [RASHCDG, Newell + Normalize32]",
        gq::kNormal,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            int32_t poly[8][3] = {};
            int n = 3;
            const std::vector<uint32_t> live = GqLiveSlots(m);
            bool real = false;
            if (c.sample && !live.empty()) {
                // a real primitive's corners, cell-local, as the query hands them over
                const uint32_t body = GqBody(m, live[c.rng->U32() % live.size()]);
                const uint32_t A = GqU16(m, body + 4u), B = GqU16(m, body + 6u);
                const uint32_t g = A + (B ? c.rng->U32() % B : 0u);
                const uint32_t grp = m.PeekWord(body + 0x28u) + 12u * g;
                const uint32_t base = m.PeekWord(body + 0x38u);
                const uint32_t tri = m.PeekWord(grp + 4u), quad = m.PeekWord(grp + 8u);
                if (base != 0 && tri + quad != 0 && tri + quad < 4096) {
                    const uint32_t k = c.rng->U32() % (tri + quad);
                    const uint32_t prim = base + m.PeekWord(grp) + (k < tri ? 20u * k : 20u * tri + 24u * (k - tri));
                    const uint32_t ix = prim + (k < tri ? 14u : 16u);
                    for (uint32_t v = 0; v < 3; ++v) GqVertex(m, body, GqU16(m, ix + 2u * v), poly[v]);
                    real = true;
                }
            }
            if (!real) {
                const uint64_t kind = c.rng->Next() % 4u;
                for (int i = 0; i < 8; ++i)
                    for (int q = 0; q < 3; ++q)
                        poly[i][q] = (kind == 0) ? c.rng->S32() : (kind == 1) ? 0 : c.rng->World();
                if (kind == 3) // collinear
                    for (int q = 0; q < 3; ++q) poly[2][q] = poly[0][q] + 2 * (poly[1][q] - poly[0][q]);
                if (c.rng->Next() % 4u == 0) n = static_cast<int>(c.rng->Next() % 9u) - 1; // -1..7
            }
            for (int i = 0; i < 8; ++i) PutVec3S32(m, c.scratch + gq::kPoly + 12u * static_cast<uint32_t>(i), poly[i]);
            const int16_t fill[3] = {c.rng->S16(), c.rng->S16(), c.rng->S16()};
            PutVec3S16(m, c.scratch + gq::kOutN, fill);
            args.a[0] = c.scratch + gq::kPoly;
            args.a[1] = static_cast<uint32_t>(n);
            args.a[2] = c.scratch + gq::kOutN;
        },
        [](Memory& m, uint32_t, uint32_t gp, const Args& args) {
            const std::vector<uint16_t> rsqrt = GqRsqrt(m, gp);
            int32_t poly[8][3] = {};
            const int32_t n = static_cast<int32_t>(args.a[1]);
            for (int32_t i = 0; i < std::max(n, 1) && i < 8; ++i)
                GetVec3S32(m, args.a[0] + 12u * static_cast<uint32_t>(i), poly[i]);
            int16_t out[3];
            rr::sim::TriNormal(poly, n, out, rsqrt.data());
            PutVec3S16(m, args.a[2], out);
            return 0u;
        },
        /*compareV0=*/false});

    // ------------------------------------------------------------------ F4, RASHCDG 0x800A8498
    rows.push_back(Row{
        "cell_lookup",
        "u32 CellLookup(const s32 p[3], const s16 *hd, u8 **prim, u8 **verts, s32 org[3], u32 *state)  [RASHCDG]",
        gq::kLookup,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            GqStirSlots(c);
            int32_t p[3];
            GqPoint(c, p);
            PutVec3S32(m, c.scratch + gq::kPoint, p);
            GqHeading(c, c.scratch + gq::kHeading);
            // the four outputs start as noise, so "not written" is visible
            for (uint32_t k = gq::kPrimOut; k < gq::kStateOut; k += 4) m.PokeWord(c.scratch + k, c.rng->U32());
            uint32_t state = c.rng->U32();
            if (c.rng->Next() % 4u != 0) state &= 0x80003FFFu;
            m.PokeWord(c.scratch + gq::kStateOut, state);
            args.a[0] = c.scratch + gq::kPoint;
            args.a[1] = c.scratch + gq::kHeading;
            args.a[2] = c.scratch + gq::kPrimOut;
            args.a[3] = c.scratch + gq::kVertsOut;
            args.a4 = c.scratch + gq::kOriginOut;
            args.more[0] = c.scratch + gq::kStateOut;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& args) -> uint32_t {
            Memory& m = env.clone->mem;
            rr::sim::GuestRam g(m.ram().data(), env.gp);
            int32_t p[3];
            GetVec3S32(m, args.a[0], p);
            const rr::sim::CellLookupResult r = rr::sim::CellLookup(g, p, args.a[1], m.PeekWord(args.more[0]));
            if (r.declined) {
                env.failure = "the port declined (a polygon shape the shipped data does not have)";
                return 0u;
            }
            if (g.Faulted()) {
                env.failure = "the port met an address the console would fault on";
                return 0u;
            }
            if (r.slotVisited) {
                m.PokeWord(args.a[3], r.verts);
                PutVec3S32(m, args.a4, r.origin);
            }
            if (r.primWritten) m.PokeWord(args.a[2], r.prim);
            m.PokeWord(args.more[0], r.state);
            return r.counts;
        }});

    // ------------------------------------------------------------------ F5, RASHCDG 0x800A7BF8
    rows.push_back(Row{
        "ground_query",
        "u32 GroundQuery(Entity *e, s32 *ref, s32 pt[3], s16 n[3], u32 hint)  [RASHCDG, the terrain under a point]",
        gq::kQuery,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            GqStirSlots(c);
            const uint32_t e = c.scratch + gq::kEntity;
            const Sample* s = c.sample ? c.sample : c.any;
            std::vector<uint8_t> ent(1096, 0);
            if (s->entity.size() >= 1096) std::copy_n(s->entity.begin(), 1096, ent.begin());
            m.WriteBlock(e, ent.data(), ent.size());
            // the LOD distances, both sides of 1280, and one or two players
            auto dist = [&c]() -> uint32_t {
                switch (c.rng->Next() % 6u) {
                    case 0: return 1279u;
                    case 1: return 1280u;
                    case 2: return 0x7FFFFFFFu;
                    case 3: return c.rng->U32();
                    default: return c.rng->U32() % 2560u;
                }
            };
            m.PokeWord(e + 0x2Cu, dist());
            m.PokeWord(e + 0x30u, dist());
            const uint32_t gs = m.PeekWord(kGameStatePtr);
            m.PokeWord(gs + 0x30u, (c.rng->Next() % 3u == 0) ? 2u : 1u);
            GqHeading(c, e + 0x1C2u);
            {
                const uint8_t surface = static_cast<uint8_t>(c.rng->U32());
                m.WriteBlock(e + 0x216u, &surface, 1);
            }
            int32_t p[3];
            GqPoint(c, p);
            const bool viaEntity = c.rng->Next() % 6u == 0;
            PutVec3S32(m, viaEntity ? e + 0xB8u : c.scratch + gq::kPoint, p);
            // the output buffers start as noise, so "not written" is visible
            for (uint32_t k = 0; k < 12; k += 4) m.PokeWord(c.scratch + gq::kOutPt + k, c.rng->U32());
            const int16_t fill[3] = {c.rng->S16(), c.rng->S16(), c.rng->S16()};
            PutVec3S16(m, c.scratch + gq::kOutN, fill);
            const bool normalInEntity = (c.rng->Next() & 1u) != 0;
            // the state the query itself will start the lookup with, for a key-consistent hint
            uint32_t state0 = 0;
            {
                const int32_t d1 = static_cast<int32_t>(m.PeekWord(e + 0x2Cu));
                const int32_t d2 = static_cast<int32_t>(m.PeekWord(e + 0x30u));
                int32_t d = d1;
                if (!(m.PeekWord(gs + 0x30u) < 2u) && d2 < d1) { state0 |= 0x80000000u; d = d2; }
                if (d < rr::sim::kGroundFineDistance) state0 |= 0x800u;
            }
            const uint32_t hint0 = GqHint(c, p, e + 0x1C2u, state0);
            args.a4 = hint0;
            if (c.rng->Next() % 5u == 0) {
                // the region-4 record count of the cell the lookup will land in lowered (to 0 at
                // times), so the query's own search can run out
                rr::sim::GuestRam g(m.ram().data(), c.gp);
                const rr::sim::CellLookupResult look =
                    rr::sim::CellLookup(g, p, e + 0x1C2u, state0 | (hint0 & 0x7FFu));
                if (look.counts != 0 && !look.declined) {
                    const uint32_t r4 = m.PeekWord(GqBody(m, look.state & 0x3Fu) + 0x30u);
                    const uint32_t which = (look.state >> 11) & 1u;
                    const uint8_t n = m.PeekByte(r4 + which);
                    const uint8_t lower = static_cast<uint8_t>(n ? c.rng->U32() % n : 0u);
                    m.WriteBlock(r4 + which, &lower, 1);
                }
            }
            args.a[0] = e;
            args.a[1] = viaEntity ? 0u : c.scratch + gq::kPoint;
            args.a[2] = c.scratch + gq::kOutPt;
            args.a[3] = normalInEntity ? e + 0x10Cu : c.scratch + gq::kOutN;
        },
        nullptr, /*compareV0=*/true,
        [](NativeEnv& env, const Args& args) -> uint32_t {
            Memory& m = env.clone->mem;
            const std::vector<uint16_t> rsqrt = GqRsqrt(m, env.gp);
            rr::sim::GuestRam g(m.ram().data(), env.gp);
            const rr::sim::GroundResult r =
                rr::sim::GroundQuery(g, args.a[0], args.a[1], args.a[2], args.a[3], args.a4, rsqrt.data());
            if (r.declined) {
                env.failure = "the port declined (a polygon shape the shipped data does not have)";
                return 0u;
            }
            if (g.Faulted()) {
                env.failure = "the port met an address the console would fault on";
                return 0u;
            }
            return r.value;
        }});

    // ------------------------------------------------------------------ F8, RASHCDG 0x8008DBCC
    rows.push_back(Row{
        "view_distance", "void ViewDistance(Entity *e)  [RASHCDG, entity +0x2C/+0x30 from the view objects]",
        gq::kView,
        [](CaseContext& c, Args& args) {
            Memory& m = *c.mem;
            const uint32_t e = c.scratch + gq::kEntity;
            const Sample* s = c.sample ? c.sample : c.any;
            std::vector<uint8_t> ent(1096, 0);
            if (s->entity.size() >= 1096) std::copy_n(s->entity.begin(), 1096, ent.begin());
            if (!c.sample)
                for (uint32_t k = 0; k < 3; ++k) {
                    const int32_t v = (c.rng->Next() & 1u) ? c.rng->S32() : c.rng->World();
                    std::memcpy(ent.data() + 0xB8 + 4 * k, &v, 4);
                }
            m.WriteBlock(e, ent.data(), ent.size());
            // the two view objects: near the entity, anywhere, or exactly on it
            for (uint32_t p = 0; p < 2; ++p) {
                const uint32_t view = rr::sim::kViewObjects + rr::sim::kViewObjectBytes * p;
                const uint64_t roll = c.rng->Next() % 4u;
                for (uint32_t k = 0; k < 3; ++k) {
                    int32_t ev;
                    std::memcpy(&ev, ent.data() + 0xB8 + 4 * k, 4);
                    int32_t v = ev;
                    if (roll == 1) v = ev + static_cast<int32_t>(c.rng->U32() % (4000u << 16)) - (2000 << 16);
                    if (roll == 2) v = c.rng->S32();
                    if (roll == 3) v = ev + static_cast<int32_t>(c.rng->U32() % (60u << 16)) - (30 << 16);
                    m.PokeWord(view + 0xB8u + 4u * k, static_cast<uint32_t>(v));
                }
            }
            const uint32_t gs = m.PeekWord(kGameStatePtr);
            m.PokeWord(gs + 0x30u, static_cast<uint32_t>(c.rng->Next() % 3u)); // 0, 1 or 2 players
            m.PokeWord(e + 0x2Cu, c.rng->U32());
            m.PokeWord(e + 0x30u, c.rng->U32());
            args.a[0] = e;
        },
        [](Memory& m, uint32_t, uint32_t gp, const Args& args) {
            rr::sim::GuestRam g(m.ram().data(), gp);
            rr::sim::ViewDistance(g, args.a[0]);
            return 0u;
        }});
}
