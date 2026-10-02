#pragma once
// The player's controls, ported from the original MIPS code.
//
// The chain, established by watching the live machine:
//
//   SIO0  ->  the driver's raw pad buffer at guest 0x800D70E0 (`00 41 <lo> <hi>`, active low)
//         ->  `0x8001C704` inverts it into the per-player INPUT RECORD at 0x800D6DE0 + 192*p,
//             whose 19 button slots live at `+0x14`, stride 8, and whose two ANALOGUE axes are the
//             raw bytes at `+0x09` and `+0x0A`
//         ->  `0x8001CB3C` copies the whole 768-byte array to 0x800D7128 (the frame-stable copy the
//             game reads), and then, per player:
//               * `0x8001CFC8`/`0x8001CFE8` turn the two axis bytes into 16.16 values through
//                 `AxisCurve` below and store them at `0x800CE540 + 8*p + 0` and `+ 4`;
//               * `0x8001D024..0x8001D32C` turn the button slots into a bit mask and merge it
//                 straight into the bike's `flagsA` at `entity + 0x230` (`0x8001D338..0x8001D34C`)
//         ->  `RASHCDG 0x8009926C` reads `0x800CE540 + 8*p` back out as the steering and throttle
//             axes and `RASHCDG 0x80073874` reads `0x800D7128 + 192*p + 16` as the "this player is
//             under manual control" gate.
//
// All addresses are in the resident executable `SLUS_010.53`, SHA-1
// 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text base 0x80010000.
#include <cstdint>

namespace rr::sim {

// ---------------------------------------------------------------------------- 0x8001CA58
// s32 AxisCurve(u8 raw, const u16 *curve, const uint8_t *cfg)
//
// One analogue axis: the raw 0..255 byte becomes a signed 16.16 value through a piecewise-linear
// curve. `cfg` is the block `ENV.EN` is loaded into at guest `0x800D38E0` (population.md 7): its
// `+0xB0` is the dead zone and its `+0xB2` the width of each segment. `curve` is the six-halfword
// breakpoint table the caller picks - `0x800D3984` for one axis and `0x800D3978` for the other,
// i.e. the same `ENV.EN` image, which is a first concrete reading of that file.
//
//   d = raw - 127;  t = (d >= 0) ? d : (1 - d);       // note the asymmetric +1
//   if (t <= cfg[0xB0]) return 0;                     // dead zone
//   walk the segments until `t` falls inside one, or the four segments run out;
//   inside a segment, interpolate between `curve[i]` and `curve[i+1]`;
//   past the last segment, saturate to 1.0;
//   return (d >= 0) ? v : -v;
//
// The interpolation divides with a real `div`, so a zero segment width takes the R3000's
// division-by-zero result (`lo = -1` for a non-negative dividend, `+1` otherwise) rather than
// trapping - which is what the original does.
int32_t AxisCurve(uint8_t raw, const uint16_t* curve, const uint8_t* cfg);

} // namespace rr::sim
