"""Scout probe for the FRONT END AT RUNTIME - the shell's own frame, input, dispatch,
career state, handover and presentation (Road Rash: Jailbreak, USA, SLUS_01053).

The sibling probe `tools\\scout\\frontend.py` verifies the shell's DATA (screens, widgets,
strings, save); this one verifies the CODE that drives it, by decoding instruction words out
of the player's own images.

Reads ONLY from work\\disc_us (the player's own disc extract, gitignored) and, for
`state`, from work\\oracle\\state\\*\\ram.bin.  Writes ONLY to stdout.  Never copies
game bytes or game text into the repository.

Usage (from the project root):

    python tools\\scout\\shell.py info                  # the decoded runtime skeleton
    python tools\\scout\\shell.py frame                 # the shell's per-frame call order
    python tools\\scout\\shell.py input                 # the pad -> menu control map
    python tools\\scout\\shell.py flags                 # widget condition-word gates
    python tools\\scout\\shell.py state                 # which overlay each capture holds
    python tools\\scout\\shell.py verify [--mutate]     # the assertion bench (the gate)

`verify` prints `shell: N checks, M failures` as its last line.  `--mutate` perturbs
eight load-bearing claims so the probe can be seen failing.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import exe as E  # noqa: E402  (sibling scout module; read-only use)

ROOT = E.ROOT
OVL_BASE = 0x8005B5E8

SHA1 = {
    "SLUS": "67ed165a2c517d4e6106fb0dfa324d66dd9a76f1",
    "F": "a3fec4b4e9292c358d0f6dc529843f5d8f25924a",
    "G": "cfe43a7786759f2cb9c57751cf99e84d1074782c",
    "I": "9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06",
}

# ---------------------------------------------------------------------------
# Addresses, each derived from the instruction that establishes it.
# Prefix F = RASHCDF, S = SLUS_010.53.
# ---------------------------------------------------------------------------

S_MAIN = 0x80012224             # main()
S_MAIN_COLD = 0x80012288        # jal FrontendBoot
S_MAIN_RESUME = 0x800122DC      # jal FrontendResume
S_GAME_FRAME = 0x80011C4C       # GameFrame - the RACE frame, not the shell's
S_POLL_PADS = 0x8001CB3C        # PollPads
S_FRAME_CLOCK = 0x8001C428      # the frame delta into game_state+0x20
S_PAD_SNAPSHOT = 0x800D7128     # the shell's pad view (a copy of 0x800D6DE0)
S_PAD_LIVE = 0x800D6DE0         # the live per-player input records
S_PAD_RAW = 0x800D70E0          # the SIO0 receive buffer: 00 41 <lo> <hi>
S_MEMCPY = 0x8001E0B4

F_BOOT = 0x8007FEDC             # cold entry
F_RESUME = 0x8007FF4C           # entry after a race
F_FRAME = 0x80080274            # THE SHELL'S FRAME LOOP
F_FRAME_END = 0x800803FC        # swap the ordering tables
F_FRAME_FLUSH = 0x80080488      # DrawSync + free last frame's screen builds
F_PREPARE = 0x80080544          # load the persistent shell resources
F_INIT = 0x800665E8             # reset the fe context
F_INPUT = 0x800667E4            # the per-frame input pass
F_TICK = 0x80066C34             # the per-frame screen state machine
F_COMMIT_SCREEN = 0x8006711C    # fe->f88 = fe->f84, then ChangeScreen
F_CHANGE_SCREEN = 0x80066EF8
F_GOTO_SCREEN = 0x800809E0
F_BUILD_NAV = 0x80068E88        # fills the nav tables AND the two handler tables
F_SCREEN_HANDLERS = 0x8006D174  # fills the transition/tick and widget tables
F_MENU_INPUT = 0x8006B03C       # THE generic menu input handler
F_MENU_INPUT_DEFAULT = 0x80069418   # the default per-screen arm; calls F_MENU_INPUT
F_OBJECT_INPUT = 0x8006B7BC     # the default per-object arm
F_OBJECT_INPUT_BODY = 0x8006B800
F_OBJECT_INPUT_31 = 0x8006BE84  # the one override of the object table
F_MOVE_NEXT = 0x8006C3A4
F_MOVE_PREV = 0x8006C558
F_FIND_BY_CODE = 0x8006C700     # (screen, actionCode) -> item index or -1
F_WIDGET_PASS = 0x8006D3E0      # update-or-draw every widget of one screen
F_TRANSITION_DEFAULT = 0x8006D5B0
F_TICK_DEFAULT = 0x8006D630
F_SCREEN_BUILD = 0x80078E80     # load a screen's resource list
F_RES_RELEASE_ALL = 0x80078EF4
F_BIND_SLIDER = 0x80064B30
F_BIND_CHOOSER = 0x800680E8
F_CHOOSER_PREV = 0x8006460C
F_CHOOSER_NEXT = 0x800647E4
F_START_RACE_INPUT = 0x8006C354  # screen 57's input handler: game_state->f00 = 3
F_START_RACE_TICK = 0x8006E008   # screen 57's tick: the per-race loading picture
F_COMMIT = 0x8007F37C            # session -> game_state (rules.md 2.3)
F_COMMIT_SHORT = 0x8007FD20      # the attract-path commit
F_NEW_GAME = 0x80068688
F_UI_SOUND = 0x8007EAC0
F_MUSIC_PLAY = 0x8007EDE0
F_MUSIC_STOP = 0x8007F158
F_TEXTBLOCK = 0x8006FAC8         # widget type 16
F_CARD_UI = 0x8006C770           # the card screens' input -> event map
F_CARD_TICK = 0x8005F21C
F_SAVE_SLOT = 0x8006CDA4
F_LOAD_SLOT = 0x8006CE60
F_BEATS_RECORD = 0x8007E7BC

T_PRE_INPUT = 0x8005B9D4        # 59 jump-table arms, consumed by `jr v0` in F_INPUT
T_SCREEN_INPUT = 0x8009C8C0     # [screenId] -> input handler
T_OBJECT_INPUT = 0x8009CC40     # [object kind] -> input handler, 37 entries
T_SLIDER_COMMIT = 0x8009C8A8    # [slider kind] -> action code to jump to, 6 entries
T_SCREEN_TRANS = 0x8009D0C0
T_SCREEN_TICK = 0x8009CFD0
T_SCREEN_RES = 0x8009D3F0       # [screenId] -> s16[] resource list, -1 terminated
T_RESUME = 0x8005CB80           # 32 arms, index = mode - 1
T_TEXTBLOCK_KIND = 0x8005C13C   # 56 arms, index = widget+0x18 - 1
T_SLIDER_DESC = 0x8009C548      # 16 bytes per slider slot
T_SLIDER_VALUE = 0x800D81AC     # = session + 0xD4, one u32 per slider slot

FE = 0x8009C5D0                 # the frontend context
FE_SCREEN_PTR = 0x8009C5C8      # the cached current Screen*
FE_TABLE_PTR = 0x8009C68C       # holds 0x800A0880, the screen pointer table
GAME_STATE_PTR = 0x8005B2F8     # -> game_state
IDLE_COUNTER = 0x8005ACAC       # frames with no pad activity
ATTRACT_FLAG = 0x8005B220
SESSION = 0x800D80D8
PLAYERS = 0x800D81D8            # six 0x24-byte records
VIB_PORTS = 0x800D7428          # four 24-byte per-port records

SCREEN_COUNT = 59
SCREEN_START_RACE = 57
ATTRACT_TIMEOUT = 1801          # slti at 0x80066BF8
ATTRACT_TARGET = 58
MUSIC_TRACKS = 18               # sltiu at F_MUSIC_PLAY + 4
CAREER_SLOTS = 10
CAREER_REC_SIZE = 484
SLIDER_SLOTS = 6
TEXTBLOCK_KINDS = 56

# The twelve movie screens, taken from docs\formats\frontend.md 3.3.  The probe does NOT trust this
# list: it re-derives the same set twice, from two independent tables, and checks that
# all three agree.
MOVIE_SCREENS = (0, 1, 2, 6, 11, 12, 15, 16, 19, 20, 22, 58)

# The shell's frame, in call order, as decoded from F_FRAME.  (site, callee, what).
FRAME_CALLS = (
    (0x800802C8, 0x8001CB3C, "SLUS PollPads - snapshot the pad edges"),
    (0x800802D0, 0x8001C428, "SLUS frame clock -> game_state+0x20"),
    (0x80080314, 0x800667E4, "the input pass"),
    (0x8008031C, 0x80066C34, "the screen state machine (update, transition, build)"),
    (0x8008033C, 0x8001C3F4, "SLUS - vsync/present when the hold counter is done"),
    (0x80080344, 0x8001C408, "SLUS - ditto"),
    (0x80080368, 0x80062774, "the screen-transition effect (front layer)"),
    (0x80080378, 0x80048DB4, "DrawOTag  (the ordering table built this frame)"),
    (0x80080380, 0x8006711C, "commit fe->f02: the screen change"),
    (0x80080388, 0x800487C0, "DrawSync(0)"),
    (0x800803A0, 0x80062774, "the screen-transition effect (back layer)"),
    (0x800803A8, 0x80080488, "free last frame's screen builds"),
    (0x800803B4, 0x800803FC, "swap the two ordering tables"),
)

# The per-frame input pass, in call order, as decoded from F_INPUT.
INPUT_CALLS = (
    (0x80066948, 0x8006FED4, "fe->f0A & 4: rebuild the text cache"),
    (0x800669A4, 0x800680E8, "bind the selected chooser -> fe->f94"),
    (0x800669FC, 0x80064B30, "bind the selected slider  -> fe->f7C / fe->f80"),
    (0x80066BC4, 0x8006738C, "per-player post-pass over the six player records"),
    (0x80066BD8, 0x800685BC, "SetMode from the selected widget's action code"),
    (0x80066BE4, 0x8006310C, "refresh the bound object"),
)

# The menu input handler's button arms: pad-record offset, button index, what it does.
# The offset is `record + 0x14 + 8*slot + 6`, i.e. slot `index` of SLUS's own 19-slot
# array.
MENU_BUTTONS = (
    (0x1A, 0, "value down (chooser/slider)", 0x40),
    (0x22, 1, "value up   (chooser/slider)", 0x80),
    (0x2A, 2, "cursor previous", 0x10),
    (0x32, 3, "cursor next", 0x20),
    (0x3A, 4, "jump to the options screen (42)", 0x08),
    (0x42, 5, "open the message panel (53)", 0x02),
    (0x4A, 6, "cancel / back", 0x04),
    (0x52, 7, "confirm", 0x01),
)

# fe+0x13, the shell's own edge mask, in the order the `ori` instructions appear.
EDGE_ORI_SEQUENCE = (4, 4, 4, 0x10, 0x20, 0x40, 0x80, 1, 4, 4, 4, 8)

# The widget condition-word gates.  (bit, where it is read, what it requires)
COND_GATES = (
    (0x40000000, F_MOVE_NEXT, "a multitap: (*(u8*)0x800D70E1 >> 4) == 8"),
    (0x02000000, F_MOVE_NEXT, "0x800D7428[24*fe->f16].f04 == 1"),
    (0x10000000, F_MOVE_NEXT, "session+0x1B != 0"),
    (0x08000000, F_MOVE_NEXT, "mode == 4 (Time Trial)"),
    (0x04000000, F_MOVE_NEXT, "mode != 4"),
)

# ---------------------------------------------------------------------------
# Low-level helpers
# ---------------------------------------------------------------------------


def simm(w):
    return (w & 0xFFFF) - 0x10000 if (w & 0x8000) else (w & 0xFFFF)


_IMAGES = {}


def images():
    if not _IMAGES:
        _IMAGES["SLUS"] = E.load_exe()
        _IMAGES["F"] = E.load_overlay("RASHCDF.BIN", OVL_BASE)
        _IMAGES["G"] = E.load_overlay("RASHCDG.BIN", OVL_BASE)
        _IMAGES["I"] = E.load_overlay("RASHCDI.BIN", OVL_BASE)
    return _IMAGES


def word(im, addr):
    if not im.contains(addr) or not im.contains(addr + 3):
        return None
    return im.word(addr)


def jal_target(im, addr):
    w = word(im, addr)
    if w is None or (w >> 26) != 3:
        return None
    return ((addr + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)


def call_sites(im, target):
    """Every `jal target` in one image."""
    t = (target >> 2) & 0x3FFFFFF
    return [a for a, w in im.words() if (w >> 26) == 3 and (w & 0x3FFFFFF) == t]


def calls_in(im, lo, hi):
    """The distinct `jal` targets in [lo, hi), in first-seen order."""
    out = []
    for a in range(lo, hi, 4):
        t = jal_target(im, a)
        if t is not None and t not in out:
            out.append(t)
    return out


def lui_immediates(im, lo, hi):
    """Every `lui rX, imm` immediate in [lo, hi), as a 32-bit value."""
    out = set()
    for a in range(lo, hi, 4):
        w = word(im, a)
        if w is not None and (w >> 26) == 0x0F:
            out.add((w & 0xFFFF) << 16)
    return out


def andi_immediates(im, lo, hi):
    out = set()
    for a in range(lo, hi, 4):
        w = word(im, a)
        if w is not None and (w >> 26) == 0x0C:
            out.add(w & 0xFFFF)
    return out


def cstr(im, addr):
    o = addr - im.base
    if o < 0 or o >= len(im.data):
        return None
    e = im.data.index(b"\0", o)
    return im.data[o:e].decode("latin1")



def is_fourcc(im, addr):
    """True when the word at `addr` is four printable id characters, not an instruction."""
    raw = im.data[addr - im.base: addr - im.base + 4]
    return len(raw) == 4 and all(0x30 <= c <= 0x5A or c == 0x5F for c in raw)


def table(im, addr, n):
    return [word(im, addr + 4 * i) for i in range(n)]


def walk_stores(im, start, limit=20000):
    """Interpret a straight-line table filler, INCLUDING its `bgez` fill loops.

    The initialisers of the shell's dispatch tables are long straight-line runs of
    `sw` with three short backward `bgez` loops that splat one value over a whole
    table.  `frontend.py`'s walker ignores branches, so it sees only the last store
    of each loop; this walker models them, which is how the DEFAULT handlers of the
    per-screen and per-object tables are recovered at all.

    Returns {address: value}.  A value of None means "formed from a register this
    walker did not track".
    """
    a, reg, out = start, {0: 0}, {}
    steps = 0
    while steps < limit:
        steps += 1
        w = word(im, a)
        if w is None:
            break
        op, rs, rt = w >> 26, (w >> 21) & 0x1F, (w >> 16) & 0x1F
        if op == 0x0F:                                   # lui
            reg[rt] = (w & 0xFFFF) << 16
        elif op == 0x09:                                 # addiu
            base = reg.get(rs)
            reg[rt] = None if base is None else (base + simm(w)) & 0xFFFFFFFF
        elif op == 0x2B:                                 # sw
            base = reg.get(rs)
            if base is not None:
                out[(base + simm(w)) & 0xFFFFFFFF] = reg.get(rt)
        elif op == 0x01 and rt == 0x01:                  # bgez rs, back
            # A fill loop: `sw rV,0(rP); addiu rC,rC,-1; bgez rC,top; addiu rP,rP,-4`.
            top = a + 4 + (simm(w) << 2)
            if top < a:
                count = reg.get(rs)
                ptr_delta = None
                body = word(im, top)
                delay = word(im, a + 4)
                if (body is not None and (body >> 26) == 0x2B
                        and delay is not None and (delay >> 26) == 0x09):
                    p_reg = (body >> 21) & 0x1F
                    v_reg = (body >> 16) & 0x1F
                    if ((delay >> 16) & 0x1F) == p_reg and ((delay >> 21) & 0x1F) == p_reg:
                        ptr_delta = simm(delay)
                    ptr = reg.get(p_reg)
                    val = reg.get(v_reg)
                    if (count is not None and ptr is not None
                            and ptr_delta is not None and -1 <= count < 4096):
                        # This linear scan has already executed the body at `top` once
                        # and its counter decrement, so `count` is one short of the
                        # loop's initial value and the loop still owes count+2 stores.
                        for k in range(count + 2):
                            out[(ptr + ptr_delta * k) & 0xFFFFFFFF] = val
                        # ... and the delay slot below advances the pointer once more.
                        reg[p_reg] = (ptr + ptr_delta * (count + 1)) & 0xFFFFFFFF
                        reg[rs] = -1
        if w == 0x03E00008:                              # jr ra - its delay slot still runs
            d = word(im, a + 4)
            if d is not None and (d >> 26) == 0x2B:
                base = reg.get((d >> 21) & 0x1F)
                if base is not None:
                    out[(base + simm(d)) & 0xFFFFFFFF] = reg.get((d >> 16) & 0x1F)
            break
        a += 4
    return out


def slice_table(stores, base, n):
    return {(a - base) // 4: v for a, v in stores.items() if base <= a < base + 4 * n}


# ---------------------------------------------------------------------------
# Derivations
# ---------------------------------------------------------------------------


def pre_input_arms(im):
    """T_PRE_INPUT: 59 inline arms reached by the `jr v0` at 0x80066880."""
    return table(im, T_PRE_INPUT, SCREEN_COUNT)


def screen_input_table(im):
    """T_SCREEN_INPUT as the initialiser really leaves it - default included."""
    return slice_table(walk_stores(im, F_BUILD_NAV), T_SCREEN_INPUT, SCREEN_COUNT)


def object_input_table(im):
    return slice_table(walk_stores(im, F_BUILD_NAV), T_OBJECT_INPUT, 37)


def slider_commit_table(im):
    return slice_table(walk_stores(im, F_BUILD_NAV), T_SLIDER_COMMIT, SLIDER_SLOTS)


def screen_tick_tables(im):
    st = walk_stores(im, F_SCREEN_HANDLERS)
    return (slice_table(st, T_SCREEN_TRANS, SCREEN_COUNT),
            slice_table(st, T_SCREEN_TICK, SCREEN_COUNT))


def menu_button_sites(im):
    """Every `lb rX, off(rY)` in F_MENU_INPUT whose offset is a pad button slot."""
    out = []
    for a in range(F_MENU_INPUT, F_OBJECT_INPUT, 4):
        w = word(im, a)
        if w is not None and (w >> 26) == 0x20:
            off = simm(w)
            if 0x1A <= off <= 0x52 and (off - 0x1A) % 8 == 0:
                out.append((a, off, (off - 0x1A) // 8))
    return out


def edge_mask_sequence(im, lo, hi):
    """The `ori` constants of every `lbu rX,19(fe); ori; sb rX,19(fe)` triple."""
    seq = []
    for a in range(lo, hi, 4):
        w = word(im, a)
        if w is None or (w >> 26) != 0x24 or simm(w) != 19:
            continue
        for k in (1, 2, 3):
            n = word(im, a + 4 * k)
            if n is not None and (n >> 26) == 0x0D:
                seq.append(n & 0xFFFF)
                break
    return seq


def overlay_residency():
    """Which overlay each capture under work\\oracle\\state holds, 64 bytes at a time."""
    rows = []
    root = os.path.join(ROOT, "work", "oracle", "state")
    if not os.path.isdir(root):
        return rows
    for name in sorted(os.listdir(root)):
        p = os.path.join(root, name, "ram.bin")
        if not os.path.isfile(p):
            continue
        with open(p, "rb") as f:
            ram = f.read()
        row = {"state": name}
        for key, fname in (("F", "RASHCDF.BIN"), ("G", "RASHCDG.BIN"), ("I", "RASHCDI.BIN")):
            im = images()[key] if key in images() else E.load_overlay(fname, OVL_BASE)
            off = OVL_BASE - 0x80000000
            n = len(im.data) // 64
            ok = sum(1 for i in range(n)
                     if ram[off + 64 * i: off + 64 * i + 64] == im.data[64 * i: 64 * i + 64])
            row[key] = (ok, n)
        rows.append(row)
    return rows


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------


def cmd_info(_args):
    im = images()
    for k in ("SLUS", "F", "G", "I"):
        print("%-5s %-14s sha1 %s" % (k, os.path.basename(im[k].path), im[k].sha1))
    F, S = im["F"], im["SLUS"]
    print()
    print("the two loops, decoded from the instruction words:")
    print("  SLUS main   %08X" % S_MAIN)
    print("    %08X  jal %08X   the shell, COLD          (state == 2)"
          % (S_MAIN_COLD, jal_target(S, S_MAIN_COLD)))
    print("    %08X  jal %08X   the shell, AFTER A RACE  (state == 2)"
          % (S_MAIN_RESUME, jal_target(S, S_MAIN_RESUME)))
    print("    %08X  jal %08X   GameFrame - the RACE frame; the shell never calls it"
          % (0x80012414, jal_target(S, 0x80012414)))
    print("  RASHCDF shell frame %08X: while (game_state->f00 == 2) { ... }" % F_FRAME)
    for site, callee, what in FRAME_CALLS:
        got = jal_target(F, site) if F.contains(site) else jal_target(S, site)
        print("    %08X  jal %08X  %-58s %s"
              % (site, callee, what, "ok" if got == callee else "MISMATCH %08X" % (got or 0)))
    print()
    print("the input pass %08X, in call order:" % F_INPUT)
    for site, callee, what in INPUT_CALLS:
        got = jal_target(F, site)
        print("    %08X  jal %08X  %-58s %s"
              % (site, callee, what, "ok" if got == callee else "MISMATCH %08X" % (got or 0)))
    print()
    tbl = screen_input_table(F)
    overrides = sorted({v for v in tbl.values() if v != F_MENU_INPUT_DEFAULT})
    print("per-screen input handlers (table %08X):" % T_SCREEN_INPUT)
    print("    filled for all %d ids with the DEFAULT %08X -> %08X(screen, f0E, f0E+f0F)"
          % (SCREEN_COUNT, F_MENU_INPUT_DEFAULT, jal_target(F, F_MENU_INPUT_DEFAULT + 0x10)))
    print("    then %d distinct overrides stored over %d of the ids; %d keep the default"
          % (len(overrides), sum(1 for v in tbl.values() if v != F_MENU_INPUT_DEFAULT),
             sum(1 for v in tbl.values() if v == F_MENU_INPUT_DEFAULT)))
    obj = object_input_table(F)
    print("per-object input handlers (table %08X): default %08X over %d kinds, override [31] = %08X"
          % (T_OBJECT_INPUT, obj.get(0) or 0, len(obj), obj.get(31) or 0))
    print()
    print("the handover:")
    print("    screen %d input %08X -> game_state->f00 = %d   (leave the shell)"
          % (SCREEN_START_RACE, F_START_RACE_INPUT, word(F, 0x8006C394) & 0xFFFF))
    print("    screen %d tick  %08X -> the per-race loading picture %s / %s under %s"
          % (SCREEN_START_RACE, F_START_RACE_TICK,
             cstr(F, 0x8005BFA0), cstr(F, 0x8005BF94),
             cstr(images()["SLUS"], 0x8005248C).replace(chr(92), "/")))
    print("    commit         %08X  (rules.md 2.3), attract form %08X"
          % (F_COMMIT, F_COMMIT_SHORT))
    print("    back in        %08X  dispatches on session+0x00 through the %d arms at %08X"
          % (F_RESUME, 32, T_RESUME))


def cmd_frame(_args):
    F, S = images()["F"], images()["SLUS"]
    print("SHELL FRAME  RASHCDF %08X" % F_FRAME)
    print("  loop test  %08X lb v1,0(game_state); %08X li v0,2; back edge %08X"
          % (0x8008029C, 0x800802A0, 0x800803D0))
    for site, callee, what in FRAME_CALLS:
        print("   %08X  jal %08X  %s" % (site, callee, what))
    print()
    print("RACE FRAME   SLUS main %08X..%08X, body = PollPads + RaceStep + GameFrame %08X"
          % (0x80012360, 0x80012478, S_GAME_FRAME))
    print("  the shell's frame is NOT a state of GameFrame: RASHCDF contains %d `jal %08X`"
          % (len(call_sites(F, S_GAME_FRAME)), S_GAME_FRAME))
    print()
    print("PollPads %08X, what it does for the shell:" % S_POLL_PADS)
    print("   %08X  jal %08X  memcpy(%08X, %08X, %d)"
          % (0x8001CB80, jal_target(S, 0x8001CB80), S_PAD_SNAPSHOT, S_PAD_LIVE,
             word(S, 0x8001CB84) & 0xFFFF))
    print("   %08X  clear slot +0x06 of 19 slots x game_state->f34 players" % 0x8001CBC0)
    print("   %08X  if (game_state->f00 == 2) return   <- the shell takes nothing else"
          % 0x8001CC00)
    print()
    print("ordering tables: %08X swaps two %d-entry OTs, %08X frees last frame's builds"
          % (F_FRAME_END, 72, F_FRAME_FLUSH))


def cmd_input(_args):
    F = images()["F"]
    sites = menu_button_sites(F)
    by_index = {}
    for a, off, idx in sites:
        by_index.setdefault(idx, []).append(a)
    print("the generic menu input handler %08X, reached from every screen through %08X"
          % (F_MENU_INPUT, F_MENU_INPUT_DEFAULT))
    print("pad record %08X, stride 192; slot k's press code is at +0x14 + 8*k + 6"
          % S_PAD_SNAPSHOT)
    print()
    print("  off  slot  fe+0x13  what the menu does                       sites")
    for off, idx, what, bit in MENU_BUTTONS:
        print("  0x%02X   %d    0x%02X     %-40s %s"
              % (off, idx, bit, what,
                 " ".join("%08X" % a for a in by_index.get(idx, []))))
    print()
    print("  cursor moves: %08X jal %08X (previous), %08X jal %08X (next)"
          % (0x8006B28C, jal_target(F, 0x8006B28C), 0x8006B2BC, jal_target(F, 0x8006B2BC)))
    print("  fe+0x13 `ori` sequence: %s"
          % " ".join("0x%02X" % v for v in edge_mask_sequence(F, F_MENU_INPUT, F_OBJECT_INPUT)))
    print()
    print("  chooser values are edited by the OBJECT arm %08X -> %08X, which runs"
          % (F_OBJECT_INPUT, F_OBJECT_INPUT_BODY))
    print("  BEFORE the screen arms and handles only slots 0 and 1: %08X jal %08X / %08X jal %08X"
          % (0x8006BA50, jal_target(F, 0x8006BA50), 0x8006BA7C, jal_target(F, 0x8006BA7C)))
    print()
    print("  sliders: %08X binds fe+0x7C = %08X + 16*slot, fe+0x80 = %08X + 4*slot (= session+0xD4)"
          % (F_BIND_SLIDER, T_SLIDER_DESC, T_SLIDER_VALUE))
    print("  slider confirm table %08X: %s"
          % (T_SLIDER_COMMIT,
             " ".join("%08X" % (v & 0xFFFFFFFF) for v in
                      [slider_commit_table(F).get(i, 0) for i in range(SLIDER_SLOTS)])))
    print()
    print("  attract: %08X counts idle frames, %08X sends screen 4 to screen %d at %d"
          % (IDLE_COUNTER, 0x80066BF8, ATTRACT_TARGET, ATTRACT_TIMEOUT))


def cmd_flags(_args):
    F = images()["F"]
    print("widget+0x00 - the NAVIGATION condition word, read by %08X (MoveNext/MovePrev):"
          % F_MOVE_NEXT)
    for bit, where, what in COND_GATES:
        print("   0x%08X  %s" % (bit, what))
    print()
    print("widget+0x04 - the VISIBILITY condition word, read by %08X (the widget pass):"
          % F_WIDGET_PASS)
    print("   0x00003F00  a VENUE mask: bit session+0x04 must be set  (%08X andi 0x%04X)"
          % (0x8006D49C, word(F, 0x8006D49C) & 0xFFFF))
    print("   0x02000000  0x800D7428[24*fe->f16].f04 == 1")
    print("   0x08000000  mode == 4        0x04000000  mode != 4")
    print("   0x10000000  session+0x1B != 0")
    print("   0x80000000  only while this screen is the current one (fe->f00 == fe->f08)")
    print()
    print("so both words are FLAG words, not colours.  The shipped high bytes")
    print("(0x04.., 0x08.., 0x14.., 0x40..) are exactly these bits and nothing else.")
    print()
    print("widget type 16 (text block) resolves its runtime string through a %d-arm jump"
          % TEXTBLOCK_KINDS)
    print("table at %08X, index = widget+0x18 - 1, bound %08X sltiu v0,v1,%d"
          % (T_TEXTBLOCK_KIND, 0x8006FB80, word(F, 0x8006FB80) & 0xFFFF))


def cmd_state(_args):
    rows = overlay_residency()
    if not rows:
        print("no captures under work\\oracle\\state")
        return
    print("which overlay each capture holds (64-byte blocks matching the disc image):")
    print("  %-10s %-18s %-18s %-18s" % ("state", "RASHCDF (shell)", "RASHCDG (race)", "RASHCDI (loader)"))
    for r in rows:
        print("  %-10s %-18s %-18s %-18s"
              % (r["state"],
                 "%d / %d" % r["F"], "%d / %d" % r["G"], "%d / %d" % r["I"]))
    print()
    print("A `rrverify phys` row runs the original on a captured machine.  No capture")
    print("holds RASHCDF, so no shell function can be benched today.  That is the single")
    print("blocker the porting plan names first.")


class Bench:
    def __init__(self):
        self.n = 0
        self.bad = []

    def check(self, cond, what, detail=""):
        self.n += 1
        if not cond:
            self.bad.append(what)
            print("  FAIL  %s%s" % (what, ("  [%s]" % detail) if detail else ""))
        return bool(cond)


def config(mutate: bool) -> dict:
    """The claims the checks depend on.  --mutate perturbs eight of them; every
    perturbation is one of the load-bearing statements."""
    cfg = dict(
        menu_default=F_MENU_INPUT_DEFAULT,
        object_default=F_OBJECT_INPUT,
        confirm_slot=7,
        cancel_slot=6,
        prev_callee=F_MOVE_PREV,
        start_race_state=3,
        pad_copy_bytes=768,
        venue_mask=0x3F00,
        edge_sequence=EDGE_ORI_SEQUENCE,
        slider_desc=T_SLIDER_DESC,
    )
    if mutate:
        cfg["menu_default"] = 0x8006B03C     # "the default arm IS the menu handler"
        cfg["confirm_slot"] = 5              # "Circle confirms"
        cfg["prev_callee"] = F_MOVE_NEXT     # "Up and Down call the same mover"
        cfg["start_race_state"] = 6          # "screen 57 asks for the results scene"
        cfg["pad_copy_bytes"] = 192          # "PollPads snapshots one pad"
        cfg["venue_mask"] = 0x003F           # "the venue mask is the low nibble pair"
        cfg["edge_sequence"] = tuple(reversed(EDGE_ORI_SEQUENCE))
        cfg["slider_desc"] = 0x8009C548 + 16  # "the slider descriptors start one slot in"
    return cfg


def cmd_verify(args):
    cfg = config(args.mutate)
    im = images()
    F, S = im["F"], im["SLUS"]
    b = Bench()

    print("== images")
    for k in ("SLUS", "F", "G", "I"):
        b.check(im[k].sha1 == SHA1[k], "%s sha1 matches the documented image" % k, im[k].sha1)

    print("== 1. the shell's frame")
    b.check(jal_target(S, S_MAIN_COLD) == F_BOOT,
            "main %08X enters the shell cold at %08X" % (S_MAIN_COLD, F_BOOT))
    b.check(jal_target(S, S_MAIN_RESUME) == F_RESUME,
            "main %08X re-enters the shell after a race at %08X" % (S_MAIN_RESUME, F_RESUME))
    b.check(jal_target(S, 0x80012414) == S_GAME_FRAME,
            "main %08X calls GameFrame %08X - the RACE frame" % (0x80012414, S_GAME_FRAME))
    b.check(len(call_sites(F, S_GAME_FRAME)) == 0,
            "RASHCDF never calls GameFrame: the shell owns its own frame")
    b.check(word(F, 0x8008029C) == 0x80430000 and word(F, 0x800802A0) == 0x24020002,
            "the shell loop runs while game_state->f00 == 2 (%08X/%08X)" % (0x8008029C, 0x800802A0))
    w = word(F, 0x800803D0)
    b.check(w is not None and (w >> 26) == 4 and
            (0x800803D4 + (simm(w) << 2)) == 0x800802C8,
            "the shell loop's back edge %08X returns to %08X" % (0x800803D0, 0x800802C8))
    for site, callee, what in FRAME_CALLS:
        got = jal_target(F, site)
        b.check(got == callee, "frame call %08X -> %08X (%s)" % (site, callee, what),
                "got %08X" % (got or 0))
    for site, callee, what in INPUT_CALLS:
        got = jal_target(F, site)
        b.check(got == callee, "input-pass call %08X -> %08X (%s)" % (site, callee, what),
                "got %08X" % (got or 0))

    print("== 2. input")
    b.check(jal_target(S, 0x8001CB80) == S_MEMCPY and
            (word(S, 0x8001CB84) & 0xFFFF) == cfg["pad_copy_bytes"],
            "PollPads snapshots %d bytes of pad state into %08X"
            % (cfg["pad_copy_bytes"], S_PAD_SNAPSHOT),
            "got %d" % (word(S, 0x8001CB84) & 0xFFFF))
    b.check((word(S, 0x8001CB70) & 0xFFFF) == (S_PAD_SNAPSHOT & 0xFFFF) and
            (word(S, 0x8001CB78) & 0xFFFF) == (S_PAD_LIVE & 0xFFFF),
            "the snapshot's destination is %08X and its source %08X" % (S_PAD_SNAPSHOT, S_PAD_LIVE))
    b.check(word(S, 0x8001CBFC) == 0x24020002 and (word(S, 0x8001CC00) >> 26) == 5,
            "PollPads returns right after the snapshot when game_state->f00 == 2")
    tbl = screen_input_table(F)
    b.check(len(tbl) == SCREEN_COUNT, "the per-screen input table has %d entries" % SCREEN_COUNT,
            "%d" % len(tbl))
    default_count = sum(1 for v in tbl.values() if v == cfg["menu_default"])
    b.check(default_count == 26,
            "%08X is the DEFAULT per-screen input handler (26 ids keep it)" % cfg["menu_default"],
            "%d ids" % default_count)
    b.check(jal_target(F, F_MENU_INPUT_DEFAULT + 0x10) == F_MENU_INPUT,
            "the default arm %08X delegates to the menu handler %08X"
            % (F_MENU_INPUT_DEFAULT, F_MENU_INPUT))
    b.check(tbl.get(SCREEN_START_RACE) == F_START_RACE_INPUT,
            "screen %d's input handler is %08X" % (SCREEN_START_RACE, F_START_RACE_INPUT))
    sites = {idx: off for _a, off, idx in menu_button_sites(F)}
    for off, idx, what, _bit in MENU_BUTTONS:
        b.check(sites.get(idx) == off,
                "the menu handler reads pad slot %d at +0x%02X (%s)" % (idx, off, what))
    b.check(jal_target(F, 0x8006B28C) == cfg["prev_callee"],
            "slot 2 moves the cursor BACK, to %08X" % cfg["prev_callee"],
            "got %08X" % (jal_target(F, 0x8006B28C) or 0))
    b.check(jal_target(F, 0x8006B2BC) == F_MOVE_NEXT,
            "slot 3 moves the cursor FORWARD, to %08X" % F_MOVE_NEXT)
    confirm_off = 0x1A + 8 * cfg["confirm_slot"]
    by_addr = {a: off for a, off, _i in menu_button_sites(F)}
    b.check(by_addr.get(0x8006B3A4) == confirm_off and
            jal_target(F, 0x8006B494) == F_FIND_BY_CODE,
            "slot %d is CONFIRM: the arm at %08X is the one that resolves an action"
            " code through %08X" % (cfg["confirm_slot"], 0x8006B3A4, F_FIND_BY_CODE),
            "the arm reads +0x%02X" % (by_addr.get(0x8006B3A4) or 0))
    b.check(sites.get(cfg["cancel_slot"]) == 0x1A + 8 * cfg["cancel_slot"],
            "slot %d is CANCEL" % cfg["cancel_slot"])
    seq = tuple(edge_mask_sequence(F, F_MENU_INPUT, F_OBJECT_INPUT))
    b.check(seq == tuple(cfg["edge_sequence"]),
            "fe+0x13 is written with the documented `ori` sequence",
            " ".join("0x%02X" % v for v in seq))
    obj = object_input_table(F)
    b.check(sum(1 for v in obj.values() if v == cfg["object_default"]) == 36
            and obj.get(31) == F_OBJECT_INPUT_31,
            "the per-object table is %08X over 37 kinds with one override at [31] = %08X"
            % (cfg["object_default"], F_OBJECT_INPUT_31))
    b.check(jal_target(F, 0x8006BA50) == F_CHOOSER_PREV
            and jal_target(F, 0x8006BA7C) == F_CHOOSER_NEXT,
            "the object arm edits a chooser with %08X / %08X" % (F_CHOOSER_PREV, F_CHOOSER_NEXT))
    b.check((word(F, 0x80066BF8) & 0xFFFF) == ATTRACT_TIMEOUT
            and (word(F, 0x80066C10) & 0xFFFF) == ATTRACT_TARGET,
            "the attract timeout is %d idle frames and targets screen %d"
            % (ATTRACT_TIMEOUT, ATTRACT_TARGET))

    print("== 3. dispatch")
    arms = pre_input_arms(F)
    b.check(len(arms) == SCREEN_COUNT and all(a is not None for a in arms),
            "the pre-input jump table %08X has %d arms" % (T_PRE_INPUT, SCREEN_COUNT))
    b.check(len(set(arms)) == 5, "the pre-input table has exactly 5 distinct arms",
            "%d" % len(set(arms)))
    stop_music = tuple(i for i, a in enumerate(arms) if a == 0x80066888)
    trans, tick = screen_tick_tables(F)
    movie_by_tick = tuple(sorted(i for i, v in tick.items() if v == 0x8006DE5C))
    b.check(stop_music == MOVIE_SCREENS,
            "the 12 screens whose pre-input arm stops the music are the movie screens",
            "%s" % (stop_music,))
    b.check(movie_by_tick == MOVIE_SCREENS,
            "the same 12 ids are the ones whose tick handler is the movie player %08X" % 0x8006DE5C,
            "%s" % (movie_by_tick,))
    b.check(trans.get(SCREEN_START_RACE) == F_START_RACE_TICK
            and tick.get(SCREEN_START_RACE) == F_START_RACE_TICK,
            "both handlers of screen %d are %08X" % (SCREEN_START_RACE, F_START_RACE_TICK))
    sc = slider_commit_table(F)
    b.check(len(sc) == SLIDER_SLOTS and all((v & 0xFFFFFFFF) == 0xFFFFFFFF for v in sc.values()),
            "the slider confirm table %08X is -1 in all %d slots: confirm on a slider does nothing"
            % (T_SLIDER_COMMIT, SLIDER_SLOTS))
    b.check((word(F, 0x8006C444) & 0xFFFF) == 2 and (word(F, 0x8006C454) & 0xFFFF) == 17,
            "only widget types 12, 13 and 17 are selectable (%08X sltiu 2, %08X li 17)"
            % (0x8006C444, 0x8006C454))
    kinds = table(F, T_TEXTBLOCK_KIND, TEXTBLOCK_KINDS)
    b.check((word(F, 0x8006FB80) & 0xFFFF) == TEXTBLOCK_KINDS
            and all(k is not None and F.contains(k) for k in kinds),
            "the type-16 kind dispatcher has %d arms at %08X, all inside RASHCDF"
            % (TEXTBLOCK_KINDS, T_TEXTBLOCK_KIND))

    print("== 4. the condition words")
    nav_bits = lui_immediates(F, 0x8006C460, 0x8006C500)
    for bit, _where, what in COND_GATES:
        b.check(bit in nav_bits, "the navigation skip tests bit 0x%08X (%s)" % (bit, what))
    b.check((word(F, 0x8006D49C) & 0xFFFF) == cfg["venue_mask"],
            "the widget pass masks the visibility word with 0x%04X - the six venues"
            % cfg["venue_mask"], "0x%04X" % (word(F, 0x8006D49C) & 0xFFFF))
    b.check(word(F, 0x8006D4A4) == 0x00561004,
            "and shifts 1 by session+0x04, i.e. by the venue (%08X sllv)" % 0x8006D4A4)
    vis_bits = lui_immediates(F, 0x8006D4B0, 0x8006D548)
    for bit in (0x02000000, 0x08000000, 0x04000000, 0x10000000):
        b.check(bit in vis_bits, "the widget pass tests bit 0x%08X too" % bit)

    print("== 5. career state")
    b.check(jal_target(F, 0x8006CA4C) == F_SAVE_SLOT and jal_target(F, 0x8006CB18) == F_LOAD_SLOT,
            "the card screens reach SaveSlot %08X and LoadSlot %08X" % (F_SAVE_SLOT, F_LOAD_SLOT))
    b.check(word(F, 0x8006C8E0) == 0x2842000A,
            "the career slot cursor wraps at %d (%08X slti)" % (CAREER_SLOTS, 0x8006C8E0))
    b.check(word(F, 0x8006CAF0) == 0x00041900 and word(F, 0x8006CB00) == 0x00031880,
            "the card screen indexes a slot with the %d-byte stride" % CAREER_REC_SIZE)
    b.check(jal_target(F, 0x8006CA8C) == F_CARD_TICK and jal_target(F, 0x8006CABC) == F_CARD_TICK,
            "the card manager is ticked from the shell at %08X" % F_CARD_TICK)
    b.check((word(F, F_MUSIC_PLAY + 4) & 0xFFFF) == MUSIC_TRACKS,
            "the music player accepts %d tracks (%08X sltiu)" % (MUSIC_TRACKS, F_MUSIC_PLAY + 4))
    b.check(word(F, 0x800666D4) == 0x804200C0,
            "the shell picks its track from session+0xC0 + track (%08X lb 192)" % 0x800666D4)
    b.check((word(F, F_BIND_SLIDER + 0x2C) & 0xFFFF) == SLIDER_SLOTS,
            "the slider binder bounds the slot at %d" % SLIDER_SLOTS)
    b.check((word(F, 0x80064B74) & 0xFFFF) == (cfg["slider_desc"] & 0xFFFF)
            and (word(F, 0x80064B8C) & 0xFFFF) == (T_SLIDER_VALUE & 0xFFFF),
            "a slider edits %08X + 16*slot and writes %08X + 4*slot (= session+0xD4)"
            % (cfg["slider_desc"], T_SLIDER_VALUE),
            "0x%04X" % (word(F, 0x80064B74) & 0xFFFF))
    b.check(T_SLIDER_VALUE - SESSION == 0xD4,
            "the slider value block is session+0xD4 .. +0xEB")

    print("== 6. the handover")
    b.check((word(F, 0x8006C394) & 0xFFFF) == cfg["start_race_state"]
            and word(F, 0x8006C398) == 0xA0620000,
            "screen %d writes game_state->f00 = %d, which ends the shell loop"
            % (SCREEN_START_RACE, cfg["start_race_state"]),
            "%d" % (word(F, 0x8006C394) & 0xFFFF))
    b.check(word(F, F_START_RACE_INPUT) == 0x3C028006 and word(F, 0x8006C358) == 0xAC40ACAC,
            "and clears the idle counter %08X first" % IDLE_COUNTER)
    names = {cstr(F, 0x8005BF94), cstr(F, 0x8005BFA0)}
    b.check(names == {"ssload.tcm", "fsload.tcm"},
            "screen %d's tick loads the per-race loading picture" % SCREEN_START_RACE,
            "%s" % sorted(names))
    b.check(call_sites(F, F_COMMIT) == [0x8007FF2C, 0x800801F8],
            "the session -> game_state commit %08X has exactly two call sites" % F_COMMIT)
    resume = table(F, T_RESUME, 32)
    b.check(resume[31] == 0x8008002C and resume[3] == 0x8008005C and resume[0] == 0x80080014,
            "the post-race dispatcher arms career (mode 32), Time Trial (4) and Five-O (1)")
    b.check(jal_target(F, 0x80080034) == 0x8007BB34,
            "the career arm calls the career result dispatcher %08X" % 0x8007BB34)
    b.check(jal_target(F, 0x80080090) == F_BEATS_RECORD
            and jal_target(F, 0x80080128) == F_BEATS_RECORD,
            "the Time Trial arm tests the result against the records table twice")
    b.check(word(F, 0x80080114) == 0x000510C0 and word(F, 0x8008011C) == 0x00021080,
            "and the value it tests is player[k]+0x00 with the 36-byte player stride")
    b.check(jal_target(F, 0x80080190) == F_GOTO_SCREEN,
            "the post-race path re-enters the shell through GotoScreen %08X" % F_GOTO_SCREEN)

    print("== 7. presentation")
    b.check(jal_target(F, 0x8006D640) == F_WIDGET_PASS
            and jal_target(F, 0x8006D5D8) == F_WIDGET_PASS,
            "both default screen handlers run the widget pass %08X" % F_WIDGET_PASS)
    b.check(word(F, 0x8006D428) == 0x2453CF28 and word(F, 0x8006D42C) == 0x2453CF78,
            "the widget pass picks the UPDATE table when screen->f02 == 0 and the DRAW table otherwise")
    b.check(jal_target(F, 0x80066D24) == F_SCREEN_BUILD,
            "flags bit 2 makes the tick load the screen's resource list through %08X" % F_SCREEN_BUILD)
    b.check(jal_target(F, 0x80078EBC) == 0x80078C68,
            "and that list is a -1 terminated s16[] of resource indices")
    b.check(jal_target(F, 0x80080378) == 0x80048DB4 and jal_target(F, 0x80080464) == 0x80048CAC,
            "the shell draws with its own ordering tables: DrawOTag + ClearOTagR")
    b.check(jal_target(F, F_START_RACE_TICK + 0x30) == F_RES_RELEASE_ALL,
            "screen %d releases every shell resource before the race loads"
            % SCREEN_START_RACE)
    cop2 = [a for a, w in F.words() if (w >> 26) in (0x12, 0x32, 0x3A)]
    b.check(all(is_fourcc(F, a) for a in cop2),
            "the shell uses no GTE at all: every word that decodes as COP2 is a FourCC in a table",
            "%d words, %d not FourCC" % (len(cop2), sum(1 for a in cop2 if not is_fourcc(F, a))))

    print("== 8. the bench blocker, measured")
    rows = overlay_residency()
    if rows:
        # A RetroArch save state with the shell overlay resident, extracted into
        # `work\oracle\state\retro-shell`, lets a shell row run the original. The check keeps that
        # capture from going missing unnoticed.
        best = max(r["F"][0] for r in rows)
        total = max(r["F"][1] for r in rows)
        b.check(best > 0.9 * total,
                "some capture holds RASHCDF, so a shell row can run the original",
                "best %d of %d blocks" % (best, total))
        b.check(any(r["G"][0] > 0.9 * r["G"][1] for r in rows),
                "at least one capture does hold RASHCDG, which is why the race rows exist")
        # RASHCDI is still missing everywhere, and the starting grid needs it. Kept as a check so
        # that the day a capture appears, the probe says so instead of staying silent.
        bestI = max(r["I"][0] for r in rows) if rows and "I" in rows[0] else 0
        b.check(bestI == 0,
                "no capture holds RASHCDI: the starting grid still cannot be benched",
                "best %d blocks" % bestI)
    else:
        print("  (skipped: no captures under work\\oracle\\state)")

    fails = len(b.bad)
    print()
    print("shell: %d checks, %d failures" % (b.n, fails))
    if fails:
        for w in b.bad:
            print("  - %s" % w)
    sys.exit(1 if fails else 0)


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="shell.py", description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("info", help="the decoded runtime skeleton (addresses only)")
    s.set_defaults(func=cmd_info)

    s = sub.add_parser("frame", help="the shell's per-frame call order, and the race frame")
    s.set_defaults(func=cmd_frame)

    s = sub.add_parser("input", help="the pad -> menu control map")
    s.set_defaults(func=cmd_input)

    s = sub.add_parser("flags", help="the two widget condition words and their gates")
    s.set_defaults(func=cmd_flags)

    s = sub.add_parser("state", help="which overlay each oracle capture holds")
    s.set_defaults(func=cmd_state)

    s = sub.add_parser("verify", help="the assertion bench (the gate)")
    s.add_argument("--mutate", action="store_true",
                   help="perturb eight documented claims so the probe can be seen failing")
    s.set_defaults(func=cmd_verify)
    return ap


def main() -> None:
    args = build_parser().parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
