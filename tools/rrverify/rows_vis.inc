// Bench rows of the cell visibility: which scene cells a view draws (src\game\sim\cell_draw.h,
// docs\formats\scene_cell.md 14). Included by verify_physics.cpp inside its anonymous namespace.
//
// The machine is a race capture (rr-race: one player, the release list STREAM1.RLS resident at
// gp+0x88C, ten cells in view 0's slots, the cursor gp+0x87C on road 9's record 974..1096).
// Named bounds (each a state the game builds or keeps):
//   * the view is 0 in the draw list (the capture has one player; view 1's bike pointer is 0 and the
//     ORIGINAL faults on it);
//   * the release list is resident whenever the draw list reaches its scan (a gp+0x88C of 0 is drawn
//     only with a cursor record that still holds the player: the scan would walk from address 0);
//   * a road number is one the release list has (0..36): the table has 37 roads.
namespace visrows {

namespace s = rr::sim;

bool Chance(Rng& r, uint32_t n) { return r.U32() % n == 0u; }
uint32_t Slot(uint32_t i) { return s::kCellSlots + 112u * i; }
void PutU16(Memory& m, uint32_t a, uint16_t v) { m.WriteBlock(a, &v, 2); }
uint16_t GetU16(Memory& m, uint32_t a) {
    uint16_t v = 0;
    m.ReadBlock(a, &v, 2);
    return v;
}

// The texture-key words +0x58 / +0x5C and the page words +0x60 / +0x64 of slot `i`: as captured, a key
// gone (-1), a page unresolved (0) or resolved (a record address).
void StirTextures(Rng& r, Memory& m, uint32_t i) {
    const uint32_t sl = Slot(i);
    for (uint32_t k = 0; k < 2; ++k) {
        switch (r.U32() % 5u) {
        case 0: m.PokeWord(sl + 88u + 4u * k, 0xFFFFFFFFu); break;
        case 1: m.PokeWord(sl + 96u + 4u * k, 0u); break;
        case 2: m.PokeWord(sl + 96u + 4u * k, 0x800D92F8u); break;
        case 3: m.PokeWord(sl + 88u + 4u * k, 0x8C00u | (r.U32() & 0x3FFu)); break;
        default: break;
        }
    }
}

// Slot `i` of view 0 changed the ways the streamer leaves a slot: emptied (+0x00 = +0x08 = -1, no
// body), a body gone (+0x04 = 0), the second id gone, or the resource id another cell's.
void StirSlot(Rng& r, Memory& m, uint32_t i, uint32_t otherId) {
    const uint32_t sl = Slot(i);
    switch (r.U32() % 6u) {
    case 0: m.PokeWord(sl, 0xFFFFFFFFu); m.PokeWord(sl + 8u, 0xFFFFFFFFu); m.PokeWord(sl + 4u, 0u); break;
    case 1: m.PokeWord(sl + 4u, 0u); break;
    case 2: m.PokeWord(sl + 8u, 0xFFFFFFFFu); break;
    case 3: m.PokeWord(sl, otherId); m.PokeWord(sl + 8u, otherId); break;
    default: break;
    }
    if (Chance(r, 2)) StirTextures(r, m, i);
}

// A cell id a release-list record names: road `road`'s record `k`'s key `j` unpacked as the draw list
// unpacks it.
uint32_t RlsId(Memory& m, uint32_t gp, uint32_t road, uint32_t k, uint32_t j) {
    const uint32_t rec = m.PeekWord(m.PeekWord(gp + s::kGpRlsTable) + 4u * road) + 32u * k;
    const uint32_t key = GetU16(m, rec + 18u + 2u * j);
    if (key == 0xFFFFu) return 0xFFFFFFFFu;
    return ((key & 0x7C00u) << 13) | (key & 0x3FFu);
}

uint32_t RoadRecords(Memory& m, uint32_t gp, uint32_t road) {
    const uint32_t first = m.PeekWord(m.PeekWord(gp + s::kGpRlsTable) + 4u * road);
    uint32_t n = 0;
    while (n < 160u && static_cast<int16_t>(GetU16(m, first + 32u * n + 12u)) == static_cast<int16_t>(road)) ++n;
    return n;
}

// ---------------------------------------------------------------- CellReady 0x800363F0
void SetupReady(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    const uint32_t i = r.U32() % 12u;
    StirTextures(r, m, i);
    if (Chance(r, 3)) StirTextures(r, m, i);
    a.a[0] = Slot(i);
}

// ---------------------------------------------------------------- CellSlotOf 0x800329BC
void SetupSlotOf(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    const uint32_t p = Chance(r, 4) ? 1u : 0u;
    uint32_t id = m.PeekWord(Slot(12u * p + r.U32() % 12u));
    if (Chance(r, 4)) id = 0x01800000u | (r.U32() & 0x3FFu);
    if (Chance(r, 8)) id = 0xFFFFFFFFu;
    a.a[0] = id;
    a.a[1] = p;
}

// ---------------------------------------------------------------- CellResidentList 0x80035E60
void SetupResident(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    const uint32_t p = Chance(r, 4) ? 1u : 0u;
    const uint32_t n = r.U32() % 5u;
    for (uint32_t k = 0; k < n; ++k) StirSlot(r, m, r.U32() % 12u, 0x01800000u | (r.U32() & 0x3FFu));
    for (uint32_t k = 0; k < 12; ++k) m.PokeWord(c.scratch + 4u * k, r.U32());
    a.a[0] = c.scratch;
    a.a[1] = p;
}

// ---------------------------------------------------------------- CellDrawList 0x80035F48
// The player (the bike, or the rider off the bike: +0x25C 3 / 4, the capture's rider moved beside it)
// put on a road of the release list at a distance inside a record, at its edges, between records or
// beyond the road's last; the direction +0x16C -1 / 0 / 1; the road word's high half now and then
// non-zero (off the road graph). The cursor gp+0x87C: 0, as captured, the record before / after the
// one that holds the player, or another road's. The slots stirred; now and then the ids a record names
// planted into free or taken slots. The list, the missing list and the counts start from noise.
void SetupDrawList(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    const uint32_t gp = c.gp;
    const uint32_t bike = m.PeekWord(s::kCellViewBikes);
    const uint32_t rider = m.PeekWord(bike + 852u);
    const bool keepCursor = Chance(r, 6);
    uint32_t e = bike;
    if (!keepCursor && Chance(r, 3)) {
        m.PokeWord(rider + 604u, 3u + (r.U32() & 1u));
        e = rider;
    } else if (Chance(r, 4)) {
        m.PokeWord(rider + 604u, Chance(r, 2) ? 2u : 5u);
    }
    uint32_t road = m.PeekWord(bike + 360u) & 0xFFFFu;
    if (!keepCursor) {
        if (Chance(r, 2)) road = r.U32() % 37u;
        const uint32_t nrec = RoadRecords(m, gp, road);
        const uint32_t first = m.PeekWord(m.PeekWord(gp + s::kGpRlsTable) + 4u * road);
        int32_t along = static_cast<int16_t>(r.U32() >> 17);
        if (nrec > 0) {
            const uint32_t k = r.U32() % nrec;
            const int32_t from = static_cast<int16_t>(GetU16(m, first + 32u * k + 14u));
            const int32_t to = static_cast<int16_t>(GetU16(m, first + 32u * k + 16u));
            switch (r.U32() % 5u) {
            case 0: along = from; break;
            case 1: along = to; break;
            case 2: along = to + 1 + static_cast<int32_t>(r.U32() % 3u); break;
            case 3: along = from - 1 - static_cast<int32_t>(r.U32() % 3u); break;
            default: along = from + (to > from ? static_cast<int32_t>(r.U32() % static_cast<uint32_t>(to - from + 1)) : 0);
            }
            const uint32_t cur = m.PeekWord(gp + s::kGpRlsCursor);
            switch (r.U32() % 5u) {
            case 0: m.PokeWord(gp + s::kGpRlsCursor, 0u); break;
            case 1: m.PokeWord(gp + s::kGpRlsCursor, first + 32u * k); break;
            case 2: if (k > 0) m.PokeWord(gp + s::kGpRlsCursor, first + 32u * (k - 1)); break;
            case 3: if (k + 1 < nrec) m.PokeWord(gp + s::kGpRlsCursor, first + 32u * (k + 1)); break;
            default: m.PokeWord(gp + s::kGpRlsCursor, cur); break;
            }
            if (Chance(r, 2)) // the cells this record names, planted into view 0's slots
                for (uint32_t j = 0; j < 6; ++j) {
                    const uint32_t id = RlsId(m, gp, road, k, j);
                    if (id == 0xFFFFFFFFu) break;
                    if (Chance(r, 2)) {
                        const uint32_t sl = Slot(r.U32() % 12u);
                        m.PokeWord(sl, id);
                        m.PokeWord(sl + 8u, id);
                        if (m.PeekWord(sl + 4u) == 0u) m.PokeWord(sl + 4u, m.PeekWord(Slot(0) + 4u));
                    }
                }
        }
        PutU16(m, e + 370u, static_cast<uint16_t>(along));
        uint32_t word = road;
        if (Chance(r, 8)) word |= (1u + (r.U32() & 3u)) << 16;
        m.PokeWord(e + 360u, word);
        m.PokeWord(e + 364u, Chance(r, 3) ? 0u : (Chance(r, 2) ? 1u : 0xFFFFFFFFu));
    } else if (Chance(r, 2)) {
        m.PokeWord(gp + s::kGpRlsTable, 0u); // no release list: the resident list, then the cursor kept
    }
    const uint32_t n = r.U32() % 4u;
    for (uint32_t k = 0; k < n; ++k) StirSlot(r, m, r.U32() % 12u, 0x01800000u | (r.U32() & 0x3FFu));
    for (uint32_t k = 0; k < 12; ++k) m.PokeWord(s::kCellDrawLists + 4u * k, r.U32() % 13u);
    for (uint32_t k = 0; k < 6; ++k) m.PokeWord(s::kCellMissing + 4u * k, r.U32());
    for (uint32_t o : {s::kGpDrawCount, s::kGpDrawTotal, s::kGpMissingCount, s::kGpDrawCount2})
        m.PokeWord(gp + o, r.U32() % 7u);
    a.a[0] = 0;
}

// ---------------------------------------------------------------- ViewPass 0x8008CFDC
// The capture's bikes and riders stirred: a bike live or not (+0x140, its near bits), moved to within
// 0..40 world units of view 0's eye (the 0x300 / 0x1000 thresholds and the LOD steps of both tables),
// its progress +0x144 within or beyond 0x2000 of the view's, +0x352 anything; its rider live or not, on
// the bike or off it (+0x25C 0..5), moved beside the eye too; every LOD byte +0x0A / +0x0B 0..3; view
// 0 frozen (+0x224 bit 0) now and then; the two counters anything. Now and then player 1 carries a
// passenger (the rider's +0x23C bit 4, +0x358 another bike), and the three records *(0x8005B304) are
// switched on (*(0x8005B314)). Named bound: one player (the capture's; view 1 is not a camera there).
void SetupViewPass(CaseContext& c, Args&) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    const uint32_t views = 0x800CD898u;
    int32_t eye[3];
    for (uint32_t k = 0; k < 3; ++k) eye[k] = static_cast<int32_t>(m.PeekWord(views + 184u + 4u * k));
    const uint32_t pool0 = m.PeekWord(0x800CE4D0u);
    const uint32_t stride = m.PeekWord(0x800CE4D4u);
    const int32_t last = static_cast<int32_t>(m.PeekWord(m.PeekWord(0x800CE4DCu)));
    auto place = [&](uint32_t e) {
        const int32_t reach = static_cast<int32_t>(r.U32() % (Chance(r, 3) ? 8u : 40u)) + 1;
        for (uint32_t k = 0; k < 3; ++k) {
            const int32_t off = static_cast<int32_t>(r.U32() % static_cast<uint32_t>(2 * reach * 65536)) - reach * 65536;
            m.PokeWord(e + 184u + 4u * k, static_cast<uint32_t>(eye[k] + (k == 1 ? off / 8 : off)));
        }
    };
    for (int32_t i = 0; i <= last && i < 18; ++i) {
        const uint32_t e = pool0 + stride * static_cast<uint32_t>(i);
        const uint32_t rider = m.PeekWord(e + 852u);
        if (Chance(r, 2)) place(e);
        if (Chance(r, 4)) PutU16(m, e + 320u, static_cast<uint16_t>(Chance(r, 3) ? 0u : (1u | (r.U32() & 0x30u))));
        if (Chance(r, 3)) m.PokeWord(e + 324u, m.PeekWord(views + 324u) + (r.U32() % 0x4000u) - 0x2000u);
        if (Chance(r, 3)) { const uint8_t b = static_cast<uint8_t>(r.U32()); m.WriteBlock(e + 850u, &b, 1); }
        for (uint32_t k = 0; k < 2; ++k) { const uint8_t l = static_cast<uint8_t>(r.U32() % 4u); m.WriteBlock(e + 10u + k, &l, 1); m.WriteBlock(rider + 10u + k, &l, 1); }
        if (Chance(r, 3)) m.PokeWord(rider + 604u, r.U32() % 6u);
        if (Chance(r, 5)) PutU16(m, rider + 320u, static_cast<uint16_t>(Chance(r, 2) ? 0u : 1u));
        if (Chance(r, 3)) place(rider);
    }
    // gteproj2: the other pools' entities (records 0x800CE4F0.. : 2 pedestrians, 3 cars, 4 / 5 props, 6 volumes) live or not
    // (+0xAC, +0x140), moved beside the eye, their LOD bytes anything - which pools the pass walks, and which take a LOD
    for (uint32_t pool = 2; pool <= 6; ++pool) {
        const uint32_t rec = 0x800CE4D0u + 16u * pool;
        const uint32_t base = m.PeekWord(rec), pstride = m.PeekWord(rec + 4u), lastp = m.PeekWord(rec + 12u);
        if (base < 0x80000000u || base >= 0x80200000u || lastp < 0x80000000u || lastp >= 0x80200000u || pstride == 0) continue;
        const int32_t hi = static_cast<int32_t>(m.PeekWord(lastp));
        for (int32_t i = 0; i <= hi && i < 24; ++i) {
            const uint32_t e = base + pstride * static_cast<uint32_t>(i);
            if (Chance(r, 2)) place(e);
            if (Chance(r, 3)) PutU16(m, e + 172u, static_cast<uint16_t>(Chance(r, 3) ? 0u : 1u + r.U32() % 0xFFu));
            if (Chance(r, 3)) PutU16(m, e + 320u, static_cast<uint16_t>(Chance(r, 3) ? 0u : 1u));
            for (uint32_t k = 0; k < 2; ++k) { const uint8_t l = static_cast<uint8_t>(r.U32() % 4u); m.WriteBlock(e + 10u + k, &l, 1); }
        }
    }
    if (Chance(r, 6)) m.PokeWord(views + 548u, m.PeekWord(views + 548u) ^ 1u);
    PutU16(m, 0x800CD540u, static_cast<uint16_t>(r.U32()));
    PutU16(m, 0x800CD542u, static_cast<uint16_t>(r.U32()));
    if (Chance(r, 4)) { // a passenger on player 1's bike
        const uint32_t b = m.PeekWord(0x8005B38Cu);
        const uint32_t other = pool0 + stride * (1u + r.U32() % 5u);
        const uint32_t rider = m.PeekWord(b + 852u);
        const uint8_t f = static_cast<uint8_t>(m.PeekByte(rider + 572u) | 0x10u);
        m.WriteBlock(rider + 572u, &f, 1);
        m.PokeWord(b + 856u, other);
    }
    if (Chance(r, 4)) m.PokeWord(0x8005B314u, 1u);
}

// ---------------------------------------------------------------- the resource list and the cell of a position
// A loaded cell's extent of the capture's resource list *(0x8005ACBC): {road, from, to} of record `i`'s
// extent `k`, or false when record i holds no cell / no such extent.
bool Extent(Memory& m, uint32_t i, uint32_t k, uint32_t out[3]) {
    const uint32_t base = m.PeekWord(s::kResourceListPtr);
    const uint32_t rec = base + 44u + 36u * i;
    const uint32_t key = m.PeekWord(m.PeekWord(rec + 20u));
    if ((key >> 28) != 0 && (key >> 28) != 8) return false;
    const uint32_t ext = m.PeekWord(rec + 24u);
    if (ext < 0x80000000u) return false;
    for (uint32_t j = 0; j <= k; ++j)
        if (m.PeekWord(ext + 12u * j) == 0xFFFFFFFFu) return false;
    for (uint32_t j = 0; j < 3; ++j) out[j] = m.PeekWord(ext + 12u * k + 4u * j);
    return true;
}
// A road and a distance: a loaded cell's extent at, just inside or just past its ends (the extents are
// in 1/64 world units, as +0x170 >> 10 is), or a random road of the 37.
void PickRoadDistance(Rng& r, Memory& m, uint32_t& road, int32_t& d) {
    const uint32_t n = m.PeekWord(m.PeekWord(s::kResourceListPtr) + 0xA58u);
    uint32_t e[3];
    for (int tries = 0; tries < 8 && n > 0; ++tries)
        if (Extent(m, r.U32() % n, r.U32() % 3u, e)) {
            road = e[0];
            const int32_t from = static_cast<int32_t>(e[1]), to = static_cast<int32_t>(e[2]);
            switch (r.U32() % 5u) {
            case 0: d = from; break;
            case 1: d = to; break;
            case 2: d = to - 1; break;
            case 3: d = from - 1; break;
            default: d = from + (to > from ? static_cast<int32_t>(r.U32() % static_cast<uint32_t>(to - from)) : 0);
            }
            return;
        }
    road = r.U32() % 37u;
    d = static_cast<int32_t>(r.U32() % 12000u);
}
// The list stirred: a record's flags losing 0x10 or 0x02, the count cut.
void StirResources(Rng& r, Memory& m) {
    const uint32_t base = m.PeekWord(s::kResourceListPtr);
    const uint32_t n = m.PeekWord(base + 0xA58u);
    if (n == 0) return;
    if (Chance(r, 3)) {
        const uint32_t f = base + 44u + 36u * (r.U32() % n);
        m.PokeWord(f, m.PeekWord(f) & (Chance(r, 2) ? ~0x10u : ~0x02u));
    }
    if (Chance(r, 6)) m.PokeWord(base + 0xA58u, r.U32() % (n + 1u));
}
void SetupCellAt(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    uint32_t road = 0;
    int32_t d = 0;
    PickRoadDistance(r, *c.mem, road, d);
    StirResources(r, *c.mem);
    a.a[0] = road;
    a.a[1] = static_cast<uint32_t>(d);
}
// An entity of the capture (a bike, a rider, the car, view 0) put on a road at a distance a loaded cell
// covers (+0x170 = d << 10 plus noise under 1/64), or in a junction (the road word's high half set, the
// low half a junction 0..24 of the graph's 25); view 0 now and then on the same road, its distance
// anywhere; the radius 0 .. 2047 or any halfword.
uint32_t PickEntity(Rng& r, Memory& m) {
    switch (r.U32() % 5u) {
    case 0: return m.PeekWord(0x800CE4D0u) + 1096u * (r.U32() % 18u);
    case 1: return m.PeekWord(0x8005B3A4u) + 628u * (r.U32() % 18u);
    case 2: return 0x800CF660u;
    case 3: return 0x800CD898u;
    default: return m.PeekWord(0x800CE4D0u);
    }
}
void PlaceEntity(Rng& r, Memory& m, uint32_t e) {
    uint32_t road = 0;
    int32_t d = 0;
    PickRoadDistance(r, m, road, d);
    if (Chance(r, 6)) {
        m.PokeWord(e + 360u, (1u + (r.U32() & 0xFFu)) << 16 | (r.U32() % 25u));
    } else {
        m.PokeWord(e + 360u, road);
        m.PokeWord(e + 368u, (static_cast<uint32_t>(d) << 10) | (r.U32() & 0x3FFu));
    }
    if (Chance(r, 3)) {
        m.PokeWord(0x800CD898u + 360u, (m.PeekWord(0x800CD898u + 360u) & 0xFFFF0000u) | (road & 0xFFFFu));
        m.PokeWord(0x800CD898u + 368u, static_cast<uint32_t>(d + static_cast<int32_t>(r.U32() % 200u) - 100) << 10);
    }
}
void SetupPositionCell(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    const uint32_t e = PickEntity(r, m);
    PlaceEntity(r, m, e);
    StirResources(r, m);
    a.a[0] = e + 172u;
    a.a[1] = Chance(r, 4) ? r.U32() : r.U32() % 2048u;
}
void SetupEntityCell(CaseContext& c, Args& a) {
    Rng& r = *c.rng;
    Memory& m = *c.mem;
    const uint32_t e = PickEntity(r, m);
    PlaceEntity(r, m, e);
    StirResources(r, m);
    if (Chance(r, 3)) m.PokeWord(e + 40u, r.U32() % 0x40000u);
    a.a[0] = e;
}

template <class F>
uint32_t RunVis(NativeEnv& env, F&& f) {
    s::GuestRam g = RqView(env);
    const uint32_t v = f(g);
    return RqDone(env, g, v);
}

Row VisRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
           std::function<uint32_t(NativeEnv&, const Args&)> native) {
    return Row{name, sig, entry, std::move(setup), nullptr, true, std::move(native), {}};
}

} // namespace visrows

void AddVisRows(std::vector<Row>& rows) {
    using namespace visrows;
    const s::CellReadyFn ready = [](s::GuestRam& g, uint32_t sl) { return s::CellReady(g, sl); };
    rows.push_back(VisRow("cell_ready", "bool 0x800363F0(Slot *s)  [SLUS, a cell's texture pages resolved]",
                          s::kCellReadyFn, SetupReady, [](NativeEnv& env, const Args& a) {
                              return RunVis(env, [&](s::GuestRam& g) { return s::CellReady(g, a.a[0]) ? 1u : 0u; });
                          }));
    rows.push_back(VisRow("cell_slot_of", "s32 0x800329BC(u32 id, s32 view)  [SLUS, the slot holding a cell]",
                          s::kCellSlotOfFn, SetupSlotOf, [](NativeEnv& env, const Args& a) {
                              return RunVis(env, [&](s::GuestRam& g) {
                                  return static_cast<uint32_t>(s::CellSlotOf(g, a.a[0], a.a[1]));
                              });
                          }));
    rows.push_back(VisRow("cell_resident_list", "s32 0x80035E60(s32 *list, s32 view)  [SLUS, the resident cells]",
                          s::kCellResidentListFn, SetupResident, [ready](NativeEnv& env, const Args& a) {
                              return RunVis(env, [&](s::GuestRam& g) {
                                  return static_cast<uint32_t>(s::CellResidentList(g, a.a[0], a.a[1], ready));
                              });
                          }));
    rows.push_back(VisRow("cell_draw_list", "s32 0x80035F48(s32 view)  [SLUS, the cells a view draws]",
                          s::kCellDrawListFn, SetupDrawList, [ready](NativeEnv& env, const Args& a) {
                              return RunVis(env, [&](s::GuestRam& g) {
                                  return static_cast<uint32_t>(s::CellDrawList(g, a.a[0], ready));
                              });
                          }));
    rows.push_back(VisRow("view_pass", "void 0x8008CFDC()  [RASHCDG, GameFrame step 4: view distances, near bits, LODs]",
                          s::kViewPassFn, SetupViewPass, [](NativeEnv& env, const Args&) {
                              return RunVis(env, [&](s::GuestRam& g) { return s::ViewPass(g) ? 0u : 0u; });
                          }));
    rows.back().compareV0 = false;
    rows.push_back(VisRow("cell_at", "s32 0x80030500(u32 road, s32 d)  [SLUS, the loaded cell holding a road position]",
                          s::kCellAtFn, SetupCellAt, [](NativeEnv& env, const Args& a) {
                              return RunVis(env, [&](s::GuestRam& g) {
                                  return static_cast<uint32_t>(s::CellAt(g, a.a[0], static_cast<int32_t>(a.a[1])));
                              });
                          }));
    rows.push_back(VisRow("position_cell", "s32 0x80030410(u16 *h, s16 r)  [SLUS, the cell an entity stands in]",
                          s::kPositionCellFn, SetupPositionCell, [](NativeEnv& env, const Args& a) {
                              return RunVis(env, [&](s::GuestRam& g) {
                                  return static_cast<uint32_t>(s::PositionCell(g, a.a[0], static_cast<int32_t>(a.a[1])));
                              });
                          }));
    rows.push_back(VisRow("entity_cell", "void 0x8008B99C(E *e)  [RASHCDG, the draw loop's cell of an entity, +0xB0]",
                          s::kEntityCellFn, SetupEntityCell, [](NativeEnv& env, const Args& a) {
                              return RunVis(env, [&](s::GuestRam& g) {
                                  return static_cast<uint32_t>(s::EntityCell(g, a.a[0]));
                              });
                          }));
}
