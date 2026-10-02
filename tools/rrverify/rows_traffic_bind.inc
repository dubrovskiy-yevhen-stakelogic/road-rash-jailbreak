// Traffic sub-pass 'bind' (rows_traffic.inc includes this file, after rows_population.inc, inside
// verify_physics.cpp's anonymous namespace): the model binder SLUS 0x8002FAD4 (with 0x8002FDEC,
// 0x8001298C, 0x800302C4), the pool release RASHCDG 0x8008C000 (with 0x800CC0B0, 0x800CB84C), the
// cop effect leave SLUS 0x8002847C (with 0x8002A738) and join SLUS 0x80028034, and the rider-object
// release RASHCDG 0x80095AEC. The port is src\game\sim\traffic_bind.{h,cpp}; every callee is ported
// (spine.h Malloc / effect helpers, population.h CopDrop, anim.h SeatRelease, the LCG), so no row
// declares an oracle callee: the guest runs the whole original tree, the port the whole native one.
//
// INPUT FAMILIES. rr-race holds one traffic car (pool 3 slot 0, handle 0x60, registry slot 10 =
// model 301), the registry slots 0..12 (pool 3's models 306 / 303 / 301 / 309 in slots 8..11), the
// class lists 0x800CE560, the pool tables 0x800D4C38, 18 bikes and riders, 20 free effect records.
//   * dump-derived: this machine's own objects, pools and registry, the class / slot / flag drawn;
//   * randomised: a synthetic object in the scratch block (its part array there, or malloc'd), a
//     registry slot freed, re-pointed out of range or replaced by a synthetic record (LOD count 1..5,
//     DOD3 kinds 0..7, ids 300 / 315 / 309 / other, part counts 0..6), the class lists shortened, the
//     list entries' ids and keys stirred, the race type (44, odd, even), the LCG seed; the pools'
//     live / high / lowest-free words around the released slot and the walked slots' handles; cop
//     effect chains of 0..5 records in every state; the seat bits, seat indices and seat records.
//
// NAMED BOUNDS (each one a place where the ORIGINAL would trap or spin on a drawn value):
//   * CLASS_LIST_COUNT: a class list's s16 count is drawn in 0..its real count, never negative: a
//     negative count passes the `pick < count` test (unsigned) and the list is indexed by a 32-bit
//     remainder, a load far outside RAM.
//   * CLASS_RANGE: pool 1 / 2 classes are drawn in 0..23 and pool 5 / 6 LODs in 0..43: the class
//     indexes byte tables (0x80054114 / 0x8005412C) and the slot table by 2 x class unchecked. A
//     negative LOD (-3..5) is drawn only onto a synthetic registry record put in the pool's slot,
//     whose LOD table has valid entries before it: LodSelect's bound test is signed and a negative
//     LOD reads the 12-byte entries before the table (the real ones hold no pointers there).
//   * PART_ARRAY: an object's part-array word +4 is kept, cleared, or points at a scratch array; a
//     drawn word is stored through (the population rows' DrawPool3 bound, the same store).
//   * ANIM_OBJECT: +0x21C / +0x22C (the animation object the releases free) is kept, cleared or a
//     scratch word, and pool 4 / 5's store-record pointer (+4) a 24-byte record of 0x800D1664: each
//     is stored through.
//   * EFFECT_CHAIN: a join's (CopJoin) chain is planted acyclic, ending in link 63, its records busy:
//     EffectLink (0x800271CC) follows links until 63 and spins forever on a cycle, and a free record
//     in the chain is handed out again and closes one.

namespace tbind {
using pop::Chance;
using pop::Put8;
using pop::U;

constexpr uint32_t kModelBindFn = 0x8002FAD4, kPoolReleaseFn = 0x8008C000, kCopLeaveFn = 0x8002847C,
                   kCopJoinFn = 0x80028034, kReleaseRiderObjFn = 0x80095AEC;
constexpr uint32_t kObjOff = 0x400;     // a synthetic object (0x70 bytes)
constexpr uint32_t kPartsOff = 0x800;   // its part array (up to 42 x 24 bytes)
constexpr uint32_t kRegLodOff = 0x1000; // a synthetic LOD table (3 + 5 x 12 bytes, from +36)
constexpr uint32_t kRegDodOff = 0x1100; // its DOD3s (5 x 0x40)
constexpr uint32_t kRegPtrOff = 0x1300; // its part-pointer arrays (5 x 0x40)
constexpr uint32_t kAnimOff = 0x300;    // a stand-in animation object (+0x24 is written)
constexpr uint32_t kHandleOff = 0x100;  // a handle word (+8 read for pool 3)
constexpr uint32_t kEffects = 0x800D39B0;

uint32_t Rec(uint32_t k) { return kEffects + 112u * k; }

// ---------------------------------------------------------------- the model registry
// A synthetic registry record in slot `slot`: LOD count 1..5, the key byte, the LOD table, DOD3s of
// one kind (0..7) and id (300 / 315 / 309 / other), 0..6 parts per LOD.
void SynthRegistry(CaseContext& c, uint32_t slot, bool lodCase = false) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t reg = rr::sim::kModelRegistry + 16u * slot;
    // a LOD case wants LOD count 1 or 5 (the kind-5 reset leaves LOD 0 or 4) half of the time
    const uint32_t lods = (lodCase && Chance(r, 2)) ? (Chance(r, 2) ? 1u : 5u) : 1u + r.U32() % 5u;
    if (m.PeekWord(reg) == 0) m.PokeWord(reg, 1000u + slot);
    Put8(m, reg + 4, lods);
    Put8(m, reg + 7, Chance(r, 4) ? 0xFFu : r.U32() % 34u);
    m.PokeWord(reg + 8, c.scratch + kRegLodOff + 36u);                    // entries -3..-1 valid too
    static const uint32_t kIds[5] = {300u, 315u, 309u, 301u, 306u};
    static const uint32_t kKinds[12] = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 3u, 3u, 4u, 5u};
    const uint32_t kind = (lodCase && Chance(r, 2)) ? 5u : kKinds[r.U32() % 12u];
    const uint32_t id = kIds[r.U32() % 5u];
    for (uint32_t l = 0; l < 8; ++l) {
        const uint32_t dod = c.scratch + kRegDodOff + 0x40u * (l % 5u);
        const uint32_t ptrs = c.scratch + kRegPtrOff + 0x40u * (l % 5u);
        m.PokeWord(c.scratch + kRegLodOff + 12u * l, dod);
        m.PokeWord(c.scratch + kRegLodOff + 12u * l + 4, ptrs);
        if (l >= 5) continue;
        RrNoise(c, dod, 0x40);
        RrNoise(c, ptrs, 0x40);
        m.PokeWord(dod + 8, Chance(r, 6) ? r.U32() : id);
        WriteU16Bench(m, dod + 14, static_cast<uint16_t>((r.U32() & 0xFF87u) | ((Chance(r, 8) ? r.U32() % 16u : kind) << 3)));
        WriteU16Bench(m, dod + 24, static_cast<uint16_t>(r.U32() % 7u));
    }
}

// The registry and the tables the binder walks, stirred (randomised cases only).
void StirRegistry(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    if (Chance(r, 3)) SynthRegistry(c, r.U32() % 13u);                    // replace a real slot
    if (Chance(r, 8)) m.PokeWord(rr::sim::kModelRegistry + 16u * (r.U32() % 13u), 0); // a free slot
    for (uint32_t p = 1; p <= 6; ++p) {
        const uint32_t list = rr::sim::kModelClassLists + 8u * p;
        const int16_t n = GetS16(m, list);
        if (n > 0 && Chance(r, 6)) PutS16(m, list, static_cast<int16_t>(r.U32() % static_cast<uint32_t>(n + 1))); // CLASS_LIST_COUNT
        const uint32_t tabs = rr::sim::kModelPoolTables + 16u * p;
        const int32_t base = GetS32(m, tabs + 8);
        const uint32_t slots = m.PeekWord(tabs + 12);
        if (n > 0) {
            const uint32_t e = m.PeekWord(list + 4) + 4u * (r.U32() % static_cast<uint32_t>(n));
            if (Chance(r, 5)) PutS16(m, e, static_cast<int16_t>(base + static_cast<int32_t>(r.U32() % 40u) - 4));
            if (Chance(r, 3)) {                                              // a key: a real one, 0 or drawn
                const uint32_t kt = m.PeekWord(rr::sim::kModelKeyTablePtr);
                PutS16(m, e + 2, static_cast<int16_t>(Chance(r, 3) ? 0 : Chance(r, 2) ? m.PeekByte(kt + 12u * (r.U32() % 34u))
                                                                              : static_cast<int32_t>(r.U32() % 300u)));
            }
        }
        if (Chance(r, 5)) {                                                  // a slot entry: -1, out of range, a real one
            static const int16_t kSlot[6] = {-1, 50, 63, 8, 11, 12};
            PutS16(m, slots + 2u * (r.U32() % 12u), Chance(r, 2) ? kSlot[r.U32() % 6u] : static_cast<int16_t>(r.U32() % 13u));
        }
    }
}

// ---------------------------------------------------------------- SLUS 0x8002FAD4
void SetupModelBind(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const bool real = c.sample != nullptr;
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    m.PokeWord(c.gp + kRandSeedGpOffset, r.U32());
    if (Chance(r, 3)) Put8(m, gs + 4, Chance(r, 3) ? 44u : r.U32() & 0x1Fu);
    if (!real) StirRegistry(c);
    // the object: the traffic car (its part array kept) or a synthetic one with a scratch array
    uint32_t obj = pop::kCar0;
    if (!real && Chance(r, 3) == false) {
        obj = c.scratch + kObjOff;
        RrNoise(c, obj, 0x70);
        m.PokeWord(obj + 4, Chance(r, 8) ? 0u : c.scratch + kPartsOff);      // PART_ARRAY
    }
    if (Chance(r, 3)) m.PokeWord(obj + 36, (m.PeekWord(obj + 36) & 0xFFFC0FFFu) | ((Chance(r, 2) ? 0x3Fu : r.U32() % 64u) << 12));
    if (Chance(r, 4)) m.PokeWord(obj + 36, m.PeekWord(obj + 36) ^ 0x400u);
    // the pool and the class
    static const int32_t kPool[12] = {3, 3, 3, 3, 1, 2, 4, 5, 6, 0, 7, -1};
    const int32_t pool = real ? (Chance(r, 4) ? kPool[r.U32() % 12u] : 3) : kPool[r.U32() % 12u];
    uint32_t cls = 0xFFFFu;
    switch (pool) {
        case 1: case 2: cls = r.U32() % 24u; break;                          // CLASS_RANGE
        case 3: cls = Chance(r, 2) ? 0xFFFFu : r.U32() % 7u; break;
        case 5: case 6: {
            // CLASS_RANGE: a negative LOD indexes BEFORE the LOD table (the signed compare lets it
            // through), so it is drawn only onto a synthetic record that has entries there.
            cls = r.U32() % 44u;
            const int32_t slot = GetS16(m, m.PeekWord(rr::sim::kModelPoolTables + 16u * U(pool) + 12));
            if (!real && slot >= 0 && slot < 50 && Chance(r, 3)) {
                SynthRegistry(c, U(slot), true);
                cls = static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 9u) - 3);
            }
            break;
        }
        default: cls = Chance(r, 2) ? 0xFFFFu : r.U32(); break;
    }
    if (!real && pool == 4 && Chance(r, 2)) {                               // pool 4's keys: found or not
        const uint32_t list = rr::sim::kModelClassLists + 32u;
        const uint32_t kt = m.PeekWord(rr::sim::kModelKeyTablePtr);
        for (int32_t i = 0; i < GetS16(m, list); ++i)
            PutS16(m, m.PeekWord(list + 4) + 4u * U(i) + 2,
                   static_cast<int16_t>(Chance(r, 2) ? m.PeekByte(kt + 12u * (r.U32() % 34u)) : 1u + r.U32() % 300u));
    }
    a.a[0] = obj;
    a.a[1] = U(pool);
    a.a[2] = cls;
    a.a[3] = (obj == pop::kCar0 && Chance(r, 2)) ? 0u : (Chance(r, 3) ? 0u : 1u + r.U32() % 3u);
}
uint32_t NativeModelBind(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    uint32_t v = 0;
    const bool ok = rr::sim::ModelBind(g, a.a[0], static_cast<int32_t>(a.a[1]), a.a[2], a.a[3], v);
    return pop::PopDone(env, g, ok, "ModelBind", v);
}

// ---------------------------------------------------------------- the effect chains
// An effect chain of `len` distinct records on entity `e` (head at +0x49): each record busy (its
// state drawn 1..15, or any state when `anyState`), its bits 14..21 2 or drawn, linked to the next,
// the last link 63 (-1).
void PlantChain(CaseContext& c, uint32_t e, uint32_t len, bool anyState) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    uint32_t pick[20];
    for (uint32_t i = 0; i < 20; ++i) pick[i] = i;
    for (uint32_t i = 0; i < len; ++i) std::swap(pick[i], pick[i + r.U32() % (20u - i)]);
    Put8(m, e + 73, len == 0 ? 0xFFu : pick[0]);
    for (uint32_t i = 0; i < len; ++i) {
        const uint32_t rec = Rec(pick[i]);
        const uint32_t state = anyState ? r.U32() % 16u : 1u + r.U32() % 15u;
        const uint32_t sub = Chance(r, 2) ? 2u : r.U32() & 0xFFu;
        const uint32_t link = (i + 1 < len) ? pick[i + 1] : 63u;
        m.PokeWord(rec, (r.U32() & 0xFFC00C00u) | (sub << 14) | (state << 6) | link);
    }
}
// Some of the effect pool busy (the join's EffectFindFree scans from *(0x800D8068)).
void StirEffects(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t mode = r.U32() % 4u;
    for (uint32_t k = 0; k < 20; ++k) {
        const bool busy = mode == 3 || (mode != 0 && Chance(r, 2));
        const uint32_t w = m.PeekWord(Rec(k));
        m.PokeWord(Rec(k), (w & ~0x3C0u) | (busy ? (1u + r.U32() % 15u) << 6 : 0u));
    }
}

// ---------------------------------------------------------------- SLUS 0x8002847C
void SetupCopLeave(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    uint32_t e = pop::SomeEntity(c);
    if (c.sample == nullptr && Chance(r, 3)) {
        e = c.scratch + kObjOff;
        RrNoise(c, e, 0x70);
    }
    if (Chance(r, 2)) m.PokeWord(e + 36, r.U32());
    if (Chance(r, 8)) {
        Put8(m, e + 73, 0xFFu);                                              // no chain
    } else if (Chance(r, 6)) {                                               // links outside the pool
        Put8(m, e + 73, r.U32() % 20u);
        m.PokeWord(Rec(m.PeekByte(e + 73)), (m.PeekWord(Rec(m.PeekByte(e + 73))) & ~0x3Fu) | (r.U32() & 0x3Fu));
    } else {
        PlantChain(c, e, 1u + r.U32() % 5u, true);
    }
    a.a[0] = e;
}
uint32_t NativeCopLeave(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    rr::sim::CopLeave(g, a.a[0]);
    return pop::PopDone(env, g, true, "CopLeave");
}

// ---------------------------------------------------------------- SLUS 0x80028034
void SetupCopJoin(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    uint32_t e = Chance(r, 3) ? pop::kCar0 : pop::Bike(m, r.U32() % 18u);
    if (c.sample == nullptr && Chance(r, 3)) {                               // a synthetic entity and DOD3
        e = c.scratch + kObjOff;
        RrNoise(c, e, 0x70);
        RrNoise(c, e + 0xB4, 4);
        const uint32_t dod = c.scratch + kRegDodOff;
        RrNoise(c, dod, 0x40);
        m.PokeWord(e + 0, dod);
        WriteU16Bench(m, dod + 14, static_cast<uint16_t>((r.U32() & 0xFF87u) | ((Chance(r, 2) ? 2u : r.U32() % 16u) << 3)));
    }
    static const uint32_t k180[8] = {17u, 18u, 19u, 20u, 21u, 0u, 0xFFFFFFF0u, 0x80000000u};
    m.PokeWord(e + 180, Chance(r, 4) ? r.U32() : k180[r.U32() % 8u]);
    m.PokeWord(e + 36, Chance(r, 3) ? r.U32() : (m.PeekWord(e + 36) & ~0x08000000u));
    StirEffects(c);
    PlantChain(c, e, Chance(r, 3) ? 0u : 1u + r.U32() % 4u, false);           // EFFECT_CHAIN
    if (Chance(r, 3)) PutS32(m, m.PeekWord(kGameStatePtr) + 16, r.S32());
    a.a[0] = e;
}
uint32_t NativeCopJoin(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    rr::sim::CopJoin(g, a.a[0]);
    return pop::PopDone(env, g, true, "CopJoin");
}

// ---------------------------------------------------------------- the seats
// Seat index `idx` at `o + at` with its bit in *(0x8005AD50) set or not, and the seat record's
// SeatRelease match (o[+56] == the record) drawn.
void DrawSeat(CaseContext& c, uint32_t o, uint32_t at) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const int32_t idx = Chance(r, 6) ? -1 : Chance(r, 6) ? static_cast<int8_t>(r.U32()) : static_cast<int32_t>(r.U32() % 8u);
    Put8(m, o + at, U(idx));
    uint32_t bits = Chance(r, 3) ? r.U32() : m.PeekWord(rr::sim::kSeatBitsWord);
    const uint32_t bit = 1u << (U(idx) & 31u);
    bits = Chance(r, 4) ? (bits & ~bit) : (bits | bit);
    m.PokeWord(rr::sim::kSeatBitsWord, bits);
    const uint32_t seat = rr::sim::kSeatRecords + 172u * U(idx);
    if (Chance(r, 2)) m.PokeWord(o + 56, seat);
}
// An animation-object word: kept, cleared or a scratch stand-in (ANIM_OBJECT).
void DrawAnim(CaseContext& c, uint32_t at, bool keep) {
    Rng& r = *c.rng;
    const uint32_t k = r.U32() % 3u;
    if (k == 0 && keep) return;
    c.mem->PokeWord(at, k == 1 ? 0u : c.scratch + kAnimOff);
}

// ---------------------------------------------------------------- RASHCDG 0x80095AEC
void SetupReleaseRiderObj(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    uint32_t rd = pop::Rider(r.U32() % 18u);
    if (c.sample == nullptr && Chance(r, 3)) {
        rd = c.scratch + kPartsOff;
        RrNoise(c, rd, 0x240);
    }
    DrawSeat(c, rd, 571);
    DrawAnim(c, rd + 556, rd != c.scratch + kPartsOff);
    Put8(m, rd + 572, Chance(r, 2) ? (r.U32() | 0x80u) : (r.U32() & 0x7Fu));
    const uint32_t seat = rr::sim::kSeatRecords + 172u * U(static_cast<int8_t>(m.PeekByte(rd + 571)));
    if (Chance(r, 2)) PlantChain(c, seat, r.U32() % 5u, true);
    else if (Chance(r, 2)) Put8(m, seat + 73, 0xFFu);
    a.a[0] = rd;
}
uint32_t NativeReleaseRiderObj(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    return pop::PopDone(env, g, rr::sim::ReleaseRiderObject(g, a.a[0]), "ReleaseRiderObject");
}

// ---------------------------------------------------------------- RASHCDG 0x8008C000
struct PoolShape {
    uint32_t ctrl;
    uint32_t cap;
};
// The record of pool `pool`'s slot `n` whose u16 at +172 (pool 6: +0) the high-index walk reads.
uint32_t PoolRecord(const Memory& m, int32_t pool, uint32_t n) {
    switch (pool) {
        case 2: return m.PeekWord(0x800D4B80) + 572u * n;
        case 3: return pop::kCar0 + 512u * n;
        case 4: return m.PeekWord(0x800CD6D4) + 596u * n;
        case 5: return m.PeekWord(0x800CE5A4) - 452u * n;
        default: return m.PeekWord(0x800CD6C4) + 280u * n;
    }
}

void SetupPoolRelease(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const bool real = c.sample != nullptr;
    static const int32_t kPool[12] = {3, 3, 3, 2, 2, 4, 4, 5, 5, 6, 6, 0};
    int32_t pool = kPool[r.U32() % 12u];
    if (!real && Chance(r, 12)) pool = Chance(r, 2) ? 1 : (Chance(r, 2) ? 7 : -1);
    static const uint32_t kCtrl[7] = {0, 0, 0x800D4B70u, 0x800CF650u, 0x800CD6C8u, 0x800CE598u, 0x800CD6A8u};
    static const uint32_t kCap[7] = {0, 0, 7u, 16u, 8u, 8u, 23u};
    const bool known = pool >= 2 && pool <= 6;
    const uint32_t cap = known ? kCap[pool] : 8u;
    const uint32_t slot = (real || !Chance(r, 8)) ? r.U32() % cap : r.U32() % 32u;
    uint32_t h = c.scratch + kHandleOff;
    if (pool == 3 && (real || Chance(r, 2))) h = pop::kCar0 + 512u * slot + 0xAC;
    WriteU16Bench(m, h, static_cast<uint16_t>(Chance(r, 3) ? ((r.U32() & 0xFFE0u) | slot) : ((U(pool) << 5) | slot)));
    if (known) {
        const uint32_t ctrl = kCtrl[pool];
        // live count, lowest-free index, high index (the walk runs when it is the slot)
        if (Chance(r, 2)) PutS32(m, ctrl, static_cast<int32_t>(r.U32() % (cap + 2u)) - 1);
        if (Chance(r, 2)) PutS32(m, ctrl + 4, static_cast<int32_t>(r.U32() % (cap + 2u)) - 1);
        if (Chance(r, 3)) PutS32(m, ctrl + 8, static_cast<int32_t>(slot));
        else if (Chance(r, 2)) PutS32(m, ctrl + 8, static_cast<int32_t>(r.U32() % (cap + 1u)) - 1);
        for (uint32_t n = 0; n < slot; ++n)
            if (Chance(r, 3)) WriteU16Bench(m, PoolRecord(m, pool, n) + (pool == 6 ? 0u : 172u),
                                            static_cast<uint16_t>(Chance(r, 2) ? 0u : 0x20u * U(pool) + n));
        const uint32_t rec = PoolRecord(m, pool, slot);
        if (pool == 2) {                                                     // the object it frees
            DrawAnim(c, rec + 540, slot < 3u);
            Put8(m, rec + 566, Chance(r, 3) ? 1u : 0u);
            DrawSeat(c, rec, 567);
        } else if (pool == 3) {                                              // a cop car leaving
            m.PokeWord(h + 8, Chance(r, 3) ? 1u : 0u);
            const uint32_t car = pop::kCar0 + 512u * slot;
            if (Chance(r, 2)) m.PokeWord(car + 36, m.PeekWord(car + 36) | 0x08000000u);
            if (Chance(r, 2)) PlantChain(c, car, r.U32() % 5u, true);
        } else if (pool == 4 || pool == 5) {                                 // its store record (ANIM_OBJECT)
            m.PokeWord(rec + 4, rr::sim::kPoolStoreBase + 24u * (r.U32() % 18u));
            if (Chance(r, 2)) PutS32(m, rr::sim::kPoolLowFree, static_cast<int32_t>(r.U32() % 20u) - 1);
        }
    }
    if (Chance(r, 3)) m.PokeWord(rr::sim::kPoolFreeScan, r.U32());
    a.a[0] = h;
    a.a[1] = U(pool);
}
uint32_t NativePoolRelease(NativeEnv& env, const Args& a) {
    rr::sim::GuestRam g = pop::View(env);
    return pop::PopDone(env, g, rr::sim::PoolRelease(g, a.a[0], static_cast<int32_t>(a.a[1])), "PoolRelease");
}

Row BindRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
            std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0) {
    Row r{name, sig, entry, std::move(setup), nullptr, v0, std::move(native)};
    r.maxSteps = 5'000'000;
    return r;
}
} // namespace tbind

void AddTrafficBindRows(std::vector<Row>& rows) {
    rows.push_back(tbind::BindRow("model_bind", "u32 ModelBind(Obj *o, s32 pool, u32 cls, u32 alloc)  [SLUS 0x8002FAD4]",
                                  tbind::kModelBindFn, tbind::SetupModelBind, tbind::NativeModelBind, true));
    rows.push_back(tbind::BindRow("pool_release", "void PoolRelease(u16 *h, s32 pool)  [RASHCDG 0x8008C000]",
                                  tbind::kPoolReleaseFn, tbind::SetupPoolRelease, tbind::NativePoolRelease, false));
    rows.push_back(tbind::BindRow("cop_leave", "void CopLeave(Entity *e)  [SLUS 0x8002847C]", tbind::kCopLeaveFn,
                                  tbind::SetupCopLeave, tbind::NativeCopLeave, false));
    rows.push_back(tbind::BindRow("cop_join", "void CopJoin(Entity *e)  [SLUS 0x80028034]", tbind::kCopJoinFn,
                                  tbind::SetupCopJoin, tbind::NativeCopJoin, false));
    rows.push_back(tbind::BindRow("release_rider_obj", "void ReleaseRiderObject(Rider *r)  [RASHCDG 0x80095AEC]",
                                  tbind::kReleaseRiderObjFn, tbind::SetupReleaseRiderObj, tbind::NativeReleaseRiderObj, false));
}
