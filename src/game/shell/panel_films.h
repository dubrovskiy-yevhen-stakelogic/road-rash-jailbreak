#pragma once
// The product's side of the film library the panel films call (shell_panel.h)
// - OURS, named: the original's RASHCDF 0x8005F36C (start: the CD stream, 64 KiB buffered before it
// returns), 0x8005F484 (one call = the next MDEC chunk of the stream; its picture is decoded and copied to
// VRAM at (x, y) + the draw buffer's offset by the frame 0x80080274 between the backdrop table and the
// main table, one call late: the chunk found by call n is shown by call n + 1, and the call that meets
// the end of the file answers 0 and shows nothing) and 0x8005F7E0 (stop), with the file layer SLUS
// 0x8001458C / 0x8001460C. Here: the file is read from the player's disc (DATA\FE\<NAME> - the path the
// original builds is DATA\<name>, frontend.md 12 item 3 - else DATA\<NAME>), its MDEC chunks listed, and
// each call queues the picture the original would show this frame for the view (ShellView::FilmShown).
// The drive: the original's call WAITS for the chunk's bytes; the product's frame does not wait - the
// drive is modelled at double speed (150 sectors a second = 5120 bytes a 60 Hz frame, as the race's
// stream model) from the 64 KiB the start buffered, and a call whose chunk has not arrived
// shows the last picture again (the console would still be showing it).
#include "game/shell/shell_view.h"
#include "rrvfs/disc_image.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace rr::shell {

class PanelFilms {
public:
    explicit PanelFilms(const DiscImage& disc) : disc_(disc) {}
    // SLUS 0x8001458C(path, 0): a handle >= 0, or -8 / -1 when the file is not on the disc.
    int32_t OpenFile(const std::string& path);
    void CloseFile(int32_t handle);                                                  // SLUS 0x8001460C
    uint32_t Start(int32_t handle, uint32_t mode, int32_t channel, int32_t x, int32_t y); // 0x8005F36C
    uint32_t Picture(int32_t x, int32_t y, int32_t channel, uint32_t mode, uint32_t count); // 0x8005F484
    void Stop(int32_t channel);                                                      // 0x8005F7E0
    // One shell frame: the drive's bytes arrive, last frame's pictures are forgotten.
    void Frame();
    const std::vector<FilmShown>& Shown() const { return shown_; }
    // This frame's library calls (address, x, y; the stop's x = its channel) - rrverify menuprims compares
    // them with the calls the original's widget pass makes on the same state.
    struct Call {
        uint32_t address = 0;
        int32_t x = 0, y = 0;
    };
    const std::vector<Call>& Calls() const { return calls_; }

    struct Counts {
        uint64_t opens = 0, missing = 0, starts = 0, pictures = 0, holds = 0, ends = 0, stops = 0;
        uint64_t channelled = 0, mode24 = 0; // a start with a sound channel / the 24-bit mode word 0 (not drawn)
    };
    const Counts& Totals() const { return counts_; }
    std::string Report() const;

private:
    struct File {
        std::string name;
        std::vector<uint8_t> bytes;
        std::vector<uint32_t> chunkEnd; // each MDEC chunk's end offset in the file
    };
    const File* Load(const std::string& name);
    const DiscImage& disc_;
    std::map<std::string, File> files_;
    std::map<int32_t, std::string> handles_;
    int32_t nextHandle_ = 0;
    const File* playing_ = nullptr;
    int32_t prepared_ = -1, uploaded_ = -1;
    uint64_t delivered_ = 0;
    bool mode24_ = false;
    std::vector<FilmShown> shown_;
    std::vector<Call> calls_;
    Counts counts_;
};

} // namespace rr::shell
