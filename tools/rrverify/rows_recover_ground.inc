// Bench rows of recover_ground.h (see rows_recover.inc): the rider on the ground. One row per function:
//
//   rider_on_ground     RASHCDG 0x80097BCC   seams: JunctionMargin SLUS 0x8003E338, EffectBurst SLUS
//                                            0x80027778, GroundSlide, GroundGetUp, GroundWalkControl,
//                                            GroundWalkRate, SetOp18
//   rider_slide         RASHCDG 0x8008F754   no seam (leaves only)
//   rider_getup         RASHCDG 0x8008FD5C   seams: ClipDone, RangedStart, SetRiderState, StanceEvent,
//                                            FollowStance
//   rider_walk_control  RASHCDG 0x8009926C   no seam
//   rider_walk_rate     RASHCDG 0x800986D0   seams: ClipDone, StanceEvent
//   set_op18            RASHCDG 0x8009A038   seams: StanceEvent, CameraResetFlags
//
// INPUT FAMILIES start from THIS machine (rr-race: the player alone on a live road, 18 bikes, each
// with its pool-1 rider R = *(bike + 852) and that rider's animation object R+0x21C). The rows with an
// animation seam take their rider from rows_anim.inc's PlantStanceCase (a captured or drawn stance
// state and object), then set the stance the row is about and re-establish the object's bank
// invariant for it. The fields each function branches on are driven (see each setup).
//
// NAMED BOUNDS (each the range the game's own code keeps):
//   1. The requested stances are ground stances 44..73, every one resident in this machine (banks
//      1..5); the object's bank is the stance's bank (rows_anim.inc's invariant), so a clip index a
//      callee reads is read in the bank it belongs to.
//   2. The object's op index +0xC is 0..4 (the program has five ops): 0x8008FF54 reads the clip byte
//      of op +0xC and chases the bank's clip table with it, and an op past the program reads the
//      NEXT object's program, whose clip byte may not be a clip of this bank.
//   3. GroundGetUp runs with the mount state +0x25C = 3: its only caller (0x800980C0) is the mount-3
//      arm, and FollowStance (0x800C4778..0x800C47B4) indexes a 5-entry table with the mount state and
//      gives the non-resident stance 77 for mounts 0/1 with +0x23C bit 5.
//   4. A rider whose +0x23C bit 5 is set (the pillion arm) has a bike +0x358 that is a pool-0 bike: the
//      walk control reads that bike's handle (0x80099310), and every capture holds +0x358 = 0.
//   5. The command-stack depth +0x3B2 is 0..15 (rows_crash.inc remount's bound): SetOp18 writes the
//      command at bike + 0x3B4 + 8 * depth.
//   6. The rider's rows (+0x1B0, +0x1B6, +0x1BC, +0x1C2, +0x204, +0x20A, +0x210, +0x26E) are unit
//      scale (|component| <= 4096): the Normalize of a GTE OP of two of them (SLUS 0x8002E468) raises
//      the console's overflow exception on a sum of squares past 2^31, which a unit row never reaches.
//   7. The velocity +0x1C8 has components below 2^21 in magnitude: Length3's SumSquares then stays a
//      positive 32-bit value (SqrtGte's two trapping `addi`s, ai.h). The speed +0x1E0 is that
//      velocity's length (GroundSlide 0x8008FA68 writes it as Length3 of +0x1C8) and is turned back
//      into it by RiderOnGround (0x80098098: velocity := speed x the unit row +0x1C2), so it is held
//      to the same 2^21: a speed of 2^23 makes that velocity overflow Length3, the reciprocal scale
//      at 0x8008FB04 then leaves +0x1C2 far past unit, and the ORIGINAL's Normalize of the GTE OP at
//      0x8008FC68 raises the overflow exception (0x8002E4A4).

namespace rcvg {

using pop::Chance;
using pop::Put8;

constexpr uint32_t kStance = rr::sim::kRcStanceTable;
constexpr uint32_t kGsPtr = rr::sim::kRcGameStatePtr;
constexpr uint32_t kPlayerRec = 0x800D43C0; // 28 bytes per bike handle (0x80097C5C)
constexpr uint32_t kPadAxes = 0x800CE540;   // {s32, s32} per player (input.h)
constexpr uint32_t kPadRecs = 0x800D7128;   // 192 bytes per player, +16 manual control

uint32_t Gs(const Memory& m) { return m.PeekWord(kGsPtr); }
uint32_t Kind(const Memory& m, uint32_t st) { return animrows::Get16(m, kStance + 8u * st + 2u); }
uint32_t From(Rng& r, std::initializer_list<uint32_t> xs) {
    const uint32_t k = r.U32() % static_cast<uint32_t>(xs.size());
    return *(xs.begin() + k);
}
int16_t Unitish(Rng& r) {
    static const int16_t kExt[6] = {4096, -4096, 0, 1, -1, 2896};
    return Chance(r, 5) ? kExt[r.U32() % 6u] : r.Unit();
}
void PutRow(Memory& m, uint32_t at, Rng& r) {
    for (uint32_t k = 0; k < 3; ++k) PutS16(m, at + 2u * k, Unitish(r));
}
// A speed-like 16.16 scalar: one side of a threshold of the functions here, or drawn.
int32_t Speed(Rng& r, std::initializer_list<int32_t> edges) {
    const uint32_t q = r.U32() % 8u;
    if (q < 4 && edges.size() != 0) {
        const int32_t e = *(edges.begin() + r.U32() % static_cast<uint32_t>(edges.size()));
        return e + static_cast<int32_t>(r.U32() % 3u) - 1;
    }
    if (q == 4) return 0;
    const int32_t v = r.S32() >> (6 + r.U32() % 20u);
    return q == 5 ? -(v < 0 ? -v : v) : (v < 0 ? -v : v);
}
// dt as the rider pass hands it (218 per 1/300 s tick, rows_bike_step.inc's DrawDt), now and then
// any 16-bit value or 0.
int32_t GroundDt(Rng& r) {
    if (Chance(r, 16)) return 0;
    if (Chance(r, 6)) return static_cast<int32_t>(r.U32() & 0xFFFFu);
    return 218 * static_cast<int32_t>(1u + r.U32() % 8u);
}
// The velocity (NAMED BOUND 7).
void PutVelocity(Memory& m, uint32_t R, Rng& r) {
    for (uint32_t k = 0; k < 3; ++k) PutS32(m, R + 456u + 4u * k, r.S32() >> (11 + r.U32() % 14u));
}

struct Ground {
    uint32_t R = 0, B = 0, anim = 0;
};

// A rider of this machine (rows_anim.inc's stance family) in stance `st`, the object's bank the
// stance's (NAMED BOUND 1) and its op index 0..4 (NAMED BOUND 2).
Ground PlantGround(CaseContext& c, uint32_t st) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const animrows::StanceCase sc = animrows::PlantStanceCase(c, false);
    Ground gr;
    gr.R = sc.rider;
    gr.anim = sc.anim;
    gr.B = m.PeekWord(gr.R + 596u);
    animrows::Put16(m, gr.R + 544u, st);
    if (animrows::Resident(m, st)) {
        const uint32_t want = animrows::BankPtr(m, m.PeekWord(kStance + 8u * st) & 0xFu);
        if (m.PeekWord(gr.anim + 40u) != want) {
            m.PokeWord(gr.anim + 40u, want);
            animrows::RandomProgram(c, gr.anim, true);
            animrows::RandomClock(c, gr.anim);
        }
    }
    if (m.PeekWord(gr.anim + 12u) > 4u) m.PokeWord(gr.anim + 12u, animrows::Pick(r, 5));
    return gr;
}

// The player count (pop::DrawPlayers), and now and then the rider made a player's rider.
uint32_t Players(CaseContext& c, uint32_t R) {
    const uint32_t np = pop::DrawPlayers(c);
    if (np != 0u && Chance(*c.rng, 2)) WriteU16Bench(*c.mem, R + 172u, static_cast<uint16_t>(0x20u + c.rng->U32() % np));
    return np;
}

// The pillion bit +0x23C bit 5 (NAMED BOUND 4: its bike +0x358 is a pool-0 bike).
void Pillion(CaseContext& c, uint32_t R, uint32_t B) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (Chance(r, 3)) Put8(m, R + 572u, m.PeekByte(R + 572u) ^ 0x20u);
    if (m.PeekByte(R + 572u) & 0x20u) m.PokeWord(B + 856u, pop::Bike(m, r.U32() % 18u));
}

// The pad state GroundWalkControl reads for this rider (the player index it computes itself).
void PutPad(CaseContext& c, uint32_t R, uint32_t B) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t two = (m.PeekByte(R + 572u) >> 5) & 1u;
    const uint32_t bike = two != 0u ? m.PeekWord(B + 856u) : B;
    const uint32_t hv = animrows::Get16(m, bike + 172u);
    const uint32_t np = m.PeekWord(Gs(m) + 48u);
    const uint32_t p = np < 2u ? hv + two : hv + 2u * two;
    // Only the four players' records are written; past them the original reads whatever follows,
    // which is left as captured.
    if (p >= 4u) return;
    m.PokeWord(kPadRecs + 192u * p + 16u, Chance(r, 4) ? 0u : 1u + r.U32() % 255u);
    for (uint32_t k = 0; k < 2; ++k) {
        const uint32_t q = r.U32() % 5u;
        const int32_t v = q == 0 ? 0 : q == 1 ? 0x10000 : q == 2 ? -0x10000 : (r.S32() >> (8 + r.U32() % 16u));
        PutS32(m, kPadAxes + 8u * p + 4u * k, v);
    }
}

// The view record of the bike's handle: +0x224 flags, +0x23C.. and +0x304 (the walk rate's arm).
void PutView(CaseContext& c, uint32_t B) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t h = animrows::Get16(m, B + 172u);
    if (h >= 18u) return;
    const uint32_t v = rr::sim::kRcViewArray + rr::sim::kRcViewStride * h;
    if (Chance(r, 2)) m.PokeWord(v + 548u, r.U32());
    if (Chance(r, 2)) m.PokeWord(v + 772u, Chance(r, 2) ? 0u : r.U32());
    for (uint32_t k = 0; k < 3; ++k)
        if (Chance(r, 3)) PutS32(m, v + 572u + 4u * k, r.World());
}

// ---------------------------------------------------------------- SetOp18 0x8009A038
// Driven: the stance (44 with +0x228 bit 27 both ways, the 70-list {45,51,53,57,61,62,68}, 60..63),
// the rows, the stack depth, the clock, the players and the bike's handle as a player's (the view arm).
void SetupSetOp18(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t st = Chance(r, 3)   ? 44u
                        : Chance(r, 2) ? From(r, {45u, 51u, 53u, 57u, 61u, 62u, 68u, 60u, 63u})
                                       : 44u + r.U32() % 30u;
    const Ground gr = PlantGround(c, st);
    if (Chance(r, 2)) m.PokeWord(gr.R + 552u, m.PeekWord(gr.R + 552u) ^ 0x08000000u);
    for (uint32_t row : {432u, 444u})
        if (!c.sample || Chance(r, 3)) PutRow(m, gr.R + row, r);
    Put8(m, gr.B + 946u, r.U32() % 16u); // NAMED BOUND 5
    m.PokeWord(Gs(m) + 16u, r.U32());
    const uint32_t np = pop::DrawPlayers(c);
    if (np != 0u && Chance(r, 3)) WriteU16Bench(m, gr.B + 172u, static_cast<uint16_t>(r.U32() % np));
    PutView(c, gr.B);
    a.a[0] = gr.B;
}
uint32_t NativeSetOp18(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    rcv::RecoverOracle c(env);
    const bool ok = rr::sim::SetOp18(g, a.a[0], env.sp, t, c);
    return rcv::Done(env, g, ok, "SetOp18");
}

// ---------------------------------------------------------------- GroundWalkRate 0x800986D0
// Driven: the stance (the walk 69/70, the kind-8 64..66 / 71..73, the rest), the speed +0x1E0 round
// 0xFFFF / 0x27FFF / 0x7FFFF, +0x228 bits 0x10 / 0x800, riderDef +0x27 = 254, a player's rider or not,
// the view record's +0x304 and flags.
void SetupWalkRate(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t q = r.U32() % 6u;
    const uint32_t st = q < 2 ? From(r, {69u, 70u}) : q < 4 ? From(r, {64u, 65u, 66u, 71u, 72u, 73u}) : 44u + r.U32() % 30u;
    const Ground gr = PlantGround(c, st);
    const uint32_t R = gr.R;
    if (Chance(r, 2)) m.PokeWord(R + 552u, m.PeekWord(R + 552u) ^ 0x800u);
    if (Chance(r, 2)) m.PokeWord(R + 552u, m.PeekWord(R + 552u) ^ 0x10u);
    PutS32(m, R + 480u, Speed(r, {0, 0x10000, 0x28000, 0x80000}));
    const uint32_t def = m.PeekWord(gr.B + 1084u);
    if (Chance(r, 4)) Put8(m, def + 39u, 254u);
    Players(c, R);
    PutView(c, gr.B);
    a.a[0] = R;
}
uint32_t NativeWalkRate(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    rcv::RecoverOracle c(env);
    uint32_t v0 = 0;
    const bool ok = rr::sim::GroundWalkRate(g, a.a[0], env.sp, t, c, v0);
    return rcv::Done(env, g, ok, "GroundWalkRate", v0);
}

// ---------------------------------------------------------------- GroundWalkControl 0x8009926C
// Driven: +0x228 (bits 0x80, 0x100, 0x200, 0x400, 0x800), the pillion bit, a player's rider or not,
// the pad record's +16 and both axes, the speed cap +0x1E4 round 4 x {0x18000, 0x30000}, +0x1E0,
// +0x1E8 both signs, dt.
void SetupWalkControl(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t B = pop::Bike(m, r.U32() % 18u);
    const uint32_t R = m.PeekWord(B + 852u);
    uint32_t f = c.sample ? m.PeekWord(R + 552u) : r.U32();
    f ^= r.U32() & 0xF80u;
    m.PokeWord(R + 552u, f);
    Pillion(c, R, B);
    Players(c, R);
    PutPad(c, R, B);
    PutS32(m, R + 484u, Speed(r, {0x60000, 0x5FFFD, 0xC0000, 0xC0003, -3}));
    PutS32(m, R + 480u, Speed(r, {0x40000, 0x80000}));
    PutS32(m, R + 488u, Chance(r, 4) ? 0 : (r.S32() >> (8 + r.U32() % 16u)));
    a.a[0] = R;
    a.a[1] = static_cast<uint32_t>(GroundDt(r));
}
uint32_t NativeWalkControl(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    rcv::RecoverOracle c(env);
    const bool ok = rr::sim::GroundWalkControl(g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp, t, c);
    return rcv::Done(env, g, ok, "GroundWalkControl");
}

// ---------------------------------------------------------------- GroundGetUp 0x8008FD5C
// Driven: the stance (every arm of the jump table 44..55, and 56..73 through the default arm), the
// speed +0x1E0 (0, and both sides of 0x15752 / 0x1908B / 0x1C9C3 / 0x165A1C), the counter +0x217 (its
// sign wrap), +0x228 bit 27, the object's frame +0x10, the seed.
void SetupGetUp(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t st = Chance(r, 5) ? 56u + r.U32() % 18u : 44u + r.U32() % 12u;
    const Ground gr = PlantGround(c, st);
    const uint32_t R = gr.R;
    m.PokeWord(R + 604u, 3u); // NAMED BOUND 3
    PutS32(m, R + 480u, Chance(r, 3) ? 0 : Speed(r, {0x15753, 0x1908C, 0x1C9C4, 0x165A1D}));
    Put8(m, R + 535u, Chance(r, 3) ? 0x7Fu : Chance(r, 3) ? 0xFFu : r.U32());
    if (Chance(r, 2)) m.PokeWord(R + 552u, m.PeekWord(R + 552u) ^ 0x08000000u);
    if (Chance(r, 2)) PutS32(m, gr.anim + 16u, static_cast<int32_t>(r.U32() % 40u) - 2);
    m.PokeWord(c.gp + rr::sim::kRandSeedGp, r.U32());
    a.a[0] = R;
}
uint32_t NativeGetUp(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    rcv::RecoverOracle c(env);
    uint32_t v0 = 0;
    const bool ok = rr::sim::GroundGetUp(g, a.a[0], env.sp, t, c, v0);
    return rcv::Done(env, g, ok, "GroundGetUp", v0);
}

// ---------------------------------------------------------------- GroundSlide 0x8008F754
// Driven: the stance (45/50/51, the list {49, 52..59}, the rest), +0x20C round 3547, the rows, the
// speed +0x1E0 (0 or not), the handle's pool (>> 5 == 1 or not), the normal +0x26E, the timer
// +0x258 round 0 / 0x10000, the velocity, +0x228 bit 27, dt.
void SetupSlide(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t B = pop::Bike(m, r.U32() % 18u);
    const uint32_t R = m.PeekWord(B + 852u);
    const uint32_t q = r.U32() % 6u;
    const uint32_t st = q < 2 ? From(r, {45u, 50u, 51u}) : q < 4 ? From(r, {49u, 52u, 53u, 54u, 55u, 56u, 57u, 58u, 59u})
                                                             : 44u + r.U32() % 30u;
    WriteU16Bench(m, R + 544u, static_cast<uint16_t>(st));
    for (uint32_t row : {432u, 438u, 444u, 450u, 522u, 622u}) // NAMED BOUND 6
        if (!c.sample || Chance(r, 2)) PutRow(m, R + row, r);
    if (Chance(r, 2)) PutS16(m, R + 524u, static_cast<int16_t>(3546 + static_cast<int32_t>(r.U32() % 3u)));
    PutS32(m, R + 480u, Chance(r, 3) ? 0 : Speed(r, {0x15753, 0x10000}));
    if (Chance(r, 3)) WriteU16Bench(m, R + 172u, static_cast<uint16_t>(Chance(r, 2) ? 0x20u + r.U32() % 32u : r.U32()));
    PutS32(m, R + 600u, Speed(r, {0, 0x10000, 1000}));
    if (!c.sample || Chance(r, 2)) PutVelocity(m, R, r);
    if (Chance(r, 2)) m.PokeWord(R + 552u, m.PeekWord(R + 552u) ^ 0x08000000u);
    const uint32_t anim = m.PeekWord(R + 540u);
    if (m.PeekWord(anim + 12u) > 4u) m.PokeWord(anim + 12u, animrows::Pick(r, 5)); // NAMED BOUND 2
    a.a[0] = R;
    a.a[1] = static_cast<uint32_t>(GroundDt(r));
    a.a[2] = R + 600u;
}
uint32_t NativeSlide(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    rcv::RecoverOracle c(env);
    const bool ok = rr::sim::GroundSlide(g, a.a[0], static_cast<int32_t>(a.a[1]), a.a[2], env.sp, t, c);
    return rcv::Done(env, g, ok, "GroundSlide");
}

// ---------------------------------------------------------------- RiderOnGround 0x80097BCC
// Driven: the mount state 3 / 4 and a stance for it (the tumbling 44..63, the walking kind-8 ones and
// 60..73), +0x228 (0x18 clear most of the time so the bounce is tested; 0x1, 0x10, 0x800, 0x80000,
// 0x100000, 0x20000 / 0x40000 driven), the per-player record 0x800D43C0 + 28*handle (its road +0,
// its point near the rider or not, its normal, +22), the slice-margin arm (+0x184 bit 0, +0x174,
// +0x24 bit 9, the slice's +0x30 / +0x31 against the lateral +0x158), the rows, the speeds, the
// timer, the pillion bit, the pad, the players, riderDef +0x27, the view record, dt.
void SetupOnGround(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const bool tumbling = Chance(r, 2);
    uint32_t st;
    if (tumbling) st = Chance(r, 6) ? 64u + r.U32() % 10u : 44u + r.U32() % 20u;
    else st = Chance(r, 2) ? From(r, {64u, 65u, 66u, 71u, 72u, 73u}) : 44u + r.U32() % 30u;
    const Ground gr = PlantGround(c, st);
    const uint32_t R = gr.R, B = gr.B;
    m.PokeWord(R + 604u, tumbling ? 3u : 4u);
    {
        uint32_t f = c.sample ? m.PeekWord(R + 552u) : r.U32();
        f ^= r.U32() & 0x081E0991u;
        if (!Chance(r, 4)) f &= ~0x18u;
        m.PokeWord(R + 552u, f);
    }
    Pillion(c, R, B);
    Put8(m, B + 946u, r.U32() % 16u); // NAMED BOUND 5
    for (uint32_t row : {432u, 438u, 444u, 450u, 516u, 522u, 528u, 622u}) // NAMED BOUND 6
        if (!c.sample || Chance(r, 2)) PutRow(m, R + row, r);
    if (Chance(r, 2)) PutS16(m, R + 524u, static_cast<int16_t>(Chance(r, 2) ? 3546 + static_cast<int32_t>(r.U32() % 3u)
                                                                                : r.Unit()));
    // the speed (NAMED BOUND 7: under 2^21 in magnitude, as the velocity it is the length of)
    PutS32(m, R + 480u, Chance(r, 4) ? 0 : Speed(r, {0x30000, 0x10000, 0x28000, 0x80000, 0x15753}) % 0x200000);
    PutS32(m, R + 484u, Speed(r, {0x60000, 0xC0000}));
    PutS32(m, R + 488u, Chance(r, 3) ? 0 : (r.S32() >> (8 + r.U32() % 16u)));
    PutS32(m, R + 600u, Speed(r, {0, 0x10000}));
    if (!c.sample || Chance(r, 2)) PutVelocity(m, R, r);
    Players(c, R);
    // the per-player record of the bike's handle
    {
        const uint32_t rec = kPlayerRec + 28u * animrows::Get16(m, B + 172u);
        if (Chance(r, 2)) {
            m.PokeWord(rec, Chance(r, 5) ? r.U32() : m.PeekWord(R + 360u));
            for (uint32_t k : {0u, 2u}) {
                const int32_t d = static_cast<int32_t>((r.U32() % 80u) << 16 | (r.U32() & 0xFFFFu));
                PutS32(m, rec + 4u + 4u * k, GetS32(m, R + 184u + 4u * k) + (Chance(r, 2) ? d : -d));
            }
            PutRow(m, rec + 16u, r);
            PutS16(m, rec + 22u, static_cast<int16_t>(Chance(r, 4) ? 0 : 1 + static_cast<int32_t>(r.U32() % 100u)));
        }
    }
    // the slice-margin / junction arm
    if (Chance(r, 2)) {
        m.PokeWord(R + 388u, m.PeekWord(R + 388u) | 1u);
        // +0x174 is only tested against 0 here; a zero one is given a real road pointer (the slice's).
        if (!Chance(r, 5) && m.PeekWord(R + 372u) == 0u) m.PokeWord(R + 372u, m.PeekWord(R + 340u));
        if (!Chance(r, 5)) m.PokeWord(R + 36u, m.PeekWord(R + 36u) & ~0x200u);
        const uint32_t sl = m.PeekWord(R + 340u);
        if (Chance(r, 2)) Put8(m, sl + 48u, Chance(r, 4) ? 0u : 1u + r.U32() % 8u);
        if (Chance(r, 2)) Put8(m, sl + 49u, Chance(r, 4) ? 0u : 1u + r.U32() % 8u);
        const int32_t e48 = static_cast<int32_t>(m.PeekByte(sl + 48u)) << 15;
        const int32_t e49 = static_cast<int32_t>(m.PeekByte(sl + 49u)) << 15;
        const int32_t d = static_cast<int32_t>(r.U32() % 0x20000u) - 0x10000;
        PutS32(m, R + 344u, Chance(r, 2) ? e48 + d : -e49 + d);
    }
    PutPad(c, R, B);
    if (Chance(r, 4)) Put8(m, m.PeekWord(B + 1084u) + 39u, 254u);
    PutView(c, B);
    m.PokeWord(Gs(m) + 16u, r.U32());
    m.PokeWord(c.gp + rr::sim::kRandSeedGp, r.U32());
    a.a[0] = R;
    a.a[1] = static_cast<uint32_t>(GroundDt(r));
}
uint32_t NativeOnGround(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = rcv::View(env);
    const rr::sim::BikeTables t = rcv::Tables(env, g);
    rcv::RecoverOracle c(env);
    const bool ok = rr::sim::RiderOnGround(g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp, t, c);
    return rcv::Done(env, g, ok, "RiderOnGround");
}

} // namespace rcvg

void AddRecoverGroundRows(std::vector<Row>& rows) {
    namespace s = rr::sim;
    {
        Row r{"rider_on_ground", "void RiderOnGround(Rider *R, s32 dt)  [RASHCDG 0x80097BCC]", s::kRiderOnGroundFn,
              rcvg::SetupOnGround, nullptr, false, rcvg::NativeOnGround,
              {OracleCallee{s::kGrJunctionMarginFn, 1}, OracleCallee{s::kGrEffectBurstFn, 4},
               OracleCallee{s::kGroundSlideFn, 3}, OracleCallee{s::kGroundGetUpFn, 1},
               OracleCallee{s::kGroundWalkControlFn, 2}, OracleCallee{s::kGroundWalkRateFn, 1},
               OracleCallee{s::kSetOp18Fn, 1}}};
        r.maxSteps = 50'000'000;
        rows.push_back(r);
    }
    {
        Row r{"rider_slide", "void GroundSlide(Rider *R, s32 dt, s32 *timer)  [RASHCDG 0x8008F754]", s::kGroundSlideFn,
              rcvg::SetupSlide, nullptr, false, rcvg::NativeSlide, {}};
        rows.push_back(r);
    }
    {
        Row r{"rider_getup", "s32 GroundGetUp(Rider *R)  [RASHCDG 0x8008FD5C]", s::kGroundGetUpFn, rcvg::SetupGetUp,
              nullptr, true, rcvg::NativeGetUp,
              {OracleCallee{s::kGrClipDoneFn, 1}, OracleCallee{s::kGrRangedStartFn, 4, 3},
               OracleCallee{s::kGrSetRiderStateFn, 3}, OracleCallee{s::kGrStanceEventFn, 3},
               OracleCallee{s::kGrFollowStanceFn, 2}}};
        r.maxSteps = 20'000'000;
        rows.push_back(r);
    }
    {
        Row r{"rider_walk_control", "void GroundWalkControl(Rider *R, s32 dt)  [RASHCDG 0x8009926C]",
              s::kGroundWalkControlFn, rcvg::SetupWalkControl, nullptr, false, rcvg::NativeWalkControl, {}};
        rows.push_back(r);
    }
    {
        Row r{"rider_walk_rate", "s32 GroundWalkRate(Rider *R)  [RASHCDG 0x800986D0]", s::kGroundWalkRateFn,
              rcvg::SetupWalkRate, nullptr, true, rcvg::NativeWalkRate,
              {OracleCallee{s::kGrClipDoneFn, 1}, OracleCallee{s::kGrStanceEventFn, 3}}};
        r.maxSteps = 20'000'000;
        rows.push_back(r);
    }
    {
        Row r{"set_op18", "void SetOp18(Bike *B)  [RASHCDG 0x8009A038]", s::kSetOp18Fn, rcvg::SetupSetOp18, nullptr,
              false, rcvg::NativeSetOp18,
              {OracleCallee{s::kGrStanceEventFn, 3}, OracleCallee{s::kGrCameraResetFn, 1}}};
        r.maxSteps = 20'000'000;
        rows.push_back(r);
    }
}
