// The front end's picture (shell_view.h). Layout ported, drawing ours: per widget type, the record
// fields the original's handler uses.
#include "game/shell/shell_view.h"

#include "game/shell/shell_text.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace rr::shell {

namespace {

std::vector<uint8_t> Read(const DiscImage& disc, const std::string& path) {
    const auto f = disc.Find(path);
    if (!f) throw std::runtime_error("the disc has no " + path);
    return disc.ReadFile(*f);
}

std::string FourCC(uint32_t w) {
    std::string s(4, ' ');
    for (int i = 0; i < 4; ++i) s[static_cast<size_t>(i)] = static_cast<char>((w >> (8 * i)) & 0xFFu);
    return s;
}

std::string Upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

// The .STR resources whose frames the shell binds to FourCC ids, in the order their runs follow one
// another in the id table RASHCDF 0x800892BC (after its seven composite ids), as the frame binder's
// arms RASHCDF 0x800765EC.. walk them; given as resource-record indices of the table 0x800897E4.
// `rrshell framecheck` compares the binding this produces with the capture's sprite records.
constexpr int kStrRunOrder[] = {0, 6, 7, 8, 10, 13, 12, 26, 27, 28, 29, 14, 19, 24, 25, 20, 21, 22, 23, 9, 16, 15};
constexpr int kCompositeIds = 7; // BAC2 BAC1 DG01 DG02 DG03 AN01 AN02: built from other frames

} // namespace

ShellView::ShellView(const DiscImage& disc) : disc_(disc) {
    book_ = LoadDefaultCodebook(Read(disc, "SLUS_010.53"));
    shapes_ = ParseShapeBank(Read(disc, "DATA/FE/FEMISC.PSH"));
    fonts_[1] = ParseFont(Read(disc, "DATA/FE/BTN_FONT.PFN"));
    fonts_[2] = ParseFont(Read(disc, "DATA/FE/MINIFONT.PFN"));
    fonts_[3] = ParseFont(Read(disc, "DATA/FE/HDR_FONT.PFN"));
    fonts_[0] = fonts_[2];
    // The race's font and strings, for the race's own results scene (race_results.h): font 4.
    fonts_[4] = ParseFont(Read(disc, "DATA/GAMEFONT.PFN"));
    gameStrings_ = ParseStringPool(Read(disc, "DATA/GAMESTRG.LOC"));
    strings_ = ParseStringPool(Read(disc, "DATA/FE/FESTRING.LOC"));
}

bool ShellView::ShapeSize(uint32_t fourcc, int* w, int* h) const {
    const Shape* sh = shapes_.Find(FourCC(fourcc).c_str());
    if (sh == nullptr) return false;
    *w = sh->width;
    *h = sh->height;
    return true;
}

void ShellView::Note(const std::string& what) {
    if (std::find(noted_.begin(), noted_.end(), what) != noted_.end()) return;
    noted_.push_back(what);
    missing_.push_back(what);
}

std::string ShellView::String(uint32_t id) const { return id < strings_.size() ? strings_[id] : std::string(); }

const std::vector<ShellView::Image>* ShellView::Film(const std::string& name) {
    const std::string key = Upper(name);
    lastId_ = "film:" + key;
    auto it = films_.find(key);
    if (it != films_.end()) return &it->second;
    std::vector<Image> frames;
    try {
        const std::vector<uint8_t> file = Read(disc_, "DATA/FE/" + key);
        for (const Picture15& p : DecodeStrFrames(file, book_)) {
            Image im;
            im.width = p.width;
            im.height = p.height;
            im.hdKey = "DATA/FE/" + key + "#" + std::to_string(frames.size()); // HD media (SetScale)
            im.hash = rr::hd::SourceHash15(p.width, p.height, p.px.data());
            im.rgba.resize(p.px.size());
            // The MDEC's 16-bit output carries the mask bit (bit 15 set): a decoded picture has no
            // transparent texel - black is drawn black (the capture: RRLG's black texels are drawn).
            for (size_t i = 0; i < p.px.size(); ++i) im.rgba[i] = Bgr555ToRgba8(static_cast<uint16_t>(p.px[i] | 0x8000u));
            frames.push_back(std::move(im));
        }
    } catch (const std::exception& e) {
        Note("film " + key + ": " + e.what());
    }
    return &(films_[key] = std::move(frames));
}

void ShellView::ReadTables(GuestRam& g) {
    tablesRead_ = true;
    // The ids of the second FourCC table, and each .STR resource's run of them.
    std::vector<std::string> ids;
    for (uint32_t i = 0; i < 288u; ++i) {
        const std::string id = FourCC(g.U32(0x800892BCu + 4u * i));
        if (id[0] < '0' || id[0] > 'Z') break;
        ids.push_back(id);
    }
    size_t at = kCompositeIds;
    for (int res : kStrRunOrder) {
        const uint32_t rec = 0x800897E4u + 32u * static_cast<uint32_t>(res);
        char name[17] = {};
        for (uint32_t k = 0; k < 16u; ++k) name[k] = static_cast<char>(g.U8(rec + 8u + k));
        const std::vector<Image>* film = Film(name);
        const size_t n = film ? film->size() : 0;
        for (size_t f = 0; f < n && at < ids.size(); ++f, ++at)
            if (frameOf_.find(ids[at]) == frameOf_.end()) frameOf_[ids[at]] = {name, static_cast<int>(f)};
    }
    // The title of the controller screen is the single frame of TTL_CON2.STR (its sprite record's data
    // pointer is that file's buffer in the capture) - it is not in the table's runs.
    frameOf_["CTLR"] = {"ttl_con2.str", 0};
}

const ShellView::Image* ShellView::Picture(const std::string& id) {
    auto it = pictures_.find(id);
    if (it != pictures_.end()) {
        lastId_ = id;
        return it->second.width > 0 ? &it->second : nullptr;
    }
    Image im;
    if (const Shape* s = shapes_.Find(id.c_str())) {
        im.width = s->width;
        im.height = s->height;
        im.rgba.resize(s->px.size());
        for (size_t i = 0; i < s->px.size(); ++i) im.rgba[i] = Bgr555ToRgba8(s->px[i]);
        im.hdKey = "DATA/FE/FEMISC.PSH:" + id; // HD media (SetScale)
        im.hash = rr::hd::SourceHash15(s->width, s->height, s->px.data());
    } else if (id == "BAC1" || id == "BAC2") {
        // The background window at VRAM (512, 0), 512 x 240: BGRND2.STR's four 256 x 128 frames as
        // BGUL/BGUR/BGLL/BGLR, and BGRND3.STR's picture (B2LR, 304 x 144) at (208, 96) inside it -
        // the sprite records of the capture place all six there.
        im.width = 512;
        im.height = 240;
        im.rgba.assign(512u * 240u, 0xFF000000u);
        auto put = [&](const Image& src, int ox, int oy) {
            for (int y = 0; y < src.height; ++y)
                for (int x = 0; x < src.width; ++x) {
                    const int tx = ox + x, ty = oy + y;
                    if (tx < 0 || ty < 0 || tx >= 512 || ty >= 240) continue;
                    im.rgba[static_cast<size_t>(ty) * 512u + static_cast<size_t>(tx)] =
                        src.rgba[static_cast<size_t>(y) * static_cast<size_t>(src.width) + static_cast<size_t>(x)];
                }
        };
        if (const auto* f2 = Film("bgrnd2.str"); f2 && f2->size() >= 4) {
            put((*f2)[0], 0, 0);
            put((*f2)[1], 256, 0);
            put((*f2)[2], 0, 128);
            put((*f2)[3], 256, 128);
            im.parts = {{"bgrnd2.str", 0, 0, 0}, {"bgrnd2.str", 1, 256, 0}, {"bgrnd2.str", 2, 0, 128}, {"bgrnd2.str", 3, 256, 128}};
        }
        if (const auto* f3 = Film("bgrnd3.str"); f3 && !f3->empty()) {
            put((*f3)[0], 208, 96);
            im.parts.push_back({"bgrnd3.str", 0, 208, 96});
        }
        im.hdKey = "composite:" + id; // HD media: composed from its parts' HD pictures (HdOf)
    } else {
        auto fo = frameOf_.find(id);
        if (fo != frameOf_.end()) {
            const std::vector<Image>* film = Film(fo->second.first);
            if (film && fo->second.second < static_cast<int>(film->size())) im = (*film)[static_cast<size_t>(fo->second.second)];
        }
    }
    if (im.width == 0) Note("picture " + id + " not bound");
    lastId_ = id;
    auto& slot = pictures_[id] = std::move(im);
    return slot.width > 0 ? &slot : nullptr;
}

void ShellView::Blit(std::vector<uint8_t>& out, const Image& im, int x0, int y0, uint32_t rgb, bool modulate, int sx,
                     int sy, int w, int h) {
    if (w < 0) w = im.width - sx;
    if (h < 0) h = im.height - sy;
    if (blitLog_ != nullptr) blitLog_->push_back({lastId_, x0, y0, w, h});
    if (scale_ > 1) return BlitHd(out, im, x0, y0, rgb, modulate, sx, sy, w, h);
    const uint32_t cr = rgb & 0xFFu, cg = (rgb >> 8) & 0xFFu, cb = (rgb >> 16) & 0xFFu;
    for (int y = 0; y < h; ++y) {
        const int ty = y0 + y;
        if (ty < 0 || ty >= kHeight || sy + y >= im.height) continue;
        for (int x = 0; x < w; ++x) {
            const int tx = x0 + x;
            if (tx < 0 || tx >= kWidth || sx + x >= im.width) continue;
            const uint32_t p = im.rgba[static_cast<size_t>(sy + y) * static_cast<size_t>(im.width) + static_cast<size_t>(sx + x)];
            if ((p >> 24) == 0) continue; // PS1 transparent black
            uint32_t r = p & 0xFFu, gg = (p >> 8) & 0xFFu, b = (p >> 16) & 0xFFu;
            if (modulate) { // the GPU's texture blend: colour * texel / 128, saturated
                r = std::min(255u, r * cr / 128u);
                gg = std::min(255u, gg * cg / 128u);
                b = std::min(255u, b * cb / 128u);
            }
            uint8_t* d = &out[(static_cast<size_t>(ty) * kWidth + static_cast<size_t>(tx)) * 4u];
            d[0] = static_cast<uint8_t>(r);
            d[1] = static_cast<uint8_t>(gg);
            d[2] = static_cast<uint8_t>(b);
            d[3] = 255;
        }
    }
}

// ------------------------------------------------------------------------------------------------ HD media

namespace {
const char* const kFontKeys[5] = {"DATA/FE/MINIFONT.PFN", "DATA/FE/BTN_FONT.PFN", "DATA/FE/MINIFONT.PFN",
                                   "DATA/FE/HDR_FONT.PFN", "DATA/GAMEFONT.PFN"};
} // namespace

void ShellView::SetScale(int scale) {
    scale = (scale == 2 || scale == 4) ? scale : 1;
    if (scale == scale_) return;
    scale_ = scale;
    hdCache_.clear();
    hdBytes_ = 0;
    for (int i = 0; i < 5; ++i) hdFonts_[i].reset(), hdFontTried_[i] = false;
}

void ShellView::HdSync() {
    if (hdGeneration_ == rr::hd::Generation()) return;
    hdGeneration_ = rr::hd::Generation();
    hdCache_.clear();
    hdBytes_ = 0;
    for (int i = 0; i < 5; ++i) hdFonts_[i].reset(), hdFontTried_[i] = false;
}

std::string ShellView::HdReport() const {
    char b[160];
    std::snprintf(b, sizeof(b), "shell HD: scale %d, %zu HD pictures loaded, %zu pictures without one, %zu contour fonts", scale_,
                  hdPictures_, hdMissing_, hdFontsUsed_);
    return b;
}

std::shared_ptr<const rr::hd::Rgba> ShellView::HdOf(const Image& im) {
    if (scale_ <= 1 || im.hdKey.empty()) return nullptr;
    HdSync();
    rr::hd::Pack* pack = rr::hd::Active();
    if (pack == nullptr) return nullptr;
    auto it = hdCache_.find(im.hdKey);
    if (it != hdCache_.end()) {
        it->second.used = ++hdUse_;
        return it->second.image;
    }
    std::shared_ptr<rr::hd::Rgba> made;
    if (!im.parts.empty()) { // BAC1 / BAC2: the parts' HD pictures (or their texels enlarged) where the SD one has them
        const int s = scale_;
        made = std::make_shared<rr::hd::Rgba>();
        made->width = im.width * s;
        made->height = im.height * s;
        made->px.assign(static_cast<size_t>(made->width) * static_cast<size_t>(made->height) * 4u, 0);
        for (size_t i = 3; i < made->px.size(); i += 4) made->px[i] = 255;
        for (const Image::Part& part : im.parts) {
            const std::vector<Image>* film = Film(part.film);
            if (film == nullptr || part.frame >= static_cast<int>(film->size())) continue;
            const Image& src = (*film)[static_cast<size_t>(part.frame)];
            const std::shared_ptr<const rr::hd::Rgba> hd = HdOf(src);
            for (int y = 0; y < src.height * s; ++y)
                for (int x = 0; x < src.width * s; ++x) {
                    const int tx = part.x * s + x, ty = part.y * s + y;
                    if (tx < 0 || ty < 0 || tx >= made->width || ty >= made->height) continue;
                    uint8_t* d = &made->px[(static_cast<size_t>(ty) * static_cast<size_t>(made->width) + static_cast<size_t>(tx)) * 4u];
                    if (hd) std::memcpy(d, &hd->px[(static_cast<size_t>(y) * static_cast<size_t>(hd->width) + static_cast<size_t>(x)) * 4u], 4);
                    else {
                        const uint32_t p = src.rgba[static_cast<size_t>(y / s) * static_cast<size_t>(src.width) + static_cast<size_t>(x / s)];
                        d[0] = static_cast<uint8_t>(p), d[1] = static_cast<uint8_t>(p >> 8), d[2] = static_cast<uint8_t>(p >> 16), d[3] = static_cast<uint8_t>(p >> 24);
                    }
                }
        }
    } else {
        rr::hd::Rgba r;
        if (pack->Picture(im.hdKey, im.hash, im.width, im.height, r)) {
            if (scale_ != rr::hd::kScale) r = rr::hd::Reduce(r, rr::hd::kScale / scale_);
            made = std::make_shared<rr::hd::Rgba>(std::move(r));
            ++hdPictures_;
        } else {
            ++hdMissing_;
        }
    }
    // A memory budget for the enlarged copies (the course films alone are 1911 pictures): the least recently drawn go.
    const size_t budget = (scale_ >= 4 ? 384u : 128u) << 20;
    if (made) hdBytes_ += made->px.size();
    while (hdBytes_ > budget && !hdCache_.empty()) {
        auto victim = hdCache_.begin();
        for (auto v = hdCache_.begin(); v != hdCache_.end(); ++v)
            if (v->second.used < victim->second.used) victim = v;
        if (victim->second.image) hdBytes_ -= victim->second.image->px.size();
        hdCache_.erase(victim);
    }
    HdCached& slot = hdCache_[im.hdKey];
    slot.image = made;
    slot.used = ++hdUse_;
    return made;
}

const rr::hd::Contour* ShellView::HdFont(int font) {
    font = (font >= 0 && font < 5) ? font : 1;
    HdSync();
    if (hdFontTried_[font]) return hdFonts_[font].get();
    hdFontTried_[font] = true;
    rr::hd::Pack* pack = rr::hd::Active();
    const Font& f = fonts_[font];
    rr::hd::Contour c;
    if (pack != nullptr && !f.sheet.empty() &&
        pack->Font(kFontKeys[font], rr::hd::SourceHashIndices(f.sheetWidth, f.sheetHeight, f.sheet.data()), f.sheetWidth,
                   f.sheetHeight, c)) {
        hdFonts_[font] = std::make_shared<rr::hd::Contour>(std::move(c));
        ++hdFontsUsed_;
    }
    return hdFonts_[font].get();
}

void ShellView::GlyphTexel(std::vector<uint8_t>& out, int font, int sx, int sy, int tx, int ty, uint32_t rgb, bool stp) {
    const Font& f = fonts_[(font >= 0 && font < 5) ? font : 1];
    const uint32_t cr = rgb & 0xFFu, cg = (rgb >> 8) & 0xFFu, cb = (rgb >> 16) & 0xFFu;
    const auto colour = [&](uint32_t idx, const uint8_t* d, uint32_t* o) {
        const uint32_t grey = ((2u * idx) << 3) | ((2u * idx) >> 2);
        const uint32_t m[3] = {std::min(255u, grey * cr / 128u), std::min(255u, grey * cg / 128u), std::min(255u, grey * cb / 128u)};
        const bool blend = stp && font >= 1 && font <= 3 && idx <= 3u;
        for (int k = 0; k < 3; ++k) o[k] = blend ? (d[k] + m[k]) / 2u : m[k];
    };
    const uint32_t own = f.sheet[static_cast<size_t>(sy) * static_cast<size_t>(f.sheetWidth) + static_cast<size_t>(sx)];
    if (scale_ == 1) {
        if (own == 0) return;
        uint8_t* d = &out[(static_cast<size_t>(ty) * kWidth + static_cast<size_t>(tx)) * 4u];
        uint32_t o[3];
        colour(own, d, o);
        for (int k = 0; k < 3; ++k) d[k] = static_cast<uint8_t>(o[k]);
        d[3] = 255;
        return;
    }
    const rr::hd::Contour* atlas = HdFont(font);
    const int s = scale_, step = rr::hd::kScale / s, w = Width();
    for (int yy = 0; yy < s; ++yy)
        for (int xx = 0; xx < s; ++xx) {
            uint32_t a = own, b = own, weight = 0;
            if (atlas != nullptr) {
                const int ax = sx * rr::hd::kScale + xx * step + step / 2, ay = sy * rr::hd::kScale + yy * step + step / 2;
                const uint8_t* t = &atlas->abw[(static_cast<size_t>(ay) * static_cast<size_t>(atlas->width) + static_cast<size_t>(ax)) * 3u];
                a = t[0], b = t[1], weight = t[2];
            }
            if (a == 0 && (weight == 0 || b == 0)) continue;
            uint8_t* d = &out[(static_cast<size_t>(ty * s + yy) * static_cast<size_t>(w) + static_cast<size_t>(tx * s + xx)) * 4u];
            uint32_t ca[3] = {d[0], d[1], d[2]}, cb2[3] = {d[0], d[1], d[2]};
            if (a != 0) colour(a, d, ca);
            if (weight != 0 && b != 0) colour(b, d, cb2);
            for (int k = 0; k < 3; ++k) d[k] = static_cast<uint8_t>((ca[k] * (255u - weight) + cb2[k] * weight + 127u) / 255u);
            d[3] = 255;
        }
}

void ShellView::BlitHd(std::vector<uint8_t>& out, const Image& im, int x0, int y0, uint32_t rgb, bool modulate, int sx, int sy,
                       int w, int h) {
    const std::shared_ptr<const rr::hd::Rgba> hd = HdOf(im);
    const int s = scale_, W = Width(), H = Height();
    const uint32_t cr = rgb & 0xFFu, cg = (rgb >> 8) & 0xFFu, cb = (rgb >> 16) & 0xFFu;
    for (int y = 0; y < h * s; ++y) {
        const int ty = y0 * s + y, by = sy + y / s;
        if (ty < 0 || ty >= H || by >= im.height) continue;
        for (int x = 0; x < w * s; ++x) {
            const int tx = x0 * s + x, bx = sx + x / s;
            if (tx < 0 || tx >= W || bx >= im.width) continue;
            uint32_t r, gg, b;
            if (hd) {
                const uint8_t* p = &hd->px[(static_cast<size_t>(sy * s + y) * static_cast<size_t>(hd->width) + static_cast<size_t>(sx * s + x)) * 4u];
                if (p[3] < 128) continue;
                r = p[0], gg = p[1], b = p[2];
            } else {
                const uint32_t p = im.rgba[static_cast<size_t>(by) * static_cast<size_t>(im.width) + static_cast<size_t>(bx)];
                if ((p >> 24) == 0) continue;
                r = p & 0xFFu, gg = (p >> 8) & 0xFFu, b = (p >> 16) & 0xFFu;
            }
            if (modulate) {
                r = std::min(255u, r * cr / 128u);
                gg = std::min(255u, gg * cg / 128u);
                b = std::min(255u, b * cb / 128u);
            }
            uint8_t* d = &out[(static_cast<size_t>(ty) * static_cast<size_t>(W) + static_cast<size_t>(tx)) * 4u];
            d[0] = static_cast<uint8_t>(r);
            d[1] = static_cast<uint8_t>(gg);
            d[2] = static_cast<uint8_t>(b);
            d[3] = 255;
        }
    }
}

namespace {
// The 8-bit characters the fonts have no glyph for (codes 32..127 only), as the nearest ASCII.
std::string Printable(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c >= 32 && c < 127) o.push_back(static_cast<char>(c));
        else if (c == 146 || c == 145) o.push_back('\'');
        else if (c == 147 || c == 148) o.push_back('"');
        else if (c == 133) o += "...";
        else if (c == 214) o.push_back('O');
    }
    return o;
}
} // namespace

int ShellView::TextWidth(int font, const std::string& text) {
    const Font& f = fonts_[(font >= 0 && font < 5) ? font : 1];
    int w = 0;
    for (unsigned char c : Printable(text))
        if (const Glyph* gl = f.Find(c)) w += gl->advance + 1;
    return w;
}

// Glyphs from the font's 4bpp sheet: the sheet's indices are a grey ramp (the only 16-entry CLUT
// resident beside the font sheets in the capture is index i -> grey 2i), modulated by the colour the
// widget record carries, as the GPU modulates a textured sprite.
int ShellView::DrawText(std::vector<uint8_t>& out, int font, const std::string& raw, int x, int y, uint32_t rgb,
                        int maxWidth, int maxHeight) {
    const Font& f = fonts_[(font >= 0 && font < 5) ? font : 1];
    const std::string text = Printable(raw);
    const uint32_t cr = rgb & 0xFFu, cg = (rgb >> 8) & 0xFFu, cb = (rgb >> 16) & 0xFFu;
    int penX = x, penY = y, lines = 1, spaces = 0;
    const Glyph* spaceGlyph = f.Find(' ');
    const int spaceAdvance = spaceGlyph ? spaceGlyph->advance + 1 : 0;
    size_t i = 0;
    while (i < text.size()) {
        // Word wrap inside the block's width. OURS, fitted to the capture's main-menu blurb (the
        // original's wrap routine is not read): one pixel between glyphs, a word breaks to the next line
        // when it would end more than 4 pixels past the block's width, and the break takes one of the
        // spaces before the word (a double space leaves one at the start of the new line).
        if (text[i] == ' ') ++spaces;
        if (maxWidth > 0 && text[i] != ' ') {
            size_t j = i;
            while (j < text.size() && text[j] != ' ') ++j;
            const int word = TextWidth(font, text.substr(i, j - i)) - 1;
            if (penX > x && penX + word > x + maxWidth + 4) {
                penX = x + (spaces > 0 ? spaces - 1 : 0) * spaceAdvance;
                penY += f.lineHeight;
                ++lines;
                if (maxHeight > 0 && penY + f.lineHeight > y + maxHeight + 2) break;
            }
            spaces = 0;
        }
        const Glyph* gl = f.Find(static_cast<unsigned char>(text[i]));
        ++i;
        if (!gl) continue;
        for (int gy = 0; gy < gl->h; ++gy)
            for (int gx = 0; gx < gl->w; ++gx) {
                const int sx = gl->x + gx, sy = gl->y + gy;
                if (sx >= f.sheetWidth || sy >= f.sheetHeight) continue;
                const uint32_t idx = f.sheet[static_cast<size_t>(sy) * static_cast<size_t>(f.sheetWidth) + static_cast<size_t>(sx)];
                if (idx == 0 && scale_ == 1) continue;
                const int tx = penX + gl->xoff + gx, ty = penY + gl->yoff + gy;
                if (tx < clip_[0] || ty < clip_[1] || tx >= clip_[2] || ty >= clip_[3]) continue;
                if (scale_ > 1) { // HD media: the font's contour atlas (GlyphTexel)
                    GlyphTexel(out, font, sx, sy, tx, ty, rgb, false);
                    continue;
                }
                const uint32_t grey = ((2u * idx) << 3) | ((2u * idx) >> 2);
                uint8_t* d = &out[(static_cast<size_t>(ty) * kWidth + static_cast<size_t>(tx)) * 4u];
                d[0] = static_cast<uint8_t>(std::min(255u, grey * cr / 128u));
                d[1] = static_cast<uint8_t>(std::min(255u, grey * cg / 128u));
                d[2] = static_cast<uint8_t>(std::min(255u, grey * cb / 128u));
                d[3] = 255;
            }
        penX += gl->advance + 1;
    }
    return lines;
}

// SLUS 0x8002D0D8: a string's width - the sum of its glyphs' advances (a character with no glyph adds 0).
int ShellView::Width(int font, const std::string& text) const {
    const Font& f = fonts_[(font >= 0 && font < 5) ? font : 1];
    int16_t w = 0;
    for (unsigned char c : text)
        if (const Glyph* gl = f.Find(c)) w = static_cast<int16_t>(w + static_cast<int8_t>(gl->advance));
    return w;
}

// SLUS 0x8002CDC8: one textured quad per glyph at (pen + xoff, y + yoff), the pen advanced by the
// glyph's advance; a space moves it by the space glyph's advance (8 without one). The quad's texels
// are the sheet's 4-bit indices through a grey ramp (the only 16-entry CLUT resident beside the font
// sheets in the capture: index i -> grey 2i), modulated by the call's colour as the GPU modulates.
void ShellView::Glyphs(std::vector<uint8_t>& out, int font, const std::string& text, int x, int y, uint32_t rgb) {
    const Font& f = fonts_[(font >= 0 && font < 5) ? font : 1];
    const Glyph* space = f.Find(' ');
    const int spaceAdvance = space ? static_cast<int8_t>(space->advance) : 8;
    const uint32_t cr = rgb & 0xFFu, cg = (rgb >> 8) & 0xFFu, cb = (rgb >> 16) & 0xFFu;
    int pen = x;
    for (unsigned char c : text) {
        if (c == ' ') {
            pen += spaceAdvance;
            continue;
        }
        const Glyph* gl = f.Find(c);
        if (!gl) continue;
        for (int gy = 0; gy < gl->h; ++gy)
            for (int gx = 0; gx < gl->w; ++gx) {
                const int sx = gl->x + gx, sy = gl->y + gy;
                if (sx >= f.sheetWidth || sy >= f.sheetHeight) continue;
                const uint32_t idx = f.sheet[static_cast<size_t>(sy) * static_cast<size_t>(f.sheetWidth) + static_cast<size_t>(sx)];
                if (idx == 0 && scale_ == 1) continue;
                const int tx = pen + gl->xoff + gx, ty = y + gl->yoff + gy;
                if (tx < clip_[0] || ty < clip_[1] || tx >= clip_[2] || ty >= clip_[3]) continue;
                if (scale_ > 1) { // HD media: the font's contour atlas (GlyphTexel)
                    GlyphTexel(out, font, sx, sy, tx, ty, rgb, true);
                    continue;
                }
                const uint32_t grey = ((2u * idx) << 3) | ((2u * idx) >> 2);
                uint8_t* d = &out[(static_cast<size_t>(ty) * kWidth + static_cast<size_t>(tx)) * 4u];
                // The shell fonts' CLUT (VRAM 960, 256 in the capture) sets the STP bit on entries 1..3 and
                // the glyph sprites are semi-transparent, mode 0 (rrverify menuprims): the three darkest
                // edge texels are averaged with what is under them.
                const bool blend = font >= 1 && font <= 3 && idx <= 3u;
                const uint32_t m[3] = {std::min(255u, grey * cr / 128u), std::min(255u, grey * cg / 128u),
                                       std::min(255u, grey * cb / 128u)};
                for (int k = 0; k < 3; ++k) d[k] = static_cast<uint8_t>(blend ? (d[k] + m[k]) / 2u : m[k]);
                d[3] = 255;
            }
        pen += static_cast<int8_t>(gl->advance);
    }
}

// SLUS 0x8002CB08 / 0x8002CC74's justification, and for mode 3 RASHCDF 0x80066318's word wrap, read
// as it is (including its quirks: a word longer than the box is cut at the character that overflows,
// and a line feed only moves the line).
void ShellView::TextLayout(std::vector<uint8_t>& out, const TextCall& t) {
    if (t.sprite != 0) { // the sprite emitter's record: its picture at x, y, modulated by the colour; a
                         // ported AnimSprite's: its frame (sx, sy, sw x sh) of the picture
        if (const Image* im = Picture(FourCC(t.sprite))) {
            if (t.sw > 0) Blit(out, *im, t.x, t.y, t.rgb, true, t.sx, t.sy, t.sw, t.sh);
            else Blit(out, *im, t.x, t.y, t.rgb, true);
        }
        return;
    }
    auto plot = [&](int x, int y, bool semi) {
        if (x < clip_[0] || y < clip_[1] || x >= clip_[2] || y >= clip_[3]) return;
        Block(out, x, y, [&](uint8_t* d) { // one pixel, or its block when scaled (HD media)
            for (int c = 0; c < 3; ++c) {
                const uint32_t f = (t.rgb >> (8 * c)) & 0xFFu;
                d[c] = static_cast<uint8_t>(semi ? (d[c] + f) / 2u : f);
            }
        });
    };
    if (t.tile) { // GP0 0x60..0x63: x..x+w-1, y..y+h-1; semi-transparent as 0.5 B + 0.5 F (the draw mode
                  // the menus' sprites leave, tpage bits 5-6 = 0)
        for (int y = t.y; y < t.y + t.h; ++y)
            for (int x = t.x; x < t.x + t.w; ++x) plot(x, y, t.semi);
        return;
    }
    if (t.box) { // the closed poly-line round x..x+w, y..y+h, both ends of each segment drawn
        for (int x = t.x; x <= t.x + t.w; ++x) plot(x, t.y, t.semi), plot(x, t.y + t.h, t.semi);
        for (int y = t.y + 1; y < t.y + t.h; ++y) plot(t.x, y, t.semi), plot(t.x + t.w, y, t.semi);
        return;
    }
    if (t.clip) return;
    if (t.quad) { // a flat POLY_F4 (the keyboard's highlights) as the GPU splits it (RasterQuad); translucent
                  // as 0.5 B + 0.5 F (the menus' draw mode, tpage bits 5-6 = 0)
        RasterQuad(t.qx, t.qy, [&](int32_t x, int32_t y) {
            if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return;
            Block(out, x, y, [&](uint8_t* d) {
                for (int c = 0; c < 3; ++c) {
                    const uint32_t f = (t.rgb >> (8 * c)) & 0xFFu;
                    d[c] = static_cast<uint8_t>(t.semi ? (d[c] + f) / 2u : f);
                }
            });
        });
        return;
    }
    if (t.panel) { // OURS: the panel emitter's frame as a band of its colour between the two rectangles
        const uint32_t c = t.rgb;
        for (int y = t.y; y < t.h; ++y)
            for (int x = t.x; x < t.w; ++x) {
                if (x >= t.ix0 && x < t.ix1 && y >= t.iy0 && y < t.iy1) continue;
                if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) continue;
                Block(out, x, y, [&](uint8_t* d) {
                    d[0] = static_cast<uint8_t>(c & 0xFFu);
                    d[1] = static_cast<uint8_t>((c >> 8) & 0xFFu);
                    d[2] = static_cast<uint8_t>((c >> 16) & 0xFFu);
                });
            }
        return;
    }
    const int font = t.font;
    const auto s16 = [](int v) { return static_cast<int>(static_cast<int16_t>(v)); };
    switch (t.just) {
    case -1: Glyphs(out, font, t.text, t.x, t.y, t.rgb); return;
    case 0: Glyphs(out, font, t.text, s16(t.x), s16(t.y), t.rgb); return;
    case 1: Glyphs(out, font, t.text, s16(t.x + t.w - Width(font, t.text)), s16(t.y), t.rgb); return;
    case 2: Glyphs(out, font, t.text, s16(t.x + (s16(t.w) >> 1) - (Width(font, t.text) >> 1)), s16(t.y), t.rgb); return;
    case 3: break;
    default: return;
    }
    const Font& f = fonts_[(font >= 0 && font < 5) ? font : 1];
    int step = 0; // the slot's +0x14: the largest glyph height (0x80065CA8)
    for (const Glyph& gl : f.glyphs) step = std::max(step, static_cast<int>(gl.h));
    const Glyph* space = f.Find(' ');
    const int spaceAdvance = space ? static_cast<int8_t>(space->advance) : 8;
    const std::string& str = t.text;
    const int n = static_cast<int>(str.size());
    int lastSpace = 0, word = 0, line = 0, start = 0;
    int y = s16(t.y);
    const int bottom = s16(t.y) + s16(t.h);
    auto draw = [&](int from, int end) {
        if (from < n) Glyphs(out, font, str.substr(static_cast<size_t>(from), static_cast<size_t>(std::max(0, end - from))), s16(t.x), y, t.rgb);
    };
    for (int i = 0; i < n; ++i) {
        const char c = str[static_cast<size_t>(i)];
        if (c == '\n') {
            y += step;
            if (line != 0) start = i + 1;
            if (bottom < s16(y) + step) break;
            continue;
        }
        if (c == ' ') {
            line += spaceAdvance;
            word = 0;
            lastSpace = i;
        } else if (const Glyph* gl = f.Find(static_cast<unsigned char>(c))) {
            line += static_cast<int8_t>(gl->advance);
            word += static_cast<int8_t>(gl->advance);
        }
        if (s16(t.w) < line) {
            if (lastSpace == 0) lastSpace = i;
            // The original terminates the string at lastSpace for the call; a line start past it
            // runs to the string's end.
            draw(start, lastSpace >= start ? lastSpace : n);
            y += step;
            if (bottom < s16(y) + step) return;
            start = lastSpace + 1;
            line = word;
        }
    }
    if (line != 0 && s16(y) + step <= bottom) draw(start, n);
}

// The bound chooser's current option's display list (option +8): mini widget records the screen's
// text blocks and logos take their text and picture from.
uint32_t ShellView::OptionList(GuestRam& g) const {
    // fe+0xA8 keeps the last option after the cursor leaves a chooser; only a bound chooser counts.
    const uint32_t opt = g.U32(kFeChooser) != 0 ? g.U32(kFeOpt) : 0u;
    if (opt == 0) return 0;
    return g.U32(opt + 8u);
}

namespace {
uint32_t Rgb(GuestRam& g, uint32_t a) { return g.U8(a) | (g.U8(a + 1u) << 8) | (g.U8(a + 2u) << 16); }
} // namespace

// A ported widget handler's calls (front_end.cpp ProductCallees) in the order the GPU draws them: the
// ordering table is walked from its last entry down, and an entry's packets from the last linked.
void ShellView::DrawRecorded(std::vector<uint8_t>& out, uint32_t widget) {
    if (texts_ == nullptr) return;
    std::vector<std::pair<size_t, const TextCall*>> mine;
    for (size_t i = 0; i < texts_->size(); ++i) {
        const TextCall& t = (*texts_)[i];
        if (t.widget != widget) continue;
        // The backdrop table 0x8009CCE4 (the product's two buffers at 0x8009D1B0, front_end.cpp) is drawn
        // first, then the films, then the main table (phase_).
        const bool backdrop = t.ot >= 0x8009D1B0u && t.ot < 0x8009D1B0u + 2u * 0x120u;
        if ((phase_ == 1 && !backdrop) || (phase_ == 2 && backdrop)) continue;
        mine.push_back({i, &t});
    }
    std::sort(mine.begin(), mine.end(), [](const auto& a, const auto& b) {
        if (a.second->ot != b.second->ot) return a.second->ot > b.second->ot;
        return a.first > b.first;
    });
    clip_[0] = 0, clip_[1] = 0, clip_[2] = kWidth, clip_[3] = kHeight;
    for (const auto& m : mine) TextLayout(out, *m.second);
}

void ShellView::DrawScreen(GuestRam& g, uint32_t s, uint32_t frame, std::vector<uint8_t>& out, bool current) {
    const uint32_t items = g.U32(s + 16u);
    const int32_t n = g.S16(s + 8u);
    if (items == 0) return;
    const int32_t sel = g.S16(s + 4u);
    const uint32_t mode = g.U32(kSession);
    const int venue = g.S8(kSession + 4u);
    const uint32_t list = OptionList(g);
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t w = items + 120u * static_cast<uint32_t>(i);
        const int16_t type = g.S16(w + 8u);
        const uint16_t flags = g.U16(w + 10u);
        // The draw gate of widget+0x04 (WidgetPass 0x8006D3E0).
        const uint32_t c1 = g.U32(w + 4u);
        if (((c1 & 0x3F00u) >> 8 & (1u << (venue & 31))) == 0) continue;
        if ((c1 & 0x08000000u) && mode != 4u) continue;
        if ((c1 & 0x04000000u) && mode == 4u) continue;
        if ((c1 & 0x10000000u) && g.S8(kSession + 0x1Bu) == 0) continue;
        if ((c1 & 0x80000000u) && !current) continue;
        if ((flags & 0x20u) && !(g.U16(s) & 2u)) continue;
        const bool selected = i == sel;
        if (portedWidgets_ && (type == 7 || type == 9 || type == 11 || type == 12 || type == 13 || type == 14 ||
                               type == 15 || type == 17 || type == 18 ||
                               (portedPanels_ && ((type >= 2 && type <= 6) || type == 8)))) {
            DrawRecorded(out, w); // their PORTED handlers' packets (shell_widgets.h, shell_panel.h)
            continue;
        }
        if (phase_ == 1) continue; // the view's own layout is drawn in the second pass
        switch (type) {
        case 7: { // sprite by FourCC
            if (const Image* im = Picture(FourCC(g.U32(w + 0x10u))))
                Blit(out, *im, g.S16(w + 0x18u), g.S16(w + 0x1Au), Rgb(g, w + 0x14u), true);
            break;
        }
        case 12:
        case 13: { // button / chooser: its sprite, its label, and for a chooser the arrow and the value
            // The record names the idle shape (BTNN / SBTN); the highlighted item is drawn with its 'H'
            // partner (BTNH / SBTH) - the capture shows the selected button wood-grain, the others blue.
            std::string sprite = FourCC(g.U32(w + 0x28u));
            if (selected && sprite.size() == 4 && sprite[3] == 'N') sprite[3] = 'H';
            if (const Image* im = Picture(sprite)) Blit(out, *im, g.S16(w + 0x30u), g.S16(w + 0x32u), Rgb(g, w + 0x2Cu), true);
            const uint32_t colour = Rgb(g, w + (selected ? 0x1Cu : 0x18u));
            DrawText(out, 1, String(g.U16(w + 0x14u)), g.S16(w + 0x24u), g.S16(w + 0x26u), colour);
            if (type == 13) {
                if (const Image* arrow = Picture(FourCC(g.U32(w + 0x34u))))
                    Blit(out, *arrow, g.S16(w + 0x3Cu), g.S16(w + 0x3Eu), Rgb(g, w + 0x38u), true, 0, 0,
                         std::min(arrow->width, 18), std::min(arrow->height, 18));
            }
            break;
        }
        case 9: { // pad-glyph hint: glyph + caption
            if (const Image* im = Picture(FourCC(g.U32(w + 0x10u)))) Blit(out, *im, g.S16(w + 0x18u), g.S16(w + 0x1Au), Rgb(g, w + 0x14u), true);
            DrawText(out, 2, String(g.U16(w + 0x1Cu)), g.S16(w + 0x20u), g.S16(w + 0x22u), Rgb(g, w + 0x28u));
            break;
        }
        case 16: { // text block: what its PORTED handler 0x8006FAC8 asked the text calls for this frame
            if (texts_ != nullptr) {
                // A draw-area primitive clips the widget's texts recorded before it (the ordering table
                // draws the last one added first): the credits' box.
                std::vector<const TextCall*> mine;
                for (const TextCall& t : *texts_)
                    if (t.widget == w) mine.push_back(&t);
                // Drawn in ordering-table order, as DrawRecorded (the jukebox's highlighted "On" is linked
                // above the SBTH button its arm emits after it).
                std::vector<size_t> order;
                for (size_t m = 0; m < mine.size(); ++m)
                    if (!mine[m]->clip) order.push_back(m);
                std::stable_sort(order.begin(), order.end(),
                                 [&](size_t a, size_t b) { return mine[a]->ot > mine[b]->ot; });
                for (const size_t m : order) {
                    clip_[0] = 0, clip_[1] = 0, clip_[2] = kWidth, clip_[3] = kHeight;
                    for (size_t j = m + 1; j < mine.size(); ++j)
                        if (mine[j]->clip) {
                            clip_[0] = std::max(0, mine[j]->x);
                            clip_[1] = std::max(0, mine[j]->y);
                            clip_[2] = std::min(kWidth, mine[j]->x + mine[j]->w);
                            clip_[3] = std::min(kHeight, mine[j]->y + mine[j]->h);
                            break;
                        }
                    TextLayout(out, *mine[m]);
                }
                clip_[0] = 0, clip_[1] = 0, clip_[2] = kWidth, clip_[3] = kHeight;
            }
            break;
        }
        case 8: { // logo, RASHCDF 0x8006E8FC's six kinds 0x30..0x35 (its layout, read from our decompile)
            if ((flags & 0x20u) && !(g.U16(s) & 2u)) break;
            const int kind = g.S16(w + 0x14u);
            const int x = g.S16(w + 0x10u), y = g.S16(w + 0x12u);
            const uint32_t item = static_cast<uint32_t>(g.S16(s + 4u) * 0x78) + g.U32(s + 0x10u);
            uint32_t tag = 0;
            switch (kind) {
            case 0x31: { // the mode's logo; the Trophy Room item's own; hidden when +0x16 & 0x1000 unless
                         // the highlighted item carries 0x100
                if ((g.U16(w + 0x16u) & 0x1000u) && !(g.U16(item + 10u) & 0x100u)) break;
                if (ActionGroup(g, item) == 2) { tag = 0x474C5954u; break; }
                switch (mode) {
                case 1: case 0x11: tag = 0x474C4F35u; break;
                case 4: tag = 0x474C5454u; break;
                case 8: case 0x18: tag = 0x474C4353u; break;
                case 0x10: tag = 0x474C4848u; break;
                case 0x20:
                    tag = venue == 1 ? 0x504C5447u : venue == 3 ? 0x504C5444u : venue == 5 ? 0x504C424Au : 0x474C424Au;
                    break;
                default: break;
                }
                break;
            }
            case 0x32: { // 0x80075BD0: the controller diagram of the edited port's layout, else BAC1
                const int32_t grp = ActionGroup(g, item);
                if (grp != 8 && grp != -1) { tag = 0x31434142u; break; }
                static const uint32_t kPad[] = {0x31304744u, 0x32304744u, 0x33304744u, 0x31304E41u, 0x32304E41u};
                const uint32_t layout = g.U8(0x800D81E0u + 0x24u * static_cast<uint32_t>(g.S8(kFePort)));
                tag = layout < 5u ? kPad[layout] : kPad[0];
                break;
            }
            case 0x33: tag = ActionGroup(g, item) == 3 ? 0x474C4E43u : 0x474C5252u; break;
            case 0x34: // the result panel's picture and the outcome (fe+0xAC / +0xB0, CareerResult)
            case 0x35: tag = g.U32(kFe + (kind == 0x34 ? 0xACu : 0xB0u)); if (tag == 300u) tag = 0; break;
            default: break;
            }
            if (kind == 0x30) { // the option display list's picture (0x80064154 over the chooser)
                for (uint32_t e = list; e != 0 && g.U16(e) != 0 && e < list + 28u * 16u; e += 28u)
                    if (g.U16(e) == 8 && g.U16(e + 2u) == static_cast<uint32_t>(kind)) {
                        tag = g.U32(e + 4u);
                        break;
                    }
            }
            if (tag != 0) {
                if (const Image* im = Picture(FourCC(tag))) Blit(out, *im, x, y, 0x808080u, true);
            }
            break;
        }
        case 3: { // animated picture: a .STR from the name array, looped (types 2, 4, 5, 6 not decoded)
            const uint32_t idx = g.U32(w + 0x10u);
            if (idx < 42u) {
                std::string name;
                const uint32_t p = g.U32(0x8008973Cu + 4u * idx);
                for (uint32_t k = 0; k < 16u; ++k) {
                    const char c = static_cast<char>(g.U8(p + k));
                    if (c == 0) break;
                    name.push_back(c);
                }
                if (name.find('.') == std::string::npos) name += ".str";
                if (const auto* film = Film(name); film && !film->empty())
                    Blit(out, (*film)[(frame / 4u) % film->size()], g.S16(w + 0x1Cu), g.S16(w + 0x1Eu), 0x808080u, false);
            }
            break;
        }
        case 17: { // slider: the bound value of session+0xD4 as a bar (ours: the drawing)
            const uint32_t slot = g.U8(w + 0x66u);
            const int32_t v = g.S32(0x800D81ACu + 4u * slot);
            const int x = g.S16(w + 0x24u) + 150, y = g.S16(w + 0x26u) + 2;
            for (int k = 0; k < 16; ++k)
                for (int yy = 0; yy < 8; ++yy)
                    for (int xx = 0; xx < 5; ++xx) {
                        const int tx = x + 7 * k + xx, ty = y + yy;
                        if (tx < 0 || ty < 0 || tx >= kWidth || ty >= kHeight) continue;
                        const bool on = k < v * 16 / 8;
                        Block(out, tx, ty, [&](uint8_t* d) {
                            d[0] = on ? 0xE0 : 0x30;
                            d[1] = on ? 0x90 : 0x30;
                            d[2] = on ? 0x30 : 0x40;
                            d[3] = 255;
                        });
                    }
            DrawText(out, 1, String(g.U16(w + 0x14u)), g.S16(w + 0x24u), g.S16(w + 0x26u), Rgb(g, w + (selected ? 0x1Cu : 0x18u)));
            break;
        }
        case 18: { // scrolling ticker line
            const int x = g.S16(w + 0x10u), y = g.S16(w + 0x12u), bw = g.S16(w + 0x14u);
            const std::string t = String(g.U16(w + 0x20u));
            const int total = TextWidth(2, t) + bw;
            const int off = total > 0 ? static_cast<int>(frame % static_cast<uint32_t>(total)) : 0;
            std::vector<uint8_t> line(out);
            DrawText(line, 2, t, x + bw - off, y, Rgb(g, w + 0x18u));
            for (int yy = y - 2; yy < y + 14; ++yy)
                for (int xx = x; xx < x + bw; ++xx) {
                    if (xx < 0 || yy < 0 || xx >= kWidth || yy >= kHeight) continue;
                    for (int sy2 = 0; sy2 < scale_; ++sy2) { // the block when scaled (HD media)
                        const size_t o = (static_cast<size_t>(yy * scale_ + sy2) * static_cast<size_t>(Width()) +
                                          static_cast<size_t>(xx * scale_)) * 4u;
                        std::memcpy(&out[o], &line[o], 4u * static_cast<size_t>(scale_));
                    }
                }
            break;
        }
        default:
            break;
        }
    }
}

// The race's results scene (race_results.h) in the shell's 512 x 240 picture: the race's 384-wide draw
// area centred (x + 64). OURS: behind it the shell's backdrop at half brightness and the scene's box
// (RASHCDG 0x800400F4's tile at 0x39, 0x24, 0x146 x 0xCC) darkened again - the original draws the
// panel over the frozen race picture, which the front end's window does not have.
void ShellView::DrawResults(GuestRam& g, const std::vector<TextCall>& texts, std::vector<uint8_t>& rgba) {
    if (!tablesRead_) ReadTables(g);
    rgba.assign(static_cast<size_t>(Width()) * static_cast<size_t>(Height()) * 4u, 0);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    if (const Image* bg = Picture("BAC1")) Blit(rgba, *bg, 0, 0, 0x404040u, true);
    constexpr int kLeft = (kWidth - 384) / 2;
    for (int y = 0x24; y < 0x24 + 0xCC && y < kHeight; ++y)
        for (int x = kLeft + 0x39; x < kLeft + 0x39 + 0x146 && x < kWidth; ++x)
            Block(rgba, x, y, [](uint8_t* d) {
                d[0] = static_cast<uint8_t>(d[0] / 3);
                d[1] = static_cast<uint8_t>(d[1] / 3);
                d[2] = static_cast<uint8_t>(d[2] / 3);
            });
    for (TextCall t : texts) {
        t.x += kLeft;
        TextLayout(rgba, t);
    }
}

// RASHCDF 0x8006E008 (screen 57, the race's start): the loading picture - DATA\FE\ssload.tcm when the
// mode word has bit 0x10 (entry race - 1, or race - 20 from race 38 on), else DATA\FE\fsload.tcm (entry
// race - 1, or session+0x0F - 1 on the attract path); the entry is one 384 x 240 MDEC chunk of the
// .TCM's table (video.md 1.3), decoded with the EXE's code book and drawn centred.
void ShellView::LoadingPicture(GuestRam& g, std::vector<uint8_t>& rgba) {
    const bool split = (g.U32(kSession) & 0x10u) != 0;
    int32_t idx;
    if (split) {
        idx = g.S8(kSession + 8u);
        if (idx >= 38) idx -= 19;
        idx -= 1;
    } else {
        idx = (g.U32(kDemo) != 0 ? g.S8(kSession + 0x0Fu) : g.S8(kSession + 8u)) - 1;
    }
    const std::string key = std::string(split ? "SSLOAD.TCM#" : "FSLOAD.TCM#") + std::to_string(idx);
    auto it = pictures_.find(key);
    if (it == pictures_.end()) {
        Image im;
        try {
            const std::vector<uint8_t> f = Read(disc_, split ? "DATA/FE/SSLOAD.TCM" : "DATA/FE/FSLOAD.TCM");
            auto le = [&](size_t at) {
                return at + 4 <= f.size() ? static_cast<uint32_t>(f[at]) | (static_cast<uint32_t>(f[at + 1]) << 8) |
                                                (static_cast<uint32_t>(f[at + 2]) << 16) | (static_cast<uint32_t>(f[at + 3]) << 24)
                                          : 0u;
            };
            const uint32_t count = le(0);
            if (count <= 100u && idx >= 0 && static_cast<uint32_t>(idx) < count) {
                const uint32_t size = le(4u + 8u * static_cast<uint32_t>(idx)), off = le(8u + 8u * static_cast<uint32_t>(idx));
                if (off + size <= f.size()) {
                    const std::vector<Picture15> frames = DecodeStrFrames(std::span<const uint8_t>(f.data() + off, size), book_);
                    if (!frames.empty()) {
                        im.width = frames[0].width;
                        im.height = frames[0].height;
                        im.hdKey = std::string("DATA/FE/") + key; // HD media (SetScale)
                        im.hash = rr::hd::SourceHash15(frames[0].width, frames[0].height, frames[0].px.data());
                        im.rgba.resize(frames[0].px.size());
                        for (size_t i = 0; i < frames[0].px.size(); ++i) im.rgba[i] = Bgr555ToRgba8(frames[0].px[i]) | 0xFF000000u;
                    }
                }
            }
        } catch (const std::exception& e) {
            Note(std::string("loading picture: ") + e.what());
        }
        if (im.width == 0) Note("loading picture " + key + " not decoded");
        it = pictures_.emplace(key, std::move(im)).first;
    }
    rgba.assign(static_cast<size_t>(Width()) * static_cast<size_t>(Height()) * 4u, 0);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    if (it->second.width > 0) Blit(rgba, it->second, (kWidth - it->second.width) / 2, (kHeight - it->second.height) / 2, 0x808080u, false);
}

void ShellView::Draw(GuestRam& g, uint32_t frame, std::vector<uint8_t>& rgba, const std::vector<TextCall>* texts) {
    texts_ = texts;
    if (!tablesRead_) ReadTables(g);
    rgba.assign(static_cast<size_t>(Width()) * static_cast<size_t>(Height()) * 4u, 0);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    // Screen 57 has no record: it is the race's start, and its handler 0x8006E008 shows the loading
    // picture while the race loads (LoadingPicture).
    if (g.S16(kFeCur) == 57) {
        LoadingPicture(g, rgba);
        return;
    }
    // The current screen and its parent chain, the deepest parent first (a modal draws over its caller).
    std::vector<uint32_t> chain;
    for (int32_t id = g.S16(kFeCur); id >= 0 && id < kScreenCount && chain.size() < 8;) {
        const uint32_t s = g.U32(kScreenTable + 4u * static_cast<uint32_t>(id));
        if (s == 0) break;
        chain.push_back(s);
        id = g.S16(s + 10u);
    }
    if (!portedWidgets_) {
        phase_ = 0;
        for (size_t k = chain.size(); k-- > 0;) DrawScreen(g, chain[k], frame, rgba, k == 0);
        DrawFilms(rgba);
        return;
    }
    // The frame 0x80080274: DrawOTag of the backdrop table, the films' pictures copied into VRAM
    // (0x80062774(0x8009C3D0)), then the main table (0x800803FC).
    phase_ = 1;
    for (size_t k = chain.size(); k-- > 0;) DrawScreen(g, chain[k], frame, rgba, k == 0);
    DrawFilms(rgba);
    phase_ = 2;
    for (size_t k = chain.size(); k-- > 0;) DrawScreen(g, chain[k], frame, rgba, k == 0);
    phase_ = 0;
}

// A film's picture is copied into the frame buffer as it is: opaque, unmodulated (MDEC's 15-bit output).
void ShellView::DrawFilms(std::vector<uint8_t>& out) {
    if (filmShown_ == nullptr) return;
    for (const FilmShown& f : *filmShown_) {
        const std::vector<Image>* film = Film(f.name);
        if (film == nullptr || f.picture < 0 || f.picture >= static_cast<int>(film->size())) continue;
        const Image& im = (*film)[static_cast<size_t>(f.picture)];
        if (blitLog_ != nullptr) blitLog_->push_back({lastId_, f.x, f.y, im.width, im.height});
        if (scale_ > 1) { // HD media: every film texel is opaque (MDEC), so an unmodulated blit is this copy
            BlitHd(out, im, f.x, f.y, 0x808080u, false, 0, 0, im.width, im.height);
            continue;
        }
        for (int y = 0; y < im.height; ++y) {
            const int ty = f.y + y;
            if (ty < 0 || ty >= kHeight) continue;
            for (int x = 0; x < im.width; ++x) {
                const int tx = f.x + x;
                if (tx < 0 || tx >= kWidth) continue;
                const uint32_t p = im.rgba[static_cast<size_t>(y) * static_cast<size_t>(im.width) + static_cast<size_t>(x)];
                uint8_t* d = &out[(static_cast<size_t>(ty) * kWidth + static_cast<size_t>(tx)) * 4u];
                d[0] = static_cast<uint8_t>(p & 0xFFu);
                d[1] = static_cast<uint8_t>((p >> 8) & 0xFFu);
                d[2] = static_cast<uint8_t>((p >> 16) & 0xFFu);
                d[3] = 255;
            }
        }
    }
}

} // namespace rr::shell
