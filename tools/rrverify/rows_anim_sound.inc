// Bench rows of the hit sounds: the animation's sound events,
// AnimSounds SLUS 0x80018E54 and AnimSoundFire SLUS 0x80017DA0 (src\game\sim\sound_frame.h), run natively with
// every callee PORTED (GetRCnt 0x80043F00, PlaySound3D 0x80017BA0 and the emitter under it). Included at the end
// of verify_physics.cpp inside its anonymous namespace, after rows_engine_note.inc (EnRun, EnPlantSystem,
// EnPlantSpu, EnPut*).
//
// INPUT FAMILIES start from the snapshot's animation descriptor 0x800CE170 (rr-race: 23 objects, 21 used, the
// ANIMNOIZ.DAT event table *(0x8005B3E4) resident) and plant on top of it: per object the playing bit (+0x24 bit
// 1), the struck bit (bit 7), the frame +0x10 and the list +0x6DC - none, a list of the file's own table
// *(0x8005B3E4)[stance], or a synthetic list of 1..5 records in the scratch block (0x1C00..0x2000) with keys
// rising from 0 and words drawn over every gate bit (24, 25, 26, 29, 30) and kind byte (0x14, 0x09, 0x40, the
// rest) the fire function branches on; the owner rider's +0x25C (0..4), his bike's riderDef +0x2E / +0x01, the
// player count gs+0x30 (1, 2), the descriptor's +8 (0 now and then) and count +0x0C (<= the capture's), root
// counter 2 (EnPlantSpu). The random family also redraws the sound system (EnPlantSystem) as the sound rows do.
// NAMED BOUNDS (each keeps the family inside what the ORIGINAL survives): an object is made playing only when it
// has an owner (every capture's objects past the used count have none: a playing object without one reads page
// zero); a list ends (the original walks a cyclic list forever - the port fails the call after 65536 records);
// the count stays at most the capture's (the array holds that many objects).
namespace asnd {

constexpr uint32_t kAnimSoundsFn = 0x80018E54, kAnimSoundFireFn = 0x80017DA0;
constexpr uint32_t kDesc = 0x800CE170, kEventTable = 0x8005B3E4, kObjBytes = 2108;
constexpr uint32_t kRecs = 0x1C00, kRecsEnd = 0x2000;

bool Chance(Rng& r, uint32_t n) { return r.Next() % n == 0; }

// The word of a synthetic event record: gate bits and a kind byte over a sound id.
uint32_t DrawWord(Rng& r) {
    static const uint32_t kTop[] = {0x01, 0x02, 0x04, 0x20, 0x40, 0x14, 0x09, 0x41, 0x0D, 0x24, 0x60, 0x03, 0x00, 0x15};
    uint32_t top = kTop[r.Next() % (sizeof(kTop) / sizeof(kTop[0]))];
    if (Chance(r, 8)) top = r.U32() & 0xFFu;
    const uint32_t mid = r.U32() & 0xFFu;                 // the kind-0x09 spread
    const uint32_t id = Chance(r, 6) ? (r.U32() & 0xFFFFu) : r.U32() % 110u;
    return (top << 24) | (mid << 16) | id;
}

// A list of the file's own table, or 0 when the table is not resident.
uint32_t FileList(CaseContext& c) {
    Memory& m = *c.mem;
    const uint32_t tab = m.PeekWord(kEventTable);
    if (tab < 0x80000000u || tab >= 0x80200000u) return 0;
    uint32_t n = 0;
    while (n < 256) {
        const uint32_t p = m.PeekWord(tab + 4u * n);
        if (p != 0 && (p < 0x80000000u || p >= 0x80200000u)) break;
        ++n;
    }
    for (int tries = 0; tries < 8 && n != 0; ++tries) {
        const uint32_t p = m.PeekWord(tab + 4u * (c.rng->U32() % n));
        if (p != 0) return p;
    }
    return 0;
}

// A synthetic list at scratch + `at`: 1..5 records, keys rising from 0.
uint32_t PlantList(CaseContext& c, uint32_t& at) {
    Rng& r = *c.rng;
    const uint32_t n = 1u + r.U32() % 5u;
    if (at + 12u * n > kRecsEnd) return 0;
    const uint32_t head = c.scratch + at;
    uint32_t key = Chance(r, 3) ? 0u : r.U32() % 6u;
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t rec = c.scratch + at;
        EnPut(c, rec + 0, Chance(r, 16) ? r.U32() : key);
        EnPut(c, rec + 4, DrawWord(r));
        EnPut(c, rec + 8, k + 1 < n ? rec + 12u : 0u);
        at += 12u;
        key += r.U32() % 8u;
    }
    return head;
}

void Setup(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    if (!c.sample) EnPlantSystem(c);
    EnPlantSpu(c);
    const uint32_t objs = m.PeekWord(kDesc);
    const uint32_t count = m.PeekWord(kDesc + 12u);
    const uint32_t gs = m.PeekWord(kGameStatePtr);
    EnPut(c, gs + 0x30u, Chance(r, 4) ? 2u : 1u);
    uint32_t at = kRecs;
    const uint32_t n = count > 23u ? 23u : count;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t o = objs + kObjBytes * i;
        const uint32_t owner = m.PeekWord(o);
        if (owner == 0 || !Chance(r, 2)) continue;
        uint32_t fl = m.PeekWord(o + 36u);
        fl = Chance(r, 5) ? (fl & ~2u) : (fl | 2u);
        fl = Chance(r, 2) ? (fl | 0x80u) : (fl & ~0x80u);
        EnPut(c, o + 36u, fl);
        EnPut(c, o + 16u, Chance(r, 8) ? r.U32() : r.U32() % 40u);
        const uint32_t pick = r.U32() % 6u;
        uint32_t list = 0;
        if (pick == 0) list = 0;
        else if (pick == 1) list = FileList(c);
        else list = PlantList(c, at);
        EnPut(c, o + 0x6DCu, list);
        // the owner rider: on the bike or off it; his bike's weapon and riderDef +0x01
        if ((m.PeekWord(owner + 0xACu) & 0xFFFFu) >> 5 == 1u) {
            if (Chance(r, 3)) EnPut(c, owner + 0x25Cu, r.U32() % 5u);
            const uint32_t bike = m.PeekWord(owner + 0x254u);
            if (bike >= 0x80000000u && bike < 0x80200000u) {
                const uint32_t def = m.PeekWord(bike + 0x43Cu);
                if (def >= 0x80000000u && def < 0x80200000u) {
                    if (Chance(r, 2)) EnPut8(c, def + 46u, static_cast<uint8_t>(r.U32() % 16u));
                    if (Chance(r, 2)) EnPut8(c, def + 1u, static_cast<uint8_t>(r.U32()));
                }
            }
        }
    }
    if (Chance(r, 12)) EnPut(c, kDesc + 8u, 0);
    if (Chance(r, 8)) EnPut(c, kDesc + 12u, Chance(r, 3) ? 0u : r.U32() % (n + 1u));
    a.a[0] = kDesc;
}

// AnimSoundFire alone: a synthetic record, a drawn flags byte, a drawn position near the listener.
void SetupFire(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    if (!c.sample) EnPlantSystem(c);
    EnPlantSpu(c);
    uint32_t at = kRecs;
    uint32_t rec = PlantList(c, at);
    if (rec == 0) rec = c.scratch + kRecs;
    const uint32_t bike0 = m.PeekWord(kPlayerArr);
    a.a[0] = m.PeekWord(bike0 + 0xB8u) + static_cast<uint32_t>(r.World());
    a.a[1] = m.PeekWord(bike0 + 0xC0u) + static_cast<uint32_t>(r.World());
    a.a[2] = rec;
    a.a[3] = r.U32() & 0xFFu;
}

} // namespace asnd

void AddAnimSoundRows(std::vector<Row>& rows) {
    {
        Row row{"anim_sounds", "void AnimSounds(AnimDesc *d)  [SLUS 0x80018E54, GameFrame step 5: the ANIMNOIZ events]",
                asnd::kAnimSoundsFn, asnd::Setup, nullptr, /*compareV0=*/false};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            rr::sim::AnimSounds(run.s, a.a[0]);
            return run.Done(env, 0);
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }
    {
        Row row{"anim_sound_fire", "Record *AnimSoundFire(s32 x, s32 z, Record *rec, u32 flags)  [SLUS 0x80017DA0]",
                asnd::kAnimSoundFireFn, asnd::SetupFire, nullptr, /*compareV0=*/true};
        row.nativeEx = [](NativeEnv& env, const Args& a) {
            EnRun run(env);
            const uint32_t v = rr::sim::AnimSoundFire(run.s, static_cast<int32_t>(a.a[0]), static_cast<int32_t>(a.a[1]),
                                                      a.a[2], a.a[3]);
            return run.Done(env, v);
        };
        row.compareSpuControl = true;
        rows.push_back(row);
    }
}
