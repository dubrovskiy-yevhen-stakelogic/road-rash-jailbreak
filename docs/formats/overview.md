# Road Rash: Jailbreak (USA, SLUS_01053) - file format inventory

Source: the player's own disc image `Road Rash - Jailbreak (USA).bin` (raw MODE2/2352, 249159 sectors),
walked as ISO9660 and scanned for the first four bytes of every file. The extract lives in `work\disc_us`
(never in git); `rrtool` reads the image directly (`rrtool list` / `rrtool extract`).

## Disc layout
- 2 directories, 487 files. Boot: `SYSTEM.CNF` -> `cdrom:\SLUS_010.53;1`, TCB 4, EVENT 16, STACK 0x801FFFF0.
- `SLUS_010.53` 311296 bytes, SHA-1 `67ed165a2c517d4e6106fb0dfa324d66dd9a76f1`.
  PS-X EXE: entry `0x8004032C`, load `0x80010000`, text size `0x0004B800`, initial SP `0x801FFFF0`.
- Overlays (loaded off-disc, not in DATA): `RASHCDF.BIN` 283936 SHA-1 `a3fec4b4e9292c358d0f6dc529843f5d8f25924a`,
  `RASHCDG.BIN` 467080 SHA-1 `cfe43a7786759f2cb9c57751cf99e84d1074782c`,
  `RASHCDI.BIN` 79196 SHA-1 `9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06`.
  All three are code overlays loaded at `0x8005B5E8` (the base is derived for RASHCDI in `road.md`);
  RASHCDF/G start with a pointer table into `0x8005xxxx`.
- `DATA\ZZZDUMMY.FIL` 30720000 bytes of ASCII digit filler = disc padding, no content.

## Magic inventory (first 4 bytes, little-endian storage)
| Tag | Extension | Count | Meaning (working hypothesis) |
|---|---|---|---|
| `RMD3` | .GEO | 112 | model container; `DOD3`/`DPD3`/`BBD3` sub-chunks. Bikes, riders, traffic cars, peds, hazards. DECODED, see `rmd3.md` |
| `DMD3` | .PSX | 12 | animation tables (ANIMTBL*) - container decoded, payload not; see `rmd3.md` |
| `LECT` | .TEX .MRO | 13 | first chunk of the chained EA container (see `textures.md`) |
| `KNBP` | .TEX | 5 | same container, starts with a CLUT bank chunk (BBLEVEL*/BBLEVJB*) |
| (none) | .TEX | 2 | `DASH1P.TEX` / `DASH2P.TEX`: headerless VRAM page, HUD art + CLUT table |
| (none) | .GTP | 1 | `G_OBJ01.GTP`: a pre-baked 4bpp VRAM page (14 CLUTs + roadside object art), no header |
| `MRPS` | .STP | 99 | race segment file (RACE1_n / RACE2_n) |
| `RGTS` | .GRF | 2 | road graph |
| `MAP_` | .MAP | 2 | road map |
| `COTS` | .TOC | 2 | stream table of contents |
| `CTLR` | .RLS | 2 | stream release/cue list |
| `MDEC` | .STR | 48 | PS1 MDEC BS-v2 pictures, chunk chain - SOLVED, see `video.md` |
| `VLC0` | .WVE | 11 | EA stream: MDEC video + SPU-ADPCM audio - SOLVED, see `video.md` / `audio.md` |
| `FNTP` | .PFN | 4 | bitmap font |
| `LOCH` | .LOC | 2 | localized string table |
| `SHPP` | .PSH | 1 | EA shape/image bank; contains `GIMX` records |
| (0x10) | .TIM | 98 | standard PS1 TIM |
| text | .TXT .CSV | 5 | PLAIN TEXT game data (see below) |

## Free documentation shipped on the disc
- `DATA\ROADGRF1.TXT` / `ROADGRF2.TXT`: the race graph in plain text. `[NUM_ENTRIES]=64`, then a block per race:
  `[RACEID]`, `[START]=node dist dir lane`, `[FINISH]=...`, `[GMAGIC]`, `[RMAGIC]`, `[START_CHECKER]`,
  `[FINISH_CHECKER]`, `[RACEINTS]=n` + one row per intersection (19 ints).
- `DATA\DASH1P.CSV`, `DASH2PH.CSV`, `DASH2PS.CSV`: HUD layout - `ClutTable`, flash timings (`SignFlash`,
  `NitroFlash`, `DemoFlash`, `SplitTimeFlash`, `MsgFlash`), slide params, 5-0 radar/nitro/timer positions, then one row
  per dash TIM with x,y and two extra fields.

## Confirmed cross-file keys
- `GMAGIC` from ROADGRF1.TXT = 945994598 = `0x3862BB66`. Appears at +8 of `MAP_`/`RGTS`/`COTS` and at +8 of `MRPS`.
- `RMAGIC` per race (e.g. race 1 = 945995209 = `0x3862BDC9`) appears at +0x10 of the matching `RACE1_n.STP`.
  So .STP <-> race id linkage is verifiable byte-wise, no guessing. Road set 1 uses GMAGIC `0x3862BB66`
  (63 .STP files), road set 2 uses `0x3862B8BD` (36 .STP files).

### Road / track data (ROADGRF*.TXT, RGTS, COTS, CTLR, MAP_, MRPS, STREAM*.STR)
Decoded - see `docs\formats\road.md`. A race is a route through a network of 25 intersections and
37 roads; both road sets share the same network (the two `.GRF` differ in 4 bytes). `RGTS` holds the
node/road tables, `COTS` maps each road x driving direction onto a byte range of `STREAM<n>.STR`
(gapless 16 KiB-aligned tiling of the whole 118 MB / 74 MB file), and a `.STP` is the preload of the
chunks needed at the start line (`+0x24` = the stream resume offset, proven for all 99 files).
The text graph is parsed by the retail game at runtime (`RASHCDI.BIN`), so it is an exact oracle;
all distance arithmetic in it reproduces from `RGTS` road lengths with 0 mismatches over 441 checks.

## Container details
### RMD3 (.GEO model container) and DMD3 (.PSX animation tables)
Fully decoded byte-exactly - see `docs\formats\rmd3.md`. All 112 `.GEO` parse with every byte
accounted for. `RMD3 -> (DOD3 verts, DPD3{n} quads, BBD3 bbox)*`, chained to EOF. Primitives are
20-byte textured quads carrying 8-bit UVs, a PS1 CLUT id and a small `tpage` INDEX into the
paired `.TEX` page set (0 for every car/ped primitive, 0..9 in BBLEVEL*, 0..34 in HAZARD*).
The only car-side texture file is `CARSC.TEX`; `CAR*.GEO` and
`PED01A.GEO` have no paired `.TEX` and every car primitive uses `tpage = 0`. Only six distinct
CLUT words exist game-wide (`0x2011, 0xA011, 0x1811, 0x2091, 0x9811, 0xA091`), decoding to VRAM
CLUT positions x=272, y=96/128/130 with bit 15 as an unknown flag.
Sub-meshes are per-part local frames: a multi-part model needs one translation per part to
stand up, and the attachment topology is a static table in the race overlay `RASHCDG.BIN`
(file +0x711A8 / +0x7126C), not in the `.GEO` - see `rmd3.md` section 9. All 43 multi-part
groups assemble to within 9 units of their authored `BBD3` box.
`BBLEVEL*.GEO` are bike + rider models, not level scenery, and
`CAR<nn>A/B.GEO` are bundles of 4/6 models out of a shared pool of 16 traffic cars, not two LODs
of one vehicle.

### Texture containers (LECT / KNBP / TSLP / CTKP, TIM, SHPP, GTP, FNTP, DASH pages)
Decoded byte-exactly - see `docs\formats\textures.md`. `LECT` is just the first chunk of a generic
chained `tag+u32 size` container that also carries `TSLP`, `KNBP`, `CTKP`, `RMD3` and `DMD3` chunks;
the chain covers every byte of all 142 chunked files. A `LECT` chunk is a 0x18-byte header
(`kind`, `bpp`, `id`, `width`, `height`) followed either by a complete embedded PS1 TIM (8bpp kinds
1/2/3, all 128x60) or by raw 4bpp pixels with no CLUT (kinds 5 and 6, 256x32 and 256x192).
All 98 `.TIM`, 4 `.PFN`, `FEMISC.PSH` (26 `GIMX` 16bpp images), `G_OBJ01.GTP` (one 4bpp texture
page, byte-identical to its VRAM rectangle in a captured state) and both `DASH?P.TEX` HUD pages decode
fully.

Cross-check with the model side: the `u,v` in an `RMD3` primitive are absolute inside the whole
`LECT` texture, and the per-`tpage` bounding boxes land exactly inside 128x60 / 256x32 / 256x192.
`tpage` is a prop/sub-image id, not an addressing offset.

The 4bpp CLUT selection is solved in `textures.md` section 5a (the `RMD3` `clut` field is not the
selector; each primitive's CLUT id picks a row parked beside the atlas in VRAM), and the extra palette
banks in `BBLEV*.TEX` (`TSLP`, `KNBP`) are racer palette banks of 128 entries (`textures.md` 1.3).

## Open questions
- The per-prop CLUT row of the road-sign atlas (`HAZARD*.TEX`), see `textures.md` section 8.
- `.VUK` and `.QTI` are unidentified (`video.md` section 7). `.BI`, `.PH`, `.EN`, `.CA` and `.VI` are
  inventoried in `population.md` section 7: `LEVEL<n>.BI` is decoded, a bike `.PH` partly, the rest
  only located in RAM.
- `.ALB` (music banks), `.TCM` (loading-screen MDEC archives), `AUDTAUNT.STR` (taunt speech)
  and the `LOCH` string tables are solved - see `audio.md` and `video.md`.
