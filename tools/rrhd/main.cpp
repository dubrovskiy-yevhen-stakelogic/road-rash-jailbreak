// rrhd - the OFFLINE preparation of the optional HD media pack (docs\HD-MEDIA.md), driven by scripts\prepare-hd.ps1.
// Everything it reads comes from the player's own disc; everything it writes goes to the work folder the script names
// (the install's runtime\.hd-work, or work\hd in development) and, finished, to the install's runtime\hd. Nothing of it
// belongs in the source tree.
//
//   rrhd extract <disc> <work> [--movies]     the originals: pictures (neural input), font and HUD contour atlases
//                                             (finished here: xBR on palette indices, hd_contours.h), film pictures
//   rrhd compose <work> <upscaled>            the neural pictures + the originals' transparency -> stage\pictures
//   rrhd pack-movie <work> <NAME> <upscaled>  a film's neural pictures, bounded and stabilised (hd_video_filter.h)
//   rrhd finish <work>                        stage\index.txt and stage\profile.txt: the pack, ready to publish
//   rrhd verify <disc> <pack>                 every entry against the disc: source hashes, files, sizes
//   rrhd compare-film <disc> <pack> <NAME> <picture> <out.png>   original (enlarged) | HD, side by side
//   rrhd sidebyside <a.png> <b.png> <out.png> [crop x y w h]      two pictures side by side (a crop of each)
//   rrhd selftest <dir>                       the pack format and loader on synthetic data (no disc)
//   rrhd synth-pack <disc> <dir> [--wrong-hash|--wrong-profile]  a pack of SYNTHETIC pictures keyed to this disc's
//                                             menu background and button font, for the loader's gates
#include "game/hud_arena.h"
#include "game/shell/shell_assets.h"
#include "rrformats/hd_contours.h"
#include "rrformats/hd_pack.h"
#include "rrformats/hd_video_filter.h"
#include "rrvfs/disc_identity.h"
#include "rrvfs/disc_image.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using rr::hd::Rgba;

namespace {

std::string Upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

std::string ExeSha1(const rr::DiscImage& disc) {
    for (const rr::DiscPart& p : rr::IdentifyDisc(disc).parts)
        if (p.name == "SLUS_010.53") return p.sha1;
    return {};
}

std::vector<uint8_t> Read(const rr::DiscImage& disc, const std::string& path) {
    const auto f = disc.Find(path);
    if (!f) throw std::runtime_error("the disc has no " + path);
    return disc.ReadFile(*f);
}

std::string Path(const rr::DiscFile& f) { return f.path.size() > 1 && f.path[0] == '/' ? f.path.substr(1) : f.path; }
bool Ends(const std::string& s, const char* ext) {
    const size_t n = std::strlen(ext);
    return s.size() >= n && Upper(s.substr(s.size() - n)) == ext;
}

Rgba FromRgba32(int w, int h, const std::vector<uint32_t>& px) {
    Rgba r;
    r.width = w;
    r.height = h;
    r.px.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    for (size_t i = 0; i < px.size(); ++i) {
        r.px[4 * i] = static_cast<uint8_t>(px[i]);
        r.px[4 * i + 1] = static_cast<uint8_t>(px[i] >> 8);
        r.px[4 * i + 2] = static_cast<uint8_t>(px[i] >> 16);
        r.px[4 * i + 3] = static_cast<uint8_t>(px[i] >> 24);
    }
    return r;
}

// The neural network's input: RGB, the transparent texels filled from their opaque neighbours (so the enlargement
// draws no dark halo into the edge the original transparency later cuts).
Rgba NeuralInput(const Rgba& in) {
    Rgba r = in;
    std::vector<uint8_t> known(static_cast<size_t>(in.width) * static_cast<size_t>(in.height));
    for (size_t i = 0; i < known.size(); ++i) known[i] = in.px[4 * i + 3] >= 128;
    for (int pass = 0; pass < 16; ++pass) {
        bool any = false;
        std::vector<uint8_t> next = known;
        for (int y = 0; y < in.height; ++y)
            for (int x = 0; x < in.width; ++x) {
                const size_t i = static_cast<size_t>(y) * static_cast<size_t>(in.width) + static_cast<size_t>(x);
                if (known[i]) continue;
                int sum[3] = {0, 0, 0}, n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= in.width || yy >= in.height) continue;
                        const size_t j = static_cast<size_t>(yy) * static_cast<size_t>(in.width) + static_cast<size_t>(xx);
                        if (!known[j]) continue;
                        for (int c = 0; c < 3; ++c) sum[c] += r.px[4 * j + static_cast<size_t>(c)];
                        ++n;
                    }
                if (n == 0) continue;
                for (int c = 0; c < 3; ++c) r.px[4 * i + static_cast<size_t>(c)] = static_cast<uint8_t>(sum[c] / n);
                next[i] = 1;
                any = true;
            }
        known.swap(next);
        if (!any) break;
    }
    for (size_t i = 0; i < known.size(); ++i) r.px[4 * i + 3] = 255;
    return r;
}

// The network's picture for input `name`: PNG, or JPEG when the script asked the upscaler for JPEG (-Intermediate jpg).
fs::path Neural(const fs::path& dir, const std::string& name) {
    const fs::path png = dir / name;
    if (fs::exists(png)) return png;
    return dir / (fs::path(name).stem().string() + ".jpg");
}

// The network's input framed by kPad texels of its own edges (repeated), and at least 64 x 64: the network then sees
// context at the picture's border, and very small sprites (the 10-pixel pad glyphs, the 18-pixel buttons) no longer
// come back as noise, which realesrgan-ncnn-vulkan 0.2.5.0 returned for them unpadded. Compose crops the frame off.
constexpr int kPad = 8;
constexpr const char* kInput = "input-pad8";
Rgba Crop(const Rgba& in, int x0, int y0, int w, int h);
Rgba Pad(const Rgba& in, int& pw, int& ph) {
    pw = std::max(in.width + 2 * kPad, 64);
    ph = std::max(in.height + 2 * kPad, 64);
    Rgba r;
    r.width = pw;
    r.height = ph;
    r.px.resize(static_cast<size_t>(pw) * static_cast<size_t>(ph) * 4u);
    for (int y = 0; y < ph; ++y)
        for (int x = 0; x < pw; ++x) {
            const int sx = std::clamp(x - kPad, 0, in.width - 1), sy = std::clamp(y - kPad, 0, in.height - 1);
            std::memcpy(&r.px[(static_cast<size_t>(y) * static_cast<size_t>(pw) + static_cast<size_t>(x)) * 4u],
                        &in.px[(static_cast<size_t>(sy) * static_cast<size_t>(in.width) + static_cast<size_t>(sx)) * 4u], 4);
        }
    return r;
}

// Edge-directed 2x of a binary mask (EPX / Scale2x, as GT2's ScaleUi2x), twice: the pictures' transparency at 4x.
std::vector<uint8_t> Epx2(const std::vector<uint8_t>& src, int w, int h) {
    std::vector<uint8_t> dst(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    const auto at = [&](int x, int y) {
        return src[static_cast<size_t>(std::clamp(y, 0, h - 1)) * static_cast<size_t>(w) + static_cast<size_t>(std::clamp(x, 0, w - 1))];
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t b = at(x, y - 1), d = at(x - 1, y), e = at(x, y), f = at(x + 1, y), hh = at(x, y + 1);
            const size_t p = static_cast<size_t>(y * 2) * static_cast<size_t>(w * 2) + static_cast<size_t>(x * 2);
            const bool edge = b != hh && d != f;
            dst[p] = edge && d == b ? d : e;
            dst[p + 1] = edge && b == f ? f : e;
            dst[p + static_cast<size_t>(w * 2)] = edge && d == hh ? d : e;
            dst[p + static_cast<size_t>(w * 2) + 1] = edge && hh == f ? f : e;
        }
    return dst;
}

// ------------------------------------------------------------------------------------------------ the sources

struct Picture {
    std::string key;
    uint64_t hash = 0;
    Rgba image; // the original as the game draws it (RGBA, alpha 0 = transparent)
    bool alpha = false;
};

// Every picture of the front end, decoded as ShellView decodes it (shell_view.cpp Film / Picture / LoadingPicture).
std::vector<Picture> Pictures(const rr::DiscImage& disc, std::string& log) {
    const rr::shell::MdecCodebook book = rr::shell::LoadDefaultCodebook(Read(disc, "SLUS_010.53"));
    std::vector<Picture> out;
    size_t strs = 0, tcms = 0;
    for (const rr::DiscFile& f : disc.Files()) {
        const std::string path = Upper(Path(f));
        if (path.rfind("DATA/FE/", 0) != 0) continue;
        if (Ends(path, ".STR")) {
            const std::vector<uint8_t> file = disc.ReadFile(f);
            const std::vector<rr::shell::Picture15> frames = rr::shell::DecodeStrFrames(file, book);
            for (size_t i = 0; i < frames.size(); ++i) {
                const auto& p = frames[i];
                Picture pic;
                pic.key = path + "#" + std::to_string(i);
                pic.hash = rr::hd::SourceHash15(p.width, p.height, p.px.data());
                std::vector<uint32_t> px(p.px.size());
                for (size_t k = 0; k < px.size(); ++k) px[k] = rr::shell::Bgr555ToRgba8(static_cast<uint16_t>(p.px[k] | 0x8000u));
                pic.image = FromRgba32(p.width, p.height, px);
                out.push_back(std::move(pic));
            }
            ++strs;
        } else if (Ends(path, ".TCM")) {
            const std::vector<uint8_t> file = disc.ReadFile(f);
            const auto le = [&](size_t at) {
                return at + 4 <= file.size() ? static_cast<uint32_t>(file[at]) | (static_cast<uint32_t>(file[at + 1]) << 8) |
                                                   (static_cast<uint32_t>(file[at + 2]) << 16) | (static_cast<uint32_t>(file[at + 3]) << 24)
                                             : 0u;
            };
            const uint32_t count = le(0);
            for (uint32_t idx = 0; idx < count && count <= 100u; ++idx) {
                const uint32_t size = le(4u + 8u * idx), off = le(8u + 8u * idx);
                if (off + size > file.size()) continue;
                const auto frames = rr::shell::DecodeStrFrames(std::span<const uint8_t>(file.data() + off, size), book);
                if (frames.empty()) continue;
                const auto& p = frames[0];
                Picture pic;
                pic.key = path + "#" + std::to_string(idx);
                pic.hash = rr::hd::SourceHash15(p.width, p.height, p.px.data());
                std::vector<uint32_t> px(p.px.size());
                for (size_t k = 0; k < px.size(); ++k) px[k] = rr::shell::Bgr555ToRgba8(p.px[k]) | 0xFF000000u;
                pic.image = FromRgba32(p.width, p.height, px);
                out.push_back(std::move(pic));
            }
            ++tcms;
        }
    }
    const rr::shell::ShapeBank bank = rr::shell::ParseShapeBank(Read(disc, "DATA/FE/FEMISC.PSH"));
    size_t shapes = 0, skipped = 0;
    for (const rr::shell::Shape& s : bank.shapes) {
        const std::string id(s.name, 4);
        bool printable = s.width > 0 && s.height > 0;
        for (char c : id) printable = printable && c > ' ' && c < 127;
        if (!printable) {
            ++skipped;
            continue;
        }
        Picture pic;
        pic.key = "DATA/FE/FEMISC.PSH:" + id;
        pic.hash = rr::hd::SourceHash15(s.width, s.height, s.px.data());
        std::vector<uint32_t> px(s.px.size());
        for (size_t k = 0; k < px.size(); ++k) {
            px[k] = rr::shell::Bgr555ToRgba8(s.px[k]);
            pic.alpha = pic.alpha || (px[k] >> 24) == 0;
        }
        pic.image = FromRgba32(s.width, s.height, px);
        out.push_back(std::move(pic));
        ++shapes;
    }
    char b[200];
    std::snprintf(b, sizeof(b), "pictures: %zu (%zu .STR files, %zu .TCM, %zu FEMISC.PSH shapes, %zu shapes without a printable id)\n",
                  out.size(), strs, tcms, shapes, skipped);
    log += b;
    return out;
}

struct FontSource {
    const char* key;
    const char* file;
};
const FontSource kFonts[] = {{"DATA/FE/BTN_FONT.PFN", "DATA/FE/BTN_FONT.PFN"},
                             {"DATA/FE/MINIFONT.PFN", "DATA/FE/MINIFONT.PFN"},
                             {"DATA/FE/HDR_FONT.PFN", "DATA/FE/HDR_FONT.PFN"},
                             {"DATA/GAMEFONT.PFN", "DATA/GAMEFONT.PFN"}};

std::array<uint16_t, 16> GreyRamp() { // the shell fonts' CLUT: index i -> grey 2i, 0 transparent (shell_view.cpp Glyphs)
    std::array<uint16_t, 16> p{};
    for (int i = 1; i < 16; ++i) {
        const uint16_t g = static_cast<uint16_t>(2 * i);
        p[static_cast<size_t>(i)] = static_cast<uint16_t>(g | (g << 5) | (g << 10));
    }
    return p;
}

rr::hd::Contour FontAtlas(const rr::shell::Font& f) {
    rr::hd::Contour c;
    c.width = f.sheetWidth * 4;
    c.height = f.sheetHeight * 4;
    c.abw.assign(static_cast<size_t>(c.width) * static_cast<size_t>(c.height) * 3u, 0);
    rr::hd::NearestContours4x(f.sheet.data(), f.sheetWidth, 0, 0, f.sheetWidth, f.sheetHeight, c.abw.data(), c.width, 0, 0);
    const auto ramp = GreyRamp();
    for (const rr::shell::Glyph& g : f.glyphs) {
        const int w = std::min<int>(g.w, f.sheetWidth - g.x), h = std::min<int>(g.h, f.sheetHeight - g.y);
        if (w <= 0 || h <= 0) continue;
        rr::hd::SmoothContours4x(f.sheet.data(), f.sheetWidth, f.sheetHeight, g.x, g.y, w, h, ramp, true, c.abw.data(), c.width,
                                 g.x * 4, g.y * 4);
    }
    return c;
}

struct HudSource {
    rr::game::HudRegion region;
    uint64_t hash = 0;
    rr::hd::Contour atlas;
};

// Every HUD region of the four layouts (one player; two players in the three split modes), deduplicated by source.
std::vector<HudSource> HudRegions(const rr::DiscImage& disc, std::string& log, bool atlases) {
    std::vector<HudSource> out;
    std::set<uint64_t> seen;
    const rr::shell::Font gamefont = rr::shell::ParseFont(Read(disc, "DATA/GAMEFONT.PFN"));
    const int layouts[4][2] = {{1, -1}, {2, 0}, {2, 1}, {2, 2}};
    for (const auto& l : layouts) {
        rr::game::HudVram vram;
        std::vector<rr::game::HudRegion> regions;
        std::string report;
        if (!rr::game::BuildHudPageForHd(disc, l[0], l[1], vram, regions, report)) {
            log += "HUD layout " + std::to_string(l[0]) + "P/" + std::to_string(l[1]) + ": " + report;
            continue;
        }
        log += report;
        std::vector<uint8_t> indices(256u * 256u);
        for (int y = 0; y < 256; ++y)
            for (int x = 0; x < 256; ++x)
                indices[static_cast<size_t>(y) * 256u + static_cast<size_t>(x)] =
                    static_cast<uint8_t>((vram.page[static_cast<size_t>(y) * 64u + static_cast<size_t>(x / 4)] >> ((x & 3) * 4)) & 15u);
        for (const rr::game::HudRegion& r : regions) {
            const uint64_t hash = rr::hd::SourceHashHudRegion(vram.page.data(), r.u, r.v, r.w, r.h, r.clut);
            if (!seen.insert(hash).second) continue;
            HudSource s;
            s.region = r;
            s.hash = hash;
            if (atlases) {
                std::array<uint16_t, 16> palette{};
                const int cx = static_cast<int>(r.clut & 0x3Fu) * 16, cy = static_cast<int>((r.clut >> 6) & 0x1FFu);
                for (int k = 0; k < 16; ++k) palette[static_cast<size_t>(k)] = vram.At(cx + k, cy);
                s.atlas.width = r.w * 4;
                s.atlas.height = r.h * 4;
                s.atlas.abw.assign(static_cast<size_t>(s.atlas.width) * static_cast<size_t>(s.atlas.height) * 3u, 0);
                rr::hd::NearestContours4x(indices.data(), 256, r.u, r.v, r.w, r.h, s.atlas.abw.data(), s.atlas.width, 0, 0);
                if (r.font) { // glyph by glyph (the font's own table), transparent around each
                    for (const rr::shell::Glyph& g : gamefont.glyphs) {
                        const int gx = r.u + g.x, gy = r.v + g.y;
                        const int w = std::min<int>(g.w, r.u + r.w - gx), h = std::min<int>(g.h, r.v + r.h - gy);
                        if (w <= 0 || h <= 0) continue;
                        rr::hd::SmoothContours4x(indices.data(), 256, 256, gx, gy, w, h, palette, true, s.atlas.abw.data(),
                                                 s.atlas.width, g.x * 4, g.y * 4);
                    }
                } else {
                    rr::hd::SmoothContours4x(indices.data(), 256, 256, r.u, r.v, r.w, r.h, palette, false, s.atlas.abw.data(),
                                             s.atlas.width, 0, 0);
                }
            }
            out.push_back(std::move(s));
        }
    }
    return out;
}

struct Film {
    std::string key, name;
    uint64_t hash = 0;
    int frames = 0, width = 0, height = 0;
};

// Every film of the front end as MoviePlayer decodes it (movie_player.cpp): `each` gets each picture (RGBA) when set.
std::vector<Film> Films(const rr::DiscImage& disc, const std::function<void(const Film&, int, const Rgba&)>& each) {
    const rr::shell::MdecCodebook exe = rr::shell::LoadDefaultCodebook(Read(disc, "SLUS_010.53"));
    std::vector<Film> out;
    for (const rr::DiscFile& f : disc.Files()) {
        const std::string path = Upper(Path(f));
        if (!Ends(path, ".WVE") || path.rfind("DATA/", 0) != 0) continue;
        const std::vector<uint8_t> file = disc.ReadFile(f);
        const auto chunks = rr::shell::ListStrChunks(file);
        const rr::shell::MdecCodebook book = rr::shell::CodebookForStream(file, exe);
        Film film;
        film.key = path;
        film.name = fs::path(path).stem().string();
        film.hash = rr::hd::SourceHashBytes(file);
        film.frames = static_cast<int>(chunks.size());
        for (size_t i = 0; i < chunks.size(); ++i) {
            const rr::shell::Picture15 p = rr::shell::DecodeMdecChunk(chunks[i], book);
            film.width = p.width;
            film.height = p.height;
            if (!each) continue;
            Rgba r;
            r.width = p.width;
            r.height = p.height;
            r.px.resize(static_cast<size_t>(p.width) * static_cast<size_t>(p.height) * 4u);
            for (size_t k = 0; k < p.px.size(); ++k) {
                const uint16_t v = p.px[k];
                const uint32_t cr = v & 31u, cg = (v >> 5) & 31u, cb = (v >> 10) & 31u;
                r.px[4 * k] = static_cast<uint8_t>((cr << 3) | (cr >> 2));
                r.px[4 * k + 1] = static_cast<uint8_t>((cg << 3) | (cg >> 2));
                r.px[4 * k + 2] = static_cast<uint8_t>((cb << 3) | (cb >> 2));
                r.px[4 * k + 3] = 255;
            }
            each(film, static_cast<int>(i), r);
        }
        out.push_back(film);
    }
    return out;
}

std::string Name(const char* prefix, size_t i, const char* ext) {
    char b[32];
    std::snprintf(b, sizeof(b), "%s%05zu%s", prefix, i, ext);
    return b;
}

void WriteText(const fs::path& path, const std::string& text) { // a new file renamed over the old (hard links, WriteBytes)
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        f << text;
        if (!f) throw std::runtime_error("cannot write " + path.string());
    }
    fs::rename(temp, path);
}

std::string IndexLine(const std::string& kind, const std::string& key, uint64_t hash, int a, int b, int c, int d, int e,
                      const std::string& file) {
    char n[96];
    std::snprintf(n, sizeof(n), " %d %d %d %d %d ", a, b, c, d, e);
    return kind + " " + key + " " + rr::hd::Hex64(hash) + n + file + "\n";
}

double Seconds(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// ------------------------------------------------------------------------------------------------ commands

int Extract(const std::string& discPath, const fs::path& work, bool movies) {
    const auto t0 = std::chrono::steady_clock::now();
    rr::DiscImage disc(discPath);
    const std::string sha = ExeSha1(disc);
    if (sha.empty()) throw std::runtime_error("the image has no SLUS_010.53");
    fs::create_directories(work / "pictures" / kInput);
    fs::create_directories(work / "pictures" / "original");
    fs::create_directories(work / "stage" / "fonts");
    fs::create_directories(work / "stage" / "hud");
    WriteText(work / "profile.txt", sha + "\n");
    std::string log;
    // pictures: the neural network's input (RGB, with a border of repeated edge texels - Pad) and the original (RGBA,
    // for the transparency)
    const std::vector<Picture> pictures = Pictures(disc, log);
    std::string list;
    for (size_t i = 0; i < pictures.size(); ++i) {
        const Picture& p = pictures[i];
        const std::string name = Name("p", i, ".png");
        int pw = 0, ph = 0;
        const Rgba input = Pad(NeuralInput(p.image), pw, ph);
        if (!fs::exists(work / "pictures" / kInput / name)) {
            if (!rr::hd::WritePngFile(work / "pictures" / kInput / name, input, true) ||
                !rr::hd::WritePngFile(work / "pictures" / "original" / name, p.image))
                throw std::runtime_error("cannot write " + name);
        }
        list += p.key + " " + rr::hd::Hex64(p.hash) + " " + std::to_string(p.image.width) + " " + std::to_string(p.image.height) +
                " " + (p.alpha ? "1" : "0") + " " + name + " " + std::to_string(kPad) + " " + std::to_string(pw) + " " +
                std::to_string(ph) + "\n";
    }
    WriteText(work / "pictures" / "list.txt", list);
    // fonts: contour atlases, finished here
    std::string fonts;
    for (size_t i = 0; i < std::size(kFonts); ++i) {
        const rr::shell::Font f = rr::shell::ParseFont(Read(disc, kFonts[i].file));
        const std::string name = Name("f", i, ".png");
        if (!rr::hd::WriteContourFile(work / "stage" / "fonts" / name, FontAtlas(f))) throw std::runtime_error("cannot write " + name);
        fonts += IndexLine("font", kFonts[i].key, rr::hd::SourceHashIndices(f.sheetWidth, f.sheetHeight, f.sheet.data()), f.sheetWidth,
                           f.sheetHeight, 0, 0, 0, "fonts/" + name);
    }
    WriteText(work / "stage" / "fonts.idx", fonts);
    log += "fonts: " + std::to_string(std::size(kFonts)) + " contour atlases\n";
    // the HUD: contour atlases per region
    std::string hud;
    const std::vector<HudSource> regions = HudRegions(disc, log, true);
    for (size_t i = 0; i < regions.size(); ++i) {
        const HudSource& s = regions[i];
        const std::string name = Name("h", i, ".png");
        if (!rr::hd::WriteContourFile(work / "stage" / "hud" / name, s.atlas)) throw std::runtime_error("cannot write " + name);
        char key[64];
        std::snprintf(key, sizeof(key), "hud:%d,%d,%d,%d,%04x", s.region.u, s.region.v, s.region.w, s.region.h, s.region.clut);
        hud += IndexLine("hud", key, s.hash, s.region.u, s.region.v, s.region.w, s.region.h, s.region.clut, "hud/" + name);
    }
    WriteText(work / "stage" / "hud.idx", hud);
    log += "HUD: " + std::to_string(regions.size()) + " contour regions (art and font, all layouts)\n";
    // films: every picture as PNG, per film
    size_t filmPictures = 0;
    if (movies) {
        std::map<std::string, std::string> meta;
        const std::vector<Film> films = Films(disc, [&](const Film& film, int i, const Rgba& r) {
            if (fs::exists(work / "stage" / "movies" / (film.name + ".idx"))) return; // packed already: no input needed
            const fs::path dir = work / "movies" / film.name / "input";
            if (i == 0) fs::create_directories(dir);
            const fs::path file = dir / Name("f", static_cast<size_t>(i), ".png");
            if (!fs::exists(file) && !rr::hd::WritePngFile(file, r, true)) throw std::runtime_error("cannot write " + file.string());
            ++filmPictures;
        });
        for (const Film& f : films)
            WriteText(work / "movies" / f.name / "movie.txt", f.key + " " + rr::hd::Hex64(f.hash) + " " + std::to_string(f.frames) + " " +
                                                               std::to_string(f.width) + " " + std::to_string(f.height) + "\n");
        log += "films: " + std::to_string(films.size()) + " (" + std::to_string(filmPictures) + " pictures)\n";
    }
    std::printf("%sextract: %.1f s\n", log.c_str(), Seconds(t0));
    return 0;
}

int Compose(const fs::path& work, const fs::path& upscaled) {
    const auto t0 = std::chrono::steady_clock::now();
    std::ifstream list(work / "pictures" / "list.txt");
    if (!list) throw std::runtime_error("no pictures/list.txt: run extract first");
    fs::create_directories(work / "stage" / "pictures");
    std::string index, line, rejected;
    size_t n = 0, png = 0, refused = 0;
    double worst = 0, sum = 0;
    while (std::getline(list, line)) {
        std::istringstream s(line);
        std::string key, hash, name;
        int w = 0, h = 0, alpha = 0, pad = 0, pw = 0, ph = 0;
        if (!(s >> key >> hash >> w >> h >> alpha >> name >> pad >> pw >> ph)) continue;
        Rgba framed;
        std::string error;
        if (!rr::hd::LoadImageFile(Neural(upscaled, name), framed, &error)) throw std::runtime_error("the neural picture is missing: " + error);
        if (framed.width != pw * rr::hd::kScale || framed.height != ph * rr::hd::kScale)
            throw std::runtime_error(name + ": the neural picture is not 4x its source");
        Rgba up = Crop(framed, pad * rr::hd::kScale, pad * rr::hd::kScale, w * rr::hd::kScale, h * rr::hd::kScale);
        Rgba original;
        if (!rr::hd::LoadImageFile(work / "pictures" / "original" / name, original, &error)) throw std::runtime_error(error);
        // The sanity bound: the network's picture against a bicubic enlargement of the original, mean over the opaque
        // texels. A picture the network ruined (noise) is left out - the game then enlarges the original's texels.
        {
            const std::vector<float> base = rr::hd::BicubicRgb(original.px, w, h, w * 4, h * 4);
            double diff = 0;
            size_t count = 0;
            for (int y = 0; y < h * 4; ++y)
                for (int x = 0; x < w * 4; ++x) {
                    if (original.px[(static_cast<size_t>(y / 4) * static_cast<size_t>(w) + static_cast<size_t>(x / 4)) * 4u + 3u] < 128) continue;
                    const size_t i = static_cast<size_t>(y) * static_cast<size_t>(w * 4) + static_cast<size_t>(x);
                    for (int c = 0; c < 3; ++c) diff += std::abs(static_cast<double>(up.px[4 * i + static_cast<size_t>(c)]) - base[3 * i + static_cast<size_t>(c)]);
                    count += 3;
                }
            const double mean = count ? diff / static_cast<double>(count) : 0.0;
            if (mean > 40.0) { // sharp small sprites (pad glyphs, arrows, weapon icons) measure 16..34 and are good
                ++refused;
                char b[160];
                std::snprintf(b, sizeof(b), "  left out (the network's picture is %.1f levels from the bicubic one): %s\n", mean, key.c_str());
                rejected += b;
                continue;
            }
            worst = std::max(worst, mean);
            sum += mean;
        }
        std::string file;
        if (alpha) { // the original's transparency, enlarged edge-directed (never the network's)
            std::vector<uint8_t> mask(static_cast<size_t>(w) * static_cast<size_t>(h));
            for (size_t i = 0; i < mask.size(); ++i) mask[i] = original.px[4 * i + 3] >= 128;
            const std::vector<uint8_t> m4 = Epx2(Epx2(mask, w, h), w * 2, h * 2);
            for (size_t i = 0; i < m4.size(); ++i) up.px[4 * i + 3] = m4[i] ? 255 : 0;
            file = "pictures/" + fs::path(name).stem().string() + ".png";
            if (!rr::hd::WritePngFile(work / "stage" / file, up)) throw std::runtime_error("cannot write " + file);
            ++png;
        } else {
            file = "pictures/" + fs::path(name).stem().string() + ".jpg";
            if (!rr::hd::WriteJpegFile(work / "stage" / file, up, 92)) throw std::runtime_error("cannot write " + file);
        }
        index += "pic " + key + " " + hash + " " + std::to_string(w) + " " + std::to_string(h) + " 0 0 0 " + file + "\n";
        ++n;
    }
    WriteText(work / "stage" / "pictures.idx", index);
    std::printf("%scompose: %zu pictures (%zu with transparency as PNG, the rest JPEG q92), %zu left out; mean distance from the "
                "bicubic enlargement %.1f levels, largest %.1f; %.1f s\n",
                rejected.c_str(), n, png, refused, n ? sum / static_cast<double>(n) : 0.0, worst, Seconds(t0));
    return 0;
}

int PackMovie(const fs::path& work, const std::string& name, const fs::path& upscaled) {
    const auto t0 = std::chrono::steady_clock::now();
    const fs::path dir = work / "movies" / name;
    std::ifstream meta(dir / "movie.txt");
    std::string key, hash;
    int frames = 0, w = 0, h = 0;
    if (!(meta >> key >> hash >> frames >> w >> h) || frames <= 0) throw std::runtime_error("no movie.txt for " + name);
    fs::create_directories(work / "stage" / "movies");
    const std::string file = "movies/" + name + ".rhm";
    rr::hd::MovieWriter writer;
    if (!writer.Open(work / "stage" / (file + ".part"), static_cast<uint32_t>(w * 4), static_cast<uint32_t>(h * 4), static_cast<uint32_t>(w),
                     static_cast<uint32_t>(h), std::strtoull(hash.c_str(), nullptr, 16)))
        throw std::runtime_error("cannot write " + file);
    rr::hd::FilmStabiliser stab;
    double change = 0;
    for (int i = 0; i < frames; ++i) {
        const std::string f = Name("f", static_cast<size_t>(i), ".png");
        Rgba original, up;
        std::string error;
        if (!rr::hd::LoadImageFile(dir / "input" / f, original, &error) || !rr::hd::LoadImageFile(Neural(upscaled, f), up, &error))
            throw std::runtime_error(name + ": " + error);
        if (original.width != w || original.height != h || up.width != w * 4 || up.height != h * 4)
            throw std::runtime_error(name + " " + f + ": unexpected picture size");
        const std::vector<uint8_t> raw = up.px;
        rr::hd::ConstrainFilmPicture(up.px, up.width, up.height, original.px, w, h, &stab);
        double sum = 0;
        for (size_t k = 0; k < raw.size(); k += 4)
            for (int c = 0; c < 3; ++c) sum += std::abs(static_cast<int>(raw[k + static_cast<size_t>(c)]) - static_cast<int>(up.px[k + static_cast<size_t>(c)]));
        change += sum / static_cast<double>(raw.size() / 4 * 3);
        if (!writer.Add(up, 92)) throw std::runtime_error("cannot encode " + f);
    }
    if (!writer.Finish()) throw std::runtime_error("cannot finish " + file);
    fs::rename(work / "stage" / (file + ".part"), work / "stage" / file);
    WriteText(work / "stage" / "movies" / (name + ".idx"), "movie " + key + " " + hash + " " + std::to_string(frames) + " " + std::to_string(w) +
                                                              " " + std::to_string(h) + " 0 0 " + file + "\n");
    std::printf("pack-movie %s: %d pictures %dx%d -> %dx%d; %.1f %% of pixels held from the picture before (still parts), "
                "mean pull of the bound %.2f levels; %.1f s\n",
                name.c_str(), frames, w, h, w * 4, h * 4, stab.total ? 100.0 * static_cast<double>(stab.held) / static_cast<double>(stab.total) : 0.0,
                change / frames, Seconds(t0));
    return 0;
}

// ---- the films streamed (prepare-hd.ps1): a chunk of pictures at a time through the network, each chunk bounded and
// stabilised here into JPEGs (movies\NAME\packed) with the stabiliser's state carried in movies\NAME\state.bin, the
// network's full-size output deleted by the script after every chunk; film-finish joins the JPEGs into the RRHDMOV1.
struct FilmMeta {
    std::string key, hash;
    int frames = 0, w = 0, h = 0;
};
FilmMeta ReadFilmMeta(const fs::path& dir, const std::string& name) {
    std::ifstream meta(dir / "movie.txt");
    FilmMeta m;
    if (!(meta >> m.key >> m.hash >> m.frames >> m.w >> m.h) || m.frames <= 0) throw std::runtime_error("no movie.txt for " + name);
    return m;
}

int FilmChunk(const fs::path& work, const std::string& name, const fs::path& upscaled, int first, int count) {
    const auto t0 = std::chrono::steady_clock::now();
    const fs::path dir = work / "movies" / name;
    const FilmMeta m = ReadFilmMeta(dir, name);
    const size_t n = static_cast<size_t>(m.w) * 4u * static_cast<size_t>(m.h) * 4u;
    rr::hd::FilmStabiliser stab;
    double change = 0;
    int next = 0;
    if (first > 0) { // the state the chunk before left
        std::ifstream s(dir / "state.bin", std::ios::binary);
        uint64_t held = 0, total = 0;
        s.read(reinterpret_cast<char*>(&next), sizeof(next));
        s.read(reinterpret_cast<char*>(&held), sizeof(held));
        s.read(reinterpret_cast<char*>(&total), sizeof(total));
        s.read(reinterpret_cast<char*>(&change), sizeof(change));
        stab.output.resize(n * 3u);
        stab.anchor.resize(n * 3u);
        s.read(reinterpret_cast<char*>(stab.output.data()), static_cast<std::streamsize>(stab.output.size()));
        s.read(reinterpret_cast<char*>(stab.anchor.data()), static_cast<std::streamsize>(stab.anchor.size() * sizeof(float)));
        if (!s || next != first) throw std::runtime_error(name + ": the stabiliser state is not at picture " + std::to_string(first));
        stab.held = held;
        stab.total = total;
    }
    fs::create_directories(dir / "packed");
    const int end = std::min(m.frames, first + count);
    for (int i = first; i < end; ++i) {
        const std::string f = Name("f", static_cast<size_t>(i), ".png");
        Rgba original, up;
        std::string error;
        if (!rr::hd::LoadImageFile(dir / "input" / f, original, &error) || !rr::hd::LoadImageFile(Neural(upscaled, f), up, &error))
            throw std::runtime_error(name + ": " + error);
        if (original.width != m.w || original.height != m.h || up.width != m.w * 4 || up.height != m.h * 4)
            throw std::runtime_error(name + " " + f + ": unexpected picture size");
        const std::vector<uint8_t> raw = up.px;
        rr::hd::ConstrainFilmPicture(up.px, up.width, up.height, original.px, m.w, m.h, &stab);
        double sum = 0;
        for (size_t k = 0; k < raw.size(); k += 4)
            for (int c = 0; c < 3; ++c) sum += std::abs(static_cast<int>(raw[k + static_cast<size_t>(c)]) - static_cast<int>(up.px[k + static_cast<size_t>(c)]));
        change += sum / static_cast<double>(raw.size() / 4 * 3);
        if (!rr::hd::WriteJpegFile(dir / "packed" / Name("f", static_cast<size_t>(i), ".jpg"), up, 92))
            throw std::runtime_error("cannot write a packed picture of " + name);
    }
    {
        const fs::path temp = dir / "state.bin.tmp";
        std::ofstream s(temp, std::ios::binary | std::ios::trunc);
        const uint64_t held = stab.held, total = stab.total;
        s.write(reinterpret_cast<const char*>(&end), sizeof(end));
        s.write(reinterpret_cast<const char*>(&held), sizeof(held));
        s.write(reinterpret_cast<const char*>(&total), sizeof(total));
        s.write(reinterpret_cast<const char*>(&change), sizeof(change));
        s.write(reinterpret_cast<const char*>(stab.output.data()), static_cast<std::streamsize>(stab.output.size()));
        s.write(reinterpret_cast<const char*>(stab.anchor.data()), static_cast<std::streamsize>(stab.anchor.size() * sizeof(float)));
        s.close();
        if (!s) throw std::runtime_error("cannot write the stabiliser state of " + name);
        fs::rename(temp, dir / "state.bin");
    }
    std::printf("film-chunk %s: pictures %d..%d of %d, %.1f s\n", name.c_str(), first, end - 1, m.frames, Seconds(t0));
    return 0;
}

int FilmFinish(const fs::path& work, const std::string& name) {
    const fs::path dir = work / "movies" / name;
    const FilmMeta m = ReadFilmMeta(dir, name);
    std::ifstream s(dir / "state.bin", std::ios::binary);
    int next = 0;
    uint64_t held = 0, total = 0;
    double change = 0;
    s.read(reinterpret_cast<char*>(&next), sizeof(next));
    s.read(reinterpret_cast<char*>(&held), sizeof(held));
    s.read(reinterpret_cast<char*>(&total), sizeof(total));
    s.read(reinterpret_cast<char*>(&change), sizeof(change));
    s.close();
    if (next != m.frames) throw std::runtime_error(name + ": only " + std::to_string(next) + " of " + std::to_string(m.frames) + " pictures packed");
    fs::create_directories(work / "stage" / "movies");
    const std::string file = "movies/" + name + ".rhm";
    rr::hd::MovieWriter writer;
    if (!writer.Open(work / "stage" / (file + ".part"), static_cast<uint32_t>(m.w * 4), static_cast<uint32_t>(m.h * 4), static_cast<uint32_t>(m.w),
                     static_cast<uint32_t>(m.h), std::strtoull(m.hash.c_str(), nullptr, 16)))
        throw std::runtime_error("cannot write " + file);
    for (int i = 0; i < m.frames; ++i) {
        std::ifstream j(dir / "packed" / Name("f", static_cast<size_t>(i), ".jpg"), std::ios::binary);
        const std::vector<uint8_t> jpeg((std::istreambuf_iterator<char>(j)), std::istreambuf_iterator<char>());
        if (!writer.AddEncoded(jpeg)) throw std::runtime_error(name + ": packed picture " + std::to_string(i) + " is missing or wrong");
    }
    if (!writer.Finish()) throw std::runtime_error("cannot finish " + file);
    fs::rename(work / "stage" / (file + ".part"), work / "stage" / file);
    WriteText(work / "stage" / "movies" / (name + ".idx"), "movie " + m.key + " " + m.hash + " " + std::to_string(m.frames) + " " +
                                                              std::to_string(m.w) + " " + std::to_string(m.h) + " 0 0 " + file + "\n");
    fs::remove_all(dir / "packed");
    fs::remove(dir / "state.bin");
    std::printf("film-finish %s: %d pictures %dx%d -> %dx%d; %.1f %% of pixels held from the picture before (still parts), "
                "mean pull of the bound %.2f levels\n",
                name.c_str(), m.frames, m.w, m.h, m.w * 4, m.h * 4, total ? 100.0 * static_cast<double>(held) / static_cast<double>(total) : 0.0,
                change / m.frames);
    return 0;
}

int Finish(const fs::path& work) {
    const fs::path stage = work / "stage";
    std::string index = "rrjb-hd-index 1\n";
    size_t parts = 0;
    for (const fs::path& p : {stage / "pictures.idx", stage / "fonts.idx", stage / "hud.idx"}) {
        std::ifstream f(p);
        if (!f) continue;
        index += std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        ++parts;
    }
    if (fs::is_directory(stage / "movies"))
        for (const auto& e : fs::directory_iterator(stage / "movies"))
            if (e.path().extension() == ".idx") {
                std::ifstream f(e.path());
                index += std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            }
    std::ifstream prof(work / "profile.txt");
    std::string sha;
    if (!(prof >> sha)) throw std::runtime_error("no profile.txt: run extract first");
    WriteText(stage / "index.txt", index);
    WriteText(stage / "profile.txt", sha + "\nrrjb-hd 1\n");
    std::printf("finish: index of %zu lines (%zu picture/font/HUD parts + films) for SLUS_010.53 SHA-1 %s\n",
                static_cast<size_t>(std::count(index.begin(), index.end(), '\n')) - 1, parts, sha.c_str());
    return 0;
}

int Verify(const std::string& discPath, const fs::path& packDir, bool deep) {
    const auto t0 = std::chrono::steady_clock::now();
    rr::DiscImage disc(discPath);
    std::string why;
    const auto pack = rr::hd::Pack::Open(packDir, ExeSha1(disc), why);
    if (!pack) {
        std::printf("verify: %s\n", why.c_str());
        return 1;
    }
    std::map<std::string, uint64_t> sources; // kind:key -> hash of this disc
    std::string log;
    for (const Picture& p : Pictures(disc, log)) sources["pic:" + p.key] = p.hash;
    for (const FontSource& f : kFonts) {
        const rr::shell::Font font = rr::shell::ParseFont(Read(disc, f.file));
        sources[std::string("font:") + f.key] = rr::hd::SourceHashIndices(font.sheetWidth, font.sheetHeight, font.sheet.data());
    }
    std::set<uint64_t> hud;
    for (const HudSource& s : HudRegions(disc, log, false)) hud.insert(s.hash);
    for (const Film& f : Films(disc, nullptr)) sources["movie:" + f.key] = f.hash;
    size_t ok = 0, refused = 0, broken = 0;
    std::map<std::string, size_t> kinds;
    for (const rr::hd::Entry& e : pack->Entries()) {
        const bool match = e.kind == "hud" ? hud.count(e.hash) != 0 : sources.count(e.kind + ":" + e.key) && sources[e.kind + ":" + e.key] == e.hash;
        if (!match) {
            ++refused;
            std::printf("  refused: %s %s\n", e.kind.c_str(), e.key.c_str());
            continue;
        }
        bool good = fs::is_regular_file(pack->PathOf(e));
        if (good && e.kind == "movie") {
            rr::hd::MovieReader r;
            good = r.Open(pack->PathOf(e)) && static_cast<int>(r.frames) == e.a && r.sourceHash == e.hash;
            Rgba p;
            for (uint32_t i = 0; good && deep && i < r.frames; ++i) good = r.Frame(i, p); // every picture decodes
        } else if (good && deep) { // the file decodes at 4x its source
            Rgba p;
            rr::hd::Contour c;
            if (e.kind == "pic") good = pack->Picture(e.key, e.hash, e.a, e.b, p);
            else if (e.kind == "font") good = pack->Font(e.key, e.hash, e.a, e.b, c);
            else if (e.kind == "hud") good = pack->HudRegion(e, c);
        }
        if (!good) {
            ++broken;
            std::printf("  broken: %s %s (%s)\n", e.kind.c_str(), e.key.c_str(), e.file.c_str());
            continue;
        }
        ++ok;
        ++kinds[e.kind];
    }
    std::string k;
    for (const auto& [kind, n] : kinds) k += (k.empty() ? "" : ", ") + std::to_string(n) + " " + kind;
    std::printf("verify: %zu entries, %zu match this disc (%s), %zu refused, %zu broken; %.1f s\n", pack->Entries().size(), ok,
                k.c_str(), refused, broken, Seconds(t0));
    return refused == 0 && broken == 0 ? 0 : 1;
}

Rgba Nearest(const Rgba& in, int s) {
    Rgba o;
    o.width = in.width * s;
    o.height = in.height * s;
    o.px.resize(static_cast<size_t>(o.width) * static_cast<size_t>(o.height) * 4u);
    for (int y = 0; y < o.height; ++y)
        for (int x = 0; x < o.width; ++x)
            std::memcpy(&o.px[(static_cast<size_t>(y) * static_cast<size_t>(o.width) + static_cast<size_t>(x)) * 4u],
                        &in.px[(static_cast<size_t>(y / s) * static_cast<size_t>(in.width) + static_cast<size_t>(x / s)) * 4u], 4);
    return o;
}

Rgba Crop(const Rgba& in, int x0, int y0, int w, int h) {
    Rgba o;
    o.width = w;
    o.height = h;
    o.px.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int sx = x0 + x, sy = y0 + y;
            if (sx < 0 || sy < 0 || sx >= in.width || sy >= in.height) continue;
            std::memcpy(&o.px[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u],
                        &in.px[(static_cast<size_t>(sy) * static_cast<size_t>(in.width) + static_cast<size_t>(sx)) * 4u], 4);
        }
    return o;
}

Rgba Beside(const Rgba& a, const Rgba& b) {
    Rgba o;
    o.width = a.width + 8 + b.width;
    o.height = std::max(a.height, b.height);
    o.px.assign(static_cast<size_t>(o.width) * static_cast<size_t>(o.height) * 4u, 0);
    for (size_t i = 3; i < o.px.size(); i += 4) o.px[i] = 255;
    for (int y = 0; y < o.height; ++y)
        for (int x = a.width; x < a.width + 8; ++x) std::memset(&o.px[(static_cast<size_t>(y) * static_cast<size_t>(o.width) + static_cast<size_t>(x)) * 4u], 255, 4);
    const auto put = [&](const Rgba& s, int ox) {
        for (int y = 0; y < s.height; ++y)
            std::memcpy(&o.px[(static_cast<size_t>(y) * static_cast<size_t>(o.width) + static_cast<size_t>(ox)) * 4u],
                        &s.px[static_cast<size_t>(y) * static_cast<size_t>(s.width) * 4u], static_cast<size_t>(s.width) * 4u);
    };
    put(a, 0);
    put(b, a.width + 8);
    return o;
}

int CompareFilm(const std::string& discPath, const fs::path& packDir, const std::string& name, int picture, const fs::path& out) {
    rr::DiscImage disc(discPath);
    std::string why;
    const auto pack = rr::hd::Pack::Open(packDir, ExeSha1(disc), why);
    if (!pack) throw std::runtime_error(why);
    Rgba original;
    Film film;
    Films(disc, [&](const Film& f, int i, const Rgba& r) {
        if (f.name == Upper(name) && i == picture) original = r, film = f;
    });
    if (original.width == 0) throw std::runtime_error("no picture " + std::to_string(picture) + " in " + name);
    const rr::hd::Entry* e = pack->Movie(film.key, film.hash, film.frames);
    if (e == nullptr) throw std::runtime_error("the pack has no HD film for " + film.key);
    rr::hd::MovieReader reader;
    Rgba hd;
    std::string error;
    if (!reader.Open(pack->PathOf(*e), &error) || !reader.Frame(static_cast<uint32_t>(picture), hd, &error)) throw std::runtime_error(error);
    if (!rr::hd::WritePngFile(out, Beside(Nearest(original, 4), hd), true)) throw std::runtime_error("cannot write " + out.string());
    std::printf("compare-film: %s picture %d: original %dx%d enlarged 4x nearest | HD %dx%d -> %s\n", film.key.c_str(), picture,
                original.width, original.height, hd.width, hd.height, out.string().c_str());
    return 0;
}

int SideBySide(const fs::path& a, const fs::path& b, const fs::path& out, int cx, int cy, int cw, int ch) {
    Rgba ia, ib;
    std::string error;
    if (!rr::hd::LoadImageFile(a, ia, &error) || !rr::hd::LoadImageFile(b, ib, &error)) throw std::runtime_error(error);
    for (int k = 2; k <= 4; ++k) // an original beside its HD version: enlarged texel by texel to the same size
        if (ia.width * k == ib.width && ia.height * k == ib.height) ia = Nearest(ia, k);
    if (cw > 0) {
        ia = Crop(ia, cx, cy, cw, ch);
        ib = Crop(ib, cx, cy, cw, ch);
    }
    if (!rr::hd::WritePngFile(out, Beside(ia, ib), true)) throw std::runtime_error("cannot write " + out.string());
    std::printf("sidebyside: %s | %s -> %s\n", a.string().c_str(), b.string().c_str(), out.string().c_str());
    return 0;
}

// ------------------------------------------------------------------------------------------------ checks

Rgba Pattern(int w, int h, int seed) { // synthetic: diagonal colour bands, no game art
    Rgba r;
    r.width = w;
    r.height = h;
    r.px.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &r.px[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u];
            const int band = ((x + y) / 16 + seed) % 6;
            p[0] = static_cast<uint8_t>(band & 1 ? 230 : 40);
            p[1] = static_cast<uint8_t>(band & 2 ? 200 : 30);
            p[2] = static_cast<uint8_t>(band & 4 ? 250 : 60);
            p[3] = 255;
        }
    return r;
}

int SelfTest(const fs::path& dir) {
    fs::remove_all(dir);
    fs::create_directories(dir / "good" / "pictures");
    int checks = 0, failed = 0;
    const auto check = [&](bool ok, const char* what) {
        ++checks;
        if (!ok) ++failed;
        std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    };
    // a synthetic source: 8 x 6 BGR555 texels
    std::vector<uint16_t> src(48);
    for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint16_t>(0x8000u | (i * 613u));
    const uint64_t hash = rr::hd::SourceHash15(8, 6, src.data());
    check(rr::hd::WriteJpegFile(dir / "good" / "pictures" / "a.jpg", Pattern(32, 24, 0), 92), "JPEG written");
    Rgba alpha = Pattern(32, 24, 3);
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 32; ++x)
            if (x < y) alpha.px[(static_cast<size_t>(y) * 32u + static_cast<size_t>(x)) * 4u + 3u] = 0;
    check(rr::hd::WritePngFile(dir / "good" / "pictures" / "b.png", alpha), "PNG with transparency written");
    // a font sheet with a diagonal stroke: the contour pass must produce mixed texels on it
    std::vector<uint8_t> sheet(16 * 16, 0);
    for (int i = 2; i < 14; ++i) sheet[static_cast<size_t>(i) * 16u + static_cast<size_t>(i)] = 15, sheet[static_cast<size_t>(i) * 16u + static_cast<size_t>(i + 1 < 16 ? i + 1 : i)] = 15;
    rr::hd::Contour c;
    c.width = c.height = 64;
    c.abw.assign(64u * 64u * 3u, 0);
    rr::hd::NearestContours4x(sheet.data(), 16, 0, 0, 16, 16, c.abw.data(), 64, 0, 0);
    rr::hd::SmoothContours4x(sheet.data(), 16, 16, 0, 0, 16, 16, GreyRamp(), true, c.abw.data(), 64, 0, 0);
    size_t mixed = 0;
    for (size_t i = 0; i < c.abw.size(); i += 3) mixed += c.abw[i + 2] != 0 && c.abw[i] != c.abw[i + 1];
    check(mixed > 0, "the contour pass mixes indices along a diagonal stroke");
    check(rr::hd::WriteContourFile(dir / "good" / "font.png", c), "contour atlas written");
    rr::hd::Contour back;
    check(rr::hd::LoadContourFile(dir / "good" / "font.png", back) && back.abw == c.abw, "contour atlas round trip is exact");
    // a film of three pictures
    {
        rr::hd::MovieWriter w;
        bool ok = w.Open(dir / "good" / "film.rhm", 32, 24, 8, 6, 0x1234);
        for (int i = 0; i < 3; ++i) ok = ok && w.Add(Pattern(32, 24, i));
        ok = ok && w.Finish();
        rr::hd::MovieReader r;
        Rgba p;
        check(ok && r.Open(dir / "good" / "film.rhm") && r.frames == 3 && r.sourceHash == 0x1234 && r.Frame(2, p) && p.width == 32,
              "RRHDMOV1 film round trip");
    }
    const uint64_t fontHash = rr::hd::SourceHashIndices(16, 16, sheet.data());
    const std::string index = "rrjb-hd-index 1\n" + IndexLine("pic", "SYN/A.STR#0", hash, 8, 6, 0, 0, 0, "pictures/a.jpg") +
                              IndexLine("pic", "SYN/B.PSH:ABCD", hash, 8, 6, 0, 0, 0, "pictures/b.png") +
                              IndexLine("font", "SYN/F.PFN", fontHash, 16, 16, 0, 0, 0, "font.png") +
                              IndexLine("movie", "SYN/M.WVE", 0x1234, 3, 8, 6, 0, 0, "film.rhm");
    WriteText(dir / "good" / "index.txt", index);
    WriteText(dir / "good" / "profile.txt", "0123456789abcdef0123456789abcdef01234567\nrrjb-hd 1\n");
    std::string why;
    auto pack = rr::hd::Pack::Open(dir / "good", "0123456789abcdef0123456789abcdef01234567", why);
    check(pack != nullptr && pack->Entries().size() == 4, "a pack of the right profile opens");
    Rgba p;
    rr::hd::Contour f;
    check(pack && pack->Picture("SYN/A.STR#0", hash, 8, 6, p) && p.width == 32 && p.px[3] == 255, "a JPEG picture of the right source loads (4x)");
    check(pack && pack->Picture("SYN/B.PSH:ABCD", hash, 8, 6, p) && p.px[3 + 4u * 32u * 23u] == 0, "a PNG picture keeps its transparency");
    check(pack && pack->Font("SYN/F.PFN", fontHash, 16, 16, f) && f.abw == c.abw, "a font atlas of the right source loads");
    check(pack && pack->Movie("SYN/M.WVE", 0x1234, 3) != nullptr, "a film of the right source and length is offered");
    // negative controls
    check(pack && !pack->Picture("SYN/A.STR#0", hash ^ 1u, 8, 6, p), "NEGATIVE: a picture whose source hash differs is refused");
    check(pack && !pack->Font("SYN/F.PFN", fontHash ^ 1u, 16, 16, f), "NEGATIVE: a font whose source hash differs is refused");
    check(pack && pack->Movie("SYN/M.WVE", 0x1235, 3) == nullptr && pack->Movie("SYN/M.WVE", 0x1234, 4) == nullptr,
          "NEGATIVE: a film of another source or length is refused");
    check(pack && !pack->Picture("SYN/A.STR#0", hash, 9, 6, p), "NEGATIVE: a picture of another source size is refused");
    check(rr::hd::Pack::Open(dir / "good", "fedcba9876543210fedcba9876543210fedcba98", why) == nullptr &&
              why.find("REJECTED") != std::string::npos,
          "NEGATIVE: the whole pack is refused for another disc (profile)");
    size_t notes = pack ? pack->TakeNotes().size() : 0;
    check(notes >= 4, "every refusal is noted for the log");
    // the film bound: a residual of 100 levels is cut to 10
    {
        Rgba o = Pattern(8, 6, 1), e = Nearest(o, 4);
        for (size_t k = 0; k < e.px.size(); k += 4) e.px[k] = static_cast<uint8_t>(std::min(255, e.px[k] + 100));
        const std::vector<float> base = rr::hd::BicubicRgb(o.px, 8, 6, 32, 24);
        rr::hd::ConstrainFilmPicture(e.px, 32, 24, o.px, 8, 6, nullptr);
        float worst = 0;
        for (size_t i = 0; i < base.size() / 3; ++i) worst = std::max(worst, std::abs(static_cast<float>(e.px[4 * i]) - base[3 * i]));
        check(worst <= 10.5f, "the film bound keeps the network within 10 levels of the bicubic picture");
    }
    std::printf("hd selftest: %d checks, %d failed\n", checks, failed);
    return failed == 0 ? 0 : 1;
}

// A pack for the loader's gates: SYNTHETIC pictures (colour bands) for the menu background's five film frames and a
// plain 4x atlas for the button font, each keyed to this disc's source hash (or a wrong one), under this disc's profile
// (or another). The gate's pictures are drawn only in work\.
int SynthPack(const std::string& discPath, const fs::path& dir, bool wrongHash, bool wrongProfile) {
    rr::DiscImage disc(discPath);
    fs::remove_all(dir);
    fs::create_directories(dir / "pictures");
    fs::create_directories(dir / "fonts");
    std::string log, index = "rrjb-hd-index 1\n";
    size_t n = 0;
    for (const Picture& p : Pictures(disc, log)) {
        if (p.key.rfind("DATA/FE/BGRND2.STR#", 0) != 0 && p.key.rfind("DATA/FE/BGRND3.STR#", 0) != 0) continue;
        const std::string file = "pictures/" + Name("s", n++, ".jpg");
        if (!rr::hd::WriteJpegFile(dir / file, Pattern(p.image.width * 4, p.image.height * 4, static_cast<int>(n)), 90))
            throw std::runtime_error("cannot write " + file);
        index += IndexLine("pic", p.key, wrongHash ? p.hash ^ 1u : p.hash, p.image.width, p.image.height, 0, 0, 0, file);
    }
    const rr::shell::Font f = rr::shell::ParseFont(Read(disc, "DATA/FE/BTN_FONT.PFN"));
    rr::hd::Contour c;
    c.width = f.sheetWidth * 4;
    c.height = f.sheetHeight * 4;
    c.abw.assign(static_cast<size_t>(c.width) * static_cast<size_t>(c.height) * 3u, 0);
    rr::hd::NearestContours4x(f.sheet.data(), f.sheetWidth, 0, 0, f.sheetWidth, f.sheetHeight, c.abw.data(), c.width, 0, 0);
    if (!rr::hd::WriteContourFile(dir / "fonts" / "f00000.png", c)) throw std::runtime_error("cannot write the font atlas");
    const uint64_t fh = rr::hd::SourceHashIndices(f.sheetWidth, f.sheetHeight, f.sheet.data());
    index += IndexLine("font", "DATA/FE/BTN_FONT.PFN", wrongHash ? fh ^ 1u : fh, f.sheetWidth, f.sheetHeight, 0, 0, 0, "fonts/f00000.png");
    WriteText(dir / "index.txt", index);
    WriteText(dir / "profile.txt", (wrongProfile ? std::string("0000000000000000000000000000000000000000") : ExeSha1(disc)) + "\nrrjb-hd 1\n");
    std::printf("synth-pack: %zu synthetic pictures + 1 font atlas%s%s -> %s\n", n, wrongHash ? ", source hashes WRONG" : "",
                wrongProfile ? ", profile WRONG" : "", dir.string().c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::string cmd = argc > 1 ? argv[1] : "";
        if (cmd == "extract" && argc >= 4) return Extract(argv[2], argv[3], argc > 4 && std::strcmp(argv[4], "--movies") == 0);
        if (cmd == "compose" && argc == 4) return Compose(argv[2], argv[3]);
        if (cmd == "pack-movie" && argc == 5) return PackMovie(argv[2], argv[3], argv[4]);
        if (cmd == "film-chunk" && argc == 7) return FilmChunk(argv[2], argv[3], argv[4], std::atoi(argv[5]), std::atoi(argv[6]));
        if (cmd == "film-finish" && argc == 4) return FilmFinish(argv[2], argv[3]);
        if (cmd == "finish" && argc == 3) return Finish(argv[2]);
        if (cmd == "verify" && (argc == 4 || argc == 5)) return Verify(argv[2], argv[3], argc == 5 && std::strcmp(argv[4], "--deep") == 0);
        if (cmd == "compare-film" && argc == 7) return CompareFilm(argv[2], argv[3], argv[4], std::atoi(argv[5]), argv[6]);
        if (cmd == "sidebyside" && (argc == 5 || argc == 10))
            return SideBySide(argv[2], argv[3], argv[4], argc == 10 ? std::atoi(argv[6]) : 0, argc == 10 ? std::atoi(argv[7]) : 0,
                              argc == 10 ? std::atoi(argv[8]) : 0, argc == 10 ? std::atoi(argv[9]) : 0);
        if (cmd == "selftest" && argc == 3) return SelfTest(argv[2]);
        if (cmd == "synth-pack" && argc >= 4)
            return SynthPack(argv[2], argv[3], argc > 4 && std::strcmp(argv[4], "--wrong-hash") == 0,
                             argc > 4 && std::strcmp(argv[4], "--wrong-profile") == 0);
        std::fprintf(stderr, "usage: rrhd extract <disc> <work> [--movies] | compose <work> <upscaled> | pack-movie <work> <NAME> <upscaled>\n"
                             "       | finish <work> | verify <disc> <pack> [--deep] | compare-film <disc> <pack> <NAME> <picture> <out.png>\n"
                             "       | sidebyside <a.png> <b.png> <out.png> [crop x y w h] | selftest <dir>\n"
                             "       | synth-pack <disc> <dir> [--wrong-hash|--wrong-profile]\n");
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rrhd: %s\n", e.what());
        return 1;
    }
}
