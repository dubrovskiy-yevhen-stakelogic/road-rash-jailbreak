// The widget draw handlers (shell_widgets.h), PORTED from RASHCDF.BIN - see the header.
#include "game/shell/shell_widgets.h"

#include "game/shell/shell_text.h"

#include <climits>
#include <initializer_list>

namespace rr::shell {

namespace {

using DrawNote = ShellCallees::DrawNote;

uint32_t U(int32_t v) { return static_cast<uint32_t>(v); }

// PTR_DAT_8009cfc8 + (s8)fe+0x11 * 4 + extra: the ordering-table entry of the layer being drawn.
uint32_t OtAt(GuestRam& g, uint32_t extra) { return g.U32(kOtTable) + 4u * U(g.S8(kFeLayer)) + extra; }

bool CallK(ShellCallees& k, uint32_t address, std::initializer_list<uint32_t> args, uint32_t* v0 = nullptr) {
    uint32_t a[12] = {};
    int n = 0;
    for (uint32_t v : args)
        if (n < 12) a[n++] = v;
    return k.Call(address, a, n, v0);
}

// The packet cursor *(0x8005B470)+0x10C: `bytes` more, through the wrap SLUS 0x80021C98 when the buffer's
// end 0x8005B4D0 would be reached (unsigned compare, as every allocation site has it).
bool Alloc(GuestRam& g, ShellCallees& k, uint32_t bytes, uint32_t* at) {
    const uint32_t env = g.U32(0x8005B470u);
    if (g.U32(kPacketLimit) <= g.U32(env + 0x10Cu) + bytes) {
        uint32_t r = 0;
        if (!CallK(k, kPacketWrap, {g.U32(env + 0x10Cu), bytes}, &r)) return false;
        g.W32(env + 0x10Cu, r);
    }
    *at = g.U32(env + 0x10Cu);
    g.W32(env + 0x10Cu, *at + bytes);
    return true;
}

// MIPS div's quotient (no trap: the R3000 leaves -1 / 1 for a zero divisor).
int32_t Div(int32_t a, int32_t b) {
    if (b == 0) return a >= 0 ? -1 : 1;
    if (a == INT_MIN && b == -1) return INT_MIN;
    return a / b;
}

uint32_t Xy(int32_t x, int32_t y) { return U(x) | (U(y) << 16); }

// The outline both bar widgets draw: GP0 0x4C (an opaque poly-line, colour 0x010101) round x..x+w,
// y..y+h, closed, 0x55555555-terminated - 8 words at `p`, linked into `ot`.
void LineBox(GuestRam& g, ShellCallees& k, uint32_t p, uint32_t ot, int32_t x, int32_t y, int32_t w, int32_t h) {
    g.W32(p, g.U32(ot) | 0x07000000u);
    g.W32(p + 4u, 0x4C010101u);
    g.W32(p + 8u, Xy(x, y));
    g.W32(p + 12u, U(x + w) | (U(y) << 16));
    g.W32(p + 16u, U(x + w) | (U(y + h) * 0x10000u));
    g.W32(p + 20u, U(x) | (U(y + h) * 0x10000u));
    g.W32(p + 28u, 0x55555555u);
    g.W32(p + 24u, Xy(x, y));
    g.W32(ot, p);
    DrawNote n;
    n.kind = DrawNote::Box;
    n.x = x, n.y = y, n.w = w, n.h = h;
    n.rgb = 0x010101u;
    n.ot = ot;
    k.Drew(n);
}

void NoteTile(ShellCallees& k, uint32_t ot, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb) {
    DrawNote n;
    n.kind = DrawNote::Tile;
    n.x = x, n.y = y, n.w = w, n.h = h;
    n.rgb = rgb & 0xFFFFFFu;
    n.semi = true; // GP0 0x62
    n.ot = ot;
    k.Drew(n);
}

// RASHCDF 0x8006EE20: a flat quad (GP0 0x28, 0x2A when the record's +0x02 bit 0) x0..x1, y0..y1.
bool Quad(GuestRam& g, ShellCallees& k, uint32_t flags, uint32_t colour, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
          uint32_t ot) {
    uint32_t p = 0;
    if (!Alloc(g, k, 0x18u, &p)) return false;
    g.W32(p, g.U32(ot) | 0x05000000u);
    g.W32(p + 4u, ((((flags & 1u) << 1) | 0x28u) << 24) | colour);
    g.W32(p + 8u, Xy(x0, y0));
    g.W32(p + 12u, Xy(x1, y0));
    g.W32(p + 16u, Xy(x0, y1));
    g.W32(p + 20u, Xy(x1, y1));
    g.W32(ot, p);
    DrawNote n;
    n.kind = DrawNote::Tile;
    n.x = x0, n.y = y0, n.w = x1 - x0, n.h = y1 - y0; // the GPU leaves the right and bottom edges out
    n.rgb = colour & 0xFFFFFFu;
    n.semi = (flags & 1u) != 0;
    n.ot = ot;
    k.Drew(n);
    return true;
}

} // namespace

// ------------------------------------------------------------------------ RASHCDF 0x8006ECDC
bool PanelEmit(GuestRam& g, ShellCallees& k, uint32_t /*s*/, uint32_t r, uint32_t ot, uint32_t* v0) {
    *v0 = 1;
    const uint32_t f = g.U16(r + 2u), c = g.U32(r + 4u);
    const int32_t ox0 = g.S16(r + 8u), oy0 = g.S16(r + 10u), ox1 = g.S16(r + 12u), oy1 = g.S16(r + 14u);
    const int32_t ix0 = g.S16(r + 0x18u), iy0 = g.S16(r + 0x1Au), ix1 = g.S16(r + 0x1Cu), iy1 = g.S16(r + 0x1Eu);
    if (!Quad(g, k, f, c, ox0, oy0, ox1, iy0, ot) || !Quad(g, k, f, c, ox0, iy1, ox1, oy1, ot) ||
        !Quad(g, k, f, c, ox0, iy0, ix0, iy1, ot) || !Quad(g, k, f, c, ix1, iy0, ox1, iy1, ot))
        return false;
    if (!(g.U16(r + 0x20u) & 1u) && !Quad(g, k, g.U16(r + 0x12u), g.U32(r + 0x14u), ix0, iy0, ix1, iy1, ot)) return false;
    return true;
}

// ------------------------------------------------------------------------ RASHCDF 0x8006ECA0
bool PanelWidget(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t* v0) {
    return PanelEmit(g, k, s, w + 0x10u, OtAt(g, 0x14u), v0);
}

// ------------------------------------------------------------------------ RASHCDF 0x8006F9A4
bool HeaderText(GuestRam& g, ShellCallees& k, uint32_t /*s*/, uint32_t w, uint32_t* v0) {
    *v0 = 1;
    const uint32_t font = g.U32(kFontMini);
    if (g.S8(kFontSlots + 0x18u * font) == 0) return true;
    g.W8(kFontSlots + 0x18u * font + 3u, 0);
    return CallK(k, kTextAt, {g.U32(kFontMini), g.U16(w + 0x10u), U(g.S16(w + 0x14u)), U(g.S16(w + 0x16u)), OtAt(g, 4u),
                              g.U32(w + 0x18u)});
}

// ------------------------------------------------------------------------ RASHCDF 0x8006FA38
bool TextLine(GuestRam& g, ShellCallees& k, uint32_t /*s*/, uint32_t w, uint32_t* v0) {
    *v0 = 1;
    const uint32_t font = g.U32(kFontMini);
    if (g.S8(kFontSlots + 0x18u * font) == 0) return true;
    g.W8(kFontSlots + 0x18u * font + 3u, 0);
    return CallK(k, kTextId, {g.U32(kFontMini), g.U16(w + 0x10u), w + 0x14u, OtAt(g, 4u), g.U32(w + 0x1Cu), g.U16(w + 0x12u)});
}

// ------------------------------------------------------------------------ RASHCDF 0x800705DC
bool AnimSprite(GuestRam& g, ShellCallees& k, uint32_t r, uint32_t ot, int32_t mode, uint32_t* v0) {
    *v0 = 1;
    const uint32_t id = g.U32(r);
    uint32_t i = 0;
    for (; i < 300u; ++i)
        if (g.U32(kSpriteIds + 4u * i) == id) break;
    if (i == 300u) return true;
    const uint32_t sr = kSpriteRecs + 0x24u * i;
    uint32_t f = g.U32(sr);
    if (!(f & 0x10000000u)) return true;
    if (f & 0x100000u) { // a shared VRAM slot: another record's pixels may be there
        if (g.U32(0x800A0810u) != id) {
            g.W32(0x800A0810u, id);
            g.W32(sr, g.U32(sr) & 0xDFFFFFFFu);
        }
        f = g.U32(sr);
    }
    if (!(f & 0x40000000u)) {
        if (!(f & 0x20000000u)) {
            if (!CallK(k, kSpriteUpload, {sr})) return false;
            g.W32(sr, g.U32(sr) | 0x20000000u);
        }
    } else if (static_cast<int32_t>(f) >= 0) { // queued for the second list's copy pass; nothing emitted
        const int32_t n = g.S32(0x8009C2F4u);
        if (5 < n) return true;
        const uint32_t q = 0x24u * U(n);
        g.W32(0x8009C3D8u + q, 3);
        g.W32(0x8009C3DCu + q, 1);
        g.W32(0x8009C3E0u + q, 0);
        g.W32(0x8009C3D0u + q, g.U32(sr + 4u));
        g.W32(0x8009C3ECu + q, U(g.S16(r + 8u)));
        g.W32(0x8009C2F4u, U(n + 1));
        g.W32(0x8009C3F0u + q, U(g.S16(r + 10u)));
        return true;
    } else if (!(f & 0x20000000u)) { // queued for the first list's copy, and its page / u / v computed
        const int32_t n = g.S32(0x8009C2F0u);
        if (n < 6) {
            const uint32_t q = 0x24u * U(n);
            g.W32(0x8009C300u + q, 4);
            g.W32(0x8009C304u + q, 1);
            g.W32(0x8009C308u + q, 0);
            g.W32(0x8009C2F8u + q, g.U32(sr + 4u));
            g.W32(0x8009C314u + q, U(g.S16(sr + 0x10u)));
            g.W32(0x8009C2F0u, U(n + 1));
            g.W32(0x8009C318u + q, U(g.S16(sr + 0x12u)));
        }
        const int16_t vx = g.S16(sr + 0x10u);
        const uint16_t vy = g.U16(sr + 0x12u);
        g.W32(sr, g.U32(sr) | 0x20000002u);
        g.W16(sr + 0x1Cu, static_cast<uint16_t>(((vy & 0x100u) >> 4) | ((g.U16(sr + 0x10u) & 0x3FFu) >> 6) | 0x100u |
                                                ((vy & 0x200u) << 2)));
        const int32_t bx = vx < 0 ? vx + 0x3F : vx;
        g.W16(sr + 0x18u, static_cast<uint16_t>(vx + static_cast<int16_t>(bx >> 6) * -0x40));
        const int32_t y = g.S16(sr + 0x12u);
        const int32_t by = y < 0 ? y + 0xFF : y;
        g.W16(sr + 0x1Au, static_cast<uint16_t>(y + static_cast<int16_t>(U(by) >> 8) * -0x100));
        g.W16(sr + 0x1Eu, 0);
    }
    auto rewind = [&] {
        g.W8(r + 0x10u, 0);
        g.W8(r + 0x11u, 0);
        g.W8(r + 0x12u, 0);
        g.W8(r + 0x13u, 0);
    };
    if (mode == 1) {
        rewind();
        g.W8(r + 0x14u, static_cast<uint8_t>(g.U8(r + 0x14u) & 0x5Fu));
    } else if (mode == 2) {
        g.W8(r + 0x14u, static_cast<uint8_t>(g.U8(r + 0x14u) | 0x20u));
    } else if (mode == 3) {
        rewind();
        g.W8(r + 0x14u, static_cast<uint8_t>((g.U8(r + 0x14u) & 0x5Fu) | 0x20u));
    }
    if (!(g.U8(r + 0x14u) & 0x20u) && g.U8(r + 0x0Fu) < g.U8(r + 0x11u)) { // the frame's delay is over
        g.W8(r + 0x11u, 0);
        const uint8_t frame = static_cast<uint8_t>(g.U8(r + 0x10u) + 1u);
        g.W8(r + 0x10u, frame);
        if (frame < g.U8(r + 0x0Cu)) {
            const uint8_t u = static_cast<uint8_t>(g.U8(r + 0x12u) + g.U8(r + 0x0Du));
            g.W8(r + 0x12u, u);
            if (g.S16(sr + 0x14u) <= static_cast<int16_t>(u)) { // the sheet's next row
                g.W8(r + 0x12u, 0);
                g.W8(r + 0x13u, static_cast<uint8_t>(g.U8(r + 0x13u) + g.U8(r + 0x0Eu)));
            }
        } else {
            g.W8(r + 0x14u, static_cast<uint8_t>(g.U8(r + 0x14u) | 0xA0u));
        }
    }
    const uint8_t b = g.U8(r + 0x14u);
    if (b & 0x80u) { // the run ended
        if (!(b & 0x10u)) {
            if (!(b & 0x40u)) return true; // hidden
            rewind();
            g.W8(r + 0x14u, static_cast<uint8_t>((g.U8(r + 0x14u) & 0x7Fu) | 0x20u));
        } else {
            rewind();
            g.W8(r + 0x14u, static_cast<uint8_t>(b & 0x5Fu));
        }
    }
    if (!(g.U8(r + 0x14u) & 0x20u)) g.W8(r + 0x11u, static_cast<uint8_t>(g.U8(r + 0x11u) + 1u));
    uint32_t p = 0;
    if (!Alloc(g, k, 0x18u, &p)) return false;
    g.W32(p, g.U32(ot) | 0x05000000u);
    g.W32(p + 4u, (g.U16(sr + 0x1Cu) & 0x9FFu) | 0xE1000200u);
    g.W16(p + 0x12u, g.U16(sr + 0x1Eu));
    g.W32(p + 8u, g.U32(r + 4u) | 0x64000000u);
    g.W16(p + 0x0Cu, g.U16(r + 8u));
    g.W16(p + 0x0Eu, g.U16(r + 10u));
    g.W8(p + 0x10u, static_cast<uint8_t>(g.U8(sr + 0x18u) + g.U8(r + 0x12u)));
    g.W8(p + 0x11u, static_cast<uint8_t>(g.U8(sr + 0x1Au) + g.U8(r + 0x13u)));
    g.W16(p + 0x14u, g.U8(r + 0x0Du));
    g.W16(p + 0x16u, g.U8(r + 0x0Eu));
    g.W32(ot, p);
    DrawNote n;
    n.kind = DrawNote::Sprite;
    n.fourcc = id;
    n.srcX = g.U8(r + 0x12u), n.srcY = g.U8(r + 0x13u);
    n.x = g.S16(r + 8u), n.y = g.S16(r + 10u), n.w = g.U8(r + 0x0Du), n.h = g.U8(r + 0x0Eu);
    n.rgb = g.U32(r + 4u) & 0xFFFFFFu;
    n.ot = ot;
    k.Drew(n);
    return true;
}

// ------------------------------------------------------------------------ RASHCDF 0x8006E894
bool SpriteWidget(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t* v0) {
    *v0 = 1;
    if ((g.U16(w + 10u) & 0x20u) && !(g.U16(s) & 2u)) return true;
    return CallK(k, kSpriteEmit, {s, w + 0x10u, OtAt(g, 0x1Cu)}, v0);
}

// ------------------------------------------------------------------------ RASHCDF 0x8006E7A4
bool PadHint(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t* v0) {
    *v0 = 1;
    if ((g.U16(w + 10u) & 0x20u) && !(g.U16(s) & 2u)) return true;
    const uint32_t font = g.U32(kFontMini);
    if (g.S8(kFontSlots + 0x18u * font) != 0 &&
        !CallK(k, kTextId, {font, g.U16(w + 0x1Cu), w + 0x20u, OtAt(g, 4u), g.U32(w + 0x28u), g.U16(w + 0x1Eu)}))
        return false;
    return CallK(k, kSpriteEmit, {s, w + 0x10u, OtAt(g, 0x14u)}, v0);
}

// ------------------------------------------------------------------------ RASHCDF 0x8006F764
bool ButtonDraw(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t* v0) {
    *v0 = 1;
    if ((g.U16(w + 10u) & 0x20u) && !(g.U16(s) & 2u)) return true;
    uint32_t colour, sprite = 0;
    if (g.U32(s + 0x10u) + U(g.S16(s + 4u) * 0x78) == w) { // the highlighted item: its 'H' shape
        colour = g.U32(w + 0x1Cu);
        if (g.U32(w + 0x28u) != 0) sprite = 0x484E5442u; // BTNH
    } else {
        const uint16_t type = g.U16(w + 8u);
        const uint32_t c = g.U32(w);
        const uint32_t mode = g.U32(kSession);
        const bool live = (static_cast<uint16_t>(type - 12u) < 2u || static_cast<int16_t>(type) == 17) &&
                          (!(c & 0x40000000u) || (g.U8(kMultitapByte) >> 4) == 8u) &&
                          (!(c & 0x02000000u) || g.U32(kPorts + 24u * U(g.S8(kFePort)) + 4u) == 1u) &&
                          (!(c & 0x10000000u) || g.S8(kSession + 0x1Bu) != 0) &&
                          (!(c & 0x08000000u) || mode == 4u) && (!(c & 0x04000000u) || mode != 4u);
        colour = g.U32(w + (live ? 0x18u : 0x20u)); // idle, or the disabled triple
        if (g.U32(w + 0x28u) != 0) sprite = 0x4E4E5442u; // BTNN
    }
    g.W32(w + 0x28u, sprite);
    const uint32_t font = g.U32(kFontMain);
    if (g.S8(kFontSlots + 0x18u * font) != 0) {
        g.W8(kFontSlots + 0x18u * font + 3u, 0);
        if (!CallK(k, kTextAt, {g.U32(kFontMain), g.U32(w + 0x14u), U(g.S16(w + 0x24u)), U(g.S16(w + 0x26u)), OtAt(g, 8u), colour}))
            return false;
    }
    return CallK(k, kSpriteEmit, {s, w + 0x28u, OtAt(g, 0x10u)}, v0);
}

// ------------------------------------------------------------------------ RASHCDF 0x8006EF30
bool ChooserDraw(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t* v0) {
    *v0 = 1;
    if ((g.U16(w + 10u) & 0x20u) && !(g.U16(s) & 2u)) return true;
    uint32_t r = 0;
    if (!ButtonDraw(g, k, s, w, &r)) return false;
    *v0 = r;
    if (r == 0) return true;
    const uint32_t ot = OtAt(g, 0x0Cu);
    // The arrows: only on the highlighted chooser, and only when its bound chooser has two or more
    // options to walk (fe+0x17).
    if (g.U32(s + 0x10u) + U(g.S16(s + 4u) * 0x78) != w) return true;
    if (g.S8(kFeValid) < 2) return true;
    const uint32_t edges = g.U8(kFeEdges);
    uint32_t a = 0, b = 0;
    if (edges == 0x40u) { // Left this frame: the left arrow's run restarts
        if (!AnimSprite(g, k, w + 0x34u, ot, 1, &a) || !AnimSprite(g, k, w + 0x4Cu, ot, 0, &b)) return false;
    } else if (edges == 0x80u) { // Right: the right one's
        if (!AnimSprite(g, k, w + 0x34u, ot, 0, &a)) return false;
        if (a == 0) {
            *v0 = 0;
            return true;
        }
        if (!AnimSprite(g, k, w + 0x4Cu, ot, 1, &b)) return false;
        *v0 = b;
        return true;
    } else if (g.U32(kFeLastItem) != w) { // just highlighted: both rewound and held
        if (!AnimSprite(g, k, w + 0x34u, ot, 3, &a) || !AnimSprite(g, k, w + 0x4Cu, ot, 3, &b)) return false;
    } else {
        const uint32_t last = g.U32(kFeLastItem);
        if (!AnimSprite(g, k, last + 0x34u, ot, 0, &a) || !AnimSprite(g, k, last + 0x4Cu, ot, 0, &b)) return false;
    }
    *v0 = a & b;
    return true;
}

// ------------------------------------------------------------------------ RASHCDF 0x8006F4A0
bool SliderDraw(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t* v0) {
    uint32_t v = 1;
    if ((g.U8(w + 0x67u) & 1u) && !ChooserDraw(g, k, s, w, &v)) return false;
    const uint32_t ot = OtAt(g, 0x14u);
    // 0x80064BB0: the slider's descriptor 0x8009C548 + 16 * slot, while its index is < 6.
    const uint32_t desc = g.U16(w + 0x64u) < 6u ? 0x8009C548u + 16u * g.U8(w + 0x66u) : 0u;
    int16_t fill = 0;
    if (desc != 0) {
        const int32_t num = static_cast<int32_t>(((g.U16(desc + 12u) - g.U16(desc)) & 0xFFFFu) * U(g.S16(w + 0x6Cu)));
        fill = static_cast<int16_t>(Div(num, g.S32(desc + 4u) - g.S32(desc)));
    }
    const int32_t x = g.S16(w + 0x68u), y = g.S16(w + 0x6Au), bw = g.S16(w + 0x6Cu), bh = g.S16(w + 0x6Eu);
    uint32_t p = 0;
    if (!Alloc(g, k, 0x20u, &p)) return false;
    LineBox(g, k, p, ot, x, y, bw, bh);
    if (!Alloc(g, k, 0x20u, &p)) return false;
    g.W32(p, g.U32(ot) | 0x03000000u);
    g.W32(p + 4u, g.U32(w + 0x74u) | 0x62000000u);
    g.W16(p + 8u, g.U16(w + 0x68u));
    g.W16(p + 12u, static_cast<uint16_t>(fill));
    g.W16(p + 10u, g.U16(w + 0x6Au));
    g.W16(p + 14u, g.U16(w + 0x6Eu));
    g.W32(ot, p);
    NoteTile(k, ot, x, y, fill, bh, g.U32(w + 0x74u));
    g.W32(p + 16u, p | 0x03000000u);
    g.W32(p + 20u, g.U32(w + 0x70u) | 0x62000000u);
    g.W16(p + 24u, static_cast<uint16_t>(x + fill));
    g.W16(p + 26u, g.U16(w + 0x6Au));
    g.W16(p + 28u, static_cast<uint16_t>(bw - fill));
    g.W16(p + 30u, g.U16(w + 0x6Eu));
    g.W32(ot, p + 16u);
    NoteTile(k, ot, static_cast<int16_t>(x + fill), y, static_cast<int16_t>(bw - fill), bh, g.U32(w + 0x70u));
    *v0 = v;
    return true;
}

// ------------------------------------------------------------------------ RASHCDF 0x8006F0B8
bool BikeStatsDraw(GuestRam& g, ShellCallees& k, uint32_t s, uint32_t w, uint32_t* v0) {
    *v0 = 1;
    // The page shows the stats of the bike the highlighted chooser edits: player 1's (action group 4)
    // or player 2's (5).
    const int32_t group = ActionGroup(g, g.U32(s + 0x10u) + U(g.S16(s + 4u) * 0x78));
    int32_t bike;
    if (group == 4) bike = g.S8(kPlayers + 7u);
    else if (group == 5) bike = g.S8(kPlayers + 0x24u + 7u);
    else return true;
    const uint32_t font = g.U32(kFontMain);
    if (g.S8(kFontSlots + 0x18u * font) == 0) return true;
    g.W8(kFontSlots + 0x18u * font + 3u, 0);
    int32_t y = g.S16(w + 0x10u);
    const uint32_t ot = OtAt(g, 0x14u), textOt = OtAt(g, 4u);
    const uint32_t env = g.U32(0x8005B470u);
    uint32_t tiles = 0, boxes = 0;
    if (!Alloc(g, k, 0x60u, &tiles)) return false;
    if (g.U32(kPacketLimit) <= tiles + 0xC0u) {
        uint32_t r = 0;
        if (!CallK(k, kPacketWrap, {tiles + 0x60u, 0x60u}, &r)) return false;
        g.W32(env + 0x10Cu, r);
    }
    boxes = g.U32(env + 0x10Cu);
    g.W32(env + 0x10Cu, boxes + 0x60u);
    const int32_t x = g.S16(w + 0x14u);
    for (uint32_t row = 0; row < 3u; ++row) {
        const uint32_t t = kStatTable + 12u * row;
        if (!CallK(k, kTextAt, {g.U32(kFontMain), U(g.S16(t + 8u)), U(g.S16(w + 0x12u)), U(y), textOt, g.U32(w + 0x18u)}))
            return false;
        LineBox(g, k, boxes, ot, x, y, 0x78, 8);
        // The bar walks one unit a frame to the bike's value, from inside [min, max].
        const uint32_t shown = kFeStatShown + 4u * row;
        const int32_t target = g.S32(kBikeStats + 4u * row + U(bike * 12));
        int32_t cur = g.S32(shown);
        if (cur < target) {
            if (cur < g.S32(t)) g.W32(shown, U(g.S32(t)));
            g.W32(shown, U(g.S32(shown) + 1));
        } else if (target < cur) {
            if (g.S32(t + 4u) < cur) g.W32(shown, U(g.S32(t + 4u)));
            g.W32(shown, U(g.S32(shown) - 1));
        }
        const int32_t num = static_cast<int32_t>(((g.U16(shown) - g.U16(t)) & 0xFFFFu) * 0x78u);
        const int16_t fill = static_cast<int16_t>(Div(num, g.S32(t + 4u) - g.S32(t)));
        g.W32(tiles, g.U32(ot) | 0x03000000u);
        g.W32(tiles + 4u, g.U32(w + 0x20u) | 0x62000000u);
        g.W16(tiles + 14u, 8);
        g.W16(tiles + 10u, static_cast<uint16_t>(y));
        g.W16(tiles + 8u, g.U16(w + 0x14u));
        g.W16(tiles + 12u, static_cast<uint16_t>(fill));
        g.W32(ot, tiles);
        NoteTile(k, ot, x, y, fill, 8, g.U32(w + 0x20u));
        g.W32(tiles + 16u, tiles | 0x03000000u);
        g.W32(tiles + 20u, g.U32(w + 0x1Cu) | 0x62000000u);
        g.W16(tiles + 26u, static_cast<uint16_t>(y));
        g.W16(tiles + 28u, static_cast<uint16_t>(0x78 - fill));
        g.W16(tiles + 30u, 8);
        g.W16(tiles + 24u, static_cast<uint16_t>(x + fill));
        g.W32(ot, tiles + 16u);
        NoteTile(k, ot, static_cast<int16_t>(x + fill), y, static_cast<int16_t>(0x78 - fill), 8, g.U32(w + 0x1Cu));
        tiles += 0x20u;
        boxes += 0x20u;
        y = static_cast<int16_t>(y + g.S16(w + 0x16u));
    }
    return true;
}

} // namespace rr::shell
