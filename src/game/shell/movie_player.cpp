// The front end's films - see movie_player.h.
#include "game/shell/movie_player.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <stdexcept>

#include "game/shell/shell_view.h"
#include "rrformats/audio.h"

namespace rr::shell {
namespace {

constexpr int kDisplayW = 320, kDisplayH = 224, kDisplayTop = 8; // 0x8001BE08(0, 8, 320, 224)
constexpr int kBlanksPerPicture = 4;                             // 15 fps on a 60 Hz display

std::string Upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

} // namespace

bool MoviePlayer::Open(const DiscImage& disc, const std::string& name, int x, int y) {
    Close();
    try {
        std::string path = "DATA/FE/" + Upper(name);
        std::optional<DiscFile> f = disc.Find(path);
        if (!f) f = disc.Find(path = "DATA/" + Upper(name));
        if (!f) return false;
        hdKey_ = path;
        if (!exeBook_) {
            const auto slus = disc.Find("SLUS_010.53");
            if (!slus) return false;
            exeBook_ = std::make_unique<MdecCodebook>(LoadDefaultCodebook(disc.ReadFile(*slus)));
        }
        file_ = disc.ReadFile(*f);
        chunks_ = ListStrChunks(file_);
        book_ = std::make_unique<MdecCodebook>(CodebookForStream(file_, *exeBook_));
        const rr::PcmBuffer pcm = rr::DecodeWve(file_);
        audio_ = pcm.samples;
        audioRate_ = pcm.sampleRate;
        audioChannels_ = pcm.channels;
    } catch (const std::exception&) {
        file_.clear();
        chunks_.clear();
        return false;
    }
    if (chunks_.empty()) return false;
    name_ = name;
    x_ = x;
    y_ = y;
    blanks_ = 0;
    shown_ = -1;
    playing_ = true;
    // HD media: the pack's pictures of exactly this film (its bytes' hash and picture count), at the draw scale.
    hdScale_ = scale_;
    if (rr::hd::Pack* pack = scale_ > 1 ? rr::hd::Active() : nullptr) {
        if (const rr::hd::Entry* e = pack->Movie(hdKey_, rr::hd::SourceHashBytes(file_), static_cast<int>(chunks_.size()))) {
            auto reader = std::make_shared<rr::hd::MovieReader>();
            std::string error;
            if (reader->Open(pack->PathOf(*e), &error) && reader->frames == chunks_.size() &&
                reader->width == reader->sourceWidth * rr::hd::kScale && reader->height == reader->sourceHeight * rr::hd::kScale)
                hdReader_ = reader;
        }
    }
    Decode(0);
    return true;
}

MoviePlayer::~MoviePlayer() {
    if (hdNext_.valid()) hdNext_.wait();
}

// The worker: picture k of the HD film, reduced to the draw scale (a 4x pack drawn at 2x on the Quest).
void MoviePlayer::RequestHd(int k) {
    if (!hdReader_ || k < 0 || k >= static_cast<int>(hdReader_->frames)) return;
    if (hdNext_.valid()) hdNext_.wait();
    hdNextIndex_ = k;
    hdNext_ = std::async(std::launch::async, [reader = hdReader_, k, scale = hdScale_]() -> std::shared_ptr<const rr::hd::Rgba> {
        auto p = std::make_shared<rr::hd::Rgba>();
        if (!reader->Frame(static_cast<uint32_t>(k), *p)) return nullptr;
        if (scale != rr::hd::kScale) *p = rr::hd::Reduce(*p, rr::hd::kScale / scale);
        return p;
    });
}

void MoviePlayer::Close() {
    if (hdNext_.valid()) hdNext_.wait();
    hdNext_ = {};
    hdNextIndex_ = -1;
    hdReader_.reset();
    hdPicture_.reset();
    playing_ = false;
    file_.clear();
    chunks_.clear();
    audio_.clear();
    picture_ = Picture15{};
    shown_ = -1;
}

void MoviePlayer::Decode(int k) {
    if (k < 0 || k >= static_cast<int>(chunks_.size()) || k == shown_) return;
    try {
        picture_ = DecodeMdecChunk(chunks_[static_cast<size_t>(k)], *book_);
    } catch (const std::exception&) {
        // a frame that does not decode keeps the previous picture (video.md 5: none on this disc)
    }
    shown_ = k;
    if (hdReader_) { // HD media: this picture from the worker (asked for one picture ago), then the next one
        if (hdNextIndex_ != k) RequestHd(k);
        if (hdNext_.valid()) {
            std::shared_ptr<const rr::hd::Rgba> p = hdNext_.get();
            if (p) hdPicture_ = std::move(p);
        }
        hdNextIndex_ = -1;
        RequestHd(k + 1);
    }
}

bool MoviePlayer::Tick() {
    if (!playing_) return false;
    ++blanks_;
    const int k = blanks_ / kBlanksPerPicture;
    if (k >= static_cast<int>(chunks_.size())) {
        playing_ = false;
        return false;
    }
    Decode(k);
    return true;
}

void MoviePlayer::Draw(std::vector<uint8_t>& rgba) const {
    if (scale_ > 1) return DrawScaled(rgba);
    const int W = ShellView::kWidth, H = ShellView::kHeight;
    rgba.assign(static_cast<size_t>(W) * H * 4u, 0);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    if (picture_.width <= 0 || picture_.height <= 0) return;
    for (int oy = 0; oy < H; ++oy) {
        const int dy = oy - kDisplayTop;
        if (dy < 0 || dy >= kDisplayH) continue;
        const int py = dy - y_;
        if (py < 0 || py >= picture_.height) continue;
        for (int ox = 0; ox < W; ++ox) {
            const int px = ox * kDisplayW / W - x_;
            if (px < 0 || px >= picture_.width) continue;
            const uint16_t p = picture_.px[static_cast<size_t>(py) * static_cast<size_t>(picture_.width) + static_cast<size_t>(px)];
            const uint32_t r = p & 31u, g = (p >> 5) & 31u, b = (p >> 10) & 31u;
            uint8_t* d = &rgba[(static_cast<size_t>(oy) * W + static_cast<size_t>(ox)) * 4u];
            d[0] = static_cast<uint8_t>((r << 3) | (r >> 2));
            d[1] = static_cast<uint8_t>((g << 3) | (g >> 2));
            d[2] = static_cast<uint8_t>((b << 3) | (b >> 2));
            d[3] = 255;
        }
    }
}

// HD media: the same display at `scale_` times the shell frame - the HD picture when the film has one (1:1 vertically,
// stretched across the width as the 320-wide display is), else the original picture's texels enlarged.
void MoviePlayer::DrawScaled(std::vector<uint8_t>& rgba) const {
    const int s = scale_, W = ShellView::kWidth * s, H = ShellView::kHeight * s;
    rgba.assign(static_cast<size_t>(W) * static_cast<size_t>(H) * 4u, 0);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    if (picture_.width <= 0 || picture_.height <= 0) return;
    const rr::hd::Rgba* hd = hdPicture_ && hdScale_ == s && hdPicture_->width == picture_.width * s ? hdPicture_.get() : nullptr;
    for (int oy = 0; oy < H; ++oy) {
        const int dy = oy - kDisplayTop * s;
        if (dy < 0 || dy >= kDisplayH * s) continue;
        const int py = dy - y_ * s;
        if (py < 0 || py >= picture_.height * s) continue;
        for (int ox = 0; ox < W; ++ox) {
            const int px = ox * kDisplayW / ShellView::kWidth - x_ * s; // in picture texels times s
            if (px < 0 || px >= picture_.width * s) continue;
            uint8_t* d = &rgba[(static_cast<size_t>(oy) * static_cast<size_t>(W) + static_cast<size_t>(ox)) * 4u];
            if (hd != nullptr) {
                std::memcpy(d, &hd->px[(static_cast<size_t>(py) * static_cast<size_t>(hd->width) + static_cast<size_t>(px)) * 4u], 3);
                d[3] = 255;
                continue;
            }
            const uint16_t p = picture_.px[static_cast<size_t>(py / s) * static_cast<size_t>(picture_.width) + static_cast<size_t>(px / s)];
            const uint32_t r = p & 31u, g = (p >> 5) & 31u, b = (p >> 10) & 31u;
            d[0] = static_cast<uint8_t>((r << 3) | (r >> 2));
            d[1] = static_cast<uint8_t>((g << 3) | (g >> 2));
            d[2] = static_cast<uint8_t>((b << 3) | (b >> 2));
            d[3] = 255;
        }
    }
}

} // namespace rr::shell
