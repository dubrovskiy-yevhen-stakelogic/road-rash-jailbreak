#pragma once
// DATA\GAMEBIN1.DAT - the per-race level bundle, and what the renderer takes out of it.
//
// Header-only on purpose (like sky_gradient.h): it adds no translation unit, so the build file does
// not have to change.
//
// THE CONTAINER [proven from our own disassembly of RASHCDI.BIN, sha1 9a8b79d8..., and byte-checked
// against the disc and the `rr-race` capture]:
//
//   file    u16 magic 0x0103, u16 count, u32 0, then `count` u32 bundle offsets measured from the end
//           of that table. `RASHCDI 0x800619F8..0x80061A1C` reads the 8 bytes and refuses any magic
//           but 259; `0x80061A24..0x80061A98` reads the table and takes entry `[raceId - 1]`, where
//           raceId is game_state+0x40 (the race id ROADGRF<set>.TXT names the race by), and seeks
//           relative to the end of the table (`0x80014634` mode 1).
//   bundle  u16 magic 0x0103, u16 sections, u32 size (the whole bundle, header included), u32, then
//           `sections` u32 offsets measured from the end of that table (`0x80061AC0..0x80061BB0`).
//   section u16 type, u16, then the payload. `0x80061BC8..0x80061BEC` hands (section, section + 4)
//           to the dispatcher `0x80061C24`, whose jump table (RASHCDI data 0x8005B764) sends
//           type 1 -> 0x80062384, 4 -> 0x80061D18, 7 -> 0x80062430, ...
//
// What this header reads:
//   * type 7, the 256-entry COLOUR TABLE: `0x80062430` copies the first 1152 bytes of the payload
//     to guest `0x800D4CA8` (`jal 0x8001E0B4` with a2 = 1152). Every cell primitive is drawn
//     modulated by an entry of it (docs\formats\scene_cell.md 13). In `rr-race` the 1024 bytes at
//     0x800D4CA8 are byte-identical to bundle 3's type-7 payload - race 4 of set 1, bundle 4 - 1.
//   * type 4, the ROAD SURFACE textures: `0x80061D18`. The payload starts with 20 u32 offsets
//     relative to the payload: [0..3] four TIMs, [4..7] not read by this handler, [8..19] twelve raw
//     16-colour CLUTs. TIMs 0, 1 and 2 are uploaded (`0x80048A6C` LoadImage) to the rectangle the
//     EXE's per-set configuration table (`SLUS 0x800533B4`, stride 0x58, 4-byte groups) names in its
//     groups 7, 6 and 5: x = (group[2] & 0xF) * 64, y = group[1] + ((group[2] & 0x10) << 4). TIM 3
//     is not uploaded. TIM 0's rectangle is the road's texture page (`0x80061DF8..0x80061E24` store
//     its tpage word at guest 0x8005B370). The twelve CLUTs are uploaded in order and their CLUT ids
//     stored at guest 0x800D5EC8[0..11] (`0x80061F48..0x80061F7C`), which is the table the band-2
//     draw routine picks its palette from - so palette ROW n of the page built here is CLUT n.
//   * type 1 is the sky-gradient field sky_gradient.h already reads at a fixed offset; the same
//     container rule now says which bundle a race uses.
#include "rrformats/texture.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace rr {

struct LevelBundleSection {
    uint16_t type = 0;
    uint16_t tag = 0;
    size_t payload = 0; // file offset of the payload (section + 4)
    size_t limit = 0;   // file offset one past the last byte the section can own
};

struct LevelBundle {
    size_t index = 0;
    size_t offset = 0; // file offset of the bundle header
    size_t size = 0;
    std::vector<LevelBundleSection> sections;
    const LevelBundleSection* Find(uint16_t type) const {
        for (const LevelBundleSection& s : sections)
            if (s.type == type) return &s;
        return nullptr;
    }
};

namespace level_bundle_detail {
inline uint16_t U16(std::span<const uint8_t> d, size_t at) {
    if (at + 2 > d.size()) throw std::runtime_error("GAMEBIN1.DAT: read past the end");
    return static_cast<uint16_t>(d[at] | (d[at + 1] << 8));
}
inline uint32_t U32(std::span<const uint8_t> d, size_t at) {
    if (at + 4 > d.size()) throw std::runtime_error("GAMEBIN1.DAT: read past the end");
    return static_cast<uint32_t>(d[at]) | (static_cast<uint32_t>(d[at + 1]) << 8) |
           (static_cast<uint32_t>(d[at + 2]) << 16) | (static_cast<uint32_t>(d[at + 3]) << 24);
}
} // namespace level_bundle_detail

constexpr uint16_t kLevelBundleMagic = 0x0103;

inline size_t LevelBundleCount(std::span<const uint8_t> file) {
    using namespace level_bundle_detail;
    if (U16(file, 0) != kLevelBundleMagic) throw std::runtime_error("GAMEBIN1.DAT: bad file magic");
    return U16(file, 2);
}

// THE RESIDENT BUNDLE. RASHCDI 0x80061A98..0x80061AB0: when table entry
// [raceId - 1] is 0x7FFFFFFF the loader closes the file and loads NO bundle - VRAM (sky, road, effect sheet)
// and the colour table 0x800D4CA8 keep what the previous race's bundle put there. GAMEBIN1.DAT has one such
// entry, race 31 (GAMEBIN2.DAT's 19..37 are exactly the ids road set 2 lacks); no menu path of RASHCDF
// reaches race 31 and RACE1_31.STP is not on the disc. The product keeps that rule: SelectLevelBundle, run
// once per race by the race's start (rrgame), remembers the last bundle it loaded in this process (the front
// end's races run in-process, so a race after another keeps the previous one's) and answers a sentinel race
// with it. OURS, named: a race started with nothing resident (rrgame --race 1 31) gets bundle 3, race 4's -
// game_state+0x40's boot value (GameStateInit SLUS 0x80011760), the only race id the console holds before a
// commit. RRJB_RESIDENT_BUNDLE=off: the negative control (the sentinel entry is parsed as before and fails).
struct LevelBundleResidency {
    int raceId = 0;            // the race SelectLevelBundle last answered
    size_t index = 0;          // its bundle
    bool substituted = false;  // the race's own entry was 0x7FFFFFFF and `index` is the resident one
    bool bootDefault = false;  // ... and nothing was resident: OURS, race 4's bundle
    bool haveLoaded = false;
    size_t lastLoaded = 0;     // the last bundle a race of this process loaded
    size_t sentinelRaces = 0;  // races answered with the resident bundle (the run tail's counter)
};
inline LevelBundleResidency& ResidentLevelBundle() {
    static LevelBundleResidency r;
    return r;
}

// The bundle a race loads: entry [raceId - 1] of the file table (RASHCDI 0x80061A80..0x80061A98), or the
// resident one when SelectLevelBundle found that entry to be the no-bundle sentinel.
inline size_t LevelBundleIndexForRace(int raceId) {
    const LevelBundleResidency& r = ResidentLevelBundle();
    if (r.substituted && r.raceId == raceId) return r.index;
    return raceId > 0 ? static_cast<size_t>(raceId - 1) : 0;
}

inline LevelBundle ParseLevelBundle(std::span<const uint8_t> file, size_t index) {
    using namespace level_bundle_detail;
    const size_t count = LevelBundleCount(file);
    if (index >= count) throw std::runtime_error("GAMEBIN1.DAT: bundle index past the table");
    const size_t tableEnd = 8 + count * 4;
    LevelBundle bundle;
    bundle.index = index;
    bundle.offset = tableEnd + U32(file, 8 + index * 4);
    if (U16(file, bundle.offset) != kLevelBundleMagic) throw std::runtime_error("GAMEBIN1.DAT: bad bundle magic");
    const size_t sections = U16(file, bundle.offset + 2);
    bundle.size = U32(file, bundle.offset + 4);
    if (bundle.offset + bundle.size > file.size()) throw std::runtime_error("GAMEBIN1.DAT: bundle past the end");
    const size_t dataBase = bundle.offset + 12 + sections * 4;
    for (size_t s = 0; s < sections; ++s) {
        LevelBundleSection section;
        const size_t at = dataBase + U32(file, bundle.offset + 12 + s * 4);
        if (at + 4 > bundle.offset + bundle.size) throw std::runtime_error("GAMEBIN1.DAT: section past the bundle");
        section.type = U16(file, at);
        section.tag = U16(file, at + 2);
        section.payload = at + 4;
        section.limit = bundle.offset + bundle.size;
        bundle.sections.push_back(section);
    }
    // A section owns the bytes up to the next section that starts after it.
    for (LevelBundleSection& a : bundle.sections)
        for (const LevelBundleSection& b : bundle.sections)
            if (b.payload > a.payload && b.payload - 4 < a.limit) a.limit = b.payload - 4;
    return bundle;
}

// Once per race, before anything parses a bundle: decides LevelBundleIndexForRace(raceId) for this race from
// the file's table (RASHCDI 0x80061A98's test) and returns one line for the run's log.
inline std::string SelectLevelBundle(std::span<const uint8_t> file, int raceId) {
    using namespace level_bundle_detail;
    LevelBundleResidency& r = ResidentLevelBundle();
    r.raceId = raceId;
    r.substituted = false;
    r.bootDefault = false;
    const size_t count = LevelBundleCount(file);
    const size_t own = raceId > 0 ? static_cast<size_t>(raceId - 1) : 0;
    if (own >= count) return "level bundle: race " + std::to_string(raceId) + " is past GAMEBIN1.DAT's table";
    const uint32_t entry = U32(file, 8 + own * 4);
    const char* off = std::getenv("RRJB_RESIDENT_BUNDLE");
    const bool control = off != nullptr && std::string(off) == "off";
    if (entry != 0x7FFFFFFFu || control) {
        r.index = own;
        if (entry != 0x7FFFFFFFu) {
            r.haveLoaded = true;
            r.lastLoaded = own;
        }
        return entry != 0x7FFFFFFFu
                   ? "level bundle: GAMEBIN1.DAT entry " + std::to_string(own) + " (race " + std::to_string(raceId) + ")"
                   : "level bundle: GAMEBIN1.DAT entry " + std::to_string(own) + " is 0x7FFFFFFF (no bundle) - "
                     "RRJB_RESIDENT_BUNDLE=off: parsed as an offset anyway (the negative control)";
    }
    r.substituted = true;
    ++r.sentinelRaces;
    r.bootDefault = !r.haveLoaded;
    r.index = r.haveLoaded ? r.lastLoaded : 3u;
    return "level bundle: GAMEBIN1.DAT entry " + std::to_string(own) + " (race " + std::to_string(raceId) +
           ") is 0x7FFFFFFF - RASHCDI 0x80061A98 loads no bundle and the resident one stays: bundle " +
           std::to_string(r.index) +
           (r.bootDefault ? " (OURS: nothing resident in this process - race 4's, game_state+0x40's boot value "
                            "SLUS 0x80011760)"
                          : " (the previous race's)");
}

// The type-2 section, the level's light: its handler RASHCDI 0x8006250C
// stores the sun's yaw at 0x80052388 = payload word 7 * 11 and its elevation at 0x8005238C = (word 8 * 11) / 4
// (rounded toward zero: +3 before the arithmetic shift when negative), the two angles the sky gradient
// RASHCDG 0x80063C5C blends by (4096 units per turn). Race 1/4's bundle: words 65, 45 -> 715, 123, the values
// every race capture holds at those addresses.
struct LevelSun {
    int32_t yaw = 0, elevation = 0;
    int32_t elevationAngle = 0; // word 8 * 11: the angle the sun VECTOR's y is the sine of (0x80052366)
};
inline LevelSun ParseLevelSun(std::span<const uint8_t> file, const LevelBundle& bundle) {
    using namespace level_bundle_detail;
    const LevelBundleSection* section = bundle.Find(2);
    if (!section) throw std::runtime_error("GAMEBIN1.DAT: bundle has no type-2 light section");
    if (section->payload + 36 > section->limit) throw std::runtime_error("GAMEBIN1.DAT: light section is short");
    LevelSun sun;
    sun.yaw = static_cast<int32_t>(U32(file, section->payload + 7 * 4)) * 11;
    int32_t e = static_cast<int32_t>(U32(file, section->payload + 8 * 4)) * 11;
    sun.elevationAngle = e;
    if (e < 0) e += 3;
    sun.elevation = e >> 2;
    return sun;
}

// The model light of the type-7 section. Its handler RASHCDI 0x80062430 copies
// 0x480 bytes of the payload to 0x800D4CA8: words 0..255 the cell colour table, words 256..287 the 32-step
// MODEL RAMP the model draw RASHCDG 0x800674D4 copies to scratchpad 0x1F800020 every frame, and the model
// emitter SLUS 0x800251E4 colours a lit vertex with ramp[intensity >> shift] and an unlit object with
// ramp[0x80052380 >> shift], 0x80052380 = s16(payload + 0x484) - 0x100. Race 1/4's bundle: the ramp runs
// (42,44,45) .. (126,118,104), the unlit step is 12 = (70,68,64) - every value the captured packets carry.
struct LevelModelLight {
    std::array<uint32_t, 32> ramp{}; // 0x00BBGGRR
    int32_t unlitStep = 12;
};
inline LevelModelLight ParseLevelModelLight(std::span<const uint8_t> file, const LevelBundle& bundle) {
    using namespace level_bundle_detail;
    const LevelBundleSection* section = bundle.Find(7);
    if (!section) throw std::runtime_error("GAMEBIN1.DAT: bundle has no type-7 colour table");
    if (section->payload + 0x486 > section->limit) throw std::runtime_error("GAMEBIN1.DAT: colour table is short");
    LevelModelLight light;
    for (size_t k = 0; k < 32; ++k) light.ramp[k] = U32(file, section->payload + 0x400 + 4 * k) & 0x00FFFFFFu;
    light.unlitStep = static_cast<int16_t>(U16(file, section->payload + 0x484)) - 0x100;
    return light;
}

// The type-7 colour table: 256 GPU colour words 0x00BBGGRR.
inline std::array<uint32_t, 256> ParseLevelColourTable(std::span<const uint8_t> file, const LevelBundle& bundle) {
    using namespace level_bundle_detail;
    const LevelBundleSection* section = bundle.Find(7);
    if (!section) throw std::runtime_error("GAMEBIN1.DAT: bundle has no type-7 colour table");
    if (section->payload + 1024 > section->limit) throw std::runtime_error("GAMEBIN1.DAT: colour table is short");
    std::array<uint32_t, 256> table{};
    for (size_t i = 0; i < 256; ++i) table[i] = U32(file, section->payload + i * 4) & 0x00FFFFFFu;
    return table;
}

// One TIM of the road section, 4 bits per pixel.
struct RoadTim {
    int bpp = 0;
    uint16_t x = 0, y = 0;        // the rectangle the TIM itself names (the loader ignores it)
    int widthTexels = 0, height = 0;
    std::vector<uint8_t> indices; // widthTexels * height
};

struct RoadTextures {
    std::array<RoadTim, 4> tims;
    std::array<std::array<uint16_t, 16>, 12> cluts{};
};

inline RoadTim ParseRoadTim(std::span<const uint8_t> file, size_t at, size_t limit) {
    using namespace level_bundle_detail;
    if (U32(file, at) != 0x10) throw std::runtime_error("GAMEBIN1.DAT: road TIM magic");
    const uint32_t flags = U32(file, at + 4);
    size_t p = at + 8;
    if (flags & 8) p += U32(file, p); // the TIM's own CLUT block - the loader does not use it
    RoadTim tim;
    tim.bpp = (flags & 3) == 0 ? 4 : (flags & 3) == 1 ? 8 : 16;
    if (tim.bpp != 4) throw std::runtime_error("GAMEBIN1.DAT: road TIM is not 4bpp");
    const uint32_t bnum = U32(file, p);
    tim.x = U16(file, p + 4);
    tim.y = U16(file, p + 6);
    const size_t wHalf = U16(file, p + 8);
    tim.height = U16(file, p + 10);
    if (bnum != 12 + wHalf * static_cast<size_t>(tim.height) * 2 || p + bnum > limit)
        throw std::runtime_error("GAMEBIN1.DAT: road TIM pixel block does not add up");
    tim.widthTexels = static_cast<int>(wHalf * 4);
    tim.indices.resize(static_cast<size_t>(tim.widthTexels) * static_cast<size_t>(tim.height));
    for (int y = 0; y < tim.height; ++y)
        for (int x = 0; x < tim.widthTexels; ++x) {
            const uint8_t byte = file[p + 12 + static_cast<size_t>(y) * wHalf * 2 + static_cast<size_t>(x / 2)];
            tim.indices[static_cast<size_t>(y) * static_cast<size_t>(tim.widthTexels) + static_cast<size_t>(x)] =
                static_cast<uint8_t>((x & 1) ? (byte >> 4) : (byte & 0xF));
        }
    return tim;
}

inline RoadTextures ParseRoadTextures(std::span<const uint8_t> file, const LevelBundle& bundle) {
    using namespace level_bundle_detail;
    const LevelBundleSection* section = bundle.Find(4);
    if (!section) throw std::runtime_error("GAMEBIN1.DAT: bundle has no type-4 road section");
    RoadTextures out;
    for (size_t k = 0; k < 4; ++k)
        out.tims[k] = ParseRoadTim(file, section->payload + U32(file, section->payload + k * 4), section->limit);
    for (size_t k = 0; k < 12; ++k) {
        const size_t at = section->payload + U32(file, section->payload + (8 + k) * 4);
        if (at + 32 > section->limit) throw std::runtime_error("GAMEBIN1.DAT: road CLUT past the section");
        for (size_t i = 0; i < 16; ++i) out.cluts[k][i] = U16(file, at + i * 2);
    }
    return out;
}

// The EXE's per-set texture configuration (`SLUS 0x800533B4`, stride 0x58): group g of set `set`
// as (x, y) in VRAM halfwords, the way `RASHCDI 0x80061DA4..0x80061DD0` turns it into a rectangle.
struct VramPoint {
    int x = 0, y = 0;
};
inline VramPoint TextureConfigPoint(std::span<const uint8_t> exe, int set, int group) {
    using namespace level_bundle_detail;
    constexpr uint32_t kTable = 0x800533B4u;
    constexpr uint32_t kLoad = 0x80010000u; // PS-X EXE text address; file offset 0x800
    const size_t at = static_cast<size_t>(kTable - kLoad) + 0x800 + static_cast<size_t>(set - 1) * 0x58 +
                      static_cast<size_t>(group) * 4;
    if (set < 1 || at + 4 > exe.size()) throw std::runtime_error("SLUS_010.53: texture config out of range");
    const uint8_t vBias = exe[at + 1], sel = exe[at + 2];
    return {static_cast<int>(sel & 0xF) * 64, static_cast<int>(vBias) + ((sel & 0x10) << 4)};
}

// The road texture PAGE as the GPU sees it: the 256 x 256 4bpp page at TIM 0's upload rectangle,
// with TIMs 0..2 placed where `0x80061D18` uploads them, and the twelve CLUTs as palette rows 0..11.
// A texel no TIM covers is left at index 0 (whatever else lives in that VRAM page is not ours to
// draw, and no road primitive samples it - the UV table only reaches rows 0..127).
struct RoadPage {
    int pageX = 0, pageY = 0; // VRAM halfword origin of the page
    std::vector<uint8_t> indices; // 256 * 256
    std::vector<uint8_t> covered; // 1 where a TIM was placed
    std::array<std::array<uint16_t, 16>, 12> cluts{};
    int timsPlaced = 0;
};
inline RoadPage BuildRoadPage(const RoadTextures& textures, std::span<const uint8_t> exe, int set) {
    RoadPage page;
    const VramPoint origin = TextureConfigPoint(exe, set, 7);
    page.pageX = origin.x;
    page.pageY = origin.y & 0x100;
    page.indices.assign(256 * 256, 0);
    page.covered.assign(256 * 256, 0);
    page.cluts = textures.cluts;
    for (int k = 0; k < 3; ++k) {
        const VramPoint at = TextureConfigPoint(exe, set, 7 - k);
        const RoadTim& tim = textures.tims[static_cast<size_t>(k)];
        const int tx = (at.x - page.pageX) * 4, ty = at.y - page.pageY;
        if (tx < 0 || ty < 0 || tx + tim.widthTexels > 256 || ty + tim.height > 256) continue;
        for (int y = 0; y < tim.height; ++y)
            for (int x = 0; x < tim.widthTexels; ++x) {
                const size_t cell = static_cast<size_t>(ty + y) * 256 + static_cast<size_t>(tx + x);
                page.indices[cell] =
                    tim.indices[static_cast<size_t>(y) * static_cast<size_t>(tim.widthTexels) + static_cast<size_t>(x)];
                page.covered[cell] = 1;
            }
        ++page.timsPlaced;
    }
    return page;
}

} // namespace rr
