// Bench rows of the scene cells: the frame's cell sort, SLUS 0x800358C0's three children
// and the ordering-table set-up (src\game\sim\cell_sort.h). Included by verify_physics.cpp inside its anonymous
// namespace, after every other rows file (it reuses spinerows::Chance and fxrows::Around).
//
// The machine is the rr-race capture: one player, view 0's slots 0..9 resident (bodies, region-3 polygons, vertex
// arrays), the render camera *(0x8005AEC0), the list 0x800D9B80 as the original left it. Inputs, each a state the
// game builds:
//   * a cell context's rotation is the render camera's rows (the view's, 0x800353C4 copies them) or another rotation
//     about the vertical; its translation the body's origin less the eye turned by them, the eye moved around the
//     cell (in front, beside, behind, far) - the depth range's every arm and the four side codes;
//   * the list holds the resident slots in any order, -1 holes, the camera's slot anywhere or absent; the view's cell
//     +0xB0 one of the listed ids, an unlisted one, or none;
//   * the sort's keys +0x2C / +0x3C: close (within 0x280) and far pairs; +0x3C follows +0x2C (the mean depth of a
//     cell's second sub-area grows with its far edge), now and then with noise - a key order that is not a total
//     order would make the ORIGINAL's bubble sort spin, which the game's geometry never gives;
//
// No row for SLUS 0x80021988 (the OT set-up): its last call, libgpu's ClearOTagR 0x80048CAC, programs DMA channel 6
// through *(0x80056090) and waits on VSync(-1) - hardware this harness does not model (the guest traps on DPCR at
// 0x80049C94). The product uses only its table map (cell_sort.h OtTableMapOf), checked in --parity against the map
// each capture's own call left in gp+0x848 / gp+0x84C / gp+0x170.
namespace cellsrows {

namespace s = rr::sim;
using fxrows::Around;
using spinerows::Chance;

uint32_t SlotRec(uint32_t i) { return 0x800D87E8u + 0x70u * i; }
uint32_t Ctx(uint32_t i) { return s::kCellCtx + s::kCellCtxStride * i; }
bool Resident(Memory& m, uint32_t i) {
    const uint32_t body = m.PeekWord(SlotRec(i) + 4u);
    return body >= 0x80000000u && body < 0x80200000u && m.PeekWord(body + 0x2Cu) >= 0x80000000u;
}
std::vector<uint32_t> ResidentSlots(Memory& m) {
    std::vector<uint32_t> v;
    for (uint32_t i = 0; i < 12; ++i)
        if (Resident(m, i)) v.push_back(i);
    return v;
}
int16_t GetS16(Memory& m, uint32_t a) {
    int16_t v = 0;
    m.ReadBlock(a, &v, 2);
    return v;
}

// The render camera moved: its eye around slot `near`'s origin, its rows a rotation about the vertical (now and then
// the captured ones kept).
void StirCamera(CaseContext& c, uint32_t near) {
    Memory& m = *c.mem;
    const uint32_t rc = m.PeekWord(s::kCellRenderCams);
    if (Chance(c, 4)) return;
    const uint32_t body = m.PeekWord(SlotRec(near) + 4u);
    const int32_t reach = Chance(c, 3) ? 60000 : 6000;
    for (uint32_t k = 0; k < 3; ++k) {
        const int32_t o = static_cast<int32_t>(m.PeekWord(body + 8u + 4u * k));
        m.PokeWord(rc + 0x1Cu + 4u * k, static_cast<uint32_t>(o + Around(c, k == 1 ? reach / 8 : reach)));
    }
    if (Chance(c, 3)) return;
    const double yaw = (c.rng->U32() % 4096u) * (6.283185307179586 / 4096.0);
    const int32_t cs = static_cast<int32_t>(std::lround(std::cos(yaw) * 4096.0)),
                  sn = static_cast<int32_t>(std::lround(std::sin(yaw) * 4096.0));
    const int16_t r[9] = {static_cast<int16_t>(cs), 0, static_cast<int16_t>(-sn),
                          0, static_cast<int16_t>(Chance(c, 2) ? 3413 : 4096), 0,
                          static_cast<int16_t>(sn), 0, static_cast<int16_t>(cs)};
    for (uint32_t k = 0; k < 9; ++k) WriteU16Bench(m, rc + 0x5Cu + 2u * k, static_cast<uint16_t>(r[k]));
}

// Slot i's context as 0x800353C4 leaves it for the render camera (rows copied, the origin less the eye turned), or a
// random rotation and translation.
void PlantContext(CaseContext& c, uint32_t i) {
    Memory& m = *c.mem;
    const uint32_t ctx = Ctx(i), rc = m.PeekWord(s::kCellRenderCams), body = m.PeekWord(ctx);
    for (uint32_t k = 0; k < 8; ++k) m.PokeWord(ctx + 0x0Cu + 4u * k, m.PeekWord(rc + 0x5Cu + 4u * k));
    int32_t d[3];
    for (uint32_t k = 0; k < 3; ++k)
        d[k] = static_cast<int32_t>(m.PeekWord(body + 8u + 4u * k)) - static_cast<int32_t>(m.PeekWord(rc + 0x1Cu + 4u * k));
    for (uint32_t r = 0; r < 3; ++r) {
        int32_t sum = 0;
        for (uint32_t k = 0; k < 3; ++k) sum += (GetS16(m, ctx + 0x0Cu + 6u * r + 2u * k) * d[k]) >> 12;
        m.PokeWord(ctx + 0x20u + 4u * r, static_cast<uint32_t>(sum));
    }
    if (Chance(c, 6))
        for (uint32_t k = 0; k < 3; ++k) m.PokeWord(ctx + 0x20u + 4u * k, static_cast<uint32_t>(Around(c, Chance(c, 3) ? 200000 : 20000)));
}

// A polygon of slot i's body: its region-3 polygon 0, a sub-area's, or a scratch list of its own indices.
void PickPolygon(CaseContext& c, uint32_t i, uint32_t& poly, int32_t& n) {
    Memory& m = *c.mem;
    const uint32_t polys = m.PeekWord(m.PeekWord(Ctx(i)) + 0x2Cu);
    const int32_t subs = static_cast<uint16_t>(GetS16(m, m.PeekWord(Ctx(i)) + 6u));
    const int32_t k = Chance(c, 2) ? -1 : static_cast<int32_t>(c.rng->U32() % static_cast<uint32_t>(std::max(subs, 1)));
    const int32_t from = GetS16(m, polys + 2u * static_cast<uint32_t>(k + 1)), to = GetS16(m, polys + 2u * static_cast<uint32_t>(k + 2));
    poly = polys + 2u * static_cast<uint32_t>(from);
    n = to - from;
    if (Chance(c, 5)) { // the body's own indices in another order and count
        const int32_t first = GetS16(m, polys), last = GetS16(m, polys + 2u * static_cast<uint32_t>(subs + 1));
        const int32_t count = 1 + static_cast<int32_t>(c.rng->U32() % 9u);
        for (int32_t j = 0; j < count; ++j) {
            const int32_t at = first + static_cast<int32_t>(c.rng->U32() % static_cast<uint32_t>(std::max(last - first, 1)));
            WriteU16Bench(m, c.scratch + 256u + 2u * static_cast<uint32_t>(j), static_cast<uint16_t>(GetS16(m, polys + 2u * static_cast<uint32_t>(at))));
        }
        poly = c.scratch + 256u;
        n = count;
    }
    if (Chance(c, 12)) n = 0;
}

// View 0's list: the resident slots shuffled with -1 holes; the camera's cell one of them, or not listed.
uint32_t PlantList(CaseContext& c, std::vector<uint32_t>& listed) {
    Memory& m = *c.mem;
    std::vector<uint32_t> slots = ResidentSlots(m);
    for (size_t k = slots.size(); k > 1; --k) std::swap(slots[k - 1], slots[c.rng->U32() % k]);
    const uint32_t n = std::min<uint32_t>(12u, static_cast<uint32_t>(slots.size()) + c.rng->U32() % 4u);
    listed.clear();
    size_t next = 0;
    for (uint32_t k = 0; k < n; ++k) {
        int32_t v = -1;
        if (next < slots.size() && !Chance(c, 4)) v = static_cast<int32_t>(slots[next++]);
        m.PokeWord(s::kCellDrawLists + 4u * k, static_cast<uint32_t>(v));
        if (v >= 0) listed.push_back(static_cast<uint32_t>(v));
    }
    for (uint32_t k = n; k < 12; ++k) m.PokeWord(s::kCellDrawLists + 4u * k, 0xFFFFFFFFu);
    return n;
}

// The sort's keys of the listed slots: +0x2C spread or clustered, +0x3C following it.
void PlantKeys(CaseContext& c, const std::vector<uint32_t>& listed) {
    Memory& m = *c.mem;
    const bool clustered = Chance(c, 2), noisy = Chance(c, 4);
    int32_t base = Around(c, 4000);
    for (uint32_t i : listed) {
        const int32_t mx = clustered ? base + static_cast<int32_t>(c.rng->U32() % 1200u) : Around(c, 40000);
        base += clustered && Chance(c, 3) ? 700 : 0;
        m.PokeWord(Ctx(i) + 0x2Cu, static_cast<uint32_t>(mx));
        m.PokeWord(Ctx(i) + 0x3Cu, static_cast<uint32_t>(mx / 2 - 300 + (noisy ? Around(c, 40) : 0)));
    }
}

void SetupDepthRange(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    const std::vector<uint32_t> slots = ResidentSlots(m);
    const uint32_t i = slots[c.rng->U32() % slots.size()];
    StirCamera(c, i);
    PlantContext(c, i);
    uint32_t poly = 0;
    int32_t n = 0;
    PickPolygon(c, i, poly, n);
    a.a[0] = Ctx(i);
    a.a[1] = poly;
    a.a[2] = static_cast<uint32_t>(n);
    a.a[3] = 0;
}

void SetupRegionRange(CaseContext& c, Args& a) {
    SetupDepthRange(c, a);
    Memory& m = *c.mem;
    // the three out-words in the scratch block (the LOD pass keeps them in its frame), noise in them first
    for (uint32_t k = 0; k < 3; ++k) m.PokeWord(c.scratch + 64u + 4u * k, c.rng->U32());
    a.a[3] = c.scratch + 64u;
    a.a4 = c.scratch + 68u;
    a.more[0] = c.scratch + 72u;
    a.more[1] = 0;
}

void SetupViewCull(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    std::vector<uint32_t> listed;
    const uint32_t n = PlantList(c, listed);
    const std::vector<uint32_t> slots = ResidentSlots(m);
    StirCamera(c, listed.empty() ? slots[0] : listed[c.rng->U32() % listed.size()]);
    const uint32_t view = s::kCellViewRecords;
    switch (c.rng->U32() % 4u) {
    case 0: m.PokeWord(view + 0xB0u, 0x01800000u | (c.rng->U32() & 0x3FFu)); break; // a cell not listed
    case 1: break;                                                                    // the captured one
    default:
        if (!listed.empty()) m.PokeWord(view + 0xB0u, m.PeekWord(SlotRec(listed[c.rng->U32() % listed.size()]) + 8u));
    }
    const uint32_t gp = c.gp;
    m.PokeWord(gp + s::kGpDrawCount2, n - c.rng->U32() % 3u);
    m.PokeWord(gp + s::kGpCameraSlot, c.rng->U32() % 13u);
    for (uint32_t i : listed) // the depth fields start from noise: the function writes them
        for (uint32_t o : {0x2Cu, 0x30u}) m.PokeWord(Ctx(i) + o, c.rng->U32());
    a.a[0] = gp + s::kGpDrawCount2;
    a.a[1] = n;
    a.a[2] = s::kCellDrawLists;
    a.a[3] = 0;
}

void SetupListSort(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    std::vector<uint32_t> listed;
    const uint32_t n = PlantList(c, listed);
    PlantKeys(c, listed);
    int32_t camera = -1;
    if (!listed.empty() && !Chance(c, 3)) { // 0x800353C4 left the camera's slot first
        camera = static_cast<int32_t>(listed[c.rng->U32() % listed.size()]);
        for (uint32_t k = 0; k < n; ++k)
            if (static_cast<int32_t>(m.PeekWord(s::kCellDrawLists + 4u * k)) == camera) {
                m.PokeWord(s::kCellDrawLists + 4u * k, m.PeekWord(s::kCellDrawLists));
                break;
            }
        m.PokeWord(s::kCellDrawLists, static_cast<uint32_t>(camera));
    }
    m.PokeWord(c.gp + s::kGpCameraSlot, static_cast<uint32_t>(camera));
    a.a[0] = n;
    a.a[1] = s::kCellDrawLists;
    a.a[2] = 0;
}

void SetupLodPass(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    std::vector<uint32_t> listed;
    const uint32_t n = PlantList(c, listed);
    StirCamera(c, listed.empty() ? ResidentSlots(m)[0] : listed[c.rng->U32() % listed.size()]);
    for (uint32_t i : listed) {
        PlantContext(c, i);
        for (uint32_t o : {0x34u, 0x3Cu}) m.PokeWord(Ctx(i) + o, c.rng->U32());
    }
    if (Chance(c, 4)) m.PokeWord(m.PeekWord(s::kCellGameState) + 0x30u, 2u);
    a.a[0] = n;
    a.a[1] = s::kCellDrawLists;
    a.a[2] = 0;
}

template <class F>
uint32_t RunCells(NativeEnv& env, F&& f) {
    s::GuestRam g = RqView(env);
    const uint32_t v = f(g);
    return RqDone(env, g, v);
}

Row CellsRow(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
             std::function<uint32_t(NativeEnv&, const Args&)> native, std::vector<OracleCallee> callees = {}) {
    return Row{name, sig, entry, std::move(setup), nullptr, true, std::move(native), std::move(callees)};
}

} // namespace cellsrows

void AddCellsRows(std::vector<Row>& rows) {
    using namespace cellsrows;
    rows.push_back(CellsRow("cells_depth_range", "s32 0x80035040(Ctx *c, s16 *poly, s32 n)  [SLUS, a cell's depth range and cull]",
                            s::kCellDepthRangeFn, SetupDepthRange, [](NativeEnv& env, const Args& a) {
                                return RunCells(env, [&](s::GuestRam& g) {
                                    return static_cast<uint32_t>(s::CellDepthRange(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2])));
                                });
                            }));
    rows.push_back(CellsRow("cells_region_range",
                            "s32 0x800351EC(Ctx *c, s16 *poly, s32 n, s32 *mean, s32 *min, s32 *max, s32 v)  [SLUS, a sub-area's range]",
                            s::kCellRegionRangeFn, SetupRegionRange, [](NativeEnv& env, const Args& a) {
                                return RunCells(env, [&](s::GuestRam& g) {
                                    return static_cast<uint32_t>(s::CellRegionRange(g, a.a[0], a.a[1], static_cast<int32_t>(a.a[2]),
                                                                                    a.a[3], a.a4, a.more[0]));
                                });
                            }));
    rows.push_back(CellsRow("cells_view_cull", "void 0x800353C4(s32 *count, s32 n, s32 *list, s32 v)  [SLUS, the cells' ranges, cull, camera first]",
                            s::kCellViewCullFn, SetupViewCull, [](NativeEnv& env, const Args& a) {
                                return RunCells(env, [&](s::GuestRam& g) {
                                    s::CellViewCull(g, a.a[0], static_cast<int32_t>(a.a[1]), a.a[2], a.a[3]);
                                    return 0u;
                                });
                            }));
    rows.back().compareV0 = false;
    rows.push_back(CellsRow("cells_list_sort", "void 0x80036438(s32 n, s32 *list, s32 v)  [SLUS, the cells by depth]",
                            s::kCellListSortFn, SetupListSort, [](NativeEnv& env, const Args& a) {
                                return RunCells(env, [&](s::GuestRam& g) {
                                    s::CellListSort(g, static_cast<int32_t>(a.a[0]), a.a[1], a.a[2]);
                                    return 0u;
                                });
                            }));
    rows.back().compareV0 = false;
    rows.push_back(CellsRow("cells_lod_pass", "void 0x80035680(s32 n, s32 *list, s32 v)  [SLUS, the cells' draw words]",
                            s::kCellLodPassFn, SetupLodPass, [](NativeEnv& env, const Args& a) {
                                return RunCells(env, [&](s::GuestRam& g) {
                                    s::CellLodPass(g, static_cast<int32_t>(a.a[0]), a.a[1], a.a[2]);
                                    return 0u;
                                });
                            }));
    rows.back().compareV0 = false;
}
