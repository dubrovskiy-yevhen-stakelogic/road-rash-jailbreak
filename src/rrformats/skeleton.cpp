#include "rrformats/skeleton.h"

#include <stdexcept>
#include <string>

namespace rr {
namespace {

uint32_t ReadU32(std::span<const uint8_t> d, size_t off) {
    if (off + 4 > d.size()) throw std::runtime_error("skeleton: read past end of overlay");
    return static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
           (static_cast<uint32_t>(d[off + 2]) << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
}

} // namespace

SkeletonTable SkeletonTable::FromOverlay(std::span<const uint8_t> overlay) {
    const size_t wordsOffset = kWordsAddress - kOverlayBase;
    const size_t indexOffset = kIndexAddress - kOverlayBase;
    if (indexOffset + kProgramCount * 3 > overlay.size())
        throw std::runtime_error("skeleton: overlay is too short to be RASHCDG.BIN");

    std::vector<uint32_t> words(kWordCount);
    for (size_t i = 0; i < kWordCount; ++i) words[i] = ReadU32(overlay, wordsOffset + i * 4);

    SkeletonTable table;
    for (size_t p = 0; p < kProgramCount; ++p) {
        AttachProgram program;
        program.startWord = overlay[indexOffset + p * 3 + 0];
        program.linkCount = overlay[indexOffset + p * 3 + 1];
        program.passes = overlay[indexOffset + p * 3 + 2];

        for (size_t i = program.startWord; i < kWordCount; ++i) {
            const uint32_t w = words[i];
            if (w == 0) break;
            if ((w & 3) == 3) continue; // control word, not a link
            AttachLink link;
            link.parent = static_cast<uint8_t>((w >> 13) & 0x1F);
            link.vertexOffset = static_cast<uint8_t>(((w >> 6) & 0x70) >> 4);
            link.matrixPart = static_cast<uint8_t>((w >> 23) & 0x1F);
            program.links.push_back(link);
            const size_t have = program.links.size();
            if (program.passes <= 1 && have >= program.linkCount) break;
            if (program.passes > 1 && program.linkCount * program.passes >= 5 &&
                have >= static_cast<size_t>(program.linkCount) * program.passes - 5)
                break;
        }
        table.programs_.push_back(std::move(program));
    }
    return table;
}

std::vector<const AttachProgram*> SkeletonTable::Candidates(size_t partCount) const {
    std::vector<const AttachProgram*> out;
    for (const AttachProgram& p : programs_)
        if (p.PartCount() == partCount) out.push_back(&p);
    return out;
}

} // namespace rr
