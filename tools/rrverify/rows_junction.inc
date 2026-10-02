// Bench rows of the junction margin and the camera aim. Included by verify_physics.cpp at its end,
// inside its anonymous
// namespace, after rows_camera.inc and rows_recover_air.inc (their helpers are used here).
//
//   junction_margin_native  SLUS 0x8003E338 JunctionMargin with its callee NodeWedge SLUS 0x8003EB58 run
//                           natively (src\game\sim\junction.h) - the composition the product's rider pass
//                           pool loop runs at 0x8007BAD0. Same inputs as `junction_margin` (a bike or rider
//                           placed on a junction core, on a road slice, without an object, dead; junk under
//                           the stack), no oracle callee.
//   camera_aim_stale        RASHCDG 0x80087420 CameraAim in the state the race reaches and `camera_aim`
//                           keeps out: the lead on (+0x224 bit 28) with the bike's
//                           +0x238 bit 9 (s6) and the lead factor +0x460 already 0, so the bike point
//                           sp+40..+51 is never written and the original blends whatever the stack holds -
//                           with the weight +0x248 at 1.0 (the rider off the bike: s5 > 0), where that
//                           term is 0, or with the blend skipped (+0x224 & 0x110000 == 0x100000). The bike
//                           point is raised (kind 6..8 / 15..17) in a third of the cases.

namespace junctionrows {

uint32_t NativeJunctionMarginWhole(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    uint32_t v0 = 0;
    const bool ok = rr::sim::JunctionMarginNative(g, a.a[0], env.sp, t, v0);
    return rcv::Done(env, g, ok, "JunctionMarginNative", v0);
}

void SetupAimStale(CaseContext& c, Args& args) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t v = camrows::PlantView(c, false);
    const uint32_t bike = m.PeekWord(v + 0x238);
    const uint32_t rider = m.PeekWord(bike + 0x354);
    uint32_t f = m.PeekWord(v + 0x224);
    f |= 0x10000000u;                                   // the lead on
    f &= ~(0x400000u | 0x8000u);                        // no fixed look point, not s8
    f = (f & ~0x110000u) | (camrows::Chance(r, 4) ? 0x100000u : camrows::Bits(r, 0x110000u, 2));
    m.PokeWord(v + 0x224, f);
    m.PokeWord(v + 0x460, 0);                           // the factor the previous frame cleared (0x8008796C)
    m.PokeWord(v + 0x248, 0x10000u);                    // the blend fully on the rider point
    uint32_t b238 = m.PeekWord(bike + 0x238);
    b238 = (b238 | 0x200u) & ~(0x400u | 0x100u);        // s6; not w96, not w88
    m.PokeWord(bike + 0x238, b238);
    m.PokeWord(bike + 0x234, m.PeekWord(bike + 0x234) & ~0x8800u);
    if (camrows::Chance(r, 3)) {                        // the raised bike point
        static const uint32_t kKinds[] = {6, 7, 8, 15, 16, 17};
        m.PokeWord(bike + 0xB4, kKinds[camrows::Pick(r, 6)]);
    }
    if (rider >= 0x80000000u && rider < 0x80200000u) {
        m.PokeWord(rider + 0x228, m.PeekWord(rider + 0x228) & ~0x40000000u);   // not flying
        m.PokeWord(rider + 0x25C, 3u + camrows::Pick(r, 3));                   // off the bike: not w92
    }
    args.a[0] = v;
    args.a[1] = static_cast<uint32_t>(camrows::CaseDt(c));
}

} // namespace junctionrows

void AddJunctionRows(std::vector<Row>& rows) {
    rows.push_back(Row{"junction_margin_native",
                       "Record *0x8003E338(Entity *e)  [SLUS, with NodeWedge 0x8003EB58 native: the pool loop's]",
                       0x8003E338, airrows::SetupJunctionMargin, nullptr, /*compareV0=*/true,
                       junctionrows::NativeJunctionMarginWhole, {}});
    rows.push_back(camrows::CamRow(
        "camera_aim_stale", "void CameraAim(View *v, s32 dt)  [RASHCDG 0x80087420, the stale bike point at weight 0]",
        camrows::kCamAim, junctionrows::SetupAimStale, [](NativeEnv& env, const Args& a) {
            return camrows::Run(env, [&](rr::sim::CameraPort& p) {
                p.Aim(a.a[0], static_cast<int32_t>(a.a[1]), env.sp);
            });
        }));
}
