#pragma once
// Part-attachment programs for RMD3 models.
//
// A .GEO stores every sub-mesh in its own local frame, all sharing the origin, so a raw draw gives
// a pile of overlapping parts. The translation that stands a model up is
//     origin(child) = origin(parent) + verts[ vertBase(parent) + k ]
// and the (parent, k) topology is NOT in the .GEO - it is a static table inside the race overlay
// RASHCDG.BIN, consumed by the original at 0x80067064. Established in docs\formats\rmd3.md section 9.
//
// This repository stores no copy of that table: it is read at run time out of the player's own
// RASHCDG.BIN, exactly like every other game asset.
#include <cstdint>
#include <span>
#include <vector>

namespace rr {

struct AttachLink {
    uint8_t parent = 0;       // (word >> 13) & 0x1F
    uint8_t vertexOffset = 0; // k, from ((word >> 6) & 0x70) >> 4
    // (word >> 23) & 0x1F: WHICH PART'S runtime 3x3 this link loads into the GTE
    // (`RASHCDG 0x80067188`..`0x800671B4`, `parts + idx * 24 + 4`). It is NOT always the child
    // index: in the 5-part bike program the links place parts 1,2,3,4 with the matrices of slots
    // 1,3,4,2, which is what makes the two wheels share one spin angle and leaves the frame
    // unrotated. The 17-part rider program is the case where the two happen to coincide.
    uint8_t matrixPart = 0;
};

// One program places parts 1..N; part 0 is the carrier and stays at the origin.
struct AttachProgram {
    uint8_t startWord = 0;
    uint8_t linkCount = 0;
    uint8_t passes = 0;
    std::vector<AttachLink> links;
    size_t PartCount() const { return links.size() + 1; }
};

class SkeletonTable {
public:
    // `overlay` is the whole RASHCDG.BIN as it sits on the disc.
    static SkeletonTable FromOverlay(std::span<const uint8_t> overlay);

    const std::vector<AttachProgram>& Programs() const { return programs_; }
    // Programs that describe exactly `partCount` parts. Several may match: the original picks by an
    // argument we have not located, so the caller disambiguates (see PickProgram in rmd3.h).
    std::vector<const AttachProgram*> Candidates(size_t partCount) const;

    // Where the table lives, for reference: words at guest 0x800CC790 (RASHCDG.BIN +0x711A8),
    // index at 0x800CC854 (+0x7126C), with the overlay window at 0x8005B5E8.
    static constexpr uint32_t kOverlayBase = 0x8005B5E8;
    static constexpr uint32_t kWordsAddress = 0x800CC790;
    static constexpr uint32_t kIndexAddress = 0x800CC854;
    static constexpr size_t kWordCount = 49;
    static constexpr size_t kProgramCount = 8;

private:
    std::vector<AttachProgram> programs_;
};

} // namespace rr
