# Road Rash: Jailbreak (USA, SLUS_01053) - texture and image containers

Every statement below was derived from the disc bytes (extract in `work\disc_us`; the disc
image is raw MODE2/2352, 249159 sectors). Parser: `src\rrformats\texture.{h,cpp}`; `rrtool tex <disc>
<path-on-disc> <outdir>` renders a file. The gate "textures: C++ decoder against the Python probe, pixel
for pixel" in `tests\run_gates.ps1` (`tests\compare_tex.py`) compares that output with an independent
decoder's.

No third-party format code was consulted or copied; the field tables were re-derived
by making the byte arithmetic close exactly and by looking at the decoded images.

---

## 0. Shared primitives

### 0.1 BGR555 texel

PS1 16-bit colour, little-endian halfword:

| bits | meaning |
|---|---|
| 15 | STP - semi-transparency flag |
| 14..10 | blue 0..31 |
| 9..5 | green 0..31 |
| 4..0 | red 0..31 |

Conversion used by the decoder: 5->8 bit by `(c << 3) | (c >> 2)`; a halfword of
`0x0000` is written out as fully transparent, everything else opaque. The STP bit
is *not* folded into alpha - it selects the blend equation at draw time and is
reported, not baked.

Verified on `DATA\CARSC.TEX` +0x2C: `de 7f bd 7b 9c 77 7a 73 ...` = 0x7FDE, 0x7BBD,
0x779C, 0x737A - a monotonically descending grey ramp (r=g=b, STP=0), which is what
the first 128 CLUT entries of every level/bike/rider texture in the game are.

### 0.2 4bpp index order

Low nibble first. `DATA\HAZARD0.TEX` +0x18 is `11 11 ...`; decoding low-nibble-first
at 256x192 produces legible road signs, high-nibble-first produces mirrored garbage.

### 0.3 TIM block pair

The PS1 TIM "block" shape (`u32 bnum; u16 dx; u16 dy; u16 w; u16 h;` + payload,
`bnum` = 12 + payload) appears both in the 98 standalone `.TIM` files and embedded
inside `LECT` chunks. `w` is in **16-bit VRAM words**, so pixel width is `w*4` at
4bpp, `w*2` at 8bpp, `w` at 16bpp.

---

## 1. The EA chunk chain (`*.TEX`, `*.MRO`) - "EACHUNK"

`LECT` is not a file format, it is the first chunk of a generic chained container:

```
+0x00  char[4]  tag
+0x04  u32      chunkSize  (the next chunk starts exactly here)
+0x08  ...      tag-specific payload
```

The chain runs to EOF with no terminator and no padding. Verified byte-exact for all
142 chunked files in `DATA`; e.g. `DATA\CARSC.TEX`
(16524 = 0x408C bytes): `LECT` @0x0 size 0x2038 -> `LECT` @0x2038 size 0x2038 ->
`CTKP` @0x4070 size 0x1C -> 0x408C = EOF.

Tags observed:

| tag | meaning | where |
|---|---|---|
| `LECT` | one texture | `*.TEX`, `*.MRO`, inside `BBLEV*.TEX` |
| `TSLP` | CLUT bank | `BBLEV*.TEX` only |
| `KNBP` | CLUT bank (256-entry) | `BBLEV*.TEX` only, always the first chunk |
| `CTKP` | table of contents for the `LECT` chunks | last chunk of `*.TEX` |
| `RMD3` | model (out of scope here) | 2nd chunk of every `*.MRO`, all `*.GEO` |
| `DMD3` | animation table (out of scope) | all `*.PSX` |

### 1.1 `LECT` chunk

```
+0x00  char[4] 'LECT'
+0x04  u32     chunkSize
+0x08  u32     0                  (0 in all 77 LECT chunks in DATA)
+0x0C  u8      kind               1,2,3 -> payload is a TIM ; 5,6 -> raw pixels
+0x0D  u8      bpp                4 or 8
+0x0E  u8      0                  (0 in all 77 chunks)
+0x0F  u8      slot               unknown, see below
+0x10  u16     id                 resource id, also listed in CTKP
+0x12  u16     width  in pixels
+0x14  u32     height in pixels
+0x18  payload
```

Field census over all 77 `LECT` chunks in `DATA`:

| kind | bpp | width x height | +0x0F slot | count | what the PNG shows |
|---|---|---|---|---|---|
| 1 | 8 | 128 x 60 | 1 | 37 | rider / clothing sheets |
| 2 | 8 | 128 x 60 | 1 | 27 | motorbike part sheets |
| 3 | 8 | 128 x 60 | 1 | 2 | traffic-car sheets (`CARSC.TEX` only) |
| 5 | 4 | 256 x 32 | 10 | 5 | small level-prop strip (`BBLEV*.TEX` only) |
| 6 | 4 | 256 x 192 | 31..35 | 6 | road-sign / street-furniture atlas (`HAZARD0..5.TEX`) |

`+0x0F` is constant per kind (1 for all 8bpp sheets, 10 for kind 5) and runs
31,31,32,33,35,34 for `HAZARD0..HAZARD5` respectively - it looks like a global
texture-slot number, but nothing in the file proves that. **Unknown.**

#### kind 1 / 2 / 3 - payload is a literal TIM

At `+0x18` the payload starts with `10 00 00 00` (the TIM id) and is a complete,
standard TIM. `DATA\CARSC.TEX`, chunk 0:

| off | bytes | meaning |
|---|---|---|
| 0x18 | `10 00 00 00` | TIM id 0x10 |
| 0x1C | `09 00 00 00` | TIM flags: pmode 1 (8bpp) + bit3 (CLUT present) |
| 0x20 | `0c 02 00 00` | CLUT block bnum = 524 = 12 + 256*1*2 |
| 0x24 | `00 00 00 00` | CLUT dx=0, dy=0 |
| 0x28 | `00 01 01 00` | CLUT w=256, h=1 |
| 0x2C | 512 bytes | 256 BGR555 entries |
| 0x22C | `0c 1e 00 00` | image block bnum = 7692 = 12 + 64*60*2 |
| 0x230 | `00 00 00 00` | image dx=0, dy=0 |
| 0x234 | `40 00 3c 00` | image w=64 halfwords (=128 px at 8bpp), h=60 |
| 0x238 | 7680 bytes | 60 rows x 128 8bpp indices |

0x18 + 8 + 524 + 7692 = 0x2038 = `chunkSize`, exactly. The `LECT` +0x12/+0x14 pair
(128, 60) is the same size expressed in pixels, so it is redundant but consistent.

CLUT VRAM `dy` varies: 0 in `CARSC.TEX` +0x26, `01 e0` = 480 in `CRUISES1.MRO` +0x26,
i.e. the CLUT is uploaded to the bottom strip of VRAM. `dx` is 0 or 1 - **unknown**
whether these are real coordinates or stale authoring values, since the game must
re-place them at load time anyway.

#### kind 5 / 6 - raw indexed pixels, **no CLUT in the file**

`DATA\HAZARD0.TEX`: `+0x0C` = `06 04 00 1f`, `+0x10` = `32 00`, `+0x12` = `00 01`
(256), `+0x14` = `c0 00 00 00` (192). Payload at +0x18 is `256*192/2` = 24576 bytes,
and 0x18 + 24576 = 0x6018 = `chunkSize`. No CLUT block anywhere in the chunk.
Same shape for the kind-5 chunk in `BBLEVEL1.TEX` @0x1C7C8: 256 x 32 -> 4096 bytes,
0x18 + 4096 = 0x1018 = `chunkSize`.

The CLUTs for these are uploaded separately and live in the spare rows of the
atlas's own VRAM page column, one 16-colour CLUT per row; the per-primitive CLUT id
picks a row. For `HAZARD*.TEX` the column is at page base + 48 halfwords and the
palettes come from `DATA\GAMEBIN1.DAT`. Full derivation in section 5a.

#### `LECT` chunks are shared by id, byte-identical

`CRUISES1.MRO` chunk id 0x0006 and `BBLEVEL1.TEX` chunk id 0x0006 are the same
8248 bytes with zero differing bytes; likewise `SPORTS1.MRO` id 0x0007 vs
`BBLEVEL1.TEX` id 0x0007. So the `+0x10` id is a global texture id and containers
simply carry copies of the chunks they need.

### 1.2 `CTKP` chunk - table of contents

```
+0x00  char[4] 'CTKP'
+0x04  u32     chunkSize
+0x08  u32     count           (= number of LECT chunks in this file)
+0x0C  count x { u32 kind; u16 unknown; u16 id; }
```

`chunkSize` = 12 + count*8, exact in all 8 files that carry a `CTKP`.
`DATA\CARSC.TEX` @0x4070: size 0x1C, count 2, records
`03 00 00 00 | 36 01 | 7e 00` and `03 00 00 00 | 30 01 | 78 00` - the `kind` and
`id` match the two `LECT` chunks (kind 3, ids 0x7E and 0x78) exactly.
`DATA\BBLEVEL1.TEX` @0x1D7E0: size 0x74, count 13 = the 13 `LECT` chunks.

The middle `u16` is **unknown**: 0x136/0x130 in `CARSC.TEX`, 0xC8 in `HAZARD0.TEX`,
0x64/0x67/0x6D/0x70/0x76/0x9F/0x96/0x320 in `BBLEVEL1.TEX`. It is not a size (both
`CARSC` chunks are 0x2038 bytes but carry different values) and not an offset.

### 1.3 `TSLP` and `KNBP` chunks - CLUT banks

Both share one header shape:

```
+0x00  char[4] tag
+0x04  u32     chunkSize
+0x08  u16     id         (unknown; 29 for KNBP, 21 for TSLP in every BBLEV*.TEX)
+0x0A  u16     kind       (1 for KNBP, 2 for TSLP)
+0x0C  u16     width      entries per row
+0x0E  u16     height     rows
+0x10  u32     0x00000207 (unknown, identical in both chunks of all 5 files)
+0x14  payload, (chunkSize-0x14)/(width*height*2) blocks of width*height halfwords
```

- `KNBP` (always the first chunk of `BBLEV*.TEX`): size 0x3014, width 384,
  height 16 -> 384*16*2 = 12288 = 0x3014-0x14, exact. Read at a 512-byte stride the
  payload is **24 CLUTs of 256 BGR555 entries**: slots 0..8 are all zero, slot 9
  starts `de 7f bd 7b 9c 77 7a 73` - the same grey ramp the 8bpp textures use.
  The declared 384x16 geometry and a 512-byte stride are both consistent with the
  byte count, but the engine reads the bank at a **256-byte stride, as 48 palettes of
  128 entries** (see "Which bank serves whom" below).
- `TSLP`: size 0x1514, width 128, height 1 -> 21 blocks of 128 entries
  (21*128*2 = 5376 = 0x1514-0x14, exact). Palette 0 is again the grey ramp; the 21
  palettes fall into 7 distinct families (seen as repeated first-halfwords), which
  looks like per-level lighting/time-of-day variants. **Assignment unknown.**

Neither bank matches any `LECT` TIM CLUT byte-for-byte (checked for all 12 TIM CLUTs
in `BBLEVEL1.TEX`), so they are additional palettes, not copies.

#### Which bank serves whom

The runtime 8bpp palette is **128 entries**, chosen per OBJECT by a skin index `a1` in the model draw
path. Followed to the disc for the two objects a race always has, both palettes are read at the
256-byte stride, confirmed by searching the whole `.TEX` for the 256 bytes the captured VRAM holds at
the address `a1` names - each is found exactly once:

| object in `work\oracle\state\rr-race` | model | `a1` | CLUT id | VRAM | file offset |
|---|---|---|---|---|---|
| `0x801B65D4`, the player's bike | 100 | 29 | `0x7DB8` | (896, 502) | `BBLEVEL1.TEX +0x2E14` = `KNBP` block 46 |
| `0x801BB2EC`, the rider | 150 | 30 | `0x7D68` | (640, 501) | `BBLEVEL1.TEX +0x3128` = `TSLP` block 1 |

So `KNBP` and `TSLP` are **both** racer palette banks of 128 entries and one object can take its
palette from either. The loader's positional upload rule for `KNBP` holds for the 23 blocks that are
not all zero, but it places block 46 at `a1 = 1`, not 29; the block it assigns to `a1 = 29` is block
20, which is all zero on the disc. The racer's VRAM row therefore holds a run-time copy: the `TSLP`
handler `0x8005DBB8` uploads three `TSLP` blocks per player into the next CLUT rows, and `TSLP` block 0
is byte-identical to `KNBP` block 46 in `BBLEVEL1.TEX` (`rules.md` 16.5). A renderer can bind block 46
and reproduce the frame - which `rrview --bikecheck` does, 99 120 pixels with 0 mismatches
(`--bikecheck-mutate`, the next `KNBP` block, is the negative control).

### 1.4 Coverage

| family | files | result |
|---|---|---|
| `LECT`-magic `*.TEX` (`CARSC`, `HAZARD0..5`) | 7 | **PASS** - every byte accounted for, all images decode |
| `LECT`-magic `*.MRO` (`CRUISES1..3`, `SPORTS1..3`) | 6 | **PARTIAL** - `LECT` chunk fully decodes; the `RMD3` chunk is a model, out of scope |
| `KNBP`-magic `*.TEX` (`BBLEVEL1..3`, `BBLEVJBD/JBK`) | 5 | **PASS** - every byte accounted for; 13/13/13/12/12 images decode |

---

## 2. Plain PS1 `TIM`

98 files (95 in `DATA\FE`, 3 in `DATA`). Standard layout, implemented from scratch:

```
+0x00  u32 0x00000010            id
+0x04  u32 flags                 bits0..2 pmode (0=4,1=8,2=16,3=24 bpp), bit3 = CLUT
       [CLUT block]  u32 bnum; u16 dx; u16 dy; u16 w; u16 h; then w*h halfwords
       [image block] u32 bnum; u16 dx; u16 dy; u16 w; u16 h; then w*h halfwords
```

All 98 pass with `sum(blocks) == filesize`:

| set | count | shape |
|---|---|---|
| `FE\CR_00..16`, `DR_00..30`, `MR_00..31`, `ECITY`, `LCITY`, `RCITY`, `STCITY`, `LCOAST1/2`, `RCOAST1/2`, `VALLEY1..6`, `WATER` | 95 | 8bpp 64x31, 256x1 CLUT at VRAM (0,480), image at VRAM (640,0) |
| `RIMA1.TIM` | 1 | 8bpp 128x60, same shape as a `LECT` kind-2 payload |
| `LOADTXT.TIM` | 1 | 16bpp 140x20, no CLUT |
| `PRELOAD.TIM` | 1 | 16bpp 66x14, no CLUT |

**What they show.** `CR_00..CR_16` are 64-pixel-wide slices of one panorama: stitched
left to right in name order they form a continuous 1088x31 horizon - a city skyline
fading into hills. `DR_*` is a desert mesa panorama,
`MR_*` a coastal one, and the named ones (`WATER`, `VALLEY1..6`, `*CITY`, `*COAST*`)
are single backdrop tiles. `RIMA1.TIM` is a wheel-rim sheet (gold spoked rim plus
tyre variants). `LOADTXT`/`PRELOAD` are the blue "Loading..." banners.

These 98 files are the **known-good cross-check** for the colour conversion: the same
`bgr555_to_rgba` + 8bpp unpack that makes `CR_00` a recognisable skyline is what
`LECT` kind 1/2/3 uses.

---

## 3. `DATA\DASH?P.TEX` - headerless VRAM page (the HUD)

`DASH1P.TEX` and `DASH2P.TEX` are 32768 bytes with no magic. They are a raw dump of a
VRAM rectangle **64 halfwords wide by 256 rows** (64*2*256 = 32768):

| rows | content |
|---|---|
| 0 .. 144 | 4bpp art, 256 pixels per row (128 bytes) |
| 145 .. 149 | filler |
| 150 .. 155 | the CLUT table, 4 slots of 16 BGR555 entries per row |
| 156 .. 255 | filler |

The filler halfword is `0x30C6` (bytes `c6 30`); a filler row is
`ff 7f` + 62 x `c6 30` + a 2-byte row tail. Counting non-filler halfwords in rows
150..155 gives exactly **22 populated CLUT slots** out of the 24 the six rows can
hold - and `TexArtDashCounts` in `DASH1P.CSV` says there are exactly 22 TIMs. That
is an independent confirmation of both the row stride and the one-CLUT-per-TIM rule.

Row 150 is exactly where `DATA\DASH1P.CSV` line 1 says it is: `ClutTable,0,150`.
`DASH1P.TEX` +0x4B00 (= 150*128) = `bd 7b f6 5e 44 00 cb 08 ...` is CLUT slot 0;
offset 150*128 + n%4*32 + (n/4)*128 is slot n.

### 3.1 `DATA\DASH?P.CSV` layout table

The CSV that ships on the disc is the authoring layout for this page:

```
ClutTable,x,y                          VRAM position of the CLUT table
SignFlash / NitroFlash / ... ,...      HUD timers, not image data
TexArtDashCounts,nTex,nArt,nDash       22, 62, 110 in all three CSVs
  nTex  rows: <name>.TIM , vramX , vramY , ? , ?
  nArt  rows: <NN>kArt<Name> , x , y , w , h , texIndex
  nDash rows: kDash<Name> , screenX , screenY , artIndex , ? , ?
```

`texIndex` indexes the nTex list; `(x,y,w,h)` is the sub-rectangle inside that TIM.

**Cross-check, and the main proof that the whole chain is right:** compositing every
one of the 22 TIM rectangles out of `DASH1P.TEX` at its CSV `vramX,vramY`, using
**CLUT slot = TIM index**, produces a coherent HUD sheet:

| CSV row | position | decoded image |
|---|---|---|
| `RADAR.TIM,0,0` | 0,0 24x48 | the 5-0 radar dial, pink/red sweep |
| `DASHSTAR.TIM,32,0` | 32,0 26x26 | a star badge |
| `BACKGRND.TIM,64,0` | 64,0 42x60 | flat blue HUD panel fill |
| `COUNT.TIM,128,0` | 128,0 52x28 | the yellow "3 2 1" race countdown digits |
| `SPEED1P.TIM,188,0` | 188,0 68x32 | speedo frame with the red gear letter "N" |
| `TKO.TIM,128,21` | 128,21 32x32 | white skull-and-crossbones |
| `WRONGWAY.TIM,32,28` | 32,28 32x32 | yellow U-turn arrow on black |
| `ARRESTS.TIM,160,30` / `ARREST.TIM,160,44` | | handcuffs, and a clenched fist |
| `HEALTH.TIM,80,64` | 80,64 45x24 | red/white health bar segments |
| `ARROWL/ARROWR.TIM,192/224,80` | 32x32 each | the two yellow curved turn arrows |
| `DASHNUM.TIM,0,94` | 0,94 128x16 | big white digits `1234567890:` |
| `WEAPON2.TIM,128,96` / `WEAPON1.TIM,48,124` | 64x14 | weapon icons (club, chain, crowbar) |
| `POSITION.TIM,0,112` | 0,112 48x32 | placing text |
| `ODNUM.TIM,48,112` | 48,112 73x9 | small odometer digits `1234567890/.` |

The 62 `kArt` sub-rectangles decode individually (e.g. `05kArtBigNum0` is the digit "0",
12x16). `rrtool dash <disc> <outdir>` parses the layout and checks it against its own declared counts
(gate "HUD: the shipped layout parses and matches its own declared counts").

Columns 4 and 5 of a `kDash` row are read by `RASHCDI 0x8005F908`: column 4 is a one-bit
flag stored at `elem+0x22`, column 5 a `kind` (0 static sprite, 1 code-driven geometry,
2 disabled) stored at `elem+0x20`. **Unknown:** columns 3 and 4 of the nTex rows (all `0,0`).
The CLUT-slot = TIM-index rule reproduces the expected colours for every sprite that
can be named (yellow countdown, yellow arrows, white skull, white digits) but is an
inference from the images, not a value read out of a file.

---

## 4. `DATA\FE\FEMISC.PSH` - EA `SHPP` shape bank

61768 bytes, fully decoded.

```
+0x00  char[4] 'SHPP'
+0x04  u32     file size            = 61768 = the real file size
+0x08  u32     entry count          = 26
+0x0C  char[4] directory tag        'GIMX'
+0x10  26 x { char[4] name; u32 offset; }      -> 0x10 + 26*8 = 0xE0
```

0xE0 is exactly the offset of the first entry, so the directory is byte-tight.
Each entry:

```
+0x00  u8   code        0x42 in all 26 entries = 16bpp BGR555
+0x01  u24  blockSize   0 in all 26 entries (unknown / unused here)
+0x04  u16  width
+0x06  u16  height
+0x08  u16[4] 0         (unknown; 0 in all 26 entries)
+0x10  width*height BGR555 halfwords
```

`16 + w*h*2 == nextOffset - offset` holds for all 26 entries (the last one against
the file size), so every byte of the file is accounted for.

**What the images are**: front-end menu furniture and the
weapon icons.

| name | size | image |
|---|---|---|
| `BLAW` / `BRAW` | 54x64 | tiled scrolling chevron blocks, brown-left and yellow-right |
| `BTNN` / `BTNH` | 164x18 | wide menu button, wood-grain (normal) and blue (highlighted) |
| `SBTN` / `SBTH` | 38x14 | small menu button, same two states |
| `LARW` `RARW` `UARW` `DARW` | 10x14, 8x14, 38x12, 38x12 | blue menu scroll arrows |
| `CRCL` `SQRE` `TRIG` `JP_X` | 14x10 | PlayStation circle / square / triangle / cross glyphs |
| `JPLR` `JPUD` | 12x10, 10x12 | d-pad left-right and up-down glyphs |
| `PROD` `CLUB` `WOOD` `CHAN` `CBAR` `PIPE` `NCHK` `SPRY` `STUN` | 46x32 | weapon icons: cattle prod, club, plank, chain, crowbar, pipe, nunchaku, spray can, stun gun |
| `HALO` | 46x32 | a pink/white radial glow (selection highlight) |

The entry names pair off as base/highlight (`SBTN`/`SBTH`, `BTNN`/`BTNH`), which is
how the front end shows a selected item.

`SHPP` is the PlayStation sibling of EA's `SHPS`/`SHPI` bank; the `GIMX` directory tag
and the code/size/width/height entry header were re-derived here from our bytes, and
nothing but code `0x42` occurs in this game, so other EA image codes are untested.

---

## 5. `DATA\G_OBJ01.GTP` - a pre-baked 4bpp texture page (NOT a CLUT bank)

The data has a clean 32-byte period, which reads like an array of 16-entry CLUTs but
is also what a 64-halfword-wide VRAM block looks like. The ground truth below comes
from real VRAM.

16384 bytes, no header. It is **one pre-baked PS1 texture page**: 64 halfwords wide
by 128 rows, uploaded to VRAM verbatim. Proof: the 16384 bytes of the file are
byte-identical to the VRAM rectangle x=896..960, y=0..128 in
`work\oracle\state\rr-race\vram.bin` (SHA-1 `808a1a198d6cb312...` on both sides),
so the file *is* the VRAM block. That puts it at **tpage X=14, Y=0, 4bpp**, i.e.
`draw_mode` `0x20E` in the captured draw stream.

Read as 4bpp it is 256 x 128 pixels and splits into three bands:

| rows | bytes/row | content |
|---|---|---|
| 0 .. 3 | 128 | **14 sixteen-colour CLUTs**, four per row, each ending `ff 03` (=`0x03FF`) |
| 4 .. 11 | 128 | all zero |
| 12 .. 127 | 128 | the object art |

Row 0 has 119 non-zero bytes of 128, rows 1 and 2 have 119/120, row 3 has 60 (two
CLUTs), rows 4..11 have 0, row 12 onward is pixel data. So 14 CLUTs, not 512.

Because the page sits at VRAM x=896, CLUT `n` is at VRAM `(896 + (n % 4) * 16, n / 4)`
and therefore carries the PS1 id

```
clut_id(n) = (n / 4) * 64 + 56 + (n % 4)        # 56 = 896 / 16
```

giving **56, 57, 58, 59, 120, 121, 122, 123, 184, 185, 186, 187, 248, 249**.
Cross-check: in `work\oracle\vr_capture\scene.csv` every primitive with
`draw_mode = 0x20E` (tpage X=14) carries `palette` in {58, 122, 187} - all three are
in that list, and nothing outside it appears.

**What the art is.** Rendered with its own CLUTs the atlas is roadside scenery: three
tree canopies, a bush, an ornate lamp post, an oval hedge, a diagonal guardrail,
and a lower band of benches, fences, bollards, a post box and a fire hydrant. The
14 CLUTs are lighting/season variants of the same art - cluts 11, 12 and 13 are the
green summer sets, cluts 0 and 5 are autumn red/orange, and the rest are pale
blue-grey or white winter/dusk sets. Entry 0 is `0x0000` (transparent) in all 14.

`G_OBJ01.GTP` is the only `.GTP` on the disc.

**Who binds it.** The scene cells do. A cell primitive whose `texRef` is `0x7800`
(`30 << 10`) takes the inline arm of the cell texture fix-up pass, which reads its page from the
single runtime record at guest `0x800D6160` - `tpage 0x000E`, i.e. VRAM x = 896 - and builds the
CLUT id from EXE config group 8, which for race set 1 gives exactly `(896 + (pal % 4) * 16, pal / 4)`,
the addresses above. Confirmation from the shipped data: over all 102 stream files the 10732 cell
primitives on this page carry `pal` in **0..13**, matching the 14 CLUTs one for one, and the three
commonest are `pal` 6, 2 and 11 -> ids **122, 58, 187**, which is the same set the captured draw
stream shows. Full derivation in `docs\formats\scene_cell.md` 12.2 and 12.3. The binding path is
the cell fix-up pass, not a model.

---

## 5a. Cross-check against the model side (`docs\formats\rmd3.md`)

The `RMD3` primitives in the paired `*.GEO` give an independent check on the
`LECT` dimensions decoded above. Using the model probe `tools\scout\rmd3.py`,
the per-`tpage` bounding box of the 8-bit `u,v` fields is:

| model | tpage values | u range | v range | matches |
|---|---|---|---|---|
| `CAR01A.GEO` | 0 only | 0..127 | 0..59 | `LECT` kind 3 in `CARSC.TEX` = **128 x 60** |
| `BBLEVEL1.GEO` | 0 | 0..127 | 0..59 | `LECT` kind 1/2 = **128 x 60** |
| `BBLEVEL1.GEO` | 1..9 | 1..253 | 1..26 | `LECT` kind 5 = **256 x 32** |
| `HAZARD0.GEO` | 0..30 | 0..254 | 0..180 | `LECT` kind 6 = **256 x 192** |
| `HAZARD3.GEO` | 0..32 | 0..254 | 0..180 | `LECT` kind 6 = **256 x 192** |

Nothing overruns, and both 4bpp widths are hit to within one texel. So the
width/height fields at `LECT` +0x12/+0x14 are right, and the `u,v` in a primitive
are **absolute coordinates inside the whole texture**, not relative to a sub-page.
`tpage` is therefore a prop/sub-image id, not an addressing offset: for
`HAZARD0.GEO` each of the 31 `tpage` values owns its own tight, essentially
non-overlapping box in the atlas (e.g. tpage 8 = u 92..111, v 55..65, 34 quads;
tpage 25 = u 1..25, v 33..58; tpage 21 = u 42..43, v 40..54).

### The 4bpp CLUT selector - solved

The model `clut` field is **not** the selector. Two reasons, both from our bytes:

* only six values exist across all 35k primitives in the game, and `0x2011` is used
  both by `CAR01A.GEO` (an **8bpp** 128x60 texture, which needs a 256-entry CLUT)
  and by `HAZARD0.GEO` (a **4bpp** atlas, which needs a 16-entry one). One id cannot
  be both.
* the engine's own texture page table (section 5b) caches a real CLUT id for every
  8bpp page and `0x0000` for every 4bpp page, i.e. it never uses the model value.

What the engine actually does:

1. A 4bpp atlas is uploaded to a VRAM page column and its **CLUTs are parked in the
   spare rows of that same column**, one 16-colour CLUT per row. `G_OBJ01.GTP` is
   the clearest case because the spare rows are the *first* four rows and they ship
   inside the file itself (section 5).
2. Each primitive carries its own PS1 CLUT id selecting one of those rows. The
   CLUT id decodes the usual way: `x = (id & 0x3F) * 16`, `y = id >> 6`.

Worked example, fully closed on our bytes - the roadside object page:

| step | evidence |
|---|---|
| art | `G_OBJ01.GTP` == VRAM (896,0) 64x128 halfwords, byte-identical |
| page | VRAM x=896 -> tpage X=14 -> `draw_mode` `0x20E` |
| CLUTs | rows 0..3 of the same file, ids 56..59 / 120..123 / 184..187 / 248 / 249 |
| use | every `0x20E` primitive in `scene.csv` carries `palette` in {58, 122, 187} |

Worked example, second page - the road-sign atlas:

| step | evidence |
|---|---|
| art | `HAZARD4.TEX` payload == VRAM (704,0), all 192 rows byte-identical |
| page | VRAM x=704 -> tpage X=11, 4bpp |
| CLUTs | column 3 of the same page (VRAM x = 704 + 48 = 752), rows ~208..241 |
| source | those 32-byte blocks are in `DATA\GAMEBIN1.DAT`, which stores raw VRAM-shaped blocks (128 bytes = one 64-halfword VRAM row); e.g. VRAM (752,208) = `GAMEBIN1.DAT` +0x121D8, and VRAM (752,255) = +0x1194C |
| proof | rendering `HAZARD0.TEX` with the CLUT at `GAMEBIN1.DAT` +0x121D8 gives white SPEED LIMIT plates with black digits, a white STOP octagon with a red ring, "NO PARK ANY TIME" white on red, "CALL BOX 7-24" yellow on black, a blue chevron barrier and green turn/pedestrian signs |

### What is still open for the sign atlas

The CLUT *column* is identified and its contents are traced to the disc, but
**which row each individual prop uses is not**. Ruled out here:

* `row = base + prim.tpage` (the prop index from `HAZARD4.GEO`). Rendering each
  prop's own UV box with `row = 208 + tpage` produces noise for most props;
  only the large multi-quad props
  happen to land on a usable row.
* one CLUT for the whole atlas. Scoring all 32768 candidate 16-colour CLUTs in the
  real VRAM for "the STOP octagon is red" and "the speed plates are white" at the
  same time tops out at 0.63; no single palette does both.
* `palette = 16364` (VRAM (704,255) = `GAMEBIN1.DAT` +0x1194C) as the atlas CLUT.
  That block is a pure 16-level **grey ramp** - every entry has r=g=b - so it cannot
  produce a red STOP sign, and the vertex colours in `scene.csv` are desaturated
  (`5C4D51`, `62504E`, `808080`), so modulation cannot add the hue either. The
  primitives that carry 16364 in the capture sample `v` 192..254, i.e. the art
  *below* the 192-row sign atlas in the same page - not the signs.

Best guess for where the answer lives: a per-object record in the level stream
(`DATA\RACE?_*.STP`).

## 5b. The engine's texture page table, and the savestate that proves it

Ground truth for everything above comes from two places that can be read directly:

* `work\oracle\state\{rr-race,rr-pack,rr-grid,quick}\vram.bin` - 1 MiB of real
  VRAM (1024 x 512 halfwords, 2048 bytes per row), extracted from emulator
  savestates (`tools\scout\savestate.py`).
* the engine's own texture page table at guest `0x800D5F70`, present both in our raw
  RAM dumps and inside the savestates.

Record layout (12 bytes), re-derived here from `ram_000200.bin` and all four states:

```
+0x00  u8  LECT id            the +0x10 field of the LECT chunk
+0x01  u8  sub-page slot      0..3 (8bpp column) / 0..7
+0x02  u8  X offset in halfwords inside the column
+0x03  u8  Y offset in rows inside the column
+0x04  u16 0
+0x06  u16 VRAM x of the column, in halfwords
+0x08  u16 PS1 tpage word     bits0..3 = x/64, bit4 = y/256, bits7..8 = depth
+0x0A  u16 PS1 CLUT id        real id for 8bpp; ALWAYS 0x0000 for a 4bpp page
```

Arrays: two 4bpp records at +0x000, the 8bpp arrays at +0x018 (count u32 at +0x198)
and +0x138 (count at +0x19C).

**Verification.** For `rr-race`, all 14 records that name a texture present on the disc
point at a byte-exact copy of that texture in `vram.bin`:

| id | kind | VRAM (x,y) | source chunk |
|---|---|---|---|
| 0x31 | 5 (4bpp 256x32) | (960,224) | `BBLEVEL1.TEX` |
| 0x93 | 6 (4bpp 256x192) | (704,0) | `HAZARD4.TEX`, 192/192 rows |
| 0x06 0x43 0x07 0x44 0x39 | 2 (8bpp 128x60) | (640..704, 256..436) | `BBLEVEL1.TEX` |
| 0x34 0x33 0x04 0x03 0x02 0x01 0x38 | 1 (8bpp 128x60) | (704..832, 256..436) | `BBLEVEL1.TEX` |

`rrverify texbind --state <dir> --data work\disc_us\DATA` re-derives the model -> `LECT` chunk ->
palette binding from the disc and diffs it against a captured state (gate "oracle: model -> texture
-> palette against the disc" for `rr-race`, `rr-pack` and `quick`).

**8bpp is one CLUT per page**, cached at +0x0A, and it is **128 entries**, not 256:
the ids in `rr-race` are 0x7d78, 0x7cb0, 0x7d28, 0x7d30, 0x7d38, 0x7ce8, 0x7cf0,
0x7cf8, 0x7ca8 -> VRAM x in {640, 768, 896}, y in {498..501}, and x=896 + 128
halfwords is exactly the 1024-halfword VRAM edge. The highest 8bpp index actually
used in those pages is 127. That is what the 128-entry `TSLP` palettes of section
1.3 are for. Two of those ids (0x7d28, 0x7cf8) also appear as `palette` on 8bpp
primitives in `work\oracle\vr_capture\scene.csv`.

Note the captures in `work\oracle\vr_capture\*.csv` and the savestates are from
different runs of the game: only 2..12 of ~41 CLUT ids per capture land on palette-looking
data in any given state's VRAM. Cross-checks that mix the two are therefore only
valid for things that do not move between sessions, such as `G_OBJ01.GTP`.

---

## 6. `*.PFN` bitmap fonts (`FNTP`) - fully decoded

4 files: `DATA\GAMEFONT.PFN`, `DATA\FE\MINIFONT.PFN`, `DATA\FE\BTN_FONT.PFN`,
`DATA\FE\HDR_FONT.PFN`.

```
+0x00  char[4] 'FNTP'
+0x04  u32   file size                     matches the real size in all 4
+0x08  u16   0x0065 (101)                  unknown, identical in all 4
+0x0A  u16   glyph count                   96 in all 4
+0x0C  u32   9, except GAMEFONT = 13        unknown
+0x10  u8[3] 0
+0x13  u8    line height                   12, except HDR_FONT = 15
+0x14  u32   first character code          32 (space) in all 4
+0x18  u32   0
+0x1C  u32   offset of the bitmap block    0x440 in all 4
+0x20  glyph table: count records of 11 bytes
         u16 code; u8 w; u8 h; u16 x; u16 y; u8 advance; s8 xoff; s8 yoff
```

0x20 + 96*11 = 0x440 = the pointer at +0x1C, exactly - which is what pins the 11-byte
record size. Codes run 32..127 with no gaps (checked by walking the table).

Bitmap block:

```
+0x00  u16  VRAM width in halfwords   64 in all 4
+0x02  u16  ?                         0
+0x04  u16  width in pixels           248 for BTN_FONT, 256 for the other three
+0x06  u16  height in pixels          MINIFONT 41, BTN_FONT 41, HDR_FONT 59, GAMEFONT 62
+0x08  u32  0
+0x0C  u16  ?  u16  ?                 0x0300/0x0308 and 0x01C2..0x01D7      unknown
+0x10  width*height 4bpp indices, no CLUT
```

`bitmapOffset + 16 + width*height/2 == file size` for all four files
(MINIFONT 0x440+16+256*41/2 = 6352; BTN_FONT 248*41/2 -> 6188; HDR_FONT 256*59/2 ->
8656; GAMEFONT 256*62/2 -> 9040). Rendering the sheet with a grey ramp gives a
legible A-Z / a-z / 0-9 / punctuation sheet whose glyph boxes line up with the
`(x,y,w,h)` in the table - e.g. MINIFONT `'!'` is 2x9 at (178,19), `'#'` 10x9 at
(166,19). **Unknown:** the two halfwords at bitmap+0x0C, header +0x08 and +0x0C, and
which CLUT the font is drawn with (not in the file).

---

## 7. Scan result

A decode scan over all 487 files:

| kind | status | count | note |
|---|---|---|---|
| `TIM` | PASS | 98 | every byte accounted for |
| `EACHUNK` | PASS | 12 | 7 `LECT`-magic `.TEX` + 5 `KNBP`-magic `.TEX` |
| `EACHUNK` | PARTIAL | 6 | the 6 `.MRO`: image chunk decodes, `RMD3` model does not |
| `EACHUNK` | SKIP | 124 | `.GEO` / `.PSX`: chain walks to EOF but holds no image chunks |
| `DASHPAGE` | PASS | 2 | `DASH1P.TEX`, `DASH2P.TEX` |
| `FNTP` | PASS | 4 | |
| `SHPP` | PASS | 1 | 26/26 entries exact |
| `GTP` | PASS | 1 | |
| other | SKIP | 234 | audio/video/text/model, 8 files over 8 MB not read |

---

## 8. Open questions

1. **Per-prop CLUT row for the sign atlas** (`HAZARD*.TEX`). The mechanism, the CLUT
   column and its disc source are all settled in section 5a; what is missing is
   which row each prop uses. `row = base + prim.tpage` is ruled out. Look for a
   per-object record in `DATA\RACE?_*.STP`, or disassemble the sign draw loop in
   `RASHCDI.BIN`.
2. **`CTKP` middle `u16`.** Not a size and not an offset.
3. **`DATA\GAMEBIN1.DAT` beyond textures.** Its first twelve 106612-byte blocks are
   per-level bundles, and one 16-byte field of a bundle - four `u8 r, g, b, flag` entries at
   `+0x1145C` - is the sky-gradient colour set the race overwrites the EXE globals at guest
   `0x800523E0` with. The parser is `src\rrformats\sky_gradient.h`; the gate "render: the
   product's gradient packets equal the original's" confirms it byte for byte. The rest of a
   bundle's layout is still unmapped, and so is the code that copies the field.
4. **`LECT` +0x0F.** Constant per kind, 31..35 across `HAZARD0..5`.
5. **Dash CSV nTex spare columns** and the CLUT-slot rule (inferred from images only).
6. Unknown header words in `FNTP` (+0x08, +0x0C, bitmap+0x0C) and `SHPP`
   (entry `blockSize`, +0x08..+0x0F - all zero in this game, so untestable here).
