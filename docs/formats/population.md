# Population - where a race gets its racers, police, traffic and pedestrians from
### (Road Rash: Jailbreak, USA, SLUS_01053)

This document covers everything that *lives* in the world while a race runs: the 18
motorcycles and their riders, the police, the traffic vehicles, the pedestrians, the
roadside props and the static collision volumes. It builds on `docs\formats\scene_cell.md`
(the cell object arrays and their spawner arms) and identifies the runtime writer of entity fields
`+0x158`/`+0x168`/`+0x16C`/`+0x170` and the 7584 "kind 6" records.

**Summary.**

1. There is one **entity pool table** at guest `0x800CE4D0` with **seven pools**, one global
   handle space `handle = (pool << 5) | slot`, and one shared "road position" header
   (`+0x158` lateral, `+0x168` road id, `+0x16C` direction, `+0x170` distance along) that
   every kind of live thing carries. The cell's five object arrays are indexed by exactly
   the same number: **the cell "kind" *is* the pool index.**
2. So the five cell arrays are, in pool order: kind **0 = motorcycles** (empty on disc),
   kind **2 = pedestrians** (353), kind **3 = traffic vehicles** (empty on disc),
   kind **4 = roadside props** (2833), kind **6 = static collision volumes** (7584).
3. **Kind 6 is the collision representation of the baked scenery.** `cls 1` (12175 of 13429) is a square-footprint
   vertical prism - a tree trunk / post / pole, median half side 0.71 and height 6.2 world
   units, sitting a median 14.8 units off the centre line. `cls 0` (1254) is a low
   axis-aligned box from two stored corners, median half side 1.27, height 1.47, 20.8 units
   out. They are instantiated into a 24- (or 32-) slot pool around the player and read by
   the crash-resolution code.
4. **Traffic vehicles and pedestrians are spawned procedurally at runtime**, not placed.
   The traffic scheduler (`RASHCDG 0x8009D0xx`) builds a *synthetic* placement record on
   its own stack from the road graph around each player and calls the same spawner the
   cells would have used. Its density/probability parameters are **hard-coded constants
   written by `RASHCDI 0x80068DE4`** (and a per-road table in the EXE), not level data - which is why `ROADGRF<n>.TXT` has no
   `[VEHICLE_DEFAULT_DENSITY]` key even though `RASHCDI`'s parser knows the string.
5. **The racers and the police are the same 18-slot pool.** Which bike model a slot gets,
   and how fast it is, comes from `DATA\LEVEL<n>.BI` - **27 records of 64 bytes**, one per
   rider definition. `bike_class` (`+0x05` low nibble) selects the three motorcycle models
   of `BBLEVEL<n>.GEO`; class 2 is the police bike.
6. **The starting grid is `DATA\STARTDFA.BIN` / `STARTDFB.BIN`** - 64 and 55 blocks of 292
   bytes, one per race of road set 1 and road set 2, each a list of
   `(rider, lateral, alongOffset)`.

Confidence marks follow `road_chunk.md`: **[proven]** = an exact identity over the whole
data set, or read straight out of the dispatching code *and* reproduced against live RAM;
**[established]** = read once out of our disassembly and holding over every sample;
**[probable]**; **[guess]**. Section 9 is the explicit unknown list.

Binaries cited (our own SHA-1 over `work\disc_us`):

* `SLUS_010.53`, `67ed165a2c517d4e6106fb0dfa324d66dd9a76f1`, text at `0x80010000`
  (file offset `f` -> address `0x80010000 + f - 0x800`).
* overlay `RASHCDG.BIN` (the race), `cfe43a7786759f2cb9c57751cf99e84d1074782c`, loaded at
  `0x8005B5E8` (file offset 0 = that address).
* overlay `RASHCDI.BIN` (the asset/level loader), `9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06`,
  same base.

Data cited: `work\disc_us\DATA\STREAM1.STR`, `STREAM2.STR`, the 99 `RACE<set>_<race>.STP`,
`LEVEL{1,2,3}.BI` (`LEVEL1.BI` = `1038ac997e96ce03bebada0dc7f0e7cc50f79346`),
`STARTDFA.BIN` (`29924c2a556bc88ae34d0a2fab6f194c4597c27c`),
`STARTDFB.BIN` (`c6a008d0448036ed3bb96976e63185d791a7492e`),
the 21 bike `.PH` files, `ROADGRF1.TXT`, and the four RAM images under
`work\oracle\state\` plus the 14 under `work\oracle\vr_capture\ramdumps\`.

The claims marked "checked in all four savestates" were asserted by an independent Python probe
over the four RAM images. The runtime that uses these structures is ported in
`src\game\sim\population.{h,cpp}` and accepted by the `rrverify phys` rows in
`tools\rrverify\rows_population.inc` and `rows_traffic*.inc` (0 mismatches over dump-derived and
randomised inputs).

---

## 1. The entity pool table - `0x800CE4D0`, 7 x 16 bytes **[proven]**

```c
struct Pool {                 // 0x800CE4D0 + 16*pool
    void *base;               // +0x00  first slot
    s32   stride;             // +0x04  bytes per slot
    s32  *live;               // +0x08  -> number of slots in use
    s32  *high;               // +0x0C  -> highest index in use (live - 1)
};
```

Read out of the pool walker at `RASHCDG 0x80095848`: `a2 = 0x800CE4D0`
(`lui v1,0x800d; addiu a2,v1,-6960` at `0x8009584C`), `v0 = *(a2+0x0C)`, `a1 = *v0`,
`a0 = *(a2+0x00)`, and the loop at `0x80095870`..`0x800958DC` advances `a0` by `*(a2+0x04)`
and decrements `a1` until it goes negative. The handle resolver at
`RASHCDG 0x800A5194`..`0x800A51EC` indexes the same table with `(handle >> 5) << 4`.

Contents, as read from all four savestates:

| pool | base (rr-race) | stride | what it holds | models seen |
|---|---|---|---|---|
| 0 | `0x801B65D4` | 1096 (`0x448`) | **motorcycle** - every racer *and* every cop bike, 18 slots | 100, 109, 118 |
| 1 | `0x801BB2EC` | 628 (`0x274`) | **rider** - the human on the bike, 18 slots, parallel to pool 0 | 150, 159 |
| 2 | `0x801B55D4` | 572 (`0x23C`) | **pedestrian / bystander**, cap 4 | 400, 430 |
| 3 | `0x800CF660` | 512 (`0x200`) | **traffic vehicle**, cap 16 | 301, 303, 306, 309 |
| 4 | `0x800D1818` | 596 (`0x254`) | **roadside prop**, grown on demand | 200 |
| 5 | `0x800D36EC` | - | never used: `*live == 0`, `*high == -1` in all four states | - |
| 6 | `0x801B3AC4` | 280 (`0x118`) | **static collision volume**, cap 24 (32 in 2-player) | none |

The model ids are read from each entity's registry pointer at `+0x60` (the model registry at
`0x800CE1B0`, 16 bytes per model) and are exactly what `rmd3.md` says: 100/109/118 are
the three motorcycles of `BBLEVEL1.GEO`, 150/159 the two riders, 400/430 the two bodies of
`PED01A.GEO`, 300..315 the traffic cars, 200 the `HAZARD<n>.GEO` prop bundle **[proven,
checked in all four savestates]**.

### 1.1 The global handle - `handle = (pool << 5) | slot` **[proven]**

Every entity of pools 0..5 keeps a `u16` handle at **`+0xAC`**; a pool-6 entry keeps it at
**`+0x00`**. The bases are written by each pool's own allocator as `slot + 32*pool`:

| pool | allocator | handle literal |
|---|---|---|
| 2 | `RASHCDG 0x800CBA20` (`addiu v0,a2,64`) | `0x40` |
| 3 | `RASHCDG 0x8009AE60` (`addiu v0,a3,96`) | `0x60` |
| 4 | `RASHCDG 0x800A26xx` (via `0x800CD6C8`) | `0x80` |
| 6 | `RASHCDG 0x8009BC44` (`addiu v0,a2,192`) | `0xC0` |

and the resolver at `0x800A5194` splits it back: `pool = handle >> 5`, `slot = handle & 0x1F`,
with pool 6 taking a special arm (`beq v1,6` at `0x800A518C`) because its base lives in its
own control block rather than in the table. `handle == (pool<<5)|slot` holds for **every live
entity of every pool in all four savestates**
(0 is also allowed: a released slot still inside the high-water mark, which every allocator
skips - `0x8009AE1C`, `0x8009BBFC`, `0x800CB9C8`, `0x800A26B8`).

### 1.2 The shared "road position" header **[proven]**

Pools 0..5 share a header. The fields that matter for population:

| off | field | evidence |
|---|---|---|
| +0x00 | current `DOD3` (the LOD group being drawn) | the model draw path |
| +0x60 | model-registry slot pointer | `0x8002FDEC` stores it |
| +0x64 | per-pool class descriptor in the resident EXE: `0x80054160` traffic, `0x80054178` bike, `0x80054198` rider, `0x800541B8` pedestrian | read from RAM, one constant per pool |
| +0xA8 | link to another entity (target / owner) | not decoded |
| +0xAC | `u16` handle (1.1) | `0x80095878` (`lhu v0,172(a0)`) |
| +0xB8 | **8 oriented-bounding-box corners**, 3 x 16.16 each, ends at `+0x117` | 1.3 |
| +0x140 | `u16` state | `0x800958AC` |
| +0x158 | **lateral offset from the road centre line**, 16.16 | `0x8009C758` |
| +0x168 | packed **`u16 id; u16 kind`**: `kind == 0` -> `id` is a **road id** (small integer, *not* a `PMTS` piece key); `kind == 1` -> `id` is a node id, the entity is inside an intersection (`rules.md` 6.2) | `0x8009C730`, `SLUS 0x8003B61C`, `0x800B9C44` |
| +0x16C | **direction along that road**, +1 or -1 | `0x8009C748`, `0x8009EDB0` |
| +0x170 | **distance along that road**, 16.16 world units | `0x8009C73C` |
| +0x1FC | `s8` lane index | `0x8009B0E0` |
| +0x43C | -> the rider definition (5.) | `0x80095890` |

`+0x158`/`+0x168`/`+0x16C`/`+0x170` are exactly the four fields the dead kind-0 spawner arm
fills (`scene_cell.md` 5.5). Their **second writer** - the one the shipped game actually uses -
is the per-frame road
traversal at `RASHCDG 0x80099Exx`..`0x8009A008`, which walks an entity from one road piece to
the next through an 8-byte connection list and rewrites `+0x168` and `+0x170`
(`0x80099EB0`, `0x80099EF0`, `0x80099F88`, `0x8009A008` for the road; `0x80099F14`,
`0x80099FA0`, `0x80099FB4`, `0x80099FC8` for the distance). Its *third* writer is the
"clone an entity onto the next road piece" helper at `0x8009EDA4`..`0x8009EE4C`, which
copies the source entity's road, forces `+0x16C` to `sign(source+0x16C)` and offsets
`+0x170` by the value of the resident table at `0x80053114` (indexed by `game_state+0x3C`;
120.0 for every bank). This helper is the police spawner's release: it puts a cop 120.0 world
units ahead of the player on his road (`rules.md` 9.2).

The savestates confirm the units directly: in `rr-race` the two police bikes (pool 0 slots
16 and 17, model 118) sit at `+0x168 = 9`, `+0x170 = 1057.0` and `1054.1`, and
`ROADGRF1.TXT` race 4 says `[START]=9 1057 -1 7`. **`+0x170` is in the same unit as the
`SLCT` `distance` field and as the `[START]` distance: 16.16 world units** **[proven]**.

### 1.3 `+0xB8` is an oriented bounding box, not a position **[proven]**

Guest `0x800CF718` (the oracle array of `road_chunk.md` section 8) is
`pool 3 base (0x800CF660) + 0xB8`, i.e. it is **one traffic vehicle's eight OBB corners**:

| state | X extent | Y extent | Z extent |
|---|---|---|---|
| `rr-race` | 3.05 | 1.78 | 4.56 |
| `rr-pack` | 2.30 | 1.81 | 5.69 |
| `ramdumps\ram_000200.bin` | 2.62 | - | 5.72 |
| `ramdumps\ram_000260.bin` | 2.89 | - | 5.77 |

A car-sized box, constant in size while the whole box translates ~6.6 world units per
capture. The points lie on the independently decoded centre line and advance at the right rate,
and a traffic car is constrained to the tarmac as tightly as a racer, so the check against the
decoded road holds.

The collision broad phase reads `entity+0xB8` as the representative point
(`0x800A51F8`..`0x800A5254` compares `+0xB8` and `+0xC0` of two entities), and takes the
pool-6 position from `+0x0C` instead (`0x800A51EC`, `addiu a0,v0,12`).

---

## 2. Where each pool comes from

### 2.1 The cell arrays and the kind/pool identity **[proven]**

`scene_cell.md` 5 established the five region-0 arrays and their kind numbers 3 / 2 / 4 / 6 / 0
from the classify helper `RASHCDG 0x8009C41C` and its 7-entry jump table at `0x8005B8BC`.
Two further 7-entry jump tables are indexed by the *same* number:

| table | at | arm per kind | what it is |
|---|---|---|---|
| spawner | `0x8005B8DC` | 0 -> `0x8009C6B0`, 2 -> `0x8009C788`, 4 -> `0x8009C85C`, 6 -> `0x8009C810`, 1/3/5 -> `0x8009CA64` (no-op) | the per-kind spawn arm (`scene_cell.md` 5) |
| budget | `0x8005B680` | 0 -> `0x8008CE1C`, 2 -> `0x8008CE34`, 3 -> `0x8008CE8C`, 4 -> `0x8008CEBC`, 6 -> `0x8008CF40` | the per-kind "is there room" test, `0x8008CDF4(kind)` |

and each budget arm reads the control block of the matching **pool**:

| kind | budget arm | control block it reads | pool |
|---|---|---|---|
| 2 | `0x8008CE34` | `0x800D4B70` (+0x10 = base, +0x04 next) and `*(0x800D8744)` | pool 2 |
| 3 | `0x8008CE8C` | `*(0x800CF654) < 16` | pool 3 |
| 4 | `0x8008CEBC` | `0x800CD6C8` (+0x04 < +0x08), `*(0x800D1814) < 596` | pool 4 |
| 6 | `0x8008CF40` / `0x8008D9E4` | `0x800CD6A8` (+0x1C = base) | pool 6 |

That closes the identity: **kind == pool**. So the five arrays of a scene cell are
*motorcycles, pedestrians, traffic vehicles, props, collision volumes*, and the census of
`scene_cell.md` 0 reads:

| kind / pool | what it places | records in the two `.STR` | records counting every file |
|---|---|---|---|
| 0 motorcycle | - | **0** | **0** |
| 2 pedestrian | bystanders | 353 | 587 |
| 3 traffic vehicle | - | **0** | **0** |
| 4 prop | `HAZARD<n>.GEO` groups | 2833 | 5146 |
| 6 collision volume | static scenery collision | **7584** | **13429** |

**The two moving populations that the cells *could* place - motorcycles and traffic - are
the two arrays that are empty in every cell of both road sets and all 99 `.STP` files**
(0 records). That is the accounting that forces sections 3 and 5.

### 2.2 A second reason the kind-0 arm is dead **[proven]**

Besides being empty, the kind-0 arm at `0x8009C71C` does

```
v1 = record[+0x08];            // the PMTS piece key
if ((v1 >> 16) != 0) return;   // bnez v0, 0x8009CA64  @ 0x8009C728
entity[+0x168] = v1;
```

i.e. it only accepts a record whose `+0x08` fits in 16 bits. Every `pieceKey` on the disc is
`0x6xxxxxxx` / `0x7xxxxxxx` (`scene_cell.md` 5.1), so **even if the array were populated the
arm would reject every record**. The field it writes is a *road id*, not a piece key - the
array on disc is in a different format from the one the code expects. Kind 0 is leftover
code from an earlier data format.

A *procedural* kind-0 spawner does exist: it is **the police spawner**, `RASHCDG 0x8009B474(0, dist)`
-> `0x8009E89C`, exactly parallel to traffic's `0x8009B474(3, dist)` -> `0x8009CFF4`, both called from
`0x8008CD88` (`rules.md` 9.2). So both moving populations the cells could place - motorcycles and
traffic - are spawned procedurally.

---

## 3. Traffic vehicles - procedural, and the parameters are compiled in

The scheduler and the spawner below are PORTED (`src\game\sim\population.h`), bit-exact against the
original in the `rrverify` rows `traffic_sched` and `car_spawn`.

### 3.1 The spawner - `RASHCDG 0x8009AD48` **[established]**

`f(record, playerEntity)`. It is entered through `lh v0, -6792(0x800D0000)` = a gate at
`0x800CE578`, allocates from the pool-3 control block at `0x800CF650` inline
(`0x8009ADE4`..`0x8009AE8C`: cap literal **16** at `0x8009ADF4`, stride **512** via
`sll v0,a1,0x9`, base = ctrl + 16 = `0x800CF660`, handle = index + 96), copies
`record[+0x02]` to `entity+0xB4` and `record[+0x3E]` to `entity+0x142`, builds the entity's
transform, and finally writes `record[+0x3C]` to `entity+0x16C` (`0x8009B0B0`) and a lateral
to `entity+0x158` (`0x8009B194`, `0x8009B19C`, negated at `0x8009B1BC` / `0x8009B1DC` by the
sign of `record[+0x3C]` and of the lane byte `entity+0x1FC`).

The `record` it reads has the **cell object-record layout** - `+0x02` class, `+0x08` piece
key, `+0x24` along, `+0x3C` direction, `+0x3E`, `+0x40` - i.e. the 68-byte kind-3 record. But
nothing on the disc supplies one.

### 3.2 The scheduler builds the record on its own stack **[established]**

The three call sites of `0x8009AD48` are `0x8009D5E8`, `0x8009D990` and `0x800C9BE4`. At
`0x8009D568`..`0x8009D5EC` the caller fills a stack buffer at `sp+0x10` with

```
rec[+0x02] = 0xFFFF            // sh 0xFFFF, 18(sp)   @ 0x8009D57C
rec[+0x08] = <road piece>      // sw v1, 24(sp)       @ 0x8009D588
rec[+0x24] = <along>           // sw a1, 52(sp)       @ 0x8009D58C
rec[+0x3C] = <direction>       // sh a2, 76(sp)       @ 0x8009D594
rec[+0x40] = 4                 // sh 4, 80(sp)        @ 0x8009D584   (the lane)
```

and calls the spawner with it. So **traffic is scheduled, not placed**: the loop that
produces those values (`0x8009D284`..`0x8009D61C`) runs once per player per frame, over
`*(game_state+0x30)` players, using the player entity from `*(0x8005B268 + 4*player)`.

Gates, in order (all instruction addresses):

* an accumulator at `0x8005B210` advanced once per spawner round, i.e. it counts time, not
  distance (`0x8009D16C`..`0x8009D178`);
* a per-player next-spawn gate `*(0x800D871C + 4*player)`, compared against that accumulator
  shifted left 16 (`0x8009D2A8`..`0x8009D2BC`);
* a random roll `rng() % 100` (the `0x51EB851F` / `srl 5` idiom at `0x8009D2D4`..`0x8009D318`)
  against one of three stored percentages at `0x800D8734`/`0x800D8736`/`0x800D8738`, chosen by
  the player's speed at `entity+0x1E0` (`0x8009D2EC`, thresholds `0x00141DDD` and `0x00050000`);
  the roll picks whether the car is placed 180.0 world units ahead or 145.0 behind (`+0x1C` /
  `+0x20` of the parameter block, 3.3), not whether to spawn;
* a race-clock gate against the resident table at `0x80052FAC`, indexed by `game_state+0x3C`
  with a `+0`/`+3`/`+6` bias from `game_state+0x04` bits 0 and 2 - the same word is scaled by
  300 and by 600 for two limits (`0x8009D1A8`..`0x8009D244`);
* a duplicate test: if the candidate is on the player's own road (`entity+0x168`) and within
  `0x780000` = 120 world units of the player's `+0x170`, it is rejected (`0x8009D598`..`0x8009D5D4`).

### 3.3 The parameters are hard-coded in `RASHCDI` **[proven]**

The traffic parameter block lives at **`0x800D8710`** and is written, not loaded, by
`RASHCDI 0x80068DE4`..`0x80068E54`:

| off | value | written at |
|---|---|---|
| +0x04 + 4*player | 16 (initial; rewritten every round from the per-road table at `SLUS 0x800524F0`) | `0x80068DEC` (loop over `game_state+0x30` players) |
| +0x0C + 4*player | 4 (initial; likewise) | `0x80068DF0` |
| +0x14 | 16 | `0x80068E1C` |
| +0x18 | 4 | `0x80068E14` |
| +0x1C | 180 | `0x80068E24` |
| +0x20 | 145 | `0x80068E2C` |
| +0x24 (`s16`) | 100 | `0x80068E34` |
| +0x26 (`s16`) | 75 | `0x80068E3C` |
| +0x28 (`s16`) | 50 | `0x80068E54` |

and the pedestrian block at **`0x800D8740`** likewise at `0x80068EB8`..`0x80068EF0`:
`+0x04 = 4` (the live cap, also read by the kind-2 budget arm at `0x8008CE64`), `+0x08 = 10`,
`+0x0C = 4`, `+0x10 = 10`, `+0x14 = 150`, `+0x18 = 300`, `+0x1C = 5`.

All of these read back byte-for-byte from all four savestates **[proven]**, and **none of the
316 files in `DATA\` contains the block** - a search of the whole extract for the 64-byte window at `0x800D8714` finds nothing.

`RASHCDI`'s `ROADGRF` parser knows the keys `[VEHICLE_DEFAULT_DENSITY]`,
`[VEHICLE_DEFAULT_RATE]`, `[REACTIVE_DEFAULT_DENSITY]` and `[REACTIVE_DEFAULT_RATE]`
(strings at `0x8005B990`, `0x8005B9AC`, `0x8005B9C4`, `0x8005B9E0`), and the shipped
`ROADGRF1.TXT` / `ROADGRF2.TXT` contain none of them - only `[NUM_ENTRIES]`, `[BEGIN]`,
`[RACEID]`, `[START]`, `[FINISH]`, `[GMAGIC]`, `[RMAGIC]`, `[START_CHECKER]`,
`[FINISH_CHECKER]`, `[RACEINTS]`, `[END]`. **This is a second precise negative: the engine
has a data-driven density path and the shipped game does not use it.** A native port carries
the constants above; there is no file to look for.

Also hard-coded by the same `RASHCDI` function (`0x80068D54`, "reset the population
subsystem"): the pool-3 slots are zeroed 16 x 512 (`0x80068D80`..`0x80068DA8`), the
pool-2 slots 4 x 572 (`0x80068E64`..`0x80068E98`), the pool-4 control block's base is set to
`0x800D1818` (`0x80068F00`), the pool-5 base to `0x800D36EC` (`0x80068F24`), and the pool-6
capacity `*(0x8005B214)` is set to **32 when `game_state+0x04` bit 4 is set and 24 otherwise**
(`0x8006903C`..`0x8006905C`), after which `cap * 280` bytes are malloc'd and the pointer
stored at `0x800CD6C4` (`0x80069094`).

---

## 4. Kind 6 - the static collision volumes

### 4.1 What the spawner builds **[established]**

`RASHCDG 0x8009BB48(record, playerEntity)` allocates a 280-byte entry from the control block
at `0x800CD6A8` (`+0x00` live count, `+0x04` next index, `+0x08` high water, `+0x1C` base;
cap `*(0x8005B214)`) and fills:

| off | content | written at |
|---|---|---|
| +0x00 | `u16` handle = slot + 0xC0 | `0x8009BC48` |
| +0x02 | `u8` index of the record inside the cell array | `0x8009CCBC` (the walker) |
| +0x03 | `u8` `1 << playerIndex` | `0x8009CCC0` |
| +0x04 | the owning cell's resource id | `0x8009CCC4` |
| +0x08 | `record[+0x02]` = **cls** | `0x8009BD84` |
| +0x0C..+0x14 | centre, `record[+0x14..+0x1C]`, 16.16 world | `0x8009BCF8`..`0x8009BD14` |
| +0x18..+0x44 | **four footprint corners**, 3 x 16.16 each | `0x8009BF94`..`0x8009BFD8` |
| +0x84,+0x88,+0x8C | half extents | `0x8009BEBC`, `0x8009BECC`, `0x8009BED8` |
| +0x98 | distance along | `0x8009BD54` |
| +0xAC | lateral offset from the centre line | `0x80036800` out-param, `0x8009BD10` |
| +0xBC | road id | read back from RAM |
| +0xC0 | `record[+0x3C]` | `0x8009BD48` |
| +0x104..+0x117 | two `s16[3]` basis vectors of the footprint plane | `0x8009BD9C`..`0x8009BE9C` |

The geometry is built two different ways, switched on `cls` at `0x8009BEA8`:

```c
if (cls == 1) {                       // 0x8009BEB0
    half   = (rec[+0x40], rec[+0x40], rec[+0x50]);   // A.x, A.x, B.y
    other  = (rec[+0x4C], rec[+0x44]);               // B.x, A.y  -> +0x54, +0x58
    corner0 = e1*(-A.x) + e2*(-A.x);
    corner1 = e1*( A.x) + e2*(-A.x);
    corner2 = -corner0;  corner3 = -corner1;         // 0x8009BF00..0x8009BF50
} else {                              // 0x8009BFE4
    half   = ((B.x - A.x)/2, ..., (B.y - A.y)/2);    // two opposite corners
}
for (i = 0; i < 4; i++) corner[i] += centre;         // 0x8009BF94..0x8009BFD8
```

Verified on live RAM: in `rr-pack` pool-6 slot 0 has `cls = 1`, centre
`(1358.63, -22.79, 5005.00)`, `+0x84 = +0x88 = 0.2612`, `+0x8C = 7.019`, and its four corners
at `+0x18..+0x44` are the four points `(centre.x +- 0.2613, centre.y, centre.z +- 0.2613)` -
a flat square footprint of half side `A.x`, at the centre's own Y **[proven]**.

### 4.2 Why these are collision, not art **[established]**

* **No model.** A pool-6 entry has no `+0x60` registry pointer and no `+0x00` `DOD3`; the
  fields at those offsets are the footprint and the class. Nothing in the 280 bytes names a
  mesh.
* **The crash resolver reads them.** The contact-resolution family at
  `RASHCDG 0x800BD8xx`..`0x800BEAxx` resolves its partner by the global handle, takes the
  pool-6 arm at `0x800BD8E0`..`0x800BD904` (`handle & 0x1F` x 280 + `*(0x800CD6C4)`), and
  then at `0x800BD908`..`0x800BD918` reads the partner's `+0x08` (**the `cls`**) and sets a
  distinct bit `0x10` in the collision result word when it is 1. It also compares the bike's
  `+0x168` (road) against the volume's `+0xBC`. Seven more sites read `*(0x800CD6C4)` the
  same way: `0x8008DB64`, `0x8008DF4C`, `0x800A51E0`, `0x800A5B78`, `0x800A657C`,
  `0x800A6D94`, `0x800A74FC`, `0x800BE8DC`, `0x800BEBF8`.
* **They are budgeted like a working set, not like scenery.** 24 (or 32) live at once out of
  7584 on disc, spawned by the same proximity walker that spawns props, with a per-player
  mask at `+0x03`. That is a collision cache around the player.
* **The shape fits.** Over both `.STR` streams:

| cls | n | median half side | median height | median \|lateral\| | p90 \|lateral\| |
|---|---|---|---|---|---|
| 0 | 1254 | 1.274 | 1.465 | 20.78 | 30.54 |
| 1 | 12175 | 0.709 | 6.201 | 14.82 | 23.20 |

  `cls 1` is a thin vertical prism ~1.4 units across and ~6 units tall standing right at the
  edge of a road whose half width is ~15 - a **tree trunk, post or pole**. `cls 0` is a low
  wide box further out - a **wall, kerb or crate**. 37 % of the kind-6 records have a tilted
  normal (`scene_cell.md` 5.3), which is what a trunk on a sloped verge looks like.

**The cell's collision representation is region 0's kind-6 array, a list of primitive volumes,
not a mesh**; regions 1/2/5/6/7 are fully accounted for by the drawn geometry (`scene_cell.md`
section 6).

### 4.3 The record tail, re-read **[established]**

Beyond `scene_cell.md` 5.1's common prefix, an 88-byte kind-6 record is

```c
u32 f30;     // +0x30  always 1
u32 f34;     // +0x34  always 0
u32 f38;     // +0x38  always 0
u32 f3c;     // +0x3C  low half 0; high half 22 distinct small values
i32 A[3];    // +0x40  corner / half-extent A, 16.16 world units
i32 B[3];    // +0x4C  corner / half-extent B, 16.16 world units
```

`A` and `B` are identified in 4.1. `f3c` is copied verbatim to `entity+0xC0`; its high
half histogram over both streams is
`{0:704, 1:723, 2:3, 3:240, 4:6785, 5:793, 6:2179, 7:27, 8:50, 9:134, 10:77, 12:20, 13:225,
14:39, 15:53, 16:17, 17:351, 18:99, 19:253, 26:171, 33:4, 0xFFFF:482}` - one dominant value
(4) and a long tail, which reads like a **material / impact-sound class** **[guess]**; the
code that consumes `+0xC0` was not followed.

---

## 5. Motorcycles, riders and police

### 5.1 One pool, 18 slots **[proven]**

Pool 0 holds **all** motorcycles: the player, the AI racers and the police bikes. Pool 1
holds the rider figure for each of them, slot for slot. In all four savestates both pools
have `*live = 18`. The distinction between a racer and a cop is the **model**, and the model
comes from the rider definition the slot points at through `+0x43C`.

### 5.2 `DATA\LEVEL<n>.BI` - 27 rider definitions of 64 bytes **[proven]**

1728 bytes = 27 x 64. `+0x00` of each record is its own index 0..26, in all three files. The name is built by `RASHCDI`'s
`%sLEVEL%ld.BI` at `0x8005B868`.

```c
struct BikeDef {              // 64 bytes
    u32 index;                // +0x00  == the record number
    u8  ordinal;              // +0x04  0..0x10 for records 0..17; 7 / 2 for 18 / 19; 0 for 20..26
    u8  rank_class;           // +0x05  high nibble = rank, low nibble = bike class
    u16 f06;                  // +0x06  0x0177 everywhere except LEVEL1.BI record 10 (0x01E7)
    u32 f08;                  // +0x08  0.75 / 0.95 / 1.0 in 16.16
    u32 topSpeed;             // +0x0C  16.16 - 125 / 95 / 85 / 75 / 100 / 90 / 120
    u8  curve[0x2C];          // +0x10  gear/accel bytes and an 11-byte signature block
    u32 f3c;                  // +0x3C  16.16 multiplier, 0.75 .. 1.2
};
```

**`rank_class & 0x0F` selects the motorcycle model.** The three bike models of
`BBLEVEL<n>.GEO` are `(100, 109, 118)` for level 1, `(101, 110, 119)` for level 2 and
`(102, 111, 120)` for level 3 (`rmd3.md`); class 0/1/2 picks the
first/second/third. Proof: in each savestate, for every one of the 18 pool-0 entities, the
model id read from the registry pointer at `+0x60` equals
`(100, 109, 118)[LEVEL1.BI[(entity[+0x43C] - 0x800D5758) / 0x48].rank_class & 0x0F]`, with
**0 mismatches over 18 entities x 4 states** **[proven]**.

In `LEVEL1.BI` the classes are `{0: 12, 1: 12, 2: 3}` - twelve of each racing bike and three
of the police bike. The live race uses records 0, 2..8 (class 0), 10..17 (class 1) and
18, 19 (class 2), i.e. **the two police bikes of a normal race are pool-0 slots 16 and 17,
driven off `.BI` records 18 and 19** **[proven]**.

`topSpeed` at `+0x0C` falls monotonically with `rank`: record 0 = 125.0, records 1..9 =
95.0 / 85.0 x 8, records 10..17 = 75.0, records 18/19 (police) = 125.0, records 20..26 =
100 / 90 / 125 / 95 / 125 / 95 / 120. The per-record array is copied into a **runtime array
at `0x800D5758`, stride `0x48` (72 bytes)** - the record is *parsed*, not `memcpy`'d (342 of
1728 bytes agree), and `entity+0x43C` points into it. **20** runtime records are used: 0 and 1 are
the two players, 2..17 the AI, 18 and 19 the police; the layout is fixed (records 0, 2..8, 10..17, 18,
19 in the observed race) and the identities come from `session+0x40` (`rules.md` 1.4). The runtime
record adds `+0x27` (place / result code), `+0x28` (finish timestamp), `+0x3E` (cop release stamp,
`RASHCDG 0x8009EF30`) and `+0x45` (`0x800C8F48`).

### 5.3 The player's bike is a `.PH` file, the opponents' are not **[established]**

21 files of **exactly 484 bytes** named `COP{1,2,3}`, `CRUISE{A,B,S}{1,2,3}`,
`SPORT{A,B,S}{1,2,3}` - seven bike families x three tiers. Only **one** of them is resident
in any savestate: `CRUISEA1.PH` at guest `0x801B640C` (found by matching its first 48 bytes
in the RAM image). The other 20 are absent. `LEVEL1.PH` sits immediately before it at
`0x801B5ECC`, and `*(0x8005B248)` points at it.

Decoded fields:

```c
i32 topSpeed;      // +0x00  16.16 - 200 / 275 / 350 / 400 / 400.1
i32 ratio;         // +0x04  16.16 - 1.20 / 1.41 / 1.44 / 1.61 / 1.68
i32 frontSplit;    // +0x08  16.16 \  sum is exactly 1.0 in all 21 files
i32 rearSplit;     // +0x0C  16.16 /
i32 f10;           // +0x10  16.16 - 0.083 .. 0.154
i32 gear[7];       // +0x14  16.16 ratios, 5 or 6 non-zero then 0
i32 curve[45];     // +0x3C  a monotonically rising 16.16 curve (torque / power)
// ... to +0x1E3
```

The `CRUISE*` families have 5 gears, `SPORT*` and `COP*` have 6. 207 of the 484 bytes are
identical across all 21 files.

So a native port needs **two** bike descriptions: the 484-byte `.PH` for the player's chosen
bike, and the 64-byte `.BI` record for each AI rider. They are different formats and the AI
does not use `.PH` **[established; the RAM evidence is a negative - only the player's `.PH`
is resident - which does not prove the AI code never reads one]**.

### 5.4 `DATA\STARTDFA.BIN` / `STARTDFB.BIN` - the starting grid **[proven]**

Both files are an exact multiple of **292 bytes (73 words)**: `STARTDFA.BIN` = 18688 =
**64** blocks, `STARTDFB.BIN` = 16060 = **55** blocks. `ROADGRF1.TXT` declares
`[NUM_ENTRIES]=64`, and the `RACE2_*.STP` files on the disc are numbered 1..55, so
**block `race - 1` is the grid of race `race`**, DFA for road set 1 and DFB for road set 2
**[established]**. The name is built through the resident path table entry
`DATA\STARTDFA.LST` at `0x80052414` plus `RASHCDI`'s `%s.bin` format at `0x8005B8CC`.

```c
struct StartBlock {           // 292 bytes
    u32 n;                    // +0x00  grid entries: 14, 16 or 18
    struct { u32 slot;        //        rider slot + 1
             i32 lateral;     //        16.16 world units
             i32 alongOffset; //        16.16 world units
    } e[n];
    i32 tail[72 - 3*n];       // camera / countdown block, not decoded
};
```

Grid sizes: `STARTDFA.BIN` `{14: 1, 16: 19, 18: 44}`, `STARTDFB.BIN` `{14: 18, 16: 37}`.
Every `slot` is in 1..18 and no slot repeats inside a block.

**The world position is**

```
along = [START].distance + [START].direction * alongOffset
world = slicePos(road, along) - lateral * sliceRight(road, along)
```

(the lateral sign convention is `scene_cell.md` 5.1's: the stored value is the negative of
`chunk.Slice.right()`).

**Oracle.** In all four savestates the two police bikes (pool 0 slots 16 and 17, model 118)
have never moved: `road = 9`, `along = 1057.0` and `1054.1`, `lateral = 0.0`. `ROADGRF1.TXT`
race 4 gives `[START] = 9 1057 -1 7`, and `STARTDFA.BIN` block 3's last two entries are
`slot 17, lateral 0.0, alongOffset 0.0` and `slot 18, lateral 0.0, alongOffset 3.0`. The
formula gives **1057.0 and 1054.0** against a measured 1057.0 and 1054.1 **[proven]** -
which simultaneously fixes the `+ direction *` sign, confirms `slot - 1 = rider index`, and
identifies the savestates as race 4 of road set 1.

The whole grid of that race in world coordinates has the riders on lateral 6.8 / 9.0 in a two-column stagger at 16-unit spacing from `along` 817 to
1057, the two police at lateral 0 on the line. It can be laid over
`chunk.py`'s independently decoded road 9 directly.

### 5.5 What is *not* shown

The **ordering** of the grid - which end is the front - is not pinned. The direction of
travel implied by `[START].direction` is the open question: for race 1 (`dir = +1`) the
pole entry sits at the *lowest* `along` and for race 4 (`dir = -1`) at the *highest*, which
is self-consistent only if `dir` flips the sense of "forward". The two police entries are
the only grid rows any savestate still holds, and they have offsets 0 and 3, so they cannot
discriminate. See section 9.

---

## 6. Pedestrians (kind 2) **[established]**

The spawner is `RASHCDG 0x800CB8C8(record, 1, entity)`, reached from the kind-2 arm
`0x8009C788` after a budget check `0x8008CDF4(2)` and a duplicate test against the *last*
entity of that kind (`0x8005B268 + 4*player`, compared on `record[+0x08]` vs `entity+0x168`
and `record[+0x24]` vs `entity+0x170`).

It allocates from the pool-2 control block at `0x800D4B70` (base at `+0x10`, stride 572,
cap 4 from `slti v0,a0,4` at `0x800CB97C` and `*(0x800D8744)` at `0x800CB98C`, handle =
index + 0x40 at `0x800CBA20`), binds a model by class through `0x8002FAD4(entity, 4, 0xFFFF, 0)`
(`0x800CBB20`), and then switches on the record's `cls`:

* `cls < 19` (`0x800CBB3C`): `entity+0x236 = cls`, and `cls` indexes a 16-byte table at
  `0x800CCC20` (`0x800CBB54`..`0x800CBB60`) - an animation descriptor.
* `cls == 19, 20, 21, 22` (`0x800CBB78`, `0x800CBBA0`, `0x800CBBE8`, `0x800CBC50`): pick a
  random variant, `entity+0x236 = rng() % 25` (the `0x4EC4EC4F` / `srl 2` idiom at
  `0x800CBB8C`..`0x800CBBDC`), with `entity+0x234` set to 1 or 2 per arm.

The classes present on the disc are `{1: 92, 19: 50, 20: 10, 22: 4, 23: 431}` - so the great
majority of pedestrian placements ask for a **random** animation.

The models are 400 and 430, the two `RMD3` chunks of `PED01A.GEO` **[proven from the live
pools in all four states]**. 430 is the second body, not a separate file; whether it is a
policeman on foot is **not established** - see section 9.

---

## 7. Other data files a native port needs

All sizes from `DATA\` on the disc. What is decoded is marked; the rest is open.

| file | size | status |
|---|---|---|
| `LEVEL{1,2,3}.BI` | 1728 | **decoded as 27 x 64** (5.2); the 0x2C byte tail of each record is not |
| `COP{1..3}.PH`, `CRUISE{A,B,S}{1..3}.PH`, `SPORT{A,B,S}{1..3}.PH` | 484 | **partly decoded** (5.3): top speed, ratio, weight split, 5-6 gear ratios, a 45-entry rising curve; the rest of the 121 words is not |
| `LEVEL{1,2,3}.PH` | 2400 | first 484 bytes have the *same* field shape as a bike `.PH` (top 400.1, ratio 1.61, gears 72/45/33/25/20); the remaining 1916 bytes are **not decoded**. Loaded at guest `0x801B5ECC`, pointer at `*(0x8005B248)` |
| `STARTDFA.BIN` | 18688 | **decoded**: 64 blocks of 292 (5.4); the 72 - 3n word tail is not |
| `STARTDFB.BIN` | 16060 | **decoded**: 55 blocks of 292 (5.4) |
| `STARTJBA.BIN` | 556 | **not decoded.** 139 words. Header `{6, 0, 0, 2, 8}` then 16.16 triples `(17.0, 2.3, -66.0)`, `(17.0, 1.4, -90.0)`, `(17.0, 4.2, -90.0)`, `(17.0, 1.4, -94.0)`, `(17.0, 4.2, -94.0)`, `(17.0, 2.3, -98.0)` - six entries with a constant first field, two lateral values and four along values, i.e. a six-rider Jailbreak-mode grid in a *different* layout from `STARTDF?.BIN`. 139 is not a multiple of 73, so it is not the same block format |
| `GLOBALS.BI` | 2732 | **not decoded.** Opened by `RASHCDI`'s `(%sGLOBALS.BI` at `0x8005B7F3` with the diagnostic "WARNING: Globals not initialized correctly" at `0x8005B83C`. Head is a byte block (`00 07 00 0A 12 16 16 1A 0A 0A 0A 0A`) then 16.16 values; it does **not** appear verbatim in any savestate, so it is parsed on load |
| `ENV.EN` | 12888 | **not decoded**, but **located**: loaded verbatim at guest `0x800D38E0` in every savestate. 3222 words, all plausible 16.16 (1.2, 1.0, 1.1, 2.0, 0.8, 0.65, 0.7 ...), no strings. Opened through the resident prefix `DATA\ENV` at `0x800524B4`. Reads as per-surface response coefficients **[guess]** |
| `FIGHT.BIN` | 4200 | **not decoded.** A table of 4-word records `{u32 code; u32 offA; u32 offB;}` - `(0x0506000B, 0x1E0, 0x228)`, `(0x0303000B, 0x264, 0x288)`, `(0x0607000B, 0x2AC, 0x300)` - whose two words are file offsets past the table (the table ends at 0x1E0). Opened through the resident prefix `DATA\FIGHT` at `0x800524DC`. Combat move -> animation pair table **[probable]** |
| `ANIMNOIZ.DAT` | 3080 | **not decoded**, but **located**: an EA chunk container, tags `ARec` (`0x63655241`, size 0x87C) and `APtr`, loaded verbatim at guest `0x801E732C`. Name literal `DATA\ANIMNOIZ.DAT` at `RASHCDI 0x8005B7B8`. Animation-to-sound table **[probable]** |
| `CAMERA.CA`, `VIEWS.VI` | 1596 | **not decoded**, but located verbatim at guest `0x800CD7B8` and `0x800D6C68` |

The disc's 24 `.PH` files are these 21 bike files plus `LEVEL{1,2,3}.PH`, which are a different
format.

---

## 8. Verification

The format claims above were checked by an independent Python probe (67 checks, 0 failures): the
flat-table shapes, the empty kind-0/kind-3 accounting, the pool strides, the handle identity, the
per-pool model sets, the `.BI` class -> model identity, the hard-coded parameter blocks, the
`0x800CF718` bounding box, and the police-bike grid oracle. The runtime code that
consumes these structures is accepted by the `rrverify phys` rows of `tools\rrverify\rows_population.inc`
and `rows_traffic*.inc`.

---

## 9. Unknown

* **The sense of `[START].direction`** and therefore which end of the grid is the front
  (5.5). Two rows of live data both have near-zero offsets and cannot discriminate.
* **Kind-6 `+0x3C` high half** (22 values, dominated by 4) and the code that consumes
  `entity+0xC0`. The material/impact-sound reading is a guess.
* **Kind-6 `+0x54`/`+0x58`** (`B.x` and `A.y`, stored by the `cls == 1` arm but not used for
  the footprint) - an outer radius and a second height, presumably for a broad-phase test.
* **Pool 5** (`0x800D36EC`): allocated and reset by `RASHCDI 0x80068F14` but never used in
  any savestate. Its stride word in the pool table is `0xFFFFFE3C`, which is not a stride.
* **Entity `+0xA4` and `+0xA8`.** `+0xA8` links to another entity (an AI target, a tow, or a
  free list) and is not decoded. Next to them, `entity+0x354` points at the paired pool-1 rider
  and `rider+0x25C` is the rider mode (0 before the start, 1 in the saddle, >= 3 thrown off / on
  foot), which gates the cop spawner, the arrest and the race-over test (`rules.md` 9).
* **Pool-1 (rider) entities.** They carry the same header but their `+0x168`/`+0x170` are 0
  for 17 of 18 slots in every state; the rider is reached from its bike through `+0x354`.
* **Model 430.** The second `RMD3` of `PED01A.GEO`, used by pool 2 alongside 400. Whether it
  is a policeman on foot is not established.
* **`LEVEL<n>.BI` `+0x04`, `+0x06`, `+0x08`, `+0x10..+0x3B` and `+0x3C`.** Only the index,
  the rank/class byte and the top speed are attributed.
* **The 45-word curve and the remaining ~60 words of a bike `.PH`.**
* **`LEVEL<n>.PH` past the first 484 bytes**, `GLOBALS.BI`, `ENV.EN`, `FIGHT.BIN`,
  `STARTJBA.BIN`, `ANIMNOIZ.DAT`, `CAMERA.CA`, `VIEWS.VI` (7).
* **The `STARTDF?.BIN` block tail** (72 - 3n words): it holds a pair of adjacent large
  16.16 values (2300.0 / 2301.0 for race 1, 1600.0 / 1601.0 for race 4), a third value
  (130.0 / 1.0), eight zeroes and three ratios near 0.91..0.94. Countdown camera path
  **[guess]**.
* **Which `.BI` records 20..26 are for.** The runtime layout is fixed (5.2: records 0, 2..8,
  10..17, 18, 19 in the observed race, identities from `session+0x40`); records 1, 9 and 20..26
  are unused there.
* **The `0x800CCC20` pedestrian animation table** (16-byte records indexed by `cls < 19`).
* The resident tables indexed by `game_state+0x3C` are named in `rules.md`: `0x80052FAC` is a
  race-clock interval (2.4, 4.2), `0x800530FC` the cop spawn-attempt distance, `0x80053114` the
  120.0 cop spawn offset and road-ahead requirement, `0x80053174` the five Jailbreak phase
  milestones `(u16 along, u16 road)` (9.2, 9.6).
