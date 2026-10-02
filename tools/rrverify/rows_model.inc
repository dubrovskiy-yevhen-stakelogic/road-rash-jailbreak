// Bench rows of the model draw's geometry half (src\game\sim\model_draw.h).
// Included by verify_physics.cpp inside its anonymous namespace, after rows_fx.inc, whose helpers
// (fxrows::PlantEntity, PlantChain, Around, PutS16, PutByte) it reuses.
//
// The GTE: the native side starts from the clone's own COP2 registers (the snapshot's, the ones the
// guest run starts from): OFX / OFY / H and SXY2 are the console's.
//
// The primitive emitter SLUS 0x800251E4 (the textured quads, the shadow, the effect capture
// 0x80029CA4) is the model draw's callee: `model_draw` has the ORACLE run it, at every depth of the
// recursion, and witnesses the calls. Everything else it reaches is ported and runs natively.
//
// Named bounds (each a state the game builds or keeps):
//   * the objects are the snapshot's own bound model objects (bikes and riders of pool 0 / 1, the live
//     cars of pool 3, the eight weapon objects): +0x00 / +0x04 / +0x60 are a registry binding;
//   * the matrix stack stays inside the scratchpad (the programs push at most five entries above
//     0x1F800320);
//   * the player count is 1 (the snapshot's): with 2 the emitter's shadow arm 0x80026960 runs, whose
//     view records a one-player capture does not hold.
namespace mdrows {

namespace s = rr::sim;
namespace M = rr::sim::model;
using fxrows::Around;
using fxrows::PutByte;
using fxrows::PutS16;
using spinerows::Chance;

constexpr uint32_t kVerts = 0x800220A4, kEuler = 0x8001FD24, kScale = 0x8004D014, kSeat = 0x80066A84,
                   kChild = 0x80066B98, kBike = 0x80066EC4, kWalk = 0x80067064, kRider = 0x8006745C,
                   kObject = 0x80066B28, kVisible = 0x80067AC4, kDraw = 0x80068468;
constexpr uint32_t kCarSlots = 0x800CF660, kCarBytes = 512, kWeaponObjs = 0x800CF018, kWeaponBytes = 172;

struct OracleEmit final : M::DrawCallees {
    NativeEnv& env;
    explicit OracleEmit(NativeEnv& e) : env(e) {}
    bool Emit(uint32_t obj, uint32_t view) override {
        env.CallOracle(M::kEmitFn, obj, view, 0, 0);
        return env.failure.empty();
    }
};

template <class F>
uint32_t Run(NativeEnv& env, F&& f) {
    s::GuestRam g = RqView(env);
    g.SetScratchpad(env.clone->mem.scratchpad().data());
    M::ModelGte gte = M::ModelGte::From(env.clone->cpu.gte().cr, env.clone->cpu.gte().dr);
    const uint32_t v = f(g, gte);
    return RqDone(env, g, v);
}

// ---------------------------------------------------------------------------- the objects
uint32_t BikeAt(Memory& m, uint32_t slot) { return m.PeekWord(kPoolBasePtr) + 1096u * slot; }

// A live car of pool 3, or 0.
uint32_t AnyCar(CaseContext& c) {
    Memory& m = *c.mem;
    uint32_t live[16];
    uint32_t n = 0;
    for (uint32_t k = 0; k < 16; ++k) {
        const uint32_t car = kCarSlots + kCarBytes * k;
        if ((m.PeekWord(car + 0xACu) & 0xFFFFu) != 0 && (m.PeekWord(car + 0x140u) & 0xFFFFu) != 0 &&
            m.PeekWord(car) >= 0x80000000u)
            live[n++] = car;
    }
    return n == 0 ? 0u : live[c.rng->U32() % n];
}

enum class Pick { Bike, Rider, Car, Weapon };

// One bound object of the snapshot: a bike (with its rider child), a rider, a live car, a weapon object.
uint32_t PickObject(CaseContext& c, Pick& kind) {
    Memory& m = *c.mem;
    const uint32_t k = c.rng->U32() % 8u;
    if (k == 5) {
        const uint32_t car = AnyCar(c);
        if (car != 0) { kind = Pick::Car; return car; }
    }
    if (k == 6) { kind = Pick::Weapon; return kWeaponObjs + kWeaponBytes * (c.rng->U32() % 8u); }
    const uint32_t b = BikeAt(m, Chance(c, 3) ? 0u : c.rng->U32() % 18u);
    if (k == 7) { kind = Pick::Rider; return m.PeekWord(b + 852u); }
    kind = Pick::Bike;
    return b;
}

void PutMatrix(CaseContext& c, uint32_t a) {
    // a unit-sized 3x3 (the sample's orientation when there is one), sometimes anything
    Memory& m = *c.mem;
    for (uint32_t k = 0; k < 9; ++k) {
        int32_t v = c.rng->Unit();
        if (c.sample != nullptr && !Chance(c, 3)) v = c.sample->mA[k];
        if (Chance(c, 40)) v = c.rng->S16();
        PutS16(m, a + 2u * k, v);
    }
}

// A matrix-stack entry: the render camera's (perturbed) or a random unit matrix, and a camera-space
// translation in the draw's range.
void PutEntry(CaseContext& c, uint32_t a) {
    Memory& m = *c.mem;
    if (Chance(c, 2)) {
        const uint32_t rc = m.PeekWord(M::kRenderCamPtrs) + 92u;
        for (uint32_t k = 0; k < 9; ++k) PutS16(m, a + 2u * k, GetS16(m, rc + 2u * k) + Around(c, Chance(c, 3) ? 300 : 0));
    } else {
        PutMatrix(c, a);
    }
    PutS16(m, a + 18u, c.rng->S16());
    m.PokeWord(a + 20u, static_cast<uint32_t>(Around(c, Chance(c, 4) ? 20000 : 3000)));
    m.PokeWord(a + 24u, static_cast<uint32_t>(Around(c, Chance(c, 4) ? 12000 : 1500)));
    m.PokeWord(a + 28u, static_cast<uint32_t>(Chance(c, 8) ? Around(c, 4000) : 200 + static_cast<int32_t>(c.rng->U32() % 40000u)));
}

// The fields of `o` (and of its children) the geometry reads, stirred.
void StirObject(CaseContext& c, uint32_t o) {
    Memory& m = *c.mem;
    if (Chance(c, 2)) for (uint32_t k = 0; k < 3; ++k) m.PokeWord(o + 12u + 4u * k, static_cast<uint32_t>(Around(c, 60000)));
    if (Chance(c, 2)) for (uint32_t k = 0; k < 3; ++k) PutS16(m, o + 28u + 2u * k, Around(c, Chance(c, 4) ? 30000 : 900));
    if (Chance(c, 3)) PutByte(m, o + 72u, Chance(c, 2) ? 3u : (Chance(c, 2) ? 1u : 0u));
    if (Chance(c, 3)) PutMatrix(c, o + 104u);
    if (Chance(c, 4)) m.PokeWord(o + 76u, Chance(c, 2) ? 0x10000u : 0x8000u + c.rng->U32() % 0x10000u);
    if (Chance(c, 2)) PutByte(m, o + 9u, c.rng->U32());
    const uint32_t parts = m.PeekWord(o + 4u);
    const uint32_t n = m.PeekWord(o) >= 0x80000000u ? (m.PeekWord(m.PeekWord(o) + 24u) & 0xFFFFu) : 0u;
    for (uint32_t k = 0; k < n && k < 17u; ++k)
        if (Chance(c, 3)) PutMatrix(c, parts + 24u * k + 4u);
    const uint32_t pool0 = m.PeekWord(kPoolBasePtr);
    const bool bike = o >= pool0 && o < pool0 + 1096u * 20u;
    if (bike && Chance(c, 3)) {
        PutS16(m, o + 836u, c.rng->S16());
        PutS16(m, o + 838u, c.rng->S16());
    }
    if (Chance(c, 3)) { // the camera matrix and position the visibility test leaves
        const uint32_t rc = m.PeekWord(M::kRenderCamPtrs) + 92u;
        for (uint32_t k = 0; k < 8; ++k) m.PokeWord(o + 136u + 4u * k, m.PeekWord(rc + 4u * k));
        m.PokeWord(o + 80u, static_cast<uint32_t>(Around(c, 3000)));
        m.PokeWord(o + 84u, static_cast<uint32_t>(Around(c, 1500)));
        m.PokeWord(o + 88u, static_cast<uint32_t>(200 + static_cast<int32_t>(c.rng->U32() % 30000u)));
    }
    if (bike && Chance(c, 4)) m.PokeWord(o + 596u, Chance(c, 2) ? 0u : c.rng->U32() % 400u);
    if (bike && Chance(c, 4)) m.PokeWord(o + 600u, c.rng->U32() % 800u);
    if (bike && Chance(c, 4)) m.PokeWord(o + 560u, c.rng->U32());
}

void Players(CaseContext& c) {
    Memory& m = *c.mem;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    if (Chance(c, 3)) m.PokeWord(gs + 4u, m.PeekWord(gs + 4u) ^ 0x10u);
    if (Chance(c, 6)) m.PokeWord(gs + 0x30u, 0u);
}

void PlantStack(CaseContext& c, uint32_t top) {
    Memory& m = *c.mem;
    PutEntry(c, top - 32u);
    PutEntry(c, top);
    m.PokeWord(M::kStackPtr, top);
}

// A part-0 array of the model's vertex buffer stirred (the attachment walk reads the parent's
// transformed vertices there).
void StirVerts(CaseContext& c) {
    Memory& m = *c.mem;
    if (!Chance(c, 2)) return;
    const uint32_t cam = m.PeekWord(M::kVertsPtr);
    for (uint32_t v = 0; v < 64; ++v)
        for (uint32_t k = 0; k < 3; ++k) m.PokeWord(cam + 16u * v + 4u * k, static_cast<uint32_t>(Around(c, 4000)));
}

Row MdRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
          std::function<uint32_t(NativeEnv&, const Args&)> native, bool compareV0,
          std::vector<OracleCallee> callees = {}) {
    return Row{name, sig, entry, std::move(setup), nullptr, compareV0, std::move(native), std::move(callees)};
}

} // namespace mdrows

void AddModelRows(std::vector<Row>& rows) {
    using namespace mdrows;

    rows.push_back(MdRow(
        "model_verts", "void ModelVerts(entry, n, SVECTOR *v, cam, [sp+16] sxy, [sp+20] flags)  [SLUS 0x800220A4]", kVerts,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            Pick kind;
            const uint32_t o = PickObject(c, kind);
            const uint32_t n = m.PeekWord(m.PeekWord(o) + 24u) & 0xFFFFu;
            const uint32_t part = n == 0 ? 0u : c.rng->U32() % n;
            const uint32_t dpd = m.PeekWord(m.PeekWord(o + 4u) + 24u * part);
            const uint32_t vb = m.PeekWord(dpd + 16u);
            uint32_t count = m.PeekWord(dpd + 12u) >> 16;
            if (Chance(c, 6)) count = 1u + c.rng->U32() % 3u; // the one- and two-vertex tails
            PutEntry(c, c.scratch + 64u);
            a.a[0] = c.scratch + 64u;
            a.a[1] = count;
            a.a[2] = m.PeekWord(m.PeekWord(o) + 36u) + vb * 8u + 4u;
            a.a[3] = m.PeekWord(M::kVertsPtr) + vb * 16u;
            a.a4 = m.PeekWord(M::kScreenPtr) + vb * 4u;
            a.more[0] = m.PeekWord(M::kFlagsPtr) + vb;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, M::ModelGte& gte) {
                M::ModelVerts(g, gte, a.a[0], a.a[1] & 0xFFFFu, a.a[2], a.a[3], a.a4, a.more[0]);
                return 0u;
            });
        },
        false));

    rows.push_back(MdRow(
        "model_euler", "void EulerMatrix(s32 x, s32 y, s32 z, MATRIX *m)  [SLUS 0x8001FD24]", kEuler,
        [](CaseContext& c, Args& a) {
            a.a[0] = c.rng->U32();
            a.a[1] = Chance(c, 2) ? 0u : c.rng->U32();
            a.a[2] = Chance(c, 2) ? 0u : c.rng->U32();
            a.a[3] = c.scratch + 64u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, M::ModelGte&) {
                M::EulerMatrix(g, a.a[0], a.a[1], a.a[2], a.a[3]);
                return 0u;
            });
        },
        false));

    rows.push_back(MdRow(
        "model_scale", "MATRIX *ScaleMatrix(MATRIX *m, const s32 s[3])  [SLUS 0x8004D014]", kScale,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            PutMatrix(c, c.scratch + 64u);
            PutS16(m, c.scratch + 82u, c.rng->S16());
            for (uint32_t k = 0; k < 3; ++k)
                m.PokeWord(c.scratch + 96u + 4u * k,
                           Chance(c, 3) ? 4096u : static_cast<uint32_t>(static_cast<int32_t>(c.rng->S16())));
            a.a[0] = c.scratch + 64u;
            a.a[1] = c.scratch + 96u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, M::ModelGte&) {
                const int32_t f[3] = {g.S32(a.a[1]), g.S32(a.a[1] + 4u), g.S32(a.a[1] + 8u)};
                M::ScaleMatrix(g, a.a[0], f);
                return a.a[0];
            });
        },
        true));

    rows.push_back(MdRow(
        "model_seat_vertex", "SVECTOR *SeatVertex(Object *o)  [RASHCDG 0x80066A84]", kSeat,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            Pick kind;
            const uint32_t o = PickObject(c, kind);
            const uint32_t reg = m.PeekWord(o + 96u);
            const uint32_t lods = reg >= 0x80000000u ? (m.PeekWord(reg + 4u) & 0xFFu) : 1u;
            if (Chance(c, 2) && lods != 0) PutByte(m, o + 8u, c.rng->U32() % lods);
            a.a[0] = o;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, M::ModelGte&) { return M::SeatVertex(g, a.a[0]); });
        },
        true));

    rows.push_back(MdRow(
        "model_child_place", "void ChildPlace(Object *o, s32 seat)  [RASHCDG 0x80066B98]", kChild,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            const uint32_t b = BikeAt(m, Chance(c, 3) ? 0u : c.rng->U32() % 18u);
            StirObject(c, b);
            StirObject(c, m.PeekWord(b + 56u));
            PlantStack(c, 0x1F800320u + 32u * (c.rng->U32() % 3u));
            a.a[0] = b;
            a.a[1] = 0;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, M::ModelGte& gte) {
                M::ChildPlace(g, gte, a.a[0], a.a[1]);
                return 0u;
            });
        },
        false));

    rows.push_back(MdRow(
        "model_attach_walk", "void AttachWalk(s32 prog, Object *o)  [RASHCDG 0x80067064]", kWalk,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            Pick kind;
            uint32_t o = PickObject(c, kind);
            if (kind == Pick::Car) o = BikeAt(m, 0);
            StirObject(c, o);
            StirVerts(c);
            PlantStack(c, 0x1F800320u);
            // the program the object's own call site picks (rmd3.md 11.6): 0x8006745C for riders and
            // pedestrians, 0x80066F50 for bikes, 0x80066B28 for everything else - a program with more
            // links than the model has parts makes the ORIGINAL read past the part array
            const uint32_t n = m.PeekWord(m.PeekWord(o) + 24u) & 0xFFFFu;
            const uint32_t k = (m.PeekWord(m.PeekWord(o) + 12u) >> 19) & 0xFu;
            uint32_t prog = 4;
            if (k == 1 || k == 4) prog = n == 17 ? 0u : (n == 12 ? 1u : 2u);
            else if (k == 2 && n != 1) prog = n == 6 ? 3u : (n == 5 ? 4u : 5u);
            else if (k != 2 && (n == 4 || n == 3)) prog = n == 4 ? 6u : 7u;
            else { // 0x80066EC4 / 0x80066B28 walk no program for this part count (a LOD-3 bike's parts
                   // past slot 0 are never bound): the player's bike instead
                o = BikeAt(m, 0);
                prog = 4;
            }
            a.a[0] = prog;
            a.a[1] = o;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, M::ModelGte& gte) {
                M::AttachWalk(g, gte, a.a[0], a.a[1]);
                return 0u;
            });
        },
        false));

    rows.push_back(MdRow(
        "model_bike_parts", "void BikeParts(Bike *b)  [RASHCDG 0x80066EC4]", kBike,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            const uint32_t b = BikeAt(m, Chance(c, 3) ? 0u : c.rng->U32() % 18u);
            StirObject(c, b);
            StirObject(c, m.PeekWord(b + 56u));
            StirVerts(c);
            PlantStack(c, 0x1F800320u);
            if (Chance(c, 3)) WriteU16Bench(m, b + 0xACu, static_cast<uint16_t>(c.rng->U32() % 3u));
            a.a[0] = b;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, M::ModelGte& gte) {
                M::BikeParts(g, gte, a.a[0]);
                return 0u;
            });
        },
        false));

    rows.push_back(MdRow(
        "model_rider_parts", "void RiderParts(Object *o)  [RASHCDG 0x8006745C]", kRider,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            Pick kind;
            uint32_t o = PickObject(c, kind);
            if (kind == Pick::Car || Chance(c, 2)) o = m.PeekWord(BikeAt(m, c.rng->U32() % 18u) + 852u);
            StirObject(c, o);
            StirVerts(c);
            PlantStack(c, 0x1F800320u);
            a.a[0] = o;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, M::ModelGte& gte) {
                M::RiderParts(g, gte, a.a[0]);
                return 0u;
            });
        },
        false));

    rows.push_back(MdRow(
        "model_object_parts", "void ObjectParts(Object *o)  [RASHCDG 0x80066B28]", kObject,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            Pick kind;
            uint32_t o = PickObject(c, kind);
            if (Chance(c, 2)) o = kWeaponObjs + kWeaponBytes * (c.rng->U32() % 8u);
            StirObject(c, o);
            StirVerts(c);
            PlantStack(c, 0x1F800320u);
            (void)m;
            a.a[0] = o;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, M::ModelGte& gte) {
                M::ObjectParts(g, gte, a.a[0]);
                return 0u;
            });
        },
        false));

    rows.push_back(MdRow(
        "model_visible", "u32 ModelVisible(Object *o, s32 view)  [RASHCDG 0x80067AC4]", kVisible,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            Pick kind;
            const uint32_t o = PickObject(c, kind);
            StirObject(c, o);
            if (Chance(c, 2)) { // the camera distance per view, the LODs by view
                for (uint32_t v = 0; v < 2; ++v)
                    m.PokeWord(o + 44u + 4u * v, Chance(c, 6) ? c.rng->U32() : c.rng->U32() % 20000u);
                const uint32_t reg = m.PeekWord(o + 96u);
                const uint32_t lods = reg >= 0x80000000u ? (m.PeekWord(reg + 4u) & 0xFFu) : 1u;
                for (uint32_t v = 0; v < 2; ++v) PutByte(m, o + 10u + v, lods ? c.rng->U32() % lods : 0u);
            }
            if (Chance(c, 3)) m.PokeWord(o + 52u, Chance(c, 2) ? 0u : BikeAt(m, 0));
            if (Chance(c, 4)) m.PokeWord(o + 176u, Chance(c, 2) ? 0xFFFFFFFFu : 0u);
            if (Chance(c, 3)) m.PokeWord(o + 100u, 0u);
            if (Chance(c, 3)) m.PokeWord(o + 36u, c.rng->U32());
            if (Chance(c, 3)) m.PokeWord(M::kDrawList, Chance(c, 2) ? 0u : BikeAt(m, 1));
            for (uint32_t k = 0; k < 3; ++k)
                if (Chance(c, 3)) m.PokeWord(o + 184u + 4u * k, m.PeekWord(m.PeekWord(M::kRenderCamPtrs) + 16u + 4u * k) +
                                                                static_cast<uint32_t>(Around(c, 0x2000000)));
            Players(c);
            a.a[0] = o;
            a.a[1] = Chance(c, 6) ? 1u : 0u;
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, M::ModelGte& gte) { return M::ModelVisible(g, gte, a.a[0], a.a[1]); });
        },
        true));

    rows.push_back(MdRow(
        "model_lod", "void ModelLod(Object *o, s32 views)  [RASHCDG 0x800667C4]", 0x800667C4u,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            Pick kind;
            const uint32_t o = PickObject(c, kind);
            const uint32_t reg = m.PeekWord(o + 96u);
            const uint32_t lods = reg >= 0x80000000u ? (m.PeekWord(reg + 4u) & 0xFFu) : 1u;
            if (Chance(c, 4)) { // another kind's LOD table (the four RegistryBind picks), or none
                static const uint32_t kTables[5] = {0x80054160u, 0x80054178u, 0x80054198u, 0x800541B8u, 0u};
                m.PokeWord(o + 100u, kTables[c.rng->U32() % 5u]);
            }
            for (uint32_t v = 0; v < 2; ++v) {
                m.PokeWord(o + 44u + 4u * v, Chance(c, 8) ? c.rng->U32() : c.rng->U32() % 40000u);
                if (Chance(c, 2) && lods != 0) PutByte(m, o + 10u + v, c.rng->U32() % lods);
            }
            if (Chance(c, 3)) WriteU16Bench(m, o + 172u, static_cast<uint16_t>(c.rng->U32()));
            if (kind == Pick::Rider && Chance(c, 3)) PutByte(m, o + 572u, c.rng->U32());
            PutByte(m, o + 9u, c.rng->U32());
            Players(c);
            a.a[0] = o;
            a.a[1] = Chance(c, 3) ? 2u : (Chance(c, 8) ? 0u : 1u);
        },
        [](NativeEnv& env, const Args& a) {
            return Run(env, [&](s::GuestRam& g, M::ModelGte&) {
                M::ModelLod(g, a.a[0], a.a[1]);
                return 0u;
            });
        },
        false));

    {   // the emitter 0x800251E4 (its frame 0x78 below this one's 136) on the ORACLE, at every depth
        Row r = MdRow(
            "model_draw", "void ModelDraw(Object *o, s32 view)  [RASHCDG 0x80068468, 0x800251E4 on the oracle]", kDraw,
            [](CaseContext& c, Args& a) {
                Memory& m = *c.mem;
                Pick kind;
                const uint32_t o = PickObject(c, kind);
                StirObject(c, o);
                for (uint32_t k = 0; k < 2; ++k) {
                    const uint32_t ch = m.PeekWord(o + 56u + 8u * k);
                    if (ch >= 0x80000000u && Chance(c, 2)) StirObject(c, ch);
                }
                fxrows::PlantEntity(c, o);
                // effect chains on the player's machine only (rows_fx.inc's bound: a part-streak record
                // names a part by its kind, and a LOD-3 model has fewer parts than the kinds reach -
                // the ORIGINAL's capture then reads past the part array)
                if (o == BikeAt(m, 0)) {
                    if (Chance(c, 2)) fxrows::PlantChain(c, o, 0, true);
                    const uint32_t rider = m.PeekWord(o + 56u);
                    if (rider >= 0x80000000u && Chance(c, 3)) fxrows::PlantChain(c, rider, 0, false);
                } else {
                    PutByte(m, o + 0x49u, 0xFFu);
                    const uint32_t ch = m.PeekWord(o + 56u);
                    if (ch >= 0x80000000u) PutByte(m, ch + 0x49u, 0xFFu);
                }
                // the depth ranges and the player count are the snapshot's (a one-player race)
                m.PokeWord(m.PeekWord(kGameStatePtr) + 0x30u, 1u);
                if (Chance(c, 3)) m.PokeWord(m.PeekWord(kGameStatePtr) + 4u, m.PeekWord(m.PeekWord(kGameStatePtr) + 4u) ^ 0x10u);
                a.a[0] = o;
                a.a[1] = 0;
            },
            [](NativeEnv& env, const Args& a) {
                return Run(env, [&](s::GuestRam& g, M::ModelGte& gte) {
                    OracleEmit emit(env);
                    M::ModelDraw(g, gte, a.a[0], a.a[1], emit);
                    return 0u;
                });
            },
            false, {OracleCallee{M::kEmitFn, 2, 0, {}, 0}});
        r.oracleFrame = 136;
        r.maxSteps = 20'000'000;
        rows.push_back(std::move(r));
    }

    {   // the render's model half: every model of the draw list, then the effect passes - the packets the
        // effects link (police lights, crash flames, trails, sprays) compared in RAM with the original's.
        // The emitter 0x800251E4 and the two siren callees run on the ORACLE.
        Row r = MdRow(
            "model_render", "void RenderModels(s32 *cells, s32 n, s32 view)  [RASHCDG 0x800674D4, 0x800251E4 + sirens on the oracle]",
            0x800674D4u,
            [](CaseContext& c, Args& a) {
                Memory& m = *c.mem;
                const uint32_t bike = BikeAt(m, 0), rider = m.PeekWord(bike + 852u);
                fxrows::PlantEntity(c, bike);
                PutByte(m, bike + 8u, 0u); // the player's machine at LOD 0 (PlantEntity stirs it)
                m.PokeWord(m.PeekWord(kGameStatePtr) + 0x30u, 1u);
                if (Chance(c, 3)) StirObject(c, bike);
                // the planted records, by the spawners the game calls (their ports are bench rows of their
                // own): the police lights, the crash emitter's pair, a trail on the rider; or random chains
                const uint32_t k = c.rng->U32() % 4u;
                if (k == 3) {
                    fxrows::PlantChain(c, bike, 0, true);
                    if (Chance(c, 2)) fxrows::PlantChain(c, rider, 0, false);
                } else {
                    // written straight into the machine's memory by the ported spawners through a view
                    std::vector<uint8_t> ram(0x200000u);
                    m.ReadBlock(0x80000000u, ram.data(), 0x200000u);
                    s::GuestRam g(ram.data(), c.gp);
                    if (k != 1) {
                        g.W32(bike + 0xB4u, 18u + c.rng->U32() % 3u);
                        s::CopJoin(g, bike);
                    }
                    if (k != 2) {
                        s::CrashEmit(g, bike, 0, 0);
                        s::CrashEmit(g, bike, 1, 2);
                    }
                    s::SpineIo io;
                    io.rootCounter = c.rng->U32() & 0xFFFFu;
                    s::weapon::ObjectEffect(g, rider, 0, 1500, 6, 0, io);
                    m.WriteBlock(0x80000000u, ram.data(), 0x200000u);
                    // and the records' clocks and sub-states stirred
                    for (uint32_t i = 0; i < 20; ++i) {
                        const uint32_t rec = s::kEffectPool + s::kEffectRecordBytes * i;
                        if (((m.PeekWord(rec) >> 6) & 0xFu) == 0 || !Chance(c, 3)) continue;
                        m.PokeWord(rec + 0x30u, m.PeekWord(rec + 0x30u) - c.rng->U32() % 400u);
                        if (Chance(c, 3)) m.PokeWord(rec, (m.PeekWord(rec) & ~0x3C00u) | ((c.rng->U32() % 3u) << 10));
                    }
                }
                // the cells of the draw list's entries, in list order, as the render hands them in
                uint32_t n = 0;
                for (uint32_t p = m.PeekWord(M::kDrawList); p != 0 && n < 32; p = m.PeekWord(p + 168u)) {
                    const uint32_t cell = m.PeekWord(p + 176u);
                    for (uint32_t idx = 0; idx < 64; ++idx) {
                        if (m.PeekWord(M::kCellRecords + 8u + 112u * idx) != cell) continue;
                        bool seen = false;
                        for (uint32_t j = 0; j < n; ++j) seen = seen || m.PeekWord(c.scratch + 64u + 4u * j) == idx;
                        if (!seen) m.PokeWord(c.scratch + 64u + 4u * n++, idx);
                        break;
                    }
                }
                a.a[0] = c.scratch + 64u;
                a.a[1] = n;
                a.a[2] = 0;
            },
            [](NativeEnv& env, const Args& a) {
                return Run(env, [&](s::GuestRam& g, M::ModelGte& gte) {
                    OracleEmit emit(env);
                    fxrows::Sirens sirens(env);
                    s::FxEnv fe;
                    fe.gte = s::FxGte::FromControl(env.clone->cpu.gte().cr);
                    fe.io.rootCounter = env.clone->cpu.rootCounterValue;
                    fe.callees = &sirens;
                    // what the effect passes linked, by the drawn record's state (RRJB_MD_STATS prints the tally)
                    struct Tally final : s::FxPacketSink {
                        s::GuestRam* g = nullptr;
                        s::FxEnv* fe = nullptr;
                        void Packet(uint32_t, int32_t, uint32_t) override { ++Counts()[(g->U32(fe->drawing) >> 6) & 15u]; }
                        static size_t* Counts() {
                            static struct Printer {
                                size_t n[16] = {};
                                ~Printer() {
                                    if (std::getenv("RRJB_MD_STATS") == nullptr) return;
                                    std::fprintf(stderr, "model_render packets by state:");
                                    for (int k = 1; k < 8; ++k) std::fprintf(stderr, " %d:%zu", k, n[k]);
                                    std::fprintf(stderr, "\n");
                                }
                            } p;
                            return p.n;
                        }
                    } tally;
                    tally.g = &g;
                    tally.fe = &fe;
                    fe.sink = &tally;
                    M::RenderModels(g, gte, fe, a.a[0], static_cast<int32_t>(a.a[1]), a.a[2], emit);
                    if (fe.refused && env.failure.empty()) env.failure = "fx: " + fe.why;
                    return 0u;
                });
            },
            false,
            {OracleCallee{M::kEmitFn, 2, 0, {}, 0}, OracleCallee{s::kFxSirenStopFn, 1, 0, {}, 0},
             OracleCallee{s::kFxSirenStartFn, 1, 0, {}, 0}});
        r.oracleFrame = 48;
        r.maxSteps = 40'000'000;
        rows.push_back(std::move(r));
    }
}
