#pragma once
// The front end's picture, drawn natively from the shell's own data.
//
// The original's widget handlers build PlayStation primitives into an ordering
// table; the product does not port that code, it ports the LAYOUT. Every position, colour, picture
// and string comes from the widget records in the shell arena (the ported logic's own memory, so the
// cursor, the choosers and the mode are exactly what the ported functions left there), from
// FEMISC.PSH / the .PFN fonts / FESTRING.LOC / the MDEC .STR pictures of the player's disc
// (shell_assets.h), and from the two FourCC tables in RASHCDF. What is not decoded yet is drawn as
// nothing rather than guessed.
//
// Output: a 512 x 240 RGBA picture, the shell's display area (SLUS 0x8001BE08(0, 0, 512, 240)).
#include "game/shell/shell_assets.h"
#include "game/shell/shell_logic.h"
#include "rrformats/hd_pack.h"
#include "rrvfs/disc_image.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rr::shell {

// One call of the SLUS text functions the ported text path makes (shell_text.h), as the product's
// callee records it: which widget's handler made it, the font slot, the string (already resolved from
// the arena's FESTRING.LOC table or sprintf buffer), the rectangle, colour and justification. The view
// lays it out as SLUS 0x8002CB08 / 0x8002CC74 / 0x8002CD78 do (shell_view.cpp TextLayout).
struct TextCall {
    uint32_t widget = 0;
    int32_t font = 1;          // the slot: 1 BTN_FONT, 2 MINIFONT, 3 HDR_FONT (shell_arena.cpp)
    std::string text;
    int32_t x = 0, y = 0, w = 0, h = 0;
    uint32_t rgb = 0x808080;
    int32_t just = 0;          // 0 left, 1 right, 2 centre, 3 wrapped in the box; -1 at (x, y)
    // Not a string: the sprite emitter 0x800700F0 (a FourCC picture at x, y) or the panel emitter
    // 0x8006ECDC (a frame between the outer x..w, y..h and the inner rectangle ix0..iy1).
    uint32_t sprite = 0;
    bool panel = false;
    // A draw-area primitive (SLUS 0x8001C304): the texts of the same widget recorded before it are
    // clipped to x, y, w, h (the ordering table draws it first). A flat quad (AddPrim of a POLY_F4):
    // the corners qx/qy, translucent when `semi`.
    bool clip = false, quad = false, semi = false;
    int32_t qx[4] = {}, qy[4] = {};
    // A ported widget handler's packet (shell_widgets.h): a sprite's frame inside its picture (sw > 0), a
    // flat rectangle (tile, x/y/w/h) or a closed line box (box, x..x+w, y..y+h), and for every call the
    // ordering-table entry it was linked into - the view draws a widget's calls in the order the GPU
    // walks the table (the highest entry first, the last linked of an entry first).
    int32_t sx = 0, sy = 0, sw = 0, sh = 0;
    bool tile = false, box = false;
    uint32_t ot = 0;
    int32_t ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0;
};

// A panel film's picture this frame (panel_films.h): picture `picture` of the .STR `name` copied into the
// display at (x, y), between the backdrop table 0x8009CCE4 and the main table (the frame 0x80080274).
struct FilmShown {
    std::string name;
    int picture = 0;
    int x = 0, y = 0;
};

// A flat quadrilateral as the GPU splits it, triangles (v0, v1, v2) and (v1, v2, v3), each pixel whose
// top-left corner lies inside (edge functions, a top-left rule for the shared and boundary edges);
// rrverify menuprims uses this same routine. The console's rule: a point on a top edge (horizontal, the
// interior below) or a left edge is drawn, one on a bottom or right edge is not. RRJB_EDGE=gl is the control:
// the horizontal test mirrored (a top edge dropped) and the last row and column left out, so a slanted quad
// loses its top row.
template <class Plot>
void RasterQuad(const int32_t qx[4], const int32_t qy[4], Plot&& plot) {
    const int tri[2][3] = {{0, 1, 2}, {1, 2, 3}};
    for (const auto& t : tri) {
        int32_t ax = qx[t[0]], ay = qy[t[0]], bx = qx[t[1]], by = qy[t[1]], cx = qx[t[2]], cy = qy[t[2]];
        int64_t area = static_cast<int64_t>(bx - ax) * (cy - ay) - static_cast<int64_t>(by - ay) * (cx - ax);
        if (area == 0) continue;
        if (area < 0) std::swap(bx, cx), std::swap(by, cy);
        const int32_t x0 = std::min({ax, bx, cx}), x1 = std::max({ax, bx, cx});
        const int32_t y0 = std::min({ay, by, cy}), y1 = std::max({ay, by, cy});
        auto edge = [](int32_t px, int32_t py, int32_t ex, int32_t ey, int32_t fx, int32_t fy) {
            return static_cast<int64_t>(fx - ex) * (py - ey) - static_cast<int64_t>(fy - ey) * (px - ex);
        };
        const bool old = [] {
            const char* e = std::getenv("RRJB_EDGE");
            return e != nullptr && std::string(e) == "gl";
        }();
        auto topLeft = [old](int32_t ex, int32_t ey, int32_t fx, int32_t fy) {
            return old ? ((ey == fy && fx < ex) || (fy < ey)) : ((ey == fy && fx > ex) || (fy < ey));
        };
        const bool tl0 = topLeft(ax, ay, bx, by), tl1 = topLeft(bx, by, cx, cy), tl2 = topLeft(cx, cy, ax, ay);
        const int32_t yEnd = old ? y1 : y1 + 1, xEnd = old ? x1 : x1 + 1;
        for (int32_t y = y0; y < yEnd; ++y)
            for (int32_t x = x0; x < xEnd; ++x) {
                const int64_t w0 = edge(x, y, ax, ay, bx, by), w1 = edge(x, y, bx, by, cx, cy), w2 = edge(x, y, cx, cy, ax, ay);
                if ((w0 > 0 || (w0 == 0 && tl0)) && (w1 > 0 || (w1 == 0 && tl1)) && (w2 > 0 || (w2 == 0 && tl2))) plot(x, y);
            }
    }
}

class ShellView {
public:
    static constexpr int kWidth = 512, kHeight = 240;

    explicit ShellView(const DiscImage& disc);

    // Draws the current screen (and the screens under it on its parent chain) out of `g`.
    // `frame` advances the animated pictures (one step per call at the shell's 60 Hz).
    void Draw(GuestRam& g, uint32_t frame, std::vector<uint8_t>& rgba, const std::vector<TextCall>* texts = nullptr);
    // The string pools (for a text call the product resolves on the host side).
    const std::vector<std::string>& Strings() const { return strings_; }
    const std::vector<std::string>& GameStrings() const { return gameStrings_; }
    // The race's results scene (race_results.h): its text calls in font 4 over the shell's backdrop.
    void DrawResults(GuestRam& g, const std::vector<TextCall>& texts, std::vector<uint8_t>& rgba);

    // What the view could not find (a FourCC with no picture, a text kind it does not decode),
    // each once - for the log.
    const std::vector<std::string>& Missing() const { return missing_; }

    // Development (rrverify menuprims): every picture the view blits, by its FourCC ("film:<name>" for a
    // film frame) and screen rectangle, so a check can tell which pixels came from which source.
    struct BlitNote {
        std::string id;
        int x = 0, y = 0, w = 0, h = 0;
    };
    void SetBlitLog(std::vector<BlitNote>* log) { blitLog_ = log; }
    // Widget types 7, 9, 11, 12, 13, 14, 15, 17, 18 drawn from their PORTED handlers' calls (shell_widgets.h) - the
    // product's default; off (RRJB_MENU_WIDGETS=off, the negative control) the view's own layout of them.
    void SetPortedWidgets(bool on) { portedWidgets_ = on; }
    // Widget types 2..6 (the panel films) and 8 (the logos) PORTED too (shell_panel.h): their calls are
    // drawn, the view's own case 3 / 8 layout is not. RRJB_MENU_FILMS=off (the negative control) and
    // RRJB_MENU_WIDGETS=off bring the view's own back.
    void SetPortedPanels(bool on) { portedPanels_ = on; }
    // The panel films' pictures of this frame (null: none).
    void SetFilmShown(const std::vector<FilmShown>* shown) { filmShown_ = shown; }
    // The size of a FEMISC.PSH shape by FourCC (false when it is not one).
    bool ShapeSize(uint32_t fourcc, int* w, int* h) const;

    // HD media (docs\HD-MEDIA.md). Scale 1 - the default and every check's - is the original path, byte for byte.
    // Above 1 the view draws the same layout `scale` times larger (Width() x Height()): each picture from the active HD
    // pack (hd_pack.h) when the pack has it for exactly this source, otherwise its texels enlarged; the glyphs of the
    // four fonts from their contour atlases (the live colour and the STP rule still apply); rectangles, lines and
    // quads as blocks of the enlarged pixel grid. The pack is the process's (rr::hd::Active()).
    void SetScale(int scale);
    int Scale() const { return scale_; }
    int Width() const { return kWidth * scale_; }
    int Height() const { return kHeight * scale_; }
    // One line for the log: HD pictures / fonts used and missing so far.
    std::string HdReport() const;

private:
    struct Image {
        int width = 0, height = 0;
        std::vector<uint32_t> rgba; // Bgr555ToRgba8 of each pixel, 0 = transparent
        // HD media: the pack key and source hash of this picture (empty key: no HD version can exist), and for a
        // composite (BAC1 / BAC2) the film frames it is built from, each at its position.
        std::string hdKey;
        uint64_t hash = 0;
        struct Part {
            std::string film;
            int frame = 0, x = 0, y = 0;
        };
        std::vector<Part> parts;
    };
    const Image* Picture(const std::string& fourcc);
    const std::vector<Image>* Film(const std::string& name); // a whole .STR by resource name
    void Blit(std::vector<uint8_t>& out, const Image& im, int x, int y, uint32_t rgb, bool modulate, int srcX = 0,
              int srcY = 0, int w = -1, int h = -1);
    int DrawText(std::vector<uint8_t>& out, int font, const std::string& text, int x, int y, uint32_t rgb,
                 int maxWidth = 0, int maxHeight = 0);
    int TextWidth(int font, const std::string& text);
    std::string String(uint32_t id) const;
    // SLUS 0x8002CB08's justification and 0x80066318's wrap, then 0x8002CDC8's glyph walk.
    void TextLayout(std::vector<uint8_t>& out, const TextCall& t);
    void Glyphs(std::vector<uint8_t>& out, int font, const std::string& text, int x, int y, uint32_t rgb);
    int Width(int font, const std::string& text) const;
    const std::vector<TextCall>* texts_ = nullptr;
    std::vector<BlitNote>* blitLog_ = nullptr;
    bool portedWidgets_ = true;
    bool portedPanels_ = true;
    const std::vector<FilmShown>* filmShown_ = nullptr;
    // The two passes of a frame with ported widgets (0x80080274): 1 the calls linked into the backdrop
    // table 0x8009CCE4 (then the panel films), 2 the rest; 0 every call (the view's own layout).
    int phase_ = 0;
    void DrawFilms(std::vector<uint8_t>& out);
    void DrawRecorded(std::vector<uint8_t>& out, uint32_t widget); // a widget's calls in ordering-table order
    std::string lastId_; // the source the next Blit draws (Picture / Film set it)
    int clip_[4] = {0, 0, kWidth, kHeight}; // x0, y0, x1, y1 of the text being laid out
    void Note(const std::string& what);
    void DrawScreen(GuestRam& g, uint32_t screen, uint32_t frame, std::vector<uint8_t>& out, bool current);
    void LoadingPicture(GuestRam& g, std::vector<uint8_t>& rgba); // screen 57 (RASHCDF 0x8006E008)
    uint32_t OptionList(GuestRam& g) const; // the bound chooser's current option's display list, or 0

    const DiscImage& disc_;
    MdecCodebook book_;
    ShapeBank shapes_;
    Font fonts_[5];                   // 1 BTN_FONT, 2 MINIFONT, 3 HDR_FONT, 4 GAMEFONT (the race's)
    std::vector<std::string> gameStrings_; // GAMESTRG.LOC
    std::vector<std::string> strings_;
    std::map<std::string, Image> pictures_;
    std::map<std::string, std::vector<Image>> films_;
    std::map<std::string, std::pair<std::string, int>> frameOf_; // FourCC -> (.STR name, frame)
    std::vector<std::string> missing_;
    std::vector<std::string> noted_;
    bool tablesRead_ = false;
    void ReadTables(GuestRam& g);

    // ---- HD media (SetScale)
    int scale_ = 1;
    uint64_t hdGeneration_ = 0;
    struct HdCached {
        std::shared_ptr<const rr::hd::Rgba> image; // null: the pack has none (or refused it)
        uint64_t used = 0;
    };
    std::map<std::string, HdCached> hdCache_;
    size_t hdBytes_ = 0;
    uint64_t hdUse_ = 0;
    std::shared_ptr<const rr::hd::Contour> hdFonts_[5];
    bool hdFontTried_[5] = {};
    size_t hdPictures_ = 0, hdMissing_ = 0, hdFontsUsed_ = 0;
    void HdSync(); // drops the HD copies when the pack or the switch changed
    std::shared_ptr<const rr::hd::Rgba> HdOf(const Image& im);
    const rr::hd::Contour* HdFont(int font);
    // One texel (sx, sy) of font `font`'s sheet drawn at the base pixel (tx, ty): its grey through the call's colour;
    // `stp`: the shell fonts' semi-transparent darkest entries (Glyphs). Scaled: the contour atlas' texels.
    void GlyphTexel(std::vector<uint8_t>& out, int font, int sx, int sy, int tx, int ty, uint32_t rgb, bool stp);
    // A base pixel (x, y) as the block of scaled pixels it covers: f(uint8_t* rgba) on each.
    template <class F>
    void Block(std::vector<uint8_t>& out, int x, int y, F&& f) {
        const int s = scale_, w = kWidth * s;
        for (int yy = 0; yy < s; ++yy)
            for (int xx = 0; xx < s; ++xx)
                f(&out[(static_cast<size_t>(y * s + yy) * static_cast<size_t>(w) + static_cast<size_t>(x * s + xx)) * 4u]);
    }
    void BlitHd(std::vector<uint8_t>& out, const Image& im, int x0, int y0, uint32_t rgb, bool modulate, int sx, int sy,
                int w, int h);
};

} // namespace rr::shell
