#include "game/sim/bike.h"

#include <cstddef>
#include <initializer_list>

#include "game/sim/fixed.h"
#include "game/sim/vec.h"

namespace rr::sim {
namespace {

inline int32_t Add32(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}
inline int32_t Sub32(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}
inline int16_t LoadS16(const uint8_t* p) {
    return static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint32_t>(p[0]) |
                                                      (static_cast<uint32_t>(p[1]) << 8)));
}
inline int32_t LoadS32(const uint8_t* p) {
    return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24));
}

// The engine's symmetric clamp, as the compiler emits it:
//   x += ((x + lim) >> 31) & (-lim - x);   x += ((lim - x) >> 31) & (lim - x);
// Written out rather than reduced to min/max so that the 32-bit wrap of `x + lim` is preserved.
inline int32_t SymClamp(int32_t x, int32_t lim) {
    const int32_t lowTerm = Sub32(Sub32(0, lim), x);
    int32_t r = Add32(x, static_cast<int32_t>(static_cast<uint32_t>(Add32(x, lim) >> 31) &
                                              static_cast<uint32_t>(lowTerm)));
    const int32_t highTerm = Sub32(lim, x);
    r = Add32(r, static_cast<int32_t>(static_cast<uint32_t>(highTerm >> 31) &
                                      static_cast<uint32_t>(highTerm)));
    return r;
}

// `Rand() % 2286`, as the compiler emits it: an unsigned magic-number division by 2286.
//   q = (((r >> 1) * 0x72AC755D) >> 32) >> 9;   rem = r - 2286*q
inline uint32_t RandMod2286(uint32_t r) {
    const uint64_t p = static_cast<uint64_t>(r >> 1) * 0x72AC755Dull;
    const uint32_t q = static_cast<uint32_t>(p >> 32) >> 9;
    const uint32_t nine = (q << 3) + q;            // 9q
    const uint32_t term = ((nine << 7) - nine) << 1; // 2286q
    return r - term;
}

} // namespace

int32_t SetImpactDirection(EntityView bike, const int16_t hit[3], const uint16_t* rsqrtTable) {
    if ((bike.U32(ent::kFlagsC) & 0x60Fu) != 0) return 0;  // 0x80075B20
    if ((bike.U32(ent::kFlagsB) & 0x400u) != 0) return 0;  // 0x80075B34
    if (!(hit[1] < 2633)) return 0;                        // 0x80075B48, `slti v0,v0,2633`

    // 0x80075B54..0x80075B8C. `mflo` keeps only the low 32 bits of each product, and the
    // `sll 4 / sra 16` pair then works on that 32-bit word.
    const int32_t px = static_cast<int32_t>(static_cast<uint32_t>(
        static_cast<uint64_t>(static_cast<int64_t>(bike.S16(ent::kHeading + 0)) * hit[0]) & 0xFFFFFFFFu));
    const int32_t pz = static_cast<int32_t>(static_cast<uint32_t>(
        static_cast<uint64_t>(static_cast<int64_t>(bike.S16(ent::kHeading + 4)) * hit[2]) & 0xFFFFFFFFu));
    const int32_t tx = static_cast<int32_t>(static_cast<uint32_t>(px) << 4) >> 16;
    const int32_t tz = static_cast<int32_t>(static_cast<uint32_t>(pz) << 4) >> 16;
    const int32_t dot = static_cast<int32_t>(static_cast<uint32_t>(tx) + static_cast<uint32_t>(tz));
    if (dot <= 0) return 0;                                // 0x80075B94

    // 0x80075B9C..0x80075BB4: the reversed contact direction, with the vertical term forced to 0.
    bike.SetU16(ent::kImpact + 0, static_cast<uint16_t>(0u - static_cast<uint32_t>(static_cast<uint16_t>(hit[0]))));
    bike.SetU16(ent::kImpact + 2, 0);
    bike.SetU16(ent::kImpact + 4, static_cast<uint16_t>(0u - static_cast<uint32_t>(static_cast<uint16_t>(hit[2]))));

    // The original then normalises the three halfwords it just stored, in place.
    // With hit[1] forced to zero the sum of squares is at most 2^31, which overflows the trapping
    // `add` for the single input pair hit[0] == hit[2] == -32768. The engine only ever hands this
    // function contact normals of 4096 = 1.0 scale, so that pair cannot occur; Normalize reports it
    // rather than inventing a result, and this port passes the report on by leaving the field as the
    // three raw halfwords it just stored.
    int16_t v[3] = {bike.S16(ent::kImpact + 0), bike.S16(ent::kImpact + 2), bike.S16(ent::kImpact + 4)};
    Normalize(v, rsqrtTable);
    bike.SetU16(ent::kImpact + 0, static_cast<uint16_t>(v[0]));
    bike.SetU16(ent::kImpact + 2, static_cast<uint16_t>(v[1]));
    bike.SetU16(ent::kImpact + 4, static_cast<uint16_t>(v[2]));

    bike.SetU32(ent::kFlagsB, bike.U32(ent::kFlagsB) | 0x400000u); // 0x80075BC4
    return 1;
}

void BikeIdleStep(EntityView bike, int32_t dt, const uint8_t* stats, uint32_t& randSeed) {
    // 1. 0x80075C0C..0x80075C40: the largest of 29 words at stats+60, and its index.
    int32_t best = 0;
    int32_t bestIndex = 0;
    for (int32_t i = 0; i < 29; ++i) {
        const int32_t v = LoadS32(stats + 60 + 4 * static_cast<size_t>(i));
        if (best < v) { bestIndex = i; best = v; }
    }

    // 2/3. 0x80075C44..0x80075C94: the rev ramp, capped by the stat block's own ceiling.
    const int32_t step = static_cast<int32_t>(static_cast<uint32_t>(
        static_cast<uint64_t>(static_cast<int64_t>(bestIndex) * LoadS32(stats + 184)) & 0xFFFFFFFFu));
    int32_t ceiling = Add32(LoadS32(stats + 176), step);
    const int32_t ramped = Add32(static_cast<int32_t>(bike.U32(ent::kIdleRev)),
                                 FixMul(0x27100000, dt)); // 0x27100000 = 10000.0
    if (ramped < ceiling) ceiling = ramped;
    bike.SetU32(ent::kIdleRev, static_cast<uint32_t>(ceiling));

    const uint32_t flagsA = bike.U32(ent::kFlagsA);
    if ((flagsA & 0x300u) != 0) {
        // 4. 0x80075C98..0x80075DC4. Bit 8 of flagsA becomes an all-ones / all-zeroes mask, and
        //    each constant is then ORed through it to flip its sign.
        const uint32_t mask = static_cast<uint32_t>(0u - ((flagsA >> 8) & 1u));

        const int32_t yawStep = Add32(static_cast<int32_t>(mask & 0xFFFF79F6u), 17157);
        const int32_t yaw = SymClamp(Add32(static_cast<int32_t>(bike.U32(ent::kIdleYaw)),
                                           FixMul(yawStep, dt)), 17157);
        bike.SetU32(ent::kIdleYaw, static_cast<uint32_t>(yaw));

        const int32_t aStep = static_cast<int32_t>((mask & 0xFFFE0000u) | 0x00010000u);
        const int32_t a = SymClamp(Add32(static_cast<int32_t>(bike.U32(ent::kIdleA)),
                                         FixMul(aStep, dt)), 0x10000);
        bike.SetU32(ent::kIdleA, static_cast<uint32_t>(a));

        const int32_t bStep = Add32(static_cast<int32_t>(mask & 0xFFFF999Au), 13107);
        const int32_t b = SymClamp(Add32(static_cast<int32_t>(bike.U32(ent::kIdleB)),
                                         FixMul(bStep, dt)), 13107);
        bike.SetU32(ent::kIdleB, static_cast<uint32_t>(b));

        // 0x80075D6C..0x80075DC4: kIdleTilt = 341 * yaw / 17159, as a magic-number division.
        const int32_t five = Add32(static_cast<int32_t>(static_cast<uint32_t>(yaw) << 2), yaw);
        const int32_t eightyFive = Add32(five, static_cast<int32_t>(static_cast<uint32_t>(five) << 4));
        const int32_t n = Add32(static_cast<int32_t>(static_cast<uint32_t>(eightyFive) << 2), yaw);
        const int64_t prod = static_cast<int64_t>(n) * static_cast<int64_t>(0x3D1DD3BF);
        const int32_t hi = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(prod) >> 32));
        const int32_t tilt = Sub32(hi >> 12, n >> 31);
        bike.SetU16(ent::kIdleTilt, static_cast<uint16_t>(static_cast<uint32_t>(tilt) & 0xFFFFu));
    }

    // 5. 0x80075DC8..0x80075E30: if the yaw has collapsed, kick it with a fresh random offset.
    const int32_t yawNow = static_cast<int32_t>(bike.U32(ent::kIdleYaw));
    const uint32_t sign = static_cast<uint32_t>(yawNow >> 31);
    const int32_t yawAbs = static_cast<int32_t>((sign + static_cast<uint32_t>(yawNow)) ^ sign);
    if (yawAbs < 1143) {
        const uint32_t r = Rand(randSeed);
        bike.SetU32(ent::kIdleYaw, static_cast<uint32_t>(Add32(Sub32(yawNow, 1143),
                                                               static_cast<int32_t>(RandMod2286(r)))));
    }

    // 6. 0x80075E34..0x80075EC0. The stores are interleaved with the second Rand() in the original,
    //    but they touch six different fields, so the order below is observationally identical.
    const uint32_t r2 = Rand(randSeed);
    bike.SetU32(ent::kIdleTimer, 0x00020000u);
    bike.SetU32(ent::kSpeedCopy, 0);
    bike.SetU32(ent::kFlagsB, bike.U32(ent::kFlagsB) | 0x08000000u);
    const int32_t lean = Add32(Sub32(static_cast<int32_t>(bike.U32(ent::kIdleLean)), 1143),
                               static_cast<int32_t>(RandMod2286(r2)));
    bike.SetU32(ent::kIdleLean, static_cast<uint32_t>(SymClamp(lean, 5719)));
}

namespace {

// The engine's sign-split around the unsigned FixDiv: both operands are made positive and the
// quotient is negated when exactly one of them was non-positive. The `<= 0` (not `< 0`) test is the
// original's `blez`/`bgtz` pair and it matters when an operand is exactly zero.
int32_t SignedFixDiv(int32_t num, int32_t den) {
    bool negate;
    int32_t n = num, d = den;
    if (n > 0) {
        if (d > 0) negate = false;
        else { d = Sub32(0, d); negate = true; }
    } else {
        n = Sub32(0, n);
        if (d > 0) negate = true;
        else { d = Sub32(0, d); negate = false; }
    }
    const uint32_t q = FixDiv(static_cast<uint32_t>(n), static_cast<uint32_t>(d));
    return static_cast<int32_t>(negate ? (0u - q) : q);
}

// The reciprocal the inlined divides of 0x80074170 use. Note the ARITHMETIC `x >> 1` and the
// `+ ((x - 2) >> 31)` correction, neither of which the shared FixDiv (0x80010028) has.
uint32_t ReciprocalSeed(int32_t x) {
    const int32_t half = x >> 1;
    const int32_t adjust = Sub32(x, 2) >> 31;
    const uint32_t d = static_cast<uint32_t>(Add32(half, adjust));
    return (d == 0) ? 0xFFFFFFFFu : (0x80000000u / d); // R3000 divu by zero -> 0xFFFFFFFF
}

} // namespace

void BikeSteerLean(EntityView bike, const uint8_t* stats, const int16_t* sincos) {
    const int32_t steer = static_cast<int32_t>(bike.U32(ent::kSteer));
    const int32_t speed = static_cast<int32_t>(bike.U32(ent::kSpeedCopy));

    // 0x80073D9C / 0x80073DB0: standing still, or no steering input at all.
    if (steer == 0 || !(0x8000 < speed)) {
        bike.SetU32(ent::kLatAccel, 0);
        // 0x80074130..0x80074148: halve the lean, rounding toward zero.
        const int32_t lean = bike.S16(ent::kLean);
        const int32_t halved = Add32(lean, (lean < 0) ? 1 : 0) >> 1;
        bike.SetU16(ent::kLean, static_cast<uint16_t>(static_cast<uint32_t>(halved) & 0xFFFFu));
        return;
    }

    // 0x80073DC4..0x80073E14: the steering angle.
    const int32_t biased = Add32(steer, static_cast<int32_t>(bike.U32(ent::kSteerBias)));
    const int32_t clamped = SymClamp(biased, 0x16571);
    const int32_t five = Add32(static_cast<int32_t>(static_cast<uint32_t>(clamped) << 2), clamped);
    const int32_t fortyOne = Add32(static_cast<int32_t>(static_cast<uint32_t>(five) << 3), clamped);
    const int32_t oneSixtyThree = Sub32(static_cast<int32_t>(static_cast<uint32_t>(fortyOne) << 2), clamped);
    const int32_t angle = static_cast<int32_t>(static_cast<uint32_t>(oneSixtyThree) << 2) >> 16;

    // 0x80073E18..0x80073E34.
    const int32_t base = Add32(FixMul(static_cast<int32_t>(bike.U32(ent::kLeanGain)),
                                      RatTan(angle, sincos)),
                               static_cast<int32_t>(bike.U32(ent::kLeanBias)));

    // 0x80073E24..0x80073F50: the grip ratio, computed through four sign-split arms that all
    // recompute the same two products.
    const int32_t p = Add32(FixMul(LoadS32(stats + 280), speed), 0x10000);
    const int32_t q = Add32(FixMul(LoadS32(stats + 284), speed), 0x10000);
    int32_t grip = SignedFixDiv(p, q);

    if (!(64879 < grip)) { // 0x80073F58
        // 0x80073F64..0x8007400C.
        const int32_t oneMinus = Sub32(0x10000, grip);
        const int32_t denominator = FixMul(oneMinus, speed);
        int32_t lat = SignedFixDiv(base, denominator);
        bike.SetU32(ent::kLatAccel, static_cast<uint32_t>(lat));
        // 0x80074014..0x80074048.
        lat = SymClamp(lat, LoadS32(stats + 232));
        bike.SetU32(ent::kLatAccel, static_cast<uint32_t>(lat));
        grip = FixMul(grip, lat);
    } else {
        bike.SetU32(ent::kLatAccel, static_cast<uint32_t>(grip)); // 0x80074054
    }

    // 0x80074058: the lean is only written in the low modes.
    if (!(static_cast<int8_t>(bike.bytes()[ent::kMode]) < 2)) return;

    // 0x8007406C..0x8007412C.
    const int32_t ratio = SignedFixDiv(grip, speed);
    const int32_t scaled = SymClamp(FixMul(ratio, LoadS32(stats + 4)), 0xC90F);
    const int32_t five2 = Add32(static_cast<int32_t>(static_cast<uint32_t>(scaled) << 2), scaled);
    const int32_t fortyOne2 = Add32(static_cast<int32_t>(static_cast<uint32_t>(five2) << 3), scaled);
    const int32_t oneSixtyThree2 = Sub32(static_cast<int32_t>(static_cast<uint32_t>(fortyOne2) << 2), scaled);
    // `srl`, not `sra`: a negative product becomes a large positive lean, which is what the
    // original stores.
    const int32_t lean = static_cast<int32_t>(static_cast<uint32_t>(oneSixtyThree2) >> 14);
    bike.SetU16(ent::kLean, static_cast<uint16_t>(static_cast<uint32_t>(lean) & 0xFFFFu));
}

void BikeSolveSteer(EntityView bike, const uint8_t* stats, const int32_t* atanTable,
                    const int16_t* sincos) {
    const int32_t speed = static_cast<int32_t>(bike.U32(ent::kSpeedCopy));

    // 0x8007418C..0x80074244: the same grip ratio BikeSteerLean computes.
    const int32_t p = Add32(FixMul(LoadS32(stats + 280), speed), 0x10000);
    const int32_t q = Add32(FixMul(LoadS32(stats + 284), speed), 0x10000);
    const int32_t grip = SignedFixDiv(p, q);

    // 0x80074248..0x80074270.
    const int32_t latAccel = static_cast<int32_t>(bike.U32(ent::kLatAccel));
    const int32_t scaledGrip = FixMul(grip, latAccel);

    // 0x80074274..0x80074334: the lean, skipped below 0.1 of speed.
    if (!(speed < 6554)) {
        const int32_t inv = FixMul(scaledGrip, static_cast<int32_t>(ReciprocalSeed(speed)));
        const int32_t term = FixMul(inv, LoadS32(stats + 4));
        const int32_t clampedTerm = SymClamp(term, LoadS32(stats + 240));
        const int32_t five = Add32(static_cast<int32_t>(static_cast<uint32_t>(clampedTerm) << 2), clampedTerm);
        const int32_t fortyOne = Add32(static_cast<int32_t>(static_cast<uint32_t>(five) << 3), clampedTerm);
        const int32_t oneSixtyThree =
            Sub32(static_cast<int32_t>(static_cast<uint32_t>(fortyOne) << 2), clampedTerm);
        const int32_t lean = static_cast<int32_t>(static_cast<uint32_t>(oneSixtyThree) >> 14);
        bike.SetU16(ent::kLean, static_cast<uint16_t>(static_cast<uint32_t>(lean) & 0xFFFFu));
    }

    // 0x80074338..0x800743B4: back out the tangent the forward model would have used.
    const int32_t numerator = Sub32(FixMul(Sub32(latAccel, scaledGrip), speed),
                                    static_cast<int32_t>(bike.U32(ent::kLeanBias)));
    int32_t tangent = SignedFixDiv(numerator, static_cast<int32_t>(bike.U32(ent::kLeanGain)));

    // 0x800743B8..0x800744E4: RatAtan2's tail, inlined - the octant code here is only 0..3 because
    // the caller has already split the sign.
    const uint32_t sign = static_cast<uint32_t>(tangent >> 31);
    uint32_t octant = sign & 2u;
    tangent = static_cast<int32_t>((sign + static_cast<uint32_t>(tangent)) ^ sign); // |tangent|
    int32_t index = tangent >> 12;
    if (0x10000 < tangent) {
        octant |= 1u;
        if (tangent >= 0) {
            tangent = static_cast<int32_t>(ReciprocalSeed(tangent));
        } else {
            const int32_t neg = Sub32(0, tangent);
            tangent = Sub32(0, static_cast<int32_t>(ReciprocalSeed(neg)));
        }
        index = tangent >> 12;
    }
    const int32_t lo = atanTable[index];
    const int32_t hi = atanTable[index + 1];
    const int32_t frac = static_cast<int32_t>(static_cast<uint32_t>(tangent) & 0xFFFu);
    int32_t slope = Sub32(hi, lo);
    slope = Add32(static_cast<int32_t>(static_cast<uint32_t>(slope) << 4), 8);
    const int32_t angle = Add32(FixMul(frac, slope), lo) >> 20;

    int32_t octantAngle;
    switch (octant) {
        case 0: octantAngle = angle; break;
        case 1: octantAngle = Sub32(1024, angle); break;
        case 2: octantAngle = Sub32(0, angle); break;
        case 3: octantAngle = Sub32(angle, 1024); break;
        default: octantAngle = 0; break;
    }

    // 0x800744E4..0x80074514: 25736 * a >> 8, then remove the bias.
    const int32_t three = Add32(static_cast<int32_t>(static_cast<uint32_t>(octantAngle) << 1), octantAngle);
    const int32_t twentyFive = Add32(static_cast<int32_t>(static_cast<uint32_t>(three) << 3), octantAngle);
    const int32_t twoHundredOne = Add32(static_cast<int32_t>(static_cast<uint32_t>(twentyFive) << 3), octantAngle);
    const int32_t threeThousand = Add32(static_cast<int32_t>(static_cast<uint32_t>(twoHundredOne) << 4), octantAngle);
    const int32_t scaled = static_cast<int32_t>(static_cast<uint32_t>(threeThousand) << 3);
    const int32_t steer = Sub32(scaled >> 8, static_cast<int32_t>(bike.U32(ent::kSteerBias)));
    bike.SetU32(ent::kSteer, static_cast<uint32_t>(steer));

    // 0x80074518..0x80074558: clamp, and re-run the forward model only if the clamp bit.
    const int32_t clamped = SymClamp(steer, LoadS32(stats + 228));
    if (clamped != steer) {
        bike.SetU32(ent::kSteer, static_cast<uint32_t>(clamped));
        BikeSteerLean(bike, stats, sincos);
    }
}

void BikeSteerDriver(BikeSteerNode* nodes, size_t count, int32_t dt, int32_t numPlayers,
                     int32_t gs34, const uint8_t* aiTable, const int32_t* atanTable,
                     const int16_t* sincos) {
    constexpr uint32_t kHoldTimer = 0x2D0; // 720 - non-zero means "hold the current steer"
    constexpr uint32_t kRecover = 0x2D4;   // 724 - the crash-recovery countdown
    constexpr uint32_t kFadeA = 0x2A0;     // 672
    constexpr uint32_t kFadeB = 0x28C;     // 652
    constexpr uint32_t kMatrixRow0 = 0x204;
    constexpr uint32_t kMatrixRow2 = 0x210;

    for (size_t i = 0; i < count; ++i) {
        BikeSteerNode& n = nodes[i];
        EntityView& e = n.bike;

        // 1. 0x800738C8..0x800739DC.
        const uint32_t flagsA0 = e.U32(ent::kFlagsA);
        const bool crashing = (e.U32(ent::kFlagsC) & 0x600u) != 0;           // t0
        const uint32_t geometric = (flagsA0 >> 27) & 1u;                     // a2
        uint32_t cleared = flagsA0 & 0xFFEFFFFFu;
        const uint32_t handle = e.U16(ent::kHandle);
        e.SetU32(ent::kFlagsA, cleared);
        if (handle < static_cast<uint32_t>(numPlayers) && geometric == 0) {
            if (LoadS32(aiTable + 192u * handle + 16u) != 0) {
                cleared |= 0x100000u;
                e.SetU32(ent::kFlagsA, cleared);
            }
            if ((n.ownerFlagByte & 0x10u) != 0 && !(static_cast<uint32_t>(gs34) < 2u)) {
                const uint32_t h = e.U16(ent::kHandle);
                const uint32_t k = (static_cast<uint32_t>(numPlayers) < 2u) ? (h + 1u) : (h + 2u);
                if (n.rider != nullptr) {
                    if (LoadS32(aiTable + 192u * k + 16u) != 0)
                        n.rider->SetU32(ent::kFlagsA, n.rider->U32(ent::kFlagsA) | 0x100000u);
                    else
                        n.rider->SetU32(ent::kFlagsA, n.rider->U32(ent::kFlagsA) & 0xFFEFFFFFu);
                }
            }
        }

        // 2. 0x800739E4..0x80073A18.
        e.SetU32(ent::kFlagsA, e.U32(ent::kFlagsA) & 0xFFF7FFFFu);
        const bool gate = crashing || (e.U32(ent::kFlagsB) & 0x40u) != 0;
        if (gate && (e.U32(ent::kFlagsC) & 0x1FFu) != 0) continue;

        // 3. 0x80073A20..0x80073A2C.
        if (e.U32(kHoldTimer) != 0) {
            BikeSteerLean(e, n.stats, sincos); // 0x80073D30
            continue;
        }

        // 4. 0x80073A30..0x80073A4C.
        const uint32_t flagsA = e.U32(ent::kFlagsA);
        const uint32_t bit13 = (flagsA >> 13) & 1u;
        if (geometric != 0 && bit13 != 0) {
            // ------------------------------------------------- crash recovery
            if ((flagsA & 0x4000u) != 0) {
                // 0x80073A58..0x80073B4C.
                int16_t axis2[3], axis0[3], impact[3];
                for (int k = 0; k < 3; ++k) {
                    axis2[k] = e.S16(kMatrixRow2 + 2u * static_cast<uint32_t>(k));
                    axis0[k] = e.S16(kMatrixRow0 + 2u * static_cast<uint32_t>(k));
                    impact[k] = e.S16(ent::kImpact + 2u * static_cast<uint32_t>(k));
                }
                const int32_t first = DotLcm(axis2, impact);
                const int32_t second = DotLcm(axis0, impact);
                const int32_t angle = RatAtan2(second, Sub32(0, first), atanTable);
                const int32_t three = Add32(static_cast<int32_t>(static_cast<uint32_t>(angle) << 1), angle);
                const int32_t twentyFive =
                    Add32(static_cast<int32_t>(static_cast<uint32_t>(three) << 3), angle);
                const int32_t twoHundredOne =
                    Add32(static_cast<int32_t>(static_cast<uint32_t>(twentyFive) << 3), angle);
                const int32_t threeThousand =
                    Add32(static_cast<int32_t>(static_cast<uint32_t>(twoHundredOne) << 4), angle);
                const int32_t scaled =
                    static_cast<int32_t>(static_cast<uint32_t>(threeThousand) << 3) >> 8;
                const int32_t offset = (scaled > 0) ? 0x0001921F : static_cast<int32_t>(0xFFFE6DE1);
                const int32_t delta = Sub32(offset, scaled);
                const uint32_t sign = static_cast<uint32_t>(delta >> 31);
                const int32_t mag = static_cast<int32_t>((sign + static_cast<uint32_t>(delta)) ^ sign);
                e.SetU32(kRecover, static_cast<uint32_t>(SignedFixDiv(mag, LoadS32(n.stats + 232))));
                int32_t lat = LoadS32(n.stats + 232);
                if (delta < 0) lat = Sub32(0, lat);
                e.SetU32(ent::kLatAccel, static_cast<uint32_t>(lat));
                e.SetU32(ent::kFlagsA, e.U32(ent::kFlagsA) & ~0x4000u);
            }
            // 0x80073B50..0x80073BA0.
            const int32_t recover = static_cast<int32_t>(e.U32(kRecover));
            uint32_t fa = e.U32(ent::kFlagsA);
            if (!(recover > 0)) fa &= ~0x2000u;
            e.SetU32(ent::kFlagsA, fa);
            const int32_t lat = (recover > 0) ? static_cast<int32_t>(e.U32(ent::kLatAccel)) : 0;
            e.SetU32(ent::kLatAccel, static_cast<uint32_t>(lat));
            if (n.rider != nullptr) n.rider->SetU32(ent::kLatAccel, static_cast<uint32_t>(lat));
            e.SetU32(kRecover, static_cast<uint32_t>(Sub32(recover, dt)));
            BikeSolveSteer(e, n.stats, atanTable, sincos);
            continue;
        }

        // 0x80073BAC..0x80073BF8: the AI gate.
        const uint32_t flagsAi = e.U32(ent::kFlagsA);
        const int32_t speed = static_cast<int32_t>(e.U32(ent::kSpeed));
        const int32_t speedCopy = static_cast<int32_t>(e.U32(ent::kSpeedCopy));
        const int32_t blend = Add32(speedCopy, static_cast<int32_t>(
                                                   static_cast<uint32_t>(0 - (crashing ? 1 : 0)) &
                                                   static_cast<uint32_t>(Sub32(speed, speedCopy))));
        const bool noLeanBits = (flagsAi & 0x300u) == 0; // a2, reused
        bool runAi = blend > 0;
        if (!runAi) {
            runAi = (static_cast<uint32_t>(n.ownerIdleRev) < 2u) && (flagsAi & 0x42u) == 2u;
        }
        if (runAi) {
            // 0x80073BFC..0x80073C64.
            if (bit13 != 0 && noLeanBits) {
                int16_t impact[3], axis0[3];
                for (int k = 0; k < 3; ++k) {
                    impact[k] = e.S16(ent::kImpact + 2u * static_cast<uint32_t>(k));
                    axis0[k] = e.S16(kMatrixRow0 + 2u * static_cast<uint32_t>(k));
                }
                const int32_t d = DotLcm(impact, axis0);
                e.SetU32(ent::kFlagsA, e.U32(ent::kFlagsA) | ((d > 0) ? 0x200u : 0x100u));
                e.SetU32(ent::kFlagsA, e.U32(ent::kFlagsA) & ~0x2000u);
            }
            BikeAimTarget(e, n.rider, n.stats, atanTable, n.handleTable);
            e.SetU32(ent::kFlagsA, e.U32(ent::kFlagsA) | 0x80000u);
            continue;
        }

        // 0x80073C68..0x80073D2C: the fade-out.
        // 0x80073C74/0x80073C7C: fade when the two flagsB bits are clear, OR when flagsA has
        // neither of bits 8/9 set.
        if ((e.U32(ent::kFlagsB) & 0x18000000u) == 0 || noLeanBits) {
            auto halveWord = [&e](uint32_t off) {
                const int32_t v = static_cast<int32_t>(e.U32(off));
                e.SetU32(off, static_cast<uint32_t>(
                                  Add32(v, static_cast<int32_t>(static_cast<uint32_t>(v) >> 31)) >> 1));
            };
            halveWord(ent::kLatAccel);
            halveWord(ent::kSteer);
            halveWord(kFadeA);
            e.SetU32(kFadeB, e.U32(ent::kSteerBias));
            const int32_t lean = e.S16(ent::kLean);
            e.SetU16(ent::kLean, static_cast<uint16_t>(
                                     static_cast<uint32_t>(Add32(lean, (lean < 0) ? 1 : 0) >> 1) &
                                     0xFFFFu));
            halveWord(ent::kIdleB);
            halveWord(ent::kIdleLean);
            if (n.rider != nullptr) {
                const int32_t rv = static_cast<int32_t>(n.rider->U32(ent::kLatAccel));
                n.rider->SetU32(ent::kLatAccel,
                                static_cast<uint32_t>(
                                    Add32(rv, static_cast<int32_t>(static_cast<uint32_t>(rv) >> 31)) >> 1));
            }
        }
    }
}

void BikeAimTarget(EntityView bike, EntityView* rider, const uint8_t* stats,
                   const int32_t* atanTable, const uint8_t* handleTable) {
    // 0x80072FE4..0x80073040.
    const uint32_t flagsA0 = bike.U32(ent::kFlagsA);
    const int32_t speed = static_cast<int32_t>(bike.U32(ent::kSpeed));
    const int32_t speedCopy = static_cast<int32_t>(bike.U32(ent::kSpeedCopy));
    const uint32_t s7 = (flagsA0 >> 27) & 1u;
    const uint32_t bit20 = (flagsA0 >> 20) & 1u;
    const bool fastFlag = (bike.U32(ent::kFlagsC) & 0x600u) != 0;
    const int32_t blend = Add32(speedCopy, static_cast<int32_t>(
                                               static_cast<uint32_t>(0 - (fastFlag ? 1 : 0)) &
                                               static_cast<uint32_t>(Sub32(speed, speedCopy))));
    const int32_t otherLateral = (rider != nullptr)
                                     ? static_cast<int32_t>(rider->U32(ent::kLatAccel))
                                     : static_cast<int32_t>(bike.U32(ent::kLatAccel));

    int32_t result = 0;  // s1
    int32_t s8 = 1;

    if (s7 != 0) {
        // ------------------------------------------------------------------ the geometric aim
        int32_t d[3];
        for (int i = 0; i < 3; ++i) {
            d[i] = Sub32(static_cast<int32_t>(bike.U32(ent::kAimFrom + 4u * static_cast<uint32_t>(i))),
                         static_cast<int32_t>(bike.U32(ent::kAimTo + 4u * static_cast<uint32_t>(i))));
        }
        auto dotAxis = [&d](EntityView& e, uint32_t axisOff) {
            int32_t acc = 0;
            for (int i = 0; i < 3; ++i) {
                const int32_t term = FixMul(
                    d[i], static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(
                                                   e.S16(axisOff + 2u * static_cast<uint32_t>(i))))
                                               << 4));
                acc = Add32(acc, term);
            }
            return acc;
        };
        const int32_t s4 = dotAxis(bike, ent::kAimAxis); // 0x80073088..0x8007313C
        int32_t lenSq = 0;                               // 0x800730D8..0x80073188
        for (int i = 0; i < 3; ++i) lenSq = Add32(lenSq, FixMul(d[i], d[i]));
        const int32_t half = lenSq >> 1; // `srav a1,a0,s8` with s8 == 1

        if ((bike.U32(ent::kFlagsA) & 0x40000u) != 0) {
            result = 0; // 0x80073254
        } else if (!(half < 17) && !(0x1FFF7FFF < half)) {
            // 0x800731C0..0x80073250.
            result = FixMul(SignedFixDiv(s4, half), blend);
            result = SymClamp(result, LoadS32(stats + 232));
        }

        const int32_t s5 = dotAxis(bike, ent::kHeading); // 0x80073258..0x800732E0
        const bool slow = speed < (LoadS32(stats + 224) >> 1); // sp+36
        // 0x800732F8..0x80073480: the inlined RatAtan2, identical to 0x80020018 including the
        // octant jump table at 0x8005B628.
        const int32_t angle = RatAtan2(s4, s5, atanTable);
        const uint32_t sign = static_cast<uint32_t>(angle >> 31);
        const int32_t absAngle = static_cast<int32_t>((sign + static_cast<uint32_t>(angle)) ^ sign);

        bool lowPath = true; // L_4B0
        if (s5 > 0) {
            const uint32_t flagsA = bike.U32(ent::kFlagsA);
            if ((flagsA & 0x400000u) == 0) {
                lowPath = false; // straight to L_5AC
            } else if (absAngle < 171) {
                // 0x800734E4: lined up.
                bike.SetU32(ent::kFlagsA, (flagsA | 0x01000000u) & 0xFFBFFFFFu);
                lowPath = false;
            }
        }
        if (lowPath) {
            // 0x800734B0..0x800734E0.
            if (slow) {
                result = LoadS32(stats + 232);
                if (s4 < 0) result = Sub32(0, result);
            }
            bike.SetU32(ent::kFlagsA, bike.U32(ent::kFlagsA) | 0x400000u);
        }
    } else if (bit20 != 0) {
        // ------------------------------------------------------------------ the speed-band aim
        const int32_t bandLow = LoadS32(stats + 356);
        int32_t base;
        if (blend < bandLow) {
            base = LoadS32(stats + 388);
        } else if (LoadS32(stats + 360) < blend) {
            base = LoadS32(stats + 392);
        } else {
            base = Add32(FixMul(Sub32(blend, bandLow), LoadS32(stats + 396)), LoadS32(stats + 388));
        }
        // 0x8007350C..0x8007351C: the per-handle word out of the runtime table.
        const int32_t handleScale = LoadS32(handleTable + 8u * bike.U16(ent::kHandle) + 4u);
        result = FixMul(base, handleScale);
    } else {
        s8 = 0; // 0x800735A4
        result = 0;
    }

    // 0x800735AC..0x80073668: the three-way "am I left of / right of / on the target" flag dance.
    if (s8 == 0) {
        bike.SetU32(ent::kFlagsA, bike.U32(ent::kFlagsA) & 0xFFDFFFFFu);
    } else {
        int32_t sel = (result < otherLateral) ? 256 : 0;
        if (otherLateral < result) sel = Add32(sel, 512);
        const bool selZero = (sel == 0);
        if (s7 != 0 && selZero) sel = Add32(sel, Sub32(256, sel)); // -> 256
        int32_t extra = 0;
        if (!selZero) {
            extra = ((bike.U32(ent::kFlagsA) & static_cast<uint32_t>(sel)) == 0) ? 1 : 0;
        }
        const uint32_t merged = bike.U32(ent::kFlagsA) | static_cast<uint32_t>(sel) |
                                (static_cast<uint32_t>(extra) << 7);
        bike.SetU32(ent::kFlagsA, merged);
        int32_t a0 = 768;
        if (sel == 512) a0 = 256;
        const uint32_t clear = (sel == 256) ? ~static_cast<uint32_t>(Sub32(a0, 256))
                                            : ~static_cast<uint32_t>(a0);
        bike.SetU32(ent::kFlagsA, merged & clear);
        bike.SetU32(ent::kFlagsA, bike.U32(ent::kFlagsA) | 0x200000u);
    }

    // 0x8007366C..0x800736CC: pick the reference angle and, if it saturates, give up for this frame.
    const uint32_t flagsA = bike.U32(ent::kFlagsA);
    bike.SetU32(ent::kSteerScratch, static_cast<uint32_t>(result)); // 0x80073680, a delay slot
    int32_t ref = 0x7FFFFFFF;
    if ((flagsA & 0x100u) != 0) {
        if ((flagsA & 0x200u) == 0) ref = static_cast<int32_t>(bike.U32(ent::kSteer));
    } else if ((flagsA & 0x200u) != 0) {
        ref = Sub32(0, static_cast<int32_t>(bike.U32(ent::kSteer)));
    }
    if (ref == 0x7FFFFFFF) {
        bike.SetU32(ent::kFlagsA, flagsA | 0x8000u); // 0x800736C4
        return;
    }

    // 0x800736D0..0x80073714.
    if ((bike.U32(ent::kFlagsA) & 0x80u) != 0) {
        if (ref > 0) bike.SetU32(ent::kFlagsB, bike.U32(ent::kFlagsB) | 2u);
        else bike.SetU32(ent::kFlagsB, bike.U32(ent::kFlagsB) & ~2u);
        bike.SetU32(ent::kFlagsA, bike.U32(ent::kFlagsA) & ~0x80u);
    }

    // 0x80073718..0x800737B0: the steering rate, by speed band, then negated for the other side.
    const int32_t bandLow = LoadS32(stats + 356);
    int32_t rateOut;
    if (blend < bandLow) {
        rateOut = Sub32(0, LoadS32(stats + 376));
    } else if (LoadS32(stats + 360) < blend) {
        rateOut = Sub32(0, LoadS32(stats + 380));
    } else {
        rateOut = Sub32(Sub32(0, LoadS32(stats + 376)),
                        FixMul(Sub32(blend, bandLow), LoadS32(stats + 384)));
    }
    bike.SetU32(ent::kSteerRate, static_cast<uint32_t>(rateOut));
    if ((bike.U32(ent::kFlagsA) & 0x200u) != 0) {
        bike.SetU32(ent::kSteerRate, static_cast<uint32_t>(Sub32(0, static_cast<int32_t>(
                                                                      bike.U32(ent::kSteerRate)))));
    }

    // 0x800737B4..0x8007382C: one more scale when flagsB bit 1 is set and the angle is inside
    // the -stats[+0x124] .. window.
    if ((bike.U32(ent::kFlagsB) & 2u) != 0) {
        const int32_t window = LoadS32(stats + 292);
        if (Sub32(0, window) < ref) {
            const int32_t k = Add32(FixMul(Add32(ref, window), LoadS32(stats + 288)), 0x10000);
            bike.SetU32(ent::kSteerRate,
                        static_cast<uint32_t>(FixMul(k, static_cast<int32_t>(bike.U32(ent::kSteerRate)))));
        }
    }
    bike.SetU32(ent::kFlagsA, bike.U32(ent::kFlagsA) & 0xFFFF7FFFu); // 0x80073834..0x80073840
}

void BikeApplySteering(EntityView bike, int32_t hasRider, EntityView* rider, int32_t extraLateral,
                       int32_t rate, const uint8_t* stats, const int32_t* atanTable,
                       const int16_t* sincos, uint8_t riderDefByte0) {
    // 0x800745B4..0x800745E0.
    const uint32_t flagsA0 = bike.U32(ent::kFlagsA);
    const int32_t speed = static_cast<int32_t>(bike.U32(ent::kSpeed));
    const int32_t speedCopy = static_cast<int32_t>(bike.U32(ent::kSpeedCopy));
    const uint32_t topBits = flagsA0 >> 27;                      // s5, masked to bit 0 below
    const bool fastFlag = (bike.U32(ent::kFlagsC) & 0x600u) != 0; // a3
    const int32_t blend = Add32(speedCopy, static_cast<int32_t>(
                                               static_cast<uint32_t>(0 - (fastFlag ? 1 : 0)) &
                                               static_cast<uint32_t>(Sub32(speed, speedCopy))));
    const uint32_t s5 = topBits & 1u;

    int32_t lateral; // s3 in the AI half; the servo half does not use it

    if ((flagsA0 & 0x108000u) == 0x8000u) {
        // ---------------------------------------------------------------- the timed steering move
        bool done = false; // "jump straight to BikeSteerLean" (L_8A0)

        if ((flagsA0 & 0x80u) != 0) { // 0x800745F4
            const int32_t seed = ((flagsA0 & 0x10000u) != 0)
                                     ? 0
                                     : Sub32(0, LoadS32(stats + 416));
            bike.SetU32(ent::kSteerPhase, static_cast<uint32_t>(seed));
            bike.SetU32(ent::kSteerRate, 0);
            bike.SetU32(ent::kFlagsA, bike.U32(ent::kFlagsA) & ~0x80u);
            bike.SetU32(ent::kFlagsB, bike.U32(ent::kFlagsB) & ~2u);
        }

        const int32_t phase = static_cast<int32_t>(bike.U32(ent::kSteerPhase)); // 0x80074644
        if (phase < 0) {
            // 0x8007464C..0x80074664: wind the phase up toward zero and stop there.
            const int32_t next = Add32(phase, rate);
            bike.SetU32(ent::kSteerPhase, static_cast<uint32_t>(next));
            const int32_t clipped = (next > 0) ? 0 : next;
            bike.SetU32(ent::kSteerPhase, static_cast<uint32_t>(clipped));
            done = true;
        } else if (static_cast<int32_t>(bike.U32(ent::kSteer)) == 0) { // 0x80074668
            done = true;
        }

        if (!done && phase == 0) {
            // 0x80074680..0x8007474C: start a new move. Pick the rate by speed band.
            const int32_t bandLow = LoadS32(stats + 356);
            int32_t moveRate;
            if (blend < bandLow) {
                moveRate = LoadS32(stats + 364);
            } else if (LoadS32(stats + 360) < blend) {
                moveRate = LoadS32(stats + 368);
            } else {
                moveRate = Add32(LoadS32(stats + 364),
                                 FixMul(Sub32(blend, bandLow), LoadS32(stats + 372)));
            }
            const int32_t steer = static_cast<int32_t>(bike.U32(ent::kSteer));
            bike.SetU32(ent::kSteerFrom, static_cast<uint32_t>(steer));
            const uint32_t sign = static_cast<uint32_t>(steer >> 31);
            const int32_t absSteer = static_cast<int32_t>((sign + static_cast<uint32_t>(steer)) ^ sign);
            const int32_t span = SignedFixDiv(absSteer, moveRate);
            bike.SetU32(ent::kSteerSpan, static_cast<uint32_t>(span));
            if (span < 9830) // 0x80074738, 9830 = 0.15
                bike.SetU32(ent::kFlagsA, bike.U32(ent::kFlagsA) | 0x20000u);
        }

        if (!done) {
            // 0x80074750..0x8007489C: advance the phase and shape the angle.
            const int32_t span = static_cast<int32_t>(bike.U32(ent::kSteerSpan));
            const int32_t next = Add32(static_cast<int32_t>(bike.U32(ent::kSteerPhase)), rate);
            bike.SetU32(ent::kSteerPhase, static_cast<uint32_t>(next));
            if (!(next < span)) {
                bike.SetU32(ent::kSteer, 0); // the move is over
            } else {
                const int32_t t = SignedFixDiv(next, span);
                const uint32_t flagsA = bike.U32(ent::kFlagsA);
                const int32_t from = static_cast<int32_t>(bike.U32(ent::kSteerFrom));
                int32_t a0, a1;
                if ((flagsA & 0x20000u) != 0) {
                    a0 = from;
                    a1 = Sub32(0x10000, t); // the linear ramp
                } else {
                    // 163 * FixMul(k, t) >>> 13, masked to an even halfword index, then +1 to
                    // land on the table's cosine entry.
                    const int32_t k = ((flagsA & 0x10000u) != 0) ? 0x0001921F : 0x0003243F;
                    const int32_t x = FixMul(k, t);
                    const int32_t five = Add32(static_cast<int32_t>(static_cast<uint32_t>(x) << 2), x);
                    const int32_t fortyOne =
                        Add32(static_cast<int32_t>(static_cast<uint32_t>(five) << 3), x);
                    const int32_t oneSixtyThree =
                        Sub32(static_cast<int32_t>(static_cast<uint32_t>(fortyOne) << 2), x);
                    const uint32_t idx =
                        ((static_cast<uint32_t>(oneSixtyThree) >> 13) & 0x1FFEu) + 1u;
                    const int32_t cosine =
                        static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(sincos[idx]))
                                             << 4);
                    if ((flagsA & 0x10000u) != 0) {
                        a0 = from;
                        a1 = cosine;
                    } else {
                        a0 = from >> 1;
                        a1 = Add32(cosine, 0x10000);
                    }
                }
                bike.SetU32(ent::kSteer, static_cast<uint32_t>(FixMul(a0, a1)));
            }
        }
    } else {
        // ---------------------------------------------------------------- the AI / drift path
        int32_t s1 = (hasRider != 0) ? static_cast<int32_t>(rider->U32(ent::kLatAccel))
                                     : static_cast<int32_t>(bike.U32(ent::kLatAccel));
        bike.SetU32(ent::kFlagsA, bike.U32(ent::kFlagsA) & 0xFFFCFFFFu); // 0x80074958

        int32_t limit; // s6
        if (static_cast<int32_t>(bike.U32(ent::kIdleLean)) != 0 || fastFlag) {
            // 0x8007496C..0x800749B4.
            bike.SetU32(ent::kSteerRate,
                        static_cast<uint32_t>(FixMul(static_cast<int32_t>(bike.U32(ent::kSteerRate)),
                                                     LoadS32(stats + 312))));
            limit = FixMul(LoadS32(stats + 232), LoadS32(stats + 312));
            const uint32_t sign = static_cast<uint32_t>(s1 >> 31);
            const int32_t mag = static_cast<int32_t>((sign + static_cast<uint32_t>(s1)) ^ sign);
            // 0x800749A0: `slt |s1|, s6` then `beqz` - the branch keeps |s1|, so this is a MAX.
            limit = (mag < limit) ? limit : mag;
        } else {
            // 0x800749B8..0x800749DC.
            limit = LoadS32(stats + 232);
            if (hasRider != 0 && s1 > 0) {
                bike.SetU32(ent::kSteerRate,
                            static_cast<uint32_t>(FixMul(static_cast<int32_t>(bike.U32(ent::kSteerRate)),
                                                         LoadS32(stats + 308))));
            }
        }
        if ((bike.U32(ent::kFlagsB) & 6u) == 4u) { // 0x800749E0
            bike.SetU32(ent::kSteerRate,
                        static_cast<uint32_t>(FixMul(static_cast<int32_t>(bike.U32(ent::kSteerRate)),
                                                     LoadS32(stats + 316))));
        }

        // 0x80074A08..0x80074A88.
        const int32_t target = static_cast<int32_t>(bike.U32(ent::kSteerScratch)); // a1
        lateral = Add32(FixMul(static_cast<int32_t>(bike.U32(ent::kSteerRate)), rate), s1);
        bool snapToTarget = false;
        if ((bike.U32(ent::kFlagsA) & 0x200000u) != 0) {
            if (target < s1) {
                if (!(target < lateral)) snapToTarget = true;
            } else if (!(lateral < target)) {
                snapToTarget = true;
            } else if (!(s1 < target) && !(target < lateral)) {
                snapToTarget = true;
            }
        }
        if (snapToTarget) lateral = Add32(lateral, Sub32(target, lateral));

        const int32_t half = LoadS32(stats + 224) >> 1; // s4, 0x80074A90 delay slot

        if (s5 != 0) {
            // 0x80074A94..0x80074BFC: the drift limit.
            uint32_t flagsA = bike.U32(ent::kFlagsA);
            if ((flagsA & 0x01000000u) != 0) {
                lateral = target;
                bike.SetU32(ent::kFlagsA, flagsA & 0xFEFFFFFFu);
            }
            if ((bike.U32(ent::kFlagsA) & 0x400000u) != 0) {
                bike.SetU32(ent::kDriftLimit, 0x00050000u);
            } else {
                const uint32_t sign = static_cast<uint32_t>(target >> 31);
                const int32_t mag = static_cast<int32_t>((sign + static_cast<uint32_t>(target)) ^ sign);
                if (lateral != target && !(mag < 26215) && (riderDefByte0 & 1u) == 0) {
                    const int32_t q = SignedFixDiv(Sub32(target, s1), rate);
                    const uint32_t qs = static_cast<uint32_t>(q >> 31);
                    const int32_t qm = static_cast<int32_t>((qs + static_cast<uint32_t>(q)) ^ qs);
                    const int32_t edge = LoadS32(stats + 376);
                    if (edge < qm) {
                        bike.SetU32(ent::kDriftLimit, static_cast<uint32_t>(LoadS32(stats + 356)));
                    } else if (LoadS32(stats + 380) < qm) {
                        const int32_t base = LoadS32(stats + 356);
                        const int32_t d = SignedFixDiv(Sub32(qm, edge), LoadS32(stats + 384));
                        bike.SetU32(ent::kDriftLimit, static_cast<uint32_t>(Add32(base, d)));
                    }
                    const int32_t current = static_cast<int32_t>(bike.U32(ent::kDriftLimit));
                    bike.SetU32(ent::kDriftLimit,
                                static_cast<uint32_t>((half < current) ? current : half));
                }
            }
        }

        if (hasRider != 0) rider->SetU32(ent::kLatAccel, static_cast<uint32_t>(lateral));
        // 0x80074C14: this tail always runs on the AI path, unlike the servo path where a zero
        // `extraLateral` returns before it.
        bike.SetU32(ent::kLatAccel,
                    static_cast<uint32_t>(SymClamp(Add32(lateral, extraLateral), limit)));
        BikeSolveSteer(bike, stats, atanTable, sincos);
        return;
    }

    // ---------------------------------------------------------------- the tail of the servo path
    BikeSteerLean(bike, stats, sincos);                       // 0x800748A0
    if (LoadS16(stats + 446) != 0) bike.SetU32(ent::kLatAccel, 0); // 0x800748A8, 0x800748B8
    if (hasRider != 0) rider->SetU32(ent::kLatAccel, bike.U32(ent::kLatAccel)); // 0x800748C4
    if (extraLateral == 0) return;                            // 0x800748DC
    const int32_t sum = Add32(static_cast<int32_t>(bike.U32(ent::kLatAccel)), extraLateral);
    bike.SetU32(ent::kLatAccel, static_cast<uint32_t>(sum));
    bike.SetU32(ent::kLatAccel, static_cast<uint32_t>(SymClamp(sum, LoadS32(stats + 232))));
    BikeSolveSteer(bike, stats, atanTable, sincos);           // 0x80074C4C
}

void BuildObbAlt(EntityView e, const uint8_t* kindTable, int32_t ownerIdleRev) {
    // 0x8008BD48: any state but 1 falls through to the ordinary builder.
    if (static_cast<int8_t>(e.bytes()[ent::kAltState]) != 1) {
        BuildObb(e, ownerIdleRev); // 0x8008BFDC
        return;
    }

    // 0x8008BD58..0x8008BD90: the centre is displaced by +0xF4.
    int32_t centre[3];
    for (int i = 0; i < 3; ++i) {
        centre[i] = Add32(static_cast<int32_t>(e.U32(ent::kObbCentre + 4u * static_cast<uint32_t>(i))),
                          static_cast<int32_t>(e.U32(ent::kAltOffset + 4u * static_cast<uint32_t>(i))));
    }

    // 0x8008BD94..0x8008BDE0: the along-B half extent is divided by 3 or by 2, toward zero.
    const uint32_t kind = e.U16(ent::kAltKind);
    const uint16_t selector = static_cast<uint16_t>(
        static_cast<uint32_t>(kindTable[8 * kind + 2]) |
        (static_cast<uint32_t>(kindTable[8 * kind + 3]) << 8));
    const int32_t halfZ = static_cast<int32_t>(e.U32(ent::kHalfZ));
    int32_t alongB;
    if (selector == 5) {
        const int64_t prod = static_cast<int64_t>(halfZ) * static_cast<int64_t>(0x55555556);
        const int32_t hi = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(prod) >> 32));
        alongB = Sub32(hi, halfZ >> 31);
    } else {
        alongB = Add32(halfZ, static_cast<int32_t>(static_cast<uint32_t>(halfZ) >> 31)) >> 1;
    }

    int16_t axis0[3], axis1[3], axis2[3];
    for (int i = 0; i < 3; ++i) {
        axis0[i] = e.S16(ent::kAltAxes + 0 + 2u * static_cast<uint32_t>(i));
        axis1[i] = e.S16(ent::kAltAxes + 6 + 2u * static_cast<uint32_t>(i));
        axis2[i] = e.S16(ent::kAltAxes + 12 + 2u * static_cast<uint32_t>(i));
    }
    int32_t A[3], B[3], C[3];
    Scale(static_cast<int32_t>(e.U32(ent::kHalfX)), axis0, A); // 0x8008BDF0
    Scale(alongB, axis1, B);                                   // 0x8008BE00
    Scale(static_cast<int32_t>(e.U32(ent::kHalfY)), axis2, C);  // 0x8008BE10

    // 0x8008BE24..0x8008BF7C, in the original's own slot order (sp+96 upward).
    int32_t corner[8][3];
    for (int i = 0; i < 3; ++i) {
        const int32_t ab = Add32(A[i], B[i]);
        const int32_t aB = Sub32(A[i], B[i]);
        const int32_t nAb = Add32(Sub32(0, A[i]), B[i]);
        const int32_t nAB = Sub32(Sub32(0, A[i]), B[i]);
        corner[0][i] = Sub32(nAb, C[i]);
        corner[1][i] = Sub32(ab, C[i]);
        corner[2][i] = Add32(ab, C[i]);
        corner[3][i] = Add32(nAb, C[i]);
        corner[4][i] = Sub32(nAB, C[i]);
        corner[5][i] = Sub32(aB, C[i]);
        corner[6][i] = Add32(aB, C[i]);
        corner[7][i] = Add32(nAB, C[i]);
    }

    // 0x8008BF80..0x8008BFC4.
    for (int k = 0; k < 8; ++k) {
        for (int i = 0; i < 3; ++i) {
            e.SetU32(ent::kObbCorners + 12u * static_cast<uint32_t>(k) + 4u * static_cast<uint32_t>(i),
                     static_cast<uint32_t>(Add32(corner[k][i], centre[i])));
        }
    }

    // 0x8008BFC8..0x8008BFD8.
    e.bytes()[ent::kAltState] =
        static_cast<uint8_t>(static_cast<uint32_t>(static_cast<int8_t>(e.bytes()[ent::kAltState])) << 1);
}

void BikeRiderPose(EntityView bike, int32_t dt, int32_t bikeOwnerIdleRev, EntityView* rider,
                   int32_t riderOwnerIdleRev, const int32_t* atanTable, const int16_t* sincos) {
    const bool hasRider = (rider != nullptr);

    // 0x80080808: the pose arithmetic is gated on the mode byte; the rest of the function is not.
    if (static_cast<int8_t>(bike.bytes()[ent::kMode]) < 2) {
        // 0x8008081C..0x80080858: step = (2608 * FixMul(speed, dt)) >> 16.
        const int32_t m = FixMul(static_cast<int32_t>(bike.U32(ent::kSpeed)), dt);
        const int32_t four = static_cast<int32_t>(static_cast<uint32_t>(m) << 2);
        int32_t acc = Add32(static_cast<int32_t>(static_cast<uint32_t>(m) << 4), four);      // 20
        acc = Add32(static_cast<int32_t>(static_cast<uint32_t>(acc) << 3), four);            // 164
        acc = Sub32(static_cast<int32_t>(static_cast<uint32_t>(acc) << 2), four);            // 652
        int32_t step = static_cast<int32_t>(static_cast<uint32_t>(acc) << 2) >> 16;          // 2608

        // 0x8008085C..0x80080884, and the identical block at 0x80080978: the +0x348 ratchet.
        auto poseCRatchet = [&bike](int32_t value) {
            const int32_t signed348 = bike.S16(ent::kPoseC);
            const uint16_t raw = bike.U16(ent::kPoseC);
            if (!(signed348 < 23)) {
                bike.SetU16(ent::kPoseC, static_cast<uint16_t>(raw - 34u));
            } else if (signed348 == 0) {
                bike.SetU16(ent::kPoseC, static_cast<uint16_t>(static_cast<uint32_t>(value) & 0xFFFFu));
            }
        };

        bool doRiderClamp = false;
        if ((bike.U32(ent::kFlagsC) & 0x7FFu) != 0) {
            poseCRatchet(step);
            // 0x80080884..0x800808C0. Both accumulators move by the SAME amount: the original
            // overwrites a1 with +0x348 at 0x80080890 before using it twice.
            const uint16_t c = bike.U16(ent::kPoseC);
            bike.SetU16(ent::kPoseA, static_cast<uint16_t>(bike.U16(ent::kPoseA) - c));
            bike.SetU16(ent::kPoseB, static_cast<uint16_t>(bike.U16(ent::kPoseB) - c));
            if (hasRider) {
                rider->SetU16(ent::kPoseA, static_cast<uint16_t>(rider->U16(ent::kPoseA) - c));
            }
        } else {
            // 0x800808C4..0x8008093C: four arms, each picking how A and B move.
            uint16_t a = bike.U16(ent::kPoseA);
            uint16_t b = bike.U16(ent::kPoseB);
            const uint16_t s = static_cast<uint16_t>(static_cast<uint32_t>(step) & 0xFFFFu);
            const int32_t idleLean = static_cast<int32_t>(bike.U32(ent::kIdleLean));
            if ((bike.U32(ent::kFlagsB) & 0x08000000u) != 0) {
                a = static_cast<uint16_t>(a - s);
                b = static_cast<uint16_t>(b - 170u);
            } else if (idleLean == 0) {
                a = static_cast<uint16_t>(a - s);
                b = static_cast<uint16_t>(b - s);
            } else if (idleLean > 0) {
                a = static_cast<uint16_t>(a - 22u);
                b = static_cast<uint16_t>(b - s);
            } else {
                a = static_cast<uint16_t>(a - s);
                b = static_cast<uint16_t>(b - 22u);
            }
            bike.SetU16(ent::kPoseA, a);
            bike.SetU16(ent::kPoseB, b);
            if (hasRider) {
                if (static_cast<int32_t>(bike.U32(ent::kSteer)) >= 0) {
                    // 0x80080958..0x80080974.
                    rider->SetU16(ent::kPoseA, static_cast<uint16_t>(rider->U16(ent::kPoseA) - s));
                    bike.SetU16(ent::kPoseC, 0);
                } else {
                    poseCRatchet(step);
                    const uint16_t c = bike.U16(ent::kPoseC);
                    rider->SetU16(ent::kPoseA, static_cast<uint16_t>(rider->U16(ent::kPoseA) - c));
                }
                doRiderClamp = true;
            }
        }

        // 0x800809CC..0x800809E4: bring the rider's +0x344 back above -4096.
        if (doRiderClamp) {
            while (rider->S16(ent::kPoseA) < -4096)
                rider->SetU16(ent::kPoseA, static_cast<uint16_t>(rider->U16(ent::kPoseA) + 4096u));
        }
        // 0x800809E8..0x80080A4C: the same for the bike's own two accumulators.
        for (uint32_t off : {ent::kPoseA, ent::kPoseB}) {
            int32_t held = bike.S16(off);
            if (held < -4096) {
                int32_t next = Add32(held, 4096);
                for (;;) {
                    bike.SetU16(off, static_cast<uint16_t>(static_cast<uint32_t>(next) & 0xFFFFu));
                    held = next;
                    // The test re-reads the halfword the store just made, sign-extended.
                    if (!(static_cast<int32_t>(static_cast<int16_t>(
                              static_cast<uint16_t>(static_cast<uint32_t>(next) & 0xFFFFu))) < -4096))
                        break;
                    next = Add32(held, 4096);
                }
            }
        }
    }

    // 0x80080A50..0x80080A9C: rebuild the boxes and pin the rider to the bike.
    BuildObb(bike, bikeOwnerIdleRev);
    if (hasRider) {
        for (int i = 0; i < 9; ++i) {
            rider->SetU16(ent::kAxes + 2u * static_cast<uint32_t>(i),
                          bike.U16(ent::kAxes + 2u * static_cast<uint32_t>(i)));
        }
        int32_t centre[3], out[3];
        int16_t axis0[3];
        for (int i = 0; i < 3; ++i) {
            centre[i] = static_cast<int32_t>(bike.U32(ent::kObbCentre + 4u * static_cast<uint32_t>(i)));
            axis0[i] = bike.S16(ent::kAxes + 2u * static_cast<uint32_t>(i));
        }
        const int32_t offset = Add32(static_cast<int32_t>(bike.U32(ent::kHalfX)),
                                     static_cast<int32_t>(rider->U32(ent::kHalfX)));
        MulAdd(centre, axis0, offset, out);
        for (int i = 0; i < 3; ++i)
            rider->SetU32(ent::kObbCentre + 4u * static_cast<uint32_t>(i), static_cast<uint32_t>(out[i]));
        BuildObb(*rider, riderOwnerIdleRev);
    }

    // 0x80080AA0..0x80080AF8: the heading angle and its sine and cosine.
    const int32_t fx = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(bike.S16(ent::kFacingX)))
                                            << 4);
    const int32_t fz = static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(bike.S16(ent::kFacingZ)))
                                            << 4);
    const int32_t angle = RatAtan2(fx, fz, atanTable);
    bike.SetU32(ent::kFacing, static_cast<uint32_t>(angle));
    const uint32_t index = static_cast<uint32_t>(angle) & 0xFFFu;
    const int32_t cosine = static_cast<int32_t>(static_cast<uint32_t>(
        static_cast<int32_t>(sincos[2 * index + 1])) << 4);
    bike.SetU32(ent::kFacingCos, static_cast<uint32_t>(cosine));
    const int32_t sine = static_cast<int32_t>(static_cast<uint32_t>(
        static_cast<int32_t>(sincos[2 * index])) << 4);
    bike.SetU32(ent::kFacingSin, static_cast<uint32_t>(sine));
}

void BuildObb(EntityView e, int32_t ownerIdleRev) {
    const uint32_t handle = e.U16(ent::kHandle);
    const uint32_t pool = handle >> 5;
    int32_t halfZ = static_cast<int32_t>(e.U32(ent::kHalfZ));
    const int32_t halfX = static_cast<int32_t>(e.U32(ent::kHalfX));

    // 0x8008BA50..0x8008BA98: the crouch. Only a motorcycle of class < 18 whose owner's +0x25C has
    // reached 2 gets its along-B half extent squashed to 5/8, rounded toward zero.
    // Both comparisons are `sltiu`, i.e. unsigned, which matters for a negative +0x25C.
    if (pool == 0 && e.U32(ent::kClass) < 18u && static_cast<uint32_t>(ownerIdleRev) >= 2u) {
        const int32_t five = Add32(static_cast<int32_t>(static_cast<uint32_t>(halfZ) << 2), halfZ);
        halfZ = (five >= 0) ? (five >> 3) : (Add32(five, 7) >> 3);
    }
    const int32_t halfY = static_cast<int32_t>(e.U32(ent::kHalfY));

    int16_t axis0[3], axis1[3], axis2[3];
    for (int i = 0; i < 3; ++i) {
        axis0[i] = e.S16(ent::kAxes + 0 + 2u * static_cast<uint32_t>(i));
        axis1[i] = e.S16(ent::kAxes + 6 + 2u * static_cast<uint32_t>(i));
        axis2[i] = e.S16(ent::kAxes + 12 + 2u * static_cast<uint32_t>(i));
    }
    int32_t A[3], B[3], C[3];
    Scale(halfX, axis0, A); // 0x8008BAA4
    Scale(halfZ, axis1, B); // 0x8008BABC
    Scale(halfY, axis2, C); // 0x8008BACC

    // 0x8008BAD4..0x8008BBFC. The eight corners, in the original's own slot order.
    int32_t corner[8][3];
    for (int i = 0; i < 3; ++i) {
        const int32_t negA = Sub32(0, A[i]);
        corner[0][i] = Sub32(negA, C[i]);                 // sp+80
        corner[1][i] = Sub32(A[i], C[i]);                 // sp+92
        corner[2][i] = Add32(A[i], C[i]);                 // sp+104
        corner[3][i] = Add32(negA, C[i]);                 // sp+116
        corner[4][i] = Sub32(Sub32(negA, B[i]), C[i]);    // sp+128
        corner[5][i] = Sub32(Sub32(A[i], B[i]), C[i]);    // sp+140
        corner[6][i] = Add32(Sub32(A[i], B[i]), C[i]);    // sp+152
        corner[7][i] = Add32(Sub32(negA, B[i]), C[i]);    // sp+164
    }

    // 0x8008BC00..0x8008BC5C: the two per-pool adjustments to B and to how many corners move.
    bool shiftAll = false;   // s3
    bool shiftFirst4 = false; // s4
    if (pool == 3) {
        shiftFirst4 = true;
        Scale(8192, axis1, B);                  // 0x8008BC20
    } else if (pool == 4 && (handle & 0x1Fu) >= 30u) {
        shiftAll = true;
        const int32_t half[3] = {B[0], B[1], B[2]};
        Scale32(0x8000, half, B);               // 0x8008BC54
    }

    // 0x8008BC6C..0x8008BD04: shift, add the centre, store.
    int32_t centre[3];
    for (int i = 0; i < 3; ++i) centre[i] = static_cast<int32_t>(e.U32(ent::kObbCentre + 4u * static_cast<uint32_t>(i)));
    for (int k = 0; k < 8; ++k) {
        if (shiftAll || (shiftFirst4 && k < 4)) {
            for (int i = 0; i < 3; ++i) corner[k][i] = Add32(corner[k][i], B[i]);
        }
        for (int i = 0; i < 3; ++i) {
            e.SetU32(ent::kObbCorners + 12u * static_cast<uint32_t>(k) + 4u * static_cast<uint32_t>(i),
                     static_cast<uint32_t>(Add32(corner[k][i], centre[i])));
        }
    }
}

// ============================================================ RASHCDG 0x80079B20, the engine
namespace {

// MIPS `div`, the low half only, with the R3000's defined corner cases - the engine divides by a
// stat word that nothing stops from being zero (0x8007A0E0, 0x8007A228, 0x8007A3A0).
int32_t MipsDivLo(int32_t n, int32_t d) {
    if (d == 0) return (n >= 0) ? -1 : 1;
    if (static_cast<uint32_t>(n) == 0x80000000u && d == -1) return static_cast<int32_t>(0x80000000u);
    return n / d;
}

// `*(s32*)((u8*)stats + byteOffset)`, with the original's 32-bit address arithmetic: the index into
// the torque curve is unbounded, so a negative or very large offset is a real input, not a bug.
int32_t StatWord(const uint8_t* stats, int32_t byteOffset) {
    return LoadS32(stats + static_cast<std::ptrdiff_t>(byteOffset));
}

// 0x8007A09C..0x8007A19C, and again instruction-for-instruction at 0x8007A1FC (the gear below) and
// 0x8007A374 (the gear above): the torque the engine reads out of the stat block's curve for a
// given rev count. The caller multiplies the result by the gear ratio.
int32_t EngineTorque(int32_t revs, const uint8_t* stats) {
    if (!(revs < StatWord(stats, 180))) return 0;             // 0x8007A0A8, `v1` stays zero
    const int32_t x = Sub32(revs, StatWord(stats, 176));      // 0x8007A0BC
    if (x < 6553) return StatWord(stats, 60);                 // 0x8007A0C0, curve entry 0
    const int32_t step = StatWord(stats, 184);
    const int32_t idx = MipsDivLo(x, step);                   // 0x8007A0E0, a real `div`
    const int32_t at = static_cast<int32_t>(static_cast<uint32_t>(idx) << 2);
    // 0x8007A130: the fractional part is the signed 16.16 quotient minus the integer one.
    const int32_t frac = Sub32(SignedFixDiv(x, step),
                               static_cast<int32_t>(static_cast<uint32_t>(idx) << 16));
    if (frac < 65) return StatWord(stats, Add32(at, 60));     // 0x8007A134
    const int32_t hi = StatWord(
        stats, Add32(static_cast<int32_t>(static_cast<uint32_t>(Add32(idx, 1)) << 2), 60));
    const int32_t lo = StatWord(stats, Add32(at, 60));
    const int32_t d = Sub32(hi, lo);
    if (d < 6) return hi;                                     // 0x8007A170: too flat to interpolate
    int32_t m = FixMul(frac, static_cast<int32_t>(static_cast<uint32_t>(d) << 10));
    if (m < 0) m = Add32(m, 1023);                            // 0x8007A184, round toward zero
    return Add32(lo, m >> 10);
}

} // namespace

void BikeEngineStep(BikeEngineNode* nodes, size_t count, int32_t dt, uint32_t& randSeed,
                    const uint16_t* atanTable, EngineSound& sound) {
    for (size_t ni = 0; ni < count; ++ni) {
        EntityView bike = nodes[ni].bike;
        const uint8_t* const stats = nodes[ni].stats;

        // ---------------------------------------------------------------- 0x80079B64
        const int32_t gear0 = static_cast<int8_t>(bike.bytes()[ent::kGear]);
        int32_t ratio = StatWord(stats, Add32(static_cast<int32_t>(
                                                  static_cast<uint32_t>(gear0) << 2), 20));
        const bool revving =
            (bike.U32(ent::kThrottleVel) != 0) || (bike.U32(ent::kThrottleAccel) != 0);

        int32_t mayShift = 0; // `s4`
        int32_t crossed = 0;  // `s1` in the second half: the throttle move reached/crossed zero

        // ---------------------------------------------------------------- 0x80079B98
        // The standing-still arm. Seven gates, in the original's order.
        if (static_cast<int32_t>(bike.U32(ent::kSpeedCopy)) < 13107 &&
            (bike.U32(ent::kFlagsA) & 0x42u) == 0x42u && (bike.U32(ent::kFlagsC) & 0x7FFu) == 0 &&
            static_cast<uint32_t>(nodes[ni].ownerIdleRev) < 2u &&
            (bike.U32(ent::kFlagsB) & 0x40000u) == 0 && !revving &&
            (bike.U32(ent::kFlagsA) & 0x08000000u) == 0) {
            BikeIdleStep(bike, dt, stats, randSeed); // 0x80079C18
        } else {
            // 0x80079C28: bit 27 of flagsB is the idle model's own "I ran last frame" latch.
            const uint32_t fb = bike.U32(ent::kFlagsB);
            const uint32_t cleared = fb & 0xF7FFFFFFu;
            bike.SetU32(ent::kFlagsB, cleared);
            if (fb & 0x08000000u) {
                if (!revving || static_cast<int32_t>(bike.U32(ent::kThrottle)) >= 0) {
                    if (bike.U32(ent::kFlagsA) & 2u) // 0x80079C64
                        bike.SetU32(ent::kFlagsB, cleared | 0x10000000u);
                    bike.SetU32(ent::kIdleTimer, 0);
                    bike.SetU32(ent::kThrottle, 0);
                }
            }
        }

        // ---------------------------------------------------------------- 0x80079C90
        // The idle model sets bit 27 again, and that is what skips the whole drivetrain.
        if ((bike.U32(ent::kFlagsB) & 0x08000000u) == 0) {
            const int32_t revs0 = static_cast<int32_t>(bike.U32(ent::kRevs));
            uint32_t flagsA = bike.U32(ent::kFlagsA);
            if (!(revs0 < StatWord(stats, 180))) flagsA |= 0x800000u; // 0x80079CBC
            bike.SetU32(ent::kFlagsA, flagsA);

            if (flagsA & 0x800000u) {
                // ------------------------------------------------------ 0x80079CD0, the limiter
                const int32_t hardCut = ((bike.U32(ent::kFlagsC) & 0x600u) != 0) ? 1 : 0;
                const uint32_t pickMask = (flagsA & 2u) ? 0xFFFFFFFFu : 0u;
                const uint32_t bias = (flagsA & 0x40u) ? 0xD8F00000u : 0xFF9C0000u;
                const uint32_t pick = (flagsA & 0x40u) ? 0x4E200000u : 0x27740000u;
                uint32_t t = bias + (pickMask & pick);
                t += 0x27100000u;
                t &= static_cast<uint32_t>(0u - static_cast<uint32_t>(hardCut));
                const int32_t rate = static_cast<int32_t>(t - 0x27100000u);
                const int32_t revs = Add32(revs0, FixMul(rate, dt)); // 0x80079D28
                mayShift = 0;
                bike.SetU32(ent::kRevs, static_cast<uint32_t>(revs));
                if (hardCut != 0) {
                    // 0x80079D44: clamp into [stats[+0xB0], stats[+0xB4]], both tests against the
                    // UNCLAMPED value, exactly as the compiler emitted it.
                    const int32_t lo = StatWord(stats, 176);
                    const int32_t hi = StatWord(stats, 180);
                    int32_t v = Add32(revs, static_cast<int32_t>(
                                                static_cast<uint32_t>(Sub32(revs, lo) >> 31) &
                                                static_cast<uint32_t>(Sub32(lo, revs))));
                    v = Add32(v, static_cast<int32_t>(static_cast<uint32_t>(Sub32(hi, revs) >> 31) &
                                                      static_cast<uint32_t>(Sub32(hi, revs))));
                    bike.SetU32(ent::kRevs, static_cast<uint32_t>(v));
                } else {
                    // 0x80079D78: the limiter releases as soon as the revs fall below the ceiling.
                    const int32_t hi = StatWord(stats, 180);
                    if (revs < hi) {
                        mayShift = 1;
                        bike.SetU32(ent::kRevs, static_cast<uint32_t>(Sub32(hi, 6553)));
                        bike.SetU32(ent::kFlagsA, bike.U32(ent::kFlagsA) & 0xFF7FFFFFu);
                    }
                }
                // 0x80079DA8: while limited, the revs drive the speed.
                bike.SetU32(ent::kSpeedCopy,
                            static_cast<uint32_t>(SignedFixDiv(
                                static_cast<int32_t>(bike.U32(ent::kRevs)), ratio)));
            } else {
                // ------------------------------------------------------ 0x80079DFC
                bool storeFlagsB = false;
                uint32_t newFlagsB = 0;
                if (flagsA & 2u) {
                    if (static_cast<int32_t>(bike.U32(ent::kSpeedCopy)) < 13107) {
                        const uint32_t fb = bike.U32(ent::kFlagsB);
                        if (fb & 0x10000000u) {
                            newFlagsB = fb | 1u; // 0x80079E28
                            storeFlagsB = true;
                        } else if (static_cast<int32_t>(bike.U32(ent::kEngWish)) >= 0) {
                            // 0x80079E38: straight to the 0x80079FD4 test, flagsB untouched.
                        } else {
                            // 0x80079E3C: solve the curve backwards for the demanded torque.
                            const int32_t demand = SignedFixDiv(
                                FixMul(Sub32(0, static_cast<int32_t>(bike.U32(ent::kEngWish))),
                                       0x11999),
                                ratio);
                            const int32_t step = StatWord(stats, 184);
                            int32_t acc = StatWord(stats, 176);
                            int32_t i = 0;
                            while (i < 29 && !(demand < StatWord(stats, 60 + 4 * i))) {
                                acc = Add32(acc, step);
                                ++i;
                            }
                            if (i >= 29) {
                                bike.SetU32(ent::kRevs, 0); // 0x80079FB4
                            } else if (i == 0) {
                                bike.SetU32(ent::kRevs, static_cast<uint32_t>(acc));
                            } else {
                                const int32_t c = StatWord(stats, 60 + 4 * i);
                                const int32_t p = StatWord(stats, 60 + 4 * (i - 1));
                                const int32_t f = SignedFixDiv(Sub32(c, demand), Sub32(c, p));
                                bike.SetU32(ent::kRevs,
                                            static_cast<uint32_t>(Sub32(acc, FixMul(f, step))));
                            }
                            newFlagsB = bike.U32(ent::kFlagsB) | 1u; // 0x80079FB8
                            storeFlagsB = true;
                        }
                    } else {
                        // 0x80079E14: moving, so the rev target is left alone.
                    }
                } else {
                    newFlagsB = bike.U32(ent::kFlagsB) & 0xEFFFFFFFu; // 0x80079FC4
                    storeFlagsB = true;
                }
                if (storeFlagsB) bike.SetU32(ent::kFlagsB, newFlagsB); // 0x80079FD0

                // 0x80079FD4
                if (bike.U32(ent::kFlagsB) & 1u) {
                    const int32_t want = SignedFixDiv(
                        static_cast<int32_t>(bike.U32(ent::kRevs)), ratio);
                    const int32_t have = static_cast<int32_t>(bike.U32(ent::kSpeedCopy));
                    if (have < want && (bike.U32(ent::kFlagsA) & 2u)) {
                        mayShift = 1; // 0x8007A050, straight to the curve
                    } else {
                        bike.SetU32(ent::kFlagsB, bike.U32(ent::kFlagsB) & 0xEFFFFFFEu);
                        mayShift = 1;
                    }
                } else {
                    // 0x8007A070: the road drives the engine, not the other way round.
                    bike.SetU32(ent::kRevs,
                                static_cast<uint32_t>(FixMul(
                                    static_cast<int32_t>(bike.U32(ent::kSpeedCopy)), ratio)));
                    mayShift = 1;
                }
            }
        }

        // ---------------------------------------------------------------- 0x8007A09C
        int32_t drive = FixMul(ratio, EngineTorque(static_cast<int32_t>(bike.U32(ent::kRevs)), stats));

        if (mayShift != 0) {
            // 0x8007A1C0: would the gear below make more drive at the same road speed?
            const int32_t gear = static_cast<int8_t>(bike.bytes()[ent::kGear]);
            if (gear > 0) {
                const int32_t r = StatWord(
                    stats, Add32(static_cast<int32_t>(static_cast<uint32_t>(gear - 1) << 2), 20));
                const int32_t revs = FixMul(static_cast<int32_t>(bike.U32(ent::kSpeedCopy)), r);
                const int32_t d = FixMul(r, EngineTorque(revs, stats));
                if (drive < d) { // 0x8007A2F8
                    drive = d;
                    bike.SetU32(ent::kRevs, static_cast<uint32_t>(revs));
                    bike.bytes()[ent::kGear] = static_cast<uint8_t>(bike.bytes()[ent::kGear] - 1u);
                }
            }
            // 0x8007A318: and the gear above - which also needs flagsA bit 1.
            const int32_t top = static_cast<int32_t>(stats[444]) - 1;
            const int32_t gearNow = static_cast<int8_t>(bike.bytes()[ent::kGear]);
            if (gearNow < top) {
                const int32_t r = StatWord(
                    stats, Add32(static_cast<int32_t>(static_cast<uint32_t>(gearNow + 1) << 2), 20));
                const int32_t revs = FixMul(static_cast<int32_t>(bike.U32(ent::kSpeedCopy)), r);
                const int32_t d = FixMul(r, EngineTorque(revs, stats));
                if (drive < d && (bike.U32(ent::kFlagsA) & 2u)) { // 0x8007A478, 0x8007A48C
                    drive = d;
                    bike.SetU32(ent::kRevs, static_cast<uint32_t>(revs));
                    bike.bytes()[ent::kGear] = static_cast<uint8_t>(bike.bytes()[ent::kGear] + 1u);
                }
            }
        }

        // ---------------------------------------------------------------- 0x8007A4AC
        bike.SetU32(ent::kDrive, static_cast<uint32_t>(drive));
        if (bike.U32(ent::kFlagsB) & 0x200u)
            bike.SetU32(ent::kDrive, static_cast<uint32_t>(FixMul(StatWord(stats, 336), drive)));
        if (bike.U32(ent::kFlagsC) & 0x600u) continue; // 0x8007A4DC: crashing, nothing else runs

        // ---------------------------------------------------------------- 0x8007A4E4
        // The throttle integrator: +0x268 is the level, +0x26C its rate, +0x270 its acceleration.
        if (bike.U32(ent::kFlagsB) & 0x200000u) {
            bike.SetU32(ent::kThrottleVel, 0);
            bike.SetU32(ent::kThrottleAccel, 0);
            bike.SetU32(ent::kFlagsB, bike.U32(ent::kFlagsB) & 0xFFFFB79Fu);
            bike.SetU32(ent::kFlagsA, bike.U32(ent::kFlagsA) & 0xFFFFF7FFu);
            // 0x8007A520 jumps straight to 0x8007A5D4.
        } else {
            const int32_t accel = static_cast<int32_t>(bike.U32(ent::kThrottleAccel));
            bool atA5A4 = false;
            if (accel != 0) {
                // 0x8007A534
                const int32_t vel = static_cast<int32_t>(bike.U32(ent::kThrottleVel));
                const int32_t sum = Add32(vel, FixMul(accel, dt));
                bool reached;
                if (sum > 0) reached = (vel < 0);
                else if (vel > 0) reached = true;
                else if (sum < 0) reached = false;
                else reached = (vel < 0);
                crossed = reached ? 1 : 0;
                if (bike.U32(ent::kThrottle) == 0) crossed = 0; // 0x8007A574
                bike.SetU32(ent::kThrottleVel, static_cast<uint32_t>(sum));
                atA5A4 = true;
            } else {
                // 0x8007A588
                crossed = 0;
                if (bike.U32(ent::kThrottle) != 0) {
                    crossed = (bike.U32(ent::kThrottleVel) == 0) ? 1 : 0;
                    atA5A4 = true;
                }
            }
            if (!(atA5A4 && bike.U32(ent::kThrottle) != 0)) { // 0x8007A5AC
                // 0x8007A5B4
                if (bike.U32(ent::kThrottleVel) == 0) {
                    bike.SetU32(ent::kThrottleAccel, 0);
                    bike.SetU32(ent::kFlagsB, bike.U32(ent::kFlagsB) & 0xFFFFF7FFu);
                }
            }
        }

        // 0x8007A5D4: with neither a rate nor an acceleration there is nothing to integrate.
        bool integrate = true;
        if (bike.U32(ent::kThrottleVel) == 0 && bike.U32(ent::kThrottleAccel) == 0) integrate = false;

        bool speedFromLevel = false;
        if (integrate) {
            // ------------------------------------------------------------ 0x8007A5F4
            const int32_t accel = static_cast<int32_t>(bike.U32(ent::kThrottleAccel));
            int32_t sum;
            if (accel != 0) {
                const int32_t elapsed = Add32(static_cast<int32_t>(bike.U32(ent::kThrottleTime)), dt);
                bike.SetU32(ent::kThrottleTime, static_cast<uint32_t>(elapsed));
                const int32_t p = FixMul(accel, elapsed);
                const int32_t half = Add32(static_cast<int32_t>(static_cast<uint32_t>(p) >> 31), p) >> 1;
                const int32_t q = FixMul(
                    elapsed, Sub32(static_cast<int32_t>(bike.U32(ent::kThrottleVel)), half));
                sum = Add32(static_cast<int32_t>(bike.U32(ent::kThrottleFrom)), q);
            } else {
                // 0x8007A640
                sum = Add32(static_cast<int32_t>(bike.U32(ent::kThrottle)),
                            FixMul(static_cast<int32_t>(bike.U32(ent::kThrottleVel)), dt));
            }

            // 0x8007A658: the move has reached or crossed zero.
            const int32_t level = static_cast<int32_t>(bike.U32(ent::kThrottle));
            bool finish;
            if (sum > 0) finish = (level < 0);
            else if (level > 0) finish = true;
            else if (sum < 0) finish = false;
            else finish = (level < 0);

            if (finish) {
                // 0x8007A688: stop the move, and tell the world about it.
                const int32_t x = static_cast<int32_t>(bike.U32(ent::kObbCentre + 0));
                const int32_t z = static_cast<int32_t>(bike.U32(ent::kObbCentre + 8));
                bike.SetU32(ent::kThrottleAccel, 0);
                bike.SetU32(ent::kThrottleVel, 0);
                bike.SetU32(ent::kThrottle, 0);
                bike.SetU32(ent::kFlagsB, bike.U32(ent::kFlagsB) & 0xFFFFBF9Fu);
                bike.SetU32(ent::kFlagsA, bike.U32(ent::kFlagsA) & 0xFFFFF7FFu);
                sound.PlaySound3D(x, z, 54, 0); // SLUS_010.53 0x80017BA0, NOT ported - see the header
            } else {
                bike.SetU32(ent::kThrottle, static_cast<uint32_t>(sum)); // 0x8007A6D0
            }

            // 0x8007A6D4
            if (bike.U32(ent::kThrottle) != 0) {
                const int32_t vel = static_cast<int32_t>(bike.U32(ent::kThrottleVel));
                const int32_t lvl = static_cast<int32_t>(bike.U32(ent::kThrottle));
                if ((lvl ^ vel) >= 0 && (bike.U32(ent::kFlagsC) & 0xCu) == 0) {
                    const int32_t acc = static_cast<int32_t>(bike.U32(ent::kThrottleAccel));
                    if (acc == 0) {
                        bike.SetU32(ent::kThrottleVel, static_cast<uint32_t>(Sub32(0, vel)));
                    } else if ((acc ^ vel) >= 0) {
                        bike.SetU32(ent::kThrottleAccel, static_cast<uint32_t>(Sub32(0, acc)));
                    }
                }
                // 0x8007A734
                if (bike.U32(ent::kThrottle) != 0) {
                    const int32_t v = static_cast<int32_t>(bike.U32(ent::kThrottleVel));
                    if (v < 0) {
                        const int32_t drop = FixMul(v, static_cast<int32_t>(bike.U32(ent::kHalfY)));
                        bike.SetU32(ent::kSpeed,
                                    static_cast<uint32_t>(Sub32(
                                        static_cast<int32_t>(bike.U32(ent::kSpeedCopy)), drop)));
                        speedFromLevel = true;
                    }
                }
            }
        }
        // 0x8007A774 / 0x8007A77C: THE BIKE'S SPEED.
        if (!speedFromLevel) bike.SetU32(ent::kSpeed, bike.U32(ent::kSpeedCopy));

        // ---------------------------------------------------------------- 0x8007A780
        const uint32_t fb = bike.U32(ent::kFlagsB);
        if (fb & 0x40u) {
            if (crossed != 0) {
                bool done = false;
                if (58594 < static_cast<int32_t>(bike.U32(ent::kSpeedCopy)) &&
                    static_cast<int32_t>(bike.U32(ent::kEngHold)) > 0) {
                    bike.SetU32(ent::kThrottleVel, 0); // 0x8007A7C0
                    bike.SetU32(ent::kThrottleAccel, 0);
                    done = true;
                }
                if (!done) {
                    // 0x8007A7CC: start a new move that reaches zero in 8.0 / level seconds.
                    const int32_t level = static_cast<int32_t>(bike.U32(ent::kThrottle));
                    const int32_t a =
                        (level > 0)
                            ? Sub32(0, static_cast<int32_t>(FixDiv(0x00080000u,
                                                                   static_cast<uint32_t>(level))))
                            : static_cast<int32_t>(
                                  FixDiv(0x00080000u, static_cast<uint32_t>(Sub32(0, level))));
                    bike.SetU32(ent::kThrottleAccel,
                                static_cast<uint32_t>(SymClamp(a, 0x01F40000)));
                    bike.SetU32(ent::kThrottleTime, 0);
                    bike.SetU32(ent::kThrottleFrom, bike.U32(ent::kThrottle));
                }
            }
        } else if (fb & 0x20u) {
            // 0x8007A840
            bool slow = false;
            if (crossed == 0) {
                if (static_cast<int32_t>(bike.U32(ent::kThrottleVel)) < 0) {
                    slow = true;
                } else if (bike.U32(ent::kEngBrake) != 0 &&
                           0x00023C35 < static_cast<int32_t>(bike.U32(ent::kSpeedCopy))) {
                    slow = true;
                }
            }
            if (!slow) {
                // 0x8007A87C: the same move, but reaching zero in 2.0 / level seconds.
                const int32_t level = static_cast<int32_t>(bike.U32(ent::kThrottle));
                const int32_t a =
                    (level > 0)
                        ? Sub32(0, static_cast<int32_t>(
                                       FixDiv(0x00020000u, static_cast<uint32_t>(level))))
                        : static_cast<int32_t>(
                              FixDiv(0x00020000u, static_cast<uint32_t>(Sub32(0, level))));
                bike.SetU32(ent::kThrottleAccel, static_cast<uint32_t>(SymClamp(a, 0x01F40000)));
                bike.SetU32(ent::kThrottleTime, 0);
                bike.SetU32(ent::kThrottleVel, 0);
                bike.SetU32(ent::kThrottleFrom, bike.U32(ent::kThrottle));
            } else if (static_cast<int32_t>(bike.U32(ent::kEngHold)) > 0) {
                bike.SetU32(ent::kThrottleAccel, 0); // 0x8007A8FC
                bike.SetU32(ent::kThrottleVel, 0xFFFE0000u);
            }
        }

        // ---------------------------------------------------------------- 0x8007A904
        bool runWheelie = true;
        if ((bike.U32(ent::kFlagsB) & 0x400u) == 0) {
            const uint32_t fc = bike.U32(ent::kFlagsC);
            if ((fc & 0x7FFu) == 0 || (fc & 0x800u) != 0) runWheelie = false;
        }
        if (runWheelie) {
            // 0x8007A934: the same second-order move again, on +0x2A4.
            if (bike.U32(ent::kWheelieRate) != 0) {
                bike.SetU32(ent::kWheelieVel,
                            static_cast<uint32_t>(Add32(
                                static_cast<int32_t>(bike.U32(ent::kWheelieVel)),
                                FixMul(static_cast<int32_t>(bike.U32(ent::kWheelieRate)), dt))));
                bike.SetU32(ent::kWheelieTime,
                            static_cast<uint32_t>(Add32(
                                static_cast<int32_t>(bike.U32(ent::kWheelieTime)), dt)));
            }
            if (bike.U32(ent::kWheelieVel) != 0) {
                const int32_t rate = static_cast<int32_t>(bike.U32(ent::kWheelieRate));
                int32_t target;
                int32_t value;
                if (rate != 0) {
                    // 0x8007A984
                    const int32_t elapsed = static_cast<int32_t>(bike.U32(ent::kWheelieTime));
                    const int32_t p = FixMul(rate, elapsed);
                    const int32_t half =
                        Add32(static_cast<int32_t>(static_cast<uint32_t>(p) >> 31), p) >> 1;
                    const int32_t q = FixMul(
                        static_cast<int32_t>(bike.U32(ent::kWheelieTime)),
                        Sub32(static_cast<int32_t>(bike.U32(ent::kWheelieVel)), half));
                    value = Add32(static_cast<int32_t>(bike.U32(ent::kWheelieTo)), q);
                    if (bike.U32(ent::kFlagsC) & 0x20000u) {
                        target = (static_cast<int32_t>(bike.U32(ent::kWheelieBias)) > 0)
                                     ? 0x0001921F
                                     : static_cast<int32_t>(0xFFFE6DE1u);
                    } else {
                        target = 0;
                    }
                } else {
                    // 0x8007A9F0
                    value = Add32(static_cast<int32_t>(bike.U32(ent::kWheelieAngle)),
                                  FixMul(static_cast<int32_t>(bike.U32(ent::kWheelieVel)), dt));
                    target = static_cast<int32_t>(bike.U32(ent::kWheelieTo));
                }

                // 0x8007AA08: step the angle toward the target and report whether it got there.
                uint32_t keep = 0; // `v1`, 0 = snap to the target, ~0 = take the new value
                int32_t reached = 0;
                const int32_t cur = static_cast<int32_t>(bike.U32(ent::kWheelieAngle));
                if (!(value < target) && target < cur) {
                    reached = 1;
                    keep = static_cast<uint32_t>(0u - static_cast<uint32_t>(reached));
                } else {
                    keep = static_cast<uint32_t>(0u - static_cast<uint32_t>(reached)); // 0x8007AA30
                    if (!(target < value)) {
                        if (cur < target) { // 0x8007AA3C
                            reached = 1;
                            keep = static_cast<uint32_t>(0u - static_cast<uint32_t>(reached));
                        }
                    }
                }
                const int32_t diff = Sub32(value, target);
                bike.SetU32(ent::kWheelieAngle,
                            static_cast<uint32_t>(Add32(
                                target, static_cast<int32_t>(keep & static_cast<uint32_t>(diff)))));
                bike.SetU32(ent::kWheelieVel, bike.U32(ent::kWheelieVel) & keep);
                bike.SetU32(ent::kWheelieRate, bike.U32(ent::kWheelieRate) & keep);
            }
        }

        // ---------------------------------------------------------------- 0x8007AA78
        // The lean angle, ramped toward the arctangent of bike[+0x212] << 4.
        if (static_cast<int8_t>(bike.bytes()[ent::kMode]) < 2) {
            int32_t t0 = static_cast<int32_t>(static_cast<uint32_t>(bike.S16(ent::kLeanSrc)) << 4);
            int32_t negate = 0;
            if (t0 < 0) { // 0x8007AA98, tested on the SHIFTED value
                negate = 1;
                t0 = Sub32(0, t0);
            }
            int32_t angle = 1024; // 0x8007AAB4: 1024 = a quarter turn, the saturated answer
            if (!(0xFFFF < t0)) {
                int32_t a2 = 0xFFF8;
                int32_t index = 0;
                int32_t shift;
                if (t0 < 0xFFF8) {
                    a2 = 0x8000;
                    shift = 15;
                    if (!(t0 < 0x8000)) {
                        do { // 0x8007AADC
                            shift -= 1;
                            a2 = Add32(a2, static_cast<int32_t>(1u << (shift & 31)));
                            index += 4; // the delay slot: it runs on the exiting pass too
                        } while (!(t0 < a2));
                    }
                    a2 = Sub32(a2, static_cast<int32_t>(1u << (shift & 31))); // 0x8007AAF4
                    shift -= 2;
                } else {
                    index = 52; // 0x8007AB08
                    shift = 0;
                }
                int32_t step = Sub32(t0, a2) >> (shift & 31); // 0x8007AB14, `srav`
                a2 = Add32(a2, static_cast<int32_t>(static_cast<uint32_t>(step) << (shift & 31)));
                index = Add32(index, step);
                const uint32_t slot = static_cast<uint32_t>(index);
                const int32_t e0 = static_cast<int32_t>(atanTable[slot]);
                const int32_t e1 = static_cast<int32_t>(atanTable[slot + 1]);
                const int32_t span = Sub32(e1, e0);
                const int32_t rest = Sub32(t0, a2);
                const int32_t lo32 = static_cast<int32_t>(static_cast<uint32_t>(
                    static_cast<uint64_t>(static_cast<int64_t>(rest) * static_cast<int64_t>(span)) &
                    0xFFFFFFFFu));
                step = lo32 >> (shift & 31);
                angle = Add32(Add32(step, 8), e0) >> 4;
            }
            if (negate != 0) angle = Sub32(0, angle);

            const int32_t err = static_cast<int32_t>(
                static_cast<uint32_t>(Sub32(angle, bike.S16(ent::kLeanOut))) << 16);
            const int32_t move = FixMul(FixMul(err, 0x00079999), dt);
            bike.SetU16(ent::kLeanOut,
                        static_cast<uint16_t>(bike.U16(ent::kLeanOut) +
                                              static_cast<uint32_t>(move >> 16)));
        }
    }
}

// ============================================================ RASHCDG 0x80074E6C, the crash timer

int32_t CrashTableHit(EntityView bike, int32_t which, const uint8_t* table) {
    // 0x80027034..0x80027044: `2 * (3 * class + which)`, all with 32-bit wrap.
    const int32_t cls = static_cast<int32_t>(bike.U32(ent::kClass));
    const int32_t triple = Add32(static_cast<int32_t>(static_cast<uint32_t>(cls) << 1), cls);
    const int32_t at = static_cast<int32_t>(static_cast<uint32_t>(Add32(triple, which)) << 1);
    // 0x80027048..0x80027058: `sltu zero, ~(s8)table[at + 2]`, i.e. "not -1".
    const int8_t v = static_cast<int8_t>(table[static_cast<std::ptrdiff_t>(Add32(at, 2))]);
    return (v != -1) ? 1 : 0;
}

void BikeCrashTimer(EntityView bike, int32_t dt, int32_t window, const uint8_t* crashTable,
                    CrashEmitter& emit) {
    if (bike.U32(ent::kFlagsB) & 0x200u) { // 0x80074E84
        const int32_t t = Add32(static_cast<int32_t>(bike.U32(ent::kCrashTimer)), dt);
        bike.SetU32(ent::kCrashTimer, static_cast<uint32_t>(t));
        uint32_t fb = bike.U32(ent::kFlagsB);
        if (window < t) fb &= 0xFFFFFDFFu; // 0x80074EAC
        bike.SetU32(ent::kFlagsB, fb);
    }

    uint32_t flagsA = bike.U32(ent::kFlagsA); // 0x80074EBC, read once and used throughout
    if (!(flagsA & 2u)) {
        bike.SetU32(ent::kFlagsB, bike.U32(ent::kFlagsB) & 0xFFFFFDFFu); // 0x80074F78
    } else if (flagsA & 4u) {
        bool wipedOut = false;
        if (static_cast<int8_t>(bike.bytes()[ent::kCrashCount]) > 0 && (flagsA & 8u)) {
            // 0x80074EF0
            bike.SetU32(ent::kFlagsB, bike.U32(ent::kFlagsB) | 0x200u);
            bike.SetU32(ent::kCrashTimer, 0);
            bike.bytes()[ent::kCrashCount] =
                static_cast<uint8_t>(bike.bytes()[ent::kCrashCount] - 1u);
            if (CrashTableHit(bike, 0, crashTable) != 0) emit.Emit(0, 0); // 0x80074F0C/0x80074F20
            if (CrashTableHit(bike, 1, crashTable) != 0) emit.Emit(1, 2); // 0x80074F2C/0x80074F40
            wipedOut = true;
        }
        if (!wipedOut) { // 0x80074F50
            if (!(flagsA & 0x800u)) flagsA |= 0x1800u;
            bike.SetU32(ent::kFlagsA, flagsA);
        }
        bike.SetU32(ent::kFlagsA, bike.U32(ent::kFlagsA) & 0xFFFFFFFBu); // 0x80074F64
    }

    // 0x80074F88
    uint32_t fb = bike.U32(ent::kFlagsB);
    if (bike.U32(ent::kFlagsA) & 0x40u) fb &= 0xFFFFFDFFu;
    bike.SetU32(ent::kFlagsB, fb);
}

// ---------------------------------------------------------------------------- RASHCDG 0x8007504C
namespace {

// `lui v0,0x1 ; ori v0,v0,0xc9c4` at 0x80075194/0x80075198 and again at 0x80075574/0x80075578.
constexpr int32_t kGroundCreepSpeed = 0x0001C9C4;

// The `sll v0,v0,0x4` that promotes a 4096 = 1.0 direction into the 16.16 the multiply wants.
inline int32_t Shl4(int16_t v) {
    return static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(v)) << 4);
}
// `negu` on a halfword that was loaded with `lhu` and is stored back with `sh`.
inline uint16_t NegU16(uint16_t v) {
    return static_cast<uint16_t>(0u - static_cast<uint32_t>(v));
}

// The creep both halves of the function apply to a stopped machine: set +0x1E0 and run
// `SLUS 0x8002EE50 Scale(speed, &m[+0x1C2], &out[+0x1C8])` on the heading, which is ported.
void GroundCreep(EntityView v) {
    v.SetU32(ent::kSpeed, static_cast<uint32_t>(kGroundCreepSpeed));
    const int16_t dir[3] = {v.S16(ent::kHeading), v.S16(ent::kHeading + 2), v.S16(ent::kHeading + 4)};
    int32_t out[3];
    Scale(kGroundCreepSpeed, dir, out);
    for (uint32_t i = 0; i < 3; ++i) v.SetU32(0x1C8u + 4u * i, static_cast<uint32_t>(out[i]));
}

} // namespace

void BikeGroundFrame(EntityView e, const int32_t refPointIn[3], BikeGroundEnv& env) {
    // The original reads all three words of `a1` before its first store (0x800751B8, 0x800751CC,
    // 0x800751E0), so copying them here is the same thing even when `a1` aliases the output at
    // `+0x1F8`, which the per-bike step's own call site arranges whenever `flagsC & 0x600` is clear.
    const int32_t refPoint[3] = {refPointIn[0], refPointIn[1], refPointIn[2]};

    // `s4` at 0x8007507C: bit 0 of `+0x184` is what decides whether this frame asks the world or
    // simply takes the road slice the bike is tracked on. The traced frame had it CLEAR on the
    // player, which is why the product never reaches the
    // unported ground query.
    const bool useQuery = (e.U32(0x184) & 1u) != 0;

    int8_t surfaceLatch = 0; // `s5`, 0x80075084 / 0x800750D0
    int32_t ret = 0;         // `a1` after the first query
    int16_t normal[3] = {0, 0, 0}; // the three halfwords `s2` points at
    // `s3`, the point the projection is taken from. It has to stay a POINTER rather than a copy:
    // the `+0x1F8` case is read once before 0x800752A8 writes it and once after (0x80075384).
    enum class PointSrc { kFrameA, kSliceOrigin, kContact };
    PointSrc pointSrc = PointSrc::kSliceOrigin;

    if (useQuery) {
        // 0x80075088. a2 = the caller's own `sp+24`; a3 = the ENTITY field at +0x10C, so the normal
        // buffer is pre-loaded from it and written straight back - a callee that returns -1 without
        // writing leaves the field exactly as it was, which is what the original gets too.
        int16_t queryNormal[3] = {e.S16(0x10C), e.S16(0x10E), e.S16(0x110)};
        ret = env.query->Query(BikeGroundSite::kMain, refPoint, env.frame->pointA, queryNormal,
                               static_cast<int32_t>(e.U32(0x218)));
        for (uint32_t i = 0; i < 3; ++i)
            e.SetU16(0x10Cu + 2u * i, static_cast<uint16_t>(queryNormal[i]));
        // 0x800750A4: `flagsA` bit 6 says "the surface under me changed this frame".
        uint32_t flags = e.U32(0x184);
        if (static_cast<uint32_t>(ret) != e.U32(0x218)) {
            e.SetU32(0x218, static_cast<uint32_t>(ret));
            flags |= 0x40u;
        } else {
            flags &= 0xFFFFFFBFu;
        }
        e.SetU32(0x184, flags);
        surfaceLatch = static_cast<int8_t>(e.bytes()[0x216]);
        for (uint32_t i = 0; i < 3; ++i) normal[i] = e.S16(0x10Cu + 2u * i);
        pointSrc = PointSrc::kFrameA;
    } else {
        // 0x800750DC: `s3` = slice+20, `s2` = slice+8 of the slice the bike is tracked on.
        ret = 0;
        e.SetU32(0x218, 0);
        for (uint32_t i = 0; i < 3; ++i) normal[i] = env.tracked.row[i];
        pointSrc = PointSrc::kSliceOrigin;
        e.bytes()[0x216] = (e.U32(0x174) != 0) ? e.bytes()[0x18A] : static_cast<uint8_t>(1);
    }

    // 0x8007510C. `slti v0,v0,-615` - the surface has to face up by more than 615/4096 before the
    // bike is allowed to stand on it. NOTE the rider half below uses -614, not -615.
    bool onSurface = false;
    if (ret >= 0 && normal[1] < -615) {
        for (uint32_t i = 0; i < 3; ++i)
            e.SetU16(0x112u + 2u * i, NegU16(static_cast<uint16_t>(normal[i])));
        onSurface = true;
    }
    if (!onSurface) {
        // 0x8007515C: fall back on the bike's own stored normal at +0x20A, and project from the
        // contact point instead of from the surface.
        e.SetU16(0x112, e.U16(0x20A));
        e.SetU16(0x114, e.U16(0x20C));
        e.SetU16(0x116, e.U16(0x20E));
        pointSrc = PointSrc::kContact;
        // 0x80075180: a bike that is standing still and has not been placed 255th creeps forward.
        if (e.U32(ent::kSpeed) == 0 && env.riderDefPlace != 255) GroundCreep(e);
    }

    // 0x800751B4: the projection. `t = dot(point - refPoint, n)` in 16.16, then the contact point
    // is `refPoint + n * t` - i.e. `point` dropped onto the line through `refPoint` along `n`.
    auto point = [&e, &env, pointSrc](uint32_t i) -> int32_t {
        switch (pointSrc) {
            case PointSrc::kFrameA: return env.frame->pointA[i];
            case PointSrc::kSliceOrigin: return env.tracked.origin[i];
            default: return static_cast<int32_t>(e.U32(0x1F8u + 4u * i));
        }
    };
    int32_t d[3];
    for (uint32_t i = 0; i < 3; ++i) d[i] = Sub32(point(i), refPoint[i]);
    const int32_t t = Add32(Add32(FixMul(d[0], Shl4(e.S16(0x112))), FixMul(d[1], Shl4(e.S16(0x114)))),
                            FixMul(d[2], Shl4(e.S16(0x116))));
    // The original re-loads each component of the normal between the three stores (0x800752AC,
    // 0x800752EC), so the port does too; nothing it writes can change them, but the shape is the
    // original's.
    e.SetU32(0x1F8, static_cast<uint32_t>(Add32(FixMul(Shl4(e.S16(0x112)), t), refPoint[0])));
    e.SetU32(0x1FC, static_cast<uint32_t>(Add32(FixMul(Shl4(e.S16(0x114)), t), refPoint[1])));
    e.SetU32(0x200, static_cast<uint32_t>(Add32(FixMul(Shl4(e.S16(0x116)), t), refPoint[2])));
    e.SetU32(0x104, static_cast<uint32_t>(t)); // 0x8007532C, a delay slot: unconditional

    if (useQuery) {
        // 0x80075330, the SECOND query: the reference point is +0xF4, the point the road-slice
        // search `SLUS 0x80036B14` left there (0x80037274), and the normal buffer is again the
        // entity field at +0x10C.
        const int32_t altRef[3] = {static_cast<int32_t>(e.U32(0x0F4)),
                                   static_cast<int32_t>(e.U32(0x0F8)),
                                   static_cast<int32_t>(e.U32(0x0FC))};
        int16_t queryNormal[3] = {e.S16(0x10C), e.S16(0x10E), e.S16(0x110)};
        const int32_t ret2 = env.query->Query(BikeGroundSite::kAlt, altRef, env.frame->pointB,
                                              queryNormal, static_cast<int32_t>(e.U32(0x23C)));
        for (uint32_t i = 0; i < 3; ++i)
            e.SetU16(0x10Cu + 2u * i, static_cast<uint16_t>(queryNormal[i]));
        // 0x80075354: `flagsB` bit 25 is cleared unconditionally (the store is in the branch's
        // delay slot) and set again below only when the second surface id changed.
        const uint32_t cleared = e.U32(ent::kFlagsB) & 0xFDFFFFFFu;
        e.SetU32(ent::kFlagsB, cleared);
        if (ret2 < 0) {
            // 0x8007536C: nothing under the alternate point - mirror the first answer.
            e.SetU16(0x10C, e.U16(0x112));
            e.SetU16(0x10E, e.U16(0x114));
            e.SetU16(0x110, e.U16(0x116));
            for (uint32_t i = 0; i < 3; ++i)
                e.SetU32(0x118u + 4u * i, static_cast<uint32_t>(point(i)));
        } else {
            if (static_cast<uint32_t>(ret2) != e.U32(0x23C))
                e.SetU32(ent::kFlagsB, cleared | 0x02000000u);
            // 0x800753C0, and the ORDER is the compiler's: +0x10C and +0x110 are both read before
            // either is written, and +0x10E is read only after +0x10C has been stored.
            const uint16_t x = e.U16(0x10C), z = e.U16(0x110);
            e.SetU16(0x10C, NegU16(x));
            const uint16_t y = e.U16(0x10E);
            e.SetU16(0x110, NegU16(z));
            e.SetU16(0x10E, NegU16(y));
            for (uint32_t i = 0; i < 3; ++i)
                e.SetU32(0x118u + 4u * i, static_cast<uint32_t>(env.frame->pointB[i]));
        }
        const uint8_t surface = e.bytes()[0x216]; // 0x80075408, what the SECOND query left there
        e.SetU32(0x23C, static_cast<uint32_t>(ret2));
        // 0x80075414, the delay slot of the jump to 0x800754B4: `sb s5,534(s1)` (0xA2350216) puts
        // back the byte the FIRST query left at +0x216; 0x800754B4 then stores the second one at
        // +0x217. (It is not a dead store to +0x217: reading it as one leaves the second query's
        // byte at +0x216.)
        e.bytes()[0x216] = static_cast<uint8_t>(surfaceLatch);
        e.bytes()[0x217] = surface;
    } else {
        // 0x80075418: both the normal and the point come from the slice at +0x100 - the slice the
        // road search bound this frame, which is not necessarily the one at +0x154.
        e.SetU32(0x23C, 0);
        for (uint32_t i = 0; i < 3; ++i)
            e.SetU16(0x10Cu + 2u * i, NegU16(static_cast<uint16_t>(env.ground.row[i])));
        for (uint32_t i = 0; i < 3; ++i)
            e.SetU32(0x118u + 4u * i, static_cast<uint32_t>(env.ground.origin[i]));
        const uint32_t fb = e.U32(ent::kFlagsB);
        e.SetU32(ent::kFlagsB, (e.U32(0x154) != env.ground.address) ? (fb | 0x02000000u)
                                                                   : (fb & 0xFDFFFFFFu));
        e.bytes()[0x217] = (e.U32(0x174) != 0) ? e.bytes()[0x18A] : static_cast<uint8_t>(1);
    }

    // 0x800754B8: and now the same thing again for the pool-1 rider hanging off +0x358, which gets
    // its own copy of +0x2C / +0x30 and its own surface, written to +0x310 and +0x32E.
    if (env.rider == nullptr) return;
    if (env.listNode == 0) return;
    EntityView r(env.rider);
    r.SetU32(0x2C, e.U32(0x2C));
    const bool riderQuery = (r.U32(0x184) & 1u) != 0;
    r.SetU32(0x30, e.U32(0x30));

    int32_t rret = 0;
    bool riderOnFrame = false; // `s3` = sp+24 / `s2` = sp+72 rather than the rider's own slice
    if (riderQuery) {
        // 0x800754F8. a1 = 0, which makes the callee use `rider+0xB8` as the reference point; a2 is
        // the SAME `sp+24` the first query used and a3 is a second frame local at `sp+72`.
        rret = env.query->Query(BikeGroundSite::kRider, nullptr, env.frame->pointA,
                                env.frame->normalC, static_cast<int32_t>(r.U32(0x218)));
        r.SetU32(0x218, static_cast<uint32_t>(rret));
        riderOnFrame = true;
    } else {
        // 0x80075524
        r.bytes()[0x216] = (r.U32(0x174) != 0) ? r.bytes()[0x18A] : static_cast<uint8_t>(1);
        r.SetU32(0x218, 0);
    }
    if (rret <= 0) {
        riderOnFrame = false; // 0x80075554: `s3` = riderSlice+20, `s2` = riderSlice+8
        if (rret < 0) {
            // 0x80075568: a stopped rider creeps, and that is the whole of this arm.
            if (r.U32(ent::kSpeed) != 0) return;
            GroundCreep(r);
            return;
        }
    }
    // 0x8007559C, with -614 where the bike half used -615.
    const int16_t* rn = riderOnFrame ? env.frame->normalC : env.riderTracked.row;
    if (!(rn[1] < -614)) return;
    for (uint32_t i = 0; i < 3; ++i)
        r.SetU16(ent::kAimAxis + 2u * i, NegU16(static_cast<uint16_t>(rn[i])));
    const int32_t* rp = riderOnFrame ? env.frame->pointA : env.riderTracked.origin;
    for (uint32_t i = 0; i < 3; ++i) r.SetU32(0x310u + 4u * i, static_cast<uint32_t>(rp[i]));
}

} // namespace rr::sim
