#pragma once
// The front end's sound: the UI clicks PlayUiSound RASHCDF 0x8007EAC0 asks for and the menu music of
// MusicPlay 0x8007EDE0 / MusicStop 0x8007F158, played from the player's own DATA\FRONTEND.VUK and
// DATA\FEALBUM.ALB.
//
// The original's way, PORTED: PlayUiSound 0x8007EAC0 is StartVoice SLUS 0x8001F174(bank *(0x800A0854),
// n < 15, {pitch -1, the effects slider 0x800D6C0C, pan 0x40}); MusicPlay 0x8007EDE0 with its loader
// 0x8007EEB8 and the chunk callbacks 0x8007F028 / 0x8007F0D0 puts the whole track - 28 chunks of 16 KiB,
// each split into its left and right 8 KiB - into SPU RAM at 0xFFB0 / 0x47FB0 + 0x2000 * chunk, and from
// the 12th chunk on starts two streamed voices there (StreamVoiceStart SLUS 0x8001F37C, pitch 0x5CE, the
// music slider 0x800D6C14, pan 0 / 0x7F; KeyOnHandles 0x8001F544): the track then plays - and LOOPS - the
// way the SPU plays it, by the ADPCM blocks' own loop flags. MusicStop 0x8007F158 stops them through
// SLUS 0x8001F5D4 (StreamVoiceStop, PORTED here: the ADSR2 release 0x1FC0 when leaving the jukebox, then
// StopVoice and reverb off). The voices reach the SPU through the PORTED SoundService SLUS 0x8001EE94 and
// libspu's register writes (sound_engine.h) into the project's SPU voice model (mixer.h), rendered by the
// host's mixer.
//
// OURS, named: the sound system's own arena (the shell's sound state lives at the original's
// addresses, but in a 2 MiB block of this class's, beside the player's SLUS_010.53 image, with the
// shell arena's sliders and screen copied in each frame); SoundInit SLUS 0x8001E614 and libspu's init
// (the layout is written: 12 bank slots at the capture's 0x800FAD0C, 24 voices at 0x800FAD44, the
// bank record at 0x8016D8E4 - where the capture's allocator put them - its samples at SPU 0x1010, the
// capture's base, patched by the PORTED PatchBank); the CD delivers a track's 28 chunks at once; the
// service runs once per shell frame (the console's vsync path 0x80064C30 is not ported).
#include "game/audio/mixer.h"
#include "game/shell/shell_logic.h"
#include "rrvfs/disc_image.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rr::shell {

// The sound options' two SLUS leaves the shell calls, PORTED (rows shell_jukebox_set / shell_sound_mode):
// SLUS 0x8002490C JukeboxSet(track, on) - bit 0 of the album table's flag word 0x80053588 + 12 * track
// (tracks below *(0x80053578)); SLUS 0x8001EFDC SoundModeSet(mono) - the word 0x800D69F4.
void JukeboxSet(GuestRam& g, uint32_t track, int32_t on);
void SoundModeSet(GuestRam& g, uint32_t mono);

class ShellSound {
public:
    explicit ShellSound(const DiscImage& disc);
    ~ShellSound();
    // One shell frame: the clicks and the music the frame asked for (ProductCallees' queues), played
    // into `mixer`. `g` is the shell arena (the sliders and the current screen are read from it).
    // `clicks` holds, in the order the shell made them, PlayUiSound indices (0..14) and the sound
    // options' three calls, each as its address followed by its arguments: 0x8007EB1C kind value,
    // 0x8007ECFC, 0x8007ED34 mode (front_end.cpp ProductCallees).
    void Frame(GuestRam& g, const std::vector<uint32_t>& clicks, int musicRequest, bool musicStop, rr::audio::Mixer& mixer);
    // The race is about to start: the music and the clicks stop.
    void Stop(rr::audio::Mixer& mixer);
    bool Ready() const { return ready_; }
    int MusicTrack() const { return track_; }
    bool MusicPlaying() const;
    size_t Clicks() const { return clicks_; }
    const std::string& Error() const { return error_; }
    std::shared_ptr<rr::audio::SpuVoices> Spu() const { return spu_; }

private:
    class Io;
    void Reset();
    void PlayUiSound(uint32_t n);                      // RASHCDF 0x8007EAC0
    void MusicPlay(uint32_t track);                    // RASHCDF 0x8007EDE0 (+ 0x8007EEB8)
    void MusicStop(int32_t flag, bool jukebox);        // RASHCDF 0x8007F158
    void StreamStop(uint32_t handle, int32_t fade);    // SLUS 0x8001F5D4
    void Deliver();                                    // the chunk callbacks 0x8007F028 / 0x8007F0D0
    void SliderPreview(int32_t kind, int32_t value);   // RASHCDF 0x8007EB1C
    void PreviewStop();                                // RASHCDF 0x8007ECFC
    void SoundModeUi(int32_t mode);                    // RASHCDF 0x8007ED34
    const DiscImage& disc_;
    std::shared_ptr<rr::audio::SpuVoices> spu_;
    std::vector<uint8_t> ram_;
    std::unique_ptr<Io> io_;
    std::vector<uint8_t> bank_, album_, exe_;
    uint16_t spuControl_[14] = {};
    uint32_t timer_ = 0;
    rr::audio::VoiceId spuVoice_ = 0;
    int track_ = -1;
    bool loading_ = false;
    size_t clicks_ = 0;
    bool ready_ = false;
    std::string error_;
};

} // namespace rr::shell
