"""0x800B3838 PropKnock(prop, bikeDir, a2, a3, [speed]) - RASHCDG, 664 B, frame 64"""
from .h_cx import *
FRAME = 64


def model(m, a0, a1, a2, a3):
    prop = a0
    sp = m.sp
    s4 = 1024
    blk(m, 0x800B3838)
    d = m.call(DOTLCM, prop + 444, a1)
    if s32(d) > 0:                                   # blez v0 not taken
        blk(m, 0x800B3878)
        for i in range(3):
            m.sh(sp + 16 + 2 * i, m.lhu(a2 + 2 * i))
        for i in range(3):
            m.sh(sp + 22 + 2 * i, m.lhu(a3 + 2 * i))
        for i in range(3):
            m.sh(sp + 28 + 2 * i, m.lhu(a1 + 2 * i))
        s4 = u32(-1024)
    else:
        blk(m, 0x800B38E8)
        for i in range(3):
            m.sh(sp + 16 + 2 * i, u32(-m.lhu(a2 + 2 * i)))
        for i in range(3):
            m.sh(sp + 28 + 2 * i, u32(-m.lhu(a1 + 2 * i)))
        for i in range(3):
            m.sh(sp + 22 + 2 * i, m.lhu(a3 + 2 * i))
    blk(m, 0x800B396C)
    r = m.call(RAND)
    q = (((r >> 1) * 0x300C0301) >> 32) >> 6                   # multu hi >> 6 = r / 682
    ang = u32(u32(r - 682 * q) - 341)
    m.call(0x8002ED94, sp + 16, sp + 28, ang)
    m.sw(prop + 592, m.lw(prop + 592) | (0x100 if mut('b3838_flag') else 0x200))                  # 0x800B39D8 (delay slot)
    m.call(0x8002ED94, sp + 22, sp + 28, u32(-s4))
    m.call(0x800716C0, sp + 16, prop + 572)
    acc = 0
    for i in range(4):
        acc += u32(m.lhs(prop + 572 + 2 * i) * m.lhs(prop + 560 + 2 * i))   # mflo, addu
    v = u32(sra(u32(acc), 14) << 2)
    f = m.lw(prop + 592) & 0xFFFFFFBF
    m.sw(prop + 592, f)                                          # 0x800B3A5C
    if s32(v) < 0:
        blk(m, 0x800B3A60)
        m.sw(prop + 592, f | 0x40)                               # 0x800B3A64
    blk(m, 0x800B3A68)
    r = m.call(FIXMUL, 6443, u32(m.arg(4) + 0xFFFEFE82))
    m.sw(prop + 484, r)                                          # 0x800B3A80
    m.sw(prop + 488, u32(r << 1))                                # 0x800B3A90
    v1 = u32(r + m.lw(prop + 556))
    m.sw(prop + 484, v1)                                         # 0x800B3AA4
    if 0xC000 < s32(v1):
        blk(m, 0x800B3AA8)
        m.sw(prop + 484, 0x10000)                                # 0x800B3AAC
        return 0x10000
    return 0
BLOCKS = [0x800B3838, 0x800B3878, 0x800B38E8, 0x800B396C, 0x800B3A60, 0x800B3A68, 0x800B3AA8]
