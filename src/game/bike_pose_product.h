#pragma once
// The bike's own part slots in the product.
//
// The draw loop RASHCDG 0x8008D56C runs, for every live bike its visibility test 0x8008B99C passes,
// BikeInstance 0x80084E10 (the fork slot, the pitch spring, slot 2) and - under the draw - the wheel
// slots of 0x80066EC4. The product runs both PORTED (bike_parts.h) for every live bike after the race
// step, with the draw itself not run (the renderer draws the parts from the slots it reads back here).
// The cell test 0x8008B99C is PORTED (cell_draw.h): the session runs BikeInstance only for
// a live bike whose +0xB0 it set > 0 (in a loaded cell), as 0x8008D5CC does.
#include <cstdint>

#include "rrformats/pose.h"

namespace rr::game {

// False: the view faulted (nothing to trust).
bool RunBikeParts(uint8_t* ram, uint32_t gp, uint32_t bike, const uint16_t* asinTable);

// The bike's five part slots for the renderer: slot 0 the identity (the object's world orientation is
// the renderer's model matrix), slots 1..4 as the arena holds them. False: no part slots.
bool ReadBikeParts(const uint8_t* ram, uint32_t bike, rr::PartMatrix out[5]);

// The same for the bound model's own part count (its DOD3 +0x18, at most `max`): the sidecar rig's six - slot 5
// is BikeWheels' third wheel. Returns the count, 0 when there are no part slots.
int ReadMachineParts(const uint8_t* ram, uint32_t bike, rr::PartMatrix* out, int max);

} // namespace rr::game
