"""shared constants / integer idioms of the impact models (RASHCDG cfe43a77..., SLUS 67ed165a...)"""
import sys
pass
import pairs as P
u32, s32, iabs = P.u32, P.s32, P.iabs

FIXMUL, FIXDIV, DOTLCM, SCALE = 0x8001FC90, 0x80010028, 0x8002E698, 0x8002EE50
RAND, ASIN, NORMALIZE, BLEND16 = 0x8001FC58, 0x8001FF3C, 0x8002E468, 0x8002EB78
RATATAN2, AIPROJ, PS3D, SURFSND = 0x80020018, 0x800B6AAC, 0x80017BA0, 0x80017B30
RUMBLE = 0x800B658C
TRIG = 0x8005624C            # SLUS {s16 sin, s16 cos} x 4096
GS = 0x8005B2F8              # -> game state (+0x30 numPlayers)

from ._util import MUTN as MUT


def mut(name):
    return bool(MUT['on'])


def sra(v, n):
    return u32(s32(v) >> n)


def MH(x, h16):
    """mult x, (lh << 4); (lo >> 16) | (hi << 16): the 64-bit product >> 16, low word"""
    hs = ((h16 & 0xFFFF) ^ 0x8000) - 0x8000
    return u32((s32(x) * s32(u32(hs << 4))) >> 16)


def dot3(m, x, axis):
    """sum of MH(x[i], lh(axis + 2i)), addu (wraps)"""
    return u32(sum(MH(x[i], m.lhu(axis + 2 * i)) for i in range(3)))


def blk(m, pc):
    m.cover.add(pc)
