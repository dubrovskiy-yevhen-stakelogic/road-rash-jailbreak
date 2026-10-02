"""Model of SLUS 0x8001A760 (sha1 67ed165a...), 2788 B (prologue .. last `jr ra` + delay slot), frame 128:
    void RiderSpeech(u32 handle, s32 isCrash)
(name ours). The rider VOICE event: for every player p (the listener p of *(gp+1920), 72-byte records) it picks
a line category from a candidate list chosen by (isCrash, is-player, riderDef+1), searches the 9 x 2 speech table
0x800D6AA0 for a slot of that category starting at a position drawn from the HARDWARE root counter 2
(GetRCnt SLUS 0x80043F00 twice per candidate), then either queues the line on a free speech voice of listener p
(*(gp+1924)[p] + 44*i, i in [L+0x24, L+0x24+L+0x28)) or plays it at once through PlaySound3D. With isCrash == 0
(a taunt: AiChooseCommand idx 6, FightSteer, the player's combat input) it also PROVOKES: the grid picker's
target of the speaker gets grudge 15 against him and, if it is racing / manoeuvring and CanEngage agrees, a
0x800B92C0 weapon pick and an AI command {16 = fight, speaker} (mode 2); and the speaker's rider gets
+0x23C bit 2 / bit 3 from the side the other bike is on. Every store pc is in a comment."""
import pairs as P
u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 128
NAME = "RiderSpeech"

GS, POOL, PLAYERS, TAB, AIMAP = 0x8005B2F8, 0x8005B3A0, 0x8005B268, 0x800D6AA0, 0x800D38B0
STOPV, RCNT, PICK, CANENG, WEAPON = 0x80017814, 0x80043F00, 0x8008B428, 0x800BC1EC, 0x800B92C0
PUSH, PROJ, PITCH, STARTV, SND3D = 0x800BCA68, 0x800B6AAC, 0x8001F934, 0x8001769C, 0x80017BA0
COVER = set()
CALLS = []                 # per call: [a0, a1, arm labels...]; dev.py prints them per run with --arms
_cur = [None]


def arm(x):
    COVER.add(x)
    _cur[0].append(x)


def mut():
    return bool(P.MUTATE["on"])


def model(m, a0, a1, a2, a3):
    _cur[0] = [hex(a0), a1, "ra=%x" % m.regs["ra"]]
    CALLS.append(_cur[0])
    gp = m.regs["gp"]
    h, crash = a0, a1
    m.sw(m.entry_sp + 0, a0)                          # 0x8001A77C: the home slots are the caller's words
    m.sw(m.entry_sp + 4, a1)                          # 0x8001A790
    S1 = u32(m.lw(POOL) + u32(1096 * h))              # the speaker's bike
    s8, s4, s6 = 255, 0, 0
    sp44 = sp48 = sp52 = sp56 = sp64 = 0              # sw zero at 0x8001A7C4..0x8001A7D8
    sp68 = sp72 = 0
    gs = m.lw(GS)
    if m.lw(gs + 0x30) == 0:
        return None
    p, sp76, sp80 = 0, 0, 0
    while True:
        # ---- 0x8001A7F4: one listener p
        if m.lw(u32(gp + 1924 + sp80)) == 0:
            arm("no-speech-voices")
            return None
        s5 = m.lw(PLAYERS + sp80)
        nxt = False
        if crash == 0:
            if 0xF000 < iabs(s32(u32(m.lw(s5 + 0x144) - m.lw(S1 + 0x144)))):
                arm("taunt-too-far"); nxt = True
            elif not (m.lhu(S1 + 0xAC) < m.lw(gs + 0x30)):
                if m.lw(u32(m.lw(gp + 1920) + sp76) + 0x2C) != 0 and m.lw(S1 + 0x440) != 0:
                    arm("listener-busy"); nxt = True
        if not nxt:
            r = body(m, gp, h, crash, S1, s5, p, sp76, sp80, (s8, s4, s6, sp44, sp48, sp52, sp56, sp64, sp68, sp72))
            if r is None:
                return None
            s8, s4, s6, sp44, sp48, sp52, sp56, sp64, sp68, sp72 = r
        # ---- 0x8001B1D8
        sp76 += 72
        sp80 += 4
        gs = m.lw(GS)
        p += 1
        if not (p < m.lw(gs + 0x30)):
            return None


def body(m, gp, h, crash, S1, s5, p, sp76, sp80, st):
    s8, s4, s6, sp44, sp48, sp52, sp56, sp64, sp68, sp72 = st
    np_ = lambda: m.lw(m.lw(GS) + 0x30)
    player = lambda: m.lhu(S1 + 0xAC) < np_()                # sltu
    # ---- 0x8001A8A0: is this speaker already talking on one of listener p's speech voices?
    L = u32(m.lw(gp + 1920) + sp76)
    s7 = m.lw(L + 0x24)
    base = m.lw(u32(gp + 1924 + sp80))
    s2 = u32(base + u32(44 * s7))
    sp40 = u32(s7 + m.lw(L + 0x28))
    s3 = s7
    while s3 < sp40:                                          # sltu
        if m.lhu(s2) == u32(h) and m.lws(s2 + 8) < 3:        # bne v0,t0 (full word); slti
            if crash == 0 and not player() and m.lw(S1 + 0x440) != 0:
                arm("ai-already-talking")
                return None                                   # 0x8001A954 -> the epilogue
            arm("stop-old-line")
            m.call(STOPV, s3, p)
            break
        s3 = u32(s3 + 1)
        s2 = u32(s2 + 44)
    # ---- 0x8001A988
    if sp64 != 0:
        arm("reuse-line")
        sp68, sp72 = sp52, sp56
    else:
        # ---- the candidate categories, sp+16.. terminated by 255
        if crash != 0:
            cand = [3, 11, 255]; arm("cand-crash")
        else:
            gs = m.lw(GS)
            a3 = m.lw(gs + 0x30)
            if m.lhu(S1 + 0xAC) < a3:
                if (m.lbu(m.lw(S1 + 0x43C) + 1) & 0xF) == 2 and m.lbu(gs + 4) != 33:
                    cand = [7, 255]; arm("cand-player-cop")
                else:
                    cand = [u32((h << 4) | 6), 255]; arm("cand-player")
                sp44 = 1
            else:
                b = m.lbu(m.lw(S1 + 0x43C) + 1)
                if (b & 0xF) == 2 or m.lbu(gs + 4) == 33:
                    cand = [7, 15, 255]; sp44 = 1; arm("cand-ai-cop")
                elif u32((b >> 4) - 1) < 2 or (b >> 4) == 5:
                    b2 = m.lbu(m.lw(S1 + 0x43C) + 1)
                    first = (b & 3) if a3 == 2 else m.lbu(m.lw(S1 + 0x43C) + 1)
                    cand = [first, (b2 & 3) + 4, (m.lbu(m.lw(S1 + 0x43C) + 1) & 3) + 68]
                    arm("cand-ai-own")
                else:
                    cand = [(b & 3) + 4, (m.lbu(m.lw(S1 + 0x43C) + 1) & 3) + 68, 255]
                    arm("cand-ai-generic")
        # ---- 0x8001AB2C: the table search, the start drawn from root counter 2
        s0 = 0
        for k in range(3):
            v = cand[k]
            if v == 255:
                break
            s8 = v
            r1 = m.call(RCNT, 0xF2000002)
            s6 = (r1 & 0xFF) >> 7                              # column 0/1
            r2 = m.call(RCNT, 0xF2000002)
            s4 = s32((r2 & 0xFF) * 9) >> 8                     # row 0..8
            _cur[0].append("rc%02x/%02x" % (r1 & 0xFF, r2 & 0xFF))
            for _ in range(9):
                if m.lw(u32(TAB + 4 * s6 + 32 * s4 + 12)) == s8:
                    s0 = 1
                else:
                    s6 ^= 1
                    if m.lw(u32(TAB + 4 * s6 + 32 * s4 + 12)) == s8:
                        s0 = 1
                if s0:
                    break
                s4 += 1
                if not (s4 < 9):
                    s4 = 0
            if s0:
                break
        if not s0:
            arm("no-line")
            return None                                        # 0x8001AC1C -> the epilogue
        arm("cat=%d" % s8)
        if s8 == 6 or ((m.lbu(m.lw(S1 + 0x43C) + 1) & 0xF) == 2 and player()):
            s6 = 1 if m.lw(gp + 1972) == 0 else 0              # sltiu 1: the alternating column
            m.sw(gp + 1972, s6)                                # 0x8001AC74
            arm("alternate")
        if m.lw(S1 + 0x440) != 0:
            if u32(s8 - 68) < 2 or (s8 == 7 and not player()):
                a0v = s32(m.lw(m.lw(GS) + 0xC)) >> 8           # the race clock / 256
                d = (a0v & 0xFFFF) - m.lhs(S1 + 0x366)
                if iabs(d) < 21:
                    arm("rate-limited")
                    return None                                # 0x8001ACEC -> the epilogue
                m.sh(S1 + 0x366, a0v)                          # 0x8001ACF4
                arm("rate-stamp")
        rec = u32(TAB + 32 * s4)
        sp68 = m.lw(rec)                                       # the bank
        sp72 = m.lw(u32(rec + 4 * s6 + 20))                    # the sound index in the bank
        _cur[0].append("row%d/col%d=bank%d:snd%d" % (s4, s6, sp68, sp72))
    # ---- 0x8001AD30: a free speech voice of listener p
    s3 = s7
    L = u32(m.lw(gp + 1920) + sp76)
    s2 = u32(m.lw(u32(gp + 1924 + sp80)) + u32(44 * m.lw(L + 0x24)))
    s0 = 0
    while s3 < sp40:
        if m.lw(s2 + 4) == 0:
            s0 = 1
            break
        s3 = u32(s3 + 1)
        s2 = u32(s2 + 44)
    if not s0:
        if crash == 0 and not player():
            arm("ai-no-voice")
            return (s8, s4, s6, sp44, sp48, sp52, sp56, sp64, sp68, sp72)     # 0x8001ADD4 -> next listener
        sp48 = 1
        arm("no-voice-3d")
    # ---- 0x8001ADE0: the provocation (taunts only)
    if crash == 0:
        s5x = s5
        if player() or m.lw(S1 + 0x440) == 0:
            s0b = S1 if m.lw(S1 + 0x440) != 0 else m.lw(S1 + 0x358)
            t = m.call(PICK, s0b + 0xAC, 1) & 0xFFFF
            arm("pick")
            if t != 224:
                s5x = u32(m.lw(POOL) + u32(1096 * t))
                d = u32(u32(m.lw(s5x + 0x144) - m.lw(s0b + 0x144)) << 4)
                if not (0x13FFFF < iabs(s32(d))) and (m.lw(s5x + 0x230) & 0x08000000):
                    idx = m.lbu(AIMAP + m.lhu(s0b + 0xAC))
                    m.sb(u32(m.lw(s5x + 0x43C) + idx + 16), 15)             # 0x8001AED4: grudge := 15
                    arm("grudge")
                    dep = m.lbs(s5x + 0x3B2)
                    op = m.lhu(u32(s5x + 8 * (dep - 1) + 0x3BC))
                    if u32(op - 4) < 13:                                     # top op 4..16
                        if m.call(CANENG, s5x, s0b, d) != 0:
                            m.call(WEAPON, m.lw(s5x + 0x43C))
                            m.sh(m.sp + 32, 16)
                            m.sh(m.sp + 34, m.lhu(S1 + 0xAC))
                            m.call(PUSH, m.sp + 32, 2, s5x)                  # {16, speaker} on the target
                            arm("provoke")
                        else:
                            arm("canengage-0")
            else:
                arm("pick-none")
            go_side = m.lw(S1 + 0x440) != 0
        else:
            go_side = True
        if go_side:
            # ---- 0x8001AF50: which side the other bike (s5x) is on
            if m.lw(s5x + 0x168) == m.lw(S1 + 0x168) and ((m.lw(s5x + 0x168) >> 16) == 0
                                                          or m.lw(s5x + 0x150) == m.lw(S1 + 0x150)):
                lat = u32(m.lw(s5x + 0x158) - m.lw(S1 + 0x158))
                if m.lws(S1 + 0x16C) < 0:
                    lat = u32(-lat)
                arm("side-road")
            else:
                lat = m.call(PROJ, s5x + 0xB8, S1 + 0x1B0, S1 + 0xB8)
                arm("side-proj")
            R = m.lw(S1 + 0x354)
            m.sb(R + 0x23C, m.lbu(R + 0x23C) | (8 if s32(lat) < 0 else 4))   # 0x8001AFF0
    # ---- 0x8001AFF8: the table slot's category
    cell = u32(TAB + 4 * s6 + 32 * s4 + 12)
    if (s8 & 0xFE) in (4, 68):
        m.sw(cell, (s8 & 1) + 68)                              # 0x8001B030
        arm("cat->68")
    elif sp64 == 0 and not (s8 & 8) and sp44 == 0:
        m.sw(cell, 0xFFFFFFFF)                                 # 0x8001B074: a one-shot line is used up
        arm("cat->-1")
    # ---- 0x8001B078
    rec = u32(TAB + 32 * s4)
    if sp48:
        m.call(SND3D, m.lw(S1 + 0xB8), m.lw(S1 + 0xC0), sp72, sp68)
        m.sw(rec + 8, 4)                                       # 0x8001B1B4
        arm("play3d")
    else:
        m.sh(s2, h)                                            # 0x8001B090 (lhu 128(sp))
        m.sw(s2 + 12, m.lw(S1 + 0xB8))                         # 0x8001B09C
        m.sw(s2 + 16, m.lw(S1 + 0xC0))                         # 0x8001B0A8
        sp_ = s32(m.lw(S1 + 0x1E0)) >> 4
        m.sw(s2 + 20, s32(u32(m.lhs(S1 + 0x1C2) * sp_)) >> 8)   # 0x8001B0C4 (mult lo, sra 8)
        m.sw(s2 + 24, s32(u32(m.lhs(S1 + 0x1C6) * sp_)) >> 8)   # 0x8001B0EC (delay slot)
        v = m.call(PITCH, sp68, sp72)
        m.sw(s2 + 28, v)                                       # 0x8001B104
        m.sw(s2 + 8, 2)                                        # 0x8001B10C (delay slot)
        m.call(STARTV, s3, p, sp68, sp72)
        gs = m.lw(GS)
        m.sw(s2 + 36, u32(m.lw(gs + 0xC) + 500))               # 0x8001B128
        if not (m.lhu(S1 + 0xAC) < m.lw(gs + 0x30)) and crash == 0:
            m.sw(u32(m.lw(gp + 1920) + sp76) + 0x2C, 1)        # 0x8001B164: listener busy
            arm("listener-mark")
        m.sw(rec + 8, 3 + (1 if mut() else 0))                 # 0x8001B17C  (MUTATE: 4)
        m.sw(s2 + 40, s4)                                      # 0x8001B184 (delay slot)
        arm("queued")
    return (s8, s4, s6, sp44, sp48, sp68, sp72, 1, sp68, sp72)   # 0x8001B1B8: sp64 = 1, sp52/56 = the line
