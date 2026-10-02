r"""Reading pass D's runs: RiderSpeech SLUS 0x8001A760 and Remount RASHCDG 0x800903F4.
Every entry is impact.py's spec form (name, source, build, frames, pad, explore, note); build(ram) returns
[(addr, size|'add', value)]. Placements are box / road-state moves; every other edit says NOT a placement."""
import impact as I
import pairs as P

u32 = P.u32
FX = I.FX
PLAYER, BIKE6, RIDER6 = P.PLAYER, P.BIKE6, P.RIDER6
BIKE = I.BIKE
RIDER_BOX = [0x0B8] + [0x0C4 + 12 * k for k in range(8)]               # spawn.py's BOX (centre + 8 corners)
BIKE_BOX = [0x1D4, 0x0B8, 0x1F8, 0x310] + [0x0C4 + 12 * k for k in range(8)]


def shift_x(e, fields, dx):
    """placement: every position field of e moved dx along world x"""
    return [(e + f, "add", dx) for f in fields]


def retire(k):
    """quick: live AI bike k moved 600 units along x (placement, spawn.py `retire`): the activation pass retires it
    in frame 1; its rider is seated (+0x25C = 1 < 3) and game_state+0x39 is 0 in quick, so the retirement calls
    Remount(e, 1) at 0x80094078"""
    return lambda ram: shift_x(BIKE(k), BIKE_BOX, FX(600))


def down_far(ram):
    """quick: the crashed bike 6 AND its downed rider 6 (mount 4) moved 600 units along x (placements): in frame 1
    the activation pass retires bike 6 (rider down and live: no remount there, flagsC |= 0x100), then the downed
    rider pass finds rider 6 out of the window with its bike dormant: Remount(bike 6, 1) at 0x800953F4"""
    return shift_x(BIKE6, BIKE_BOX, FX(600)) + shift_x(RIDER6, RIDER_BOX, FX(600))


def two_up(stop=True):
    """rr-race: a PLANTED passenger - the player's +0x358 := bike 6 (NOT a placement) and his rider's +0x23C |= 0x10
    (NOT a placement: FORCED "has a passenger"); bike 6's rider is down in state 73 (category 8, remountable) in
    rr-race; the player's speed +0x1E0 / +0x240 := 0 (NOT a placement): RaceDirector's arm
    calls Remount(player, 1) at 0x800B9538"""
    def f(ram):
        pr = I._rd(ram, PLAYER + 0x354, "<I")
        ed = [(PLAYER + 0x358, 4, BIKE6)] + I.orw(ram, pr + 0x23C, 0x10, 1)
        if stop:
            ed += [(PLAYER + 0x1E0, 4, 0), (PLAYER + 0x240, 4, 0)]
        return ed
    return f


_IR = {r[0]: r for r in I.RUNS}
REUSE = ["launch38", "launch39", "launch40", "launch41", "launch42", "launch90", "crash", "crash-90", "crash-4",
         "pole", "pole-fast", "bb-side", "bb-rear90", "ride-over", "ko-forced", "ko-forced16", "police",
         "punch-miss", "punch-cop", "knock-cop", "swing-cop", "punch-last", "ai-punch", "stance-l1",
         "decode2wNone", "decode3wNone", "decode4wNone", "decode8wNone", "decode7w5", "decode5w7", "decode1w0",
         "decode6w4"]

RUNS = [_IR[n] for n in REUSE if n in _IR] + [
    ("retire5", I.QUICK, retire(5), 4, "0xFFFF", False, "quick: live bike 5 moved 600 along x (placement)"),
    ("down-far", I.QUICK, down_far, 4, "0xFFFF", False, "quick: bike 6 and its downed rider 6 moved 600 along x"),
    ("two-up", I.RR, two_up(), 6, "0xFFFF", False,
     "rr-race: player's +0x358 := bike 6 (PLANTED), his rider +0x23C |= 0x10 (FORCED), speed 0 (NOT a placement)"),
]

PLAN_PASS, PLAN_ACC = 0x8005B2A8, 0x8005B30C


def planner_due(ram):
    """NOT placements: the AI planner's pass counter *(0x8005B2A8) := 1 and its accumulator *(0x8005B30C) := 0x7800,
    one frame (dt 0x884) short of its 0x8000 period, so RaceTick calls AiPlan(acc) in frame 1"""
    return [(PLAN_PASS, 4, 1), (PLAN_ACC, 4, 0x7800)]


def ai_taunt(ram):
    """quick: AI bike 15 put 1.5 along the player's +0x1B0 row (placement); its riderDef+0x02 mood low nibble := 15
    (NOT a placement; 0x77 -> 0x7F) so that AiChooseCommand's mood gate sets idx bit 2 (idx 6 = taunt + fight);
    the planner due in frame 1 (NOT placements)"""
    rd15 = I._rd(ram, BIKE(15) + 0x43C, "<I")
    return I.place_bike(ram, 15, 1.5, 0.0) + [(rd15 + 2, 1, (I._rd(ram, rd15 + 2, "<B") & 0xF0) | 0xF)] + planner_due(ram)


def near15(ram):
    """quick: AI bike 15 put 1.5 along the player's +0x1B0 row (placement)"""
    return I.place_bike(ram, 15, 1.5, 0.0)


def cop_player(ram):
    """quick: the player's riderDef class nibble := 2 (a cop; NOT a placement, FORCED as impact.police does)"""
    pd = I._rd(ram, PLAYER + 0x43C, "<I")
    return [(pd + 1, 1, (I._rd(ram, pd + 1, "<B") & 0xF0) | 2)]


L2 = "0xFEFF"          # the player's control 8 in both savestates' pad configuration: found by the btn-* probe
RUNS += [
    ("taunt", I.RR, None, 12, L2, False, "rr-race, L2 held: the player's speech button"),
    ("taunt-q", I.QUICK, None, 14, L2, False, "quick, L2 held"),
    ("taunt-q15", I.QUICK, near15, 14, L2, False, "quick, bike 15 1.5 beside the player (placement), L2 held"),
    ("taunt-cop", I.QUICK, cop_player, 14, L2, False, "quick, the player a cop (FORCED), L2 held"),
    ("ai-taunt", I.QUICK, ai_taunt, 14, "0xFFFF", False,
     "quick, bike 15 beside the player (placement), its mood 15 and the planner due (NOT placements)"),
]


def with_(*fs):
    return lambda ram: [e for f in fs for e in f(ram)]


def forced_class2(ram):
    """NOT a placement: the player's riderDef class nibble := 2 (a cop; FORCED, as impact.police does)"""
    return cop_player(ram)


def ai_taunt_k(k):
    """quick: AI bike k put 1.5 along the player's +0x1B0 row (placement); its riderDef+0x02 low nibble := 15 (NOT
    a placement: mood forced); the planner due in frame 1 (NOT placements)"""
    def f(ram):
        rdk = I._rd(ram, BIKE(k) + 0x43C, "<I")
        return I.place_bike(ram, k, 1.5, 0.0) + [(rdk + 2, 1, (I._rd(ram, rdk + 2, "<B") & 0xF0) | 0xF)] + planner_due(ram)
    return f


def race_type(t):
    """NOT a placement: game_state+4 (the race type byte) := t (FORCED)"""
    return lambda ram: [(I._rd(ram, P.GS, "<I") + 4, 1, t)]


def used_5(ram):
    """NOT a placement: the speech table's two category-5 cells (0x800D6AA0 row 3 and row 6, column 1, +0x10) :=
    69, the value 0x8001B030 writes into a category-4/5 cell when its line has been spoken (as if both had been)"""
    return [(0x800D6AA0 + 32 * 3 + 16, 4, 69), (0x800D6AA0 + 32 * 6 + 16, 4, 69)]


def stamp_now(k):
    """NOT a placement: bike k's +0x366 (the speech rate stamp) := game_state+0x0C >> 8 (as if it had just spoken)"""
    def f(ram):
        gs = I._rd(ram, P.GS, "<I")
        return [(BIKE(k) + 0x366, 2, (I._rd(ram, gs + 0xC) >> 8) & 0xFFFF)]
    return f


def cat7(ram):
    """NOT a placement: the speech table's row 8 column 1 category (+0x10, 0xFF = none in both savestates) := 7"""
    return [(0x800D6AA0 + 32 * 8 + 16, 4, 7)]


def road_plus(k):
    """a road-state edit (counted as a placement, but NOT consistent: the box stays where it is): bike k's road id
    +0x168 += 1, so that it and the player are on different road pieces for 0x8001A760's side test"""
    return lambda ram: [(BIKE(k) + 0x168, "add", 1)]


def finished(k):
    """NOT a placement: bike k's riderDef+0x28 (finish time) := 1 (FORCED "finished")"""
    return lambda ram: [(I._rd(ram, BIKE(k) + 0x43C, "<I") + 0x28, 4, 1)]


def jailbreak2(ram):
    """NOT placements: game_state+4 := 44 and game_state+0x39 := 2 (FORCED: the Jailbreak race type in phase 2)"""
    gs = I._rd(ram, P.GS, "<I")
    return [(gs + 4, 1, 44), (gs + 0x39, 1, 2)]


def reversed_(ram):
    """NOT a placement: the player's flagsC (+0x238) |= 0x400000 (FORCED: the "reversed on its road" bit the remount
    reads)"""
    return I.orw(ram, PLAYER + 0x238, 0x400000)


_crash = _IR["crash"][2]
RUNS += [
    ("crash-cop", I.RR, with_(_crash, forced_class2), 20, "0xBFFF", False,
     "impact crash + the player's class nibble 2 (FORCED)"),
    ("ai-taunt-own", I.QUICK, with_(ai_taunt, lambda ram: [(I._rd(ram, BIKE(15) + 0x43C, "<I") + 1, 1, 0x51)]), 14,
     "0xFFFF", False, "ai-taunt + bike 15's riderDef+1 := 0x51 (NOT a placement: its character nibble 8 -> 5, FORCED;"
     " the class nibble 1 kept)"),
    ("ai-taunt-full", I.QUICK, with_(ai_taunt, I.knockoff_requests({k: v for k, v in I.KO_SPEC.items() if k != 15}),
                                     lambda ram: [(PLAN_ACC, 4, 0x8000 - 2 * 0x884 + 0x10)]), 14, "0xFFFF", False,
     "ai-taunt, the planner due in frame 2 (NOT a placement) and 12 FORCED knock-off requests (impact KO_SPEC "
     "without bike 15): the crash lines of frame 1 fill listener 0's three speech voices first"),
    ("ai-taunt-33", I.QUICK, with_(ai_taunt, race_type(33)), 14, "0xFFFF", False, "ai-taunt + race type 33 (FORCED)"),
    ("ai-taunt-33c7", I.QUICK, with_(ai_taunt, race_type(33), cat7), 14, "0xFFFF", False,
     "ai-taunt-33 + a category-7 cell planted"),
    ("ai-taunt-69", I.QUICK, with_(ai_taunt, used_5), 14, "0xFFFF", False, "ai-taunt, both category-5 lines used"),
    ("ai-taunt-69rl", I.QUICK, with_(ai_taunt, used_5, stamp_now(15)), 14, "0xFFFF", False,
     "ai-taunt-69 + bike 15's speech stamp = now"),
    ("retire5-done", I.QUICK, with_(retire(5), finished(5)), 4, "0xFFFF", False, "retire5 + rider 5 finished (FORCED)"),
    ("two-up-rev", I.RR, with_(two_up(), reversed_), 6, "0xFFFF", False, "two-up + the player's flagsC bit 22 (FORCED)"),
]

