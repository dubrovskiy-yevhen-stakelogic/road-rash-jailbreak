#pragma once
// The optional HD media pack (docs\HD-MEDIA.md): pictures, font and HUD atlases and films enlarged four times OFFLINE
// from the player's own disc by scripts\prepare-hd.ps1 (tools\rrhd), stored in the install's runtime\hd folder (on the
// Quest in files/hd) and never in this source tree. The game only reads it; nothing is enlarged while playing.
//
// Layout of a pack folder:
//   profile.txt   line 1: the SHA-1 of the disc's SLUS_010.53 the pack was made from (a pack of another disc is
//                 refused whole), line 2: the format ("rrjb-hd 1")
//   index.txt     "rrjb-hd-index 1", then one entry per line:
//                   <kind> <key> <source hash, 16 hex> <a> <b> <c> <d> <e> <file>
//                 kind pic   : a picture (key = "DATA/FE/X.STR#frame", "DATA/FE/X.TCM#entry", "DATA/FE/FEMISC.PSH:ABCD"),
//                              a b = its source width and height; the file is its 4x RGBA (PNG, or JPEG when opaque)
//                 kind font  : a 4-bit font sheet (key = "DATA/FE/BTN_FONT.PFN"), a b = sheet width and height; the file
//                              is the 4x contour atlas (below)
//                 kind hud   : a region of the race HUD's VRAM page, a b c d = its u, v, width, height in 4-bit texels of
//                              the page, e = the CLUT id the region is drawn with; the file is its 4x contour atlas
//                 kind movie : a film (key = "DATA/FE/INTRO.WVE"), a = frames, b c = source width and height; the file is
//                              an RRHDMOV1 container (below)
//   manifest.json SHA-256 of every file (written by the preparation; the game does not need it)
// The SOURCE HASH is FNV-1a 64 of the decoded original asset as the game itself decodes it (SourceHash* below): an entry
// whose hash differs from what the running game decoded is refused and the original is drawn - a pack never replaces
// an asset it was not made from.
//
// A CONTOUR ATLAS (the xBR contour pass of hd_contours.h) keeps palette indices, not colours: each 4x texel holds two
// original 4-bit indices a, b and a weight w (R, G, B of an RGB PNG). The game mixes the LIVE palette's colour of a and
// b by w, so fades, flashes and the CLUT the packet selects still apply.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace rr::hd {

constexpr int kScale = 4; // every pack entry is four times its source

uint64_t Fnv64(const void* data, size_t bytes, uint64_t seed = 14695981039346656037ull);
std::string Hex64(uint64_t v);

// The source hashes (the same functions in the game and in tools\rrhd).
uint64_t SourceHash15(int width, int height, const uint16_t* px);             // MDEC pictures, PSH shapes
uint64_t SourceHashIndices(int width, int height, const uint8_t* indices);    // a font sheet's 4-bit indices
uint64_t SourceHashBytes(std::span<const uint8_t> file);                      // a film file
// A HUD region: its indices (u, v, w, h in 4-bit texels of the page `page`, 64 halfwords x 256 rows at VRAM x 960) and
// the 16 colours of its CLUT.
uint64_t SourceHashHudRegion(const uint16_t* page, int u, int v, int w, int h, uint32_t clut);

struct Rgba {
    int width = 0, height = 0;
    std::vector<uint8_t> px; // width * height * 4, rows top to bottom
};
struct Contour {
    int width = 0, height = 0;
    std::vector<uint8_t> abw; // width * height * 3: index a, index b, weight of b (0..255)
};

// PNG / JPEG bytes (stb_image) -> RGBA8. False with `error` on bad data.
bool DecodeImage(std::span<const uint8_t> file, Rgba& out, std::string* error = nullptr);
bool LoadImageFile(const std::filesystem::path& path, Rgba& out, std::string* error = nullptr);
bool LoadContourFile(const std::filesystem::path& path, Contour& out, std::string* error = nullptr);
// Encoders (tools\rrhd): a compressed PNG (RGBA or, with `rgbOnly`, RGB), a baseline JPEG.
bool WritePngFile(const std::filesystem::path& path, const Rgba& image, bool rgbOnly = false);
bool WriteContourFile(const std::filesystem::path& path, const Contour& atlas);
std::vector<uint8_t> EncodeJpeg(const Rgba& image, int quality);
bool WriteJpegFile(const std::filesystem::path& path, const Rgba& image, int quality);

// Integer box reduction (a 4x entry drawn at 2x on the Quest). Binary alpha: a block is opaque when at least half of
// its texels are, and takes the mean colour of its opaque texels.
Rgba Reduce(const Rgba& image, int factor);

struct Entry {
    std::string kind, key, file;
    uint64_t hash = 0;
    int a = 0, b = 0, c = 0, d = 0, e = 0;
};

class Pack {
public:
    // Opens `dir`. Null (and `why`) when there is no pack there, when its profile names another executable than
    // `exeSha1`, or when its index is not a pack index.
    static std::shared_ptr<Pack> Open(const std::filesystem::path& dir, const std::string& exeSha1, std::string& why);

    const std::filesystem::path& Dir() const { return dir_; }
    const std::vector<Entry>& Entries() const { return entries_; }
    const Entry* Find(const std::string& kind, const std::string& key) const;
    std::filesystem::path PathOf(const Entry& e) const { return dir_ / e.file; }

    // An entry by kind and key whose source hash is `hash` (and, for a picture or font, whose source size is w x h):
    // loaded, checked (4x the source) and returned. False when absent or refused - a refusal is noted once
    // (Notes()); the caller then draws the original.
    bool Picture(const std::string& key, uint64_t hash, int w, int h, Rgba& out);
    bool Font(const std::string& key, uint64_t hash, int w, int h, Contour& out);
    bool HudRegion(const Entry& e, Contour& out);
    // The film entry for `key` when its hash and frame count match (null otherwise, noted).
    const Entry* Movie(const std::string& key, uint64_t hash, int frames);

    // One line: how many entries of each kind.
    std::string Summary() const;
    // Refusals and load errors so far, each once (for the log); TakeNotes empties the list.
    std::vector<std::string> TakeNotes();

private:
    void Note(const std::string& s);
    std::filesystem::path dir_;
    std::vector<Entry> entries_;
    std::vector<std::string> notes_, noted_;
};

// The process's pack and switch (graphics settings "HD textures and media"): Active() is the pack while HD media are
// on, null otherwise. Generation() changes whenever either changes, so a cache knows to drop its HD copies.
void SetPack(std::shared_ptr<Pack> pack);
std::shared_ptr<Pack> LoadedPack();
void SetEnabled(bool on);
bool Enabled();
Pack* Active();
uint64_t Generation();
// The scale the game draws HD 2D at: 4 on the desktop, 2 on Android (the Quest's theatre screen and HUD panel are
// about 1000 pixels wide in the eye; a 4x shell frame there costs CPU for no visible gain). RRJB_HD_SCALE=2|4 overrides.
int DrawScale();

// RRHDMOV1: a film's enlarged pictures, one baseline JPEG per picture, in order. Little-endian:
//   +0  "RRHDMOV1"   +8 u32 width  +12 u32 height  +16 u32 frames  +20 u32 source width  +24 u32 source height
//   +28 u32 0        +32 u64 source hash (SourceHashBytes of the film)  +40 u64 index offset  +48 16 bytes 0
//   index: frames x {u64 offset, u32 bytes, u32 0}
// The sound stays the film's own (decoded from the disc as without HD).
class MovieReader {
public:
    bool Open(const std::filesystem::path& path, std::string* error = nullptr);
    uint32_t width = 0, height = 0, frames = 0, sourceWidth = 0, sourceHeight = 0;
    uint64_t sourceHash = 0;
    bool Frame(uint32_t index, Rgba& out, std::string* error = nullptr);

private:
    std::ifstream file_;
    uint64_t length_ = 0;
    std::vector<std::pair<uint64_t, uint32_t>> index_;
};

class MovieWriter {
public:
    bool Open(const std::filesystem::path& path, uint32_t width, uint32_t height, uint32_t sourceWidth,
              uint32_t sourceHeight, uint64_t sourceHash);
    bool Add(const Rgba& picture, int quality = 92);
    bool AddEncoded(const std::vector<uint8_t>& jpeg); // a JPEG already made at this size (the streamed films)
    bool Finish();

private:
    std::ofstream out_;
    uint32_t width_ = 0, height_ = 0, sourceWidth_ = 0, sourceHeight_ = 0;
    uint64_t hash_ = 0;
    std::vector<std::pair<uint64_t, uint32_t>> index_;
};

} // namespace rr::hd
