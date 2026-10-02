#include "game/cell_sort_product.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "game/sim/cell_draw.h"
#include "game/sim/cell_sort.h"
#include "game/sim/effects.h" // RenderCamera 0x8002F17C

namespace rr::game {
namespace {

namespace s = rr::sim;

struct Totals {
    size_t frames = 0, views = 0, listed = 0, culled = 0, behind = 0, cameraFirst = 0, cameraNotListed = 0,
           cameraNone = 0, passesMax = 0, unsettled = 0, fineCells = 0, faults = 0;
    std::string check;
};
Totals& T() {
    static Totals t;
    return t;
}
int32_t g_widen = s::kWidenUnit;

constexpr int32_t kSortPassCap = 1000; // OURS: the console would spin on a key order that never settles

// One view: 0x800358C0 after its draw list.
void SortView(s::GuestRam& g, uint32_t p, int32_t widen, bool count) {
    const uint32_t gp = g.gp();
    const int32_t n = g.S32(gp + s::kGpDrawCount + 4u * p);
    if (n <= 0) return;
    const uint32_t list = s::kCellDrawLists + 48u * p;
    const int32_t before = g.S32(gp + s::kGpDrawCount2 + 4u * p);
    s::CellViewCull(g, gp + s::kGpDrawCount2 + 4u * p, n, list, p, widen);
    bool settled = true;
    const int32_t passes = s::CellListSort(g, n, list, p, &settled, kSortPassCap);
    s::CellLodPass(g, n, list, p, widen);
    if (!count) return;
    ++T().views;
    T().passesMax = std::max(T().passesMax, static_cast<size_t>(passes));
    if (!settled) ++T().unsettled;
    T().culled += static_cast<size_t>(std::max(0, before - g.S32(gp + s::kGpDrawCount2 + 4u * p)));
    const uint32_t cameraCell = g.U32(s::kCellViewRecords + s::kCellViewStride * p + 0xB0u);
    if (g.S32(gp + s::kGpCameraSlot) != -1) ++T().cameraFirst;
    else if (cameraCell == 0 || cameraCell == 0xFFFFFFFFu) ++T().cameraNone;
    else ++T().cameraNotListed;
    for (int32_t k = 0; k < n && k < 12; ++k) {
        const int32_t slot = g.S32(list + 4u * static_cast<uint32_t>(k));
        if (slot < 0 || slot >= 24) continue;
        ++T().listed;
        const uint32_t ctx = s::kCellCtx + s::kCellCtxStride * static_cast<uint32_t>(slot);
        if (g.S32(ctx + 0x2Cu) <= 0) ++T().behind; // 0x80035958 skips it
        if ((g.U32(ctx + 0x34u) & 0x22222222u) != 0) ++T().fineCells;
    }
}

} // namespace

bool CellSortOn() {
    static const bool on = [] {
        const char* v = std::getenv("RRJB_CELL_SORT");
        return v == nullptr || std::strcmp(v, "off") != 0;
    }();
    return on;
}

void SetCellSortWiden(int32_t widen) { g_widen = widen > 0 ? widen : s::kWidenUnit; }
int32_t CellSortWiden() { return g_widen; }

void CellSortPass(s::GuestRam& g, int players) {
    if (!CellSortOn()) return;
    if (g.U32(g.gp() + s::kGpRlsTable) == 0) return; // no draw list (cell_view.cpp CellDrawPass)
    ++T().frames;
    for (int p = 0; p < players && p < 2; ++p) {
        s::RenderCamera(g, static_cast<uint32_t>(p)); // SLUS 0x80011C4C: 0x8002F2E8 before 0x8008D56C / 0x800358C0
        SortView(g, static_cast<uint32_t>(p), g_widen, true);
        if (g.Faulted()) {
            ++T().faults;
            g.ClearFault();
        }
    }
}

std::string CellSortCheckCapture(const uint8_t* ram, int players) {
    std::vector<uint8_t> copy(ram, ram + 0x200000);
    s::GuestRam g(copy.data(), 0x8005AC8Cu);
    s::GuestRam c(const_cast<uint8_t*>(ram), 0x8005AC8Cu);
    size_t cells = 0, rangesEqual = 0, wordsEqual = 0, listsEqual = 0, views = 0;
    int32_t worst = 0;
    for (int p = 0; p < players && p < 2; ++p) {
        const uint32_t pp = static_cast<uint32_t>(p);
        const int32_t n = c.S32(c.gp() + s::kGpDrawCount + 4u * pp);
        if (n <= 0) continue;
        ++views;
        // the capture's list is already the sorted one: its sort ran with the previous frame's +0x3C, which the
        // capture no longer holds - so the check compares the cull and the ranges, then the words over the
        // capture's own order
        s::RenderCamera(g, pp);
        s::CellViewCull(g, g.gp() + s::kGpDrawCount2 + 4u * pp, n, s::kCellDrawLists + 48u * pp, pp, s::kWidenUnit);
        bool same = true;
        for (int32_t k = 0; k < n && k < 12; ++k) {
            const uint32_t a = s::kCellDrawLists + 48u * pp + 4u * static_cast<uint32_t>(k);
            same = same && g.U32(a) == c.U32(a);
        }
        if (same) ++listsEqual;
        for (int32_t k = 0; k < n && k < 12; ++k) { // the capture's order for the words
            const uint32_t a = s::kCellDrawLists + 48u * pp + 4u * static_cast<uint32_t>(k);
            g.W32(a, c.U32(a));
        }
        s::CellLodPass(g, n, s::kCellDrawLists + 48u * pp, pp, s::kWidenUnit);
        for (int32_t k = 0; k < n && k < 12; ++k) {
            const int32_t slot = c.S32(s::kCellDrawLists + 48u * pp + 4u * static_cast<uint32_t>(k));
            if (slot < 0 || slot >= 24) continue;
            const uint32_t ctx = s::kCellCtx + s::kCellCtxStride * static_cast<uint32_t>(slot);
            ++cells;
            const int32_t d = std::max(std::abs(g.S32(ctx + 0x2Cu) - c.S32(ctx + 0x2Cu)), std::abs(g.S32(ctx + 0x30u) - c.S32(ctx + 0x30u)));
            worst = std::max(worst, d);
            if (d == 0) ++rangesEqual;
            if (g.U32(ctx + 0x34u) == c.U32(ctx + 0x34u) && g.U32(ctx + 0x3Cu) == c.U32(ctx + 0x3Cu)) ++wordsEqual;
            if (std::getenv("RRJB_CELL_SORT_TRACE"))
                std::printf("cellsort check: view %d slot %d max %d / %d min %d / %d word %08X / %08X key %d / %d\n", p, slot,
                            g.S32(ctx + 0x2Cu), c.S32(ctx + 0x2Cu), g.S32(ctx + 0x30u), c.S32(ctx + 0x30u), g.U32(ctx + 0x34u),
                            c.U32(ctx + 0x34u), g.S32(ctx + 0x3Cu), c.S32(ctx + 0x3Cu));
        }
    }
    char b[400];
    std::snprintf(b, sizeof(b),
                  "cellsort: the PORTED 0x800353C4 / 0x80035680 re-run on a copy of the capture: %zu view(s), list after "
                  "the cull equal to the capture's in %zu; %zu listed cell(s), depth ranges +0x2C / +0x30 equal in %zu "
                  "(worst %d), draw words +0x34 / +0x3C equal in %zu",
                  views, listsEqual, cells, rangesEqual, worst, wordsEqual);
    T().check = b;
    return b;
}

std::string CellSortTotals() {
    char b[700];
    if (!CellSortOn())
        return "cellsort: the frame's cell sort (SLUS 0x800353C4 / 0x80036438 / 0x80035680) NOT run (RRJB_CELL_SORT=off, the "
               "control): the renderer orders and picks the cells by its own float depths\n";
    std::snprintf(b, sizeof(b),
                  "cellsort: the frame's cell sort PORTED (SLUS 0x800353C4 / 0x80036438 / 0x80035680, cell_sort.h) in %zu "
                  "frame(s), %zu view pass(es): %zu listed cell(s), %zu culled by 0x80035040 (set to -1), %zu behind (max <= 0, "
                  "not drawn by 0x80035958), %zu with fine groups; the camera's cell (view +0xB0) first in %zu, not listed in "
                  "%zu, none in %zu; the depth sort took at most %zu pass(es), %zu did not settle; %zu fault(s); side planes "
                  "widened x%.3f (4:3 = 1)%s%s\n",
                  T().frames, T().views, T().listed, T().culled, T().behind, T().fineCells, T().cameraFirst,
                  T().cameraNotListed, T().cameraNone, T().passesMax, T().unsettled, T().faults,
                  static_cast<double>(g_widen) / 4096.0, T().check.empty() ? "" : "\n", T().check.c_str());
    return b;
}

} // namespace rr::game
