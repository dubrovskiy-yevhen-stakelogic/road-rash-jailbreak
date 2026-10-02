#!/usr/bin/env python3
"""
Road Rash: Jailbreak (USA, SLUS_01053) - texture/image container probe.

An independent Python decoder: the acceptance run compares rrtool's C++ texture
decoder with its output pixel for pixel (docs\\formats\\textures.md). Everything
it claims is derived from the bytes of the disc extract in work\\disc_us. It never
writes game data into the repository; all output goes to a caller-supplied
directory (the acceptance run uses work\\tex).

Formats handled
  TIM    plain PS1 TIM (id 0x10)
  EACHUNK a chained tag/size container used by *.TEX / *.MRO:
          chunk tags seen: LECT (texture), TSLP (palette bank), KNBP (16bpp
          bank), CTKP (table of contents), RMD3 (model, not decoded here)
  SHPP   EA shape bank (DATA\\FE\\FEMISC.PSH), directory tag GIMX
  GTP    DATA\\G_OBJ01.GTP - one pre-baked 256x128 4bpp texture page of
         roadside objects; rows 0..3 carry its own 14 CLUTs
  FNTP   DATA\\FE\\*.PFN bitmap fonts
  DASHPAGE DATA\\DASH?P.TEX - headerless VRAM page (4bpp art + CLUT table),
         laid out by the sibling DATA\\DASH?P*.CSV

Usage
  python tex.py info <file>
  python tex.py png  <file> <outdir> [--clut <gtpfile>] [--clut-index N]
                                    [--vram <swanstation .sav>]
  python tex.py scan <dir> [outdir]
  python tex.py vram <swanstation .sav> <outdir>

`--vram` sources the runtime CLUT for an 8bpp LECT chunk from the engine's own
texture page table inside a savestate, so the PNG comes out in the colours the
game actually drew.  `vram` dumps a savestate's whole VRAM plus that table.
"""

import os
import sys
import glob
import zlib
import struct

# ---------------------------------------------------------------- primitives

def u8(b, o):
    return b[o]


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u24(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


def write_png(path, width, height, rgba):
    """Minimal RGBA8 PNG writer (zlib + struct only)."""
    if width <= 0 or height <= 0:
        raise ValueError("bad size %dx%d" % (width, height))
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)                       # filter type 0 (None)
        raw += rgba[y * stride:(y + 1) * stride]
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "wb") as f:
        f.write(png)


# ------------------------------------------------------------ colour (BGR555)
# PS1 16-bit texel: bit15 = STP (semi-transparency flag), bits 14..10 = blue,
# 9..5 = green, 4..0 = red.  Convention used here, which is what the hardware
# does for a textured primitive without the "raw texture" bit:
#   value 0x0000            -> fully transparent
#   STP set,   value != 0   -> opaque here, STP reported separately
#   STP clear, value != 0   -> opaque

def bgr555_to_rgba(v):
    r = v & 0x1F
    g = (v >> 5) & 0x1F
    b = (v >> 10) & 0x1F
    a = 0 if v == 0 else 255
    # 5 -> 8 bit with replicated high bits (exact, monotone, 0->0, 31->255)
    return ((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), a)


def clut_from_bytes(b, off, count):
    return [bgr555_to_rgba(u16(b, off + 2 * i)) for i in range(count)]


def grey_clut(n):
    out = []
    for i in range(n):
        v = i * 255 // max(1, n - 1)
        out.append((v, v, v, 0 if i == 0 else 255))
    return out


def indexed_to_rgba(data, off, width, height, bpp, clut):
    """Unpack `width`x`height` indexed pixels; 4bpp is low-nibble-first."""
    out = bytearray(width * height * 4)
    ncol = len(clut)
    p = 0
    if bpp == 8:
        row_bytes = width
        for y in range(height):
            base = off + y * row_bytes
            for x in range(width):
                idx = data[base + x]
                c = clut[idx] if idx < ncol else (255, 0, 255, 255)
                out[p] = c[0]; out[p + 1] = c[1]; out[p + 2] = c[2]; out[p + 3] = c[3]
                p += 4
    elif bpp == 4:
        row_bytes = (width + 1) // 2
        for y in range(height):
            base = off + y * row_bytes
            for x in range(width):
                byte = data[base + (x >> 1)]
                idx = (byte & 0x0F) if (x & 1) == 0 else (byte >> 4)
                c = clut[idx] if idx < ncol else (255, 0, 255, 255)
                out[p] = c[0]; out[p + 1] = c[1]; out[p + 2] = c[2]; out[p + 3] = c[3]
                p += 4
    else:
        raise ValueError("bpp %d" % bpp)
    return out


def direct16_to_rgba(data, off, width, height):
    out = bytearray(width * height * 4)
    p = 0
    for i in range(width * height):
        c = bgr555_to_rgba(u16(data, off + 2 * i))
        out[p] = c[0]; out[p + 1] = c[1]; out[p + 2] = c[2]; out[p + 3] = c[3]
        p += 4
    return out


def clut_strip_rgba(clut, cell=8):
    """Render a palette as a horizontal strip so it can be eyeballed."""
    n = len(clut)
    w, h = n * cell, cell * 2
    out = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            c = clut[min(n - 1, x // cell)]
            p = (y * w + x) * 4
            out[p] = c[0]; out[p + 1] = c[1]; out[p + 2] = c[2]; out[p + 3] = 255
    return w, h, out


# ------------------------------------------------------------------ TIM

TIM_BPP = {0: 4, 1: 8, 2: 16, 3: 24}


def parse_tim(b, base=0):
    """Parse a TIM starting at `base`. Returns dict or raises ValueError."""
    if u32(b, base) != 0x10:
        raise ValueError("not a TIM (id=0x%x)" % u32(b, base))
    flags = u32(b, base + 4)
    pmode = flags & 7
    has_clut = bool(flags & 8)
    if pmode not in TIM_BPP:
        raise ValueError("bad pmode %d" % pmode)
    bpp = TIM_BPP[pmode]
    o = base + 8
    clut = None
    clut_block = None
    if has_clut:
        bnum = u32(b, o)
        cdx, cdy, cw, ch = u16(b, o + 4), u16(b, o + 6), u16(b, o + 8), u16(b, o + 10)
        if bnum != 12 + cw * ch * 2:
            raise ValueError("clut bnum %d != %d" % (bnum, 12 + cw * ch * 2))
        clut_block = dict(bnum=bnum, dx=cdx, dy=cdy, w=cw, h=ch, off=o)
        clut = [clut_from_bytes(b, o + 12 + row * cw * 2, cw) for row in range(ch)]
        o += bnum
    bnum = u32(b, o)
    dx, dy, w, h = u16(b, o + 4), u16(b, o + 6), u16(b, o + 8), u16(b, o + 10)
    if bnum != 12 + w * h * 2:
        raise ValueError("img bnum %d != %d" % (bnum, 12 + w * h * 2))
    img_block = dict(bnum=bnum, dx=dx, dy=dy, w=w, h=h, off=o)
    # `w` is in 16-bit VRAM words; convert to pixels for the storage mode.
    px_w = {4: w * 4, 8: w * 2, 16: w, 24: w * 2 // 3}[bpp]
    return dict(base=base, flags=flags, bpp=bpp, has_clut=has_clut,
                clut=clut, clut_block=clut_block, img=img_block,
                px_w=px_w, px_h=h, data_off=o + 12, total=(o + bnum) - base)


def tim_to_rgba(b, t, clut_row=0):
    if t["bpp"] == 16:
        return t["px_w"], t["px_h"], direct16_to_rgba(b, t["data_off"], t["px_w"], t["px_h"])
    if t["bpp"] == 24:
        w, h = t["px_w"], t["px_h"]
        out = bytearray(w * h * 4)
        for i in range(w * h):
            o = t["data_off"] + 3 * i
            out[4 * i] = b[o]; out[4 * i + 1] = b[o + 1]
            out[4 * i + 2] = b[o + 2]; out[4 * i + 3] = 255
        return w, h, out
    clut = t["clut"][clut_row] if t["clut"] else grey_clut(1 << t["bpp"])
    return t["px_w"], t["px_h"], indexed_to_rgba(b, t["data_off"], t["px_w"], t["px_h"],
                                                 t["bpp"], clut)


# ------------------------------------------------- EA chunked container (.TEX/.MRO)

KNOWN_TAGS = (b"LECT", b"TSLP", b"KNBP", b"CTKP", b"RMD3", b"DMD3")


def chunk_chain(b):
    """Walk the tag/size chain. Returns (chunks, complete_bool)."""
    chunks = []
    o = 0
    while o + 8 <= len(b):
        tag = bytes(b[o:o + 4])
        size = u32(b, o + 4)
        if tag not in KNOWN_TAGS or size < 8 or o + size > len(b):
            return chunks, False
        chunks.append((o, tag, size))
        o += size
    return chunks, (o == len(b))


def parse_lect(b, off, size):
    """LECT chunk header (all offsets relative to the chunk start).

    +0x00 char[4] 'LECT'
    +0x04 u32     chunk size (offset of the next chunk)
    +0x08 u32     0
    +0x0C u8      kind   1,2,3 -> payload is a TIM ; 5,6 -> raw 4bpp, no CLUT
    +0x0D u8      bpp    4 or 8
    +0x0E u8      0
    +0x0F u8      slot   (unknown; sequential across sibling files)
    +0x10 u16     id     (matches the CTKP table of contents)
    +0x12 u16     width  in pixels
    +0x14 u32     height in pixels
    +0x18 payload
    """
    d = dict(off=off, size=size, zero=u32(b, off + 8), kind=u8(b, off + 0x0C),
             bpp=u8(b, off + 0x0D), pad=u8(b, off + 0x0E), slot=u8(b, off + 0x0F),
             id=u16(b, off + 0x10), width=u16(b, off + 0x12), height=u32(b, off + 0x14))
    d["payload"] = off + 0x18
    d["tim"] = None
    if off + 0x18 + 4 <= len(b) and u32(b, off + 0x18) == 0x10:
        try:
            d["tim"] = parse_tim(b, off + 0x18)
        except ValueError as e:
            d["tim_error"] = str(e)
    return d


def parse_ctkp(b, off, size):
    """CTKP = table of contents for the LECT chunks in the same file.

    +0x08 u32 count, then count records of
    +0x00 u32 kind (matches the LECT kind byte)
    +0x04 u16 unknown
    +0x06 u16 id   (matches LECT +0x10)
    """
    n = u32(b, off + 8)
    recs = []
    for i in range(n):
        r = off + 12 + i * 8
        if r + 8 > off + size:
            break
        recs.append(dict(kind=u32(b, r), unk=u16(b, r + 4), id=u16(b, r + 6)))
    return dict(count=n, recs=recs, exact=(12 + n * 8 == size))


def parse_tslp(b, off, size):
    """TSLP = bank of CLUTs.  +0x08 u16 count, +0x0A u16 ?, +0x0C u16 entries,
    +0x0E u16 rows, +0x10 u32 ?, +0x14 data (count*entries BGR555 halfwords)."""
    d = dict(off=off, size=size, count=u16(b, off + 8), f0a=u16(b, off + 0x0A),
             entries=u16(b, off + 0x0C), rows=u16(b, off + 0x0E), f10=u32(b, off + 0x10))
    d["data"] = off + 0x14
    d["exact"] = (0x14 + d["count"] * d["entries"] * 2 == size)
    return d


def parse_knbp(b, off, size):
    """KNBP = one 16bpp block.  +0x08 u16 id, +0x0A u16 ?, +0x0C u16 width,
    +0x0E u16 height, +0x10 u32 ?, +0x14 data (width*height BGR555)."""
    d = dict(off=off, size=size, id=u16(b, off + 8), f0a=u16(b, off + 0x0A),
             width=u16(b, off + 0x0C), height=u16(b, off + 0x0E), f10=u32(b, off + 0x10))
    d["data"] = off + 0x14
    d["exact"] = (0x14 + d["width"] * d["height"] * 2 == size)
    return d


# ------------------------------------------------------------------ SHPP

def parse_shpp(b):
    if bytes(b[:4]) != b"SHPP":
        raise ValueError("not SHPP")
    size = u32(b, 4)
    count = u32(b, 8)
    dirtag = bytes(b[12:16])
    ents = []
    for i in range(count):
        o = 16 + i * 8
        ents.append(dict(name=bytes(b[o:o + 4]).decode("latin1"), off=u32(b, o + 4)))
    for i, e in enumerate(ents):
        o = e["off"]
        e["code"] = u8(b, o)
        e["blocksize"] = u24(b, o + 1)
        e["w"] = u16(b, o + 4)
        e["h"] = u16(b, o + 6)
        e["f8"] = u16(b, o + 8)
        e["fa"] = u16(b, o + 0x0A)
        e["fc"] = u16(b, o + 0x0C)
        e["fe"] = u16(b, o + 0x0E)
        e["data"] = o + 16
        nxt = ents[i + 1]["off"] if i + 1 < len(ents) else size
        e["span"] = nxt - o
        e["exact"] = (16 + e["w"] * e["h"] * 2 == e["span"])
    return dict(size=size, count=count, dirtag=dirtag, ents=ents,
                size_ok=(size == len(b)))


# ------------------------------------------------------------------ GTP

# G_OBJ01.GTP is one pre-baked PS1 texture page, NOT a palette bank: 64
# halfwords x 128 rows = 16384 bytes, uploaded verbatim to VRAM (896, 0).
# Verified byte-identical against work\oracle\state\rr-race\vram.bin.
# Read as 4bpp it is 256x128 pixels:
#   rows  0..3    14 sixteen-colour CLUTs, 4 per row, each ending 0x03FF
#   rows  4..11   zero
#   rows 12..127  the object art (trees, bushes, hedges, lamp posts, fences)
GTP_W, GTP_H = 256, 128
GTP_ROW = GTP_W // 2
GTP_CLUTS = 14
GTP_ART_Y = 12
GTP_VRAM_X = 896


def parse_gtp(b):
    if len(b) != GTP_ROW * GTP_H:
        raise ValueError("not a %dx%d 4bpp page" % (GTP_W, GTP_H))
    n = sum(1 for i in range(16)
            if b[(i // 4) * GTP_ROW + (i % 4) * 32:][:32] != bytes(32))
    return dict(w=GTP_W, h=GTP_H, ncluts=n, art_y=GTP_ART_Y)


def gtp_clut(b, i):
    """CLUT i of the page; it lives at VRAM (896 + (i%4)*16, i/4)."""
    return clut_from_bytes(b, (i // 4) * GTP_ROW + (i % 4) * 32, 16)


def gtp_clut_id(i):
    """The PS1 CLUT id the engine uses for CLUT i of this page."""
    return (i // 4) * 64 + (GTP_VRAM_X // 16) + (i % 4)


# ------------------------------------------- disc-side CLUTs for 4bpp atlases
# A 4bpp atlas carries no palette of its own.  At run time its CLUTs sit in the
# spare rows of its own VRAM page column, and they are uploaded from a raw
# VRAM-shaped block on the disc (128 bytes = one 64-halfword VRAM row).
# The entries below were each located by finding the exact 32 bytes of a CLUT
# read out of work\oracle\state\rr-race\vram.bin inside a disc file.
CLUT_SOURCES = {
    # LECT id -> (file under DATA, byte offset, note)
    0x32: ("GAMEBIN1.DAT", 0x121D8, "road-sign atlas, VRAM(752,208), clut id 13359"),
    0x93: ("GAMEBIN1.DAT", 0x121D8, "road-sign atlas, VRAM(752,208), clut id 13359"),
    0x94: ("GAMEBIN1.DAT", 0x121D8, "road-sign atlas, VRAM(752,208), clut id 13359"),
    0x96: ("GAMEBIN1.DAT", 0x121D8, "road-sign atlas, VRAM(752,208), clut id 13359"),
}


def load_clut_source(data_dir, lect_id):
    """Return (clut, note) for a 4bpp LECT id, or (None, None)."""
    ent = CLUT_SOURCES.get(lect_id)
    if not ent or not data_dir:
        return None, None
    path = os.path.join(data_dir, ent[0])
    if not os.path.exists(path):
        return None, None
    with open(path, "rb") as f:
        f.seek(ent[1])
        blob = f.read(32)
    if len(blob) < 32:
        return None, None
    return clut_from_bytes(blob, 0, 16), "%s+0x%X (%s)" % (ent[0], ent[1], ent[2])


# ------------------------------------------------------------------ FNTP

GLYPH_REC = 11


def parse_fntp(b):
    """DATA\\FE\\*.PFN bitmap font.

    +0x00 char[4] 'FNTP'
    +0x04 u32  file size
    +0x08 u16  0x0065 in all three fonts (unknown)
    +0x0A u16  glyph count (96)
    +0x0C u32  9 in all three fonts (unknown)
    +0x10 u8[3] 0 ; +0x13 u8 line height (12 / 12 / 15)
    +0x14 u32  first character code (32 = space)
    +0x18 u32  0
    +0x1C u32  offset of the bitmap block (= 0x20 + count*11)
    +0x20 glyph table, `count` records of 11 bytes:
          u16 code; u8 w; u8 h; u16 x; u16 y; u8 advance; s8 ?; s8 ?
    bitmap block:
          u16 vram width in halfwords; u16 ?; u16 width px; u16 height px;
          u32 0; u16 ?; u16 ?; then width*height 4bpp pixels (no CLUT)
    """
    if bytes(b[:4]) != b"FNTP":
        raise ValueError("not FNTP")
    d = dict(size=u32(b, 4), size_ok=(u32(b, 4) == len(b)),
             f08=u16(b, 8), count=u16(b, 0x0A), f0c=u32(b, 0x0C),
             line_height=u8(b, 0x13), first_code=u32(b, 0x14),
             f18=u32(b, 0x18), bmp=u32(b, 0x1C))
    d["table_ok"] = (0x20 + d["count"] * GLYPH_REC == d["bmp"])
    g = []
    for i in range(d["count"]):
        o = 0x20 + i * GLYPH_REC
        code, w, h, x, y = struct.unpack_from("<HBBHH", b, o)
        g.append(dict(code=code, w=w, h=h, x=x, y=y, adv=b[o + 8],
                      o1=struct.unpack_from("<b", b, o + 9)[0],
                      o2=struct.unpack_from("<b", b, o + 10)[0]))
    d["glyphs"] = g
    o = d["bmp"]
    d["vram_w"] = u16(b, o)
    d["bw"] = u16(b, o + 4)
    d["bh"] = u16(b, o + 6)
    d["pix"] = o + 16
    d["exact"] = (d["pix"] + d["bw"] * d["bh"] // 2 == len(b))
    return d


# ----------------------------------------------------------- DASH?P.TEX page
# Headerless dump of a VRAM rectangle 64 halfwords wide by 256 rows:
#   rows 0..149   4bpp art, 256 px per row (128 bytes)
#   row  150..    the CLUT table announced by `ClutTable,0,150` in DASH?P.CSV:
#                 24 slots of 16 BGR555 entries, 4 slots per row
#   unused space is filled with the halfword 0xC630
DASH_ROW = 128          # bytes per VRAM row for this rectangle
DASH_CLUT_Y = 150       # from DASH?P.CSV "ClutTable,0,150"
DASH_CLUT_SLOTS = 24


def dash_clut(b, n):
    o = (DASH_CLUT_Y + n // 4) * DASH_ROW + (n % 4) * 32
    return clut_from_bytes(b, o, 16)


def dash_pixel(b, x, y):
    byte = b[y * DASH_ROW + (x >> 1)]
    return (byte & 0x0F) if (x & 1) == 0 else (byte >> 4)


def dash_csv_for(path):
    """DASH1P.TEX -> DASH1P.CSV ; DASH2P.TEX -> DASH2PH.CSV (the 2-player head
    to head layout; DASH2PS.CSV is the split-screen variant of the same page)."""
    stem = os.path.splitext(path)[0]
    for cand in (stem + ".CSV", stem + "H.CSV", stem + "S.CSV"):
        if os.path.exists(cand):
            return cand
    return None


def parse_dash_csv(path):
    """DATA\\DASH?P.CSV: `TexArtDashCounts,nTex,nArt,nDash` then nTex rows of
    `name,vramX,vramY,..`, nArt rows of `name,x,y,w,h,texIndex`, nDash rows of
    `name,screenX,screenY,artIndex,..`."""
    rows = [l.strip().split(",") for l in open(path, "r", encoding="latin1") if l.strip()]
    hdr = None
    for i, r in enumerate(rows):
        if r[0] == "TexArtDashCounts":
            hdr = i
            break
    if hdr is None:
        raise ValueError("no TexArtDashCounts row")
    nt, na, nd = (int(rows[hdr][1]), int(rows[hdr][2]), int(rows[hdr][3]))
    i = hdr + 1
    tims = rows[i:i + nt]
    arts = rows[i + nt:i + nt + na]
    dash = rows[i + nt + na:i + nt + na + nd]
    return dict(tims=tims, arts=arts, dash=dash)


def dash_tex_extents(csv):
    """Per-TIM pixel extent, derived from the kArt sub-rectangles."""
    ext = {}
    for a in csv["arts"]:
        try:
            x, y, w, h, t = (int(a[1]), int(a[2]), int(a[3]), int(a[4]), int(a[5]))
        except (ValueError, IndexError):
            continue
        if w <= 0 or h <= 0:
            continue
        e = ext.setdefault(t, [0, 0])
        e[0] = max(e[0], x + w)
        e[1] = max(e[1], y + h)
    return ext


# --------------------------------------------- swanstation savestate / VRAM
# A swanstation ("DUCC7") savestate of this game lays its components out at
# fixed file offsets.  Both bases below were pinned on OUR files, not guessed:
#   RAM  - constant delta 0x1B71 between the savestate and our raw 2 MB RAM
#          dumps, measured on five separate BIOS/kernel strings.
#   VRAM - solved from the engine's own page table (below): the (x, y) each
#          record names must hold a byte-exact copy of the matching LECT
#          payload from the disc.  0x281CF5 satisfies 14 of 14 records.
SAV_MAGIC = b"DUCC7"
SAV_RAM_BASE = 0x1B71
SAV_VRAM_BASE = 0x281CF5
VRAM_W = 1024                       # halfwords
VRAM_ROW = VRAM_W * 2               # bytes
# Guest address of the texture page table (from the EXE-side RE, re-verified
# here on ram_000200.bin and on all four savestates).
PAGETABLE_ADDR = 0x800D5F70
PT_KIND4 = (0x000, 2)               # (offset, capacity) 4bpp pages
PT_KIND1 = (0x018, 24)              # 8bpp, count at +0x198
PT_KIND2 = (0x138, 8)               # 8bpp, count at +0x19C


def open_savestate(path):
    b = open(path, "rb").read()
    if b[:5] != SAV_MAGIC:
        raise ValueError("not a swanstation savestate (%r)" % b[:5])
    return b


def sav_ram(b, addr):
    return SAV_RAM_BASE + (addr & 0x1FFFFF)


def vram_bytes(b, x, y, halfwords):
    o = SAV_VRAM_BASE + y * VRAM_ROW + x * 2
    return b[o:o + halfwords * 2]


def vram_clut(b, clut_id, entries):
    """PS1 CLUT id -> palette. x = (id & 0x3F) * 16, y = id >> 6."""
    x = (clut_id & 0x3F) * 16
    y = clut_id >> 6
    return clut_from_bytes(b, SAV_VRAM_BASE + y * VRAM_ROW + x * 2, entries)


def page_table(b):
    """Decode the texture page table.  One 12-byte record per resident page:

    +0x00 u8  LECT id (the +0x10 field of the LECT chunk)
    +0x01 u8  sub-page slot inside the VRAM column
    +0x02 u8  X offset inside the column, in halfwords
    +0x03 u8  Y offset inside the column, in rows
    +0x04 u16 0
    +0x06 u16 VRAM x of the column, in halfwords
    +0x08 u16 PS1 tpage word (bit7..8 = depth, bits0..3 = x/64, bit4 = y/256)
    +0x0A u16 PS1 CLUT id -- 0x0000 for every 4bpp page, a real id for 8bpp
    """
    base = sav_ram(b, PAGETABLE_ADDR)
    out = []
    n1 = u32(b, base + 0x198)
    n2 = u32(b, base + 0x19C)
    for (start, cnt, grp) in ((PT_KIND4[0], PT_KIND4[1], "4bpp"),
                              (PT_KIND1[0], min(n1, PT_KIND1[1]), "8bpp-a"),
                              (PT_KIND2[0], min(n2, PT_KIND2[1]), "8bpp-b")):
        for i in range(cnt):
            o = base + start + i * 12
            rid = u8(b, o)
            if rid == 0xFF:
                continue
            tp = u16(b, o + 8)
            depth = {0: 4, 1: 8, 2: 16}.get((tp >> 7) & 3)
            out.append(dict(group=grp, id=rid, slot=u8(b, o + 1),
                            x=u16(b, o + 6) + u8(b, o + 2),
                            y=((tp >> 4) & 1) * 256 + u8(b, o + 3),
                            tpage=tp, depth=depth, clut=u16(b, o + 0x0A)))
    return out


# ------------------------------------------------------------------ dispatch

def identify(path, b):
    if len(b) >= 4:
        m = bytes(b[:4])
        if m == b"SHPP":
            return "SHPP"
        if m == b"FNTP":
            return "FNTP"
        if m in KNOWN_TAGS:
            return "EACHUNK"
        if u32(b, 0) == 0x10 and len(b) >= 8 and (u32(b, 4) & ~0xF) == 0:
            return "TIM"
    name = os.path.basename(path).upper()
    if name.endswith(".GTP") and len(b) % 32 == 0:
        return "GTP"
    if name.startswith("DASH") and name.endswith(".TEX") and len(b) == 0x8000:
        return "DASHPAGE"
    return "?"


# ------------------------------------------------------------------ info

def cmd_info(path):
    b = open(path, "rb").read()
    kind = identify(path, b)
    print("%s  %d bytes  -> %s" % (path, len(b), kind))

    if kind == "TIM":
        t = parse_tim(b)
        print("  flags=0x%08x bpp=%d clut=%s  image %dx%d px" %
              (t["flags"], t["bpp"], t["has_clut"], t["px_w"], t["px_h"]))
        if t["clut_block"]:
            c = t["clut_block"]
            print("  clut block bnum=%d vram=(%d,%d) %dx%d" % (c["bnum"], c["dx"], c["dy"], c["w"], c["h"]))
        i = t["img"]
        print("  img  block bnum=%d vram=(%d,%d) %dx%d halfwords" % (i["bnum"], i["dx"], i["dy"], i["w"], i["h"]))
        print("  bytes accounted: %d / %d  %s" % (t["total"], len(b), "OK" if t["total"] == len(b) else "MISMATCH"))

    elif kind == "EACHUNK":
        chunks, complete = chunk_chain(b)
        print("  %d chunks, chain %s" % (len(chunks), "covers every byte" if complete else "INCOMPLETE"))
        for (off, tag, size) in chunks:
            line = "  @0x%06x %s size=0x%-6x" % (off, tag.decode(), size)
            if tag == b"LECT":
                d = parse_lect(b, off, size)
                line += " kind=%d bpp=%d slot=0x%02x id=0x%04x %dx%d" % (
                    d["kind"], d["bpp"], d["slot"], d["id"], d["width"], d["height"])
                if d["tim"]:
                    t = d["tim"]
                    line += "  TIM(%dbpp %dx%d clut@%d,%d)%s" % (
                        t["bpp"], t["px_w"], t["px_h"],
                        t["clut_block"]["dx"] if t["clut_block"] else -1,
                        t["clut_block"]["dy"] if t["clut_block"] else -1,
                        " exact" if 0x18 + t["total"] == size else " SIZE-MISMATCH")
                else:
                    need = d["width"] * d["height"] * d["bpp"] // 8
                    line += "  raw %d bytes%s" % (need, " exact" if 0x18 + need == size else " SIZE-MISMATCH")
            elif tag == b"CTKP":
                c = parse_ctkp(b, off, size)
                line += " count=%d%s" % (c["count"], "" if c["exact"] else " SIZE-MISMATCH")
                line += " ids=[%s]" % ",".join("0x%x/k%d/u0x%x" % (r["id"], r["kind"], r["unk"]) for r in c["recs"])
            elif tag == b"TSLP":
                d = parse_tslp(b, off, size)
                line += " cluts=%d x %d entries (f0a=%d rows=%d f10=0x%x)%s" % (
                    d["count"], d["entries"], d["f0a"], d["rows"], d["f10"],
                    "" if d["exact"] else " SIZE-MISMATCH")
            elif tag == b"KNBP":
                d = parse_knbp(b, off, size)
                line += " id=%d %dx%d 16bpp (f0a=%d f10=0x%x)%s" % (
                    d["id"], d["width"], d["height"], d["f0a"], d["f10"],
                    "" if d["exact"] else " SIZE-MISMATCH")
            print(line)

    elif kind == "SHPP":
        s = parse_shpp(b)
        print("  size=%d (%s) count=%d dir=%s" % (s["size"], "OK" if s["size_ok"] else "MISMATCH",
                                                  s["count"], s["dirtag"].decode()))
        for e in s["ents"]:
            print("  %-4s @0x%06x code=0x%02x blocksize=%d %3dx%-3d span=%-6d %s  f8=%d fa=%d fc=%d fe=%d" % (
                e["name"], e["off"], e["code"], e["blocksize"], e["w"], e["h"], e["span"],
                "exact" if e["exact"] else "SIZE-MISMATCH", e["f8"], e["fa"], e["fc"], e["fe"]))

    elif kind == "GTP":
        g = parse_gtp(b)
        print("  pre-baked 4bpp texture page %dx%d, uploaded to VRAM (%d,0) = tpage X=%d" %
              (g["w"], g["h"], GTP_VRAM_X, GTP_VRAM_X // 64))
        print("  rows 0..3 hold %d sixteen-colour CLUTs, rows 4..11 are zero, "
              "rows %d..%d are the object art" % (g["ncluts"], GTP_ART_Y, GTP_H - 1))
        for i in range(GTP_CLUTS):
            print("    clut %2d -> VRAM (%d,%d), PS1 clut id %d (0x%04x)  %s" % (
                i, GTP_VRAM_X + (i % 4) * 16, i // 4, gtp_clut_id(i), gtp_clut_id(i),
                b[(i // 4) * GTP_ROW + (i % 4) * 32:][:12].hex()))

    elif kind == "FNTP":
        f = parse_fntp(b)
        print("  size=%d (%s) glyphs=%d first=0x%02x lineHeight=%d f08=%d f0c=%d" %
              (f["size"], "OK" if f["size_ok"] else "MISMATCH", f["count"],
               f["first_code"], f["line_height"], f["f08"], f["f0c"]))
        print("  glyph table 0x20..0x%x (%s), bitmap block @0x%x: %dx%d 4bpp, vramW=%d halfwords" %
              (f["bmp"], "exact" if f["table_ok"] else "MISMATCH", f["bmp"],
               f["bw"], f["bh"], f["vram_w"]))
        print("  bytes accounted: %s" % ("OK" if f["exact"] else "MISMATCH"))
        for g in f["glyphs"][:6]:
            print("    '%s' %2dx%-2d at (%3d,%3d) adv=%d off=(%d,%d)" %
                  (chr(g["code"]) if 32 <= g["code"] < 127 else "?",
                   g["w"], g["h"], g["x"], g["y"], g["adv"], g["o1"], g["o2"]))

    elif kind == "DASHPAGE":
        print("  headerless VRAM rectangle, %d halfwords x 256 rows" % (DASH_ROW // 2))
        print("  rows 0..%d: 4bpp art, 256 px wide; CLUT table at row %d, %d slots of 16" %
              (DASH_CLUT_Y - 1, DASH_CLUT_Y, DASH_CLUT_SLOTS))
        csvp = dash_csv_for(path)
        if csvp:
            csv = parse_dash_csv(csvp)
            ext = dash_tex_extents(csv)
            print("  layout from %s: %d TIMs, %d art rects, %d dash items" %
                  (os.path.basename(csvp), len(csv["tims"]), len(csv["arts"]), len(csv["dash"])))
            for i, t in enumerate(csv["tims"]):
                e = ext.get(i, [0, 0])
                print("    [%2d] %-14s vram=(%3s,%3s) %3dx%-3d" % (i, t[0], t[1], t[2], e[0], e[1]))
        else:
            print("  no sibling CSV next to the file, layout unknown")

    else:
        print("  unrecognised")


# ------------------------------------------------------------------ png

def _save(outdir, name, w, h, rgba, made):
    p = os.path.join(outdir, name)
    write_png(p, w, h, rgba)
    made.append(p)


def cmd_png(path, outdir, gtp_path=None, clut_index=None, sav_path=None):
    b = open(path, "rb").read()
    kind = identify(path, b)
    stem = os.path.splitext(os.path.basename(path))[0]
    made = []
    gtp = open(gtp_path, "rb").read() if gtp_path else None
    sav = open_savestate(sav_path) if sav_path else None
    pt = {}
    if sav is not None:
        for r in page_table(sav):
            pt.setdefault(r["id"], r)

    if kind == "TIM":
        t = parse_tim(b)
        rows = len(t["clut"]) if t["clut"] else 1
        for r in range(min(rows, 8)):
            w, h, rgba = tim_to_rgba(b, t, r)
            suf = "" if rows == 1 else ".clut%d" % r
            _save(outdir, "%s%s.png" % (stem, suf), w, h, rgba, made)
        if t["clut"]:
            w, h, rgba = clut_strip_rgba(t["clut"][0])
            _save(outdir, "%s.pal.png" % stem, w, h, rgba, made)

    elif kind == "EACHUNK":
        chunks, _ = chunk_chain(b)
        tslp = None
        for (off, tag, size) in chunks:
            if tag == b"TSLP":
                tslp = parse_tslp(b, off, size)
        n = 0
        for (off, tag, size) in chunks:
            if tag == b"TSLP":
                d = parse_tslp(b, off, size)
                for i in range(d["count"]):
                    cl = clut_from_bytes(b, d["data"] + i * d["entries"] * 2, d["entries"])
                    w, h, rgba = clut_strip_rgba(cl, cell=4)
                    _save(outdir, "%s.tslp%02d.png" % (stem, i), w, h, rgba, made)
            elif tag == b"KNBP":
                d = parse_knbp(b, off, size)
                if d["exact"]:
                    w, h = d["width"], d["height"]
                    _save(outdir, "%s.knbp.png" % stem, w, h,
                          direct16_to_rgba(b, d["data"], w, h), made)
            elif tag == b"LECT":
                d = parse_lect(b, off, size)
                tagname = "%s.lect%02d.id%04x.k%d" % (stem, n, d["id"], d["kind"])
                n += 1
                if d["tim"]:
                    t = d["tim"]
                    w, h, rgba = tim_to_rgba(b, t, 0)
                    _save(outdir, tagname + ".png", w, h, rgba, made)
                    if t["clut"]:
                        cw, ch, crgba = clut_strip_rgba(t["clut"][0])
                        _save(outdir, tagname + ".pal.png", cw, ch, crgba, made)
                    r = pt.get(d["id"])
                    if r and r["clut"]:
                        cl = vram_clut(sav, r["clut"], 256)
                        _save(outdir, tagname + ".runtime-clut%04x.png" % r["clut"],
                              w, h, indexed_to_rgba(b, t["data_off"], w, h, 8, cl), made)
                else:
                    w, h = d["width"], d["height"]
                    _save(outdir, tagname + ".grey.png", w, h,
                          indexed_to_rgba(b, d["payload"], w, h, d["bpp"],
                                          grey_clut(1 << d["bpp"])), made)
                    cl, note = load_clut_source(os.path.dirname(path), d["id"])
                    if cl is not None:
                        _save(outdir, tagname + ".clut.png", w, h,
                              indexed_to_rgba(b, d["payload"], w, h, 4, cl), made)
                        print("  CLUT for id 0x%02x taken from %s" % (d["id"], note))
                    if sav is not None and clut_index is not None:
                        cl = vram_clut(sav, clut_index, 1 << d["bpp"])
                        _save(outdir, tagname + ".vramclut%04x.png" % clut_index,
                              w, h, indexed_to_rgba(b, d["payload"], w, h, d["bpp"], cl), made)
                    r = pt.get(d["id"])
                    if r is not None:
                        print("  note: id 0x%02x is resident at VRAM (%d,%d), tpage 0x%03x, "
                              "page-table CLUT = 0x%04x"
                              % (r["id"], r["x"], r["y"], r["tpage"], r["clut"])
                              + ("  (4bpp: no per-page CLUT, it is per-primitive)"
                                 if not r["clut"] else ""))

    elif kind == "SHPP":
        s = parse_shpp(b)
        for e in s["ents"]:
            if e["exact"] and e["code"] == 0x42:
                _save(outdir, "%s.%s.png" % (stem, e["name"]), e["w"], e["h"],
                      direct16_to_rgba(b, e["data"], e["w"], e["h"]), made)

    elif kind == "GTP":
        # the object art, rendered once per CLUT the page carries
        h_art = GTP_H - GTP_ART_Y
        want = range(GTP_CLUTS) if clut_index is None else [clut_index]
        for i in want:
            cl = gtp_clut(b, i)
            out = bytearray(GTP_W * h_art * 4)
            p = 0
            for y in range(GTP_ART_Y, GTP_H):
                row = b[y * GTP_ROW:(y + 1) * GTP_ROW]
                for x in range(GTP_W):
                    c = cl[(row[x >> 1] & 0x0F) if (x & 1) == 0 else (row[x >> 1] >> 4)]
                    out[p] = c[0]; out[p + 1] = c[1]; out[p + 2] = c[2]; out[p + 3] = c[3]
                    p += 4
            _save(outdir, "%s.objects.clut%02d-id%d.png" % (stem, i, gtp_clut_id(i)),
                  GTP_W, h_art, out, made)
        _save(outdir, "%s.page.grey.png" % stem, GTP_W, GTP_H,
              indexed_to_rgba(b, 0, GTP_W, GTP_H, 4, grey_clut(16)), made)

    elif kind == "FNTP":
        f = parse_fntp(b)
        _save(outdir, "%s.sheet.png" % stem, f["bw"], f["bh"],
              indexed_to_rgba(b, f["pix"], f["bw"], f["bh"], 4, grey_clut(16)), made)

    elif kind == "DASHPAGE":
        _save(outdir, "%s.page4bpp.grey.png" % stem, 256, DASH_CLUT_Y,
              indexed_to_rgba(b, 0, 256, DASH_CLUT_Y, 4, grey_clut(16)), made)
        cs = []
        for n in range(DASH_CLUT_SLOTS):
            cs.extend(dash_clut(b, n))
        w, h, rgba = clut_strip_rgba(cs, cell=4)
        _save(outdir, "%s.cluttable.png" % stem, w, h, rgba, made)
        csvp = dash_csv_for(path)
        if csvp:
            csv = parse_dash_csv(csvp)
            ext = dash_tex_extents(csv)
            W, H = 256, DASH_CLUT_Y
            out = bytearray(W * H * 4)
            for i, t in enumerate(csv["tims"]):
                if i not in ext:
                    continue
                bx, by = int(t[1]), int(t[2])
                tw, th = ext[i]
                cl = dash_clut(b, i)            # CLUT slot index == TIM index
                for y in range(th):
                    for x in range(tw):
                        X, Y = bx + x, by + y
                        if X >= W or Y >= H:
                            continue
                        c = cl[dash_pixel(b, X, Y)]
                        p = (Y * W + X) * 4
                        out[p] = c[0]; out[p + 1] = c[1]; out[p + 2] = c[2]; out[p + 3] = c[3]
            _save(outdir, "%s.composite.png" % stem, W, H, out, made)
            sub = os.path.join(outdir, stem.lower() + "_art")
            for a in csv["arts"]:
                try:
                    x, y, w, h, ti = (int(a[1]), int(a[2]), int(a[3]), int(a[4]), int(a[5]))
                except (ValueError, IndexError):
                    continue
                if w <= 0 or h <= 0 or ti >= len(csv["tims"]):
                    continue
                bx, by = int(csv["tims"][ti][1]), int(csv["tims"][ti][2])
                cl = dash_clut(b, ti)
                o = bytearray(w * h * 4)
                for yy in range(h):
                    for xx in range(w):
                        c = cl[dash_pixel(b, bx + x + xx, by + y + yy)]
                        p = (yy * w + xx) * 4
                        o[p] = c[0]; o[p + 1] = c[1]; o[p + 2] = c[2]; o[p + 3] = c[3]
                _save(sub, "%s.png" % a[0], w, h, o, made)
    else:
        print("  cannot decode %s (%s)" % (path, kind))
        return []

    for p in made:
        print("  wrote %s" % p)
    return made


# ------------------------------------------------------------------ scan

def verdict(path, b):
    """Return (kind, status, note) - status in PASS/PARTIAL/FAIL/SKIP."""
    kind = identify(path, b)
    try:
        if kind == "TIM":
            t = parse_tim(b)
            if t["total"] == len(b):
                return kind, "PASS", "%dbpp %dx%d clut=%s" % (t["bpp"], t["px_w"], t["px_h"], t["has_clut"])
            return kind, "PARTIAL", "%d of %d bytes" % (t["total"], len(b))

        if kind == "EACHUNK":
            chunks, complete = chunk_chain(b)
            if not complete:
                return kind, "FAIL", "chain stops after %d chunks" % len(chunks)
            bad = []
            imgs = 0
            for (off, tag, size) in chunks:
                if tag == b"LECT":
                    d = parse_lect(b, off, size)
                    if d["tim"]:
                        if 0x18 + d["tim"]["total"] != size:
                            bad.append("LECT@0x%x tim size" % off)
                        else:
                            imgs += 1
                    else:
                        need = d["width"] * d["height"] * d["bpp"] // 8
                        if 0x18 + need != size:
                            bad.append("LECT@0x%x raw size" % off)
                        else:
                            imgs += 1
                elif tag == b"CTKP":
                    if not parse_ctkp(b, off, size)["exact"]:
                        bad.append("CTKP@0x%x" % off)
                elif tag == b"TSLP":
                    if not parse_tslp(b, off, size)["exact"]:
                        bad.append("TSLP@0x%x" % off)
                elif tag == b"KNBP":
                    if not parse_knbp(b, off, size)["exact"]:
                        bad.append("KNBP@0x%x" % off)
            tags = "+".join(sorted(set(t.decode() for (_, t, _) in chunks)))
            if not any(t in (b"LECT", b"TSLP", b"KNBP") for (_, t, _) in chunks):
                return kind, "SKIP", "%s; geometry/animation container, no image chunks" % tags
            if bad:
                return kind, "PARTIAL", "%s; bad: %s" % (tags, ",".join(bad))
            has_rmd3 = any(t == b"RMD3" for (_, t, _) in chunks)
            return kind, ("PARTIAL" if has_rmd3 else "PASS"), \
                "%s; %d images%s" % (tags, imgs, "; RMD3 model not decoded" if has_rmd3 else "")

        if kind == "SHPP":
            s = parse_shpp(b)
            ok = sum(1 for e in s["ents"] if e["exact"])
            return kind, ("PASS" if ok == s["count"] and s["size_ok"] else "PARTIAL"), \
                "%d/%d images exact" % (ok, s["count"])

        if kind == "GTP":
            g = parse_gtp(b)
            return kind, "PASS", "4bpp texture page %dx%d + %d own CLUTs" % (
                g["w"], g["h"], g["ncluts"])

        if kind == "FNTP":
            f = parse_fntp(b)
            if f["size_ok"] and f["table_ok"] and f["exact"]:
                return kind, "PASS", "%d glyphs, %dx%d 4bpp sheet" % (f["count"], f["bw"], f["bh"])
            return kind, "PARTIAL", "size_ok=%s table_ok=%s pixels_ok=%s" % (
                f["size_ok"], f["table_ok"], f["exact"])

        if kind == "DASHPAGE":
            csvp = dash_csv_for(path)
            if csvp:
                c = parse_dash_csv(csvp)
                return kind, "PASS", "4bpp page + CLUT table; %d TIMs / %d art rects from CSV" % (
                    len(c["tims"]), len(c["arts"]))
            return kind, "PARTIAL", "4bpp page + CLUT table; no sibling CSV"

        return kind, "SKIP", "not an image container"
    except Exception as exc:                            # noqa: BLE001
        return kind, "FAIL", "%s: %s" % (type(exc).__name__, exc)


def cmd_scan(root, outdir=None):
    rows = []
    for dirpath, _dirs, names in os.walk(root):
        for nm in sorted(names):
            p = os.path.join(dirpath, nm)
            try:
                sz = os.path.getsize(p)
            except OSError:
                continue
            if sz > 8 * 1024 * 1024:                    # .ALB/.STR bulk media
                rows.append((os.path.relpath(p, root), sz, "BIG", "SKIP", "not read (%d bytes)" % sz))
                continue
            with open(p, "rb") as f:
                b = f.read()
            k, st, note = verdict(p, b)
            rows.append((os.path.relpath(p, root), sz, k, st, note))

    lines = []
    lines.append("%-24s %10s %-8s %-8s %s" % ("file", "size", "kind", "status", "note"))
    lines.append("-" * 110)
    for r in sorted(rows, key=lambda r: (r[2], r[0])):
        lines.append("%-24s %10d %-8s %-8s %s" % r)
    counts = {}
    for r in rows:
        counts.setdefault((r[2], r[3]), 0)
        counts[(r[2], r[3])] += 1
    lines.append("")
    lines.append("summary (kind, status, count):")
    for k in sorted(counts):
        lines.append("  %-8s %-8s %d" % (k[0], k[1], counts[k]))
    text = "\n".join(lines)
    print(text)
    if outdir:
        os.makedirs(outdir, exist_ok=True)
        with open(os.path.join(outdir, "scan.txt"), "w", encoding="utf-8") as f:
            f.write(text + "\n")
        print("\nwrote %s" % os.path.join(outdir, "scan.txt"))


def cmd_vram(sav_path, outdir):
    """Dump a savestate's VRAM as a PNG and print the engine page table."""
    sav = open_savestate(sav_path)
    stem = os.path.splitext(os.path.basename(sav_path))[0]
    made = []
    _save(outdir, "%s.vram.png" % stem, VRAM_W, 512,
          direct16_to_rgba(sav, SAV_VRAM_BASE, VRAM_W, 512), made)
    print("texture page table at 0x%08X (%d resident pages):" % (PAGETABLE_ADDR, len(page_table(sav))))
    print("  %-7s %-6s %-4s %-14s %-7s %s" % ("group", "id", "slot", "vram(x,y)", "tpage", "clut"))
    for r in page_table(sav):
        print("  %-7s 0x%02x   %-4d (%4d,%3d)     0x%03x/%-2dbpp 0x%04x%s" % (
            r["group"], r["id"], r["slot"], r["x"], r["y"], r["tpage"], r["depth"], r["clut"],
            "" if r["clut"] else "   <- 4bpp page: CLUT is per-primitive"))
    for p in made:
        print("  wrote %s" % p)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    cmd = argv[1]
    if cmd == "info" and len(argv) >= 3:
        for p in argv[2:]:
            for g in sorted(glob.glob(p)) or [p]:
                cmd_info(g)
        return 0
    if cmd == "png" and len(argv) >= 4:
        gtp = None
        ci = None
        sv = None
        rest = argv[4:]
        i = 0
        while i < len(rest):
            if rest[i] == "--clut":
                gtp = rest[i + 1]; i += 2
            elif rest[i] == "--clut-index":
                ci = int(rest[i + 1], 0); i += 2
            elif rest[i] == "--vram":
                sv = rest[i + 1]; i += 2
            else:
                i += 1
        for g in sorted(glob.glob(argv[2])) or [argv[2]]:
            cmd_png(g, argv[3], gtp, ci, sv)
        return 0
    if cmd == "vram" and len(argv) >= 4:
        cmd_vram(argv[2], argv[3])
        return 0
    if cmd == "scan" and len(argv) >= 3:
        cmd_scan(argv[2], argv[3] if len(argv) > 3 else None)
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
