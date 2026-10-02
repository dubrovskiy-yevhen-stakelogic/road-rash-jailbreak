// The PC graphics settings (graphics_settings.h).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "graphics_settings.h"

#include "platform/app_paths.h"
#include "settings_file.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace rrgame {

GraphicsSettings GraphicsSettings::Original() { return GraphicsSettings{}; }

GraphicsSettings GraphicsSettings::Modern() {
    GraphicsSettings s;
    s.renderScale = 100;
    s.renderHeight = 0;
    s.fullscreen = false;
    s.wide = true;
    s.vsync = true;
    s.fpsCap = 60; // the cadence every scripted run and gate steps the simulation at (5 ticks of 1/300 s)
    s.msaa = 4;
    s.smoothTextures = true;
    s.ps1Dither = false;
    s.ps1Colour = false;
    s.preciseVertices = true;
    s.affine = false;
    s.ps1DrawOrder = false;
    s.maxDetail = true;
    s.drawDistance = 1;
    s.hdMedia = true; // used only when an HD pack is installed (hd_media.h)
    s.profiler = false;
    return s;
}

namespace {

bool ParseBool(const std::string& v, bool& out) {
    if (v == "1" || v == "on" || v == "true" || v == "yes") return out = true, true;
    if (v == "0" || v == "off" || v == "false" || v == "no") return out = false, true;
    return false;
}

bool ParseInt(const std::string& v, int& out) {
    if (v.empty()) return false;
    char* end = nullptr;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') return false;
    out = static_cast<int>(n);
    return true;
}

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

const char* const kDistanceNames[3] = {"original", "extended", "maximum"};

} // namespace

bool GraphicsSettings::Parse(const std::string& key, const std::string& value) {
    const std::string v = Trim(value);
    int n = 0;
    if (key == "render_scale") {
        if (!ParseInt(v, n) || n < 50 || n > 200) return false;
        renderScale = n;
        return true;
    }
    if (key == "render_height") {
        if (!ParseInt(v, n) || (n != 0 && n != 720 && n != 1080 && n != 1440 && n != 2160)) return false;
        renderHeight = n;
        return true;
    }
    if (key == "fps_cap") {
        if (!ParseInt(v, n) || n < 0 || n > 1000) return false;
        fpsCap = n;
        return true;
    }
    if (key == "msaa") {
        if (!ParseInt(v, n) || (n != 0 && n != 1 && n != 2 && n != 4 && n != 8)) return false;
        msaa = std::max(1, n);
        return true;
    }
    if (key == "textures") {
        if (v == "nearest") return smoothTextures = false, true;
        if (v == "smooth") return smoothTextures = true, true;
        return false;
    }
    if (key == "draw_order") {
        if (v == "ps1") return ps1DrawOrder = true, true;
        if (v == "depth") return ps1DrawOrder = false, true;
        return false;
    }
    if (key == "draw_distance") {
        for (int k = 0; k < 3; ++k)
            if (v == kDistanceNames[k]) return drawDistance = k, true;
        return false;
    }
    if (key == "colour") {
        if (v == "15bit") return ps1Colour = true, true;
        if (v == "24bit") return ps1Colour = false, true;
        return false;
    }
    bool* flag = key == "fullscreen"         ? &fullscreen
                 : key == "widescreen"       ? &wide
                 : key == "vsync"            ? &vsync
                 : key == "dither"           ? &ps1Dither
                 : key == "precise_vertices" ? &preciseVertices
                 : key == "affine"           ? &affine
                 : key == "max_detail"       ? &maxDetail
                 : key == "profiler"         ? &profiler
                 : key == "hd_media"         ? &hdMedia
                                             : nullptr;
    return flag != nullptr && ParseBool(v, *flag);
}

std::string GraphicsSettings::Serialize() const {
    std::ostringstream o;
    o << "render_scale=" << renderScale << "\n"
      << "render_height=" << renderHeight << "\n"
      << "fullscreen=" << (fullscreen ? 1 : 0) << "\n"
      << "widescreen=" << (wide ? 1 : 0) << "\n"
      << "vsync=" << (vsync ? 1 : 0) << "\n"
      << "fps_cap=" << fpsCap << "\n"
      << "msaa=" << msaa << "\n"
      << "textures=" << (smoothTextures ? "smooth" : "nearest") << "\n"
      << "colour=" << (ps1Colour ? "15bit" : "24bit") << "\n"
      << "dither=" << (ps1Dither ? 1 : 0) << "\n"
      << "precise_vertices=" << (preciseVertices ? 1 : 0) << "\n"
      << "affine=" << (affine ? 1 : 0) << "\n"
      << "draw_order=" << (ps1DrawOrder ? "ps1" : "depth") << "\n"
      << "max_detail=" << (maxDetail ? 1 : 0) << "\n"
      << "draw_distance=" << kDistanceNames[std::clamp(drawDistance, 0, 2)] << "\n"
      << "hd_media=" << (hdMedia ? 1 : 0) << "\n"
      << "profiler=" << (profiler ? 1 : 0) << "\n";
    return o.str();
}

std::string GraphicsSettings::Describe() const {
    char b[512];
    char res[48];
    if (renderHeight > 0) std::snprintf(res, sizeof(res), "fixed %dp", renderHeight);
    else std::snprintf(res, sizeof(res), "%d%% of the window", renderScale);
    std::snprintf(b, sizeof(b),
                  "graphics: %s; resolution %s, %s, %s, vsync %s, fps cap %s; MSAA %dx, textures %s, %s colour%s, "
                  "vertices %s, mapping %s, draw order %s; detail %s, draw distance %s; profiler %s",
                  IsOriginal() ? "ORIGINAL (today's frame)" : (*this == Modern() ? "modern" : "custom"), res,
                  fullscreen ? "fullscreen" : "windowed", wide ? "widescreen" : "4:3", vsync ? "on" : "off",
                  fpsCap > 0 ? std::to_string(fpsCap).c_str() : "none", msaa, smoothTextures ? "smooth + mipmaps" : "PS1 nearest",
                  ps1Colour ? "15-bit" : "24-bit", ps1Colour ? (ps1Dither ? " + dither" : "") : "",
                  preciseVertices ? "precise" : "GTE integer", affine ? "PS1 affine" : "perspective",
                  ps1DrawOrder ? "PS1 ordering table" : "depth buffer", maxDetail ? "MAXIMUM" : "original",
                  kDistanceNames[std::clamp(drawDistance, 0, 2)], profiler ? "on" : "off");
    return b;
}

GraphicsSettings& Graphics() {
    static GraphicsSettings s;
    return s;
}

std::string SettingsPath() {
    if (const char* p = std::getenv("RRJB_SETTINGS"); p != nullptr && *p != '\0') return p;
#ifndef _WIN32
    return (rr::platform::SavesDir() / "rrgame_settings.ini").string(); // the Quest: files/saves (docs\QUEST.md)
#else
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    const size_t slash = dir.find_last_of("\\/");
    dir = slash == std::string::npos ? std::string(".") : dir.substr(0, slash);
    // Settings live with the saves in saves\ next to the exe (the installed layout, scripts\install.ps1); a file
    // from before that, beside the exe, is copied there once (fail-if-exists: never overwrites) and left in place.
    CreateDirectoryA((dir + "\\saves").c_str(), nullptr);
    CopyFileA((dir + "\\rrgame_settings.ini").c_str(), (dir + "\\saves\\rrgame_settings.ini").c_str(), TRUE);
    return dir + "\\saves\\rrgame_settings.ini";
#endif
}

bool LoadGraphics(const std::string& path, GraphicsSettings& s) {
    IniPairs kv;
    if (!ReadIniSection(path, "graphics", kv)) return false;
    for (const auto& [key, value] : kv)
        if (!s.Parse(key, value)) std::fprintf(stderr, "settings: %s: ignored '%s=%s'\n", path.c_str(), key.c_str(), value.c_str());
    return true;
}

bool SaveGraphics(const std::string& path, const GraphicsSettings& s) { return WriteIniSection(path, "graphics", s.Serialize()); }

namespace {
std::vector<std::pair<std::string, std::string>>& Overrides() { // the command line's single values (flag order free)
    static std::vector<std::pair<std::string, std::string>> o;
    return o;
}
std::string& Preset() { // the command line's --gfx, "" when none
    static std::string p;
    return p;
}
} // namespace

GraphicsSettings& StartGraphics(bool scripted) {
    static bool loaded = false;
    static GraphicsSettings fromFile;
    GraphicsSettings& s = Graphics();
    if (scripted) {
        s = GraphicsSettings::Original();
    } else {
        if (!loaded) { // once a process: the front end and every race of it share the settings
            loaded = true;
            fromFile = GraphicsSettings::Modern();
            if (LoadGraphics(SettingsPath(), fromFile)) std::printf("settings: %s\n", SettingsPath().c_str());
            else std::printf("settings: no %s yet - the modern defaults\n", SettingsPath().c_str());
            s = fromFile;
        }
    }
    if (Preset() == "original") s = GraphicsSettings::Original();
    else if (Preset() == "modern") s = GraphicsSettings::Modern();
    for (const auto& o : Overrides()) s.Parse(o.first, o.second);
    return s;
}

bool GraphicsFlagGiven(const std::string& key) {
    for (const auto& o : Overrides())
        if (o.first == key) return true;
    return false;
}

bool ApplyGraphicsFlag(int argc, char** argv, int& i, GraphicsSettings& s) {
    std::vector<std::pair<std::string, std::string>>& overrides = Overrides();
    const std::string a = argv[i];
    const auto value = [&]() -> std::string {
        if (i + 1 >= argc) throw std::runtime_error(a + " needs a value");
        return argv[++i];
    };
    if (a == "--gfx") {
        const std::string v = value();
        if (v == "original") s = GraphicsSettings::Original();
        else if (v == "modern") s = GraphicsSettings::Modern();
        else throw std::runtime_error("--gfx: 'original' or 'modern', not '" + v + "'");
        Preset() = v;
        for (const auto& o : overrides) s.Parse(o.first, o.second);
        return true;
    }
    static const char* const kKeys[][2] = {
        {"--render-scale", "render_scale"}, {"--render-height", "render_height"}, {"--fullscreen", "fullscreen"},
        {"--vsync", "vsync"},               {"--fps-cap", "fps_cap"},             {"--msaa", "msaa"},
        {"--textures", "textures"},         {"--precise-vertices", "precise_vertices"}, {"--affine", "affine"},
        {"--draw-order", "draw_order"},     {"--max-detail", "max_detail"},       {"--draw-distance", "draw_distance"},
        {"--profiler", "profiler"},         {"--hd-media", "hd_media"}};
    for (const auto& k : kKeys)
        if (a == k[0]) {
            const std::string v = value();
            if (!s.Parse(k[1], v)) throw std::runtime_error(a + ": bad value '" + v + "'");
            overrides.emplace_back(k[1], v);
            return true;
        }
    return false;
}

} // namespace rrgame
