#include "game/sim/hud.h"
#include "game/sim/modes.h" // the modes' HUD elements (clock, splits, suspect)

#include "game/sim/fixed.h"
#include "game/sim/road_runtime.h"
#include "game/sim/vec.h"

namespace rr::sim {
namespace {

constexpr uint32_t kMask24 = 0x00FFFFFFu;

uint32_t Item(uint32_t items, int32_t index) {
    return items + static_cast<uint32_t>(index) * kHudItemBytes;
}
uint32_t Art(int32_t index) { return kHudArtTable + 16u * static_cast<uint32_t>(index); }
uint32_t GameState(GuestRam& g) { return g.U32(kHudGameStatePtr); }
uint32_t PerPlayer(uint32_t base, int32_t p) { return base + 4u * static_cast<uint32_t>(p); }

// The packet-length word of item kind `kind` (s8 at item+0x20), shifted into a tag.
uint32_t LengthTag(GuestRam& g, uint32_t item) {
    const int32_t kind = g.S8(item + 0x20);
    return g.U32(kHudPacketLength + 4u * static_cast<uint32_t>(kind)) << 24;
}

// "tag of `last` = its length | *ot; *ot = first", the link every element inlines.
void Link(GuestRam& g, uint32_t ot, uint32_t first, uint32_t last) {
    g.W32(last, LengthTag(g, last) | g.U32(ot));
    g.W32(ot, first & kMask24);
}

// 32-bit signed high product, the `mult` + `mfhi` pair.
int32_t MulHi(int32_t a, int32_t b) {
    return static_cast<int32_t>((static_cast<int64_t>(a) * static_cast<int64_t>(b)) >> 32);
}
// The compiler's divide-by-10 sequence: mulhi(v, 0x66666667) >> 2, minus the sign.
int32_t Div10(int32_t v) { return (MulHi(v, 0x66666667) >> 2) - (v >> 31); }
// ... by 100: mulhi(v, 0x51EB851F) >> 5, minus the sign.
int32_t Div100(int32_t v) { return (MulHi(v, 0x51EB851F) >> 5) - (v >> 31); }

// The flash test every bar and the place inline: the flash timers do not run in states 3 and 4.
bool FlashRuns(GuestRam& g) { return static_cast<uint32_t>(g.U8(GameState(g)) - 3u) >= 2u; }

// The health / damage bar every panel inlines (0x80060EE0 .. 0x80060FF4 and its five copies): a
// flat TILE whose width is value / 4 (at least 4) and whose colour is red (< 33, flashing), yellow
// (< 65), green (< 97) or green again at full.
void Bar(GuestRam& g, uint32_t item, int32_t value, uint32_t dash) {
    int32_t shade;
    if (value < 33) {
        if (FlashRuns(g)) HudFlashTimer(g, dash + 192u);
        shade = 1 - g.S32(dash + 208u);
    } else if (value < 65) {
        shade = 1;
    } else if (value < 97) {
        shade = 2;
    } else {
        shade = 3;
    }
    const int32_t hidden = (shade == 0) ? -1 : 0;
    int32_t width;
    if ((value >> 2) < 4)
        width = (~hidden) & 4;
    else
        width = (value >> 2) + ((-(value >> 2)) & hidden);
    g.W16(item + 28u, static_cast<uint16_t>(width));
    const int32_t index = (shade > 0) ? shade - 1 : shade;
    const uint32_t colour = kHudBarColours + 4u * static_cast<uint32_t>(index);
    const uint32_t semi = static_cast<uint32_t>(static_cast<int32_t>(g.S16(item + 34u))) << 25;
    g.W32(item + 4u, semi | 0x60000000u | (static_cast<uint32_t>(g.U8(colour + 2u)) << 16) |
                         (static_cast<uint32_t>(g.U8(colour + 1u)) << 8) | g.U8(colour));
    g.W32(item + 8u, g.U16(item + 24u) | (static_cast<uint32_t>(g.U16(item + 26u)) << 16));
    g.W32(item + 12u, g.U16(item + 28u) | (static_cast<uint32_t>(g.U16(item + 30u)) << 16));
}

// (rd[num] << 7) / (rd[den] + 1), clamped to 2..127 when rd[num] is not 0 (0x80060D04..0x80060D68).
int32_t Health(GuestRam& g, uint32_t rd, uint32_t num, uint32_t den) {
    const int32_t n = g.U8(rd + num);
    int32_t h = (n << 7) / (static_cast<int32_t>(g.U8(rd + den)) + 1);
    if (n != 0) {
        const int32_t lo = h + (((h - 2) >> 31) & (2 - h));
        h = lo + (((127 - h) >> 31) & (127 - h));
    }
    return h;
}

// The bike's condition (0x80060D94..0x80060DEC): the damage when the rider def carries one, else 2
// while the rider is still on (state < 3) and 0 once he is off.
int32_t Condition(GuestRam& g, uint32_t bike) {
    const uint32_t rd = g.U32(bike + 1084u);
    const int32_t n = g.U8(rd + 37u);
    int32_t h = (n << 7) / (static_cast<int32_t>(g.U8(rd + 36u)) + 1);
    if (n != 0) {
        const int32_t lo = h + (((h - 2) >> 31) & (2 - h));
        return lo + (((127 - h) >> 31) & (127 - h));
    }
    return (g.U32(g.U32(bike + 852u) + 604u) < 3u) ? 2 : 0;
}

// The heap allocation every packet writer inlines: `bytes` from *(0x8005B470)+0x10C, through the heap
// manager when the next one would pass the end.
bool HeapTake(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t bytes, uint32_t* at) {
    uint32_t heap = g.U32(kHudHeapPtr);
    const uint32_t next = g.U32(heap + 268u);
    if (!(next + bytes < g.U32(kHudHeapEnd))) {
        uint32_t answer = 0;
        if (!c.HeapOverflow(sp, next, bytes, &answer)) return false;
        heap = g.U32(kHudHeapPtr);
        g.W32(heap + 268u, answer);
    }
    heap = g.U32(kHudHeapPtr);
    *at = g.U32(heap + 268u);
    g.W32(heap + 268u, *at + bytes);
    return true;
}

// BIOS A(1Bh) strlen. A string running past RAM faults the view.
uint32_t StrLen(GuestRam& g, uint32_t s) {
    uint32_t n = 0;
    while (g.U8(s + n) != 0) {
        if (g.Faulted() || n > 0x10000u) break;
        ++n;
    }
    return n;
}

} // namespace

// ============================================================================ the leaves

void HudFlashTimer(GuestRam& g, uint32_t t) {
    const uint32_t period = (g.U32(t + 16u) != 0) ? g.U32(t + 4u) : g.U32(t + 8u);
    uint32_t gs = GameState(g);
    if (static_cast<uint32_t>(g.U8(gs) - 3u) < 2u) return;
    const uint32_t start = g.U32(t + 20u);
    const int32_t since = (start != 0) ? g.S32(gs + 16u) - static_cast<int32_t>(start) : 0;
    int32_t inWindow = 1; // a2
    int32_t past = 0;     // t0
    if (since != 0) {
        const int32_t delay = g.S32(t + 24u);
        inWindow = (delay < since) ? 0 : 1;
        int32_t within = 0;
        if (!inWindow) within = (delay + g.S32(t + 28u) < since) ? 0 : 1;
        past = 0;
        if (!inWindow) past = (within == 0) ? 1 : 0;
    }
    if (inWindow) {
        gs = GameState(g);
        if (static_cast<int32_t>(g.U32(t) + period) < g.S32(gs + 16u)) {
            g.W32(t + 16u, g.U32(t + 16u) ^ 1u);
            const int32_t toggles = g.S32(t + 12u) + 1;
            g.W32(t + 12u, static_cast<uint32_t>(toggles));
            g.W32(t, g.U32(gs + 16u));
            if (toggles < 17) return;
            g.W32(t + 12u, static_cast<uint32_t>(-60));
            return;
        }
    }
    if (g.U32(t + 20u) == 0) return;
    if (inWindow) return;
    const uint32_t phase = (past == 0) ? 1u : 0u;
    g.W32(t + 16u, phase);
    if (phase != 0) return;
    g.W32(t + 20u, 0);
}

void HudSetArtFull(GuestRam& g, uint32_t item, uint32_t art) {
    const int32_t kind = g.S8(item + 32u);
    if (kind == 0) {
        g.W32(item + 20u, art);
        g.W16(item + 28u, g.U8(art + 2u));
        g.W16(item + 30u, g.U8(art + 3u));
        const uint32_t wh = g.U8(art + 2u) | (static_cast<uint32_t>(g.U8(art + 3u)) << 16);
        g.W32(item + 8u, g.U32(item + 24u));
        if (g.S8(item + 32u) == 0)
            g.W32(item + 16u, wh);
        else
            g.W32(item + 12u, wh);
        g.W32(item + 28u, wh);
        return;
    }
    if (kind == 1) {
        g.W16(item + 28u, g.U8(art + 2u));
        g.W16(item + 30u, g.U8(art + 3u));
    }
}

void HudSetArt(GuestRam& g, uint32_t item, uint32_t art) {
    if (g.S8(item + 32u) != 0) return;
    g.W32(item + 12u, g.U32(art + 4u));
    const uint32_t wh = g.U8(art + 2u) | (static_cast<uint32_t>(g.U8(art + 3u)) << 16);
    g.W32(item + 8u, g.U32(item + 24u));
    if (g.S8(item + 32u) == 0)
        g.W32(item + 16u, wh);
    else
        g.W32(item + 12u, wh);
    g.W32(item + 28u, wh);
}

void HudLinkRange(GuestRam& g, uint32_t ot, uint32_t items, int32_t first, int32_t last) {
    Link(g, ot, Item(items, first), Item(items, last));
}

void HudSetDrawMode(GuestRam& g, uint32_t p, uint32_t dfe, uint32_t dtd, uint32_t tpage) {
    g.W8(p + 3u, 1);
    uint32_t w = (dtd != 0) ? 0xE1000200u : 0xE1000000u;
    uint32_t t = tpage & 0x9FFu;
    if (dfe != 0) t |= 0x400u;
    g.W32(p + 4u, w | t);
}

void HudSlideItems(GuestRam& g, uint32_t items, int16_t dy, int32_t first, int32_t last) {
    if (last < first) return;
    uint32_t item = Item(items, first);
    for (int32_t i = first; i <= last; ++i, item += kHudItemBytes) {
        g.W16(item + 26u, static_cast<uint16_t>(g.U16(item + 26u) + static_cast<uint16_t>(dy)));
        const int32_t kind = g.S8(item + 32u);
        if (kind == 0) {
            const uint32_t art = g.U32(item + 20u);
            const uint32_t xy = g.U32(item + 24u);
            const uint32_t wh = g.U8(art + 2u) | (static_cast<uint32_t>(g.U8(art + 3u)) << 16);
            g.W32(item + 8u, xy);
            if (g.S8(item + 32u) == 0)
                g.W32(item + 16u, wh);
            else
                g.W32(item + 12u, wh);
            g.W32(item + 28u, wh);
        }
        if (g.S8(item + 32u) == 1)
            g.W32(item + 8u, g.U16(item + 24u) | (static_cast<uint32_t>(g.U16(item + 26u)) << 16));
    }
}

void HudSlide(GuestRam& g, uint32_t items, uint32_t s, int32_t trigger) {
    const int32_t state = g.S32(s);
    if (state == 1) {
        if (trigger != 0) {
            g.W32(s, 3);
            return;
        }
        int32_t step = g.S32(s + 20u) - g.S32(s + 4u);
        const int32_t speed = g.S32(s + 16u);
        if (speed < step) step = speed;
        HudSlideItems(g, items, static_cast<int16_t>(step * g.S32(s + 24u)), g.S32(s + 28u), g.S32(s + 32u));
        const int32_t at = g.S32(s + 4u) + step;
        g.W32(s + 4u, static_cast<uint32_t>(at));
        if (at != g.S32(s + 20u)) return;
        g.W32(s, 2);
        g.W32(s + 4u, 0);
    } else if (state == 0) {
        if (trigger == 0)
            g.W32(s + 4u, g.U32(s + 4u) + 1u);
        else
            g.W32(s + 4u, 0);
        if (g.S32(s + 4u) != g.S32(s + 8u)) return;
        g.W32(s, 1);
        g.W32(s + 4u, 0);
    } else if (state == 2) {
        if (trigger == 0) return;
        g.W32(s, 3);
        g.W32(s + 4u, g.U32(s + 20u));
    } else if (state == 3) {
        int32_t step = g.S32(s + 4u);
        const int32_t speed = g.S32(s + 12u);
        if (speed < step) step = speed;
        HudSlideItems(g, items, static_cast<int16_t>(-step * g.S32(s + 24u)), g.S32(s + 28u), g.S32(s + 32u));
        const int32_t at = g.S32(s + 4u) - step;
        g.W32(s + 4u, static_cast<uint32_t>(at));
        if (at != 0) return;
        g.W32(s, 0);
        g.W32(s + 4u, 0);
    }
}

uint32_t HudGlyph(GuestRam& g, uint32_t ch, uint32_t header, uint32_t table) {
    ch &= 0xFFu;
    const uint32_t direct = table + 11u * (ch - 32u);
    if (g.U8(direct) == ch) return direct;
    // SLUS 0x8002D1F8: a binary search over `count` 11-byte records.
    int32_t n = g.U16(header + 10u);
    uint32_t base = table;
    if (n == 0) return 0;
    while (true) {
        const uint32_t mid = base + static_cast<uint32_t>(n >> 1) * 11u;
        const int32_t d = static_cast<int32_t>(ch) - static_cast<int32_t>(g.U8(mid));
        if (d == 0) return mid;
        if (d > 0) {
            base = mid + 11u;
            n -= 1;
        }
        n >>= 1;
        if (n == 0) return 0;
        if (g.Faulted()) return 0;
    }
}

int32_t HudStringWidth(GuestRam& g, int32_t font, uint32_t str) {
    if (font == -1) return 0;
    const uint32_t rec = kHudFontRecords + 24u * static_cast<uint32_t>(font);
    const uint32_t len = StrLen(g, str);
    int32_t width = 0;
    for (uint32_t i = 0; i < len; ++i) {
        const uint32_t gl = HudGlyph(g, g.U8(str + i), g.U32(rec + 4u), g.U32(rec + 8u));
        if (gl != 0) width += g.S8(gl + 8u);
    }
    return static_cast<int16_t>(width);
}

bool HudDrawString(GuestRam& g, HudCallees& c, uint32_t sp, int32_t font, uint32_t str, int16_t x, int16_t y,
                   uint32_t ot, uint32_t colour) {
    // SLUS 0x8002CDC8, frame 88.
    const uint32_t frame = sp - 88u;
    if (font == -1) return true;
    const uint32_t rec = kHudFontRecords + 24u * static_cast<uint32_t>(font);
    const uint32_t clut = g.U16(kHudClutTable + 2u * static_cast<uint32_t>(static_cast<int32_t>(g.S8(rec + 3u))));
    const uint32_t tpage = g.U16(rec + 16u);
    const uint32_t len = StrLen(g, str);
    const uint32_t space = HudGlyph(g, 32, g.U32(rec + 4u), g.U32(rec + 8u));
    const int32_t spaceAdvance = (space != 0) ? g.S8(space + 8u) : 8;
    {
        const uint32_t heap = g.U32(kHudHeapPtr);
        const uint32_t next = g.U32(heap + 268u);
        if (!(next + len * 40u < g.U32(kHudHeapEnd))) {
            uint32_t answer = 0;
            if (!c.HeapOverflow(frame, next, len * 40u, &answer)) return false;
            g.W32(g.U32(kHudHeapPtr) + 268u, answer);
        }
    }
    uint32_t p = g.U32(g.U32(kHudHeapPtr) + 268u);
    uint32_t glyphs = 0;
    int32_t pen = x;
    const uint32_t cmd = colour | 0x2E000000u;
    for (uint32_t i = 0; i < len; ++i) {
        const uint32_t ch = g.U8(str + i);
        if (ch == 32) {
            pen += spaceAdvance;
            continue;
        }
        const uint32_t gl = HudGlyph(g, ch, g.U32(rec + 4u), g.U32(rec + 8u));
        if (gl == 0) continue;
        ++glyphs;
        const int32_t xoff = g.S8(gl + 9u), yoff = g.S8(gl + 10u);
        const uint32_t w = g.U8(gl + 2u), h = g.U8(gl + 3u);
        const uint32_t u0full = g.U8(rec + 18u) + g.U8(gl + 4u);
        const uint32_t u1 = (w + u0full) & 0xFFu;
        const uint32_t u0 = u0full & 0xFFu;
        const int32_t x0 = pen + xoff;
        const uint32_t x0s = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(x0)));
        const uint32_t x1s = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(x0 + static_cast<int32_t>(w))));
        const uint32_t v0full = g.U8(rec + 19u) + g.U8(gl + 6u);
        const uint32_t v1 = (h + v0full) & 0xFFu;
        const uint32_t v0 = v0full & 0xFFu;
        const uint32_t y0 = (static_cast<uint32_t>(static_cast<uint16_t>(y)) + static_cast<uint32_t>(yoff));
        const uint32_t y0hi = y0 << 16;
        const uint32_t y1hi = (y0 + h) << 16;
        g.W32(p, g.U32(ot) | 0x09000000u);
        g.W32(p + 4u, cmd);
        g.W32(p + 8u, x0s | y0hi);
        g.W32(p + 12u, u0 | (v0 << 8) | (clut << 16));
        g.W32(p + 16u, x1s | y0hi);
        g.W32(p + 20u, u1 | (v0 << 8) | (tpage << 16));
        g.W32(p + 24u, x0s | y1hi);
        g.W32(p + 28u, u0 | (v1 << 8));
        g.W32(p + 32u, x1s | y1hi);
        g.W32(p + 36u, u1 | (v1 << 8));
        g.W32(ot, p);
        p += 40u;
        pen += g.S8(gl + 8u);
    }
    const uint32_t heap = g.U32(kHudHeapPtr);
    g.W32(heap + 268u, g.U32(heap + 268u) + glyphs * 40u);
    return !g.Faulted();
}

bool HudDrawText(GuestRam& g, HudCallees& c, uint32_t sp, int32_t font, int32_t id, int16_t x, int16_t y,
                 uint32_t ot, uint32_t colour) {
    // SLUS 0x8002CD78, frame 32.
    const uint32_t table = g.U32(kHudStringTable);
    if (table == 0) return true;
    const uint32_t str = g.U32(table + 4u * static_cast<uint32_t>(id));
    return HudDrawString(g, c, sp - 32u, font, str, x, y, ot, colour);
}

// ============================================================================ the road side

int32_t HudRouteOff(GuestRam& g, uint32_t e) {
    const uint32_t route = g.U32(e + 428u);
    if (route != 0) {
        if (((static_cast<uint32_t>(g.U16(route + 118u)) >> g.U16(e + 172u)) & 1u) == 0) return 1;
    }
    const uint32_t key = g.U32(e + 360u);
    if ((key >> 16) == 1u) return RouteLegFor(g, g.U32(e + 428u), static_cast<int32_t>(key & 0xFFFFu)) != 0 ? 0 : 1;
    return RouteFindLegView(g, g.U32(e + 428u), key & 0xFFFFu) != 0 ? 0 : 1;
}

uint32_t HudRoadObject(GuestRam& g, int32_t id, int32_t kind) {
    if (id < 0) return 0;
    const uint32_t map = g.U32(0x8005B240u);
    const int32_t count = g.S16(map + 40u);
    if (count <= 0) return 0;
    if (g.S16(map + 44u) <= 0) return 0;
    int32_t i = g.S16(map + 42u);
    uint32_t p = g.U32(map + 28u) + 32u * static_cast<uint32_t>(i);
    for (; i < count; ++i, p += 32u) {
        if (g.S16(p + 4u) != 1) continue;
        if (g.S32(p + 16u) != id) continue;
        if (g.S32(p + 28u) == kind) return p;
        if (g.Faulted()) return 0;
    }
    return 0;
}

int32_t HudJunctionExits(GuestRam& g, int32_t node, int32_t road, uint32_t out, int32_t max, uint32_t sp) {
    const uint32_t frame = sp - 56u;
    if (node < 0) return 0;
    if (road < 0) return 0;
    const uint32_t obj = HudRoadObject(g, node, 0);
    if (obj == 0) return 0;
    const uint32_t x = g.U32(obj + 12u);
    if (x == 0) return 0;
    const uint32_t ipt = RoadJunctionIndex(g, g.S32(x));
    if (ipt == 0) return 0;
    const int32_t n = RoadTurnsFrom(g, ipt, road, frame + 16u, max);
    if (n < 0 || max < n) return 0;
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t rec = g.U32(frame + 16u + 4u * static_cast<uint32_t>(i));
        g.W32(out + 4u * static_cast<uint32_t>(i), static_cast<uint32_t>(static_cast<int32_t>(g.S16(rec + 10u))));
    }
    return n;
}

uint32_t HudWrongWayMask(GuestRam& g, uint32_t e, uint32_t sp) {
    const uint32_t frame = sp - 64u;
    uint32_t mask = (g.U16(e + 362u) == 1u) ? 0x80u : 0u;
    if (HudRouteOff(g, e) == 0) {
        if (g.U8(g.U32(e + 1084u)) & 0x80u) {
            int16_t v[3], m[3];
            const uint32_t row = g.U32(e + 340u) + 14u;
            for (uint32_t k = 0; k < 3; ++k) {
                v[k] = g.S16(e + 450u + 2u * k);
                m[k] = g.S16(row + 2u * k);
            }
            // The original evaluates the dot three times (an abs() macro); the GTE answer is the same.
            const int32_t d = DotLcm(v, m);
            const int32_t a = ((d >> 31) + d) ^ (d >> 31);
            if (a > 0xC000) mask |= 1u;
        }
    }
    if (!(mask & 1u) && HudRouteOff(g, e) != 0) mask |= 2u;
    if (mask & 3u) return mask;
    if (g.U16(e + 362u) != 0) return mask;
    const int32_t dist = RoadEndNode(g, e + 360u, frame + 32u, 1);
    const int32_t node = g.S32(frame + 32u);
    if (node == -1) return mask;
    const int32_t bank = g.S32(GameState(g) + 60u);
    if (g.S32(kHudArrowRange + 4u * static_cast<uint32_t>(bank)) < dist) return mask;
    const uint32_t leg = RouteLegFor(g, g.U32(e + 428u), node);
    if (leg == 0) return mask;
    if (HudJunctionExits(g, g.S32(frame + 32u), g.U16(e + 360u), frame + 16u, 3, frame) != 2) return mask;
    if (RouteLegHasRoad(g, leg, g.S32(frame + 16u))) mask |= 0x10u;
    if (RouteLegHasRoad(g, leg, g.S32(frame + 20u))) mask |= 0x40u;
    return mask;
}

uint32_t HudNearestRival(GuestRam& g, uint32_t e) {
    int32_t best = -1;
    int32_t bestDistance = 0x7FFF0000;
    const int32_t off = HudRouteOff(g, e);
    const int32_t n = g.S32(kHudLiveBikes);
    if (n > 0) {
        const uint32_t gs = GameState(g);
        const uint32_t own = g.U16(e + 172u);
        uint32_t b = g.U32(kHudPool0);
        for (int32_t i = 0; i < n; ++i, b += 1096u) {
            const uint32_t slot = g.U16(b + 172u);
            if (slot == own) continue;
            if (!(slot < g.U32(gs + 48u))) {
                if ((g.U8(g.U32(b + 1084u) + 1u) & 0xFu) == 2u && !(g.U8(b + 928u) & 0x10u)) continue;
            }
            if (off != 0 && g.U32(b + 360u) != g.U32(e + 360u)) continue;
            const int32_t d = static_cast<int32_t>(static_cast<uint32_t>(g.S32(b + 324u) - g.S32(e + 324u)) << 4);
            const int32_t a = ((d >> 31) + d) ^ (d >> 31);
            if (a < bestDistance) {
                bestDistance = a;
                best = i;
            }
            if (g.Faulted()) return 0;
        }
    }
    if (best < 0) return 0;
    return g.U32(kHudPool0) + 1096u * static_cast<uint32_t>(best);
}

// ============================================================================ the elements

bool HudTko(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t ot, uint32_t items, uint32_t dash, uint32_t bike,
            int32_t p, uint32_t mask) {
    (void)dash;
    const uint32_t frame = sp - 72u;
    const uint32_t rec = kHudPlayerRecs + 0x18u + 36u * static_cast<uint32_t>(p);
    uint32_t other = 0;
    if (g.U32(bike + 856u) != 0 && g.U32(bike + 1088u) != 0)
        other = kHudPlayerRecs + 0x18u + 72u + 36u * g.U16(bike + 172u);
    int32_t count = g.U8(rec + 1u);
    if (other != 0) count += g.U8(other + 1u);
    auto digits = [&](int32_t value) {
        // 0x8005F13C / 0x8005F304: a two-digit count in items 6 and 7, the tens only when non-zero.
        int32_t tens, ones = value;
        if (value >= 20) {
            ones = value - 20;
            tens = 2;
        } else if (value >= 10) {
            ones = value - 10;
            tens = 1;
        } else {
            tens = 0;
        }
        if (tens != 0) HudSetArt(g, Item(items, 6), Art(kArtSmallNum0 + tens));
        HudSetArt(g, Item(items, 7), Art(kArtSmallNum0 + ones));
        const int32_t first = (count < 10) ? 7 : 6;
        g.W32(Item(items, 7), LengthTag(g, Item(items, 7)) | g.U32(ot));
        g.W32(ot, Item(items, first) & kMask24);
    };
    const uint32_t gs = GameState(g);
    if ((g.U8(gs + 4u) & 0x10u) && g.S32(kHudSplitMode) == 2) {
        Link(g, ot, Item(items, 5), Item(items, 5));
        digits(count);
        return !g.Faulted();
    }
    if (mask != 0) return true;
    if (g.S32(dash + 140u) > 0) return true;
    if (count <= 0) return true;
    const uint32_t quota = Item(items, 3);
    const uint32_t step = g.U16(quota + 28u);
    if (count >= 6) {
        Link(g, ot, Item(items, 5), Item(items, 5));
        digits(count);
        return !g.Faulted();
    }
    // 1..5: that many TKO icons in a row, centred on item 5, and four lines joining them to the
    // speed box and the place box.
    const uint32_t icon = Item(items, 5);
    const uint32_t half = static_cast<uint32_t>(static_cast<int32_t>((count >> 1)) * static_cast<int32_t>(step));
    uint32_t x = (g.U16(icon + 24u) - half) & 0xFFFFFFFFu;
    const uint32_t left = g.U16(quota + 24u) - half;
    const uint32_t right = left + step * static_cast<uint32_t>(count) - 1u;
    const uint32_t iy = g.U16(icon + 26u);
    for (int32_t i = 0; i < count; ++i) {
        uint32_t a = 0;
        if (!HeapTake(g, c, frame, 20u, &a)) return false;
        const int32_t semi = g.S16(icon + 34u);
        const uint32_t cmd = (semi != 0) ? ((static_cast<uint32_t>(semi) << 25) | 0x64000000u) : 0x65000000u;
        g.W32(a + 4u, cmd);
        g.W32(a + 8u, (x & 0xFFFFFFFFu) | (iy << 16));
        x += step;
        g.W32(a, g.U32(ot) | 0x04000000u);
        g.W32(a + 16u, static_cast<uint32_t>(static_cast<int32_t>(g.S16(icon + 16u))) |
                           (static_cast<uint32_t>(static_cast<int32_t>(g.S16(icon + 18u))) << 16));
        g.W32(a + 12u, g.U8(icon + 12u) | (static_cast<uint32_t>(g.U8(icon + 13u)) << 8) |
                           (static_cast<uint32_t>(g.U16(icon + 14u)) << 16));
        g.W32(ot, a & kMask24);
    }
    const uint32_t speedBox = Item(items, 9);
    const uint32_t boxRight = g.U16(speedBox + 24u) + g.U16(speedBox + 28u);
    const uint32_t y1 = g.U16(speedBox + 26u) + 1u;
    const uint32_t y4 = g.U16(speedBox + 26u) + 4u;
    auto line = [&](uint32_t xa, uint32_t ya, uint32_t xb, uint32_t yb) {
        uint32_t a = 0;
        if (!HeapTake(g, c, frame, 16u, &a)) return false;
        g.W32(a + 4u, 0x40808080u);
        g.W32(a + 8u, xa | (ya << 16));
        g.W32(a + 12u, xb | (yb << 16));
        g.W32(a, g.U32(ot) | 0x03000000u);
        g.W32(ot, a & kMask24);
        return true;
    };
    if (!line(boxRight - 3u, y1, left, y1)) return false;
    if (!line(left, y4, boxRight - 1u, y4)) return false;
    const uint32_t place = (g.U8(GameState(g) + 4u) == 33) ? Item(items, 50) : Item(items, 8);
    const uint32_t px = g.U16(place + 24u);
    if (!line(px + 2u, y1, right, y1)) return false;
    if (!line(right, y4, px, y4)) return false;
    return !g.Faulted();
}

void HudSign(GuestRam& g, uint32_t ot, uint32_t items, uint32_t state, uint32_t bike, uint32_t mask,
             uint32_t dash, int32_t p) {
    // The sign code lives at guest address 0 (the original's `sw zero,0(a0)` with a0 = 0), exactly as
    // the console has it.
    const uint32_t gs = GameState(g);
    const int32_t st = g.S8(gs);
    if (st == 1 && !(g.U8(g.U32(bike + 1084u)) & 0x40u)) {
        const bool riding = g.U32(g.U32(bike + 852u) + 604u) < 2u;
        g.W32(0, 0);
        if (riding && g.S32(kHudDemo) == 0 && g.U32(PerPlayer(kHudFinished, p)) == 0) {
            if (mask & 1u)
                g.W32(0, 4);
            else if (mask & 0x10u)
                g.W32(0, 3);
            else if (mask & 0x40u)
                g.W32(0, static_cast<uint32_t>(st));
        }
        const uint32_t code = g.U32(0);
        if (code != g.U32(state + 60u)) {
            g.W32(state + 60u, code);
            g.W32(dash, 0);
            g.W32(dash + 16u, 0);
            g.W32(dash + 12u, 0);
            const int32_t now = g.S32(0);
            if (now == 1) {
                HudSetArt(g, Item(items, 82), Art(kArtTurnLeft));
            } else if (now >= 2) {
                if (now == 3) {
                    HudSetArt(g, Item(items, 83), Art(kArtTurnRight));
                } else if (now == 4) {
                    HudSetArt(g, Item(items, 81), Art(kArtWrongWay));
                    g.W32(dash + 12u, static_cast<uint32_t>(-8));
                }
            }
        }
    }
    const int32_t code = g.S32(state + 60u);
    if (code == 0) return;
    int32_t element;
    if (static_cast<uint32_t>(code - 4) < 2u)
        element = 81;
    else
        element = (code == 1) ? 82 : 83;
    HudFlashTimer(g, dash);
    int32_t show = 1;
    if (g.S32(dash + 12u) < 0) show = (element != 81) ? 1 : 0;
    if (g.U32(dash + 16u) == 0) return;
    if (show == 0) return;
    Link(g, ot, Item(items, element), Item(items, element));
}

void HudArrestIcon(GuestRam& g, uint32_t ot, uint32_t items, uint32_t dash, uint32_t bike) {
    if (g.U32(bike + 180u) < 18u) {
        if (g.S32(dash + 140u) > 0) {
            HudFlashTimer(g, dash + 128u);
            if (g.U32(dash + 144u) == 0) return;
            Link(g, ot, Item(items, 4), Item(items, 4));
            return;
        }
        g.W32(dash + 140u, 0);
        return;
    }
    uint32_t on = 1;
    if (g.S32(dash + 108u) < 3) on = g.U32(dash + 112u);
    if (on == 0) return;
    const int32_t fsm = g.S32(kHudArrestFsm);
    if (fsm < 2 || fsm >= 9) return;
    Link(g, ot, Item(items, 1), Item(items, 1));
}

bool HudCountdown(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t ot, uint32_t items, uint32_t state,
                  uint32_t bike) {
    const uint32_t frame = sp - 40u;
    const uint32_t item = Item(items, 52);
    if ((g.U32(g.U32(bike + 1084u)) & 0x60u) == 0x40u) {
        const int32_t t = g.S32(kHudCountdown);
        const int32_t whole = t >> 16;
        int32_t digit = whole + (((t >> 31) & (-whole)) + 1);
        const int32_t room = 2 - whole;
        digit += (room >> 31) & room;
        HudSetArt(g, item, Art(kArtCount0) + 16u * static_cast<uint32_t>(digit - 0));
        if (g.S32(state + 84u) != digit) {
            if (!c.PlaySound(frame, 76)) return false;
            if (!c.PlaySound(frame, 75)) return false;
            if (!c.PlaySound(frame, 29)) return false;
        }
        g.W32(state + 84u, static_cast<uint32_t>(digit));
        Link(g, ot, item, item);
        return !g.Faulted();
    }
    if (g.S32(state + 84u) == 0) return true;
    if (!(g.U8(GameState(g) + 4u) & 1u)) {
        if (!c.PlaySound(frame, 65)) return false;
        if (!c.PlaySound(frame, 106)) return false;
        if (!c.PlaySound(frame, 80)) return false;
    }
    if (!c.StopCountdownVoice(frame)) return false;
    g.W32(state + 84u, 0);
    return !g.Faulted();
}

void HudSpeed(GuestRam& g, uint32_t ot, uint32_t items, uint32_t state, uint32_t bike) {
    const uint32_t riderState = g.U32(g.U32(bike + 852u) + 604u);
    const int32_t v = (riderState - 3u < 2u) ? 0 : (g.S32(bike + 480u) >> 8);
    if (v != g.S32(state + 8u)) {
        g.W32(state + 8u, static_cast<uint32_t>(v));
        int32_t mph = FixMul(v, 0x23CA7);
        if (mph < 0) mph = 0;
        const int32_t hundreds = Div100(mph) >> 8;
        mph -= hundreds * 25600;
        const int32_t tens = Div10(mph) >> 8;
        mph -= tens * 2560;
        const int32_t ones = mph >> 8;
        HudSetArt(g, Item(items, 10), Art(kArtBigNum0) + 16u * static_cast<uint32_t>(hundreds));
        HudSetArt(g, Item(items, 11), Art(kArtBigNum0) + 16u * static_cast<uint32_t>(tens));
        HudSetArt(g, Item(items, 12), Art(kArtBigNum0) + 16u * static_cast<uint32_t>(ones));
    }
    Link(g, ot, Item(items, 10), Item(items, 12));
}

void HudOdometer(GuestRam& g, uint32_t ot, uint32_t items, uint32_t state, int32_t p) {
    if ((g.U32(kHudFrameCount) & 7u) == 7u) {
        int32_t v = FixMul(g.S32(PerPlayer(kHudOdometer, p)), 40);
        if (v != g.S32(state + 12u)) {
            g.W32(state + 12u, static_cast<uint32_t>(v));
            if (v >= 25600) {
                const int32_t q = (MulHi(v, 0x51EB851F) >> 13) - (v >> 31);
                v -= q * 25600;
            }
            int32_t tens = Div10(v) >> 8;
            v -= tens * 2560;
            int32_t ones = v >> 8;
            v -= ones << 8;
            int32_t tenths = (v * 10) >> 8;
            if (tens >= 10) tens = 0;
            if (ones >= 10) ones = 0;
            if (tenths >= 10) tenths = 0;
            HudSetArt(g, Item(items, 13), Art(kArtSmallNum0) + 16u * static_cast<uint32_t>(tens));
            HudSetArt(g, Item(items, 14), Art(kArtSmallNum0) + 16u * static_cast<uint32_t>(ones));
            HudSetArt(g, Item(items, 15), Art(kArtSmallDot));
            HudSetArt(g, Item(items, 16), Art(kArtSmallNum0) + 16u * static_cast<uint32_t>(tenths));
        }
    }
    Link(g, ot, Item(items, 13), Item(items, 16));
}

bool HudPlace(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t ot, uint32_t items, uint32_t state,
              uint32_t bike, uint32_t mask, int32_t p, uint32_t dash) {
    const uint32_t frame = sp - 56u;
    uint32_t gs = GameState(g);
    if (g.U8(gs + 4u) & 4u) return true;
    if (g.S32(kHudDemo) != 0) return true;
    if (g.U32(PerPlayer(kHudFinished, p)) != 0 && g.U8(g.U32(bike + 1084u) + 39u) != 255u) return true;
    if (g.U32(kHudFrameCount) & 1u) {
        const int32_t dist = RoadEndNode(g, bike + 360u, frame + 16u, 0);
        int32_t place;
        if (g.U16(bike + 362u) == 0 && 0xA0000 < dist) {
            if (!c.ComputePlace(frame, bike, 0, &place)) return false;
        } else {
            place = g.S32(state);
        }
        if (place != g.S32(state) && place < 20) {
            g.W32(state, static_cast<uint32_t>(place));
            const int32_t tens = (place >= 10) ? 1 : 0;
            const int32_t ones = (place >= 10) ? place - 10 : place;
            if (tens != 0) HudSetArt(g, Item(items, 45), Art(kArtBigNum0) + 16u * static_cast<uint32_t>(tens));
            HudSetArt(g, Item(items, 46), Art(kArtBigNum0) + 16u * static_cast<uint32_t>(ones));
            HudSetArt(g, Item(items, 47), Art(kArtSmallSlash));
            const int32_t field = g.S32(kHudFieldSize);
            const int32_t ftens = (field >= 10) ? 1 : 0;
            const int32_t fones = (field >= 10) ? field - 10 : field;
            if (ftens != 0) HudSetArt(g, Item(items, 48), Art(kArtSmallNum0) + 16u * static_cast<uint32_t>(ftens));
            HudSetArt(g, Item(items, 49), Art(kArtSmallNum0) + 16u * static_cast<uint32_t>(fones));
        }
    }
    // Near the line (progress * 40 < 4096) and on its route, the place flashes with the timer at +64.
    int32_t nearStart = (FixMul(g.S32(bike + 324u), 40) < 4096) ? 1 : 0;
    int32_t onRoute = 0;
    const uint32_t route = g.U32(bike + 428u);
    if (route != 0) onRoute = static_cast<int32_t>((static_cast<uint32_t>(g.U16(route + 118u)) >> g.U16(bike + 172u)) & 1u);
    nearStart &= onRoute;
    bool rank = true;
    if (nearStart != 0) {
        if (g.S32(kHudDemo) == 0 && !(mask & 2u) && g.U32(PerPlayer(kHudFinished, p)) == 0)
            HudFlashTimer(g, dash + 64u);
        if (g.U32(dash + 80u) == 0) rank = false;
    }
    if (rank) {
        const int32_t first = (g.S32(state) < 10) ? 46 : 45;
        g.W32(Item(items, 46), LengthTag(g, Item(items, 46)) | g.U32(ot));
        g.W32(ot, Item(items, first) & kMask24);
    }
    const int32_t first2 = (g.S32(kHudFieldSize) < 10) ? 49 : 48;
    const uint32_t head = Item(items, first2) & kMask24;
    g.W32(Item(items, 49), LengthTag(g, Item(items, 49)) | g.U32(ot));
    g.W32(ot, head);
    g.W32(Item(items, 47), LengthTag(g, Item(items, 47)) | head);
    g.W32(ot, Item(items, 47) & kMask24);
    return !g.Faulted();
}

void HudPlayerPanel(GuestRam& g, uint32_t ot, uint32_t items, uint32_t state, uint32_t bike, int32_t p,
                    uint32_t slides, uint32_t dash) {
    const uint32_t dirty = PerPlayer(kHudPanelDirty, p);
    if (g.U32(kHudFrameCount) & 1u) {
        uint32_t rd = g.U32(bike + 1084u);
        const uint32_t weapon = g.U8(rd + 46u);
        if (weapon != g.U32(state + 20u)) {
            g.W32(state + 20u, weapon);
            HudSetArt(g, Item(items, 24), Art(kArtWeapon0) + 16u * weapon);
            g.W32(dirty, 1);
        }
        rd = g.U32(bike + 1084u);
        const int32_t rider = Health(g, rd, 15u, 13u);
        if (rider != g.S32(state + 24u)) {
            g.W32(state + 24u, static_cast<uint32_t>(rider));
            g.W32(dirty, 1);
        }
        const int32_t machine = Condition(g, bike);
        if (machine != g.S32(state + 28u)) {
            g.W32(state + 28u, static_cast<uint32_t>(machine));
            g.W32(dirty, 1);
        }
        const uint32_t gs = GameState(g);
        const uint32_t passenger = g.U32(bike + 856u);
        if ((g.U8(gs + 4u) & 8u) && passenger != 0 && g.U32(bike + 1088u) != 0 &&
            g.U8(gs + static_cast<uint32_t>(p) + 10u) != 0) {
            const int32_t monkey = Health(g, g.U32(passenger + 1084u), 15u, 13u);
            if (monkey != g.S32(state + 36u)) {
                g.W32(state + 36u, static_cast<uint32_t>(monkey));
                g.W32(dirty, 1);
                Bar(g, Item(items, 28), monkey, dash);
            }
            const uint32_t mw = g.U8(g.U32(g.U32(bike + 856u) + 1084u) + 46u);
            if (mw != g.U32(state + 32u)) {
                g.W32(state + 32u, mw);
                HudSetArt(g, Item(items, 29), Art(kArtWeapon0) + 16u * mw);
                g.W32(dirty, 1);
            }
        }
        Bar(g, Item(items, 20), rider, dash);
        Bar(g, Item(items, 23), machine, dash);
    }
    if (g.U32(slides) == 2) return;
    g.W32(Item(items, 24), LengthTag(g, Item(items, 24)) | g.U32(ot));
    const uint32_t head = Item(items, 18) & kMask24;
    g.W32(ot, head);
    const uint32_t rd = g.U32(bike + 1084u);
    if (g.U8(rd + 47u) != 0 && g.U8(rd + 46u) != 9u) {
        g.W32(Item(items, 25), LengthTag(g, Item(items, 25)) | head);
        g.W32(ot, Item(items, 25) & kMask24);
    }
    const uint32_t gs = GameState(g);
    if (g.U32(bike + 856u) == 0) return;
    if (g.U32(bike + 1088u) == 0) return;
    if (g.U8(gs + static_cast<uint32_t>(p) + 10u) == 0) return;
    g.W32(Item(items, 29), LengthTag(g, Item(items, 29)) | g.U32(ot));
    const uint32_t head2 = Item(items, 26) & kMask24;
    g.W32(ot, head2);
    const uint32_t rdp = g.U32(g.U32(bike + 856u) + 1084u);
    if (g.U8(rdp + 47u) == 0) return;
    if (g.U8(rdp + 46u) == 9u) return;
    g.W32(Item(items, 30), LengthTag(g, Item(items, 30)) | head2);
    g.W32(ot, Item(items, 30) & kMask24);
}

int32_t HudTargetPanel(GuestRam& g, uint32_t ot, uint32_t items, uint32_t state, uint32_t bike, int32_t p,
                       uint32_t slides, uint32_t dash) {
    int32_t changed = 0;
    const uint32_t dirty = PerPlayer(kHudPanelDirty, p);
    const uint32_t targetSlot = PerPlayer(kHudTarget, p);
    if (g.U32(kHudFrameCount) & 1u) {
        uint32_t target = 0;
        const int32_t contacts = g.S8(bike + 946u);
        const uint32_t last = g.U16(bike + 8u * static_cast<uint32_t>(contacts - 1) + 958u);
        if (static_cast<int32_t>(last) < g.S32(kHudLiveBikes) && last != static_cast<uint32_t>(p)) {
            changed = 1;
            g.W32(dirty, 1);
            target = g.U32(kHudPool0) + 1096u * last;
        } else {
            const uint32_t gs = GameState(g);
            if (g.U8(gs + 4u) != 44u || g.U8(gs + 57u) != 0) target = HudNearestRival(g, bike);
        }
        bool clear = (target == 0);
        if (!clear) {
            const int32_t theirs = g.S8(target + 946u);
            const uint32_t theirLast = g.U16(target + 8u * static_cast<uint32_t>(theirs - 1) + 958u);
            if (theirLast == g.U16(bike + 172u)) {
                changed = 1;
                g.W32(dirty, 1);
            }
            if (g.S32(state + 60u) == 5 && (g.U8(g.U32(target + 1084u) + 1u) & 0xFu) != 2u) clear = true;
        }
        if (clear) {
            g.W32(targetSlot, 0);
        } else {
            g.W32(targetSlot, target);
            uint32_t rd = g.U32(target + 1084u);
            const int32_t rider = Health(g, rd, 15u, 13u);
            const int32_t machine = Condition(g, target);
            rd = g.U32(target + 1084u);
            const uint32_t weapon = g.U8(rd + 46u);
            if (target != g.U32(state + 64u)) {
                g.W32(state + 64u, target);
                HudSetArtFull(g, Item(items, 31), Art(kArtText));
                HudSetArt(g, Item(items, 31), Art(kArtText));
                changed = 1;
            }
            if (rider != g.S32(state + 40u)) {
                g.W32(state + 40u, static_cast<uint32_t>(rider));
                changed = 1;
            }
            if (machine != g.S32(state + 44u)) {
                g.W32(state + 44u, static_cast<uint32_t>(machine));
                changed = 1;
            }
            if (weapon != g.U32(state + 48u)) {
                g.W32(state + 48u, weapon);
                HudSetArt(g, Item(items, 38), Art(kArtWeapon0) + 16u * weapon);
                changed = 1;
            }
        }
    }
    const uint32_t target = g.U32(state + 64u);
    if (target == 0) return changed;
    if (g.U32(slides + 36u) == 2) return changed;
    Bar(g, Item(items, 34), g.S32(state + 40u), dash);
    Bar(g, Item(items, 37), g.S32(state + 44u), dash);
    const uint32_t gs = GameState(g);
    if (g.U32(target + 856u) != 0 && g.U32(target + 1088u) != 0 &&
        g.U8(gs + static_cast<uint32_t>(g.U16(target + 172u)) + 10u) != 0) {
        const int32_t monkey = Health(g, g.U32(g.U32(target + 856u) + 1084u), 15u, 13u);
        if (monkey != g.S32(state + 56u)) {
            g.W32(state + 56u, static_cast<uint32_t>(monkey));
            Bar(g, Item(items, 42), monkey, dash);
            changed = 1;
        }
        const uint32_t mw = g.U8(g.U32(g.U32(target + 856u) + 1084u) + 46u);
        if (mw != g.U32(state + 52u)) {
            g.W32(state + 52u, mw);
            HudSetArt(g, Item(items, 43), Art(kArtWeapon0) + 16u * mw);
            changed = 1;
        }
        g.W32(Item(items, 43), LengthTag(g, Item(items, 43)) | g.U32(ot));
        const uint32_t head = Item(items, 40) & kMask24;
        g.W32(ot, head);
        const uint32_t rdp = g.U32(g.U32(target + 856u) + 1084u);
        if (g.U8(rdp + 47u) != 0 && mw != 9u) {
            g.W32(Item(items, 44), LengthTag(g, Item(items, 44)) | head);
            g.W32(ot, Item(items, 44) & kMask24);
        }
    }
    g.W32(Item(items, 38), LengthTag(g, Item(items, 38)) | g.U32(ot));
    const uint32_t head = Item(items, 32) & kMask24;
    g.W32(ot, head);
    const uint32_t rd = g.U32(target + 1084u);
    if (g.U8(rd + 47u) != 0 && g.U8(rd + 46u) != 9u) {
        g.W32(Item(items, 39), LengthTag(g, Item(items, 39)) | head);
        g.W32(ot, Item(items, 39) & kMask24);
    }
    return changed;
}

bool HudNames(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t items, int32_t p, uint32_t slides) {
    const uint32_t frame = sp - 48u;
    const uint32_t ot = g.U32(PerPlayer(kHudOt, p));
    if (g.U32(slides) != 2) {
        const uint32_t item = Item(items, 17);
        const uint32_t own = g.U32(kHudNamePool) + 1096u * static_cast<uint32_t>(p);
        const uint32_t name = g.U8(g.U32(own + 1084u) + 38u);
        if (!HudDrawText(g, c, frame, g.S32(kHudFontIndex), static_cast<int32_t>(name), g.S16(item + 24u),
                         g.S16(item + 26u), g.U32(PerPlayer(kHudOt, p)), 0x808080u))
            return false;
    }
    (void)ot;
    const uint32_t target = g.U32(PerPlayer(kHudTarget, p));
    if (target == 0) return !g.Faulted();
    if (g.U32(slides + 36u) == 2) return !g.Faulted();
    const uint32_t item = Item(items, 31);
    const uint32_t name = g.U8(g.U32(target + 1084u) + 38u);
    return HudDrawText(g, c, frame, g.S32(kHudFontIndex), static_cast<int32_t>(name), g.S16(item + 24u),
                       g.S16(item + 26u), g.U32(PerPlayer(kHudOt, p)), 0x808080u);
}

void HudRadarDistance(GuestRam& g, uint32_t ot, uint32_t items, uint32_t state, uint32_t bike, int32_t p) {
    const uint32_t gs = GameState(g);
    const uint32_t type = g.U8(gs + 4u);
    uint32_t target;
    if (type & 0x10u)
        target = g.U32(kHudPool0) + 1096u * (static_cast<uint32_t>(p) ^ 1u);
    else if (type & 1u)
        target = g.U32(kHudPool0) + 1096u * g.U8(gs + 6u);
    else
        target = g.U32(PerPlayer(kHudTarget, p));
    if (target == 0) return;
    const int32_t d = static_cast<int32_t>(static_cast<uint32_t>(g.S32(target + 324u) - g.S32(bike + 324u)) << 4);
    int32_t arrow;
    if (HudRouteOff(g, bike) != 0 && g.S32(bike + 364u) > 0)
        arrow = (d > 0) ? 31 : 32;
    else
        arrow = (d > 0) ? 32 : 31;
    const int32_t a = FixMul(((d >> 31) + d) ^ (d >> 31), 40);
    if (!(0x9FFFF < a) && a != g.S32(state + 16u)) {
        g.W32(state + 16u, static_cast<uint32_t>(a));
        const int32_t whole = a >> 16;
        int32_t frac = a - (whole << 16);
        const int32_t tenths = (frac * 10) >> 16;
        frac -= tenths * 6553;
        int32_t hundredths = (frac * 100) >> 16;
        if (hundredths >= 10) hundredths = 0;
        HudSetArt(g, Item(items, 95), kHudArtTable + 16u * static_cast<uint32_t>(arrow));
        HudSetArt(g, Item(items, 99), Art(kArtSmallDot));
        HudSetArt(g, Item(items, 96), Art(kArtSmallNum0) + 16u * static_cast<uint32_t>(whole));
        HudSetArt(g, Item(items, 97), Art(kArtSmallNum0) + 16u * static_cast<uint32_t>(tenths));
        HudSetArt(g, Item(items, 98), Art(kArtSmallNum0) + 16u * static_cast<uint32_t>(hundredths));
    }
    Link(g, ot, Item(items, 95), Item(items, 99));
}

void HudNitro(GuestRam& g, uint32_t ot, uint32_t items, uint32_t state, uint32_t bike, uint32_t dash) {
    const int32_t held = static_cast<int32_t>((g.U32(bike + 564u) >> 9) & 1u) + g.S8(bike + 848u);
    const uint32_t rec = kHudPlayerRecs + 36u * g.U16(bike + 172u);
    int32_t cap = g.U8(rec + 20u);
    const int32_t base = static_cast<int32_t>(g.U8(g.U32(bike + 556u) + 445u)) + g.S8(kHudNitroBonus);
    if (held < cap) cap = held;
    int32_t shown = base + cap;
    if (shown <= 0) return;
    if (shown >= 9) shown = 8;
    if (held != g.S32(state + 4u)) {
        g.W32(state + 4u, static_cast<uint32_t>(held));
        for (int32_t i = 0; i < shown; ++i) {
            const int32_t art = (i < shown - held) ? kArtNitroDim : kArtNitroDim + 1;
            HudSetArt(g, Item(items, 84 + i), Art(art));
        }
    }
    if (g.U32(bike + 564u) & 0x200u) {
        const int32_t capNow = g.U8(kHudPlayerRecs + 36u * g.U16(bike + 172u) + 20u);
        int32_t index = g.U8(g.U32(bike + 556u) + 445u);
        if (capNow < held) index -= held - capNow;
        HudFlashTimer(g, dash + 32u);
        const uint32_t item = Item(items, 84 + index);
        const int32_t art = kArtNitroDim + static_cast<int32_t>(g.U32(dash + 48u) & 1u);
        HudSetArt(g, item, Art(art));
    } else if (g.U32(dash + 48u) != 0) {
        g.W32(state + 4u, static_cast<uint32_t>(-1));
        g.W32(dash + 48u, 0);
    }
    Link(g, ot, Item(items, 84), Item(items, 83 + shown));
}

void HudPanelSlides(GuestRam& g, uint32_t bike, uint32_t items, uint32_t slides, int32_t p, int32_t changed) {
    const uint32_t gs = GameState(g);
    if (g.S8(gs) != 1) return;
    if (g.U8(g.U32(bike + 1084u)) & 0x40u) return;
    const uint32_t dirty = PerPlayer(kHudPanelDirty, p);
    const int32_t own = (g.U32(dirty) != 0 || g.S32(kHudDemo) != 0 || g.U32(PerPlayer(kHudFinished, p)) != 0) ? 1 : 0;
    HudSlide(g, items, slides, own);
    const int32_t theirs = (changed != 0 || g.S32(kHudDemo) != 0 || g.U32(PerPlayer(kHudFinished, p)) != 0) ? 1 : 0;
    HudSlide(g, items, slides + 36u, theirs);
    g.W32(dirty, 0);
}

bool HudMessages(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t items, uint32_t dash, uint32_t bike,
                 int32_t p) {
    const uint32_t frame = sp - 56u;
    HudFlashTimer(g, dash + 64u);
    if (g.U32(dash + 80u) == 0) return !g.Faulted();
    int32_t id = -1;
    const uint32_t status = g.U8(g.U32(bike + 1084u) + 39u);
    if (status == 254u)
        id = 28;
    else if (status == 255u)
        id = 29;
    const uint32_t ot = g.U32(PerPlayer(kHudOt, p));
    const int32_t font = g.S32(kHudFontIndex);
    if (id == -1) {
        id = (g.S32(kHudDemo) > 0) ? 26 : 27;
        if (id == 27 && g.U32(g.U32(bike + 1084u) + 40u) != 0) return !g.Faulted();
        if (!HudDrawText(g, c, frame, font, id, g.S16(Item(items, 100) + 24u), g.S16(Item(items, 100) + 26u), ot,
                         0x808080u))
            return false;
        return HudDrawText(g, c, frame, g.S32(kHudFontIndex), id, g.S16(Item(items, 101) + 24u),
                           g.S16(Item(items, 101) + 26u), g.U32(PerPlayer(kHudOt, p)), 0x808080u);
    }
    const int32_t width = g.S32(kHudMessageWidth + 4u * static_cast<uint32_t>(id - 15));
    const int16_t x = static_cast<int16_t>(g.U16(Item(items, 102) + 24u) - static_cast<uint32_t>(width >> 1));
    return HudDrawText(g, c, frame, font, id, x, g.S16(Item(items, 102) + 26u), ot, 112u);
}

// ============================================================================ the radar strip

uint32_t HudAreaTopLeft(GuestRam& g, int32_t x, int32_t y) {
    int32_t ax = x, ay = y;
    const int32_t xs = static_cast<int16_t>(x), ys = static_cast<int16_t>(y);
    if (xs < 0)
        ax = 0;
    else if (g.S16(kGpuVramSize) - 1 < xs)
        ax = static_cast<int32_t>(g.U16(kGpuVramSize)) - 1;
    if (ys < 0)
        ay = 0;
    else if (g.S16(kGpuVramSize + 2u) - 1 < ys)
        ay = static_cast<int32_t>(g.U16(kGpuVramSize + 2u)) - 1;
    return 0xE3000000u | ((static_cast<uint32_t>(ay) & 0x3FFu) << 10) | (static_cast<uint32_t>(ax) & 0x3FFu);
}

uint32_t HudAreaBottomRight(GuestRam& g, int32_t x, int32_t y) {
    return (HudAreaTopLeft(g, x, y) & 0x00FFFFFFu) | 0xE4000000u;
}

void HudSetDrawArea(GuestRam& g, uint32_t p, uint32_t rect) {
    g.W8(p + 3u, 2);
    g.W32(p + 4u, HudAreaTopLeft(g, g.S16(rect), g.S16(rect + 2u)));
    const int32_t right = static_cast<int16_t>(g.U16(rect) + g.U16(rect + 4u) - 1u);
    const int32_t bottom = static_cast<int16_t>(g.U16(rect + 2u) + g.U16(rect + 6u) - 1u);
    g.W32(p + 8u, HudAreaBottomRight(g, right, bottom));
}

bool HudDrawArea(GuestRam& g, HudCallees& c, uint32_t sp, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t ot) {
    const uint32_t frame = sp - 56u;
    uint32_t p = 0;
    if (!HeapTake(g, c, frame, 12u, &p)) return false;
    g.W16(frame + 16u, static_cast<uint16_t>(x));
    g.W16(frame + 18u, static_cast<uint16_t>(y));
    g.W16(frame + 20u, static_cast<uint16_t>(w));
    g.W16(frame + 22u, static_cast<uint16_t>(h));
    HudSetDrawArea(g, p, frame + 16u);
    g.W32(p, (g.U32(p) & 0xFF000000u) | (g.U32(ot) & kMask24));
    g.W32(ot, (g.U32(ot) & 0xFF000000u) | (p & kMask24));
    return !g.Faulted();
}

void HudSinCos(GuestRam& g, int32_t angle, uint32_t sinOut, uint32_t cosOut) {
    const uint32_t e = 0x8005624Cu + 4u * (static_cast<uint32_t>(angle) & 0xFFFu);
    g.W32(sinOut, static_cast<uint32_t>(static_cast<int32_t>(g.S16(e)) << 4));
    g.W32(cosOut, static_cast<uint32_t>((g.S32(e) >> 16) << 4));
}

void HudRotate2(GuestRam& g, int32_t angle, uint32_t v, uint32_t sp) {
    const uint32_t frame = sp - 40u;
    HudSinCos(g, angle, frame + 16u, frame + 20u);
    const int32_t x = g.S32(v);
    const int32_t s = g.S32(frame + 16u), co = g.S32(frame + 20u);
    g.W32(v, static_cast<uint32_t>(FixMul(co, x) - FixMul(s, g.S32(v + 4u))));
    g.W32(v + 4u, static_cast<uint32_t>(FixMul(s, x) + FixMul(co, g.S32(v + 4u))));
}

void HudScale2(GuestRam& g, uint32_t v, int32_t k) {
    g.W32(v, static_cast<uint32_t>(FixMul(k, g.S32(v))));
    g.W32(v + 4u, static_cast<uint32_t>(FixMul(k, g.S32(v + 4u))));
}

bool HudRadarQuad(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t verts, uint32_t colour, uint32_t code,
                  int32_t p) {
    const uint32_t frame = sp - 40u;
    const uint32_t ot = g.U32(PerPlayer(kHudOt, p));
    uint32_t q = g.U32(g.U32(kHudHeapPtr) + 268u);
    if (!(q + 24u < g.U32(kHudHeapEnd))) {
        uint32_t answer = 0;
        if (!c.HeapOverflow(frame, q, 24u, &answer)) return false;
        q = answer;
    }
    g.W32(q, g.U32(ot) | 0x05000000u);
    g.W32(q + 4u, ((code | 0x29u) << 24) | (static_cast<uint32_t>(g.U8(colour + 2u)) << 16) |
                      (static_cast<uint32_t>(g.U8(colour + 1u)) << 8) | g.U8(colour));
    g.W32(ot, q);
    static const uint32_t kFrom[8] = {0, 2, 4, 6, 12, 14, 8, 10};
    for (uint32_t k = 0; k < 8; ++k) g.W16(q + 8u + 2u * k, g.U16(verts + kFrom[k]));
    g.W32(g.U32(kHudHeapPtr) + 268u, q + 24u);
    return !g.Faulted();
}

bool HudRadarMark(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t vec, int32_t angle, int32_t p, int32_t kind) {
    const uint32_t frame = sp - 72u;
    const int32_t k = g.S32(kHudRadarScale);
    g.W32(frame + 16u, g.U32(vec));
    g.W32(frame + 20u, g.U32(vec + 8u));
    const uint32_t colour = kHudMarkColours + 4u * static_cast<uint32_t>(kind);
    const uint32_t rect = kHudRadarRects + 12u * static_cast<uint32_t>(p);
    HudScale2(g, frame + 16u, k);
    HudRotate2(g, angle, frame + 16u, frame);
    const uint32_t x = g.U16(rect + 8u) + static_cast<uint32_t>(static_cast<int32_t>(g.S16(frame + 18u)));
    g.W16(frame + 24u, static_cast<uint16_t>(x));
    const uint32_t y = g.U16(rect + 10u) - static_cast<uint32_t>(static_cast<int32_t>(g.S16(frame + 22u)));
    g.W16(frame + 26u, static_cast<uint16_t>(y));
    const int32_t xs = static_cast<int16_t>(x), ys = static_cast<int16_t>(y);
    const int32_t rx = g.S16(rect), ry = g.S16(rect + 2u);
    if (xs < rx) return true;
    if (rx + g.S16(rect + 4u) - 1 < xs) return true;
    if (ys < ry) return true;
    if (ry + g.S16(rect + 6u) - 1 < ys) return true;
    auto put = [&](uint32_t o, uint32_t v) { g.W16(frame + o, static_cast<uint16_t>(v)); };
    if (kind == 0) {
        put(38, y - 3u);
        put(34, y - 3u);
        put(44, x - 3u);
        put(40, x + 3u);
        put(36, x);
        put(32, x);
        put(42, y + 3u);
        put(46, y + 3u);
    } else if (kind == 5) {
        put(44, x - 2u);
        put(32, x - 2u);
        put(38, y - 3u);
        put(34, y - 3u);
        put(42, y + 3u);
        put(46, y + 3u);
        put(40, x + 2u);
        put(36, x + 2u);
    } else {
        put(44, x - 1u);
        put(32, x - 1u);
        put(38, y - 2u);
        put(34, y - 2u);
        put(42, y + 2u);
        put(46, y + 2u);
        put(40, x + 1u);
        put(36, x + 1u);
    }
    return HudRadarQuad(g, c, frame, frame + 32u, colour, 0, p);
}

bool HudRadarRectArea(GuestRam& g, HudCallees& c, uint32_t sp, int32_t p) {
    const uint32_t frame = sp - 48u;
    const uint32_t r = kHudRadarRects + 12u * static_cast<uint32_t>(p);
    const uint32_t heap = g.U32(kHudHeapPtr);
    const uint32_t env = heap + 112u * g.U8(heap + 5u);
    const uint32_t x = g.U16(r) + static_cast<uint32_t>(static_cast<int32_t>(g.S16(env + 24u)));
    const uint32_t y = g.U16(r + 2u) + static_cast<uint32_t>(static_cast<int32_t>(g.S16(env + 26u)));
    const uint32_t w = g.U16(r + 4u), h = g.U16(r + 6u);
    g.W16(frame + 32u, static_cast<uint16_t>(x));
    g.W16(frame + 34u, static_cast<uint16_t>(y));
    g.W16(frame + 36u, static_cast<uint16_t>(w));
    g.W16(frame + 38u, static_cast<uint16_t>(h));
    return HudDrawArea(g, c, frame, static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(w),
                       static_cast<int16_t>(h), g.U32(PerPlayer(kHudOt, p)));
}

bool HudRadarFullArea(GuestRam& g, HudCallees& c, uint32_t sp, int32_t p) {
    const uint32_t frame = sp - 144u;
    for (uint32_t k = 0; k < 92u; ++k) g.W8(frame + 40u + k, g.U8(kGpuDrawEnv + k)); // SLUS 0x80048FBC GetDrawEnv
    g.W16(frame + 36u, 384);
    g.W16(frame + 38u, 240);
    const uint32_t x = g.U16(frame + 40u), y = g.U16(frame + 42u);
    g.W16(frame + 32u, static_cast<uint16_t>(x));
    g.W16(frame + 34u, static_cast<uint16_t>(y));
    return HudDrawArea(g, c, frame, static_cast<int16_t>(x), static_cast<int16_t>(y), 384, 240,
                       g.U32(PerPlayer(kHudOt, p)));
}

bool HudRadarMarks(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t e, uint32_t kind, int32_t angle, int32_t p) {
    const uint32_t frame = sp - 80u;
    const uint32_t pool = 0x800CE4D0u;
    uint32_t b = g.U32(pool);
    const uint32_t dash = kHudDash224 + 224u * static_cast<uint32_t>(p);
    int32_t i = g.S32(g.U32(pool + 12u));
    if (i >= 0) {
        g.W32(frame + 32u, kind & 0xFu);
        for (; i >= 0; --i, b += g.U32(pool + 4u)) {
            if (g.S16(b + 320u) == 0) continue;
            if (g.U16(b + 172u) == static_cast<uint32_t>(p)) continue;
            const uint32_t rider = g.U32(b + 852u);
            const uint32_t from = (g.U32(rider + 604u) < 3u) ? b : g.U32(b + 852u);
            for (uint32_t k = 0; k < 3; ++k)
                g.W32(frame + 16u + 4u * k, static_cast<uint32_t>(g.S32(from + 184u + 4u * k) - g.S32(e + 184u + 4u * k)));
            const uint32_t gs = GameState(g);
            const uint32_t k2 = g.U8(g.U32(b + 1084u) + 1u) & 0xFu;
            int32_t type;
            if (k2 == g.U32(frame + 32u))
                type = 2;
            else if (k2 == 2u)
                type = 4;
            else
                type = (g.U8(gs + 4u) == 44u) ? 4 : 3;
            const uint32_t t = g.U8(GameState(g) + 4u);
            const uint32_t slot = g.U16(b + 172u);
            bool flash = false;
            if ((t & 0x10u) && slot == (static_cast<uint32_t>(p) ^ 1u))
                flash = true;
            else if ((t & 1u) && slot != static_cast<uint32_t>(p) && slot == g.U8(GameState(g) + 6u))
                flash = true;
            if (flash) {
                HudFlashTimer(g, dash + 64u);
                if (g.U32(dash + 80u) == 0) continue;
                type = 5;
            }
            if (!HudRadarMark(g, c, frame, frame + 16u, angle, p, type)) return false;
            if (g.Faulted()) return false;
        }
    }
    g.W32(frame + 24u, 0);
    g.W32(frame + 20u, 0);
    g.W32(frame + 16u, 0);
    return HudRadarMark(g, c, frame, frame + 16u, angle, p, 0);
}

bool HudRadarStrip(GuestRam& g, HudCallees& c, uint32_t sp, uint32_t bike, int32_t p) {
    const uint32_t frame = sp - 32u;
    if (g.S32(kHudDemo) != 0) return true;
    if (g.U32(PerPlayer(kHudFinished, p)) != 0) return true;
    const uint32_t rider = g.U32(bike + 852u);
    const uint32_t e = (g.U32(rider + 604u) < 3u) ? bike : rider;
    if (e == 0) return true;
    if (g.S16(e + 320u) == 0) return true;
    if (!HudRadarFullArea(g, c, frame, p)) return false;
    int32_t table[20];
    for (uint32_t k = 0; k < 20; ++k) table[k] = g.S32(0x8005285Cu + 4u * k);
    int32_t angle = RatAtan2(static_cast<int32_t>(g.S16(e + 444u)) << 4, static_cast<int32_t>(g.S16(e + 448u)) << 4, table);
    if (angle < 0) angle += 4096;
    if (!HudRadarMarks(g, c, frame, e, g.U8(g.U32(bike + 1084u) + 1u), angle, p)) return false;
    return HudRadarRectArea(g, c, frame, p);
}

// ============================================================================ the driver

bool HudFrame(GuestRam& g, HudCallees& c, uint32_t sp) {
    const uint32_t frame = sp - 88u;
    if (g.U32(kHudEnabled) == 0) return true;
    g.W32(kHudFrameCount, g.U32(kHudFrameCount) + 1u);
    uint32_t gs = GameState(g);
    if ((g.U8(gs + 4u) & 0x10u) && g.S32(kHudSplitMode) != 1) {
        // The split-screen divider, a POLY_F4 across the screen at the split rectangle's edge.
        const uint32_t d = kHudDivider;
        g.W8(d + 3u, 5);
        const uint32_t src = g.U32(kHudDividerSrc);
        g.W8(d + 7u, 40);
        const int32_t top = g.S16(src + 6u);
        const int32_t bottom = g.S16(src + 10u);
        g.W16(d + 8u, 0);
        g.W16(d + 12u, 384);
        g.W16(d + 16u, 0);
        g.W16(d + 20u, 384);
        const int32_t diff = bottom - top;
        const int32_t height = ((diff >> 31) + diff) ^ (diff >> 31);
        g.W16(d + 10u, static_cast<uint16_t>(top));
        g.W16(d + 14u, static_cast<uint16_t>(top));
        g.W16(d + 18u, static_cast<uint16_t>(top + height));
        g.W16(d + 22u, static_cast<uint16_t>(top + height));
        const uint32_t ot = g.U32(kHudOt);
        g.W32(d, (g.U32(d) & 0xFF000000u) | (g.U32(ot) & kMask24));
        g.W32(ot, (g.U32(ot) & 0xFF000000u) | (d & kMask24));
    }
    for (int32_t p = 0; p < g.S32(GameState(g) + 48u); ++p) {
        const uint32_t bike = g.U32(PerPlayer(kHudPlayerBikes, p));
        const uint32_t ot = g.U32(PerPlayer(kHudOt, p));
        const uint32_t items = g.U32(PerPlayer(kHudItemsPtr, p));
        const uint32_t state = kHudState88 + 88u * static_cast<uint32_t>(p);
        const uint32_t dash = kHudDash224 + 224u * static_cast<uint32_t>(p);
        const uint32_t slides = kHudSlide72 + 72u * static_cast<uint32_t>(p);
        const uint32_t mask = HudWrongWayMask(g, bike, frame);
        gs = GameState(g);
        if (g.S8(gs) == 6) {
            if ((g.U8(gs + 4u) & 0x10u) && g.S32(kHudSplitMode) == 2) {
                HudLinkRange(g, ot, items, 105, 108);
                HudLinkRange(g, ot, items, 0, 0);
            }
            if (g.Faulted()) return false;
            continue;
        }
        uint32_t type = g.U8(gs + 4u);
        uint32_t finished = 0;
        if (((type & 1u) || type == 44u) && (g.U32(bike + 560u) & 0x08000000u)) finished = 1;
        g.W32(PerPlayer(kHudFinished, p), finished);
        type = g.U8(GameState(g) + 4u);
        bool messages = false;
        bool copHud = false;
        if (type & 1u) {
            if (g.S32(kHudDemo) != 0) {
                messages = true;
            } else if (type != 44u) {
                const uint32_t rd = g.U32(bike + 1084u);
                if ((g.U8(rd + 1u) & 0xFu) == 2u && (type != 33u || g.U8(rd + 39u) == 248u)) {
                    if (!HudArrestMessage(g, c, frame, items, bike, dash, p)) return false;   // 0x80062F34, PORTED (modes.h)
                    HudArrestIcon(g, ot, items, dash, bike);
                }
            }
        }
        if (!messages) {
            if (g.S32(kHudDemo) != 0 || g.U32(PerPlayer(kHudFinished, p)) != 0 ||
                g.U32(g.U32(bike + 1084u) + 40u) != 0)
                messages = true;
        }
        if (messages) {
            if (!HudMessages(g, c, frame, items, dash, bike, p)) return false;
        } else {
            if (!HudCountdown(g, c, frame, ot, items, state, bike)) return false;
            HudNitro(g, ot, items, state, bike, dash);
            type = g.U8(GameState(g) + 4u);
            const uint32_t rd = g.U32(bike + 1084u);
            if ((type & 1u) && type != 33u && (g.U8(rd + 1u) & 0xFu) == 2u) {
                HudCopClock(g, ot, items, dash, state, bike);           // 0x80062C40, PORTED (modes.h)
                if (g.Faulted()) return false;
                HudLinkRange(g, ot, items, 51, 51);
            } else {
                HudSpeed(g, ot, items, state, bike);
                HudOdometer(g, ot, items, state, p);
                HudLinkRange(g, ot, items, 9, 9);
            }
            if (!(g.U8(GameState(g) + 4u) & 4u)) HudRadarDistance(g, ot, items, state, bike, p);
            type = g.U8(GameState(g) + 4u);
            if ((type & 1u) && (g.U8(g.U32(bike + 1084u) + 1u) & 0xFu) == 2u) {
                copHud = true;
                if (!HudSuspectName(g, c, frame, ot, items + 3384u, p)) return false;   // 0x8005FAC4, PORTED (modes.h)
                if (!HudRadarStrip(g, c, frame, bike, p)) return false;
                HudLinkRange(g, ot, items, 50, 50);
                HudLinkRange(g, ot, items, 104, 104);
                if (g.U8(GameState(g) + 4u) != 33u) {
                    if (!HudArrests(g, c, frame, ot, items, p, mask)) return false;   // 0x80062610, PORTED (modes.h)
                } else {
                    if (!HudTko(g, c, frame, ot, items, dash, bike, p, mask)) return false;
                }
            } else {
                gs = GameState(g);
                type = g.U8(gs + 4u);
                const bool copMode = ((type & 4u) && type != 44u) || (type == 44u && g.U8(gs + 57u) < 2u);
                if (copMode) {
                    type = g.U8(GameState(g) + 4u);
                    const uint32_t flag = (type == 44u || type == 36u) ? 1u : 0u;
                    HudRaceClock(g, ot, items, state, dash, bike, flag, mask);   // 0x80063530, PORTED (modes.h)
                    if (g.Faulted()) return false;
                    if (g.U8(GameState(g) + 4u) == 4u) {
                        HudSplits(g, ot, items, dash, bike, mask);           // 0x800636F0, PORTED (modes.h)
                        if (g.Faulted()) return false;
                    }
                } else {
                    if (!(g.U8(GameState(g) + 4u) & 4u)) {
                        if (!HudTko(g, c, frame, ot, items, dash, bike, p, mask)) return false;
                    }
                    if (!HudPlace(g, c, frame, ot, items, state, bike, mask, p, dash)) return false;
                    if (!HudRadarStrip(g, c, frame, bike, p)) return false;
                    HudLinkRange(g, ot, items, 8, 8);
                    HudLinkRange(g, ot, items, 104, 104);
                }
            }
            (void)copHud;
            const int32_t changed = HudTargetPanel(g, ot, items, state, bike, p, slides, dash);
            HudPanelSlides(g, bike, items, slides, p, changed);
            HudPlayerPanel(g, ot, items, state, bike, p, slides, dash);
            if (!HudNames(g, c, frame, items, p, slides)) return false;
            HudArrestIcon(g, ot, items, dash, bike);
            HudSign(g, ot, items, state, bike, mask, dash, p);
        }
        gs = GameState(g);
        if ((g.U8(gs + 4u) & 0x10u) && g.S32(kHudSplitMode) == 2) HudLinkRange(g, ot, items, 105, 108);
        HudLinkRange(g, ot, items, 0, 0);
        if (g.Faulted()) return false;
    }
    HudSetDrawMode(g, kHudDrawModeHead, 1, 1, 0);
    const uint32_t ot2 = g.U32(kHudOt2);
    g.W32(kHudDrawModeHead, g.U32(ot2) | 0x01000000u);
    g.W32(ot2, kHudDrawModeHead & kMask24);
    return !g.Faulted();
}

} // namespace rr::sim
