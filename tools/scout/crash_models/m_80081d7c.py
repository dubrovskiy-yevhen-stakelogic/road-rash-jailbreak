"""RASHCDG 0x80081D7C (sha1 cfe43a7786759f2cb9c57751cf99e84d1074782c), 6888 bytes (0x80081D7C..0x80083864),
1722 instructions, frame 280, callee-saved s0..s8 + ra at sp+240..276:
    s32 BikeBikeGate(Bike *a, Bike *b, u32 codeA, u32 codeB, [entry+16] s16 n[3])  -> 0 / 1 / class bits
One caller: BikeBikeReact 0x800AC130 at 0x800AC2E4 (n = the caller's sp+24 face normal; n is NEGATED in place on
some arms, those writes land in the caller's frame and are compared).
The bike-bike analogue of HitOutcome 0x80083928: latch +0x35C/+0x360 (flagsC bit 25) on BOTH bikes, the crashed /
airborne arms (Bounce 0x80084564, Spin 0x800849D8, ImpactTurn 0x80083F30, a knock-on crash + SLUS 0x8001A760),
else classify the hit per bike into sp+184 / sp+188 (-1 exchange, 1, 2, 4, 8, 16, 32), the momentum exchange
(MassExchange 0x80083864) and per bike HitSpeed 0x80080D1C (class < 16) or TakePartnerHeading 0x80080B10.
Frame map: sp+48 bike[2] (swappable), sp+56 s32[3] momentum, sp+72 rel velocity a-b, sp+88 pos delta, sp+104 ang[2],
sp+112 lim[2], sp+120 closing[2], sp+128 speed out[2], sp+136 a9[2] (1 deg / 10 deg / 0), sp+144 s16 nrm0[3],
sp+150 s16 nrm1[3], sp+160 &dir[2] (+0x360), sp+168 &pspeed[2] (+0x35C), sp+176/180 bike order, sp+184/188
class[2], sp+216 forced exchange, sp+220 main-path flag, sp+224 knock-on crash index.
sp+108 / sp+116 are NEVER written: the ANGLED arm reads them for bike index 1 (uninitialised stack)."""
import os
import pairs as P, impact as I
from impact_models.h_cx import MH

u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 280
NAME = "BikeBikeGate"

FIXMUL, FIXDIV, DOT, SCALE, ATAN2 = 0x8001FC90, 0x80010028, 0x8002E698, 0x8002EE50, 0x80020018
LEN3, MULADD32, SCALETO16 = 0x8002E548, 0x8002E570, 0x8002EED8
BOUNCE, SPIN, TURN, MASSX = 0x80084564, 0x800849D8, 0x80083F30, 0x80083864
HITSPEED, TAKEHD, CRASHEV = 0x80080D1C, 0x80080B10, 0x8001A760
GS = 0x8005B2F8
HALF_PI = 0x1921F


def _stk(tr):
    """the guest's stack stores of the trace, {(addr, width): [(seq, value)]} (built once per trace)"""
    if not hasattr(tr, "_bstk"):
        d = {}
        for sq, pc, a, n, v in tr.writes:
            if 0x801FE000 <= a < 0x80200000:
                d.setdefault((a, n), []).append((sq, pc, v))
        tr._bstk = d
    return tr._bstk


def own(m, addr, n):
    """OWN-FRAME CHECK (the harness ignores own-frame stores): the model's n-byte value at addr must equal the
    guest's at the moment of the guest's next call - its last store there (sh, or the covering sw; a store in that
    jal's delay slot counts: calls.bin stamps the jal before its delay slot), or the RAM the call started with. Used where an own-frame value reaches a callee only by pointer or through a hot leaf's
    uncompared argument: the GTE OP result, the halved momentum, the closing speeds, the per-bike normals."""
    if m.ci >= len(m.gcalls):
        raise P.Mismatch(f"own-frame check at {addr:#x}: no guest call left")
    seq, frm = m.gcalls[m.ci][0], m.gcalls[m.ci][1]
    idx, best = _stk(m.tr), None
    for key, sh in (((addr, n), 0), ((addr & ~3, 4), (addr & 3) * 8)):
        for sq, pc, v in idx.get(key, ()):
            if (sq < seq or (pc == frm + 4 and sq < seq + 8)) and (best is None or sq > best[0]):
                best = (sq, (v >> sh) & ((1 << (8 * n)) - 1))
    g = best[1] if best else int.from_bytes(m.tr.ram0[addr & 0x1FFFFF:(addr & 0x1FFFFF) + n], "little")
    mv = m.lw(addr) if n == 4 else m.lhu(addr)
    if g != mv:
        raise P.Mismatch(f"own frame {addr - m.sp:+#x}: model {mv:#x} guest {g:#x} (before call #{m.ci + 1})")


def mut():
    return bool(P.MUTATE["on"])


def rad(ang):
    """(ang * 25736) >> 8 (sll/addu chain, 32-bit wrap, sra)"""
    return s32(u32(s32(ang) * 25736)) >> 8


def dot16(m, v, h):
    """sum MH(v[k], lh(h + 2k)), addu wrap (order irrelevant mod 2^32)"""
    return s32(u32(sum(MH(v[k], m.lhu(h + 2 * k)) for k in range(3))))


def neg3(m, p):
    """lhu p0, lhu p2; sh -p0; lhu p1; sh -p2; sh -p1  (store order 0, 4, 2)"""
    x0, x2 = m.lhu(p), m.lhu(p + 4)
    m.sh(p, -x0)
    x1 = m.lhu(p + 2)
    m.sh(p + 4, -x2)
    m.sh(p + 2, -x1)


def divu(n, d):
    """R3000 divu LO; divide by 0 -> 0xFFFFFFFF"""
    d = u32(d)
    return 0xFFFFFFFF if d == 0 else u32(n) // d


def angle(m, sp, k, dirp, e, nrm):
    """0x800833xx / 0x800825F4: s = pi/2 -+ rad(RatAtan2(Dot(e+0x32E, nrm), -Dot(dir, nrm))); lim by the sign of
    s ^ e+0x1E8: 65 deg (0x1226C) if opposite, else 55 deg (0xF5BE)"""
    s0 = s32(m.call(DOT, dirp, nrm))
    v = s32(m.call(DOT, e + 814, nrm))
    t = rad(m.call(ATAN2, v, u32(-s0)))
    m.sw(sp + 104 + 4 * k, t)
    s = s32(u32((HALF_PI if t > 0 else -HALF_PI) - t))
    m.sw(sp + 104 + 4 * k, s)
    lim = 0x1226C if (s ^ m.lws(e + 488)) < 0 else 0xF5BE
    m.sw(sp + 112 + 4 * k, lim)
    return s, lim


def faces(code):
    """sp+192..200 (codeA) / sp+204..212 (codeB): (is200, X, Y, Z)
    code & 0x200: face f = code & 0xFF, X = f == 3, Y = f == 1, Z = f == 2
    else:          c = code & 0xFF, X = bit 1, Y = c < 8 && !bit 1, Z = bit 0 ^ bit 1"""
    c = code & 0xFF
    if code & 0x200:
        return 1, int(c == 3), int(c == 1), int(c == 2)
    return 0, (c >> 1) & 1, int(c < 8 and not (code & 2)), (code & 1) ^ ((code & 2) >> 1)


def model(m, a, b, codeA, codeB):
    E, sp = m.entry_sp, m.sp
    n = m.lw(E + 16)                                      # s7
    m.sw(E + 12, codeB)                                   # 0x80081D94 home slots (the caller's frame)
    m.sw(E + 8, codeA)                                    # 0x80081D9C
    m.sw(E + 4, b)                                        # 0x80081DB0
    m.sw(sp + 216, 0)
    m.sw(sp + 48, a)
    m.sw(sp + 52, b)
    for k, e in enumerate((a, b)):                        # latch the pre-contact heading / speed, both bikes
        m.sw(e + 564, m.lw(e + 564) & 0xFFFFBFFF)         # 0x80081DF8 flagsB &= ~0x4000
        if not (m.lw(e + 568) & 0x02000000):
            for i in range(3):
                m.sh(e + 864 + 2 * i, m.lhu(e + 450 + 2 * i))      # 0x80081E20/34/48 +0x360 = +0x1C2
            m.sw(e + 860, m.lw(e + 480))                  # 0x80081E5C +0x35C = +0x1E0
            m.sw(e + 568, m.lw(e + 568) | 0x02000000)     # 0x80081E74 flagsC bit 25
        m.sw(sp + 160 + 4 * k, e + 864)
        m.sw(sp + 168 + 4 * k, e + 860)
    D = lambda k: m.lw(sp + 160 + 4 * k)
    V = lambda k: m.lw(sp + 168 + 4 * k)
    BK = lambda k: m.lw(sp + 48 + 4 * k)
    npl = lambda: m.lw(m.lw(GS) + 48)
    m.sw(sp + 220, 1)
    s8 = 0
    if m.lhu(a + 172) < npl():
        s8 = int(m.lhu(b + 172) < npl())                  # both bikes are players
    fa = m.lw(a + 568)
    m.sw(sp + 224, u32(-1))

    def bounce(k, nrm, k5):
        v = m.call(FIXMUL, 0xCCCC, m.lw(V(k)))           # 0.8 x the pre-contact speed
        m.sw(sp + 16, k5)
        m.sw(sp + 20, v)
        m.sw(sp + 24, 0)
        return s32(m.call(BOUNCE, nrm, D(k), V(k), BK(k) + 456, stack=(k5, v, 0)))

    if (fa & 0x200) or (m.lw(b + 568) & 0x200):
        # ---- 0x80081F18: a crashed bike (flagsC 0x200) ----
        m.sw(sp + 220, 0)
        if fa & 0x200 and (m.lw(b + 568) & 0x200):
            # both crashed: Bounce + Spin each, n negated in between
            if bounce(0, n, 5) >= 0:
                m.call(SPIN, a)
            neg3(m, n)                                    # 0x80081F98/A4/AC
            if bounce(1, n, 5) >= 0:
                m.call(SPIN, b)
        else:
            if fa & 0x200:
                c, o = 0, 1                               # 0x80082004
            else:
                c, o = 1, 0                               # 0x80082010
                neg3(m, n)                                # 0x80082024/30/38
            C, O = BK(c), BK(o)
            if bounce(c, n, 5) >= 0:
                m.call(SPIN, C)
            fo = m.lw(O + 568)
            if fo & 0x400:
                # ---- 0x800820C8: the other bike is airborne ----
                d = [s32(u32(m.lw(C + 184 + 4 * i) - m.lw(O + 184 + 4 * i))) for i in range(3)]
                for i in range(3):
                    m.sw(sp + 88 + 4 * i, d[i])
                if dot16(m, d, C + 450) < 0 and not (m.lw(C + 560) & 0x20000000):
                    # O ahead of C along C's heading: O is thrown (0x200 | 0x800), C's heading, >= 25.0
                    m.sw(O + 568, m.lw(O + 568) & 0xFFFFFBFF)                 # 0x800821DC
                    m.sw(O + 568, m.lw(O + 568) | 0xA00)                      # 0x800821F4
                    for i in range(3):
                        m.sh(O + 450 + 2 * i, m.lhu(C + 450 + 2 * i))        # 0x80082208/1C/30
                    v = s32(m.call(FIXMUL, 0xCCCC, m.lw(C + 480)))
                    m.sw(sp + 128, v)
                    if v < 0x190000:
                        v = 0x190000                      # at least 25.0
                    m.sw(O + 480, v)                      # 0x80082268 +0x1E0
                    m.sh(O + 796, m.lhu(O + 454))         # 0x8008227C +0x31C = +0x1C6
                    m.sh(O + 798, 0)                      # 0x80082288 +0x31E
                    m.sh(O + 800, -m.lhu(O + 450))        # 0x800822A0 +0x320 = -+0x1C2
                    m.sw(O + 488, 0x50000)                # 0x800822AC +0x1E8 = 5.0
                    m.sw(O + 728, 0)                      # 0x800822B8 +0x2D8
                else:
                    neg3(m, n)                            # 0x800822D0/DC/E8
                    m.call(TURN, O, C + 172, n, 5)
            elif not (fo & 0x7FF) and (s8 or not (m.lhu(O + 172) < npl())) \
                    and not (m.lw(O + 560) & 0x20000000):
                m.sw(O + 568, fo | 0x200840)              # 0x80082358 the knock-on crash
                m.sw(sp + 224, o)
    elif (fa & 0x400) or (m.lw(b + 568) & 0x400):
        # ---- 0x80082374: one of them airborne ----
        d = [s32(u32(m.lw(b + 184 + 4 * i) - m.lw(a + 184 + 4 * i))) for i in range(3)]
        for i in range(3):
            m.sw(sp + 88 + 4 * i, d[i])
        dt = dot16(m, d, a + 438)                         # along a's +0x1B6 row
        h = m.lw(a + 312)
        half = s32(u32(h + (h >> 31))) >> 1               # +0x138 / 2 (srl 31; addu; sra 1)
        if half < iabs(dt):
            lo, up = (1, 0) if dt <= 0 else (0, 1)
            U, L = BK(up), BK(lo)
            fu = m.lw(U + 568)
            if fu & 0x400:
                bounce(up, L + 522, 0)                    # onto L's +0x20A, no Spin
            elif not (fu & 0x7FF) and (s8 or not (m.lhu(U + 172) < npl())) \
                    and not (m.lw(U + 560) & 0x20000000):
                m.sw(sp + 224, up)
                m.sw(U + 568, fu | 0x200840)              # 0x80082544
                m.sw(L + 828, U + 172)                    # 0x8008255C L.+0x33C = &U.shape
            m.sw(sp + 220, 0)

    if not m.lw(sp + 220):
        # ---- 0x80083804 ----
        m.sw(sp + 184, 1)
        w = m.lws(sp + 224)
        if w >= 0:
            m.call(CRASHEV, m.lhu(BK(w) + 172), 1)
        return m.lw(sp + 184)

    # ---- 0x80082570: the main path ----
    m.sw(sp + 140, 1143)
    m.sw(sp + 136, 1143)                                  # 1 degree
    m.call(SCALE, m.lw(V(0)), D(0), a + 456)
    m.call(SCALE, m.lw(V(1)), D(1), b + 456)
    rel = [s32(u32(m.lw(a + 456 + 4 * i) - m.lw(b + 456 + 4 * i))) for i in range(3)]
    for i in range(3):
        m.sw(sp + 72 + 4 * i, rel[i])
    angle(m, sp, 0, D(0), a, n)
    sA, XA, YA, ZA = faces(codeA)
    sB, XB, YB, ZB = faces(codeB)
    m.sw(sp + 188, 0)
    m.sw(sp + 184, 0)
    cond = (XA | YA) & (XB | YB) & (1 - (YB & YA))

    def spd(k):
        return m.lws(V(k))

    def cls_at(k):
        return m.lws(sp + 184 + 4 * k)

    if cond:
        head = False
        if XA & YB:
            b0, b1, d0, d1, v0, v1 = BK(0), BK(1), D(0), D(1), V(0), V(1)
            m.sw(sp + 48, b1)
            m.sw(sp + 52, b0)
            m.sw(sp + 160, d1)
            m.sw(sp + 164, d0)
            m.sw(sp + 168, v1)
            m.sw(sp + 172, v0)
        elif not (YA & XB):
            head = True
        if not head:
            # ---- 0x8008281C REAR: bike 0 hit on its face 1 (Y), bike 1 on its face 3 (X) ----
            # REAR never writes cl[0] / cl[1] (sp+120 / +124) and the JOIN still reads them: with
            # RRJB_CRASH_STALE set, print what the live game left there.
            if os.environ.get("RRJB_CRASH_STALE"):
                print(f"      REAR stale cl words: sp+120 {m.lw(sp + 120):#010x} sp+124 {m.lw(sp + 124):#010x}")
            if spd(0) < spd(1):
                h0 = [m.lhu(D(0) + 2 * i) for i in range(3)]
                h1 = [m.lhu(D(1) + 2 * i) for i in range(3)]
                for i in range(3):
                    m.sh(sp + 150 + 2 * i, -h0[i])
                    m.sh(sp + 144 + 2 * i, -h1[i])
                m.sw(sp + 216, 1)
                m.sw(sp + 188, u32(-1))
                m.sw(sp + 184, u32(-1))
        else:
            # ---- 0x800828BC HEAD: both on face 3 (X) ----
            t6, t5, s0, s1 = D(1), D(0), V(1), V(0)
            p = dot16(m, rel, t6)
            q = s32(u32(-dot16(m, rel, t5)))
            m.sw(sp + 120, p)
            m.sw(sp + 124, q)
            t4 = s32(u32(m.lw(s0) - m.lw(s1)))
            t1 = 0
            if t4 < -0xA0000:                             # bike 0 faster by more than 10.0: swap
                t1 = 1
                bb0, bb1 = BK(0), BK(1)
                m.sw(sp + 160, t6)
                m.sw(sp + 164, t5)
                m.sw(sp + 168, s0)
                m.sw(sp + 172, s1)
                m.sw(sp + 120, q)
                m.sw(sp + 124, p)
                m.sw(sp + 48, bb1)
                m.sw(sp + 52, bb0)
            elif 0xA0000 < t4:
                t1 = 1
            for i in range(3):
                m.sh(sp + 150 + 2 * i, m.lhu(D(0) + 2 * i))
            for i in range(3):
                m.sh(sp + 144 + 2 * i, m.lhu(D(1) + 2 * i))
            for k in range(t1):
                hard = m.lws(sp + 120 + 4 * k) < -0x165A1C and not (m.lw(BK(k) + 560) & 0x20000000)
                m.sw(sp + 184 + 4 * k, 32 if hard else 16)
            for k in range(t1, 2):
                v = m.lws(sp + 120 + 4 * k)
                a1 = v < -0x312CA5                        # -49.17
                v1 = int(not a1) if v < -0x1AD288 else 0  # -26.82
                m.sw(sp + 184 + 4 * k, (v1 << 1) + (u32(-int(a1)) & 6) + 2)   # 2 / 4 / 8
                if m.lw(BK(k) + 560) & 0x20000000:
                    m.sw(sp + 184 + 4 * k, 2)
    elif m.lws(sp + 112) < iabs(m.lws(sp + 104)):
        # ---- 0x80082BDC ANGLED: the hit angle at a beyond its limit ----
        for i in range(3):
            m.sh(sp + 144 + 2 * i, m.lhu(n + 2 * i))
        for i in range(3):
            m.sh(sp + 150 + 2 * i, -m.lhu(n + 2 * i))
        dt = dot16(m, rel, n)
        m.sw(sp + 120, dt)
        m.sw(sp + 124, -dt)
        if dt > 0 or 0xD6944 < s32(u32(spd(1) - spd(0))):     # 13.41
            t1, o = 1, 0
        else:
            t1, o = 0, 1
        c = 32 if m.lws(sp + 120 + 4 * t1) < -0x165A1C else 16  # -22.35
        m.sw(sp + 184 + 4 * o, c)
        if m.lw(BK(o) + 560) & 0x20000000:
            m.sw(sp + 184 + 4 * o, 16)
        S = m.lws(sp + 104 + 4 * t1)                      # t1 = 1: sp+108, never written (stack garbage)
        L = m.lws(sp + 112 + 4 * t1)                      # t1 = 1: sp+116, never written
        beyond = S < s32(u32(-L)) or L < S
        v = m.lws(sp + 120 + 4 * t1)
        if not beyond:
            c = 1
        elif v < -0x1AD288:
            c = 8 if v < -0x312CA5 else 4
        else:
            c = 0
        m.sw(sp + 184 + 4 * t1, c)
        if m.lw(BK(t1) + 560) & 0x20000000 and c != 1:
            m.sw(sp + 184 + 4 * t1, 0)
        if cls_at(t1) == 0:
            m.sw(sp + 184 + 4 * t1, u32(-1))
    else:
        # ---- 0x80082E74 SIDE ----
        bb = m.lw(E + 4)
        if m.lw(a + 828) == u32(bb + 172) or m.lw(bb + 828) == u32(a + 172):
            pass                                          # the same partner as last time: nothing
        else:
            if 0x10000 < spd(0) or 0x10000 < spd(1):
                n0, n1 = m.lws(BK(1) + 316), m.lws(BK(0) + 316)
                if n0 > 0:
                    q = s32(m.call(FIXDIV, n0, n1)) if n1 > 0 else -s32(m.call(FIXDIV, n0, u32(-n1)))
                else:
                    q = s32(m.call(FIXDIV, u32(-n0), u32(-n1))) if n1 <= 0 else -s32(m.call(FIXDIV, u32(-n0), n1))
                m.call(MULADD32, BK(0) + 456, BK(1) + 456, u32(q), sp + 56)    # a.v + (mb/ma) b.v
                t0 = 0x5A8000                             # 90.5
                while any(iabs(m.lw(sp + 56 + 4 * i)) > t0 for i in range(3)):
                    w = [m.lws(sp + 56 + 4 * i) >> 1 for i in range(3)]
                    for i in range(3):
                        m.sw(sp + 56 + 4 * i, w[i])
                s0 = sp + 56
                for i in range(3):
                    own(m, sp + 56 + 4 * i, 4)                # the halved momentum
                if s32(m.call(LEN3, s0)) >= 0:
                    l1 = s32(m.call(LEN3, s0))
                    l2 = s32(m.call(LEN3, s0))
                    dv = s32(u32((l1 >> 1) + (s32(u32(l2 - 2)) >> 31)))
                    inv = divu(0x80000000, dv)
                else:
                    l1 = s32(m.call(LEN3, s0))
                    l2 = s32(m.call(LEN3, s0))
                    dv = s32(u32((s32(u32(-l1)) >> 1) + (s32(u32(-l2 - 2)) >> 31)))
                    inv = u32(-divu(0x80000000, dv))
                m.call(SCALETO16, inv, s0, sp + 144)       # s16 unit momentum direction
                r = [m.lhs(sp + 144 + 2 * i) for i in range(3)]
                ir = [m.lhs(a + 522 + 2 * i) for i in range(3)]
                o3 = I.gte_op(r[0], r[1], r[2], ir[0], ir[1], ir[2])   # 0x800830F0 OP sf=1 (u x a.up)
                for i in range(3):
                    m.sh(sp + 150 + 2 * i, o3[i])
                for i in range(3):
                    m.sh(sp + 144 + 2 * i, m.lhu(sp + 150 + 2 * i))
                for i in range(6):
                    own(m, sp + 144 + 2 * i, 2)               # the OP result, in both halves
                if s32(m.call(DOT, sp + 144, D(0))) > 0:
                    neg3(m, sp + 144)
                else:
                    neg3(m, sp + 150)
            else:
                # ---- 0x8008318C both at most 1.0: each bike's own +0x32E (negated by Z) ----
                for k, (e, z) in enumerate(((a, ZA), (bb, ZB))):
                    w = [m.lhu(BK(k) + 814 + 2 * i) for i in range(3)]
                    for i in range(3):
                        m.sh(sp + 144 + 6 * k + 2 * i, w[i])
                    if z:
                        for i in range(3):
                            m.sh(sp + 144 + 6 * k + 2 * i, -w[i])
            m.sw(sp + 120, dot16(m, rel, sp + 144))
            m.sw(sp + 188, 1)
            m.sw(sp + 184, 1)
            m.sw(sp + 124, -dot16(m, rel, sp + 150))
            if sA and sB:
                m.sw(sp + 140, 0)
                m.sw(sp + 136, 0)

    # ---- 0x80083358 JOIN ----
    if cls_at(0) == 0 and cls_at(1) == 0:
        return m.lw(sp + 184)
    for k in range(2):
        angle(m, sp, k, D(k), BK(k), sp + 144 + 6 * k)
        if m.lw(BK(k) + 568) & 0x100:
            m.sw(sp + 184 + 4 * k, 16)
    if cls_at(0) < 16 and cls_at(1) < 16 and not m.lw(sp + 216):
        m.sw(sp + 176, 0)
        m.sw(sp + 180, 1)
        m.sw(sp + 128, m.lw(V(0)))
        m.sw(sp + 132, m.lw(V(1)))
    else:
        # ---- 0x800834A8 the momentum exchange ----
        i = int(not (cls_at(1) < 16))
        j = int(i == 0)
        m.sw(sp + 176, i)
        m.sw(sp + 180, j)
        M = s32(u32(m.lw(BK(0) + 316) + m.lw(BK(1) + 316)))
        inv = divu(0x80000000, (M >> 1) + (s32(u32(M - 2)) >> 31))
        c = m.call(DOT, D(0), D(1))
        s2 = s32(m.call(FIXMUL, m.lw(V(i)), c))
        mi = m.call(FIXMUL, m.lw(BK(i) + 316), inv)
        mj = m.call(FIXMUL, m.lw(BK(j) + 316), inv)
        if s2 < 0:
            s2 = 0
        st = (sp + 128 + 4 * i, sp + 128 + 4 * j)
        m.sw(sp + 16, st[0])
        m.sw(sp + 20, st[1])
        m.call(MASSX, mi, mj, s2, m.lw(V(j)), stack=st)
    for step in range(2):
        s3 = m.lw(sp + 176 + 4 * step)
        o = int(s3 == 0)
        s5 = BK(s3)
        part = BK(o)
        m.sw(E + 4, part)                                 # 0x8008362C (delay slot) home slot 1 := the partner
        if m.lw(s5 + 568) & 0x80:
            m.sw(sp + 136 + 4 * s3, 11438)                # 10 degrees
            m.sw(s5 + 560, m.lw(s5 + 560) | 0x04000000)   # 0x80083648 flagsA bit 26
        c = cls_at(s3)
        if c == 0:
            continue
        f = m.lw(s5 + 568)
        if f & 0xF:
            f &= 0xFFF007F0
            m.sw(s5 + 568, f)                             # 0x80083684
            m.sw(s5 + 828, 0)                             # 0x8008368C +0x33C
            m.sw(s5 + 568, f | 0x08000000)                # 0x80083690
        if c == -1:
            m.sw(s5 + 564, m.lw(s5 + 564) | 0x4000)       # 0x800836B4 flagsB bit 14: the exchange
        else:
            m.sw(s5 + 568, m.lw(s5 + 568) | u32(c))       # 0x800836C4 THE CLASS
        if c < 16:
            own(m, sp + 120 + 4 * s3, 4)                  # the closing speed (FixMul's a0, a load before the jal)
            v = m.call(FIXMUL, m.lw(sp + 120 + 4 * s3), 2443 + (1 if mut() else 0))
            st = (sp + 144 + 6 * s3, v, m.lw(sp + 104 + 4 * s3), m.lw(sp + 112 + 4 * s3),
                  m.lw(sp + 128 + 4 * s3), m.lw(sp + 136 + 4 * s3), 0)
            for q, x in enumerate(st):
                m.sw(sp + 16 + 4 * q, x)
            for i in range(3):
                own(m, sp + 144 + 6 * s3 + 2 * i, 2)      # the normal HitSpeed reads through its pointer
            r = m.call(HITSPEED, s5, D(s3), V(s3), m.lw(E + 4) + 172, stack=st)
        else:
            m.sw(sp + 16, m.lw(sp + 128 + 4 * s3))
            r = m.call(TAKEHD, s5, D(s3), V(s3), D(o), stack=(m.lw(sp + 128 + 4 * s3),))
        m.sw(sp + 184 + 4 * s3, r)
        f = m.lw(s5 + 568)
        bit = 0x1000 if f & 0x1F else (0x800 if f & 0x220 else 0)
        m.sw(s5 + 568, m.lw(s5 + 568) | bit)              # 0x800837D8
    m.sw(sp + 184, m.lw(sp + 184) | m.lw(sp + 188))
    return m.lw(sp + 184)
