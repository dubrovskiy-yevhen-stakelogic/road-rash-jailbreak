// Bench rows of the cell draw's four polygon subdividers (src\game\sim\subdiv.h, docs\formats\scene_cell.md 13.8).
// Included by verify_physics.cpp inside its anonymous namespace, after rows_fx.inc / rows_model.inc, whose helpers
// (fxrows::Around, spinerows::Chance, the model draw's ModelGte) it reuses.
//
// Inputs, each a state the cell draw builds: the corner records 0..3 (0..2) at 0x1F800100 with view-space
// points (cell units << 8) from behind the eye to 1600 units ahead, around the view axis and now and then far
// off it, their SXY the projection of that point (now and then anything), a texel word (lhu), a colour index
// (lbu) and an outcode byte as the callers compute it (sometimes with extra bits); the records the call will
// write are junk. The level: 5 for the road (its callers' constant), 5 - (attr >> 16 & 15) with the nibble 0 or 1
// for the band-1 quads (a deeper start leaves the scratchpad) as for the triangles. The packet heap is the
// snapshot's, its end *(0x8005B4D0) sometimes just ahead so the overflow arm SLUS 0x80021C98 runs (the oracle
// runs it, it is a callee). The child tables and the colour table are the snapshot's RASHCDG data; the GTE is
// the snapshot's own (OFX / OFY / H, and the RT x V0 term RTPS adds - 0 in the cell draw, where RT is cleared).
namespace subdivrows {

namespace s = rr::sim;
namespace D = rr::sim::subdiv;
namespace M = rr::sim::model;
using fxrows::Around;
using spinerows::Chance;

struct GuestEnv final : D::Env {
    NativeEnv& env;
    s::GuestRam& g;
    M::ModelGte gte;
    int16_t v0[3] = {};
    GuestEnv(NativeEnv& e, s::GuestRam& gr) : env(e), g(gr) {
        gte = M::ModelGte::From(e.clone->cpu.gte().cr, e.clone->cpu.gte().dr);
        const uint32_t* dr = e.clone->cpu.gte().dr;
        v0[0] = static_cast<int16_t>(dr[0] & 0xFFFFu);
        v0[1] = static_cast<int16_t>(dr[0] >> 16);
        v0[2] = static_cast<int16_t>(dr[1] & 0xFFFFu);
    }
    uint32_t Project(int32_t tx, int32_t ty, int32_t tz) override {
        gte.tr[0] = tx;
        gte.tr[1] = ty;
        gte.tr[2] = tz;
        int32_t mac[3];
        return gte.Rtps(v0, mac);
    }
    uint32_t Colour(uint32_t index) override { return g.U32(D::kColourTable + 4u * index); }
    bool Stored(int i, const D::Rec& r, bool uv) override {
        const uint32_t a = D::kRecords + 32u * static_cast<uint32_t>(i);
        g.W32(a, static_cast<uint32_t>(r.x));
        g.W32(a + 4u, static_cast<uint32_t>(r.y));
        g.W32(a + 8u, static_cast<uint32_t>(r.z));
        if (uv) g.W32(a + 16u, r.uv);
        g.W32(a + 20u, static_cast<uint32_t>(r.shade));
        g.W32(a + 24u, r.sxy);
        g.W32(a + 28u, r.rgb);
        return !g.Faulted();
    }
    // The heap test of every emitter: room when next + size < end (unsigned), else the overflow arm's answer.
    uint32_t Alloc(uint32_t size) {
        uint32_t p = g.U32(D::kHeapNext);
        if (!(p + size < g.U32(D::kHeapEnd))) {
            p = env.CallOracle(D::kHeapFullFn, p, size, 0, 0);
            g.W32(D::kHeapNext, p);
        }
        return g.U32(D::kHeapNext);
    }
    bool Close(uint32_t p, uint32_t size) {
        g.W32(D::kHeapPrev, p);
        g.W32(D::kHeapNext, p + size);
        return env.failure.empty() && !g.Faulted();
    }
    bool Gt4(const D::Rec* r, const int v[4]) override {
        const uint32_t p = Alloc(52);
        g.W32(p, g.U32(D::kHeapPrev) | 0x0C000000u);
        g.W32(p + 4u, (r[v[0]].rgb & 0xFFFFFFu) | 0x3C000000u);
        g.W32(p + 8u, r[v[0]].sxy);
        g.W32(p + 12u, r[v[0]].uv | g.U32(D::kUvHigh0));
        g.W32(p + 16u, r[v[1]].rgb & 0xFFFFFFu);
        g.W32(p + 20u, r[v[1]].sxy);
        g.W32(p + 24u, r[v[1]].uv | g.U32(D::kUvHigh1));
        g.W32(p + 28u, r[v[2]].rgb & 0xFFFFFFu);
        g.W32(p + 32u, r[v[2]].sxy);
        g.W32(p + 36u, r[v[2]].uv);
        g.W32(p + 40u, r[v[3]].rgb & 0xFFFFFFu);
        g.W32(p + 44u, r[v[3]].sxy);
        g.W32(p + 48u, r[v[3]].uv);
        return Close(p, 52);
    }
    bool Gt3(const D::Rec* r, const int v[3]) override {
        const uint32_t p = Alloc(40);
        g.W32(p, g.U32(D::kHeapPrev) | 0x09000000u);
        g.W32(p + 4u, (r[v[0]].rgb & 0xFFFFFFu) | 0x34000000u);
        g.W32(p + 8u, r[v[0]].sxy);
        g.W32(p + 12u, r[v[0]].uv | g.U32(D::kUvHigh0));
        g.W32(p + 16u, r[v[1]].rgb & 0xFFFFFFu);
        g.W32(p + 20u, r[v[1]].sxy);
        g.W32(p + 24u, r[v[1]].uv | g.U32(D::kUvHigh1));
        g.W32(p + 28u, r[v[2]].rgb & 0xFFFFFFu);
        g.W32(p + 32u, r[v[2]].sxy);
        g.W32(p + 36u, r[v[2]].uv);
        return Close(p, 40);
    }
    bool F4(const D::Rec* r, const int v[4], uint32_t colour) override {
        const uint32_t p = Alloc(24);
        g.W32(p, g.U32(D::kHeapPrev) | 0x05000000u);
        g.W32(p + 4u, colour | 0x28000000u);
        for (uint32_t k = 0; k < 4; ++k) g.W32(p + 8u + 4u * k, r[v[k]].sxy);
        return Close(p, 24);
    }
};

enum class Fn { Tri, Quad, Road, Line };

uint32_t Run(NativeEnv& env, Fn fn, const Args& a) {
    s::GuestRam g = RqView(env);
    g.SetScratchpad(env.clone->mem.scratchpad().data());
    GuestEnv ge(env, g);
    D::Kids kids;
    for (uint32_t k = 0; k < 4; ++k) kids.tri[k] = g.U32(D::kTriKids + 4u * k);
    for (uint32_t k = 0; k < 4; ++k) kids.quad[k] = g.U32(D::kQuadKids + 4u * k);
    for (uint32_t k = 0; k < 2; ++k) kids.line[k] = g.U32(D::kLineKids + 4u * k);
    D::Subdivider sd(ge, kids);
    for (int i = 0; i < D::kMaxRecords; ++i) {
        const uint32_t r = D::kRecords + 32u * static_cast<uint32_t>(i);
        sd.rec[i].x = g.S32(r);
        sd.rec[i].y = g.S32(r + 4u);
        sd.rec[i].z = g.S32(r + 8u);
        sd.rec[i].uv = g.U32(r + 16u);
        sd.rec[i].shade = g.S32(r + 20u);
        sd.rec[i].sxy = g.U32(r + 24u);
        sd.rec[i].rgb = g.U32(r + 28u);
    }
    const int base = static_cast<int>(a.a[0]);
    const int32_t level = static_cast<int32_t>(a.a[2]);
    bool ok = false;
    switch (fn) {
    case Fn::Tri: ok = sd.Tri(base, a.a[1], level); break;
    case Fn::Quad: ok = sd.Quad(base, a.a[1], level); break;
    case Fn::Road: ok = sd.Road(base, a.a[1], level); break;
    case Fn::Line: ok = sd.Line(base, a.a[1], a.a[2]); break;
    }
    if (!ok && env.failure.empty()) env.failure = "the subdivider left the scratchpad's records (the console faults there)";
    return RqDone(env, g, 0);
}

void PutWord(Memory& m, uint32_t a, uint32_t v) { m.PokeWord(a, v); }

// One corner record: a view-space point, its projection (the snapshot's OFX / OFY / H, a plain divide), a texel,
// a colour index and the outcode the callers compute for it.
void PutCorner(CaseContext& c, uint32_t i, int32_t zCell, bool road) {
    Memory& m = *c.mem;
    const uint32_t r = D::kRecords + 32u * i;
    const int32_t spread = std::max<int32_t>(40, std::abs(zCell) + 40);
    int32_t xCell = Around(c, Chance(c, 4) ? 3 * spread : spread);
    int32_t yCell = Around(c, Chance(c, 4) ? 2 * spread : spread / 2);
    const int32_t x = xCell * 256 + Around(c, 255), y = yCell * 256 + Around(c, 255), z = zCell * 256 + Around(c, 255);
    int32_t sx = 192, sy = 120;
    if (z > 256) {
        sx = 192 + static_cast<int32_t>(static_cast<int64_t>(x) * 237 / z);
        sy = 120 + static_cast<int32_t>(static_cast<int64_t>(y) * 237 / z);
    } else {
        sx = x < 0 ? -1024 : 1023;
        sy = y < 0 ? -1024 : 1023;
    }
    sx = std::clamp(sx, -1024, 1023);
    sy = std::clamp(sy, -1024, 1023);
    if (Chance(c, 16)) { sx = Around(c, 1024); sy = Around(c, 1024); }
    const uint32_t sxy = (static_cast<uint32_t>(sy) << 16) | (static_cast<uint32_t>(sx) & 0xFFFFu);
    PutWord(m, r, static_cast<uint32_t>(x));
    PutWord(m, r + 4u, static_cast<uint32_t>(y));
    PutWord(m, r + 8u, static_cast<uint32_t>(z));
    PutWord(m, r + 16u, c.rng->U32() & 0xFFFFu);
    PutWord(m, r + 20u, c.rng->U32() & 0xFFu);
    PutWord(m, r + 24u, sxy);
    // The callers: the road's stepped points the same outcode as the subdivider's, a band-1 corner the vertex
    // pass's byte (0x20 below 1024 units, 0x10 below 40, the screen bits) ORed with the depth bit of 0x32000.
    uint32_t f = D::Outcode(sxy, z);
    if (!road) {
        f &= 0x1Fu;
        f |= (zCell < 1024 ? 0x20u : 0u);
        f |= z < 0x32000 ? (z < 0xC800 ? 0x80u : 0x40u) : 0x20u;
    }
    if (Chance(c, 12)) f |= 1u << (c.rng->U32() % 8u);
    PutWord(m, r + 28u, (f << 24) | (c.rng->U32() & 0xFFFFFFu));
}

// The scene: the corners, junk in the records the call writes, the heap and the two texel high halves.
void Plant(CaseContext& c, uint32_t corners, bool road) {
    Memory& m = *c.mem;
    for (uint32_t i = corners; i < static_cast<uint32_t>(D::kMaxRecords); ++i)
        for (uint32_t k = 0; k < 8; ++k) PutWord(m, D::kRecords + 32u * i + 4u * k, c.rng->U32());
    // near geometry mostly (the subdividers run for near polygons), sometimes a far or a behind-the-eye corner
    const int32_t zBase = Chance(c, 3) ? 20 + static_cast<int32_t>(c.rng->U32() % 400u)
                                       : static_cast<int32_t>(c.rng->U32() % 1600u);
    for (uint32_t i = 0; i < corners; ++i) {
        int32_t z = zBase + Around(c, 200);
        if (Chance(c, 10)) z = Around(c, 80);
        PutCorner(c, i, z, road);
    }
    const uint32_t state = m.PeekWord(0x8005B470u);
    uint32_t heap = m.PeekWord(state + 0x10Cu);
    const uint32_t end = m.PeekWord(D::kHeapEnd);
    if (heap < 0x80000000u || heap >= 0x80200000u || heap + 8192u >= end) heap = end - 8192u;
    PutWord(m, D::kHeapNext, heap);
    if (Chance(c, 8)) PutWord(m, D::kHeapEnd, heap + 24u + 4u * (c.rng->U32() % 64u)); // the overflow arm
    PutWord(m, D::kHeapPrev, Chance(c, 2) ? heap - 52u : (0x80000000u | (c.rng->U32() & 0x1FFFFCu)));
    PutWord(m, D::kUvHigh0, (c.rng->U32() & 0x7FFFu) << 16);
    PutWord(m, D::kUvHigh1, (c.rng->U32() & 0x01FFu) << 16);
}

Row SubRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup, Fn fn) {
    return Row{name, sig, entry, std::move(setup), nullptr, false,
               [fn](NativeEnv& env, const Args& a) { return Run(env, fn, a); },
               {OracleCallee{D::kHeapFullFn, 2, 0, {}, 0}}};
}

} // namespace subdivrows

void AddSubdivRows(std::vector<Row>& rows) {
    using namespace subdivrows;

    rows.push_back(SubRow(
        "subdiv_tri", "void SubTri(s32 base, u32 corners, s32 level)  [RASHCDG 0x8006929C]", D::kSubTriFn,
        [](CaseContext& c, Args& a) {
            Plant(c, 3, false);
            a.a[0] = 3; // 0x8006A630's call: fn(3, 0x20100, 5 - n)
            a.a[1] = 0x20100u;
            a.a[2] = 5u - (Chance(c, 2) ? 0u : 1u);
        },
        Fn::Tri));

    rows.push_back(SubRow(
        "subdiv_quad", "void SubQuad(s32 base, u32 corners, s32 level)  [RASHCDG 0x80069784]", D::kSubQuadFn,
        [](CaseContext& c, Args& a) {
            Plant(c, 4, false);
            a.a[0] = 4; // 0x8006A630's call: fn(4, 0x3020100, 5 - n)
            a.a[1] = 0x3020100u;
            a.a[2] = 5u - (Chance(c, 2) ? 0u : 1u);
        },
        Fn::Quad));

    rows.push_back(SubRow(
        "subdiv_road", "void SubRoad(s32 base, u32 corners, s32 level)  [RASHCDG 0x80069CF0]", D::kSubRoadFn,
        [](CaseContext& c, Args& a) {
            Plant(c, 4, true);
            a.a[0] = 4; // 0x8006F500..0x8006F534: fn(4, 0x1030200, 5)
            a.a[1] = 0x1030200u;
            a.a[2] = 5u;
        },
        Fn::Road));

    rows.push_back(SubRow(
        "subdiv_line", "void SubLine(s32 base, u32 corners, u32 colour)  [RASHCDG 0x8006A25C]", D::kSubLineFn,
        [](CaseContext& c, Args& a) {
            Plant(c, 4, true);
            a.a[0] = 4; // 0x8006E474's lane lines: fn(4, 0x1030200, colour)
            a.a[1] = 0x1030200u;
            a.a[2] = c.rng->U32() & 0xFFFFFFu;
        },
        Fn::Line));
}
