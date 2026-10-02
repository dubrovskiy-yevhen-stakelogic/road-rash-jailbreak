// rrgame's side of the HD media pack - see hd_media.h.
#include "hd_media.h"

#include "cheat_menu.h"
#include "platform/app_paths.h"
#include "render/render_target.h"
#include "rrformats/hd_pack.h"
#include "rrvfs/disc_identity.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace rrgame {

namespace {
std::string& OpenedFor() {
    static std::string s;
    return s;
}
} // namespace

void OpenHdPack(const rr::DiscImage& disc) {
    std::string exeSha1;
    for (const rr::DiscPart& p : rr::IdentifyDisc(disc).parts)
        if (p.name == "SLUS_010.53") exeSha1 = p.sha1;
    if (exeSha1.empty() || exeSha1 == OpenedFor()) return;
    OpenedFor() = exeSha1;
    std::filesystem::path dir;
    if (const char* e = std::getenv("RRJB_HD_PACK"); e != nullptr && *e != '\0') dir = e;
#ifdef __ANDROID__
    else dir = rr::platform::DataRoot() / "hd";
#else
    else dir = rr::platform::ExecutableDir() / "runtime" / "hd";
#endif
    std::string why;
    std::shared_ptr<rr::hd::Pack> pack = rr::hd::Pack::Open(dir, exeSha1, why);
    rr::hd::SetPack(pack);
    if (pack) std::printf("hd: HD media pack %s (%s), draw scale %d; %s\n", dir.string().c_str(), pack->Summary().c_str(),
                          rr::hd::DrawScale(), rr::hd::Enabled() ? "on" : "off until 'HD textures and media' is on");
    else std::printf("hd: %s - the original pictures, fonts, HUD and films\n", why.c_str());
}

void ApplyHdSwitch(bool on) {
    const bool was = rr::hd::Active() != nullptr;
    rr::hd::SetEnabled(on);
    const bool now = rr::hd::Active() != nullptr;
    if (was != now) std::printf("hd: HD textures and media %s\n", now ? "ON (the pack's pictures, fonts, HUD and films)" : "off");
}

int HdShellScale() { return rr::hd::Active() != nullptr ? rr::hd::DrawScale() : 1; }

bool UploadHudHd(rr::game::HudHd& hd, const std::vector<rr::game::HudPacket>& packets, const rr::game::HudVram& vram,
                 int32_t originX, int32_t originY, GLuint texture, bool stampCheats, int& uploadedW, int& uploadedH) {
    using rr::game::HudOverlay;
    if (rr::hd::Active() == nullptr) return false;
    static bool reported = false;
    const bool matched = hd.Prepare(vram);
    if (!reported) {
        reported = true;
        std::printf("%s%s\n", hd.Report().c_str(), matched ? "" : " - the HUD's texels enlarged, its edges at the finer grid");
    }
    const int s = rr::hd::DrawScale(), w = HudOverlay::kWidth * s, h = HudOverlay::kHeight * s;
    static std::vector<uint8_t> bytes;
    hd.Rasterize(packets, vram, originX, originY, s, bytes);
    if (stampCheats) { // the tag at the original size, enlarged (cheat_menu.h)
        std::vector<uint8_t> tag(static_cast<size_t>(HudOverlay::kWidth) * HudOverlay::kHeight * 4u, 0);
        StampCheatsTag(tag.data(), HudOverlay::kWidth, HudOverlay::kHeight);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint8_t* t = &tag[(static_cast<size_t>(y / s) * HudOverlay::kWidth + static_cast<size_t>(x / s)) * 4u];
                if (t[3] != 0) std::memcpy(&bytes[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u], t, 4);
            }
    }
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (uploadedW != w || uploadedH != h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, bytes.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        uploadedW = w;
        uploadedH = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, bytes.data());
    }
    rr::render::GenerateMipmap(GL_TEXTURE_2D);
    return true;
}

} // namespace rrgame
