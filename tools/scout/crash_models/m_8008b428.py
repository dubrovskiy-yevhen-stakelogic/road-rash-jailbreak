# Pick 0x8008B428(u16 *handleField, u32 poolMask) - RASHCDG cfe43a77, 1060 B, frame 56, no calls.
# The nearest entity (octagonal XZ distance) in the collision grid, searched in growing
# squares around the caller's own cell; returns its u16 handle (entity +0xAC), 224 = none.
import pairs as P, impact as I
u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 56
NAME = "Pick"
GS, POOL0, POOL1 = 0x8005B2F8, 0x8005B3A0, 0x8005B3A4
OX, OZ = 0x800CCF98, 0x800CCFA0          # grid origins, player 1 / player 2 at +4
GRID, NODES = 0x800CD0B0, 0x800CCFA8      # u8 grid[48][24], {u8 prev, u8 handle}[129]
DESC, HOFF = 0x800CE4D0, 0x800CCA68       # pools >= 2: {base, stride, ..} x 16 B; handle-field offset per pool - 2


def model(m, a0, a1, a2, a3):
    m.sw(m.entry_sp + 4, a1)                                   # 0x8008B43C: a1 spilled to its home slot
    myh = m.lhu(a0)                                            # sh a0,4(sp) (own frame)
    x, z = m.lw(a0 + 12), m.lw(a0 + 20)                        # +0xB8 / +0xC0 relative to +0xAC
    col = (s32(x - m.lw(OX)) >> 21) + 11
    row = (s32(z - m.lw(OZ)) >> 21) + 11
    np_ = m.lw(m.lw(GS) + 48)
    if np_ != 1:
        if not (u32(col) < 24 and u32(row) < 24):             # re-bin against player 2's origin
            col = (s32(x - m.lw(OX + 4)) >> 21) + 11
            row = (s32(z - m.lw(OZ + 4)) >> 21) + 11
            row = s32(row + u32(24 << ((row >> 31) & 31)))      # + 24 when row >= 0 (24 << 31 == 0)
    s0 = 1
    lim = s32(u32(24 << ((m.lw(m.lw(GS) + 48) - 1) & 31)))
    s8 = 24 if (row >= 24 and row < lim) else 0                # player 2's half of the grid
    row -= s8
    best, bestd = 224, 0x7FFF0000
    mymut = P.MUTATE["on"]
    while u32(col) < 24 or u32(row) < 24 or u32(col + s0) < 24 or u32(row + s0) < 24:   # 0x8008B7E4..0x8008B814
        r0 = row if row >= 0 else 0
        c0 = col if col >= 0 else 0
        c1 = col + s0 if col + s0 - 24 < 0 else 24
        r1 = row + s0 if row + s0 - 24 < 0 else 24
        r = r0
        while r < r1:
            c = c0
            while c < c1:
                n = m.lbu(GRID + (s8 + r) * 24 + c)
                while n != 128:
                    h = m.lbu(NODES + 2 * n + 1)
                    pool = h >> 5
                    if m.lw(m.entry_sp + 4) & (1 << pool):
                        cand = None
                        if pool == 0:
                            e = u32(m.lw(POOL0) + 1096 * h)
                            hh = m.lhu(e + 172)
                            if hh != myh or mymut:                 # MUTATION: the self test dropped
                                cand = (hh, m.lw(e + 184), m.lw(e + 192))
                        elif pool == 1:
                            e = u32(m.lw(POOL1) + 628 * (h & 31))
                            hh = m.lhu(e + 172)
                            if hh != myh and m.lw(e + 604) == 4:   # a rider only in mount state 4
                                cand = (hh, m.lw(e + 184), m.lw(e + 192))
                        else:
                            d = DESC + 16 * pool
                            p = u32(m.lw(d) + u32(m.lw(d + 4) * (h & 31)) + m.lw(HOFF + 4 * (pool - 2)))
                            hh = m.lhu(p)
                            if hh != myh:
                                cand = (hh, m.lw(p + 12), m.lw(p + 20))
                        if cand is not None:
                            hh, px, pz = cand
                            dx = iabs(s32(px - m.lw(a0 + 12)))
                            dz = iabs(s32(pz - m.lw(a0 + 20)))
                            mn = dx if dx < dz else dz
                            dist = s32(u32(dx + dz) - u32((mn + (u32(mn) >> 31)) >> 1))
                            if dist < bestd:                   # strict: the first minimum wins
                                bestd, best = dist, hh
                    n = m.lbu(NODES + 2 * n)
                c += 1
            r += 1
        s0 += 2
        col -= 1
        if best & 0xFFFF != 224:
            return best & 0xFFFF
        row -= 1
    return best & 0xFFFF
