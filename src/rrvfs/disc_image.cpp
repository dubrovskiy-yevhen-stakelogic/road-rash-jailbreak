#include "rrvfs/disc_image.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#define rr_fseek64 _fseeki64
#define rr_ftell64 _ftelli64
#else
#define rr_fseek64 fseeko
#define rr_ftell64 ftello
#endif

namespace rr {
namespace {

uint32_t ReadU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

std::string ToUpper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(const std::string& s) {
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\0')) --e;
    return s.substr(0, e);
}

} // namespace

DiscImage::DiscImage(const std::string& binPath) {
    file_ = std::fopen(binPath.c_str(), "rb");
    if (!file_) throw std::runtime_error("cannot open disc image: " + binPath);
    rr_fseek64(file_, 0, SEEK_END);
    const int64_t length = rr_ftell64(file_);
    if (length <= 0 || length % kRawSectorSize != 0) {
        std::fclose(file_);
        file_ = nullptr;
        throw std::runtime_error("not a raw 2352-byte-sector BIN image: " + binPath);
    }
    sectorCount_ = static_cast<uint32_t>(length / kRawSectorSize);
    ParseIso();
}

DiscImage::~DiscImage() {
    if (file_) std::fclose(file_);
}

void DiscImage::ReadRawSector(uint32_t lba, uint8_t* out2352) const {
    if (lba >= sectorCount_) throw std::runtime_error("disc read past end of image");
    if (rr_fseek64(file_, static_cast<int64_t>(lba) * kRawSectorSize, SEEK_SET) != 0)
        throw std::runtime_error("seek failed");
    if (std::fread(out2352, 1, kRawSectorSize, file_) != kRawSectorSize)
        throw std::runtime_error("short sector read");
}

void DiscImage::ReadForm1(uint32_t lba, uint64_t byteOffset, uint8_t* out, size_t size) const {
    uint8_t sector[kRawSectorSize];
    uint64_t pos = byteOffset;
    size_t done = 0;
    while (done < size) {
        const uint32_t index = static_cast<uint32_t>(pos / kForm1DataSize);
        const uint32_t inside = static_cast<uint32_t>(pos % kForm1DataSize);
        ReadRawSector(lba + index, sector);
        const size_t take = std::min<size_t>(kForm1DataSize - inside, size - done);
        std::memcpy(out + done, sector + kDataOffset + inside, take);
        done += take;
        pos += take;
    }
}

std::vector<uint8_t> DiscImage::ReadFile(const DiscFile& f) const {
    std::vector<uint8_t> data(f.size);
    if (f.size) ReadForm1(f.lba, 0, data.data(), data.size());
    return data;
}

std::optional<DiscFile> DiscImage::Find(const std::string& path) const {
    std::string want = ToUpper(path);
    constexpr char kBackslash = static_cast<char>(92);
    std::replace(want.begin(), want.end(), kBackslash, '/');
    if (!want.empty() && want.front() != '/') want.insert(want.begin(), '/');
    for (const DiscFile& f : files_)
        if (ToUpper(f.path) == want) return f;
    return std::nullopt;
}

void DiscImage::ParseIso() {
    uint8_t pvd[kForm1DataSize];
    ReadForm1(16, 0, pvd, sizeof(pvd));
    if (std::memcmp(pvd + 1, "CD001", 5) != 0)
        throw std::runtime_error("no ISO9660 primary volume descriptor at LBA 16");
    volumeId_ = Trim(std::string(reinterpret_cast<const char*>(pvd + 40), 32));
    const uint8_t* root = pvd + 156;
    ParseDirectory(ReadU32(root + 2), ReadU32(root + 10), "", 0);
    std::sort(files_.begin(), files_.end(),
              [](const DiscFile& a, const DiscFile& b) { return a.lba < b.lba; });
}

void DiscImage::ParseDirectory(uint32_t lba, uint32_t size, const std::string& prefix, int depth) {
    if (depth > 8) throw std::runtime_error("ISO9660 directory nesting too deep");
    std::vector<uint8_t> blob(size);
    ReadForm1(lba, 0, blob.data(), blob.size());

    struct Child { std::string name; uint32_t lba; uint32_t size; };
    std::vector<Child> subdirs;

    size_t off = 0;
    while (off < blob.size()) {
        const uint8_t recordLength = blob[off];
        if (recordLength == 0) { // padding to the end of the 2048-byte block
            off = (off / kForm1DataSize + 1) * kForm1DataSize;
            continue;
        }
        if (off + recordLength > blob.size()) break;
        const uint8_t* rec = blob.data() + off;
        const uint32_t extentLba = ReadU32(rec + 2);
        const uint32_t extentSize = ReadU32(rec + 10);
        const uint8_t flags = rec[25];
        const uint8_t nameLength = rec[32];
        std::string name(reinterpret_cast<const char*>(rec + 33), nameLength);
        off += recordLength;

        if (nameLength == 1 && (name[0] == '\0' || name[0] == '\1')) continue; // "." and ".."
        if (name.size() > 2 && name.compare(name.size() - 2, 2, ";1") == 0) name.resize(name.size() - 2);
        const std::string full = prefix + "/" + name;
        if (flags & 0x02)
            subdirs.push_back({full, extentLba, extentSize});
        else
            files_.push_back({full, extentLba, extentSize});
    }
    for (const Child& d : subdirs) ParseDirectory(d.lba, d.size, d.name, depth + 1);
}

} // namespace rr
