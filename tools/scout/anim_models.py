r"""Executable transcriptions of the rider animation machine, RASHCDG 0x8005BD74..0x8005E848 (cfe43a77...), plus the
four leaves outside that range it reaches that no bench row covers (RASHCDG 0x800710C0, 0x800714FC, 0x80066A60 and
SLUS 0x8001005C, 67ed165a...), in a form the probe (tools\scout\anim_machine.py) runs against the original executing
in our interpreter, one call at a time, on the pairs.Machine contract (every write outside the own frame in order, every
call with its argument registers, the return value; callees replayed from the guest).

Names are ours. Every model carries one deliberate change that `anim.py verify --mutate` switches on; each must then
fail its own check and nothing else.

Pointer arguments into the caller's own frame (the quaternion and root buffers) are not seen by the pairs harness, which
compares argument REGISTERS; `bcall` below compares the bytes behind them with the guest's own stack writes (the probe
sets `m.stack_check`)."""

MUT = {"on": False}


def u32(v):
    return v & 0xFFFFFFFF


def s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def s8(v):
    v &= 0xFF
    return v - 0x100 if v & 0x80 else v


def tdiv(a, b):
    """MIPS div: truncation toward zero"""
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def mut(orig, changed):
    return changed if MUT["on"] else orig


GS = 0x8005B2F8           # *(GS) = game state; +0x1C the animation dt
SINE = 0x8005624C         # SLUS (s16 sin, s16 cos) x 4096
MIRROR = 0x800CC1B0       # u8[20] part -> mirrored part
OFFQ = 0x800CC1C4         # u32 mask; s16[3] root offset at +4; s16[4] quaternions at +10 + 8k
WIDTHS = 0x800CC654       # u8 *[8], first byte - 1 = the run escape of a w-bit delta stream
STANCE = 0x800541D4       # SLUS: 8-byte stance records (bank, clip, category, mask)

# anim object (0x83C bytes) field names used below
PROG, LEN, PC, FRAME_, LOOPS, RATE, ACC, SUB, FLAGS, BANK, CLIP, TRK = (4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48)
EX, MASK, XB = 1756, 1760, 1764


def bcall(m, target, bufs, a0=None, a1=None, a2=None, a3=None, stack=()):
    """m.call plus a comparison of the bytes behind pointer arguments into the own frame with the guest's own stack
    writes (the probe installs m.stack_check); bufs = [(addr, nbytes)]"""
    chk = getattr(m, "stack_check", None)
    if chk is not None:
        for ad, n in bufs:
            chk(m, target, ad, n)
    return m.call(target, a0, a1, a2, a3, stack)


def op_at(m, a):
    return u32(m.lw(a + PROG) + 12 * m.lw(a + PC))


# ------------------------------------------------------------------------------------------------ the small ones

def m_bd74(m, a0, a1, a2, a3):
    """Restart(a): zero the clock, select the first playable op, prime it"""
    a = a0
    f = m.lw(a + FLAGS)
    for off in (PC, FRAME_, LOOPS, ACC, SUB, EX):
        m.sw(a + off, 0)
    m.sw(a + RATE, 10)
    m.sw(a + FLAGS, f & mut(0xE5, 0xE7))
    pc = m.call(0x8005C418, a)
    m.sw(a + PC, pc)
    m.call(0x8005C8F4, a)
    if m.lw(a + PROG) == 0:
        return 0
    if m.lw(a + LEN) == 0:
        return 0
    v = (m.lw(a + FLAGS) & 0xFFFFFFFD) | 2
    m.sw(a + FLAGS, v)
    return v


def m_be0c(m, a0, a1, a2, a3):
    """Stop(a) - RiderDismount's"""
    v = m.lw(a0 + FLAGS) & mut(0xFFFFFFFD, 0xFFFFFFFF)
    m.sw(a0 + FLAGS, v)
    return v


def m_be20(m, a0, a1, a2, a3):
    v1 = m.lw(a0 + FLAGS)
    if not (v1 & 2):
        return 0xFFFFFFFD
    m.sw(a0 + FLAGS, v1 & 0xFFFFFFFD)
    return v1 & 0xFFFFFFFD


def m_be44(m, a0, a1, a2, a3):
    v = m.lw(a0 + FLAGS)
    m.sw(a0 + ACC, 0)
    m.sw(a0 + FLAGS, v | 2)
    return v | 2


def m_be58(m, a0, a1, a2, a3):
    """ClipDone(a): 1 = not playing, stopped, or a play-to-frame op at/after its end frame"""
    a = a0
    if not (m.lw(a + FLAGS) & 2):
        return 1
    P = m.lw(a + PROG)
    op = u32(P + 12 * m.lw(a + PC))
    o = m.lbu(op + 1)
    if o == 1:
        if m.lbu(P + 1) == 3:
            return 0
        if m.lbu(P + 13) == 3:
            return 0
        return 0 if s32(m.lw(a + FRAME_)) < m.lhs(op + 6) + mut(0, 1) else 1
    if o < 2:
        return 0
    return 1 if o == 5 else 0


def m_bef4(m, a0, a1, a2, a3):
    """ChannelFree(a): 1 = a queued transition may start now"""
    a = a0
    P = m.lw(a + PROG)
    if m.lbu(P + 1) == 5:
        return 1
    if m.lw(a + PC) == 1 and m.lbu(P + 13) == 5:
        return 1
    if not (m.lbu(P + 1) < mut(2, 1)):
        return 0
    if m.lbu(P + 13) == 3:
        return 0
    return 1 if m.lw(a + PC) == 0 else 0


def _tail2(m, t0, code):
    """the second op of a start: a copy of the first with another opcode"""
    v1, a0_, a1_, a2_ = m.lbu(t0 + 2), m.lw(t0 + 8), m.lbu(t0), m.lbu(t0 + 3)
    return v1, a0_, a1_, a2_


def m_bf6c(m, a0, a1, a2, a3):
    """HardStart(a, clip, flags, rate, [ex]): [op1 clip 0..A-1, op5]"""
    a, clip, flags, rate = a0, a1, a2, a3
    t0 = m.lw(a + PROG)
    cp = m.lw(u32(m.lw(m.lw(a + BANK) + 4) + 4 * clip))
    ex = m.arg(4)
    A = m.lhu(cp + 16)
    m.sb(t0 + 2, flags)
    m.sb(t0 + 1, 1)
    m.sb(t0 + 0, clip)
    m.sh(t0 + 4, 0)
    m.sw(t0 + 8, ex)
    m.sh(t0 + 6, A - mut(1, 2))
    m.sb(t0 + 3, rate if flags & 8 else 0)
    v1, w8, b0, b3 = _tail2(m, t0, 5)
    m.sb(t0 + 13, 5)
    m.sh(t0 + 16, 0)
    m.sh(t0 + 18, 0)
    m.sw(t0 + 20, w8)
    m.sb(t0 + 14, v1)
    m.sb(t0 + 12, b0)
    m.sb(t0 + 15, b3)
    return m.call(0x8005BD74, a)


def m_c018(m, a0, a1, a2, a3):
    """RangedStart(a, clip, flags, first, [last, rate, ex]): [op1 clip first..last, op5]"""
    a, clip, flags, first = a0, a1, a2, a3
    t1 = m.lw(a + PROG)
    last, ex = m.arg(4), m.arg(6)
    m.sb(t1 + 2, flags)
    m.sb(t1 + 0, clip)
    m.sb(t1 + 1, mut(1, 0))
    m.sh(t1 + 4, first)
    m.sh(t1 + 6, last)
    m.sw(t1 + 8, ex)
    m.sb(t1 + 3, m.arg(5) if flags & 8 else 0)
    v1, w8, b0, b3 = _tail2(m, t1, 5)
    m.sb(t1 + 13, 5)
    m.sh(t1 + 16, 0)
    m.sh(t1 + 18, 0)
    m.sw(t1 + 20, w8)
    m.sb(t1 + 14, v1)
    m.sb(t1 + 12, b0)
    m.sb(t1 + 15, b3)
    return m.call(0x8005BD74, a)


def m_c0b0(m, a0, a1, a2, a3):
    """LoopStart(a, clip, flags, rate, [ex]): [op0 clip, repeat 0xFFFFFFFF times; op2 -> 0]"""
    a, clip, flags, rate = a0, a1, a2, a3
    t0 = m.lw(a + PROG)
    ex = m.arg(4)
    m.sb(t0 + 2, flags)
    m.sw(t0 + 8, ex)
    m.sb(t0 + 0, clip)
    m.sb(t0 + 1, 0)
    m.sb(t0 + 3, rate if flags & 8 else 0)
    v1, w8, b0, b3 = _tail2(m, t0, 2)
    m.sh(t0 + 4, mut(0xFFFF, 0xFFFE))
    m.sh(t0 + 6, 0)
    m.sb(t0 + 13, 2)
    m.sh(t0 + 16, 0)
    m.sh(t0 + 18, 0)
    m.sw(t0 + 20, w8)
    m.sb(t0 + 14, v1)
    m.sb(t0 + 12, b0)
    m.sb(t0 + 15, b3)
    return m.call(0x8005BD74, a)


def m_c140(m, a0, a1, a2, a3):
    """Transition(a, clip, queued, flags, [once, b5, rate, ex]): [op3 blend 5 frames, op4 (old clip, frame),
    op4 (new clip, ex), op5] written at op 0, or at op 1 behind the running op when queued"""
    a, clip, s2, s1 = a0, a1, a2, a3
    s6 = a3 & 1
    s0 = m.lw(a + PROG)
    s5 = m.lbu(m.entry_sp + 16)
    s4 = m.lbu(s0 + 0)
    s7 = m.lbu(s0 + 2) & 1
    if s2 == 1:
        if m.call(0x8005BE58, a) != 0:
            s2 = 0
        elif m.lbu(s0 + 1) == 0:
            m.sb(s0 + 1, 1)
    if s2 == 0:
        fr = m.lw(a + FRAME_)
    elif s2 == 1:
        cp = m.lw(u32(m.lw(m.lw(a + BANK) + 4) + 4 * s4))
        fr = u32(s16(m.lhu(cp + 16) - 1))
        s0 = u32(s0 + 12)
    else:
        s6 = 0
        fr = 0xFFFFFFFF
    m.sb(s0 + 2, s1)
    if s1 & 8:
        m.sb(s0 + 2, s1 | 8)
        m.sb(s0 + 3, m.arg(6))
    else:
        m.sb(s0 + 3, 0)
    v1 = m.lbu(s0 + 2)
    m.sb(s0 + 0, s5)
    m.sb(s0 + 1, 3)
    m.sh(s0 + 4, mut(5, 4))
    m.sh(s0 + 6, 0)
    m.sw(s0 + 8, 0)
    m.sb(s0 + 12, 0)
    m.sb(s0 + 14, s1)
    m.sb(s0 + 13, 4)
    m.sh(s0 + 16, s4)
    m.sh(s0 + 18, fr)
    m.sw(s0 + 20, 0)
    m.sb(s0 + 26, 0)
    m.sb(s0 + 24, 0)
    b2 = u32(m.arg(5) << 5) | (v1 & 0xDF) | 0x80 | 4
    m.sb(s0 + 2, b2)
    m.sb(s0 + 14, s7 | (s1 & 0xFE))
    if s1 & 8:
        m.sb(s0 + 26, m.lbu(s0 + 26) | 8)
    b3 = m.lbu(s0 + 3)
    v1 = m.lbu(s0 + 26)
    m.sb(s0 + 25, 4)
    m.sh(s0 + 28, clip)
    ex = m.arg(7)
    m.sh(s0 + 30, 0)
    m.sb(s0 + 36, 0)
    m.sb(s0 + 37, 5)
    m.sw(s0 + 32, ex)
    m.sb(s0 + 27, b3)
    m.sb(s0 + 26, s6 | (v1 & 0xFE))
    if s2 == 0:
        return m.call(0x8005BD74, a)
    return ex


def m_c338(m, a0, a1, a2, a3):
    """QuatGet(a, out): the root part's current quaternion (channels 3..6, `h4`)"""
    for k, off in enumerate((128, 152, 176, 200)):
        v = m.lhu(a0 + off)
        m.sh(a1 + 2 * mut(k, (k + 1) & 3), v)
    return v


def m_c36c(m, a0, a1, a2, a3):
    """QuatSet(a, in)"""
    for k, off in enumerate(mut((128, 152, 176, 200), (152, 128, 176, 200))):
        v = m.lhu(a1 + 2 * k)
        m.sh(a0 + off, v)
    return v


def m_c39c(m, a0, a1, a2, a3):
    """ClipSelect(a, clip): decode the clip header into the track table, unless it is the current clip untouched"""
    a = a0
    cur = m.lw(a + CLIP)
    if cur and (a1 & 0xFF) == m.lbu(cur + 14) and m.lhs(a + TRK) == mut(-1, -2):
        return m.lw(a + CLIP)
    cp = m.lw(u32(m.lw(m.lw(a + BANK) + 4) + 4 * (a1 & 0xFF)))
    m.sw(a + CLIP, cp)
    m.call(0x8005E2BC, a, u32(a + TRK))
    return m.lw(a + CLIP)


def m_c418(m, a0, a1, a2, a3):
    """Seq(a): from the pc, follow jumps (2) and skips (4) to the next playable op (0, 1, 3) or a stop (5)"""
    a = a0
    done = 0
    P, a1_, t0 = m.lw(a + PROG), m.lw(a + PC), m.lw(a + LEN)
    a2_ = a1_
    while True:
        op = u32(P + 12 * a1_)
        o = m.lbu(op + 1)
        if o < 6:
            if o == 2:
                a1_ = u32(m.lhs(op + 4))
            elif o == 4:
                a1_ = u32(a1_ + 1)
                if a1_ == t0:
                    a1_ = 0
            elif o == 5:
                done = 1
                m.sw(a + FLAGS, m.lw(a + FLAGS) & mut(0xFFFFFFFD, 0xFFFFFFFF))
            else:
                done = 1
        if s32(t0) < s32(a2_):
            m.sw(a + FLAGS, m.lw(a + FLAGS) & 0xFFFFFFFD)
            return a1_
        a2_ = u32(a2_ + 1)
        if done:
            return a1_


def m_c4ec(m, a0, a1, a2, a3):
    """Tick(a, dt): acc += dt; while acc >= rate: frame++, acc -= rate"""
    a = a0
    acc = u32(a1 + m.lw(a + ACC))
    rate = m.lw(a + RATE)
    if not (s32(acc) < s32(rate)):
        while True:
            f = m.lw(a + FRAME_)
            acc = u32(acc - rate)
            m.sw(a + FRAME_, f + mut(1, 2))
            if s32(acc) < s32(rate):
                break
    m.sw(a + SUB, acc)
    m.sw(a + ACC, acc)
    return 1


def m_c52c(m, a0, a1, a2, a3):
    """LoopRestart(a): the looping op starts its clip over"""
    a = a0
    op = op_at(m, a)
    m.call(0x8005C39C, a, m.lbu(op))
    m.sw(a + FRAME_, 0)
    m.sw(a + mut(ACC, SUB), 0)
    v = m.lw(op + 8)
    m.sw(a + EX, v)
    return v


def m_c58c(m, a0, a1, a2, a3):
    """Advance(a, dt): tick the clock and say whether the current op is finished"""
    a, dt = a0, a1
    op = op_at(m, a)
    cp = m.lw(a + CLIP)
    o = m.lbu(op + 1)
    if o == 0:
        m.call(0x8005C4EC, a, dt)
        A = m.lhu(cp + 16)
        if not (s32(m.lw(a + FRAME_)) < A - 1):
            m.sw(a + SUB, 0)
        if s32(m.lw(a + FRAME_)) < A:
            return 0
        v = m.lw(a + LOOPS)
        if v != 0xFFFF:
            m.sw(a + LOOPS, v - 1)
        if m.lw(a + LOOPS) == 0:
            return 1
        m.call(0x8005C52C, a)
        return 0
    if o == 1:
        m.call(0x8005C4EC, a, dt)
        if not (s32(m.lw(a + FRAME_)) < m.lhs(op + 6)):
            m.sw(a + SUB, 0)
        return 1 if m.lhs(op + 6) < s32(m.lw(a + FRAME_)) + mut(0, 1) else 0
    if o == 3:
        m.call(0x8005C4EC, a, dt)
        if s32(m.lw(a + FRAME_)) < m.lhs(op + 4):
            return 0
        m.sw(a + FRAME_, 0)
        m.sw(a + SUB, 0)
        if not (m.lbu(op + 2) & 4):
            return 1
        m.sw(a + PC, m.lw(a + LEN) - 1)
        P = m.lw(a + PROG)
        m.sb(P + 2, m.lbu(op + 26))
        m.sw(P + 8, m.lw(op + 32))
        if m.lbu(op + 0) == 1:
            m.sb(P + 0, m.lbu(op + 28))
            m.sb(P + 1, 1)
            m.sh(P + 4, 0)
            m.sh(P + 6, m.lhu(cp + 16) - 1)
            m.sw(P + 8, m.lw(op + 32))
            m.sb(P + 3, m.lbu(op + 27) if m.lbu(P + 2) & 8 else 0)
            code = 5
        else:
            m.sb(P + 0, m.lbu(op + 28))
            m.sb(P + 1, 0)
            m.sb(P + 3, m.lbu(op + 27) if m.lbu(P + 2) & 8 else 0)
            m.sh(P + 4, 0xFFFF)
            m.sh(P + 6, 0)
            code = 2
        m.sb(P + 14, m.lbu(P + 2))
        m.sw(P + 20, m.lw(P + 8))
        m.sb(P + 12, m.lbu(P + 0))
        m.sb(P + 15, m.lbu(P + 3))
        m.sb(P + 13, code)
        m.sh(P + 16, 0)
        m.sh(P + 18, 0)
        return 1
    if o == 5:
        return 1
    return 0


def _rate(m, op, cp_rate):
    if m.lbu(op + 2) & 8:
        r = m.lbu(op + 3)
    else:
        r = cp_rate
    return r if r else 10


def m_c8f4(m, a0, a1, a2, a3):
    """PoseStart(a): prime the op at the pc - its clip, rate, first frame, loop count"""
    a = a0
    op = op_at(m, a)
    o = m.lbu(op + 1)
    if o == 1:
        cp = m.call(0x8005C39C, a, m.lbu(op))
        m.sw(a + RATE, _rate(m, op, m.lhu(cp + 18)))
        m.sw(a + ACC, 0)
        m.sw(a + EX, m.lw(op + 8))
        A = m.lhu(cp + 16)
        if not (m.lhs(op + 6) < A):
            m.sh(op + 6, A - 1)
        v = u32(m.lhs(op + 4))
        m.sw(a + FRAME_, v)
        return v
    if o == 0:
        cp = m.call(0x8005C39C, a, m.lbu(op))
        m.sw(a + FRAME_, 0)
        m.sw(a + RATE, _rate(m, op, m.lhu(cp + mut(18, 16))))
        m.sw(a + ACC, 0)
        m.sw(a + LOOPS, u32(m.lhs(op + 4)))
        v = m.lw(op + 8)
        m.sw(a + EX, v)
        return v
    if o == 3:
        b2 = m.lbu(op + 2)
        if not (b2 & 2) or not (b2 >> 7):
            m.call(0x8005CB70, a, op, u32(a + XB))
            m.sb(op + 2, m.lbu(op + 2) | 2)
        m.sw(a + FRAME_, 0)
        m.sw(a + EX, m.lw(op + 8))
        r = _rate(m, op, m.lw(a + XB + 4))
        m.sw(a + RATE, r)
        return r
    return 3


def m_cb04(m, a0, a1, a2, a3):
    """AdvanceFrame(a, dt): Advance; when the op finished and the object still plays, step to the next op"""
    a = a0
    if not m.call(0x8005C58C, a, a1):
        return 0
    if not (m.lw(a + FLAGS) & 2):
        return 0
    pc = u32(m.lw(a + PC) + 1)
    m.sw(a + PC, pc)
    if pc == m.lw(a + LEN):
        m.sw(a + PC, mut(0, 1))
    m.sw(a + PC, m.call(0x8005C418, a))
    return m.call(0x8005C8F4, a)


def m_e1d8(m, a0, a1, a2, a3):
    """AnimationPass(d): once a frame, every playing object: apply the pose, then advance by gs+0x1C"""
    d = a0
    if m.lw(d + 8) == 0 or s32(m.lw(d + 12)) <= 0:
        return None
    i = 0
    while i < s32(m.lw(d + 12)):
        A = u32(m.lw(d) + 2108 * i)
        f = m.lw(A + FLAGS)
        if f & 2:
            if (m.lbu(m.lw(A) + 9) & 3) or not (f & 0x10):
                m.call(0x8005D2A8, A)
            if not (m.lw(A + FLAGS) & mut(8, 0x80)):
                m.call(0x8005CB04, A, m.lw(m.lw(GS) + 28))
        i += 1
    return None


def m_d2a8(m, a0, a1, a2, a3):
    """ApplyFrame(a): the pose of the current op (a blend for op 3), then flags bit 4, then the facing toggle"""
    a = a0
    op = op_at(m, a)
    if m.lbu(op + 1) == 3:
        m.call(0x8005D36C, a)
    else:
        m.call(0x8005E5A4, a, u32(a + TRK))
        m.call(0x8005D63C, a)
    m.sw(a + FLAGS, m.lbu(a + FLAGS) | mut(0x10, 0x30))
    if not (m.lbu(op + 2) & 0x10):
        return 0
    R = m.lw(a)
    m.sb(R + 547, -m.lbu(R + 547))
    v = m.lbu(op + 2) & 0xEF
    m.sb(op + 2, v)
    return v


def m_e558(m, a0, a1, a2, a3):
    """Bits(&ptr, &pos, w): w bits, MSB first, from a halfword stream"""
    t1 = m.lbu(a1)
    p = m.lw(a0)
    t0 = u32(t1 + a2)
    step = (t0 >> 3) & 0x1E
    hi, lo = m.lhu(p), m.lhu(p + 2)
    m.sw(a0, p + step)
    m.sb(a1, t0 & mut(0xF, 0x7))
    win = u32(((hi << 16) | lo) << (t1 & 31))
    return (win >> ((32 - a2) & 31)) & 0xFF


def m_e2bc(m, a0, a1, a2, a3):
    """DecodeHeader(a, T): the clip's channel table - 3 root channels, then 4 per part"""
    a, T = a0, a1
    t0 = u32(T + 4)
    cp = m.lw(a + CLIP)
    a3_ = u32(cp + 24)
    m.sh(T + 2, m.lbu(cp + 15) * 4 + 3)
    t2 = m.lw(cp + 4)
    A = m.lhu(cp + 16)
    npq = m.lbu(cp + 15) * 4
    m.sh(T + 0, 0xFFFF)
    f13 = m.lbu(cp + 13)
    t2 = s32(u32(t2 - 24))
    if f13 & 1:
        e = u32(T + 10)
        for _ in range(3):
            t2 -= 2 * A
            m.sb(e + 14, m.lbu(e + 14) & 0xFD)
            m.sw(t0, a3_)
            x = m.lhu(a3_)
            t0 = u32(t0 + 24)
            m.sh(e - 2, x)
            m.sh(e, x)
            a3_ = u32(a3_ + 2 * m.lhu(cp + 16))
            e = u32(e + 24)
    else:
        e = u32(T + 8)
        for _ in range(3):
            t0 = u32(t0 + 24)
            v = m.lbu(e + 16)
            m.sh(e + 2, 0)
            m.sh(e, 0)
            m.sb(e + 16, (v & 0xFC) | 2)
            e = u32(e + 24)
    if m.lbu(cp + 13) & 2:
        if t2 < 3:
            return 1
        e = u32(t0 + 6)
        while True:
            v1 = m.lbu(e + 14) | 2
            m.sb(e + 14, v1)
            anim = 1 if (m.lhs(a3_ + 2) & 0x8000) else 0
            m.sb(e + 14, anim | (v1 & 0xFE))
            base = m.lhu(a3_ + 2) & 0x7FFF
            m.sw(e + 2, base)
            if base & 0x4000:
                m.sw(e + 2, base | 0xFFFF8000)
            if not (m.lbu(e + 14) & 1):
                v = u32(m.lw(e + 2) << 6)
                m.sh(e - 2, v)
                m.sh(e, v)
            else:
                sc = m.lhu(a3_ + 4) & 0x7FFF
                m.sh(e + 10, sc)
                prod = u32(s32(m.lw(e + 2)) * sc)
                w = (m.lhu(a3_ + 6) & 0x7FFF) >> mut(8, 7)
                m.sb(e + 16, w)
                m.sh(e - 2, s32(prod) >> 9)
                m.sw(t0, u32(a3_ + 6))
                m.sb(e + 17, 8)
                fl, w = m.lbu(e + 14), m.lbu(e + 16)
                m.sw(e + 6, 0)
                m.sb(e + 12, 0)
                m.sb(e + 14, fl & 0xFB)
                first = m.lbu(m.lw(WIDTHS + 4 * w))
                m.sb(e + 13, (first - 1) & ((1 << (w & 31)) - 1))
            m.sh(e, m.lhu(e - 2))
            t0 = u32(t0 + 24)
            n = m.lhs(a3_)
            e = u32(e + 24)
            t2 -= 2 * (n + 1)
            a3_ = u32(a3_ + 2 * n + 2)
            if t2 < 3:
                return 1
    if npq == 0:
        return 0
    e = u32(t0 + 6)
    for _ in range(npq):
        m.sb(e + 14, m.lbu(e + 14) & 0xFD)
        m.sw(t0, a3_)
        x = m.lhu(a3_)
        t0 = u32(t0 + 24)
        m.sh(e - 2, x)
        m.sh(e, x)
        a3_ = u32(a3_ + 2 * m.lhu(cp + 16))
        e = u32(e + 24)
    return 0


def m_e5a4(m, a0, a1, a2, a3):
    """Sample(a, T): every channel's value at key `frame` (h4) and the next key (h6); animated channels are decoded
    forward from the last sampled frame T.h0"""
    a, T = a0, a1
    cp = m.lw(a + CLIP)
    s6 = s32(m.lw(a + FRAME_))
    s4 = s6 + 1
    if (m.lhu(cp + 16) - 1) < s4:
        s4 = s6
    s1 = u32(T + 4)
    n = m.lhs(T + 2)
    k = 0
    e = u32(T + 10)
    while k < n:
        fl = m.lbu(e + 14)
        if not (fl & 2):
            m.sh(e - 2, m.lhu(u32(m.lw(s1) + 2 * s6)))
            m.sh(e, m.lhu(u32(m.lw(s1) + 2 * s4)))
        elif fl & 1:
            s2 = m.lhs(T) + 1
            if s2 < s4:
                s3 = u32(s1 + 23)
                while True:
                    fl = m.lbu(e + 14)
                    if fl & 4:
                        v = m.lbu(e + 12)
                        c = u32(m.lw(e + 6) - 1)
                        m.sb(e + 15, v)
                        m.sw(e + 6, c)
                        m.sb(e + 14, (fl & 0xFB) | (4 if s32(c) > 0 else 0))
                    else:
                        v = m.call(0x8005E558, s1, u32(s1 + 23), m.lbu(e + 16))
                        m.sb(e + 15, v)
                        if s8(v) == m.lbs(e + 13):
                            m.sw(e + 6, 0)
                            while True:
                                v = m.call(0x8005E558, s1, s3, m.lbu(e + 16))
                                m.sb(e + 15, v)
                                lim = 1 << m.lbu(e + 16)
                                if s8(v) != lim - 1:
                                    break
                                m.sw(e + 6, u32(m.lw(e + 6) - 1 + lim))
                            m.sw(e + 6, u32(m.lw(e + 6) + mut(4, 3) + m.lbs(e + 15)))
                            v = m.call(0x8005E558, s1, s3, m.lbu(e + 16))
                            m.sb(e + 12, v)
                            m.sb(e + 15, v)
                            fl = m.lbu(e + 14) | 4
                            c = u32(m.lw(e + 6) - 1)
                            m.sb(e + 14, fl)
                            m.sw(e + 6, c)
                    w = m.lbu(e + 16)
                    d = m.lbs(e + 15)
                    if (d >> ((w - 1) & 31)) & 1:
                        m.sw(e + 2, u32(m.lw(e + 2) + ((d - (1 << (w & 31))) << 4)))
                    else:
                        m.sw(e + 2, u32(m.lw(e + 2) + (d << 4)))
                    sc = m.lhu(e + 10)
                    acc = s32(m.lw(e + 2))
                    m.sh(e - 2, m.lhu(e))
                    m.sh(e, s32(u32(acc * sc)) >> 9)
                    s2 += 1
                    if not (s2 < s4):
                        break
            if s4 == s6:
                m.sh(e - 2, m.lhu(e))
        e = u32(e + 24)
        k += 1
        s1 = u32(s1 + 24)
        n = m.lhs(T + 2)
    m.sh(T, s6)
    return None


# ------------------------------------------------------------------------------------------------ the pose

def _typ(m, R):
    return (m.lhu(m.lw(R) + 14) & 0x78) >> 3


def _setup(m, R):
    """the rider's mirror offset (s7), height offset (s4) and the parent's class (s8) - 0x8005D6E8.. / 0x8005CBC8.."""
    ox, oy, cls = 0, 0, 0
    if ((m.lw(R + 36) >> 18) & 1) == 1 and m.lw(R + 52) != 0:
        ox = 1148
        oy = 10 if 0xFFFF < s32(m.lw(R + 76)) else 140
    elif _typ(m, R) == 1 and m.lw(R + 52) != 0:
        cls = (m.lhu(m.lw(m.lw(R + 52)) + 14) & 0xF80) >> 7
    return ox, oy, cls


def _mirror_q(q, typ, part, mirror):
    """the facing flip of one part's quaternion (x, y, z, w) - 0x8005DB00.. / 0x8005CDF0.."""
    if not mirror:
        return list(q)
    if typ in (1, 4):
        if part == 0:
            return [-q[2], -q[3], -q[0], -q[1]]
        return [-q[0], -q[1], q[2], q[3]]
    if typ == 5:
        return [-q[0], -q[1], q[2], q[3]]
    return [q[0], -q[1], -q[2], q[3]]


def m_d63c(m, a0, a1, a2, a3):
    """Pose(a): root translation and 20 part quaternions, lerped by the sub-frame, into the rider's root and part
    matrices (SLUS 0x8001005C); stance 42 tilts the whole rider with three GTE MVMVA"""
    a = a0
    op = op_at(m, a)
    b2 = m.lbu(op + 2)
    mask = 0xFFFFFFFF if (b2 & 0x40) else m.lw(a + MASK)
    cp = m.lw(a + CLIP)
    mirror = b2 & 1
    s2 = 0
    if m.lw(a + FLAGS) & 4:
        s2 = u32(tdiv(s32(u32(m.lw(a + SUB) << 16)), s32(m.lw(a + RATE))))
    R = m.lw(a)
    ox, oy, cls = _setup(m, R)
    sp = m.sp
    E = lambda ch, h: u32(a + 52 + 24 * ch + h)
    if s2 == 0:
        root = [m.lhu(E(c, 4)) for c in range(3)]
    else:
        s1 = u32(0x10000 - s2)
        root = []
        for c in range(3):                     # the guest parks h4 at sp+40+2c and h6 at sp+48+2c, then lh's them
            for off, h in ((40, 4), (48, 6)):
                ad = (sp + off + 2 * c) & 0x1FFFFF
                m.mem[ad:ad + 2] = m.lhu(E(c, h)).to_bytes(2, "little")
        for c in range(3):
            x = m.call(0x8001FC90, s1, u32(m.lhs(E(c, 4))))
            y = m.call(0x8001FC90, s2, u32(m.lhs(E(c, 6))))
            root.append(u32(x + y) & 0xFFFF)
    if cls == 1:
        root = [(root[c] + m.lhu(OFFQ + 4 + 2 * c)) & 0xFFFF for c in range(3)]
    rx = (ox - root[0]) if mirror else root[0]
    buf = [rx & 0xFFFF, (root[1] + oy) & 0xFFFF, root[2]]
    for k in range(3):
        m.mem[(sp + 24 + 2 * k) & 0x1FFFFF:(sp + 26 + 2 * k) & 0x1FFFFF] = buf[k].to_bytes(2, "little")
    bcall(m, 0x80066A60, [(sp + 24, 6)], R, u32(sp + 24))
    parts = m.lbu(cp + 15)
    f14 = s32(u32(s2 << 14)) >> 16
    g14 = 16384 - f14
    for k in range(parts):
        dst = m.lbu(MIRROR + k) if mirror else k
        if not ((mask >> (dst & 31)) & 1):
            continue
        typ = _typ(m, R)
        chans = [3 + 4 * k + c for c in range(4)]
        if f14 == 0:
            q = [m.lhs(E(c, 4)) for c in chans]
        else:
            q = [s16((g14 * m.lhs(E(c, 4)) >> 14) + (f14 * m.lhs(E(c, 6)) >> 14)) for c in chans]
        qb = sp + 32
        for i in range(4):
            m.mem[(qb + 2 * i) & 0x1FFFFF:(qb + 2 * i + 2) & 0x1FFFFF] = (q[i] & 0xFFFF).to_bytes(2, "little")
        if typ in (1, 4) and cls == 1 and (m.lw(OFFQ) >> k) & 1:
            bcall(m, 0x800714FC, [(qb, 8)], u32(OFFQ + 10 + 8 * k), u32(qb), u32(qb))
            q = [m.lhs(qb + 2 * i) for i in range(4)]
        q = _mirror_q(q, typ if typ in (1, 4, 5) else 0, k, mirror)
        q = [s16(v) for v in q]
        if MUT["on"] and k == 1:
            q[0] = s16(q[0] + 1)
        for i in range(4):
            m.mem[(sp + 32 + 4 * i) & 0x1FFFFF:(sp + 36 + 4 * i) & 0x1FFFFF] = u32(q[i] << 2).to_bytes(4, "little")
        bcall(m, 0x8001005C, [(sp + 32, 16)], u32(m.lw(m.lw(a) + 4) + 24 * dst + 4), u32(sp + 32))
    R = m.lw(a)
    if m.lhu(R + 544) != 42:
        return None
    a1_ = u32(tdiv(s32(u32(m.lw(a + SUB) << 16)), s32(m.lw(a + RATE))))
    op = op_at(m, a)
    cp = m.lw(u32(m.lw(m.lw(a + BANK) + 4) + 4 * m.lbu(op)))
    last = s16(m.lhu(cp + 16) - 1)
    half = u32(((last + (1 if last < 0 else 0)) >> 1) << 16)
    t = u32(u32(m.lw(a + FRAME_) << 16) + a1_)
    bike_ang = m.lw(m.lw(R + 596) + 652)
    if s32(t) < s32(half):
        r = u32(tdiv(s32(u32(t << 1)), last))
        x = m.call(0x8001FC90, bike_ang, r)
    else:
        x = bike_ang
    ang = s32(u32(652 * s32(x))) >> 16
    i = ang & 0xFFF
    sn, cs = m.lhs(SINE + 4 * i), m.lhs(SINE + 4 * i + 2)
    Rm = [[cs, sn, 0], [-sn, cs, 0], [0, 0, 4096]]
    M = u32(m.lw(R + 4) + 4)
    for col in (0, 2, 4):
        v = [m.lhs(M + col + 6 * r) for r in range(3)]
        out = mvmva(Rm, v)
        for r in range(3):
            m.sh(M + col + 6 * r, out[r])
    v = [m.lhs(R + 28), m.lhs(R + 30), m.lhs(R + 32)]
    out = mvmva(Rm, v)
    for r in range(3):
        m.sh(R + 28 + 2 * r, out[r])
    return None


def mvmva(Rm, v):
    """GTE MVMVA sf=1, mx=RT, cv=none, lm=0 (commands 0x49E012 / 0x486012): IR_i = sat16((sum_j R_ij v_j) >> 12)"""
    out = []
    for r in range(3):
        acc = sum(Rm[r][j] * v[j] for j in range(3)) >> 12
        out.append(max(-0x8000, min(0x7FFF, acc)) & 0xFFFF)
    return out


def m_cb70(m, a0, a1, a2, a3):
    """TransitionCapture(a, op, B): the current pose (from) and the new clip's first key (to) into the blend
    buffer B = a+0x6E4, and the set of parts both define"""
    a, op, B = a0, a1, a2
    sp = m.sp
    m.sw(m.entry_sp + 4, op)                  # 0x8005CBA8: a1 parked in the CALLER's home area
    m.sw(B + 4, 10)
    R = m.lw(a)
    newclip = m.lbu(op + 28)
    ox, oy, cls = _setup(m, R)
    masks = []
    for half in (0, 1):
        if half == 0:
            cp = m.lw(a + CLIP)
            mirror = m.lbu(op + 14) & 1
            rbase, qbase = 12, 24
        else:
            cp = m.call(0x8005C39C, a, newclip)
            mirror = m.lbu(op + 26) & 1
            rbase, qbase = 18, 32
        root = [m.lhu(a + 56 + 24 * c) for c in range(3)]
        if cls == 1:
            root = [(root[c] + m.lhu(OFFQ + 4 + 2 * c)) & 0xFFFF for c in range(3)]
        m.sh(B + rbase, (ox - root[0]) if mirror else root[0])
        m.sh(B + rbase + 2, root[1] + oy)
        m.sh(B + rbase + 4, root[2])
        if half == 1:
            m.sb(B + 0, 20)
        mk = 0
        for k in range(m.lbu(cp + 15)):
            dst = m.lbu(MIRROR + k) if mirror else k
            mk |= 1 << (dst & 31)
            typ = _typ(m, R)
            q = [m.lhs(a + 128 + 24 * (4 * k + i)) for i in range(4)]
            if typ in (1, 4) and cls == 1 and (m.lw(OFFQ) >> dst) & 1:
                qb = sp + 16
                for i in range(4):
                    m.mem[(qb + 2 * i) & 0x1FFFFF:(qb + 2 * i + 2) & 0x1FFFFF] = (q[i] & 0xFFFF).to_bytes(2, "little")
                bcall(m, 0x800714FC, [(qb, 8)], u32(OFFQ + 10 + 8 * dst), u32(qb), u32(qb))
                q = [m.lhs(qb + 2 * i) for i in range(4)]
            q = _mirror_q(q, typ if typ in (1, 4, 5) else 0, dst if typ in (1, 4) else 1, mirror)
            d = u32(B + qbase + 16 * dst)
            if MUT["on"] and half == 1 and k == 2:
                q[3] += 1
            for i in range(4):
                m.sh(d + 2 * i, q[i])
        masks.append(mk)
    v = masks[0] & masks[1] & m.lw(a + MASK)
    m.sw(B + 8, v)
    return v


def m_d36c(m, a0, a1, a2, a3):
    """TransitionBlend(a): root lerp and per-part slerp from B's 'from' to its 'to', over the op's h4 frames"""
    a = a0
    op = op_at(m, a)
    n = m.lhs(op + 4)
    t = tdiv(s32(u32(m.lw(a + FRAME_) << 16)), n)
    if m.lw(a + FLAGS) & 4:
        den = s32(u32(s32(m.lw(a + RATE)) * n))
        t += tdiv(s32(u32(m.lw(a + SUB) << 16)), den)
    t = u32(t)
    u = u32(0x10000 - t)
    root = []
    for c in range(3):
        x = m.call(0x8001FC90, u, u32(m.lhs(a + 1776 + 2 * c)))
        y = m.call(0x8001FC90, t, u32(m.lhs(a + 1782 + 2 * c)))
        root.append(u32(x + y))
    buf = [v & 0xFFFF for v in root]
    R = m.lw(a)
    M = m.lw(R)
    if (m.lhu(M + 14) & 1) and m.lw(R + 52) == 0:
        sc = s32(m.lw(M + 20))
        buf = [(s32(u32((s16(buf[c]) << 12) * sc)) >> 24) & 0xFFFF for c in range(3)]
    if m.lbu(op + 2) & 0x20:
        buf[0] = buf[2] = 0
        st = m.lhu(R + 544)
        if m.lhu(STANCE + 8 * st + 2) != 8 and not (u32(st - 69) < 2):
            buf[1] = 0
    sp = m.sp
    for k in range(3):
        m.mem[(sp + 24 + 2 * k) & 0x1FFFFF:(sp + 26 + 2 * k) & 0x1FFFFF] = buf[k].to_bytes(2, "little")
    bcall(m, 0x80066A60, [(sp + 24, 6)], R, u32(sp + 24))
    for k in range(m.lbu(a + XB)):
        if not ((m.lw(a + XB + 8) >> k) & 1):
            continue
        q0 = u32(a + 1788 + 16 * k)
        m.call(0x800710C0, mut(t, u32(t + 1)), q0, u32(q0 + 8), u32(sp + 16))
        q = [m.lhs(sp + 16 + 2 * i) for i in range(4)]
        for i in range(4):
            m.mem[(sp + 32 + 4 * i) & 0x1FFFFF:(sp + 36 + 4 * i) & 0x1FFFFF] = u32(q[i] << 2).to_bytes(4, "little")
        bcall(m, 0x8001005C, [(sp + 32, 16)], u32(m.lw(m.lw(a) + 4) + 24 * k + 4), u32(sp + 32))
    return 0


# ------------------------------------------------------------------------------------------------ the leaves

def m_66a60(m, a0, a1, a2, a3):
    """SetRoot(R, v): R+0x1C..0x20 := v[0..2]"""
    for k in range(3):
        v = m.lhu(a1 + 2 * k)
        m.sh(a0 + 28 + 2 * mut(k, 2 - k), v)
    return v


def m_714fc(m, a0, a1, a2, a3):
    """QuatMul(p, q, out): out = p * q, (x, y, z, w) 1.14"""
    px, py, pz, pw = (m.lhs(a0 + 2 * i) for i in range(4))
    qx, qy, qz, qw = (m.lhs(a1 + 2 * i) for i in range(4))
    w = s32(u32(pw * qw - px * qx - py * qy - pz * qz)) >> 14
    x = s32(u32(pw * qx + px * qw + py * qz - pz * qy)) >> 14
    y = s32(u32(pw * qy + py * qw + pz * qx - px * qz)) >> 14
    z = s32(u32(pw * qz + pz * qw + px * qy - py * qx)) >> mut(14, 13)
    m.sh(a2 + 0, x)
    m.sh(a2 + 2, y)
    m.sh(a2 + 4, z)
    m.sh(a2 + 6, w)
    return w & 0xFFFF


def m_710c0(m, a0, a1, a2, a3):
    """Slerp(t, q0, q1, out), 1.14 quaternions, t in 16.16; linear when the angle is tiny"""
    t, p, q, out = a0, a1, a2, a3
    d = s32(u32(sum(m.lhs(p + 2 * i) * m.lhs(q + 2 * i) for i in range(4)))) >> 14
    c = s32(u32(d << 2))
    neg = 0
    if c < 0:
        c, neg = -c, 1
    s4 = u32(0x10000 - t)
    s3 = t
    if 16 < 0x10000 - c:
        th = u32(1024 - s32(m.call(0x8001FF3C, u32(c))))
        sn = lambda ang: m.lhs(SINE + 4 * (ang & 0xFFF)) << 4
        st = sn(th)

        def coef(frac):
            v = sn(m.call(0x8001FC90, frac, th))
            if v > 0:
                if st > 0:
                    return m.call(0x80010028, u32(sn(m.call(0x8001FC90, frac, th))), u32(st))
                return u32(-s32(m.call(0x80010028, u32(sn(m.call(0x8001FC90, frac, th))), u32(-st))))
            if st > 0:
                return u32(-s32(m.call(0x80010028, u32(-sn(m.call(0x8001FC90, frac, th))), u32(st))))
            return m.call(0x80010028, u32(-sn(m.call(0x8001FC90, frac, th))), u32(-st))
        s4 = coef(u32(0x10000 - t))
        s3 = coef(t)
    if neg:
        s3 = u32(-s32(s3))
    k0 = s32(u32(s4 << 14)) >> 16
    k1 = s32(u32(s3 << mut(14, 13))) >> 16
    for i in range(4):
        v = (s32(u32(k0 * m.lhs(p + 2 * i))) >> 14) + (s32(u32(k1 * m.lhs(q + 2 * i))) >> 14)
        m.sh(out + 2 * i, v)
    return None


def m_1005c(m, a0, a1, a2, a3):
    """QuatToMatrix(M, q): q = s32[4] (x, y, z, w) 16.16 -> the 3x3 at M (4.12), s = 2 / |q|^2"""
    x, y, z, w = (s32(m.lw(a1 + 4 * i)) for i in range(4))
    fm = lambda p, q: s32(u32((p * q) >> 16))
    n = u32(fm(x, x) + fm(y, y) + fm(z, z) + fm(w, w))
    hi = (0x20000 // n) & 0xFFFFFFFF
    lo = (u32((0x20000 % n) << 16) // n) & 0xFFFF
    s = s32(u32((hi << 16) | lo))
    xs, ys, zs = fm(x, s), fm(y, s), fm(z, s)
    wx, yz = fm(w, xs), fm(y, zs)
    st = lambda off, v: m.sh(a0 + off, s32(u32(v)) >> 4)
    st(14, wx + yz)
    xx = fm(x, xs)
    st(10, yz - wx)
    yy, zz = fm(y, ys), fm(z, zs)
    st(0, 0x10000 - (yy + zz))
    st(8, 0x10000 - (xx + zz))
    wz = fm(w, zs)
    st(16, 0x10000 - (xx + yy))
    xy, wy, xz = fm(x, ys), fm(w, ys), fm(x, zs)
    st(2, xy - wz)
    st(6, xy + wz)
    st(4, xz + wy)
    st(12, mut(xz - wy, xz + wy))
    return None


# (entry, name, frame, model) - frame = the function's own stack frame (0x800714FC opens 8 bytes mid-body)
MODELS = [
    (0x8005BD74, "Restart", 32, m_bd74), (0x8005BE0C, "Stop", 0, m_be0c), (0x8005BE20, "StopIfPlaying", 0, m_be20),
    (0x8005BE44, "Resume", 0, m_be44), (0x8005BE58, "ClipDone", 0, m_be58), (0x8005BEF4, "ChannelFree", 0, m_bef4),
    (0x8005BF6C, "HardStart", 24, m_bf6c), (0x8005C018, "RangedStart", 24, m_c018),
    (0x8005C0B0, "LoopStart", 24, m_c0b0), (0x8005C140, "Transition", 56, m_c140),
    (0x8005C338, "QuatGet", 0, m_c338), (0x8005C36C, "QuatSet", 0, m_c36c), (0x8005C39C, "ClipSelect", 24, m_c39c),
    (0x8005C418, "Seq", 0, m_c418), (0x8005C4EC, "Tick", 0, m_c4ec), (0x8005C52C, "LoopRestart", 32, m_c52c),
    (0x8005C58C, "Advance", 40, m_c58c), (0x8005C8F4, "PoseStart", 32, m_c8f4), (0x8005CB04, "AdvanceFrame", 24, m_cb04),
    (0x8005CB70, "TransitionCapture", 112, m_cb70), (0x8005D2A8, "ApplyFrame", 32, m_d2a8),
    (0x8005D36C, "TransitionBlend", 80, m_d36c), (0x8005D63C, "Pose", 136, m_d63c),
    (0x8005E1D8, "AnimationPass", 40, m_e1d8), (0x8005E2BC, "DecodeHeader", 0, m_e2bc), (0x8005E558, "Bits", 0, m_e558),
    (0x8005E5A4, "Sample", 56, m_e5a4),
    (0x80066A60, "SetRoot", 0, m_66a60), (0x800714FC, "QuatMul", 8, m_714fc), (0x800710C0, "Slerp", 56, m_710c0),
    (0x8001005C, "QuatToMatrix", 0, m_1005c),
]

# ------------------------------------------------------------------------------------------------ the clock writers
# Three helpers of the presentation pass (RASHCDG 0x80090814) that SET the clip clock of a rider leaving the bike
# (mount 2) from the bike's physics, freeze it (flags bit 3: AnimationPass then skips AdvanceFrame) and call
# RiderLaunch 0x80091468 when the fall is over. Not part of the machine's text, but they write its state.

def _clipref(m, A):
    op = op_at(m, A)
    cp = m.lw(u32(m.lw(m.lw(A + BANK) + 4) + 4 * m.lbu(op)))
    return s16(m.lhu(cp + 16) - 1)


def _scrub(m, A, t, last):
    """flags |= 8; frame = (t * last) >> 16; sub = rate * frac >> 16; acc = 0"""
    prod = s32(u32(s32(t) * last))
    m.sw(A + FLAGS, m.lw(A + FLAGS) | 8)
    m.sw(A + FRAME_, prod >> 16)
    fr = s32(m.lw(A + FRAME_))
    m.sw(A + SUB, s32(u32(s32(m.lw(A + RATE)) * s32(u32(prod - u32(fr << 16))))) >> 16)
    m.sw(A + ACC, 0)
    return prod


def _clamp1(v):
    """max(v, 0) + min(1.0 - v, 0), as the and/nor/sra masks compute it"""
    v = s32(v)
    a = (~(v >> 31)) & v
    b = s32(u32(0x10000 - v))
    return u32(a + ((b >> 31) & b))


def _set_last(m, R, A, last, launch=True):
    m.sw(A + FLAGS, m.lw(A + FLAGS) | 8)
    m.sw(A + FRAME_, last)
    fr = s32(m.lw(A + FRAME_))
    m.sw(A + SUB, s32(u32(s32(m.lw(A + RATE)) * s32(u32((last - fr) << 16)))) >> 16)
    m.sw(A + ACC, 0)
    return m.call(0x80091468, R)


def m_c31cc(m, a0, a1, a2, a3):
    """FallScrubLean(R) - stances 39 / 89: the clip follows the bike's +0x27C (the lean angle)"""
    R = a0
    s1 = (m.lw(R + 0x228) >> 27) & 1
    B = m.lw(R + 0x254)
    fc, s0, pas = m.lw(B + 0x238), s32(m.lw(B + 0x27C)), m.lw(B + 0x358)
    s2 = (fc >> 5) & 1
    s4 = (u32(s0) >> 31) if (pas != 0 and s1 == 0) else 0
    A = m.lw(R + 0x21C)
    last = _clipref(m, A)
    if s2:
        if s4:
            scrub = s32(0xFFFF79F6) < s0
        elif s0 == 0:
            scrub = False
        elif last == 0:
            scrub = not (s0 < 0)
        else:
            scrub = s0 <= 0
    else:
        if m.call(0x8005BE58, A) != 0:
            scrub = False
        else:
            ab = s32(u32((s0 + (s0 >> 31)) ^ (s0 >> 31)))
            if 0xC90F < ab:
                scrub = False
            else:
                return None                                  # 0x800C335C: nothing to do
    if not scrub:
        _set_last(m, R, A, last + mut(0, 1))
        return None
    a0_ = u32(u32(-last) & u32(-s0 - s0))                   # 0x800C3364: the mask is the clip's LAST frame
    a0_ = u32(s0 + a0_)
    v = s32(m.call(0x8001FC90, a0_, 0x1E8EC))
    t = -v
    if not s4:
        t += 0x10000
    prod = s32(u32(t * last))
    m.sw(A + FLAGS, m.lw(A + FLAGS) | 8)
    m.sw(A + FRAME_, prod >> 16)
    fr = s32(m.lw(A + FRAME_))
    m.sw(A + SUB, s32(u32(s32(m.lw(A + RATE)) * s32(u32(prod - u32(fr << 16))))) >> 16)
    m.sw(A + ACC, 0)
    m.sw(A + SUB, 0)
    return None


def m_c341c(m, a0, a1, a2, a3):
    """FallScrubLift(R) - stances 40 / 88: the rider takes the bike's frame; the clip follows the bike's +0x268"""
    R = a0
    s0 = m.lw(R + 0x254)
    s1 = s32(m.lw(s0 + 0x268))
    if m.lbu(R + 0x23C) & 0x20:
        s0 = m.lw(s0 + 0x358)
    A = m.lw(R + 0x21C)
    last = _clipref(m, A)
    for k in range(3):
        m.sw(R + 0x244 + 4 * k, u32(m.lw(s0 + 0xB8 + 4 * k) - m.lw(s0 + 0x1F8 + 4 * k)))
    m.call(0x8003FA18, 9, u32(s0 + 0x1B0), u32(R + 0x1B0))
    if s32(m.lw(s0 + 0x26C)) >= 0 or s1 < s32(0xFFFF4D48):
        q = tdiv(s32(u32(m.lw(A + SUB) << 16)), s32(m.lw(A + RATE)))
        v1 = u32(u32(m.lw(A + FRAME_) << 16) + q + u32(last << 16))
        m.sw(A + FLAGS, m.lw(A + FLAGS) | 8)
        m.sw(A + FRAME_, s32(v1) >> 16)
        fr = s32(m.lw(A + FRAME_))
        m.sw(A + SUB, s32(u32(s32(m.lw(A + RATE)) * s32(u32(v1 - u32(fr << 16))))) >> 16)
        m.sw(A + ACC, 0)
        return m.call(0x80091468, R)
    ab = s32(u32((s1 + (s1 >> 31)) ^ (s1 >> 31)))
    t = _clamp1(m.call(0x8001FC90, u32(ab), mut(0x16EB1, 0x16EB2)))
    _scrub(m, A, t, last)
    return None


def m_c3630(m, a0, a1, a2, a3):
    """FallScrubPitch(R) - stance 38 (and a passenger's 39 / 89 with flagsC bit 6): the clip follows |bike+0x28C|"""
    R = a0
    B = m.lw(R + 0x254)
    A = m.lw(R + 0x21C)
    x = s32(m.lw(B + 0x28C))
    ab = s32(u32((x + (x >> 31)) ^ (x >> 31)))
    last = _clipref(m, A)
    if 0x138C2 < ab:
        return _set_last(m, R, A, last)
    t = _clamp1(m.call(0x8001FC90, u32(ab + s32(0xFFFF4D48)), mut(0x1E8EC, 0x1E8ED)))
    _scrub(m, A, t, last)
    return None


MODELS += [(0x800C31CC, "FallScrubLean", 40, m_c31cc), (0x800C341C, "FallScrubLift", 40, m_c341c),
           (0x800C3630, "FallScrubPitch", 32, m_c3630)]
