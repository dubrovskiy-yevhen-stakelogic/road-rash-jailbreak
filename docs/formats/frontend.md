# The front end: screen graph, menu database, career shell and save format
### (Road Rash: Jailbreak, USA, SLUS_01053)

This document covers everything the overlay `RASHCDF.BIN` owns: the boot sequence, the 58
menu screens and how they connect, the 120-byte widget records that describe every item on
every screen, how the pad drives them, the career shell (venue, unlocks, prize money, where
the player's bike and progress live), the PlayStation memory-card save, and the two `.LOC`
string pools.

It is the specification a native front end is meant to be written from, so every section
states the instruction address or file+offset that establishes it. Section 12 is the explicit
unknown list.

Prerequisites: `docs\formats\rules.md` (the mode bitmask, the session record, the race-result
codes and the prize table), `docs\formats\video.md` (the MDEC decoder), `docs\formats\textures.md`
(`FEMISC.PSH`, the `.PFN` fonts).

Confidence marks follow `rules.md`: **[proven]** = an exact identity over the whole data set,
or read straight out of the dispatching code *and* reproduced against real bytes;
**[established]** = read once out of our disassembly and holding over every sample;
**[probable]**; **[guess]**.

Binaries cited (our own SHA-1 over `work\disc_us`):

| file | SHA-1 | base |
|---|---|---|
| `RASHCDF.BIN` (the front end) | `a3fec4b4e9292c358d0f6dc529843f5d8f25924a` | `0x8005B5E8`, file offset 0 |
| `SLUS_010.53` | `67ed165a2c517d4e6106fb0dfa324d66dd9a76f1` | text at `0x80010000`, file offset `f` -> `0x80010000 + f - 0x800` |
| `DATA\FE\FESTRING.LOC` | `4081f537ff016f27942a124a9137d0f7328079bb` | 89232 bytes |
| `DATA\GAMESTRG.LOC` | `fcae2f23a2456b6a3e6b430711e6b35d58b841e8` | 3292 bytes |
| `DATA\FRONTEND.VUK` | (see 11.3) | 61660 bytes |

External evidence: two real PlayStation memory card images, both opened read-only - a RetroArch
`.srm` holding an in-progress career, SHA-1 `4a10c46a91fd2471c224c75a2aec441cd94b9cd6`, and an
empty formatted `.mcr`, SHA-1 `1b3c99be55273085e14b30983b636529b6dd7f46` (section 9).

Tool: `tools\scout\frontend.py` (gate "frontend: screen graph, widget tables and the save layout" in
`tests\run_gates.ps1`). Outputs: `work\frontend\` (gitignored). Re-run everything with

```
python tools\scout\frontend.py verify                 # 44 checks, 0 failures
python tools\scout\frontend.py info [screens|graph|widgets|codes|res|strings|career|save]
python tools\scout\frontend.py scan "<card.mcr or .srm>"
python tools\scout\frontend.py emit [outdir]          # CSVs + graph.dot into work\frontend
```

---

## 0. TL;DR

1. **The front end is a table-driven widget toolkit, not per-screen code.** There are
   **58 screen records of 20 bytes** and **1 reserved screen id**, built into a 59-entry
   pointer table at `0x800A0880` by `RASHCDF 0x800806D0`. Each screen points at an array of
   **120-byte widget records**; the widget's type byte selects one of **20 handlers** from the
   tables at `0x8009CF28` (update) and `0x8009CF78` (draw). The only per-screen code is an
   *input* handler in the table at `0x8009C8C0`; only 33 of the 59 ids have one at all, and eleven of
   those share a single handler.
2. **The screen graph is four lookup tables**, all filled by one initialiser,
   `RASHCDF 0x80068E88`: `0x8009C6C8[screenId]` = where "advance" goes, `0x8009C7B8[screenId]`
   = where "cancel" goes, `0x8009CAF8[actionCode]` = where a pressed button goes, and
   `0x8009C9B0[actionCode]` = which item of the same screen the cursor jumps to instead when the
   first table says `-1`. Nothing else changes screens; the single consumer is
   `0x8006B440..0x8006B4A4`.
3. **Screen id 57 has no record and means "leave the front end and start the race."** Every
   "Race" button in the game targets it (3.2).
4. **A menu item is described entirely by data**: a widget type, a 16-bit *action code*
   (0..80), a `FESTRING.LOC` string id for its label, x/y positions, three RGB triples, and a
   FourCC naming a sprite in `FEMISC.PSH`. `SetMode` (`rules.md` 2.1) is just one consumer of
   the action code.
5. **The career shell lives in the session record at `0x800D80D8`** (`rules.md` 1.2). The
   frontend edits it directly through "chooser" widgets bound to concrete globals; venue
   progress is a 65-bit bitmap at `session+0xF0` whose **setters and clearers are located**
   (7.3).
6. **The save is one 8 KiB PlayStation memory-card block** named `BASLUS-01053ROADRASH`,
   holding a 6948-byte payload: a 512-byte icon/title header, a 1584-byte records table, and
   **ten 484-byte career slots**, each a verbatim copy of `0x800D81D8` (216 bytes) followed by
   `0x800D80D8` (256 bytes) and a two-word checksum. **The checksum algorithm was recovered and
   reproduces all ten stored pairs of a real card bit-exactly.**
7. Two precise negatives: the seven `spr*.psh` resources the frontend declares
   (records 30..36) **are not on the disc**, and the 16 AI-opponent identities at
   `session+0x40..0xBF` are inside the saved range but **are all zero in a real, in-progress
   career**, so they are regenerated rather than restored.

---

## 1. Entry points and the frame

### 1.1 The two entries **[established, fully disassembled]**

`main` (`EXE 0x80012224`, `rules.md` 3.1) enters the front end at two places, both after
`load_overlay(16)`:

| at | call | role |
|---|---|---|
| `EXE 0x80012288` | `RASHCDF 0x8007FEDC` | cold boot |
| `EXE 0x800122DC` | `RASHCDF 0x8007FF4C` | return from a race |

**Cold boot `0x8007FEDC`**, in call order:

```
0x8007FEE4  EXE 0x80011738      game_state initialiser
0x8007FEEC  0x80080CC8          video / draw-environment setup
0x8007FEF4  0x8007FDA8          session record init: *(u32*)0x800D80D8 = 32 (career), ...
0x8007FEFC  0x80080544          load the persistent frontend resources
0x8007FF04  0x800665E8          fe->f00 = 0 (0x800665FC: sh zero,-14896(s0))
0x8007FF0C  0x800806D0          build the screen pointer table
0x8007FF14  0x800809E0(0)       GotoScreen(0)  - the EA logo movie
0x8007FF1C  0x80080C04
0x8007FF24  0x80080274          THE FRONTEND MAIN LOOP
0x8007FF2C  EXE 0x8007F37C      commit the menu selection into game_state
0x8007FF34  0x80080220          tear down
```

**`0x8007FDA8`** sets `*(u32*)0x800D80D8 = 32` at `0x8007FDC8` (`li v1,32; sw v1,-32552(v0)`),
i.e. **the front end boots with the mode word already set to "career"**.

**Return from a race `0x8007FF4C`** does the same three setup calls and then dispatches on the
mode word:

```
0x8007FFD8  a0 = *(u32*)0x800D80D8;
0x8007FFE8  v1 = a0 - 1;
0x8007FFEC  if ((unsigned)v1 >= 32) { s0 = 4; goto show; }    // 0x80080180
0x8007FFF8  jump table at 0x8005CB80, index = v1
...
0x80080184  0x80063908();
0x80080190  GotoScreen(s0);                                    // 0x800809E0
0x80080198  s0 = screenTable[s0]->parent;                       // +0x0A
0x800801C4  while (s0 != -1) { 0x800809A8(s0); s0 = screenTable[s0]->parent; }
0x800801F0  0x80080274();                                       // the main loop again
```

So after a race the front end re-enters at a **hub screen chosen by the mode**, and then walks
the new screen's parent chain arming every screen in it. Observed hub ids in the arms: 4, 24,
26, 30, 32, 34, 38, 40, 52.

Two side effects on this path worth porting: `0x8007FF88..0x8007FFB0` increments
`session+0x0F` and wraps it from 38 back to 32 (`slti v0,v0,38; li v0,32`) when
`*(u32*)0x8005B220` is non-zero - the attract/demo rotation; and `0x8007FFDC..0x8007FFE4`
clears bits of `session+0x05` (`andi v0,v0,0xC7`) and copies `session+0x1A` into `session+0x19`.

### 1.2 The main loop - `0x80080274` **[established]**

```c
while (game_state->state == 2) {            // 0x8008029C, 0x800803C8
    EXE 0x8001CB3C();                       // 0x800802C8  pad poll
    EXE 0x8001C428();                       // 0x800802D0  frame delta
    ... double-buffer flip via 0x8005AE00 / 0x8005AE04 ...
    0x800667E4(...);                        // 0x80080314  INPUT pass
    0x80066C34();                           // 0x8008031C  SCREEN STATE MACHINE
    if ((s8)fe->f12 > 0) fe->f12--;         // 0x80080324
    else { EXE 0x8001C3F4(); EXE 0x8001C408(); *(u32*)0x80098C44 = 0; }
    if (*(0x8009C2F0)) 0x80062774(0x8009C2F8);
    EXE 0x80048DB4(otag);                   // 0x80080378  DrawOTag
    0x8006711C();                           // 0x80080380  DRAW pass
    EXE 0x800487C0(0);
    if (*(0x8009C2F4)) 0x80062774(0x8009C3D0);
    0x80080488();  *(0x8009C2F0) = 0; *(0x8009C2F4) = 0;  0x800803FC();
}
```

The loop exits when something sets `game_state+0x00` away from 2; the frontend's own way of
doing that is `0x8006E008` (section 3.5).

---

## 2. The screen database

### 2.1 The pointer table **[proven]**

`RASHCDF 0x800806D0` is a straight-line initialiser that stores 58 pointers into
`0x800A0880`; `tools\scout\frontend.py` replays it symbolically. The array is dimensioned
**59** - the loop at `0x800809F8..0x80080A24` runs `for (i = 0; i < 59; i++)` (`slti v0,a2,59`
at `0x80080A1C`), and the same bound appears in the per-frame sweep at `0x80066D44`.

The one index never written is **57**. `GotoScreen`'s sweep therefore dereferences a NULL
pointer for that entry once per screen change. See section 12, unknown 1.

### 2.2 The 20-byte screen record **[proven]**

| off | type | meaning | evidence |
|---|---|---|---|
| +0x00 | u16 | flags; bit 0 and 1 = "live", bit 2 = "needs a rebuild" | `0x800809C8` sets `\|= 3`; `0x800809A0..0x80080A0C` clears the low byte; `0x80066D80` tests `& 4`; `0x8006DE94` tests `& 4` |
| +0x02 | s16 | transition state: **32767 = entering, 32766 = leaving, 0 = idle** | `0x800809D8` (`li v0,32767`), `0x8006D5EC` (`li v0,32766`), `0x80066CA8` |
| +0x04 | s16 | **the selected item index** | `0x8006696C..0x80066988` computes `items + 120*this` into `fe+0x84`; written by `0x8006C3A4` / `0x8006C558` |
| +0x06 | s16 | **the screen's own id** - equals its table index in all 58 records | `0x80066D8C`, `0x80066A3C` use it to index the handler tables; asserted by `frontend.py verify` |
| +0x08 | s16 | **item count** | `0x8006C3D0` bounds the cursor with it |
| +0x0A | s16 | **parent screen id, -1 = none** | walked at `0x80066F9C`, `0x80080198`, `0x800801E0`; patched at run time for the modals (section 3.6) |
| +0x0C | u8 | **enter transition id** | `0x8006D5D4` / `0x8006D638`: `lbu v1,12(a0)` -> `fe+0x11`, then `0x8006D3E0` |
| +0x0D | u8 | **leave transition id** | `0x8006D5F8`: `lbu v1,13(a0)` -> `fe+0x11` |
| +0x0E | u8 | **first pad index** | `0x8006A954`: `lb s1,14(s2)` is the loop start |
| +0x0F | u8 | **pad count** | `0x8006A958`: `lb v0,15(s2)`, loop bound `s1 < first + count` |
| +0x10 | u32 | **pointer to the widget array** | `0x8006695C` (`lw a0,16(v0)`), `0x8006DB78`, `0x8006C42C` |

The shipped values of `+0x0C`/`+0x0D` are only two pairs: **54/63** on the 50 full screens and
**36/45** on the eight that behave as overlays (25, 39, 51, 52, 53, 54, 55, 56). `+0x0E`/`+0x0F`
is `0+2` on the attract, main-menu, multiplayer and options screens, `0+1` on the
single-player ones, and **`1+1` on screens 31, 33 and 35** - the second-player half of each
two-player setup, which only listens to pad 2. **[established]**

### 2.3 What the 58 screens are

Names below are descriptive, derived from the screen's title sprite (a `type 7` widget whose
FourCC is not a backdrop), from the `FESTRING.LOC` ids its buttons carry, and from the graph. No
game text is reproduced. Full table: `screens.csv` (`frontend.py emit`).

| id | title tag | what it is | reached from |
|---|---|---|---|
| 0 | - | movie `ea_logo.wve` | boot |
| 1, 58 | - | movie `intro.wve` | 0 / attract |
| 2 | - | movie `spazpunt.wve` | 3 |
| 3 | `SPLH` | the legal / splash panel; its handler `0x8006AC80` is a pure **301-frame timeout** into the attract movie, with no button path | 1 |
| 4 | `MAIN` | **main menu** - Solo / Multiplayer / Options | advance from 2, 22, 58; cancel from 5, 29, 42, 58 |
| 5 | `SNGL` | single-player menu - Jail Break / Five-O / Time Trial | 4 |
| 6 | - | movie `gangs.wve` | 5 |
| 7 | `JAIL` | **career setup** - Continue / Gang / Alias | 6 |
| 8 | `JAIL`+`LVLP` | **the career hub** - Race / Course / Bike / Rap Sheet | 7 |
| 9 | `RAPS` | the rap sheet (career statistics) | 8 |
| 10, 13, 14, 17, 18, 23 | - | the six between-race story / notification screens; all six have the same seven-widget shape | the story movies |
| 11 | - | movie `gauntlet.wve` | career progress |
| 12 | - | movie `movingup.wve` | career progress |
| 15 | - | movie `busted.wve` | career progress |
| 16 | - | movie `got_oink.wve` | career progress |
| 19 | - | movie `thesetup.wve` | career progress |
| 20 | - | movie `jailbrak.wve` | career completion |
| 21 | `CRED` | the end credits panel | 20 |
| 22 | - | movie `credits.wve` | 21 |
| 24 | `TIME` | **Time Trial hub** - Race / Rank / Bike / Course / Trophy Room / Race Options | 5 |
| 25 | - | race-options overlay (parent 24) | 24 |
| 26 | `TRPY` | trophy room | 24, 40 |
| 27 | `FIVE` | **Five-O hub** - Race / Fugitive | 5 |
| 28 | - | Five-O result / notification | 27 |
| 29 | `MULT` | **multiplayer menu** - 5 entries | 4 |
| 30, 31 | `SKUL` | Head-to-Head setup, player 1 then player 2 | 29 |
| 32, 33 | `COPS` | Cops & Robbers setup, player 1 then player 2 | 29 |
| 34, 35 | `SCAR` | Side Car (Versus) setup, player 1 then player 2 | 29 |
| 36 | `SCAR` | Side Car (Co-op) setup | 29 |
| 37 | - | Side Car result / notification | 36 |
| 38, 40 | `TIME` | multiplayer Time Trial setup, player 1 then player 2 | 29 |
| 39 | - | race-options overlay (parent 38) | 38 |
| 41 | `TRPY` | trophy room reached from 40 | 40 |
| 42 | `OPTN` | **options menu** - 9 entries | 4 |
| 43, 44 | `LOAD` | load game / load records | 42 |
| 45, 46 | `SAVE` | save game / save records | 42 |
| 47 | `JUKE` | jukebox | 42 |
| 48 | `NOIS` | sound options - six sliders plus an audio-mode chooser | 42 |
| 49 | `CTLR` | controller options | 42 |
| 50 | `CRED` | credits | 42 |
| 51 | - | multiplayer options overlay (parent 42) | 42 |
| 52 | `TRPY` | new-record panel | post-race, when `0x8007E7BC` returns true |
| 53 | - | message panel | card path |
| 54, 55 | - | **the "abort game?" confirm modal**; 55 when `game_state+0x04 == 4`, 54 otherwise | `0x8006B110..0x8006B118` |
| 56 | - | record display panel | 42 |
| **57** | - | **no record: "start the race"** | every Race button |

The `.STR` result screens named in `rules.md` 7.3 (`bust`, `lose`, `win`, `wreck`, `jailed`,
`escape`) are **not** screens in this table. They are resource records 20..25 (section 5.2)
played *over* whatever screen is current by `res_show` (`RASHCDF 0x800782B0`). **[established]**

---

## 3. The screen graph and the state machine

### 3.1 The frontend context `fe` = `0x8009C5D0` **[proven]**

One struct drives everything. Fields that matter:

| off | type | meaning | evidence |
|---|---|---|---|
| +0x00 | s16 | **the current screen id** | read at `0x80066834`, `0x80066C84`, `0x8006A898`, ~15 sites |
| +0x02 | s16 | **the requested screen id** | 13 writers; consumed at `0x80066FF0` |
| +0x04 | s16 | a screen to enter after a modal resolves | `0x800686E8`, `0x80080094`; copied into `+0x02` at `0x800695E0` |
| +0x08 | s16 | the sweep cursor of the per-frame loops | `0x80066C7C` |
| +0x0A | u16 | **this frame's button/edge mask** | `0x80066904` (`& 1`), `0x80066934` (`& 4`), `0x8006A88C` (`& 8`) |
| +0x11 | s8 | the transition id currently playing | 40 readers; written from `screen+0x0C`/`+0x0D` |
| +0x12 | s8 | a frame countdown the main loop decrements | `0x80080324` |
| +0x1C | s16 | a `FESTRING.LOC` id for the pending notification | `0x8007D934`, read at `0x8007D940` |
| +0x70,+0x72,+0x74,+0x78,+0x7A | s16 | the post-race bonus fields; **`+0x70` is the race bonus in cash** | cleared at `0x8007BCC0..0x8007BCD0`, filled at `0x8007BFE8`, `0x8007C018` |
| +0x7C,+0x80 | u32 | the slider object and its value table (widget type 17) | `0x80064B84`, `0x80064B98` |
| +0x84 | Widget* | **the currently selected widget** | `0x80066988` |
| +0x94 | void* | **the data object bound to the selected chooser** | `0x800683E4`, read at `0x800669AC` |
| +0xAC,+0xB0 | u32 | a FourCC / a countdown used by the career notifications | `0x8007D518`, `0x8007BCB4` |
| +0xB4,+0xB8 | u32/s16 | the open movie file handle and its state | `0x8006DC14`, `0x8006DEB8` |

### 3.2 The four navigation tables **[proven]**

All four are filled by the single initialiser `RASHCDF 0x80068E88` and are read with a
2-bit shift, i.e. they are `u32[]` read as halfwords:

| table | index | meaning | reader |
|---|---|---|---|
| `0x8009C6C8` | screen id | **advance**: where a movie/credits screen goes when it ends or is skipped | `0x8006A89C`, `0x8006AABC`, `0x8006AB38`, `0x8006ACF8`, `0x8006B3CC` |
| `0x8009C7B8` | screen id | **cancel**: where the back button goes; `-1` means "no back" | `0x8006AF5C`, `0x8006B08C`, `0x8006B244`, `0x8006B860`, `0x8006BCC0` |
| `0x8009CAF8` | action code | **the screen a pressed button navigates to**, `-1` = none | `0x8006B448..0x8006B470` |
| `0x8009C9B0` | action code | **another action code on the same screen**: when the first table says `-1`, the cursor jumps to that item instead | `0x8006B478..0x8006B4A4` |

The instruction that actually consumes them is the confirm handler at
`0x8006B440..0x8006B4A4` **[proven]**:

```c
code = item->f12;                                  // 0x8006B444
p = (u32*)0x8009CAF8 + code;                       // 0x8006B448..0x8006B450
if (*p != -1) { fe->f02 = (u16)*p; return; }       // 0x8006B454..0x8006B470  NAVIGATE
q = (u32*)0x8009C9B0 + code;                       // 0x8006B478
if (*q != -1) screen->f04 = 0x8006C700(screen, (s16)*q);   // 0x8006B480..0x8006B4A4
```

`0x8006C700(screen, code)` resolves an action code to an item index on that screen, so the
second table means "pressing confirm on this chooser moves the cursor to that item" - which
in the shipped data is always the group's **Race** / **Continue** button.

Both code-indexed tables are terminated by `-1` at index 80, which fixes the action-code range
at **0..80** and matches the three gates that read it: `0x800685E8` (`code - 2 < 30`),
`0x80072120` (`code < 80`) and `0x80068124` (`code - 9 < 72`). **[proven]**

The full decoded graph is `graph.dot` / `action_codes.csv` (`frontend.py emit`).
The load-bearing spine, all four from the tables above:

```
0 ea_logo -> 1 intro -> 3 splash --(301-frame timeout)--> 2 spazpunt -> 4 MAIN
4 MAIN  code 2 -> 5 SNGL        code 3 -> 29 MULT       code 4 -> 42 OPTN
5 SNGL  code 5 -> 6 gangs -> 7 JAIL      code 6 -> 27 FIVE      code 7 -> 24 TIME
7 JAIL  code 8 -> 8 career hub  (codes 9, 10 are choosers, they do not navigate)
8 hub   code 11 -> 57 START RACE          code 14 -> 9 RAPS
24 TIME code 15 -> 57 START RACE  code 19 -> 26 TRPY   code 20 -> 25 options overlay
27 FIVE code 25 -> 57 START RACE
29 MULT code 27 -> 30 -> (32) 31 -> 57     code 28 -> 32 -> (38) 33 -> 57
        code 29 -> 36 -> 57                code 30 -> 34 -> (47) 35 -> 57
        code 31 -> 38 -> (55) 40 -> 57
42 OPTN codes 63..71 -> 43,44,45,46,49,48,47,50,51
```

Every "Race" button in the game - codes 11, 15, 25, 36, 42, 44, 52, 61 - targets screen 57.
**[proven]**

### 3.3 The per-frame screen state machine - `0x80066C34` **[established]**

```c
fe->f08 = 0;
for (i = 0; i < 59; i++) {                       // 0x80066C80 .. 0x80066D48
    if (i == fe->f00) continue;                  // the current screen is done last
    scr = screenTable[i];
    if (scr->f02 != 0) {                         // 0x80066CA8  a transition is in flight
        f = transitionTable[i];                  // 0x8009D0C0
        if (f) { f(scr); continue; }
        scr->f02 = 0; continue;
    }
    if (scr->flags & 2) { f = tickTable[i]; if (f) f(scr); continue; }   // 0x8009CFD0
    if (scr->flags & 4) { scr->flags &= ~4; 0x80078E80(*(void**)(0x8009D3F0 + 4*scr->f06)); }
}
// then the same body for i = fe->f00                                     0x80066D50
```

The two handler tables are filled by `RASHCDF 0x8006D174`: **every entry defaults to
`0x8006D5B0` (transition) and `0x8006D630` (tick)**, with these overrides:

| screens | transition | tick | what they are |
|---|---|---|---|
| 0, 1, 2, 6, 11, 12, 15, 16, 19, 20, 22, 58 | `0x8006DB5C` | `0x8006DE5C` | the movie screens |
| 25, 39, 51, 53, 54, 55 | `0x8006D8B4` | `0x8006D658` | the overlay/modal screens |
| 57 | `0x8006E008` | `0x8006E008` | **start the race** |

### 3.4 Changing screen - `0x80066EF8` **[established]**

```c
cur = fe->f00; next = fe->f02;
if (cur == next) return;                          // 0x80066F20
*(u32*)0x8005ACAC = 0; *(u32*)0x80098C50 = 0;
for (p = cur; p != -1; p = screenTable[p]->f0A) {  // walk the OLD parent chain
    if (p is also in the NEW screen's parent chain) { keep it live; }
    else { scr->flags &= ~3; if (scr->f02 == 0) scr->f02 = 32766; }   // 0x80066FC4..0x80066FD4
}
scr = screenTable[next];
if (!(scr->flags & 3) && scr->f02 == 0) scr->f02 = 32767;             // 0x8006702C
if (cur == 4) session->f10 = 4;                                       // 0x80067048
if (cur == 5 || 6 || 7 || cur == 29) 0x8006883C(next);                // 0x80067078
```

So a screen change is expressed as *transitions on two parent chains*, which is what lets a
modal sit on top of a live screen. `32767`/`32766` are consumed by the default transition
handler `0x8006D5B0`, which plays `screen+0x0D` on the way out and `screen+0x0C` on the way in.

`GotoScreen(id)` (`0x800809E0`) is the hard reset: it clears `flags & 0xFF00` and `f02` on
**all 59** screens, sets `fe->f00 = fe->f02 = id`, caches `screenTable[id]` in `0x8009C5C8`,
and calls `0x800809A8(id)` to arm it. It is used exactly twice, at `0x8007FF14` (boot) and
`0x80080190` (post-race). **[proven]**

### 3.5 Leaving the front end - screen 57 **[established]**

Screen 57 has no record, but it does have handlers: `0x8009D0C0[57]` and `0x8009CFD0[57]` are
both `0x8006E008`, and `0x8009C8C0[57] = 0x8006C354`. The main loop ends when
`game_state+0x00` stops being 2, and `EXE 0x80012304` then loads the race overlay. The
selection reaches `game_state` through `RASHCDF 0x8007F37C`, called at `0x8007FF2C` and
`0x800801F8` - the function `rules.md` 2.3 documents.

`0x8007FD20` is the short form of the same commit, used on the attract path:

```
0x8007FD38  *(u32*)0x8005B220 = 0
0x8007FD3C  game_state->f04 = (u8)session->f00        // race type <- mode low byte
0x8007FD4C  game_state->f30 = (s8)session->f06        // player count
0x8007FD58  game_state->f40 = (s8)session->f08        // race id
0x8007FD64  game_state->f3C = game_state->f3A = (s8)session->f04   // bank / environment
0x8007FD78  game_state->f48 = *(s8*)0x800D81DF        // player 1 bike index
0x8007FD80  screenTable[(s8)session->f0C]->f04 = (s8)session->f0D  // restore a selection
```

The last line is a small but load-bearing fact: **`session+0x0C` is a screen id and
`session+0x0D` the item index to preselect on it.** **[established]**

### 3.6 The confirm modal **[established]**

At `0x8006B0D8..0x8006B15C`, when the cancel button is pressed on a screen whose
`flags & 0x400` is set, `session+0x18` is non-zero and a race is in progress:

```c
fe->f02 = (game_state->f04 == 4) ? 55 : 54;       // 0x8006B10C..0x8006B118
screenTable[55]->f0A     = fe->f00;               // 0x8006B130  parent <- the caller
screenTable[55]->f0E     = caller->f0E;           // 0x8006B140  inherit the pad window
screenTable[55]->f0F     = caller->f0F;           // 0x8006B158
fe->f13 |= 4;
```

That is how screens 54/55 float over an arbitrary screen: **their parent field is written at
run time.** Screens 25, 39 and 51 instead carry a static parent (24, 38 and 42).

---

## 4. The widget record - how a menu is described

### 4.1 Layout **[established]**

A screen's items are a flat array of **120-byte** records (`0x80066974..0x8006697C` computes
`(i*16 - i)*8 = 120*i`; `0x8006C420..0x8006C428` the same). The header is common; everything
from `+0x10` is type-specific.

| off | type | meaning | evidence |
|---|---|---|---|
| +0x00 | u32 | a per-widget condition word; the navigation skip test reads bit `0x40000000` (`0x8006C464`: `lui v0,0x4000`). The shipped values are `0x00000000` (423 widgets), `0x00063F7F` (90) and five one-offs that add a high byte: `0x04063F7F`, `0x08063F7F`, `0x14063F7F`, `0x400A3F7F` - so the high byte really is a separate flag field | `0x8006C460`, `0x8006C468` |
| +0x04 | u32 | a second condition word of the same shape | set alongside `+0x00` in every shipped record |
| +0x08 | s16 | **widget type, 0..19** | `0x80072100`, `0x8006C438`, and the two handler tables |
| +0x0A | u16 | **widget flags**; `0x0020` = "draw only while the screen is live" is **[established]** (`0x8006FADC`, `0x8006E934`: `andi v0,v0,0x20` then a test of `screen->flags & 2`). `0x1000` marks a starting item **[probable]** - 29 of the 35 screens that have selectable widgets carry it on exactly one of them, but screens 27, 32, 42 and 51 carry it on two, 49 on three and 48 on none. `0x0100`/`0x2000`/`0x4000` gate visibility, mechanism not decoded | `0x8006FADC`, `0x8006E934`, `0x8006FB60` |
| +0x0C | u32 | unused in every shipped record (always 0) | inspection of all 596 widgets |
| +0x10.. | - | type-specific | below |

For the two interactive types, **12 (button)** and **13 (chooser)**:

| off | type | meaning | evidence |
|---|---|---|---|
| +0x10 | u16 | 0 in every shipped record | |
| +0x12 | u16 | **the action code, 0..80** | `0x800685E0` (`lhu v0,18(a1)`), `0x80068118`, `0x80072118` |
| +0x14 | u16 | **the `FESTRING.LOC` id of the label**; the blurb shown beside it is `id + 1` | see 4.3 |
| +0x18,+0x1C,+0x20 | 3 x u8[4] | three RGB triples (idle / highlighted / disabled) | shipped values `(0xB4,0x6E,0x28)`, `(0x96,0xA0,0xB9)`, `(0x8C,0x8C,0xA0)` on every button |
| +0x24,+0x26 | u16 | **text x, y** | 37 / 50+20k on every list; matches the item order |
| +0x28 | char[4] | the FourCC of the button sprite, always `BTNN` | `FEMISC.PSH` entry |
| +0x2C | u8[4] | the sprite's colour | |
| +0x30,+0x32 | u16 | **sprite x, y** | 20 / 46+20k |

For **type 7 (sprite)**: `+0x10` FourCC, `+0x14` RGB, `+0x18`/`+0x1A` x/y.
For **type 8 (logo)**: `+0x14` is a kind in **48..53**; `0x8006E958..0x8006E970` computes
`kind - 48` and jumps through the 6-arm table at `0x8005C08C`. That function is the one
`rules.md` 2.2 documents as the mode-logo installer.
For **type 0 (movie)**: `+0x10` is a **u32 index into the 42-entry name array** - see 5.1.
For **type 16 (text block)**: `+0x10..+0x16` are x, y, w, h and `+0x18` a small "kind" enum
(observed 1..33 and 60) selecting which runtime string the block renders.
For **type 17 (slider)**: `+0x64` is an index `< 6` and `+0x66` a slot; `0x80064B54..0x80064B98`
turns them into `fe+0x7C` = `0x8009C548 + 16*slot` and `fe+0x80` = `0x800D81AC + 4*slot`.

### 4.2 The 20 widget types **[established]**

`RASHCDF 0x8006D174` fills `0x8009CF28` (update) and `0x8009CF78` (draw) with one handler per
type. Both tables agree except at index 1.

| type | update | draw | instances | reading |
|---|---|---|---|---|
| 0 | - | - | 14 | **movie** - handled by the screen, not the widget (`0x8006DB5C`) |
| 1 | `0x8006E400` | `0x8006E3BC` | 0 | unused in the shipped data |
| 2..6 | `0x8006E4D8` | `0x8006E4D8` | 18 | a family of animated decorations (2:1, 3:1, 4:1, 5:2, 6:13) |
| 7 | `0x8006E894` | `0x8006E894` | 73 | **sprite by FourCC** |
| 8 | `0x8006E8FC` | `0x8006E8FC` | 46 | **mode / venue logo** |
| 9 | `0x8006E7A4` | `0x8006E7A4` | 59 | **pad-glyph button hint** |
| 10 | `0x8006EDE4` | `0x8006EDE4` | 0 | - |
| 11 | `0x8006ECA0` | `0x8006ECA0` | 14 | panel / picture frame |
| 12 | `0x8006F764` | `0x8006F764` | 47 | **button** |
| 13 | `0x8006EF30` | `0x8006EF30` | 44 | **chooser** (a button plus a bound value) |
| 14 | `0x8006F9A4` | `0x8006F9A4` | 6 | header bar |
| 15 | `0x8006FA38` | `0x8006FA38` | 2 | - |
| 16 | `0x8006FAC8` | `0x8006FAC8` | 181 | **text block** |
| 17 | `0x8006F4A0` | `0x8006F4A0` | 6 | **slider** |
| 18 | `0x8006F0B8` | `0x8006F0B8` | 10 | **the bike page's stat bars** (Acceleration / Durability / Top Speed) - not a scrolling list; checked against the original's primitives by the gate "menus: the Bike page against the original's primitives" |
| 19 | - | `0x80070580` | 0 | draw-only, unused |

### 4.3 How a screen refers to a string **[proven]**

Three mechanisms, all live:

1. **Buttons and choosers carry the id directly** in `widget+0x14`, into `FESTRING.LOC`. The
   convention throughout the shipped data is that the id is the label and `id + 1` is the
   long description drawn in the blurb box; every menu list uses consecutive odd/even pairs.
   `frontend.py verify` asserts every such id is `< 1945`.
2. **Text blocks (type 16)** carry a small *kind* in `+0x18` instead of an id: the handler
   resolves it to a runtime string (the selected course's name, the player's cash, and so on).
   The kind-to-source mapping is **not decoded** - unknown 6.
3. **Code-installed notifications**: `fe+0x1C` holds a `FESTRING.LOC` id written directly by
   the career progress arms, e.g. `0x8007D930` (`addiu v0,s1,1218`), `0x8007D524`
   (`addiu v0,s1,1190`), `0x8007D584` (`1166`), `0x8007D5B0` (`1178`); `s1 = rnd & 3` selects
   one of four phrasings.

Lookup itself is `((const char **)0x8005B544)[id]`, the table `RASHCDI 0x80063D94` relocates
(`rules.md` 7.5); the frontend's own relocation of the same two globals is `0x80065F4C`.

---

## 5. Frontend assets

### 5.1 The 42-entry name array - `0x8008973C` **[proven]**

42 pointers into the string pool `0x8005C618..0x8005C857`.

| range | content |
|---|---|
| 0..10 | the **eleven `.WVE` cut-scenes**: `ea_logo`, `intro`, `spazpunt`, `gangs`, `gauntlet`, `movingup`, `busted`, `got_oink`, `thesetup`, `jailbrak`, `credits` |
| 11..20 | the ten mode `.STR` screens: `rr_logo`, `modejb`, `modefivo`, `modett`, `modeh2h`, `modecnr`, `modescar`, `modeh2hs`, `trophy`, `options` |
| 21..41 | the 21 bike `.STR` thumbnails, in the bike-index order `rules.md` 1.3 proves |

**The movie widget indexes this array.** `0x8006DBE4..0x8006DBF8`:
`name = ((char**)0x8008973C)[ item->f10 ]`, then `sprintf(buf, "DATA\%s", name)` at
`0x8006DBFC` (format string at `0x8005BF84`) and open at `0x8006DC08`. Every shipped movie
widget's index is in 0..10, i.e. a `.WVE`; `frontend.py verify` asserts it. **[proven]**

Note the path mismatch: the files live in `DATA\FE\`, the format string says `DATA\`. See
unknown 3.

### 5.2 The 37 resource records - `0x800897E4` **[proven]**

32 bytes each; the count is fixed by the `slti v0,a0,37` bounds at `0x80076344` and
`0x800782D4`. Layout as `rules.md` 7.3 gives it (`u16 flags; u8 class; u8 idInClass; void*
data; char name[16]; s32 lba; u32 reserved`). `class` is 1 `.psh`, 2 `.str`, 3 `.pfn`,
4 `.loc`, 5/6 dynamic. `res_show(index, mode)` is `0x800782B0`.

Records 0..29 are the shipped screens and fonts; **20..25 are `bust`, `lose`, `win`, `wreck`,
`jailed`, `escape`** and 26..29 the two gangs' win/lose screens, exactly as `rules.md` 7.3
predicted from the other end.

**Precise negative - records 30..36 are not shipped.** They name `sprfiveo.psh`, `sprjb.psh`,
`sprscar.psh`, `sprh2h.psh`, `spropts.psh`, `sprtt.psh`, `sprtrphy.psh`; a sweep of the whole
disc extract finds **`FEMISC.PSH` as the only `.psh` file**. All seven carry `flags == 0x0000`
(every shipped record has a non-zero flags word) and **no instruction takes the address of any
of them** (`exe.scan_xrefs` over the record addresses returns empty). They are a per-mode
sprite-sheet scheme that was dropped before release. `frontend.py verify` asserts this
exactly, so it is a regression check rather than a tolerated failure. **[proven]**

### 5.3 The FourCC id tables **[established]**

Two runs of four-character ids, `0x80088DF4` (306 entries) and `0x800892BC` (288 entries).
They are the game's asset-id enum: `FEMISC.PSH` is a `SHPP` container whose 26 entries are
keyed by exactly these ids (`DATA\FE\FEMISC.PSH` +0x00 `SHPP`, +0x08 count `0x1A`, then
`FourCC, u32 offset` pairs - `BLAW`, `BRAW`, `DARW`, `SBTN`, `LARW`, `RARW`, `UARW`, `SBTH`,
`BTNH`, `CRCL`, `JPLR`, `SQRE`, `TRIG`, `JPUD`, ...). The tables also contain the mode logos
`RRLG`/`JBLG`/`5OLG`/`TTLG`/`HHLG`/`CRLG`/`SCLG`/`SHLG`/`TYLG`/`CNLG` and the venue logos
`GTLP`/`DTLP`/`JBLP` that `rules.md` 2.2 lists, the screen titles (`MAIN`, `MULT`, `OPTN`,
`RSLT`, `RAPS`, `SCAR`, `SNGL`, `TRPY`, `JAIL`, `LOAD`, `SAVE`, `NOIS`, `JUKE`, `CTLR`,
`CRED`, `TIME`, `FIVE`, `COPS`, `SKUL`, `LVLP`), the ten weapon ids (`NCHK`, `CLUB`, `WOOD`,
`CBAR`, `CHAN`, `PIPE`, `PROD`, `STUN`, `SPRY`, `HALO`), the 21 bike ids, and two course
runs: **`FS01..FS30` + `FS38..FS64` (57 ids)** and **`SS01..SS18` + `SS38..SS55` (36 ids)** -
the "first set" and "second set" course thumbnails, matching the 64/36 race split of
`ROADGRF1.TXT`/`ROADGRF2.TXT`. The 36 `SS` ids equal `ROADGRF2.TXT`'s race count exactly.
**[established]**

The alias-entry keyboard is one 47-character string at `0x80088C9C`
(A..Z, 0..9, then four three-letter keys). **[proven]**

---

## 6. Input

### 6.1 The pad record **[established]**

`0x800D7128`, stride **192**. `0x8006A978..0x8006A984` computes `(p*3) << 6 = 192*p`.
Per-button state lives at `pad + 0x1A + 8*b`, a signed counter per button that the menu tests
with `bgtz` (pressed this frame) - the sites are `+0x1A`, `+0x22`, `+0x2A`, `+0x32`, `+0x3A`,
`+0x42`, `+0x4A`, `+0x52`, `+0x5A`, `+0x62`, `+0x6A`, `+0x72`, `+0x7A`, `+0x82`, i.e. **14
buttons**. `pad+0x04` and `pad+0xC4` are polled at `0x80066808`/`0x80066818` to decide whether
any pad is connected.

Identified buttons, from what the menu does with them:

| offset | index | what the menu does | evidence |
|---|---|---|---|
| +0x1A | 0 | decrement the selected chooser's value | `0x8006B2CC..0x8006B304` |
| +0x2A | 2 | **move the selection back** -> `0x8006C558` | `0x8006B26C..0x8006B28C` |
| +0x32 | 3 | **move the selection forward** -> `0x8006C3A4` | `0x8006B29C..0x8006B2BC` |
| +0x4A | 6 | **cancel / back** -> `fe->f02 = backTable[fe->cur]` | `0x8006AF88..0x8006AFD0` |

The remaining ten are read in the same sweep but their consumers were not followed - unknown 4.

### 6.2 Moving the selection - `0x8006C3A4` / `0x8006C558` **[established]**

```c
void MoveNext(Screen *s) {
    cur = s->f04;
    i = (cur == 32767) ? (s->f04 = 0, 0) : (cur + 1 < s->itemCount ? cur + 1 : 0);
    while (i != s->f04) {                       // 0x8006C3F0
        w = s->items + 120*i;
        if (w->type == 12 || w->type == 13 || w->type == 17) {
            ... test w->f00 against 0x40000000 and the per-gang / per-mode gates ...
            if (selectable) { s->f04 = i; play a sound; return; }
        }
        i = (i + 1 < s->itemCount) ? i + 1 : 0;
    }
}
```

So **only types 12, 13 and 17 are selectable**, the cursor wraps, and a widget can be skipped
by its condition word. `0x8006C558` is the mirror image walking backwards. `32767` is the
"nothing selected yet" sentinel; `widget+0x0A & 0x1000` marks the item a screen starts on
(exactly one per screen in the shipped data).

### 6.3 Confirming - the action code **[proven]**

The per-frame input pass `0x800667E4` does, in order:

```
0x80066834  fe->f08 = fe->f00;  scr = screenTable[fe->f00];  *(0x8009C5C8) = scr
0x8006686C  jump through the 59-arm per-screen pre-input table at 0x8005B9D4 (jr at 0x80066880)
0x8006695C  fe->f84 = scr->items + 120 * scr->f04        // the selected widget
0x800669A4  0x800680E8(scr, fe->f84)                     // bind its data object
0x800669BC  if (fe->f94) { h = 0x8009CC40[ *(u8*)(fe->f94 + 2) ]; if (h) h(scr); }
0x800669FC  0x80064B30(scr, fe->f84)                     // slider binding
0x80066A24  for (p = fe->f08; p != -1; p = scr->f0A)     // the parent chain
                { h = 0x8009C8C0[scr->f06]; if (h) consumed = h(scr); }
```

`0x8009C8C0` is the per-screen input handler table. Only 33 of the 59 ids have an entry:
eleven share `0x8006A8FC` (the "any button skips" movie handler), two share `0x8006A838`,
two share `0x8006AB58`, and the other eighteen are one-offs. The handlers are what write
`fe->f02`.

`0x800680E8(screen, item)` binds the **chooser**: if `item->type == 13` and
`(code - 9) < 72`, it jumps through the 72-arm table at `0x8005BAC4` and installs a pointer
into `fe+0x94`. The pointers are a small array of "editable value" objects at
`0x8009C4B0..0x8009C540`, plus `0x8009C678` as the null object. Two arms are conditional and
are the most informative:

* **code 10 ("Alias")** - `0x8006815C` reads `*(s8*)0x800D81E1` (= `player[0] + 0x09`, the
  gang id) and selects `0x8009C4B4` or `0x8009C4B8`.
* **code 13 ("Bike")** - `0x800681B0` does the same test and selects `0x8009C4C0` or
  `0x8009C4C4`.

i.e. **the alias list and the bike list are per-gang**. **[established]**

The full code table, with which screen each code is used on, its label id, its target screen,
its group commit code, the mode it installs and the object it binds, is `action_codes.csv`
(`frontend.py emit`).

---

## 7. The career shell

### 7.1 Where the player's state lives **[proven]**

Everything persistent is in the session record `0x800D80D8` and the player records at
`0x800D81D8` that `rules.md` 1.2/1.3 document. The front end is the only writer.

The chooser bindings make the career screens concrete:

| screen | code | what the item edits |
|---|---|---|
| 7 (career setup) | 9 | the gang, through object `0x8009C4B0` |
| 7 | 10 | the alias, through a per-gang object (6.3) |
| 8 (career hub) | 12 | the course, object `0x8009C4BC` |
| 8 | 13 | the bike, through a per-gang object |
| 33 (Cops & Robbers P2) | 43 | `*(s8*)0x800D81DF` = `player[0]+0x07`, **the bike index** |
| 24 / 30 / 32 / 34 / 38 | 16, 33, 39, 48, 56 | "Rank", all bound to the same object `0x8009C4C8` |

`player[k]+0x07` is the bike index `rules.md` 1.3 proves; it reaches `game_state+0x48` at
`0x8007FD78` and `0x8007F59C`.

### 7.2 Starting a career - `0x80068688(screenId)` **[established]**

```c
EXE 0x8002D250();                              // clear the 65-bit bitmap, bit by bit
session->f11 = 0;                              // 0x800686C0
player[0].f06 = player[1].f06 = 127;           // 0x800686C4, 0x800686C8  "no second bike"
session->f05 = 0x30;                           // 0x800686D0  default option bits
session->f15 = session->f16 = session->f1D = 0;
fe->f04 = 40;  fe->f22 = 0;                    // 0x800686E8, 0x800686F0
for (k = 0; k < 6; k++) {                      // 0x800686F4 .. 0x80068718
    player[k].f0C = 0; player[k].f0E = 0; player[k].f0F = 0;
    player[k].f10 = 0; player[k].f14 = 0; player[k].f16 = 0;
}
memset(session + 0x40, 0, 128);                // 0x8006872C  the 16 AI identities
memset(session + 0x20, 0,  16);                // 0x80068734  the result counters
if (screenId != 24 && screenId != 38) { session->f16 = 1; fe->f04 = 24; }
```

**The loop bound is 6**: the game keeps **six** 0x24-byte player/profile records, not two.
That is corroborated three ways - the loop at `0x80068710` (`slti v0,a2,6`), the six strings
`Player 1`..`Player 6` at `0x8005B98C..0x8005B9CF`, and the save block, which stores
**216 bytes = 6 x 0x24** from `0x800D81D8` (section 8.4). **[established]**

### 7.3 Unlocks - the two bitmaps **[proven]**

`rules.md` 1.2 found the bitmaps and their wholesale clears but not their accessors. Both are
here.

**Readers.** `0x8007E724(first, last)` and `0x8007E770(first, last)` are the same loop over
`session+0xF0` and `session+0xFC`:

```c
int AllDone(int first, int last) {
    if (last < first) return 1;                        // 0x8007E724
    do {
        b = *(u8*)(session + 0xF0 + (first >> 3));     // 0x8007E738, 0x8007E73C
        if (!((b >> (first & 7)) & 1)) return 0;       // 0x8007E744..0x8007E754
        first++;
    } while (first <= last);
    return 1;
}
```

So **bit `i` of `session+0xF0` means "item `i` is done"**, and the venue gate is a *range* test.

**Writers, all four of them, in the result dispatchers:**

| at | operation | context |
|---|---|---|
| `0x8007BF8C` | **set** bit `session->f08` (the race id) of `+0xF0` | the career win arm; `session->f11` is incremented immediately after (`0x8007BF98`) |
| `0x8007E0E8` | **set** a bit of `+0xF0` | `0x8007E0EC` also bumps `session->f11` |
| `0x8007D904` | **clear** a bit of `+0xF0` (`nor` + `and`) | the venue-progress arms |
| `0x8007E2A4` | **clear** a bit of `+0xF0` | |
| `0x8007E47C`, `0x8007E54C` | **set** a bit of `+0xFC`, index `session->f07` | the Five-O / mission path; `0x8007E484`/`0x8007E48C` also bump `session->f11` and `session->f20` |

The reading "bit = race completed" is **[established]**: the setter uses `session+0x08`, which
`rules.md` 1.2 proves is the race id, and a real save has exactly 8 bits set in an early career
(section 9).

### 7.4 Advancing the venue - `0x8007D4A0` **[established]**

```c
v = session->f04;                               // 0x8007D4A0
if (v < 6) jump table[0x8005C988 + 4*v];        // 0x8007D4B4..0x8007D4C8
```

Venue-0 arm, `0x8007D4D0`:

```c
if (AllDone(1, 9)) {                            // 0x8007D4D4  races 1..9 of the venue
    session->f04 += 1;                          // 0x8007D4F0..0x8007D500   NEXT VENUE
    res_show(9, 1);                             // 0x8007D4FC  miscres.str
    fe->fAC = 'JB04';                           // 0x8007D518
    k = EXE 0x8001FC58() & 3;                   // 0x8007D514, 0x8007D51C
    fe->f1C = 1190 + k;                         // 0x8007D934  the notification string
} else if (outcome == 1) {
    switch (session->f0D) { case 3: fe->fAC='PT01', fe->f1C=1166+k; break;
                            case 6: fe->fAC='SP01', fe->f1C=1178+k; break; }
}
```

The other five arms (`0x8007D5B4`, `0x8007D64C`, `0x8007D730`, `0x8007D7C8`, `0x8007D8B8`)
have the same shape with different ranges; venue 1's is `AllDone(28, 28)` (`0x8007D5B8`).
**This is the unlock rule: a venue is finished when every race in its id range carries its bit,
and finishing it increments `session+0x04`.** **[established]**

### 7.5 Money **[established, with one gap]**

The prize table at `0x80089F44` is `rules.md` 7.4's; `frontend.py` re-reads it and `verify`
asserts row 1 (`420 / 780 / 780 / 1260 / 1260 / 1260`) and that places 1..15 fall
monotonically in every venue column.

The award path, from the career result dispatcher `0x8007BB34`:

```
0x8007BCC0..0x8007BCD0  fe->f70 = f72 = f74 = f78 = f7A = 0     // clear the bonus fields
0x8007BCD8              dispatch on *(u32*)0x800D81F8           // the result code
  win arm  0x8007BD90   prize = *(u16*)(0x80089F44 + 4*session->f04)
           0x8007BDA4   session->f20 += 1
           0x8007BF6C   fe->f70 = prize                          // the race bonus
  lose arm 0x8007BFB4   prize = *(u16*)(0x80089F44 + 24*(place-1) + (venue << outcome))
           0x8007BFDC   session->f26 += 1
           0x8007C018   fe->f70 = prize
```

**`fe+0x70` is the race bonus as cash**, and `+0x72`, `+0x74`, `+0x78`, `+0x7A` are the other
four bonus lines the results screen shows (`FESTRING.LOC` 1519..1534 name them per
`rules.md` 7.4). Where the running balance is accumulated is still open - see unknown 7. The
strongest candidate the data offers is `player[0]+0x00`, the only large counter in the whole
save (81690 in the examined card); no instruction reading or writing it was located.

---

## 8. The save format

Derived from `RASHCDF` and **verified against a real memory card image**.

### 8.1 Which API **[proven]**

The **raw BIOS card API** (PsyQ `libcard`), not the BIOS file API and not `libmcrd`. The
trampolines and their call sites:

| trampoline | BIOS call | called from `RASHCDF` at |
|---|---|---|
| `SLUS 0x800449F4` | `A0(0xAB)` `_card_info` | 11 sites, `0x8005D240`.. |
| `SLUS 0x80044A04` | `A0(0xAC)` `_card_load` | `0x8005D650` |
| `SLUS 0x80044A24` | `B0(0x4E)` `_card_write` | `0x8005D21C`, `0x8005D3CC`, `0x8005D510`, `0x8005D7A8` |
| `SLUS 0x80044A34` | `B0(0x4F)` `_card_read` | `0x8005D098`, `0x8005D364`, `0x8005D4A8`, `0x8005D614`, `0x8005D6BC` |
| `SLUS 0x80044A44` | `B0(0x50)` `_new_card` | `0x8005D8E8` |

`RASHCDF` has **zero direct MMIO accesses** (`exe.py io`), so this is the whole hardware path.

**Negative: the `bu0%1ld:%s` string at `0x8005B66C` is dead.** It is formatted at exactly two
places, `0x8005E6C4` and `0x8005E938`, into a 256-byte stack buffer that is never read again
(`0x8005E6CC..0x8005E71C` and `0x8005E940..0x8005E9CC` contain no load from it). It is a
leftover from a BIOS-file-API version of the code and has no effect on the format.

### 8.2 The call path **[established]**

```
0x8005EB80  InitCardManager()   ctx = 0x80099550, 11644 bytes
              0x8005EBBC  memcpy(ctx+0x20, 0x80080E8C, 512)      // the "SC" block header
              0x8005EBD4  every career slot marked EMPTY (rec[0] = 1)
              0x8005EBF4  0x8005DFC0(0, ctx+0x1BC4)              // install the driver,
                          0x8005DFF0: *(0x800548CC) = ctx+0x1BC4    port 0
0x8005E858  BuildDirectoryFrame(name, slot)
0x8005E790  FindFreeBlocks(size)       n = (size + 8191) >> 13
0x8005E5B8  FindFile(name)             20-char strcmp per slot (0x8005E61C)
0x8005E668  WriteFile(name, buf, size) -> 0x8005CC38 (cmd 2, write), 0x8005CE68 (poll)
0x8005E8F0  ReadOrCreateFile(name, buf, size) -> 0x8005CD08 (cmd 3) or 0x8005CD90 (cmd 6)
0x8005F1A8  SaveGame()          WriteFile("BASLUS-01053ROADRASH", ctx+0x20, 6948)
0x8005F1DC  LoadOrCreateGame()  0x8005EA54 (checksum every slot) then ReadOrCreateFile(...)
0x8005F21C  the per-frame tick, called from the frontend at 0x8006CA8C and 0x8006CABC
```

The payload length **6948** is the literal `a2` at `0x8005F1BC` and `0x8005F1F8`.

### 8.3 The block on the card **[proven]**

The manager context from `ctx+0x20` **is** the block image: block offset `X` is guest
`0x80099570 + X`. (Independently confirmed: block offset `0x1BE8` holds the value
`0x8009B1D8`, which is exactly the guest address of the slot-name buffer that appears at block
offset `0x1C68`.)

| block off | size | content |
|---|---|---|
| 0x000 | 512 | the PS1 `SC` title frame, a **byte-exact copy of the static template at `RASHCDF 0x80080E8C`** |
| 0x200 | 1 | the current career slot index (`sb` at `0x8006CDC4`) |
| 0x201 | 1 | "records table initialised" (`sb` at `0x8006CE44`) |
| 0x202 | 2 | padding, zero |
| 0x204 | 1584 | the **records / best-times table**, seeded from `SLUS 0x80053A88` (`memcpy` at `0x8006CE48`) |
| 0x834 | 10 x 484 | the **ten career slots** |
| 0x1B1C | 8 | padding to 6948 |

`512 + 4 + 1584 + 4840 + 8 == 6948` exactly; `frontend.py verify` asserts the arithmetic.

**Header as this game fills it**, all from the template:

| off | size | value |
|---|---|---|
| 0x00 | 2 | `'SC'` |
| 0x02 | 1 | `0x13` - three icon frames |
| 0x03 | 1 | `0x01` - block number, **hardcoded** |
| 0x04 | 0x40 | the title, 20 full-width Shift-JIS characters |
| 0x60 | 0x20 | 16 x BGR555 icon CLUT |
| 0x80 / 0x100 / 0x180 | 0x80 each | three 16x16 4bpp icon frames - **byte-identical to each other**, so the icon is static |

**Directory frame**, built by `0x8005E858` into `*(0x800548CC) + 2480 + 128*slot`:
`+0x00 = 0x51` (`0x8005E894`), `+0x04 = 8192` (`0x8005E89C`), `+0x08 = 0xFFFF`
(`0x8005E8A4`), `+0x0A = "BASLUS-01053ROADRASH"` (20 bytes, `memcpy` at `0x8005E8AC`),
`+0x7F = XOR of bytes 0..126` (`0x8005E8BC..0x8005E8D8`).

**Granularity, and a latent original bug.** `WriteFile` writes `(6948 + 127) >> 7 = 55`
frames = 7040 bytes (`0x8005E6D0`). `ReadOrCreateFile` uses a hardcoded **64 frames = 8192
bytes** (`0x8005E954`, `0x8005E98C`), so the create path dumps `ctx+0x20 .. ctx+0x2020` onto
the card - which is why the real file's tail holds the card driver's own state - and the read
path reads 8192 bytes back over `ctx+0x20`, **overwriting the live driver context at
`ctx+0x1BC4` with the stale copy from the file**. A native port should read 6948 bytes, or
re-establish the driver state after a load.

### 8.4 The 484-byte career record **[proven]**

Base `ctx + 2132 + 484*slot`, i.e. block offset `0x834 + 484*slot`.

| rec off | size | content | evidence |
|---|---|---|---|
| +0x000 | 4 | **slot state: 1 = empty, 0 = in use** | init `0x8005EBD4`; cleared on save `0x8006CDFC`; restored to 1 after a checksum failure `0x8005EB60` |
| +0x004 | 216 | verbatim copy of guest **`0x800D81D8`** = the **six** 0x24-byte player records | `memcpy` `0x8006CDF8` / `0x8006CEA8`, `a2 = 216` at `0x8006CDB8` / `0x8006CE70` |
| +0x0DC | 256 | verbatim copy of guest **`0x800D80D8`** = `session+0x00..0xFF` | `memcpy` `0x8006CE0C` / `0x8006CEBC`, `a2 = 256` |
| +0x1DC | 4 | checksum A | `0x8005EA84` |
| +0x1E0 | 4 | checksum B | `0x8005EA8C` |

Note the **halves are stored in reverse order**: the record holds the contiguous guest range
`0x800D80D8..0x800D82F7` as `[+0x100..+0x1D7][+0x000..+0x0FF]`. Field meanings inside both
halves are `rules.md` 1.2 and 1.3.

`0x8006CDA4` is `SaveSlot(slot)`, `0x8006CE60` is `LoadSlot(slot)`; `LoadSlot` finishes by
re-dispatching on the restored mode word (`0x8006CEC4`: `lw v0, 0x800D80D8`; `0x8006CED0`:
jump table `0x8006BF04`, index `mode - 1`). `0x8006CE24` seeds the records table.

### 8.5 The checksum - `0x8005E9D0` **[proven, reproduced bit-exactly]**

Over the **first 476 bytes** of a record, i.e. everything but the two checksum words:

```c
u32 a = 0, b = 0, acc = 0;
for (u32 i = 0; i < 476; i++) {
    u8 x = rec[i];
    a   = a + x + i + acc;          /* 32-bit wrap */
    b   = ((x ^ b) << 1) + acc;     /* 32-bit wrap */
    acc = acc + (slot + 1);
}
```

Registers: `t0` = `i` (`0x8005E9D0`, bumped `0x8005EA2C`), `t1` = `slot + 1` (`0x8005E9D4`),
`a3` = `acc` (`0x8005E9D8`, bumped in the delay slot `0x8005EA48`); sum A `0x8005EA08..0x8005EA1C`,
sum B `0x8005EA20..0x8005EA3C`, bound 476 at `0x8005EA40`. Because `acc` at step `i` is
`i*(slot+1)`, **the checksum depends on the slot index** - a record cannot be moved between
slots without recomputation.

`0x8005EA54` recomputes all ten (bound `slti 10` at `0x8005EA90`) and is called only from
`0x8005F1E4`, immediately before the file is created, so a fresh file is born valid.
`0x8005EAB4(slot, eraseOnFail)` verifies (`0x8005EAFC`, `0x8005EB10`) and on mismatch zeroes
the record (`0x8005EB4C`, `a1 = 484`) and sets `rec[0] = 1`.

**`python tools\scout\frontend.py scan "<card.srm>"` recomputes all ten pairs and they all match
what the card stores.** `verify` exercises the same code without needing a card, by
asserting the pairs of a fresh (all-zero, `rec[0] = 1`) record in all ten slots.

There is **no** checksum over the records table, the header, or the file as a whole; only
these ten pairs, plus the standard per-frame XOR byte in block 0.

---

## 9. What a real card contains

`.1.mcr` is an **empty, formatted card**: all 15 directory frames are state `0xA0` and all 15
data blocks are zero. It holds no save of this game. The `.srm` is the same 128 KiB raw card
format (both begin `'MC'`); it holds one block:

```
slot 0  state 0x51  size 8192  link 0xFFFF  name BASLUS-01053ROADRASH   (frame XOR ok)
```

Decoded (`frontend.py scan`), values only - no game text:

* Header: byte-identical to the template at `0x80080E8C`.
* `+0x200` current slot **0**; `+0x201` records-initialised **0**.
* Records table `+0x204`: **entirely zero**, 0 non-zero bytes of 1584. `0x8006CE24` has never
  run in this playthrough - which matters for a port, because `SLUS 0x80053A88` is *not* zero.
* Career slot 0 in use, slots 1..9 empty; **all ten checksum pairs verify**.
* Session record: mode **8**, venue **2**, players **1**, race id **13**, progress counters
  1/1/1, `+0x11 = 2`, `+0x20 = 2`, `+0x22 = 3`, `+0x24 = 3`, `+0x26 = 3`.
* `session+0xF0`: bits **{1,2,3,4,5,6,10,11}** set - eight races done in an early career,
  consistent with the venue-0 gate `AllDone(1, 9)`.
* `session+0xFC`: **no bits set**.
* `session+0x40..0xBF` (the 16 AI identities): **all zero**. They are saved but not
  meaningfully persisted, so the roster is rebuilt at race setup.
* Player records: `player[0] = {+0x00: 81690, +0x06: 7, +0x07: 7, +0x09: 0, +0x20: 1}`,
  `player[1] = {+0x00: 0, +0x06: 127, +0x07: 7, ...}`, records 2..5 essentially zero. Bike
  index 7 is inside the documented 0..20 range, and the "no second bike" sentinel 127 that
  `0x800686C4` writes is present.

This is the cross-check that matters: every field the code predicts is present and in range,
and the checksum - the one thing that cannot be fitted by accident - reproduces exactly.

---

## 10. The string pools

Both files share the container `rules.md` 7.5 documents, re-verified here:

```
+0x00  char magic[4] = "LOCH"
+0x04  u32  headerSize = 0x14
+0x08  u32  0
+0x0C  u32  1                       // version
+0x10  u32  chunkOffset = 0x14
--- chunk ---
+0x00  char magic[4] = "LOCL"
+0x04  u32  chunkSize = fileSize - 0x14
+0x08  u32  0
+0x0C  u32  count
+0x10  u32  offset[count]           // relative to the start of the LOCL chunk
       ...   NUL-terminated 8-bit strings, unaligned
```

| file | size | count | index ends | first string | empty | longest |
|---|---|---|---|---|---|---|
| `DATA\GAMESTRG.LOC` | 3292 | **181** | 0x2E4 | 0x2E4 | 0 | 42 bytes |
| `DATA\FE\FESTRING.LOC` | 89232 | **1945** | 0x1E74 | 0x1E74 | 102 | 236 bytes |

`frontend.py verify` asserts, for both: the magic pair, `chunkSize + 0x14 == fileSize`, that
the offset index ends exactly where the first string begins, and that every offset is inside
the chunk. Those four together make the layout non-negotiable.

**How a screen refers to a string** is section 4.3. The id ranges the front end actually
indexes, described by function only (ids and counts, never text):

| ids | evidence | function |
|---|---|---|
| 30..36 | `widget+0x14` of the type-16 field labels and of codes 0, 16/33/39/48/56 | field captions on the setup screens |
| 37..144 | `widget+0x14` of every type-12/13 widget on screens 4, 5, 7, 8, 24, 27, 29..42 | the menu entries, in label / blurb pairs (label even, blurb odd) |
| 176..190 | `widget+0x14` on screens 48 and 49 | sound sliders, audio mode, controller options |
| 405..1004 | `rules.md` 7.4 | the 100 course / event descriptors, strict groups of six |
| 1005..1070 | `rules.md` 7.4 | the 22 bike catalogue entries, groups of three |
| 1166..1221 | `0x8007D584` (`+1166`), `0x8007D5B0` (`+1178`), `0x8007D524` (`+1190`), `0x8007D930` (`+1218`), each with `rnd & 3` | the career-progress notifications, groups of four |
| 1510..1534 | `rules.md` 7.4 | the unlock notices and the post-race bonus labels |
| 1542..1557 | `widget+0x14` on screens 51, 54, 55 | the modal prompts |

---

## 11. Odds and ends

### 11.1 The mode word, from the menu side **[proven]**

`rules.md` 2.1 derived the menu-code -> mode map from the jump table at `0x8005BC64`. The
widget data confirms it independently: the codes that carry those arms are exactly the buttons
of screens 4, 5 and 29, and their labels line up -

| code | screen | label id | mode installed |
|---|---|---|---|
| 2, 3 | 4 (main menu) | 37, 39 | 32 |
| 5 | 5 (single player) | 43 | 32 (career / Jail Break) |
| 6 | 5 | 45 | 1 (Five-O) |
| 7 | 5 | 47 | 4 (Time Trial) |
| 27 | 29 (multiplayer) | 75 | 16 |
| 28 | 29 | 77 | 17 |
| 29 | 29 | 79 | 8 |
| 30 | 29 | 81 | 24 |
| 31 | 29 | 83 | 4 |

`frontend.py verify` asserts the whole map equals `rules.md`'s. Modes 8 and 24 are the *Side
Car* modes - code 29 ("Side Car (Co-op)") installs 8 and code 30 ("Side Car (Versus)") installs
24, and both target the `SCAR` screens 36 and 34. The logo tag `SCLG` therefore reads "Side
Car", not "scar". **[established]**

`modecnr` (Cops & Robbers) is entered by **code 28 on screen 29**, which installs mode 17 and
goes to the `COPS` screens 32/33. **[established]**

### 11.2 `0x8007E7BC` - the record test **[established]**

```c
int BeatsRecord(int raceId, u32 value) {      // 0x8007E7BC
    i = raceId - 56;
    return value < *(u32*)(0x80053A88 + 176*i + 0xA8);
}
```

Called from the post-race dispatcher at `0x80080090` and `0x80080128`; a true result sets
`session+0x18 = 1` and sends the player to screen **52** instead of the hub. `0x80053A88` is
the same EXE table the save's records region is seeded from (8.3).

### 11.3 `DATA\FRONTEND.VUK` - the frontend sound bank **[established]**

Loaded as `DATA\FrontEnd.VUK` (`0x8005CA30`) alongside `DATA\FEAlbum.alb` (`0x8005CA44`).
Structure, read straight off the bytes:

```
+0x00  u32  2                 // version
+0x04  u32  0x7F0F0F0F
+0x08  u32  0
+0x0C  u32  0xEFA0 = 61344    // payload size; 61660 - 61344 = 0x13C
+0x10  u32  offset[15]        // 0x4C, 0x5C, ... 0x12C  (stride 16)
+0x4C  15 x 16-byte voice records:
         u8 kind(=1); u8 volume; u16 0x0040; u8 volL; u8 volR; u16 0x80FF;
         u16 adsr1(=0x1FC0); u16 adsr2; u32 sampleOffset
+0x13C 61344 bytes of SPU ADPCM (3834 16-byte blocks; the first is silence)
```

The 15 sample offsets are `0, 9232, 2448, 5120, 7280, 11344, 13040, 19200, 27136, 30016,
33088, 40480, 49696, 58912, 59648`; entries 2..14 ascend, entry 1 is out of order, and all
fifteen are inside the 61344-byte payload. This is the menu's click/confirm/cancel bank; `0x8007EAC0(n)` is the "play UI sound n" call
(`n = 2` on advance, `0x8006AA6C`; `n = 3` on cancel, `0x8006AFBC`). **[established]**
This is a structural reading of the bytes; it has not been cross-checked against the audio
decoder.

---

## 12. Unknown / not established

1. **`screenTable[57]` is never written**, yet `GotoScreen` (`0x800809F8..0x80080A24`) sweeps
   all 59 entries and does `lhu`/`sh` through each pointer. On a PS1 that writes four bytes at
   guest address 0 (kernel RAM). Whether this is harmless in practice was not tested. A port
   must decide deliberately what to do here.
2. **The condition words `widget+0x00` and `widget+0x04`.** The navigation skip test reads
   `+0x00` as a u32 and tests bit `0x40000000` (`0x8006C460`), while the drawing code treats
   the same bytes as colour. Every shipped interactive widget holds `0x00063F7F` or `0`, so the
   two readings cannot be separated from the data alone. Needs an interpreter run.
3. **The movie path mismatch.** `0x8006DBFC` builds `"DATA\<name>.wve"` but the files are in
   `DATA\FE\`. Either `0x8001458C` resolves relative names against the prefix table at
   `EXE 0x80052400` (which lists `DATA\FE\`), or the format string is
   reached with a different base. Not resolved.
4. **Ten of the fourteen pad buttons.** Only indices 0, 2, 3 and 6 were followed to a
   consumer (6.1). The physical mapping (which PlayStation button is which index) goes through
   a remap table and was not traced; `rules.md` 13 lists the same gap for the combat code.
5. **Widget types 1, 2..6, 10, 11, 14, 15, 18, 19.** Their handlers are known by address and
   their instance counts by data, but their field layouts were not decoded. Types 1, 10 and 19
   have **zero instances** in the shipped data.
6. **The type-16 "kind" enum** (`widget+0x18`, observed values 1..33 and 60, 181 instances).
   This is the single biggest remaining gap in the menu description: it is how every dynamic
   string on every screen is selected, and it is not decoded.
7. **The persistent cash balance.** The per-race award is `fe+0x70` (7.5) but nothing was
   found that accumulates it into the session record. `player[k]+0x00` (81690 in the real
   save) is the only candidate and has no located reader or writer.
8. **`session+0xC0..0xD1`** (18 bytes, every one `0x01` in the real save) and
   **`session+0xD4..0xEF`** (seven u32, each `7`). The count 18 matches the `+0xFC` bitmap, so
   a per-mission status array is the obvious reading - unproven, no writer found.
9. **The six-record player array** is `[probable]`: 216/36 = 6, the `for (k < 6)` loop at
   `0x80068710`, and six `Player N` strings. No *indexing* loop bounded by 6 was found.
10. **Screens 10, 13, 14, 17, 18, 23, 28 and 37** are structurally identical and are reached
    from the story movies, but which career event each one belongs to was not established.
11. **The per-screen pre-input jump table at `0x8005B9D4`** (59 arms, `jr` at `0x80066880`)
    and the per-object handler table at `0x8009CC40` were located but not read.
12. **The `u16 flags` of the resource records** - `rules.md` 13 lists the same gap. The
    observed values are 0x0000/0x0100/0x0200/0x0C00/0x1000/0x1100/0x1900.
13. **The records/best-times table layout** at `SLUS 0x80053A88` (1584 bytes). The entries
    look like `{char name[12]; u32 value; u8 f[4]}` after a 16-byte header, but
    `(1584 - 16) / 20` is not an integer, so it is probably several sub-tables. Not resolved;
    the region is zero in the examined save.
14. **Multi-block saves.** The header template hardcodes block number 1 (`0x80080E8F`) while
    `0x8005E790` is written for N blocks. With a 6948-byte payload N is always 1, so the
    generic path is never exercised.
15. **The `%1ld` port argument** of the dead path is `(*(*0x800548CC)) >> 4`; the first word
    of the driver context is presumably a port/slot encoding. Unverified.

---

## 13. Probe outputs

`work\frontend\` (gitignored), written by `python tools\scout\frontend.py emit`:

| file | content |
|---|---|
| `screens.csv` | the 58 screen records with their navigation targets and handlers |
| `widgets.csv` | all 520 widgets: screen, index, type, flags, action code, string id, tag, position |
| `action_codes.csv` | the 81 action codes: target screen, group commit code, mode word, bound object, the screens that use it |
| `graph.txt` | **the screen graph as a plain-text table a human can follow**: every screen, its movie or title, its advance/cancel edges and every button with its target |
| `graph.dot` | the same graph for Graphviz; movies are ellipses, screen 57 a double octagon |
| `resources.csv` | the 37 resource records and the 42-entry name array |
| `prizes.csv` | the prize table as read from `0x80089F44` |
| `screens_full.txt` | the human-readable per-screen widget dump behind section 2.3 |
| `save\` | the extracted save block, the session and player halves, the checksum trace |

`python tools\scout\frontend.py verify` is the bench: **44 checks, 0 failures**. It is
re-runnable and self-contained (disc extract only, no card needed) and runs in the gate suite.
The checks that would catch a real regression rather than a typo are: the screen-table
replay producing exactly ids 0..56 and 58; `screen+0x06 == index` for all 58; every navigation
target resolving to a real screen or to 57; both action-code tables terminating at index 80;
every widget type having a handler; every menu label id being a valid `FESTRING.LOC` id; the
four `.LOC` container identities; the menu-code -> mode map matching `rules.md`; the prize
table's first row and its monotonicity; the seven absent `.psh` records; and the save
checksum reproducing a fresh record's pair in all ten slots.
