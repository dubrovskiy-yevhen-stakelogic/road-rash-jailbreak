#include "platform/app_paths.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <system_error>
#include <vector>

namespace rr::platform {
namespace {
std::string g_external = ".", g_internal = ".";
}

void SetAppDirs(const std::string& external, const std::string& internal) {
    g_external = external.empty() ? "." : external;
    g_internal = internal.empty() ? "." : internal;
    std::error_code ec;
    std::filesystem::create_directories(SavesDir(), ec);
}

std::filesystem::path DataRoot() { return g_external; }

std::filesystem::path ExecutableDir() {
#ifdef _WIN32
    char exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const std::string path(exe, n);
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::filesystem::path(".") : std::filesystem::path(path.substr(0, slash));
#else
    return DataRoot();
#endif
}

// docs\QUEST.md headset layout: the memory card, the settings and the controls in the app's INTERNAL files/saves,
// where the USB save provider exposes only the memory card. Windows: saves\ next to the exe.
std::filesystem::path SavesDir() {
    std::error_code ec;
#ifdef _WIN32
    const std::filesystem::path dir = ExecutableDir() / "saves";
#else
    const std::filesystem::path dir = g_internal == "." ? std::filesystem::path("saves") : std::filesystem::path(g_internal) / "saves";
#endif
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::filesystem::path FindDiscImage() {
    std::error_code ec;
    const std::filesystem::path root = DataRoot();
    if (std::filesystem::is_regular_file(root / "disc.bin", ec)) return root / "disc.bin";
    std::vector<std::filesystem::path> found;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string ext = entry.path().extension().string();
        for (char& c : ext) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        if (ext == ".bin" || ext == ".img") found.push_back(entry.path());
    }
    // docs\QUEST.md: the installer copies the image as disc.bin; otherwise the game opens the first one by name
    std::sort(found.begin(), found.end());
    return found.empty() ? std::filesystem::path() : found.front();
}

} // namespace rr::platform
