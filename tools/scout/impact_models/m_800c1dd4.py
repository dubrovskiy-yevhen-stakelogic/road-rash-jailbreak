# FightPush 0x800C1DD4(bike) - RASHCDG cfe43a77, 604 B, frame 32
import sys
from pairs import u32, s32, iabs
FRAME = 32
from ._util import MUTX as MUT
PICK, PUSH = 0x8008B428, 0x800BCA68
LASTSLOT, LASTTIME, GS, POOL0 = 0x800CCB6C, 0x800CCB70, 0x8005B2F8, 0x8005B3A0

def model(m, a0, a1, a2, a3):
    e = a0
    rd = m.lw(e + 1084)
    if m.lw(rd + 40) != 0:                  # riderDef+0x28 finish stamp
        return None
    if not m.lbu(rd + 39) < 248:            # riderDef+0x27 place / result code
        return None
    sp = m.sp
    m.sh(sp + 16, 16)                       # command record {u16 op = 16, u16 target}
    if m.lw(e + 1088) == 0:                 # +0x440
        v = m.call(PICK, u32(m.lw(e + 856) + 172), 1)
        m.sh(sp + 18, v)
        t = m.lhu(sp + 18)
        if m.lw(m.lw(e + 852) + 552) & 0x00800000:
            t = m.lhu(m.lw(e + 856) + 172)
        m.sh(sp + 18, t)
    else:
        r = m.lw(e + 852)
        if (m.lbu(r + 572) & 0x10) and (m.lw(r + 552) & 0x00800000):
            m.sh(sp + 18, m.lhu(m.lw(e + 856) + 172))
        else:
            h = m.lhu(e + 172)
            slotp = u32(LASTSLOT + u32(h << 1))
            keep = 0
            if m.lhu(slotp) != 224:
                gs = m.lw(GS)
                if s32(m.lw(gs + 16) - 59) < m.lws(u32(LASTTIME + u32(h << 2))):
                    t = u32(m.lw(POOL0) + 1096 * m.lhu(slotp))
                    keep = t
                    dx = s32(m.lw(e + 504) - m.lw(t + 504))
                    dz = s32(m.lw(e + 512) - m.lw(t + 512))
                    ax = ((dx >> 16) + (dx >> 31)) ^ (dx >> 31)
                    az = ((dz >> 16) + (dz >> 31)) ^ (dz >> 31)
                    big, small = (az, ax) if ax < az else (ax, az)
                    s15 = small + (small >> 1)
                    d = big - (big >> 5) - (big >> 7) + (s15 >> 2) + (s15 >> 6)
                    if not d < 2 and not MUT:
                        keep = 0
            if keep == 0:
                v = m.call(PICK, u32(e + 172), 1)
                m.sh(sp + 18, v)
            else:
                m.sh(sp + 18, m.lhu(u32(LASTSLOT + u32(m.lhu(e + 172) << 1))))
    depth = m.lbs(e + 946)
    top = u32(e + 948 + 8 * depth)          # the top command record (+0x3B4 + 8*(depth-1))
    if m.lhu(top) == 16 and m.lhu(top + 2) == m.lhu(sp + 18):
        return None
    m.call(PUSH, sp + 16, 2, e)
    return None
