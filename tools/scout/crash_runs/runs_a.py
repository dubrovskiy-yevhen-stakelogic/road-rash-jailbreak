"""Runs for reading pass A (RASHCDG 0x80078DB4 ImpactStatePass).  impact.py's spec form:
(name, source savestate, edit builder or None, frames, pad, explore, note).  Every edit that is not a placement
says so in its note.  The player's bike is on 0x8005B298 in rr-race / quick; the crashed bike 6 is on 0x8005B350
in both (flagsC 0x00500100, rider mount 4, +0x28C = pi/2, +0x2D8 = 5878).  A forced flagsC bit 27 (0x08000000)
makes pass A's list mover 0x80071BCC re-list the bike the same frame."""
import struct
import impact as I

PL, B6 = I.PLAYER, I.BIKE6
FX, u32 = I.FX, I.u32
_R = {r[0]: r for r in I.RUNS}
REUSE = ["crash", "crash-90", "crash-4", "bump", "hit", "hit-side", "pole", "pole-fast", "land", "land-fast",
         "bb-side", "bb-rear90", "ko-forced", "car-f100", "bb-f100"]


def rd(ram, a, f="<I"):
    return struct.unpack_from(f, ram, a & 0x1FFFFF)[0]


def fc(e, bits, extra=()):
    """NOT a placement: flagsC (+0x238) of bike e OR-ed with bits (FORCED), plus the named word stores in extra"""
    def f(ram):
        return I.orw(ram, e + 0x238, bits) + [(e + o, 4, u32(v)) for o, v in extra]
    return f


def rider_of(ram, e):
    return rd(ram, e + 0x354)


RUNS = [_R[n] for n in REUSE] + [
    ("rr-plain", I.RR, None, 24, "0xFFFF", False, "rr-race unmodified, no buttons"),
    ("quick-plain", I.QUICK, None, 14, "0xFFFF", False, "quick unmodified, no buttons"),
    # ---- the player, mounted, forced into each class (re-listed by pass A through bit 27)
    ("a-c2", I.RR, fc(PL, 0x08000002), 12, "0xFFFF", False,
     "NOT a placement: the player's flagsC |= 0x08000002 (class 2 FORCED, re-list)"),
    ("a-c2-q", I.QUICK, fc(PL, 0x08000002), 10, "0xBFFF", False,
     "NOT a placement: quick, the player's flagsC |= 0x08000002 (class 2 FORCED); Cross"),
    ("a-c4", I.RR, fc(PL, 0x08000004), 12, "0xFFFF", False,
     "NOT a placement: the player's flagsC |= 0x08000004 (class 4 FORCED, re-list)"),
    ("a-c4-pitch", I.RR, fc(PL, 0x08000004, [(0x268, -FX(0.2))]), 12, "0xFFFF", False,
     "NOT a placement: class 4 FORCED and the player's +0x268 = -0.2 (FORCED)"),
    ("a-c404-run", I.RR, fc(PL, 0x08000404), 12, "0xFFFF", False,
     "NOT a placement: flagsC |= 0x08000404 (FORCED), the ramp words as they are"),
    ("a-c8", I.RR, fc(PL, 0x08000008), 12, "0xFFFF", False,
     "NOT a placement: the player's flagsC |= 0x08000008 (class 8 FORCED at 0 pitch, re-list)"),
    ("a-c1", I.RR, fc(PL, 0x08000001), 12, "0xFFFF", False,
     "NOT a placement: the player's flagsC |= 0x08000001 (class 1 FORCED)"),
    ("a-b4", I.RR, fc(PL, 0x08000010), 12, "0xFFFF", False,
     "NOT a placement: the player's flagsC |= 0x08000010 (bit 4 FORCED, re-list)"),
    # ---- the crashed bike 6 (rider down, mount 4), already on 0x8005B350
    ("b6-b4", I.RR, fc(B6, 0x10), 8, "0xFFFF", False, "NOT a placement: bike 6 flagsC |= 0x10 (FORCED)"),
    ("b6-c2", I.RR, fc(B6, 0x2), 8, "0xFFFF", False,
     "NOT a placement: bike 6 flagsC |= 2 (FORCED): lying at +0x28C = pi/2, its ramp +0x2D8 = 5878 unexpired"),
    ("b6-c2-end", I.RR, fc(B6, 0x2, [(0x2D8, 0)]), 8, "0xFFFF", False,
     "NOT a placement: bike 6 flagsC |= 2, +0x2D8 = 0 (FORCED): the impact ends with the rider down"),
    ("b6-c2-pitch", I.RR, fc(B6, 0x2, [(0x2D8, 0), (0x268, -FX(0.3))]), 8, "0xFFFF", False,
     "NOT a placement: as b6-c2-end with +0x268 = -0.3 (FORCED)"),
    ("b6-ramp", I.RR, fc(B6, 0x2, [(0x28C, FX(0.5))]), 8, "0xFFFF", False,
     "NOT a placement: bike 6 flagsC |= 2, its roll +0x28C = 0.5 (FORCED): the fall-over ramp"),
    ("b6-ramp-neg", I.RR, fc(B6, 0x2, [(0x28C, -FX(0.5)), (0x26C, FX(0.01)), (0x270, FX(1.5))]), 8, "0xFFFF", False,
     "NOT a placement: bike 6 flagsC |= 2, +0x28C = -0.5, +0x26C = 0.01, +0x270 = 1.5 (FORCED): the ramp's rate"),
    ("b6-ramp-neg2", I.RR, fc(B6, 0x2, [(0x28C, -FX(0.5)), (0x26C, FX(0.01)), (0x270, -FX(0.25))]), 8, "0xFFFF",
     False, "NOT a placement: as b6-ramp-neg with +0x270 = -0.25 (FORCED)"),
    ("b6-ramp-4000", I.RR, fc(B6, 0x4002, [(0x28C, FX(0.5))]), 8, "0xFFFF", False,
     "NOT a placement: bike 6 flagsC |= 0x4002, +0x28C = 0.5 (FORCED): the ramp already started"),
    ("b6-c1-end", I.RR, fc(B6, 0x1, [(0x2D8, 0)]), 8, "0xFFFF", False,
     "NOT a placement: bike 6 flagsC |= 1, +0x2D8 = 0 (FORCED): a bump that ends with the rider down"),
    ("b6-c4", I.RR, fc(B6, 0x4, [(0x2D8, 0)]), 8, "0xFFFF", False,
     "NOT a placement: bike 6 flagsC |= 4, +0x2D8 = 0 (FORCED): class 4 with the rider down"),
    ("b6-c8", I.RR, fc(B6, 0x8, [(0x2D8, 0), (0x2D4, 100)]), 8, "0xFFFF", False,
     "NOT a placement: bike 6 flagsC |= 8, +0x2D8 = 0, +0x2D4 = 100 (FORCED): class 8 with the rider down"),
    ("b6-dir3", I.RR, fc(B6, 0, [(0x16C, 3)]), 6, "0xFFFF", False,
     "NOT a placement: bike 6 +0x16C = 3 (FORCED): the bit-22 toggle"),
    ("b6-c204-run", I.RR, fc(B6, 0x204), 8, "0xFFFF", False,
     "NOT a placement: bike 6 flagsC |= 0x204 (FORCED), its ramp (+0x2D8 = 5878) unexpired"),
    ("b6-c201", I.RR, fc(B6, 0x2201), 8, "0xFFFF", False,
     "NOT a placement: bike 6 flagsC |= 0x2201 (FORCED): class 1 on a 0x200 bike, heading already taken"),
    ("b6-c201-dot", I.RR, fc(B6, 0x201), 8, "0xFFFF", False,
     "NOT a placement: bike 6 flagsC |= 0x201 (FORCED): class 1 on a 0x200 bike"),
    ("b6-c202-end", I.RR, fc(B6, 0x202, [(0x2D8, 0)]), 8, "0xFFFF", False,
     "NOT a placement: bike 6 flagsC |= 0x202, +0x2D8 = 0 (FORCED): the end on a 0x200 bike"),
    ("b6-c208-t", I.RR, fc(B6, 0x208, [(0x2D8, -1)]), 8, "0xFFFF", False,
     "NOT a placement: bike 6 flagsC |= 0x208, +0x2D8 = -1 (FORCED; the rider pass zeroes +0x2D4 first)"),
    ("b6-c8-t", I.RR, fc(B6, 0x8, [(0x2D8, -1)]), 8, "0xFFFF", False,
     "NOT a placement: bike 6 flagsC |= 8, +0x2D8 = -1 (FORCED): class 8, rider down, ramp expired"),
    ("b6-c208-m1", I.RR, lambda r: fc(B6, 0x208, [(0x2D8, -1)])(r) + [(rider_of(r, B6) + 0x25C, 4, 1)], 8, "0xFFFF",
     False, "NOT a placement: b6-c208-t with rider 6's mount +0x25C = 1 (FORCED)"),
    ("q15-c2", I.QUICK, fc(I.BIKE(15), 0x08000002), 10, "0xFFFF", False,
     "NOT a placement: quick, AI bike 15 flagsC |= 0x08000002 (class 2 FORCED, re-list)"),
    ("q15-c1", I.QUICK, fc(I.BIKE(15), 0x08000001, [(0x2D8, -1)]), 10, "0xFFFF", False,
     "NOT a placement: quick, AI bike 15 flagsC |= 0x08000001, +0x2D8 = -1 (FORCED)"),
    ("b6-c204-m1", I.RR, lambda r: fc(B6, 0x204, [(0x2D8, -1)])(r) + [(rider_of(r, B6) + 0x25C, 4, 1)], 8, "0xFFFF",
     False, "NOT a placement: bike 6 flagsC |= 0x204, +0x2D8 = -1, rider 6's mount +0x25C = 1 (FORCED)"),
    ("b6-c201-vert", I.RR, lambda r: fc(B6, 0x2201)(r) + [(B6 + 0x1BC, 2, 0), (B6 + 0x1BE, 2, 4096),
                                                          (B6 + 0x1C0, 2, 0), (B6 + 0x1C4, 2, 4096)],
     8, "0xFFFF", False, "NOT a placement: b6-c201 with bike 6's matrix row 2 (+0x1BC) = (0, 4096, 0) and heading y "
     "+0x1C4 = 4096 (FORCED)"),
    ("b6-c8-t-up", I.RR, lambda r: fc(B6, 0x8, [(0x2D8, -1)])(r) + [(B6 + 0x328, 2, 0), (B6 + 0x32A, 2, 4096),
                                                                     (B6 + 0x32C, 2, 0)],
     8, "0xFFFF", False, "NOT a placement: b6-c8-t with bike 6's +0x328 heading = (0, 4096, 0) (FORCED)"),
    # ---- a natural class-4 hit: the car ahead of the player at 45..50 (the geometry HitOutcome calls 4, not 8)
    ("hard-4-45", I.RR, I.car(-4.0, 0.0, pspd=45.0), 20, "0xBFFF", False,
     "the car 4 ahead, the player's speed 45 (NOT a placement); Cross"),
    ("hard-6-45", I.RR, I.car(-6.0, 0.0, pspd=45.0), 20, "0xBFFF", False,
     "the car 6 ahead, the player's speed 45 (NOT a placement); Cross"),
    ("hard-6-50", I.RR, I.car(-6.0, 0.0, pspd=50.0), 20, "0xBFFF", False,
     "the car 6 ahead, the player's speed 50 (NOT a placement); Cross"),
]
