"""The verified models of the pair responses, resolvers and wall contact, one per guest function (RASHCDG).

Each is an executable transcription, run by tools\\scout\\pairs.py against the original in the
interpreter exactly like the models in pairs.py itself: callees are not modelled, their recorded
effects are replayed. Assembled from the per-function readings; top-level names are prefixed by
the entry address so the functions can share this module. Do not edit by hand."""

def u32(v):
    return v & 0xFFFFFFFF


def s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


# ---- shared helpers of the rider / point / box / pole models
"""Shared helpers of the rider / point / box / pole models. Register-level idioms only;
every callee goes through m.call."""

D_SINCOS = 0x8005624C          # SLUS table {s16 sin, s16 cos} x 4096
D_GAME = 0x8005B2F8            # -> game state (+0x30 numPlayers, +0x39 u8 mode)
D_DT = 0x800CCE38              # s32 dt of the pass
D_CREC = 0x800CCE48            # ContactRec[8] x 36 B
D_CCOUNT = 0x800CCF68          # s32 count
D_POOLTAB = 0x800CE4D0         # {base, stride, &live, &last} x 16 B

D_FIXMUL, D_FIXDIV, D_DOTLCM, D_MULADD, D_SCALE, D_RATATAN2 = 0x8001FC90, 0x80010028, 0x8002E698, 0x8002EAD8, 0x8002EE50, 0x80020018
D_STALE, D_INBAND, D_APPLY, D_AAD30, D_AA140, D_AF224, D_B675C = 0x800A8FE8, 0x800A8C78, 0x800A8DF0, 0x800AAD30, 0x800AA140, 0x800AF224, 0x800B675C
D_AIPROJ, D_MEMSET32, D_MEMCPY32, D_SOUND3D = 0x800B6AAC, 0x8001E100, 0x8001E0B4, 0x80017BA0


def D_lhs(m, a):
    return m.lhs(a)


def D_sticky(m, e, shape):
    """s = (e->f340 == shape) ? (u8 e[0x235]) >> 7 : 0  (bit 15 of flagsB)"""
    if m.lw(e + 0x340) == u32(shape):
        return m.lbu(e + 0x235) >> 7
    return 0


def D_rebuild_heading(m, e):
    """e->f124 = RatAtan2(e->f1C2 << 4, e->f1C6 << 4); f128 = cos<<4 (table +2); f12C = sin<<4 (table +0)"""
    a = m.call(D_RATATAN2, u32(m.lhs(e + 0x1C2) << 4), u32(m.lhs(e + 0x1C6) << 4))
    m.sw(e + 0x124, a)
    m.sw(e + 0x128, u32(m.lhs(D_SINCOS + ((a & 0xFFF) << 2 | 2)) << 4))
    a2 = m.lw(e + 0x124)
    m.sw(e + 0x12C, u32(m.lhs(D_SINCOS + ((a2 & 0xFFF) << 2)) << 4))


def D_maxu_228(m, e, mag):
    """e->f228 = (mag <u e->f228) ? e->f228 : mag"""
    old = m.lw(e + 0x228)
    m.sw(e + 0x228, old if u32(mag) < old else mag)


def D_fixdot32(ax, ay, az, bx, by, bz):
    """the inline idiom: sum of the low words of (a*b) >> 16 per component, 32-bit adds (x+y first)"""
    def t(a, b):
        return ((s32(a) * s32(b)) >> 16) & 0xFFFFFFFF
    return u32(u32(t(az, bz) + u32(t(ay, by) + t(ax, bx))))


# ==== 0x800aad30 Response ========================================

F0aad30_DT = 0x800CCE38
F0aad30_FixMul, F0aad30_FixDiv, F0aad30_DotLcm, F0aad30_Scale, F0aad30_Blend16To32 = 0x8001FC90, 0x80010028, 0x8002E698, 0x8002EE50, 0x8002ECB8
F0aad30_ApplyImpulse, F0aad30_AiProject = 0x800A8DF0, 0x800B6AAC


def F0aad30_sdiv(a, b):
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def F0aad30_sabs(x):
    x = s32(x); s = x >> 31
    return s32((x + s) ^ s)


def F0aad30_model(m, A, B, outA, outB):
    """0x800AAD30 ContactResponse(A, B, &outA, &outB, [sp+16] s32[3] out, [sp+20] &remaining) frame 200.
    All locals live in RAM at their guest addresses (sp = m.sp)."""
    sp = m.sp
    L = lambda o: m.lws(sp + o)
    S = lambda o, v: m.sw(sp + o, v)
    # home-area spills of a0..a3 (writes into the caller's frame)
    m.sw(sp + 200, A); m.sw(sp + 204, B); m.sw(sp + 208, outA); m.sw(sp + 212, outB)
    vout = m.lw(sp + 216)                          # s6
    m.sw(outB, 0)
    m.sw(outA, 0)
    s1 = s32(m.lw(A + 188) - m.lw(B + 188))        # centre y difference
    if 0x140000 < F0aad30_sabs(s1):
        return 0
    s8 = 1
    S(104, 8); S(108, 6); S(140, 0); S(136, 0)
    S(64, A); S(68, B); S(76, B)
    S(72, m.lw(A + 856))
    s4 = 0
    v1 = 0
    if m.lw(A + 856) != 0:
        v1 = 1 if m.lw(A + 1088) != 0 else 0
    S(152, 2 << v1)
    d = m.call(F0aad30_DotLcm, A + 450, B + 450)
    v0 = s32(m.call(F0aad30_FixMul, d, m.lw(B + 480)))
    sA, sB = s32(m.lw(A + 480)), s32(m.lw(B + 480))
    rel = s32(sA - v0)
    S(124, rel)
    if sB < sA: take = rel < sA
    else: take = rel < sB
    if take:
        s2 = sA if sB < sA else sB
    else:
        s2 = rel
    v0 = s32(m.call(F0aad30_FixMul, m.lw(F0aad30_DT), u32(s2)))
    v0 = s32(v0 + 0x8000) >> 16
    v0 = s32(v0 * 3)
    if v0 < 0: v0 = s32(v0 + 3)
    n = v0 >> 2
    a0 = s32(n + (((s32(n - 1) >> 31)) & s32(1 - n)))
    t = s32(4 - n)
    a0 = s32(a0 + ((t >> 31) & t))
    S(148, a0)
    dt = m.lw(F0aad30_DT)
    S(132, dt)
    if not (a0 < 2):
        step = s32(F0aad30_sdiv(s32(dt), a0))              # div t1,a0 ; mflo (a0 >= 2: no zero divide)
        S(132, step)
        k = m.call(F0aad30_FixMul, m.lw(A + 480), u32(step - s32(dt)))
        m.call(F0aad30_Scale, k, A + 450, vout)
        m.call(F0aad30_ApplyImpulse, A, vout, 0)
        dt2 = m.lw(F0aad30_DT)
        k = m.call(F0aad30_FixMul, m.lw(B + 480), u32(L(132) - s32(dt2)))
        m.call(F0aad30_Scale, k, B + 450, vout)
        m.call(F0aad30_ApplyImpulse, B, vout, 0)
    S(112, m.call(F0aad30_FixMul, m.lw(sp + 132), m.lw(sp + 124)))
    d = m.call(F0aad30_DotLcm, B + 450, A + 450)
    S(144, 0)
    v0 = s32(m.call(F0aad30_FixMul, d, m.lw(A + 480)))
    v1 = s32(m.lw(B + 480) - v0)
    S(128, v1)
    S(116, m.call(F0aad30_FixMul, m.lw(sp + 132), u32(v1)))
    s5 = None
    s3 = None
    if s8 == 0:                                    # dead: s8 is 1 here
        m.notes.append('dead-ab020')
    else:
        while True:                                # ab02c: outer (substep) loop
            s7 = 0
            while True:                            # ab030: pair loop
                s4 = m.lw(sp + 64 + 4 * s7)
                s3 = m.lw(sp + 64 + 4 * ((s7 + 3) & 3))
                if s7 & 1:
                    s2 = L(116); a2 = L(128)
                else:
                    s2 = L(112); a2 = L(124)
                p3 = m.lhu(s3 + 172) >> 5
                s5 = 0
                if p3 == 3:
                    s5 = 16
                    if (m.lhu(s4 + 172) >> 5) == 0 and (m.lw(s4 + 568) & 0x600) == 0:
                        s5 = 48
                big = 1 if (p3 == 0 or p3 == 3) else 0
                hw, hl = s32(m.lw(s3 + 304)), s32(m.lw(s3 + 308))
                mn = hw if hw < hl else hl
                thr = s32(mn * 20) & u32(-big)
                thr = s32(thr)
                S(84, thr)
                if F0aad30_sabs(a2) < thr:
                    s0 = s32(m.call(F0aad30_FixMul, m.lw(s4 + 300), u32(m.lw(s3 + 468) - m.lw(s4 + 468))))
                    v0 = s32(m.call(F0aad30_FixMul, m.lw(s4 + 296), u32(m.lw(s3 + 476) - m.lw(s4 + 476))))
                    s1 = s32(s0 + v0)
                    S(16, s3 + 432); S(20, s5); S(24, sp + 80); S(28, sp + 84)
                    s5 = m.call(0x800B71AC, s4 + 196, s4 + 450, u32(s1), s3 + 196,
                                stack=(s3 + 432, s5, sp + 80, sp + 84))
                else:
                    s1 = s2
                    S(16, s3 + 432); S(20, s5); S(24, sp + 80); S(28, sp + 84)
                    s5 = m.call(0x800B74F0, s4 + 196, s4 + 450, u32(s2), s3 + 196,
                                stack=(s3 + 432, s5, sp + 80, sp + 84))
                if s5 < 8:                         # ab1b0 (unsigned)
                    s8 = 0
                    S(16, 0)
                    m.call(0x800B675C, s3 + 196, s3 + 432, m.lw(sp + 80), sp + 56, stack=(0,))
                    v0 = s32(m.call(F0aad30_DotLcm, sp + 56, s4 + 450))
                    s0 = s32(-v0) if s1 < 0 else v0
                    a1 = s32(m.lw(s4 + 308))
                    s1 = a1
                    if not (a1 < s2):
                        if s2 <= 0:
                            if a1 <= 0:
                                s1 = s32(m.call(F0aad30_FixDiv, u32(-s2), u32(-a1)))
                            else:
                                s1 = s32(-s32(m.call(F0aad30_FixDiv, u32(-s2), u32(a1))))
                        else:
                            if a1 > 0:
                                s1 = s32(m.call(F0aad30_FixDiv, u32(s2), u32(a1)))
                            else:
                                s1 = s32(-s32(m.call(F0aad30_FixDiv, u32(s2), u32(-a1))))
                    if s0 < -3275:
                        a0 = s32(L(84) + 8192 + s1)
                        S(84, a0)
                        m.call(F0aad30_Scale, u32(a0), sp + 56, vout)       # a1 = sp+56 set in the delay slot at 0x800AB24C
                    else:
                        v0 = s32(L(84) + s1)
                        S(84, v0)
                        S(16, 8192)
                        m.call(F0aad30_Blend16To32, s4 + 450, sp + 56, vout, u32(-v0), stack=(8192,))
                    pB = m.lhu(B + 172) >> 5
                    if pB == 0 or pB == 3:
                        f2 = s5 & 2
                        sp80 = s32(m.lw(sp + 80))
                        if (f2 and sp80 == 3) or (not f2 and sp80 == 1):
                            if s32(m.call(F0aad30_DotLcm, A + 450, B + 450)) > 0:
                                v0 = s32(m.call(F0aad30_AiProject, s4 + 468, s3 + 432, s3 + 468))
                                S(84, v0)
                                if v0 < 0:
                                    S(80, 0)
                                    s5 = 2 if (s5 & 2) else 1
                                    a0 = s32(s32(-s32(m.lw(s3 + 304))) - s32(m.lw(s4 + 304)))
                                else:
                                    S(80, 2)
                                    s5 = 3 if (s5 & 2) else 0
                                    a0 = s32(m.lw(s3 + 304) + m.lw(s4 + 304))
                                m.call(F0aad30_Scale, u32(a0), s3 + 432, vout)
                                S(84, s32(m.lw(s3 + 304) + m.lw(s4 + 304)))
                    # ab3a8
                    saved = False
                    if s7 + 2 < L(152):
                        if (s3 == A and L(80) == 2) or (s4 == A and (s5 & 1) != ((s5 & 2) >> 1)):
                            S(104, s5); S(136, s4); S(108, L(80))
                            S(40, m.lw(vout)); S(140, s3); S(44, m.lw(vout + 4)); S(48, m.lw(vout + 8))
                            s7 = 1; s8 = 1
                            S(120, L(84))
                            saved = True
                    if not saved and u32(L(104)) < 8:
                        if s3 == m.lw(sp + 140):
                            if L(108) != L(80):
                                add = True
                            else:
                                add = False
                                if L(84) < L(120):
                                    m.sw(vout, m.lw(sp + 40)); m.sw(vout + 4, m.lw(sp + 44))
                                    m.sw(vout + 8, m.lw(sp + 48))
                        else:
                            S(40, -L(40)); S(48, -L(48)); S(44, -L(44))
                            add = True
                        if add:
                            m.sw(vout, m.lw(vout) + m.lw(sp + 40))
                            m.sw(vout + 4, m.lw(vout + 4) + m.lw(sp + 44))
                            m.sw(vout + 8, m.lw(vout + 8) + m.lw(sp + 48))
                # ab510
                if s8 != 0:
                    s7 += 1
                    if s7 < L(152):
                        continue
                break
            # ab52c
            if u32(L(104)) < 8:
                s8 = 0
            if s8 == 0:
                break
            s4 = 0
            if L(144) < s32(L(148) - 1):
                k = m.call(F0aad30_FixMul, m.lw(A + 480), m.lw(sp + 132))
                m.call(F0aad30_Scale, k, A + 450, vout)
                m.call(F0aad30_ApplyImpulse, A, vout, 0)
                k = m.call(F0aad30_FixMul, m.lw(B + 480), m.lw(sp + 132))
                m.call(F0aad30_Scale, k, B + 450, vout)
                m.call(F0aad30_ApplyImpulse, B, vout, 0)
                S(144, L(144) + 1)
                if (m.lhu(B + 172) >> 5) != 0 or L(104) != 8:
                    continue                       # ab67c with s8 = 1
                S(16, sp + 96); S(20, sp + 100)
                r = s32(m.call(0x800ABE78, A, B, sp + 88, sp + 92, stack=(sp + 96, sp + 100)))
                if not (r < 15):
                    S(16, m.lw(sp + 96)); S(24, outA); S(28, outB); S(32, vout); S(20, m.lw(sp + 100))
                    s4 = m.call(0x800AA474, A, B, m.lw(sp + 88), m.lw(sp + 92),
                                stack=(m.lw(sp + 96), m.lw(sp + 100), outA, outB, vout))
                if s4 != 0:
                    return F0aad30_tail(m, sp, s4)
                continue
            else:
                s8 = 0
                break
    # ab684
    if u32(L(104)) < 8 and s5 == 8:
        S(80, m.lw(sp + 108))
        m.sw(vout, m.lw(sp + 40))
        s5 = m.lw(sp + 104)
        m.sw(vout + 4, m.lw(sp + 44))
        s4 = m.lw(sp + 136)
        m.sw(vout + 8, m.lw(sp + 48))
    if not (u32(s5) < 8):
        return s4
    if s4 == B:
        m.sw(outB, s5 | 0x100)
        a2 = outA
    else:
        m.sw(outA, s5 | 0x100)
        a2 = outB
    m.sw(a2, m.lw(sp + 80) | 0x200)
    return F0aad30_tail(m, sp, s4)


def F0aad30_tail(m, sp, s4):
    """ab710: the optional remaining-time out-param (arg 6)"""
    t0 = m.lw(sp + 220)
    if t0 == 0:
        return s4
    k = s32(m.lw(sp + 144))
    if k < s32(m.lw(sp + 148) - 1):
        m.sw(t0, m.lw(0x800CCE38) - (k + 1) * s32(m.lw(sp + 132)))
    else:
        m.sw(t0, 0)
    return s4


# ==== 0x800aa474 Response1F ========================================

F0aa474_FixMul, F0aa474_FixDiv, F0aa474_SqrtGte, F0aa474_Scale, F0aa474_AiProject = 0x8001FC90, 0x80010028, 0x8004CF74, 0x8002EE50, 0x800B6AAC


F0aa474_OOR = None


def F0aa474_divu(a, b):
    a, b = u32(a), u32(b)
    return 0xFFFFFFFF if b == 0 else a // b


def F0aa474_inv_len(m, s3):
    """0x800AA788..0x800AA814: three F0aa474_SqrtGte calls, a = 0x80000000 /u ((4r)/2 fix), negated when 4r < 0"""
    r = s32(m.call(F0aa474_SqrtGte, u32(s3)))
    if s32(u32(r << 2)) < 0:
        s0 = s32(m.call(F0aa474_SqrtGte, u32(s3)))
        v0 = s32(m.call(F0aa474_SqrtGte, u32(s3)))
        s0 = s32(u32(-s32(u32(s0 << 2)))) >> 1
        v0 = s32(u32(-s32(u32(v0 << 2)) - 2)) >> 31
        s0 = F0aa474_divu(0x80000000, s0 + v0)
        m.notes.append('neg-sqrt')
        return s32(u32(-s0))
    s0 = s32(m.call(F0aa474_SqrtGte, u32(s3)))
    v0 = s32(m.call(F0aa474_SqrtGte, u32(s3)))
    s0 = s32(u32(s0 << 2)) >> 1
    v0 = s32(u32((v0 << 2) - 2)) >> 31
    return s32(F0aa474_divu(0x80000000, s0 + v0))


def F0aa474_model(m, a0, a1, a2, a3):
    """0x800AA474 HeavyResponse(A, B, flags, depth, [16] i4, [20] i5, [24] &outA, [28] &outB, [32] s32[3] out)
    frame 264. Locals in RAM at their guest addresses."""
    sp = m.sp
    L = lambda o: m.lws(sp + o)
    S = lambda o, v: m.sw(sp + o, v)
    S(200, 131); S(204, 131); S(208, 1)
    s8 = 0
    t0 = s32(m.lw(sp + 280))
    if a2 & 2:
        a1 = m.lw(a1 + 856)
    elif a2 & 1:
        a0 = m.lw(a0 + 856)
    s4 = 1 if s32(m.lw(sp + 284)) < 1 else 0
    if not (s32(a3) < 2049):
        s4 ^= 1
    if t0 > 0:
        noswap = (s4 == 1)
    else:
        noswap = (s4 == 0)
    if noswap:
        S(168, a0); S(172, a1); S(192, m.lw(sp + 288)); S(196, m.lw(sp + 292))
    else:
        S(192, m.lw(sp + 292)); S(168, a1); S(172, a0); S(196, m.lw(sp + 288))
    P, Q = m.lw(sp + 168), m.lw(sp + 172)
    s4 = s32(-s4)                               # 0 or -1
    s5 = s4 & -3                                # 0 or -3
    # P's rectangle in its own (0x1B0, 0x1B6) plane, at sp+16: (-w,0) (w,0) (w,-h) (-w,-h)
    w, h = s32(m.lw(P + 304)), s32(m.lw(P + 312))
    S(20, 0); S(16, -w); S(28, 0); S(24, w); S(32, w); S(36, -h)
    S(212, sp + 80)
    S(40, -w)
    S(216, sp + 48)
    S(44, -h)
    ax1, ax2, org = P + 432, P + 438, P + 184
    # Q's four corners projected: (3,2,6,7) when not flipped, (0,1,5,4) when flipped
    idx = [s5 + 3, s4 + 2, s4 + 6, s5 + 7]
    for k, ci in enumerate(idx):
        c = u32(Q + 196 + 12 * ci)
        S(48 + 8 * k, m.call(F0aa474_AiProject, c, ax1, org))
        S(52 + 8 * k, m.call(F0aa474_AiProject, c, ax2, org))
    while True:                                 # aa6ec: two passes, s8 = 0 then 1 (| 0x10)
        s7 = 0
        if s32(s8) <= 0:
            S(160, P); S(176, sp + 16); S(184, m.lw(sp + 192)); S(164, Q); S(180, sp + 48)
            S(80, 0x10000); S(84, 0); S(88, 0); S(92, 0xFFFF0000); S(96, 0xFFFF0000)
            S(100, 0); S(104, 0); S(108, 0x10000); S(188, m.lw(sp + 196))
            s3 = 0
        else:
            s2 = m.lw(sp + 216); s1 = m.lw(sp + 212)
            s5 = L(72); s6 = L(76)
            S(160, Q); S(176, sp + 48); S(184, m.lw(sp + 196)); S(164, P); S(180, sp + 16)
            S(188, m.lw(sp + 192))
            for s7 in range(4):
                nx = s32(m.lw(s2 + 4) - s6)
                m.sw(s1, nx)
                ny = s32(s5 - m.lw(s2))
                q0 = s32(m.call(F0aa474_FixMul, u32(nx), u32(nx)))
                m.sw(s1 + 4, ny)
                q1 = s32(m.call(F0aa474_FixMul, m.lw(s1 + 4), m.lw(s1 + 4)))
                s3 = s32(q0 + q1)
                if s3 < 17:
                    m.sw(m.lw(sp + 292), 0)
                    m.sw(m.lw(sp + 288), 0)
                    return 0
                k = F0aa474_inv_len(m, s3)
                m.sw(s1, m.call(F0aa474_FixMul, u32(k), m.lw(s1)))
                m.sw(s1 + 4, m.call(F0aa474_FixMul, u32(k), m.lw(s1 + 4)))
                s5 = s32(m.lw(s2)); s6 = s32(m.lw(s2 + 4))
                s2 += 8; s1 += 8
            s3 = 0
            if L(80) < 1024:
                s8 |= 0x10
        # aa8f8: point-in-polygon: points at sp180 against polygon sp176 with normals sp212
        pts, poly, nrm = m.lw(sp + 180), m.lw(sp + 176), m.lw(sp + 212)
        found = None
        for s7 in range(4):
            s2 = pts + 8 * s7
            inside = True
            for s4 in range(4):
                s1 = poly + 8 * s4
                n = nrm + 8 * s4
                x = s32(m.call(F0aa474_FixMul, u32(m.lw(s2) - m.lw(s1)), m.lw(n)))
                y = s32(m.call(F0aa474_FixMul, u32(m.lw(s2 + 4) - m.lw(s1 + 4)), m.lw(n + 4)))
                d = s32(x + y)
                S(144 + 4 * s4, d)
                if s32(d + 1024) < 0:
                    inside = False
                    break
            if inside:
                found = s7
                break
        s3 = 0 if found is None else (1 << found)
        if s3 != 0:
            other, mine = m.lw(sp + 164), m.lw(sp + 160)
            v = s32(m.call(F0aa474_AiProject, other + 468, mine + 516, mine + 468))
            if v < 0:
                s7 = 1 if (s3 & 2) else 2
                v1 = u32(-1) if (s8 & 0x10) else 0
                s4 = (v1 & 5) if L(84) < 0 else (v1 & 4)
                pt = m.lw(sp + 180) + 8 * s7
                s6 = s32(m.lw(pt + 4)); vx = m.lw(pt); off = 0xFFF80000
            else:
                s7 = u32((s3 & 1) - 1) & 3      # negu ; nor
                v1 = u32(-1) if (s8 & 0x10) else 0
                s4 = ((v1 & 2) if L(84) < 0 else (v1 & 3)) + 2
                pt = m.lw(sp + 180) + 8 * s7
                s6 = s32(m.lw(pt + 4)); vx = m.lw(pt); off = 0x80000
            s5 = s32(vx + off)
            s1 = m.lw(sp + 176) + 8 * s4
            n = m.lw(sp + 212) + 8 * s4
            x = s32(m.call(F0aa474_FixMul, u32(s5 - m.lw(s1)), m.lw(n)))
            y = s32(m.call(F0aa474_FixMul, u32(s6 - m.lw(s1 + 4)), m.lw(n + 4)))
            a1v = s32(x + y)
            e = L(144 + 4 * s4)
            if F0aa474_OOR is not None and s4 >= 4:          # analysis hook only (no effect on the F0aa474_model)
                F0aa474_OOR(dict(sp=sp, s4=s4, s7=s7, s8=s8, e=e, a1=a1v, v=(m.lws(s1), m.lws(s1 + 4)),
                         n=(m.lws(n), m.lws(n + 4)), pt=(s5, s6), P=m.lw(sp + 168), Q=m.lw(sp + 172)))
            skip = False
            if s32(e - a1v) < 131 and a1v < 131:
                S(208, 0)
                skip = True
            if not skip:
                a0v = s32(-a1v)
                if a0v > 0:
                    v0 = s32(e - a1v)
                    if v0 <= 0:
                        s3 = s32(-s32(m.call(F0aa474_FixDiv, u32(a0v), u32(a1v - e))))
                    else:
                        s3 = s32(m.call(F0aa474_FixDiv, u32(a0v), u32(v0)))
                else:
                    v0 = s32(e - a1v)
                    if v0 <= 0:
                        s3 = s32(m.call(F0aa474_FixDiv, u32(a1v), u32(a1v - e)))
                    else:
                        s3 = s32(-s32(m.call(F0aa474_FixDiv, u32(a1v), u32(v0))))
                if not (s3 < -15):
                    s2 = s32(0x10000 - s3)
                    s0 = s32(m.call(F0aa474_FixMul, u32(s2), u32(s5)))
                    q = m.lw(sp + 180) + 8 * s7
                    S(208, 0)
                    v0 = s32(m.call(F0aa474_FixMul, u32(s3), m.lw(q)))
                    S(200, s0 + v0)
                    s0 = s32(m.call(F0aa474_FixMul, u32(s2), u32(s6)))
                    v0 = s32(m.call(F0aa474_FixMul, u32(s3), m.lw(q + 4)))
                    S(204, s0 + v0)
        # aab94
        t1 = m.lw(sp + 208)
        s8 = s32(s8 + 1)
        if t1 == 0:
            break
        if s8 < 2:
            continue
        m.sw(m.lw(sp + 292), 0)
        m.sw(m.lw(sp + 288), 0)
        return 0
    # aabc8: found
    if s4 == 1: s4 = 4
    elif s4 == 3: s4 = 5
    m.sw(m.lw(sp + 188), s7 | 0x100)
    m.sw(m.lw(sp + 184), s4 | 0x200)
    a0 = u32(-1) if (s4 == 0 or s4 == 2) else 0
    p = m.lw(sp + 188)
    v1 = m.lw(p)
    v0 = u32(((s4 ^ 2) | 0x200) - v1)
    m.sw(p, v1 + (a0 & v0))
    q = m.lw(sp + 180) + 8 * s7
    v = s32(L(200) - m.lw(q)); v = s32(v * 5)
    if v < 0: v = s32(v + 3)
    s5 = v >> 2
    v = s32(L(204) - m.lw(q + 4)); v = s32(v * 5)
    if v < 0: v = s32(v + 3)
    s6 = v >> 2
    P = m.lw(sp + 168)
    m.call(F0aa474_Scale, u32(s5), P + 432, sp + 112)
    m.call(F0aa474_Scale, u32(s6), P + 438, sp + 128)
    out = m.lw(sp + 296)
    m.sw(out, m.lw(sp + 112) + m.lw(sp + 128))
    m.sw(out + 4, m.lw(sp + 116) + m.lw(sp + 132))
    m.sw(out + 8, m.lw(sp + 120) + m.lw(sp + 136))
    return m.lw(sp + 164)


# ==== 0x800aa140 ResponseFar ========================================


def F0aa140_quad(x):
    """((x + 8192) & 0xFFF) + 256, then the compiler's /1024 with round-to-zero fix (dead: always >= 0)"""
    v0 = ((x + 8192) & 0xFFF) + 256
    v1 = v0 + 512
    if s32(v1) < 0: v1 = v0 + 1535          # never taken: v1 >= 768
    return s32(v1) >> 10


def F0aa140_model(m, A, B, outA, outB):
    """0x800AA140 BoxOverlapInViewPlane(A, B, &outA, &outB, [sp+16] vec3 out) frame 80"""
    sp = m.sp
    rec = m.lw(A + 340)                     # A->f154
    vout = m.lw(m.entry_sp + 16)           # s1 = 96(sp)
    m.call(0x800AA34C, A + 196, rec, sp + 16)
    m.call(0x800AA34C, B + 196, rec, sp + 32)
    bA = [m.lws(sp + 16 + 4 * k) for k in range(4)]     # min u, min v, max u, max v
    bB = [m.lws(sp + 32 + 4 * k) for k in range(4)]
    t0 = s32(bB[2] - bA[0])
    a3 = s32(bA[2] - bB[0])
    sep = 1 if (t0 < 0 or a3 < 0) else 0
    if not sep:
        a1 = s32(bB[3] - bA[1])
        v0 = s32(bA[3] - bB[1])
        if a1 < 0 or v0 < 0: sep = 1
    if sep:
        m.sw(outB, 0)
        m.sw(outA, 0)
        return 0
    # a0 = min(t0, a3), a3flag = t0 < a3 ; v1 = min(a1, v0), a2flag = a1 < v0
    fa = 1 if t0 < a3 else 0
    ox = t0 if fa else a3
    fb = 1 if a1 < v0 else 0
    oy = a1 if fb else v0
    if ox < oy:
        k = -ox if fa else ox
        s0 = fa << 1
        axis = rec + 2
    else:
        k = -oy if fb else oy
        s0 = 3 if fb else 1
        axis = rec + 14
    m.call(0x8002EE50, u32(k), axis, vout)
    s1 = s0 ^ 2
    ang = s32(m.call(0x80020018, u32(m.lhs(rec + 14) << 4), u32(m.lhs(rec + 18) << 4)))
    s0 = (s0 + F0aa140_quad(s32(m.lw(B + 292)) - ang)) & 3
    m.sw(outB, s0 | 0x200)
    s1 = (s1 + F0aa140_quad(s32(m.lw(A + 292)) - ang)) & 3
    m.sw(outA, s1 | 0x200)
    return B


# ==== 0x800aa34c ViewExtent ========================================


def F0aa34c_model(m, corners, rec, out, a3):
    """0x800AA34C BoxExtent2D(corners, rec, out[4]) frame 48.
    out = {min u, min v, max u, max v} of corners 0,2,1,3 projected on rec+2 / rec+14 about rec+20."""
    m.sw(out + 4, 0x3FFF0000)
    m.sw(out + 0, 0x3FFF0000)
    m.sw(out + 12, 0xC0010000)
    m.sw(out + 8, 0xC0010000)
    s3 = 29984                                  # 0x7520: corner order 0, 2, 1, 3 (2 bits each, low first)
    for _ in range(4):
        p = u32(corners + 12 * (s3 & 3))
        u = s32(m.call(0x800B6AAC, p, rec + 2, rec + 20))
        v = s32(m.call(0x800B6AAC, p, rec + 14, rec + 20))
        a0 = m.lws(out + 0)
        if u < a0: a0 = u
        m.sw(out + 0, a0)
        a1 = m.lws(out + 8)
        m.sw(out + 8, u if a1 < u else a1)
        a0 = m.lws(out + 4)
        if v < a0: a0 = v
        m.sw(out + 4, a0)
        a1 = m.lws(out + 12)
        m.sw(out + 12, v if a1 < v else a1)
        s3 >>= 4
    return None


# ==== 0x800b5b48 PairPushDir ========================================


def F0b5b48_fix(a, b):
    """mult a,b ; (lo >> 16) | (hi << 16): bits 16..47 of the signed 64-bit product (inline FixMul)"""
    return s32((s32(a) * s32(b)) >> 16)


def F0b5b48_sabs(x):
    """(x + (x >> 31)) ^ (x >> 31): abs, INT_MIN stays INT_MIN"""
    x = s32(x)
    s = x >> 31
    return s32((x + s) ^ s)


def F0b5b48_model(m, X, Y, out, a3):
    """0x800B5B48 PairVelocityDir(X, Y, out s16[3]) frame 56"""
    k = s32(m.call(0x80010028, m.lw(Y + 316), m.lw(X + 316)))     # FixDiv(Y->f13C, X->f13C)
    t0 = F0b5b48_fix(m.lw(Y + 480), k)                                     # Y->f1E0 * k
    v = []
    for i in range(3):
        a = F0b5b48_fix(m.lw(X + 480), m.lhs(X + 450 + 2 * i) << 4)
        b = F0b5b48_fix(t0, m.lhs(Y + 450 + 2 * i) << 4)
        v.append(s32(b + a))
    T = 0x5A8000
    if F0b5b48_sabs(v[0]) > T or F0b5b48_sabs(v[1]) > T or F0b5b48_sabs(v[2]) > T:
        while True:
            v = [x >> 1 for x in v]                                # sra 1
            if F0b5b48_sabs(v[0]) > T or F0b5b48_sabs(v[1]) > T or F0b5b48_sabs(v[2]) > T:
                continue
            break
    ss = s32(F0b5b48_fix(v[2], v[2]) + s32(F0b5b48_fix(v[1], v[1]) + F0b5b48_fix(v[0], v[0])))
    r = s32(m.call(0x8004CF74, u32(ss)))
    r4 = s32(u32(r << 2))
    adj = -1 if s32(r4 - 2) < 0 else 0                             # (r4 - 2) sra 31
    d = u32((r4 >> 1) + adj)
    q = 0xFFFFFFFF if d == 0 else (0x80000000 // d)                 # divu, LO
    for i in range(3):
        m.sh(out + 2 * i, F0b5b48_fix(q, v[i]) >> 4)
    return None


# ==== 0x800b3ad0 WallContact ========================================
"""B: F0b3ad0_model of RASHCDG 0x800B3AD0 (7968 bytes, frame 296) - WallContact(e, force).
Transcribed from our own disassembly of RASHCDG.BIN sha1 cfe43a7786759f2cb9c57751cf99e84d1074782c.
Only the Machine interface of pairs.py is used; every callee goes through m.call.
Locals whose address is passed to a callee (or that a callee writes) live in guest RAM at m.sp+off:
  +24 s16 N[3] (working wall normal)   +32 s32 P0[3]   +48 s32 P1[3]   +64 s32 C[3] (contact point)
  +80 s32 A[3]   +96 s32 I[3] (impulse)   +120 s32 depth   +124 s32 count   +16/+20 outgoing stack args
Everything else is a Python int (a register, or a stack slot whose address never escapes); the
comment on each gives its guest home (sp offset)."""

F0b3ad0_DT = 0x800CCE38          # s32 dt of the collision pass
F0b3ad0_GS = 0x8005B2F8          # -> game state (+48 numPlayers)
F0b3ad0_CRTAB = 0x800CCAA0       # u32[16] corner-order nibble table, indexed by the class word (184)
F0b3ad0_VIEW = 0x800CD898        # per-player view object, 1132 bytes
F0b3ad0_CREC = 0x800CCE48        # ContactRec[8] x 36
F0b3ad0_CCNT = 0x800CCF68        # contact count
F0b3ad0_PROPB = 0x800CD6D4       # -> prop pool (596)
F0b3ad0_RIDB = 0x8005B3A4        # -> rider pool (628)
F0b3ad0_PEDB = 0x800D4B80        # -> pedestrian pool (572)

F0b3ad0_FixMul, F0b3ad0_FixDiv, F0b3ad0_DotLcm, F0b3ad0_MulAdd = 0x8001FC90, 0x80010028, 0x8002E698, 0x8002EAD8
F0b3ad0_Scale, F0b3ad0_Normalize, F0b3ad0_Blend16To32, F0b3ad0_SqrtGte = 0x8002EE50, 0x8002E468, 0x8002ECB8, 0x8004CF74
F0b3ad0_AiProject, F0b3ad0_StaleHeading, F0b3ad0_InCameraBox, F0b3ad0_ApplyImpulse = 0x800B6AAC, 0x800A8FE8, 0x800A8C78, 0x800A8DF0
F0b3ad0_RayPlane, F0b3ad0_CornerMin, F0b3ad0_ContactMerge, F0b3ad0_ViewTrack = 0x800B6BD0, 0x800B6F40, 0x800B59F0, 0x800A451C
F0b3ad0_BikeWallHit, F0b3ad0_PropTopple, F0b3ad0_RiderWallHit, F0b3ad0_PedHit = 0x800B12A0, 0x800B3344, 0x800B2794, 0x800A9868


def F0b3ad0_iabs(v):
    """sra t,v,31; addu; xor  (INT_MIN stays INT_MIN)"""
    v = s32(v); t = v >> 31
    return s32((v + t) ^ t)


def F0b3ad0_half(v):
    """srl t,v,31; addu; sra 1  = C (int)v/2 of a wrapped 32-bit value"""
    v = u32(v)
    return s32(v + (v >> 31)) >> 1


def F0b3ad0_div4(v):
    """bgez; addiu 3; sra 2  = C (int)v/4"""
    v = s32(v)
    return s32(v + 3 if v < 0 else v) >> 2


def F0b3ad0_div16(v):
    """bgez; addiu 15; sra 4  = C (int)v/16"""
    v = s32(v)
    return s32(v + 15 if v < 0 else v) >> 4


def F0b3ad0_octdist(dx, dz):
    """0x800B4528..0x800B4580 (and ..0x800B45E4): octagonal length of (dx, dz), every shift sra:
    max - max>>5 - max>>7 + (min + min>>1)>>2 + (min + min>>1)>>6"""
    a1, a0 = F0b3ad0_iabs(dx), F0b3ad0_iabs(dz)
    if a1 < a0: a1, a0 = a0, a1
    a2 = s32(a0 + (a0 >> 1))
    v = s32(a1 - (a1 >> 5)); v = s32(v - (a1 >> 7)); v = s32(v + (a2 >> 2))
    return s32(v + (a2 >> 6))


def F0b3ad0_model(m, a0, a1, a2, a3):
    e, force, sp = a0, a1, m.sp
    lw, lh, lhu, lbu, sw, sh = m.lws, m.lhs, m.lhu, m.lbu, m.sw, m.sh
    sw(m.entry_sp + 4, force)                      # 0x800B3B18 sw a1,300(sp): home-area spill
    NX, NY, NZ = sp + 24, sp + 26, sp + 28
    P0, P1, CP, A, I = sp + 32, sp + 48, sp + 64, sp + 80, sp + 96
    DEPTH, COUNT = sp + 120, sp + 124
    HEAD = u32(e + 450)                            # 232(sp): &e->1C2 (s16[3] heading)

    def neg_n():                                   # lhu 24, lhu 28, sh -24, lhu 26, sh -28, sh -26
        x, z = lhu(NX), lhu(NZ); sh(NX, -x); y = lhu(NY); sh(NZ, -z); sh(NY, -y)

    def fdiv(a, b):
        """the sign-magnitude F0b3ad0_FixDiv idiom (0x800B4A34, 0x800B5160): branches on a > 0 then b > 0"""
        a, b = s32(a), s32(b)
        if a > 0:
            if b > 0: return s32(m.call(F0b3ad0_FixDiv, a, b))
            return s32(-s32(m.call(F0b3ad0_FixDiv, a, -b)))
        if b > 0: return s32(-s32(m.call(F0b3ad0_FixDiv, -a, b)))
        return s32(m.call(F0b3ad0_FixDiv, -a, -b))

    st = dict(mode=1,          # 128
              cls=0,           # 184: 4-bit corner-class word (0x800CCAA0 index); reused at 0x800B5690
              lim2=2048,       # 192
              ent2=0,          # 196: the bike that takes +0x228 (the bike or its +0x358)
              best=0x7FFF0000, # 204
              tmin=0,          # 208
              bestSeg=0,       # 212
              result=0,        # 216
              L112=0, L116=0,  # 112/116: horizontal wall normal (16.16)
              travel=0,        # 188
              segVal=0,        # 248: segment +4 (or the type-1 fraction)
              s5=0)
    isView = 0                                     # 176
    view = 0                                       # 200
    f = m.lw(e + 388)
    hi12 = s32(f) >> 20                            # 160
    sw(e + 388, f & 0x000FFFEF)                    # 0x800B3B5C
    h = lhu(e + 172)
    if (h >> 5) == 4 and (h & 0x1F) >= 30:         # the per-player view object (pool 4, slot >= 30)
        isView = 1; view = e; sw(e + 768, 0)       # 0x800B3B90
    S = m.lw(e + 328)                              # 140: -> road section
    q = m.lw(e + 496)
    if q != 0:                                     # 0x800B3BA8: an explicit wall group at +0x1F0
        segCount = lbu(q + 3)                      # 164
        if segCount == 0: return 0
        if (m.lw(e + 388) & 1) == 0:
            if m.lw(e + 372) == 0: return 0
            if lh(e + 392) == 4: return 0
        groups = 1; groupNo = 0                    # 168, 156
        first = lh(m.lw(e + 496))                  # 152
        st['s5'] |= 0x100
    else:
        q = m.lw(e + 492)
        if q == 0: return 0
        if m.lw(e + 372) == 0: return 0
        lat = lw(e + 344)
        if lat < 0:
            groups = lbu(q + 1)
            if groups == 0: return 0
            prod = u32(lw(e + 424) * lh(e + 408))
            groupNo = lbu(q + 0)
            if s32(prod - 32768) < s32(-lat): st['s5'] = 64
        else:
            groups = lbu(q + 3)
            if groups == 0: return 0
            prod = u32(lw(e + 424) * lh(e + 420))
            groupNo = lbu(q + 2)
            if s32(prod - 32768) < lat: st['s5'] = 128
        g = u32(m.lw(S + 100) + 4 * groupNo)
        segCount = lbu(g + 3); first = lh(g)
        if force == 0 and isView == 0 and st['s5'] == 0: return 0
    # 0x800B3D18
    isBike = 1 if (lhu(e + 172) >> 5) == 0 else 0  # 172: 0 not a bike, 1 bike with a list node, 2 without
    if isBike:
        node = m.lw(e + 1088)
        st['ent2'] = e
        isBike += 1 if node == 0 else 0
        if node != 0: m.call(F0b3ad0_StaleHeading, e)
        if F0b3ad0_iabs(m.lw(e + 676)) >= 2130: st['mode'] = 2
        if m.lw(e + 856) != 0 and m.lw(e + 1088) != 0 and (lhu(e + 320) & 8) == 0:
            m.call(F0b3ad0_InCameraBox, e, 0x140000, 0x1C0000)
            sh(m.lw(e + 856) + 320, lhu(e + 320))   # 0x800B3DC8
        if lw(st['ent2'] + 636) < 0: st['cls'] |= 2

    # ------------------------------------------------------------------------------------------
    def seg(cur, s8, nrm, step, other, prevF, segCount, groups):
        """0x800B4244..0x800B5864 for one segment. Returns (code, prevF, segCount):
        code None -> 0x800B5868 next segment, 'G' -> 0x800B58B8 next group, int -> return it."""
        s5 = st['s5']
        if s8 == 0: s8 = cur
        s5 &= 0xFFFFFDFF
        if m.lh(s8 + 2) != prevF: s5 |= 0x200     # lh (sign-extended) against the u16 slot
        sh(NX, lhu(s8 + 32)); sh(NY, lhu(s8 + 34)); sh(NZ, lhu(s8 + 36))
        for k in range(3): sw(P0 + 4 * k, m.lw(s8 + 8 + 4 * k))
        for k in range(3): sw(P1 + 4 * k, m.lw(s8 + 20 + 4 * k))
        ny = lh(s8 + 34)
        prevF = lhu(s8 + 2)
        st['segVal'] = m.lw(s8 + 4)
        if F0b3ad0_iabs(ny) >= 65:
            sh(NY, 0); m.call(F0b3ad0_Normalize, NY - 2)
        s5 &= 0xFFFFFFE0
        st['s5'] = s5
        # ---- 0x800B4318: the step (type 5) segments
        go = None
        if (s5 & 0x200) and (prevF & 0xF) == 5:
            if force == 0 and isView == 0:
                go = 'gate'
            elif (hi12 & 0xF) == 5:
                go = 'up' if (isView or lh(e + 452) < 0) else 'gate'
            elif isView:
                go = 'up'
            else:
                R = m.lw(e + 340)
                if lw(e + 188) < s32(m.lw(R + 24) - st['segVal'] + 0x10000):
                    x = lhu(R + 14); sh(NX, x)
                    y = lhu(m.lw(e + 340) + 16); sh(NY, y)
                    z = lhu(m.lw(e + 340) + 18)
                    s5 |= 0x8
                    sh(NZ, z)
                    segCount += 1
                    if lw(e + 364) > 0:
                        sh(NX, -x); sh(NY, -y); sh(NZ, -z)
                    if st['dirn'] < 0:
                        for k in range(3): sw(P0 + 4 * k, m.lw(s8 + 20 + 4 * k))
                    road = m.lw(e + 372)
                    R = m.lw(e + 340)
                    if lw(e + 344) < 0:
                        m.call(F0b3ad0_MulAdd, P0, R + 2, -m.lw(road + 16), P1)
                    else:
                        m.call(F0b3ad0_MulAdd, P0, R + 2, -m.lw(road + 144), P1)
                    go = 'test'
                else:
                    go = 'up' if lh(e + 452) < 0 else 'gate'
            if go == 'up':                           # 0x800B4498
                s5 |= 0x10
                sh(NX, 0); sh(NY, 4096); sh(NZ, 0)
                R = m.lw(e + 340)
                segCount += 1
                m.call(F0b3ad0_MulAdd, R + 20, R + 8, st['segVal'], P0)
                road = m.lw(e + 372); R = m.lw(e + 340)
                m.call(F0b3ad0_MulAdd, P0, R + 2, m.lw(road + 144), P1)
                road = m.lw(e + 372); R = m.lw(e + 340)
                m.call(F0b3ad0_MulAdd, P0, R + 2, m.lw(road + 16), P0)
        st['s5'] = s5
        if (s5 & 0x1D8) == 0: return 'G', prevF, segCount       # 0x800B4510 -> 0x800B58B8
        # ---- 0x800B4518: the distance gate
        bx, bz = m.lw(e + 184), m.lw(e + 192)
        d0 = F0b3ad0_octdist(u32(bx - m.lw(P0)), u32(bz - m.lw(P0 + 8)))
        d1 = F0b3ad0_octdist(u32(bx - m.lw(P1)), u32(bz - m.lw(P1 + 8)))
        if d0 > 0xBF0000 or d1 > 0xBF0000: return None, prevF, segCount
        if prevF & 0x400:
            if (lhu(e + 172) >> 5) == 1 and (prevF & 0xA00) == 0 and lw(e + 604) == 4:
                return None, prevF, segCount
            v = m.call(F0b3ad0_AiProject, e + 468, NX, P0)
            s5 |= u32(v) >> 31
            st['s5'] = s5
            if (prevF & 0xF) == 1 and (s5 & 1): return None, prevF, segCount
        st['travel'] = s32(m.call(F0b3ad0_FixMul, m.lw(F0b3ad0_DT), m.lw(e + 480)))   # 188
        sw(sp + 188, st['travel'])
        st['cls'] &= 0xFFFFFFF2
        s3 = 0
        if not isView and not (s5 & 0x18) and not (prevF & 0x800):
            gs = m.lw(F0b3ad0_GS); hh = lhu(e + 172)
            player = (hh >> 5) < 2 and (hh & 0x1F) < lw(gs + 48)
            near = True
            if not player:
                near = m.call(F0b3ad0_InCameraBox, e, 0x140000, 0x1C0000) != 0
            if near:                                               # 0x800B470C
                s3 = F0b3ad0_iabs(m.call(F0b3ad0_DotLcm, HEAD, NX))
                if (hi12 & 0xF) == 0 or (st['travel'] >= 132 and 5701 < s3):
                    s5 |= 4                                        # 0x800B4768 (delay slot)
                    if st['travel'] < 0x20000: st['travel'] = 0x20000; sw(sp + 188, 0x20000)
                    if isBike:
                        p = u32(lh(e + 450) * lh(NZ)); q2 = u32(lh(e + 454) * lh(NX))
                        b = 1 if s32(q2) >= s32(p) else 0
                        b ^= s5 & 1
                        st['cls'] |= b << 2
                        st['cls'] |= 1 if s3 <= 0x926D else 0
            else:                                                  # 0x800B47E0
                if (hi12 & 0xF) == 0 or F0b3ad0_iabs(st['travel']) >= 131:
                    s3 = F0b3ad0_iabs(m.call(F0b3ad0_DotLcm, HEAD, NX))
                    if s3 >= 11469:
                        s5 |= 2
                        if not (s5 & 0x20):
                            m.call(F0b3ad0_MulAdd, e + 184, HEAD, -st['travel'], A)
                            v = m.call(F0b3ad0_FixMul, 0xB4FD, u32(m.lw(e + 304) + m.lw(e + 308)))
                            m.call(F0b3ad0_MulAdd, e + 184, HEAD, v, I)
                            if st['mode'] == 1:
                                if s32(m.call(F0b3ad0_DotLcm, HEAD, e + 444)) < 0: st['mode'] = 0
                            s5 |= 0x20
        st['s5'] = s5
        # ---- 0x800B48C0: find the corner
        s1 = 4096
        s7 = 8
        if s5 & 4:                                                 # 0x800B48C8 swept corners
            s7 = 0
            tab = u32(F0b3ad0_CRTAB + 4 * st['cls'])
            while True:
                if isBike: s0 = (m.lw(tab) >> (4 * s7)) & 7
                else: s0 = (7 - s7) & 7
                crn = u32(e + 196 + 12 * s0)
                m.call(F0b3ad0_MulAdd, crn, HEAD, -st['travel'], A)
                t = s32(m.call(F0b3ad0_RayPlane, A, HEAD, NX, P0))
                if t < -s1:
                    s7 += 1
                elif s32(st['travel'] + s1) < t:
                    s7 += 1
                else:
                    s7 = s0
                    m.call(F0b3ad0_MulAdd, A, HEAD, t, CP)
                    v = s32(m.call(F0b3ad0_AiProject, crn, NX, P0))
                    sw(DEPTH, -v)
                    dd = s1 if not (s1 < s32(-v)) else s32(-v)
                    sw(DEPTH, dd)
                    if isBike:
                        s2 = s32(-s32(m.call(F0b3ad0_DotLcm, NX, HEAD)))
                        if s2 >= 132 and st['travel'] >= 132:
                            x = m.call(F0b3ad0_FixMul, m.lw(F0b3ad0_DT), lw(DEPTH))
                            y = m.call(F0b3ad0_FixMul, s2, st['travel'])
                            st['tmin'] = fdiv(x, y)
                        dt = lw(F0b3ad0_DT)
                        st['tmin'] = st['tmin'] if st['tmin'] < dt else dt
                    break
                if not (s7 < 8): break
        elif s5 & 2:                                               # 0x800B4AD0 crossing test
            x = s32(m.call(F0b3ad0_AiProject, A, NX, P0))
            y = s32(m.call(F0b3ad0_AiProject, I, NX, P0))
            if (x ^ y) < 0:
                sw(DEPTH, ((-(s5 & 1)) & 0x80020000) + 0x3FFF0000)
                for s0 in range(8):
                    if st['mode'] == 2 or ((s0 >> 1) & 1) == st['mode']:
                        t = s32(m.call(F0b3ad0_RayPlane, e + 196 + 12 * s0, HEAD, NX, P0))
                        take = (s5 & 1) and lw(DEPTH) < t
                        if not take: take = t < lw(DEPTH)
                        if take:
                            s7 = s0; sw(DEPTH, t)
                if s7 < 8:
                    m.call(F0b3ad0_MulAdd, e + 196 + 12 * s7, HEAD, lw(DEPTH), CP)
        else:                                                      # 0x800B4BC4 static test
            if s5 & 1: neg_n()
            s3 = 0x80000
            if not isView:
                a = u32(m.lw(e + 308) + m.lw(e + 304) + F0b3ad0_half(m.lw(e + 312)))
                s3 = F0b3ad0_div4(u32(a * 3))
            sw(sp + 16, COUNT)
            s7 = m.call(F0b3ad0_CornerMin, e + 196, NX, P0, DEPTH, stack=(COUNT,))
            keep = True
            if lw(COUNT) == 8:
                x = False
                if prevF & 0x800:
                    gs = m.lw(F0b3ad0_GS); hh = lhu(e + 172); npl = m.lw(gs + 48)
                    if hh < npl and (m.lw(m.lw(e + 852) + 552) & 0x80000):
                        x = True
                    elif (hh >> 5) == 1 and s32(hh & 0x1F) < s32(npl):
                        x = (m.lw(e + 552) & 0x80000) != 0
                else:
                    x = True
                if x:
                    if s3 < lw(DEPTH): s7 = 8
                    elif (prevF & 0xF) == 2 and groups >= 2: s7 = 8
            if s7 < 8:
                m.call(F0b3ad0_MulAdd, e + 196 + 12 * s7, NX, lw(DEPTH), CP)
        # ---- 0x800B4D58
        sw(e + 388, u32(prevF << 20) | (m.lw(e + 388) & 0x000FFFEF))   # 0x800B4D84
        if s5 & 1:
            dx = u32(m.lw(P0) - m.lw(e + 184))
            a = m.call(F0b3ad0_FixMul, dx, dx)
            dz = u32(m.lw(P0 + 8) - m.lw(e + 192))
            d = s32(a + m.call(F0b3ad0_FixMul, dz, dz))
            if d < st['best']:
                st['bestSeg'] = s8; st['best'] = d
        if s7 == 8: return None, prevF, segCount
        # ---- 0x800B4DE0: the horizontal normal and the extent along the wall
        s1 = 2048
        if s5 & 0x10:
            st['L112'] = u32(lh(s8 + 32) << 4); sw(sp + 112, st['L112'])
            st['L116'] = u32(lh(s8 + 36) << 4); sw(sp + 116, st['L116'])
        else:
            a = u32(lh(NZ) << 4)
            st['L112'] = a; sw(sp + 112, a)
            st['L116'] = u32(-lh(NX) << 4); sw(sp + 116, st['L116'])
            if F0b3ad0_iabs(lh(NY)) >= 65:
                s0 = m.call(F0b3ad0_FixMul, a, a)
                v = m.call(F0b3ad0_FixMul, st['L116'], st['L116'])
                ss = u32(s0 + v)
                r = m.call(F0b3ad0_SqrtGte, ss)
                if s32(u32(r << 2)) < 0:
                    s0 = m.call(F0b3ad0_SqrtGte, ss)
                    v = m.call(F0b3ad0_SqrtGte, ss)
                    s0 = s32(u32(-u32(s0 << 2))) >> 1
                    v = s32(u32(-u32(v << 2) - 2)) >> 31
                    dv = u32(s0 + v)
                    q = 0xFFFFFFFF if dv == 0 else 0x80000000 // dv
                    k = u32(-q)
                else:
                    s0 = m.call(F0b3ad0_SqrtGte, ss)
                    v = m.call(F0b3ad0_SqrtGte, ss)
                    s0 = s32(u32(s0 << 2)) >> 1
                    v = s32(u32(u32(v << 2) - 2)) >> 31
                    dv = u32(s0 + v)
                    k = 0xFFFFFFFF if dv == 0 else 0x80000000 // dv
                st['L112'] = m.call(F0b3ad0_FixMul, st['L112'], k); sw(sp + 112, st['L112'])
                st['L116'] = m.call(F0b3ad0_FixMul, st['L116'], k); sw(sp + 116, st['L116'])
        L112, L116 = st['L112'], st['L116']
        a = m.call(F0b3ad0_FixMul, L112, u32(m.lw(CP) - m.lw(P0)))
        s3 = s32(a + m.call(F0b3ad0_FixMul, L116, u32(m.lw(CP + 8) - m.lw(P0 + 8))))
        a = m.call(F0b3ad0_FixMul, L112, u32(m.lw(P1) - m.lw(CP)))
        s2 = s32(a + m.call(F0b3ad0_FixMul, L116, u32(m.lw(P1 + 8) - m.lw(CP + 8))))
        if prevF & 0x400:
            s1 = s32(u32(F0b3ad0_div16(lh(e + 482)) * lw(e + 304)) + 2048)
        elif isView:
            if s5 & 0x10000:
                if s5 & 0x400: v = m.call(F0b3ad0_AiProject, P1, nrm, cur + 8)
                else: v = m.call(F0b3ad0_AiProject, P1, other + 32, other + 8)
                st['lim2'] = s32(v)
                ext = s32(u32(F0b3ad0_half(u32(m.lw(e + 308) * 3)) + u32(m.lw(e + 304) << 1)))
                if 0x8000 < F0b3ad0_iabs(st['lim2']):
                    st['lim2'] = s1
                    s1 = s32(s1 + ext)
                else:
                    st['lim2'] = s32(s1 + ext)
        if not (s5 & 0x10000): st['lim2'] = s1
        if s1 < F0b3ad0_iabs(s3) and st['lim2'] < F0b3ad0_iabs(s2) and (s3 ^ s2) < 0:
            return None, prevF, segCount
        # ---- 0x800B50DC
        t = prevF & 0xF
        special = t == 1 or (force != 0 and (prevF & 0x400))
        skipit = False
        if special:
            if (lhu(e + 172) >> 5) < 3: sw(P0 + 4, m.lw(e + 508))
            s0 = 0
            if t == 1 and lhu(s8 + 38) != 0:
                a3v, a2v = F0b3ad0_iabs(s3), F0b3ad0_iabs(s2)
                s3 = fdiv(a3v, s32(a3v + a2v))
                st['segVal'] = s3
                if lhu(s8 + 38) == 1: st['segVal'] = s32(0x10000 - s3)
            lvl = u32(m.lw(P0 + 4) - st['segVal'])
            s0 = 0
            while s0 < 8:
                if s32(lvl) < lw(e + 200 + 12 * s0): break
                s0 += 1
            if s0 >= 8:
                s1f = 1
                gs = m.lw(F0b3ad0_GS); hh = lhu(e + 172)
                if (hh >> 5) < 2 and (hh & 0x1F) < lw(gs + 48) and (prevF & 0x200):
                    sw(sp + 16, COUNT)
                    r = m.call(F0b3ad0_CornerMin, e + 196, NX, P0, 0, stack=(COUNT,))
                    if r != 8:
                        a = s32(m.call(F0b3ad0_DotLcm, HEAD, NX))
                        if F0b3ad0_iabs(a) < 22413:
                            s1f = 0
                            if a > 0: neg_n()
                            s5 &= 0xFFFFFFFE; st['s5'] = s5
                        elif lw(COUNT) == 8:
                            if isBike:
                                who = m.lw(e + 852)
                                vo = u32(F0b3ad0_VIEW + 1132 * lhu(e + 172))
                            else:
                                who = e
                                vo = u32(F0b3ad0_VIEW + 1132 * lhu(m.lw(e + 596) + 172))
                            sw(vo + 788, 0xFFFF0000)
                            sw(vo + 548, m.lw(vo + 548) | 0x40000)
                            sw(who + 552, m.lw(who + 552) | 0x80000)
                if s1f: return None, prevF, segCount
        # ---- 0x800B5380: the response vector I (sp+96)
        if s5 & 2:
            s0 = s5 & 1
            k = s32(((-s0) & 0xFFFFC000) + 8192)
            sw(sp + 16, k)
            m.call(F0b3ad0_Blend16To32, HEAD, NX, I, u32(lw(DEPTH) - st['travel'] - 4096), stack=(k,))
            if s0: neg_n()
        else:
            if (s5 & 5) == 5: neg_n()
            if (lhu(e + 172) >> 5) != 1 and not (s5 & 4):
                sw(DEPTH, lw(DEPTH) + 8192)
            if (prevF & 0x400) and not (s5 & 1):
                sw(DEPTH, u32(lw(DEPTH) + u32(F0b3ad0_div16(lh(e + 482)) << 13)))
            m.call(F0b3ad0_Scale, lw(DEPTH), NX, I)
        # ---- 0x800B54A0: apply it
        if isBike:
            if m.lw(st['ent2'] + 1088) == 0:
                st['ent2'] = m.lw(st['ent2'] + 856)
            e2 = st['ent2']
            old = m.lw(e2 + 552)
            sw(e2 + 552, st['tmin'] if u32(st['tmin']) >= old else old)   # 0x800B54FC max (unsigned)
            n = lw(F0b3ad0_CCNT)
            if (lhu(e + 320) & 4) and n < 8:
                rec = u32(F0b3ad0_CREC + 36 * n)
                sh(rec + 0, lhu(e2 + 172))
                idv = lhu(s8 + 0)
                sw(rec + 12, s8)
                sh(rec + 2, idv)
                sh(rec + 4, lhu(NX)); sh(rec + 6, lhu(NY))
                sh(rec + 10, prevF); sh(rec + 8, lhu(NZ))
                sw(rec + 16, m.lw(I)); sw(rec + 20, m.lw(I + 4))
                sw(rec + 28, st['segVal']); sw(rec + 32, m.lw(DEPTH)); sw(rec + 24, m.lw(I + 8))
                if isBike >= 2:
                    v = m.call(F0b3ad0_ContactMerge, rec)
                    sw(F0b3ad0_CCNT, u32(m.lw(F0b3ad0_CCNT) + v))
                else:
                    sw(F0b3ad0_CCNT, n + 1)
            else:
                sw(sp + 16, s8); sw(sp + 20, I)
                m.call(F0b3ad0_BikeWallHit, e2, NX, prevF, st['segVal'], stack=(s8, I))
        elif isView:
            if s5 & 0x20000:
                pv = u32(cur - step)
                a = s32(u32(lh(pv + 32) * (s32(u32(L112 << 12)) >> 16)))
                b = s32(u32(lh(pv + 36) * (s32(u32(L116 << 12)) >> 16)))
                st['cls'] = s32(F0b3ad0_div4(a) + F0b3ad0_div4(b))          # 184 reused as a signed side value
                s1v = s3
                x = m.call(F0b3ad0_FixMul, u32(m.lw(P1) - m.lw(P0)), L112)
                y = m.call(F0b3ad0_FixMul, u32(m.lw(P1 + 8) - m.lw(P0 + 8)), L116)
                s0 = s32(x + y)
                st['lim2'] = s0
                if (st['cls'] ^ s0) >= 0: s1v = s2
                s1v = s32(F0b3ad0_iabs(s1v) + 16384)
                if st['cls'] < 0: s1v = s32(-s1v)
                v = m.call(F0b3ad0_FixMul, L112, s1v)
                sw(I, u32(m.lw(I) + v))
                v = m.call(F0b3ad0_FixMul, L116, s1v)
                sw(I + 8, u32(m.lw(I + 8) + v))
            m.call(F0b3ad0_ApplyImpulse, e, I, 0)
            if m.lw(view + 768) == 0: sw(view + 768, s8)   # 0x800B574C
        else:
            m.call(F0b3ad0_ApplyImpulse, e, I, 1)
            hh = lhu(e + 172); pool = hh >> 5; slot = hh & 0x1F
            if pool == 2:
                ped = u32(m.lw(F0b3ad0_PEDB) + 572 * slot)
                sw(sp + 20, 0); sw(sp + 16, ped + 172)
                m.call(F0b3ad0_PedHit, ped, NX, 0, NX, stack=(ped + 172, 0))
            elif pool == 1:
                m.call(F0b3ad0_RiderWallHit, u32(m.lw(F0b3ad0_RIDB) + 628 * slot), NX, prevF)
            elif pool == 4:
                m.call(F0b3ad0_PropTopple, u32(m.lw(F0b3ad0_PROPB) + 596 * slot), 0, NX, 0xFFFFFFFF)
        # ---- 0x800B5858
        s7 |= 0x100
        st['result'] = s7
        if not isView:
            sw(e + 388, m.lw(e + 388) | ((st['s5'] & 1) << 4))       # 0x800B5984
            return s7, prevF, segCount
        return None, prevF, segCount

    # ------------------------------------------------------------------------------------------
    gi = 0                                         # 148
    if groups > 0:
        sw(sp + 232, HEAD); sw(sp + 240, P0)       # 0x800B3E0C/0x800B3E10 (reloaded before jals)
    while gi < groups:                             # 0x800B3E14
        if gi > 0:
            g = u32(m.lw(S + 100) + 4 * groupNo)
            segCount = lbu(g + 3); first = lh(g)
            if isView and m.lw(view + 768) != 0: return 1
        st['dirn'] = 1                             # 180
        base = u32(m.lw(S + 96) + 40 * first)
        endp = u32(base + 40 * segCount)
        cur = base                                 # 132
        other = u32(endp - 40)                     # 136
        R = m.lw(e + 340)
        s3 = s32(m.call(F0b3ad0_AiProject, base + 8, R + 2, R + 20))
        R = m.lw(e + 340)
        v = s32(m.call(F0b3ad0_AiProject, endp - 20, R + 2, R + 20))
        R = m.lw(e + 340)
        m.call(F0b3ad0_MulAdd, endp - 20, R + 2, u32(s3 - v), P1)
        s3 = s32(m.call(F0b3ad0_AiProject, base + 8, HEAD, e + 184))
        v = s32(m.call(F0b3ad0_AiProject, P1, HEAD, e + 184))
        if v < s3:
            st['dirn'] = -1; cur = other
        st['s5'] &= 0xFFFEF3FF
        prevF = 0                                  # 224 (u16)
        other = 0
        si = 0                                     # 144
        if segCount > 0:
            step = u32(st['dirn'] * 40)            # 236
            nrm = u32(cur + 32)                    # 252
            sw(sp + 252, nrm)
            while True:                            # 0x800B3F84
                t = lhu(cur + 2) & 0xF
                if t == 0 or t >= 9: sh(cur + 2, 1026)        # 0x800B3FB0
                code = None
                s8 = 0
                skip = False
                if isView:
                    s5 = st['s5']
                    if (lhu(cur + 2) & 0x100) == 0:
                        skip = True
                    else:
                        if m.lw(view + 768) != 0:
                            if (s5 & 0x10800) != 0x10000: return 1
                            m.call(F0b3ad0_ViewTrack, view, 1)
                            s5 |= 0x800
                        s5 &= 0xFFFDFFFF
                        if (s5 & 0x18) == 0:
                            if (s5 & 0x800) == 0: s5 &= 0xFFFEFFFF
                            if s5 & 0x400:
                                s8 = u32(other - step); s5 &= 0xFFFFFBFF
                            elif si < segCount - 1:
                                nx = u32(step + cur)
                                if lhu(nx + 2) & 0x100:
                                    other = nx
                                    for k in range(3):
                                        sw(P0 + 4 * k, F0b3ad0_half(m.lw(nx + 8 + 4 * k) + m.lw(nx + 20 + 4 * k)))
                                    v = m.call(F0b3ad0_AiProject, P0, nrm, cur + 20)
                                    sw(DEPTH, v)
                                    s3 = s32(m.call(F0b3ad0_DotLcm, nrm, HEAD))
                                    s2 = s32(m.call(F0b3ad0_DotLcm, other + 32, HEAD))
                                    if 0x8000 < lw(DEPTH):
                                        s5 |= 0x10400 if F0b3ad0_iabs(s3) < F0b3ad0_iabs(s2) else 0x10000
                                    else:
                                        b = 0 if (s5 & 0x10000) else (1 if s3 < -46333 else 0)
                                        s5 |= b << 10
                            if (s5 & 0x400) == 0 and si > 0:  # 0x800B41A8
                                pv = u32(cur - step)
                                if lhu(pv + 2) & 0x100:
                                    s2 = s32(m.call(F0b3ad0_DotLcm, nrm, pv + 32))
                                    s3 = s32(m.call(F0b3ad0_DotLcm, nrm, HEAD))
                                    b = 0 if 0xB4FC < s2 else (1 if 0xDDB2 < s3 else 0)
                                    s5 |= b << 17
                                    if s5 & 0x20000: s5 &= 0xFFFEFFFF
                            if s5 & 0x400: s8 = other     # 0x800B4240
                    st['s5'] = s5
                if not skip:
                    code, prevF, segCount = seg(cur, s8, nrm, step, other, prevF, segCount, groups)
                    if code == 'G': break
                    if code is not None: return code
                # 0x800B5868: next segment
                si += 1
                if (st['s5'] & 0x18) == 0:
                    nrm = u32(nrm + step); cur = u32(cur + step); sw(sp + 252, nrm)
                if not (si < segCount): break
        # 0x800B58B8: next group
        groupNo += 1
        gi += 1
    # 0x800B58DC
    if st['result'] != 0: return st['result']
    bs = st['bestSeg']
    if bs == 0: return 0
    R = m.lw(e + 340)
    sw(e + 388, m.lw(e + 388) | 0x10)             # 0x800B5914 (the jal's delay slot)
    x = m.call(F0b3ad0_AiProject, bs + 8, R + 2, R + 20)
    R = m.lw(e + 340)
    y = m.call(F0b3ad0_AiProject, bs + 20, R + 2, R + 20)
    x, y = F0b3ad0_iabs(x), F0b3ad0_iabs(y)
    d = x if y < x else y
    hi12 = s32(u32(d << 3)) >> 16
    sw(e + 388, u32(hi12 << 8) | (m.lw(e + 388) & 0xFFF000FF))   # 0x800B59B8
    return 0


# ==== 0x800ac5bc BikeVsTraffic ========================================
"""0x800AC5BC BikeVsTraffic(bike, car) - RASHCDG cfe43a7786759f2cb9c57751cf99e84d1074782c. Frame 96."""

F0ac5bc_FRAME = 96
F0ac5bc_TRIG = 0x8005624C     # {s16 sin, s16 cos} x 4096


def F0ac5bc_model(m, bike, car, a2, a3):
    sp = m.sp
    shape = u32(car + 0xAC)
    m.sw(sp + 64, 0)                                   # 0x800AC5DC
    s3 = 0
    if m.lw(bike + 0x340) == shape:                    # 0x800AC5E8
        s3 = m.lbu(bike + 0x235) >> 7                  # bit 31 of +0x234
    if not (m.lw(bike + 0x238) & 1):
        if m.lw(bike + 0x33C) == shape and s3 == 0:    # 0x800AC618 / 0x800AC620
            return None
    stale = m.call(0x800A8FE8, bike)                   # StaleHeading(bike) -> s0
    s0 = stale
    for o in (32, 28, 24, 60, 56):                     # 0x800AC640..0x800AC654
        m.sw(sp + o, 0)
    band = m.call(0x800A8C78, bike, 0x00140000, 0x001C0000)
    if band:
        m.sw(sp + 16, sp + 24)
        m.sw(sp + 20, sp + 64)
        s0 = m.call(0x800AAD30, bike, car, sp + 56, sp + 60, stack=(sp + 24, sp + 64))
        a0 = m.lw(sp + 56)
        a1 = (a0 >> 8) & 1
        if s0 != 0 and m.lw(bike + 0x358) != 0:
            h = m.lhu(s0 + 0xAC)
            a3v = 0
            ph = m.lhu(m.lw(bike + 0x358) + 0xAC)
            if h == ph and (a0 & 1) != ((a0 & 2) >> 1):          # 0x800AC6B0..0x800AC6C4
                a3v = 1
            elif h == m.lhu(bike + 0xAC) and (a0 & 0xFF) < 8 and (a0 & 1) == ((a0 & 2) >> 1):
                a3v = 1                                          # 0x800AC6CC..0x800AC6F0
            a1 &= a3v
            if s0 == m.lw(bike + 0x358):
                s0 = bike                              # 0x800AC70C
    else:
        if s0 != 0:                                    # rebuild the heading (0x800AC71C)
            ang = m.call(0x80020018, u32(m.lhs(bike + 0x1C2) << 4), u32(m.lhs(bike + 0x1C6) << 4))
            m.sw(bike + 0x124, ang)
            m.sw(bike + 0x128, u32(m.lhs(F0ac5bc_TRIG + ((ang & 0xFFF) << 2) + 2) << 4))
            ang2 = m.lw(bike + 0x124)
            m.sw(bike + 0x12C, u32(m.lhs(F0ac5bc_TRIG + ((ang2 & 0xFFF) << 2)) << 4))
        m.sw(sp + 16, sp + 24)
        s0 = m.call(0x800AA140, bike, car, sp + 56, sp + 60, stack=(sp + 24,))
        a1 = 0
    a0 = m.lw(sp + 56)
    if a0 == 0 and s3 == 0:
        return None
    if s3 != 0:
        m.sw(sp + 32, 0); m.sw(sp + 28, 0); m.sw(sp + 24, 0)
    else:
        if a1 != 0:
            m.sw(sp + 16, sp + 24)
            v = m.call(0x800A91AC, a0, m.lw(sp + 60), bike, car, stack=(sp + 24,))
            m.sw(sp + 60, v)
        if s0 == u32(car):                             # negate the impulse (0x800AC7F4)
            m.sw(sp + 24, -s32(m.lw(sp + 24)))
            m.sw(sp + 32, -s32(m.lw(sp + 32)))
            m.sw(sp + 28, -s32(m.lw(sp + 28)))
    mag = s32(m.lw(sp + 64))
    if mag > 0:                                        # 0x800AC828
        k = m.call(0x8001FC90, m.lw(car + 0x1E0), mag)
        m.call(0x8002EE50, k, car + 0x1C2, sp + 40)
        m.call(0x800A8DF0, bike, sp + 40, 1)
        m.call(0x800A8DF0, car, sp + 40, 1)
        v1 = m.lw(sp + 64)
        a0w = m.lw(bike + 0x228)
        if v1 < a0w:                                   # sltu: unsigned max
            v1 = a0w
        m.sw(bike + 0x228, v1)
    defer = False
    if s3 == 0 and (m.lhu(bike + 0x140) & 4):
        n = s32(m.lw(0x800CCF68))
        if n < 8:
            defer = True
    if not defer:
        m.sw(sp + 16, sp + 24)
        m.call(0x800AC958, bike, car, m.lw(sp + 56), m.lw(sp + 60), stack=(sp + 24,))
        return None
    rec = u32(0x800CCE48 + 36 * n)                     # 0x800AC8DC: append a ContactRec
    m.sh(rec + 0, m.lhu(bike + 0xAC))
    m.sh(rec + 2, m.lhu(car + 0xAC))
    m.sw(rec + 16, m.lw(sp + 24))
    m.sw(0x800CCF68, n + 1)
    m.sw(rec + 20, m.lw(sp + 28))
    m.sw(rec + 28, m.lw(sp + 56) | u32(m.lw(sp + 60) << 16))
    m.sw(rec + 24, m.lw(sp + 32))
    return None


# ==== 0x800a91ac TrafficSideShove ========================================
"""0x800A91AC TrafficSideShove(code, flags, bike, car, [imp]) - RASHCDG cfe43a77... Frame 64.
Returns the (possibly replaced) flags word."""


def F0a91ac_tdiv(x, d):
    """C truncating division of a signed 32-bit value by a power of two, as (x + (x<0 ? d-1 : 0)) >> k"""
    return -((-x) // d) if x < 0 else x // d


def F0a91ac_model(m, code, flags, bike, car):
    sp = m.sp
    imp = m.arg(4)                                     # lw s1,80(sp)
    s2 = flags
    if not (flags & 0x200):
        return s2
    if (flags & 0xFF) not in (3, 1):
        return s2
    c = bike + 0xC4 + 12 * (code & 0xFF)               # corner number (code & 0xFF)
    m.sw(sp + 16, u32(m.lw(imp + 0) + m.lw(c + 0)))
    m.sw(sp + 20, u32(m.lw(imp + 4) + m.lw(c + 4)))
    m.sw(sp + 24, u32(m.lw(imp + 8) + m.lw(c + 8)))
    # s4 = bike.1C2 * car.1C0 - bike.1C6 * car.1BC  (mult, mflo, subu)
    s4 = s32(u32(m.lhs(bike + 0x1C2) * m.lhs(car + 0x1C0) - m.lhs(bike + 0x1C6) * m.lhs(car + 0x1BC)))
    p = s32(m.call(0x800B6AAC, sp + 16, car + 0x1B0, car + 0xB8))      # AiProject
    w = s32(m.lw(car + 0x130))
    a1 = s32(u32(3 * w))
    a1 = (a1 + 3 if a1 < 0 else a1) >> 2                              # (3w)/4 truncating
    w8 = (w + 7 if w < 0 else w) >> 3                                  # w/8 truncating
    done = False
    if a1 < p and p < s32(u32(w + w8)):
        if s4 > 0:
            m.call(0x8002EE50, u32(p - a1), car + 0x1B0, sp + 16)      # Scale
            s2 = 514
            done = True
    if not done:
        if not (p < s32(u32(-a1))):
            return s2
        w = s32(m.lw(car + 0x130))
        w8 = (w + 7 if w < 0 else w) >> 3
        if not (s32(u32(-w - w8)) < p):
            return s2
        if s4 >= 0:
            return s2
        m.call(0x8002EE50, u32(p + a1), car + 0x1B0, sp + 16)
        s2 = 512
    d = s32(m.call(0x800B6AAC, bike + 0x1D4, car + 0x1BC, car + 0x1D4))
    sg = d >> 31
    ad = s32(u32((sg + d) ^ sg))                                       # abs, 0x80000000 stays
    lim = s32(u32(m.lw(car + 0x134) + m.lw(bike + 0x134) - 4096))
    if ad < lim:
        m.sw(imp + 0, m.lw(sp + 16))
        m.sw(imp + 4, m.lw(sp + 20))
        m.sw(imp + 8, m.lw(sp + 24))
    else:
        m.sw(imp + 0, u32(m.lw(imp + 0) + m.lw(sp + 16)))
        m.sw(imp + 4, u32(m.lw(imp + 4) + m.lw(sp + 20)))
        m.sw(imp + 8, u32(m.lw(imp + 8) + m.lw(sp + 24)))
    return s2


# ==== 0x800ac958 BikeTrafficReact ========================================
"""0x800AC958 BikeTrafficReact(bike, car, code, flags, [imp]) - RASHCDG cfe43a77... Frame 80.
The car is passed as the ENTITY (the callee 0x800AF224 gets &car->AC)."""

F0ac958_TRIG = 0x8005624C


def F0ac958_clamp16(v):
    return -0x8000 if v < -0x8000 else (0x7FFF if v > 0x7FFF else v)


def F0ac958_model(m, bike, car, code, flags):
    sp = m.sp
    imp = m.arg(4)                                            # lw v0,96(sp)
    s4 = flags
    s3 = 6
    m.sw(sp + 16, imp)
    r = m.call(0x800AF224, bike, car + 0xAC, code, flags, stack=(imp,))
    s0 = None
    if r != 0:
        if not (s4 & 0x200):                                   # 0x800ACA20
            v = s32(m.call(0x800B6B58, car + 0xB8, bike + 0x204, bike + 0x1F8))
            s3 = (1 if 0 < v else 0) << 1
        else:
            m.sw(sp + 16, 0)
            m.call(0x800B675C, car + 0xC4, car + 0x1B0, s4 & 0xFF, sp + 24, stack=(0,))
            d = s32(m.call(0x8002E698, bike + 0x1C2, sp + 24))  # DotLcm
            if 0xDDB2 < d:
                s3 = 1
            elif d < -0xDDB2:
                s3 = 3
            else:
                x = s32(u32(m.lhs(bike + 0x1C2) * m.lhs(sp + 28)))
                y = s32(u32(m.lhs(bike + 0x1C6) * m.lhs(sp + 24)))
                s3 = (0 if x < y else 1) << 1
        if (s3 & 5) == 0:                                      # s3 in {0, 2}
            # NOTE: on the flags&0x200 == 0 path sp+24/sp+28 were never written by this call
            t = m.call(0x8001FC90, m.lw(bike + 0x128), u32(m.lhs(sp + 28) << 4))
            v = m.call(0x8001FC90, m.lw(bike + 0x12C), u32(m.lhs(sp + 24) << 4))
            a0 = u32(-s32(t) - s32(v))
        else:
            t = m.call(0x8001FC90, m.lw(bike + 0x128), m.lw(car + 0x128))
            v = m.call(0x8001FC90, m.lw(bike + 0x12C), m.lw(car + 0x12C))
            a0 = u32(t + v)
        k = m.call(0x8001FC90, a0, u32(m.lw(bike + 0x1E0) - m.lw(car + 0x1E0)))
        k = s32(k)
        sg = k >> 31
        s0 = u32((sg + k) ^ sg)                                 # abs
        m.sw(sp + 16, s3)
        sev = s32(m.call(0x800A9408, bike, s0, 0x50000, 0x10000, stack=(s3,)))
        if s32(m.lw(bike + 0x1E0)) > 0x1017E or s32(m.lw(car + 0x1E0)) > 0x1017E:
            a2 = 48
            if sev < 3 and not (m.lw(bike + 0x234) & 0x8000):
                if sev < 2:
                    rnd = m.call(0x80043F00, 0xF2000002)
                    a2 = ((rnd & 0xFF) >> 6) + 50
                else:
                    a2 = 18
            m.call(0x80017BA0, m.lw(car + 0xB8), m.lw(car + 0xC0), a2, 0)   # PlaySound3D
            gs = m.lw(0x8005B2F8)
            if (m.lhu(bike + 0xAC) < m.lw(gs + 48) and m.lw(m.lw(bike + 0x354) + 0x25C) < 2
                    and m.lw(0x8005B220) == 0):
                a2 = s0
                if m.lw(bike + 0x238) & 0x20D:
                    a2 = m.lw(bike + 0x1E0)
                m.sw(sp + 16, 1)
                m.call(0x800B658C, bike, 0, a2, 0x00165A1C, stack=(1,))      # pad rumble
        if m.lw(bike + 0x238) & 0x20E:                         # 0x800ACBD4
            go = (not (s4 & 0x200)) or (s4 & 0xFF) in (1, 3)
            if go:
                m.sw(car + 0x1E4, 0)
                if (m.lw(bike + 0x238) & 2) and (s4 & 0xFF) == 1:
                    v = u32(m.lw(car + 0x1E0) - s0)
                    m.sw(car + 0x1E0, v)
                    if s32(v) < 0:
                        v = 0
                    m.sw(car + 0x1E0, v)
                else:
                    b = m.lbu(car + 0x1FD)
                    m.sw(car + 0x1E0, 0)
                    m.sb(car + 0x1FD, b | 0x10)
    # 0x800ACC58
    f234 = m.lw(bike + 0x234)
    if (f234 & 0x28000) == 0x8000:
        if m.lw(bike + 0x268) == 0 and m.lw(bike + 0x304) != 0:
            if f234 & 0x40000:
                if m.lw(bike + 0x1E8) != 0:                    # 0x800ACCB0
                    a = m.call(0x8002E698, bike + 0x1C2, car + 0x1B0)
                    b = m.call(0x8002E698, bike + 0x1C2, car + 0x1BC)
                    m.sw(bike + 0x2B4, m.call(0x80020018, a, b))
                else:                                          # 0x800ACCE0
                    ang = m.lw(bike + 0x2B4) & 0xFFF
                    c = u32(m.lhs(F0ac958_TRIG + (ang << 2) + 2) << 4)
                    s = u32(m.lhs(F0ac958_TRIG + (ang << 2)) << 4)
                    m.sw(sp + 16, s)
                    m.call(0x8002EB78, car + 0x1BC, car + 0x1B0, bike + 0x210, c, stack=(s,))  # Blend16
                    # GTE: RT11/RT22/RT33 = bike+0x20A[0..2], IR1..3 = bike+0x210[0..2], OP sf=1 lm=0
                    r11, r22, r33 = m.lhs(bike + 0x20A), m.lhs(bike + 0x20C), m.lhs(bike + 0x20E)
                    i1, i2, i3 = m.lhs(bike + 0x210), m.lhs(bike + 0x212), m.lhs(bike + 0x214)
                    o1 = F0ac958_clamp16((r22 * i3 - r33 * i2) >> 12)
                    o2 = F0ac958_clamp16((r33 * i1 - r11 * i3) >> 12)
                    o3 = F0ac958_clamp16((r11 * i2 - r22 * i1) >> 12)
                    m.sh(bike + 0x204, o1); m.sh(bike + 0x206, o2); m.sh(bike + 0x208, o3)
                    m.call(0x8002E468, bike + 0x204)                  # Normalize (a1 not set)
            else:                                              # 0x800ACD98
                d = m.call(0x8002E698, car + 0x1C2, bike + 0x1C2)
                k = m.call(0x8001FC90, m.lw(car + 0x1E0), d)
                v1 = u32(m.lw(bike + 0x240) - k)
                m.sw(bike + 0x240, v1)
                if s32(v1) < 0x23C36:
                    v1 = 0x23C36
                m.sw(bike + 0x240, v1)
                m.sw(bike + 0x1E0, v1)
                dx = u32(m.lw(bike + 0x1F8) - m.lw(car + 0xB8)); m.sw(sp + 32, dx)
                dy = u32(m.lw(bike + 0x1FC) - m.lw(car + 0xBC)); m.sw(sp + 36, dy)
                dz = u32(m.lw(bike + 0x200) - m.lw(car + 0xC0)); m.sw(sp + 40, dz)
                for dst, ax in ((0x308, 0x1B0), (0x30C, 0x1BC)):
                    acc = 0
                    for k3, dv in enumerate((dx, dy, dz)):
                        p = s32(dv) * (m.lhs(car + ax + 2 * k3) << 4)
                        p &= 0xFFFFFFFFFFFFFFFF
                        lo, hi = p & 0xFFFFFFFF, p >> 32
                        acc = u32(acc + ((lo >> 16) | (hi << 16)))
                        if k3 == 2:
                            m.sw(sp + 48, lo); m.sw(sp + 52, hi)
                    m.sw(bike + dst, acc)
                a = m.call(0x8002E698, bike + 0x1C2, car + 0x1B0)
                b = m.call(0x8002E698, bike + 0x1C2, car + 0x1BC)
                ang = m.call(0x80020018, a, b)
                f = m.lw(bike + 0x234)
                m.sw(bike + 0x2B4, ang)
                m.sw(bike + 0x234, f | 0x40000)
        elif m.lw(bike + 0x234) & 0x40000:                    # 0x800ACF6C (re-read)
            d = m.call(0x8002E698, car + 0x1C2, bike + 0x1C2)
            k = m.call(0x8001FC90, m.lw(car + 0x1E0), d)
            v1 = u32(m.lw(bike + 0x240) + k)
            m.sw(bike + 0x240, v1)
            if s32(v1) < 0x23C36:
                v1 = 0x23C36
            m.sw(bike + 0x240, v1)
            m.sw(bike + 0x1E0, v1)
            m.call(0x8002EE50, v1, bike + 0x1C2, bike + 0x1C8)          # Scale
            m.sw(bike + 0x234, m.lw(bike + 0x234) & 0xFFFBFFFF)
    # 0x800ACFE8
    if ((m.lw(bike + 0x24) >> 25) & 3) < 2:
        if s3 == 0:
            m.call(0x80027258, bike, 4)
        elif s3 == 2:
            m.call(0x80027258, bike, 3)
    return None


# ==== 0x800ac130 BikeBikeReact ========================================
"""0x800AC130 BikeBikeReact(a, b, codeA, codeB) - RASHCDG cfe43a77... Frame 72."""


def F0ac130_absw(k):
    k = s32(k)
    sg = k >> 31
    return u32((sg + k) ^ sg)


def F0ac130_model(m, a, b, codeA, codeB):
    sp = m.sp
    s7 = s8 = 8
    s4 = s5 = 6
    if not (m.lw(a + 0x238) & 1) and not (m.lw(b + 0x238) & 1):
        if m.lw(a + 0x33C) == u32(b + 0xAC) or m.lw(b + 0x33C) == u32(a + 0xAC):
            return None
    s6 = 1 if (s32(m.lw(a + 0x1E0)) > 0x1017E or s32(m.lw(b + 0x1E0)) > 0x1017E) else 0
    ok = 0
    if m.lhu(m.lw(a + 0x354) + 0x220) != 0 and not (m.lw(a + 0x238) & 0xE):
        if m.lhu(m.lw(b + 0x354) + 0x220) != 0:
            ok = 1 if (m.lw(b + 0x238) & 0xE) == 0 else 0
    if not ok:
        return None
    if codeB & 0x200:
        if codeA & 0x200:
            s4 = codeA & 0xFF
        else:
            s7 = codeA & 0xFF
        s5 = codeB & 0xFF
        m.sw(sp + 16, 0)
        m.call(0x800B675C, b + 0xC4, b + 0x1B0, s5, sp + 24, stack=(0,))
    else:
        s4 = codeA & 0xFF
        m.sw(sp + 16, 0)
        m.call(0x800B675C, a + 0xC4, a + 0x1B0, s4, sp + 24, stack=(0,))
        s8 = codeB & 0xFF
        v0, v1 = m.lhu(sp + 24), m.lhu(sp + 28)
        m.sh(sp + 24, -v0)
        v0 = m.lhu(sp + 26)
        m.sh(sp + 28, -v1)
        m.sh(sp + 26, -v0)
    m.sw(sp + 16, sp + 24)
    r = m.call(0x80081D7C, a, b, codeA, codeB, stack=(sp + 24,))
    if r == 0:
        return None
    t = m.call(0x8001FC90, m.lw(a + 0x128), m.lw(b + 0x128))
    v = m.call(0x8001FC90, m.lw(a + 0x12C), m.lw(b + 0x12C))
    s3 = u32(t + v)
    k = m.call(0x8001FC90, s3, m.lw(b + 0x1E0))
    s0 = F0ac130_absw(u32(m.lw(a + 0x1E0) - k))
    if s4 == 6:
        s4 = m.call(0x800AC56C, s3, s7, s5)
    m.sw(sp + 16, s4)
    m.call(0x800A9408, a, s0, m.lw(b + 0x13C), m.lw(a + 0x13C), stack=(s4,))
    if s6:
        gs = m.lw(0x8005B2F8)
        if (m.lhu(a + 0xAC) < m.lw(gs + 48) and m.lw(m.lw(a + 0x354) + 0x25C) < 2
                and m.lw(0x8005B220) == 0):
            a2 = m.lw(a + 0x1E0)
            if m.lw(a + 0x238) & 0x20D:
                a2 = s0
            m.sw(sp + 16, 1)
            m.call(0x800B658C, a, 0, a2, 0x00165A1C, stack=(1,))
    k = m.call(0x8001FC90, s3, m.lw(a + 0x1E0))
    s0 = F0ac130_absw(u32(m.lw(b + 0x1E0) - k))
    if s5 == 6:
        s5 = m.call(0x800AC56C, s3, s8, s4)
    m.sw(sp + 16, s5)
    sev = s32(m.call(0x800A9408, b, s0, m.lw(a + 0x13C), m.lw(b + 0x13C), stack=(s5,)))
    if s6:
        if sev >= 3:
            a2 = 48
        elif sev >= 2:
            a2 = 18
        else:
            rnd = m.call(0x80043F00, 0xF2000002) & 0xFF
            a2 = ((rnd * 5) >> 8) + 50
        m.call(0x80017BA0, m.lw(b + 0xB8), m.lw(b + 0xC0), a2, 0)
        gs = m.lw(0x8005B2F8)
        if (m.lhu(b + 0xAC) < m.lw(gs + 48) and m.lw(m.lw(b + 0x354) + 0x25C) < 2
                and m.lw(0x8005B220) == 0):
            a2 = m.lw(b + 0x1E0)
            if m.lw(b + 0x238) & 0x20D:
                a2 = s0
            m.sw(sp + 16, 1)
            m.call(0x800B658C, b, 0, a2, 0x00165A1C, stack=(1,))
    m.call(0x800A8BE0, m.lhu(b + 0xAC), a + 0x390)
    m.call(0x800A8BE0, m.lhu(a + 0xAC), b + 0x390)
    return None


# ==== 0x800a9408 ImpactSeverity ========================================
"""0x800A9408 ImpactSeverity(e, mag, num, den, [mode]) - RASHCDG cfe43a77... Frame 56.
Returns the severity 0..4 = clamp(5*mag / (*(e+0x22C))->+0xE0, 0, 4)."""

F0a9408_RIDER_STATE_TAB = 0x800541D4      # 8-byte records indexed by the rider-state id, u16 at +2


def F0a9408_mips_div(n, d):
    n, d = s32(n), s32(d)
    if d == 0:
        return 0xFFFFFFFF if n >= 0 else 1
    if n == -0x80000000 and d == -1:
        return 0x80000000
    q = abs(n) // abs(d)
    return u32(q if (n < 0) == (d < 0) else -q)


def F0a9408_model(m, e, mag, num, den):
    mode = m.arg(4)                                   # lw s5,72(sp)
    rider = m.lw(e + 0x354)
    s4 = 1 if (m.lw(e + 0x238) & 0x22C) else 0
    s1 = num
    s3 = 21
    s7 = 0
    if s4 and den != 0x10000:                         # s1 = num/den with explicit sign handling
        n, d = s32(num), s32(den)
        if n > 0:
            if d > 0:
                s1 = m.call(0x80010028, n, d)
            else:
                s1 = u32(-s32(m.call(0x80010028, n, -d)))
        else:
            if d <= 0:
                s1 = m.call(0x80010028, -n, -d)
            else:
                s1 = u32(-s32(m.call(0x80010028, -n, d)))
    q = s32(F0a9408_mips_div(u32(5 * s32(mag)), m.lw(m.lw(e + 0x22C) + 0xE0)))
    a0 = q & ~(q >> 31)                                # max(q, 0)
    v1 = s32(u32(4 - q))
    s0 = s32(u32(a0 + ((v1 >> 31) & v1)))              # + min(4 - q, 0)
    if mode == 0:
        s7 = 256
    st = m.lw(rider + 0x25C)
    if st == 1:
        sid = m.lhu(rider + 0x220)
        ok = u32(sid - 26) >= 12
        if ok and m.lhu(F0a9408_RIDER_STATE_TAB + (sid << 3) + 2) == 3:
            ok = False
        if ok and m.lhu(rider + 0x260) != 0:
            sid2 = m.lhu(rider + 0x262)
            if u32(sid2 - 26) < 12:
                ok = False
            elif m.lhu(F0a9408_RIDER_STATE_TAB + (sid2 << 3) + 2) == 3:
                ok = False
        if ok:
            if mode == 1:
                s3 = 27
            elif mode == 0 or mode == 2:
                s3 = 26
            elif mode == 3:
                s3 = 28
            m.call(0x800C4550, s3, rider, s7 | 0xA)    # the unported stance call
    if s4:
        rec = m.lw(e + 0x43C)
        old = m.lbu(rec + 0x25)
        prod = s32(u32(s0 * s32(s1)))                  # mult s0,s1 ; mflo
        a1 = s32(u32(old - (prod >> 16)))
        if a1 <= 0:
            m.sb(rec + 0x25, 0)
        else:
            f = m.lbu(rec + 0x44)
            flag = False
            if (old & 0x80) and a1 < 128:
                flag = True
            elif not (old < 64) and a1 < 64:
                flag = True
            if flag:
                f |= 0x40
            m.sb(rec + 0x44, f)
            m.sb(m.lw(e + 0x43C) + 0x25, a1)
    return s0


# ==== 0x800a77b0 DeferredContacts ========================================
"""0x800A77B0 DeferredContacts() - RASHCDG cfe43a77... Frame 64. No stores outside its frame."""

F0a77b0_COUNT = 0x800CCF68
F0a77b0_RECS = 0x800CCE48
F0a77b0_CHAIN = 0x800CCF70
F0a77b0_BIKES = 0x8005B3A0         # -> pool-0 base, stride 1096


def F0a77b0_chainbits(m, h):
    return (m.lw(F0a77b0_CHAIN + ((h >> 4) << 2)) >> ((h & 0xF) << 1)) & 3


def F0a77b0_model(m, a0, a1, a2, a3):
    sp = m.sp
    s4 = 0
    s2 = s32(m.lw(F0a77b0_COUNT)) - 1
    while s2 >= 0:                                      # backwards over the list
        r = u32(F0a77b0_RECS + 36 * s2)
        hb = m.lhu(r + 2)
        pool = hb >> 5
        if pool != 0:
            e = u32(m.lw(F0a77b0_BIKES) + 1096 * m.lhu(r + 0))
            if pool == 8:
                m.sw(sp + 20, r + 16)
                m.sw(sp + 16, m.lw(r + 12))
                m.call(0x800B12A0, e, r + 4, m.lhu(r + 10) & 0xFFF, m.lw(r + 28),
                       stack=(m.lw(r + 12), r + 16))
            elif pool == 3:
                car = u32(0x800CF660 + ((hb & 0x1F) << 9))
                w = m.lw(r + 28)
                m.sw(sp + 16, r + 16)
                m.call(0x800AC958, e, car, w & 0xFFFF, s32(w) >> 16, stack=(r + 16,))
            else:
                if pool == 6:
                    shp = u32(m.lw(0x800CD6A8 + 28) + 280 * (hb & 0x1F))
                else:
                    shp = u32(m.lw(0x800CE598 + 12) + 452 * (hb & 0x1F) + 0xAC)
                if (m.lhu(shp) >> 5) == 6 and m.lw(shp + 8) == 1:
                    f = m.lhu(r + 10)
                    m.sw(sp + 20, r + 16)
                    m.sw(sp + 16, f & 0x10)
                    m.call(0x800AF0A0, e, shp, r + 4, f & 0xFFEF, stack=(f & 0x10, r + 16))
                else:
                    w = m.lw(r + 28)
                    m.sw(sp + 16, r + 16)
                    m.sw(sp + 20, r + 4)
                    m.call(0x800B11B4, e, shp, w & 0xFFFF, s32(w) >> 16, stack=(r + 16, r + 4))
            h = m.lhu(e + 0xAC)
            s4 |= u32(1 << (h & 31))                        # sllv: shift by h mod 32
            if F0a77b0_chainbits(m, h):
                m.call(0x800A7AB4, e, r + 16, 31)
        s2 -= 1
    i = 0
    while i < s32(m.lw(F0a77b0_COUNT)):                          # forwards: the bike x bike records
        r = u32(F0a77b0_RECS + 36 * i)
        if (m.lhu(r + 2) >> 5) == 0:
            base = m.lw(F0a77b0_BIKES)
            e = u32(base + 1096 * m.lhu(r + 0))
            if not ((s4 >> (m.lhu(e + 0xAC) & 31)) & 1):
                w = m.lw(r + 28)
                o = u32(base + 1096 * m.lhu(r + 2))
                m.call(0x800AC130, e, o, w & 0xFFFF, s32(w) >> 16)
        i += 1
    return None


# ==== 0x800a7ab4 ChainReaction ========================================
"""0x800A7AB4 ChainReaction(e, vec, skipHandle) - RASHCDG cfe43a77... Frame 48. No own stores."""

F0a7ab4_CHAIN = 0x800CCF70
F0a7ab4_NBIKES = 0x8005B1F8
F0a7ab4_BIKES = 0x8005B3A0


def F0a7ab4_model(m, e, vec, skip, a3):
    s1 = 0
    if s32(m.lw(F0a7ab4_NBIKES)) <= 0:
        return None
    while True:
        a2 = s1 & 0xFFFF
        if a2 != m.lhu(e + 0xAC) and a2 != (skip & 0xFFFF):
            mine = (m.lw(F0a7ab4_CHAIN + ((a2 >> 4) << 2)) >> ((s1 & 0xF) << 1))
            eh = m.lhu(e + 0xAC)
            his = (m.lw(F0a7ab4_CHAIN + ((eh >> 4) << 2)) >> ((eh & 0xF) << 1)) & 3
            if mine & his:
                o = u32(m.lw(F0a7ab4_BIKES) + 1096 * a2)
                if a2 < eh:
                    r = m.call(0x800AB7A0, e, o, 1)
                else:
                    r = m.call(0x800AB7A0, o, e, 1)
                if r != 0:
                    m.call(0x800A8DF0, o, vec, 1)
        s1 += 1
        if not ((s1 & 0xFFFF) < s32(m.lw(F0a7ab4_NBIKES))):
            break
    return None


# ==== 0x800ad04c BikeVsRider ========================================
"""0x800AD04C  bike x rider / pedestrian resolver (RASHCDG cfe43a77..., frame 136).  F0ad04c_model(m, e, p)"""

F0ad04c_FRAME = 136
F0ad04c_RIDER_BASE_PTR = 0x8005B3A4      # -> pool-1 base (stride 628)
F0ad04c_P1_BIKE = 0x8005B38C
F0ad04c_RUMBLE_OFF = 0x8005B220
F0ad04c_STATE_TAB = 0x800541D4           # SLUS: 8-byte records per rider state, +2 u16 category


def F0ad04c_model(m, a0, a1, a2, a3):
    e, p = a0, a1
    sp = m.sp
    m.sw(sp + 88, 0)
    s6 = D_sticky(m, e, u32(p + 0xAC))
    s4 = 0
    h = m.lhu(p + 0xAC)
    if (h >> 5) == 1:
        s4 = u32(m.lw(F0ad04c_RIDER_BASE_PTR) + 628 * (h & 0x1F))
    s3 = 0
    if s4:
        own = m.lw(e + 0x354)
        hs4 = m.lhu(s4 + 0xAC)
        if m.lhu(own + 0xAC) == hs4:
            s3 = 1
        elif m.lbu(own + 0x23C) & 0x10:
            if m.lhu(m.lw(m.lw(e + 0x358) + 0x354) + 0xAC) == hs4:
                s3 = 1
        if s4:
            if m.lw(s4 + 0x25C) == 2 or (m.lw(e + 0x238) & 0xEC):
                if s3:
                    return None
    if m.lw(e + 0x33C) == u32(p + 0xAC) and not s6:
        return None
    s0 = m.call(D_STALE, e)
    if m.call(D_INBAND, e, 0x140000, 0x1C0000):
        m.sw(sp + 16, sp + 24)
        m.sw(sp + 20, sp + 88)
        s5 = m.call(D_AAD30, e, p, sp + 80, sp + 84, stack=(sp + 24, sp + 88))
        if s5 and s5 == m.lw(e + 0x358):
            s5 = e
        mag = m.lw(sp + 88)
        if s32(mag) > 0:
            v = m.call(D_FIXMUL, m.lw(e + 0x1E0), mag)
            m.call(D_SCALE, v, e + 0x1C2, sp + 56)
            m.call(D_APPLY, e, sp + 56, 1)
            m.call(D_APPLY, p, sp + 56, 1)
            D_maxu_228(m, e, m.lw(sp + 88))
    else:
        if s0:
            D_rebuild_heading(m, e)
        m.sw(sp + 16, sp + 24)
        s5 = m.call(D_AA140, e, p, sp + 80, sp + 84, stack=(sp + 24,))
    if s5:
        if m.lhu(s5 + 0xAC) == m.lhu(e + 0xAC):
            s0, a2_ = p, m.lw(sp + 84)
        else:
            s0, a2_ = e, m.lw(sp + 80)
        m.sw(sp + 16, 0)
        m.call(D_B675C, s0 + 0xC4, s0 + 0x1B0, a2_ & 0xFF, sp + 72, stack=(0,))
        if s0 == p:
            for o in (72, 76, 74):
                m.sh(sp + o, -m.lhu(sp + o))
    # L_358
    kill = False
    if m.lw(sp + 84) and s4 and s3:
        f238 = m.lw(e + 0x238)
        if m.lw(e + 0x1E0) == 0 and u32(m.lhu(s4 + 0x220) - 72) < 2:
            kill = True
        elif f238 & 0xE0:
            kill = True
        elif (m.lw(s4 + 0x228) & 0x40000000) and (f238 & 0x600):
            kill = True
        elif m.lbu(m.lw(D_GAME) + 57) == 2 and s4 == m.lw(m.lw(m.lw(F0ad04c_P1_BIKE) + 0x358) + 0x354):
            kill = True
        if kill:
            m.sw(sp + 84, 0)
            s3 = 0
        else:
            s3 = m.lw(sp + 84)
    else:
        s3 = m.lw(sp + 84)
    if s3 or s6:
        if not (m.lw(e + 0x238) & 0x02000000):
            m.sh(e + 0x360, m.lhu(e + 0x1C2))
            m.sh(e + 0x362, m.lhu(e + 0x1C4))
            m.sh(e + 0x364, m.lhu(e + 0x1C6))
            m.sw(e + 0x35C, m.lw(e + 0x1E0))
            m.sw(e + 0x238, m.lw(e + 0x238) | 0x02000000)
        for k in range(3):
            m.sw(sp + 40 + 4 * k, u32(m.lw(p + 0xB8 + 4 * k) - m.lw(e + 0xB8 + 4 * k)))
        m.call(D_SCALE, m.lw(p + 0x1E0), p + 0x1C2, sp + 56)
        for k in range(3):
            m.sw(sp + 56 + 4 * k, u32(m.lw(sp + 56 + 4 * k) - m.lw(e + 0x1C8 + 4 * k)))
        s3 = 0
        if s6 or not (m.lw(p + 0x228) & 0x20000000):
            s3 = 1
        elif s32(m.call(0x8002E604, sp + 40, sp + 56)) < 0:
            s3 = 1
        if s3:
            if not s6 and (m.lw(p + 0x228) & 0x40000000):
                # branch B
                if s4 and not (m.lw(s4 + 0x228) & 0x20) and not (m.lw(e + 0x230) & 0x20000000):
                    t = D_fixdot32(m.lw(sp + 40), m.lw(sp + 44), m.lw(sp + 48),
                                 m.lhs(e + 0x20A) << 4, m.lhs(e + 0x20C) << 4, m.lhs(e + 0x20E) << 4)
                    if s32(t) < s32(0xFFFF4000):
                        m.call(0x800C29F0, s4, e)
            else:
                # branch A: lowest corner of p
                s3v = m.lw(p + 0x11C)
                a2c = 7
                for v1 in range(6, -1, -1):
                    y = m.lw(p + 0xC4 + 12 * v1 + 4)
                    if s32(y) < s32(s3v):
                        a2c = v1
                        s3v = y
                f138 = m.lw(p + 0x138)
                old134 = m.lw(p + 0x134)
                m.sw(p + 0x134, f138)
                c = p + 0xC4 + 12 * a2c
                for k in range(3):
                    m.sw(sp + 40 + 4 * k, u32(m.lw(c + 4 * k) - m.lw(p + 0xB8 + 4 * k)))
                t = D_fixdot32(m.lw(sp + 40), m.lw(sp + 44), m.lw(sp + 48),
                             m.lhs(e + 0x20A) << 4, m.lhs(e + 0x20C) << 4, m.lhs(e + 0x20E) << 4)
                m.sw(p + 0x138, t)
                at = abs(s32(t)) if s32(t) != -0x80000000 else -0x80000000
                if at < 19660:
                    at = 19660
                m.sw(p + 0x138, at)
                m.sw(sp + 16, 0)
                r = m.call(D_AF224, e, p + 0xAC, m.lw(sp + 80), m.lw(sp + 84), stack=(0,))
                v1 = m.lw(p + 0x134)
                m.sw(p + 0x134, old134)
                m.sw(p + 0x138, v1)
                s6 = D_sticky(m, e, u32(p + 0xAC))
                s3 = r
            # L_790
            if s3:
                s0 = m.call(D_FIXMUL, m.lw(e + 0x128), m.lw(p + 0x128))
                v = m.call(D_FIXMUL, m.lw(e + 0x12C), m.lw(p + 0x12C))
                v = m.call(D_FIXMUL, u32(s0 + v), m.lw(p + 0x1E0))
                d = s32(u32(m.lw(e + 0x1E0) - v))
                a = abs(d) if d != -0x80000000 else -0x80000000
                q = (a + 0x1FFFF if a < 0 else a) >> 17
                q = (q if a >= 0 else 0) + min(0, 4 - q)
                s0 = q
                m.sw(sp + 16, e + 0xAC)
                m.sw(sp + 20, s6)
                r = m.call(0x800A9868, p, sp + 72, m.lw(e + 0x35C), e + 0x360, stack=(e + 0xAC, s6))
                if r and s0 > 0:
                    m.call(D_SOUND3D, m.lw(p + 0xB8), m.lw(p + 0xC0), 19, 0)
                    if (m.lhu(e + 0xAC) < m.lw(m.lw(D_GAME) + 0x30) and m.lw(m.lw(e + 0x354) + 0x25C) < 2
                            and m.lw(F0ad04c_RUMBLE_OFF) == 0):
                        m.sw(sp + 16, 2)
                        m.call(0x800B658C, e, 0, m.lw(e + 0x1E0), 0x0023C361, stack=(2,))
                    if m.lhu(p + 0xAC) < 64:
                        m.call(0x8001A760, m.lhu(m.lw(s4 + 0x254) + 0xAC), 1)
                    else:
                        m.call(0x8001B44C, m.lw(p + 0xB8), m.lw(p + 0xC0), p, 1)
    # L_8fc
    if m.lw(sp + 84) == 0:
        return None
    if s3 and s6:
        return None
    if m.lhu(F0ad04c_STATE_TAB + 8 * m.lhu(p + 0x220) + 2) == 5:
        return None
    if m.lhu(s5 + 0xAC) == m.lhu(e + 0xAC):
        for o in (24, 32, 28):
            m.sw(sp + o, -m.lw(sp + o))
        m.call(D_APPLY, p, sp + 24, 1)
    else:
        m.call(D_APPLY, s5, sp + 24, 1)
    return None


# ==== 0x800b09c4 PointResolve ========================================
"""0x800B09C4  the POINT resolver (frame 608).  F0b09c4_model(m, e, shape)"""

F0b09c4_FRAME = 608


def F0b09c4_model(m, a0, a1, a2, a3):
    s1, s4 = a0, a1
    sp = m.sp
    m.sw(sp + 568, 0)
    s5 = D_sticky(m, s1, s4)
    if m.lw(s1 + 0x33C) == s4 and not s5:
        return None
    h = m.lhu(s4)
    if (h >> 5) < 5:
        pt = D_POOLTAB + 16 * (h >> 5)
        s3 = u32(m.lw(pt) + s32(m.lw(pt + 4)) * (h & 0x1F))
    else:
        m.call(D_MEMSET32, sp + 24, 0, 172)
        m.call(D_MEMCPY32, sp + 196, s4, 280)
        m.sh(sp + 474, m.lhu(s4 + 272))
        m.sh(sp + 476, m.lhu(s4 + 274))
        s3 = sp + 24
        m.sw(sp + 504, 0)
        m.sw(sp + 480, 0)
        m.sw(sp + 484, 0)
        m.sw(sp + 488, 0)
        m.sw(sp + 512, 0)
        m.sw(sp + 516, 0)
        m.sw(sp + 520, 0)
        m.sh(sp + 478, m.lhu(s4 + 276))
    s0 = m.call(D_STALE, s1)
    if m.call(D_INBAND, s1, 0x140000, 0x1C0000):
        m.sw(sp + 16, sp + 528)
        m.sw(sp + 20, sp + 568)
        s2 = m.call(D_AAD30, s1, s3, sp + 560, sp + 564, stack=(sp + 528, sp + 568))
        if s2 and s2 == m.lw(s1 + 0x358):
            s2 = s1
        mag = m.lw(sp + 568)
        if s32(mag) > 0:
            v = m.call(D_FIXMUL, m.lw(s1 + 0x1E0), mag)
            m.call(D_SCALE, v, s1 + 0x1C2, sp + 544)
            m.call(D_APPLY, s1, sp + 544, 1)
            if (m.lhu(s4) >> 5) < 5:
                m.call(D_APPLY, s3, sp + 544, 1)
            D_maxu_228(m, s1, m.lw(sp + 568))
    else:
        if s0:
            D_rebuild_heading(m, s1)
        m.sw(sp + 16, sp + 528)
        s2 = m.call(D_AA140, s1, s3, sp + 560, sp + 564, stack=(sp + 528,))
    # L_c30
    if m.lw(sp + 560) != 0 or s5:
        if s2 != s1:
            for o in (528, 532, 536):
                v = (-m.lhu(sp + o)) & 0xFFFF
                m.sw(sp + o, u32(v - 0x10000 if v & 0x8000 else v))
        m.sw(sp + 16, sp + 528)
        r = m.call(D_AF224, s1, s4, m.lw(sp + 560), m.lw(sp + 564), stack=(sp + 528,))
        m.sw(sp + 560, r)
    if m.lw(sp + 560) == 0:
        return None
    v1 = m.lw(s4 + 188)
    if v1 == m.lw(s1 + 360) and ((v1 >> 16) == 0 or m.lw(s4 + 164) == m.lw(s1 + 336)):
        d = u32(m.lw(s4 + 172) - m.lw(s1 + 344))
        if s32(m.lw(s1 + 364)) < 0:
            d = u32(-s32(d))
    else:
        d = m.call(D_AIPROJ, s4 + 12, s1 + 432, s1 + 184)
    d = s32(d)
    hw = s32(m.lw(s1 + 304))
    code = 0 if d < s32(u32(-hw)) else 3
    if hw < d:
        code -= 1
    m.sw(sp + 16, u32(code))
    m.call(0x800A9408, s1, 0, 0, 0x10000, stack=(code,))
    return None


# ==== 0x800b0d8c BoxResolve ========================================
"""0x800B0D8C  the BOX resolver (frame 96).  F0b0d8c_model(m, e, shape)"""

F0b0d8c_FRAME = 96
F0b0d8c_B2C98, F0b0d8c_B7030, F0b0d8c_B11B4, F0b0d8c_B59F0 = 0x800B2C98, 0x800B7030, 0x800B11B4, 0x800B59F0


def F0b0d8c_model(m, a0, a1, a2, a3):
    s0, s4 = a0, a1
    sp = m.sp
    s1 = 0
    if (m.lhu(s4) >> 5) == 6 and 0x20000 < s32(m.lw(s4 + 140)) and m.lw(s4 + 8) != 1:
        m.call(F0b0d8c_B2C98, a0, a1)
    s5 = D_sticky(m, s0, s4)
    if m.lw(s0 + 0x33C) == s4 and not s5:
        return m.lw(s0 + 0x33C)
    m.call(D_STALE, s0)
    s7 = 16 if (m.lw(s0 + 0x238) & 0x600) else 48
    while True:
        # 0x800B0E60
        m.sw(sp + 16, sp + 48)
        m.sw(sp + 20, sp + 52)
        a0v = m.call(F0b0d8c_B7030, s0 + 196, s4 + 260, s4 + 24, s7, stack=(sp + 48, sp + 52))
        s2 = 0 if a0v == 8 else (a0v | 0x100)
        s3 = 0
        if m.lw(s0 + 856) and m.lw(s0 + 1088) and not s5:
            if a0v == 8 or (a0v & 1) != ((a0v & 2) >> 1):
                s3 = 1
        if s2:
            m.sw(sp + 16, 0)
            m.call(D_B675C, s4 + 24, s4 + 260, m.lw(sp + 48), sp + 24, stack=(0,))
            m.call(D_SCALE, u32(m.lw(sp + 52) + 8192), sp + 24, sp + 32)
            if m.lhu(s0 + 172) < m.lw(m.lw(D_GAME) + 48) and not (s32(m.lw(s0 + 480)) < 132):
                v = m.call(D_DOTLCM, sp + 24, s0 + 450)
                v1 = s32(u32(-s32(v)))
                if v1 > 0:
                    v1 = s32(m.call(D_FIXMUL, u32(v1), m.lw(s0 + 480)))
                    if not (v1 < 132):
                        a0d = s32(m.lw(sp + 52))
                        if a0d < v1:
                            if a0d > 0:
                                if v1 > 0:
                                    s1 = m.call(D_FIXDIV, u32(a0d), u32(v1))
                                else:
                                    s1 = u32(-s32(m.call(D_FIXDIV, u32(a0d), u32(-v1))))
                            else:
                                if v1 <= 0:
                                    s1 = m.call(D_FIXDIV, u32(-a0d), u32(-v1))
                                else:
                                    s1 = u32(-s32(m.call(D_FIXDIV, u32(-a0d), u32(v1))))
                    dt = m.lw(D_DT)
                    s1 = s1 if s32(s1) < s32(dt) else dt
                    D_maxu_228(m, s0, s1)
        if s5:
            m.sw(sp + 40, 0)
            m.sw(sp + 36, 0)
            m.sw(sp + 32, 0)
            m.sh(sp + 28, 0)
            m.sh(sp + 26, 0)
            m.sh(sp + 24, 0)
        v0 = 1
        if s2 or s5:
            logit = False
            if not s5:
                if (m.lhu(s0 + 320) & 4) or m.lw(s0 + 856):
                    a2c = m.lw(D_CCOUNT)
                    if s32(a2c) < 8:
                        logit = True
            if not logit:
                s3 = 0
                m.sw(sp + 16, sp + 32)
                m.sw(sp + 20, sp + 24)
                m.call(F0b0d8c_B11B4, s0, s4, s2, m.lw(sp + 48) | 0x200, stack=(sp + 32, sp + 24))
                v0 = 1
            else:
                if m.lw(s0 + 1088) == 0:
                    s3 = 2
                    s0 = m.lw(s0 + 856)
                rec = u32(D_CREC + 36 * a2c)
                m.sh(rec + 0, m.lhu(s0 + 172))
                m.sh(rec + 2, m.lhu(s4))
                m.sh(rec + 4, m.lhu(sp + 24))
                m.sh(rec + 6, m.lhu(sp + 26))
                m.sh(rec + 8, m.lhu(sp + 28))
                m.sw(rec + 16, m.lw(sp + 32))
                m.sw(rec + 20, m.lw(sp + 36))
                m.sw(rec + 28, s2 | u32((m.lw(sp + 48) | 0x200) << 16))
                m.sw(rec + 32, m.lw(sp + 52))
                m.sw(rec + 24, m.lw(sp + 40))
                if s3 == 2:
                    r = m.call(F0b0d8c_B59F0, rec)
                    m.sw(D_CCOUNT, u32(m.lw(D_CCOUNT) + r))
                else:
                    m.sw(D_CCOUNT, u32(a2c + 1))
                v0 = 1
        if s3 == 1:
            s0 = m.lw(s0 + 856)
            continue
        return v0


# ==== 0x800ae794 PoleResolve ========================================
"""0x800AE794  the POLE resolver, bike x class-1 static volume / knock-over prop (frame 152).  F0ae794_model(m, e, shape)"""

F0ae794_FRAME = 152
F0ae794_ADC74, F0ae794_AF0A0 = 0x800ADC74, 0x800AF0A0


def F0ae794__abs(v):
    v = s32(v)
    return u32(-v) if v < 0 else u32(v)          # abs idiom: INT_MIN stays INT_MIN


def F0ae794_model(m, a0, a1, a2, a3):
    s1, shape = a0, a1
    sp = m.sp
    m.sw(m.entry_sp + 4, shape)                   # sw a1,156(sp): the caller's home slot
    m.sw(sp + 96, 0)
    if m.lw(s1 + 0x340) == shape:
        m.sw(sp + 96, m.lbu(s1 + 0x235) >> 7)
    if m.lw(s1 + 0x33C) == shape and m.lw(sp + 96) == 0:
        return None
    m.sw(sp + 100, m.call(D_STALE, s1))
    if m.lw(s1 + 0x238) & 0x600:
        a2v = a0v = m.lw(s1 + 0xC8)
        for k in range(1, 8):
            v1 = m.lw(s1 + 0xC8 + 12 * k)
            if s32(a0v) < s32(v1):
                a0v = v1
            elif s32(v1) < s32(a2v):
                a2v = v1
    else:
        a0v = m.lw(s1 + 0xC8)
        a2v = m.lw(s1 + 0xF8)
        for k in range(3):
            v1 = m.lw(s1 + 0xC8 + 12 * (1 + k))
            if s32(a0v) < s32(v1):
                a0v = v1
            v1 = m.lw(s1 + 0xC8 + 12 * (5 + k))
            if s32(v1) < s32(a2v):
                a2v = v1
    cy = m.lw(s1 + 0x1FC)
    a0v = u32(cy - a0v)
    if s32(m.lw(shape + 0x8C)) < s32(a0v):
        return None
    a2v = u32(cy - a2v)
    if (m.lhu(shape) >> 5) == 6:
        m.sw(sp + 104, 0)
        a1f = 1 if s32(m.lw(shape + 0x58)) < s32(a2v) else 0
        r = m.lw(shape + 0x54) if a1f else m.lw(shape + 0x84)
        r = u32(r - 16)
        m.sw(sp + 92, r)
        if 0x10000 < s32(r):
            m.sw(sp + 104, a1f & 1)
    else:
        a = m.lw(shape + 0x88)
        b = m.lw(shape + 0x84)
        m.sw(sp + 104, 0)
        if s32(b) < s32(a):
            a = b
        m.sw(sp + 92, u32(a - 16))
    s6 = s7 = 0
    s8 = 0
    m.sh(sp + 50, 0)                              # 0x800AE96C, once: the loop head is 0x800AE970
    while True:
        # LOOP_TOP 0x800AE970
        s6 = 6
        m.sw(sp + 88, 0)
        v1 = 0
        if m.lw(s1 + 0x358):
            v1 = 1 if m.lw(s1 + 0x440) != 0 else 0
        s8 = v1 & (1 if m.lw(sp + 96) == 0 else 0)
        took = False
        if m.call(D_INBAND, s1, 0x140000, 0x1C0000):
            lean = s32(m.lw(s1 + 0x2A4))
            if s32(0xFFFE6DE1) < lean and not (0x1921E < lean) and lean != 0:
                m.sw(sp + 16, sp + 88)
                s6 = m.call(F0ae794_ADC74, s1, shape, m.lw(sp + 92), sp + 24, stack=(sp + 88,))
                s7 = 1 if u32(s6) < 6 else 0
                took = True
        if not took:
            s7 = 0
            if m.lw(sp + 100):
                D_rebuild_heading(m, s1)
                s7 = 0
            r92 = m.lw(sp + 92)
            s4 = s32(u32(m.lw(s1 + 0x130) + r92))
            s5 = s32(u32(m.lw(s1 + 0x134) + r92))
            dx = u32(m.lw(shape + 0x0C) - m.lw(s1 + 0xB8))
            s0 = m.call(D_FIXMUL, m.lw(s1 + 0x12C), dx)
            dz = u32(m.lw(shape + 0x14) - m.lw(s1 + 0xC0))
            s3 = s32(u32(s0 + m.call(D_FIXMUL, m.lw(s1 + 0x128), dz)))
            dx = u32(m.lw(shape + 0x0C) - m.lw(s1 + 0xB8))
            s0 = m.call(D_FIXMUL, m.lw(s1 + 0x128), dx)
            dz = u32(m.lw(shape + 0x14) - m.lw(s1 + 0xC0))
            s0 = s32(u32(s0 + m.call(D_FIXMUL, u32(-s32(m.lw(s1 + 0x12C))), dz)))
            spd = m.lw(s1 + 0x1E0)
            s2 = 1 if 0x100000 < s32(spd) else 0
            v0 = m.call(D_FIXMUL, m.lw(D_DT), spd)
            v1 = v0 if s2 else 0
            if s3 < s5:
                if s32(u32(-s5 - s32(v1))) < s3:
                    t = s32(m.lw(s1 + 0x27C))
                    off = ((u32(163 * t)) >> 12) & 0x3FFC
                    a0s = u32(m.lhs(D_SINCOS + off) << 4)
                    m.sw(sp + 88, a0s)
                    v = s32(m.call(D_FIXMUL, a0s, m.lw(s1 + 0x138)))
                    m.sw(sp + 88, v)
                    if s32(m.lw(s1 + 0x27C)) < 0:
                        if s0 < s4:
                            s7 = 1 if s32(u32(v - s4)) < s0 else 0
                    else:
                        if s0 < s32(u32(s4 + v)):
                            s7 = 1 if s32(u32(-s4)) < s0 else 0
            if s7:
                if s32(u32(s32(F0ae794__abs(s0)) - s32(m.lw(sp + 92)))) < 8519:
                    v1b = u32(-s5 - s5)
                    if s3 > 0:
                        v0 = u32(s5 + v1b)
                    else:
                        v0 = u32(s5 + (v1b if s2 else 0))
                    m.sw(sp + 88, u32(s3 + v0))
                    s6 = 3 if s3 > 0 else 2 * s2 + 1
                    m.sw(sp + 24, m.call(D_FIXMUL, m.lw(s1 + 0x12C), m.lw(sp + 88)))
                    m.sw(sp + 32, m.call(D_FIXMUL, m.lw(s1 + 0x128), m.lw(sp + 88)))
                else:
                    m.sw(sp + 88, u32(s0 - s4) if s0 >= 0 else u32(s0 + s4))
                    s6 = 2 if s0 >= 0 else 0
                    m.sw(sp + 24, m.call(D_FIXMUL, m.lw(s1 + 0x128), m.lw(sp + 88)))
                    m.sw(sp + 32, u32(-s32(m.call(D_FIXMUL, m.lw(s1 + 0x12C), m.lw(sp + 88)))))
                m.sw(sp + 28, 0)
        # L_c90
        v0 = (1 if s7 == 0 else 0) | (1 if s6 == 2 else 0) | (1 if s6 == 3 else 0)
        s8 &= v0
        if s7:
            base = s1 + (814 - 364 if (s6 & 5) == 1 else 814)
            n = [m.lhu(base), m.lhu(base + 2), m.lhu(base + 4)]
            for k in range(3):
                m.sh(sp + 40 + 2 * k, n[k])
            if s6 & 2:
                for k in range(3):
                    m.sh(sp + 40 + 2 * k, -n[k])
            if s8:
                m.sh(sp + 48, m.lhu(s1 + 0xAC))
                m.sh(sp + 52, m.lhu(sp + 40))
                m.sh(sp + 56, m.lhu(sp + 44))
                m.sw(sp + 64, m.lw(sp + 24))
                m.sw(sp + 68, m.lw(sp + 28))
                m.sw(sp + 72, m.lw(sp + 32))
                m.sh(sp + 54, m.lhu(sp + 42))
                m.sw(sp + 80, F0ae794__abs(m.lw(sp + 88)))
                m.sh(sp + 58, s6 | (m.lw(sp + 104) << 4))
                m.sh(sp + 50, m.lhu(shape))
        # L_d84
        if m.lw(s1 + 0x440) == 0:
            s8 = 2
        if s8:
            s1 = m.lw(s1 + 0x358)
            m.sw(sp + 100, 1)
        if s8 != 1:
            break
    # after the loop
    if m.lhu(sp + 50) == m.lhu(shape):
        m.sw(sp + 88, F0ae794__abs(m.lw(sp + 88)))
        take = False
        if s7 == 0:
            take = True
        else:
            code = m.lhu(sp + 58) & 0xF
            if s6 == 0 or code == 2:
                # L_e08
                s6 = 3
                for k in range(3):
                    m.sh(sp + 40 + 2 * k, -m.lhu(s1 + 0x1C2 + 2 * k))
                other = m.lw(s1 + 0x358)
                k_ = u32(m.lw(s1 + 0x134) - m.lw(other + 0x134))
                m.call(D_MULADD, sp + 24, sp + 40, k_, sp + 24)
                for k in range(3):
                    m.sw(sp + 24 + 4 * k, u32(m.lw(sp + 24 + 4 * k) + m.lw(sp + 64 + 4 * k)))
                m.sw(sp + 88, u32(m.lw(sp + 88) + m.lw(sp + 80)))
            else:
                # L_e94 (v1 = code)
                if not (s6 == 3 and code == 3):
                    for k in range(3):
                        m.sw(sp + 24 + 4 * k, u32(m.lw(sp + 24 + 4 * k) + m.lw(sp + 64 + 4 * k)))
            # L_ed4 (s7 != 0 here)
            if (s32(m.lw(sp + 88)) < s32(m.lw(sp + 80)) and s6 == 3 and (m.lhu(sp + 58) & 0xF) == 3):
                take = True
        if take:
            s7 = 1
            for k in range(3):
                m.sh(sp + 40 + 2 * k, m.lhu(sp + 52 + 2 * k))
            for k in range(3):
                m.sw(sp + 24 + 4 * k, m.lw(sp + 64 + 4 * k))
            s6 = m.lhu(sp + 58) & 0xF
        m.sw(sp + 104, m.lw(sp + 104) | (m.lhu(sp + 58) >> 4))
    # L_f60
    if s7 == 0:
        return None
    log = False
    if m.lw(sp + 96) == 0 and (m.lhu(shape) >> 5) == 6 and (m.lhu(s1 + 0x140) & 4):
        if s32(m.lw(D_CCOUNT)) < 8:
            log = True
    if not log:
        m.sw(sp + 20, sp + 24)
        m.sw(sp + 16, m.lw(sp + 104))
        m.call(F0ae794_AF0A0, s1, shape, sp + 40, s6, stack=(m.lw(sp + 104), sp + 24))
        return None
    n = m.lw(D_CCOUNT)
    rec = u32(D_CREC + 36 * n)
    m.sh(rec + 0, m.lhu(s1 + 0xAC))
    m.sh(rec + 2, m.lhu(shape))
    m.sh(rec + 4, m.lhu(sp + 40))
    m.sh(rec + 6, m.lhu(sp + 42))
    m.sh(rec + 8, m.lhu(sp + 44))
    m.sw(rec + 16, m.lw(sp + 24))
    m.sw(D_CCOUNT, u32(n + 1))
    m.sw(rec + 20, m.lw(sp + 28))
    m.sh(rec + 10, s6 | (m.lw(sp + 104) << 4))
    m.sw(rec + 24, m.lw(sp + 32))
    return None


MODELS = [
    (0x800aad30, 200, F0aad30_model, "Response"),
    (0x800aa474, 264, F0aa474_model, "Response1F"),
    (0x800aa140, 80, F0aa140_model, "ResponseFar"),
    (0x800aa34c, 48, F0aa34c_model, "ViewExtent"),
    (0x800b5b48, 56, F0b5b48_model, "PairPushDir"),
    (0x800b3ad0, 296, F0b3ad0_model, "WallContact"),
    (0x800ac5bc, 96, F0ac5bc_model, "BikeVsTraffic"),
    (0x800a91ac, 64, F0a91ac_model, "TrafficSideShove"),
    (0x800ac958, 80, F0ac958_model, "BikeTrafficReact"),
    (0x800ac130, 72, F0ac130_model, "BikeBikeReact"),
    (0x800a9408, 56, F0a9408_model, "ImpactSeverity"),
    (0x800a77b0, 64, F0a77b0_model, "DeferredContacts"),
    (0x800a7ab4, 48, F0a7ab4_model, "ChainReaction"),
    (0x800ad04c, 136, F0ad04c_model, "BikeVsRider"),
    (0x800b09c4, 608, F0b09c4_model, "PointResolve"),
    (0x800b0d8c, 96, F0b0d8c_model, "BoxResolve"),
    (0x800ae794, 152, F0ae794_model, "PoleResolve"),
]
