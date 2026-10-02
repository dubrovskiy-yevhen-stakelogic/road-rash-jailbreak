#pragma once
// The crash chain, ported function by function. Transcribed from our
// own disassembly of the player's own images:
//
//   RASHCDG.BIN  SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8
//   SLUS_010.53  SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text at 0x80010000
//
// and accepted only by `rrverify phys` rows (tools\rrverify\rows_crash.inc).
//
// THE MEMORY MODEL is road_query.h's: a guest-address view of main RAM (`GuestRam`). These
// functions walk the intrusive bike lists (`{prev, next}` at entity +0x440, entity = node - 1088) and
// chase bike -> rider -> partner bike -> its rider exactly where the original does - with the
// original's missing null tests - so they run on the guest addresses themselves. Static tables
// indexed by a masked or bounded index (sine/cosine, arcsine, RatAtan2's, the reciprocal square
// root) are host pointers in `BikeTables`; SqrtGte's table is a WINDOW (ai.h), because the square
// root of a negative argument reads below it.
//
// A function returns false - and the caller must not trust anything it wrote - when the original
// would do something the view refuses (a load outside RAM, the overflow exception `Normalize`
// raises, a list that never closes, an emitter access the sound port cannot serve). It never guesses.
#include <cstdint>

#include "game/sim/integrator.h"
#include "game/sim/road_query.h"

namespace rr::sim {

// ------------------------------------------------------------------------ RASHCDG 0x80078DB4
// void ImpactStatePass(Node *head) - 3436 bytes, 859 instructions, frame 64, one `jr ra`, no COP2,
// no `jalr`, no jump table. The rider pass `0x8007B840` calls it on the class
// list 0x8005B350 (at 0x8007DCD8) and on the airborne list 0x8005B2D8 (at 0x8007DCE4).
//
// Per bike on the list: flagsC bit 22 toggled when |+0x16C| >= 3; class 16 waits for +0x2A4 == 0
// and ends with 0x08000000 or 0x880; classes 1/2/4/8 cut the controls, take a bump's heading once
// (bit 13), post the knock-off request rider +0x228 |= 0x8000 (0x8007916C, and the +0x358 bike's
// rider at 0x800791BC, with NO null test of +0x358), THROW the bike (flagsC := (fc & 0xFFF00200) |
// 0x40A00 - three Rand in the fixed order 0x800792C0, 0x80079548, 0x800795AC; none on the class-4
// airborne arm), start the roll ramp of a riderless bike (bit 14), and end the impact: a bump by
// BikeSteerLean, anything else by re-facing the bike and - rider off - the crash sound 50 + (5 x
// (GetRCnt(0xF2000002) & 0xFF) >> 8) through PlaySound3D.
//
// Callees, all ported: FixMul, FixDiv, DotLcm, SqrtGte, Scale, MulAdd, Rand (seed at gp+2076), Asin,
// Normalize, RatAtan2, BikeSteerLean (RASHCDG 0x80073D74, run on a host copy of the bike and the
// four stat words it reads), and the two the caller supplies through `ImpactSound`: GetRCnt
// (sound_engine.h) and PlaySound3D (sound.h). They are behind an interface because they live with
// the sound system, not with the bikes: the bench answers them with the ported functions on the
// same guest RAM, the product with its sound runtime (its own arena) and a counter stand-in.
//
// Not reproduced: the store of `head` into the CALLER's home slot (entry sp + 0) at 0x80078DE0 and
// its re-read at 0x80079ADC - no callee can reach that slot, so the port keeps `head` in a local.
// `t` needs sincos, asin, atan, rsqrt and sqrt.
struct ImpactSound {
    virtual ~ImpactSound() = default;
    // SLUS 0x80043F00 GetRCnt(id), called with 0xF2000002 (root counter 2) at 0x80079A50. False: the
    // answerer could not serve it (the call fails, it is never guessed).
    virtual bool GetRCnt(uint32_t id, uint32_t& value) = 0;
    // SLUS 0x80017BA0 PlaySound3D(x, z, id, bank) at 0x80079A74 (id 50..54, bank 0).
    virtual bool PlaySound3D(int32_t x, int32_t z, int32_t id, int32_t bank) = 0;
};
bool ImpactStatePass(GuestRam& g, uint32_t head, const BikeTables& t, ImpactSound& snd);

// ------------------------------------------------------------------------ SLUS 0x8002EA20
// s32 MulAdd16(const s16 base[3], const s16 dir[3], s32 t, s32 out[3]) - 46 instructions, a leaf:
// per component, in order, out[i] = (base[i] << 4) + the middle word of
// (dir[i] << 4) x t (`(lo >> 16) | (hi << 16)`). Returns the last sum (v0). The loads of component i
// come after the store of component i - 1, so an `out` that overlaps the inputs is reproduced.
uint32_t MulAdd16(GuestRam& g, uint32_t base, uint32_t dir, int32_t t, uint32_t out);

// ------------------------------------------------------------------------ RASHCDG 0x800849D8
// void Spin(Bike *e) - 528 bytes, frame 40. Unless flagsC bit 19: the 0.5 s
// ramp (+0x2D4 := 0, +0x2D8 := 0x8000), THREE Rand in order (0x80084A0C, 0x80084A54, 0x80084A94),
// +0x290 / +0x280 := 2 x (r %u 45752 - 22876), +0x1E8 scaled by [0.8, 1.2) and clamped by the speed
// (the negative clamp is NOT the mirror of the positive one), and its sign set by the tumble axis
// against the heading. Callees: Rand, FixMul.
bool Spin(GuestRam& g, uint32_t e);

// ------------------------------------------------------------------------ RASHCDG 0x80084564
// s32 Bounce(const s16 n[3], s16 d[3], s32 *pSpeed, s32 vel[3], [sp+16] k, [sp+20] floor,
//            [sp+24] flat) - 1140 bytes, frame 56. DotLcm(d, n) >= 0: returns -1
// and writes nothing. Otherwise a wall-like normal (|n.y| < 0.5) with the heading going down mirrors
// the horizontal part about the unit tangent (-n.z, 0, n.x) and sets d.y := flat ? 0 : (0.7071 -
// d.y) / 2, floor halved; anything else leaves at angle theta + clamp(k theta / 10, 56, 512) from
// the normal (4.9 .. 45 degrees; k scaled by the speed when floor == 0), at least 1.1 near head-on,
// through MulAdd16. Then *pSpeed := max(0.1 or 0.8 x *pSpeed, floor), Normalize(d), vel := Scale.
// Returns 1 (wall-like) or 0. Callees: DotLcm, FixMul, SqrtGte, Asin, RatTan, MulAdd16, Normalize,
// Scale. `t` needs sincos, asin, rsqrt and sqrt. The home-slot store of `vel` (entry sp + 12,
// 0x800845B0) is in the caller's frame and is not reproduced; the port keeps `vel` in a local.
// `ok` is false where the port refused (a fault, Normalize's overflow exception).
int32_t Bounce(GuestRam& g, uint32_t n, uint32_t d, uint32_t pSpeed, uint32_t vel, int32_t k,
               int32_t floor, int32_t flat, const BikeTables& t, bool& ok);

// ------------------------------------------------------------------------ RASHCDG 0x800903F4
// void Remount(Bike *B, s32 fromRoad) - 1056 bytes, frame 48: the INSTANT
// remount. fromRoad and a live bike (+0x140): back onto its road slice (MulAdd along the slice's
// axis by +0x15C into +0x1F8, the box centre and heading from the slice, reversed with +0x16C = -2
// when flagsC bit 22, else +0x16C = 2, then the rows); +0x158 := 0. Then the bike state reset,
// flagsC |= 0x08000000, the rider's +0x228 &= 0xBFE67FC7 and +0x140 from the bike, the rider object
// released, the rider attached and dismounted to mount state 0 (the passenger too, to 1, when the
// rider's +0x23C bit 4 and game_state +0x39 != 1), the AI stack cleared and {2 or 4, 224} pushed
// (mode 1), a live bike re-faced along its road leg (and the passenger's lateral and direction
// set beside it), and for a player the view record 0x800CD898 + 1132 h and three presentation calls.
//
// The callees that are NOT written inline. Each returns false when the caller could not run it;
// Remount then refuses. The bench supplies the unread ones from the oracle and runs
// the two PORTED AI functions (ai.h) on its own RAM; the product must supply them all.
struct RemountCallees {
    virtual ~RemountCallees() = default;
    virtual bool RowsFromHeading(uint32_t B) = 0;                          // RASHCDG 0x8007EC30 (1 arg)
    virtual bool ResetBikeState(uint32_t B) = 0;                           // SLUS 0x8002090C (1)
    virtual bool ReleaseRiderObject(uint32_t R) = 0;                       // RASHCDG 0x80095AEC (1)
    virtual bool Attach(uint32_t B, uint32_t R, int32_t kind, int32_t seat) = 0;   // SLUS 0x80012838 (4)
    virtual bool Dismount(uint32_t R, int32_t how) = 0;                    // RASHCDG 0x800C3104 (2)
    virtual bool ClearCommands(uint32_t B) = 0;                            // RASHCDG 0x800BCD10, PORTED
    // RASHCDG 0x800BCA68(cmd, mode, B), PORTED. The original's 8-byte `cmd` lives in Remount's own
    // frame with only its first two halfwords written; mode 1 compares only those, and a push
    // overwrites the other four bytes, so the stale half never reaches memory.
    virtual bool PushCommand(uint16_t op, uint16_t target, int32_t mode, uint32_t B) = 0;
    virtual bool ReFace(uint32_t B) = 0;                                   // RASHCDG 0x80096564 (1)
    virtual bool ViewReset(uint32_t view) = 0;                             // RASHCDG 0x80086AF8 (1)
    virtual bool PlayerVoice(uint32_t h, int32_t a1) = 0;                  // SLUS 0x80018440 (2)
    virtual bool PlayerBind(uint32_t h, uint32_t B) = 0;                   // SLUS 0x800235B0 (2)
};
bool Remount(GuestRam& g, uint32_t B, int32_t fromRoad, RemountCallees& c);

// ------------------------------------------------------------------------ SLUS 0x8002EED8
// void ScaleTo16(s32 t, const s32 v[3], s16 out[3]) - 112 bytes: out[i] = FixMul(t, v[i]) >> 4, in
// order, each v[i] read after out[i-1] was stored (an overlapping `out` is reproduced).
void ScaleTo16(GuestRam& g, int32_t t, uint32_t v, uint32_t out);

// ------------------------------------------------------------------------ RASHCDG 0x80083864
// void MassExchange(s32 m1, s32 m2, s32 v1, s32 v2, s32 *o1, s32 *o2) - 196 bytes, frame 48, the
// 1-D elastic exchange: o1 = FixMul(m1 - m2, v1) + FixMul(2 m2, v2) and
// o2 = FixMul(2 m1, v1) + FixMul(m2 - m1, v2), each stored raw and then again clamped at 0.
void MassExchangeValues(int32_t m1, int32_t m2, int32_t v1, int32_t v2, int32_t& o1raw, int32_t& o1,
                        int32_t& o2raw, int32_t& o2);
void MassExchange(GuestRam& g, int32_t m1, int32_t m2, int32_t v1, int32_t v2, uint32_t o1, uint32_t o2);

// ------------------------------------------------------------------------ RASHCDG 0x80081D7C
// s32 BikeBikeGate(Bike *a, Bike *b, u32 codeA, u32 codeB, [sp+16] s16 *n) - 6888 bytes, 1722
// instructions, frame 280: HitOutcome for a PAIR of bikes. Latches both; a
// crashed bike is bounced (Bounce + Spin) and spreads the crash to its partner or throws an
// airborne one; an airborne pair above / below each other bounces or knocks on; otherwise both are
// classified (REAR / HEAD / ANGLED / SIDE), exchange momentum, and are handed to HitSpeed or
// TakePartnerHeading. Returns 0 (no class), 1 (the crashed / airborne arms), else the OR of the
// hand-over results.
//
// THE UNINITIALISED STACK WORDS. Two arms read frame slots this call has not written:
//   * ANGLED with t1 = 1 reads the angle and limit of bike 1 out of sp+108 / sp+116 (only the JOIN
//     loop writes them);
//   * REAR never computes the closing speeds cl[0] / cl[1] (sp+120 / sp+124), and the JOIN loop
//     hands FixMul(cl[k], 2443) to HitSpeed as its sixth argument all the same (this port's bench
//     row covers it).
// `stale` = {sp+108, sp+116, sp+120, sp+124} as the original finds them: the bench reads the four
// words out of the guest's own stack at entry sp - 280 + 108 .. + 124; the product passes what the
// live game leaves there: {8, 0x800B4C58, 0x801B7F84, 0x00140000} in every live call the crash runs
// recorded (the ANGLED pair is a return address left at that depth by the road-wall collision, which
// makes `beyond` true; the REAR pair's first word is a stale RAM address).
//
// The ONE GTE OP (0x800830F0, sf = 1, lm = 0) is `OuterProduct` (integrator.h), the same native OP
// every benched row that issues it uses. Ported callees run inline: FixMul, FixDiv, DotLcm, RatAtan2,
// Length3, MulAdd32, Scale, ScaleTo16, MassExchange, Bounce, Spin. The rest is asked for:
struct GateCallees {
    virtual ~GateCallees() = default;
    // RASHCDG 0x80083F30 ImpactTurn(e, partnerHandle, n, mode) - 4 args.
    virtual bool ImpactTurn(uint32_t e, uint32_t partner, uint32_t n, int32_t mode) = 0;
    // RASHCDG 0x80080D1C HitSpeed(e, dir, pSpeed, partner, [nrm, k, ang, lim, out, a9, 0]) - 11
    // args; `nrm` is a 3 x s16 buffer in the ORIGINAL's frame, slot `nrmSlot` (sp+144 + 6 nrmSlot):
    // the caller hands it over by value and the answerer puts it where the callee can read it.
    virtual bool HitSpeed(uint32_t e, uint32_t dir, uint32_t pSpeed, uint32_t partner, const int16_t nrm[3],
                          uint32_t nrmSlot, int32_t k, int32_t ang, int32_t lim, int32_t out, int32_t a9,
                          int32_t& v0) = 0;
    // RASHCDG 0x80080B10 TakePartnerHeading(e, dir, pSpeed, partnerDir, [out]) - 5 args.
    virtual bool TakePartnerHeading(uint32_t e, uint32_t dir, uint32_t pSpeed, uint32_t partnerDir, int32_t out,
                                    int32_t& v0) = 0;
    // SLUS 0x8001A760 RiderSpeech(h, crash) - 2 args.
    virtual bool RiderSpeech(uint32_t h, int32_t crash) = 0;
};
int32_t BikeBikeGate(GuestRam& g, uint32_t a, uint32_t b, uint32_t codeA, uint32_t codeB, uint32_t n,
                     const int32_t stale[4], const BikeTables& t, GateCallees& c, bool& ok);

} // namespace rr::sim
