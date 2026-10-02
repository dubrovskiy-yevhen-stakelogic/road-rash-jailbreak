# Race rules, game modes and career structure
### (Road Rash: Jailbreak, USA, SLUS_01053)

This document covers the layer that turns a road and a bike into a *game*: the game-mode enum and
where it lives, the race state machine and the per-frame order of business, how a racer's progress
along the route is measured, how the race ends, how positions are computed, what the result codes
mean, how the career series advances, the combat system, and the police/arrest path.

It is the specification a native race loop is meant to be written from, so every section states the
instruction address or file+offset that establishes it. Section 13 is the explicit unknown list.

Prerequisites: `docs\formats\road.md` (the route through the road network), `docs\formats\population.md`
(the entity pools and `LEVEL<n>.BI`), `docs\formats\frontend.md` (the career shell that writes the
session record).

Confidence marks follow `population.md`: **[proven]** = an exact identity over the whole data set, or
read straight out of the dispatching code *and* reproduced against live RAM; **[established]** = read
once out of our disassembly and holding over every sample; **[probable]**; **[guess]**.

Binaries cited (our own SHA-1 over `work\disc_us`):

| file | SHA-1 | base |
|---|---|---|
| `SLUS_010.53` | `67ed165a2c517d4e6106fb0dfa324d66dd9a76f1` | text at `0x80010000`, file offset `f` -> `0x80010000 + f - 0x800` |
| `RASHCDF.BIN` (frontend) | `a3fec4b4e9292c358d0f6dc529843f5d8f25924a` | `0x8005B5E8` |
| `RASHCDG.BIN` (race) | `cfe43a7786759f2cb9c57751cf99e84d1074782c` | `0x8005B5E8` |
| `RASHCDI.BIN` (loader) | `9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06` | `0x8005B5E8` |
| `DATA\FIGHT.BIN` | `15e41265f8b0379d4ca080924ffe5f73a76e97dc` | 4200 bytes |
| `DATA\GAMESTRG.LOC` | `fcae2f23a2456b6a3e6b430711e6b35d58b841e8` | 3292 bytes |
| `DATA\FE\FESTRING.LOC` | `4081f537ff016f27942a124a9137d0f7328079bb` | 89232 bytes |

RAM evidence: `work\oracle\state\{rr-race,rr-pack,quick}\ram.bin` (three distinct savestates of race 4
of road set 1) and the 14 consecutive frames in `work\oracle\vr_capture\ramdumps\`.

Verification: the identities stated "in all three savestates" / "in every frame" were asserted by an
independent Python probe over the RAM evidence above (59 checks, 0 failures; section 12). The ported
functions of sections 15 and 16 are accepted by `rrverify` bench rows.

---

## 0. TL;DR

1. **The mode is a 7-valued bitmask** living in the frontend's persistent session record at guest
   `0x800D80D8`: **1 = Five-O, 4 = Time Trial, 8 = Side Car (Co-op), 16 = Head-to-Head,
   17 = 2P Five-O (Cops & Robbers), 24 = Side Car (Versus), 32 = the career series.** Bit `0x10` means "two players"; bit `0x20` means
   "career". Proved by the 32-arm jump table at `RASHCDF 0x8005C0A4` (index = mode - 1) whose arms
   install the mode's logo FourCC, and by the seven store sites `RASHCDF 0x80068614..0x8006867C`
   that are the only writers of the word.
2. **Jailbreak is not a mode; it is venue 5 of the career mode.** The career runs over six
   *venues* (`session+0x04`, 0..5), each of which picks the environment, the asset bank, and a
   per-venue **race-type byte** that is what `game_state+0x04` actually holds during a race
   (`0x22`, `0x24`, `0x21`, `0x2C`). Venue 5's logo is the Jailbreak logo and it selects
   environment 3 / asset bank 2 (`bblevJBD` / `bblevJBK`).
3. **The race loop is three nested pieces**: `main` (`0x80012224`) drives frames; `GameFrame`
   (`0x80011C4C`) is the whole per-frame job; `RaceStep` (`0x80012524`) is the simulation tick.
   The state machine lives in one byte, `game_state+0x00`, with values 0/1/2/3/4/5/6.
4. **Time base:** one *tick* is 1/300 s; the simulation is advanced with a 16.16-seconds `dt`
   of `218 * ticks`. `game_state+0x10` is the race clock in ticks. The countdown is exactly
   3.00 s.
5. **Progress is one scalar, `entity+0x144` = distance remaining to the finish in 20.12 road
   units (~metres), smaller is ahead.** Computed statelessly every frame by `SLUS 0x8003B61C`
   from `(road, direction, distance-along, current-intersection-record)`. **There is no lap and no
   checkpoint concept** in ordinary races; section 6.7 proves the negative eight ways.
6. **Placing** is `SLUS 0x800138E8`: `place = 1 + #{racers with smaller progress} + #{racers already
   finished}`, stored as a u8 in the runtime rider record at `+0x27`. Values >= 248 are result
   codes, not places. **Proven against all three savestates and all 14 frame dumps: the places of
   the live racers are always an exact permutation of 1..n, and the two police bikes always get
   `numRacers + 1`.**
7. **`[START_CHECKER]` / `[FINISH_CHECKER]` are not race logic.** They are a `(scene-cell resource
   id, primitive-group index, quad index)` triple that names one quad of the streamed world - the
   finish banner - whose texture is swapped at draw time. Only three code sites read them and none
   of them is in the finish, progress or AI path.
8. **The frontend owns the career.** The race overlay writes the result into the runtime rider
   record; the frontend reads the per-player result code at `0x800D81F8`, picks the result screen,
   and updates the session record - including a **65-bit race bitmap at `session+0xF0`** and an
   18-bit map at `session+0xFC`. No overlay other than `RASHCDF` writes the session record.

---

## 1. The three records the rules layer lives in

### 1.1 `game_state` - guest `0x800D5D38`, pointer at `0x8005B2F8` **[proven]**

The pointer is written once, at `EXE 0x80011748`, from the literal built at `0x8001173C`
(`lui v0,0x800d; addiu v0,v0,23864`). The same function `0x80011738` is the struct's initialiser and
its literals are readable directly:

| off | init | set at | meaning |
|---|---|---|---|
| +0x00 | - | | `s8` **main state**: 0 exit, 1 race running, 2 frontend, 3 restart, 4 paused, 5 post-race, 6 quit-to-results |
| +0x01 | - | `0x80011CC8` | state saved across a pause |
| +0x02 | - | `0x8001244C` | restart request; `main` turns it into state 3 |
| +0x03 | 1 | `0x8001174C`, `RASHCDI 0x80063BA4` | "this is the first frame of a race"; `main` consumes and clears it at `0x80012408` |
| +0x04 | 34 | `0x80011754` | `u8` **race-type byte** (section 2.3) |
| +0x05 | 3 | `0x80011770` | option bits copied from `session+0x05` (`RASHCDF 0x8007F51C`/`0x8007F568`) |
| +0x06 | 1 | `0x800117B0` | `f(raceId)` (section 2.5); also read by the arrest test at `RASHCDG 0x80096FE4` |
| +0x07 | 3 | `0x80011774` | per-venue byte from `RASHCDF` table `0x80099530` |
| +0x08 | 120 | `0x800117B8` | `u16` per-venue value from `RASHCDF` table `0x80099500`, overridden by some venue arms |
| +0x0C | 0 | `0x8001177C` | `u32` frame clock in ticks, `+= 5` per video callback (`0x8001B724`) |
| +0x10 | 0 | `0x8001B6C0` | `u32` **race clock in ticks** (section 4) |
| +0x14 | | `0x8001B71C` area | frame-clock snapshot |
| +0x18/+0x1C/+0x20 | | | frame delta in ticks; `+0x1C` is the delta clamped to 30 (`0x8001255C`), `+0x20` is recomputed at `0x8001C488` |
| +0x28/+0x2A/+0x2C | 0 | `0x80011780`.. | pause request / ack / clock-at-pause |
| +0x30 | 1 | `0x8001179C` | `u32` **number of players** (1 or 2) |
| +0x34 | | `RASHCDF 0x80080560` | number of pad-polled players |
| +0x38 | 0 | `0x80011798` | |
| +0x3A | 0 | `0x8001178C` | `u16` environment id 0..3 |
| +0x3C | 0 | `0x80011790` | `u32` **asset bank / "level"** 0..2 -> `LEVEL<n+1>.BI` |
| +0x40 | 4 | `0x80011760` | `u32` **race id** |
| +0x44 | 0 | `0x80011794` | from `session+0x11` |
| +0x48 / +0x4C | - / 9 | `0x80011768` | `u32` player 1 / player 2 **bike index** 0..20 |
| +0x64 | | `0x8001B734` | video callback counter, reset every 60 at `0x8001C450` |

**`game_state+0x30` is the player count**, although the `.STP` name builder uses it as the `%d` that
looks like a road-set digit: `GameFrame` loops `for (p = 0; p < *(gs+0x30); p++)` over the
player-entity pointer array at `0x8005B268` (`0x80011E50`, `0x8001207C`), `RaceStep` does the same
(`0x80012614`), and the pad poller uses `+0x34` for the same purpose (`0x8001CB94`). In all three
savestates it is 1, and the loaded track is `RACE1_4.STP`; that the digit also happens to select the
road set in the observed capture is a coincidence of a one-player game. **Which field really
selects the road set is open** - see section 13.

### 1.2 The session / career record - guest `0x800D80D8` **[proven]**

Lives above the overlay window (`0x800CD670`), so it survives overlay switches. **It is written only
by `RASHCDF`** - a store scan over all four images finds no writer in the resident EXE, `RASHCDG` or
`RASHCDI`; the resident EXE only *clears* its two bitmaps.

| off | type | meaning | evidence |
|---|---|---|---|
| +0x00 | u32 | **the game mode** (section 2) | seven `sw` sites `RASHCDF 0x80068614..0x8006867C` |
| +0x04 | s8 | **venue** 0..5 | `RASHCDF 0x8007F790`, `0x8006EB94`, `0x8007F680` |
| +0x05 | u8 | option bits (init 0x30 at `0x800686D0`); bit `0x10` -> `game_state+0x05` bit 0, bit `0x20` -> bit 1 | `0x8007F518`, `0x8007F54C` |
| +0x06 | s8 | -> `game_state+0x30` (player count) | `0x8007FD44` |
| +0x08 | s8 | **race id** -> `game_state+0x40` | `0x8007F4F8`, `0x8007FD50` |
| +0x09..+0x0B | u8 | progress counters | `0x80063454`, `0x800634AC`, `0x800636B0` |
| +0x11 | u8 | -> `game_state+0x44` | `0x8007F504` |
| +0x26 | u16 | a running counter, bumped on one result path | `0x8007E3C4` |
| +0x40..+0xBF | 16 x 8 | **the 16 AI opponents' identities**, `{u16 nameId; u8 b; u8 c; u32 d}` | copied into rider records 2..17 at `0x8007F750..0x8007F788` and again at `RASHCDI 0x80063FF0..0x80064028` |
| +0xF0 | 9 bytes | **65-bit bitmap**, one bit per race of road set 1 | cleared bit-by-bit for `i < 65` at `EXE 0x8002D250..0x8002D28C`; read by `RASHCDF 0x80062E18` and 9 more sites |
| +0xFC | 3 bytes | **18-bit bitmap** | cleared for `i < 18` at `EXE 0x8002D298..0x8002D2D4` |
| +0x100 | 6 x 0x24 | the six **player records** (1.3) | `0x800D81D8`, stride 36 |

The 65-bit bitmap is the only structure in the game sized to the 64 races of road set 1
(`ROADGRF1.TXT` declares `[NUM_ENTRIES]=64`), and its setter uses `session+0x08`, the race id, so it
reads "race completed" **[established]**. The setters and clearers are in `frontend.md` 7.3.

### 1.3 The per-player record - `0x800D81D8`, stride 0x24 **[established]**

There are **six** records: the frontend initialiser loops `for (k = 0; k < 6; k++)` at
`RASHCDF 0x800686F4..0x80068718`, there are six `Player N` strings at `0x8005B98C`, and the save
copies **216 bytes = 6 x 0x24** from `0x800D81D8` (`a2 = 216` at `0x8006CDB8`; `frontend.md` 7.2, 8.4).

| off | meaning | evidence |
|---|---|---|
| +0x06 | a second bike slot, initialised to 127 = "none" | `RASHCDF 0x800686C4`, `0x800686C8` |
| +0x07 | **bike index 0..20** -> `game_state+0x48` (p0) / `+0x4C` (p1) | `0x8007F59C`, `0x8007F5A8` |
| +0x09 | gang id (0 or non-zero) - picks which of two gang result screens plays | `RASHCDF 0x8007D990`, `0x8007DA7C` |
| +0x0C..+0x13 | the player's rider identity, copied into rider record `p` | `0x8007F72C..0x8007F74C` |
| +0x20 | **the race result code** the frontend dispatches on | `RASHCDF 0x8007BCD8`, `0x8007DE88`, `0x8007E358` |

**The bike index is proven.** The frontend resource-name array at `RASHCDF 0x8008973C` entries 21..41
is exactly the 21 bike identities in order (`cruisea1..3`, `cruiseb1..3`, `cruises1..3`,
`sporta1..3`, `sportb1..3`, `sports1..3`, `cop1..3`), and this list is exactly the set of 484-byte
`.PH` files on the disc. In all three savestates `game_state+0x48 = 0`
(= `cruisea1`), and `population.md` 5.3 independently found that **`CRUISEA1.PH` is the only bike
`.PH` resident in those images**.

### 1.4 The runtime rider record - `0x800D5758`, stride 0x48 **[proven]**

Twenty records are used: **0 = player 1, 1 = player 2, 2..17 = the 16 AI racers, 18 and 19 = the two
police bikes.** Built from `LEVEL<n>.BI` by `RASHCDI` (`0x80065110`, `0x80065174`, `0x800652EC`,
`0x80065DE4`, `0x80067180`); the identity fields are then overwritten from the session record by
`RASHCDF 0x8007F714` and by `RASHCDI 0x80063FB8`. An entity reaches its record through
`entity + 0x43C`.

| off | type | meaning | evidence |
|---|---|---|---|
| +0x00 | u8 | flags; bit `0x40` = "still frozen on the grid", bit `0x20` = "arm the countdown", bit `0x80` gates the wrong-way test | `RASHCDG 0x8008AD50`, `0x8008AD90`, `SLUS 0x8003C5DC` |
| +0x01 | u8 | high nibble **rank** 1..8, low nibble **bike class** 0/1/2 (2 = police) - the same byte as `LEVEL<n>.BI +0x05` | `RASHCDG 0x80096880`, `SLUS 0x8003191C`, `RASHCDG 0x800B9368` |
| +0x08 | s32 | reach bonus in the hit test | `RASHCDG 0x800C1730` |
| +0x0C | s32 | strike-strength multiplier | `RASHCDG 0x800C1948` |
| +0x0E | u8 | slow "bike" damage bar | `RASHCDG 0x800C1A64` |
| +0x0F | u8 | **rider health; 0 = knocked off** | `RASHCDG 0x800C197C`, `0x800C19C0` |
| +0x24/+0x25 | u8 | health regeneration counter and value | `RASHCDG 0x800C1BC8..0x800C1C10` |
| +0x26 | u8 | **rider name id** - `GAMESTRG.LOC` ids 34..91 are the rider names, and the 16 AI records carry 34..49 consecutively, the two police 69 and 71 **[probable]** | measured in all three savestates |
| +0x27 | u8 | **current place, 1-based; >= 248 is a result code** (section 7.3) | `SLUS 0x800138E8`, `RASHCDG 0x80092CCC`, `0x80097008` |
| +0x28 | u32 | **finish / elimination timestamp in ticks; 0 = still racing** | `RASHCDG 0x80092CE8` (`= game_state+0x10`), read at `SLUS 0x8001399C` |
| +0x2C | u16 | `.LOC` string id of the display name, from the session | `RASHCDF 0x8007F740` |
| +0x2C | u16 | (same word) **weapon-possession bitmask**, bits 0..8 | `RASHCDG 0x800B935C`, `0x800C22D8` |
| +0x2E | u8 | **current weapon 0..8; 9 = fists** | `RASHCDG 0x800BFA5C`, `0x800C00EC` |
| +0x2F | u8 | swings left on the current weapon | `RASHCDG 0x800C08B4` |
| +0x30 | u32 | 8 nibbles - per-weapon swing counters | `RASHCDG 0x800C08D8` |
| +0x34..+0x3B | u8 | the AI's move repertoire | `RASHCDG 0x800B9108`, `0x800B9318` |
| +0x3C | u8 | the current combat command | `RASHCDG 0x800BF990` |
| +0x3D | u8 | low nibble hits in the current combo, high nibble the limit | `RASHCDG 0x800C1820` |
| +0x44 | u8 | HUD flags; bit `0x20` = flash the health bar; bit 0 is set at each quarter-mile | `RASHCDG 0x800C1A48`, `SLUS 0x8003B568` |

**Note the collision at +0x2C**: the frontend writes a `.LOC` name id there
(`RASHCDF 0x8007F740`, `sh v1,44(t0)`) and the combat code reads a weapon bitmask from the same
halfword (`RASHCDG 0x800B935C`). Both readings are backed by code. In the savestates the values are
`0x0601`, `0x0621`, `0x0604`, `0x0600`, `0x0784`, `0x0780`; masking `& 0x1FF` (which the combat code
does) gives plausible weapon sets, and the high bits 9/10 are set on every record, which is what a
string-id base of `0x0600` looks like. **Most likely the frontend and the combat code are two views
of the same halfword and the "name id" reading is wrong** - but the `sh` at `0x8007F740` is
unambiguous, so this is flagged as an open contradiction in section 13.

**Race position is proven on the oracle.** For all three savestates and all 14 frame dumps,
`riderDef+0x27` over the live racers is an exact permutation of `1..n`
(n = 16 in these captures) and that the two class-2 records get `n_all + 1 = 19`. That is the
strongest single result in this document.

---

## 2. The mode set

### 2.1 Where the mode is stored and who can write it **[proven]**

`*(u32*)0x800D80D8`. A store scan over all four images finds **exactly seven writers**, all inside
one dispatcher:

```
RASHCDF 0x800685BC   SetMode(MenuItem *item)
  0x800685C4  if (*(s16*)(item+0x08) < 12 || >= 14) return;     // widget type gate
  0x800685E0  code = *(u16*)(item+0x12);
  0x800685E8  if ((unsigned)(code - 2) >= 30) return;
  0x800685FC  jump table at 0x8005BC64, index = code - 2
```

Arms and the constants they store:

| arm | mode |
|---|---|
| `0x80068614` | 32 |
| `0x80068624` | 1 |
| `0x80068634` | 4 |
| `0x80068644` | 16 |
| `0x80068654` | 17 |
| `0x80068664` | 8 |
| `0x80068674` | 24 |

Menu action code -> mode (confirmed from the widget data in `frontend.md` 11.1):
codes 2, 3, 5 -> 32; 6 -> 1; 7 -> 4; 27 -> 16; 28 -> 17; 29 -> 8; 30 -> 24; 31 -> 4. Every other code
in 2..31 falls through to the no-op arm `0x80068680`.

### 2.2 The enum, named by its own logo **[proven]**

`RASHCDF 0x8006EB5C` dispatches on the mode to install a four-character logo asset id:

```
0x8006EB5C  v = *(u32*)0x800D80D8;
0x8006EB68  if ((unsigned)(v - 1) >= 32) goto default;
0x8006EB74  jump table at 0x8005C0A4, index = v - 1
```

Only seven arms are reachable, and the other 25 land on the no-logo default `0x8006EC3C`:

| mode | hex | logo | reading |
|---|---|---|---|
| 1 | 0x01 | `5OLG` | **Five-O** (`modefivo.str`) |
| 4 | 0x04 | `TTLG` | **Time Trial** (`modett.str`) |
| 8 | 0x08 | `SCLG` | **Side Car (Co-op)** - `modescar.str`; menu code 29 (`frontend.md` 11.1) |
| 16 | 0x10 | `HHLG` | **Head to Head**, two players (`modeh2h.str`) |
| 17 | 0x11 | `5OLG` | Head-to-Head Five-O = **Cops & Robbers**, menu code 28 (`modeh2hs.str` **[probable]**) |
| 24 | 0x18 | `SCLG` | **Side Car (Versus)**, menu code 30 |
| 32 | 0x20 | (defers) | **the career series**; arm `0x8006EB90` dispatches on the *venue* instead |

The bit reading follows directly: **bit `0x10` = two players**, **bit `0x20` = career**, and bits
`0x01` / `0x04` / `0x08` select the single-player race type. `mode|0x10` keeps the base mode's logo
(1/17 and 8/24 agree) and mode 32's arm is the venue dispatcher.

Two more logos exist in the same function but are **not** mode-driven: `RRLG` / `CNLG` at
`0x8006EA78` (chosen by `0x80072100(widget) == 3`) and `TYLG` at `0x8006EB4C` (`== 2`). `modecnr.str`
("cops and robbers") therefore exists as a *screen* and has its own result path
(`RASHCDF 0x8007E340`, section 7.4), but **it is not a value of this mode word**: the menu enters it
with code 28, which installs mode 17 (`frontend.md` 11.1). **[established]**

The mode-to-`mode*.str` screen mapping does not go through the mode word either: the index is
`widget+0x14` of a type-8 (logo) widget, in the range 48..53, dispatched through the 6-arm table at
`RASHCDF 0x8005C08C` from `0x8006E958..0x8006E970` (`frontend.md` 4.1).

The independent confirmation that `0x10` means two players: `population.md` 3.3 found that
`RASHCDI 0x8006903C` allocates a 32-slot collision cache when `game_state+0x04 & 0x10` is set and 24
otherwise, and `(game_state+0x30 >= 2) == !!(game_state+0x04 & 0x10)` holds in every savestate.

### 2.3 Career venues, and why `game_state+0x04` is not the mode word **[proven]**

`RASHCDF 0x8007F37C` is the "commit the menu selection into `game_state`" function (prologue at
`0x8007F384`, `jr ra` at `0x8007FB80`). It dispatches on `session+0x04` twice.

*First*, at `0x8007F45C`, through the 6-entry table at `0x8005CAB8`, to set the environment and the
asset bank:

*Second*, at `0x8007F798`, through the 16-entry table at `0x8005CB50`, to set **`game_state+0x04`**
- and the values it installs are **not** mode values:

| venue | logo (table `0x8005C124`) | `game_state+0x04` | `+0x3A` env | `+0x3C` bank | `+0x07` | `+0x08` |
|---|---|---|---|---|---|---|
| 0 | `JBLG` | 0x22 | 0 | 0 | 4 | 180 |
| 1 | `GTLP` | 0x24 | 0 | 0 | 4 | 140 (overridden at `0x8007F7E8`) |
| 2 | `JBLG` | 0x22 | 1 | 1 | 5 | 255 |
| 3 | `DTLP` | 0x21 | 1 | 1 | 5 | 255 |
| 4 | `JBLG` | 0x22 | 2 | 2 | 6 | 335 |
| 5 | `JBLP` | 0x2C | 3 | 2 | 6 | 335 |

Also on the non-career path, `0x8007F4E4` copies the low byte of the mode word straight into
`game_state+0x04`. So:

> **`game_state+0x04` is the *race type* byte. For modes 1..24 it is the mode's low byte; for the
> career mode it is the venue's race-type byte (0x21, 0x22, 0x24 or 0x2C).** Every piece of race
> code that "tests the mode" is really testing this byte.

The savestates settle it: `session+0x00 = 32` (career), `session+0x04 = 0` (venue 0), and
`game_state+0x04 = 0x22` - exactly venue 0's arm, not the mode word. The identity holds in all three.

**Venue 5 is Jailbreak** (logo `JBLP`, environment 3, asset bank 2 = `bblevJBD`/`bblevJBK`), which is
the same pairing `RASHCDI`'s level-bank names imply and which explains `DATA\STARTJBA.BIN`
(`population.md` 7) being a six-rider grid in a different layout. Venue 3 is `DTLP` with race type
`0x21` - the only race type with bit 0 set, and therefore **the only one that arms the race time
limit** (section 4.3) and the one whose arrest outcome counts as a *success* (section 8).

### 2.4 What the race code actually branches on

Every consumer of `game_state+0x04` in `RASHCDG`, `RASHCDI` and the resident EXE was enumerated
(a mask scan over all four images; 130 sites). The masks used are `0x01`, `0x04`, `0x08`,
`0x10`, `0x20`, `0x22`, and `0x18` as a word mask. The two structural idioms:

**(a) the difficulty-variant row.** Repeated verbatim at `RASHCDG 0x8008AB8C`, `0x8009602C`,
`0x8009D1EC` and elsewhere:

```c
mode  = game_state->raceType;     // +0x04
level = game_state->bank;         // +0x3C, 0..2
row   = (mode & 0x04) ? 2 : ((mode & 0x01) ? 1 : 0);
value = table[level + 3*row];
```

Known tables:

| table | variant 0 | variant 1 | variant 2 |
|---|---|---|---|
| `0x80052FAC` (read by `0x8008ABB8`, and by the traffic scheduler per `population.md` 3.2) | 10.0 s, 30.0 s, 32.0 s | 10.0 s x3 | 10.0 s x3 |
| `0x80052FD0` (read by `0x80096058`) | 10.0, 24.0, 30.0 s | same | same |

**(b) whole-value comparisons.** `GameFrame` tests `game_state+0x04 == 24` at `0x80011EEC`;
`RASHCDG 0x800B9AE0` tests `== 44 (0x2C)`; the arrest path tests `== 33 (0x21)` at `0x80096FF0`;
`RASHCDI 0x80063BB8` computes `*(0x8005B254) = ((*(u32*)(gs+0x04) & 0x18) == 0)`, i.e. "neither
two-player nor 'SC'".

Consequences for a native port, with addresses:

| race type / bit | what it changes | where |
|---|---|---|
| bit `0x01` | selects difficulty row 1 for the tables above; **arms the race time limit**; gates a post-race global at `0x8001215C` | `RASHCDG 0x8008AB9C`, `RASHCDI 0x800636B8`, `EXE 0x80012150` |
| bit `0x04` | selects difficulty row 2; gates the HUD radar | `RASHCDG 0x8008AB94`, `0x800B9AD0`, `0x80061F6C` |
| bit `0x08` | gates a per-player behaviour block in `GameFrame`'s neighbour | `RASHCDG 0x80060E2C` |
| bit `0x10` (2P) | collision-cache capacity 32 vs 24; the split-screen viewport and draw-flag toggling; the 2P time-limit table; the 2P HUD `.CSV` | `RASHCDI 0x8006903C`, `EXE 0x80011E88..0x80011FA4`, `RASHCDI 0x800636C8`, `RASHCDI 0x8005FAB4` |
| bit `0x20` (career) | 12 sites in `RASHCDG` + 6 in `RASHCDI` take a distinct arm | `0x80064C84`, `0x80066E68`, `0x8006781C`, `0x80068AE8`, ... |
| `== 0x21` | an arrest is a **success** for the player, not a bust | `RASHCDG 0x80096FF4..0x80097008` |
| `== 0x2C` | the hard-coded five-entry checkpoint table (section 6.5) and two race-clock thresholds | `RASHCDG 0x800B9AE0`, `SLUS 0x80053174` |

### 2.5 `game_state+0x06 = f(race id)` **[established]**

`RASHCDF 0x8007F2A8`: `v = raceId - 38; if (v < 18) a1 = table[0x8005CA70 + 4v]`, otherwise 0. The
18 arms give the repeating pattern `{9, 10, 3, 4, 1, 1}` for race ids 38..43, 44..49 and 50..55,
i.e. exactly the ids that `ROADGRF2.TXT` declares. The value is stored to `game_state+0x06` at
`0x8007F678`. The **arrest test at `RASHCDG 0x80096FE4` compares a bike's handle against this byte**,
so it names a specific rider slot in the police-side modes. Its meaning in ordinary races is
unknown (section 13).

---

## 3. The race state machine and the per-frame order

### 3.1 The outer loop - `main` at `0x80012224` **[established, fully disassembled]**

```
0x80012234  init: 0x800403CC, 0x800140E8, 0x8001444C, 0x800117BC (game_state init), 0x8002E080
0x8001225C  if (state == 2) { load_overlay(16 = F); 0x8007FEDC(); }        boot into the frontend
0x800122A4  if (state == 0) goto exit
outer:                                                                      0x800122B4
0x800122B4    if (state == 2) { load_overlay(16 = F); 0x8001B868(); 0x8007FF4C(); }   FRONTEND
0x800122E4    load_overlay(2 = I); 0x80064610(); 0x80063B90();              LOAD LEVEL  (sets +0x03)
0x80012304    load_overlay(4 = G); 0x8001F080();                            ENTER RACE (0x8001F080: sound/SPU)
0x8001231C    if (game_state+0x30 == 1) { 0x800247A0(); 0x80020E30(0); }
0x80012348    0x800119C0();  0x8001B868();
frame:                                                                      0x80012360
0x80012360      if (state == 5) goto post
0x80012370      0x8001CB3C();                                              PAD POLL (may set state)
0x8001238C      if (game_state+0x03) {                                     FIRST RACE FRAME ONLY
0x80012394          *(u32*)0x8005B228 = 0x00050000;
0x8001239C          *(u16*)0x800CD542 = 0; *(u16*)0x800CD540 = 0;
0x800123AC          0x8008AB00(1094);                                      race clock, dt = 1/60 s
0x800123B8          0x800881B4(0x800CD898, 1094);                          view/camera, player 0
0x800123E0          if (game_state+0x30 >= 2) 0x800881B4(0x800CDD04, 1094);
0x800123E8          0x8008CFDC();
0x800123F4          0x8005E1D8(0x800CE170);
0x80012408          game_state+0x03 = 0;
                } else 0x8001C428();                                       frame-delta bookkeeping
0x80012414      0x80011C4C();                                              GAME FRAME
0x8001241C      if (game_state+0x02) { state = 3; +0x02 = 0; 0x80018C1C(1); 0x80020E30(1); }
0x80012464      if (state == 0) goto post
0x80012474      if (state != 2) goto frame
post:                                                                       0x8001247C
0x8001247C    0x8001C408(); 0x80022FC0(); load_overlay(2 = I); 0x80063A20();
0x800124B0    if (state == 5) { 0x80063FA0(); if (state still 5) goto 0x800122FC; }   NEXT RACE
0x800124E4    if (state != 0) goto outer
exit:       0x80012500  0x800121E4(); return 0
```

Two things this settles:

* **`game_state+0x03` is a one-shot.** Its only setter is `RASHCDI 0x80063BA4`, inside the
  level-load entry point `0x80063B90` which runs once per race; `main` clears it at `0x80012408`.
  The block it gates is therefore the **first simulation frame of the race**, run with a fixed
  `dt = 1094` (= 1/60 s in 16.16 seconds).
* **The "next race in the series" shortcut.** After a race ends with state 5, `RASHCDI 0x80063FA0`
  re-copies the 18 rider identities out of the session record **only when `session+0x00 == 32`**
  (`0x80063FA8`: `li v0,32; bne v1,v0`), and `main` jumps back to `0x800122FC` without going through
  the frontend. That is the career series loop. **[proven]**

### 3.2 The per-frame job - `GameFrame` at `0x80011C4C`..`0x800121DC` **[established]**

In call order:

| # | at | call | what |
|---|---|---|---|
| 1 | `0x80011C68` | `0x8002305C` | |
| 2 | `0x80011C7C` | pause handling | `game_state+0x28` request -> state 4, `+0x01` saves the state; `+0x2A`/`+0x2C` on resume (`0x80011CB8..0x80011D80`) |
| 3 | `0x80011DA4` | **`0x80012524` = `RaceStep`** | only when `state == 1` |
| 4 | `0x80011DAC` | `RASHCDG 0x8008CFDC` | only when `state == 1` |
| 5 | `0x80011DBC` | `0x80018E54(0x800CE170)` | |
| 6 | `0x80011DF4..0x80011E34` | `0x80043E24`, `RASHCDG 0x8005E1D8`, `0x8001264C` | scratchpad `0x1F8003E4` is saved into `game_state+0x24` across the call |
| 7 | `0x80011E3C` | `RASHCDG 0x800C89A0` | |
| 8 | `0x80011E78` | **per-player loop** `for (p = 0; p < game_state+0x30; p++)` | body below |
| 9 | `0x80012094` | `0x8004D184(192, 120)` | libgte screen offset |
| 10 | `0x8001209C..0x80012124` | `0x8001E084`, `0x8004CE14`, `0x8001C304`, **`RASHCDI 0x8005E848` = the HUD**, `0x8002CA5C` | |
| 11 | `0x8001212C` | if `state == 6`: `if (raceType & 1 && *(0x8005AD48)) *(0x8005AD48) = 0; RASHCDG 0x800C5918()` | |
| 12 | `0x80012188` | else if `state < 2`: `0x8002D2F4()` | the confirm dialog |
| 13 | `0x800121A8` | `0x80048DB4(otag)` | libgpu draw |
| 14 | `0x800121B0` | `0x8001C3F4()`, `0x80018FAC()` | |

Per-player loop body (`0x80011E78..0x8001208C`), with `pe = *(0x8005B268 + 4p)` and
`rd = 0x800D5758 + 72p`:

```
if (game_state+0x30 == 2 && game_state+0x04 != 24)     0x80011E80, 0x80011EEC
     set the split-screen viewport from *(0x8005B474)+8p and toggle bit 11 of
     entity+0x24 on both players' bikes and riders                 0x80011EF4..0x80011FA4
0x80011FA8  0x8002F2E8(p)
0x80011FB0  RASHCDG 0x800674C8()
0x80011FBC  if (pe->handle < game_state+0x30 && rd[0x27] == 255) 0x80027778(?, 3, 600, 0)
0x80011FF8  RASHCDG 0x8008D56C(p)
0x80012000  0x800358C0(p)
0x80012008  RASHCDG 0x800C8B24(p)
0x80012010  if (*(0x8005B314)) RASHCDG 0x800A2138(p)
0x8001202C  0x80035958(p)
0x80012034  RASHCDG 0x800C8CD4(p)
0x8001203C  if (*(u8*)(0x800D8060 + p)) 0x8002C928(p, *(0x800CD660))
```

### 3.3 The simulation tick - `RaceStep` at `0x80012524` **[established]**

```c
void RaceStep(void) {
    gs = *(0x8005B2F8);
    ticks = gs->f18;                       // +0x18, frame delta in ticks
    if (ticks == 0) { gs->f1C = 0; return; }        // 0x80012548
    if (ticks >= 31) ticks = 30;                    // 0x8001254C..0x80012558
    gs->f1C = ticks;                                // 0x8001255C
    dt = 218 * ticks;                               // 0x80012564..0x80012574
    *(u32*)0x8005B580 = 0;                          // 0x80012584
    gs->raceClock += ticks;                         // +0x10, 0x80012588/0x80012590
    RASHCDG_RaceClock(dt);                          // 0x8008AB00
    for (p = 0; p < gs->numPlayers; p++) {          // 0x800125A0..0x80012624
        view = 0x800CD898 + 1132*p;
        view->f1D4 = view->fB8; view->f1D8 = view->fBC; view->f1DC = view->fC0;
        RASHCDG_ViewUpdate(view, dt);               // 0x800881B4
        if (view->f224 & 0x100 && RASHCDG 0x800A421C(view)) RASHCDG 0x80086E1C(view);
    }
}
```

`218 * ticks` is built as `((t<<3 - t) << 5 - (t<<3 - t)) + t = 217t + t`, i.e. the shift chain at
`0x80012564..0x80012574`.

### 3.4 The countdown, and what "GO" means - `RASHCDG 0x8008AD38` **[established]**

Called from `0x8008AB34` with the same `dt`, and its return value gates everything else in the race
clock function.

```c
int TickCountdown(int dt) {
    p0 = *(0x8005B38C);  rd0 = p0->riderDef;                 // 0x8008AD3C, 0x8008AD48
    if (!(rd0->flags0 & 0x40)) return 1;                     // 0x8008AD58  already racing
    p1 = *(0x8005B21C);                                      // second player, 0 if 1P
    p0->f2D0 = 0xFFFF0000;  if (p1) p1->f2D0 = 0xFFFF0000;   // 0x8008AD70/74
    if (rd0->flags0 & 0x20) {                                // 0x8008AD90  arm it
        game_state->raceClock = 0;                           // 0x8008ADA4
        *(s32*)0x8005B230 = 0x00030000;                      // 0x8008ADB0  = 3.00 s
        rd0->flags0 &= ~0x20;  if (p1) p1->riderDef->flags0 &= ~0x20;
        return 0;
    }
    *(s32*)0x8005B230 -= dt;                                 // 0x8008ADFC/0x8008AE04
    if (*(s32*)0x8005B230 < 0) {                             // 0x8008AE00
        rd0->flags0 &= ~0x40;  p0->f2D0 = 0;                 // 0x8008AE18, 0x8008AE30
        if (p1) { p1->riderDef->flags0 &= ~0x40; p1->f2D0 = 0; }
        game_state->raceClock = 0;                           // 0x8008AE6C
        *(u32*)0x8005B30C = 0;                               // 0x8008AE64
        RASHCDG 0x80090270();                                // the "GO" event
        return 1;
    }
    return 0;
}
```

So the sequence is: **`riderDef+0x00` bit `0x40` freezes a rider on the grid; bit `0x20` arms a 3.00
second countdown; when the countdown expires both bits are clear, the race clock is reset to 0 and
`0x80090270` fires.** The HUD's countdown digit element reads the same timer
(`RASHCDG 0x8005FF84` -> element 52 `kDashCountDownDigit`).

### 3.5 State transitions **[established]**

All writers of `game_state+0x00` (a store scan over all four images finds exactly these):

| at | new state | condition |
|---|---|---|
| `0x800117DC` | 2 | boot: go to the frontend |
| `0x80011CA0` | 2 | pause menu asked to quit to the frontend (`0x80011C90` tests `*(0x8005B220)`) |
| `0x80011CD8` | 4 | **pause**; `+0x01` saves the previous state |
| `0x80011D2C` | 3 | resume from pause when the saved state was 1 |
| `0x80012438` | 3 | `main` consumes `game_state+0x02` (a restart request) |
| `0x8001CC30` | 3 | pad poll, when `*(gp+0x7EC)` is set and the state was 1 |
| `0x8001CCB0` | 3 | pad poll, per-player path |
| `0x8001CD14` | 1 | pad poll, when `game_state+0x03` is set - this is what starts the race |
| `0x8001CDF0` | 2 | pad poll, abandon |
| `0x8002D4B4` | **6** | the quit-confirm dialog was accepted; also sets `0x8005AF58`/`0x8005AF5C` = `0x00101010` |
| `0x8002D524` | **5** | the dialog resolved into "go to the results" |
| `0x8002DC98` | 1 | dialog dismissed, resume racing |
| `0x8002DCF4` | (arg) | dialog dismissed, restore |
| `RASHCDF 0x8006C398` | 3 | frontend restart |

State 6 additionally normalises both players' places before leaving (`0x8002D478..0x8002D494`):
if the race type has bit `0x04` or bits `0x22`, the player's `riderDef+0x27` is forced to 255; and at
`0x8002D454` any place greater than `*(0x8005B1FC)` (the racer count) is replaced by 255.

---

## 4. Time

### 4.1 The two clocks **[established, with an independent anchor]**

* **tick** - the unit of `game_state+0x0C` (frame clock) and `game_state+0x10` (race clock). The
  video callback `0x8001B700` adds **5 per call** (`0x8001B724`); `RaceStep` adds the frame's tick
  delta to the race clock (`0x80012588`).
* **`dt`** - 16.16 *seconds*, the unit the race clock function and the countdown work in.
  `RaceStep` computes `dt = 218 * ticks`; the first race frame uses the literal 1094.

The tick rate is pinned by an independent site: `RASHCDI 0x800636F0` builds the race time limit as
`secondsTable[level] * 300`, and the HUD clock (`RASHCDG 0x80062C40`) divides the elapsed tick count
by 30 to get tenths. **1 tick = 1/300 s.** That makes `218/65536 s = 1/300.6 s` (the engine rounds
`65536/300 = 218.45` down to 218) and `1094 ~= 65536/60`, i.e. the first-frame `dt` is one 60 Hz
frame. Consistent both ways.

Observed frame deltas: `game_state+0x18 = 5` in `rr-race` (60 fps) and `10` in `rr-pack` and `quick`
(30 fps). The engine is variable-rate and clamps the delta to 30 ticks (100 ms).

Race clock in the captures: `rr-race` 3420 ticks = 11.4 s, `rr-pack` 8285 = 27.6 s, `quick` 2110 =
7.0 s; across the 14 frame dumps it runs 9285 -> 10590 ticks = 4.34 s and is strictly monotone.

### 4.2 The race-clock function - `RASHCDG 0x8008AB00(dt)` **[established]**

```c
void RaceClock(int dt) {
    *(u32*)0x8005B30C += dt;                        // 0x8008AB14/0x8008AB30
    if (*(u32*)0x8005B2A8) goto other;              // 0x8008AB2C
    if (!TickCountdown(dt)) return;                 // 0x8008AB34/0x8008AB3C
    acc = *(u32*)0x8005B30C;
    if (acc > 0x8000 && acc - dt > 0x8000) { 0x800B8018(0); return; }   // 0x8008AB44..0x8008AB6C
    limit = table_80052FAC[level + 3*variant];      // section 2.4(a)
    if (limit < acc) { ... *(u32*)0x8005B30C = dt; }                    // 0x8008ABC8, 0x8008ABDC
}
```

`0x8005B30C` is therefore a **periodic event accumulator**, not the race clock: it is reset to `dt`
each time it passes the per-(variant, level) interval, and separately fires `0x800B8018(0)` once
every half second (`0x8000` in 16.16 seconds). It is zeroed at "GO" (`0x8008AE64`) and by
`RASHCDI 0x80069A8C`.

### 4.3 The race time limit **[established]**

Set up at `RASHCDI 0x80063670`: `*(0x8005ACD0) = *(0x8005ACCC) = *(0x8005ACC8) = 0`, and then
**only if `game_state+0x04 & 1`**:

```
table = (game_state+0x04 & 0x10) ? 0x80053090 : 0x80053084;     // 0x800636C8
*(0x8005ACC8) = table[game_state+0x3C] * 300;                    // 0x800636F0, ticks
```

The tables are `{140, 140, 140}` seconds for one player and `{150, 150, 150}` for two. The HUD clock
(`RASHCDG 0x80062C40`) shows `remaining = *(0x8005ACC8) - (game_state+0x10 - *(0x8005ACD0))`.

Since bit 0 is set only by race type `0x21` (venue 3) and by modes 1 and 17, **only the police-side
races are timed**; ordinary career races and Time Trial are not.

---

## 5. Where a race's rules come from, per race

For a given `(mode, venue, raceId)` a native port must assemble:

| what | from |
|---|---|
| the route | `ROADGRF<set>.TXT` block with `[RACEID] = raceId` (`road.md`) |
| the starting grid | `STARTDF{A,B}.BIN` block `raceId - 1`, or `STARTJBA.BIN` for Jailbreak (`population.md` 5.4, 7) |
| the 18 riders | `LEVEL<bank+1>.BI` filtered by the session's opponent list (`session+0x40`) |
| the player's bike | `<bikes[game_state+0x48]>.PH` |
| race type | `game_state+0x04` from the venue table (section 2.3) |
| difficulty row | `(raceType & 4) ? 2 : (raceType & 1) ? 1 : 0` |
| time limit | `(raceType & 1) ? table[bank] * 300 : none` |
| traffic density | hard-coded in `RASHCDI 0x80068DE4` (`population.md` 3.3) |
| the finish banner quad | `[FINISH_CHECKER]` (section 6.1) |

**Worked example - race 4 of road set 1, the race the savestates capture.** Session mode 32
(career), venue 0 (`JBLG`, environment 0, asset bank 0 -> `LEVEL1.BI`), race type `0x22` -> difficulty
row 0, no time limit, one player, player bike index 0 = `cruisea1`. Route: `[START] = road 9,
distance 1057, direction -1, node 7` with the grid from `STARTDFA.BIN` block 3. Eighteen bikes:
records 0 (the player, class 0 rank 8), 2..8 (class 0, ranks 1..7), 10..17 (class 1, ranks 1..8),
18 and 19 (class 2 = police, ranks 1 and 2, parked on the start line and never moving). Sixteen of
them race; the two police get place 19. `[FINISH_CHECKER] = 0x0080003F, 0, 0`, i.e. scene-cell
resource `0x0080003F`, primitive group 0, quad 0 carries the finish banner. At the captured moment
the player is 16th with health 90/96 and rider 12 is 15th with health 24 - the two visibly damaged
riders are the two at the back.

---

## 6. Progress, the finish, junctions and wrong way

### 6.1 `[START_CHECKER]` / `[FINISH_CHECKER]` - decoded **[established]**

`road.md` 1.2 records these as three ints stored verbatim at `+0x10..+0x18` of the 28-byte
START/FINISH records (filled at `RASHCDI 0x8006A41C` / `0x8006A47C`). They are **not** race logic. All 24 `lui` references to `0x800D6170` / `+0x14` / `+0x18` in all four images
were enumerated; exactly three sites touch `+0x10..+0x18`:

| at | what |
|---|---|
| `SLUS 0x80031870..0x800318A8` (inside the chunk dispatcher `0x80031604`) | compares the resource id of the scene cell being instantiated against `START->+0x10` then `FINISH->+0x10`; on a match stores the record pointer into `0x8005B310` |
| `RASHCDG 0x8006906C..0x800690D4` | takes that record, re-checks `+0x10` against the cell being drawn, reads `+0x14`, checks a visibility bit and calls `0x800706A4` |
| `RASHCDG 0x800706BC..0x800707A8` | **clamps and rewrites** `+0x14` and `+0x18`, locates a quad by them and draws it with a substituted texture |

So:

```c
struct Checker {
    u32 cellResourceId;   // +0x10  the 28-bit scene-cell resource id (key & 0x0FFFFFFF)
    u32 groupIndex;       // +0x14  index into the cell's third primitive-group band
    u32 quadIndex;        // +0x18  index of the quad inside that group
};
```

* `groupIndex` is resolved at `0x800706F4` as `idx = chk1 + 2*B + A` with `A = *(u16*)(body+0x04)`,
  `B = *(u16*)(body+0x06)`, over the 12-byte group records of region 2 - the third band described in
  `scene_cell.md`. Clamped to `B-1` at `0x800706D4`.
* `quadIndex` is clamped against the group's `quadCount` (`group+0x08`) at `0x80070724`, and the
  primitive is `region7 + group->+0x00 + 24*chk2` at `0x8007079C..0x800707BC` - **stride 24 = the
  quad record size**, matching `scene_cell.md`.
* The name is literal: **checker = the chequered flag.**

Data (all 100 races): every `[START_CHECKER]` in both files is
`-1 -1 -1`; `[FINISH_CHECKER]` is set in **38 races of set 1** (ids 1..28 and 56..64) and in **none**
of set 2. 34 distinct `chk0` values, every one of which was found in `STREAM1.STR`/`STREAM2.STR` -
the nibble-8 form (`0x8400002 8` etc.) exactly at chunk offset 0, the nibble-0 form inside the same
chunk. `chk1` is 0/1/2; `chk2` is 0..15.

### 6.2 The progress scalar - `entity+0x144` **[proven on the oracle]**

Computed by `SLUS 0x8003B61C(RoadPos *p)` with `p = entity + 0xAC`, driven from `SLUS 0x8003B520`
(which walks the active-entity list out of scratchpad `0x1F800000`) and stored at `0x8003B574`
(`sw v0,324(s0)`). `0x8003B520` is called from `RASHCDG 0x80078B6C`.

Entity fields it uses (relative to `entity`):

| off | meaning |
|---|---|
| +0x168 | **packed `u16 id; u16 kind`**: `kind == 0` -> `id` is a road id; `kind == 1` -> `id` is a **node** id (the racer is inside an intersection) |
| +0x16C | direction +-1 |
| +0x170 | distance along the road, 16.16 |
| +0x1AC | pointer to the **120-byte runtime intersection record** the racer is currently referred to |
| +0x144 | the resulting scalar |

On-road branch:

```c
link = findLinkByRoad(e->f1AC, e->f168 & 0xFFFF);      // SLUS 0x8003B4B0, link[] at rec+0x14, stride 16
if (!link) return e->f170 >> 4;                        // 0x8003B8BC
if (link.dir > 0) { rem = link.len16 - e->f170; next = link.nodeB; }
else              { rem = e->f170;              next = link.nodeA; }
d = rem >> 4;                                          // 16.16 -> 20.12
if (next != -1) { rec = findIntersectionByNode(cur, next);      // SLUS 0x8003F408
                  if (rec) d += rec->f04; }            // = dist_to_finish << 12
return d < 0 ? 0 : d;                                  // 0x8003B8C8
```

In-intersection branch (`0x8003B684..0x8003B798`): `d = rec->f04`, and when `rec->f10 > 0`
(a route road exists) `d = d - rec->f08 + (offsetInNode >> 4)` with `rec->f08 = node_span << 12`.
Early out: if `*(s16*)0x800D6182 == -1` (a race with no intersections) it returns 0.

**Units.** `SLUS 0x8003B568..0x8003B594` walks thresholds `0x00649581` with step `-0x1927A0`;
`0x649581 >> 12 = 1609` and the step `>> 12 = 402` - a mile and a quarter mile. So the 20.12 unit is
**one metre**, which is the same unit `ROADGRF<n>.TXT` distances are in (`road_chunk.md` already
proved 1 road unit = 1 world unit). At each quarter mile bit 0 of `riderDef+0x44` is set.

**Oracle check**: the formula predicts `entity+0x144`
byte-exactly in **280 of 302** samples over the 14 frame dumps plus the three savestates. The 22
misses are all pool-0 slots 16 and 17 - the two parked police bikes, which are not in the active list
and whose `+0x144` is stale at its spawn value. **For the 16 real racers: 0 errors in every frame.**

### 6.3 Progress -> place **[established]**

`SLUS 0x8003B96C(entity)` returns `valid(entity+0xAC) ? entity->f144 : 0x7FFFF000`, where
`valid` = `0x8003B8F4` ("this racer's route binding is intact": it has a `+0x1AC`, and either
`kind == 1 && node == rec->node` or `findLinkByRoad(rec, road) != 0"). A racer who has left the route
gets "infinitely far", i.e. last place.

`SLUS 0x800138E8(Entity *e, int mode)`:

```c
rd = e->riderDef;                                        // e+0x43C
place = 1;
if ((rd->rankClass & 0x0F) == 2 && rd->place < 247) {    // 0x80013924, 0x80013938
    if (e->handle >= gs->numPlayers || !(gs->raceType & 1) || !(*(gp+0xBC) & 1))
        return *(gp+0x56C) + 1;                          // 0x80013988  police sit behind everyone
}
if (rd->finishTime != 0 || rd->place >= 248) return rd->place;    // 0x8001399C, 0x800139B4
my = Progress(e);                                        // 0x8003B96C
for (o in pool 0) {                                      // 0x800139F4..0x80013AB4
    if (o->handle == e->handle) continue;
    if (o->riderDef->rankClass & 0x0F == 2) continue;     // ignore police  (0x80013A24)
    if (o->riderDef->place > *(gp+0x56C)) continue;       // 0x80013A4C
    if (mode == 1) place += (o->riderDef->finishTime > 0);
    else {
        if (o->riderDef->finishTime > 0) place += 1;      // 0x80013A8C
        else place += (Progress(o) < my);                 // 0x80013A94   SMALLER = AHEAD
    }
}
return place;
```

`*(gp+0x56C)` (gp = `0x8005AC8C`) is the racer count; `*(gp+0x66C)` is a gp-relative copy of the
`game_state` pointer. **The comparison is `<`** - a smaller distance-to-finish means further ahead.

The savestates confirm the police arm numerically: both class-2 records carry place **19** =
18 + 1, so `*(gp+0x56C) = 18`.

The result is stored to `riderDef+0x27`. In the race loop it is recomputed by the HUD
(`RASHCDG 0x800607C8`, only when `*(0x8005ACDC) & 1` and the racer has travelled more than
`0xA0000`) and by the AI (`RASHCDG 0x800968E0`).

### 6.4 The finish test - `RASHCDG 0x800B9958` **[established]**

Core at `0x800B9C08..0x800B9D50`, called from `0x800B9868` inside `0x800B9794`:

```c
rec          = e->f1AC;
onFinishRoad = (e->f168 == FINISH->road);                 // 0x800B9C44, FULL word: kind 0 AND road
claimed      = (*(u16*)(rec + 0x76) >> (e->handle & 31)) & 1;   // 0x800B9C60
delta        = distAlong - FINISH->dist;                  // 0x800B9D00  (16.16)
nodeOk       = (rec->node == FINISH->node) || (FINISH->node == -1);  // 0x800B9D1C/0x800B9D2C
finished     = onFinishRoad && claimed && nodeOk && ((FINISH->dir ^ delta) >= 0);  // 0x800B9D44
```

i.e. **the sign of `(distance_along - FINISH.dist)` must agree with the sign of `FINISH.dir`** - the
line has been crossed in the right direction - *and* the racer must be legitimately bound to the
finish intersection (its bit in `rec+0x76`). On success (`0x800B9F20` onward) `entity+0x230 |=
0x08000000`, `0x8005B230` is cleared, and a code goes into `riderDef+0x27`.

`RASHCDG 0x80092C7C(entity, reason)` is the generic "this rider's race is over":

```c
entity->f230 |= 0x08000000;                                // 0x80092CA8
rd = entity->riderDef;
if (rd->finishTime == 0) {
    *(u32*)0x8005B230 = 0;                                 // 0x80092CB8
    rd->place      = (reason == 9) ? 255 : 254;            // 0x80092CC4/0x80092CC8/0x80092CCC
    rd->finishTime = game_state->raceClock;                // 0x80092CE8
    RASHCDG 0x800BC7CC(entity);
}
```

### 6.5 Junction validation **[established]**

`SLUS 0x8003AF9C` -> `SLUS 0x8003B1C4(RoadPos *p, int kind, Entity *e)`.

Entering a node (`0x8003B1E4..0x8003B314`):

```c
rec = p->f100;                                     // = e->f1AC
if (rec->node == newNode) { rec->f76 |= 1 << slot; return; }        // 0x8003B25C
for (i = 0; i < rec->f10; i++)                                      // 0x8003B270
    if (rec->nextNode[i] == newNode) { p->f100 = recOf(newNode); ...; return; }
rec->f76 &= ~(1 << slot);                                           // 0x8003B314   off-route
```

Entering a road (`0x8003B32C..0x8003B494`):

```c
if (findLinkByRoad(rec, road)) return;                              // fine
if (mode >= 2) { p->f100 = 0x8003F4D8(road); return; }              // traffic / props: no validation
if (e && mode == 0 && bikeClass != 2 && handle >= numRacers)
     { p->f100 = 0x8003F4D8(road); return; }                        // 0x8003B358..0x8003B38C
dist = 0x8003A5F4(&e->f168, &node, 0);
if (dist > 0x00320000) return;                                      // 50.0 world units, 0x8003B404
for (i = 0; i < rec->f10; i++)
    if (rec->nextNode[i] == node && 0x8003F580(recOf(node), road))   // road must be a route_road
        p->f100 = recOf(node);
```

**A wrong turn is physically possible** - nothing blocks riding onto a non-route road. But for
racers and police the re-binding happens **only** to a route `nextNode` and **only** if the new road
is in `routeRoad[]`. Otherwise `+0x1AC` stays on the old intersection, `0x8003B8F4` returns 0, the
progress scalar becomes `0x7FFFF000` and the racer is instantly last; returning to a route road
restores everything. Traffic and props (`mode >= 2`) are never validated.

### 6.6 Wrong way **[established]**

The flag mask is produced once per frame by `SLUS 0x8003C590(Entity *e)`, called from
`RASHCDG 0x8005E9FC`:

```c
s2 = (*(u16*)(e+0x16A) == 1) << 7;                        // bit 7 = inside an intersection
if (0x80095410(e) == 0) {                                 // route binding intact   0x8003C5BC
    if (e->riderDef->flags0 & 0x80) {                     // 0x8003C5DC
        d = 0x8002E698(e + 0x1C2, *(e+0x154) + 0x0E);     // GTE MVMVA dot product
        if (abs(d) > 0xC000) s2 |= 0x01;                  // 0x8003C630..0x8003C640   WRONG WAY
    }
}
if (!(s2 & 1) && 0x80095410(e)) s2 |= 0x02;               // 0x8003C660  off route
if (s2 & 3) return s2;
if (*(u16*)(e+0x16A) != 0) return s2;
dist = 0x8003A5F4(&e->f168, &node, 1);
if (node == -1 || dist > *(0x800531A0 + 4*game_state->bank)) return s2;   // 0x8003C6C8
rec = findIntersectionByNode(e->f1AC, node);
if (0x8003BC48(node, road, out, 3) != 2) return s2;       // exactly two alternatives
if (0x8003F580(rec, out[0])) s2 |= 0x10;                  // 0x8003C71C  TURN RIGHT
if (0x8003F580(rec, out[1])) s2 |= 0x40;                  // 0x8003C734  TURN LEFT
```

* `0x8002E698` loads a 3 x s16 forward vector from `entity+0x1C2` into IR1..IR3 and a road-basis row
  from `*(entity+0x154) + 0x0E` into the GTE colour matrix, executes **MVMVA** (`c2 0x45E012`) and
  returns `MAC1 >> 8`. With a 4096 = 1.0 scale, `4096*4096 >> 8 = 65536 = 1.0`, so the threshold
  `0xC000 = 49152` is **|cos| > 0.75, about 41 degrees**.
* `entity+0x1C2` is a cached "forward": it is byte-identical to the third row of the orientation
  matrix at `entity+0x1B0` in both racers checked.
* Measured on `ram_000200.bin` for two correctly-travelling racers: `|dot| = 41034` and `40973`,
  about 17 % of headroom below the threshold.
* **There is no hysteresis or timer on the condition itself** - it is recomputed every frame. The
  only timers are on the sprite's flashing (`SignFlash`, 75/100 frames) and they are reset whenever
  the sign code changes (`RASHCDG 0x8005FC44`).
* The arrow-distance table `SLUS 0x800531A0` indexed by the asset bank is `{180, 225, 280}` units
  `<< 16`; indices beyond 2 are garbage.

The mask reaches the HUD at `RASHCDG 0x8005FBF4..0x8005FC40`:

```c
if      (flags & 0x01) signCode = 4;      // WRONG WAY
else if (flags & 0x10) signCode = 3;      // TURN RIGHT
else if (flags & 0x40) signCode = 1;      // TURN LEFT
else                   signCode = 0;
if (signCode != dash->f3C) { dash->f3C = signCode; reset three flash timers; }   // 0x8005FC40
```

and `0x8005FD94..0x8005FDBC` turns it into HUD element 81 (`kDashSign`, art 55 = `WRONGWAY.TIM`),
82 (`kDashTurnLeft`) or 83 (`kDashTurnRight`).

### 6.7 There is no lap and no checkpoint - a precise negative **[proven]**

Eight independent arguments:

1. The progress scalar is computed from the instantaneous position with **no stored state**
   (`0x8003B61C` reads and writes no counter); 280/302 exact predictions from
   `(road, dir, dist, +0x1AC)` alone.
2. The place is a pure function of that scalar (`0x800139F4..0x80013AB4`); no lap counter appears.
3. The finish test is a single sign comparison (`0x800B9CE8..0x800B9D50`), not an "N-th crossing".
4. `ROADGRF<n>.TXT` has no lap field, and `dist_to_finish` is monotonically decreasing along the
   route by the identity `road.md` 1.4 proves.
5. The only per-racer "passed here" mark is the bit in `intersection+0x76`, set and *cleared*
   (`0x8003B25C` / `0x8003B314`) - a presence flag, not a counter.
6. The only checkpoint-like table in the whole game is `SLUS 0x80053174`, five
   `{u16 along; u16 road}` records `(2025, 28) (1975, 28) (1925, 28) (700, 27) (1200, 27)`, advanced
   through `game_state+0x39` at `RASHCDG 0x800B9E90`, and reachable **only when
   `game_state+0x04 == 44` (0x2C = the Jailbreak venue)**. Those are the **Jailbreak mission phase
   milestones** (section 9.6), hard-coded in the EXE and unrelated to `ROADGRF`.
7. A diff of the 14 consecutive frame dumps over the whole of BSS `0x800CD670..0x800DC0C8`
   produces no monotone small counter that behaves like a lap.
8. The HUD layout confirms it from the other side: `DASH1P.CSV` declares 110 elements and **none of
   them is a lap or checkpoint counter** - only position (`kDashRaceRank0/1`, `kDashNumRacers0/1`),
   a radar and a clock.

---

## 7. Placing, results, and the career

### 7.1 The result codes **[proven, from both ends]**

`riderDef+0x27` carries either a 1-based place or a result code >= 248. Both ends agree:

| code | written by | frontend reading |
|---|---|---|
| 1..n | `SLUS 0x800138E8` | a finishing position |
| 248 | `RASHCDG 0x80097008`, **only when `game_state+0x04 == 0x21`** | success (career arm `0x8007BDB8` -> win) |
| 249 | not found in `RASHCDG` | success (`0x8007BDEC` -> win) |
| 250 | (see 13) | failure / target escaped (`0x8007BE24` -> lose; Cops'n'Robbers arm shows the escape screen) |
| 251 | | success (`0x8007BD80` -> win) |
| 252 | `RASHCDG 0x80097020` (cop contact, race type != 0x21) | wreck (`0x8007BE5C`) |
| 253 | `RASHCDG 0x80097060` when the counter at `0x8005AD44` runs out | wreck |
| 254 | `RASHCDG 0x80092CCC` with `reason != 9` | **BUSTED** (`0x8007BE90`) |
| 255 | `RASHCDG 0x80092CCC` with `reason == 9`; also forced at `EXE 0x8002D494` on quit | **WRECKED** (`0x8007BE5C`) |

`riderDef+0x28` is the timestamp in ticks of whichever of these happened
(`RASHCDG 0x80092CE8`, `0x8009701C`, `0x80097298`, `0x800972EC`, `0x8009737C`, `0x8009E750`).

### 7.2 How the result reaches the frontend **[established]**

The race overlay writes the codes into `riderDef+0x27`; the frontend dispatches on **the per-player
record's `+0x20`**, `0x800D81F8` for player 1 (`RASHCDF 0x8007BCD8`, `0x8007DE88`, `0x8007E358`).
The write site of that field was **not found** (section 13) - a scan of all four images for a store
at offset 0x20 relative to a register built from `0x800D81D8` finds nothing, so it goes through a
pointer held elsewhere.

### 7.3 Which result screen plays **[established]**

`RASHCDF` keeps a 37-entry resource table at `0x800897E4`, 32 bytes per record:

```c
struct ResRecord {         // 32 bytes
    u16  flags;            // +0x00  bits 0x000E are the load/show state, cleared at 0x80076344
    u8   klass;            // +0x02  1 = .psh, 2 = .str, 3 = .pfn, 4 = .loc, 5/6 = dynamic
    u8   id;               // +0x03  index within the class
    void *data;            // +0x04
    char name[16];         // +0x08
    s32  lba;              // +0x18  -1 = not loaded
    u32  reserved;         // +0x1C  always 0
};
```

(The record count 37 is independently confirmed by the `slti v0,a0,37` bounds at `0x80076344` and
`0x800782D4`.) The result screens are records **20 = bust, 21 = lose, 22 = win, 23 = wreck,
24 = jailed, 25 = escape**, plus **26..29 = the two gangs' win/lose screens**.
`res_show(index, mode)` is `0x800782B0`. Records **30..36** name seven `.psh` files
(`sprfiveo.psh` .. `sprtrphy.psh`) that are **not on the disc**; they carry `flags == 0x0000` and no
instruction references them (`frontend.md` 5.2).

Career / Jailbreak (`RASHCDF 0x8007BB34`, dispatch at `0x8007BCD8`) folds the result code into an
internal outcome `s3` (1 win, 2 lose, 3 bust, 4 wreck) and then at `0x8007D950`:

```
s3 == 1 -> the gang win  screen: record 26 or 27, chosen by *(s8*)0x800D81E1 (player+0x09 = gang id)
s3 == 2 -> the gang lose screen: record 28 or 29, same selector
s3 == 3 -> res_show(20 = bust.str,  1)          // 0x8007DC3C
s3 == 4 -> res_show(23 = wreck.str, 1)          // 0x8007DCA0
```

Single-player modes (`0x8007DE88`): `250 -> lose`, `251/252/253/255 -> wreck`, `254 -> bust`,
`code < 4 -> win`, otherwise lose; the arms also install the four-character event tags `LOSR`,
`WRKD`, `BSTD`, `WNNR`.

Cops'n'Robbers (`0x8007E358`, table `0x8005C9D0`): `250 -> escape.str` (the robber got away),
`252` and `253 -> jailed.str`, and `251/254/255` show nothing.

### 7.4 The prize table - the cash economy **[proven from the bytes]**

`RASHCDF 0x80089F44` is a **`u16 prize[place][venue]` table, row stride 24 bytes, six venues per
row** (listed below). The winning arms index it as
`*(u16*)(0x80089F44 + 4 * session->venue)`, i.e. row 0 (`0x8007BD90..0x8007BDA0`,
`0x8007BE00`, `0x8007BEDC`, `0x8007BF14`, `0x8007BF4C`); the losing arm at `0x8007BFA4` indexes it as
`(place - 1) * 24 + (venue << s3)` (`0x8007BFB4..0x8007BFD8`).

| place | venue 0 | venues 1, 2 | venues 3, 4, 5 |
|---|---|---|---|
| 1 | 420 | 780 | 1260 |
| 2 | 396 | 737 | 1190 |
| 3 | 359 | 666 | 1077 |
| 4 | 298 | 553 | 894 |
| 5 | 252 | 468 | 756 |
| 6 | 199 | 370 | 598 |
| 7 | 157 | 292 | 472 |
| 8 | 126 | 234 | 378 |
| 9 | 100 | 187 | 302 |
| 10 | 77 | 144 | 233 |
| 11 | 58 | 109 | 176 |
| 12 | 46 | 85 | 138 |
| 13 | 35 | 66 | 107 |
| 14 | 29 | 54 | 88 |
| 15 | 25 | 46 | 75 |
| row 16, 17 (`0x8008A0C4`, `0x8008A0DC`) | -2 | -3 | -6 |
| row 18 (`0x8008A0F4`) | -21 | -78 | -126 |
| row 19 (`0x8008A10C`) | -21 | -78 | -126 (venue 3..5 row is zero) |

**The rows past 15 are the outcome penalties**: the result-category arms
(`RASHCDF 0x8007BE5C` wreck -> `0x8008A0DC`, `0x8007BE90` bust -> `0x8008A0F4`,
`0x8007BE24` lose -> `0x8008A10C`) are the same table indexed past the placing range. So a venue-0
win pays 420, a 15th place pays 25, and being busted costs 21.

Two session counters move with the result: `session+0x20` (u16) is bumped on every win arm
(`0x8007BDA4..0x8007BDB4`) and `session+0x26` (u16) on the lose arm (`0x8007BFD4..0x8007BFE0`).

Other economy facts:

* The per-venue progress branch is `RASHCDF 0x8007D4A0`: `v = session+0x04; if (v < 6) jump
  table[0x8005C988 + 4v]` with arms `0x8007D4D0, 0x8007D5B4, 0x8007D64C, 0x8007D730, 0x8007D7C8,
  0x8007D8B8` - one per venue. **This is where a race unlocks the next**; the arms are read in
  `frontend.md` 7.4.
* The session's two bitmaps (`+0xF0`, 65 bits; `+0xFC`, 18 bits) are the persistent progress; both
  are cleared wholesale by `EXE 0x8002D250` and `0x8002D298` and read by ten sites in `RASHCDF`
  (`0x80062E18`, `0x80062FB0`, `0x80064378`, `0x8006450C`, `0x800646BC`, `0x80064890`, `0x80064A40`,
  `0x80067488`, `0x800675FC`, `0x80067770`, ...). The setters are in the frontend's result
  dispatchers: `+0xF0` is set at `0x8007BF8C` (index = `session+0x08`, the race id) and `0x8007E0E8`
  and cleared at `0x8007D904` and `0x8007E2A4`; `+0xFC` is set at `0x8007E47C` and `0x8007E54C`
  (index = `session+0x07`) (`frontend.md` 7.3).
* `GAMESTRG.LOC` (181 strings, the in-race pool) contains no cash string at all; `FESTRING.LOC` ids
  1519..1534 are the statistics and bonus labels (pro / con / steals / hits / takedowns, weapon and
  nitro gain and loss, **race bonus, combat bonus, total bonus, cash**) and ids 1510..1518 are the
  progress-unlock notifications. The prize table above is the *race bonus*; the **combat bonus
  arithmetic was not located**, and its inputs are presumably `riderDef+0x30` (per-weapon swing
  counters) and `riderDef+0x3D` (combo length).
* `FESTRING.LOC` ids 405..1004 are 600 strings in strict groups of six - the course / event
  descriptors: name, blurb, route, traffic density, curve factor, and either a distance or a "beat
  length" for cop missions. **100 events x 6 lines** matches the 100 races of the two road sets
  exactly. That is the human-readable side of the per-race rules and a good cross-check for a native
  port. ids 1005..1070 are the 22 bike catalogue entries in groups of three.

### 7.5 The string containers **[proven]**

Both `.LOC` files share one format:

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

| file | size | count | index ends | first string |
|---|---|---|---|---|
| `GAMESTRG.LOC` | 3292 | **181** | 0x2F8 | 0x2F8 |
| `FESTRING.LOC` | 89232 | **1945** | 0x1E88 | 0x1E88 |

`chunkSize + 0x14 == fileSize` in both. 102 of the 1945 frontend strings are empty (placeholders
inside fixed-size blocks). **Neither pool contains a `printf` specifier or a bracketed key** - all
number substitution is done by code.

Loader: `RASHCDI 0x80063D94(name, flags)` - `sprintf("%s%s", "DATA\", name)`, file load into
`0x8005AE78`, then relocate `table[i] += base + base[0x10]` for `i < count` and publish the table at
`*(char***)0x8005B544` (`0x80063E34`). `RASHCDF 0x80065F4C` is the same relocation for data the
frontend's resource manager already loaded, writing the *same* two globals - which are in the
resident EXE below the overlay window, so the frontend and the race share one slot by turns.
Lookup is inlined everywhere as `((const char **)0x8005B544)[id]`; the only wrapper is the
frontend's `0x800662CC(ctx, id, ..., ...)`.

`RASHCDI 0x8005EC20` pre-renders `GAMESTRG` ids 15..29 into 15 sprites at `0x800D6120` - exactly the
in-race message set (11 cop-mission messages, demo/auto labels, and the two state words drawn when a
rider is arrested or wrecked).

`GAMESTRG.LOC` id map (counts and function only, no text): 0..14 pause menu and disc errors;
**15..25 cop-mission HUD messages**; 26..27 demo/auto labels; **28..29 the arrested / wrecked
words**; 30..33 player labels; **34..91 the 58 rider display names** (this is the range
`riderDef+0x26` indexes); 92..93 placeholders; 94..98 the results-table header and column labels;
99..107 cop-mission outcomes and player statuses; 108..115 team and sidecar names; 116..139 the
verdict paragraphs (time out, new record, track record, mission pass/fail, the Jailbreak story
outcomes); **140..142 the quota / arrests / time labels**; 143..155 cop success lines;
**156..161 the six Cops'n'Robbers result headlines**; 162..172 player and team labels; 173..180 the
results columns (position, time, hits, TKOs, steals, wins, for, against).

That last group is the shape of the **post-race results table**: position, time, hits, TKOs, steals,
wins, for, against - exactly the counters the combat system maintains.

---

## 8. Combat

The summary a race loop needs.

### 8.1 `DATA\FIGHT.BIN` - fully decoded **[proven]**

Loader `RASHCDI 0x800653E8`: `sprintf("%s.bin", "DATA\FIGHT")`, file load, then **relocate exactly
40 records of 12 bytes** (`slti v0,a1,40` at `0x8006544C`, `addiu a0,a0,12` at `0x80065454`),
turning the two file offsets at `+4` and `+8` into pointers. The pointer is published at
`0x8005AD4C` (`RASHCDI 0x800637B8`). Confirmed on live RAM: in all three savestates and all 14 frame
dumps `*(u32*)0x8005AD4C == 0x801A6B5C`, and the live image equals the file except for those 80
relocated words.

```c
struct FightRec {          // 12 bytes, 40 of them at file 0x0000..0x01E0
    u16 idleEvent;         // +0x00  rider-state id of the idle stance (11 for 0..19, 77 for 20..39)
    u8  countA;            // +0x02  entries in block A     (RASHCDG 0x800C2638)
    u8  countB;            // +0x03  entries in block B
    u32 blockA;            // +0x04  -> input windows / chain edges   (RASHCDG 0x800C25EC)
    u32 blockB;            // +0x08  -> move nodes                    (RASHCDG 0x800BF898)
};

struct FightNode {         // 12 bytes
    u16 animStart;         // +0x00  rider state played on entering
    u16 animEnd;           // +0x02  on leaving; 224 = none -> use FightRec.idleEvent
    u16 animHit;           // +0x04  the state the TARGET gets on a hit
    u16 animMiss;          // +0x06  ... on a miss / block
    u16 damage;            // +0x08  base damage
    u8  reach;             // +0x0A  reach bonus, 16.16: reach << 12  (reach/16 world units)
    u8  hitFrame;          // +0x0B  animation frame from which the strike is live
};

struct FightEdge {         // 12 bytes
    s32 tStart, tEnd;      // input window in the node's own accumulator
    u32 packed;            // 0..3 button condition, 4..7 op, 8..13 slot,
                           // 14..19 required previous node, 20..25 node, 26..31 alternative node
};
```

The record index is `rider+0x239`; **41 means "not fighting"**. Records 0..19 are the normal set,
20..39 the passenger variant.

### 8.2 The hit test and damage **[established]**

Coarse gate in `FightUpdate` (`RASHCDG 0x800C035C`): lateral separation must be
`<= 2 * *(0x80052F74) = 2 * 1.2 = 2.4` world units (`0x800C0798`), longitudinal `<= 0.7`
(`0x800C07BC`). Then `RASHCDG 0x800C159C`:

```c
reach = trig(headingDelta) + (node.reach << 12) + me.def[0x08] + me[0x130] + other[0x130];
if (|lateral| < reach) flags |= 1;                       // HIT   (0x800C1714..0x800C175C)
```

`RASHCDG 0x800C17B0 ApplyHit`:

```c
dmg = node.damage;
if (attacker.rider[0x23C] & 0x80) dmg *= 3;              // charged strike, 0x800C18E8
k   = max(attacker.def[0x0F], 96);                       // strength scales with own health, floor 96
final = (dmg * k * attacker.def[0x0C]) >> 14;            // 0x800C1940
h = target.def[0x0F] - final;                            // 0x800C1984
if (h < 0) { target.def[0x0F] = 0; }                     // KNOCKED OFF THE BIKE
else       { target.def[0x0F] = h; if crossed 64 or 32 -> def[0x44] |= 0x20; }
target.def[0x0E] = max(0, def[0x0E] - (final + 1) / DIV[game_state->bank]);   // 0x800C1998
```

`DIV` is `SLUS 0x80053188 = {8, 8, 8, 120, 112, 120}`. **The knock-off threshold is simply
`def[0x0F] - final < 0`.** Confirmed on the frame dumps: pool-0 slot 0 has `def[0x0F] = 0x5A` in
`ram_000200..260` and `0x00` with `def[0x44] = 0x40` from `ram_000280` on.

Combo length is limited by `def[0x3D]`: `(low nibble) < (high nibble)` (`0x800C0BE8`); the savestates
have `0x70`, i.e. seven hits.

### 8.3 Weapons **[established, with one [probable] link]**

`def+0x2C` bits 0..8 = possession mask; `def+0x2E` = current weapon 0..8 (9 = fists); `def+0x2F` =
swings left; `def+0x30` = 8 nibbles of per-weapon swing counters. Selection is
`RASHCDG 0x800B9340` using a 9-byte preference order per fighter class from `SLUS 0x80052EEC`
(stride 36): class 0 `[0,4,2,5,3,7,6,8,1]`, class 1 `[4,6,8,7,3,1,0,5,2]`, class 2
`[1,2,5,7,6,3,4,0,8]`.

| id | art tag | weapon | fight record (by sub-group) |
|---|---|---|---|
| 0 | `CHAN` | chain | 6 / 7 / 14 |
| 1 | `CLUB` | club | 4 / 5 / 13 |
| 2 | `PIPE` | pipe | 2 / 3 / 12 |
| 3 | `WOOD` | board | 2 / 3 / 12 |
| 4 | `NCHK` | nunchaku | 8 / 9 / 15 |
| 5 | `CBAR` | crowbar | 4 / 5 / 13 |
| 6 | `PROD` | cattle prod | 18 |
| 7 | `STUN` | stun gun | 18 |
| 8 | `SPRY` | spray can | 19 |
| 9 | - | fists | 0 / 1 / 16 |

The id-to-tag column is **[established]**; the naive `tag = 91 + id` reading (which would name
0 NCHK, 2 WOOD, 3 CBAR, 4 CHAN, 5 PIPE) does not hold. The rap sheet
`RASHCDF 0x80074554` (PORTED, `shell_text.cpp`) draws owned bit k of `player+0x0C` with the sprite tags
`CHAN CLUB PIPE WOOD NCHK CBAR PROD STUN SPRY` for k = 0..8 (entries 95 92 96 93 91 94 97 98 99 of the
frontend art-tag list `RASHCDF 0x80088DF4`, whose 91..99 are in the shop's own order). In the race the
ported HUD's weapon icon (HudFrame item 24, art 41 + weapon) is a chain for weapon 0 and a nunchaku for
weapon 4, and model 800 (BBLEVEL1.GEO) agrees: group 0 is four equal links (177 / 180 / 184 / 187 model
units, program 6), group 4 a 277 stick, two short links (101 / 97) and a 419 stick. The records' grouping
is {1,5} club and crowbar, {2,3} pipe and board, {6,7} prod and stun gun; {0},{4},{8}.

**Weapons are stolen, never picked up.** `RASHCDG 0x800BFF04`, reached from `0x800C0938` only when
the attacker's command is bare-fisted (`def[0x3C] == 32`) and gated at `0x800C2E9C` by record 0,
node 1 and a frame window: the victim's weapon moves into the attacker's mask, the victim is left
with fists, and the swing counters transfer. No ground-pickup code exists anywhere in the four
images. `RASHCDF 0x8007F628` copies `def+0x2C/+0x2E/+0x2F/+0x30` out of the career record before a
race and `0x8007BC0C` copies them back, so weapons persist between races.

### 8.4 Animation binding **[established]**

Every id in `FIGHT.BIN` is a **rider state id**, not an animation index. `RASHCDG 0x800C2FF4` stores
it at `rider+0x220` and looks the state up in the resident table `SLUS 0x800541D4`, 8 bytes per
record, records 0..222 (223 of them; 223 and 224 are zero, which is why **224 means "no
animation"**):

```c
struct RiderState {   // 8 bytes
    u16 anim;         // (clip << 4) | subflag;  clip runs 0..122 = the 123 DMD3 blocks of ANIMTBL1.PSX
    u16 category;     // 0..8; category 3 is the precondition of nearly every combat call
    u16 w2, w3;       // 0 / 96 / 112 / 127 / 480 / 0xFFFF - not decoded
};
```

`ANIMTBL*.PSX` / `ANIMTBS*.PSX` are flat `DMD3` chains (`rmd3.md` 5); block counts: `ANIMTBJ3` 14,
`ANIMTBL1` 123, `ANIMTBL2` 17, `ANIMTBL3` 14, `ANIMTBLB` 1, `ANIMTBLJ` 69, `ANIMTBLP` 13,
`ANIMTBLS` 69, `ANIMTBLW` 27, `ANIMTBS1` 123, `ANIMTBSB` 1, `ANIMTBSW` 41. The name table is
`SLUS 0x80052DF4`, 12 records of 16 bytes.

**Precise negative:** in all 3 savestates and all 14 frame dumps, **no rider is in combat** - all
306 samples have `rider[0x239] == 41` and `rider[0x23C] == 0`. The combat runtime fields cannot be
observed on the available captures; that needs a capture with a fight in progress.

---

## 9. Police, pursuit and arrest

### 9.0 Who arrests whom **[proven]**

**The arrest path is the player arresting somebody, not the police arresting the player.** The one
arrest function, `RASHCDG 0x80096F30`, rejects at its head anything whose handle is
`>= game_state+0x30`, i.e. anything that is not a player (`0x80096F54..0x80096F64`), and then
requires that player to be on a class-2 bike. The whole branch is gated by **bit 0 of
`game_state+0x04`**.

There is therefore **no code by which an AI cop arrests the player** - a precise negative. A player
is "busted" only by the Jailbreak phase deadline or by the general race time-out (9.4). And the HUD
elements `kDashArrest`, `kDashArrests`, `kDashQuotaBox`, `kDashSuspect`, `kDashSuspectName`,
`kDashTimerCop`, `kDashRadarArrow`, `kDashRadarDist0..2`, `kDashFiveOArrestMsg` are the **Five-O
HUD, where the player is the cop** - an arrest quota, a mission timer and a radar to the suspect.

### 9.1 Identifying a cop **[proven]**

`(riderDef+0x01) & 0x0F == 2`; the canonical shape is
`lw v0,1084(e); lbu v0,1(v0); andi v0,v0,0xf; bne v0,2`, which occurs **68 times in `RASHCDG`**
(reference sites `0x80095890..0x800958A4`, `0x80093F14`,
`0x800940F0`, `0x80094ECC`, `0x80094FE4`, `0x80096B78`, `0x800B8500`, `0x800BD2C0`,
`RASHCDI 0x8006504C`, `SLUS 0x80013924`, `SLUS 0x80013A20`).

There is **no separate cop list**. Two counters, both written by `RASHCDI 0x80068684`:

| global | meaning | written at |
|---|---|---|
| `0x800D86F4` | total class-2 bikes in pool 0 (the cap) | `RASHCDI 0x800686A4`, from a census `0x80068614(class = 2)` |
| `0x800D86F0` | cops currently released into the world | `RASHCDI 0x800686AC`, then `RASHCDG 0x8009DAF8(+-1)` clamped to `[0, cap]` (`0x8009DB00..0x8009DB54`) |

`0x8009DAF8` has exactly two callers: `+1` from `0x80095024`, `-1` from `0x80094134`.

**"This cop is in play" is bit `0x10` of `entity+0x3A0`**: set at `RASHCDG 0x8009503C` (which also
clears bit `0x20`), cleared at `0x8009411C`, and read together with the class predicate at ten sites
(`0x8008B8E4`, `0x800908A4`, `0x80093F30`, `0x8009410C`, `0x80094F5C`, `0x80095010`, `0x800957A8`,
`0x800958BC`, `0x80095D44`, `0x800B851C`).

**Oracle:** in all savestates `*(0x800D86F4) = 2`, `*(0x800D86F0) = 0`, rider records 18/19 are
class 2 in pool-0 slots 16 and 17, `entity+0x3A0 = 0x08` (bit `0x10` clear) and `entity+0x140 = 0`.
Both cops are asleep on the start line - which is exactly what `population.md` 5.4 measured
geometrically. **[proven]**

Cops are excluded from the placing loop (`SLUS 0x80013A24`) and are given `numRacers + 1` when asked
for their own place (`SLUS 0x80013988`) - measured as 19 in all three savestates.

### 9.2 There is no pursuit FSM; there is a spawner **[established]**

A cop has no chase state machine. The 18 pool-0 slots are fixed for the whole race; two of them are
class 2 and start switched off; a scheduler **teleports a free cop onto the player's road ahead of
him** and switches it on as an ordinary AI rider with a different target speed.

Entry point and its place in the frame:

```
RASHCDG 0x8008CD88(distanceThisFrame)
    acc = (*(u32*)0x8005B2D8) += dist
    if (acc >= 16384) 0x8009C308()
    if (acc > 0xFFFF) { 0x8009B474(0, acc);      // kind 0 = MOTORCYCLES -> police
                        0x8009B474(3, acc); }    // kind 3 = traffic
RASHCDG 0x8009B474(kind, dist):  kind == 0 -> 0x8009E89C(dist)     // the police scheduler
                                 kind == 3 -> 0x8009CFF4(dist)     // population.md 3.2
```

The cells' kind-0 array is dead (`population.md` 2.2), but a *procedural* kind-0 spawner exists and it
is the police spawner, exactly mirroring the traffic one.

The scheduler `RASHCDG 0x8009E89C` (body `0x8009E8A4..0x8009EF8C`) is gated by
`if (*(u32*)0x8005ACC0 == 0) return;` (`0x8009E8A0`) - set to 1 by `RASHCDF 0x8007F340`, cleared by
Jailbreak phase 4 (`RASHCDG 0x800C91A0`) and by `RASHCDI 0x80069898`.

*Pass 1 - eligible players* (`0x8009E910..0x8009E9CC`), per player: `riderDef[0x28] == 0`
(not finished) **and** `!(game_state[0x04] & 1)` **and** `!(riderDef[0x00] & 0x40)` (not frozen on
the grid) **and** the rider is in a usable state **and** `*(0x800D86F4) > 0`.

*Pass 2 - the release conditions* (`0x8009EA04..0x8009EF8C`), per eligible player:

| term | meaning | at |
|---|---|---|
| `s8` | `0x80095410(player)` - the player is on a not-yet-claimed stretch | `0x8009EA28` |
| `s6` | `riderDef[0x00] >> 7` | `0x8009EA4C` |
| leading | `riderDef[0x27] == 1` | `0x8009EA50` |
| `s1` | if `riderDef[0x27] >= *(0x8005B1F8) - *(0x800D86F4)` (near the back), then `speed <= 0x00165A11 = 22.36` | `0x8009EA64..0x8009EA80` |
| `s7` | force: `game_state[0x04] == 44 && game_state[0x39] == 3` | `0x8009EA8C..0x8009EAAC` |

Probability accumulator A at `0x8005B2A0 + 4p` (`0x8009EAB0..0x8009EBCC`):

```c
accA += distThisFrame;
row = game_state->bank + (*(0x800D86F0) > 0 ? 3 : 0);          // 0x8009EAD4
if (accA >= *(0x800530FC + 4*row)) {                            // 30.0 with no cop out, 60.0 with one
    if (rng() % 100 < *(0x80053120 + 4*row))                    // 10 %, or 2 % with a cop already out
        if (0x8003A5F4(player+0x168, &tmp, 1) > *(0x80053114 + 4*bank))    // 120.0 of road ahead
            force = 1;
    accA = 0;
}
```

Main accumulator B at `0x8005B368 + 4p` (`0x8009EBD0..0x8009ED44`):

```c
if (rider->f25C == 1 && player->speed > *(0x80053138 + 4*bank)   // 18.0   0x8009EC0C
    && !s6 && !s8 && !leading && !s1 && !force)
        { accB = 0xFFFF0000; continue; }                          // 0x8009EF38  suppressed
accB = (accB >= 0) ? accB + dist : 0;                             // 0x8009EC50
thr = *(0x800530FC + 4*row);
if (force || accB > thr || ((s6 || s8) && accB > thr/2)) release();
```

Release (`0x8009ED48..0x8009EF38`):

```c
cop = FindFreeCop();                                     // 0x80095848
if (!cop) { accB = 0xFFFF0000; continue; }
cop->f168 = player->f168;                                // same road       0x8009EDA4
cop->f16C = (player->f16C > 0) ? +1 : -1;                //                 0x8009EDBC
cop->f170 = player->f170 +- *(0x80053114 + 4*bank);      // 120.0 AHEAD     0x8009EDE0/0x8009EE00
cop->f39C = cop->f1E0 = *(0x80053138 + 4*bank);          // start at 18.0   0x8009EEB0/0x8009EED0
cop->f140 = 1;  0x80093F94(cop, 0);                      // activate        0x8009EEEC
cop->rider->f25C = 1;
0x800BCD10(cop); 0x800BCA68({4, 224}, 1, cop);           // siren
cop->riderDef[0x3E] = game_state->raceClock >> 8;        // release stamp   0x8009EF30
accB = 0xFFFF0000;
```

`FindFreeCop` = `RASHCDG 0x80095848`: walks pool 0 through the table at `0x800CE4D0` and returns the
first entity with `handle >= numPlayers` (`0x80095884`), class 2 (`0x800958A4`), `+0x140 == 0`
(`0x800958B4`) and `+0x3A0 & 0x10 == 0` (`0x800958C8`).

Activation/deactivation is the generic sleep flag, not a pursuit state: `entity+0x140` bit 0 = asleep;
`0x80093F94(e, want)` switches through `0x8009432C` (wake: set bit `0x10` of `+0x3A0`, clear `0x20`,
`0x8009DAF8(+1)`, `riderDef[0x28] = 0`) and `0x80093FE4` (sleep: clear `0x10`, `0x8009DAF8(-1)`).

**Parameter tables** (resident, all indexed by `game_state+0x3C` = bank 0..2):

| table | values | role |
|---|---|---|
| `0x800530FC` | 30.0 x3, then 60.0 x3 | spawn-attempt distance; the second triple is used when a cop is already out |
| `0x80053120` | 10 x3, then 2 x3 | spawn probability in per cent |
| `0x80053114` | 120.0 x3 | road-ahead requirement **and** the spawn offset ahead of the player |
| `0x80053138` | 18.0 x3 | "the player is going fast" threshold, and the cop's initial speed |
| `0x800530F0` | 5.0 x3 | base timeout of the arrest cut-scene (halved -> 2.5) |
| `0x8005309C` | 3.0 x3 | **the arrest radius** (the high half, = 3) |
| `0x80053174` | `(2025, 28) (1975, 28) (1925, 28) (700, 27) (1200, 27)` as `(u16 along, u16 road)` | **the Jailbreak phase milestones** |
| `0x80053144` / `0x80053150` / `0x8005315C` | 30.0 / 10.0 / 10 | the cop's speed ramp (9.5) |

**Oracle check of the suppression rule** - the accumulator `0x8005B368[0]` in the three savestates:

| state | player place | speed | expected | measured |
|---|---|---|---|---|
| `rr-race` | 16 (near the back, `s1` set) | 4.16 | accumulating | **670350** |
| `rr-pack` | 15 | 45.4 > 18 | suppressed | **-65536 = 0xFFFF0000** |
| `quick` | 16, `s1` clear (43.6 > 22.36) | 43.6 > 18 | suppressed | **-65536** |

Three for three. **[proven]**

### 9.3 The arrest - `RASHCDG 0x80096F30(cop, suspect, 9)` **[established]**

Preconditions (`0x80096F54..0x80096FD8`): the arresting entity's `handle < game_state+0x30` (it is a
**player**); its `riderDef[0x01] & 0x0F == 2` (on a police bike); `rider->f25C < 2` (still on the
bike); `!(rider->f228 & 0x8000)`; `*(u32*)0x8005AD48 == 0` (the arrest FSM is idle); and the
suspect's `riderDef[0x28] == 0`.

```c
if (suspect.handle == game_state->f06) {                          // the designated suspect
    if (game_state->raceType == 33) { suspect.riderDef[0x27] = 248;    // 0x80097008
                                      suspect.riderDef[0x28] = raceClock; }   // 0x8009701C
    else                            { suspect.riderDef[0x27] = 252;    // 0x80097020
                                      0x8001B244(0); }                        // camera 0
} else if (game_state->raceType == 33) return;
else if (--*(s32*)0x8005AD44 <= 0)  cop.riderDef[0x27] = 253;      // 0x80097060  quota exhausted
0x80097308(suspect);
```

then the stun (`0x80097164..0x800971F4`): animation, sound (`speed > 8.9408 ? 4 : 1`),
`*(0x8005AD48) |= 2` (`0x800971B8`), and the suspect's `+0x1E0`, `+0x1E4`, `+0x240`, `+0x39C`,
`+0x1CC`, `+0x1D0`, `+0x1C8` zeroed with `+0x2D0 = 0x20000`.

So **`game_state+0x06` is the handle of the designated suspect** and **`game_state+0x07` is the
arrest quota**; both are read by `RASHCDI 0x80065930`/`0x80065940` and the quota is copied to
`0x8005AD44` (`0x80065938`), which is what the HUD quota box draws (`RASHCDG 0x800626B4`,
`0x800627C0`, `0x800628BC`).

**Three triggers** feed `0x80096F30`:

**(A) proximity while riding** - `RASHCDG 0x8009130C..0x8009140C` (inside `0x80090D84`):

```c
if (!(game_state->raceType & 1) || suspectClass == 2 || raceType == 33) return;
dx = |cop->fBA - s->fBA|;  dz = |cop->fC2 - s->fC2|;              // 0x80091350..0x8009137C
order them; d = max*(1 - 1/32 - 1/128) + (1.5*min)*(1/4 + 1/64);  // 0x80091394..0x800913D0
if (cop->speed > 0x0008F0D8 /* 8.9408 */ && d < 3)                // 0x800913C8, 0x800913F0
    Arrest(cop, s, 9);
```

**The arrest radius is 3 world units and the cop must be moving faster than ~8.96. There is no
"stay close for N seconds" timer.**

**(B) stopped alongside** - `RASHCDG 0x80097388`, called from `0x800B8564`: requires
`rider->f25C < 3`, `!(rider->f228 & 0x40)`, **`entity+0x1E0 <= 8.9408`** (the player is nearly
stopped, `0x800973E0`) and `riderDef[0x27] != 255`; then for every pool-0 entity
`0x80097470(me, other)` runs the combat reach test (`0x8009DB58` / `0x8009E444` / `0x8009DBA0`) and a
hit becomes an arrest.

**(C) collision** - `RASHCDG 0x800BF848` (inside `0x800BF674`, from `0x800C1B78`).

### 9.4 The arrest FSM - `*(u32*)0x8005AD48` **[established]**

Dispatcher `RASHCDG 0x80096818` (`jr` through the 16-entry table at `0x8005B87C`, index
`value - 1`), called from `0x8009562C` for the player only and only when `game_state+0x04 & 1`. The
values are powers of two:

| value | arm | what it does |
|---|---|---|
| 1 | `0x80096868` | camera 4, `*(0x8005B2EC) = 0`, assign the cop its target via `SLUS 0x800138E8` (`0x800968E0`) and the place comparison at `0x800968F4..0x80096944`. Set by `RASHCDI 0x800668C4` |
| 2 | `0x80096B78` | class 2 required; when the current animation index `>= 3`, `state = (state & ~2) | 4` (`0x80096C1C`) |
| 4 | `0x80096C20` | `*(0x8005B2EC) += dt`; after `*(0x800530F0 + 4*bank) >> 1` = **2.5** the bit clears; then if `riderDef[0x27]` is 252 or 253 pick a camera and set bit `0x10`, else set bit `8` |
| 8 | `0x80096D4C` | a second 2.5-unit timer, then clears itself |
| 16 | `0x80096DC0` | terminal - the mission is closed |
| other | `0x80096DE4` | `state &= ~7` |

Read back by the HUD (`RASHCDG 0x80062F8C`: class 2 and `state & 0x1E` draws the extra light sprite,
the index chosen from `riderDef[0x27] in {248, 252, 253}`), by `SLUS 0x800198C8` / `0x8003D9D8`, and
cleared by `main` at `0x80012160`/`0x80012174`.

### 9.5 The cop's driving **[established]**

A released cop runs the same per-frame AI as any racer (`0x800954A0`, `0x80093ED4`, `0x8009432C`,
`0x800B8020`, `0x800BD4D4`). **The single divergence is the target-speed function
`RASHCDG 0x80095BF8`**, at `0x80095D24..0x80095D3C`:

```c
if ((riderDef[0x01] & 0xF) == 2 && (entity->f3A0 & 0x10)) {
    if (game_state->f39 >= 4) target = *(0x80053030 + 4*bank);        // 18.0   0x80095D6C
    else {
        t = game_state->raceClock - (riderDef[0x3E] << 8);            // time since release
        a = t/30 - hi16(*(0x80053144 + 4*bank));                      // minus 30.0
        if (a > 0) {
            k   = a / hi16(*(0x80053150 + 4*bank));                   // steps of 10.0
            v   = (k + 1) * *(0x8005315C + 4*bank);                   // 10 per step
            cap = (128 - v) * *(0x800D86F8) / 128;                    // 0x80095E0C
            if (cap > *(0x80053138 + 4*bank)) clamp;
            entity->f22C->fE0 = cap;                                  // 0x80095E64
        }
    }
}
```

i.e. **the cop's speed ramps up with time since release** (in steps after the first 30.0 units),
capped from `*(0x800D86F8)` (60.35 in the savestates), whereas a racer's target comes from the
place-based rubber band (`0x80095F78`, tables `0x8005303C` / `0x80053054`). There is no
target-follow routine: the cop is spawned directly onto the player's road 120 units ahead and simply
accelerates.

Two further divergences: the wake-up animation set (`0x80094F70..0x80094F88` picks
`0x800C4550(4, ...)` and `rider->f25C = 0` for a cop, `(11, ...)` and `= 1` otherwise) and the light
sprite (`0x80062F6C..0x8006301C`).

### 9.6 Jailbreak: the phase machine **[established]**

`game_state+0x39` is the Jailbreak phase 0..5, advanced at `RASHCDG 0x800B9E34..0x800B9E94`:

```c
if (trigger & 2) {
    m = hi16(*(u32*)(0x80053174 + 4*game_state->f39));    // milestone: 2025 / 1975 / 1925 / 700 / 1200
    d = distAlong - (m << 16);
    if (sign(dir) == sign(*(0x8005B2E8)) && sign(dir) == sign(d)) {
        game_state->f39++;                                // 0x800B9E90
        0x800C8D4C();                                     // arm the new phase
    }
}
if (game_state->f39 == 1) 0x800C92F8();                   // per-frame phase-1 update
```

Phase arms, dispatched by `RASHCDG 0x800C8D5C` through the 5-entry table at `0x8005BD60`:

| phase | arm | what |
|---|---|---|
| 1 | `0x800C8DC0` | `expired = *(0x8005ACC8) < game_state->raceClock`; on expiry `riderDef[0x28] = raceClock`, **`riderDef[0x27] = 250`** (`0x800C8E0C`/`0x800C8E18`); calls `0x800C9420(!expired)` to stage the escape scene |
| 2 | `0x800C8EB0` | `player->f230 |= 0x28000000`, sound `{2, 224}`, `riderDef[0x45] = 64` |
| 3 | `0x800C8FE8` | flag reset, a class-dependent branch |
| 4 | `0x800C9194` | **`*(0x8005ACC0) = 0` and `*(0x8005ACC4) = 0` - the police are switched off**; every bike of the other class is silenced |
| 5 | `0x800C9280` | `riderDef[0x28] = raceClock`, **`riderDef[0x27] = 249` (`0x800C92A8`) - this is ESCAPE** |

So **jailed/busted = code 250** (the escape deadline expired in phase 1, or the general race time
limit) and **escape = code 249** (the last milestone, `along 1200` on road 27, was passed). The
deadline `*(0x8005ACC8)` is the race time limit of section 4.3.

### 9.7 `DATA\STARTJBA.BIN` - decoded **[proven, structurally exact]**

Read by `RASHCDI 0x80068740`, gated on `game_state+0x04 == 44` (`0x80068798`). The name is built by
taking the resident path `DATA\STARTDFA.LST` at `0x80052414`, finding the `'.'`, stepping back three
bytes and writing `'J','B','A' + i` (`0x800687F0..0x80068808`). The outer loop runs `i = 0..1`
(`0x80068998`) but the file is **opened only on `i = 0`** (`0x80068804`), and the read cursor is not
reset, so the second iteration continues parsing the same file. Hence:

```c
struct EscapeBlock {
    u32 count[5];                                   // five groups
    struct { i32 packed; i32 lateral; i32 along; } rec[sum(count)];   // this order IN THE FILE
};
// in RAM the record is 12 bytes as { lateral, along, packed }
//   (0x80068940 -> +8, 0x80068954 -> +0, 0x80068968 -> +4)
```

`*(0x8005B208 + 4i)` = the total, `*(0x8005B340 + 4i)` = a `malloc(12 * total)` array
(`0x800688D0`, `0x800688DC`).

The 556-byte file is **two blocks back to back**: block A (words 0..52) `count = {6, 0, 0, 2, 8}`,
16 records; block B (words 53..138) `count = {6, 12, 0, 3, 6}`, 27 records.
`5 + 16*3 + 5 + 27*3 = 139` words = 556 bytes exactly.

`packed` is unpacked by `RASHCDG 0x800C94BC..0x800C9504`:

```c
angleDeg = sign_extend12(packed & 0xFFF);     // -(deg << 12)/360   0x800C94D0
sub      = (packed >> 12) & 0xF;              // sub-group slot     0x800C94F4
road     = packed >> 16;                      // road id            0x800C9504
```

`along` is negated when `(*(0x8005B38C))->f16C < 0` (`0x800C9500..0x800C9514`) - the same sign
convention as `STARTDF?.BIN`.

Block A group 0 is **the six-bike Jailbreak grid**: `(road 17, lateral 2.3, along -66)`,
`(17, 1.4, -90)`, `(17, 4.2, -90)`, `(17, 1.4, -94)`, `(17, 4.2, -94)`, `(17, 2.3, -98)` - two
lateral columns at 1.4 and 4.2 with a nose and a tail rider at 2.3. Group 3 is two records, group 4
is eight (a line across the road at +-10 degrees).

The consumer is `RASHCDG 0x800C9420(blockIndex)`, called only from phase 1 (`0x800C8E1C`) with
`blockIndex = !timeExpired`; it dispatches on the group number (`0x800C954C..0x800C9580`): group 0 ->
`0x800C9584` (motorcycles, walking pool 0 from `*(0x8005B3A0)` with stride 1096 and skipping class
0), group 1 -> `0x800C96E4`, group 3 -> `0x800C9B88`, group 4 -> `0x800C9D10`, group 2 skipped. The
world anchor is **not** taken from `packed` but from the milestone:
`road = lo16(*(0x80053178)) = 28`, `along = hi16(*(0x80053178)) = 1975` (`0x800C9650..0x800C9664`).

So the "Jailbreak start" is an **escape scene on road 28 near distance 1975, six bikes instead of the
usual 14/16/18, plus groups of extra objects** - a different container, a different record order and
a milestone anchor instead of `[START]`. `STARTJBB.BIN` is not on the disc; the second block lives
inside the same file because the cursor is not reset.

### 9.8 Bust / jailed / escape, end to end

The race side writes `riderDef+0x27` (codes in 7.1) and `riderDef+0x28`; `RASHCDG 0x800B941C..
0x800B9608` builds the "everyone is done" mask from `riderDef[0x28] != 0 || riderDef[0x27] >= 248`
(`0x800B9548..0x800B9570`) and then sets `game_state+0x00 = 6` (`0x800B9600`) or `2` (`0x800B9604`).

The classifier `RASHCDG 0x800B9958` assigns the codes:

| condition | code | at |
|---|---|---|
| race type 36, `riderDef[0x27] < 247`, `raceClock <= *(0x8005ACC8)` | 251 | `0x800B9FAC` |
| race type 33 | 250 | `0x800B9FC4` |
| `raceType & 1` and the bike is class 2 | 250 | `0x800B9FEC` |
| `game_state+0x39 == 1` | 250 | `0x800BA004` |
| `game_state+0x39 == 2` | 254 | `0x800BA00C` |
| `raceType & 4`, type 44, `raceClock > 0x34BC0` (216000 ticks = 720 s) | 250 | `0x800BA050` |
| `raceType & 4`, other types, `raceClock > 0x1A5E0` (108000 ticks = 360 s) | 250 | `0x800BA068` |

The frontend maps them to screens (7.3) and to prize-table rows (7.4).

HUD side: `ARREST.TIM` is art 0 / element 1 `kDashArrest`, parked off-screen at (600, 600) in
`DASH1P.CSV` and moved on demand; `ARRESTS.TIM` is art 1 / element 2, the arrest *counter*; element 3
is the quota box; element 103 is the arrest message. **There is no BUST/JAILED/ESCAPE sprite in the
HUD** - those words come from `GAMESTRG.LOC` ids 28/29 and 15..25 through the pre-render at
`RASHCDI 0x8005EC20` and are drawn into elements 100/101/102 (`kDashMsgLeft/Right/Center`) and 103.
The screens themselves are frontend resource records 20 (`bust.str`), 23 (`wreck.str`),
24 (`jailed.str`), 25 (`escape.str`), and the music is `busted.wve` / `jailbrak.wve` (pointer array
`0x80089754` / `0x80089760`).

---

## 10. The HUD, as a rules readout

Layout comes from `DATA\DASH1P.CSV` (1 player), `DASH2PS.CSV` / `DASH2PH.CSV` (2 players); the
parser `RASHCDI 0x8005F104` is **purely positional** - it never reads the name column, so the row
order *is* the index. `DASH2PV.CSV` is named in `RASHCDI` (`0x8005B72C`, used at `0x8005FB50`) but
**is not on the disc** - a precise negative: the vertical split was planned and the asset never
shipped.

Elements a rules implementation must feed, with the code that computes them:

| element(s) | value | function |
|---|---|---|
| 45/46 `kDashRaceRank0/1`, 47 slash, 48/49 `kDashNumRacers0/1` | the place from `SLUS 0x800138E8`, and `*(0x8005B1FC)` | `RASHCDG 0x800606F0` |
| 10..12 `kDashSpeed0..2` | `entity+0x1E0 >> 8`, forced to 0 while the rider is in states 3/4 | `RASHCDG 0x80060178` |
| 13..16 odometer | `*(0x8005B380 + 4p)`, refreshed every 8th frame | `RASHCDG 0x800603E4` |
| 18..23 / 32..37 health bars | `(def[0x0F] << 7) / (def[0x0D] + 1)` and `(def[0x25] << 7) / (def[0x24] + 1)`; colour by thresholds 33/65/97 | `RASHCDG 0x80060C10`, `0x80061A00` |
| 24 weapon icon | `def[0x2E]` into the art table at `0x800D4860` | `RASHCDG 0x8006132C` |
| 53..80 clock | `remaining = *(0x8005ACC8) - (game_state+0x10 - *(0x8005ACD0))`, divided by 30 | `RASHCDG 0x80062C40` |
| 52 countdown digit | the countdown timer of section 3.4 | `RASHCDG 0x8005FF84` |
| 81/82/83 sign | the wrong-way / turn mask of section 6.6 | `RASHCDG 0x8005FD94` |
| 95..99, 104 radar | distance to the target; gated on `game_state+0x04 & 4` | `RASHCDG 0x80061F6C` |
| 1..7, 50 | arrest / arrests / quota / TKO counters | `RASHCDG 0x8005F030`, `0x80062610` |

**There is no lap element and no cash element**; money appears only on the frontend's post-race
screen.

---

## 11. The race loop skeleton a native port should implement

```
LoadRace(mode, venue, raceId):
    game_state.raceType = (mode == 32) ? venueRaceType[venue] : (mode & 0xFF)
    game_state.env      = venueEnv[venue];  game_state.bank = venueBank[venue]
    game_state.raceId   = raceId;  game_state.numPlayers = (mode & 0x10) ? 2 : 1
    variant   = (raceType & 4) ? 2 : (raceType & 1) ? 1 : 0
    timeLimit = (raceType & 1) ? limitTable[numPlayers>1][bank] * 300 : 0     // ticks
    parse ROADGRF route; build the grid from STARTDF?.BIN / STARTJBA.BIN
    build 20 rider records from LEVEL<bank+1>.BI + the session opponent list
    every racer: riderDef.flags0 |= 0x60   // frozen + arm the countdown
    game_state.raceClock = 0;  state = 1

Frame(ticks):                                 // ticks = elapsed 1/300 s, clamped to [1, 30]
    PollPads()                                // may set state 2/3/4
    if (state == 1):
        dt = 218 * ticks
        game_state.raceClock += ticks
        if (!TickCountdown(dt)) { draw; return }      // 3.00 s, frozen grid
        for each entity: integrate physics, road traversal, junction validation
        for each entity: entity.progress = DistanceToFinish(entity)      // 0x8003B61C
        for each racer:  FightUpdate(racer, neighbour)
        for each cop:    PursuitUpdate(cop)
        for each racer:  if (FinishTest(racer)) EndRace(racer, ComputePosition(racer))
        if (timeLimit && raceClock - raceStart > timeLimit) EndRace(all, timeout)
        for each player: ViewUpdate(view[p], dt)
    UpdateHUD()
    Draw()
    if (all players' riderDef.finishTime != 0) state = 5
```

The per-frame *order* above follows `GameFrame` (section 3.2) and `RaceStep` (section 3.3); the
physics, AI, draw and audio sub-steps are out of scope for this document.

---

## 12. Verification

The oracle results of this document come from an independent Python probe over the three savestates
and the 14 frame dumps (59 checks, 0 failures) and the per-race tables it produced. The ones a port
most needs to keep true:

* race 4 of road set 1 in plain terms (5, worked example): mode, venue, race-type byte, difficulty row,
  time limit, countdown, cop parameters, the prize ladder and the live 20-rider table;
* `game_state+0x04 == 0x22` for venue 0 of the career in all three savestates (2.3);
* the bike-index list equal to the 21 bike `.PH` files (1.3);
* the places of the live racers an exact permutation of `1..n`, the police at `n_all + 1` (1.4);
* the progress formula against 14 frames + 3 savestates, 280/302 exact, every miss a parked police
  bike (6.2);
* no monotone BSS word across the 14 frames that could be a lap counter (6.7);
* the `[START_CHECKER]` / `[FINISH_CHECKER]` census of all 100 races (6.1);
* `FIGHT.BIN` relocated in RAM exactly as the loader describes (8.1).

The ported functions of sections 15 and 16 are accepted by the `rrverify` rows named there.

---

## 13. Unknown / not established

**The mode and session layer**

* **Which field selects the road set** for the `race%d_%ld.stp` name. `EXE 0x80023714` uses
  `game_state+0x30`, which section 1.1 shows is the player count. Either the name builder is wrong
  about its own first argument or a two-player game really does load set 2. The available captures are
  all one-player, so they cannot discriminate. This needs the interpreter or a two-player capture.
* `game_state+0x06`: the race-id function (section 2.5) and the designated-suspect handle
  (section 9.3) are two different uses of the same byte and it is not clear how they coexist; the
  same for `+0x07`, which is both a per-venue byte and the arrest quota.
* `game_state+0x08`, `+0x38`, `+0x44`, `+0x58`, `+0x5C`, `+0x60` and the word table at
  `RASHCDF 0x80099518` (values 18/19/20 per venue): read once each, meaning unknown. `+0x48`/`+0x4C`
  are written twice on the same path - from the per-player bike index at `0x8007F59C` and from the
  venue table at `0x8007F6A4` - and only the bike-index reading is confirmed by RAM.
* **The writer of the per-player result code `0x800D81F8`.** Not found in any of the four images by
  a store scan; it goes through a pointer.
* **The combat bonus arithmetic.** The race bonus is the prize table of 7.4, but the function that
  turns hits / takedowns / steals into money was not located, and neither was the persistent balance
  field in the session record.
* Where `s3` (the frontend's 1/2/3/4 outcome category) becomes a concrete screen id: the arms at
  `RASHCDF 0x8007BF6C`, `0x8007C014`, `0x8007C01C` were not read, and the resource ids
  `0x020F` (`bust`), `0x0213` (`jailed`), `0x0214` (`escape`) were found in the table but not in the
  code that selects them.

**The race loop**

* `0x8005B228`, `0x800CD540`, `0x800CD542` - written on the first race frame (`EXE 0x80012394`),
  purpose unknown.
* `0x800881B4` is called with `(0x800CD898 + 1132p, dt)` and the block is indexed by entity handle
  at `RASHCDG 0x8009E6C0`. It is the per-player view/camera record; its 1132-byte layout is not
  decoded, and it overlaps the address `population.md` gives for pool 4 (`0x800D1818`) if more than
  two players existed.
* `RASHCDG 0x8008CFDC`, `0x800C89A0`, `0x800C8B24`, `0x800C8CD4`, `0x8008D56C`, `0x800A2138` - the
  per-frame calls of `GameFrame`, identified by position only.
* The periodic event that `RASHCDG 0x8008AB00` fires through `0x800B8018(0)` every half second, and
  the meaning of the per-(variant, level) intervals in `0x80052FAC` / `0x80052FD0`.
* The thresholds at `RASHCDG 0x800B9AE0` / `0x800BA050` / `0x800BA068`: for race type `0x2C` the
  race clock is compared against `0x34BC0` (216000 ticks = 720 s) and otherwise against `0x1A5E0`
  (108000 = 360 s). Plausible for a scripted mission, but the field was not confirmed to be in ticks
  at that site.
* `RASHCDG 0x8003A5F4` (the "road ahead" test in the cop spawner) was not disassembled; its reading
  as "remaining road ahead" is a **[guess]**.
* `RASHCDG 0x80095410` (the `s8` term of the spawner and the off-route gate of the wrong-way test)
  reads `entity+0x1AC` and the bitmask at `+0x76` shifted by the handle; its meaning is
  **[probable]**.
* Where the `& 2` trigger that advances a Jailbreak phase (`RASHCDG 0x800B9E2C`) comes from.
* The per-player HUD-message record at `0x800D5898 + 1132p`: `+0x228` bit values 0x10/0x12/0x14 and
  `+0x308` message ids 3/6/8/10/12 were observed but not resolved to text.
* Escape-scene groups 1, 3 and 4 (`RASHCDG 0x800C96E4`, `0x800C9B88`, `0x800C9D10`) - which kind of
  entity each one places.
* `0x8005ACC4`, the second police switch (written by `RASHCDF 0x8007F348`, read by
  `RASHCDG 0x8009D000`).
* `STARTJBA.BIN`'s `packed` road ids (17, 28, 30, 1, 2, 3) were checked structurally against the
  unpacker but never against real road geometry - there is no Jailbreak savestate.

**Progress and the finish**

* The geometric identity of `*(entity+0x154) + 0x0E` in the wrong-way dot product: it is a unit
  3 x s16 vector inside the 52-byte road-slice record, but a correctly-oriented bike gives a dot of
  -0.626 with its forward, not +-1. The mechanism works (the threshold is never crossed) but the
  exact vector is not identified. Needs an interpreter run with a deliberately reversed bike.
* `0x8003BC48(node, road, out, 3)` - which of the two returned roads is "left" and which is "right".
* Bits 1 and 7 of the `0x8003C590` mask are not read by the sign code; their consumers in
  `RASHCDG 0x8005E840` were not followed.
* The in-intersection branch of `0x8003B61C` is derived structurally but never exercised: no racer
  is inside a node in any of the 17 captured frames.
* 26 races of set 1 and all 36 of set 2 have `[FINISH_CHECKER] = -1 -1 -1`. That they genuinely have
  no finish banner was not verified against a frame.
* Result codes 249, 250 and 251 have frontend consumers but **no writer was found in `RASHCDG`**.

**Combat**

* The physical pad-button mapping behind the 9 button pairs at `RASHCDG 0x800CCB78` (the indices go
  through `input[0xB8]` into a remap table).
* `riderDef+0x3C` as an enumeration: the classes (`& 0x40` = 71..77, `& 0x20` = 32..38, 142..147)
  are known, 32 = fist and 142 = weapon, but the individual values are not named.
* `riderDef+0x23C & 0x20` ("I am a passenger", records 20..39) - which menu option enables it.
* `FightRec+0x03 (countB)` has no reader in the code that was read; the identity
  `countB * 12 == sizeof(blockB)` holds for 39 of 40 records (record 39's block is padded).
* `RiderState+0x04`/`+0x06`, `riderDef+0x10..0x23`, `riderDef+0x2C` bits 9 and 10 (see the
  name-id / weapon-mask contradiction in section 1.4), and where `def[0x0F]` is restored from
  `def[0x0E]`.
* The `DMD3` payload is still undecoded (`rmd3.md` section 8, item 6); `clip = RiderState.anim >> 4` is
  **[established]** but not confirmed by rendering.
* **No capture contains a fight.** All 306 rider samples are idle, so the combat runtime fields have
  never been observed changing.

**Strings and screens**

* The `u16 flags` of the `RASHCDF` resource record; the observed static values
  (0x0000/0x0001/0x0002/0x000C/0x0010/0x0011/0x0019) are not decoded.
* `.PFN` font format (out of scope here).
* The `*.TIM` CSV rows' third column lands in a `u16` at record `+6` and is 0 in every shipped file;
  columns 4 and 5 are never read.
* The behaviour of the missing `DASH2PV.CSV` branch (`RASHCDI 0x8005FB40`).

---

## 14. Where the sibling documents take this further

| topic | where |
|---|---|
| the session bitmaps' setters and clearers, the venue-advance arms | `frontend.md` 7.3, 7.4 |
| the six player records, the save layout | `frontend.md` 7.2, 8.4 |
| the menu codes that install modes 8, 17 and 24 (Side Car Co-op, Cops & Robbers, Side Car Versus) | `frontend.md` 11.1 |
| the logo widget that maps a mode to its `mode*.str` screen | `frontend.md` 4.1 |
| the resource records 30..36 that name `.psh` files absent from the disc | `frontend.md` 5.2 |
| the runtime intersection record's packed `routeRoad[]` / `nextNode[]` | `road.md` 1.3 |
| the `[START_CHECKER]` / `[FINISH_CHECKER]` fields as stored by the parser | `road.md` 1.2 |
| `entity+0x168` as packed `u16 id; u16 kind`; the 20 runtime rider records; the police spawner as the procedural kind-0 spawner | `population.md` 1.2, 5.2, 2.2 |
| the `kDash*` CSV columns 4 and 5 | `textures.md` 3.1 |

---

## 15. The game modes in the product

What a mode started from the front end gets in the product, transcribed from our own disassembly (hashes
in the header). Ported functions are `src\game\sim\modes.{h,cpp}`, each accepted by one row of
`tools\rrverify\rows_modes.inc` on two seeds (1160 cases each, 0 mismatches); the loader's per-race-type
setup and the session's side are `src\game\race_modes.{h,cpp}`.

### 15.1 Ported (bench rows)

| function | row | what |
|---|---|---|
| `RASHCDG 0x80096F30` Arrest | `mode_arrest` | 9.3 as read; callees SpeechCue, `0x8009D664` (seam), ViewEvent, the stance event, the AI stack |
| `0x80097308` Stun | `mode_stun` | the arrested bike: place 254, stamp, speeds zeroed unless stance 72/73 |
| `0x80096818` ArrestFsm | `mode_arrest_fsm` | 9.4; AiDrive's player-cop arm (`ai.h AiPlayerCopArm`) |
| `0x8009DA4C` JailTest / `0x800A0708` JailRelease | `mode_jail_test` / `mode_jail_release` | CopIdle's quota arm (cops.h) |
| `0x800C8D4C` MilestoneAdvance / `0x800C92F8` MilestoneFirst | `mode_milestone` / `mode_milestone_first` | 9.6's five phase arms, FinishTest's callees |
| `SLUS 0x80013B90` ClockDigits | `mode_hud_clock_digits` | seconds x 256 into five digit items |
| `0x80063530` RaceClock, `0x800636F0` Splits | `mode_hud_race_clock`, `mode_hud_splits` | Time Trial's clock and three splits against the race's record |
| `0x80062C40` CopClock, `0x8005FAC4` SuspectName, `0x80062610` Arrests, `0x80062F34` ArrestMessage | `mode_hud_cop_clock`, `mode_hud_suspect`, `mode_hud_arrests`, `mode_hud_arrest_msg` | the Five-O HUD (mission clock, suspect, quota box, the arrest lines) |

The `hud_frame` row runs these six HUD elements natively (they are not on its seam list) and passes both
seeds.

### 15.2 The loader's part (transcribed, race_modes.cpp)

* **SetUpRace `RASHCDI 0x80063670`**: the clock triple `0x8005ACC8/CC/D0 = 0`; race type bit 0: the limit
  `0x80053084/90[bank] * 300`; type 36: `gs+0x08 * 300`; 33: `*(0x8005ADE0) * 300`; 44: `*(0x8005ADE4) * 300`.
  Five-O on bank 0 measures 54000 ticks (180 s). Then the start-position record `0x800CF578` =
  `*(*(0x800D6184) + 0/4/8)` (SetUpRace's `0x8006ACD0`; its two road lookups are not run).
* **BuildRace, type 44** (`0x80069858`): phase `gs+0x39 = 0`, `gs+0x0A = 0`, the police off, the direction
  `0x8005B2E8` from milestone 0.
* **The commit's Time Trial switches** (`RASHCDF 0x8007F37C`, mode 4): traffic `0x8005ACC4 = session+0x0A`,
  police `0x8005ACC0 = session+0x0B`.
* **SpawnBike, race type bit 0**: a player gets `+0x230` bit 27 (`0x80066248`); player 1 on a class-2 bike
  gets arrest word `|= 1`, `PlayerCopPlace 0x8006581C` (Rand bit: keep the lateral, `0x8005B2B0 = 0`, the
  ~9.7 degree turn `0x800656A0` and command 1; else the road piece's edge, `0x8005B2B0 = 1`, command 4), the
  quota `0x8005AD44 = gs+0x07` and the suspect's bit of `0x800D8708`; **every other bike starts on command 4
  in these races** (`0x80066BE8`: the loader's command 1 is used only outside race type bit 0).
* **BuildGrid's entry count** (`0x80067C1C`, `0x80067EB4`): race type bit 2 - `gs+0x38` is the race-options
  bitmask the commit builds (bit 0 opponents, bit 1 police; 3 for the career): 0 -> the player alone (two
  with bit 3), 2 -> the player and two police entries (slots 17, 18), otherwise the block's entries. A race
  the front end starts gets the block's whole field, as in the original.
* **The HUD loader's clocks** (`RASHCDI 0x80060454..0x800605BC`, hud_arena.cpp): the record time into
  items 60 / 67 / 74 (type bit 2), the limit into 53 (44 / 36).

### 15.3 Per mode, started from the menu (measured in the product)

* **Five-O** (Solo -> Five-O -> Race; race 38, type 1, COP1.PH, 17 bikes): the player cop rides behind the
  field under AI control (FSM 1) until it is last, then the pad has the bike and the mission clock starts
  (`0x8005ACD0`); the HUD shows the clock, the suspect ("Gr Mongo", handle 9) with the radar distance, and
  the quota box. Arrests reach Arrest from the knock-off (4 calls in a scripted run, correctly refused while
  the FSM was still in state 1). Run out of time: **result 250 at 56990 ticks**, state 6, the results scene,
  `FiveOResult 0x8007E2C8` -> screen 28 -> the Five-O hub.
* **Time Trial** (Solo -> Time Trial -> Race; race 56, type 4, options 3 -> the full field): the race clock,
  the record time and the splits run; the autosteered player finished 16th at 51725 ticks, state 6, the
  results scene, the dispatch -> **screen 52, the new-record name keyboard**.
* **Career venue 0** (race type 0x22): the full field. Venues 1 (0x24: the 6:00 limit of FinishTest),
  3 (0x21 = 33: the player cop against one designated suspect, success 248, limit `0x8005ADE0`) and
  5 (0x2C Jailbreak, section 16) run through the same code but need career progress to be chosen.
* **Head to Head / two-player modes**: the shell's player-2 setup screen waits for pad 2; the drama
  director is part of the ported AiPlan and runs whenever `gs+0x30 == 2`.

### 15.4 Not ported

The arrest's police car and shot `0x8009D664`, the ambient voices' hold `SLUS 0x80020E30` (named seams);
the TT records the shell edits are not handed to the race (the race arena's table is the disc's); no
scripted run yet shows a player catching a suspect end to end (the arrest chain is benched).

## 16. Jailbreak and the two-seat bike in the product

Images: RASHCDG.BIN SHA-1 `cfe43a7786759f2cb9c57751cf99e84d1074782c`, RASHCDI.BIN `9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06`
(both at 0x8005B5E8), SLUS_010.53 `67ed165a2c517d4e6106fb0dfa324d66dd9a76f1`. All from our own disassembly.

### 16.1 Ported (bench rows, `tools\rrverify\rows_jail.inc`, `src\game\sim\jail.{h,cpp}`)

| function | row | default seed (1160 cases) | `--seed 0xA5A5C3C312345678 --cases 2048` (2696) | `--mutate` |
|---|---|---|---|---|
| `RASHCDG 0x800C9420` EscapeScene(block) | `jail_escape_scene` | 0 mismatches | 0 | FAIL |
| `0x800C9E74` JailbreakFinish (AI op 2 for player 1, phases 1/2) | `jail_finish` | 0 | 0 | FAIL |
| `0x800CA05C` JailBoard(late) | `jail_board` | 0 | 0 | FAIL |

Seams of the escape scene in the product (`jail_session.cpp`): GetRCnt `SLUS 0x80043F00` is answered by a
deterministic counter of ours (named). The roadblock prop of group 4, PropAlloc4 `0x800A2630`, is PORTED
(world_pop.h, row `world_prop_alloc4`) and runs through `RaceSession::PopCall` - 6 props placed from block 1.
Phase 3's rider re-key `SLUS 0x800302C4` = ModelKeySet (PORTED, traffic_bind.h).

### 16.2 Transcribed (loader, `src\game\jail_session.cpp`)

* BuildGrid's two-rider arm `RASHCDI 0x80067EDC..0x800680D8` (TwoRiderGrid) and the animation bank set
  `0x80066414..0x80066594` (the short set + ANIMTBLJ / ANIMTBLS passenger bank for bit-3 race types).
* SpawnPassenger `0x800670FC` (BuildGrid `0x80068344..0x80068418`): a player on a sidecar index 6..8 / 15..17 gets the
  partner bike (pool 0 at the count, not walked by the pool-0 passes) and rider (pool 1), `+0x358` both ways, seated
  in seat 1 (`Attach(H, R, 2, 1)`) only when `gs+0x0A + handle` is set - in Jailbreak it is not: the partner is
  picked up at the jail.
* STARTJBA.BIN `0x80068740`: both blocks from the one file (16 + 27 records).
* **The sidecar rig** (`src\render\race_scene_sidecar.cpp`): LoadBikeBank `RASHCDI 0x8005C45C` loads, for a player on
  a sidecar index, `DATA\<name>.MRO` (name table `0x8006B51C` via `0x80064034`) through `0x8005C30C`: a LECT texture
  and the RMD3 model `100 + index` (CRUISES3.MRO = model 108, 6 parts, the whole bike + car). `rrformats ParseMro`,
  `rrtool geo` reads .MRO. The renderer draws that model in place of the bike model for a bike whose `+0xB4` is the
  index, both seated children on its SeatVertex seat, and a rider in seat 1 (`+0x40`) as the passenger with its own
  pose. The rig's level bank, palette and part slots are in 16.5.

### 16.3 Measured in the product

Start: menu -> Solo -> Jail Break -> Continue -> hub (venue 5) -> Race, i.e. `rrgame <disc> --jailbreak`;
handover set 1 race 30, type 0x2C, bank 2, CRUISES3.PH.
* 6600 frames autosteer: no fault; phase 0 throughout (the autosteered rider is at progress 5865 of ~7600 to the
  jail); the sidecar rig is drawn with its empty car.
* 30000 frames asked: at 37505 ticks the 125 s limit (`*(0x8005ACC8)` = `0x8005ADE4 * 300`) ends the race with
  **250 (jailed)**, `gs+0x00 = 6`, the results scene, back to the front end.
* `--start 6200`, autosteer: phase 0 -> 1 -> 2, EscapeScene staged block 1 (in time), 27 records; the AI takes
  player 1 ("Auto") off the road into the jail yard, where the autosteered line can meet the escape scene's
  parked police car (16.5) and the rider is thrown (mount 3) -> JailBoard(late) -> **254 (busted)** at 9585
  ticks, state 6, results.
* Normal races unchanged: set 1 race 1 and set 2 race 40, 6600 frames each, no fault.

### 16.4 Open

The product's page table `0x800D5F70` holds no ids (its renderer binds the sheets), so ModelKeySet `SLUS 0x800302C4`
in phase 3 answers -1 for keys 1..3 (the rider keeps its sheet; the a1 re-derivation it would do for a1 = 63 does not
arise). Two-player Side Car (24): the scene keeps player 2's own rig too.
`--autosteer` pursues the route's centre line: at the jail gate it can run into the escape scene's parked police car
(lat -5.2, anchor + 1.2) in phase 1, and on road 11 (race 30) into a median pole (pool-6 volume, class 1, road 11
along 5175 lat -1.47); `--autosteer-lane 3` drives a lane.

### 16.5 The level bundle, the rig's palette and parts, the escape end to end

**The level bundle** (`rrformats\level_bank.h`). `0x800635D0` calls LoadBikeBank `RASHCDI 0x8005C45C(gs+0x3C)`:
`a0 = min(a0, 2)`; race type 0x2C: `a0 = gs+0x48 < 9 ? 3 : 4`; name = `0x8006B4A4[a0]` = bblevel1 / bblevel2 /
bblevel3 / bblevJBD / bblevJBK, "DATA\" + name + ".GEO"/".TEX" through `0x8005CA10`. Applied to the model arena,
the rider boxes, the weapon arena and the cars' palette bank. Jailbreak (race 30, bike 8): `BBLEVJBD.GEO` models
102, 120, 150, 159 (+800). Negative control `RRJB_JBBANK=off`: `BBLEVEL3.GEO` again.

**The rig in the model arena.** LoadBikeBank's tail: per player with bike index 6..8 / 15..17 whose model
100 + index is not registered (`0x8005C010`), `DATA\<name>.MRO` (`0x8006B51C`, 9 bytes apart) through `0x8005C30C`:
the leading LECT to the uploader `0x8005DDB8`, the rest to the chunk walker `0x8005C0C4` (a3 = 1). The product's
model arena does this (`108@9 (DATA/CRUISES3.MRO)`), so the PORTED binder gives player 1's bike model 108
(BikeClassModel: class 8 -> 108). RegistryBind `SLUS 0x8002FDEC` mallocs the part array as group 0's part count x 24;
`JailRigParts` gives the bike its own 6 slots (OURS placement) before BindModels. Negative control
`RRJB_RIGBIND=off`.

**The palette.** `0x8005DDB8` kinds 1..3 return at once when the page table already holds the LECT id - and
`BBLEVJBD.TEX` carries id 112, byte-identical to `CRUISES3.MRO`'s LECT - so the rig samples the page of its
DOD3+0x1C. An 8bpp object's palette is its a1 (`+0x24` bits 12..17, the skin index of `textures.md` 1.3): SpawnBike
`0x80065A94` (race type even or 0x21, a player) sets the bike's a1 = `0x8005DD9C(p)` = byte `0x8006B898 + 4p + 3`,
the rider's a1 + 1; SpawnPassenger `0x800670FC` the passenger's a1 + 2. That byte is written by the **TSLP handler
`0x8005DBB8`**, which is also the run-time palette copy `textures.md` 1.3 describes: per player whose bike index is
below 18, class c = index < 9 ? 0 : 1, u = the player record's `+0x0A` (`lb`, 0x800D81D8 + 36p) or p in a two-player
race, three TSLP blocks from `u*6 + (c ? 3 : 0)` into the next CLUT rows (`0x8005DAD0`), the first row recorded. So
the rig's palette is TSLP block `6u (+3)`, its rim (prim.clut bit 7, RIMA1.TIM) the same, the passenger rider block
+ 2 (`jail_session.h PlayerPaletteBlock`). RegistryBind keeps a1 for kind 2; ModelKeySet's re-derivation from the
page record `+0x0A` applies only to a1 = 63. (TSLP block 0 = KNBP block 46 in BBLEVEL1.TEX, which is why binding
KNBP block 46 reproduces the player's bike for u = 0.)

**The rig's parts.** BikeInstance `0x80084E10` (fork 1, pitch 2) and BikeWheels `0x80066EC4` (wheels 3 / 4, and with
six parts slot 5 = RotMatrix(the passenger bike's +0x344)) run PORTED on the rig's six slots; the renderer re-poses
LOD 0 from them (`DrawRequest::sidecarLocal`). Jailbreak 6600 frames: 5682 frames posed, slot changes fork 985, pitch 4430,
wheels 5492 / 5500, sidecar wheel 5492; `RRJB_RIG=off`: 0 (rest pose).

**The obstacle in the jail yard is the escape scene's parked police car.** A frame log and a `--start 6400` run
agree: at road 17 along 1218 lat -7.6 the player (still driving himself: the AI takes player 1 only in phase 2,
`+0x230 |= 0x28000000` at `0x800C8EB0`; the trace shows no aim in phase 1) hits car 0 of STARTJBA block 1 group 3
(lat -5.2, anchor 17/1220 + 1.2) through the bike-vs-car reaction (`0x800AC958` / `0x800AF224`), is thrown, and
JailBoard(late) busts him (254). The car is the original's; steering into it is `--autosteer`'s line. With
`--start 6200` the player passes it.

**Escape end to end** (`--jailbreak --start 6200 --autosteer --autosteer-lane 3 --hold T`, 12000 frames): phase
0 -> 1 (f~1900, EscapeScene block 1: the partner placed at the jail, mount 3, stance 65, +0x23C 0x60) -> 2
(JailbreakFinish drives player 1 to the stop, 748 frames; at 0 speed the partner's top op is 18: he walks to the rig
and boards, mount 1) -> 3 (the player drives off, guards chase) -> 4 (road 11 along 1700) -> 5 (road 11 along 550):
**result 249 (escape) at 52925 ticks**, state 6, the results scene. Same without the lane: phase 3 ends on road
11's median pole (knock-off, the guards' phase-3 bust 254).

**Side Car from the menu**: Multiplayer (4 -> 29) -> entry 3 (screen 36, type 0x08): script
`2:g4;6:down;10:x;14:down;18:down;24:x;30:x`; entry 4 (screens 34/35, type 0x18, player 2 confirms):
`2:g4;6:down;10:x;14:down;18:down;22:down;28:x;34:x;40:p2x`. Both 6600 frames, no fault; the passenger seated
(gs+0x0A), the rig posed (CRUISES1.MRO model 106 from BBLEVEL1).
