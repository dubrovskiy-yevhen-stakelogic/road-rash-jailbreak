#pragma once
// SHA-1 (FIPS 180-4), written for this project. Used to identify the player's disc by the hashes of its
// executable and overlays (disc_identity.h); not used for anything security related.
#include <cstddef>
#include <cstdint>
#include <string>

namespace rr {

class Sha1 {
public:
    Sha1();
    void Update(const void* data, size_t size);
    // Lower-case hex of the 20-byte digest. The object is finished afterwards.
    std::string HexDigest();

private:
    void Block(const uint8_t* block);

    uint32_t h_[5];
    uint8_t buffer_[64];
    size_t buffered_ = 0;
    uint64_t totalBytes_ = 0;
};

std::string Sha1Hex(const void* data, size_t size);

} // namespace rr
