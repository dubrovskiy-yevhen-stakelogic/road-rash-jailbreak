// Bench rows of the model shadow SLUS 0x80025EE0
// (src\game\sim\shadow.h). Included by verify_physics.cpp inside its anonymous namespace, after rows_fxdraw.inc,
// whose helpers (fxrows::Around / PutS16 / PutByte, fxdrawrows::PlantRanges, mdrows::BikeAt) it reuses.
//
// Inputs: an object of the snapshot with a quadsD list (a bike of pool 0 at its LOD, or its rider), view 0, its
// ground point +0x1F8 near the render camera's eye (as the >> 10 halfwords the function subtracts), its ground
// normal +0x20A (the snapshot's, a random unit vector, or one nearly across the level light so that both arms of the
// 45-degree bend and a tiny n . L are taken), the level light sometimes another unit vector, the counter pair
// 0x800CCD78 / 0x800CCDA0 equal (the matrix is built) or not (the stored matrix is used, sometimes stirred), the
// model draw's camera-space vertex buffer filled around the camera (sometimes behind it or far), and the
// scratchpad's depth ranges the snapshot's or RenderModels 0x800674D4's.
namespace lookrows {

namespace s = rr::sim;
namespace S = rr::sim::shadow;
using fxrows::Around;
using fxrows::PutByte;
using fxrows::PutS16;
using spinerows::Chance;

void PutUnit(CaseContext& c, uint32_t a) {
    // a random direction of length ~4096
    double v[3], len = 0.0;
    do {
        len = 0.0;
        for (double& x : v) {
            x = static_cast<double>(Around(c, 4096));
            len += x * x;
        }
    } while (len < 1.0);
    len = std::sqrt(len);
    for (uint32_t k = 0; k < 3; ++k) PutS16(*c.mem, a + 2u * k, static_cast<int32_t>(v[k] / len * 4096.0));
}

// An object with a quadsD list: a bike of pool 0 or its rider, or 0.
uint32_t ShadowObject(CaseContext& c) {
    Memory& m = *c.mem;
    for (int tries = 0; tries < 40; ++tries) {
        uint32_t o = mdrows::BikeAt(m, Chance(c, 3) ? 0u : c.rng->U32() % 18u);
        if (Chance(c, 2)) o = m.PeekWord(o + 852u);
        if (o < 0x80000000u || o >= 0x80200000u) continue;
        const uint32_t dod = m.PeekWord(o);
        if (dod < 0x80000000u || dod >= 0x80200000u) continue;
        const uint32_t q = m.PeekWord(dod + 0x2Cu);
        if (q < 0x80000000u || q >= 0x80200000u) continue;
        const uint32_t n = m.PeekWord(q);
        if (n == 0u || n > 64u) continue;
        return o;
    }
    return mdrows::BikeAt(m, 0);
}

} // namespace lookrows

void AddLookRows(std::vector<Row>& rows) {
    using namespace lookrows;

    rows.push_back(fxrows::FxRow(
        "look_shadow", "void Shadow(Object *o, s32 view, s32 *ground, s16 *normal)  [SLUS 0x80025EE0]", S::kShadowFn,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            const uint32_t o = ShadowObject(c);
            a.a[0] = o;
            a.a[1] = 0;
            const uint32_t owner = m.PeekWord(o + 0x34u);
            const uint32_t base = owner != 0 && Chance(c, 2) ? owner : o;
            const uint32_t ground = base + 0x1F8u, normal = base + 0x20Au;
            a.a[2] = ground;
            a.a[3] = normal;
            // the ground point near the eye (the render camera's +0x1C.., world >> 10 units)
            const uint32_t cam = m.PeekWord(S::kRenderCams);
            if (Chance(c, 3)) {
                for (uint32_t k = 0; k < 3; ++k) {
                    const int32_t eye = static_cast<int32_t>(m.PeekWord(cam + 0x1Cu + 4u * k));
                    const int32_t d = Around(c, Chance(c, 6) ? 20000 : 600);
                    m.PokeWord(ground + 4u * k, (static_cast<uint32_t>(eye + d) << 10) | (c.rng->U32() & 0x3FFu));
                }
            }
            // the ground normal and the light: the snapshot's, other unit vectors, or nearly across each other
            if (Chance(c, 3)) PutUnit(c, normal);
            if (Chance(c, 5)) PutUnit(c, S::kLight);
            if (Chance(c, 6)) { // n nearly perpendicular to L: n.L near 0 (the reciprocal's edge)
                const int32_t lx = GetS16(m, S::kLight), ly = GetS16(m, S::kLight + 2u), lz = GetS16(m, S::kLight + 4u);
                PutS16(m, normal, -ly + Around(c, 20));
                PutS16(m, normal + 2u, lx + Around(c, 20));
                PutS16(m, normal + 4u, Around(c, 20) + (Chance(c, 2) ? 0 : lz / 64));
            }
            // the counter pair: equal builds the matrix
            const uint32_t top = c.rng->U32() % 4u;
            m.PokeWord(S::kCounterTop, top);
            m.PokeWord(S::kCounter, Chance(c, 3) ? top : top - 1u - c.rng->U32() % 2u);
            if (Chance(c, 4))
                for (uint32_t k = 0; k < 8; ++k) m.PokeWord(S::kMatrix + 4u * k, c.rng->U32() % 3u == 0 ? c.rng->U32() : static_cast<uint32_t>(Around(c, 4096)) & 0xFFFFu);
            if (Chance(c, 4)) m.PokeWord(0x8005AC8Cu + S::kGpScale, static_cast<uint32_t>(Around(c, 70000)));
            // the camera-space vertices the model draw left (the object's exponent units)
            const uint32_t verts = m.PeekWord(0x8005ACB0u);
            const bool wild = Chance(c, 8);
            for (uint32_t v = 0; v < 400; ++v) {
                int32_t x = Around(c, 3000), y = Around(c, 2500), z = 400 + static_cast<int32_t>(c.rng->U32() % 30000u);
                if (wild && Chance(c, 3)) x = Around(c, 200000), y = Around(c, 200000), z = Around(c, 200000);
                m.PokeWord(verts + 16u * v, static_cast<uint32_t>(x) << 4);
                m.PokeWord(verts + 16u * v + 4u, static_cast<uint32_t>(y) << 4);
                m.PokeWord(verts + 16u * v + 8u, static_cast<uint32_t>(z) << 4);
            }
            fxdrawrows::PlantRanges(c);
        },
        [](NativeEnv& env, const Args& a) {
            s::GuestRam g = RqView(env);
            g.SetScratchpad(env.clone->mem.scratchpad().data());
            const uint32_t* cr = env.clone->cpu.gte().cr;
            S::Gte gte;
            gte.ofx = static_cast<int32_t>(cr[24]);
            gte.ofy = static_cast<int32_t>(cr[25]);
            gte.h = static_cast<uint16_t>(cr[26] & 0xFFFFu);
            gte.zsf4 = static_cast<int16_t>(cr[30] & 0xFFFFu);
            if (!S::Shadow(g, gte, a.a[0], a.a[1], a.a[2], a.a[3]) && !g.Faulted() && env.failure.empty())
                env.failure = "look_shadow: the port refused (heap full / Normalize overflow)";
            return RqDone(env, g, 0u);
        },
        false));
}
