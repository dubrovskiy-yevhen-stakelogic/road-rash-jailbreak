// Bench rows of recover_walk.h (see rows_recover.inc): the walk back to the bike (AI op 18) and the
// animated re-seat. One row per function:
//
//   angle_between   RASHCDG 0x80093BE0   a leaf (DotLcm, Asin native)
//   axis_rotation   SLUS    0x8003FB34   a leaf (FixMul native)
//   vec_mat         SLUS    0x8002EFF4   a leaf
//   walk_target     RASHCDG 0x80098A50   seam: ReSeat
//   walk_pivot      RASHCDG 0x80098F2C   no seam
//   walk_turn       RASHCDG 0x80099710   seam: ReSeat
//   walk_step       RASHCDG 0x8009989C   no seam
//   rider_recover   RASHCDG 0x80092E04   seams: ClipDone, RouteBind, ReFace, RoadPosition, ResetBikeState,
//                                        RoadsideZones, EndRace, CameraSpringReset, ReSeat, WalkTarget,
//                                        Remount, WalkPivot, WalkTurn, WalkStep, 0x800CA05C
//   reseat          RASHCDG 0x8009277C   seams: ReleaseRiderObject, FreeFarRider, ViewSlot, Remount, Attach,
//                                        StanceEvent, ClimbDone, BankSwitch, the voice, CamTarget
//   climb_done      RASHCDG 0x80092AD4   seams: AiPopCommand, ResetBikeState, ReFace
//   re_face         RASHCDG 0x80096564   seam: RoadPosition
//   free_far_rider  RASHCDG 0x8008CC94   seam: PoolRelease
//
// INPUT FAMILIES start from THIS machine (rr-race: the player's bike slot 0 live on the road, one player,
// 18 bikes and riders, 21 of 23 rider animation objects in use, *(0x8005B254) = 1). Riders and bikes are
// this machine's own records; the fields each function branches on are driven (see each setup).

namespace rcvw {

using pop::Chance;
using pop::Put8;
using pop::U;

constexpr uint32_t kStance = rr::sim::kRcStanceTable;
constexpr uint32_t kGsPtr = rr::sim::kRcGameStatePtr;

uint32_t Bike(const Memory& m, uint32_t k) { return pop::Bike(m, k); }
uint32_t RiderOf(const Memory& m, uint32_t b) { return m.PeekWord(b + 852u); }
uint32_t Gs(const Memory& m) { return m.PeekWord(kGsPtr); }
uint16_t Kind(const Memory& m, uint32_t st) {
    return static_cast<uint16_t>(m.PeekByte(kStance + 8u * st + 2u) | (m.PeekByte(kStance + 8u * st + 3u) << 8));
}
// A stance whose table kind is (or is not) `kind`; resident = its clip's bank is loaded in this machine.
uint32_t StanceWith(CaseContext& c, bool is, uint16_t kind, bool resident) {
    for (int t = 0; t < 400; ++t) {
        const uint32_t st = c.rng->U32() % 224u;
        if ((Kind(*c.mem, st) == kind) != is) continue;
        if (resident && !animrows::Resident(*c.mem, st)) continue;
        return st;
    }
    return is ? 0u : 1u;
}
int16_t Unitish(Rng& r) {
    static const int16_t kExt[6] = {4096, -4096, 0, 1, -1, 2896};
    return Chance(r, 5) ? kExt[r.U32() % 6u] : static_cast<int16_t>(static_cast<int32_t>(r.U32() % 8193u) - 4096);
}
void PutRow(Memory& m, uint32_t at, Rng& r) {
    for (uint32_t k = 0; k < 3; ++k) PutS16(m, at + 2u * k, Unitish(r));
}
// A real unit row of this machine: one of the bike rows, the rider rows or a slice row.
void PutRealRow(Memory& m, uint32_t at, Rng& r) {
    const uint32_t b = Bike(m, r.U32() % 18u);
    static const uint32_t kRows[5] = {0x1B0, 0x1B6, 0x1BC, 0x204, 0x20A};
    const uint32_t src = Chance(r, 6) ? b + 0x210u : b + kRows[r.U32() % 5u];
    for (uint32_t k = 0; k < 3; ++k) PutS16(m, at + 2u * k, GetS16(m, src + 2u * k));
    if (Chance(r, 3))
        for (uint32_t k = 0; k < 3; ++k) PutS16(m, at + 2u * k, static_cast<int16_t>(-GetS16(m, at + 2u * k)));
}
void PutRowAny(Memory& m, uint32_t at, Rng& r) {
    if (Chance(r, 2)) PutRealRow(m, at, r);
    else PutRow(m, at, r);
}
// A 16.16 offset of magnitude around 2^sh units (both signs).
int32_t Off(Rng& r, int sh) {
    const int32_t v = static_cast<int32_t>(r.U32() >> (32 - sh - 16 - 1));
    return Chance(r, 2) ? v : -v;
}
void PutPos(Memory& m, uint32_t at, uint32_t near, Rng& r, int sh) {
    for (uint32_t k = 0; k < 3; ++k) m.PokeWord(at + 4u * k, m.PeekWord(near + 4u * k) + static_cast<uint32_t>(Off(r, sh)));
}
// The rider carries its bike's road state (what RiderSync 0x8008DF74 copies at the launch): the cursor
// +0x148..+0x167, the road coordinate +0x168..+0x173, +0x174..+0x1AB, the route object +0x1AC, the
// progress +0x144, the roadside run +0x1EC / +0x1F0, and the bike's +0x184 road flags.
void GiveRoad(Memory& m, uint32_t R, uint32_t B) {
    uint8_t buf[0x1B0 - 0x144];
    m.ReadBlock(B + 0x144u, buf, sizeof(buf));
    m.WriteBlock(R + 0x144u, buf, sizeof(buf));
    m.PokeWord(R + 0x1ECu, m.PeekWord(B + 0x1ECu));
    m.PokeWord(R + 0x1F0u, m.PeekWord(B + 0x1F0u));
    m.PokeWord(R + 0x184u, m.PeekWord(B + 0x184u));
}
// The bikes whose road state is live (a real cursor): this machine's live bikes.
uint32_t LiveBike(CaseContext& c) {
    for (int t = 0; t < 64; ++t) {
        const uint32_t b = Bike(*c.mem, c.rng->U32() % 18u);
        if (GetS16(*c.mem, b + 0x140u) != 0 && c.mem->PeekWord(b + 0x154u) >= 0x80000000u) return b;
    }
    return Bike(*c.mem, 0);
}

// ---------------------------------------------------------------- 0x80093BE0
void SetupAngleBetween(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t v = c.scratch, pa = c.scratch + 8u, pb = c.scratch + 16u;
    PutRowAny(m, v, r);
    PutRowAny(m, pa, r);
    PutRowAny(m, pb, r);
    if (Chance(r, 4)) m.WriteBlock(v, std::vector<uint8_t>(6, 0).data(), 6);
    if (Chance(r, 5)) for (uint32_t k = 0; k < 6; k += 2) WriteU16Bench(m, pa + k, static_cast<uint16_t>(GetS16(m, v + k)));
    if (Chance(r, 5)) for (uint32_t k = 0; k < 6; k += 2) WriteU16Bench(m, pb + k, static_cast<uint16_t>(GetS16(m, v + k)));
    a.a[0] = v;
    a.a[1] = pa;
    a.a[2] = pb;
}
uint32_t NativeAngleBetween(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    int32_t v0 = 0;
    const bool ok = rr::sim::AngleBetween(g, a.a[0], a.a[1], a.a[2], t, v0);
    return rcv::Done(env, g, ok, "AngleBetween", U(v0));
}

// ---------------------------------------------------------------- SLUS 0x8003FB34 / 0x8002EFF4
void SetupAxisRotation(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    PutRowAny(m, c.scratch, r);
    a.a[0] = c.scratch;
    a.a[1] = Chance(r, 3) ? r.U32() : r.U32() % 4096u;
    a.a[2] = c.scratch + 16u;
}
uint32_t NativeAxisRotation(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    rr::sim::AxisRotation(g, a.a[0], static_cast<int32_t>(a.a[1]), a.a[2], env.sp, t);
    return rcv::Done(env, g, true, "AxisRotation");
}
void SetupVecMat(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    for (uint32_t k = 0; k < 24; k += 2) PutS16(m, c.scratch + k, Chance(r, 3) ? static_cast<int16_t>(r.U32()) : Unitish(r));
    a.a[0] = c.scratch;
    a.a[1] = c.scratch + 6u;
    const uint32_t alias = r.U32() % 5u;
    a.a[2] = alias == 0 ? c.scratch : (alias == 1 ? c.scratch + 6u : c.scratch + 32u);
}
uint32_t NativeVecMat(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    rr::sim::VecMat(g, a.a[0], a.a[1], a.a[2]);
    return rcv::Done(env, g, true, "VecMat");
}

// ---------------------------------------------------------------- the walk: a rider R off bike B
// R = the rider of bike k (the bike its own, R+0x254), its position near B's +0x1F8 (within ~2 units, the
// re-seat reach), mid-range or far (beyond the 90-unit halving limit 0x5A8000); +0x244 the target near R.
struct Walker {
    uint32_t R = 0, B = 0;
};
Walker PlantWalker(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    Walker w;
    w.B = Bike(m, r.U32() % 18u);
    w.R = RiderOf(m, w.B);
    const int sh = static_cast<int>(r.U32() % 4u == 0 ? 8u : (r.U32() % 3u == 0 ? 0u : 2u));
    PutPos(m, w.R + 0xB8u, w.B + 0x1F8u, r, sh);
    PutPos(m, w.R + 0x244u, w.R + 0xB8u, r, static_cast<int>(r.U32() % 9u));
    if (Chance(r, 3)) PutRow(m, w.R + 0x204u, r);
    if (Chance(r, 3)) PutRow(m, w.R + 0x20Au, r);
    if (Chance(r, 3)) PutRow(m, w.R + 0x210u, r);
    m.PokeWord(w.R + 0x228u, m.PeekWord(w.R + 0x228u) ^ (r.U32() & r.U32()));
    return w;
}

// 0x80098A50. Driven: B+0x28C (+-0x1921F lying on a side, or small +-), R+0x228 bits 3/4/11, R+0x1E0,
// the distance (the re-seat reach: octagonal < 6 and Length3 / 2 < 6553), B's rows.
void SetupWalkTarget(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    Walker w = PlantWalker(c);
    const uint32_t k = r.U32() % 4u;
    PutS32(m, w.B + 0x28Cu, k == 0 ? 0x1921F : (k == 1 ? -0x1921F : (Chance(r, 2) ? Off(r, 1) : 0)));
    if (Chance(r, 3)) PutRow(m, w.B + 0x1B6u, r);
    if (Chance(r, 3)) PutRow(m, w.B + 0x1BCu, r);
    uint32_t f = m.PeekWord(w.R + 0x228u);
    f = Chance(r, 3) ? (f & ~0x818u) : f;
    m.PokeWord(w.R + 0x228u, f);
    m.PokeWord(w.R + 0x1E0u, Chance(r, 2) ? 0u : r.U32());
    a.a[0] = w.R;
    a.a[1] = w.B;
    a.a[2] = c.scratch;
    a.a[3] = c.scratch + 8u;
}
uint32_t NativeWalkTarget(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    rcv::RecoverOracle c(env);
    uint32_t v0 = 0;
    const bool ok = rr::sim::WalkTarget(g, a.a[0], a.a[1], a.a[2], a.a[3], env.sp, t, c, v0);
    return rcv::Done(env, g, ok, "WalkTarget", v0);
}

// 0x80098F2C (R, dirA, dirB, sq, flag at sp+16). Driven: the rows, dot(R+0x204, dirA) either side of
// +-655, the two projections' signs, sq, flag.
void SetupWalkPivot(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    Walker w = PlantWalker(c);
    PutRowAny(m, c.scratch, r);
    PutRowAny(m, c.scratch + 8u, r);
    if (Chance(r, 4)) for (uint32_t k = 0; k < 6; k += 2) PutS16(m, c.scratch + k, GetS16(m, w.R + 0x204u + k));
    a.a[0] = w.R;
    a.a[1] = c.scratch;
    a.a[2] = c.scratch + 8u;
    a.a[3] = Chance(r, 3) ? r.U32() : r.U32() >> (r.U32() % 24u);
    a.a4 = Chance(r, 2) ? 0u : (Chance(r, 2) ? 1u : r.U32());
}
uint32_t NativeWalkPivot(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    const bool ok = rr::sim::WalkPivot(g, a.a[0], a.a[1], a.a[2], static_cast<int32_t>(a.a[3]),
                                       static_cast<int32_t>(a.a4), env.sp, t);
    return rcv::Done(env, g, ok, "WalkPivot");
}

// 0x80099710 (R, dirA, dirB, dt). Driven: R+0x210 lined up with dirA (the re-seat) or not, +0x258 sign,
// +0x1E0, +0x250, dt.
void SetupWalkTurn(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    Walker w = PlantWalker(c);
    PutRowAny(m, c.scratch, r);
    PutRowAny(m, c.scratch + 8u, r);
    if (Chance(r, 3)) for (uint32_t k = 0; k < 6; k += 2) PutS16(m, w.R + 0x210u + k, GetS16(m, c.scratch + k));
    PutS32(m, w.R + 0x258u, Chance(r, 5) ? 0 : Off(r, static_cast<int>(r.U32() % 6u)));
    PutS32(m, w.R + 0x250u, Chance(r, 5) ? 0 : Off(r, static_cast<int>(r.U32() % 6u)));
    m.PokeWord(w.R + 0x1E0u, Chance(r, 2) ? 0u : 0x80000u);
    a.a[0] = w.R;
    a.a[1] = c.scratch;
    a.a[2] = c.scratch + 8u;
    a.a[3] = Chance(r, 2) ? 1090u : r.U32() % 0x8000u;
}
uint32_t NativeWalkTurn(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    rcv::RecoverOracle c(env);
    const bool ok = rr::sim::WalkTurn(g, a.a[0], a.a[1], a.a[2], static_cast<int32_t>(a.a[3]), env.sp, t, c);
    return rcv::Done(env, g, ok, "WalkTurn");
}

// 0x8009989C (R, B, d, sq, flag at sp+16, dt at sp+20). Driven: d (the target minus R, near or beyond the
// halving limit), flag, sq (the SqrtGte arm), R+0x1E0, +0x1E8, the rows (the turn within reach or not).
void SetupWalkStep(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    Walker w = PlantWalker(c);
    const uint32_t d = c.scratch;
    for (uint32_t k = 0; k < 3; ++k)
        PutS32(m, d + 4u * k, static_cast<int32_t>(m.PeekWord(w.R + 0x244u + 4u * k) - m.PeekWord(w.R + 0xB8u + 4u * k)));
    if (Chance(r, 4)) for (uint32_t k = 0; k < 3; ++k) PutS32(m, d + 4u * k, Off(r, static_cast<int>(r.U32() % 12u)));
    m.PokeWord(w.R + 0x1E0u, Chance(r, 2) ? 0u : r.U32());
    m.PokeWord(w.R + 0x1E8u, Chance(r, 3) ? 0u : static_cast<uint32_t>(Off(r, 2)));
    // NAMED BOUND: sq and flag are what the one caller (0x80092E04 at 0x80093B08) hands over: flag 0 with
    // sq = SumSquares(d) when every |d[i]| <= 0x5A8000, else flag 1 (sq 0x7FFF0000, unread). A drawn sq
    // unrelated to d scales d far past unit length and the ORIGINAL then trapped in Normalize's add
    // (0x8002E4A4, the console's overflow exception) in 7 of 1160 cases.
    int32_t dv[3];
    bool big = false;
    for (uint32_t k = 0; k < 3; ++k) {
        dv[k] = GetS32(m, d + 4u * k);
        const int32_t ad = dv[k] < 0 ? -dv[k] : dv[k];
        if (dv[k] != INT32_MIN && ad > 0x5A8000) big = true; // the caller's test (Iabs(INT32_MIN) < 0)
    }
    a.a[0] = w.R;
    a.a[1] = w.B;
    a.a[2] = d;
    a.a[3] = big ? 0x7FFF0000u : static_cast<uint32_t>(rr::sim::SumSquares(dv));
    a.a4 = big ? 1u : 0u;
    a.more[0] = Chance(r, 2) ? 1090u : r.U32() % 0x8000u;
}
uint32_t NativeWalkStep(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    const bool ok = rr::sim::WalkStep(g, a.a[0], a.a[1], a.a[2], static_cast<int32_t>(a.a[3]),
                                      static_cast<int32_t>(a.a4), static_cast<int32_t>(a.more[0]), env.sp, t);
    return rcv::Done(env, g, ok, "WalkStep");
}

// ---------------------------------------------------------------- 0x80092E04 (e, dt)
// Driven: the phase gs+0x39 (2: the Jailbreak timer at the top command's +4, player or not, player 1's
// riderDef +0x28/+0x27 and its rider's distance), the rider's live word, R+0x228 bit 12, the partner
// (e+0x358 / +0x440, the passenger bit R+0x23C bit 5 -> e := (R+0x254)+0x358), *(0x8005B220), a player's
// off-road test (e+0x140, +0x184 bit 0, +0x238 bit 8, the lateral against the slice), the distance
// R-e either side of 200, e+0x230 bits 25/27, the stance (kind 8, 69/70 with the clip done or not,
// anything else), riderDef +0x27 / +0x25 / +0x0E (a player's finished arm), R+0x228 bit 20 and +0x258,
// bits 3/4/11, e+0x1E0 / +0x1E8, the target +0x244 near / far / beyond the halving limit.
//
// NAMED BOUND 1: the downed rider carries a road cursor (GiveRoad: RiderSync 0x8008DF74 copies its
// bike's at the launch); this machine's seated riders hold none and the bike-back arm walks the rider's
// slice +0x154 and hands its cursor to RouteBind / RoadPosition.
// NAMED BOUND 2: the command-stack depth e+0x3B2 is 0..15 (the Jailbreak arm indexes the top slot).
void SetupRiderRecover(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t gs = Gs(m);
    pop::DrawPlayers(c);
    uint32_t e = Bike(m, Chance(r, 3) ? 0u : r.U32() % 18u);
    const uint32_t R = RiderOf(m, e);
    const uint32_t src = LiveBike(c);
    GiveRoad(m, R, src);
    WriteU16Bench(m, R + 0x140u, Chance(r, 8) ? 0u : 1u);
    // the passenger arm
    Put8(m, R + 0x23Cu, Chance(r, 6) ? (m.PeekByte(R + 0x23Cu) | 0x20u) : (m.PeekByte(R + 0x23Cu) & ~0x20u));
    const uint32_t other = Bike(m, r.U32() % 18u);
    if (m.PeekByte(R + 0x23Cu) & 0x20u) m.PokeWord(m.PeekWord(R + 0x254u) + 0x358u, other);
    const uint32_t s1 = (m.PeekByte(R + 0x23Cu) & 0x20u) ? other : e;
    if (Chance(r, 5)) {
        m.PokeWord(s1 + 0x358u, Bike(m, r.U32() % 18u));
        m.PokeWord(s1 + 0x440u, Chance(r, 2) ? 1u : 0u);
    }
    // the phase
    Put8(m, gs + 0x39u, Chance(r, 5) ? 2u : (Chance(r, 3) ? 1u : 0u));
    Put8(m, s1 + 0x3B2u, r.U32() % 4u);
    const uint32_t slot = s1 + 948u + 8u * m.PeekByte(s1 + 0x3B2u);
    WriteU16Bench(m, slot + 4u, Chance(r, 2) ? 0u : static_cast<uint16_t>(r.U32() % 40u));
    const uint32_t p1 = m.PeekWord(rr::sim::kRcPlayer1Bike);
    if (Chance(r, 3)) m.PokeWord(m.PeekWord(p1 + 1084u) + 40u, Chance(r, 2) ? 1u : 0u);
    m.PokeWord(0x8005B220u, Chance(r, 8) ? 1u : 0u);
    // the flags and the geometry
    uint32_t f = m.PeekWord(R + 0x228u) & ~0x00101818u;
    if (Chance(r, 6)) f |= 0x1000u;
    if (Chance(r, 3)) f |= 0x00100000u;
    if (Chance(r, 3)) f |= (r.U32() % 4u) << 3;
    if (Chance(r, 5)) f |= 0x800u;
    m.PokeWord(R + 0x228u, f);
    PutS32(m, R + 0x258u, Chance(r, 2) ? static_cast<int32_t>(r.U32() % 3000u) : -static_cast<int32_t>(r.U32() % 100u));
    const int sh = static_cast<int>(Chance(r, 4) ? 8u + r.U32() % 2u : r.U32() % 6u);
    PutPos(m, R + 0xB8u, s1 + 0xB8u, r, sh);
    PutPos(m, R + 0x244u, R + 0xB8u, r, static_cast<int>(Chance(r, 5) ? 7u : r.U32() % 4u));
    WriteU16Bench(m, s1 + 0x140u, Chance(r, 6) ? 0u : 1u);
    m.PokeWord(s1 + 0x184u, m.PeekWord(s1 + 0x184u) ^ (Chance(r, 3) ? 1u : 0u));
    m.PokeWord(s1 + 0x238u, m.PeekWord(s1 + 0x238u) ^ (Chance(r, 3) ? 0x100u : 0u));
    m.PokeWord(s1 + 0x230u, m.PeekWord(s1 + 0x230u) ^ (Chance(r, 3) ? 0x08000000u : 0u) ^ (Chance(r, 3) ? 0x02000000u : 0u));
    m.PokeWord(s1 + 0x1E0u, Chance(r, 2) ? 0u : r.U32());
    m.PokeWord(s1 + 0x1E8u, Chance(r, 2) ? 0u : r.U32());
    // the stance
    const uint32_t k = r.U32() % 4u;
    const uint32_t st = k == 0 ? StanceWith(c, false, 8, true) : (k == 1 ? 69u + r.U32() % 2u : StanceWith(c, true, 8, true));
    WriteU16Bench(m, R + 0x220u, static_cast<uint16_t>(st));
    // the riderDef
    const uint32_t rd = m.PeekWord(s1 + 1084u);
    Put8(m, rd + 0x27u, Chance(r, 6) ? 247u + r.U32() % 9u : r.U32() % 247u);
    Put8(m, rd + 0x25u, Chance(r, 3) ? 0u : 1u);
    Put8(m, rd + 0x0Eu, Chance(r, 3) ? 0u : 1u);
    a.a[0] = e;
    a.a[1] = Chance(r, 2) ? 1090u : r.U32() % 0x8000u;
}
std::vector<OracleCallee> RiderRecoverSeams() {
    return {OracleCallee{rr::sim::kRcJailbreakFn, 1}, OracleCallee{0x8005BE58, 1},
            OracleCallee{rr::sim::kRcRouteBindFn, 3},   OracleCallee{rr::sim::kReFaceFn, 1},
            OracleCallee{rr::sim::kRcRoadPositionFn, 3}, OracleCallee{rr::sim::kRcResetBikeFn, 1},
            OracleCallee{rr::sim::kRcZonesFn, 1},       OracleCallee{rr::sim::kRcEndRaceFn, 2},
            OracleCallee{rr::sim::kRcSpringResetFn, 1}, OracleCallee{rr::sim::kReSeatFn, 1},
            OracleCallee{rr::sim::kWalkTargetFn, 4},    OracleCallee{rr::sim::kRcRemountFn, 2},
            OracleCallee{rr::sim::kWalkPivotFn, 4, 1},  OracleCallee{rr::sim::kWalkTurnFn, 4},
            OracleCallee{rr::sim::kWalkStepFn, 4, 2}};
}
uint32_t NativeRiderRecover(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    rcv::RecoverOracle c(env);
    const bool ok = rr::sim::RiderRecover(g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp, t, c);
    return rcv::Done(env, g, ok, "RiderRecover");
}

// ---------------------------------------------------------------- 0x8009277C (B)
// Driven: gs+0x39 (< 2, 1, >= 2 with the rider's object index +0x23B = -1 or not), *(0x8005B254), the
// handle (a player or not) and B+0x140 bit 2, the descriptor's used count against its capacity (full ->
// FreeFarRider, and then no slot -> Remount), B+0x21C (0 or one of the free objects), the two-rider arm
// (B+0x358 / +0x440, the rider's +0x23C bit 4), B+0x140, B+0x28C.
// NAMED BOUND: the climb stances 0 / 4 / 77 the event is asked for are resident in this machine (checked:
// the row fails loudly otherwise, rows_anim.inc's bound).
void SetupReSeat(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t gs = Gs(m);
    pop::DrawPlayers(c);
    const uint32_t B = Bike(m, Chance(r, 3) ? 0u : r.U32() % 18u);
    const uint32_t R = RiderOf(m, B);
    Put8(m, gs + 0x39u, Chance(r, 4) ? 2u + r.U32() % 2u : (Chance(r, 3) ? 1u : 0u));
    if (Chance(r, 3)) Put8(m, R + 0x23Bu, Chance(r, 2) ? 0xFFu : r.U32() % 8u);
    m.PokeWord(0x8005B254u, Chance(r, 3) ? 0u : 1u);
    WriteU16Bench(m, B + 0x140u, static_cast<uint16_t>(Chance(r, 4) ? 0u : (Chance(r, 3) ? 5u : 1u)));
    const uint32_t desc = rr::sim::kRcAnimDesc;
    const uint32_t cap = m.PeekWord(desc + 12u);
    if (Chance(r, 4)) m.PokeWord(desc + 8u, cap);                       // full: FreeFarRider, maybe no slot
    m.PokeWord(B + 0x21Cu, 0);
    if (Chance(r, 5)) {
        m.PokeWord(B + 0x358u, Bike(m, r.U32() % 18u));
        m.PokeWord(B + 0x440u, 1);
        Put8(m, R + 0x23Cu, Chance(r, 2) ? (m.PeekByte(R + 0x23Cu) | 0x10u) : (m.PeekByte(R + 0x23Cu) & ~0x10u));
        // NAMED BOUND: the passenger's climb, event 77, is stance 77 of bank 6 (the passenger clips), which
        // this machine does not hold resident (no two-up bike in rr-race): the ORIGINAL trapped in HardStart
        // (0x8005BF8C) in 56 of 1160 cases. The passenger's arm is taken only where stance 77 is resident;
        // elsewhere gs+0x39 = 1 skips it (the arm's own test), so its lines are transcription only here.
        if ((m.PeekByte(R + 0x23Cu) & 0x10u) && !animrows::Resident(m, 77)) Put8(m, gs + 0x39u, 1);
    } else if (Chance(r, 2)) {
        m.PokeWord(B + 0x440u, 0);
    }
    const uint32_t kk = r.U32() % 4u;
    PutS32(m, B + 0x28Cu, kk == 0 ? 0x1921F : (kk == 1 ? -0x1921F : Off(r, 1)));
    a.a[0] = B;
}
std::vector<OracleCallee> ReSeatSeams() {
    return {OracleCallee{rr::sim::kRcReleaseObjFn, 1}, OracleCallee{rr::sim::kFreeFarRiderFn, 0},
            OracleCallee{rr::sim::kRcViewSlotFn, 2},   OracleCallee{rr::sim::kRcRemountFn, 2},
            OracleCallee{rr::sim::kRcAttachFn, 4},     OracleCallee{rr::sim::kRcStanceEventFn, 3},
            OracleCallee{rr::sim::kClimbDoneFn, 1},    OracleCallee{rr::sim::kRcBankSwitchFn, 2},
            OracleCallee{rr::sim::kRcVoiceFn, 2},      OracleCallee{rr::sim::kRcCamTargetFn, 2}};
}
uint32_t NativeReSeat(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    rcv::RecoverOracle c(env);
    const bool ok = rr::sim::ReSeat(g, a.a[0], env.sp, c);
    return rcv::Done(env, g, ok, "ReSeat");
}

// ---------------------------------------------------------------- 0x80092AD4 (B)
// Driven: the rider's stance (kind 0 or not, stance 0), the handle (a player or not), B's distance to
// view record 0's eye x / z (0x800CD950 / 0x800CD958) either side of 120.0, B+0x21C (0 or an object).
void SetupClimbDone(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    pop::DrawPlayers(c);
    const uint32_t B = LiveBike(c);
    const uint32_t R = RiderOf(m, B);
    const uint32_t k = r.U32() % 4u;
    const uint32_t st = k == 0 ? 0u : (k == 1 ? StanceWith(c, false, 0, false) : StanceWith(c, true, 0, false));
    WriteU16Bench(m, R + 0x220u, static_cast<uint16_t>(st));
    for (uint32_t w : {0x800CD950u, 0x800CD958u})
        PutS32(m, w, GetS32(m, B + 0xB8u + (w - 0x800CD950u)) + Off(r, static_cast<int>(r.U32() % 9u)));
    m.PokeWord(B + 0x21Cu, Chance(r, 2) ? 0u : m.PeekWord(R + 0x21Cu));
    Put8(m, B + 0x3B2u, 1u + r.U32() % 3u);
    a.a[0] = B;
}
std::vector<OracleCallee> ClimbDoneSeams() {
    return {OracleCallee{rr::sim::kRcPopCommandFn, 1}, OracleCallee{rr::sim::kRcResetBikeFn, 1},
            OracleCallee{rr::sim::kReFaceFn, 1}};
}
uint32_t NativeClimbDone(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    rcv::RecoverOracle c(env);
    const bool ok = rr::sim::ClimbDone(g, a.a[0], env.sp, t, c);
    return rcv::Done(env, g, ok, "ClimbDone");
}

// ---------------------------------------------------------------- 0x80096564 (B)
// Driven: the route binding (B+0x1AC kept / 0, the road key's kind +0x16A), flagsC bit 22, the rider's
// mount state (>= 3: the rider's road state is read - GiveRoad), the facing +0x210 (the dot's sign).
// The junction arm (+0x168 kind 1) reaches the node record of the cursor object's id: drawn from this
// machine's resident objects when it is a junction, otherwise the arm's lookups answer 0.
void SetupReFace(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t B = LiveBike(c);
    const uint32_t R = RiderOf(m, B);
    if (Chance(r, 4)) m.PokeWord(B + 0x1ACu, 0);
    m.PokeWord(B + 0x238u, m.PeekWord(B + 0x238u) ^ (Chance(r, 2) ? 0x00400000u : 0u));
    m.PokeWord(R + 0x25Cu, r.U32() % 5u);
    if (m.PeekWord(R + 0x25Cu) >= 3u) GiveRoad(m, R, LiveBike(c));
    if (Chance(r, 2))
        for (uint32_t k = 0; k < 6; k += 2) PutS16(m, B + 0x210u + k, static_cast<int16_t>(-GetS16(m, B + 0x210u + k)));
    if (Chance(r, 4)) {
        const uint32_t s = m.PeekWord(R + 0x25Cu) < 3u ? B : R;
        WriteU16Bench(m, s + 0x16Au, 1);                                 // the key's kind: a node
    }
    a.a[0] = B;
}
uint32_t NativeReFace(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    rcv::RecoverOracle c(env);
    const bool ok = rr::sim::ReFace(g, a.a[0], env.sp, t, c);
    return rcv::Done(env, g, ok, "ReFace");
}

// ---------------------------------------------------------------- 0x8008CC94 ()
// Pool 2 (0x800CE4F0) walked: the handles (0 = empty), +0x144 against player 1's, +9's low bits; the
// high index (-1 = empty pool).
void SetupFreeFarRider(CaseContext& c, Args&) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t pool = 0x800CE4F0u;
    const uint32_t base = m.PeekWord(pool), stride = m.PeekWord(pool + 4u), hi = m.PeekWord(pool + 12u);
    const int32_t n = static_cast<int32_t>(m.PeekWord(hi));
    if (Chance(r, 6)) m.PokeWord(hi, 0xFFFFFFFFu);
    for (int32_t i = 0; i <= n && i < 64; ++i) {
        const uint32_t e = base + stride * static_cast<uint32_t>(i);
        if (Chance(r, 3)) WriteU16Bench(m, e + 0xACu, Chance(r, 2) ? 0u : static_cast<uint16_t>(0x40u + i));
        if (Chance(r, 2)) PutS32(m, e + 0x144u, static_cast<int32_t>(r.U32()) >> (r.U32() % 16u));
        if (Chance(r, 3)) Put8(m, e + 9u, r.U32() & 0xFFu);
    }
}
uint32_t NativeFreeFarRider(NativeEnv& env, const Args&) {
    rr::sim::GuestRam g = rcv::View(env);
    rcv::RecoverOracle c(env);
    uint32_t v0 = 0;
    const bool ok = rr::sim::FreeFarRider(g, env.sp, c, v0);
    return rcv::Done(env, g, ok, "FreeFarRider", v0);
}

} // namespace rcvw

void AddRecoverWalkRows(std::vector<Row>& rows) {
    using namespace rcvw;
    auto add = [&rows](Row r, uint64_t steps) {
        r.maxSteps = steps;
        rows.push_back(r);
    };
    add(Row{"angle_between", "s32 AngleBetween(s16 *v, s16 *a, s16 *b)  [RASHCDG 0x80093BE0]", rr::sim::kAngleBetweenFn,
            SetupAngleBetween, nullptr, true, NativeAngleBetween},
        5'000'000);
    add(Row{"axis_rotation", "void AxisRotation(s16 *axis, s32 ang, s16 *m)  [SLUS 0x8003FB34]", rr::sim::kAxisRotationFn,
            SetupAxisRotation, nullptr, false, NativeAxisRotation},
        5'000'000);
    add(Row{"vec_mat", "void VecMat(s16 *v, s16 *m, s16 *out)  [SLUS 0x8002EFF4]", rr::sim::kVecMatFn, SetupVecMat, nullptr,
            false, NativeVecMat},
        5'000'000);
    add(Row{"walk_target", "u32 WalkTarget(Rider *R, Bike *B, s16 *a, s16 *b)  [RASHCDG 0x80098A50]",
            rr::sim::kWalkTargetFn, SetupWalkTarget, nullptr, true, NativeWalkTarget,
            {OracleCallee{rr::sim::kReSeatFn, 1}}},
        50'000'000);
    add(Row{"walk_pivot", "void WalkPivot(Rider *R, s16 *a, s16 *b, s32 sq, s32 flag)  [RASHCDG 0x80098F2C]",
            rr::sim::kWalkPivotFn, SetupWalkPivot, nullptr, false, NativeWalkPivot},
        5'000'000);
    add(Row{"walk_turn", "void WalkTurn(Rider *R, s16 *a, s16 *b, s32 dt)  [RASHCDG 0x80099710]", rr::sim::kWalkTurnFn,
            SetupWalkTurn, nullptr, false, NativeWalkTurn, {OracleCallee{rr::sim::kReSeatFn, 1}}},
        50'000'000);
    add(Row{"walk_step", "void WalkStep(Rider *R, Bike *B, s32 *d, s32 sq, s32 flag, s32 dt)  [RASHCDG 0x8009989C]",
            rr::sim::kWalkStepFn, SetupWalkStep, nullptr, false, NativeWalkStep},
        5'000'000);
    add(Row{"rider_recover", "void RiderRecover(Bike *e, s32 dt)  [RASHCDG 0x80092E04, AI op 18]", rr::sim::kRiderRecoverFn,
            SetupRiderRecover, nullptr, false, NativeRiderRecover, RiderRecoverSeams()},
        100'000'000);
    add(Row{"reseat", "void ReSeat(Bike *B)  [RASHCDG 0x8009277C]", rr::sim::kReSeatFn, SetupReSeat, nullptr, false,
            NativeReSeat, ReSeatSeams()},
        100'000'000);
    add(Row{"climb_done", "void ClimbDone(Bike *B)  [RASHCDG 0x80092AD4]", rr::sim::kClimbDoneFn, SetupClimbDone, nullptr,
            false, NativeClimbDone, ClimbDoneSeams()},
        50'000'000);
    add(Row{"re_face", "void ReFace(Bike *B)  [RASHCDG 0x80096564]", rr::sim::kReFaceFn, SetupReFace, nullptr, false,
            NativeReFace, {OracleCallee{rr::sim::kRcRoadPositionFn, 3}}},
        50'000'000);
    add(Row{"free_far_rider", "u32 FreeFarRider(void)  [RASHCDG 0x8008CC94]", rr::sim::kFreeFarRiderFn, SetupFreeFarRider,
            nullptr, true, NativeFreeFarRider, {OracleCallee{rr::sim::kRcPoolReleaseFn, 2}}},
        50'000'000);
}
