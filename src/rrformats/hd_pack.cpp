// The optional HD media pack - see hd_pack.h.
#include "rrformats/hd_pack.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#pragma warning(disable : 4244 4456 4457 4702)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 8192
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb/stb_image.h"
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb/stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace rr::hd {

uint64_t Fnv64(const void* data, size_t bytes, uint64_t seed) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

std::string Hex64(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
    return b;
}

namespace {
uint64_t Header(int width, int height, uint32_t tag) {
    const uint32_t h[3] = {tag, static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    return Fnv64(h, sizeof(h));
}
} // namespace

uint64_t SourceHash15(int width, int height, const uint16_t* px) {
    const uint64_t h = Header(width, height, 0x31354752u); // "RG51"
    std::vector<uint8_t> bytes(static_cast<size_t>(width) * static_cast<size_t>(height) * 2u);
    for (size_t i = 0; i < bytes.size() / 2; ++i) {
        bytes[2 * i] = static_cast<uint8_t>(px[i] & 0xFFu);
        bytes[2 * i + 1] = static_cast<uint8_t>(px[i] >> 8);
    }
    return Fnv64(bytes.data(), bytes.size(), h);
}

uint64_t SourceHashIndices(int width, int height, const uint8_t* indices) {
    return Fnv64(indices, static_cast<size_t>(width) * static_cast<size_t>(height), Header(width, height, 0x34584449u));
}

uint64_t SourceHashBytes(std::span<const uint8_t> file) { return Fnv64(file.data(), file.size()); }

uint64_t SourceHashHudRegion(const uint16_t* page, int u, int v, int w, int h, uint32_t clut) {
    std::vector<uint8_t> bytes;
    bytes.reserve(static_cast<size_t>(w) * static_cast<size_t>(h) + 64);
    for (int y = v; y < v + h; ++y)
        for (int x = u; x < u + w; ++x) {
            if (x < 0 || y < 0 || x >= 256 || y >= 256) {
                bytes.push_back(0xFF);
                continue;
            }
            const uint16_t hw = page[static_cast<size_t>(y) * 64u + static_cast<size_t>(x / 4)];
            bytes.push_back(static_cast<uint8_t>((hw >> ((x & 3) * 4)) & 15u));
        }
    const int cx = static_cast<int>(clut & 0x3Fu) * 16 - 960, cy = static_cast<int>((clut >> 6) & 0x1FFu);
    for (int k = 0; k < 16; ++k) {
        const uint16_t c = (cx + k >= 0 && cx + k < 64 && cy < 256) ? page[static_cast<size_t>(cy) * 64u + static_cast<size_t>(cx + k)] : 0xFFFFu;
        bytes.push_back(static_cast<uint8_t>(c & 0xFFu));
        bytes.push_back(static_cast<uint8_t>(c >> 8));
    }
    const uint32_t head[6] = {0x44554852u, static_cast<uint32_t>(u), static_cast<uint32_t>(v), static_cast<uint32_t>(w),
                              static_cast<uint32_t>(h), clut};
    return Fnv64(bytes.data(), bytes.size(), Fnv64(head, sizeof(head)));
}

namespace {
bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff n = f.tellg();
    if (n <= 0 || n > (1ll << 30)) return false;
    out.resize(static_cast<size_t>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), n);
    return static_cast<bool>(f);
}
void Append(void* context, void* data, int size) {
    auto& v = *static_cast<std::vector<uint8_t>*>(context);
    const auto* p = static_cast<const uint8_t*>(data);
    v.insert(v.end(), p, p + size);
}
// A new file under a temporary name, then renamed over the old one: a published pack that shares the old file (a hard
// link, scripts\prepare-hd.ps1) keeps its bytes.
bool WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::filesystem::path temp = path;
    temp += ".tmp";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    return !ec;
}
} // namespace

bool DecodeImage(std::span<const uint8_t> file, Rgba& out, std::string* error) {
    int w = 0, h = 0, c = 0;
    if (file.empty() || file.size() > 0x7FFFFFFFu) {
        if (error) *error = "empty image";
        return false;
    }
    stbi_uc* data = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &w, &h, &c, 4);
    if (data == nullptr) {
        if (error) *error = std::string("image decode: ") + stbi_failure_reason();
        return false;
    }
    out.width = w;
    out.height = h;
    out.px.assign(data, data + static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    stbi_image_free(data);
    return true;
}

bool LoadImageFile(const std::filesystem::path& path, Rgba& out, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!ReadFile(path, bytes)) {
        if (error) *error = "cannot read " + path.string();
        return false;
    }
    return DecodeImage(bytes, out, error);
}

bool LoadContourFile(const std::filesystem::path& path, Contour& out, std::string* error) {
    Rgba im;
    if (!LoadImageFile(path, im, error)) return false;
    out.width = im.width;
    out.height = im.height;
    out.abw.resize(static_cast<size_t>(im.width) * static_cast<size_t>(im.height) * 3u);
    for (size_t i = 0; i < out.abw.size() / 3; ++i) {
        out.abw[3 * i] = im.px[4 * i];
        out.abw[3 * i + 1] = im.px[4 * i + 1];
        out.abw[3 * i + 2] = im.px[4 * i + 2];
        if (out.abw[3 * i] > 15 || out.abw[3 * i + 1] > 15) {
            if (error) *error = "contour atlas holds an index above 15: " + path.string();
            return false;
        }
    }
    return true;
}

bool WritePngFile(const std::filesystem::path& path, const Rgba& image, bool rgbOnly) {
    std::vector<uint8_t> bytes;
    int ok = 0;
    if (rgbOnly) {
        std::vector<uint8_t> rgb(static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 3u);
        for (size_t i = 0; i < rgb.size() / 3; ++i) std::memcpy(&rgb[3 * i], &image.px[4 * i], 3);
        ok = stbi_write_png_to_func(Append, &bytes, image.width, image.height, 3, rgb.data(), image.width * 3);
    } else {
        ok = stbi_write_png_to_func(Append, &bytes, image.width, image.height, 4, image.px.data(), image.width * 4);
    }
    return ok != 0 && WriteBytes(path, bytes);
}

bool WriteContourFile(const std::filesystem::path& path, const Contour& atlas) {
    std::vector<uint8_t> bytes;
    const int ok = stbi_write_png_to_func(Append, &bytes, atlas.width, atlas.height, 3, atlas.abw.data(), atlas.width * 3);
    return ok != 0 && WriteBytes(path, bytes);
}

std::vector<uint8_t> EncodeJpeg(const Rgba& image, int quality) {
    std::vector<uint8_t> rgb(static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 3u);
    for (size_t i = 0; i < rgb.size() / 3; ++i) std::memcpy(&rgb[3 * i], &image.px[4 * i], 3);
    std::vector<uint8_t> bytes;
    if (!stbi_write_jpg_to_func(Append, &bytes, image.width, image.height, 3, rgb.data(), quality)) bytes.clear();
    return bytes;
}

bool WriteJpegFile(const std::filesystem::path& path, const Rgba& image, int quality) {
    const std::vector<uint8_t> bytes = EncodeJpeg(image, quality);
    return !bytes.empty() && WriteBytes(path, bytes);
}

Rgba Reduce(const Rgba& image, int factor) {
    if (factor <= 1) return image;
    Rgba out;
    out.width = image.width / factor;
    out.height = image.height / factor;
    out.px.assign(static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * 4u, 0);
    const int n = factor * factor;
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            uint32_t sum[3] = {0, 0, 0};
            int opaque = 0;
            for (int dy = 0; dy < factor; ++dy)
                for (int dx = 0; dx < factor; ++dx) {
                    const uint8_t* p = &image.px[(static_cast<size_t>(y * factor + dy) * static_cast<size_t>(image.width) +
                                                  static_cast<size_t>(x * factor + dx)) * 4u];
                    if (p[3] < 128) continue;
                    ++opaque;
                    for (int c = 0; c < 3; ++c) sum[c] += p[c];
                }
            uint8_t* d = &out.px[(static_cast<size_t>(y) * static_cast<size_t>(out.width) + static_cast<size_t>(x)) * 4u];
            if (opaque * 2 < n || opaque == 0) continue;
            for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>((sum[c] + static_cast<uint32_t>(opaque) / 2u) / static_cast<uint32_t>(opaque));
            d[3] = 255;
        }
    return out;
}

// ------------------------------------------------------------------------------------------------ Pack

std::shared_ptr<Pack> Pack::Open(const std::filesystem::path& dir, const std::string& exeSha1, std::string& why) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        why = "no HD pack at " + dir.string();
        return nullptr;
    }
    std::ifstream profile(dir / "profile.txt");
    std::string sha, format;
    if (!(profile >> sha)) {
        why = "HD pack " + dir.string() + " has no profile.txt";
        return nullptr;
    }
    if (sha != exeSha1) {
        why = "HD pack " + dir.string() + " REJECTED: made from the disc whose SLUS_010.53 SHA-1 is " + sha +
              ", this disc's is " + exeSha1;
        return nullptr;
    }
    std::ifstream index(dir / "index.txt");
    std::string line;
    if (!std::getline(index, line) || line.rfind("rrjb-hd-index 1", 0) != 0) {
        why = "HD pack " + dir.string() + " has no valid index.txt";
        return nullptr;
    }
    auto pack = std::shared_ptr<Pack>(new Pack());
    pack->dir_ = dir;
    while (std::getline(index, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream s(line);
        Entry e;
        std::string hash;
        if (!(s >> e.kind >> e.key >> hash >> e.a >> e.b >> e.c >> e.d >> e.e >> e.file) || hash.size() != 16) continue;
        if (e.file.find("..") != std::string::npos || e.file.find(':') != std::string::npos || e.file[0] == '/' ||
            e.file[0] == '\\')
            continue;
        e.hash = std::strtoull(hash.c_str(), nullptr, 16);
        pack->entries_.push_back(std::move(e));
    }
    if (pack->entries_.empty()) {
        why = "HD pack " + dir.string() + " is empty";
        return nullptr;
    }
    return pack;
}

const Entry* Pack::Find(const std::string& kind, const std::string& key) const {
    for (const Entry& e : entries_)
        if (e.kind == kind && e.key == key) return &e;
    return nullptr;
}

void Pack::Note(const std::string& s) {
    if (std::find(noted_.begin(), noted_.end(), s) != noted_.end()) return;
    noted_.push_back(s);
    notes_.push_back(s);
}

std::vector<std::string> Pack::TakeNotes() {
    std::vector<std::string> n;
    n.swap(notes_);
    return n;
}

bool Pack::Picture(const std::string& key, uint64_t hash, int w, int h, Rgba& out) {
    const Entry* e = Find("pic", key);
    if (e == nullptr) return false;
    if (e->hash != hash || e->a != w || e->b != h) {
        Note("HD picture " + key + " refused: made from another source (hash " + Hex64(e->hash) + ", this disc " + Hex64(hash) + ")");
        return false;
    }
    std::string error;
    if (!LoadImageFile(PathOf(*e), out, &error)) {
        Note("HD picture " + key + ": " + error);
        return false;
    }
    if (out.width != w * kScale || out.height != h * kScale) {
        Note("HD picture " + key + " refused: " + std::to_string(out.width) + "x" + std::to_string(out.height) + " is not 4x");
        return false;
    }
    return true;
}

bool Pack::Font(const std::string& key, uint64_t hash, int w, int h, Contour& out) {
    const Entry* e = Find("font", key);
    if (e == nullptr) return false;
    if (e->hash != hash || e->a != w || e->b != h) {
        Note("HD font " + key + " refused: made from another source");
        return false;
    }
    std::string error;
    if (!LoadContourFile(PathOf(*e), out, &error)) {
        Note("HD font " + key + ": " + error);
        return false;
    }
    if (out.width != w * kScale || out.height != h * kScale) {
        Note("HD font " + key + " refused: not 4x");
        return false;
    }
    return true;
}

bool Pack::HudRegion(const Entry& e, Contour& out) {
    std::string error;
    if (!LoadContourFile(PathOf(e), out, &error)) {
        Note("HD HUD region " + e.key + ": " + error);
        return false;
    }
    if (out.width != e.c * kScale || out.height != e.d * kScale) {
        Note("HD HUD region " + e.key + " refused: not 4x");
        return false;
    }
    return true;
}

const Entry* Pack::Movie(const std::string& key, uint64_t hash, int frames) {
    const Entry* e = Find("movie", key);
    if (e == nullptr) return nullptr;
    if (e->hash != hash || e->a != frames) {
        Note("HD film " + key + " refused: made from another source");
        return nullptr;
    }
    return e;
}

std::string Pack::Summary() const {
    std::map<std::string, int> n;
    for (const Entry& e : entries_) ++n[e.kind];
    std::string s;
    for (const auto& [k, v] : n) s += (s.empty() ? "" : ", ") + std::to_string(v) + " " + k;
    return s;
}

// ------------------------------------------------------------------------------------------------ process state

namespace {
std::mutex& StateMutex() {
    static std::mutex m;
    return m;
}
std::shared_ptr<Pack>& PackSlot() {
    static std::shared_ptr<Pack> p;
    return p;
}
std::atomic<bool> gEnabled{false};
std::atomic<uint64_t> gGeneration{1};
} // namespace

void SetPack(std::shared_ptr<Pack> pack) {
    std::lock_guard<std::mutex> lock(StateMutex());
    if (PackSlot() == pack) return;
    PackSlot() = std::move(pack);
    ++gGeneration;
}
std::shared_ptr<Pack> LoadedPack() {
    std::lock_guard<std::mutex> lock(StateMutex());
    return PackSlot();
}
void SetEnabled(bool on) {
    if (gEnabled.exchange(on) != on) ++gGeneration;
}
bool Enabled() { return gEnabled.load(); }
Pack* Active() {
    std::lock_guard<std::mutex> lock(StateMutex());
    return gEnabled.load() ? PackSlot().get() : nullptr;
}
uint64_t Generation() { return gGeneration.load(); }

int DrawScale() {
    if (const char* e = std::getenv("RRJB_HD_SCALE"); e != nullptr && (e[0] == '2' || e[0] == '4') && e[1] == '\0')
        return e[0] - '0';
#ifdef __ANDROID__
    return 2;
#else
    return 4;
#endif
}

// ------------------------------------------------------------------------------------------------ RRHDMOV1

namespace {
uint64_t Get(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}
void Put(std::vector<uint8_t>& b, uint64_t v, int n) {
    for (int i = 0; i < n; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
} // namespace

bool MovieReader::Open(const std::filesystem::path& path, std::string* error) {
    file_ = std::ifstream(path, std::ios::binary | std::ios::ate);
    if (!file_) {
        if (error) *error = "cannot open " + path.string();
        return false;
    }
    length_ = static_cast<uint64_t>(file_.tellg());
    uint8_t h[64] = {};
    file_.seekg(0);
    file_.read(reinterpret_cast<char*>(h), 64);
    if (!file_ || std::memcmp(h, "RRHDMOV1", 8) != 0) {
        if (error) *error = "not an RRHDMOV1 film: " + path.string();
        return false;
    }
    width = static_cast<uint32_t>(Get(h + 8, 4));
    height = static_cast<uint32_t>(Get(h + 12, 4));
    frames = static_cast<uint32_t>(Get(h + 16, 4));
    sourceWidth = static_cast<uint32_t>(Get(h + 20, 4));
    sourceHeight = static_cast<uint32_t>(Get(h + 24, 4));
    sourceHash = Get(h + 32, 8);
    const uint64_t at = Get(h + 40, 8);
    if (width == 0 || height == 0 || width > 4096 || height > 4096 || frames == 0 || frames > 100000 || at < 64 ||
        at + static_cast<uint64_t>(frames) * 16u > length_) {
        if (error) *error = "bad RRHDMOV1 header: " + path.string();
        return false;
    }
    std::vector<uint8_t> idx(static_cast<size_t>(frames) * 16u);
    file_.seekg(static_cast<std::streamoff>(at));
    file_.read(reinterpret_cast<char*>(idx.data()), static_cast<std::streamsize>(idx.size()));
    if (!file_) return false;
    index_.clear();
    for (uint32_t i = 0; i < frames; ++i) {
        const uint64_t off = Get(&idx[16u * i], 8);
        const uint32_t bytes = static_cast<uint32_t>(Get(&idx[16u * i + 8], 4));
        if (off < 64 || bytes == 0 || off + bytes > at) {
            if (error) *error = "bad RRHDMOV1 index: " + path.string();
            return false;
        }
        index_.emplace_back(off, bytes);
    }
    return true;
}

bool MovieReader::Frame(uint32_t i, Rgba& out, std::string* error) {
    if (i >= index_.size()) return false;
    std::vector<uint8_t> jpeg(index_[i].second);
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(index_[i].first));
    file_.read(reinterpret_cast<char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
    if (!file_) {
        if (error) *error = "truncated HD film picture";
        return false;
    }
    if (!DecodeImage(jpeg, out, error)) return false;
    if (static_cast<uint32_t>(out.width) != width || static_cast<uint32_t>(out.height) != height) {
        if (error) *error = "HD film picture size differs from its header";
        return false;
    }
    return true;
}

bool MovieWriter::Open(const std::filesystem::path& path, uint32_t width, uint32_t height, uint32_t sourceWidth,
                       uint32_t sourceHeight, uint64_t sourceHash) {
    out_ = std::ofstream(path, std::ios::binary | std::ios::trunc);
    width_ = width;
    height_ = height;
    sourceWidth_ = sourceWidth;
    sourceHeight_ = sourceHeight;
    hash_ = sourceHash;
    index_.clear();
    const char zero[64] = {};
    out_.write(zero, 64);
    return static_cast<bool>(out_);
}

bool MovieWriter::Add(const Rgba& picture, int quality) {
    if (static_cast<uint32_t>(picture.width) != width_ || static_cast<uint32_t>(picture.height) != height_) return false;
    return AddEncoded(EncodeJpeg(picture, quality));
}

bool MovieWriter::AddEncoded(const std::vector<uint8_t>& jpeg) {
    int w = 0, h = 0, c = 0;
    if (jpeg.empty() || !stbi_info_from_memory(jpeg.data(), static_cast<int>(jpeg.size()), &w, &h, &c) ||
        static_cast<uint32_t>(w) != width_ || static_cast<uint32_t>(h) != height_)
        return false;
    index_.emplace_back(static_cast<uint64_t>(out_.tellp()), static_cast<uint32_t>(jpeg.size()));
    out_.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
    return static_cast<bool>(out_);
}

bool MovieWriter::Finish() {
    if (index_.empty()) return false;
    const uint64_t at = static_cast<uint64_t>(out_.tellp());
    std::vector<uint8_t> idx;
    for (const auto& [off, bytes] : index_) {
        Put(idx, off, 8);
        Put(idx, bytes, 4);
        Put(idx, 0, 4);
    }
    out_.write(reinterpret_cast<const char*>(idx.data()), static_cast<std::streamsize>(idx.size()));
    std::vector<uint8_t> h;
    for (char c : std::string("RRHDMOV1")) h.push_back(static_cast<uint8_t>(c));
    Put(h, width_, 4);
    Put(h, height_, 4);
    Put(h, index_.size(), 4);
    Put(h, sourceWidth_, 4);
    Put(h, sourceHeight_, 4);
    Put(h, 0, 4);
    Put(h, hash_, 8);
    Put(h, at, 8);
    Put(h, 0, 8);
    Put(h, 0, 8);
    out_.seekp(0);
    out_.write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(h.size()));
    out_.close();
    return !out_.fail();
}

} // namespace rr::hd
