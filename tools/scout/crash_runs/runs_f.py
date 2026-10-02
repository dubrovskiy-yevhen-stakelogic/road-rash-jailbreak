"""Reading pass F's runs (combat seams of FightUpdate 0x800C035C). impact.py's RUNS form:
(name, src, build, frames, pad, explore, note). Every edit that is not a placement is named NOT a placement."""
import struct
import pairs as P, impact as I

RR, QUICK = I.RR, I.QUICK
PLAYER, BIKE = P.PLAYER, I.BIKE
_rd = I._rd


def _from_impact(name):
    return next(r for r in I.RUNS if r[0] == name)


def top_cmd(k, op, tgt):
    """NOT a placement: bike k's TOP AI command (slot at its current depth, s8 +0x3B2) PLANTED as {op, tgt}"""
    def f(ram):
        b = BIKE(k)
        d = _rd(ram, b + 0x3B2, "<b")
        return [(b + 0x3B4 + 8 * d, 2, op), (b + 0x3B6 + 8 * d, 2, tgt)]
    return f


def rd_set(k, off, size, v):
    """NOT a placement: bike k's riderDef field +off set"""
    return lambda ram: [(_rd(ram, BIKE(k) + 0x43C, "<I") + off, size, v)]


def cat(*fs):
    return lambda ram: [e for f in fs for e in f(ram)]


def armed(k, w=2, swings=5, mask=0x0605):
    """NOT a placement: bike k's weapon block set (possession mask +0x2C, weapon +0x2E, swings +0x2F, the weapon's
    swing nibble in +0x30) as impact.cop_beside(armed=True) does for the player"""
    def f(ram):
        d = _rd(ram, BIKE(k) + 0x43C, "<I")
        return [(d + 0x2C, 2, mask), (d + 0x2E, 1, w), (d + 0x2F, 1, swings), (d + 0x30, 2, swings << (4 * w))]
    return f


RUNS = [_from_impact(n) for n in ("punch-miss", "punch-cop", "knock-cop", "swing-cop", "punch-last", "ai-punch")]

RUNS += [
    # the cop fights back: a PLANTED op 16 aimed at the player on the cop's stack, the cop's command 32 (fists: the
    # steal gate of FightUpdate), the player armed as in swing-cop and swinging (R1). NEGATIVE: the release resets the
    # cop's stack on frame 1 (0x8009508C AiClearCommands, 0x800950B0 pushes op 4; op 2 at frame 8)
    ("steal-cop", RR, cat(I.cop_beside(-0.3, armed=True), top_cmd(16, 16, 0), rd_set(16, 0x3C, 1, 32)), 30, "0xF7FF",
     False, "swing-cop + the cop's top command PLANTED {16, 0} and its riderDef+0x3C := 32 (NOT placements)"),
    ("cop-fights", RR, cat(I.cop_beside(-0.3), top_cmd(16, 16, 0)), 30, "0xF7FF", False,
     "punch-cop + the cop's top command PLANTED {16, 0} (NOT a placement): both fight"),
    # the player's own op 16 with no target: FightStart's Rand side
    ("rand-side", RR, top_cmd(0, 16, 224), 6, "0xFFFF", False,
     "rr-race, the player's top command PLANTED {16, 224} (NOT a placement): FightStart with no target"),
    # AI 15 fights the player who fights back (Cross + R1)
    ("ai-punch-r1", QUICK, I.ai_fight, 14, "0xB7FF", False, "ai-punch, pad Cross + R1: the player fights back"),
    # AI 15 with command 32 (bare fists: FightUpdate calls WeaponSteal) against the ARMED player who swings
    ("steal-ai", QUICK, cat(I.ai_fight, rd_set(15, 0x3C, 1, 32), armed(0)), 14, "0xB7FF", False,
     "ai-punch + bike 15's riderDef+0x3C := 32 and the player armed with weapon 2, 5 swings (NOT placements); "
     "Cross + R1"),
    ("steal-ai-b", QUICK, cat(I.ai_fight, rd_set(15, 0x3C, 1, 32), armed(0)), 14, "0xBFFF", False,
     "steal-ai with Cross only: the player does not swing"),
]


def both_fight(cmd, w):
    """quick: ai_fight (bike 15 beside the player with a PLANTED {16, 0}) + bike 15's riderDef+0x3C := 32 (bare
    fists, FightUpdate's steal gate) + the player's top command PLANTED {16, 15}, his riderDef+0x3C := cmd and his
    weapon block := weapon w, 5 swings (all NOT placements)"""
    return cat(I.ai_fight, rd_set(15, 0x3C, 1, 32), armed(0, w=min(w, 8)), rd_set(0, 0x2E, 1, w),
               rd_set(0, 0x3C, 1, cmd), top_cmd(0, 16, 15))


# the player's command / weapon sweep: every arm of FightPickRecord's human half, and the steal window
SWEEP = [(143, 2), (142, 2), (146, 2), (148, 3), (143, 0), (146, 0), (148, 0), (143, 4), (146, 4), (148, 4),
         (143, 1), (147, 5), (148, 1), (143, 6), (143, 8), (143, 9), (71, 9), (75, 9), (77, 9), (78, 9), (36, 9),
         (38, 9), (39, 9)]
RUNS += [(f"fight-c{c}w{w}", QUICK, both_fight(c, w), 14, "0xBFFF", False,
          f"both fight: bike 15 bare (32), the player's command {c}, weapon {w} (NOT placements, see both_fight)")
         for c, w in SWEEP]


def ai_fight_at(dlat, dalong, tgt=0):
    """quick: bike 15 put dlat along the player's +0x1B0 and dalong along his +0x210 (placement); its top command
    PLANTED {16, tgt} and the player's speed 51.0 (NOT placements) - impact.ai_fight generalised"""
    return lambda ram: (I.place_bike(ram, 15, dlat, dalong) + top_cmd(15, 16, tgt)(ram)
                        + I.spd(PLAYER, 0x330000 / 65536))


def rider_or(k, bits):
    """NOT a placement: bike k's rider +0x23C |= bits (FORCED)"""
    return lambda ram: I.orw(ram, _rd(ram, BIKE(k) + 0x354, "<I") + 0x23C, bits, 1)


def pool_full(ram):
    """NOT a placement: the animation-object pool's in-use count 0x800CE178 FORCED to its capacity 0x800CE17C"""
    return [(0x800CE178, 4, _rd(ram, 0x800CE17C, "<I"))]


RUNS += [
    ("ai-ko", QUICK, cat(I.ai_fight, rd_set(0, 0x0F, 1, 5)), 14, "0xBFFF", False,
     "ai-punch + the player's riderDef+0x0F := 5 (NOT a placement): the blow knocks him off"),
    ("ai-combo1", QUICK, cat(I.ai_fight, rd_set(15, 0x3D, 1, 0x10)), 14, "0xBFFF", False,
     "ai-punch + bike 15's riderDef+0x3D := 0x10 (combo limit 1; NOT a placement)"),
    ("ai-end77", QUICK, cat(I.ai_fight, rd_set(15, 0x3D, 1, 0x10), rider_or(15, 0x40)), 14, "0xBFFF", False,
     "ai-combo1 + bike 15's rider +0x23C |= 0x40 (FORCED, NOT a placement)"),
    ("ai-redraw", QUICK, cat(I.ai_fight, rd_set(15, 0x3D, 1, 0x72)), 14, "0xBFFF", False,
     "ai-punch + bike 15's riderDef+0x3D := 0x72 (combo nibble 2: FightRestart redraws; NOT a placement)"),
    ("ai-nunchaku", QUICK, cat(I.ai_fight, armed(15, w=0, mask=0x601), rd_set(15, 0x3C, 1, 143), pool_full), 14,
     "0xBFFF", False, "ai-punch + bike 15 armed with weapon 0 and command 143, the animation pool FORCED full "
     "(NOT placements)"),
    ("ai-chain", QUICK, cat(I.ai_fight, armed(15, w=4, mask=0x610), rd_set(15, 0x3C, 1, 143)), 14, "0xBFFF", False,
     "ai-punch + bike 15 armed with weapon 4 and command 143 (NOT placements); the pool not full"),
    ("ai-lat2", QUICK, ai_fight_at(2.0, -0.3), 14, "0xBFFF", False, "ai-punch with bike 15 2.0 across"),
    ("ai-lat16", QUICK, ai_fight_at(1.6, 0.0), 14, "0xBFFF", False, "ai-punch with bike 15 1.6 across, level"),
    ("ai-ahead13", QUICK, ai_fight_at(1.0, 13.0), 14, "0xBFFF", False, "ai-punch with bike 15 13 ahead"),
    ("ai-behind13", QUICK, ai_fight_at(1.0, -13.0), 14, "0xBFFF", False, "ai-punch with bike 15 13 behind"),
    ("ai-behind4", QUICK, ai_fight_at(1.0, -4.0), 14, "0xBFFF", False, "ai-punch with bike 15 4 behind"),
    ("far-15on14", QUICK, top_cmd(15, 16, 14), 14, "0xBFFF", False,
     "quick, bike 15's top command PLANTED {16, 14} (NOT a placement): bike 14 is ~24 units away"),
    ("far-15on14-slow", QUICK, cat(top_cmd(15, 16, 14), lambda ram: I.spd(BIKE(14), 20.0)), 14, "0xBFFF", False,
     "far-15on14 + bike 14's speed 20.0 (NOT a placement)"),
    ("far-14on15", QUICK, top_cmd(14, 16, 15), 14, "0xBFFF", False,
     "quick, bike 14's top command PLANTED {16, 15} (NOT a placement): bike 15 is ~24 units away"),
    ("two-on-player", QUICK, cat(I.ai_fight, lambda ram: I.place_bike(ram, 14, 1.8, 0.3), top_cmd(14, 16, 0)), 14,
     "0xBFFF", False, "ai-punch + bike 14 put 1.8 across / 0.3 ahead (placement) with a PLANTED {16, 0} (NOT)"),
    ("two-on-player-b", QUICK, cat(I.ai_fight, lambda ram: I.place_bike(ram, 14, 1.0, -1.5), top_cmd(14, 16, 0)), 14,
     "0xBFFF", False, "ai-punch + bike 14 put 1.0 across / 1.5 behind (placement) with a PLANTED {16, 0} (NOT)"),
    ("pack-otherroad", I.PACK, top_cmd(0, 16, 1), 3, "0xFFFF", True,
     "rr-pack, the player's top command PLANTED {16, 1} (NOT a placement): bike 1 is on road 0xA, the player on 9"),
    ("steal-cls0", QUICK, cat(both_fight(148, 1), rd_set(15, 0x01, 1, 0x80)), 14, "0xBFFF", False,
     "fight-c148w1 + bike 15's riderDef+0x01 := 0x80 (class nibble 0; NOT a placement)"),
    ("ai-vs-ai", QUICK, lambda ram: (I.place_bike(ram, 14, 1.0, -0.3, rel=P.BIKE15) + top_cmd(15, 16, 14)(ram)
                                     + top_cmd(14, 16, 15)(ram) + rd_set(15, 0x3C, 1, 32)(ram)), 14, "0xBFFF", False,
     "bike 14 put beside bike 15 (placement, 22 units); both top commands PLANTED {16, the other}, bike 15's "
     "riderDef+0x3C := 32 (NOT placements)"),
]
