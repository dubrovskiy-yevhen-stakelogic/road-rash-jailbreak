// rrgame_settings.ini's sections (settings_file.h).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "settings_file.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace rrgame {

namespace {

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// "[name]" for a header line (trimmed), "" otherwise
std::string HeaderOf(const std::string& line) {
    const std::string t = Trim(line);
    if (t.size() < 2 || t.front() != '[' || t.back() != ']') return {};
    return t.substr(1, t.size() - 2);
}

bool KeyValue(const std::string& line, std::string& key, std::string& value) {
    const std::string t = Trim(line);
    if (t.empty() || t[0] == ';' || t[0] == '#') return false;
    const size_t eq = t.find('=');
    if (eq == std::string::npos) return false;
    key = Trim(t.substr(0, eq));
    value = Trim(t.substr(eq + 1));
    return !key.empty();
}

bool ReplaceFile(const std::string& from, const std::string& to) {
#ifdef _WIN32
    return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code ec;
    std::filesystem::rename(from, to, ec); // POSIX rename replaces the old file atomically
    return !ec;
#endif
}

} // namespace

bool ReadIniLines(const std::string& path, std::vector<std::string>& lines) {
    lines.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    static const bool keepBom = [] {
        const char* v = std::getenv("RRJB_INI_BOM");
        return v != nullptr && std::strcmp(v, "keep") == 0;
    }();
    if (!keepBom && text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);
    std::istringstream s(text);
    std::string line;
    while (std::getline(s, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return true;
}

bool ReadIniSection(const std::string& path, const std::string& name, IniPairs& out, bool* present) {
    out.clear();
    if (present != nullptr) *present = false;
    std::vector<std::string> lines;
    if (!ReadIniLines(path, lines)) return false;
    bool in = false;
    for (const std::string& l : lines) {
        const std::string h = HeaderOf(l);
        if (!h.empty()) {
            in = h == name;
            if (in && present != nullptr) *present = true;
            continue;
        }
        std::string k, v;
        if (in && KeyValue(l, k, v)) out.emplace_back(k, v);
    }
    return true;
}

bool WriteIniSection(const std::string& path, const std::string& name, const std::string& body,
                     const std::vector<std::string>& retired) {
    std::vector<std::string> lines;
    ReadIniLines(path, lines); // (none yet: a new file)
    // the keys this save writes
    std::vector<std::string> written;
    {
        std::istringstream b(body);
        std::string l, k, v;
        while (std::getline(b, l))
            if (KeyValue(l, k, v)) written.push_back(k);
    }
    const auto has = [](const std::vector<std::string>& list, const std::string& k) {
        return std::find(list.begin(), list.end(), k) != list.end();
    };
    std::vector<std::string> keep, carried; // the lines outside the section; the old section's keys this save lacks
    bool in = false;
    for (const std::string& l : lines) {
        const std::string h = HeaderOf(l);
        if (!h.empty()) in = h == name;
        if (!in) {
            keep.push_back(l);
            continue;
        }
        std::string k, v;
        if (!h.empty() || !KeyValue(l, k, v)) continue;
        if (has(written, k) || has(retired, k)) continue;
        bool dup = false;
        for (const std::string& c : carried) dup = dup || c.compare(0, k.size() + 1, k + "=") == 0;
        if (!dup) carried.push_back(k + "=" + v);
    }
    while (!keep.empty() && Trim(keep.back()).empty()) keep.pop_back();
    const std::string temp = path + "." + name + "tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        for (const std::string& l : keep) out << l << "\n";
        if (!keep.empty()) out << "\n";
        out << "[" << name << "]\n" << body;
        if (!body.empty() && body.back() != '\n') out << "\n";
        for (const std::string& c : carried) out << c << "\n";
        if (!out) return false;
    }
    if (ReplaceFile(temp, path)) return true;
    std::error_code ec; // (a runtime that refuses the rename over an existing file: copy, then remove the temporary)
    std::filesystem::copy_file(temp, path, std::filesystem::copy_options::overwrite_existing, ec);
    std::filesystem::remove(temp);
    return !ec;
}

bool SettingsMarkersOff() {
    static const bool off = [] {
        const char* v = std::getenv("RRJB_SETTINGS_MARK");
        return v != nullptr && std::strcmp(v, "off") == 0;
    }();
    return off;
}

} // namespace rrgame
