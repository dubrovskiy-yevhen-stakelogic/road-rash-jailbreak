#include "game/sim/race.h"

namespace rr::sim {
namespace {

// The little-endian word reads the original does out of game data the caller handed over as bytes.
int32_t LoadS32(const uint8_t* p) {
    return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                (static_cast<uint32_t>(p[2]) << 16) |
                                (static_cast<uint32_t>(p[3]) << 24));
}

uint16_t LoadU16(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8));
}

void StoreS32(uint8_t* p, int32_t v) {
    const uint32_t u = static_cast<uint32_t>(v);
    p[0] = static_cast<uint8_t>(u);
    p[1] = static_cast<uint8_t>(u >> 8);
    p[2] = static_cast<uint8_t>(u >> 16);
    p[3] = static_cast<uint8_t>(u >> 24);
}

int16_t LoadS16(const uint8_t* p) {
    return static_cast<int16_t>(LoadU16(p));
}

// `sra v1,v0,0x1f; addu v0,v1,v0; xor v0,v0,v1` - the compiler's absolute value, which wraps on
// INT32_MIN exactly as the original does rather than trapping.
int32_t MipsAbs(int32_t v) {
    const uint32_t m = static_cast<uint32_t>(v >> 31);
    return static_cast<int32_t>((static_cast<uint32_t>(v) + m) ^ m);
}

// `xor v0,a,b; bltz v0` - "a and b have the same sign", the idiom both finish-line arms use.
bool SameSign(int32_t a, int32_t b) {
    return ((static_cast<uint32_t>(a) ^ static_cast<uint32_t>(b)) & 0x80000000u) == 0;
}

// `bike[+0x2D0]`, the word the countdown holds at 0xFFFF0000 and releases to 0, the steering hold
// flag; `0x80073874` is its other reader.
constexpr uint32_t kHold = 0x2D0;

} // namespace

// ---------------------------------------------------------------------------- RASHCDG 0x8008AD38
int32_t TickCountdown(int32_t dt, const CountdownEnv& env) {
    // 0x8008AD48..0x8008AD5C. No null guard on player 1 in the original either.
    if ((env.rider1[0] & 0x40u) == 0) return 1;

    // 0x8008AD68..0x8008AD74: both players are pinned while the countdown holds. The store to
    // player 1 is in the branch's delay slot, so it happens whether or not player 2 exists.
    EntityView(env.player1).SetU32(kHold, 0xFFFF0000u);
    if (env.player2 != nullptr) EntityView(env.player2).SetU32(kHold, 0xFFFF0000u);

    if ((env.rider1[0] & 0x20u) != 0) {
        // 0x8008AD9C..0x8008ADEC - arm it. The race clock restarts here, which is why it never
        // includes the countdown.
        StoreS32(env.raceClock, 0);
        *env.countdown = 0x00030000; // `lui v0,0x3` = 3.00 s in 16.16
        env.rider1[0] = static_cast<uint8_t>(env.rider1[0] & 0xDFu);
        if (env.player2 != nullptr) env.rider2[0] = static_cast<uint8_t>(env.rider2[0] & 0xDFu);
        return 0;
    }

    // 0x8008ADF4..0x8008AE04. The subtraction is stored back before the sign is tested, so the
    // word keeps the negative overshoot on the frame the countdown expires.
    const int32_t left = *env.countdown - dt;
    *env.countdown = left;
    if (left >= 0) return 0;

    // 0x8008AE08..0x8008AE6C - "GO".
    env.rider1[0] = static_cast<uint8_t>(env.rider1[0] & 0xBFu);
    EntityView(env.player1).SetU32(kHold, 0);
    if (env.player2 != nullptr) {
        env.rider2[0] = static_cast<uint8_t>(env.rider2[0] & 0xBFu);
        EntityView(env.player2).SetU32(kHold, 0);
    }
    *env.eventAcc = 0;
    StoreS32(env.raceClock, 0);
    env.go->Go(); // RASHCDG 0x80090270, supplied by the oracle
    return 1;
}

// ---------------------------------------------------------------------------- RASHCDG 0x8008AB00
void RaceTick(int32_t dt, const RaceTickEnv& env) {
    // 0x8008AB14..0x8008AB30. The write-back is unconditional and happens in the delay slot of the
    // planner test, i.e. before anything below can read the word again.
    const int32_t acc = static_cast<int32_t>(static_cast<uint32_t>(*env.eventAcc) +
                                             static_cast<uint32_t>(dt));
    *env.eventAcc = acc;

    if (*env.planCount != 0) {
        // 0x8008AC10..0x8008AC34. `a0` still holds `acc` from 0x8008AB28, so this site passes it.
        if (0x8000 < acc) {
            env.children->AiPlan(acc);
            *env.eventAcc = 0;
            *env.planCount = *env.planCount + 1;
        }
    } else if (TickCountdown(dt, env.countdown) == 0) {
        // 0x8008ABEC..0x8008AC00. The one early exit in the whole function: during the countdown
        // nothing ticks at all, unless this is the one-shot first race frame.
        if (static_cast<int8_t>(env.gameState[3]) == 0) return;
    } else {
        // 0x8008AB44. Re-read, because "GO" may have just zeroed it.
        const int32_t a = *env.eventAcc;
        const int32_t back = static_cast<int32_t>(static_cast<uint32_t>(a) -
                                                  static_cast<uint32_t>(dt));
        if (0x8000 < a && !(0x8000 < back)) {
            env.children->AiPlan(0); // 0x8008AB64, `move a0,zero`
        } else {
            // 0x8008AB78..0x8008ABE8. `raceType` is a byte at gameState+0x04 and the bank a word
            // at gameState+0x3C; the index is not bounded by the original.
            const uint32_t raceType = env.gameState[4];
            const int32_t bank = LoadS32(env.gameState + 0x3C);
            const int32_t arm = (raceType & 4u) != 0
                                    ? 6
                                    : static_cast<int32_t>(static_cast<uint32_t>(
                                          -static_cast<int32_t>(raceType & 1u)) & 3u);
            const int32_t k = static_cast<int32_t>(static_cast<uint32_t>(bank) +
                                                   static_cast<uint32_t>(arm));
            if (env.planTable[k] < *env.eventAcc) {
                *env.eventAcc = dt;
                *env.planCount = *env.planCount + 1;
            }
        }
    }

    // 0x8008AC38..0x8008AC64 - the six children, in this order, each with `dt`.
    env.children->RaceDirector(dt);
    env.children->SpawnerPass(dt);
    env.children->WorldBikePass(dt);
    env.children->CollisionPass(dt);
    env.children->RiderEnginePass(dt);
    env.children->PresentationPass(dt);
}

// ---------------------------------------------------------------------------- SLUS 0x80012524
void RaceStep(const RaceStepEnv& env) {
    // 0x80012540..0x80012558
    int32_t ticks = LoadS32(env.gameState + 0x18);
    if (ticks == 0) {
        StoreS32(env.gameState + 0x1C, 0); // 0x80012630
        return;
    }
    if (!(ticks < 31)) ticks = 30; // `slti v0,a1,31` - SIGNED
    StoreS32(env.gameState + 0x1C, ticks);

    // 0x80012564..0x80012574: 218 * ticks, built out of shifts, so it wraps like the original.
    const uint32_t t = static_cast<uint32_t>(ticks);
    const uint32_t seven = (t << 3) - t;
    const int32_t dt = static_cast<int32_t>(((seven << 5) - seven) + t);

    *env.frameFlag = 0; // *(0x8005B580), 0x80012584
    // 0x80012578..0x80012590: the race clock advances in TICKS, not in 16.16 seconds.
    StoreS32(env.gameState + 0x10, static_cast<int32_t>(
                                       static_cast<uint32_t>(LoadS32(env.gameState + 0x10)) + t));

    RaceTick(dt, env.tick);

    // 0x800125A0..0x80012624. The player count is re-read from `game_state` on every iteration and
    // the comparison is UNSIGNED.
    uint32_t n = static_cast<uint32_t>(LoadS32(env.gameState + 0x30));
    if (n == 0) return;
    uint32_t p = 0;
    do {
        uint8_t* view = env.camera->View(static_cast<int32_t>(p));
        // 0x800125B8..0x800125D8: remember last frame's camera position before the update.
        StoreS32(view + 0x1DC, LoadS32(view + 0x0C0));
        StoreS32(view + 0x1D4, LoadS32(view + 0x0B8));
        StoreS32(view + 0x1D8, LoadS32(view + 0x0BC));
        env.camera->Update(static_cast<int32_t>(p), dt);
        view = env.camera->View(static_cast<int32_t>(p));
        if ((LoadS32(view + 0x224) & 0x100) != 0) {
            if (env.camera->Test(static_cast<int32_t>(p)) != 0)
                env.camera->Apply(static_cast<int32_t>(p));
        }
        ++p;
        n = static_cast<uint32_t>(LoadS32(env.gameState + 0x30));
    } while (p < n);
}

// ---------------------------------------------------------------------------- RASHCDG 0x800B9794
bool ProgressPass(uint32_t* skip, const ProgressPassEnv& env) {
    if (*env.liveBikes <= 0) return true; // 0x800B97CC, `blez`
    int32_t i = 0;
    for (;;) {
        if (i >= env.count) return false; // the caller did not resolve this one
        ProgressNode& n = env.bikes[i];
        EntityView e(n.entity);

        // 0x800B97E4..0x800B9824: the entry gate. A police bike that is not a player and is not
        // held in by `+0x3A0` bit 0x10 is skipped outright.
        const uint32_t handle = e.U16(ent::kHandle);
        const uint32_t players = static_cast<uint32_t>(LoadS32(env.gameState + 0x30));
        bool process = handle < players;
        if (!process) process = (n.riderDef[1] & 0x0Fu) != 2;
        if (!process) process = (n.entity[ent::kRaceFlags] & 0x10u) != 0;

        bool setSkip = !process;
        if (process) {
            // 0x800B9828..0x800B9854 - three per-frame clears.
            StoreS32(n.owner + 0x228,
                     static_cast<int32_t>(static_cast<uint32_t>(LoadS32(n.owner + 0x228)) &
                                          0xFDFFFFFFu));
            e.SetU32(ent::kFlagsA, e.U32(ent::kFlagsA) & 0xFFFBFFFFu);
            n.entity[ent::kRaceFlags] = static_cast<uint8_t>(n.entity[ent::kRaceFlags] & 0xBFu);

            // 0x800B9858..0x800B9874. The finish test runs only for a rider whose finish time is
            // still zero, and a zero answer jumps PAST the result-code block.
            bool done = LoadS32(n.riderDef + 0x28) != 0;
            if (!done) done = env.finish->Test(i) != 0;

            bool tail = true; // whether to fall through to the "is this entity still live" tail
            if (done) {
                const uint32_t place = n.riderDef[0x27];
                if (place <= 246u || place == 250u || place == 254u || place == 255u)
                    e.SetU32(ent::kFlagsA, e.U32(ent::kFlagsA) | 0x08000000u);
                // 0x800B98B4: `(u8)(place + 2) < 2`, i.e. place is 254 or 255.
                if (static_cast<uint8_t>(n.riderDef[0x27] + 2u) < 2u) {
                    setSkip = true;
                    tail = false;
                }
            }
            if (tail) {
                // 0x800B98D0..0x800B9900
                if (e.S16(ent::kLiveState) != 0) setSkip = false;
                else if (static_cast<uint32_t>(LoadS32(n.owner + 0x25C)) < 3u) setSkip = true;
                else setSkip = EntityView(n.owner).S16(ent::kLiveState) == 0;
            }
        }
        if (setSkip) *skip |= 1u << (static_cast<uint32_t>(i) & 31u);

        ++i;
        if (!(i < *env.liveBikes)) return true; // 0x800B991C, the count is re-read
    }
}

// ---------------------------------------------------------------------------- RASHCDG 0x800B9414
bool RaceDirector(int32_t dt, const RaceDirectorEnv& env) {
    uint32_t doneMask = 0;
    uint32_t players = static_cast<uint32_t>(LoadS32(env.gameState + 0x30));
    // 0x800B945C: with no players the whole first loop is skipped.
    for (uint32_t p = 0; p < players; ++p) {
        if (static_cast<int32_t>(p) >= env.playerCount) return false;
        RaceDirectorPlayer& pl = env.players[p];
        EntityView e(pl.entity);

        // --- the remount arm, 0x800B9490..0x800B953C
        if ((pl.owner[0x23C] & 0x10u) != 0) {
            // The original chases `entity[+0x358]` here with no null guard.
            if (pl.partner == nullptr || pl.partnerOwner == nullptr) return false;
            StoreS32(pl.partnerOwner + 0x228,
                     static_cast<int32_t>(static_cast<uint32_t>(LoadS32(pl.partnerOwner + 0x228)) &
                                          0xFDFFFFFFu));
            if (static_cast<uint32_t>(LoadS32(pl.owner + 0x25C)) < 2u) {
                const uint32_t kind = EntityView(pl.partnerOwner).U16(0x220);
                const uint32_t rec = 8u * kind;
                const uint32_t tag = static_cast<uint32_t>(env.kindTable[rec + 2]) |
                                     (static_cast<uint32_t>(env.kindTable[rec + 3]) << 8);
                if (tag == 8 && e.U32(ent::kSpeed) == 0 && env.gameState[0x39] == 0)
                    env.calls->RiderRemount(static_cast<int32_t>(p));
            }
        }

        // --- the end-of-race test, 0x800B9540..0x800B9570. A player is "in" when the finish time
        // is stamped or the place byte is a result code.
        const bool done = LoadS32(pl.riderDef + 0x28) != 0 ||
                          static_cast<uint32_t>(pl.riderDef[0x27]) >= 248u;
        if (done) doneMask |= 1u << (p & 31u);

        players = static_cast<uint32_t>(LoadS32(env.gameState + 0x30));
        const uint32_t all = (1u << (players & 31u)) - 1u;
        if (doneMask == all) {
            int32_t t = *env.postDelay;
            if (t < 0) t = 0;
            t = static_cast<int32_t>(static_cast<uint32_t>(t) + static_cast<uint32_t>(dt));
            const uint32_t viewFlags =
                static_cast<uint32_t>(LoadS32(pl.view + 0x228)); // read BEFORE the store, as the
            *env.postDelay = t;                                  // original does
            const bool skipWait = (viewFlags & 0x8u) != 0;
            if (skipWait || !(t < *env.postLimit)) {
                if (*env.skipResults != 0) {
                    env.gameState[0] = 2; // 0x800B9604, straight back to the shell
                } else {
                    env.calls->ResultsPrepare();
                    env.gameState[0] = 6; // 0x800B9600, run the results scene
                }
                env.calls->RaceOverSignal();
            }
        }
        players = static_cast<uint32_t>(LoadS32(env.gameState + 0x30));
    }

    // --- the progress pass and the three AI passes, 0x800B9634..0x800B966C
    uint32_t skip = 0, maskB = 0, maskC = 0;
    if (!ProgressPass(&skip, env.progress)) return false;
    env.calls->AiDrivePass(dt, skip, &maskB, &maskC);
    env.calls->AiRunCommands(dt, skip, maskB);
    env.calls->AiBrainPass(dt, skip, maskC);

    // --- the two-rider loop, 0x800B9670..0x800B9764
    players = static_cast<uint32_t>(LoadS32(env.gameState + 0x30));
    for (uint32_t p = 0; p < players; ++p) {
        if (static_cast<int32_t>(p) >= env.playerCount) return false;
        RaceDirectorPlayer& pl = env.players[p];
        if ((pl.owner[0x23C] & 0x10u) != 0) {
            if (pl.partner == nullptr) return false; // again no null guard in the original
            const int32_t depth = static_cast<int8_t>(pl.partner[0x3B2]);
            const uint32_t slot = 0x3B4u + 8u * static_cast<uint32_t>(depth);
            EntityView partner(pl.partner);
            if (partner.U16(slot) == 16) {
                env.calls->PartnerCommand(static_cast<int32_t>(p), partner.U16(slot + 2), dt);
            }
        }
        if (pl.partner != nullptr && env.gameState[0x39] == 2) {
            const int32_t depth = static_cast<int8_t>(pl.partner[0x3B2]);
            const uint32_t slot = 0x3B4u + 8u * static_cast<uint32_t>(depth);
            if (EntityView(pl.partner).U16(slot) == 18)
                env.calls->PartnerFinish(static_cast<int32_t>(p), dt);
        }
        players = static_cast<uint32_t>(LoadS32(env.gameState + 0x30));
    }
    return true;
}

// ---------------------------------------------------------------------------- SLUS 0x8003B4B0
int32_t RouteFindLeg(const uint8_t* legs, int32_t legCount, uint32_t key, int16_t routeArmed) {
    if (routeArmed == -1) return -1; // 0x8003B4BC, tested BEFORE the null check
    if (legs == nullptr) return -1;
    if (!(0 < legCount)) return -1; // `slt v0,t0,v1` with t0 = 0
    for (int32_t i = 0; i < legCount; ++i) {
        if (static_cast<uint32_t>(LoadS32(legs + 16 * i)) == key) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------- SLUS 0x8003B8F4
int32_t RouteBindingValid(const uint8_t* entity, const RouteBinding& b) {
    if (entity == nullptr) return 0;
    if (b.routeObject == nullptr) return 0;
    // `p[+0x0BC]` with `p = entity + 0xAC`, i.e. entity[+0x168] - the road id and the `kind`
    // packed into one word.
    const uint32_t w = static_cast<uint32_t>(LoadS32(entity + 0x168));
    const uint32_t kind = w >> 16;
    const uint32_t roadId = w & 0xFFFFu;
    if (kind == 1) return (roadId ^ b.firstWord) < 1u ? 1 : 0;
    return RouteFindLeg(b.legs, b.legCount, roadId, b.routeArmed) >= 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------- SLUS 0x8003B96C
int32_t ProgressOf(const uint8_t* entity, const RouteBinding& b) {
    if (RouteBindingValid(entity, b) == 0) return 0x7FFFF000;
    return LoadS32(entity + 0x144);
}

// ---------------------------------------------------------------------------- SLUS 0x800138E8
bool ComputePlace(const uint8_t* entity, const uint8_t* riderDef, const RouteBinding& binding,
                  int32_t mode, const ComputePlaceEnv& env, int32_t* place) {
    const uint32_t handle = LoadU16(entity + ent::kHandle);
    const uint32_t players = static_cast<uint32_t>(LoadS32(env.gameState + 0x30));

    // 0x8001391C..0x80013990 - the police arm.
    if ((riderDef[1] & 0x0Fu) == 2 && static_cast<uint32_t>(riderDef[0x27]) < 247u) {
        bool park = handle >= players;
        if (!park) park = (env.gameState[4] & 1u) != 0 && (env.raceFlags & 1u) == 0;
        if (park) {
            *place = env.liveBikes + 1;
            return true;
        }
    }

    // 0x80013994..0x800139C8 - anyone who is already done keeps the code that was stamped.
    if (LoadS32(riderDef + 0x28) != 0 || !(static_cast<uint32_t>(riderDef[0x27]) < 248u)) {
        *place = riderDef[0x27];
        return true;
    }

    const int32_t mine = ProgressOf(entity, binding);
    int32_t n = 1;
    for (int32_t i = env.poolHigh; i >= 0; --i) {
        if (i >= env.poolCount) return false;
        const PlaceNode& o = env.pool[i];
        // 0x800139F4..0x80013A28: the police filter applies only while WE are not a player.
        if (!(handle < players) && (o.riderDef[1] & 0x0Fu) == 2) continue;
        if (LoadU16(o.entity + ent::kHandle) == handle) continue;
        if (env.liveBikes < static_cast<int32_t>(o.riderDef[0x27])) continue;
        if (mode == 1) {
            n += (static_cast<int32_t>(LoadS32(o.riderDef + 0x28)) > 0) ? 1 : 0;
            continue;
        }
        const int32_t theirs = ProgressOf(o.entity, o.binding);
        n += (LoadS32(o.riderDef + 0x28) > 0 || theirs < mine) ? 1 : 0;
    }
    *place = n;
    return true;
}

// ---------------------------------------------------------------------------- RASHCDG 0x800B9958
bool FinishTest(const FinishTestEnv& env, int32_t* result) {
    *result = 0;
    if (env.entity == nullptr) return true; // 0x800B9984 -> 0x800BA2DC, v0 = 0
    if (env.riderDef == nullptr || env.owner == nullptr || env.gameState == nullptr) return false;

    EntityView e(env.entity);
    uint8_t* const gs = env.gameState;
    uint8_t* const rd = env.riderDef;
    uint8_t* const ow = env.owner;

    // Every one of these is RE-READ at each site the original reads it at, because an
    // oracle-supplied callee between two reads may have changed it.
    const auto handle = [&]() { return static_cast<uint32_t>(e.U16(ent::kHandle)); };
    const auto players = [&]() { return static_cast<uint32_t>(LoadS32(gs + 0x30)); };
    const auto raceType = [&]() { return static_cast<uint32_t>(gs[4]); };
    const auto clock = [&]() { return LoadS32(gs + 0x10); };

    int32_t done = 0;      // s3 - the answer being built
    uint32_t flags = 0;    // s4 - bit 1 the milestone road matched, bit 0 the route's own bit
    int32_t along = 0;     // t1 - the distance the two crossing tests compare
    int32_t frameWord = 0; // the ONE word of the original's frame that is reused, at its sp+32
    bool frameWordSet = false;

    // --- 1. already finished, 0x800B998C..0x800B99DC -------------------------------------------
    if (LoadS32(rd + 0x28) != 0) {
        if (handle() < players()) {
            const uint32_t f = e.U32(ent::kFlagsA);
            if ((f & 0x08000000u) == 0) e.SetU32(ent::kFlagsA, f | 0x08000000u);
        }
        *result = 1;
        return true;
    }

    // --- 2. the clocks, 0x800B99E0..0x800B9B18. Players only, and never in race type 33. --------
    if (handle() < players() && raceType() != 33u) {
        const int32_t over = *env.raceOverFlag; // *(0x8005B220)
        if (over != 0 && over < 3 && 0x1A5E0 < clock()) {
            done = 1; // 0x800B9B14 - 108000 ticks = 6:00
        } else if (raceType() == 36u && *env.timeLimit < clock()) {
            rd[0x27] = 250;
            done = 1; // 0x800B9AAC
        } else if ((raceType() & 1u) != 0 &&
                   !(0 < static_cast<int32_t>(
                         static_cast<uint32_t>(*env.timeLimit) -
                         (static_cast<uint32_t>(clock()) - static_cast<uint32_t>(*env.timeBase))))) {
            rd[0x27] = 250;
            done = 1;
        } else if ((raceType() & 4u) != 0) {
            // 0x800B9ADC: 216000 ticks = 12:00 for race type 44, 108000 for anything else.
            const int32_t limit = (raceType() == 44u) ? 0x34BC0 : 0x1A5E0;
            if (limit < clock()) done = 1;
        }
    }

    // A hit above jumps straight into the result block at 0x800B9F28, skipping every route arm.
    if (done == 0) {
        if (env.routeArmed == -1) {
            // --- no route is loaded, 0x800B9EC4 ---------------------------------------------
            if (e.S16(ent::kLiveState) != 0) {
                if (!env.sliceValid) return false;
                bool ask = env.sliceIndex == 0;
                if (!ask) {
                    if (!env.chunkValid) return false;
                    ask = env.sliceIndex == static_cast<int16_t>(env.chunkLast - 1);
                }
                if (ask && env.calls->RoadAdvance() != 0) done = 1;
            }
        } else {
            // --- 3. the milestone, 0x800B9B34 ----------------------------------------------
            if (raceType() == 44u && handle() < players()) {
                const uint32_t m = gs[0x39];
                if (env.milestones == nullptr || static_cast<int32_t>(m) >= env.milestoneCount)
                    return false;
                const uint8_t* ms = env.milestones + 4u * m;
                // The record's u16 road half against the WHOLE of entity[+0x168], so it can only
                // match while the `kind` half is zero.
                if (static_cast<uint32_t>(LoadU16(ms)) ==
                    static_cast<uint32_t>(LoadS32(env.entity + 0x168)))
                    flags = 2;
                if (m == 0 && *env.timeLimit < clock()) {
                    const uint32_t f = e.U32(ent::kFlagsA);
                    if ((f & 0x08000000u) == 0) {
                        bool stamp = true;
                        if (flags != 0) {
                            // entity[+0x172] is the integer half of the 16.16 distance at +0x170,
                            // read SIGNED, so this 400-unit window wraps at 32768.
                            const int32_t d = static_cast<int32_t>(
                                static_cast<uint32_t>(
                                    static_cast<int32_t>(LoadS16(env.entity + 0x172))) -
                                static_cast<uint32_t>(LoadU16(ms + 2)));
                            if (MipsAbs(d) < 400) {
                                e.SetU32(ent::kFlagsA, f | 0x28000000u);
                                stamp = false;
                            }
                        }
                        if (stamp) {
                            rd[0x27] = 250;
                            done = 1;
                        }
                    }
                }
            }

            // --- the route source, 0x800B9C08..0x800B9C68 -----------------------------------
            // `owner[+0x25C] < 3` - "the rider is still on the bike" - selects whose road binding
            // and whose route object the rest of the function reads.
            const uint8_t* src = nullptr;
            const uint8_t* routeObj = nullptr;
            if (static_cast<uint32_t>(LoadS32(ow + 0x25C)) < 3u) {
                src = env.entity;
                routeObj = env.entityRoute;
            } else {
                src = ow;
                routeObj = env.ownerRoute;
            }
            if (env.routeRecord == nullptr) return false; // the original has no null guard here
            uint32_t bit = 0;
            if (LoadS32(src + 0x168) == LoadS32(env.routeRecord) && routeObj != nullptr) {
                // A per-rider bit of the route object's own halfword at +0x76, selected by the
                // bike's handle. `srav` takes the shift amount modulo 32.
                const uint32_t w = LoadU16(routeObj + 118);
                bit = (w >> (handle() & 31u)) & 1u;
            }
            flags |= bit;

            // --- the distance the crossing tests use, 0x800B9C74..0x800B9CE4 ----------------
            if (flags != 0) {
                if (static_cast<uint32_t>(LoadS32(ow + 0x25C)) < 3u) {
                    if (e.S16(ent::kLiveState) != 0) {
                        if (!env.sliceValid) return false;
                        const int32_t base[3] = {LoadS32(env.entity + 0x1F8),
                                                 LoadS32(env.entity + 0x1FC),
                                                 LoadS32(env.entity + 0x200)};
                        const int16_t dir[3] = {LoadS16(env.entity + 0x210),
                                                LoadS16(env.entity + 0x212),
                                                LoadS16(env.entity + 0x214)};
                        int32_t p[3] = {0, 0, 0};
                        MulAdd(base, dir, LoadS32(env.entity + 0x134), p);
                        int32_t out = 0;
                        RoadProject(p, env.slice, nullptr, &out); // lateral pointer is null here
                        frameWord = out;
                        frameWordSet = true;
                        along = static_cast<int32_t>(static_cast<uint32_t>(env.sliceAlongBase) +
                                                     static_cast<uint32_t>(out));
                    } else {
                        along = LoadS32(env.entity + 0x170);
                    }
                } else {
                    along = LoadS32(ow + 0x170);
                }
            }

            // --- 4. the finish line, 0x800B9CF0 ---------------------------------------------
            if ((flags & 1u) != 0) {
                const int32_t delta =
                    static_cast<int32_t>(static_cast<uint32_t>(along) -
                                         static_cast<uint32_t>(LoadS32(env.routeRecord + 4)));
                frameWord = delta;
                frameWordSet = true;
                done = 0; // 0x800B9CFC - this arm RESETS the answer arms 2 and 3 may have set
                const int32_t leg = LoadS32(env.routeRecord + 0x0C);
                bool binding = (routeObj != nullptr && LoadS32(routeObj) == leg);
                if (!binding) binding = (leg == -1);
                if (binding && SameSign(LoadS32(env.routeRecord + 8), delta)) done = 1;

                // ... and the same test for the partner of a two-rider bike, 0x800B9D54.
                if ((ow[0x23C] & 0x10u) != 0) {
                    if (env.partnerOwner == nullptr) return false; // no null guard in the original
                    const uint32_t k =
                        static_cast<uint32_t>(LoadS32(env.partnerOwner + 0x25C)) - 3u;
                    if (k < 2u) {
                        const int32_t pdelta = static_cast<int32_t>(
                            static_cast<uint32_t>(LoadS32(env.partnerOwner + 0x170)) -
                            static_cast<uint32_t>(LoadS32(env.routeRecord + 4)));
                        const uint32_t w =
                            static_cast<uint32_t>(LoadS32(env.partnerOwner + 0x168));
                        frameWord = pdelta; // 0x800B9DB8, a delay slot: stored either way
                        frameWordSet = true;
                        if ((w >> 16) == 0 &&
                            (w & 0xFFFFu) == static_cast<uint32_t>(LoadS32(env.routeRecord))) {
                            const int32_t pleg = LoadS32(env.routeRecord + 0x0C);
                            bool pbind =
                                (env.partnerRoute != nullptr && LoadS32(env.partnerRoute) == pleg);
                            if (!pbind) pbind = (pleg == -1);
                            int32_t add = 0;
                            if (pbind && SameSign(LoadS32(env.routeRecord + 8), pdelta)) add = 1;
                            done |= add;
                        }
                    }
                }
            }

            // --- 5. the milestone advance, 0x800B9E34 ---------------------------------------
            if ((flags & 2u) != 0) {
                const uint32_t m = gs[0x39];
                if (env.milestones == nullptr || static_cast<int32_t>(m) >= env.milestoneCount)
                    return false;
                const uint8_t* ms = env.milestones + 4u * m;
                const int32_t dir = LoadS32(env.entity + 0x16C);
                const int32_t delta = static_cast<int32_t>(
                    static_cast<uint32_t>(along) -
                    (static_cast<uint32_t>(LoadU16(ms + 2)) << 16));
                frameWord = delta; // 0x800B9E70, a delay slot again
                frameWordSet = true;
                if (SameSign(dir, *env.startDir) && SameSign(dir, delta)) {
                    gs[0x39] = static_cast<uint8_t>(gs[0x39] + 1u);
                    done = env.calls->MilestoneAdvance(); // RASHCDG 0x800C8D4C
                }
                if (gs[0x39] == 1u) env.calls->MilestoneFirst(); // RASHCDG 0x800C92F8
            }
        }
    }

    if (done == 0) return true; // 0x800B9F20 -> 0x800BA2DC, v0 = 0

    // --- the result, 0x800B9F28 ----------------------------------------------------------------
    if (handle() < players()) {
        const uint32_t f = e.U32(ent::kFlagsA);
        if ((f & 0x08000000u) == 0) {
            e.SetU32(ent::kFlagsA, f | 0x08000000u);
            *env.postDelay = 0; // 0x800B9F60 - the post-race word starts again from zero
        }
        bool stored = false;
        uint8_t code = 0;
        if (raceType() == 36u) { // 0x800B9F74
            if (static_cast<uint32_t>(rd[0x27]) < 247u && !(*env.timeLimit < clock())) {
                code = 251;
                stored = true;
            }
        }
        if (!stored) {
            if (raceType() == 33u) {
                code = 250;
                stored = true;
            } else if ((raceType() & 1u) != 0 && (rd[1] & 0x0Fu) == 2u) {
                code = 250; // a police rider on a police-side race
                stored = true;
            } else if (gs[0x39] == 1u) {
                code = 250;
                stored = true;
            } else if (gs[0x39] == 2u) {
                code = 254;
                stored = true;
            } else if ((raceType() & 4u) != 0) {
                StoreS32(env.jailbreakClock, clock()); // *(0x800D9C4C), 0x800BA030
                const int32_t limit = (raceType() == 44u) ? 0x34BC0 : 0x1A5E0;
                if (limit < clock()) {
                    code = 250;
                    stored = true;
                }
            }
        }
        if (stored) rd[0x27] = code;

        // --- race type 17 hands the OTHER player the same finish time, 0x800BA080 --------------
        // This is INSIDE the player guard: `beqz v1,0x800BA100` at 0x800B9F3C jumps past both the
        // result-code block above and this arm, so an AI rider never reaches either.
        if (raceType() == 17u && (rd[1] & 0x0Fu) != 2u) {
            const uint32_t idx = (handle() < 1u) ? 1u : 0u; // `sltiu v0,handle,1`
            uint8_t* other = env.playerRiderDef[idx];
            if (other == nullptr) return false; // the original chases it with no guard
            if (LoadS32(other + 0x28) == 0 && static_cast<uint32_t>(other[0x27]) < 248u)
                StoreS32(other + 0x28, clock());
        }
    }

    // --- the place, the stamp and the two result records, 0x800BA100..0x800BA144 ---------------
    if (static_cast<uint32_t>(rd[0x27]) < 247u) {
        int32_t place = 0;
        if (!env.calls->ComputePlaceMode1(&place)) return false;
        rd[0x27] = static_cast<uint8_t>(place);
    }
    StoreS32(rd + 0x28, clock()); // 0x800BA138, in the call's DELAY SLOT: before the call
    env.calls->StampResult();     // RASHCDG 0x800BC7CC(entity)
    env.calls->RecordFinish(handle(), 1); // SLUS 0x8003F680(handle, 1)

    // --- the two-player tie-break, 0x800BA148. Reached only when THIS bike is player 2. --------
    if (env.player2 != nullptr && env.player2 == env.entity && (raceType() & 1u) == 0) {
        if (env.p1Entity == nullptr || env.p1RiderDef == nullptr) return false;
        if (LoadS32(env.p1RiderDef + 0x28) == clock() &&
            static_cast<uint32_t>(rd[0x27]) < 247u) {
            const uint8_t p1Place = env.p1RiderDef[0x27];
            const uint8_t myPlace = rd[0x27];
            if (static_cast<uint32_t>(p1Place) < 247u) {
                // The original reads one word of its own frame here that not every path into this
                // block has written. Declining is the only honest answer a port can give.
                if (!frameWordSet || env.routeRecord == nullptr) return false;
                const int32_t theirs = MipsAbs(static_cast<int32_t>(
                    static_cast<uint32_t>(LoadS32(env.p1Entity + 0x170)) -
                    static_cast<uint32_t>(LoadS32(env.routeRecord + 4))));
                if (MipsAbs(frameWord) < theirs) {
                    rd[0x27] = p1Place;
                    env.p1RiderDef[0x27] = myPlace;
                    // 0x800BA218..0x800BA244: the finishing-order table at 0x800D5DA8, 16-byte
                    // records indexed by `place - 1`. The original bounds the index by nothing.
                    const int32_t a = (static_cast<int32_t>(rd[0x27]) - 1) * 16;
                    const int32_t b = (static_cast<int32_t>(myPlace) - 1) * 16;
                    if (env.finishOrder == nullptr || a < 0 || b < 0 ||
                        a + 4 > env.finishOrderCount || b + 4 > env.finishOrderCount)
                        return false;
                    StoreS32(env.finishOrder + a, static_cast<int32_t>(handle()));
                    StoreS32(env.finishOrder + b,
                             static_cast<int32_t>(EntityView(env.p1Entity).U16(ent::kHandle)));
                }
            }
        }
    }

    // --- the two-player dismount, 0x800BA24C ---------------------------------------------------
    if (LoadS32(gs + 0x30) == 2 && e.S16(ent::kLiveState) != 0 &&
        static_cast<uint32_t>(LoadS32(ow + 0x25C)) < 2u && !(handle() < 2u)) {
        int32_t v = LoadS32(env.entity + 0x30);
        const int32_t w = LoadS32(env.entity + 0x2C);
        if (w < v) v = w;
        const int32_t x = static_cast<int32_t>(static_cast<uint32_t>(v) << 10);
        frameWord = x;
        frameWordSet = true;
        if (!(x < 3201)) {
            e.SetU16(ent::kLiveState, 0);
            EntityView(ow).SetU16(ent::kLiveState, 0);
        }
    }
    (void)frameWordSet;
    *result = 1;
    return true;
}

// ---------------------------------------------------------------------------- SLUS 0x8003F680
bool RecordFinish(int32_t flag, const FinishOrderEnv& env) {
    if (env.entity == nullptr || env.riderDef == nullptr) return false;
    // 0x8003F6A8..0x8003F6B8: a place of 1..18 and nothing else reaches the table.
    if (!(static_cast<uint32_t>(env.riderDef[0x27]) - 1u < 18u)) return true;
    const int32_t at = 16 * static_cast<int32_t>(env.riderDef[0x27]);
    if (env.table == nullptr || at < 0 || at + 16 > env.tableBytes) return false;
    // The handle comes back out of the entity, not out of the argument (0x8003F6C8).
    StoreS32(env.table + at + 0, static_cast<int32_t>(LoadU16(env.entity + ent::kHandle)));
    StoreS32(env.table + at + 4, env.riderDef[0x27]);
    StoreS32(env.table + at + 8, flag);
    StoreS32(env.table + at + 12, LoadS32(env.riderDef + 0x28));
    return true;
}

// ---------------------------------------------------------------------------- RASHCDG 0x80092C7C
bool EndRace(int32_t reason, const EndRaceEnv& env) {
    if (env.entity == nullptr || env.riderDef == nullptr || env.gameState == nullptr) return false;
    EntityView e(env.entity);
    // 0x80092C9C..0x80092CB8. Both of these happen on every call, whatever the rider's state.
    e.SetU32(ent::kFlagsA, e.U32(ent::kFlagsA) | 0x08000000u);
    *env.postDelay = 0;

    if (LoadS32(env.riderDef + 0x28) == 0) {
        // 0x80092CBC..0x80092CCC. The delay slot loads 255 and the FALL-THROUGH loads 254, so the
        // code is 254 when `reason == 9` and 255 otherwise (not the other way round).
        env.riderDef[0x27] = (reason == 9) ? 254u : 255u;
        StoreS32(env.riderDef + 0x28, LoadS32(env.gameState + 0x10)); // the call's delay slot
        env.calls->StampResult();                                     // RASHCDG 0x800BC7CC(entity)
        if (env.riderDef[0x27] == 255u) { // re-read: the callee ran in between
            if (env.owner == nullptr) return false;
            if (static_cast<uint32_t>(LoadS32(env.owner + 0x25C)) < 2u) {
                env.calls->WipeoutEffect(); // SLUS 0x8002090C(entity)
                EntityView post(env.entity);
                post.SetU32(0x238, post.U32(0x238) | 0x08000000u);
            }
        }
    }

    if (env.view == nullptr) return false;
    if (reason == 10) {
        // 0x80092D3C..0x80092DA4
        StoreS32(env.view + 0x304, 1);
        StoreS32(env.view + 0x228, static_cast<int32_t>(
                                       static_cast<uint32_t>(LoadS32(env.view + 0x228)) | 0x14u));
    } else if ((LoadS32(env.view + 0x228) & 4) == 0) {
        env.calls->ViewEvent(reason); // RASHCDG 0x8008A998(view, reason)
    }
    return true;
}

} // namespace rr::sim
