#pragma once
// The frame's cell sort in the product.
//
// SLUS 0x800358C0, per view p, after its draw list 0x80035F48 (cell_view.h CellDrawPass) and the draw loop's cell
// tests 0x8008D56C (DrawLoopCells: the view record's own cell +0xB0, which is the camera's cell): the PORTED
// 0x800353C4 (the cells' depth ranges and cull, the camera's cell first), 0x80036438 (the rest by depth; its +0x3C
// key is the previous frame's, as on the console, because 0x80035680 runs after it) and 0x80035680 (the cells'
// draw words +0x34 and +0x3C), on the session's arena, with the view's render camera RenderCamera 0x8002F17C (PORTED,
// effects.h) made first as SLUS 0x80011C4C makes it before the draw loop. The renderer then draws from the arena's
// list and words (race_scene_ot.cpp): which cells, in which of the two ordering tables, with which groups fine.
//
// OURS, named: the side planes of the two range tests are opened by the picture's width over 4:3 (cell_sort.h
// `widen`); a 4:3 picture (and --parity) runs the console's test bit for bit. 0x80036438's 0x8002E698 (a GTE-only
// product, no byte in RAM) is not run. RRJB_CELL_SORT=off: the pass is not run and the renderer orders and picks the
// cells by its own float depths (the control).
#include <cstdint>
#include <string>

#include "game/sim/road_query.h"

namespace rr::game {

bool CellSortOn();
// The picture's width over 4:3, in 1/4096 (4096 = 4:3). The product sets it from its window each frame.
void SetCellSortWiden(int32_t widen);
int32_t CellSortWiden();

void CellSortPass(rr::sim::GuestRam& g, int players);

// --parity: the pass run on a COPY of the capture (the arena stays the capture's own - the original's list, words
// and keys), and its list, depth ranges and draw words compared with the capture's. One line for the log.
std::string CellSortCheckCapture(const uint8_t* ram, int players);

std::string CellSortTotals();

} // namespace rr::game
