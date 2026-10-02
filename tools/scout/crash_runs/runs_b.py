"""reading pass B: runs that reach the bike-bike gate RASHCDG 0x80081D7C (via BikeBikeReact 0x800AC130).
Each run = a copy of a savestate with DATA edits only, then the unmodified game in the interpreter (impact.py's
RUNS form). Every edit that is not a placement is named in the note: speeds (+0x1E0/+0x240), forced flag bits,
and the TURN of a bike (turned(): every orientation field of the bike rotated by quarter turns about the vertical
through its box centre - not a placement; exact for quarter turns)."""
import struct as _st
import impact as I, pairs as P

QUICK = I.QUICK
BIKE, FX = I.BIKE, I.FX
PLAYER, B14, B15 = P.PLAYER, P.BIKE14, P.BIKE15
_by = {r[0]: r for r in I.RUNS}


def cat(*fs):
    return lambda ram: [x for f in fs for x in f(ram)]


def fc(e, bits):
    """NOT a placement: flagsC +0x238 |= bits FORCED"""
    return lambda ram: I.orw(ram, e + 0x238, bits)


def fa(e, bits):
    """NOT a placement: flagsA +0x230 |= bits FORCED"""
    return lambda ram: I.orw(ram, e + 0x230, bits)


def pb(k, dlat, dal, v14=None, v15=None, rel=B15):
    """placement: bike k's box put dlat along bike `rel`'s +0x1B0 row and dal along its +0x210 row from its centre;
    optionally the speeds of bikes 14 / 15 (NOT placements)"""
    def f(ram):
        ed = I.place_bike(ram, k, dlat, dal, rel=rel)
        if v14 is not None:
            ed += I.spd(B14, v14)
        if v15 is not None:
            ed += I.spd(B15, v15)
        return ed
    return f


def _apply(ram, edits):
    m = bytearray(ram)
    for a, n, v in edits:
        o = a & 0x1FFFFF
        if n == "add":
            _st.pack_into("<I", m, o, (_st.unpack_from("<I", m, o)[0] + v) & 0xFFFFFFFF)
        else:
            _st.pack_into({4: "<I", 2: "<H", 1: "<B"}[n], m, o, v & ((1 << (8 * n)) - 1))
    return bytes(m)


ROWS16 = [0x1B0, 0x1B6, 0x1BC, 0x1C2, 0x204, 0x20A, 0x210, 0x32E]
PTS = [0x0C4 + 12 * j for j in range(8)] + [0x1F8, 0x310, 0x1D4]
TRIG = 0x8005624C


def turned(e, q, before):
    """NOT a placement: bike e TURNED by q quarter turns (1024 of 4096 each) about the world vertical through its box
    centre +0xB8, applied after `before`: every s16 axis row (+0x1B0/1B6/1BC/1C2/204/20A/210/32E), the velocity
    +0x1C8, the eight corners and the points +0x1F8/+0x310/+0x1D4, the heading angle +0x124 and its cos/sin
    +0x128/+0x12C (from the SLUS table 0x8005624C, as BikeVsBike rebuilds them). (x, z) -> (z, -x) per quarter
    (the heading is (sin a, ., cos a), so a -> a + 1024): exact, no rounding. The rider entity is not turned."""
    def rot(x, z, k):
        for _ in range(k % 4):
            x, z = z, -x
        return x, z

    def f(ram):
        ed = before(ram)
        m = _apply(ram, ed)
        rh = lambda a: _st.unpack_from("<h", m, a & 0x1FFFFF)[0]
        ri = lambda a: _st.unpack_from("<i", m, a & 0x1FFFFF)[0]
        out = list(ed)
        for o in ROWS16:
            x, z = rot(rh(e + o), rh(e + o + 4), q)
            out += [(e + o, 2, x & 0xFFFF), (e + o + 4, 2, z & 0xFFFF)]
        x, z = rot(ri(e + 0x1C8), ri(e + 0x1D0), q)
        out += [(e + 0x1C8, 4, x & 0xFFFFFFFF), (e + 0x1D0, 4, z & 0xFFFFFFFF)]
        cx, cz = ri(e + 0xB8), ri(e + 0xC0)
        for o in PTS:
            x, z = rot(ri(e + o) - cx, ri(e + o + 8) - cz, q)
            out += [(e + o, 4, (cx + x) & 0xFFFFFFFF), (e + o + 8, 4, (cz + z) & 0xFFFFFFFF)]
        a = (ri(e + 0x124) + 1024 * (q % 4)) & 0xFFF
        out += [(e + 0x124, 4, a), (e + 0x128, 4, (rh(TRIG + 4 * a + 2) << 4) & 0xFFFFFFFF),
                (e + 0x12C, 4, (rh(TRIG + 4 * a) << 4) & 0xFFFFFFFF)]
        return out
    return f


def launch_pair(k, dlat, dalong, v, hy=0.3, dy=0.3, ox=1.2):
    """quick: impact's box-low (pool-6 slot 0 rewritten as a 2 x 2 x hy class-0 box, centre dy up - NOT a placement
    for the record; the player put ox off its centre at his first pass - he rides over it and 0x80084BE8 LAUNCHES him,
    flagsC 0xC00 at 0x80084D74) and bike k's box put dlat along the player's +0x1B0 row / dalong along his +0x210
    row from that first-pass spot (placement), its speed v (NOT a placement): it also rides over the box"""
    def f(ram):
        ed = I.box_q(ox, 0.0, hy=hy, dy=dy)(ram)
        vx, vz = I._rd(ram, P.VOL0 + 0x0C), I._rd(ram, P.VOL0 + 0x14)
        lat, fwd = I.row(ram, PLAYER, 0x1B0), I.row(ram, PLAYER, 0x210)
        tx = vx + FX(ox) + int(round((dlat * lat[0] + dalong * fwd[0]) * 65536))
        tz = vz + int(round((dlat * lat[2] + dalong * fwd[2]) * 65536))
        return ed + I.conv(P.move_box(ram, BIKE(k), tx, tz)) + I.spd(BIKE(k), v)
    return f


bbai = I.pairs_run("bb-ai")
bbside = I.pairs_run("bb-side")
N = "0xFFFF"

RUNS = [
    # --- the pair runs of impact.py / pairs.py (placements; bb-f100 FORCES flagsC bit 8, bb-rear90 sets a speed)
    _by["bb-side"], _by["bb-f100"], _by["bb-rear90"],
    ("p-bb-rear", QUICK, I.pairs_run("bb-rear"), 20, N, False, "pairs bb-rear: the player 1.6 behind bike 15"),
    ("p-bb-ai", QUICK, bbai, 20, N, False, "pairs bb-ai: bike 14 0.5/0.5 off bike 15"),
    ("p-bb-close", QUICK, I.pairs_run("bb-close"), 20, N, False, "pairs bb-close: the player 0.35 beside 15"),
    ("p-far", QUICK, I.pairs_run("far"), 20, N, False, "pairs far: bike 13 0.4 beside bike 12"),
    # --- rear-ends between two AI bikes (placement + a speed, NOT a placement)
    ("rear14-70", QUICK, pb(14, 0.1, -2.6, v14=70.0), 14, N, False, "14 2.6 behind 15; 14's speed 70 (NOT a placement)"),
    ("rear14-80", QUICK, pb(14, 0.0, -3.0, v14=80.0), 14, N, False, "14 3.0 behind 15; 14's speed 80 (NOT a placement)"),
    # --- a crashed bike (flagsC 0x200 FORCED)
    ("c15-ai", QUICK, cat(bbai, fc(B15, 0x200)), 14, N, False, "bb-ai; bike 15 flagsC 0x200 FORCED: knock-on crash of 14"),
    ("cboth-ai", QUICK, cat(bbai, fc(B14, 0x200), fc(B15, 0x200)), 14, N, False, "bb-ai; 14 and 15 flagsC 0x200 FORCED"),
    ("cboth-side", QUICK, cat(bbside, fc(PLAYER, 0x200), fc(B15, 0x200)), 14, N, False,
     "bb-side; the player and bike 15 flagsC 0x200 FORCED"),
    ("c15-pl", QUICK, cat(bbside, fc(B15, 0x200)), 14, N, False, "bb-side; bike 15 flagsC 0x200 FORCED: the player immune"),
    # --- airborne: both bikes ride over a low box (the game's own launch); no flag forced
    ("air-up", QUICK, launch_pair(15, 0.4, -7.5, 62.0), 14, N, False,
     "box-low launch of the player; bike 15 0.4 beside / 7.5 behind the launch spot at 62 (NOT a placement)"),
    ("air-knock", QUICK, launch_pair(15, 0.4, -9.0, 62.0, hy=0.5, dy=0.5), 14, N, False,
     "a 2x2x0.5 box launch; bike 15 0.4 beside / 9.0 behind at 62 (NOT a placement): the player falls below it"),
    ("air-knock0", QUICK, launch_pair(15, 0.4, -9.0, 62.0), 14, N, False,
     "box-low launch; bike 15 0.4 beside / 9.0 behind at 62 (NOT a placement)"),
    # --- airborne partner of a crashed bike (the launch + flagsC 0x200 FORCED on one bike)
    ("air-c15", QUICK, cat(launch_pair(15, 0.4, -9.0, 60.0), fc(B15, 0x200)), 14, N, False,
     "box-low launch; bike 15 9.0 behind at 60 (NOT a placement), flagsC 0x200 FORCED on 15"),
    ("air-c15b", QUICK, cat(launch_pair(15, 0.4, -9.0, 62.0), fc(B15, 0x200)), 14, N, False,
     "box-low launch; bike 15 9.0 behind at 62 (NOT a placement), flagsC 0x200 FORCED on 15"),
    ("air-throw", QUICK, cat(launch_pair(15, 0.4, -4.0, 28.0), fc(B15, 0x200), lambda r: I.spd(PLAYER, 22.0)), 14, N, False,
     "box-low launch; the player's speed 22, bike 15 4.0 behind at 28 (NOT placements), flagsC 0x200 FORCED on 15"),
    ("air-cP", QUICK, cat(launch_pair(15, 0.4, -7.5, 62.0), fc(PLAYER, 0x200)), 14, N, False,
     "box-low launch; bike 15 7.5 behind at 62 (NOT a placement), flagsC 0x200 FORCED on the player"),
    # --- flag arms (FORCED)
    ("slow-side", QUICK, cat(bbside, lambda r: I.spd(PLAYER, 0.5) + I.spd(B15, 0.5)), 14, N, False,
     "bb-side; the player's and bike 15's speeds 0.5 (NOT placements)"),
    ("slow-side2", QUICK, cat(lambda r: I.conv(P.move_box(r, PLAYER, I._rd(r, B15 + 0xB8) - FX(0.6), I._rd(r, B15 + 0xC0))),
                              lambda r: I.spd(PLAYER, 0.5) + I.spd(B15, 0.5)), 14, N, False,
     "the player's box 0.6 on the other side of bike 15 (placement); both speeds 0.5 (NOT placements)"),
    ("f80-side", QUICK, cat(bbside, fc(PLAYER, 0x80), fc(B15, 0x80)), 14, N, False, "bb-side; flagsC 0x80 FORCED on both"),
    ("f1-side", QUICK, cat(bbside, fc(PLAYER, 0x1)), 14, N, False, "bb-side; the player's flagsC class bit 0 FORCED"),
    ("b29-rear", QUICK, cat(I.bikes(0.0, 2.0, pl_v=90.0), fa(PLAYER, 0x20000000), fa(B15, 0x20000000)), 20, N, False,
     "bb-rear90 (speed 90, NOT a placement); flagsA bit 29 FORCED on both"),
    # --- head-on and T contacts: bike 14 TURNED (NOT a placement, see turned())
    ("head", QUICK, turned(B14, 2, pb(14, 0.0, 3.0)), 14, N, False, "14 3.0 ahead of 15, TURNED 180 (NOT a placement)"),
    ("head-80-51", QUICK, turned(B14, 2, pb(14, 0.0, 4.0, 80.0, 51.0)), 14, N, False,
     "14 4.0 ahead of 15, TURNED 180; speeds 80 / 51 (NOT placements)"),
    ("head-30-60", QUICK, turned(B14, 2, pb(14, 0.0, 4.0, 30.0, 60.0)), 14, N, False,
     "14 4.0 ahead of 15, TURNED 180; speeds 30 / 60 (NOT placements)"),
    ("head-60-30", QUICK, turned(B14, 2, pb(14, 0.0, 3.0, 60.0, 30.0)), 14, N, False,
     "14 3.0 ahead of 15, TURNED 180; speeds 60 / 30 (NOT placements)"),
    ("head-b29", QUICK, cat(turned(B14, 2, pb(14, 0.0, 4.0)), fa(B14, 0x20000000), fa(B15, 0x20000000)), 14, N, False,
     "14 4.0 ahead of 15, TURNED 180; flagsA bit 29 FORCED on both"),
    ("tb1", QUICK, turned(B14, 1, pb(14, 0.0, 2.0)), 14, N, False, "14 2.0 ahead of 15, TURNED 90 (NOT a placement)"),
    ("tb3", QUICK, turned(B14, 3, pb(14, 0.0, 2.0)), 14, N, False, "14 2.0 ahead of 15, TURNED 270 (NOT a placement)"),
    ("tb1-30-60", QUICK, turned(B14, 1, pb(14, 0.0, 2.0, 30.0, 60.0)), 14, N, False,
     "14 2.0 ahead of 15, TURNED 90; speeds 30 / 60 (NOT placements)"),
    ("tb3-30-60", QUICK, turned(B14, 3, pb(14, 0.0, 2.0, 30.0, 60.0)), 14, N, False,
     "14 2.0 ahead of 15, TURNED 270; speeds 30 / 60 (NOT placements)"),
    ("tb1-b29", QUICK, cat(turned(B14, 1, pb(14, 0.0, 2.0)), fa(B14, 0x20000000), fa(B15, 0x20000000)), 14, N, False,
     "14 2.0 ahead of 15, TURNED 90; flagsA bit 29 FORCED on both"),
    ("tb3-b29", QUICK, cat(turned(B14, 3, pb(14, 0.0, 2.0)), fa(B14, 0x20000000), fa(B15, 0x20000000)), 14, N, False,
     "14 2.0 ahead of 15, TURNED 270; flagsA bit 29 FORCED on both"),
    ("tb3-80-51", QUICK, turned(B14, 3, pb(14, 0.0, 2.0, 80.0, 51.0)), 14, N, False,
     "14 2.0 ahead of 15, TURNED 270; speeds 80 / 51 (NOT placements)"),
]
