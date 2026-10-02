// Bench rows of the emitter's two glow sprites (src\game\sim\fx_glow.h).
// Included by verify_physics.cpp inside its anonymous namespace, after rows_fx.inc and rows_model.inc,
// whose helpers (fxrows::Run / FxRow / Around / PutS16 / PutByte, mdrows::kWeaponObjs) it reuses.
//
// Inputs: the snapshot's own bound objects (the eight weapon objects 0x800CF018, the bikes of pool 0 with
// their class +0xB4 or another class whose light primitive this bike's model has), the model draw's two
// buffers *(0x8005ACB0) / *(0x8005ACB4) filled with projected points and camera-space depths around the
// screen and the depth range of the snapshot (sometimes negative, sometimes far), and the scratchpad's
// depth ranges either the snapshot's or the ones RenderModels 0x800674D4 writes (both near-depth arms).
// The packet heap and the ordering table are the snapshot's.
namespace fxdrawrows {

namespace s = rr::sim;
using fxrows::Around;
using fxrows::PutByte;
using fxrows::PutS16;
using spinerows::Chance;

// The scratchpad's depth ranges: the snapshot's, or 0x800674D4's stores with *(0x8005B4D4) / *(0x8005B4D8).
void PlantRanges(CaseContext& c) {
    if (Chance(c, 2)) return;
    Memory& m = *c.mem;
    PutS16(m, 0x1F80000Au, 0x800);
    PutS16(m, 0x1F80000Cu, 0x1000);
    PutS16(m, 0x1F800012u, 0x200);
    PutS16(m, 0x1F800014u, 0x300);
    PutByte(m, 0x1F800018u, 0xFD);
    PutByte(m, 0x1F800019u, 0xFE);
    PutS16(m, 0x1F800008u, 0);
    PutS16(m, 0x1F800010u, 0);
    PutByte(m, 0x1F80001Au, 0);
    m.PokeWord(0x1F800004u, Chance(c, 3) ? c.rng->U32() % 9000u : 0u);
    m.PokeWord(0x1F80001Cu, m.PeekWord(s::kFxOtLength) - 2u);
    m.PokeWord(0x1F800000u, Chance(c, 2) ? 5u : c.rng->U32() % 8u);
}

// A camera-space depth word whose value >> `shift` lands near the sprite scale's range, now and then
// non-positive or far.
uint32_t DepthWord(CaseContext& c, uint32_t shift) {
    const int32_t ref = static_cast<int32_t>(c.mem->PeekWord(s::kFxLightScale));
    int32_t d = 1 + static_cast<int32_t>(c.rng->U32() % static_cast<uint32_t>(std::max(ref, 64)));
    if (Chance(c, 3)) d = 1 + static_cast<int32_t>(c.rng->U32() % 60000u);
    if (Chance(c, 12)) d = -static_cast<int32_t>(c.rng->U32() % 3000u);
    return static_cast<uint32_t>(d) << shift | (c.rng->U32() & ((1u << shift) - 1u));
}

// The projected points of every buffer vertex (SX around the 384-wide screen, SY around the 240 lines,
// now and then anything) and their depths.
void PlantBuffers(CaseContext& c, uint32_t shift) {
    Memory& m = *c.mem;
    const uint32_t screen = m.PeekWord(s::kFxModelScreen), verts = m.PeekWord(s::kFxModelVerts);
    const bool wild = Chance(c, 8);
    for (uint32_t k = 0; k < 1200; ++k) {
        int32_t x = 192 + Around(c, 260), y = 120 + Around(c, 180);
        if (wild && Chance(c, 3)) x = c.rng->S16(), y = c.rng->S16();
        m.PokeWord(screen + 4u * k, (static_cast<uint32_t>(x) & 0xFFFFu) | (static_cast<uint32_t>(y) << 16));
        m.PokeWord(verts + 16u * k + 8u, DepthWord(c, shift));
    }
}

uint32_t DodShift(Memory& m, uint32_t obj) { return (GetS16(m, m.PeekWord(obj) + 14u) & 0xFFFF) >> 12; }

// A bike of pool 0 bound to a kind-2 model, or 0.
uint32_t BoundBike(CaseContext& c) {
    Memory& m = *c.mem;
    for (int tries = 0; tries < 40; ++tries) {
        const uint32_t b = m.PeekWord(kPoolBasePtr) + 1096u * (Chance(c, 3) ? 0u : c.rng->U32() % 18u);
        const uint32_t dod = m.PeekWord(b);
        if (dod < 0x80000000u || dod >= 0x80200000u) continue;
        if (((static_cast<uint32_t>(GetS16(m, dod + 14u)) & 0x78u) >> 3) != 2u) continue;
        return b;
    }
    return 0;
}

// True when class `cls` names a light primitive (and vertices) this bike's model has.
bool ClassFits(Memory& m, uint32_t bike, uint32_t cls) {
    const uint32_t row = s::kFxLightClass + 6u * cls;
    const int32_t prim = static_cast<int8_t>(m.PeekByte(row));
    const int32_t set = static_cast<int8_t>(m.PeekByte(row + 1u));
    const uint32_t dod = m.PeekWord(bike);
    const uint32_t groups = static_cast<uint32_t>(GetS16(m, dod + 24u)) & 0xFFFFu;
    if (groups == 0 || prim < 0 || set < 0 || set > 13) return false;
    const uint32_t group = m.PeekWord(m.PeekWord(bike + 4u) + 24u * groups - 24u);
    if (group < 0x80000000u || group >= 0x80200000u) return false;
    const uint32_t list = m.PeekWord(group + 20u);
    if (list < 0x80000000u || list >= 0x80200000u) return false;
    if (static_cast<int32_t>(m.PeekWord(list)) <= prim) return false;
    const uint32_t rec = list + 20u * static_cast<uint32_t>(prim) + 4u;
    const uint32_t pset = s::kFxPartSets + 4u * static_cast<uint32_t>(set);
    const uint32_t idx[3] = {0u, static_cast<uint32_t>(static_cast<int8_t>(m.PeekByte(pset + 2u))),
                             static_cast<uint32_t>(static_cast<int8_t>(m.PeekByte(pset + 3u)))};
    for (uint32_t k : idx)
        if (k > 3u || (static_cast<uint32_t>(GetS16(m, rec + 12u + 2u * k)) & 0xFFFFu) >= 1200u) return false;
    return true;
}

} // namespace fxdrawrows

void AddFxDrawRows(std::vector<Row>& rows) {
    using namespace fxdrawrows;

    rows.push_back(fxrows::FxRow(
        "fxdraw_weapon_glow", "void WeaponGlow(Object *weapon)  [SLUS 0x80027B80]", s::kFxWeaponGlowFn,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            const uint32_t obj = mdrows::kWeaponObjs + mdrows::kWeaponBytes * (c.rng->U32() % 8u);
            PutByte(m, obj + 8u, Chance(c, 8) ? c.rng->U32() % 10u : 6u + c.rng->U32() % 3u);
            PlantBuffers(c, DodShift(m, obj));
            PlantRanges(c);
            a.a[0] = obj;
        },
        [](NativeEnv& env, const Args& a) {
            return fxrows::Run(env, [&](s::GuestRam& g, s::FxEnv& fe) {
                s::WeaponGlow(g, fe, a.a[0]);
                return 0u;
            });
        },
        false));

    rows.push_back(fxrows::FxRow(
        "fxdraw_bike_light", "void BikeLight(Bike *e)  [SLUS 0x80028534]", s::kFxBikeLightFn,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            // a bound bike whose class (its own, or one of the 21 the table holds) names a light primitive of
            // its model: the emitter reaches BikeLight only for such a bike (the class picks the model)
            uint32_t bike = 0;
            for (int tries = 0; tries < 64 && bike == 0; ++tries) {
                const uint32_t b = BoundBike(c);
                if (b == 0) continue;
                const uint32_t cls = Chance(c, 2) ? m.PeekWord(b + 0xB4u) : c.rng->U32() % 21u;
                if (cls > 20u || !ClassFits(m, b, cls)) continue;
                m.PokeWord(b + 0xB4u, cls);
                bike = b;
            }
            PlantBuffers(c, bike != 0 ? DodShift(m, bike) : 0u);
            PlantRanges(c);
            a.a[0] = bike;
        },
        [](NativeEnv& env, const Args& a) {
            return fxrows::Run(env, [&](s::GuestRam& g, s::FxEnv& fe) {
                s::BikeLight(g, fe, a.a[0]);
                return 0u;
            });
        },
        false));
}
