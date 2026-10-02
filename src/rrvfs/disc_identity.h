#pragma once
// Which disc the player supplied, and where the installed game finds it.
//
// IdentifyDisc reads the executable and the three overlays through DiscImage and compares their
// SHA-1 with the one supported release (docs\formats\overview.md). It is what `rrtool identify` and the
// installer (scripts\install.ps1) use to accept or reject an image; the verdict is by bytes, never by
// file name.
//
// FindInstalledDisc is the lookup rrgame uses when its command line names no image:
//   1. disc.txt next to rrgame.exe (first non-empty line not starting with '#'; relative to the exe),
//   2. runtime\disc\*.bin next to rrgame.exe (the folder scripts\install.ps1 fills),
//   3. disc\*.bin next to rrgame.exe.
// A .cue anywhere in that chain is followed to its single BINARY data file.
#include <string>
#include <vector>

namespace rr {

class DiscImage;

struct DiscPart {
    std::string name;      // "SLUS_010.53", "RASHCDF.BIN", ...
    std::string expected;  // SHA-1 of the supported release
    std::string sha1;      // SHA-1 read from the image ("" when the file is missing)
    unsigned size = 0;
    bool present = false;
    bool matches = false;
};

struct DiscIdentity {
    std::string volume;
    unsigned sectors = 0;
    unsigned files = 0;
    std::vector<DiscPart> parts; // the executable first, then RASHCDF / RASHCDG / RASHCDI
    bool supported = false;
    std::string release; // "Road Rash: Jailbreak (USA) SLUS-01053" when supported
    std::string reason;  // why not, when not
};

DiscIdentity IdentifyDisc(const DiscImage& disc);

// A .cue is followed to the one "FILE ... BINARY" it names (relative to the cue); any other path is
// returned unchanged. Throws std::runtime_error for a cue with no, or more than one, data file.
std::string ResolveImagePath(const std::string& path);

// The lookup described above. Returns "" when nothing is found; `source` says which rule matched
// ("disc.txt", "runtime\\disc", "disc") or, when nothing matched, what was searched.
std::string FindInstalledDisc(const std::string& exeDir, std::string& source);

} // namespace rr
