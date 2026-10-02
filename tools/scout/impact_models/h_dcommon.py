"""shared helpers of the impact models"""
import sys
pass
import pairs as P  # noqa: E402
u32, s32, iabs = P.u32, P.s32, P.iabs
TAB = 0x800541D4          # SLUS rider-state table, 8 B per state: u32 w0 (bits 0..3 bank, 4..15 clip, 16..31 cat), u32 mask
GS = 0x8005B2F8           # -> game state
PERSTATE = 0x8005B3E4     # -> u32[state] (0 = none)
BANKS = 0x800CE190        # u32[16], indexed by w0 & 0xF
from ._util import MUTD as MUT


def cat(m, s):
    return m.lhu(TAB + 8 * (s & 0xFFFF) + 2)
