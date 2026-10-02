// The product's film library for the panel films (panel_films.h) - OURS, named there.
#include "game/shell/panel_films.h"

#include "game/shell/shell_assets.h"

#include <cstdio>
#include <exception>
#include <optional>

namespace rr::shell {

namespace {
constexpr uint64_t kPrebuffer = 0x40u * 0x400u; // the start's wait: 0x40 * 0x400 bytes (0x8005F36C)
constexpr uint64_t kDrivePerFrame = 5120;       // 2x: 150 sectors * 2048 bytes / 60 frames

std::string Upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}
} // namespace

const PanelFilms::File* PanelFilms::Load(const std::string& name) {
    auto it = files_.find(name);
    if (it != files_.end()) return it->second.bytes.empty() ? nullptr : &it->second;
    File f;
    f.name = name;
    try {
        std::optional<DiscFile> d = disc_.Find("DATA/FE/" + name);
        if (!d) d = disc_.Find("DATA/" + name);
        if (d) {
            f.bytes = disc_.ReadFile(*d);
            for (const auto& c : ListStrChunks(f.bytes))
                f.chunkEnd.push_back(static_cast<uint32_t>(c.data() - f.bytes.data() + c.size()));
        }
    } catch (const std::exception&) {
        f.bytes.clear();
        f.chunkEnd.clear();
    }
    if (f.chunkEnd.empty()) f.bytes.clear();
    auto& slot = files_[name] = std::move(f);
    return slot.bytes.empty() ? nullptr : &slot;
}

int32_t PanelFilms::OpenFile(const std::string& path) {
    const size_t cut = path.find_last_of("\\/");
    const std::string name = Upper(cut == std::string::npos ? path : path.substr(cut + 1));
    if (Load(name) == nullptr) {
        ++counts_.missing;
        return -1;
    }
    int32_t h = 0;
    while (h < 16 && handles_.count(h)) ++h; // the file layer's first free entry of 16 (SLUS 0x80014978)
    if (h == 16) return -1;
    handles_[h] = name;
    ++counts_.opens;
    return h;
}

void PanelFilms::CloseFile(int32_t handle) { handles_.erase(handle); }

uint32_t PanelFilms::Start(int32_t handle, uint32_t mode, int32_t channel, int32_t x, int32_t y) {
    calls_.push_back({0x8005F36Cu, x, y});
    const auto it = handles_.find(handle);
    if (it == handles_.end()) return 0;
    playing_ = Load(it->second);
    if (playing_ == nullptr) return 0;
    ++counts_.starts;
    if (channel != 0) ++counts_.channelled;
    mode24_ = mode == 0;
    if (mode24_) ++counts_.mode24;
    delivered_ = kPrebuffer;
    uploaded_ = -1;
    prepared_ = 0; // the start's own call (count 0): the first chunk found and decoded, nothing shown
    (void)x;
    (void)y;
    return 1;
}

uint32_t PanelFilms::Picture(int32_t x, int32_t y, int32_t /*channel*/, uint32_t /*mode*/, uint32_t /*count*/) {
    calls_.push_back({0x8005F484u, x, y});
    if (playing_ == nullptr) return 0;
    const int32_t next = prepared_ + 1;
    if (next >= static_cast<int32_t>(playing_->chunkEnd.size())) { // the end of the file: 0, nothing shown
        ++counts_.ends;
        return 0;
    }
    if (playing_->chunkEnd[static_cast<size_t>(next)] > delivered_) { // the drive has not delivered it yet
        ++counts_.holds;
        if (uploaded_ >= 0 && !mode24_) shown_.push_back({playing_->name, uploaded_, x, y});
        return 1;
    }
    uploaded_ = prepared_;
    prepared_ = next;
    ++counts_.pictures;
    if (!mode24_) shown_.push_back({playing_->name, uploaded_, x, y});
    return 1;
}

void PanelFilms::Stop(int32_t channel) {
    calls_.push_back({0x8005F7E0u, channel, 0});
    ++counts_.stops;
    playing_ = nullptr;
    prepared_ = uploaded_ = -1;
}

void PanelFilms::Frame() {
    shown_.clear();
    calls_.clear();
    if (playing_ != nullptr) delivered_ += kDrivePerFrame;
}

std::string PanelFilms::Report() const {
    char line[256];
    std::snprintf(line, sizeof(line),
                  "panel films: %llu opened (%llu missing), %llu started, %llu pictures shown, %llu held for the drive, "
                  "%llu ends, %llu stops, %llu with a sound channel, %llu in the 24-bit mode (not drawn)",
                  static_cast<unsigned long long>(counts_.opens), static_cast<unsigned long long>(counts_.missing),
                  static_cast<unsigned long long>(counts_.starts), static_cast<unsigned long long>(counts_.pictures),
                  static_cast<unsigned long long>(counts_.holds), static_cast<unsigned long long>(counts_.ends),
                  static_cast<unsigned long long>(counts_.stops), static_cast<unsigned long long>(counts_.channelled),
                  static_cast<unsigned long long>(counts_.mode24));
    return line;
}

} // namespace rr::shell
