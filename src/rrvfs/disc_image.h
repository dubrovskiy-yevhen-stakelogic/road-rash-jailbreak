#pragma once
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace rr {

struct DiscFile {
    std::string path; // "/DATA/CAR01A.GEO", no ";1" version suffix
    uint32_t lba = 0;
    uint32_t size = 0;
};

// Read-only view of a raw MODE2/2352 PS1 disc image (single data track .bin).
// Road Rash: Jailbreak (USA) is one such track, 249159 sectors, ISO9660 with 2 directories and 487 files.
class DiscImage {
public:
    static constexpr uint32_t kRawSectorSize = 2352;
    static constexpr uint32_t kForm1DataSize = 2048;
    static constexpr uint32_t kForm2DataSize = 2324;
    static constexpr uint32_t kSubheaderOffset = 16;
    static constexpr uint32_t kDataOffset = 24;

    explicit DiscImage(const std::string& binPath);
    ~DiscImage();
    DiscImage(const DiscImage&) = delete;
    DiscImage& operator=(const DiscImage&) = delete;

    uint32_t SectorCount() const { return sectorCount_; }
    const std::string& VolumeId() const { return volumeId_; }
    const std::vector<DiscFile>& Files() const { return files_; }

    // Case-insensitive, accepts "/DATA/X.GEO" or "DATA/X.GEO".
    std::optional<DiscFile> Find(const std::string& path) const;

    void ReadRawSector(uint32_t lba, uint8_t* out2352) const;
    // Form1 user data starting at `lba`, skipping the 24-byte sync+header+subheader of every sector.
    void ReadForm1(uint32_t lba, uint64_t byteOffset, uint8_t* out, size_t size) const;
    std::vector<uint8_t> ReadFile(const DiscFile& file) const;

private:
    void ParseIso();
    void ParseDirectory(uint32_t lba, uint32_t size, const std::string& prefix, int depth);

    std::FILE* file_ = nullptr;
    uint32_t sectorCount_ = 0;
    std::string volumeId_;
    std::vector<DiscFile> files_;
};

} // namespace rr
