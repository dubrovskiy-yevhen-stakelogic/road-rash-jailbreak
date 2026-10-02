// rrgame --parity <statedir>: the PRODUCT's frame of a captured console state.
//
// The capture's RAM becomes the session's arena after the first frame (RaceSession::AdoptCapture,
// src\game\parity_session.cpp) and the ordinary frame assembly draws it - cells, the machines and
// riders, props, cars, pedestrians, the effect pass, the HUD - into a 384 x 240 window shown 4:3, the
// console's own draw area pixel for pixel. tools\scout\psxgpu.py draws the original's GPU packets of
// the same state and `psxgpu.py parity` compares the two frames (tests\run_gates.ps1).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

bool ParityPrepare(const std::string& dir, std::vector<uint8_t>& ram, int& raceSet, int& raceId, std::string& err) {
    // a capture directory (its ram.bin), or a RAM image itself - e.g. `rrverify trace --dump-ram-frame 1`'s
    // ram_frame1.bin, the state the traced frame's packets were built from
    const bool file = dir.size() > 4 && dir.compare(dir.size() - 4, 4, ".bin") == 0;
    const std::string path = file ? dir : dir + "/ram.bin";
    ram.assign(0x200000u, 0);
    FILE* f = std::fopen(path.c_str(), "rb");
    const bool read = f != nullptr && std::fread(ram.data(), 1, ram.size(), f) == ram.size();
    if (f != nullptr) std::fclose(f);
    if (!read) {
        err = "parity: " + path + " is not a 2 MiB guest RAM image";
        ram.clear();
        return false;
    }
    const auto word = [&ram](uint32_t a) {
        uint32_t v = 0;
        std::memcpy(&v, ram.data() + (a & 0x1FFFFFu), 4);
        return v;
    };
    // game_state *(0x8005B2F8): the road set +0x30 and the race id +0x40 (as
    // race_session.cpp CheckRouteArena reads them)
    const uint32_t gs = word(0x8005B2F8u);
    if (gs < 0x80000000u || gs >= 0x80200000u) {
        err = "parity: " + path + " holds no game_state pointer at 0x8005B2F8 - not a race capture";
        return false;
    }
    const int set = static_cast<int>(word(gs + 0x30u)), race = static_cast<int>(word(gs + 0x40u));
    if (set < 1 || set > 2 || race < 1 || race > 50) {
        char b[160];
        std::snprintf(b, sizeof(b), "parity: game_state names set %d race %d - not a race capture", set, race);
        err = b;
        return false;
    }
    raceSet = set;
    raceId = race;
    std::printf("parity: %s is set %d race %d; the product draws it from the capture's RAM\n", dir.c_str(), set, race);
    return true;
}
