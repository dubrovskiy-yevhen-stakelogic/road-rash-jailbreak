"""Reading pass C's runs: impact.py's form (name, src, build, frames, pad, explore, note).
Every edit that is not a placement (a box / road-state move) is named as such in the note."""
import impact as I
import pairs as P

_BY = {r[0]: r for r in I.RUNS}


def reuse(*names):
    return [_BY[n] for n in names]


def combo(*fs):
    return lambda ram: [x for f in fs for x in f(ram)]


def ai_pole(k, back=0.0, lat=0.3):
    """quick: AI bike k's box put so that its first step (its heading row +0x1C2 x speed x 0.05) ends `lat` beside
    the class-1 pole of pool-6 slot 0, `back` units further back along its heading - a placement"""
    def f(ram):
        e = I.BIKE(k)
        vx, vz = I._rd(ram, I.VOL0 + 0x0C), I._rd(ram, I.VOL0 + 0x14)
        h, s = I.row(ram, e, 0x1C2), I.row(ram, e, 0x1B0)
        step = I._rd(ram, e + 0x1E0) / 65536 * 0.05 + back
        tx = vx + int(round((s[0] * lat - h[0] * step) * 65536))
        tz = vz + int(round((s[2] * lat - h[2] * step) * 65536))
        return I.conv(P.move_box(ram, e, tx, tz))
    return f


def rider6_m2(stance, passenger=False):
    """quick: rider 6 (the downed rider of bike 6, left where it lies) FORCED to stance `stance` and mount +0x25C
    = 2 (leaving the bike) - NOT placements; with `passenger`: its +0x23C bit 5 FORCED and bike 6's +0x358
    PLANTED := bike 5"""
    def f(ram):
        ed = [(I.RIDER6 + 0x220, 2, stance), (I.RIDER6 + 0x25C, 4, 2)]
        if passenger:
            ed += I.orw(ram, I.RIDER6 + 0x23C, 0x20, 1) + [(I.BIKE6 + 0x358, 4, I.BIKE(5))]
        return ed
    return f


def ai_crashed(ks, t2f4=1.0, speed=None):
    """quick: AI bikes ks FORCED into the crashed state (flagsC |= 0x200) with +0x2F4 = t2f4 - NOT placements;
    optionally their +0x1E0 / +0x240 = speed (NOT a placement)"""
    def f(ram):
        ed = []
        for k in ks:
            ed += I.orw(ram, I.BIKE(k) + 0x238, 0x200) + [(I.BIKE(k) + 0x2F4, 4, I.FX(t2f4))]
            if speed is not None:
                ed += I.spd(I.BIKE(k), speed)
        return ed
    return f


RUNS = []

# --- UNFORCED: the crash chain of impact.py (the pose initialisers through RiderLaunch and 0x8008F138)
RUNS += reuse("crash", "crash-90", "crash-4", "pole", "pole-fast")
RUNS += [("cc6-40", I.RR, I.car(-6.0, 0.0, pspd=40.0, cspd=0.0), 30, "0xBFFF", False,
          "the car 6 ahead, its speed 0 and the player's 40 (speeds NOT placements); Cross: a class-4 hit"),
         ("cl6-40", I.RR, I.car(-6.0, 0.0, pspd=40.0, cspd=0.0, extra=lambda r: [(I.PLAYER + 0x268, 4, I.FX(0.3))]),
          30, "0xBFFF", False, "cc6-40 with the player's +0x268 = 0.3 (NOT a placement): HitSpeed's crash arm"),
         ("cc4-60", I.RR, I.car(-4.0, 0.0, pspd=60.0, cspd=0.0), 30, "0xBFFF", False,
          "the car 4 ahead, speed 0, player 60 (speeds NOT placements): a class-8 crash that runs 12 frames")]

# --- UNFORCED Bounce / Spin: AI bike 15 meets the pole (class 8 -> knock-off -> flagsC 0x200 from
#     BikeCrashLaunch), AI bike 13 / 14 placed behind it runs into the crashed bike: the bike-bike gate 0x80081D7C
RUNS += [(f"pair{k}-{b}-{l}", I.QUICK, combo(ai_pole(15), ai_pole(k, b, l)), 20, "0xFFFF", False,
          f"AI bike 15 placed to meet the pole; AI bike {k} placed {b} further back, {l} beside (placements only)")
         for k, b, l in ((13, 4.5, 0.0), (13, 5.0, -0.3), (13, 6.0, -0.3), (13, 6.0, 0.0), (14, 5.0, 0.0),
                         (14, 4.0, 0.0))]
RUNS += [(f"pm{k}-{b}-{l}", I.QUICK, combo(ai_pole(15, 0.0, -0.3), ai_pole(k, b, l)), 20, "0xFFFF", False,
          f"AI bike 15 placed to meet the pole on its other side; AI bike {k} placed {b} further back, {l} beside "
          "(placements only)")
         for k, b, l in ((13, 4.0, 0.0), (14, 4.0, 0.0), (13, 4.5, -0.6), (14, 6.0, -0.6))]
# --- UNFORCED Bounce through PropTopple (rr-pack's live props; prop-far is a placement only)
RUNS += reuse("prop-far", "prop-kick", "prop-side", "prop-fwd")

# --- FORCED
RUNS += reuse("ride-over", "ride-over-p", "ko-forced", "ko-forced16",
              "launch38", "launch39", "launch40", "launch41", "launch42", "launch90")
RUNS += [(f"m2-{st}", I.QUICK, rider6_m2(st), 10, "0xFFFF", False,
          f"rider 6's stance FORCED to {st} and its mount +0x25C FORCED to 2: 0x8008F138 dispatches its pose")
         for st in (38, 39, 40, 41, 42, 43, 20)]
RUNS += [(f"m2p-{st}", I.QUICK, rider6_m2(st, True), 10, "0xFFFF", False,
          f"m2-{st} + rider 6's passenger bit FORCED, bike 6 +0x358 PLANTED := bike 5")
         for st in (38, 89, 88, 90, 91)]
RUNS += [("wipe-f", I.QUICK, ai_crashed(range(1, 16)), 10, "0xFFFF", False,
          "AI bikes 1..15 FORCED crashed (flagsC |= 0x200, +0x2F4 = 1.0): the wipeout arm 0x800B1978"),
         ("hit-200", I.RR, I.car(extra=lambda r: I.orw(r, I.PLAYER + 0x238, 0x200)), 20, "0xFFFF", False,
          "pairs tr-over0 with the player's flagsC |= 0x200 FORCED: HitOutcome -> Bounce"),
         ("pole-200", I.QUICK, lambda r: I.pairs_run("pole")(r) + I.orw(r, I.PLAYER + 0x238, 0x200), 20, "0xFFFF",
          False, "pairs pole with the player's flagsC |= 0x200 FORCED: ImpactTurn -> Bounce, Spin")]
