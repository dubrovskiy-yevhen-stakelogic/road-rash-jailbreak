"""Model of RASHCDG 0x80091468 (sha1 cfe43a77...), 4100 B (0x80091468..0x8009246C), frame 80 - the rider
fall launch (name ours, [guess]): by the rider's stance (jump table 0x8005B7A4, stance 38..91) it sets up the
thrown rider's pose frame (+0x1B0..+0x1C0), direction (+0x1C2..+0x1C6), speed (+0x1E0) and velocity
(+0x1C8..), picks the next stance, commits it through StanceEvent, then settles the rider on the ground."""
import os, sys
pass
import pairs as P
import impact as I

FRAME = 80
from ._util import MUTX as MUT
COVER = set()                       # which jump-table case / arm ran (filled while modelling)
GS = 0x8005B2F8
TBL = 0x8005624C                    # SLUS sin/cos table, 4 bytes per entry: +0 sin, +2 cos (4096 = 1.0)
KIND = 0x800541D4                   # SLUS rider state table, u16 kind at +2
VIEW, VIEWSZ = 0x800CD898, 1132     # the per-player view object
STANCE, FIXMUL, SCALE, NORM, DOT, ASIN, RAND = (0x800C4550, 0x8001FC90, 0x8002EE50, 0x8002E468, 0x8002E698,
                                                0x8001FF3C, 0x8001FC58)
POSE_A, POSE_E, POSE_B, POSE_C, POSE_D = 0x8008EE60, 0x8008E044, 0x8008E818, 0x8008E50C, 0x8008EB88
IDENT, SYNC, EVT, LAUNCH, CAMT = 0x8008CF74, 0x8008DF74, 0x8001A760, 0x8007E868, 0x800235B0
QGET, QSET, SETTLE, OBB = 0x8005C338, 0x8005C36C, 0x8009246C, 0x8008BD2C
u32, s32 = P.u32, P.s32
TABLE = 0x8005B7A4


def s16(v):
    return ((v & 0xFFFF) ^ 0x8000) - 0x8000


def mul(a, b):                      # mult ... mflo: the low word, signed
    return s32(u32(a * b))


def model(m, a0, a1, a2, a3):
    R = a0
    s8 = (m.lbu(R + 0x23C) >> 5) & 1                              # the passenger bit
    if s8:
        s5 = m.lw(m.lw(R + 0x254) + 0x358)
        m.sw(s5 + 0x2D4, 0)                                       # 0x800914C0
    else:
        s5 = m.lw(R + 0x254)
    st = {'phi': 10, 's7': 1, 'ang2': 0, 's3': 0, 's1': None}
    r228 = m.lw(R + 0x228)
    stance = m.lhu(R + 0x220)
    m.sw(R + 0x1E8, 0)                                            # 0x800914E4
    s6 = (r228 >> 27) & 1
    m.sw(R + 0x244, m.lw(0x800D3964))                             # 0x800914F8
    idx = u32(stance - 38)
    if idx < 54:
        tgt = m.lw(TABLE + 4 * idx)
    else:
        tgt = 0x8009215C
    COVER.add(tgt)
    COVER.add('%x/s6=%d/s8=%d' % (tgt, s6, s8))
    COVER.add('ra=%x' % m.regs['ra'])
    {0x80091528: case_a, 0x800918B8: case_b, 0x80091B64: case_c, 0x80091F44: case_d,
     0x800916D8: case_e}.get(tgt, case_default)(m, R, s5, s6, s8, st)
    return tail(m, R, s5, s8, st)


def gte(m, dst, rsrc, irsrc):
    r = [m.lhs(rsrc + 2 * k) for k in range(3)]
    ir = [m.lhs(irsrc + 2 * k) for k in range(3)]
    o = I.gte_op(r[0], r[1], r[2], ir[0], ir[1], ir[2])
    for k in range(3):
        m.sh(dst + 2 * k, o[k])


def load_dir(m, R, s5):
    if m.lw(R + 0x228) & 0x200000:
        v = [m.lhu(R + 0x1C8), m.lhu(R + 0x1CA), m.lhu(R + 0x1CC)]
        m.sh(R + 0x1C2, v[0]); m.sh(R + 0x1C4, v[1]); m.sh(R + 0x1C6, v[2])
    else:
        m.sh(R + 0x1C2, m.lhu(s5 + 0x1C2))
        m.sh(R + 0x1C4, m.lhu(s5 + 0x1C4))
        m.sh(R + 0x1C6, m.lhu(s5 + 0x1C6))


def rot357(m, R):                   # the passenger's direction turned by 357/4096 rad (~5 degrees)
    v0 = s32(357 * m.lhs(R + 0x1C6)) >> 12
    x = u32(m.lhu(R + 0x1C2) + v0)
    m.sh(R + 0x1C2, x)
    v0 = s32(357 * s16(x)) >> 12
    m.sh(R + 0x1C6, u32(m.lhu(R + 0x1C6) - v0))


def launch(m, R):                   # Asin(dir.y << 4) -> cos((1137 - a) & 4095) -> 0x8007E868
    a = m.call(ASIN, u32(m.lhs(R + 0x1C4) << 4))
    i = u32(1137 - a) & 0xFFF
    m.call(LAUNCH, R + 0x1C2, m.lw(R + 0x1E0), u32(m.lhs(TBL + 4 * i + 2)), 0x633B6)


def view_or(m, s5, bits, tag=''):
    h = m.lhu(s5 + 0xAC)
    if h < m.lw(m.lw(GS) + 0x30):
        COVER.add('view:' + tag)
        rec = u32(VIEW + VIEWSZ * h)
        v1 = m.lw(rec + 0x224)
        m.sw(rec + 0x224, v1 | bits)
        return True
    return False


def cam_bits(m, R):
    return 0x0E000000 if (m.lw(R + 0x228) & 0x10000) else 0x06000000


def case_default(m, R, s5, s6, s8, st):          # stances 43..87 and anything outside 38..91
    COVER.add('default-in-table' if 43 <= m.lhu(R + 0x220) <= 87 else 'default-out-of-range')
    st['s1'] = m.lhu(R + 0x220)


def case_a(m, R, s5, s6, s8, st):                # stance 38
    m.sw(R + 0x228, m.lw(R + 0x228) & 0xBFFFFFFF)                 # 0x8009153C
    a = m.lws(R + 0x1E0)
    if a < 0x30000:
        a = 0x30000
    m.sw(R + 0x1E0, a)                                           # 0x80091554
    st['s1'] = 48 if a > 0x140000 else 51
    st['s7'] |= 0x182 if (m.lw(R + 0x228) & 0x08000000) else 0x82
    COVER.add('A:s7=%x' % st['s7'])
    m.call(POSE_A, R, 0)
    load_dir(m, R, s5)
    k = 0xD999 if st['s1'] == 48 else 0xE666
    v = m.call(FIXMUL, k, m.lw(s5 + 0x1E0))
    m.sw(R + 0x1E0, v)                                           # 0x80091618 (delay slot)
    m.call(SCALE, v, R + 0x1C2, R + 0x1C8)
    a0, v1 = m.lhu(R + 0x1C2), m.lhu(R + 0x1C6)
    m.sh(R + 0x1B6, m.lhu(s5 + 0x20A))                           # 0x80091628
    m.sh(R + 0x1B8, m.lhu(s5 + 0x20C))                           # 0x80091634
    a1 = m.lhu(s5 + 0x20E)
    v0 = m.lhu(R + 0x1C4)
    m.sh(R + 0x1BC, a0); m.sh(R + 0x1C0, v1); m.sh(R + 0x1BE, v0); m.sh(R + 0x1BA, a1)
    gte(m, R + 0x1B0, R + 0x1B6, R + 0x1BC)                      # side = up x dir
    if m.call(NORM, R + 0x1B0) == 0:
        COVER.add('A:ident')
        m.call(IDENT, R)
    m.sw(R + 0x258, 0x30000)                                     # 0x800916CC
    m.sw(R + 0x1FC, 0)                                           # 0x800916D4


def case_e(m, R, s5, s6, s8, st):                # stances 42, 91
    m.sw(R + 0x228, m.lw(R + 0x228) | 0x40000000)                 # 0x800916F0
    m.call(POSE_E, R, 0)
    m.call(SCALE, m.lw(R + 0x1E0), R + 0x1C2, R + 0x1C8)
    v0 = m.lhu(s5 + 0x20A); v1 = m.lhu(R + 0x1C4); a0 = m.lhu(R + 0x1C6)
    m.sh(R + 0x1B6, v0)                                          # 0x80091714
    m.sh(R + 0x1B8, m.lhu(s5 + 0x20C))                           # 0x80091720
    st['s7'] |= 0x82
    a1 = m.lhu(s5 + 0x20E)
    v0 = m.lhu(R + 0x1C2)
    m.sh(R + 0x1BE, v1); m.sh(R + 0x1C0, a0); m.sh(R + 0x1BC, v0); m.sh(R + 0x1BA, a1)
    gte(m, R + 0x1B0, R + 0x1B6, R + 0x1BC)
    m.call(NORM, R + 0x1B0)
    gte(m, R + 0x1B6, R + 0x1BC, R + 0x1B0)
    d = m.call(DOT, R + 0x1C2, s5 + 0x1BC)
    s3 = s32(u32(1024 - s32(m.call(ASIN, d))))
    st['s3'] = s3
    if s3 < 683:
        COVER.add('E:<683')
        st['phi'] = 20
        st['s1'] = 48
        if not s6:
            st['s7'] |= 0x100
    else:
        COVER.add('E:>=683')
        st['s1'] = 46
    m.call(EVT, m.lhu(s5 + 0xAC), 1)
    if not s8:
        view_or(m, s5, cam_bits(m, R) | 0x80, 'E')


def case_b(m, R, s5, s6, s8, st):                # stances 39, 89
    st['s7'] |= u32(-s6) & 0x100
    st['s1'] = 48
    st['phi'] = 40
    m.sw(R + 0x228, m.lw(R + 0x228) | 0x40000000)                 # 0x800918E0
    if s8:
        st['phi'] = 25
    m.call(POSE_B, R, 0)
    for d, s in ((0x1B0, 0x1BC), (0x1B2, 0x1BE), (0x1B4, 0x1C0), (0x1BC, 0x1B0), (0x1BE, 0x1B2), (0x1C0, 0x1B4),
                 (0x1B6, 0x1B6), (0x1B8, 0x1B8), (0x1BA, 0x1BA)):
        m.sh(R + d, m.lhu(s5 + s))                               # 0x80091900..0x80091960
    if s6:
        a, c = m.lhu(R + 0x1B0), m.lhu(R + 0x1B4)
        m.sh(R + 0x1B0, u32(-a)); m.sh(R + 0x1B4, u32(-c)); m.sh(R + 0x1B2, u32(-m.lhu(R + 0x1B2)))
    else:
        a, c = m.lhu(R + 0x1BC), m.lhu(R + 0x1C0)
        m.sh(R + 0x1BC, u32(-a)); m.sh(R + 0x1C0, u32(-c)); m.sh(R + 0x1BE, u32(-m.lhu(R + 0x1BE)))
    st['s3'] = 1024
    if m.lw(s5 + 0x238) & 0x140:
        v = 0x50000; COVER.add('B:0x140')
    else:
        v = m.call(FIXMUL, 0x13333, m.lw(s5 + 0x1E0))
    m.sw(R + 0x1E0, v)                                           # 0x800919DC
    load_dir(m, R, s5)
    if s8:
        rot357(m, R)
    launch(m, R)
    m.call(SCALE, m.lw(R + 0x1E0), R + 0x1C2, R + 0x1C8)
    m.call(EVT, m.lhu(s5 + 0xAC), 1)
    if not s8:
        view_or(m, s5, 0x06000080, 'B')                          # 0x80091B60
    m.sw(R + 0x244, 65)                                          # 0x80091F40


def case_c(m, R, s5, s6, s8, st):                # stances 40, 88
    st['s7'] |= 0x82
    m.sw(R + 0x228, m.lw(R + 0x228) | 0x40000000)                 # 0x80091B7C
    m.call(POSE_C, R)                                             # a1 = the caller's a1, not set here
    load_dir(m, R, s5)
    if s8:
        rot357(m, R)
    if m.lw(R + 0x228) & 0x10000:
        COVER.add('C:10000')
        m.call(LAUNCH, R + 0x1C2, 0, u32(-2868), 0)
        v = m.lws(R + 0x1E0)
        q = s32(u32(((v * 0x55555556) >> 32) - (v >> 31)))           # v / 3, truncated toward 0
        m.sw(R + 0x1E0, q)                                        # 0x80091C8C
        if q < 0x50000:
            q = 0x50000
        m.sw(R + 0x1E0, q)                                        # 0x80091C98
    else:
        COVER.add('C:plain')
        launch(m, R)
        d = m.call(DOT, R + 0x1BC, R + 0x1C2)
        st['ang2'] = s32(u32(s32(m.call(ASIN, d)) - 1024))
    m.sh(R + 0x1B6, m.lhu(s5 + 0x20A)); m.sh(R + 0x1B8, m.lhu(s5 + 0x20C)); m.sh(R + 0x1BA, m.lhu(s5 + 0x20E))
    gte(m, R + 0x1B0, R + 0x1B6, R + 0x1C2)
    if m.call(NORM, R + 0x1B0) == 0:
        COVER.add('C:ident')
        m.call(IDENT, R)
    gte(m, R + 0x1BC, R + 0x1B0, R + 0x1B6)
    m.call(SCALE, m.lw(R + 0x1E0), R + 0x1C2, R + 0x1C8)
    o = m.lw(s5 + 0x354)
    s0 = -1
    if m.lbu(o + 0x23C) & 0x10:
        sid = m.lhu(o + 0x220) if s8 else m.lhu(m.lw(m.lw(s5 + 0x358) + 0x354) + 0x220)
        if m.lhu(u32(KIND + 8 * sid + 2)) == 6:
            s0 = sid
    while True:
        r = u32(m.call(RAND))
        k = r % 3
        s1 = 49 if k == 0 else 47 if k == 1 else 46
        if s1 != s0:
            break
        COVER.add('C:reroll')
    st['s1'] = s1
    COVER.add('C:%d' % s1)
    if s1 == 47:
        st['phi'] = 20
    m.call(EVT, m.lhu(s5 + 0xAC), 1)
    if not s8:
        view_or(m, s5, cam_bits(m, R) | 0x80, 'C')
    m.sw(R + 0x244, 65)                                          # 0x80091F40


def case_d(m, R, s5, s6, s8, st):                # stances 41, 90
    st['s7'] |= 0x82
    st['s1'] = 43
    m.sw(R + 0x228, m.lw(R + 0x228) | 0x40000000)                 # 0x80091F64
    m.call(POSE_D, R, 0)
    load_dir(m, R, s5)
    v = [m.lhu(R + 0x1C2), m.lhu(R + 0x1C4), m.lhu(R + 0x1C6)]
    m.sh(R + 0x1BC, v[0]); m.sh(R + 0x1BE, v[1]); m.sh(R + 0x1C0, v[2])
    a1 = m.lws(s5 + 0x1E0)
    if 0x50000 < a1:
        COVER.add('D:fast')
        m.sw(R + 0x1E0, m.call(FIXMUL, 0x8000, a1))               # 0x80091FF4
    else:
        COVER.add('D:slow')
        x, z = m.lhu(R + 0x1C2), m.lhu(R + 0x1C6)
        m.sh(R + 0x1C2, u32(-x)); m.sh(R + 0x1C6, u32(-z)); m.sh(R + 0x1C4, u32(-m.lhu(R + 0x1C4)))
        m.sw(R + 0x1E0, u32(0x50000 - m.lw(s5 + 0x1E0)))          # 0x80092028
    m.call(SCALE, m.lw(R + 0x1E0), R + 0x1C2, R + 0x1C8)
    v1, v0 = m.lhu(R + 0x1C0), m.lhu(R + 0x1BC)
    m.sh(R + 0x1B2, 0); m.sh(R + 0x1B0, v1); m.sh(R + 0x1B4, u32(-v0))
    if m.call(NORM, R + 0x1B0) == 0:
        COVER.add('D:ident')
        m.call(IDENT, R)
    gte(m, R + 0x1B6, R + 0x1BC, R + 0x1B0)
    m.call(EVT, m.lhu(s5 + 0xAC), 1)
    if not s8 and m.lhu(s5 + 0xAC) < m.lw(m.lw(GS) + 0x30) and (m.lw(s5 + 0x238) & 0x200):
        view_or(m, s5, cam_bits(m, R), 'D')                      # 0x80092158


def qrot(m, R, ang, second):
    anim = m.lw(R + 0x21C)
    m.call(QGET, anim, m.sp + 16)
    a = s32(ang)
    h = ((a + (1 if a < 0 else 0)) >> 1) & 0xFFF
    c = s32(u32(m.lhs(TBL + 4 * h + 2) << 18)) >> 16
    s = s32(u32(m.lhs(TBL + 4 * h) << 18)) >> 16
    q0, q1, q2, q3 = (m.lhs(m.sp + 16 + 2 * k) for k in range(4))
    if not second:          # 0x800921A8..0x800922BC
        o30 = (mul(c, q3) >> 14) - (mul(s, q1) >> 14)
        o26 = (mul(c, q1) >> 14) + (mul(s, q3) >> 14)
        o28 = (mul(c, q2) >> 14) - (mul(s, q0) >> 14)
        o24 = (mul(c, q0) >> 14) + (mul(s, q2) >> 14)
    else:                   # 0x800922DC..0x800923F0
        o30 = (mul(c, q3) >> 14) - (mul(s, q0) >> 14)
        o26 = (mul(c, q1) >> 14) - (mul(s, q2) >> 14)
        o28 = (mul(c, q2) >> 14) + (mul(s, q1) >> 14)
        o24 = (mul(c, q0) >> 14) + (mul(s, q3) >> 14)
    m.sh(m.sp + 30, u32(o30)); m.sh(m.sp + 26, u32(o26)); m.sh(m.sp + 28, u32(o28)); m.sh(m.sp + 24, u32(o24))
    m.call(QSET, m.lw(R + 0x21C), m.sp + 24)


def tail(m, R, s5, s8, st):
    m.call(SYNC, R)                                              # 0x80092160
    if not s8:
        h = m.lhu(s5 + 0xAC)
        if h < m.lw(m.lw(GS) + 0x30):
            m.call(CAMT, h, R)
    if st['s3'] != 0:
        COVER.add('rot1')
        qrot(m, R, st['s3'], False)
    if st['ang2'] != 0:
        COVER.add('rot2')
        qrot(m, R, st['ang2'], True)
    p = u32((st['s7'] | 0x800) | (st['phi'] << 16))
    if False:
        p ^= 0x800                  # second control: only the entry-probe argument check can see this
    m.call(STANCE, st['s1'] & 0xFFFF, R, p)                       # 0x80092408
    m.call(SETTLE, R)
    m.call(OBB, R)
    m.sb(R + 0x217, 0)                                           # 0x8009242C
    v = (m.lw(R + 0x228) | 1) & (0xFFD7FFFF if MUT else 0xFFD8FFFF)      # MUT: keep bit 16 (0x10000)
    m.sw(R + 0x228, v)                                           # 0x80092438
    return v
