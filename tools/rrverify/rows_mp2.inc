// Bench rows of the two-player leftovers: the cell-state copy SLUS 0x8001339C, the
// class-50 release RASHCDG 0x800A3ECC (src\game\sim\mp_world.h) and the two-player shadow SLUS 0x80026960
// (src\game\sim\shadow2p.h). Included LAST by verify_physics.cpp inside its anonymous namespace, after
// rows_world.inc (worldrows::WRow / RunWorld / StirWorld) and rows_look.inc (lookrows::ShadowObject / PutUnit).
//
// The machine is a one-player race capture, so player 1's half of the cell slot table (0x800D87E8 + 1344) is not
// live there. The cell-copy cases build it: one of player 1's twelve slots is made the twin of one of player 0's
// resident cells (same id, its own body in the case's scratch buffer with a region-0 header whose five arrays
// hold the real cell's records, their live words +4 stirred), the other eleven empty; the two-player mode bit
// game_state+4 bit 4 is set in most cases. Named bound: the cell's record counts are cut (in the real header) so
// that the twin's arrays fit the 8 KiB scratch buffer - a cell with fewer records, a shape the game loads.
// The shadow cases are the look_shadow row's inputs (a bike or rider of the capture, view 0, its ground point
// near the render camera, the normal / light stirred and nearly across) plus the object's four points +0xC4..
// put around the ground point (16.16, within 3 units) now and then, and the sprite descriptor 0x800D4270 as
// captured or stirred.
namespace mp2rows {

namespace s = rr::sim;
namespace S = rr::sim::shadow;
using fxrows::Around;
using fxrows::PutS16;
using spinerows::Chance;

constexpr uint32_t kSlots1 = 0x800D87E8u + 1344u; // player 1's twelve 112-byte slots

// The twin of `cell` in player 1's slot `k` (or none), its body at `scratch`.
void BuildTwin(CaseContext& c, const worldrows::Cell& cell) {
    Memory& m = *c.mem;
    static const uint32_t kStride[5] = {68, 76, 64, 88, 64}; // arrays +0x24..+0x34, counts +0..+8
    // cut the real counts so that 0x40 + 0x38 + the arrays fit the scratch buffer
    uint32_t total = 0x80u;
    for (uint32_t a = 0; a < 5; ++a) {
        uint32_t n = worldrows::Get16(m, cell.r0 + 2u * a);
        const uint32_t room = total < kScratchBytes ? (kScratchBytes - total) / kStride[a] : 0u;
        if (n > room) {
            n = room;
            worldrows::Put16(m, cell.r0 + 2u * a, static_cast<uint16_t>(n));
        }
        total += n * kStride[a];
    }
    const uint32_t body = c.scratch, r0 = c.scratch + 0x40u;
    for (uint32_t k = 0; k < 0x40u; k += 4) m.PokeWord(body + k, c.rng->U32());
    m.PokeWord(body + 0x20u, r0);
    std::vector<uint8_t> hdr(0x38);
    m.ReadBlock(cell.r0, hdr.data(), 0x38);
    m.WriteBlock(r0, hdr.data(), 0x38);
    uint32_t at = r0 + 0x40u;
    for (uint32_t a = 0; a < 5; ++a) {
        const uint32_t n = worldrows::Get16(m, cell.r0 + 2u * a);
        const uint32_t src = m.PeekWord(cell.r0 + 0x24u + 4u * a);
        m.PokeWord(r0 + 0x24u + 4u * a, n != 0 ? at : 0u);
        if (n != 0 && src >= 0x80000000u && src < 0x80200000u) {
            std::vector<uint8_t> b(n * kStride[a]);
            m.ReadBlock(src, b.data(), static_cast<uint32_t>(b.size()));
            m.WriteBlock(at, b.data(), static_cast<uint32_t>(b.size()));
            for (uint32_t i = 0; i < n; ++i)
                if (Chance(c, 2)) worldrows::StirRecord(*c.rng, m, at + kStride[a] * i);
            if (a == 2 && Chance(c, 3)) // a kind-4 record of class 50 (the class-50 arm)
                worldrows::Put16(m, at + 64u * (c.rng->U32() % n) + 2u, 50);
        }
        at += n * kStride[a];
    }
    // player 1's slots: all empty, one the twin (same id) - or a stranger's id, or none
    for (uint32_t k = 0; k < 12; ++k) {
        const uint32_t sl = kSlots1 + 112u * k;
        m.PokeWord(sl, 0xFFFFFFFFu);
        m.PokeWord(sl + 4u, 0);
        m.PokeWord(sl + 8u, 0xFFFFFFFFu);
    }
    if (Chance(c, 8)) return;
    const uint32_t sl = kSlots1 + 112u * (c.rng->U32() % 12u);
    const uint32_t id = Chance(c, 8) ? (0x01800000u | (c.rng->U32() & 0x3FFu)) : cell.id;
    m.PokeWord(sl, id);
    m.PokeWord(sl + 4u, body);
    m.PokeWord(sl + 8u, id);
}

} // namespace mp2rows

void AddMp2Rows(std::vector<Row>& rows) {
    using namespace mp2rows;
    using G = s::GuestRam;
    using T = s::BikeTables;
    using C = rr::sim::RecoverCallees;

    rows.push_back(worldrows::WRow(
        "mp2_cell_copy", "void 0x8001339C(id, p)  [SLUS, the two-player cell-state copy]", s::mp::kCellCopyFn,
        [](CaseContext& c, Args& a) {
            const worldrows::Pick pk = worldrows::StirWorld(c);
            Memory& m = *c.mem;
            const uint32_t gs = m.PeekWord(s::kWpGameStatePtr);
            const uint8_t mode = m.PeekByte(gs + 4u);
            worldrows::Put8(m, gs + 4u, static_cast<uint8_t>(Chance(c, 6) ? (mode & ~0x10u) : (mode | 0x10u)));
            if (pk.cell.slot != 0) BuildTwin(c, pk.cell);
            a.a[0] = Chance(c, 10) ? (0x01800000u | (c.rng->U32() & 0x3FFu)) : pk.cell.id;
            a.a[1] = c.rng->U32() % 2u;
        },
        [](NativeEnv& env, const Args& a) {
            return worldrows::RunWorld(env, [&](G& g, const T&, C&, uint32_t&) {
                s::mp::CellStateCopy(g, a.a[0], a.a[1]);
                return true;
            });
        },
        false));

    rows.push_back(fxrows::FxRow(
        "mp2_class50_release", "void 0x800A3ECC(id, p)  [RASHCDG, the two-player class-50 release]", s::mp::kClass50FreeFn,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            const uint32_t id = 0x01000000u | (c.rng->U32() & 0xFFFFFu);
            for (uint32_t p = 0; p < 2; ++p) {
                const uint32_t r = 0x800D43C0u + 28u * p;
                for (uint32_t k = 0; k < 22u; k += 2) PutS16(m, r + k, Around(c, 30000));
                PutS16(m, r + 22u, Chance(c, 3) ? 0 : (Chance(c, 2) ? 1 : Around(c, 30000)));
                m.PokeWord(r + 24u, Chance(c, 4) ? (id ^ (1u + c.rng->U32() % 64u)) : id);
            }
            a.a[0] = id;
            a.a[1] = c.rng->U32() % 2u;
        },
        [](NativeEnv& env, const Args& a) {
            s::GuestRam g = RqView(env);
            s::mp::Class50Release(g, a.a[0], a.a[1]);
            return RqDone(env, g, 0u);
        },
        false));

    rows.push_back(fxrows::FxRow(
        "mp2_shadow", "void Shadow2p(Object *o, s32 view, s32 *ground, s16 *normal)  [SLUS 0x80026960]", 0x80026960u,
        [](CaseContext& c, Args& a) {
            Memory& m = *c.mem;
            const uint32_t o = lookrows::ShadowObject(c);
            a.a[0] = o;
            a.a[1] = 0;
            const uint32_t owner = m.PeekWord(o + 0x34u);
            const uint32_t base = owner != 0 && Chance(c, 2) ? owner : o;
            const uint32_t ground = base + 0x1F8u, normal = base + 0x20Au;
            a.a[2] = ground;
            a.a[3] = normal;
            const uint32_t cam = m.PeekWord(S::kRenderCams);
            if (Chance(c, 3)) {
                for (uint32_t k = 0; k < 3; ++k) {
                    const int32_t eye = static_cast<int32_t>(m.PeekWord(cam + 0x1Cu + 4u * k));
                    const int32_t d = Around(c, Chance(c, 6) ? 20000 : 600);
                    m.PokeWord(ground + 4u * k, (static_cast<uint32_t>(eye + d) << 10) | (c.rng->U32() & 0x3FFu));
                }
            }
            // the four points around the ground point (the capture's otherwise)
            if (!Chance(c, 3))
                for (uint32_t p = 0; p < 4; ++p)
                    for (uint32_t k = 0; k < 3; ++k)
                        m.PokeWord(o + 0xC4u + 12u * p + 4u * k,
                                   m.PeekWord(ground + 4u * k) + static_cast<uint32_t>(Around(c, Chance(c, 8) ? 0x7FFFFF : 0x30000)));
            if (Chance(c, 3)) lookrows::PutUnit(c, normal);
            if (Chance(c, 5)) lookrows::PutUnit(c, S::kLight);
            if (Chance(c, 6)) { // n nearly perpendicular to L: n.L near 0 (the reciprocal's edge)
                const int32_t lx = GetS16(m, S::kLight), ly = GetS16(m, S::kLight + 2u), lz = GetS16(m, S::kLight + 4u);
                PutS16(m, normal, -ly + Around(c, 20));
                PutS16(m, normal + 2u, lx + Around(c, 20));
                PutS16(m, normal + 4u, Around(c, 20) + (Chance(c, 2) ? 0 : lz / 64));
            }
            if (Chance(c, 4)) // the sprite descriptor: another sprite's size, page and palette
                for (uint32_t k = 0; k < 24u; k += 4) m.PokeWord(S::kSprites + k, c.rng->U32() % (k < 16u ? 256u : 65536u));
            fxdrawrows::PlantRanges(c);
        },
        [](NativeEnv& env, const Args& a) {
            s::GuestRam g = RqView(env);
            g.SetScratchpad(env.clone->mem.scratchpad().data());
            const uint32_t* cr = env.clone->cpu.gte().cr;
            S::Gte gte;
            gte.ofx = static_cast<int32_t>(cr[24]);
            gte.ofy = static_cast<int32_t>(cr[25]);
            gte.h = static_cast<uint16_t>(cr[26] & 0xFFFFu);
            gte.zsf4 = static_cast<int16_t>(cr[30] & 0xFFFFu);
            if (!S::Shadow2p(g, gte, a.a[0], a.a[1], a.a[2], a.a[3]) && !g.Faulted() && env.failure.empty())
                env.failure = "mp2_shadow: the port refused (heap full / Normalize overflow)";
            return RqDone(env, g, 0u);
        },
        false));
}
