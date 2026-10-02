// Bench rows for src/game/sim/resolvers.{h,cpp}. Included by rows_coll.inc.
//
// The geometry leaves run on boxes taken from THIS machine's live bikes (the eight corners +0xC4 and
// the rows +0x1B0 of one bike, the corners of another moved into, through or beside it), so the
// inside / outside / crossing arms are all reached; their buffers sit in the scratch block. The
// resolvers run on the machine's own bikes against its own bikes or against pool-6 volumes of our own
// making (co::PlantVolumes, in the spawner's layout), their unported callees from the original code
// at the exact stack pointer.

namespace crs {

using co::Chance;
using co::Pick;

// The scratch layout of the leaf rows.
constexpr uint32_t kPts = 0, kBox = 100, kRows = 200, kDir = 220, kOutFace = 240, kOutDist = 244, kQuad = 260,
                   kEdge = 320, kN16 = 360, kOutPt = 380, kPt = 400, kQ = 420;

uint32_t Fin(NativeEnv& env, EnRun& run, bool ok, uint32_t v, const char* what) {
    if (!ok && env.failure.empty() && !run.s.Faulted()) env.failure = std::string(what) + " refused";
    return run.Done(env, v);
}

int32_t Units(Rng& rng, int32_t units) {
    return static_cast<int32_t>(rng.U32() % (2u * 65536u * static_cast<uint32_t>(units))) - 65536 * units;
}

// A box (corners + rows) from a live bike (or the captured one), and eight points: another bike's
// corners moved so that its centre lies near the box's, or the box's own corners pulled towards its
// centre (all inside), or scattered.
void PlantBoxes(CaseContext& c) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t S = c.scratch;
    const uint32_t A = co::Pool0(m, rng.U32() % 18u), B = co::Pool0(m, rng.U32() % 18u);
    std::vector<uint8_t> a(1096), b(1096);
    m.ReadBlock(A, a.data(), 1096);
    m.ReadBlock(B, b.data(), 1096);
    if (c.sample != nullptr && c.sample->entity.size() == 1096) (Chance(rng, 2) ? a : b) = c.sample->entity;
    int32_t ca[3] = {}, cb[3] = {};
    for (uint32_t k = 0; k < 3; ++k) {                                       // the corners' centroids
        int64_t sa = 0, sb = 0;
        for (uint32_t i = 0; i < 8; ++i) {
            sa += static_cast<int32_t>(GetU32V(a, 196 + 12 * i + 4 * k));
            sb += static_cast<int32_t>(GetU32V(b, 196 + 12 * i + 4 * k));
        }
        ca[k] = static_cast<int32_t>(sa / 8);
        cb[k] = static_cast<int32_t>(sb / 8);
    }
    for (uint32_t i = 0; i < 24; ++i) m.PokeWord(S + kBox + 4 * i, GetU32V(a, 196 + 4 * i));
    for (uint32_t i = 0; i < 9; ++i) WriteU16Bench(m, S + kRows + 2 * i, static_cast<uint16_t>(a[432 + 2 * i] | (a[433 + 2 * i] << 8)));
    const uint32_t mode = static_cast<uint32_t>(rng.Next() % 4u);
    int32_t off[3] = {Units(rng, 1), Units(rng, 1) / 4, Units(rng, 1)};
    if (mode == 3) for (int32_t& o : off) o = Units(rng, 4);
    for (uint32_t i = 0; i < 8; ++i)
        for (uint32_t k = 0; k < 3; ++k) {
            int32_t v;
            if (mode == 2) {                                                  // the box's own, shrunk
                const int32_t w = static_cast<int32_t>(GetU32V(a, 196 + 12 * i + 4 * k));
                v = ca[k] + static_cast<int32_t>((static_cast<int64_t>(w - ca[k]) * static_cast<int32_t>(rng.U32() % 20u)) / 16);
            } else {
                v = static_cast<int32_t>(GetU32V(b, 196 + 12 * i + 4 * k)) - cb[k] + ca[k] + off[k];
            }
            m.PokeWord(S + kPts + 12 * i + 4 * k, static_cast<uint32_t>(v));
        }
    int16_t d[3];
    if (Chance(rng, 2)) for (uint32_t k = 0; k < 3; ++k) d[k] = static_cast<int16_t>(b[450 + 2 * k] | (b[451 + 2 * k] << 8));
    else co::Dir(rng, d);
    PutVec3S16(m, S + kDir, d);
    m.PokeWord(S + kOutFace, rng.U32());
    m.PokeWord(S + kOutDist, rng.U32());
}
uint32_t Mask(Rng& rng) { return Pick(rng, {0u, 0u, 16u, 48u, rng.U32() & 63u, rng.U32()}); }

// ---------------------------------------------------------------- FaceNormal, 0x800B675C
// Driven: faces 0..5, 6..7 (the table copy's unwritten frame bytes) and now and then any byte (the
// caller's frame; every one lands inside RAM); outPt 0 or a buffer; out aliasing the rows now and then.
void SetupFaceNormal(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    PlantBoxes(c);
    const uint32_t S = c.scratch;
    a.a[0] = S + kBox;
    a.a[1] = S + kRows;
    a.a[2] = Chance(rng, 8) ? rng.U32() % 256u : rng.U32() % 8u;
    a.a[3] = Chance(rng, 8) ? S + kRows + 2u * (rng.U32() % 6u) : S + kN16;
    a.a4 = Chance(rng, 3) ? 0u : S + kOutPt;
}
uint32_t NativeFaceNormal(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const bool ok = rr::sim::FaceNormal(run.g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4, env.sp);
    return Fin(env, run, ok, 0, "FaceNormal");
}

// ---------------------------------------------------------------- XzProject, 0x800B6B58
void SetupXz(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    PlantBoxes(c);
    const uint32_t S = c.scratch;
    if (Chance(rng, 4)) for (uint32_t k = 0; k < 3; ++k) c.mem->PokeWord(S + kPts + 4 * k, rng.U32());
    a.a[0] = S + kPts + 12u * (rng.U32() % 8u);
    a.a[1] = Chance(rng, 2) ? S + kRows + 6u * (rng.U32() % 3u) : S + kDir;
    a.a[2] = S + kBox + 12u * (rng.U32() % 8u);
}
uint32_t NativeXz(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const int32_t v = rr::sim::XzProject(run.g, a.a[0], a.a[1], a.a[2]);
    return Fin(env, run, true, static_cast<uint32_t>(v), "XzProject");
}

// ---------------------------------------------------------------- PointInPoly, 0x800B6D70
// A face quad and its edge normals (built by the original FaceQuad's layout: the box's corners and
// rows), a point near the face; n 0..4 and past it, tol 2048, 0 or anything.
void SetupPoly(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    PlantBoxes(c);
    const uint32_t S = c.scratch;
    for (uint32_t i = 0; i < 4; ++i)
        for (uint32_t k = 0; k < 3; ++k) m.PokeWord(S + kQuad + 12 * i + 4 * k, m.PeekWord(S + kBox + 12 * Pick(rng, {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u}) + 4 * k));
    for (uint32_t i = 0; i < 4; ++i) {
        int16_t n[3];
        GetVec3S16(m, S + kRows + 6u * (rng.U32() % 3u), n);
        if (Chance(rng, 2)) for (int16_t& x : n) x = static_cast<int16_t>(-x);
        PutVec3S16(m, S + kEdge + 6 * i, n);
    }
    a.a[0] = S + kPts + 12u * (rng.U32() % 8u);
    a.a[1] = S + kQuad;
    a.a[2] = S + kEdge;
    a.a[3] = Pick(rng, {4u, 4u, 4u, 0u, 1u, 3u, 0xFFFFFFFFu});
    a.a4 = Pick(rng, {2048u, 2048u, 0u, rng.U32() >> (rng.Next() % 24u), 0x80000000u});
}
uint32_t NativePoly(NativeEnv& env, const Args& a) {
    EnRun run(env);
    uint32_t v = 0;
    const bool ok = rr::sim::PointInPoly(run.g, a.a[0], a.a[1], a.a[2], static_cast<int32_t>(a.a[3]),
                                         static_cast<int32_t>(a.a4), env.sp, v);
    return Fin(env, run, ok, v, "PointInPoly");
}

// ---------------------------------------------------------------- FirstPointInsideBox, 0x800B7030
void SetupFirstInside(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    PlantBoxes(c);
    const uint32_t S = c.scratch;
    a.a[0] = S + kPts;
    a.a[1] = S + kRows;
    a.a[2] = S + kBox;
    a.a[3] = Mask(rng);
    a.a4 = S + kOutFace;
    a.more[0] = Chance(rng, 5) ? 0u : S + kOutDist;
}
uint32_t NativeFirstInside(NativeEnv& env, const Args& a) {
    EnRun run(env);
    uint32_t v = 0;
    const bool ok = rr::sim::FirstPointInsideBox(run.g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4, a.more[0], env.sp, v);
    return Fin(env, run, ok, v, "FirstPointInsideBox");
}

// ---------------------------------------------------------------- DeepestInsideAlongDir, 0x800B71AC
void SetupDeepInside(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    PlantBoxes(c);
    const uint32_t S = c.scratch;
    a.a[0] = S + kPts;
    a.a[1] = S + kDir;
    a.a[2] = Pick(rng, {1u, 0u, 0xFFFFFFFFu, rng.U32()});
    a.a[3] = S + kBox;
    a.a4 = S + kRows;
    a.more[0] = Mask(rng);
    a.more[1] = S + kOutFace;
    a.more[2] = S + kOutDist;
}
uint32_t NativeDeepInside(NativeEnv& env, const Args& a) {
    EnRun run(env);
    uint32_t v = 0;
    const bool ok = rr::sim::DeepestInsideAlongDir(run.g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), a.a[3], a.a4,
                                                   a.more[0], a.more[1], a.more[2], env.sp, v);
    return Fin(env, run, ok, v, "DeepestInsideAlongDir");
}

// ---------------------------------------------------------------- FaceQuad, 0x800B7810
// Faces 0..5 and past them; the edge normals now and then written OVER the rows (the original reads
// the rows after its first stores), the quad over the box.
void SetupQuad(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    PlantBoxes(c);
    const uint32_t S = c.scratch;
    a.a[0] = S + kBox;
    a.a[1] = S + kRows;
    a.a[2] = Chance(rng, 8) ? rng.U32() : rng.U32() % 7u;
    a.a[3] = Chance(rng, 8) ? S + kBox + 4u * (rng.U32() % 8u) : S + kQuad;
    a.a4 = Chance(rng, 6) ? S + kRows - 2u * (rng.U32() % 6u) + 2u * (rng.U32() % 6u) : S + kEdge;
}
uint32_t NativeQuad(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const bool ok = rr::sim::FaceQuad(run.g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4);
    return Fin(env, run, ok, 0, "FaceQuad");
}

// ---------------------------------------------------------------- DeepestThroughFace, 0x800B74F0
// len of both signs, 0 .. 6.0 units (the swept distance of a sub-step) or anything.
void SetupDeepThrough(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    PlantBoxes(c);
    const uint32_t S = c.scratch;
    a.a[0] = S + kPts;
    a.a[1] = S + kDir;
    const int32_t len = Chance(rng, 6) ? rng.S32() : static_cast<int32_t>(rng.U32() % 0x60000u) * (Chance(rng, 3) ? -1 : 1);
    a.a[2] = static_cast<uint32_t>(len);
    a.a[3] = S + kBox;
    a.a4 = S + kRows;
    a.more[0] = Mask(rng);
    a.more[1] = S + kOutFace;
    a.more[2] = S + kOutDist;
}
uint32_t NativeDeepThrough(NativeEnv& env, const Args& a) {
    EnRun run(env);
    uint32_t v = 0;
    const bool ok = rr::sim::DeepestThroughFace(run.g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]), a.a[3], a.a4,
                                                a.more[0], a.more[1], a.more[2], env.sp, v);
    return Fin(env, run, ok, v, "DeepestThroughFace");
}

// ---------------------------------------------------------------- NoteBigVolume, 0x800B2C98
// A live bike with its camera-band cache (+0x140 bits 2/3) set in, out or unset; the list count
// 0..9 and its ids, the shape's handle byte one of them or not.
void SetupBig(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t e = co::Pool0(m, Pick(rng, {0u, 0u, 6u, rng.U32() % 18u}));
    const uint16_t st = static_cast<uint16_t>(m.PeekWord(e + 320) & 0xFFFFu);
    WriteU16Bench(m, e + 320, static_cast<uint16_t>((st & ~0xCu) | Pick(rng, {0xCu, 0xCu, 0x8u, 0u})));
    const uint32_t n = Pick(rng, {0u, 1u, 3u, 7u, 8u, 9u, rng.U32() % 8u});
    m.PokeWord(0x800CCF90u, n);
    for (uint32_t i = 0; i < 8; ++i) {
        const uint8_t id = static_cast<uint8_t>(0xC0u | (rng.U32() % 8u));
        m.WriteBlock(0x800CCF88u + i, &id, 1);
    }
    const uint32_t S = c.scratch;
    const uint8_t h = static_cast<uint8_t>(0xC0u | (rng.U32() % 8u));
    m.WriteBlock(S, &h, 1);
    a.a[0] = e;
    a.a[1] = S;
}
uint32_t NativeBig(NativeEnv& env, const Args& a) {
    EnRun run(env);
    const bool ok = rr::sim::NoteBigVolume(run.g, a.a[0], a.a[1], env.sp);
    return Fin(env, run, ok, 0, "NoteBigVolume");
}

// ---------------------------------------------------------------- the resolvers' common case
//
// e: the player's bike or bike 6 (both live, with their own road pointers), now and then with a
// passenger (bike 17, listed or not: +0x440). The shape (`kind`): a pool-6 record (co::PlantVolumes,
// class and half extents forced per resolver) or another live bike's +0xAC, moved into or beside e.
// Drawn: the sticky state (+0x340 == shape with +0x235 bit 7) and +0x33C == shape, flagsC +0x238
// bits 9/10, the camera-band cache +0x140, the speed +0x1E0, the lean +0x2A4 and +0x27C, the contact
// count, dt, the partner's road id (+0xBC / +0xA4 against e's +0x168 / +0x150), the players.
// NAMED BOUNDS: no pool 1..5 partner (pool 4 and 5 are empty on this machine and in the product; the
// response callees would walk stale memory there), and every bike is one of this machine's 18. The
// passenger is UNLISTED (+0x440 = 0) as the game makes it (ApplyImpulse moves a listed bike's
// passenger with it): two bikes naming each other as passenger with both listed make BoxResolve and
// PoleResolve alternate between them without end on the console. A pool-6 record keeps the basis
// PlantVolumes gives it (+0x110..+0x115, row 2, becomes the fake entity's heading in PointResolve).
// A bike with NO list node (+0x440 = 0) is only ever a passenger: all 18 bikes of this machine are
// listed. So e is unlisted only while it carries a passenger. An unlisted e without one sends BoxResolve's
// log path through the null passenger (it reads *(u16*)0xAC, never observed)
// and PoleResolve's tail onto entity 0, whose PoleReact then stores into the BIOS image.
struct ResolveCase {
    uint32_t e = 0, shape = 0;
};
ResolveCase PlantResolve(CaseContext& c, uint32_t kind /*0 point, 1 box, 2 pole*/) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    ResolveCase rc;
    const uint32_t P = co::Pool0(m, 0);
    const uint32_t e = co::Pool0(m, Pick(rng, {0u, 0u, 0u, 6u}));
    if (e != P && Chance(rng, 2)) co::MoveNear(m, e, P, Units(rng, 8), 0, Units(rng, 8));
    if (c.sample != nullptr && c.sample->entity.size() == 1096 && Chance(rng, 2)) {
        m.WriteBlock(e + 0x1B0, c.sample->entity.data() + 0x1B0, 0x216 - 0x1B0);
        PutS32(m, e + 0x1E0, c.sample->speed);
    }
    const uint32_t n6 = 1u + rng.U32() % 3u;
    co::PlantVolumes(c, e, n6);
    const uint32_t base6 = m.PeekWord(rr::sim::kPoolTableAddr + 16u * 6u);
    const bool bike = kind != 1 && Chance(rng, kind == 0 ? 2 : 6);
    if (bike) {
        uint32_t o = co::Pool0(m, 1u + rng.U32() % 17u);
        if (o == e) o = co::Pool0(m, 7);
        co::MoveNear(m, o, e, Units(rng, 2), Chance(rng, 3) ? Units(rng, 1) / 2 : 0, Units(rng, 2));
        rc.shape = o + 0xAC;
    } else {
        const uint32_t r = base6 + 280u * (rng.U32() % n6);
        if (kind == 1) {
            m.PokeWord(r + 8, Chance(rng, 8) ? 1u : 0u);
            PutS32(m, r + 0x84, 0x10001 + static_cast<int32_t>(rng.U32() % 0x40000u));
            PutS32(m, r + 0x88, 0x10001 + static_cast<int32_t>(rng.U32() % 0x40000u));
            if (Chance(rng, 2)) PutS32(m, r + 0x8C, 0x20001 + static_cast<int32_t>(rng.U32() % 0x40000u));
        } else if (kind == 2) {
            m.PokeWord(r + 8, 1);
            PutS32(m, r + 0x54, static_cast<int32_t>(rng.U32() % 0x30000u));
            PutS32(m, r + 0x58, static_cast<int32_t>(rng.U32() % 0x60000u));
            if (Chance(rng, 3)) PutS32(m, r + 0x84, static_cast<int32_t>(rng.U32() % 0x8000u));
        }
        // the record's box moved onto e now and then (so the corner tests go both ways)
        if (Chance(rng, 2)) {
            const int32_t dx = GetS32(m, e + 184) + Units(rng, 1) - GetS32(m, r + 12);
            const int32_t dz = GetS32(m, e + 192) + Units(rng, 1) - GetS32(m, r + 20);
            for (uint32_t f = 12; f < 120; f += 12) {
                PutS32(m, r + f, GetS32(m, r + f) + dx);
                PutS32(m, r + f + 8, GetS32(m, r + f + 8) + dz);
            }
        }
        // the road id / piece / lateral of the record: e's own now and then
        if (Chance(rng, 3)) {
            m.PokeWord(r + 188, m.PeekWord(e + 360));
            if (Chance(rng, 2)) m.PokeWord(r + 164, m.PeekWord(e + 336));
            PutS32(m, r + 172, GetS32(m, e + 344) + Units(rng, 1));
        }
        rc.shape = r;
    }
    // sticky / ignored partner
    if (Chance(rng, 5)) m.PokeWord(e + 832, rc.shape);
    if (Chance(rng, 3)) {
        uint8_t b = static_cast<uint8_t>(rng.U32());
        m.WriteBlock(e + 565, &b, 1);
    }
    if (Chance(rng, 6)) m.PokeWord(e + 828, rc.shape);
    uint32_t fc = m.PeekWord(e + 568);
    if (Chance(rng, 4)) fc = (fc & ~0x600u) | Pick(rng, {0x200u, 0x400u, 0x600u});
    m.PokeWord(e + 568, fc);
    const uint16_t st = static_cast<uint16_t>(m.PeekWord(e + 320) & 0xFFFFu);
    if (Chance(rng, 2)) WriteU16Bench(m, e + 320, static_cast<uint16_t>((st & ~0xCu) | Pick(rng, {0xCu, 0x8u, 0u})));
    if (Chance(rng, 3)) m.PokeWord(e + 480, Pick(rng, {0u, 100u, 0x8000u, 0x80000u, 0x180000u, rng.U32() % 0x400000u}));
    if (Chance(rng, 3)) m.PokeWord(e + 676, Pick(rng, {0u, 0x8000u, 0xFFFF0000u, 0x20000u, rng.U32()}));
    if (Chance(rng, 3)) m.PokeWord(e + 636, rng.S32() >> (rng.Next() % 16u));
    // a passenger: bike 17, listed (+0x440) or not
    if (Chance(rng, 4)) {
        const uint32_t b17 = co::Pool0(m, 17);
        m.PokeWord(e + 856, b17);
        m.PokeWord(b17 + 856, e);
        m.PokeWord(b17 + 1088, 0);
        co::MoveNear(m, b17, e, Units(rng, 1) / 4, 0, Units(rng, 1) / 4);
        if (Chance(rng, 6)) m.PokeWord(e + 1088, 0);                      // e itself unlisted
    }
    if (Chance(rng, 6)) co::SetNumPlayers(m, 2);
    m.PokeWord(rr::sim::kContactCount, Pick(rng, {0u, 1u, 3u, 7u, 8u, rng.U32() % 9u}));
    m.PokeWord(rr::sim::kCollDt, Pick(rng, {0x884u, 0x884u, 0x442u, rng.U32() % 0x2000u}));
    m.PokeWord(c.gp + kRandSeedGpOffset, rng.U32());
    rc.e = e;
    return rc;
}

void SetupPoint(CaseContext& c, Args& a) {
    const ResolveCase rc = PlantResolve(c, 0);
    a.a[0] = rc.e;
    a.a[1] = rc.shape;
}
uint32_t NativePoint(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    const bool ok = rr::sim::PointResolve(run.g, a.a[0], a.a[1], env.sp, tab.t, cc);
    return co::Refuse(env, run, ok, "PointResolve");
}
void SetupBoxR(CaseContext& c, Args& a) {
    const ResolveCase rc = PlantResolve(c, 1);
    a.a[0] = rc.e;
    a.a[1] = rc.shape;
}
uint32_t NativeBoxR(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    uint32_t v = 0;
    const bool ok = rr::sim::BoxResolve(run.g, a.a[0], a.a[1], env.sp, tab.t, cc, v);
    return Fin(env, run, ok, v, "BoxResolve");
}
void SetupPole(CaseContext& c, Args& a) {
    const ResolveCase rc = PlantResolve(c, 2);
    a.a[0] = rc.e;
    a.a[1] = rc.shape;
}
uint32_t NativePole(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    const bool ok = rr::sim::PoleResolve(run.g, a.a[0], a.a[1], env.sp, tab.t, cc);
    return co::Refuse(env, run, ok, "PoleResolve");
}

// ---------------------------------------------------------------- RiderWallHit, 0x800B2794
// One of this machine's riders (pool 1, *(0x8005B3A4) + 628 slot), a unit wall normal in the scratch
// (a road wall's: small y), the rider's speed +0x1E0 over the four classes and both signs.
void SetupRiderWall(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t r = m.PeekWord(0x8005B3A4u) + 628u * Pick(rng, {6u, 6u, 0u, rng.U32() % 18u});
    int16_t n[3];
    co::Dir(rng, n);
    if (Chance(rng, 2)) n[1] = static_cast<int16_t>(n[1] / 8);
    PutVec3S16(m, c.scratch, n);
    // +0x228 bit 30 (a rider off its bike): PedHit answers 0 at once without it, the speed being 0 here
    if (!Chance(rng, 3)) m.PokeWord(r + 552, m.PeekWord(r + 552) | 0x40000000u);
    if (Chance(rng, 2)) {
        const int32_t s = static_cast<int32_t>(Pick(rng, {0u, 0x1FFFFu, 0x20000u, 0x60000u, 0x90000u, rng.U32() % 0x200000u, rng.U32()}));
        PutS32(m, r + 480, Chance(rng, 3) ? -s : s);
    }
    m.PokeWord(c.gp + kRandSeedGpOffset, rng.U32());
    a.a[0] = r;
    a.a[1] = c.scratch;
    a.a[2] = rng.U32();
}
uint32_t NativeRiderWall(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const bool ok = rr::sim::RiderWallHit(run.g, a.a[0], a.a[1], env.sp, cc);
    return co::Refuse(env, run, ok, "RiderWallHit");
}

// ---------------------------------------------------------------- Dot32, SLUS 0x8002E604
void SetupDot32(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    PlantBoxes(c);
    const uint32_t S = c.scratch;
    if (Chance(rng, 3)) for (uint32_t k = 0; k < 3; ++k) c.mem->PokeWord(S + kQ + 4 * k, rng.U32() >> (rng.Next() % 20u));
    a.a[0] = S + kPts + 12u * (rng.U32() % 8u);
    a.a[1] = Chance(rng, 2) ? S + kBox + 12u * (rng.U32() % 8u) : S + kQ;
}
uint32_t NativeDot32(NativeEnv& env, const Args& a) {
    EnRun run(env);
    return Fin(env, run, true, static_cast<uint32_t>(rr::sim::Dot32(run.g, a.a[0], a.a[1])), "Dot32");
}

// ---------------------------------------------------------------- BikeVsRider, 0x800AD04C
// e: the player's bike or bike 6; p: one of this machine's riders (pool 1, e's own rider now and then) or
// pedestrians (pool 2, stride 572), moved into or beside e. Drawn: p's +0x228 bits 29/30 (approaching
// test, the two arms), the rider's +0x25C, e's flagsC bits 5..7 / 9..10 / 25, +0x230 bit 29, the
// speeds, the sticky state, the camera-band cache, the game mode byte +0x39 (2 now and then).
// NAMED BOUNDS: p's state +0x220 is left as the machine has it (the caller only passes a partner whose
// state is not 60..63, and a stance of another animation bank traps the original's stance layer);
// every entity is one of this machine's own.
void SetupBvr(CaseContext& c, Args& a) {
    Rng& rng = *c.rng;
    Memory& m = *c.mem;
    const uint32_t e = co::Pool0(m, Pick(rng, {0u, 0u, 6u}));
    uint32_t p;
    if (Chance(rng, 3)) {
        p = m.PeekWord(rr::sim::kPoolTableAddr + 32u) + 572u * (rng.U32() % 3u);
    } else {
        p = Chance(rng, 3) ? m.PeekWord(e + 852) : m.PeekWord(0x8005B3A4u) + 628u * (rng.U32() % 18u);
    }
    co::MoveNear(m, p, e, Units(rng, 2), Chance(rng, 3) ? Units(rng, 1) / 2 : 0, Units(rng, 2));
    uint32_t f = m.PeekWord(p + 552);
    if (Chance(rng, 2)) f = (f & ~0x60000000u) | (rng.U32() & 0x60000000u);
    m.PokeWord(p + 552, f);
    if (Chance(rng, 3)) m.PokeWord(p + 480, rng.U32() % 0x200000u);
    const uint32_t rd = m.PeekWord(e + 852);
    if (Chance(rng, 5)) m.PokeWord(rd + 604, Pick(rng, {1u, 2u, 3u}));
    uint32_t fc = m.PeekWord(e + 568);
    if (Chance(rng, 4)) fc = (fc & ~0x6ECu) | (rng.U32() & 0x6ECu);
    if (Chance(rng, 3)) fc ^= 0x02000000u;
    m.PokeWord(e + 568, fc);
    if (Chance(rng, 4)) m.PokeWord(e + 560, m.PeekWord(e + 560) ^ 0x20000000u);
    // the speed: 0 .. 256.0 (the fast end makes the response sub-step, which is what writes its
    // remaining-time out-param, the push magnitude sp+88)
    if (Chance(rng, 3)) m.PokeWord(e + 480, Pick(rng, {0u, 0x8000u, 0x80000u, 0x180000u, rng.U32() % 0x400000u, 0x600000u + rng.U32() % 0xA00000u}));
    // p's corner heights stirred now and then (the lowest-corner search of 0x800AD544 goes both ways)
    if (Chance(rng, 4)) for (uint32_t k = 0; k < 8; ++k) PutS32(m, p + 200 + 12 * k, GetS32(m, p + 200 + 12 * k) + Units(rng, 1) / 2);
    if (Chance(rng, 5)) m.PokeWord(e + 832, p + 172);
    if (Chance(rng, 3)) {
        uint8_t b = static_cast<uint8_t>(rng.U32());
        m.WriteBlock(e + 565, &b, 1);
    }
    if (Chance(rng, 8)) m.PokeWord(e + 828, p + 172);
    const uint16_t st = static_cast<uint16_t>(m.PeekWord(e + 320) & 0xFFFFu);
    if (Chance(rng, 2)) WriteU16Bench(m, e + 320, static_cast<uint16_t>((st & ~0xCu) | Pick(rng, {0xCu, 0x8u, 0u})));
    if (Chance(rng, 8)) {
        const uint8_t two = 2;
        m.WriteBlock(m.PeekWord(co::kGameStatePtr) + 57u, &two, 1);
    }
    m.PokeWord(0x8005B220u, Chance(rng, 4) ? 1u : 0u);
    m.PokeWord(rr::sim::kCollDt, Pick(rng, {0x884u, 0x884u, 0x442u, 0x1100u, rng.U32() % 0x2000u}));
    m.PokeWord(c.gp + kRandSeedGpOffset, rng.U32());
    a.a[0] = e;
    a.a[1] = p;
}
uint32_t NativeBvr(NativeEnv& env, const Args& a) {
    EnRun run(env);
    co::CloneCallees cc(env, run);
    const cr::Tables tab(env.clone->mem, env.gp);
    const bool ok = rr::sim::BikeVsRider(run.g, a.a[0], a.a[1], env.sp, tab.t, cc);
    return co::Refuse(env, run, ok, "BikeVsRider");
}

void Ex(std::vector<Row>& rows, const char* name, const char* sig, uint32_t entry,
        std::function<void(CaseContext&, Args&)> setup, std::function<uint32_t(NativeEnv&, const Args&)> native,
        bool compareV0, std::vector<OracleCallee> callees = {}) {
    rows.push_back(Row{name, sig, entry, std::move(setup), nullptr});
    Row& r = rows.back();
    r.compareV0 = compareV0;
    r.nativeEx = std::move(native);
    r.oracleCallees = std::move(callees);
}

} // namespace crs

void AddCollResolveRows(std::vector<Row>& rows) {
    using namespace rr::sim::rsv;
    crs::Ex(rows, "face_normal", "void 0x800B675C(corners, rows, face, s16 out[3], [s32 *outPt])  [RASHCDG]", kFaceNormal,
            crs::SetupFaceNormal, crs::NativeFaceNormal, false);
    crs::Ex(rows, "xz_project", "s32 0x800B6B58(s32 p[3], s16 n[3], s32 q[3])  [RASHCDG]", kXzProject, crs::SetupXz,
            crs::NativeXz, true);
    crs::Ex(rows, "point_in_quad", "s32 0x800B6D70(pt, verts, normals, n, [tol])  [RASHCDG]", kPointInPoly,
            crs::SetupPoly, crs::NativePoly, true);
    crs::Ex(rows, "first_point_inside", "u32 0x800B7030(pts, rows, box, mask, [*face, *depth])  [RASHCDG]",
            kFirstPointInside, crs::SetupFirstInside, crs::NativeFirstInside, true);
    crs::Ex(rows, "deepest_inside", "u32 0x800B71AC(pts, dir, sign, box, [rows, mask, *face, *dist])  [RASHCDG]",
            kDeepestInside, crs::SetupDeepInside, crs::NativeDeepInside, true);
    crs::Ex(rows, "face_quad", "void 0x800B7810(box, rows, face, quad, [edgeN])  [RASHCDG, the six-way jump table]",
            kFaceQuad, crs::SetupQuad, crs::NativeQuad, false);
    crs::Ex(rows, "deepest_through", "u32 0x800B74F0(pts, dir, len, box, [rows, mask, *face, *dist])  [RASHCDG]",
            kDeepestThrough, crs::SetupDeepThrough, crs::NativeDeepThrough, true);
    crs::Ex(rows, "note_big_volume", "void 0x800B2C98(E *e, u8 *shape)  [RASHCDG]", kNoteBigVolume, crs::SetupBig,
            crs::NativeBig, false);
    {
        using namespace rr::sim::coll;
        crs::Ex(rows, "point_resolve", "void 0x800B09C4(E *e, u8 *shape)  [RASHCDG, the point resolver]", kPointResolve,
                crs::SetupPoint, crs::NativePoint, false,
                {OracleCallee{kResponse, 4, 2}, OracleCallee{kResponseFar, 4, 1}, OracleCallee{kImpactGate, 4, 1},
                 OracleCallee{kImpactSeverity, 4, 1}});
        rows.back().maxSteps = 20'000'000;
        crs::Ex(rows, "box_resolve", "u32 0x800B0D8C(E *e, u8 *shape)  [RASHCDG, the box resolver]", kBoxResolve,
                crs::SetupBoxR, crs::NativeBoxR, true, {OracleCallee{kBoxReact, 4, 2}});
        rows.back().maxSteps = 20'000'000;
        crs::Ex(rows, "pole_resolve", "void 0x800AE794(E *e, u8 *shape)  [RASHCDG, the pole resolver]", kPoleResolve,
                crs::SetupPole, crs::NativePole, false,
                {OracleCallee{kLeanPoleTest, 4, 1}, OracleCallee{kPoleReact, 4, 2}});
        rows.back().maxSteps = 20'000'000;
        crs::Ex(rows, "rider_wall_hit", "void 0x800B2794(Rider *r, s16 n[3], u32 flags)  [RASHCDG]", kRiderWallHit,
                crs::SetupRiderWall, crs::NativeRiderWall, false, {OracleCallee{kPedHit, 4, 2}});
        rows.back().maxSteps = 20'000'000;
        crs::Ex(rows, "dot32", "s32 0x8002E604(s32 a[3], s32 b[3])  [SLUS]", kDot32, crs::SetupDot32, crs::NativeDot32, true);
        crs::Ex(rows, "bike_vs_rider", "void 0x800AD04C(Bike *e, E *p)  [RASHCDG, the bike against a rider / pedestrian]",
                kBikeVsRider, crs::SetupBvr, crs::NativeBvr, false,
                {OracleCallee{kResponse, 4, 2}, OracleCallee{kResponseFar, 4, 1}, OracleCallee{kImpactGate, 4, 1},
                 OracleCallee{kRiderGrab, 2}, OracleCallee{kPedHit, 4, 2}, OracleCallee{kRiderSpeech, 2},
                 OracleCallee{kPedVoice, 4}, OracleCallee{kPadMotor, 4}});
        rows.back().maxSteps = 20'000'000;
    }
}
