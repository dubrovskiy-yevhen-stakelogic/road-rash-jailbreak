#include "rrvfs/disc_identity.h"

#include "rrvfs/disc_image.h"
#include "rrvfs/sha1.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace rr {
namespace {

// The supported release: Road Rash: Jailbreak (USA), volume SLUS_01053. Sizes and hashes measured on the
// user's disc and recorded in docs\formats\overview.md.
struct KnownPart {
    const char* name;
    unsigned size;
    const char* sha1;
};
constexpr char kVolume[] = "SLUS_01053";
constexpr KnownPart kParts[] = {
    {"SLUS_010.53", 311296, "67ed165a2c517d4e6106fb0dfa324d66dd9a76f1"},
    {"RASHCDF.BIN", 283936, "a3fec4b4e9292c358d0f6dc529843f5d8f25924a"},
    {"RASHCDG.BIN", 467080, "cfe43a7786759f2cb9c57751cf99e84d1074782c"},
    {"RASHCDI.BIN", 79196, "9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06"},
};

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string TrimLine(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    s = s.substr(b);
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF)
        s = s.substr(3); // a UTF-8 BOM (Notepad, PowerShell 5.1 Set-Content)
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
}

// The first *.bin (sorted by name) directly in `dir`, "" when none.
std::string FirstBin(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return {};
    std::vector<std::filesystem::path> bins;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        if (entry.is_regular_file(ec) && Lower(entry.path().extension().string()) == ".bin") bins.push_back(entry.path());
    std::sort(bins.begin(), bins.end());
    return bins.empty() ? std::string() : bins.front().string();
}

} // namespace

DiscIdentity IdentifyDisc(const DiscImage& disc) {
    DiscIdentity id;
    id.volume = disc.VolumeId();
    id.sectors = disc.SectorCount();
    id.files = static_cast<unsigned>(disc.Files().size());
    bool all = true;
    for (const KnownPart& known : kParts) {
        DiscPart part;
        part.name = known.name;
        part.expected = known.sha1;
        if (const auto file = disc.Find(std::string("/") + known.name)) {
            part.present = true;
            part.size = file->size;
            const std::vector<uint8_t> bytes = disc.ReadFile(*file);
            part.sha1 = Sha1Hex(bytes.data(), bytes.size());
            part.matches = part.sha1 == known.sha1 && part.size == known.size;
        }
        all = all && part.matches;
        id.parts.push_back(part);
    }
    if (id.volume != kVolume && !id.parts[0].present) {
        id.reason = "volume " + (id.volume.empty() ? std::string("(none)") : id.volume) +
                    " is not Road Rash: Jailbreak (USA), SLUS_01053";
    } else if (!id.parts[0].present) {
        id.reason = "the executable SLUS_010.53 is missing";
    } else if (!id.parts[0].matches) {
        id.reason = "SLUS_010.53 has SHA-1 " + id.parts[0].sha1 + ", not the supported " + id.parts[0].expected +
                    " (another revision or a modified image)";
    } else if (!all) {
        for (const DiscPart& p : id.parts)
            if (!p.matches) {
                id.reason = p.name + (p.present ? " differs from the supported release (SHA-1 " + p.sha1 + ")"
                                                : std::string(" is missing"));
                break;
            }
    }
    id.supported = all && id.volume == kVolume;
    if (all && id.volume != kVolume) id.reason = "unexpected volume id " + id.volume;
    if (id.supported) id.release = "Road Rash: Jailbreak (USA) SLUS-01053";
    return id;
}

std::string ResolveImagePath(const std::string& path) {
    const std::filesystem::path p(path);
    if (Lower(p.extension().string()) != ".cue") return path;
    std::ifstream in(p);
    if (!in) throw std::runtime_error("cannot read cue sheet: " + path);
    std::string line, bin;
    int dataFiles = 0;
    while (std::getline(in, line)) {
        std::string t = TrimLine(line);
        if (t.size() < 5 || Lower(t.substr(0, 5)) != "file ") continue;
        const size_t q1 = t.find('"'), q2 = t.rfind('"');
        std::string name;
        if (q1 != std::string::npos && q2 > q1) name = t.substr(q1 + 1, q2 - q1 - 1);
        else {
            std::istringstream words(t.substr(5));
            words >> name;
        }
        if (Lower(t).find("binary") == std::string::npos) continue;
        ++dataFiles;
        if (bin.empty()) bin = name;
    }
    if (dataFiles != 1)
        throw std::runtime_error("the cue sheet must name exactly one BINARY file (a single-track MODE2/2352 dump): " + path);
    return (p.parent_path() / bin).string();
}

std::string FindInstalledDisc(const std::string& exeDir, std::string& source) {
    const std::filesystem::path dir(exeDir);
    std::error_code ec;
    const std::filesystem::path txt = dir / "disc.txt";
    if (std::filesystem::is_regular_file(txt, ec)) {
        std::ifstream in(txt);
        std::string line;
        while (std::getline(in, line)) {
            line = TrimLine(line);
            if (line.empty() || line[0] == '#') continue;
            std::filesystem::path named(line);
            if (named.is_relative()) named = dir / named;
            source = "disc.txt";
            return ResolveImagePath(named.string());
        }
    }
    if (std::string bin = FirstBin(dir / "runtime" / "disc"); !bin.empty()) {
        source = "runtime\\disc";
        return bin;
    }
    if (std::string bin = FirstBin(dir / "disc"); !bin.empty()) {
        source = "disc";
        return bin;
    }
    source = "disc.txt, runtime\\disc\\*.bin and disc\\*.bin in " + exeDir;
    return {};
}

} // namespace rr
