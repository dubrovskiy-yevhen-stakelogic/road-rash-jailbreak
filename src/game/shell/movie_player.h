#pragma once
// The front end's films: the eleven .WVE cut-scenes the
// twelve movie screens play - the EA logo and the intro at boot, the intro again in the attract loop, the
// career's story films and the credits.
//
// What is the original's and what is ours:
//   * WHEN a film plays, what skips it and where the shell goes after it are the original's: the movie
//     screens' transition RASHCDF 0x8006DB5C opens the film named by the screen's movie widget (+0x10
//     indexes the 42-name array 0x8008973C, "DATA\%s" at 0x8005BF84) and sets fe+0x0A bit 0; the PORTED
//     per-screen handlers (MovieInput 0x8006A8FC: any button sets bit 2; LogoInput 0x8006AAF8 for the EA
//     logo: no button) decide the skip and the advance; the tick 0x8006DE5C closes the film on bit 2 or
//     at its end and clears the bits - transcribed in front_end.cpp (FrontEnd::MovieSweep);
//   * the PICTURES are the project's MDEC decoder (shell_assets.h: the EXE's code book, the film's own
//     VLC0 symbol table, the EXE's quantisation tables and IDCT matrix; video.md 5: every frame of all
//     eleven films decodes exactly), shown at 15 frames a second (one per 4 vertical blanks, video.md
//     6.1) at the widget's position in the 320 x 224 display the transition sets up (0x8001BE08(0, 8,
//     320, 224)); the SOUND is the project's .WVE decoder (audio.md 2: 22050 Hz stereo SPU-ADPCM) played
//     by the host. The library player 0x8005F36C / 0x8005F484 (the CD stream, the MDEC DMA, the XA-less
//     SPU streaming) is not ported: this class stands in for it, named.
#include <cstdint>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "game/shell/shell_assets.h"
#include "rrformats/hd_pack.h"
#include "rrvfs/disc_image.h"

namespace rr::shell {

class MoviePlayer {
public:
    // Opens `name` (the name array's entry, e.g. "intro.wve") from DATA\FE of the player's disc (the path
    // the original builds is "DATA\<name>", frontend.md 12 item 3: the files are in DATA\FE). `x`, `y`
    // are the picture's position in the display (the movie widget's +0x1C / +0x1E). False when the file
    // is missing or its chunk chain does not parse - the caller then takes the original's no-film path.
    bool Open(const DiscImage& disc, const std::string& name, int x, int y);
    void Close();
    bool Playing() const { return playing_; }
    // One 60 Hz vertical blank. False once the last picture has had its four blanks: the film is over.
    bool Tick();
    // The shell frame (ShellView::kWidth x kHeight RGBA) with the film's display in it: the 320 x 224
    // display stretched across the width, 8 lines of border above and below.
    void Draw(std::vector<uint8_t>& rgba) const;
    // The film's sound, interleaved 16-bit, decoded at Open.
    const std::vector<int16_t>& Audio() const { return audio_; }
    int AudioRate() const { return audioRate_; }
    int AudioChannels() const { return audioChannels_; }
    const std::string& Name() const { return name_; }
    int Frame() const { return shown_; }
    int Frames() const { return static_cast<int>(chunks_.size()); }

    // HD media (docs\HD-MEDIA.md): Draw fills a frame `scale` times the shell's (ShellView::SetScale); a film opened
    // while HD media are on plays the active pack's enlarged pictures when the pack has them for exactly this file
    // (its source hash and picture count), each decoded on a worker thread while the one before it shows. The sound
    // and the timing are the film's own either way. A film already playing keeps what it opened with.
    void SetScale(int scale) { scale_ = (scale == 2 || scale == 4) ? scale : 1; }
    bool PlayingHd() const { return hdReader_ != nullptr; }
    ~MoviePlayer();

private:
    void Decode(int k);
    void RequestHd(int k);
    void DrawScaled(std::vector<uint8_t>& rgba) const;
    std::string hdKey_; // the film's disc path, the pack's key
    int scale_ = 1, hdScale_ = 1;
    std::shared_ptr<rr::hd::MovieReader> hdReader_;
    std::shared_ptr<const rr::hd::Rgba> hdPicture_;
    std::future<std::shared_ptr<const rr::hd::Rgba>> hdNext_;
    int hdNextIndex_ = -1;
    std::vector<uint8_t> file_;
    std::vector<std::span<const uint8_t>> chunks_;
    std::unique_ptr<MdecCodebook> exeBook_, book_;
    Picture15 picture_;
    std::vector<int16_t> audio_;
    int audioRate_ = 0, audioChannels_ = 0;
    std::string name_;
    int x_ = 0, y_ = 0;
    int blanks_ = 0, shown_ = -1;
    bool playing_ = false;
};

} // namespace rr::shell
