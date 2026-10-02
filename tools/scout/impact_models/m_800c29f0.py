# RiderRunOver(rd, e) RASHCDG 0x800C29F0, 1196 B, frame 56 (name ours; only caller BikeVsRider at 0x800AD788)
from .h_dcommon import GS, MUT, s32, u32, iabs
FRAME = 56
DOT, FIXMUL, CNT = 0x8002E698, 0x8001FC90, 0x800BFE58
ROW = 0x800D6198   # 224-byte records; +140 := 1


def _heading(m, e, get_r):
    """rider->f1C8..f1CC := bike->f360..f364 (u16), the rider pointer re-read before each store"""
    m.sh(get_r() + 456, m.lhu(e + 864))
    m.sh(get_r() + 458, m.lhu(e + 866))
    m.sh(get_r() + 460, m.lhu(e + 868))


def model(m, a0, a1, a2, a3):
    rd, e = a0, a1                                          # s3, s0
    s1 = u32(rd + 450)                                      # rd + 0x1C2
    d = s32(m.call(DOT, e + 444, s1))                       # DotLcm(bike + 0x1BC, rd + 0x1C2)
    s2 = 1 if m.lw(e + 856) != 0 else 0                     # bike->f358 != 0
    m.sw(m.sp + 20, 0)
    m.sw(m.sp + 16, 0)

    def own():
        return m.lw(e + 852)                                # e->f354

    def pas():
        return m.lw(m.lw(e + 856) + 852)                    # e->f358->f354
    if iabs(d) > 0xDDB2:
        if d > 0:
            if m.lw(own() + 604) < 2:
                _heading(m, e, own)
                v = m.call(FIXMUL, 0x13333, m.lw(rd + 480))
                m.sw(own() + 480, v)
                x = own()
                m.sw(x + 552, m.lw(x + 552) | 0x00204000)
                m.sw(m.sp + 16, e)
            if s2 and m.lw(pas() + 604) < 2:
                _heading(m, e, pas)
                v = m.call(FIXMUL, 0x13333, m.lw(rd + 480))
                m.sw(pas() + 480, v)
                x = pas()
                m.sw(x + 552, m.lw(x + 552) | 0x00204000)
                m.sw(m.sp + 20, m.lw(e + 856))
            if m.lw(e + 616) == 0:
                m.sw(e + 624, 0x002DD62D)
                m.sw(e + 632, 0)
                m.sw(e + 620, 0xFFF80000)
        else:
            v = m.lw(e + 568)
            rr = own()
            m.sw(e + 568, v | 0x880)
            if m.lw(rr + 604) < 2:
                _heading(m, e, own)
                x = own()
                m.sw(x + 552, m.lw(x + 552) | 0x00208000)
                m.sw(m.sp + 16, e)
            if s2 and m.lw(pas() + 604) < 2:
                _heading(m, e, pas)
                x = pas()
                m.sw(x + 552, m.lw(x + 552) | 0x00208000)
                m.sw(m.sp + 20, m.lw(e + 856))
    else:
        a3v = 1 if s32(m.call(DOT, e + 432, s1)) > 0 else 0    # DotLcm(bike + 0x1B0, rd + 0x1C2)
        a2v = e
        if not a3v and s2:
            a2v = m.lw(e + 856)
        x = m.lw(a2v + 852)
        if m.lw(x + 604) < 2:
            if s2:
                m.sw(x + 552, m.lw(x + 552) & 0xFFF9FFFF)
                y = m.lw(a2v + 852)
                m.sw(y + 552, m.lw(y + 552) | 0x8000 | u32((a3v << 17) + 0x20000))
            else:
                m.sw(x + 552, m.lw(x + 552) | 0x8000)
                v = u32((u32(-a3v) & 0xFFFF8000) + 0x10000)
                m.sw(a2v + 568, m.lw(a2v + 568) | 0x840 | v)
            m.sw(m.sp + 16, a2v)
    for slot in (16, 20):
        who = m.lw(m.sp + slot)
        if who:
            m.call(CNT, m.lw(rd + 596), 1, 0)
            m.call(CNT, who, 1, 1)
            gs = m.lw(GS)
            h = m.lhu(m.lw(rd + 596) + 0xAC)
            if h < m.lw(gs + 48):
                t = u32(1 - h)
                i = u32(h + (t if s32(t) < 0 else 0))
                m.sw(u32(ROW + 224 * i + 140), 1)
    rdf = m.lw(e + 1084)
    v0 = m.lbu(rdf + 36) >> (2 if MUT['on'] else 3)       # MUTATION: >> 2
    v1 = s32(m.lbu(rdf + 37) - v0)
    m.sb(rdf + 37, v1 if v1 >= 0 else 0)
    return v0
