// Bench rows of the pedestrians of pool 2 (src\game\sim\peds.h). Included LAST by verify_physics.cpp
// inside its anonymous namespace, after
// rows_world.inc, whose helpers (worldrows::Cells / Record / Count / EyeNear) it uses.
//
// The machine is a race capture (rr-race by default: three live pedestrians in pool 2, the switch
// 0x8005B254 on, 21 of the 23 animation objects in use). Every case stirs one or all of the live
// pedestrians inside the states the game gives them: the state +0x229 0..5 with its side +0x22A, the
// "start the clip" bit, the knocked / thrown bits 26 / 29 / 30 of +0x228 now and then, the walk kind
// +0x234 0..2, the flags +0x235 bits 0..3, the class row +0x236 0..18, the speed, the lateral inside or
// past the edges, the walk length and the distance walked, the draw bits +0x09, the re-seat bit +0x238,
// the velocity and lift of a knocked one; the player's bike put ahead of or behind a pedestrian, heading
// at it or not, and the reaction timer and clock around their threshold. dt = 218 x ticks (1..12), the
// race tick's. The seams (peds.h) are answered by the original code at the port's
// call-site sp and their call sequence compared.
//
// Named bounds (each a state the game builds or keeps):
//   * a pedestrian's animation object +0x21C is its own (the capture's; the spawner's ViewSlot gives one);
//   * the class row is one of the 19 of 0x800CCC20 (the spawner stores cls < 19, Rand % 13, % 19, % 6 + 13);
//   * the spawner is called by the cell walker with a1 = 1 only (population.md 6) - the a1 = 0 arm is
//     ported and stirred too, on the same records.
namespace pedrows {

namespace s = rr::sim;
using worldrows::Chance;
using worldrows::Get16;
using worldrows::Put16;
using worldrows::Put8;

uint32_t Dt(Rng& r) { return 218u * (1u + r.U32() % 12u); }

// The live pedestrians of the machine.
std::vector<uint32_t> LivePeds(Memory& m) {
    std::vector<uint32_t> v;
    const uint32_t base = m.PeekWord(s::kPedBlock + 16u);
    if (base == 0) return v;
    for (uint32_t i = 0; i < 4; ++i)
        if (Get16(m, base + 572u * i + 172u) != 0) v.push_back(base + 572u * i);
    return v;
}

// One pedestrian stirred inside the game's states (the header's list).
void StirPed(Rng& r, Memory& m, uint32_t e, bool knocked) {
    uint32_t f = m.PeekWord(e + 552u);
    const uint32_t state = Chance(r, 3) ? (f >> 8) & 0xFFu : r.U32() % 6u;
    f = (f & 0xFF0000FEu) | (state << 8) | ((r.U32() % 2u) << 16) | (Chance(r, 3) ? 1u : 0u);
    f &= ~0x64000000u;
    if (knocked && Chance(r, 4)) {
        static const uint32_t kK[4] = {0x20000000u, 0x60000000u, 0x24000000u, 0x64000000u};
        f |= kK[r.U32() % 4u];
    }
    m.PokeWord(e + 552u, f);
    if (Chance(r, 2)) Put8(m, e + 564u, static_cast<uint8_t>(r.U32() % 3u));
    if (Chance(r, 2)) Put8(m, e + 565u, static_cast<uint8_t>(r.U32() & 0xFu));
    if (Chance(r, 3)) Put8(m, e + 566u, static_cast<uint8_t>(r.U32() % 19u));
    if (Chance(r, 3)) m.PokeWord(e + 480u, Chance(r, 2) ? m.PeekWord(s::kPedClassTable + 16u * m.PeekByte(e + 566u) + 8u)
                                                        : r.U32() % 0x30000u);
    if (Chance(r, 3)) m.PokeWord(e + 344u, m.PeekWord(e + 344u) + (r.U32() % 0x100000u) - 0x80000u);
    if (Chance(r, 3)) {
        const uint32_t len = r.U32() % 0x200000u;
        m.PokeWord(e + 556u, len);
        m.PokeWord(e + 560u, Chance(r, 3) ? len - r.U32() % 0x20000u : r.U32() % (len + 1u));
    }
    if (Chance(r, 3)) Put8(m, e + 9u, static_cast<uint8_t>((m.PeekByte(e + 9u) & 0xFCu) | (r.U32() % 4u)));
    if (Chance(r, 4)) m.PokeWord(e + 568u, m.PeekWord(e + 568u) ^ 2u);
    if (Chance(r, 3)) Put16(m, e + 544u, static_cast<uint16_t>(Chance(r, 2) ? 224u + r.U32() % 13u : 40u + r.U32() % 30u));
    if (f & 0x20000000u) {
        for (uint32_t k = 0; k < 3; ++k) m.PokeWord(e + 456u + 4u * k, (r.U32() % 0x100000u) - 0x80000u);
        m.PokeWord(e + 508u, Chance(r, 2) ? 0u : r.U32() % 0x8000u);
    }
}

// The player's bike put near pedestrian `e`: ahead of it or behind, heading at it or not, faster or slower.
void BikeNear(Rng& r, Memory& m, uint32_t e) {
    const uint32_t bike = m.PeekWord(0x8005B38Cu);
    if (Chance(r, 4)) return;
    const int32_t dist = static_cast<int32_t>(r.U32() % 60u) - 20;          // whole units along the walk
    const int16_t fx = static_cast<int16_t>(Get16(m, e + 444u)), fz = static_cast<int16_t>(Get16(m, e + 448u));
    m.PokeWord(bike + 184u, m.PeekWord(e + 184u) + static_cast<uint32_t>((dist * fx) << 4));
    m.PokeWord(bike + 192u, m.PeekWord(e + 192u) + static_cast<uint32_t>((dist * fz) << 4));
    if (Chance(r, 2)) { // heading at the pedestrian
        Put16(m, bike + 444u, static_cast<uint16_t>(-fx));
        Put16(m, bike + 446u, static_cast<uint16_t>(-static_cast<int16_t>(Get16(m, e + 446u))));
        Put16(m, bike + 448u, static_cast<uint16_t>(-fz));
    }
    if (Chance(r, 2)) m.PokeWord(bike + 480u, r.U32() % 0x600000u);
    if (Chance(r, 3)) m.PokeWord(bike + 344u, m.PeekWord(e + 344u) + (r.U32() % 0x80000u) - 0x40000u);
    const uint32_t gs = m.PeekWord(0x8005B2F8u);
    if (Chance(r, 2)) m.PokeWord(s::kPedReactTimer, 300u + r.U32() % 900u);
    if (Chance(r, 2)) m.PokeWord(e + 548u, m.PeekWord(gs + 16u) - r.U32() % 1400u);
}

// Every live pedestrian stirred (the passes).
void StirAll(CaseContext& c, bool knocked) {
    for (uint32_t e : LivePeds(*c.mem)) {
        StirPed(*c.rng, *c.mem, e, knocked);
        if (Chance(*c.rng, 2)) Put16(*c.mem, e + 320u, Chance(*c.rng, 4) ? 0u : 1u);
    }
    const std::vector<uint32_t> v = LivePeds(*c.mem);
    if (!v.empty()) BikeNear(*c.rng, *c.mem, v[c.rng->U32() % v.size()]);
}

// One pedestrian stirred; 0 when the machine has none.
uint32_t OnePed(CaseContext& c, bool knocked) {
    const std::vector<uint32_t> v = LivePeds(*c.mem);
    if (v.empty()) return 0;
    const uint32_t e = v[c.rng->U32() % v.size()];
    StirPed(*c.rng, *c.mem, e, knocked);
    BikeNear(*c.rng, *c.mem, e);
    return e;
}

// The spawner's case: a kind-2 record of a resident cell, the eye near it, pool 2 with free slots (a live
// pedestrian released as PoolRelease leaves it), full, or the animation objects all taken.
void SpawnArgs(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    std::vector<uint32_t> recs;
    for (const worldrows::Cell& cl : worldrows::Cells(m))
        for (uint32_t i = 0; i < worldrows::Count(m, cl.r0, 1); ++i) recs.push_back(worldrows::Record(m, cl.r0, 1, i));
    uint32_t rec = recs.empty() ? 0u : recs[r.U32() % recs.size()];
    if (rec != 0) {
        if (!Chance(r, 5)) worldrows::EyeNear(r, m, rec, static_cast<int32_t>(r.U32() % 180u));
        if (Chance(r, 4)) m.PokeWord(rec + 40u, r.U32() % 5u);
        if (Chance(r, 4)) m.PokeWord(rec + 44u, r.U32() % 5u);
        if (Chance(r, 3)) Put16(m, rec + 2u, static_cast<uint16_t>(r.U32() % 25u)); // every class arm
        if (Chance(r, 4)) m.PokeWord(rec + 32u, Chance(r, 2) ? (Chance(r, 2) ? 0x10000u : 0xFFFF0000u) : r.U32() % 0x60000u - 0x30000u);
    }
    const uint32_t base = m.PeekWord(s::kPedBlock + 16u);
    int32_t live = 0, high = -1, next = 4;
    for (uint32_t i = 0; i < 4 && base != 0; ++i) {
        const uint32_t e = base + 572u * i;
        if (Get16(m, e + 172u) != 0 && Chance(r, 2)) Put16(m, e + 172u, 0); // released
        if (Get16(m, e + 172u) != 0) {
            ++live;
            high = static_cast<int32_t>(i);
        } else if (next == 4 && m.PeekWord(e + 4u) != 0) {
            next = static_cast<int32_t>(i);
        }
    }
    m.PokeWord(s::kPedBlock, static_cast<uint32_t>(live));
    m.PokeWord(s::kPedBlock + 4u, static_cast<uint32_t>(next));
    m.PokeWord(s::kPedBlock + 8u, static_cast<uint32_t>(high));
    if (Chance(r, 8)) m.PokeWord(0x800D8744u, static_cast<uint32_t>(live)); // the live cap reached
    if (Chance(r, 10)) m.PokeWord(0x800CE178u, m.PeekWord(0x800CE17Cu)); // no animation object left
    if (Chance(r, 3)) m.PokeWord(s::kPedObjectMask, r.U32() & 0xFFu);
    a.a[0] = rec;
    a.a[1] = Chance(r, 5) ? 0u : 1u;
    a.a[2] = m.PeekWord(0x8005B38Cu);
}

std::vector<OracleCallee> Seams() {
    return {OracleCallee{s::kPedBankSwitchFn, 2}, OracleCallee{s::kPedLoopStartFn, 4, 1},
            OracleCallee{s::kPedHardStartFn, 4, 1}, OracleCallee{s::kPedClipDoneFn, 1},
            OracleCallee{s::kPedHoldFn, 1},         OracleCallee{s::kPedResumeFn, 1},
            OracleCallee{s::kPedGetUpFn, 1},        OracleCallee{s::kPedSettleFn, 1},
            OracleCallee{s::kPedSlideFn, 3},        OracleCallee{s::kPedBoxFn, 1},
            OracleCallee{s::kPedVoiceFn, 4}};
}

template <class F>
uint32_t RunPeds(NativeEnv& env, F&& f) {
    s::GuestRam g = RqView(env);
    rcv::RecoverOracle o(env);
    const s::BikeTables t = rcv::Tables(env, g);
    uint32_t v0 = 0;
    const bool ok = f(g, t, o, v0);
    if (!ok && env.failure.empty() && !g.Faulted()) env.failure = "the port refused";
    return RqDone(env, g, v0);
}

Row PRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
         std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0 = true, bool seams = false) {
    Row r{name, sig, entry, std::move(setup), nullptr, v0, std::move(native)};
    r.maxSteps = 40'000'000;
    if (seams) r.oracleCallees = Seams();
    return r;
}

} // namespace pedrows

void AddPedsRows(std::vector<Row>& rows) {
    using namespace pedrows;
    using G = s::GuestRam;
    using T = s::BikeTables;
    using C = rr::sim::RecoverCallees;
    rows.push_back(PRow("peds_edge", "s32 0x800CA42C(e, s32 *step)  [RASHCDG, the walk meets the edge or its length]",
                        s::kPedEdgeFn,
                        [](CaseContext& c, Args& a) {
                            const uint32_t e = OnePed(c, false);
                            c.mem->PokeWord(c.scratch, Chance(*c.rng, 2) ? c.rng->U32() % 0x40000u : c.rng->U32() % 0x400u);
                            if (e != 0 && Chance(*c.rng, 4)) c.mem->PokeWord(e + 388u, c.mem->PeekWord(e + 388u) ^ (1u << (20 + c.rng->U32() % 4u)));
                            a.a[0] = e;
                            a.a[1] = c.scratch;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunPeds(env, [&](G& g, const T&, C&, uint32_t& v) {
                                v = static_cast<uint32_t>(s::PedEdge(g, a.a[0], a.a[1]));
                                return true;
                            });
                        }));
    rows.push_back(PRow("peds_walk_along", "void 0x800CA564(e, dt)  [RASHCDG, a step of the walk along the road]",
                        s::kPedWalkAFn,
                        [](CaseContext& c, Args& a) {
                            a.a[0] = OnePed(c, false);
                            a.a[1] = Chance(*c.rng, 8) ? 0u : Dt(*c.rng);
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunPeds(env, [&](G& g, const T& t, C&, uint32_t&) {
                                return s::PedWalkA(g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp, t);
                            });
                        }, false));
    rows.push_back(PRow("peds_walk_across", "void 0x800CA7C4(e, dt)  [RASHCDG, a step of the walk across the road]",
                        s::kPedWalkBFn,
                        [](CaseContext& c, Args& a) {
                            a.a[0] = OnePed(c, false);
                            a.a[1] = Chance(*c.rng, 8) ? 0u : Dt(*c.rng);
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunPeds(env, [&](G& g, const T& t, C&, uint32_t&) {
                                return s::PedWalkB(g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp, t);
                            });
                        }, false));
    rows.push_back(PRow("peds_step", "void 0x800CAA44(e, dt)  [RASHCDG, the walk of the pedestrian's kind]", s::kPedStepFn,
                        [](CaseContext& c, Args& a) {
                            a.a[0] = OnePed(c, false);
                            a.a[1] = Dt(*c.rng);
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunPeds(env, [&](G& g, const T& t, C&, uint32_t&) {
                                return s::PedStep(g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp, t);
                            });
                        }, false));
    rows.push_back(PRow("peds_start", "void 0x800CAAA8(e)  [RASHCDG, the walk set up]", s::kPedStartFn,
                        [](CaseContext& c, Args& a) {
                            const uint32_t e = OnePed(c, false);
                            if (e != 0 && Chance(*c.rng, 2)) Put8(*c.mem, e + 565u, static_cast<uint8_t>(c.mem->PeekByte(e + 565u) & 0xFBu));
                            a.a[0] = e;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunPeds(env, [&](G& g, const T& t, C&, uint32_t&) {
                                return s::PedStart(g, a.a[0], env.sp, t);
                            });
                        }, false));
    rows.push_back(PRow("peds_anim", "void 0x800CAAF0(e)  [RASHCDG, the pedestrian's animation state machine]", s::kPedAnimFn,
                        [](CaseContext& c, Args& a) { a.a[0] = OnePed(c, true); },
                        [](NativeEnv& env, const Args& a) {
                            return RunPeds(env, [&](G& g, const T& t, C& o, uint32_t&) {
                                return s::PedAnim(g, a.a[0], env.sp, t, o);
                            });
                        }, false, true));
    rows.push_back(PRow("peds_face", "void 0x800CB02C(e)  [RASHCDG, a bike heading at the pedestrian]", s::kPedFaceFn,
                        [](CaseContext& c, Args& a) {
                            const uint32_t e = OnePed(c, false);
                            if (Chance(*c.rng, 5)) { // the player off his bike
                                const uint32_t bike = c.mem->PeekWord(0x8005B38Cu);
                                c.mem->PokeWord(c.mem->PeekWord(bike + 852u) + 604u, 3u + c.rng->U32() % 3u);
                            }
                            a.a[0] = e;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunPeds(env, [&](G& g, const T& t, C&, uint32_t&) {
                                return s::PedFace(g, a.a[0], env.sp, t);
                            });
                        }, false));
    rows.push_back(PRow("peds_pass", "void 0x800CB304(dt)  [RASHCDG, WorldBikePass's pedestrian pass]", s::kPedsPassFn,
                        [](CaseContext& c, Args& a) {
                            StirAll(c, true);
                            a.a[0] = Dt(*c.rng);
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunPeds(env, [&](G& g, const T& t, C& o, uint32_t&) {
                                return s::PedPass(g, static_cast<int32_t>(a.a[0]), env.sp, t, o);
                            });
                        }, false, true));
    rows.push_back(PRow("peds_release", "void 0x800CB4F8(dt)  [RASHCDG, the rider-engine pass's pedestrian pass]",
                        s::kPedsReleaseFn,
                        [](CaseContext& c, Args& a) {
                            StirAll(c, true);
                            if (Chance(*c.rng, 3)) c.mem->PokeWord(s::kPedDrag, c.rng->U32() % 0x8000u);
                            a.a[0] = Dt(*c.rng);
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunPeds(env, [&](G& g, const T& t, C& o, uint32_t&) {
                                return s::PedRelease(g, static_cast<int32_t>(a.a[0]), env.sp, t, o);
                            });
                        }, false, true));
    rows.push_back(PRow("peds_prop", "s32 0x800CB78C(e, kind, lod)  [RASHCDG, a held object for a pedestrian]", s::kPedPropFn,
                        [](CaseContext& c, Args& a) {
                            const uint32_t e = OnePed(c, false);
                            c.mem->PokeWord(s::kPedObjectMask, Chance(*c.rng, 6) ? 255u : c.rng->U32() & 0xFFu);
                            a.a[0] = e;
                            a.a[1] = 10;
                            a.a[2] = Chance(*c.rng, 3) ? c.rng->U32() % 10u : 9u;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return RunPeds(env, [&](G& g, const T&, C&, uint32_t& v) {
                                v = s::PedProp(g, a.a[0], static_cast<int32_t>(a.a[1]), static_cast<int32_t>(a.a[2]));
                                return true;
                            });
                        }));
    rows.push_back(PRow("peds_spawn", "Ped *0x800CB8C8(rec, onRecord, bike)  [RASHCDG, the pedestrian spawner]", s::kPedSpawnFn,
                        SpawnArgs,
                        [](NativeEnv& env, const Args& a) {
                            return RunPeds(env, [&](G& g, const T& t, C& o, uint32_t& v) {
                                return s::PedSpawn(g, a.a[0], a.a[1], a.a[2], env.sp, t, o, v);
                            });
                        }, true, true));
}
