// Bench rows for the traffic domain: the two functions the population transitions still refused
// (RiderDismount RASHCDG 0x800C3104, RowsFromHeading RASHCDG 0x8007EC30), the traffic's seam callees,
// the traffic pass and the police scheduler. Included by
// verify_physics.cpp inside its anonymous namespace, AFTER rows_population.inc, so the pop:: helpers
// (families, PopSeams, TrafficRow) are visible here.
//
// One file per sub-pass, included below; AddTrafficRows appends them in a fixed order.

void AddTrafficOwnRows(std::vector<Row>& rows);    // this file, below the includes
void AddTrafficLeafRows(std::vector<Row>& rows);   // rows_traffic_leaves.inc
void AddTrafficBindRows(std::vector<Row>& rows);   // rows_traffic_bind.inc
void AddTrafficDriveRows(std::vector<Row>& rows);  // rows_traffic_drive.inc
void AddPoliceRows(std::vector<Row>& rows);        // rows_police.inc

#include "rows_traffic_leaves.inc"
#include "rows_traffic_bind.inc"
#include "rows_traffic_drive.inc"
#include "rows_police.inc"

// ================================================================ RASHCDG 0x8007EC30 / 0x800C3104
namespace traffic {

// RowsFromHeading reads the heading +0x1C2..+0x1C6, the slice +0x154 (its +8..+12, the up vector) and
// the second bike +0x358. Dump-derived: a bike of this machine as it is (its own slice and heading);
// randomised: the heading drawn (unit-ish rows, extremes -0x8000 / 0x7FFF that saturate the OP), the
// slice another bike's or a drawn one in the scratch block, the second bike 0 or one of the 18.
// NAMED BOUND: +0x154 and +0x358 are pointers into RAM (a slice / a bike or 0): the original chases
// both without a test.
void SetupRowsFromHeading(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t e = pop::Bike(m, r.U32() % 18u);
    if (c.sample == nullptr) {
        static const int16_t kExt[6] = {-32768, 32767, 4096, -4096, 0, 1};
        for (uint32_t k = 0; k < 3; ++k)
            PutS16(m, e + 450 + 2u * k, pop::Chance(r, 4) ? kExt[r.U32() % 6u] : static_cast<int16_t>(r.U32() % 8193u) - 4096);
        if (pop::Chance(r, 3)) {
            const uint32_t sl = c.scratch + 0x300;
            for (uint32_t k = 0; k < 16; k += 2) PutS16(m, sl + k, pop::Chance(r, 4) ? kExt[r.U32() % 6u] : static_cast<int16_t>(r.U32()));
            m.PokeWord(e + 340, sl);
        } else if (pop::Chance(r, 2)) {
            m.PokeWord(e + 340, m.PeekWord(pop::Bike(m, r.U32() % 18u) + 340));
        }
        if (pop::Chance(r, 2)) m.PokeWord(e + 856, pop::Chance(r, 2) ? 0u : pop::Bike(m, r.U32() % 18u));
    }
    a.a[0] = e;
}
uint32_t NativeRowsFromHeading(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    const uint32_t v = rr::sim::RowsFromHeading(g, a.a[0]);
    return pop::PopDone(env, g, true, "RowsFromHeading", v);
}

// RiderDismount's callees are the ported stance layer: the event witnessed (declared 0x800C4550, run
// natively by AnimNativeStanceEvent), StanceLeave and the animation stop run natively on the clone,
// the layer's own four seams (TransitionCapture, ApplyFrame, CombatLeave, CombatEnter) on the oracle.
// Each is made at the dismount's frame (sp - 32), the stance layer's own depths below it are not the
// original's (the same concession).
struct DismountBench final : rr::sim::DismountCallees {
    NativeEnv& env;
    explicit DismountBench(NativeEnv& e) : env(e) {}
    template <class F>
    bool At(uint32_t sp, F&& f) {
        const uint32_t saved = env.sp;
        env.sp = sp;
        f();
        env.sp = saved;
        return env.failure.empty();
    }
    bool StanceEvent(uint32_t ev, uint32_t r, uint32_t p, uint32_t sp) override {
        return At(sp, [&] { AnimNativeStanceEvent(env, ev, r, p, sp); });
    }
    bool StanceLeave(uint32_t ev, uint32_t r, uint32_t p, uint32_t sp) override {
        return At(sp, [&] { animrows::RunNative(env, [&](rr::sim::StanceLayer& l) { return l.Leave(ev, r, p); }); });
    }
    bool AnimStop(uint32_t obj, uint32_t sp) override {
        return At(sp, [&] { animrows::RunNative(env, [&](rr::sim::StanceLayer& l) { return l.anim().Stop(obj); }); });
    }
};

std::vector<OracleCallee> DismountSeams() {
    return {OracleCallee{kAiStance, 3}, OracleCallee{rr::sim::kAnimTransitionCaptureFn, 3},
            OracleCallee{rr::sim::kAnimApplyFrameFn, 1}, OracleCallee{rr::sim::kCombatLeaveFn, 4},
            OracleCallee{rr::sim::kCombatEnterFn, 4}};
}

// A rider of this machine with a captured or drawn stance state (rows_anim.inc's family), then the
// fields the dismount decides on: the live word (both arms), the current stance 6 / 77 (the early
// return), +0x23C bit 5 (77 instead of 6), the animation object (0 now and then), and `how`.
void SetupRiderDismount(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const animrows::StanceCase sc = animrows::PlantStanceCase(c, false);
    const uint32_t R = sc.rider;
    if (pop::Chance(r, 2)) WriteU16Bench(m, R + 320, 0);
    else if (pop::Chance(r, 3)) WriteU16Bench(m, R + 320, static_cast<uint16_t>(1u + r.U32() % 3u));
    if (pop::Chance(r, 5)) WriteU16Bench(m, R + 544, pop::Chance(r, 2) ? 6u : 77u);
    if (pop::Chance(r, 3)) pop::Put8(m, R + 572, m.PeekByte(R + 572) ^ 0x20u);
    // NAMED BOUND: the animation object +0x21C is 0 only for a DORMANT rider (the arm that tests it).
    // A live rider's event reaches the bank switch SLUS 0x80012858 on object 0, and the ORIGINAL
    // trapped storing through the kernel's word there (0x80012880) - the same bound as the
    // placement's.
    if (pop::Chance(r, 8) && (m.PeekWord(R + 320) & 0xFFFFu) == 0) m.PokeWord(R + 540, 0);
    // NAMED BOUND (rows_anim.inc's, applied to the event this function makes): a live rider's stance
    // event 6 / 77 is asked for only when that stance's bank is resident in this machine. Otherwise the
    // gate starts a clip index read in a bank that is not loaded and the ORIGINAL trapped in HardStart
    // (0x8005BF8C, a load through the drawn clip table) in 158 of 1160 cases before this bound.
    if (m.PeekWord(R + 320) & 0xFFFFu) {
        const bool b5 = (m.PeekByte(R + 572) & 0x20u) != 0;
        if (!animrows::Resident(m, b5 ? 77u : 6u)) pop::Put8(m, R + 572, m.PeekByte(R + 572) ^ 0x20u);
        const bool n5 = (m.PeekByte(R + 572) & 0x20u) != 0;
        if (!animrows::Resident(m, n5 ? 77u : 6u)) WriteU16Bench(m, R + 320, 0);
    }
    a.a[0] = R;
    a.a[1] = pop::Chance(r, 5) ? r.U32() : (pop::Chance(r, 2) ? 1u : 0u);
}
uint32_t NativeRiderDismount(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    DismountBench cb(env);
    const bool ok = rr::sim::RiderDismount(g, a.a[0], static_cast<int32_t>(a.a[1]), env.sp, cb);
    return pop::PopDone(env, g, ok, "RiderDismount");
}

} // namespace traffic

void AddTrafficOwnRows(std::vector<Row>& rows) {
    {
        Row r{"rows_from_heading", "u16 RowsFromHeading(Bike *e)  [RASHCDG 0x8007EC30]", rr::sim::kRowsFromHeadingFn,
              traffic::SetupRowsFromHeading, nullptr, true, traffic::NativeRowsFromHeading};
        rows.push_back(r);
    }
    {
        Row r{"rider_dismount", "void RiderDismount(Rider *r, s32 how)  [RASHCDG 0x800C3104]", rr::sim::kRiderDismountFn,
              traffic::SetupRiderDismount, nullptr, false, traffic::NativeRiderDismount, traffic::DismountSeams()};
        r.maxSteps = 50'000'000;
        rows.push_back(r);
    }
}

void AddTrafficRows(std::vector<Row>& rows) {
    AddTrafficOwnRows(rows);
    AddTrafficLeafRows(rows);
    AddTrafficBindRows(rows);
    AddTrafficDriveRows(rows);
    AddPoliceRows(rows);
}
