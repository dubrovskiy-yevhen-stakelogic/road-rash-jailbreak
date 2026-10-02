"""RASHCDG 0x80080B10 (sha1 cfe43a77...), 524 bytes, frame 32:
    s32 SetHitSpeed(Bike *e, s16 dir[3], s32 *pSpeed, const s16 src[3], [entry+16] s32 speed) -> 0 or 1
The "hard" arm of 0x80083928 / 0x80081D7C: the bike takes `speed` along `src`."""
import sys
pass
import impact as I, pairs as P
from .m_80080d1c import gte, rad
u32, s32 = P.u32, P.s32
FRAME = 32
DOT, SCALE, ATAN2, NORM = 0x8002E698, 0x8002EE50, 0x80020018, 0x8002E468
from ._util import MUTD as MUT


def model(m, e, dirp, pspeed, src):
    s0 = m.lws(m.entry_sp + 16)
    f = m.lw(e + 568)
    f = (f | 0x400000) if m.lws(e + 364) < 0 else (f & 0xFFBFFFFF)
    m.sw(e + 568, f)                          # 0x80080B54
    if not (0x1FFFF < s0):
        m.sw(e + 568, (m.lw(e + 568) & 0xFFFFFFCF) | 0x08000000)       # 0x80080B84
        return 0
    for k in range(3):
        m.sh(dirp + 2 * k, m.lhu(src + 2 * k))                          # 0x80080B90/9C/A8
    if m.lw(e + 568) & 0x600:
        m.sw(pspeed, s0)                      # 0x80080BC0
        m.call(SCALE, s0, dirp, e + 456)
        return 1
    gte(m, e + 522, dirp, e + 814)                                      # 0x80080C24..2C
    if m.call(NORM, e + 814) == 0:
        for k in range(3):
            m.sh(e + 814 + 2 * k, m.lhu(e + 432 + 2 * k))               # 0x80080C4C..54
    p = m.lw(e + 856)
    m.sw(e + 576, s0)                         # 0x80080C5C  entity +0x240 = speed
    m.sw(e + 488, 0)                          # 0x80080C64
    if p:
        m.sw(p + 488, 0)                      # 0x80080C68
    d1 = m.call(DOT, e + 528, e + 814)
    d2 = m.call(DOT, e + 528, e + 450)
    ang = m.call(ATAN2, d1, d2)
    m.sw(e + 676, rad(ang) + (1 if MUT['on'] else 0))                  # 0x80080CC4
    fl = m.lw(e + 568)
    m.sw(e + 680, 0)                          # 0x80080CD0
    if fl & 0x100:
        v = 0x20000 if m.lws(e + 636) > 0 else 0xFFFE0000
        fl = m.lw(e + 568)
        m.sw(e + 656, v)                      # 0x80080CEC
        m.sw(e + 568, fl & 0xFFFFFFCF)        # 0x80080CF8
    return 1
