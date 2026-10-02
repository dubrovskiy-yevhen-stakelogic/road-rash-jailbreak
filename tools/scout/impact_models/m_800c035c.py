# FightUpdate 0x800C035C(bike me, u16 targetSlot, dt) - RASHCDG cfe43a77, 1620 B, frame 72
import sys
from pairs import u32, s32, iabs
FRAME = 72
from ._util import MUTX as MUT
POOL0, GS, STATES, LATLIM = 0x8005B3A0, 0x8005B2F8, 0x800541D4, 0x80052F74
S7 = 0x08000000


def model(m, a0, a1, a2, a3):
    me, slot = a0, a1 & 0xFFFF
    sp = m.sp
    FL, ST = sp + 24, sp + 28
    m.sw(ST, 0)
    m.sw(FL, 0)
    s8 = 0
    rider = m.lw(me + 852)                                    # +0x354 own rider
    tgt = 0 if slot == 224 else u32(m.lw(POOL0) + 1096 * slot)
    if m.lbu(m.lw(me + 1084) + 60) & 0x40:                    # riderDef+0x3C bit 6
        m.sw(FL, 32)
    cat3 = lambda: m.lhu(u32(STATES + u32(m.lhu(rider + 544) << 3)) + 2) == 3
    locked = lambda: u32(m.lhu(rider + 544) - 30) < 8         # rider+0x220 in 30..37
    gated = lambda: (m.lw(me + 560) & S7) or (m.lbu(rider + 572) & 0x40)
    if not gated():
        if locked():
            m.call(0x800C1014, rider)
            return None
        if not cat3():
            m.call(0x800C110C, me, tgt)
            s8 = 1
        else:
            m.sw(ST, m.call(0x800C09B0, me))
        if slot == 224:
            return None
    along = s32(m.call(0x800B6AAC, u32(me + 504), u32(tgt + 528), u32(tgt + 504)))   # s6
    gs = m.lw(GS)
    if m.lbu(gs + 57) == 3 and m.lhu(tgt + 172) < m.lw(gs + 48):
        s4 = 5
    else:
        s4 = m.call(0x800BC1EC, me, tgt, u32(-along))
    v1 = m.lw(me + 360)
    if v1 == m.lw(tgt + 360) and ((v1 >> 16) == 0 or m.lw(me + 336) == m.lw(tgt + 336)):
        lat = s32(m.lw(me + 344) - m.lw(tgt + 344))
        if m.lws(tgt + 364) < 0:
            lat = s32(-lat)
    else:
        lat = s32(m.call(0x800B6AAC, u32(me + 184), u32(tgt + 432), u32(tgt + 184)))
    s5 = 0
    if m.lbu(m.lw(tgt + 852) + 572) & 0x10:
        p = m.lw(tgt + 856)
        if me != p and m.lw(m.lw(p + 852) + 604) < 2:
            s5 = 1 if 0 < lat else 0
    s5 <<= 1
    if gated():
        if cat3():
            m.sw(ST, m.call(0x800C09B0, me))
        if not (s4 & 4):
            if m.call(0x800C0BE8, me, tgt, u32(along), s4 | s5) == 0:
                return None
        if not (m.lw(me + 568) & 0x600) and (m.lw(me + 560) & S7):
            m.call(0x800C0CE8, me, tgt, u32(along))
        if locked():
            return None
    if s4 == 0:
        return None
    if s4 & 4:
        skip = False
        if not m.lws(tgt + 576) < 6553:
            if m.lw(m.lw(tgt + 852) + 604) == 1 and m.lw(m.lw(m.lw(tgt + 856) + 852) + 604) == 1:
                skip = True
        if not skip and not 0xEFFFF < iabs(along):
            v = m.lw(me + 564)
            m.sw(me + 924, 0)
            m.sw(me + 564, v | 0x80000)
            m.sb(m.lw(tgt + 1084) + 39, 254)
            if m.lw(m.lw(tgt + 1084) + 40) == 0:
                if m.lw(m.lw(tgt + 852) + 604) < 2:
                    m.call(0x80092C7C, tgt, 9)
                m.sw(m.lw(tgt + 1084) + 40, m.lw(m.lw(GS) + 16))
            v = m.lw(tgt + 564)
            m.sw(tgt + 924, 0)
            m.sw(tgt + 564, v | 0x80000)
            return None
    if s5:
        lat = s32(lat - u32(m.lw(tgt + 304) + m.lw(m.lw(tgt + 856) + 304)))
    if s32(u32(m.lw(LATLIM) << 1)) < iabs(lat):
        return None
    if 0xB333 < iabs(along):
        return None
    s4 = (1 if (lat < 0 if MUT else 0 < lat) else 0) << 8
    if s5:
        tgt = m.lw(tgt + 856)
    if gated() and not cat3():
        m.call(0x800C122C, me, tgt, s4)
        s8 = 1
    if s8:
        rd = m.lw(me + 1084)
        if m.lbu(rd + 47) != 0 and (m.lbu(rd + 60) & 0x80) and m.lbs(rider + 571) != -1:
            m.sw(FL, m.lw(FL) | 0x100)
            m.sw(sp + 16, FL)
            if m.call(0x800C159C, me, tgt, u32(lat), u32(along), stack=(FL,)):
                rd = m.lw(me + 1084)
                m.sb(rd + 47, m.lbu(rd + 47) - 1)                        # swings left
                rd = m.lw(me + 1084)
                sh = (m.lbu(rd + 46) << 2) & 31
                m.sw(rd + 48, m.lw(rd + 48) & u32(~u32(15 << sh)))
                rd = m.lw(me + 1084)
                sh = (m.lbu(rd + 46) << 2) & 31
                m.sw(rd + 48, m.lw(rd + 48) | u32((m.lbu(rd + 47) & 15) << sh))
            m.sw(FL, m.lw(FL) & u32(-257))
    if m.lbu(m.lw(me + 1084) + 60) == 32:
        m.call(0x800BFF04, me, tgt, s4, FL)
    if m.lw(ST) != 0:
        m.sw(sp + 16, FL)
        if m.call(0x800C159C, me, tgt, u32(lat), u32(along), stack=(FL,)):
            m.call(0x800C17B0, me, tgt, s4, m.lw(FL))
    return None
