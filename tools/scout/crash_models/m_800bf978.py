"""FightPickRecord RASHCDG 0x800BF978(me) - cfe43a77, 740 B, frame 24; one caller 0x800C12FC (FightBegin).
The FIGHT.BIN record from riderDef+0x3C (the command) / +0x2E (the weapon) through three jump tables, and the node
queue rider+0x23E[6]."""
import pairs as P, impact as I
u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 24
NAME = "FightPickRecord"
COPY, QUEUES = 0x8001E08C, 0x800CCAD0
JT71, JT32, JTW = 0x8005BA1C, 0x8005BA3C, 0x8005BA5C


def _jt(tab, i):
    return P.word_at(tab + 4 * i)                                # the table words, read out of the image


def model(m, a0, a1, a2, a3):
    me = a0
    rd = m.lw(me + 0x43C)
    s0 = 0
    c = m.lbu(rd + 0x3C)
    r = m.lw(me + 0x354)
    a2 = c & 0x1F
    in4 = lambda v: ((v + 114) & 0xFF) < 4                       # 142..145
    in2 = lambda v: ((v + 110) & 0xFF) < 2                       # 146..147
    if c & 0x40:
        i = c - 71
        if u32(i) < 7:
            s0, a2 = {0x800BF9D4: (10, a2), 0x800BF9DC: (11, a2), 0x800BF9E4: (17, 23)}[_jt(JT71, i)]
        else:
            s0 = 17
    elif c & 0x20:
        i = m.lbu(rd + 0x3C) - 32
        if u32(i) < 7:
            s0, a2 = {0x800BFA38: (0, a2), 0x800BFA40: (1, a2), 0x800BFA48: (16, 23)}[_jt(JT32, i)]
        else:
            s0 = 16
    else:
        w = m.lbu(rd + 0x2E)
        if w < 9:
            tgt = _jt(JTW, w)
            c = m.lbu(rd + 0x3C)
            if tgt == 0x800BFA8C:                                # weapon 4
                s0 = 8 if in4(c) else 9 if in2(c) else 15
            elif tgt == 0x800BFAC4:                              # weapon 0
                if in4(c):
                    s0 = 6
                    if c == 143:
                        a2 = 22
                else:
                    s0 = 7 if in2(c) else 14
            elif tgt == 0x800BFAFC:                              # weapons 2, 3
                if in4(c):
                    s0 = 2
                    if c == 143:
                        a2 = 22
                elif in2(c):
                    s0 = 3
                else:
                    s0, a2 = 12, 23
            elif tgt == 0x800BFB38:                              # weapons 1, 5
                if in4(c):
                    s0 = 4
                    if c == 143:
                        a2 = 22
                elif in2(c):
                    s0 = 5
                else:
                    s0, a2 = 13, 23
            elif tgt in (0x800BFB88, 0x800BFB90):                # 6, 7 -> 18; 8 -> 19
                s0 = 18 if tgt == 0x800BFB88 else 19
                a2 = 25 - (a2 & 1)
            else:
                raise P.Mismatch(f"unknown jump target {tgt:#x}")
    if m.lbu(r + 0x23C) & 0x20:                                  # a passenger: the 20..39 variant
        if (s0 & 0xFF) == 19:
            s0, a2 = 12, 23
        s0 += 20
    m.sb(r + 0x239, s0)                                          # 0x800BFBCC  the record
    m.sb(r + 0x23A, 0)                                           # 0x800BFBD0  node index
    if not (m.lw(me + 0x230) & 0x08000000) and not (m.lbu(r + 0x23C) & 0x40):   # the human: {0, 63 x 5}
        m.sb(r + 0x23E, 0)                                       # 0x800BFBFC
        for i in range(1, 6):
            m.sb(r + 0x23E + i, 63)                              # 0x800BFC0C
    else:                                                        # the AI: a canned 6-node queue
        q = u32(QUEUES + 6 * a2 + (6 if P.MUTATE["on"] else 0))
        m.call(COPY, u32(r + 0x23E), q, 6)                       # 0x800BFC40  SLUS byte copy
    return s0 & 0xFF
