"""Scout probe: the SPU control registers 0x1F801D80..0x1F801D9F out of the player's own savestates.

The bench's SPU control register file starts from these values.

The SPU register block of a savestate (located, not parsed, by savestate.py) is `SPU::DoState` of
the emulator that wrote it. Its first 100 bytes, in write order, are:

    +0   ticks_carry s32        +4  SPUCNT u16        +6  SPUSTAT u16      +8  transfer ctl u16
    +10  transfer addr u32      +14 transfer reg u16  +16 IRQ addr u16     +18 capture pos u16
    +20  main vol L reg u16     +22 main vol R reg u16
    +24  main vol L sweep (12 B, a POD)               +36 main vol R sweep (12 B)
    +48  CD vol L/R s16 x2      +52 ext vol L/R s16 x2
    +56  KON u32   +60 KOFF u32   +64 ENDX u32   +68 PMON u32   +72 NON u32
    +76  noise count u32        +80 noise level u32   +84 EON u32
    +88  reverb base u32        +92 reverb current u32   +96 vLOUT s16   +98 vROUT s16

That layout was READ out of the serialisation code (the same GPL tree savestate.py names; nothing
is copied from it) and is then CHECKED here against facts that do not come from that code at all:

  1. PMON must hold exactly one bit, and it must be the channel of engine layer L3 - the handle at
     `*(gp+1952) + 0x1C` in the same state's own ram.bin (EngineStart turns PMON on
     for L3's channel and nothing else in the game ever does);
  2. EON must be clear exactly on the two channels that carry the `.ALB` music (voices whose
     descriptor is S+0x20 / S+0x34) and set on every other channel below 24;
  3. SPUCNT must have bit 15 (SPU enable) set, and the main volume must be the fixed-volume form
     (bit 15 clear) - a mis-aligned read lands on neither.

Writes `work\\oracle\\state\\<name>\\spuctl.bin`: 16 little-endian halfwords, the values a read of
0x1F801D80 + 2*i returned on the console at the instant of the capture (reverb output volume,
KON/KOFF, PMON, NON, EON, ENDX). Game-derived, so it lives under work\\ only.

    python tools\\scout\\spu_regs.py verify     # checks only
    python tools\\scout\\spu_regs.py write      # checks, then writes spuctl.bin for every state
    python tools\\scout\\spu_regs.py verify --mutate   # shifts the layout by 4 bytes: must FAIL
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import savestate as SS  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SAV_DIR = os.path.join(ROOT, "work", "oracle", "vr_capture")
STATE_DIR = os.path.join(ROOT, "work", "oracle", "state")
STATES = ("rr-race", "rr-pack", "rr-grid", "quick")

GP = 0x8005AC8C
G_ENGINE = GP + 1952
SOUND_SYS = 0x800D6870

# offset of each 0x1F801D80 + 2*i halfword inside the SPU register block
LAYOUT = {
    0: (20, 2), 1: (22, 2),          # main volume L/R (the register form)
    2: (96, 2), 3: (98, 2),          # reverb output volume vLOUT / vROUT
    4: (56, 0), 5: (56, 1),          # KON lo/hi
    6: (60, 0), 7: (60, 1),          # KOFF
    8: (68, 0), 9: (68, 1),          # PMON
    10: (72, 0), 11: (72, 1),        # NON
    12: (84, 0), 13: (84, 1),        # EON
    14: (64, 0), 15: (64, 1),        # ENDX
}


def regs_of(blob, shift=0):
    out = []
    for i in range(16):
        off, half = LAYOUT[i]
        off += shift
        if half == 2:
            v = struct.unpack_from("<H", blob, off)[0]
        else:
            v = (struct.unpack_from("<I", blob, off)[0] >> (16 * half)) & 0xFFFF
        out.append(v)
    return out


def w(ram, a):
    return struct.unpack_from("<I", ram, a & 0x1FFFFF)[0]


def check_state(name, mutate):
    sav = os.path.join(SAV_DIR, name + ".sav")
    s = SS.SaveState(sav)
    start = s._after("SPU")
    blob = s.d[start:start + s.spu_reg_bytes]
    regs = regs_of(blob, 4 if mutate else 0)
    ram = s.ram()
    fails = []
    spucnt = struct.unpack_from("<H", blob, 4 + (4 if mutate else 0))[0]
    if not spucnt & 0x8000:
        fails.append("SPUCNT 0x%04X has no enable bit" % spucnt)
    for i in (0, 1):
        if regs[i] & 0x8000:
            fails.append("main volume %d = 0x%04X is a sweep, not a fixed volume" % (i, regs[i]))
    pmon = regs[8] | (regs[9] << 16)
    eon = regs[12] | (regs[13] << 16)
    eng = w(ram, G_ENGINE)
    l3 = w(ram, eng + 0x1C)
    ch3 = l3 >> 27
    if pmon != (1 << ch3):
        fails.append("PMON 0x%08X is not exactly L3's channel %d (handle 0x%08X)" % (pmon, ch3, l3))
    voices = w(ram, SOUND_SYS + 0x0C)
    music = set()
    for v in range(24):
        d = w(ram, voices + 44 * v + 8)
        if d in (SOUND_SYS + 0x20, SOUND_SYS + 0x34):
            music.add(w(ram, voices + 44 * v + 0x1C))
    want = 0
    for ch in range(24):
        if ch not in music:
            want |= 1 << ch
    if len(music) != 2:
        fails.append("expected two music voices, found channels %s" % sorted(music))
    if eon != want:
        fails.append("EON 0x%08X, expected 0x%08X (all but music channels %s)" % (eon, want, sorted(music)))
    return regs, fails, (pmon, eon, ch3, sorted(music), s.spu_reg_bytes)


def main():
    args = sys.argv[1:]
    cmd = args[0] if args else "verify"
    mutate = "--mutate" in args
    total = 0
    for name in STATES:
        regs, fails, (pmon, eon, ch3, music, nbytes) = check_state(name, mutate)
        total += len(fails)
        print("%-8s block %d B  PMON 0x%08X (L3 on ch %d)  EON 0x%08X (music on %s)  %s"
              % (name, nbytes, pmon, ch3, eon, music, "ok" if not fails else "FAIL"))
        print("         " + " ".join("%04X" % r for r in regs))
        for f in fails:
            print("         FAIL: " + f)
        if cmd == "write" and not fails and not mutate:
            out = os.path.join(STATE_DIR, name)
            if os.path.isdir(out):
                with open(os.path.join(out, "spuctl.bin"), "wb") as f:
                    f.write(struct.pack("<16H", *regs))
                print("         wrote %s" % os.path.join(out, "spuctl.bin"))
    print("spu_regs: %d state(s), %d failure(s)" % (len(STATES), total))
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
