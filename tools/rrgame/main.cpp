// rrgame - the product.
//
//   rrgame <disc.bin> [--race <set> <id>] [options]
//
// It assembles the world of one of the game's 100 races out of the player's own disc image, puts the
// player on it, and runs a fixed-timestep loop that calls the PORTED original simulation and the
// PORTED race camera (ViewUpdate RASHCDG 0x800881B4), draws the frame with the SAME renderer `rrview` checks per pixel, mixes the game's own
// audio and draws the HUD the PORTED HudFrame RASHCDG 0x8005E848 decides each frame.
//
// It is honest about its seams. `src\game\race_session.h` lists, function by function and address
// by address, what in the frame is the original ported bit-exact and what is a stand-in of ours
// for something not ported yet; the session records every unported callee the frame actually asked
// for, and this program prints that list at the end of the run and writes it into `--log`. The
// world lives in one guest arena and the whole per-bike step `RASHCDG 0x80075EE0` runs in it, with
// the route records and the finish record the loader builds from the disc.
//
// Portable: the window, the keyboard, the controllers and the
// frame's presentation come from the process's host (game_host.h) - the desktop's Win32 window, or the VR host
// (OpenXR on Windows and the Quest, or the desktop VR mock). `main` is main_desktop.cpp's (Windows) and
// main_android.cpp's android_main (the Quest).
#include "render/gl_api.h"

#include "game/audio/mixer.h"
#include "game/race_session.h"
#include "game/pause_product.h" // the pause menu
#include "game/rider_pose.h"
#include "game/head_camera.h" // the head camera
#include "game/bike_pose_product.h" // the bike's part slots
#include "game/ai_race.h"
#include "game/cop_race.h" // --shadow (a scripted player beside a rival)
#include "game/grid_loader.h" // --ridercheck
#include "game/jail_session.h" // the sidecar rig's palette (PlayerPaletteBlock)
#include "game/weapon_session.h" // --weapon, the weapon log line
#include "game/solid_product.h" // the solid objects: log line and totals
#include "game/animobj_product.h" // the animated objects: totals
#include "game/junction_product.h" // the junctions: totals line
#include "game/takedown_product.h" // the takedowns: totals
#include "game/strike_product.h" // the strikes: totals
#include "game/world.h"
#include "platform/audio_device.h"
#include "platform/gamepad_state.h" // the controllers' record and the key codes (the host reads the devices)
#include "platform/png.h"
#include "render/gl_api.h"
#include "render/mat4.h"
#include "render/race_scene.h"
#include "render/traffic_draw.h"
#include "render/ped_draw.h" // the pedestrians' draw
#include "render/fx_draw.h"
#include "render/edge_rule.h" // the polygon edge rule
#include "game/fx_runtime.h"
#include "game/sky_product.h" // the sky
#include "render/sky_gpu.h"
#include "game/model_runtime.h" // the model draw before the effect pass
#include "game/cell_view.h" // the cells each view draws
#include "game/cell_sort_product.h" // the frame's cell sort
#include "game/world_pop_product.h" // props and volumes from the PORTED cell walker
#include "game/hazard_product.h" // the hazard objects
#include "game/peds_product.h" // the pedestrians
#include "game/anim_detail.h" // maximum animation detail
#include "game/passes_product.h"
#include "game/loader_product.h" // the level loader
#include "game/route_product.h"  // the route records
#include "game/stream_files_product.h"
#include "game/stream_product.h"
#include "game/mp_input.h" // player 2's controls (two players)
#include "game/mp2_product.h" // the two-player leftovers
#include "game/audio/root_counter.h" // the console's root counter 2 in a live run
#include "render/weapon_draw.h"
#include "render/render_target.h" // the offscreen target, MSAA, VSync
#include "render/smooth_texture.h"
#include "render/text_overlay.h"
#include "race_render.h"      // the race frame's views (VR-ready)
#include "graphics_settings.h" // the PC graphics settings
#include "hd_media.h"         // the HD media pack (docs\HD-MEDIA.md)
#include "pc_overlay.h"        // the F10 settings overlay and the profiler
#include "cheat_menu.h"        // the cheat menu
#include "handling_settings.h"  // Original / Modern (SA-style) handling
#include "handling_view.h"      // the head view's drawn lean and view roll
#include "vr_wheelie.h"         // the wheelie's drawn pitch, the VR gesture
#include "game/cheats.h"
#include "game/traffic_arena.h"
#include "render/scene_geometry.h"
#include "render/shaders.h"
#include "render/frame_shot.h" // ReadFrame / SaveShot
#include "game_host.h"         // the window / headset, the keyboard, the controllers
#include "game_host_vr.h"      // the VR host's stereo frame (only used when the host is one)
#include "vr_comfort.h"        // VR: smooth motion, the view off the bike, the motion meter
#include "vr_bike_shake.h"     // the own bike's part slots in the head view, the shake meter
#include "vr_horizon.h"        // the head view's bike pitch with the road, the riders near the eye
#include "vr_pacing.h"          // VR: the race stepped on the display's clock
#include "vr_draw.h"           // the VR HUD panel and the comfort vignette
#include "vr_handlebars.h"     // VR: the hands on the handlebars
#include "vr_melee.h"          // VR: physical blows - a fist or a weapon touching a rider
#include "vr_holsters.h"       // VR: holsters, the weapons in the hands, the grip
#include "rrformats/audio.h"
#include "rrformats/level_bank.h" // the level bundle (RASHCDI 0x8005C45C)
#include "game/hud_arena.h"
#include "game/hud_view.h"
#include "game/shell/front_end.h" // rrgame without --race: the front end (front_end_main.cpp)
#include "game/sim/hud.h"
#include "game/rumble_product.h" // the controllers' motors
#include "game/mp_sound.h"       // the two-player sound world's counters
#include "rrvfs/disc_image.h"
#include "rrvfs/disc_identity.h" // main(): the installed disc when the command line names none

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace rr::render;

namespace {

// The original's voice volumes are the PS1 SPU's own 15-bit per-channel registers, where 0x3FFF is
// unity and a NEGATIVE value means the channel is phase-inverted (pan 129..255 gives
// the left channel a negative volume). Our mixer takes a volume and a balance instead, so this
// inverts its own law exactly - `PanGains` in mixer.cpp - rather than inventing a second one. The
// phase inversion itself is dropped, and that is the one thing about the original's pan this
// conversion does not carry.
void SpuVolumeToMixer(int32_t left, int32_t right, int32_t& volume, int32_t& pan) {
    auto gain = [](int32_t v) {
        int32_t a = (v < 0) ? -v : v;
        if (a > 0x3FFF) a = 0x3FFF;
        return static_cast<int32_t>((static_cast<int64_t>(a) * rr::audio::kVolumeUnit) / 0x3FFF);
    };
    const int32_t gainL = gain(left), gainR = gain(right);
    volume = (gainL > gainR) ? gainL : gainR;
    pan = 0;
    if (volume <= 0) return;
    if (gainL >= gainR)
        pan = static_cast<int32_t>((static_cast<int64_t>(gainR) * rr::audio::kVolumeUnit) / volume) -
              rr::audio::kVolumeUnit;
    else
        pan = rr::audio::kVolumeUnit -
              static_cast<int32_t>((static_cast<int64_t>(gainL) * rr::audio::kVolumeUnit) / volume);
}

// ---------------------------------------------------------------- music
//
// The hit sounds: the voices the SPU model keyed on in the game frames where the player landed a
// blow (FightStat(player, 2, 0) grew) and where a rival's blow lowered a rider's health, with the registers each
// started from - what the gate compares with the original's key-on for a landed punch (sound 56 of bank 0, taken
// from the oracle's punch-cop run: one voice, @0x168B0 pitch 0x514 ADSR 0x80FF/0x5FDE).
struct HitSoundTally {
    size_t landed = 0, landedKeyOns = 0, hurt = 0, hurtKeyOns = 0;
    std::string landedList, hurtList;
    static std::string Describe(const std::vector<rr::audio::SpuVoices::KeyOnEvent>& kon) {
        std::string out;
        for (const auto& k : kon) {
            char v[80];
            std::snprintf(v, sizeof(v), " ch%d@%05X p%04X v%04X/%04X a%04X/%04X", k.channel, k.address, k.pitch, k.volL,
                          k.volR, k.adsr1, k.adsr2);
            out += v;
        }
        return out;
    }
    void Frame(uint32_t frame, size_t landedNow, size_t hurtNow, const std::vector<rr::audio::SpuVoices::KeyOnEvent>& kon) {
        const std::string one = "f" + std::to_string(frame) + Describe(kon) + ";";
        if (landedNow != 0) {
            landed += landedNow;
            landedKeyOns += kon.size();
            if (landedList.size() < 600) landedList += " " + one;
        } else if (hurtNow != 0) {
            hurt += hurtNow;
            hurtKeyOns += kon.size();
            if (hurtList.size() < 600) hurtList += " " + one;
        }
    }
    std::string Line(size_t animSoundCalls) const {
        return "the hit sounds: AnimSounds SLUS 0x80018E54 " + std::to_string(animSoundCalls) +
               " call(s); the player's landed blows " + std::to_string(landed) + ", key-ons on their frames " +
               std::to_string(landedKeyOns) + ":" + landedList + " other blows that hurt " + std::to_string(hurt) +
               ", key-ons on their frames " + std::to_string(hurtKeyOns) + ":" + hurtList + "\n";
    }
};
HitSoundTally g_hitSounds;

// The sink also measures what it handed the device: how many frames were pulled and the largest
// absolute sample among them. That is what turns "the game makes a sound" from a claim into a
// number - a run whose emitter started voices but whose peak is 0 produced silence.
class MixerSink final : public rr::platform::AudioSink {
public:
    explicit MixerSink(rr::audio::Mixer& mixer) : mixer_(mixer) {}
    void Render(int16_t* out, size_t frames) override {
        if (muted.load(std::memory_order_relaxed)) { // the settings overlay holds the race: nothing advances
            std::memset(out, 0, frames * 2 * sizeof(int16_t));
            return;
        }
        mixer_.Mix(out, frames);
        int32_t peak = 0;
        size_t rail = 0;
        for (size_t i = 0; i < frames * 2; ++i) {
            const int32_t a = out[i] < 0 ? -static_cast<int32_t>(out[i]) : out[i];
            if (a > peak) peak = a;
            if (a >= 32767) ++rail; // a sample the 16-bit clamp decided
        }
        framesRendered.fetch_add(frames, std::memory_order_relaxed);
        samplesAtRail.fetch_add(rail, std::memory_order_relaxed);
        int32_t was = peak_.load(std::memory_order_relaxed);
        while (peak > was && !peak_.compare_exchange_weak(was, peak, std::memory_order_relaxed)) {
        }
    }
    int32_t Peak() const { return peak_.load(std::memory_order_relaxed); }
    std::atomic<size_t> framesRendered{0};
    std::atomic<size_t> samplesAtRail{0};
    std::atomic<bool> muted{false};

private:
    rr::audio::Mixer& mixer_;
    std::atomic<int32_t> peak_{0};
};

std::shared_ptr<rr::audio::MemorySource> LoadMusicTrack(const rr::DiscImage& disc, size_t trackIndex,
                                                        size_t maxUnits, double& secondsOut) {
    const auto exeFile = disc.Find("SLUS_010.53");
    const auto albumFile = disc.Find("DATA/ALBUM.ALB");
    if (!exeFile || !albumFile) return nullptr;
    const std::vector<rr::AlbTrack> tracks = rr::ParseAlbumTrackTable(disc.ReadFile(*exeFile));
    if (trackIndex >= tracks.size()) return nullptr;
    const rr::AlbTrack& track = tracks[trackIndex];
    const size_t units = std::min(track.Units(), maxUnits);
    if (units == 0) return nullptr;
    std::vector<uint8_t> bytes(units * rr::kAlbUnitSize);
    disc.ReadForm1(albumFile->lba, track.StartUnit() * rr::kAlbUnitSize, bytes.data(), bytes.size());
    rr::PcmBuffer pcm = rr::DecodeAlb(bytes);
    secondsOut = pcm.Seconds();
    return std::make_shared<rr::audio::MemorySource>(std::move(pcm.samples), pcm.channels, pcm.sampleRate);
}

// ---------------------------------------------------------------- the HUD
//
// The PORTED HudFrame RASHCDG 0x8005E848 runs at the end of every session frame (race_session.cpp)
// and links THIS frame's HUD packets - only the elements the race state calls
// for, with its values - into the arena's HUD ordering-table slot. This walks that list the way the
// GPU's DMA does, rasterises it the way the PlayStation GPU draws it (src\game\hud_view.h) into a
// premultiplied overlay of the 384 x 240 draw area over the HUD page of VRAM the loader transcription
// built, and lays the part a television shows over the 3D frame.
//
// The visible part: every race capture's GPU state (gpu.json) shows x 9.. (365 pixels) and y 8..
// (224 lines) of the 384 x 240 draw area; that rectangle is stretched over the window, as the 3D frame
// is.
const char* const kHudOverlayVs = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
out vec2 vUv;
void main() {
    vUv = aUv;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";
const char* const kHudOverlayFs = R"(#version 330 core
in vec2 vUv;
uniform sampler2D uOverlay;
out vec4 oColor;
void main() { oColor = texture(uOverlay, vUv); } // premultiplied: blended ONE, ONE_MINUS_SRC_ALPHA
)";

struct Hud {
    GLuint program = 0, vao = 0, vbo = 0, texture = 0;
    GLint overlayLocation = -1;
    bool ready = false;
    rr::game::HudOverlay overlay;
    std::vector<rr::game::HudPacket> packets;
    rr::game::HudRasterStats stats;
    // HD media (hd_media.h): the HUD drawn at the HD pack's scale, and the size the texture was last given
    rr::game::HudHd hdHud;
    int texW = rr::game::HudOverlay::kWidth, texH = rr::game::HudOverlay::kHeight;
    // --parity: the capture's HUD slot *(0x8005B590) and its drawing offset (the libgpu DRAWENV's ofs,
    // 0x80055F84 / 86): a capture taken on the second buffer draws at y 256, its DR_AREA words are absolute.
    uint32_t parityOt = 0;
    int32_t originX = 0, originY = 0;

    void Init(bool wholeArea = false) {
        program = BuildProgram(kHudOverlayVs, kHudOverlayFs);
        overlayLocation = gl.GetUniformLocation(program, "uOverlay");
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rr::game::HudOverlay::kWidth, rr::game::HudOverlay::kHeight, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        float u0 = 9.0f / 384.0f, u1 = (9.0f + 365.0f) / 384.0f;
        float v0 = 8.0f / 240.0f, v1 = (8.0f + 224.0f) / 240.0f;
        if (wholeArea) u0 = v0 = 0.0f, u1 = v1 = 1.0f; // --parity: the window is the whole draw area
        const float quad[6][4] = {{-1, 1, u0, v0}, {1, 1, u1, v0}, {-1, -1, u0, v1},
                                  {1, 1, u1, v0},  {1, -1, u1, v1}, {-1, -1, u0, v1}};
        gl.GenVertexArrays(1, &vao);
        gl.BindVertexArray(vao);
        gl.GenBuffers(1, &vbo);
        gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
        gl.BufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * static_cast<GLsizei>(sizeof(float)), reinterpret_cast<void*>(0));
        gl.EnableVertexAttribArray(0);
        gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * static_cast<GLsizei>(sizeof(float)),
                               reinterpret_cast<void*>(sizeof(float) * 2));
        gl.EnableVertexAttribArray(1);
        ready = true;
        std::printf("hud: the PORTED HudFrame (RASHCDG 0x8005E848) decides the HUD each frame from the race state;\n"
                    "     its packets are drawn as the PlayStation GPU draws them. --no-hud turns it off.\n");
    }

    // The frame's HUD rasterised and uploaded into `texture` (premultiplied RGBA, 384 x 240, row 0 at the top). The VR
    // frame shows the texture on its HUD panel (vr_draw.h); Draw lays it over the window.
    bool Upload(const rr::game::RaceSession& session) {
        if (!ready || !session.HudReady()) return false;
        overlay.Clear();
        packets.clear(); // every player's HUD slot, in the GPU's order (mp_session.cpp HudListHeads)
        std::vector<uint32_t> heads =
            parityOt != 0 ? std::vector<uint32_t>{session.ArenaWord(parityOt)} : session.HudListHeads();
        if (parityOt == 0) // the pause menu's slots 6..2, drawn after the HUD's (pause_product.h)
            for (const uint32_t h : session.PauseListHeads()) heads.push_back(h);
        for (const uint32_t head : heads) {
            const std::vector<rr::game::HudPacket> more = rr::game::WalkHudList(session.ArenaRam(), head);
            packets.insert(packets.end(), more.begin(), more.end());
        }
        rrgame::ApplyHdSwitch(rrgame::Graphics().hdMedia); // HD media: the switch as the settings hold it now
        gl.UseProgram(program);
        gl.ActiveTexture(GL_TEXTURE0);
        if (parityOt == 0 && rrgame::UploadHudHd(hdHud, packets, session.HudPage(), originX, originY, texture,
                                                 rrgame::CheatsShown(), texW, texH))
            return true;
        if (texW != rr::game::HudOverlay::kWidth || texH != rr::game::HudOverlay::kHeight) { // back from HD: 384 x 240, nearest
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rr::game::HudOverlay::kWidth, rr::game::HudOverlay::kHeight, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            texW = rr::game::HudOverlay::kWidth;
            texH = rr::game::HudOverlay::kHeight;
        }
        stats = rr::game::RasterizeHud(packets, session.HudPage(), overlay, originX, originY);
        std::vector<uint8_t> bytes = overlay.Bytes();
        if (rrgame::CheatsShown()) // the CHEATS tag while any cheat is on (cheat_menu.h)
            rrgame::StampCheatsTag(bytes.data(), rr::game::HudOverlay::kWidth, rr::game::HudOverlay::kHeight);
        gl.UseProgram(program);
        gl.ActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, rr::game::HudOverlay::kWidth, rr::game::HudOverlay::kHeight, GL_RGBA,
                        GL_UNSIGNED_BYTE, bytes.data());
        return true;
    }

    // The uploaded texture over the bound framebuffer's viewport (the shown part, as the window gets it).
    void DrawUploaded() {
        gl.UseProgram(program);
        gl.ActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        gl.Uniform1i(overlayLocation, 0);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        gl.BindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }

    void Draw(const rr::game::RaceSession& session) {
        if (!Upload(session)) return;
        gl.Uniform1i(overlayLocation, 0);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        gl.BindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }
};

// ---------------------------------------------------------------- the machine on screen
// The machines' frames, the rivals' own models, the render camera and every other per-view helper live with the
// per-view drawing in race_render.cpp.
using rrgame::BikeFrameFromGround;
using rrgame::ObjectLook;
using rrgame::RivalsAsPlayer;

int Usage() {
    std::fprintf(stderr,
                 "usage: rrgame <disc.bin> [--race <set> <id>] [options]\n"
                 "  --race <set> <id>   which of the 100 races to start (default 1 20)\n"
                 "  --opponents <n>     how many AI riders to put on the grid (default 5)\n"
                 "  --frames <n>        run n frames and exit, instead of until the window closes.\n"
                 "                      A scripted run also gets a FIXED time step, so that the same\n"
                 "                      command produces the same numbers; the wall clock does not.\n"
                 "  --ticks <n>         force the step to n units of 1/300 s (0 = read the clock)\n"
                 "  --shot <out.png>    write the last frame out as a PNG\n"
                 "  --log <out.txt>     write what the loop called each frame, and the seam list\n"
                 "  --music <n>         play ALBUM.ALB track n through our own mixer\n"
                 "  --no-sfx            do not open an audio device for the sound effects the\n"
                 "                      ported emitter (SLUS 0x80017BA0) starts\n"
                 "  --no-fx             do not draw the effects (dust, sparks); the effect pass still runs\n"
                 "  --sound-wav <out.wav>  MEASUREMENT: no audio device; the SPU model is rendered\n"
                 "                      1470 frames (1/30 s at 44100 Hz) per game frame, in step with\n"
                 "                      the game, into a WAV - a deterministic record of the sound\n"
                 "  --sfx-probe <id>    DEVELOPMENT CONTROL: ask the ported emitter for sound <id>\n"
                 "                      of the default bank, once a second, at the listener. No\n"
                 "                      call site of the original does this - the only ported\n"
                 "                      caller of the emitter is the engine's throttle crossing,\n"
                 "                      and nothing this session sets up reaches it - so this is\n"
                 "                      how the chain from the emitter to the device is measured\n"
                 "  --camera <n>        press the camera control (Select) on the first n frames of the\n"
                 "                      run: a scripted way to reach chase cameras 1..3 of CAMERA.CA\n"
                 "  --camera head       start on the head camera (first person; C cycles 0..3 -> head -> 0)\n"
                 "  --no-hud            do not draw the HUD (the PORTED HudFrame still runs)\n"
                 "  --wide              fill the window, showing more at the sides (default: the\n"
                 "                      original's 4:3 picture, with bars) - F2 toggles it\n"
                 "  --no-dither         15-bit colour without the GPU's 4x4 dither - F3 toggles it\n"
                 "  --smooth            24-bit colour, no PS1 quantisation at all - F4 toggles it\n"
                 "  --gfx original|modern  the PC graphics preset a run starts from (a scripted run: original, an\n"
                 "                      interactive one: rrgame_settings.ini, else modern); single values on top:\n"
                 "                      --render-scale P (50..200) | --render-height 720|1080|1440|2160, --msaa 1|2|4|8,\n"
                 "                      --textures nearest|smooth, --precise-vertices 0|1, --affine 0|1, --draw-order\n"
                 "                      ps1|depth, --max-detail 0|1, --draw-distance original|extended|maximum,\n"
                 "                      --vsync 0|1, --fps-cap N, --fullscreen 0|1, --profiler 0|1 (graphics_settings.h;\n"
                 "                      F10 or Select+Start opens the settings overlay in the game)\n"
                 "  --profile-log <f>   write the frame profiler's run totals (FPS, CPU / GPU ms, draw calls) to f\n"
                 "  --hold <TBLRV>      hold these controls for the whole run, so a scripted run\n"
                 "                      can show what the keys do without a hand on the keyboard\n"
                 "                      (V = look behind, pad control 2)\n"
                 "  --pause-at <n>      press Start at frame n (the pause menu, pause_product.h)\n"
                 "  --pause-script <s>  menu presses \"f:key,key;f:key\", keys start up down left right x t\n"
                 "  --log-from <n>      log every frame from frame n on (default: the first 2400, then every 300th)\n"
                 "  --heartbeat <file>  DEVELOPMENT: write \"frame <n>\" after every frame, flushed at once, so a\n"
                 "                      run that never returns names the last frame it finished\n"
                 "  --autosteer         a scripted steering player: throttle, brake and steer chosen\n"
                 "                      each frame to follow the race route (ai_race.h AutoSteer)\n"
                 "  --autosteer-lane L  the autosteer's line L units right of the route (a lane, not the centre)\n"
                 "  --aiglobalscheck <ram|dir>  DEVELOPMENT CHECK: GLOBALS.BI and LEVEL<n>.BI where the\n"
                 "                      loader puts them, against captured RAM (-mutate: must FAIL)\n"
                 "  --ridercheck <ram|dir>  DEVELOPMENT CHECK, no window: the rider-record loader (grid_loader.h)\n"
                 "                      re-run on each race capture's own grid over a wiped copy, against the\n"
                 "                      capture byte for byte (the shell image refused; -mutate: must FAIL)\n"
                 "  --budget <n>        cap on stream chunks read while assembling the world\n"
                 "  --keys              print the keyboard mapping and exit\n"
                 "  --arenacheck <ram>  DEVELOPMENT CHECK, no window: rebuild ROAD<set>.MAP and the\n"
                 "                      road objects a captured 2 MiB RAM image holds resident, from\n"
                 "                      the disc, through the road arena the ported road query runs\n"
                 "                      on, and compare them with the image byte for byte\n"
                 "  --cellarenacheck <ram>  DEVELOPMENT CHECK, no window: place every scene cell a\n"
                 "                      captured RAM image holds resident, from the disc, with this\n"
                 "                      session's cell loader at the image's own addresses, and\n"
                 "                      compare every byte the ported ground query can read\n"
                 "  --cellarenacheck-mutate <ram>  its negative control: the same check with the\n"
                 "                      loader's region relocation left out - must FAIL\n"
                 "  --routearenacheck <ram>  DEVELOPMENT CHECK, no window: rebuild the race graph\n"
                 "                      STREAM<n>.GRF and the route block ROADGRF<n>.TXT's parser builds for\n"
                 "                      the race the image names, at the image's own addresses, byte for byte\n"
                 "  --routearenacheck-mutate <ram>  its control (next nodes at their text slot): must FAIL\n"
                 "  --animarenacheck <ram>  DEVELOPMENT CHECK, no window: rebuild the rider animation\n"
                 "                      banks, clip tables, slot records and bank table at the image's own\n"
                 "                      addresses, byte for byte, and check the object wiring\n"
                 "  --animarenacheck-mutate <ram>  its control (no +0x14 fix-up): must FAIL\n"
                 "  --camarenacheck <ram>  DEVELOPMENT CHECK, no window: CAMERA.CA at 0x800CD7B8 and view\n"
                 "                      record 0's init-constant fields, built by the session's own\n"
                 "                      CameraInit transcription, byte for byte\n"
                 "  --camarenacheck-mutate <ram>  its control (CAMERA.CA from offset 4): must FAIL\n"
                 "  --poparenacheck <ram>  DEVELOPMENT CHECK, no window: the resident piece list 0x800D4B10\n"
                 "                      the window test reads, gp+204 and 0x800D8068, byte for byte, and our\n"
                 "                      residency rule against what the image holds\n"
                 "  --poparenacheck-mutate <ram>  its control (BTT_ record in the slot): must FAIL\n"
                 "  --hudarenacheck <statedir>  DEVELOPMENT CHECK, no window: rebuild the HUD's arena (the layout\n"
                 "                      loader's item records, art and texture tables, timers, slides, font,\n"
                 "                      string table, message widths) at the capture's own addresses and the\n"
                 "                      HUD page of VRAM, and compare them with ram.bin / vram.bin\n"
                 "  --hudarenacheck-mutate <statedir>  its control (no art fix-up 0x8005EFFC): must FAIL\n"
                 "  --hudcheck <statedir> <tracedir>  DEVELOPMENT CHECK, no window: run the PORTED HudFrame on the\n"
                 "                      state at the original's HudFrame entry (the capture plus the traced\n"
                 "                      run's stores) and compare the packets it links with the original's\n"
                 "                      GPU packets of that frame, and our rasterised HUD with the shown frame\n"
                 "  --hudcheck-mutate <statedir> <tracedir>  its control (one bit of the speed flipped): must FAIL\n"
                 "  --hudshots <prefix> with --hudcheck: write <prefix>_shown.png and _ours_over_shown.png\n"
                 "  up/W throttle (Cross), down/S brake (Square), left/right steer, C camera (Select),\n"
                 "  V look behind, Z/X/Q fight (R1/L1/R2; R/F hold d-pad up/down), T taunt (L2), Esc quits;\n"
                 "  two players: player 2 arrows, M , . K L fight, N taunt, / camera, ; look behind, F6 its\n"
                 "  controller's analogue mode; --hold2 / --autosteer2 / --taunt2 N script player 2\n"
                 "  --punch N / --kick N / --chase +-1 / --opponent-health N: scripted fight tests (rrgame --keys)\n"
                 "  --punch-action A    the --punch script presses combat action A 1..8 (6: prod / stun gun, 5: spray)\n"
                 "  --weapon W[:S] / --opponent-weapons W[:S]: DEVELOPMENT weapon records (weapon_session.h)\n"
                 "  --cheat-weapon W|off, --cheat-weapon-now F:W, --cheat-swings, --cheat-no-police, --cheat-no-traffic,\n"
                 "  --cheat-sparring passive|fight, --cheat-passive, --cheat-god, --cheat-nitro, --cheat-freeze-timer,\n"
                 "  --cheat-sparring-attack, --cheat-sparring-weapon own|fists|0..8, --cheat-sparring-standing,\n"
                 "  --cheat-at F:key=value (a [cheats] key at frame F), --cheat-report (the cheats' tally in the tail):\n"
                 "                      the cheat menu's cheats for a scripted run (cheat_menu.h)\n"
                 "  --cheat-career max|venue <card.mcr>: the menu's career cheat on a card (backup, verify), then exit\n");
    return 2;
}

} // namespace

// tools\rrgame\fx_check.cpp: the effects against the original's packets.
int FxCheck(const rr::DiscImage& disc, const std::string& dir, const std::string& primsCsv, int mutate);
// tools\rrgame\model_check.cpp: the model draw's effect capture against the original.
int ModelPlant(const std::string& dir, const std::string& out);
int ModelFxCheck(const rr::DiscImage& disc, const std::string& dir, const std::string& primsCsv, int mutate);
int CarClutCheck(const rr::DiscImage& disc, const std::string& dir);
int WeaponTexCheck(const rr::DiscImage& disc, const std::string& dir, bool mutate);
// tools\rrgame\zfight_probe.cpp: RRJB_ZFIGHT, the depth-fight detector (DEVELOPMENT).
namespace rrgame {
void ZFightProbe(RaceRenderer& renderer, const GameView& view, long frame);
void CarFlickProbe(RaceRenderer& renderer, const GameView& view, long frame); // RRJB_CARFLICK (carflick_probe.cpp)
void ZFightSoupReport(const rr::TriangleSoup& soup, const std::vector<rr::render::CellRange>& ranges);
void ZFightCoverReport(const rr::TriangleSoup& soup, const std::vector<rr::render::CellRange>& ranges,
                       const std::vector<rr::RoadSlice>& path);
}
// tools\rrgame\parity.cpp: --parity, the capture's RAM and the race it names.
bool ParityPrepare(const std::string& dir, std::vector<uint8_t>& ram, int& raceSet, int& raceId, std::string& err);
#ifdef _WIN32
// tools\rrgame\padmap_check.cpp: the controllers' decoding and bindings on synthetic reports.
int PadMapCheck(bool mutate);
int DualSenseProbe();
#endif

// The race path (every command line with --race or a check). `main` is at the end of this file: without
// --race it opens the front end first (front_end_main.cpp), which calls this for each race it starts.
int RaceMain(int argc, char** argv) {
    std::string discPath, shotPath, logPath, arenaCheck, cellArenaCheck, routeArenaCheck, animArenaCheck;
    std::string camArenaCheck, popArenaCheck;
    std::string streamCheck; // --streamcheck
    bool streamCheckMutate = false;
    std::string hudArenaCheck, hudCheckState, hudCheckTrace, hudShots;
    bool hudArenaMutate = false, hudCheckMutate = false;
    bool cellArenaMutate = false, routeArenaMutate = false, animArenaMutate = false;
    bool camArenaMutate = false, popArenaMutate = false;
    std::string trafficArenaCheck;
    bool trafficArenaMutate = false;
    std::string fxSheetCheck, fxCheckState, fxCheckPrims; // fx_runtime.h, fx_check.cpp
    std::string modelCheckState, modelCheckPrims, carClutCheck, weaponTexCheck; // model_check.cpp
    int modelCheckMutate = 0;
    bool weaponTexMutate = false;
    bool fxSheetMutate = false;
    int fxCheckMutate = 0;
    bool drawFx = true; // --no-fx: the effects are not drawn (the pass still runs)
    std::string aiGlobalsCheck; // ai_race.h
    bool autoSteerOn = false;   // --autosteer, ai_race.h AutoSteer (a scripted steering player)
    double autoSteerLane = 0.0; // --autosteer-lane L: its line L units right of the route (ai_race.h)
    long logFrom = 2400;        // --log-from: every frame from here on goes into --log (else every 300th)
    std::string arenaDumpPath;  // --dump-arena: DEVELOPMENT, the session's 2 MiB guest arena at the end (to work\)
    std::string heartbeatPath;  // --heartbeat: DEVELOPMENT, "frame <n>" flushed after every frame (the gate's hang report)
    rr::game::AutoSteer autoSteer;
    bool aiGlobalsMutate = false;
    std::string riderCheck; // grid_loader.h: the rider-record loader against the race captures
    bool riderCheckMutate = false;
    long cameraPresses = 0;
    bool headView = false; // the head camera (head_camera.h): the fifth entry of the camera cycle
    long punchEvery = 0, kickEvery = 0; // --punch / --kick N: R1 / R2 down 2 frames in every N (fight)
    long punchFrom = 0;                 // --punch-from F: the --punch script starts at frame F (race_modes.cpp runs)
    int punchAction = 1;                // --punch-action A: the --punch script presses combat action A (1..8)
    long tauntEvery = 0;                // --taunt N: L2 down 2 frames in every N (the riders' voices)
    long toBikeEvery = 0, toBikeFrom = 0; // --to-bike N[:F]: Triangle down 2 frames in every N from frame F
    long taunt2Every = 0;               // --taunt2 N: the same for player 2 (mp_input.h)
    int opponentHealth = -1; // --opponent-health N: DEVELOPMENT, every opponent's riderDef+0x0F := N at the start
    int devWeapon = -1, devSwings = 3; // --weapon W[:S]: DEVELOPMENT, the player's weapon in hand (weapon_session.h)
    std::string cheatCareer, cheatCareerCard; // --cheat-career max|venue <card>: a career cheat on a card (cheat_menu.h)
    int devFoeWeapon = -1, devFoeSwings = 3; // --opponent-weapons W[:S]: DEVELOPMENT, every opponent owns W
    int chaseSign = 0; // --chase +1/-1: a scripted player that closes on its fight target (fight_session.h)
    int shadowBike = -1; long brakeFrom = -1; // --shadow <bike> / --brake-from <frame>: scripts (cop_race.h)
    int raceSet = 1, raceId = 20, opponents = 5, musicTrack = -1;
    int playersArg = 0; // --players 2: the two-player split screen (mp_session.cpp); 0 = the front end's count
    rr::game::PadState hold2;          // --hold2 <TBLRV>: player 2's held controls (mp_input.h)
    rr::game::PauseScript pauseScript; // --pause-at / --pause-script (pause_product.h)
    rr::game::PauseKeys pauseKeys;     // the pause's key roles (pause_product.h)
    rr::game::PauseKeys pauseKeys2;    // ... player 2's (mp_input.h: H / the second controller's Start)
    const bool pad2Start = !(std::getenv("RRJB_P2_START") != nullptr && std::strcmp(std::getenv("RRJB_P2_START"), "off") == 0);
    uint64_t p2StartFrames = 0;       // frames player 2's pad carried Start (the race log)
    uint64_t pausesBy[4] = {};        // pauses by the pad that paused (gp+260)
    std::string pauseCheckCsv;         // --pausecheck <orig_prims.csv>: the menu's packets against the original's
    bool pauseCheckMutate = false;     // --pausecheck-mutate: its negative control
    bool autoSteer2On = false;         // --autosteer2: the autosteer script on player 2's bike
    rr::game::AutoSteer autoSteer2;
    double startDistance = 0.0;
    long frameLimit = -1;
    // 0 means "read the wall clock". A scripted run (`--frames`) defaults to 5, one 60 Hz frame in
    // the original's 1/300 s unit, so that the same command always produces the same numbers.
    int32_t fixedTicks = 0;
    bool drawHud = true;
    bool drawRangeOff = false; // --draw-range-off: the negative control of the rivals' draw range
    bool propsFromRecords = false; // --props-from-records: the props from every kind-4 record, no spawner (the control)
    std::string worldCheck; // --worldcheck[-mutate] <ram>: the PORTED walker against a capture's props and volumes
    bool worldCheckMutate = false;
    // The PS1 look (race_scene.h PostProcess) and the frame shape. Defaults are the original's:
    // 15-bit colour with the GPU's dither, a 4:3 picture.
    bool wideScreen = false, ps1Colour = true, ps1Dither = true;
    bool sfx = true;
    std::string soundWav; // --sound-wav: the deterministic sound measurement
    int sfxProbe = -1;
    size_t chunkBudget = 4000;
    rr::game::PadState hold;
    // RRJB_SHOWN=off: the one-player picture shows the whole 240-line draw area again (the shown-part
    // projection's negative control)
    const bool shownOff = std::getenv("RRJB_SHOWN") != nullptr && std::strcmp(std::getenv("RRJB_SHOWN"), "off") == 0;
    // RRJB_SUN=off: the sky gradient's blend measured from yaw 0 again, not the level's sun (its negative control)
    const bool sunOff = std::getenv("RRJB_SUN") != nullptr && std::strcmp(std::getenv("RRJB_SUN"), "off") == 0;
    // RRJB_MODEL_LIGHT=off: the machines shaded by the old fixed lambert again (the model light's negative control)
    const bool modelLightOff =
        std::getenv("RRJB_MODEL_LIGHT") != nullptr && std::strcmp(std::getenv("RRJB_MODEL_LIGHT"), "off") == 0;
    std::string profileLog; // --profile-log: the profiler's run totals
    rrgame::GraphicsSettings gfxFlags; // the command line's graphics flags (graphics_settings.h StartGraphics)
    std::string parityState; // --parity <statedir>: the product's frame of a captured state (parity.cpp)
    std::vector<uint8_t> parityRam;
    std::string shadowCheckCsv; // --shadowcheck <orig_prims.csv>: the ported shadow's packets (shadow_product.h)
    for (int i = 1; i < argc; ++i) {
        if (rrgame::ApplyGraphicsFlag(argc, argv, i, gfxFlags)) continue; // the PC graphics settings (graphics_settings.h)
        if (rrgame::ApplyCheatFlag(argc, argv, i)) continue; // the cheats (cheat_menu.h)
        if (rrgame::ApplyHandlingFlag(argc, argv, i)) continue; // the handling (handling_settings.h)
        if (std::strcmp(argv[i], "--cheat-career") == 0 && i + 2 < argc) { // max|venue <card.mcr>: a career cheat, then exit
            cheatCareer = argv[++i];
            cheatCareerCard = argv[++i];
            continue;
        }
        if (std::strcmp(argv[i], "--profile-log") == 0 && i + 1 < argc) {
            profileLog = argv[++i];
            continue;
        }
        if (std::strcmp(argv[i], "--parity") == 0 && i + 1 < argc) {
            parityState = argv[++i];
        } else if (std::strcmp(argv[i], "--shadowcheck") == 0 && i + 1 < argc) { // shadow_product.h
            shadowCheckCsv = argv[++i];
        } else if (std::strcmp(argv[i], "--race") == 0 && i + 2 < argc) {
            raceSet = std::atoi(argv[++i]);
            raceId = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--opponents") == 0 && i + 1 < argc)
            opponents = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--players") == 0 && i + 1 < argc) // two players, split screen
            playersArg = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--split-mode") == 0 && i + 1 < argc) // VIEWS.VI record 0..2 (mp_session.cpp)
            rr::game::RaceSession::DevSplitMode() = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--hold2") == 0 && i + 1 < argc) {
            const std::string held = argv[++i];
            hold2.throttle = held.find('T') != std::string::npos;
            hold2.brake = held.find('B') != std::string::npos;
            hold2.left = held.find('L') != std::string::npos;
            hold2.right = held.find('R') != std::string::npos;
            hold2.lookBack = held.find('V') != std::string::npos;
        } else if (std::strcmp(argv[i], "--autosteer2") == 0)
            autoSteer2On = true;
        else if (std::strcmp(argv[i], "--start") == 0 && i + 1 < argc)
            startDistance = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            frameLimit = std::atol(argv[++i]);
        else if (std::strcmp(argv[i], "--ticks") == 0 && i + 1 < argc)
            fixedTicks = static_cast<int32_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--shot") == 0 && i + 1 < argc)
            shotPath = argv[++i];
        else if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc)
            logPath = argv[++i];
        else if (std::strcmp(argv[i], "--music") == 0 && i + 1 < argc)
            musicTrack = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--no-sfx") == 0)
            sfx = false;
        else if (std::strcmp(argv[i], "--sound-wav") == 0 && i + 1 < argc)
            soundWav = argv[++i];
        else if (std::strcmp(argv[i], "--sfx-probe") == 0 && i + 1 < argc)
            sfxProbe = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--camera") == 0 && i + 1 < argc && std::strcmp(argv[i + 1], "head") == 0)
            headView = (++i, true); // --camera head: start on the head camera (head_camera.h)
        else if (std::strcmp(argv[i], "--camera") == 0 && i + 1 < argc)
            cameraPresses = std::atol(argv[++i]);
        else if (std::strcmp(argv[i], "--punch") == 0 && i + 1 < argc)
            punchEvery = std::atol(argv[++i]);
        else if (std::strcmp(argv[i], "--punch-from") == 0 && i + 1 < argc)
            punchFrom = std::atol(argv[++i]);
        else if (std::strcmp(argv[i], "--punch-action") == 0 && i + 1 < argc) // 1..8
            punchAction = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--kick") == 0 && i + 1 < argc)
            kickEvery = std::atol(argv[++i]);
        else if (std::strcmp(argv[i], "--taunt") == 0 && i + 1 < argc) // L2 down 2 frames in every N (speech)
            tauntEvery = std::atol(argv[++i]);
        else if (std::strcmp(argv[i], "--to-bike") == 0 && i + 1 < argc) // Triangle, back to the bike
            std::sscanf(argv[++i], "%ld:%ld", &toBikeEvery, &toBikeFrom);
        else if (std::strcmp(argv[i], "--taunt2") == 0 && i + 1 < argc) // player 2's L2, the same (mp_input.h)
            taunt2Every = std::atol(argv[++i]);
        else if (std::strcmp(argv[i], "--opponent-health") == 0 && i + 1 < argc)
            opponentHealth = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--weapon") == 0 && i + 1 < argc) // W[:S], weapon_session.h
            std::sscanf(argv[++i], "%d:%d", &devWeapon, &devSwings);
        else if (std::strcmp(argv[i], "--opponent-weapons") == 0 && i + 1 < argc) // W[:S], weapon_session.h
            std::sscanf(argv[++i], "%d:%d", &devFoeWeapon, &devFoeSwings);
        else if (std::strcmp(argv[i], "--chase") == 0 && i + 1 < argc)
            chaseSign = std::atoi(argv[++i]) < 0 ? -1 : 1;
        else if (std::strcmp(argv[i], "--shadow") == 0 && i + 1 < argc) // cop_race.h ShadowSteer
            shadowBike = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--brake-from") == 0 && i + 1 < argc) // from frame n: brake, no steer
            brakeFrom = std::atol(argv[++i]);
        else if (std::strcmp(argv[i], "--no-hud") == 0)
            drawHud = false;
        else if (std::strcmp(argv[i], "--draw-range-off") == 0) // a negative control: rivals drawn at any range
            drawRangeOff = true;
        else if (std::strcmp(argv[i], "--props-from-records") == 0) // the control: props without the spawner
            propsFromRecords = true;
        else if ((std::strcmp(argv[i], "--worldcheck") == 0 || std::strcmp(argv[i], "--worldcheck-mutate") == 0) &&
                 i + 1 < argc) { // world_pop_product.h
            worldCheckMutate = std::strcmp(argv[i], "--worldcheck-mutate") == 0;
            worldCheck = argv[++i];
        }
        else if (std::strcmp(argv[i], "--wide") == 0)
            wideScreen = true;
        else if (std::strcmp(argv[i], "--no-dither") == 0)
            ps1Dither = false;
        else if (std::strcmp(argv[i], "--smooth") == 0)
            ps1Colour = false;
        else if (std::strcmp(argv[i], "--dump-arena") == 0 && i + 1 < argc)
            arenaDumpPath = argv[++i];
        else if (std::strcmp(argv[i], "--heartbeat") == 0 && i + 1 < argc)
            heartbeatPath = argv[++i];
        else if (std::strcmp(argv[i], "--log-from") == 0 && i + 1 < argc)
            logFrom = std::atol(argv[++i]);
        else if (std::strcmp(argv[i], "--autosteer-lane") == 0 && i + 1 < argc)
            autoSteerLane = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--autosteer") == 0)
            autoSteerOn = true;
        else if (std::strcmp(argv[i], "--pausecheck") == 0 && i + 1 < argc) // pause_product.h
            pauseCheckCsv = argv[++i];
        else if (std::strcmp(argv[i], "--pausecheck-mutate") == 0)
            pauseCheckMutate = true;
        else if (std::strcmp(argv[i], "--pause-at") == 0 && i + 1 < argc) // Start at frame N
            pauseScript.At(std::atol(argv[++i]), "start");
        else if (std::strcmp(argv[i], "--pause-script") == 0 && i + 1 < argc) // "f:key,key;..." (pause_product.h)
            pauseScript.Parse(argv[++i]);
        else if (std::strcmp(argv[i], "--hold") == 0 && i + 1 < argc) {
            const std::string held = argv[++i];
            hold.throttle = held.find('T') != std::string::npos;
            hold.brake = held.find('B') != std::string::npos;
            hold.left = held.find('L') != std::string::npos;
            hold.right = held.find('R') != std::string::npos;
            hold.lookBack = held.find('V') != std::string::npos;
        }
        else if (std::strcmp(argv[i], "--arenacheck") == 0 && i + 1 < argc)
            arenaCheck = argv[++i];
        else if (std::strcmp(argv[i], "--cellarenacheck") == 0 && i + 1 < argc)
            cellArenaCheck = argv[++i];
        else if (std::strcmp(argv[i], "--cellarenacheck-mutate") == 0 && i + 1 < argc) {
            cellArenaCheck = argv[++i];
            cellArenaMutate = true;
        }
        else if (std::strcmp(argv[i], "--routearenacheck") == 0 && i + 1 < argc)
            routeArenaCheck = argv[++i];
        else if (std::strcmp(argv[i], "--routearenacheck-mutate") == 0 && i + 1 < argc) {
            routeArenaCheck = argv[++i];
            routeArenaMutate = true;
        } else if (std::strcmp(argv[i], "--animarenacheck") == 0 && i + 1 < argc)
            animArenaCheck = argv[++i];
        else if (std::strcmp(argv[i], "--animarenacheck-mutate") == 0 && i + 1 < argc) {
            animArenaCheck = argv[++i];
            animArenaMutate = true;
        }
        else if (std::strcmp(argv[i], "--camarenacheck") == 0 && i + 1 < argc)
            camArenaCheck = argv[++i];
        else if (std::strcmp(argv[i], "--camarenacheck-mutate") == 0 && i + 1 < argc) {
            camArenaCheck = argv[++i];
            camArenaMutate = true;
        } else if (std::strcmp(argv[i], "--aiglobalscheck") == 0 && i + 1 < argc) {
            aiGlobalsCheck = argv[++i];
        } else if (std::strcmp(argv[i], "--aiglobalscheck-mutate") == 0 && i + 1 < argc) {
            aiGlobalsCheck = argv[++i];
            aiGlobalsMutate = true;
        } else if (std::strcmp(argv[i], "--ridercheck") == 0 && i + 1 < argc) {
            riderCheck = argv[++i];
        } else if (std::strcmp(argv[i], "--ridercheck-mutate") == 0 && i + 1 < argc) {
            riderCheck = argv[++i];
            riderCheckMutate = true;
        } else if (std::strcmp(argv[i], "--streamcheck") == 0 && i + 1 < argc) {
            streamCheck = argv[++i];
        } else if (std::strcmp(argv[i], "--streamcheck-mutate") == 0 && i + 1 < argc) {
            streamCheck = argv[++i];
            streamCheckMutate = true;
        } else if (std::strcmp(argv[i], "--poparenacheck") == 0 && i + 1 < argc)
            popArenaCheck = argv[++i];
        else if (std::strcmp(argv[i], "--poparenacheck-mutate") == 0 && i + 1 < argc) {
            popArenaCheck = argv[++i];
            popArenaMutate = true;
        }
        else if (std::strcmp(argv[i], "--trafficarenacheck") == 0 && i + 1 < argc)
            trafficArenaCheck = argv[++i];
        else if (std::strcmp(argv[i], "--trafficarenacheck-mutate") == 0 && i + 1 < argc) {
            trafficArenaCheck = argv[++i];
            trafficArenaMutate = true;
        }
        else if (std::strcmp(argv[i], "--hudarenacheck") == 0 && i + 1 < argc)
            hudArenaCheck = argv[++i];
        else if (std::strcmp(argv[i], "--hudarenacheck-mutate") == 0 && i + 1 < argc) {
            hudArenaCheck = argv[++i];
            hudArenaMutate = true;
        } else if ((std::strcmp(argv[i], "--hudcheck") == 0 || std::strcmp(argv[i], "--hudcheck-mutate") == 0) &&
                   i + 2 < argc) {
            hudCheckMutate = std::strcmp(argv[i], "--hudcheck-mutate") == 0;
            hudCheckState = argv[++i];
            hudCheckTrace = argv[++i];
        } else if (std::strcmp(argv[i], "--hudshots") == 0 && i + 1 < argc)
            hudShots = argv[++i];
        else if ((std::strcmp(argv[i], "--posecheck") == 0 || std::strcmp(argv[i], "--posecheck-mutate") == 0) &&
                 i + 1 < argc) // rider_pose.h: the ported Pose against a capture's matrices (no disc needed)
            return rr::game::CheckRiderPose(argv[i + 1], std::strcmp(argv[i], "--posecheck-mutate") == 0);
        else if (std::strcmp(argv[i], "--no-fx") == 0)
            drawFx = false;
        else if ((std::strcmp(argv[i], "--fxsheetcheck") == 0 || std::strcmp(argv[i], "--fxsheetcheck-mutate") == 0) &&
                 i + 1 < argc) {
            fxSheetMutate = std::strcmp(argv[i], "--fxsheetcheck-mutate") == 0;
            fxSheetCheck = argv[++i];
        } else if (std::strcmp(argv[i], "--fxcheck") == 0 && i + 2 < argc) {
            fxCheckState = argv[++i];
            fxCheckPrims = argv[++i];
        } else if (std::strcmp(argv[i], "--fxcheck-mutate") == 0 && i + 3 < argc) {
            fxCheckMutate = std::atoi(argv[++i]);
            fxCheckState = argv[++i];
            fxCheckPrims = argv[++i];
        } else if (std::strcmp(argv[i], "--carclutcheck") == 0 && i + 1 < argc) { // model_check.cpp
            carClutCheck = argv[++i];
        } else if ((std::strcmp(argv[i], "--weapontexcheck") == 0 || std::strcmp(argv[i], "--weapontexcheck-mutate") == 0) &&
                   i + 1 < argc) { // model_check.cpp
            weaponTexMutate = std::strcmp(argv[i], "--weapontexcheck-mutate") == 0;
            weaponTexCheck = argv[++i];
        } else if (std::strcmp(argv[i], "--modelplant") == 0 && i + 2 < argc) { // model_check.cpp
            const std::string from = argv[i + 1], to = argv[i + 2];
            return ModelPlant(from, to);
        } else if ((std::strcmp(argv[i], "--modelfxcheck") == 0 && i + 2 < argc) ||
                   (std::strcmp(argv[i], "--modelfxcheck-mutate") == 0 && i + 3 < argc)) { // model_check.cpp
            modelCheckMutate = std::strcmp(argv[i], "--modelfxcheck") == 0 ? 0 : std::atoi(argv[++i]);
            modelCheckState = argv[++i];
            modelCheckPrims = argv[++i];
        }
        else if (std::strcmp(argv[i], "--budget") == 0 && i + 1 < argc)
            chunkBudget = static_cast<size_t>(std::atoi(argv[++i]));
#ifdef _WIN32
        else if (std::strcmp(argv[i], "--padmapcheck") == 0 || std::strcmp(argv[i], "--padmapcheck-mutate") == 0)
            return PadMapCheck(std::strcmp(argv[i], "--padmapcheck-mutate") == 0); // padmap_check.cpp
        else if (std::strcmp(argv[i], "--dualsenseprobe") == 0)
            return DualSenseProbe();
#else
        else if (std::strcmp(argv[i], "--padmapcheck") == 0 || std::strcmp(argv[i], "--padmapcheck-mutate") == 0 ||
                 std::strcmp(argv[i], "--dualsenseprobe") == 0) {
            std::fprintf(stderr, "%s: the desktop controllers' check, not built on this platform\n", argv[i]);
            return 2;
        }
#endif
        else if (std::strcmp(argv[i], "--keys") == 0) {
            std::printf("The keyboard's defaults (rebindable in controls.ini; the table at the end is what is bound now):\n"
                        "up / W    throttle  - Cross,  bike flags +0x230 bits 0x1|0x2\n"
                        "down / S  brake     - Square, bike flags +0x230 bits 0x20|0x40\n"
                        "left / A  steer     - bike flags +0x230 bits 0x80|0x100\n"
                        "right / D steer     - bike flags +0x230 bits 0x80|0x200\n"
                        "C         camera    - pad control 0 (Select): the next chase camera of CAMERA.CA; after\n"
                        "                      the fourth the head camera (first person, ours), then the first\n"
                        "V         look back - pad control 2, held: the look-behind column\n"
                        "Z         R1: combat action 1 - bare fists command 32 (FIGHT.BIN record 0, the punch),\n"
                        "          with a club/pipe/board/... command 142 (the swing)\n"
                        "X         L1: combat action 2 - bare fists command 36 (record 1), armed 146\n"
                        "Q         R2: combat action 3 - command 71 (record 10, the move set that is not the\n"
                        "          fists' records 0/1: the kick)\n"
                        "R / F     d-pad Up / Down held with Z/X/Q: actions 5, 6, 4 / 7, 8 (commands 75, 77,\n"
                        "          148, 38) - the pad record's co-held modifier\n"
                        "          A strike lands on the nearest rider beside you; health bars drop; at 0 he\n"
                        "          is knocked off (KnockOff 0x800BF674). --punch N / --kick N script R1 / R2.\n"
                        "F2        4:3 with bars / widescreen;  F3 dither on/off;  F4 15-bit / 24-bit colour\n"
                        "F5        the gamepad's ANALOG mode on/off (a 0x73 pad: left stick steers, right stick\n"
                        "          and the triggers throttle / brake, as the original's analogue branch reads them)\n"
                        "On foot   (thrown off the bike): up / W or R walk forward, down / S or F back, left /\n"
                        "          right turn - the pad reader's walk bits (rider +0x228, pad_reader.h)\n"
                        "Enter / Esc / P  Start: the pause menu (RESUME / QUIT / RESTART, pause_product.h); in it\n"
                        "          arrows or W A S D move, Enter / Space = Cross (select), Esc / Backspace =\n"
                        "          Triangle (back / resume), P = Start (resume). RRJB_PAUSE=off: Esc quits as before\n"
                        "Gamepad: XInput (Xbox layout), a DualSense / DualSense Edge natively over USB or Bluetooth\n"
                        "  (rumble, adaptive triggers), else any WinMM/DirectInput controller. Buttons by PlayStation\n"
                        "  position (Xbox A = Cross, B = Circle, X = Square, Y = Triangle). Hold d-pad Up / Down (or\n"
                        "  the left stick) with an attack for the modified attacks. In menus and the pause menu:\n"
                        "  d-pad / stick move, Cross select, Triangle back, Start resume.\n"
                        "The bit assignments are measured on the original, not guessed.\n"
                        "Bound now (%s):\n",
                        rr::platform::BindingsPath().c_str());
            rr::platform::PrintControls(stdout, rr::platform::ActiveBindings());
            return 0;
        } else if (discPath.empty())
            discPath = argv[i];
        else
            return Usage();
    }
    if (discPath.empty()) return Usage();
    if (!parityState.empty()) { // the capture names the race; every bike of its pool 0 is bound (parity.cpp)
        std::string err;
        if (!ParityPrepare(parityState, parityRam, raceSet, raceId, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        opponents = 99;
        if (frameLimit < 0) frameLimit = 1;
        wideScreen = true; // the window IS the 384 x 240 draw area (below)
    }
    // A run with a frame count is a scripted run, and a scripted run must be reproducible. 5 units
    // of 1/300 s is one 60 Hz frame. `--ticks 0` asks for the wall clock back, explicitly.
    if (frameLimit >= 0 && fixedTicks == 0) fixedTicks = 5;
    // The PC graphics settings (graphics_settings.h): a scripted run (a frame count,
    // --parity, a check, a hidden run) starts from the original's frame and never reads the settings file; an
    // interactive one from rrgame_settings.ini (the modern defaults without one). --wide / --no-dither / --smooth and
    // --parity's draw area set theirs on top.
    const bool hiddenWindow = std::getenv("RRJB_WINDOW") != nullptr && std::strcmp(std::getenv("RRJB_WINDOW"), "hidden") == 0;
    const bool interactive = frameLimit < 0 && parityState.empty() && !hiddenWindow;
    rrgame::GraphicsSettings& gfx = rrgame::StartGraphics(!interactive);
    rrgame::StartCheats(!interactive); // the cheats: a scripted run only its --cheat-* flags (cheat_menu.h)
    rrgame::StartHandling(!interactive); // the handling: a scripted run Original unless --handling
    if (wideScreen) gfx.wide = true;
    if (!ps1Dither) gfx.ps1Dither = false;
    if (!ps1Colour) gfx.ps1Colour = false;
    // VR (game_host_vr.h): no console look in an eye image, the detail and distance of the VR settings
    rrgame::VrHost* const vrHost = rrgame::HostSet() ? rrgame::Host().Vr() : nullptr;
    if (vrHost != nullptr) vrHost->ApplyGraphics(gfx);
    wideScreen = gfx.wide;
    ps1Dither = gfx.ps1Dither;
    ps1Colour = gfx.ps1Colour;

    try {
        rr::DiscImage disc(discPath);
        rrgame::OpenHdPack(disc); // HD media (hd_media.h): the HUD's pack, once a process
        {   // the cheat menu's weapon list, from the disc (cheats.h)
            const std::string line = rrgame::LoadCheatWeapons(disc);
            if (interactive || rr::game::Cheats().AnyOn() || !cheatCareer.empty()) std::printf("%s", line.c_str());
        }
        if (!cheatCareer.empty()) { // --cheat-career: the menus' career cheat on a card file, then exit
            if (cheatCareer != "max" && cheatCareer != "venue") {
                std::fprintf(stderr, "--cheat-career: 'max' or 'venue', not '%s'\n", cheatCareer.c_str());
                return 1;
            }
            bool ok = false;
            const std::string status = rrgame::ApplyCareerCheat(
                disc, cheatCareerCard, cheatCareer == "max" ? rrgame::CareerCheat::MaxNitroWeapons : rrgame::CareerCheat::NextVenue, &ok);
            std::printf("cheat-career: %s\n", status.c_str());
            return ok ? 0 : 1;
        }
        if (!cellArenaCheck.empty()) {
            std::string report;
            const bool ok = rr::game::CheckCellArena(disc, raceSet, cellArenaCheck, report, cellArenaMutate);
            std::printf("%s", report.c_str());
            return ok ? 0 : 1;
        }
        if (!routeArenaCheck.empty()) {
            std::string report;
            const bool ok = rr::game::CheckRouteArena(disc, routeArenaCheck, report, routeArenaMutate);
            std::printf("%s\n", report.c_str());
            return ok ? 0 : 1;
        }
        if (!hudArenaCheck.empty()) {
            std::string report;
            const bool ok = rr::game::CheckHudArena(disc, hudArenaCheck, report, hudArenaMutate);
            std::printf("%s", report.c_str());
            return ok ? 0 : 1;
        }
        if (!hudCheckState.empty()) {
            std::string report;
            std::vector<uint8_t> shown, composed;
            const bool ok = rr::game::CheckHudAgainstTrace(disc, hudCheckState, hudCheckTrace, report, hudCheckMutate,
                                                           &shown, &composed);
            std::printf("%s", report.c_str());
            if (!hudShots.empty() && !shown.empty()) {
                rr::WritePng(hudShots + "_shown.png", rr::game::HudOverlay::kWidth, rr::game::HudOverlay::kHeight, shown);
                rr::WritePng(hudShots + "_ours_over_shown.png", rr::game::HudOverlay::kWidth,
                             rr::game::HudOverlay::kHeight, composed);
                std::printf("  pictures      %s_shown.png, %s_ours_over_shown.png\n", hudShots.c_str(), hudShots.c_str());
            }
            return ok ? 0 : 1;
        }
        if (!camArenaCheck.empty()) {
            std::string report;
            const bool ok = rr::game::CheckCameraArena(disc, camArenaCheck, report, camArenaMutate);
            std::printf("%s\n", report.c_str());
            return ok ? 0 : 1;
        }
        if (!aiGlobalsCheck.empty()) {
            std::string report;
            const bool ok = rr::game::CheckAiGlobalsArena(disc, aiGlobalsCheck, report, aiGlobalsMutate);
            std::printf("%s\n", report.c_str());
            return ok ? 0 : 1;
        }
        if (!riderCheck.empty()) {
            std::string report;
            const bool ok = rr::game::CheckRiderRecords(disc, riderCheck, riderCheckMutate, report);
            std::printf("%s\n", report.c_str());
            return ok ? 0 : 1;
        }
        if (!worldCheck.empty()) return rr::game::CheckWorldSpawn(worldCheck, worldCheckMutate); // world_pop_product.h
        if (!trafficArenaCheck.empty()) // traffic_arena.h
            return rr::game::CheckTrafficArena(disc, trafficArenaCheck, trafficArenaMutate);
        if (!fxSheetCheck.empty()) return rr::game::CheckFxSheet(disc, fxSheetCheck, fxSheetMutate); // fx_runtime.h
        if (!fxCheckState.empty()) return FxCheck(disc, fxCheckState, fxCheckPrims, fxCheckMutate);  // fx_check.cpp
        if (!modelCheckState.empty()) return ModelFxCheck(disc, modelCheckState, modelCheckPrims, modelCheckMutate); // model_check.cpp
        if (!carClutCheck.empty()) return CarClutCheck(disc, carClutCheck);                          // model_check.cpp
        if (!weaponTexCheck.empty()) return WeaponTexCheck(disc, weaponTexCheck, weaponTexMutate);   // model_check.cpp
        if (!streamCheck.empty()) { // stream_product.h
            std::string report;
            const bool ok = rr::game::CheckStream(disc, streamCheck, report, streamCheckMutate);
            std::printf("%s", report.c_str());
            return ok ? 0 : 1;
        }
        if (!popArenaCheck.empty()) {
            std::string report;
            const bool ok = rr::game::CheckPopArena(disc, popArenaCheck, report, popArenaMutate);
            std::printf("%s\n", report.c_str());
            return ok ? 0 : 1;
        }
        if (!animArenaCheck.empty()) {
            std::string report;
            const bool ok = rr::game::CheckAnimArena(disc, animArenaCheck, report, animArenaMutate);
            std::printf("%s", report.c_str());
            return ok ? 0 : 1;
        }
        if (!arenaCheck.empty()) {
            std::string report;
            const bool ok = rr::game::CheckRoadArena(disc, raceSet, arenaCheck, report);
            std::printf("%s\n", report.c_str());
            return ok ? 0 : 1;
        }
        std::printf("rrgame: set %d race %d, %d opponents\n", raceSet, raceId, opponents);
        std::string bundleLine; // the level bundle this race loads, or the resident one (level_bundle.h)
        if (const auto gb = disc.Find("DATA/GAMEBIN1.DAT")) {
            try {
                bundleLine = rr::SelectLevelBundle(disc.ReadFile(*gb), raceId);
            } catch (const std::exception& e) {
                bundleLine = std::string("level bundle: ") + e.what();
            }
            std::printf("%s\n", bundleLine.c_str());
        }

        // ---- the world, exactly as `rrtool raceworldall` checks it over all 100 races.
        if (rrgame::HostSet()) rrgame::Host().LoadingTick(); // VR: the compositor keeps getting frames through the load
        rr::RaceWorld world = rr::LoadRaceWorld(disc, raceSet, raceId, chunkBudget);
        if (rrgame::HostSet()) rrgame::Host().LoadingTick();
        if (world.path.size() < 2) {
            std::fprintf(stderr, "that race assembled no drivable road\n");
            return 1;
        }
        std::printf("world: %zu legs, %zu road chunks, %zu cells, %zu slices, route %.0f world units, "
                    "%zu junctions stitched of %zu\n",
                    world.legs.size(), world.roadChunksRead, world.cells.size(), world.path.size(),
                    world.assembledLength, world.junctionsStitched, world.junctionReports.size());

        // ---- the simulation
        // An interactive run reads the console's root counter 2 from the clock at the race start (the first
        // speech record, the first music track: root_counter.h); a scripted run keeps it fixed.
        rr::game::LiveRootCounter() = (frameLimit < 0 || std::getenv("RRJB_ROOT_COUNTER_LIVE") != nullptr) && // (DEV: a
            std::getenv("RRJB_ROOT_COUNTER_FIXED") == nullptr; // scripted run can ask for the live counter)
        rr::game::RaceSession session(disc, world, opponents, startDistance, playersArg);
        if (rrgame::HostSet()) rrgame::Host().LoadingTick();
        if (opponentHealth >= 0) session.DevOpponentHealth(static_cast<uint8_t>(opponentHealth));
        if (devWeapon >= 0) session.DevPlayerWeapon(devWeapon, devSwings); // --weapon, weapon_session.cpp
        if (devFoeWeapon >= 0) session.DevOpponentWeapons(devFoeWeapon, devFoeSwings); // --opponent-weapons
        rr::game::SetCheatsInRace(true); // the cheat menu (cheats.h): race-start values; the menus' race rows
        struct CheatRaceScope {
            ~CheatRaceScope() { rr::game::SetCheatsInRace(false); }
        } cheatRaceScope;
        rr::game::CheatRaceStart(session);
        autoSteer.path = &world.path; // --autosteer pursues the assembled route (ai_race.h)
        autoSteer.lane = autoSteerLane;
        autoSteer2.path = &world.path;
        std::printf("simulation: the ported race spine (RaceStep / RaceTick / RaceDirector /\n"
                    "            FinishTest ...), the ported AI passes, the WHOLE ported per-bike step\n"
                    "            RASHCDG 0x80075EE0 (regions A..J), the engine, ImpactStatePass, the\n"
                    "            heading writer, the stance layer and the animation clock, the race\n"
                    "            camera ViewUpdate, the population passes and the spine's seams, on one\n"
                    "            guest arena with the loader's route and animation data.\n");

        // ---- music and sound effects
        rr::audio::Mixer mixer(44100, 32);
        MixerSink sink(mixer);
        std::unique_ptr<rr::platform::AudioDevice> audioDevice;
        // One mixer voice per SPU channel, so that a channel the original's allocator stole and
        // restarted stops what it was playing - which is the whole of the original's lifetime
        // model. 0 means "nothing on this channel".
        rr::audio::VoiceId channelVoice[32] = {};
        std::vector<rr::game::SoundVoiceStart> startedVoices;
        std::vector<int32_t> stoppedVoices;
        size_t sfxPlayed = 0;
        // The PORTED engine note: every SPU register write the ported
        // EngineNote / RoadNote / AudioVSyncTick / SoundService make lands in the session's SPU
        // voice model, and this plays it - one stereo source for all 24 channels.
        if (session.Sounds().EngineMode())
            mixer.Play(rr::audio::VoiceDesc{std::make_shared<rr::audio::SpuVoicesSource>(session.Sounds().Spu())});
        if (musicTrack >= 0) {
            double seconds = 0.0;
            auto source = LoadMusicTrack(disc, static_cast<size_t>(musicTrack), 64, seconds);
            if (source) {
                rr::audio::VoiceDesc voice;
                voice.source = source;
                voice.loop = true;
                mixer.Play(voice);
                std::printf("music: album track %d, %.1f s buffered\n", musicTrack, seconds);
            } else {
                std::fprintf(stderr, "music: album track %d could not be loaded\n", musicTrack);
            }
        }
        std::vector<int16_t> soundRecord; // --sound-wav
        if (soundWav.empty() && (musicTrack >= 0 || sfx)) {
            audioDevice = rr::platform::OpenAudioDevice(sink, mixer.OutputRate());
            audioDevice->Start();
            std::printf("audio: device %s at %d Hz; sound effects %s (%d sounds in the default "
                        "bank of DATA\\RASHNZ_E.DAT)\n",
                        audioDevice->BackendName(), audioDevice->SampleRate(),
                        sfx ? "ON" : "off", session.Sounds().SoundCountOfBank(0));
        }

        // ---- the window. It is shown without taking the keyboard, and says so.
        rrgame::GameHost& host = rrgame::Host();
        if (rr::game::PauseOn()) host.SetEscapeQuits(false); // Esc pauses (pause_product.h)
        if (parityState.empty()) host.OpenWindow(1280, 720);
        else host.OpenWindow(384, 240);
        const bool* keys = host.Keys(); // the 256 held-key flags (all false without a keyboard)
        rrgame::VrHost* const vr = host.Vr(); // the VR host: stereo frames, the theatre quad, the VR menu (game_host_vr.h)

        RaceScene scene;
        scene.CreatePrograms();
        const bool envAffine = scene.Affine(), envSubdivide = scene.Subdivides(); // RRJB_AFFINE / RRJB_SUBDIV
        std::printf("%s\n", gfx.Describe().c_str());
        // race_scene_ot.cpp: the 3D scene in the original's ordering-table order;
        // RRJB_OT_ORDER=zbuffer draws it with the depth buffer instead (the negative control).
        const bool otZbuffer = [] {
            const char* v = std::getenv("RRJB_OT_ORDER");
            return v != nullptr && std::string(v) == "zbuffer";
        }();
        scene.SetOtOrder(!otZbuffer);
        const bool cellNclip = [] { // the cell emitters' back-face test
            const char* v = std::getenv("RRJB_CELL_NCLIP");
            return v == nullptr || std::string(v) != "off";
        }();
        scene.SetCellNclip(cellNclip, [] { // DEVELOPMENT: RRJB_CELL_NCLIP_STATIC=0 / -1 (the static soup's sign)
            const char* v = std::getenv("RRJB_CELL_NCLIP_STATIC");
            return v != nullptr ? std::atoi(v) : rr::render::kPropNclip;
        }());
        scene.SetOtCheck(!parityState.empty()); // the capture's own cell depths and table map to compare with
        // The cells as the renderer draws them: a copy of the world's, with every type-8 cell joined
        // to the type-9 chunk that carries its region 7 (bands 1 and 2, the fine ground and the road
        // surface), and one run per (cell, group, page) so the original's per-group level of detail
        // (`SLUS 0x80035680`) can choose every frame. The shade and texture window of every vertex
        // come from the race's own level bundle and the overlay's tables (scene_cell.md 13).
        std::vector<rr::CellData> drawCells = world.cells;
        size_t joinFailed = 0;
        const size_t joined = JoinCellRegion7(disc, raceSet, world.legs, drawCells, &joinFailed);
        scene.LoadLevel(disc, raceSet, raceId);
        std::vector<CellRange> cellRanges;
        const rr::TriangleSoup cellSoup = BuildCellSoup(drawCells, 3, false, false, &cellRanges, &scene.Look());
        scene.SetCells(cellSoup, cellRanges);
        rrgame::ZFightSoupReport(cellSoup, cellRanges); // DEVELOPMENT: RRJB_ZFIGHT_SOUP (zfight_probe.cpp)
        rrgame::ZFightCoverReport(cellSoup, cellRanges, world.path); // DEVELOPMENT: RRJB_ZFIGHT_COVER
        scene.SetCellData(&drawCells, raceSet);
        std::map<uint32_t, size_t> cellIndexById; // resource id -> drawCells index (cell_view.h)
        for (size_t k = 0; k < drawCells.size(); ++k) cellIndexById.emplace(drawCells[k].header.id, k);
        std::printf("cells: %zu, region 7 joined from type-9 chunks for %zu (%zu failed), %zu draw runs\n",
                    drawCells.size(), joined, joinFailed, cellRanges.size());
        host.LoadingTick();
        scene.LoadCellTextures(disc, raceSet, world.legs, drawCells, true);
        host.LoadingTick();
        scene.LoadSky(disc, raceSet, world.legs);
        // The sky gradient is section 1 of the SAME level bundle (RASHCDI 0x80061A80: entry raceId - 1).
        scene.LoadSkyGradient(disc, rr::LevelBundleIndexForRace(raceId));
        {   // the prop set the loader picks (race_scene.h PickHazardSet): game_state+4 and the Rand seed
            // gp+2076. OURS, named: the seed is the arena's at set-up, read without advancing it - the
            // loader's own draw happens on the console before the race, from a seed the product does not have.
            const uint8_t* ar = session.ArenaRam();
            uint32_t gs = 0, seed = 0;
            std::memcpy(&gs, ar + (0x8005B2F8u & 0x1FFFFFu), 4);
            std::memcpy(&seed, ar + (0x8005B4A8u & 0x1FFFFFu), 4);
            int hazardSet = rr::render::PickHazardSet(disc, ar[(gs + 4u) & 0x1FFFFFu], seed);
            if (session.HazardSet() >= 0) hazardSet = session.HazardSet(); // the set the world arena loaded
            scene.LoadProps(disc, world.cells, true, false, hazardSet < 0 ? 0 : hazardSet);
        }
        scene.LoadMachine(disc, true, {}, {});
        if (!RivalsAsPlayer()) { // the other riders' own models (race_scene_rivals.cpp): every pair pool 0 holds now
            std::vector<std::pair<uint32_t, uint32_t>> pairs;
            for (size_t b = 1; b < session.Bikes().size(); ++b) {
                uint32_t bm = 0, rm = 0;
                int a = 0;
                ObjectLook(session.ArenaRam(), session.Bikes()[b].entityAddress, bm, a);
                ObjectLook(session.ArenaRam(), session.Bikes()[b].ownerAddress, rm, a);
                if (bm != 0) pairs.emplace_back(bm, rm);
            }
            if (!parityRam.empty()) // --parity: the capture's pairs too (its pool 0 is bound after the first frame)
                for (uint32_t b = 1; b < 18; ++b) {
                    const uint32_t e = 0x801B65D4u + 1096u * b;
                    uint32_t bm = 0, rm = 0, owner = 0;
                    int a = 0;
                    std::memcpy(&owner, parityRam.data() + ((e + 0x354u) & 0x1FFFFFu), 4);
                    ObjectLook(parityRam.data(), e, bm, a);
                    ObjectLook(parityRam.data(), owner, rm, a);
                    if (bm != 0) pairs.emplace_back(bm, rm);
                }
            scene.LoadRivalMachines(disc, pairs);
        }
        {   // the two-seat machine: player 1's sidecar rig (race_scene_sidecar.cpp; RASHCDI 0x8005C45C loads each
            // player's .MRO - OURS: the scene holds one rig, player 1's)
            uint32_t b4 = 0;
            std::memcpy(&b4, session.ArenaRam() + ((session.Bikes()[0].entityAddress + 0xB4u) & 0x1FFFFFu), 4);
            uint32_t gsp = 0;
            std::memcpy(&gsp, session.ArenaRam() + (0x8005B2F8u & 0x1FFFFFu), 4);
            scene.LoadSidecar(disc, b4, rr::LevelBankIndexOf(session.ArenaRam() + (gsp & 0x1FFFFFu)),
                              rr::game::PlayerPaletteBlock(session.ArenaRam(), 0));
            if (session.Players() == 2 && session.Bikes().size() > 1 && rr::game::Mp2On()) { // player 2's own (sidecar2)
                uint32_t b4p2 = 0;
                std::memcpy(&b4p2, session.ArenaRam() + ((session.Bikes()[1].entityAddress + 0xB4u) & 0x1FFFFFu), 4);
                scene.LoadSidecarFor(1, disc, b4p2, rr::LevelBankIndexOf(session.ArenaRam() + (gsp & 0x1FFFFFu)),
                                     rr::game::PlayerPaletteBlock(session.ArenaRam(), 1));
            }
        }
        rr::render::TrafficDraw traffic; // the pool-3 cars, with their models (traffic_draw.h)
        // the sidecar rig drawn from its own part slots (race_scene_sidecar.cpp); RRJB_RIG=off: the rest pose
        const bool rigOff = std::getenv("RRJB_RIG") != nullptr && std::strcmp(std::getenv("RRJB_RIG"), "off") == 0;
        {   // the level bundle LoadBikeBank RASHCDI 0x8005C45C picks (rrformats/level_bank.h): the cars' palette bank
            const uint8_t* ar = session.ArenaRam();
            uint32_t gs = 0;
            std::memcpy(&gs, ar + (0x8005B2F8u & 0x1FFFFFu), 4);
            traffic.Load(disc, raceId, rr::LevelBankIndexOf(ar + (gs & 0x1FFFFFu)));
        }
        rr::render::PedDraw pedDraw; // the pedestrians of pool 2, posed (ped_draw.h)
        if (rr::game::PedsEnabled()) pedDraw.Load(disc, raceId);
        rr::render::WeaponDraw weapons; // the weapon in a rider's hand (weapon_draw.h)
        weapons.Load(disc, 0);          // OURS: bank 0's BBLEVEL1.GEO, the file LoadMachine draws the riders from
        rr::game::HeadCameraRig& headCam = rr::game::ProductHeadCamera(); // the head camera (head_camera.h)
        const bool headCamLoaded = headCam.Load(disc);
        if (vr != nullptr) { // VR: the headset supplies the pitch (head_camera.h), the view starts where the settings say
            headCam.lookDownDegrees = 0.0f;
            if (!headView) headView = vr->Settings().headView;
        }
        if (!headCamLoaded) headView = false;
        if (headCamLoaded && (vr != nullptr || headView)) { // vr_bike_shake.h: the own bike's drawn slots
            rrgame::ProductBikeShake().Invalidate();
            rrgame::ProductBikeShakeMeter().Load(disc);
        }
        host.LoadingTick();
        // The effects (fx_runtime.h): the sheet, the ordering table, and the PORTED pass each frame.
        rr::game::FxRuntime fx;
        rr::game::ModelRuntime models; // model_runtime.h: the PORTED model draw before the pass (the effect capture)
        fx.modelPass = [&models, &fx](rr::sim::GuestRam& mg, const std::vector<uint32_t>& ents, std::vector<uint32_t>& drawn,
                                      uint32_t fxView, int32_t ofx, int32_t ofy) {
            models.fx = fx.ActiveEnv(); // the glow sprites link into the pass's table (fx_glow.h)
            const bool ran = models.Frame(mg, ents, drawn, fxView, ofx, ofy);
            models.fx = nullptr;
            return ran;
        };
        {
            rr::sim::GuestRam fg(session.MutableArenaRam(), 0x8005AC8Cu);
            uint32_t gs = 0, players = 1;
            std::memcpy(&gs, session.ArenaRam() + (0x8005B2F8u & 0x1FFFFFu), 4);
            std::memcpy(&players, session.ArenaRam() + ((gs + 0x30u) & 0x1FFFFFu), 4);
            session.NoteSeam(fx.Setup(fg, disc, raceId, static_cast<int>(players)));
            session.NoteSeam(rr::game::LevelLightStores(fg, disc, raceId)); // the loader's light stores
            session.NoteSeam(rr::game::SkyCloudSetUp(fg, disc, raceId)); // the cloud layer's tables
        }
        rr::render::FxDraw fxDraw;
        fxDraw.Init();
        size_t trafficMaxLive = 0, trafficMaxCops = 0, spawnerFrames = 0, spawnerDeclined = 0, trafficFrames = 0,
               trafficDeclined = 0;
        Hud hud;
        if (drawHud) hud.Init(!parityState.empty());
        // The race frame's views (race_render.h): everything one view draws, the VR-ready entry.
        rrgame::RaceRenderer::Options renderOptions;
        renderOptions.drawFx = drawFx;
        renderOptions.drawRangeOff = drawRangeOff;
        renderOptions.propsFromRecords = propsFromRecords;
        renderOptions.parity = !parityState.empty();
        renderOptions.shownOff = shownOff;
        renderOptions.sunOff = sunOff;
        renderOptions.modelLightOff = modelLightOff;
        renderOptions.rigOff = rigOff;
        rrgame::RaceRenderer raceRender(session, world, scene, traffic, pedDraw, weapons, fx, models, fxDraw, cellIndexById,
                                        renderOptions);
        raceRender.onPlayerRiderCaptured = [&headCam](const CapturedVerts& c) { // the head camera's oracle
            headCam.NoteCaptured(0, c.model, c.lod, c.eye, c.rel);
        };

        // The rivals a view drew last frame and whether they were in the air then (the draw-range check):
        // a rival drawn airborne in one frame and gone the next, retired to the dormant list, is a bike
        // seen to fly off and never come down.
        // (the rivals' draw-range account, the subdiv and otsort counters: race_render.h RaceRenderer::Counters)
        // DEVELOPMENT (RRJB_RENDER_TIME=1): the 3D views' wall time per frame, CPU and GPU (glFinish on both sides)
        const bool renderTimed = std::getenv("RRJB_RENDER_TIME") != nullptr;
        double renderSeconds = 0.0, renderWorst = 0.0;
        long renderFrames = 0;
        std::string log;
        log += "rrgame frame log. One line per frame: what the loop actually called.\n";
        log += "t=ticks dt=16.16 s clock=race clock in ticks cd=countdown 16.16 s\n";
        log += "Every number after the first bar is read back out of the session's guest arena after\n";
        log += "the frame: the PORTED engine's +0x1E0/+0x250/+0x25C/+0x351, region E's speed +0x240,\n";
        log += "the servo's +0x27C, the box centre +0xB8 the PORTED integrator and contact response\n";
        log += "moved. pos is OURS and display only: the slice of world.cpp's route nearest that box.\n\n";

        // Run totals of the PORTED road layer and ground query (the frame log is per frame).
        struct GroundTotals {
            size_t offRoadFrames = 0, maxOffRoad = 0, playerOffRoadFrames = 0, declined = 0;
            size_t queries = 0, hits = 0, exhausted = 0, misses = 0, qDeclined = 0, loads = 0, unloads = 0;
            size_t fineHits = 0, playerHits = 0, playerFineHits = 0;
            size_t maxResident = 0;
            int16_t lastHitNormal[3] = {0, 0, 0};
            uint32_t lastHitAnswer = 0;
            uint16_t minPitch = 0xFFFF, maxPitch = 0;
        } totals;

        glEnable(GL_DEPTH_TEST);
        auto last = std::chrono::steady_clock::now();
        long frames = 0;
        FILE* heartbeat = heartbeatPath.empty() ? nullptr : std::fopen(heartbeatPath.c_str(), "w");
        std::vector<uint8_t> lastShot;
        int lastWidth = 0, lastHeight = 0;
        bool cameraKeyWas = false;
        const std::unique_ptr<rrgame::PadDevice> gamepad = host.OpenPad(), gamepad2 = host.OpenPad();
        rr::game::SecondPlayerInput input2; // mp_input.h
        const bool twoPlayers = session.Players() == 2; // the arrows are player 2's then
        if (twoPlayers)
            std::printf("two players: player 1 W/A/S/D (+ Z X Q R F C V T) and the first controller; player 2 the arrows\n"
                        "             (+ M , . K L, N taunt, / camera, ; look back, F6 analogue) and the second controller\n");
        bool padCameraWas = false, f2Was = false, f3Was = false, f4Was = false;
        const char* padSource = "none";
        // Run totals of the camera and the population.
        struct CameraTotals {
            size_t frames = 0, declined = 0, fine = 0, racing = 0;
            double minDist = 1e30, maxDist = 0.0, sumDist = 0.0;
            // the eye against what the camera follows: the rider's box once he is off the bike (+0x25C >= 2)
            double subjMax = 0.0, subjSum = 0.0;
            size_t offFrames = 0;
            long subjMaxFrame = 0;
            size_t minLive = 1000, maxLive = 0, maxDormant = 0, transitions = 0, refused = 0, downed = 0;
            size_t downedRefused = 0, dormantDrives = 0, bursts = 0, sprays = 0, maxBusy = 0, crashEmits = 0;
            size_t crashDeclined = 0, stamps = 0, resets = 0, viewEvents = 0, raceGo = 0;
            uint32_t lastRefusal = 0;
        } cam;
        // ---- the picture: the views, the offscreen target when the settings
        // ask for another resolution or multisampling, the PS1 look, the HUD, the overlay and the profiler.
        RenderTarget target;
        TextOverlay overlayText;
        rrgame::PcOverlay overlay;
        rrgame::FrameProfiler profiler;
        GpuTimer gpuTimer;
        rrgame::ProfileNumbers profile;
        profile.race = true;
        struct ProfileTotals {
            long frames = 0;
            double cpuMs = 0, renderMs = 0, gpuMs = 0, frameMs = 0, worstMs = 0;
            long gpuFrames = 0;
            unsigned long long drawCalls = 0, triangles = 0;
            size_t cellRuns = 0, cellRunsCulled = 0, band2 = 0, band2Culled = 0, props = 0, propsCulled = 0, objects = 0,
                   objectsCulled = 0;
            int renderW = 0, renderH = 0, samples = 1;
        } ptotals;
        const bool profiling = interactive || !profileLog.empty();
        // DEVELOPMENT: RRJB_OVERLAY_SHOT=<page> draws the settings overlay open on that page in a
        // scripted run (its shot shows it; no input is read); RRJB_PC_CULL=off measures maximum detail without the
        // frustum cull.
        const bool overlayShot = std::getenv("RRJB_OVERLAY_SHOT") != nullptr;
        if (overlayShot) overlay.ShowPage(std::atoi(std::getenv("RRJB_OVERLAY_SHOT")));
        const bool cullOff = std::getenv("RRJB_PC_CULL") != nullptr && std::strcmp(std::getenv("RRJB_PC_CULL"), "off") == 0;
        int width = 0, height = 0;
        bool headNow = false, headLookBack = false; // the head camera's view this frame (set by the frame's step)
        int32_t deskStepTicks = 5; // the last step's ticks (the desktop head camera's low-pass)
        rr::game::HeadPose headPose;
        rrgame::VisualLean handlingLean; // the Modern handling's drawn lean of the player's bike (off: none)
        bool syncWide = wideScreen, syncDither = ps1Dither, syncColour = ps1Colour, appliedVsync = false, appliedFull = false;
        bool firstApply = true;
        auto frameStart = std::chrono::steady_clock::now();
        auto nextFrame = frameStart;
        double renderCpuMs = 0.0;
        // The settings into the scene and the renderer; F2 / F3 / F4 and the overlay edit the same values.
        const auto applySettings = [&]() {
            bool keysChanged = false;
            if (wideScreen != syncWide) gfx.wide = wideScreen, keysChanged = true;
            if (ps1Dither != syncDither) gfx.ps1Dither = ps1Dither, keysChanged = true;
            if (ps1Colour != syncColour) gfx.ps1Colour = ps1Colour, keysChanged = true;
            if (keysChanged && interactive) rrgame::SaveGraphics(rrgame::SettingsPath(), gfx);
            wideScreen = syncWide = gfx.wide;
            ps1Dither = syncDither = gfx.ps1Dither;
            ps1Colour = syncColour = gfx.ps1Colour;
            RaceScene::PcOptions pc;
            pc.maxDetail = gfx.maxDetail;
            pc.drawDistance = gfx.drawDistance;
            pc.cull = (gfx.maxDetail || gfx.drawDistance > 0) && !cullOff;
            pc.smoothTextures = gfx.smoothTextures;
            pc.farPlane = gfx.drawDistance >= 2 ? 60000.0f : 20000.0f;
            pc.centroid = (vr != nullptr ? vr->Settings().msaa : gfx.msaa) > 1; // texels at the centroid when multisampled
            scene.SetPcOptions(pc);
            if (gfx.affine) scene.SetAffine(envAffine, envSubdivide);
            else scene.SetAffine(false, false);
            raceRender.options.wideScreen = wideScreen;
            raceRender.options.otOrder = !otZbuffer && gfx.ps1DrawOrder;
            raceRender.options.preciseVertices = gfx.preciseVertices;
            raceRender.options.maxDetail = gfx.maxDetail;
            raceRender.options.cull = pc.cull;
            if (interactive) {
                if (firstApply || gfx.vsync != appliedVsync) host.SetVsync(gfx.vsync);
                if (firstApply || gfx.fullscreen != appliedFull) host.SetFullscreen(gfx.fullscreen);
                appliedVsync = gfx.vsync;
                appliedFull = gfx.fullscreen;
            } else if (firstApply && rrgame::GraphicsFlagGiven("vsync")) { // a scripted measurement: --vsync 0 / 1
                host.SetVsync(gfx.vsync);
            }
            if (profiling && firstApply) CountDrawCalls(true);
            firstApply = false;
        };
        // One picture of the race as the session holds it: the views, the PS1 look, the HUD. False when the window
        // has no area (minimized with a zero client rect).
        const auto drawPicture = [&]() -> bool {
            host.ClientRect(width, height);
            if (width <= 0 || height <= 0) return false;
            applySettings();
            const uint64_t callsBefore = DrawCallCount(), trianglesBefore = DrawTriangleCount();
            const bool timeGpu = profiling && (gfx.profiler || !profileLog.empty());
            if (timeGpu) gpuTimer.Begin();
            // The internal resolution: the window's, a percentage of it or a fixed height (the offscreen target).
            bool useTarget = !parityState.empty() ? false : gfx.NeedsTarget();
            int rw = width, rh = height;
            if (useTarget) {
                if (gfx.renderHeight > 0) {
                    rh = gfx.renderHeight;
                    rw = static_cast<int>(std::lround(static_cast<double>(rh) * width / height));
                } else {
                    rw = std::max(16, width * gfx.renderScale / 100);
                    rh = std::max(16, height * gfx.renderScale / 100);
                }
                useTarget = target.Ensure(rw, rh, gfx.msaa);
                if (!useTarget) rw = width, rh = height;
            }
            if (useTarget) target.Bind();
            // The picture: the original's 4:3 frame centred with black bars, or (--wide / F2) the
            // whole window with the original's vertical field and more at the sides.
            int viewX = 0, viewY = 0, viewW = rw, viewH = rh;
            if (!wideScreen) {
                viewW = std::min(rw, rh * 4 / 3);
                viewH = std::min(rh, viewW * 3 / 4);
                viewX = (rw - viewW) / 2;
                viewY = (rh - viewH) / 2;
            }
            glViewport(0, 0, rw, rh);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            const int fullX = viewX, fullY = viewY, fullW = viewW, fullH = viewH;
            if (session.Players() == 2) {
                glEnable(GL_SCISSOR_TEST);
                glScissor(fullX, fullY, fullW, fullH);
            }
            if (renderTimed) glFinish();
            const auto renderStart = std::chrono::steady_clock::now();
            raceRender.counters.objectsDrawn = raceRender.counters.objectsCulled = 0;
            size_t cellRuns = 0, cellRunsCulled = 0, band2 = 0, band2Culled = 0, props = 0, propsCulled = 0;
            for (int vp = 0; vp < session.Players(); ++vp) {
                rrgame::GameView view = raceRender.GameCamera(vp, fullX, fullY, fullW, fullH, useTarget ? target.DrawFramebuffer() : 0u);
                view.farPlane = gfx.drawDistance >= 2 ? 60000.0f : 20000.0f; // the PC settings' maximum draw distance
                if (view.farPlane != 20000.0f) raceRender.Reproject(view);
                if (vp == 0) view.playerLean = handlingLean; // Modern's drawn lean (off under Original)
                if (headNow && vp == 0) { // the head camera (head_camera.h): the eye at the head, the
                    // bike's frame (V looks behind), a near plane of 0.05 world units (the hands and the handlebars are
                    // that close), its own float camera and the depth buffer, not its own head, the machine posed
                    headCam.NoteDrawn();
                    for (int k = 0; k < 3; ++k) {
                        view.eye[k] = headPose.eye[k];
                        view.target[k] = headPose.eye[k] + (headLookBack ? -headPose.fwd[k] : headPose.fwd[k]);
                        view.up[k] = headPose.up[k];
                    }
                    if (handlingLean.on) { // the eye rides the drawn bike's Modern lean
                        float fwd[3] = {view.target[0] - view.eye[0], view.target[1] - view.eye[1], view.target[2] - view.eye[2]};
                        handlingLean.Point(view.eye, view.eye);
                        handlingLean.Dir(fwd, fwd);
                        handlingLean.Dir(view.up, view.up);
                        for (int k = 0; k < 3; ++k) view.target[k] = view.eye[k] + fwd[k];
                    }
                    if (vr == nullptr && !headLookBack) { // the desktop head view's drawn lean and roll
                        const float fwd[3] = {view.target[0] - view.eye[0], view.target[1] - view.eye[1], view.target[2] - view.eye[2]};
                        rrgame::HandlingView().Note(frames, false, handlingLean.on ? handlingLean
                                                        : rrgame::HandlingGameRoll(session.ArenaRam(), session.Bikes()[0].entityAddress),
                                                    fwd, view.up);
                    }
                    view.nearPlane = 0.05f;
                    view.consoleCamera = false;
                    view.riderHideParts = headCam.HiddenRiderParts();
                    view.playerFromCaptures = false;
                    raceRender.Reproject(view);
                }
                // vr_bike_shake.h: the desktop head camera's own bike drawn with the [vr] bike_shake slots
                const bool deskShake = headNow && vp == 0 && vr == nullptr &&
                                       rrgame::ProductBikeShake().Apply(session.MutableArenaRam(), session.Bikes()[0].entityAddress,
                                                                        rrgame::VrPrefs().bikeShake, 1.0f, deskStepTicks / 300.0);
                raceRender.RenderView(view, frames);
                if (headNow && vp == 0 && vr == nullptr && raceRender.drawnPlayerMachine.valid &&
                    raceRender.drawnPlayerMachine.frame == frames) { // the shake meter
                    const auto& d = raceRender.drawnPlayerMachine;
                    rr::PartMatrix gameParts[5];
                    const bool haveGame = rrgame::ProductBikeShake().GameParts(session.Bikes()[0].entityAddress, gameParts);
                    const float fwd[3] = {view.target[0] - view.eye[0], view.target[1] - view.eye[1], view.target[2] - view.eye[2]};
                    rrgame::ProductBikeShakeMeter().Frame(frames, view.eye, fwd, view.up, 1.0f, deskStepTicks / 300.0, d.model,
                                                          d.posed ? d.parts : nullptr, haveGame ? gameParts : nullptr);
                } else if (vp == 0 && vr == nullptr) {
                    rrgame::ProductBikeShake().Break();
                    rrgame::ProductBikeShakeMeter().Break();
                }
                if (deskShake) rrgame::ProductBikeShake().Restore(session.MutableArenaRam());
                if (vp == 0) rrgame::ZFightProbe(raceRender, view, frames); // DEVELOPMENT: RRJB_ZFIGHT (zfight_probe.cpp)
                if (vp == 0) rrgame::CarFlickProbe(raceRender, view, frames); // DEVELOPMENT: RRJB_CARFLICK
                const RaceScene::FrameStats& st = scene.LastStats();
                cellRuns += st.cellRuns;
                cellRunsCulled += st.cellRunsCulled;
                band2 += st.band2Pieces;
                band2Culled += st.band2Culled;
                props += st.props;
                propsCulled += st.propsCulled;
            }
            if (session.Players() == 2) {
                glDisable(GL_SCISSOR_TEST);
                viewX = fullX;
                viewY = fullY;
                viewW = fullW;
                viewH = fullH;
                glViewport(viewX, viewY, viewW, viewH);
            }
            // The PS1 look over the 3D picture, before the HUD (the GPU does not dither sprites).
            if (renderTimed) {
                glFinish();
                const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - renderStart).count();
                if (frames > 60) { // past the first frames' uploads
                    renderSeconds += s;
                    renderWorst = std::max(renderWorst, s);
                    ++renderFrames;
                }
            }
            renderCpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - renderStart).count();
            if (useTarget) { // the target: its PS1 look at its own resolution, then scaled to the window, the HUD at the window's
                target.Resolve();
                glViewport(viewX, viewY, viewW, viewH);
                if (ps1Colour) scene.PostProcess(viewX, viewY, viewW, viewH, ps1Dither);
                target.PresentToWindow(0, 0, width, height);
                int wx = 0, wy = 0, ww = width, wh = height;
                if (!wideScreen) {
                    ww = std::min(width, height * 4 / 3);
                    wh = std::min(height, ww * 3 / 4);
                    wx = (width - ww) / 2;
                    wy = (height - wh) / 2;
                }
                glViewport(wx, wy, ww, wh);
                if (drawHud) hud.Draw(session);
            } else {
                if (ps1Colour) scene.PostProcess(viewX, viewY, viewW, viewH, ps1Dither);
                if (drawHud) hud.Draw(session);
            }
            if ((interactive || overlayShot) && (overlay.Open() || gfx.profiler)) { // the overlay and the profiler, at the window's size
                if (!overlayText.Ready()) overlayText.Init();
                profile.frames = &profiler;
                profile.renderCpuMs = renderCpuMs;
                profile.gpuMs = gpuTimer.LastMs();
                profile.renderW = rw;
                profile.renderH = rh;
                profile.samples = useTarget ? target.Samples() : 1;
                profile.cellsDrawn = cellRuns + band2;
                profile.cellsCulled = cellRunsCulled + band2Culled;
                profile.propsDrawn = props;
                profile.propsCulled = propsCulled;
                profile.objectsDrawn = raceRender.counters.objectsDrawn;
                profile.objectsCulled = raceRender.counters.objectsCulled;
                overlay.Draw(overlayText, width, height, profile);
                glViewport(0, 0, width, height);
            }
            if (timeGpu) gpuTimer.End();
            profile.drawCalls = DrawCallCount() - callsBefore;
            profile.triangles = DrawTriangleCount() - trianglesBefore;
            if (!profileLog.empty() && frames > 60) { // the run's totals past the first frames' uploads
                ++ptotals.frames;
                ptotals.renderMs += renderCpuMs;
                ptotals.drawCalls += profile.drawCalls;
                ptotals.triangles += profile.triangles;
                ptotals.cellRuns += cellRuns + band2;
                ptotals.cellRunsCulled += cellRunsCulled + band2Culled;
                ptotals.props += props;
                ptotals.propsCulled += propsCulled;
                ptotals.objects += raceRender.counters.objectsDrawn;
                ptotals.objectsCulled += raceRender.counters.objectsCulled;
                ptotals.renderW = rw;
                ptotals.renderH = rh;
                ptotals.samples = useTarget ? target.Samples() : 1;
            }
            return true;
        };
        // After the swap: the profiler's clock, the frame cap (an interactive run).
        const auto frameDone = [&]() {
            const auto now = std::chrono::steady_clock::now();
            const double cpuMs = std::chrono::duration<double, std::milli>(now - frameStart).count();
            profile.cpuMs = cpuMs;
            profiler.Record(std::chrono::duration<double>(now.time_since_epoch()).count());
            if (!profileLog.empty() && frames > 60) {
                ptotals.cpuMs += cpuMs;
                if (gpuTimer.LastMs() >= 0) {
                    ptotals.gpuMs += gpuTimer.LastMs();
                    ++ptotals.gpuFrames;
                }
            }
            if (interactive && gfx.fpsCap > 0) {
                const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(1.0 / gfx.fpsCap));
                nextFrame += period;
                if (nextFrame < now - period) nextFrame = now; // fell behind (a load): no catch-up burst
                std::this_thread::sleep_until(nextFrame);
            } else {
                nextFrame = now;
            }
            const auto end = std::chrono::steady_clock::now();
            if (!profileLog.empty() && frames > 60) {
                const double f = std::chrono::duration<double, std::milli>(end - frameStart).count();
                ptotals.frameMs += f;
                ptotals.worstMs = std::max(ptotals.worstMs, f);
            }
            frameStart = end;
        };
        // The settings overlay (pc_overlay.h): F10 / Select + Start. While it is open - and after it closes, until the
        // keys and buttons that closed it are let go - the race does not advance: the picture is drawn again from the
        // session as it is, the sound is held and the clock of the time step restarts.
        bool overlaySwallow = false;
        const auto overlayHolds = [&](const rr::platform::GamepadState& pad) -> bool {
            if (!interactive) return false;
            const bool open = overlay.Update(keys, pad);
            if (open) overlaySwallow = true;
            if (!open && overlaySwallow) {
                using rr::platform::PadButton;
                const bool held = keys[VK_ESCAPE] || keys[VK_RETURN] || keys[VK_SPACE] ||
                                  keys[VK_BACK] || keys[VK_F10] || pad.raw.Down(PadButton::Start) ||
                                  pad.raw.Down(PadButton::Select) || pad.raw.Down(PadButton::Triangle) ||
                                  pad.raw.Down(PadButton::Cross);
                if (!held) overlaySwallow = false;
            }
            sink.muted.store(overlaySwallow, std::memory_order_relaxed);
            if (!overlaySwallow) return false;
            if (overlay.TakeChanged()) applySettings();
            if (drawPicture()) host.Present();
            frameDone();
            last = std::chrono::steady_clock::now(); // the time step does not see the held time
            return true;
        };
        // ---- VR (game_host_vr.h): one stereo frame of the race as the session holds
        // it. Per eye race_render.h RenderView with consoleCamera = false and the eye's own projection, anchored on the
        // rider's head (head view) or the original's chase camera, horizon-locked (vr_rig.h); the original HUD on a
        // panel fixed to the levelled seat, the pause menu and the VR menu on the theatre quad.
        rrgame::VrPanel vrHudPanel;
        rrgame::VrVignette vrVignette;
        rrgame::VrHandlebars vrBars; // the Handlebars steering mode (vr_handlebars.h): grips, gloves, twist, swings
        rrgame::VrMelee vrMelee;     // physical combat (vr_melee.h): contacts of the tracked hands
        rrgame::VrHolsters vrHolsters; // the holsters and the weapons in the hands (vr_holsters.h)
        rr::xr::WorldAnchor vrLastAnchor;
        auto vrLastTime = std::chrono::steady_clock::now();
        bool vrHaveAnchor = false;
        size_t vrHeadFrames = 0, vrChaseFrames = 0, vrPauseQuadFrames = 0;
        // VR's time step (vr_pacing.h): on the display's clock - rrgame::ProductPacing()
        // vr_comfort.h: smooth motion - the last step's length, whether the step came from the tick carry
        // (a headset, --vr-mock-hz: then the frame is drawn between the last two steps), the cameras of the last two steps
        int32_t vrStepTicks = 0;
        bool vrCarryStep = false;
        rrgame::CamPose vrHeadCam[2], vrChaseCam[2]; // [0] the step before, [1] the last step
        bool vrStabHead = false; // the last frame stabilised the head view's bike (a new head view starts it again)
        rrgame::FallView vrFall;
        rrgame::VrFade vrFade;
        struct {
            bool valid = false;
            uint32_t bike = 0, rider = 0;
            const float* seat = nullptr;
        } vrBikeCall; // vrBars.UpdateBike's arguments of the last step (called again on the drawn state)
        bool vrViewSetting = vr != nullptr && vr->Settings().headView; // the VR menu's View row followed mid-race
        // the sound's vertical blanks: one per kTicksPerVblank of game time
        constexpr int32_t kTicksPerVblank = 5;
        int32_t vsyncTickCarry = 0;
        long long soundTicks = 0;
        size_t mockHitRumbles = 0; // the VR mock's haptics measurement
        std::string mockHitFrames;
        size_t hostVsyncs = 0, tallyVsyncs = 0;
        struct {
            bool active = false;
            uint32_t frame = 0;
            size_t landed = 0, hurt = 0;
        } heldBlow; // the hit tally's blow waiting for its vertical blank
        static const bool vsyncTwoPerFrame = [] {
            const char* v = std::getenv("RRJB_SOUND_VSYNC");
            return v != nullptr && std::strcmp(v, "frame") == 0;
        }();
        std::FILE* engineTrace = nullptr; // RRJB_ENGINE_TRACE=<csv>: the engine record per frame (a measurement)
        if (const char* tp = std::getenv("RRJB_ENGINE_TRACE")) {
            engineTrace = std::fopen(tp, "wb");
            if (engineTrace) std::fprintf(engineTrace, "frame,ticks,vsyncs,level,levelTarget,load,loadTarget\n");
        }
        struct TraceCloser {
            std::FILE*& f;
            ~TraceCloser() {
                if (f) std::fclose(f);
            }
        } traceCloser{engineTrace};
        struct VrCpu {
            long frames = 0;
            double simMs = 0.0, eyeMs[2] = {0.0, 0.0};
            std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        } vrCpu;
        struct MuteGuard { // the host mutes the race's sound while it blocks; the sink goes away with this function
            rrgame::VrHost* vr;
            ~MuteGuard() {
                if (vr != nullptr) vr->SetAudioMute(nullptr);
            }
        } muteGuard{vr};
        if (vr != nullptr) {
            vr->SetAudioMute(&sink.muted);
            vr->RequestRecentre(); // the seat is latched on the head at the race's start
            rrgame::ProductFrameInterp().Invalidate(); // no step of an earlier race to draw from
            rrgame::ProductViewPitch().Invalidate();
            rrgame::ProductRivalSmooth().Reset();
            rrgame::ProductPacing().Reset(); // an earlier race's clock does not step this one
            vrBars.Load(disc, vr->Config().mock);
            vrMelee.Load(disc, vr->Config().mock);
            vrHolsters.Load(disc, vr->Config().mock);
        }
        // vr_pacing.h: the frame opened before the step (the display clock's mode): 1 open, 0 the host
        // had no frame, -1 not opened yet (vrFrame opens it)
        int vrOpen = -1;
        const auto vrFrame = [&]() {
            applySettings();
            const int opened = vrOpen;
            vrOpen = -1;
            if (opened == 0) return;
            if (opened < 0 && !vr->BeginFrame()) return;
            // vr_comfort.h: smooth motion - the frame is drawn at a uniform time between the last two
            // steps (one tick behind the real time): the objects' fields written into the arena for this frame's
            // drawing and put back when the frame is done (the guard), the grips / lean / eye on that drawn state
            rrgame::FrameInterp& vrInterp = rrgame::ProductFrameInterp();
            // vr_pacing.h: the frame's display clock (xrWaitFrame) - the drawn time between the last two
            // steps, and the filters' dt, from where this frame's display time falls
            rrgame::DisplayPacing& vrPacing = rrgame::ProductPacing();
            if (opened < 0) vrPacing.Frame(vr->FrameClock());
            const bool vrSmooth = vrCarryStep && vr->Settings().smoothMotion;
            const float vrAlpha = vrCarryStep ? vrPacing.Alpha(vrSmooth && rrgame::SmoothInterpOn()) : 1.0f;
            const double vrRatePeriod = vr->Config().mock && rrgame::MockDisplayHz() > 0.0 ? 1.0 / rrgame::MockDisplayHz()
                                                                                            : vr->DisplayPeriod();
            const double vrPeriod = vrCarryStep && vrPacing.Mode() == rrgame::PacingMode::kDisplay
                                        ? vrPacing.FrameSeconds(vrRatePeriod)
                                        : vrRatePeriod;
            struct InterpGuard {
                rrgame::FrameInterp& f;
                uint8_t* ram;
                ~InterpGuard() { f.Restore(ram); }
            } interpGuard{vrInterp, session.MutableArenaRam()};
            struct PitchGuard { // ViewPitch's share handed to the wheelie layer is this frame's only
                ~PitchGuard() { rrgame::ProductViewPitch().EndFrame(); }
            } pitchGuard;
            if (vrSmooth) {
                bool patched = vrInterp.Apply(session.MutableArenaRam(), vrAlpha);
                // vr_horizon.h: the head view's own bike pitched with the road, not with its own nods
                // (the wheelie / stoppie move), before the stabiliser takes it as the drawn bike
                if (headNow && vrBikeCall.valid) {
                    rrgame::WheelContacts wheels;
                    float front[2], rear[2];
                    if (vrBars.WheelContacts(front, rear)) {
                        wheels.valid = true;
                        wheels.frontUp = front[0], wheels.frontFwd = front[1], wheels.rearUp = rear[0], wheels.rearFwd = rear[1];
                    }
                    patched = rrgame::ProductViewPitch().Apply(session.MutableArenaRam(), vrBikeCall.bike, vr->Settings().viewPitch,
                                                               vrAlpha, vrPeriod, wheels, vrInterp) || patched;
                } else {
                    rrgame::ProductViewPitch().Break();
                }
                // the head view's own bike stabilised, the eye rigid on it (FrameInterp::Stabilize)
                if (headNow && rrgame::SmoothStableOn() && vrBikeCall.valid)
                    patched = vrInterp.Stabilize(session.MutableArenaRam(), vrBikeCall.bike, vrPeriod, vrStepTicks / 300.0,
                                                 !vrStabHead) || patched;
                vrStabHead = headNow;
                { // vr_horizon.h RivalSmooth: the rival bikes near the player as smooth as the player's own
                    std::vector<uint32_t> rivalBikes, rivalRiders;
                    for (size_t i = 1; i < session.Bikes().size(); ++i)
                        if (session.BikeLive(i)) {
                            rivalBikes.push_back(session.Bikes()[i].entityAddress);
                            rivalRiders.push_back(session.Bikes()[i].ownerAddress);
                        }
                    float centre[3];
                    for (uint32_t k = 0; k < 3; ++k) {
                        int32_t v = 0;
                        std::memcpy(&v, session.ArenaRam() + ((session.Bikes()[0].entityAddress + 0xB8u + 4u * k) & 0x1FFFFFu), 4);
                        centre[k] = static_cast<float>(static_cast<double>(v) / 65536.0);
                    }
                    rrgame::ProductRivalSmooth().Apply(session.MutableArenaRam(), rivalBikes, rivalRiders, centre, vrPeriod, frames,
                                                       vrInterp);
                }
                if (patched && vrBikeCall.valid)
                    vrBars.UpdateBike(session.ArenaRam(), vrBikeCall.bike, vrBikeCall.rider, vrBikeCall.seat, headNow);
            }
            // vr_bike_shake.h: the own bike's part slots in the head view, for this frame's drawing (the
            // renderer, the grips, the weapon on the bars), put back by the guard
            struct ShakeGuard {
                uint8_t* ram;
                ~ShakeGuard() { rrgame::ProductBikeShake().Restore(ram); }
            } shakeGuard{session.MutableArenaRam()};
            if (headNow && vrBikeCall.valid) {
                if (rrgame::ProductBikeShake().Apply(session.MutableArenaRam(), vrBikeCall.bike, vr->Settings().bikeShake,
                                                     vrAlpha, vrPeriod))
                    vrBars.UpdateBike(session.ArenaRam(), vrBikeCall.bike, vrBikeCall.rider, vrBikeCall.seat, headNow);
            } else {
                rrgame::ProductBikeShake().Break();
            }
            float vrFadeNow = 0.0f; // the black over the eyes at a fall / a re-seat
            if (vr->ShouldRender()) {
                const rrgame::GameView base = raceRender.GameCamera(0, 0, 0, 16, 16, 0);
                rrgame::vr::Camera cam;
                const bool head = headNow;
                // the cameras at the drawn time, and the head-view mode's view off the bike (a short fade at
                // both ends, fixed and level while off; RRJB_FALL_VIEW=legacy: the original's camera, no fade)
                rrgame::CamPose drawnCam = rrgame::LerpCam(head ? vrHeadCam[0] : vrChaseCam[0],
                                                           head ? vrHeadCam[1] : vrChaseCam[1], vrAlpha);
                float stableFwd[3], stableUp[3]; // the stabilised bike's axes: the head camera rigid on the drawn bike
                if (head && drawnCam.valid && vrInterp.StableAxes(stableFwd, stableUp)) {
                    std::memcpy(drawnCam.fwd, stableFwd, sizeof(stableFwd));
                    std::memcpy(drawnCam.up, stableUp, sizeof(stableUp));
                }
                if (!drawnCam.valid) { // (no capture yet)
                    drawnCam.valid = true;
                    for (int k = 0; k < 3; ++k) {
                        drawnCam.eye[k] = head ? headPose.eye[k] : base.eye[k];
                        drawnCam.fwd[k] = head ? headPose.fwd[k] : base.target[k] - base.eye[k];
                        drawnCam.up[k] = head ? headPose.up[k] : base.up[k];
                    }
                }
                if (headView && session.Players() == 1) { // the head-view mode
                    bool changed = false, off = !head;
                    if (!rrgame::FallViewLegacy()) {
                        rr::game::HeadPose rp; // the rider's pelvis (the head camera's unsmoothed root, on or off the bike)
                        float rider[3];
                        if (headCam.Get(0, rp)) std::memcpy(rider, rp.root, sizeof(rider));
                        else std::memcpy(rider, drawnCam.eye, sizeof(rider));
                        const rrgame::CamPose chase = head ? rrgame::LerpCam(vrChaseCam[0], vrChaseCam[1], vrAlpha) : drawnCam;
                        drawnCam = vrFall.Frame(head, drawnCam, chase, rider, vr->Settings().fallFixed,
                                                static_cast<float>(vrPeriod), vrFadeNow);
                        changed = vrFall.JustChanged();
                        off = vrFall.Off();
                    } else {
                        static int legacyHead = -1; // (the control: the same counting)
                        changed = legacyHead >= 0 && legacyHead != static_cast<int>(head);
                        legacyHead = head ? 1 : 0;
                    }
                    rrgame::ComfortCounters& cc = rrgame::Comfort();
                    if (changed) {
                        vrHaveAnchor = false; // the vignette's turn rate starts again
                        ++(off ? cc.falls : cc.reseats);
                    }
                    if (vrFadeNow > 0.0f) ++cc.fadeFrames;
                    if (off) ++cc.offFrames;
                    else if (cc.reseats > 0) ++cc.headFramesAfterReseat;
                }
                // the rider back on the bike after a fall, the settings' view the head, and yet not drawn from the head
                if (vr->Settings().headView && rrgame::Comfort().reseats > 0 && vrHeadCam[1].valid && !head)
                    ++rrgame::Comfort().chaseAfterReseat;
                for (int k = 0; k < 3; ++k) {
                    cam.eye[k] = drawnCam.eye[k];
                    cam.fwd[k] = drawnCam.fwd[k];
                    cam.up[k] = drawnCam.up[k];
                }
                // the head view's visual lean - the eye rides the drawn bike (vr_visual_lean.h)
                const rrgame::VisualLean lean = head ? vrBars.Lean() : handlingLean; // (chase: Modern's lean)
                // the fixed view off the bike is level in the world - no lean of the fallen bike turns it
                const rrgame::VisualLean& camLean = headView && !head && vrFall.Off() && vr->Settings().fallFixed && !rrgame::FallViewLegacy()
                                                        ? rrgame::VisualLean{} : lean;
                float seatEye[3]; // or fixed on the drawn bike ([vr] eye_on_bike)
                // [vr] seat_back_cm - the eye moved back (forward) along the drawn bike
                const float seatBack = static_cast<float>(vr->Settings().seatBackCm) / 100.0f * vr->Rig().unitsPerMetre;
                if (head && vrBars.SeatEye(headPose.eye, seatEye, seatBack)) std::memcpy(cam.eye, seatEye, sizeof(seatEye));
                else {
                    camLean.Point(cam.eye, cam.eye);
                    const float n = std::sqrt(cam.fwd[0] * cam.fwd[0] + cam.fwd[1] * cam.fwd[1] + cam.fwd[2] * cam.fwd[2]);
                    if (head && seatBack != 0.0f && n > 1e-6f)
                        for (int k = 0; k < 3; ++k) cam.eye[k] -= cam.fwd[k] / n * seatBack;
                }
                camLean.Dir(cam.fwd, cam.fwd);
                camLean.Dir(cam.up, cam.up);
                // the head view on Modern's drawn lean keeps its share of that lean against the horizon lock
                if (head) cam.rollKeep = rr::game::PlayerHandling().CameraRollKeep(); // (seated: camLean is `lean`)
                if (head) rrgame::WheelieViewComfort(camLean, cam.fwd, cam.up); // the view keeps 30 % of the pitch
                const float farZ = gfx.drawDistance >= 2 ? 60000.0f : 20000.0f;
                rrgame::VrStereo st;
                if (vr->LocateStereo(cam, head && headLookBack, farZ, st)) {
                    ++(head ? vrHeadFrames : vrChaseFrames);
                    if (head) headCam.NoteDrawn();
                    if (head && !headLookBack) rrgame::HandlingView().Note(frames, true, lean, st.anchor.ahead, st.anchor.up);
                    const uint8_t gameState = session.GameStateByte();
                    const bool paused = gameState == 3 || gameState == 4; // the pause menu: on the theatre quad
                    vrBars.UpdateHands(*vr, st.anchor, head, paused, frames, session.Log().fight.landed);
                    vrMelee.UpdateHands(*vr, st.anchor, vrBars, head, paused, frames);
                    vrHolsters.UpdateHands(*vr, st.anchor, vrMelee, vrBars, head, paused, frames);
                    const bool hudReady = drawHud && hud.Upload(session);
                    // The HUD: the original's 4:3 picture on a CURVED panel around the levelled
                    // seat - a strip of a cylinder 1.8 m from the eye, centred at eye height straight ahead of the bike's
                    // heading, 55 / 70 / 85 degrees wide (vr_settings.h hud_size). The original's HUD sits in the top
                    // corners of its picture, so the timer and the place land up and to the sides (about 15 degrees up,
                    // 20..30 degrees out), well above the handlebars and the dash, at one comfortable depth, every column
                    // at the same distance; the middle of the picture is empty and the road stays clear.
                    constexpr int kHudStrips = 8;
                    float hudStrip[kHudStrips][4][3], hudStripUv[kHudStrips][4];
                    {
                        static const float kDegrees[3] = {55.0f, 70.0f, 85.0f};
                        const float radius = 1.8f;
                        const float arc = kDegrees[std::clamp(vr->Settings().hudSize, 0, 2)] * 3.14159265f / 180.0f;
                        const float height = radius * arc * 0.75f; // 4:3 as the television showed it
                        const rr::xr::WorldAnchor& a = st.anchor;
                        const float u0 = 9.0f / 384.0f, v0 = 8.0f / 240.0f, u1 = (9.0f + 365.0f) / 384.0f,
                                    v1 = (8.0f + 224.0f) / 240.0f;
                        for (int s = 0; s < kHudStrips; ++s) {
                            const float f0 = static_cast<float>(s) / kHudStrips, f1 = static_cast<float>(s + 1) / kHudStrips;
                            for (int c = 0; c < 4; ++c) {
                                const float f = (c & 1) ? f1 : f0;
                                const float az = (f - 0.5f) * arc, y = (c & 2) ? -0.5f * height : 0.5f * height;
                                for (int k = 0; k < 3; ++k)
                                    hudStrip[s][c][k] = a.origin[k] + (a.ahead[k] * std::cos(az) * radius +
                                                                       a.right[k] * std::sin(az) * radius + a.up[k] * y) *
                                                                          a.unitsPerMetre;
                            }
                            hudStripUv[s][0] = u0 + (u1 - u0) * f0;
                            hudStripUv[s][1] = v0;
                            hudStripUv[s][2] = u0 + (u1 - u0) * f1;
                            hudStripUv[s][3] = v1;
                        }
                    }
                    // the comfort vignette: the view narrows while the seat turns (vr_settings.h comfort)
                    const auto now = std::chrono::steady_clock::now();
                    const float turn = !vrHaveAnchor ? 0.0f : rrgame::vr::AnchorTurnRate(vrLastAnchor, st.anchor,
                                                                  std::chrono::duration<double>(now - vrLastTime).count());
                    vrHaveAnchor = true;
                    vrLastAnchor = st.anchor;
                    vrLastTime = now;
                    const float vignette = vr->Settings().comfort ? std::clamp(0.15f + turn / 1.6f, 0.0f, 0.85f) : 0.0f;
                    raceRender.counters.objectsDrawn = raceRender.counters.objectsCulled = 0;
                    // One view of the race per eye (or, single-pass, one view at the mid eye whose draws reach both
                    // eyes' layers with their own view-projections: render/multiview.h).
                    const auto eyeView = [&](int e, const rr::xr::FrameTarget& t) {
                        rrgame::GameView view = base;
                        const rrgame::vr::Eye& eye = st.eyes[e];
                        for (int k = 0; k < 3; ++k) {
                            view.eye[k] = eye.world.eye[k];
                            view.target[k] = eye.world.eye[k] + eye.world.forward[k];
                            view.up[k] = eye.world.up[k];
                        }
                        view.view = LookAt(view.eye, view.target, view.up);
                        view.proj = eye.proj;
                        view.nearPlane = st.nearZ;
                        view.farPlane = farZ;
                        view.fovY = eye.fov.up - eye.fov.down;
                        view.aspect = static_cast<float>(t.width) / static_cast<float>(std::max(1, t.height));
                        view.consoleCamera = false;
                        view.shownView = false;
                        view.split = false;
                        view.framebuffer = t.framebuffer;
                        view.viewport[0] = 0;
                        view.viewport[1] = 0;
                        view.viewport[2] = t.width;
                        view.viewport[3] = t.height;
                        // the head view: not the rider's own body - only the forearms and the gloves on the grips
                        // (head_camera.h HiddenRiderPartsVr)
                        view.riderHideParts = head ? headCam.HiddenRiderPartsVr() : 0u;
                        view.riderHideParts |= vrBars.HiddenRiderParts(head); // the Handlebars mode: the player's own hands
                        view.riderHideParts |= vrMelee.HiddenRiderParts(head); // the Stick mode's tracked hands
                        view.playerFromCaptures = !head;
                        view.playerLean = lean; // the drawn bike's visual lean
                        view.haveForkOverride = head && vrBars.ForkOverride(view.playerFork); // the hands' bars
                        view.cullViewProj = st.cullViewProj; // both eyes culled with one frustum
                        view.haveCullViewProj = true;
                        return view;
                    };
                    // the player's bike exactly as RenderView drew it this frame (race_render.h
                    // DrawnMachine) - the held hands are seated on its grips
                    const auto drawnBike = [&]() -> const rr::render::Mat4* {
                        const auto& d = raceRender.drawnPlayerMachine;
                        return d.valid && d.frame == frames ? &d.model : nullptr;
                    };
                    const auto drawnBikeParts = [&]() -> const rr::PartMatrix* {
                        const auto& d = raceRender.drawnPlayerMachine;
                        return d.valid && d.frame == frames && d.posed ? d.parts : nullptr;
                    };
                    const auto clearEyes = [](const rr::xr::FrameTarget& t) {
                        glViewport(0, 0, t.width, t.height);
                        glDisable(GL_SCISSOR_TEST);
                        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
                    };
                    const auto drawHudPanel = [&](const Mat4& viewProj) {
                        if (!hudReady || paused) return;
                        for (int s = 0; s < kHudStrips; ++s) vrHudPanel.Draw(viewProj, hudStrip[s], hud.texture, hudStripUv[s]);
                    };
                    if (vr->Multiview()) {
                        const rr::xr::FrameTarget t = vr->BindEyes();
                        clearEyes(t);
                        const rrgame::GameView left = eyeView(0, t), right = eyeView(1, t);
                        const Mat4 both[2] = {Multiply(left.proj, left.view), Multiply(right.proj, right.view)};
                        rrgame::GameView mid = left; // the CPU's view: the mid eye (cells, levels of detail, billboards)
                        for (int k = 0; k < 3; ++k) {
                            mid.eye[k] = 0.5f * (left.eye[k] + right.eye[k]);
                            mid.target[k] = mid.eye[k] + (left.target[k] - left.eye[k]);
                        }
                        mid.view = LookAt(mid.eye, mid.target, mid.up);
                        const auto passStart = std::chrono::steady_clock::now();
                        rr::render::SetStereoViewProj(both);
                        raceRender.RenderView(mid, frames);
                        rr::render::BindFramebuffer(t.framebuffer);
                        glViewport(0, 0, t.width, t.height);
                        drawHudPanel(both[0]);
                        vrBars.Draw(both[0], drawnBike(), drawnBikeParts()); // the gloves (vr_handlebars.h) on the drawn bike
                        vrMelee.Draw(both[0]); // the Stick mode's hands, the colliders (vr_melee.h)
                        vrHolsters.Draw(both[0]); // the holsters' rings (vr_holsters.h)
                        vrVignette.Draw(vignette);
                        vrFade.Draw(vrFadeNow);
                        rr::render::SetStereoViewProj(nullptr);
                        vrCpu.eyeMs[0] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - passStart).count();
                        vr->FinishEyes();
                    } else {
                        for (int e = 0; e < 2; ++e) {
                            const rr::xr::FrameTarget t = vr->BindEye(e);
                            clearEyes(t);
                            const rrgame::GameView view = eyeView(e, t);
                            const auto eyeStart = std::chrono::steady_clock::now();
                            raceRender.RenderView(view, frames);
                            if (e == 0) rrgame::ZFightProbe(raceRender, view, frames); // DEVELOPMENT: RRJB_ZFIGHT
                            if (e == 0) rrgame::CarFlickProbe(raceRender, view, frames); // DEVELOPMENT: RRJB_CARFLICK
                            vrCpu.eyeMs[e] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - eyeStart).count();
                            rr::render::BindFramebuffer(t.framebuffer);
                            glViewport(0, 0, t.width, t.height);
                            drawHudPanel(Multiply(view.proj, view.view));
                            vrBars.Draw(Multiply(view.proj, view.view), drawnBike(), drawnBikeParts()); // the gloves
                            vrMelee.Draw(Multiply(view.proj, view.view)); // the Stick mode's hands, the colliders
                            vrHolsters.Draw(Multiply(view.proj, view.view)); // the holsters' rings
                            vrVignette.Draw(vignette);
                            vrFade.Draw(vrFadeNow);
                            vr->FinishEye(e);
                        }
                    }
                    { // the motion meter (vr_comfort.h) on the smooth head-view frames; the view off the bike
                        static rr::xr::WorldAnchor offPrev;
                        static bool offHave = false;
                        rrgame::ComfortCounters& cc = rrgame::Comfort();
                        if (offHave && headView && !head) { // off the bike: how far the anchor moves / turns a frame
                            float d2 = 0.0f, da = 0.0f, du = 0.0f;
                            for (int k = 0; k < 3; ++k) {
                                d2 += (st.anchor.origin[k] - offPrev.origin[k]) * (st.anchor.origin[k] - offPrev.origin[k]);
                                da += st.anchor.ahead[k] * offPrev.ahead[k];
                                du += st.anchor.up[k] * offPrev.up[k];
                            }
                            if (!vrFall.JustChanged()) { // (the cut itself is under the fade)
                                cc.maxEyeStepOff = std::max(cc.maxEyeStepOff, static_cast<double>(std::sqrt(d2)));
                                // the anchor's turn in the frame: the larger of its forward's and its up's (degrees)
                                const double turnDeg = std::max(std::acos(std::clamp(static_cast<double>(da), -1.0, 1.0)),
                                                                std::acos(std::clamp(static_cast<double>(du), -1.0, 1.0))) *
                                                       57.29577951308232;
                                cc.maxRollOff = std::max(cc.maxRollOff, turnDeg);
                            }
                        }
                        offPrev = st.anchor;
                        offHave = true;
                        if (head && gameState == 1 && !vr->MenuOpen()) {
                            rrgame::ProductMotionMeter().Frame(frames, vrStepTicks, vrAlpha, vrPeriod, st.anchor, drawnBike(),
                                                               vrCarryStep ? &vrPacing.Row() : nullptr);
                            { // vr_horizon.h: the view's pitch and height, the rival bikes as drawn in the eye
                                rrgame::ProductViewPitch().Meter(frames, st.anchor, vrPeriod, drawnBike());
                                std::vector<uint32_t> rivals;
                                for (size_t i = 1; i < session.Bikes().size(); ++i)
                                    if (session.BikeLive(i)) rivals.push_back(session.Bikes()[i].entityAddress);
                                const double shown = vrCarryStep && vrPacing.Row().displayMs > 0.0 ? vrPacing.Row().displayMs * 1e-3 : vrPeriod;
                                rrgame::ProductNearMeter().Frame(frames, session.ArenaRam(), rivals, st.anchor, shown, vrRatePeriod);
                            }
                            if (drawnBike() != nullptr) { // the own bike's vertices in the eye
                                rr::PartMatrix gameParts[5];
                                const bool haveGame = rrgame::ProductBikeShake().GameParts(vrBikeCall.bike, gameParts);
                                rrgame::ProductBikeShakeMeter().Frame(frames, st.eyes[0].world.eye, st.eyes[0].world.forward,
                                                                      st.eyes[0].world.up, st.anchor.unitsPerMetre, vrPeriod,
                                                                      *drawnBike(), drawnBikeParts(), haveGame ? gameParts : nullptr);
                            }
                        } else {
                            rrgame::ProductMotionMeter().Break();
                            rrgame::ProductBikeShakeMeter().Break();
                            rrgame::ProductViewPitch().MeterBreak();
                            rrgame::ProductNearMeter().Break();
                        }
                    }
                    if (vr->MenuOpen()) {
                        vr->DrawMenuQuad();
                    } else if (paused && hudReady) { // the pause menu (the HUD's own packets) big on the theatre quad
                        const rr::xr::FrameTarget q = vr->BindQuad();
                        glViewport(0, 0, q.width, q.height);
                        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                        glClear(GL_COLOR_BUFFER_BIT);
                        hud.DrawUploaded();
                        vr->FinishQuad();
                        ++vrPauseQuadFrames;
                    }
                }
            }
            vr->EndFrame();
        };
        while (host.Pump()) {
            if (vr != nullptr && vr->TakeTimingReset()) { // held: no time passed
                last = std::chrono::steady_clock::now();
                rrgame::ProductPacing().Reset(); // no catch-up from the display's clock either
            }
            if (vr != nullptr && vr->MenuHolds()) { // the VR settings menu holds the race (game_host_vr.h)
                vrFrame();
                last = std::chrono::steady_clock::now();
                rrgame::ProductPacing().Reset();
                continue;
            }
            // ---- input, on the original's pad bits
            rr::game::PadState pad;
            // The keyboard through the rebindable bindings (platform/input_bindings.h, controls.ini; the arrows
            // are player 2's in a two-player race).
            using rr::platform::GameAction;
            const auto bound = [&](GameAction a) {
                return rr::platform::KeyHeld(rr::platform::ActiveBindings(), a, keys, twoPlayers);
            };
            pad.throttle = hold.throttle || bound(GameAction::Throttle);
            pad.brake = hold.brake || bound(GameAction::Brake);
            pad.left = hold.left || bound(GameAction::SteerLeft);
            pad.right = hold.right || bound(GameAction::SteerRight);
            pad.lookBack = hold.lookBack || bound(GameAction::LookBack);
            // Control 0 acts on its press edge (SLUS 0x8001D720 reads the edge byte of the pad record).
            // A game controller drives the same controls (platform/gamepad_win32.h). A scripted run
            // (a frame count) does not read it, so a pad left on the desk cannot change a gate.
            rr::platform::GamepadState gp;
            if (frameLimit < 0 || (vr != nullptr && rrgame::MockPadDrivesRace())) gp = gamepad->Poll();
            if (vr == nullptr && overlayHolds(gp)) continue; // the settings overlay (pc_overlay.h) holds the race while it is open
            if (gp.connected && padSource != gp.source) {
                padSource = gp.source;
                std::printf("gamepad: %s controller in use\n", padSource);
            }
            if (vr != nullptr) vrHolsters.FilterButtons(gp, *vr, vrMelee, frames); // VR: the Handlebars mode's buttons off, the prod on B
            pad.throttle = pad.throttle || gp.throttle;
            pad.brake = pad.brake || gp.brake;
            pad.left = pad.left || gp.left;
            pad.right = pad.right || gp.right;
            pad.lookBack = pad.lookBack || gp.lookBack;
            // ---- combat (fight_session.cpp): the pad BITS R1 / L1 / R2 and the d-pad
            // Up / Down co-held; the combat input map turns them into actions 1..8.
            // --punch N presses combat action --punch-action A (default 1): the combat input map 0x800CCB78's
            // control and co-held modifier: 1 R1, 2 L1, 3 R2, 4 R2+Up, 5 R1+Up, 6 L1+Up,
            // 7 R1+Down, 8 R2+Down. The prod / stun gun swing is action 6, the spray can's action 5 (CombatDecode
            // 0x800C2348); action 1 with them is the taunt.
            const bool punchNow = punchEvery > 0 && frames >= punchFrom && frames % punchEvery < 2;
            const int pa = punchAction;
            // The controller's R1 / L1 / R2 / Up / Down bits come from its bindings (R1, L1,
            // Triangle, the d-pad or the left stick by default).
            pad.r1 = bound(GameAction::Attack1) || gp.r1 || (punchNow && (pa == 1 || pa == 5 || pa == 7));
            pad.l1 = bound(GameAction::Attack2) || gp.l1 || (punchNow && (pa == 2 || pa == 6));
            pad.r2 = bound(GameAction::Attack3) || gp.r2 || (kickEvery > 0 && frames % kickEvery < 2) ||
                     (punchNow && (pa == 3 || pa == 4 || pa == 8));
            pad.padUp = bound(GameAction::ModUp) || gp.padUp || (punchNow && (pa == 4 || pa == 5 || pa == 6));
            pad.padDown = bound(GameAction::ModDown) || gp.padDown || (punchNow && (pa == 7 || pa == 8));
            // L2, the taunt (speech_session.cpp): the rider shouts, and a player's taunt provokes the nearest AI
            pad.taunt = bound(GameAction::Taunt) || gp.taunt || (tauntEvery > 0 && frames % tauntEvery < 2);
            // Triangle (slot 6) - off the bike the original puts the bike back beside the standing rider
            pad.triangle = bound(GameAction::ToBike) || gp.toBike ||
                           (toBikeEvery > 0 && frames >= toBikeFrom && frames % toBikeEvery < 2);
            if (autoSteerOn) {                      // ai_race.h: steers the player from its road cursor
                bool thr = false, brk = false;
                const int d = autoSteer.Decide(session.ArenaRam(), session.Bikes()[0].entityAddress, thr, brk);
                pad.left = pad.left || d < 0;
                pad.right = pad.right || d > 0;
                pad.throttle = thr;                 // the script owns the throttle and the brake
                pad.brake = brk;
                bool fwd = false;                   // on foot and walking by hand: back to the bike (pad_product.h)
                int turn = 0;
                if (rr::game::WalkToBike(session.ArenaRam(), session.Bikes()[0].entityAddress, fwd, turn)) {
                    pad.throttle = fwd;
                    pad.brake = false;
                    pad.left = turn < 0;
                    pad.right = turn > 0;
                } else if (rr::game::StandingStart(session.ArenaRam(), session.Bikes()[0].entityAddress)) {
                    pad.throttle = true;
                    pad.brake = false;
                }
            }
            if (shadowBike > 0 && static_cast<size_t>(shadowBike) < session.Bikes().size()) { // cop_race.h: ride beside it
                static rr::game::ShadowSteer shadow;
                shadow.rival = session.Bikes()[static_cast<size_t>(shadowBike)].entityAddress;
                bool thr = false, brk = false;
                const int d = shadow.Decide(session.ArenaRam(), session.Bikes()[0].entityAddress, thr, brk);
                pad.left = d < 0;
                pad.right = d > 0;
                pad.throttle = thr;
                pad.brake = brk;
            }
            if (brakeFrom >= 0 && frames >= brakeFrom) { // a SCRIPT: stop and stay seated (the police, cop_race.h)
                pad.throttle = false;
                pad.brake = true;
                pad.left = pad.right = false;
            }
            if (chaseSign != 0) {                   // a SCRIPT (a test input, like --autosteer): last frame's
                const auto& ff = session.Log().fight; // fight geometry steers and paces the player onto the target
                if (ff.engage >= 0 && ff.along > -0x100000 && ff.along < 0x100000) {
                    if (ff.ownLat > 0x6000 || ff.ownLat < -0x6000) {
                        const bool l = (ff.ownLat > 0) == (chaseSign > 0);
                        pad.left = l;
                        pad.right = !l;
                    }
                    pad.throttle = ff.along < 0x2000;
                    pad.brake = ff.along > 0x10000;
                }
            }
            const bool cameraKey = bound(GameAction::Camera);
            const bool cameraPress = (cameraKey && !cameraKeyWas) || (gp.camera && !padCameraWas);
            pad.cameraNext = cameraPress || frames < cameraPresses;
            if (cameraPress && headCamLoaded && !twoPlayers) { // the head camera: chase 0 -> 1 -> 2 -> 3 -> head -> 0
                rr::game::HeadPose seatedPose;
                const bool seatedNow = headCam.Get(0, seatedPose) && seatedPose.seated;
                if (headView && !seatedNow && !rrgame::FallViewLegacy()) {
                    // the rider is off the bike - the head view stays chosen (a press here would turn off a view that
                    // is not being drawn and leave the chase camera after the re-seat)
                    pad.cameraNext = false;
                } else if (headView) { // back to the chase camera the view record already holds (mode 0, below)
                    headView = false;
                    pad.cameraNext = false;
                } else if (session.ArenaWord(0x800CD898u + 0x21Cu) == 3u && session.ArenaWord(0x800CD898u + 0x304u) == 0u) {
                    headView = true; // and the press still takes the record 3 -> 0 (CameraSetMode), the chase fallback
                }
            }
            cameraKeyWas = cameraKey;
            padCameraWas = gp.camera;
            if (vr == nullptr) { // (VR: the eye images have no console look to toggle)
                const bool f2 = keys[VK_F2] != 0, f3 = keys[VK_F3] != 0, f4 = keys[VK_F4] != 0;
                if (f2 && !f2Was) wideScreen = !wideScreen;
                if (f3 && !f3Was) ps1Dither = !ps1Dither;
                if (f4 && !f4Was) ps1Colour = !ps1Colour;
                f2Was = f2;
                f3Was = f3;
                f4Was = f4;
            }
            {   // F5: the controller's ANALOG mode (a 0x73 pad: sticks -> AxisCurve -> 0x800CE540), pad_product.h
                static bool f5Was = false, analogMode = false;
                const bool f5 = keys[VK_F5] != 0;
                if (f5 && !f5Was) {
                    analogMode = !analogMode;
                    std::printf("pad: %s mode\n", analogMode ? "ANALOGUE (0x73: left stick steers, right stick / "
                                                               "triggers throttle)" : "digital (0x41)");
                }
                f5Was = f5;
                pad.device.analog = analogMode && gp.connected;
                if (pad.device.analog) {
                    pad.device.lx = gp.lx;
                    pad.device.ly = gp.ly;
                    pad.device.ry = gp.ry;
                }
            }
            if (vr != nullptr) vrBars.ApplyToPad(pad, frames); // VR Handlebars mode: the bars / throttle as the analogue pad
            {   // game/wheelie.h: the lean-back input - the stick pulled back / F with the throttle, the bars
                // raised in VR - before the handling shapes the steering; off: nothing
                rr::game::WheelieIn win;
                win.ram = session.ArenaRam();
                win.bike = session.Bikes()[0].entityAddress;
                win.vrRun = vr != nullptr;
                win.modern = rr::game::Handling().ModeFor(vr != nullptr) == rr::game::HandlingMode::kModern;
                win.paused = session.GameStateByte() == 3 || session.GameStateByte() == 4;
                win.twoPlayers = twoPlayers;
                win.stickBack = rrgame::WheelieStickBack(gp.connected, gp.ly);
                win.vrHandlebars = vr != nullptr && rrgame::VrPrefs().bars.steering == rrgame::BarsSettings::kHandlebars;
                if (vr == nullptr || !rrgame::WheelieBarsInput(vrBars, frames, win.barsWant, win.barsKeep, &win.barsLow, &win.barsMean,
                                                                   &win.barsLegacy)) {
                    win.barsKeep = -1.0f;
                    win.barsLegacy = -1.0f;
                }
                win.frame = frames;
                rr::game::PlayerWheelie().BeforeHandling(win, pad);
            }
            {   // the handling (handling_modern.h): Modern shapes the steering into the analogue pad; Original
                // returns the pad untouched (only --steer-script, a test input, sets keys)
                rr::game::HandlingPadIn hin;
                hin.ram = session.ArenaRam();
                hin.bike = session.Bikes()[0].entityAddress;
                hin.vrRun = vr != nullptr;
                rrgame::VrHandlebars::HandView hv;
                for (int h = 0; h < 2 && vr != nullptr; ++h) hin.barsHeld = hin.barsHeld || (vrBars.Hand(h, hv) && hv.grabbed);
                hin.paused = session.GameStateByte() == 3 || session.GameStateByte() == 4;
                hin.gamepad = gp.connected;
                hin.stickX = gp.lx;
                hin.frame = frames;
                if (!twoPlayers) rr::game::PlayerHandling().ApplyToPad(hin, pad);
            }
            rr::game::PlayerWheelie().AfterHandling(pad); // the front wheel up steers half as strong
            if (vr != nullptr) vrMelee.ApplyToPad(pad); // VR physical combat: a contact for the fight code
            if (vr != nullptr) vrHolsters.ApplyToPad(pad, session, frames); // VR: the weapon drawn / holstered, the prod's discharge
            {   // pause_product.h: Start in the race, the menu pad while paused
                rr::game::PauseKeyInput pk;
                const auto k = [keys](int vk) { return keys[vk] != 0; };
                const auto sc = [&](const char* key) { return pauseScript.Has(frames, key); };
                pk.enter = k(VK_RETURN);
                pk.escape = k(VK_ESCAPE);
                pk.back = k(VK_BACK) || sc("t");
                pk.space = k(VK_SPACE) || sc("x");
                pk.p = bound(GameAction::Pause) || sc("start");
                pk.up = (!twoPlayers && k(VK_UP)) || k('W') || sc("up");
                pk.down = (!twoPlayers && k(VK_DOWN)) || k('S') || sc("down");
                pk.left = (!twoPlayers && k(VK_LEFT)) || k('A') || sc("left");
                pk.right = (!twoPlayers && k(VK_RIGHT)) || k('D') || sc("right");
                pk.padStart = gp.start;
                pk.padUp = gp.up;
                pk.padDown = gp.down;
                pk.padLeft = gp.left;
                pk.padRight = gp.right;
                pk.padCross = gp.cross;
                pk.padTriangle = gp.triangle;
                const uint8_t st = session.GameStateByte();
                pauseKeys.Apply(pad, pk, st == 3 || st == 4);
            }

            // ---- the time step. The original's unit is 1/300 s and `RaceStep`
            // clamps a delta of 31 or more to 30, so an interactive run hands it the real elapsed
            // time in those units and lets the ported clamp do its own job.
            //
            // A SCRIPTED run does not: it uses a fixed step, so that two runs of the same command
            // produce the same numbers. Reading the wall clock makes a run reproducible only by
            // luck - on an idle machine every frame rounds to the same tick count and eight runs
            // agree, while on a loaded one they do not. A gate
            // compares two scripted runs against each other, so "reproducible when the machine
            // happens to be quiet" is not good enough.
            const auto simStart = std::chrono::steady_clock::now(); // VR's CPU split (the 5 s "vr cpu" line)
            int32_t ticks = fixedTicks;
            // VR (a headset; the scripted mock with --vr-mock-hz / --vr-mock-timing): the display's rate
            // (72 / 80 / 90 / 120 Hz) is not a multiple of the 1/300 s unit - the fraction carries to the next frame.
            // vr_pacing.h: the race is stepped to the next frame's predicted display time (xrWaitFrame's
            // predictedDisplayTime + period), not by the loop's wall clock (RRJB_VR_PACING=wallclock: the control)
            const bool vrMockClock = vr != nullptr && vr->Config().mock && rrgame::MockDisplayHz() > 0.0;
            vrCarryStep = vr != nullptr && (fixedTicks <= 0 || vrMockClock);
            if (vrCarryStep) {
                const auto now = std::chrono::steady_clock::now();
                last = now;
                rrgame::DisplayPacing& pacing = rrgame::ProductPacing();
                // GT2's order (xr_field_pacing.h): wait for the frame, then step the race to its display time
                if (pacing.Mode() == rrgame::PacingMode::kDisplay) {
                    vrOpen = vr->BeginFrame() ? 1 : 0;
                    if (vrOpen == 1) pacing.Frame(vr->FrameClock());
                }
                ticks = pacing.Step(vrMockClock ? pacing.FrameLoop() : rrgame::SteadyNowNs(), 1, 30,
                                    vrMockClock ? 1.0 / rrgame::MockDisplayHz() : vr->DisplayPeriod(), vrOpen == 1);
            } else if (ticks <= 0) {
                const auto now = std::chrono::steady_clock::now();
                const double elapsed = std::chrono::duration<double>(now - last).count();
                last = now;
                ticks = static_cast<int32_t>(std::lround(elapsed * 300.0));
            }
            ticks = std::clamp(ticks, 1, 30);
            vrStepTicks = ticks;
            deskStepTicks = ticks;
            rrgame::CheatScriptFrame(frames);       // --cheat-weapon-now (cheat_menu.h)
            rr::game::CheatBeforeFrame(session);    // the cheats (cheats.h)
            if (session.Players() == 2) { // player 2: its keyboard layout and the second controller (mp_input.h)
                rr::platform::GamepadState gp2;
                if (frameLimit < 0) gp2 = gamepad2->Poll(1);
                rr::game::PadState pad2 = input2.Read(keys, gp2, hold2);
                pad2.taunt = pad2.taunt || (taunt2Every > 0 && frames % taunt2Every < 2);
                if (autoSteer2On && session.Bikes().size() > 1) { // the same test script as --autosteer, player 2
                    bool thr = false, brk = false;
                    const uint32_t b2 = session.Bikes()[1].entityAddress;
                    const int d = autoSteer2.Decide(session.ArenaRam(), b2, thr, brk);
                    pad2.left = pad2.left || d < 0;
                    pad2.right = pad2.right || d > 0;
                    pad2.throttle = thr;
                    pad2.brake = brk;
                    bool fwd = false;
                    int turn = 0;
                    if (rr::game::WalkToBike(session.ArenaRam(), b2, fwd, turn)) {
                        pad2.throttle = fwd;
                        pad2.brake = false;
                        pad2.left = turn < 0;
                        pad2.right = turn > 0;
                    } else if (rr::game::StandingStart(session.ArenaRam(), b2)) {
                        pad2.throttle = true;
                        pad2.brake = false;
                    }
                }
                {   // player 2's Start (mp_input.h): pad record 1's control 1, which the pause test reads for every
                    // pad - H, the second controller's Start or the script's p2start; paused,
                    // player 2's pad is the menu pad the pause menu reads when player 2 paused (gp+260 = 1):
                    // the arrows / d-pad, M or Cross = Cross, N or Triangle = Triangle, H or Start = Start.
                    rr::game::PauseKeyInput pk2;
                    const auto k = [keys](int vk) { return keys[vk] != 0; };
                    const auto sc = [&](const char* key) { return pauseScript.Has(frames, key); };
                    pk2.p = k('H') || sc("p2start");
                    pk2.space = k('M') || sc("p2x");
                    pk2.back = k('N') || sc("p2t");
                    pk2.up = k(VK_UP) || sc("p2up");
                    pk2.down = k(VK_DOWN) || sc("p2down");
                    pk2.left = k(VK_LEFT) || sc("p2left");
                    pk2.right = k(VK_RIGHT) || sc("p2right");
                    pk2.padStart = gp2.start;
                    pk2.padUp = gp2.up;
                    pk2.padDown = gp2.down;
                    pk2.padLeft = gp2.left;
                    pk2.padRight = gp2.right;
                    pk2.padCross = gp2.cross;
                    pk2.padTriangle = gp2.triangle;
                    const uint8_t st = session.GameStateByte();
                    if (!pad2Start) pk2 = rr::game::PauseKeyInput{}; // RRJB_P2_START=off: the negative control
                    pauseKeys2.Apply(pad2, pk2, st == 3 || st == 4);
                    if (pad2.start) ++p2StartFrames;
                }
                const uint8_t stateBefore = session.GameStateByte();
                session.Frame(pad, ticks, &pad2);
                if (stateBefore != 3 && session.GameStateByte() == 3) { // who paused: gp+260
                    uint32_t who = 0;
                    std::memcpy(&who, session.ArenaRam() + (0x8005AD90u & 0x1FFFFFu), 4);
                    ++pausesBy[who < 4u ? who : 3u];
                }
            } else
                session.Frame(pad, ticks);
            rr::game::CheatAfterFrame(session);
            if (vr != nullptr) vr->SetHapticsRacing(session.GameStateByte() == 1); // the pause holds the rumble
            if (!twoPlayers) // the handling's drawn lean and metrics
                rr::game::PlayerHandling().AfterFrame(session.MutableArenaRam(), session.Bikes()[0].entityAddress, session.Log().dt,
                                                      session.RouteDistance(0), frames);
            if (!twoPlayers) // the wheelie's state, the loop-over, the flight over a car
                rr::game::PlayerWheelie().AfterFrame(session.MutableArenaRam(), session.Bikes()[0].entityAddress, session.Log().dt,
                                                     session.RouteDistance(0), frames);
            // rumble_product.h: the DualShock motors the frame left, on the real controllers. A
            // scripted run (a frame count) never polls a controller, so it never drives one; nor does any run
            // HideIfScriptedRun hid (RRJB_WINDOW=hidden: a script's options, or a hidden run by request).
            static const bool hiddenRun = [] {
                const char* w = std::getenv("RRJB_WINDOW");
                return w != nullptr && std::strcmp(w, "hidden") == 0;
            }();
            // The desktop VR mock has no actuator: a scripted mock run drives it too, so the haptics it would have sent
            // are counted (its line in the "vr:" report)
            const bool mockHaptics = vr != nullptr && vr->Config().mock && frameLimit >= 0;
            if (mockHaptics) {
                if (rr::game::RumbleCounters().hitRumbles != mockHitRumbles) { // the blows' motors, for the report
                    mockHitRumbles = rr::game::RumbleCounters().hitRumbles;
                    if (mockHitFrames.size() < 400) mockHitFrames += " f" + std::to_string(frames);
                }
                gamepad->Vibrate(rr::game::RumbleOutput(0).small, rr::game::RumbleOutput(0).large);
            }
            if (frameLimit < 0 && !hiddenRun) {
                const rr::game::MotorState m1 = rr::game::RumbleOutput(0);
                if (gamepad->Vibrate(m1.small, m1.large)) rr::game::NoteDeviceWrite();
                if (twoPlayers) {
                    const rr::game::MotorState m2 = rr::game::RumbleOutput(1);
                    if (gamepad2->Vibrate(m2.small, m2.large)) rr::game::NoteDeviceWrite();
                }
                // a DualSense's adaptive triggers: only while racing (game_state 1), not paused
                const bool racingNow = session.GameStateByte() == 1;
                gamepad->Triggers(racingNow);
                if (twoPlayers) gamepad2->Triggers(racingNow);
            }
            if (!parityRam.empty() && frames == 0) {
                std::printf("%s\n", session.AdoptCapture(parityRam).c_str());
                std::printf("%s\n", rr::game::SkyParityRebuild(session.ArenaRam()).c_str()); // sky_product.h
                if (rr::game::CellSortOn()) // the ported sort against the capture's own
                    std::printf("%s\n", rr::game::CellSortCheckCapture(session.ArenaRam(), session.Players()).c_str());
                // the effects' OT cut from the capture's own heap (fx_runtime.h RecarveOt)
                if (!fx.RecarveOt(session.MutableArenaRam())) std::printf("parity: the effects' OT was NOT re-cut\n");
                if (rr::game::GteObjectsOn()) { // the render's draw loop 0x8008D56C (PORTED pools
                    // 2..5: part 0 = the rows) before the model draw, as the original frame runs it - the capture was taken
                    // before it, so its pedestrians' +0x68 still held the animation's matrix
                    rr::sim::GuestRam pg(session.MutableArenaRam(), 0x8005AC8Cu);
                    rr::game::DrawLoopPools(pg, session.Players());
                    std::printf("parity: the draw loop's pools 2..5 (0x8008D56C) run on the capture\n");
                }
                // The capture's frame already aged its effect records - draw them, do
                // not age them again (RRJB_PARITY_FX_AGE=again: the control, one more step)
                const char* fxAge = std::getenv("RRJB_PARITY_FX_AGE");
                const bool fxAgain = fxAge != nullptr && std::strcmp(fxAge, "again") == 0;
                if (!fxAgain) fx.KeepCaptureAgesOnce();
                std::printf("parity: the capture's effect records %s\n",
                            !fxAgain ? "drawn as its own draw cycle left them (+9 bit 3 kept)"
                                     : "aged once more (RRJB_PARITY_FX_AGE=again, the control)");
                // The PORTED HudFrame on the adopted capture (hud_view.h RunHudFrameOnCapture)
                std::printf("parity: HudFrame on the capture %s\n",
                            rr::game::RunHudFrameOnCapture(session.MutableArenaRam()) ? "ran" : "did NOT run");
                if (std::getenv("RRJB_PARITY_HUD_ORIGIN") == nullptr ||
                    std::strcmp(std::getenv("RRJB_PARITY_HUD_ORIGIN"), "off") != 0) {
                    hud.parityOt = session.ArenaWord(rr::sim::kHudOt);
                    hud.originX = static_cast<int16_t>(session.ArenaWord(rr::sim::kGpuDrawEnv + 8u) & 0xFFFFu);
                    hud.originY = static_cast<int16_t>(session.ArenaWord(rr::sim::kGpuDrawEnv + 8u) >> 16);
                    std::printf("parity: the HUD walked from the capture's slot 0x%08X, drawing offset (%d, %d)\n",
                                hud.parityOt, hud.originX, hud.originY);
                }
            }
            {   // the PORTED effect pass on every live bike (and, through +0x38 / +0x40, its riders)
                std::vector<uint32_t> fxEntities;
                for (size_t b = 0; b < session.Bikes().size(); ++b)
                    if (session.BikeLive(b)) fxEntities.push_back(session.Bikes()[b].entityAddress);
                std::vector<uint32_t> fxList = rr::game::FxEntities(session.ArenaRam(), fxEntities);
                rr::game::AppendHazardRecords(session.ArenaRam(), fxList); // HazardDraw's ModelVisible (hazard_product.h)
                if (rr::game::GteObjectsOn()) { // the props of pools 4 / 5 and the pedestrians
                    // through the PORTED ModelVisible / ModelDraw as the original's per-cell draw list takes every filed
                    // object - their SXY / MAC3 are what the renderer places them at
                    if (!propsFromRecords)
                        for (const rr::game::LiveProp& lp : rr::game::LiveProps(session.ArenaRam())) fxList.push_back(lp.entity);
                    if (rr::game::PedsEnabled())
                        for (const rr::game::LivePed& lp : rr::game::LivePeds(session.ArenaRam())) fxList.push_back(lp.entity);
                }
                if (!fx.Frame(session.MutableArenaRam(), fxList, session.Players()) &&
                    fx.Ready() && !fx.Error().empty())
                    session.NoteSeam(fx.Error());
                { // sky_product.h: the PORTED sky draw RASHCDG 0x80064B9C, its packets to the renderer
                    const bool skyDrew = rr::game::SkyFrameDraw(session.MutableArenaRam(), session.Players(),
                                                                static_cast<uint32_t>(frames));
                    rr::render::SkyGpuSubmit(skyDrew ? &rr::game::SkyFramePackets() : nullptr, &rr::game::SkyHostVram());
                }
                if (std::getenv("RRJB_SEAT_CHECK")) { // DEVELOPMENT CHECK: the renderer's seat per LOD (race_scene.h
                    // RiderAttach(lod)) against the PORTED SeatVertex 0x80066A84 / ChildPlace 0x80066B98 fallback on
                    // the bikes the model draw just placed (their +8 = the LOD LodSelect gave them)
                    rr::sim::GuestRam sg(session.MutableArenaRam(), 0x8005AC8Cu);
                    for (uint32_t e : fxEntities) {
                        if (sg.U32(e + 0x60u) == 0 || sg.U32(e) == 0) continue;
                        const int lod = sg.S8(e + 8u);
                        uint32_t a = rr::sim::model::SeatVertex(sg, e), exp = 0;
                        if (a == 0) {
                            const uint32_t dod0 = sg.U32(sg.U32(sg.U32(e + 96u) + 8u));
                            exp = sg.U16(dod0 + 14u) >> 12;
                            const uint32_t a0 = sg.U32(dod0 + 36u) + 4u + sg.U32(sg.U32(sg.U32(e + 4u)) + 16u) * 8u;
                            a = sg.U16(dod0 + 24u) < 6u ? a0 + 24u : a0 + 32u;
                        } else {
                            exp = sg.U16(sg.U32(e) + 14u) >> 12;
                        }
                        const float f = static_cast<float>(1 << (exp < 4 ? 4 - exp : 0));
                        const float* ours = scene.RiderAttach(lod);
                        bool same = true;
                        for (uint32_t k = 0; k < 3; ++k) same = same && static_cast<float>(sg.S16(a + 2u * k)) * f == ours[k];
                        static size_t seatSame[4] = {}, seatDiffer[4] = {};
                        ++(same ? seatSame : seatDiffer)[lod & 3];
                        if (frames == frameLimit - 1 || !same)
                            std::printf("seatcheck: frame %ld bike %08X LOD %d %s (equal by LOD 0..3: %zu %zu %zu %zu, "
                                        "differ %zu %zu %zu %zu)\n", frames, e, lod, same ? "equal" : "DIFFERS",
                                        seatSame[0], seatSame[1], seatSame[2], seatSame[3], seatDiffer[0],
                                        seatDiffer[1], seatDiffer[2], seatDiffer[3]);
                    }
                }
            }
            // The vertical blanks: the PORTED AudioVSyncTick and SoundService. On the console
            // they are the VSync callback's (SLUS 0x8001B700: `game_state+0x0C += 5`, then `jal 0x80019990`,
            // unconditionally, every vblank), so they run at 60 Hz whatever the game's own frame rate - and one of
            // them is exactly 5 of the race clock's 1/300 s ticks. Here: one per 5 ticks of game time, the
            // remainder carried to the next frame - one per frame at the scripted 5-tick step, 72 in 60 at 72 Hz,
            // the same 60 a simulated second at any display rate. They run whether or not a device is open - they are the
            // game's own sound state, and the device only plays what they leave in the SPU model.
            // RRJB_SOUND_VSYNC=frame: two a frame, 120 a second on a 60 Hz display (the negative control of the gate).
            soundTicks += ticks;
            if (vsyncTwoPerFrame) {
                session.Sounds().VSync();
                session.Sounds().VSync();
                hostVsyncs += 2;
            } else {
                vsyncTickCarry += ticks;
                for (; vsyncTickCarry >= kTicksPerVblank; vsyncTickCarry -= kTicksPerVblank) {
                    session.Sounds().VSync();
                    ++hostVsyncs;
                }
            }
            if (engineTrace != nullptr && session.Sounds().EngineMode()) { // RRJB_ENGINE_TRACE
                const auto& snd = session.Sounds();
                const uint32_t E = snd.ArenaWord(0x8005AC8C + 1952);
                std::fprintf(engineTrace, "%ld,%lld,%zu,%d,%d,%d,%d\n", frames, soundTicks, snd.VSyncs(),
                             E ? static_cast<int32_t>(snd.ArenaWord(E + 0x30)) : 0,
                             E ? static_cast<int32_t>(snd.ArenaWord(E + 0x34)) : 0,
                             E ? static_cast<int32_t>(snd.ArenaWord(E + 0x60)) : 0,
                             E ? static_cast<int32_t>(snd.ArenaWord(E + 0x64)) : 0);
            }
            if (!soundWav.empty() && session.Sounds().EngineMode()) { // the frame's game time of the SPU model, in step:
                // 44100 / 300 = 147 samples a tick (the console's 60 Hz sound service: 1/300 s a tick)
                const size_t n = static_cast<size_t>(ticks) * 147u;
                const size_t at = soundRecord.size();
                soundRecord.resize(at + 2u * n);
                session.Sounds().Spu()->Render(soundRecord.data() + at, n);
            }
            {
                const rr::game::FrameLog& f = session.Log();
                if (f.offRoad) ++totals.offRoadFrames;
                totals.maxOffRoad = std::max(totals.maxOffRoad, f.offRoad);
                if (f.playerFlags184 & 1u) ++totals.playerOffRoadFrames;
                totals.queries += f.groundQueries;
                totals.hits += f.groundHits;
                totals.fineHits += f.groundFineHits;
                totals.playerHits += f.playerGroundHits;
                totals.playerFineHits += f.playerGroundFineHits;
                totals.exhausted += f.groundExhausted;
                totals.misses += f.groundMisses;
                totals.qDeclined += f.groundDeclined;
                totals.loads += f.cellLoads;
                totals.unloads += f.cellUnloads;
                totals.maxResident = std::max(totals.maxResident, f.cellsResident);
                if ((f.playerFlags184 & 1u) && static_cast<int32_t>(f.playerGroundAnswer) >= 0 &&
                    (f.playerGroundAnswer >> 24) != 0x7Fu) {
                    for (int k = 0; k < 3; ++k) totals.lastHitNormal[k] = f.playerGroundNormal[k];
                    totals.lastHitAnswer = f.playerGroundAnswer;
                }
                if (session.Sounds().EngineMode() && session.Log().countdownRunning == false) {
                    const auto& snd = session.Sounds();
                    const uint32_t E = snd.ArenaWord(0x8005AC8C + 1952);
                    if (E != 0) {
                        const uint32_t h0 = snd.ArenaWord(E + 0x10);
                        const uint16_t pitch = snd.Spu()->Read(16u * (h0 >> 27) + 4u);
                        totals.minPitch = std::min(totals.minPitch, pitch);
                        totals.maxPitch = std::max(totals.maxPitch, pitch);
                    }
                }
            }

            // ---- the sound the frame asked for. `SoundRuntime::Service` stands in for
            // `SoundService SLUS 0x8001EE94` and hands over the six values a
            // port should stop at; everything above it - which sound, on which voice, at what
            // volume and pan, and which voice gets stolen - came out of the PORTED emitter.
            if (sfx) {
                // The development probe. It goes through the PORTED emitter - the same
                // `rr::sim::PlaySound3D` the bench proves - at the listener's own position, so
                // what it exercises is the whole chain and not a shortcut past it.
                if (sfxProbe >= 0 && frames % 60 == 0) {
                    float pos[3], tan[3], lat[3], nrm[3];
                    session.BikePlacement(0, pos, tan, lat, nrm);
                    session.Sounds().PlaySound3D(static_cast<int32_t>(pos[0] * 65536.0f),
                                                 static_cast<int32_t>(pos[2] * 65536.0f), sfxProbe,
                                                 0);
                }
                session.Sounds().Service(startedVoices, stoppedVoices);
                for (int32_t ch : stoppedVoices) {
                    const uint32_t slot = static_cast<uint32_t>(ch) & 31u;
                    if (channelVoice[slot] != 0) {
                        mixer.Stop(channelVoice[slot]);
                        channelVoice[slot] = 0;
                    }
                }
                for (const rr::game::SoundVoiceStart& s : startedVoices) {
                    if (s.pcm == nullptr || s.pcm->empty()) continue;
                    const uint32_t slot = static_cast<uint32_t>(s.channel) & 31u;
                    if (channelVoice[slot] != 0) mixer.Stop(channelVoice[slot]);
                    int32_t volume = 0, pan = 0;
                    SpuVolumeToMixer(s.volumeLeft, s.volumeRight, volume, pan);
                    rr::audio::VoiceDesc voice;
                    voice.source = std::make_shared<rr::audio::MemorySource>(*s.pcm, 1,
                                                                            rr::kSpuBaseRate);
                    voice.pitch = s.pitch; // the SPU's own Q12 register, 0x1000 = 44100 Hz
                    voice.volume = volume;
                    voice.pan = pan;
                    channelVoice[slot] = mixer.Play(voice);
                    if (channelVoice[slot] != 0) ++sfxPlayed;
                }
            }

            // ---- the camera: the PORTED ViewUpdate RASHCDG 0x800881B4 filled view record 0 inside
            // the frame (RaceStep's camera slot); the views are drawn from the render camera record the original
            // draws with (race_render.cpp GameCamera). The head camera (head_camera.h): the player
            // rider's posed head, every frame (its smoothing is warm when the view is switched to); drawn from while
            // the view is on, one player, and the rider seated - off the bike the view record's chase camera is drawn.
            headNow = false;
            headLookBack = pad.lookBack;
            if (headCamLoaded) {
                const uint32_t hb = session.Bikes()[0].entityAddress;
                const float* headSeat = scene.HasSidecarFor(0) && 100u + session.ArenaWord(hb + 0xB4u) == scene.SidecarModelIdFor(0)
                                            ? scene.SidecarAttachFor(0, 0) : nullptr;
                headCam.Update(session.ArenaRam(), hb, session.Bikes()[0].ownerAddress, 0, headSeat);
                if (vr != nullptr) vrMelee.UpdateTargets(session); // the riders' bodies as drawn
                if (vr != nullptr) vrHolsters.UpdateGame(session); // the inventory: a steal, an arrival
                headNow = headView && session.Players() == 1 && headCam.Get(0, headPose) && headPose.seated;
                { // the head view's falls and re-seats in the log (the desktop head camera and VR alike)
                    static int headWas = -1;
                    if (headView && session.Players() == 1 && headWas >= 0 && headWas != static_cast<int>(headNow))
                        std::printf("head camera: frame %ld - %s\n", frames,
                                    headNow ? "the rider is back on the bike, the head view drawn again"
                                            : "the rider is off the bike, the view off the bike drawn");
                    headWas = headView && session.Players() == 1 ? static_cast<int>(headNow) : -1;
                }
                // the visual lean only while the head view is drawn - the renderer's player bike likewise
                if (vr != nullptr) vrBars.UpdateBike(session.ArenaRam(), hb, session.Bikes()[0].ownerAddress, headSeat, headNow);
                if (vr != nullptr || headView) rrgame::ProductBikeShake().Capture(session.ArenaRam(), hb);
            }
            handlingLean = session.Players() == 1 // Modern's lean for the drawn bike (and the head camera)
                               ? rrgame::HandlingLean(session.ArenaRam(), session.Bikes()[0].entityAddress, 1.0f,
                                                      rrgame::HandlingContactUp(session.ArenaRam(), session.Bikes()[0].entityAddress))
                               : rrgame::VisualLean{};
            if (session.Players() == 1) // the wheelie's drawn pitch after the lean (off: nothing)
                rrgame::AddWheeliePitch(handlingLean, session.ArenaRam(), session.Bikes()[0].entityAddress,
                                        rrgame::HandlingContactUp(session.ArenaRam(), session.Bikes()[0].entityAddress));
            if (vr != nullptr) { // vr_comfort.h: what the frames until the next step are drawn from
                rrgame::ProductFrameInterp().Capture(session.ArenaRam());
                rrgame::ProductViewPitch().Capture(session.ArenaRam(), session.Bikes()[0].entityAddress);
                vrHeadCam[0] = vrHeadCam[1];
                vrChaseCam[0] = vrChaseCam[1];
                vrHeadCam[1] = rrgame::CamPose{};
                vrBikeCall.valid = false;
                rr::game::HeadPose hp;
                if (headCamLoaded && headCam.Get(0, hp) && hp.seated) {
                    vrHeadCam[1].valid = true;
                    std::memcpy(vrHeadCam[1].eye, hp.eye, sizeof(hp.eye));
                    std::memcpy(vrHeadCam[1].fwd, hp.fwd, sizeof(hp.fwd));
                    std::memcpy(vrHeadCam[1].up, hp.up, sizeof(hp.up));
                }
                const rrgame::GameView cb = raceRender.GameCamera(0, 0, 0, 16, 16, 0);
                vrChaseCam[1].valid = true;
                for (int k = 0; k < 3; ++k) {
                    vrChaseCam[1].eye[k] = cb.eye[k];
                    vrChaseCam[1].fwd[k] = cb.target[k] - cb.eye[k];
                    vrChaseCam[1].up[k] = cb.up[k];
                }
                if (headCamLoaded) {
                    vrBikeCall.valid = true;
                    vrBikeCall.bike = session.Bikes()[0].entityAddress;
                    vrBikeCall.rider = session.Bikes()[0].ownerAddress;
                    vrBikeCall.seat = scene.HasSidecarFor(0) && 100u + session.ArenaWord(vrBikeCall.bike + 0xB4u) ==
                                                                    scene.SidecarModelIdFor(0)
                                          ? scene.SidecarAttachFor(0, 0) : nullptr;
                }
                if (vr->Settings().headView != vrViewSetting) { // the VR menu's View row, changed mid-race
                    vrViewSetting = vr->Settings().headView;
                    headView = vrViewSetting && headCamLoaded;
                }
            }
            // ---- the picture: every view (race_render.h), the PS1 look, the HUD, the overlay / profiler
            if (vr != nullptr) {
                // the scripted VR run's shot: the last frame's eyes (and the theatre quad when drawn), as one PNG
                if (frameLimit >= 0 && frames + 1 >= frameLimit && !shotPath.empty()) vr->RequestShot(shotPath);
                vrCpu.simMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - simStart).count();
                vrFrame();
                ++vrCpu.frames;
                const auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration<double>(now - vrCpu.start).count() >= 5.0 && vrCpu.frames > 0) {
                    const double n = static_cast<double>(vrCpu.frames);
                    if (vr->Multiview())
                        std::printf("vr cpu: %ld frames - the frame's game logic (simulation, effect pass, sound) %.2f ms, "
                                    "the single stereo pass's draw calls %.2f ms (the renderer's CPU, before the GPU)\n",
                                    vrCpu.frames, vrCpu.simMs / n, vrCpu.eyeMs[0] / n);
                    else
                        std::printf("vr cpu: %ld frames - the frame's game logic (simulation, effect pass, sound) %.2f ms, "
                                    "the two eyes' draw calls %.2f + %.2f ms (the renderer's CPU, before the GPU)\n",
                                    vrCpu.frames, vrCpu.simMs / n, vrCpu.eyeMs[0] / n, vrCpu.eyeMs[1] / n);
                    vrCpu = VrCpu{};
                    vrCpu.start = now;
                }
            } else if (!drawPicture()) {
                continue;
            }
            {   // once a frame, not once a view: the traffic's run totals
                const rr::game::FrameLog& tl = session.Log();
                trafficMaxLive = std::max<size_t>(trafficMaxLive, static_cast<size_t>(std::max(0, tl.carsLive)));
                trafficMaxCops = std::max<size_t>(trafficMaxCops, static_cast<size_t>(std::max(0, tl.copsOut)));
                if (tl.spawnerRan) ++spawnerFrames;
                if (tl.spawnerDeclined) ++spawnerDeclined;
                if (tl.trafficRan) ++trafficFrames;
                if (tl.trafficDeclined) ++trafficDeclined;
            }

            const rr::game::FrameLog& fl = session.Log();
            {
                ++cam.frames;
                if (fl.cameraDeclined) ++cam.declined;
                if (!fl.countdownRunning) {
                    ++cam.racing;
                    cam.minDist = std::min(cam.minDist, fl.eyeToBike);
                    cam.maxDist = std::max(cam.maxDist, fl.eyeToBike);
                    cam.sumDist += fl.eyeToBike;
                    if (fl.playerViewDistance < 1280) ++cam.fine;
                    double subj = fl.eyeToBike;
                    if (fl.playerMount >= 2) { // the rider thrown or walking: ViewUpdate follows him, not the bike
                        const uint8_t* ram = session.ArenaRam();
                        const uint32_t ro = session.Player().ownerAddress & 0x1FFFFFu;
                        double d2 = 0.0;
                        for (uint32_t k = 0; k < 3; ++k) {
                            int32_t v;
                            std::memcpy(&v, ram + ro + 0xB8u + 4u * k, 4);
                            const double d = (static_cast<double>(fl.viewEye[k]) - v) / 65536.0;
                            d2 += d * d;
                        }
                        subj = std::sqrt(d2);
                        ++cam.offFrames;
                    }
                    cam.subjSum += subj;
                    if (subj > cam.subjMax) {
                        cam.subjMax = subj;
                        cam.subjMaxFrame = frames;
                    }
                }
                cam.minLive = std::min(cam.minLive, fl.liveBikes);
                cam.maxLive = std::max(cam.maxLive, fl.liveBikes);
                cam.maxDormant = std::max(cam.maxDormant, fl.dormantBikes);
                cam.transitions += fl.transitions;
                cam.refused += fl.transitionsRefused;
                cam.downed += fl.downedCalls;
                cam.downedRefused += fl.downedRefused;
                cam.dormantDrives += fl.dormantDrives;
                cam.bursts += fl.bursts;
                cam.sprays += fl.sprays;
                cam.maxBusy = std::max(cam.maxBusy, fl.effectRecordsBusy);
                cam.crashEmits += fl.crashEmits;
                cam.crashDeclined += fl.crashEmitsDeclined;
                cam.stamps += fl.stampResults;
                cam.resets += fl.resets;
                cam.viewEvents += fl.viewEvents;
                cam.raceGo += fl.raceGo;
                if (fl.lastRefusal != 0) cam.lastRefusal = fl.lastRefusal;
            }
            // The hit sounds: this frame's key-ons (the SPU model's trace, after both vertical blanks)
            // and the emitter calls, for the hit tally and the frame log.
            std::vector<rr::audio::SpuVoices::KeyOnEvent> frameKeyOns;
            if (session.Sounds().EngineMode()) {
                session.Sounds().Spu()->TraceKeyOns(true);
                frameKeyOns = session.Sounds().Spu()->TakeKeyOns();
            }
            const std::vector<rr::game::SoundRuntime::Played> framePlayed = session.Sounds().TakePlayed();
            // A key-on happens in a vertical blank (SoundService): a blow in a frame that ended no vblank (with a step
            // under 5 ticks, e.g. one frame in five at 72 Hz) keys its voice in the next frame that does,
            // as the console keys it at the next vblank after the game frame. At the scripted 5-tick step every frame
            // ends one, so this is the frame itself.
            if (fl.fight.landed != 0 || fl.fight.hits != 0) {
                if (!heldBlow.active) heldBlow.frame = fl.frame;
                heldBlow.active = true;
                heldBlow.landed += fl.fight.landed;
                heldBlow.hurt += fl.fight.hits;
            }
            if (hostVsyncs != tallyVsyncs && heldBlow.active) {
                g_hitSounds.Frame(heldBlow.frame, heldBlow.landed, heldBlow.hurt, frameKeyOns);
                heldBlow = {};
            }
            tallyVsyncs = hostVsyncs;
            if (!logPath.empty() && (frames < 2400 || frames >= logFrom || frames % 300 == 0)) {
                char line[640];
                std::snprintf(line, sizeof(line),
                              "f%-5u t=%-2d dt=%-6d clock=%-6d cd=%-8d %s | director=%d progress=%d "
                              "drive=%d cmds=%d engine=%zu steer=%zu finishTests=%zu | spd=%-8d "
                              "v240=%-8d drive=%-8d revs=%-8d gear=%d steer=%-7d lean=%-6d | pos=%.1f "
                              "place=%d | pad %c%c%c%c\n",
                              fl.frame, fl.ticks, fl.dt, fl.raceClock, fl.countdown,
                              fl.countdownRunning ? "COUNTDOWN" : "racing   ", fl.raceDirectorRan ? 1 : 0,
                              fl.progressPassComplete ? 1 : 0, fl.aiDrivePassComplete ? 1 : 0,
                              fl.aiRunCommandsComplete ? 1 : 0, fl.engineBikes, fl.steerBikes,
                              fl.finishTestsRequested, fl.playerSpeed, fl.playerSpeed240, fl.playerDrive,
                              fl.playerRevs, fl.playerGear, fl.playerSteer, fl.playerLean, session.RouteDistance(0),
                              session.Player().place, fl.pad.throttle ? 'T' : '.', fl.pad.brake ? 'B' : '.',
                              fl.pad.left ? 'L' : '.', fl.pad.right ? 'R' : '.');
                log += line;
                if (fl.fight.decodes || fl.fight.updates || fl.fight.combos || fl.fight.hits || fl.fight.strikes) {
                    const auto& pb = session.Bikes()[0];
                    const uint32_t R = session.ArenaWord(pb.entityAddress + 0x354u);
                    const uint32_t top = pb.entityAddress + 0x3B4u +
                                         8u * static_cast<uint32_t>(static_cast<int8_t>(pb.entity[0x3B2]));
                    std::snprintf(line, sizeof(line),
                                  "      fight: decode=%zu combo=%zu update=%zu refused=%zu leave=%zu enter=%zu "
                                  "sf=%zu strikes=%zu landed=%zu hits=%zu ko=%zu | player cmd=%u op=%u tgt=%u stance=%u rec=%u qi=%u hp=%u along=%.2f lat=%.2f engage=%d dy=%.2f h=%.2f%s\n",
                                  fl.fight.decodes, fl.fight.combos, fl.fight.updates, fl.fight.refused,
                                  fl.fight.leaves, fl.fight.enters, fl.fight.strikeFrames, fl.fight.strikes, fl.fight.landed,
                                  fl.fight.hits, fl.fight.knockOffs,
                                  static_cast<unsigned>(pb.riderDef[0x3C]), static_cast<unsigned>(session.ArenaHalf(top)),
                                  static_cast<unsigned>(session.ArenaHalf(top + 2u)),
                                  static_cast<unsigned>(session.ArenaHalf(R + 0x220u)),
                                  (session.ArenaWord(R + 0x238u) >> 8) & 0xFFu, (session.ArenaWord(R + 0x238u) >> 16) & 0xFFu,
                                  static_cast<unsigned>(pb.riderDef[0x0F]), fl.fight.along / 65536.0, fl.fight.lat / 65536.0,
                                  fl.fight.engage, fl.fight.dy / 65536.0, fl.fight.height / 65536.0, fl.fight.note.c_str());
                    log += line;
                }
                log += rr::game::WeaponFrameLog(session); // weapon_session.h
                log += rr::game::CheatFrameLog(session);  // cheats.h (only when a cheat is on)
                log += rr::game::SolidFrameLog(); // solid_product.h
                // What the PORTED HudFrame linked this frame: the item records by index (DASH1P.CSV
                // order) and the packets from the heap (text, TKO icons, radar marks).
                if (session.HudReady()) {
                    const uint32_t items = session.ArenaWord(rr::sim::kHudItemsPtr);
                    std::string hudLine = "        hud " + std::string(fl.hudRan ? "ran" : "REFUSED") + " items";
                    size_t heap = 0;
                    for (const rr::game::HudPacket& p : rr::game::WalkHudList(session.ArenaRam(), session.HudListHead())) {
                        if (p.address >= items && p.address < items + 3960u)
                            hudLine += " " + std::to_string((p.address - items) / 36u);
                        else
                            ++heap;
                    }
                    char tail[96];
                    std::snprintf(tail, sizeof(tail), " | heap packets %zu | cues %zu | mask 0x%02X\n", heap,
                                  fl.hudSounds, fl.hudMask);
                    hudLine += tail;
                    log += hudLine;
                }
                auto w32 = [](const rr::game::ArenaBytes& e, size_t o) {
                    return static_cast<int32_t>(static_cast<uint32_t>(e[o]) | (static_cast<uint32_t>(e[o + 1]) << 8) |
                                                (static_cast<uint32_t>(e[o + 2]) << 16) |
                                                (static_cast<uint32_t>(e[o + 3]) << 24));
                };
                std::string rivals = "        rivals";
                for (size_t r = 1; r < session.Bikes().size(); ++r) {
                    const rr::game::ArenaBytes& e = session.Bikes()[r].entity;
                    const int16_t steer = static_cast<int16_t>(static_cast<uint16_t>(e[0x27C]) |
                                                               (static_cast<uint16_t>(e[0x27D]) << 8));
                    double aim = 0.0;
                    for (size_t k = 0; k < 3; ++k) {
                        const double d = (w32(e, 0x370 + 4 * k) - w32(e, 0xB8 + 4 * k)) / 65536.0;
                        aim += d * d;
                    }
                    char one[400];
                    std::snprintf(one, sizeof(one),
                                  "  %zu: pos=%.1f v=%d cmd=%d op=%u>%u steer=%d place=%d aim=%.1f road=0x%X lat=%.1f st=%u fc=%X "
                                  "h=%.2f s=%u r184=%X",
                                  r, session.RouteDistance(r), w32(e, 0x240), w32(e, 0x39C),
                                  static_cast<unsigned>(e[0x3B4 + 8 * static_cast<int8_t>(e[0x3B2])] |
                                                        (e[0x3B5 + 8 * static_cast<int8_t>(e[0x3B2])] << 8)),
                                  static_cast<unsigned>(e[0x3B6 + 8 * static_cast<int8_t>(e[0x3B2])]), steer,
                                  session.Bikes()[r].place, std::sqrt(aim), static_cast<unsigned>(w32(e, 0x168)),
                                  w32(e, 0x158) / 65536.0,
                                  static_cast<unsigned>(session.Bikes()[r].owner[0x220] |
                                                        (session.Bikes()[r].owner[0x221] << 8)),
                                  static_cast<unsigned>(w32(e, 0x238)), w32(e, 0x104) / 65536.0,
                                  static_cast<unsigned>(e[0x216]), static_cast<unsigned>(w32(e, 0x184)));
                    rivals += one;
                }
                log += rivals + "\n";
                {   // ai_race.h: the AI passes this frame (planner, command pass by top opcode, brain)
                    char ai[320];
                    int n = std::snprintf(ai, sizeof(ai), "        ai plan=%s brain=%s pushes=%d pops=%d aims=%d unported=%d ops",
                                          fl.aiPlanRan ? (fl.aiPlanOk ? "ran" : "REFUSED") : "-",
                                          fl.aiBrainRan ? (fl.aiBrainOk ? "ran" : "REFUSED") : "-", fl.ai.pushes,
                                          fl.ai.pops, fl.ai.aims, fl.ai.unported);
                    for (int op = 0; op < 20 && n > 0 && n < static_cast<int>(sizeof(ai)) - 12; ++op)
                        if (fl.ai.byOp[op] != 0) n += std::snprintf(ai + n, sizeof(ai) - n, " %d:%d", op, fl.ai.byOp[op]);
                    if (n > 0 && n < static_cast<int>(sizeof(ai)) - 64 && (fl.ai.copTails || fl.ai.copIntercepts))
                        std::snprintf(ai + n, sizeof(ai) - n, " | cops tail=%d stop=%d intercept=%d arrest=%d", // cop_race.h
                                      fl.ai.copTails, fl.ai.copStops, fl.ai.copIntercepts, fl.ai.arrests);
                    log += std::string(ai) + "\n";
                }
                char step[640];
                std::snprintf(step, sizeof(step),
                              "        step ran=%d declined=%d migr=%zu | G integ=%zu listed=%zu | H ground=%zu "
                              "moved=%zu contact=%zu rebind=%zu | I pose=%zu | J endrace=%zu act=%zu down=%zu | "
                              "rider pass: impact=%zu%s heading=%zu%s | stance ev=%zu chg=%zu riderlayer=%zu | "
                              "anim playing=%zu%s | lookahead=%zu\n",
                              fl.stepRan ? 1 : 0, fl.stepDeclined ? 1 : 0, fl.stepMigrations, fl.integratorRan,
                              fl.integratorListed, fl.groundFrames, fl.groundMoved, fl.contactRan,
                              fl.contactRebinds, fl.riderPoses, fl.endRaces, fl.activationSeams, fl.downedSeams,
                              fl.impactBikes, fl.impactDeclined ? "(declined)" : "", fl.headingBikes,
                              fl.headingDeclined ? "(declined)" : "", fl.stanceEvents, fl.stanceChanged,
                              fl.riderLayerCalls, fl.animObjects, fl.animDeclined ? "(declined)" : "",
                              fl.aiLookAheads);
                log += step;
                if (fl.launches != 0) log += "        LAUNCH" + fl.launchNote + "\n";
                {
                    // The PORTED collision pass 0x800A4774 and thrown walk.
                    char coll[420];
                    std::snprintf(coll, sizeof(coll),
                                  "        collision ran=%d declined=%d touchdown=%zu sounds=%zu release=%zu rumble=%zu "
                                  "| thrown walk=%zu%s | airborne=%zu | unported %zu:%s\n",
                                  fl.collisionRan ? 1 : 0, fl.collisionDeclined ? 1 : 0, fl.touchDowns,
                                  fl.collisionSounds, fl.releaseContacts, fl.rumbles, fl.thrownWalked,
                                  fl.thrownDeclined ? "(declined)" : "", fl.airborne, fl.collisionSeamCalls,
                                  fl.collisionNote.empty() ? " none" : fl.collisionNote.c_str());
                    log += coll;
                }
                char camLine[700];
                std::snprintf(camLine, sizeof(camLine),
                              "        camera ran=%d declined=%d collide-refused=%zu collide-pushed=%zu mode=%u flags=0x%X director=%u "
                              "eye=%.2f,%.2f,%.2f look=%.2f,%.2f,%.2f | eye-bike %.2f (behind %.2f above %.2f) "
                              "eye-aim %.2f | +0x2C=%d %s | keys=%zu\n"
                              "        population live=%zu dormant=%zu activate=%zu transitions=%zu refused=%zu%s "
                              "downed=%zu/%zu refused dormantdrive=%zu/%zu refused pieces=%zu | go=%zu crashemit=%zu"
                              "(%zu declined) stamp=%zu reset=%zu viewevent=%zu burst=%zu spray=%zu fx busy=%zu\n",
                              fl.cameraRan ? 1 : 0, fl.cameraDeclined ? 1 : 0, fl.collideSkipped, fl.collidePushes, fl.viewMode,
                              fl.viewFlags, fl.viewDirector, fl.viewEye[0] / 65536.0, fl.viewEye[1] / 65536.0,
                              fl.viewEye[2] / 65536.0, fl.viewLook[0] / 65536.0, fl.viewLook[1] / 65536.0,
                              fl.viewLook[2] / 65536.0, fl.eyeToBike, fl.eyeBehind, fl.eyeAbove, fl.eyeToAim,
                              fl.playerViewDistance, fl.playerViewDistance < 1280 ? "FINE" : "coarse", fl.cameraKeys,
                              fl.liveBikes, fl.dormantBikes, fl.activations, fl.transitions, fl.transitionsRefused,
                              fl.lastRefusal ? (" (last at 0x" + [&] {
                                  char h[16];
                                  std::snprintf(h, sizeof(h), "%08X", fl.lastRefusal);
                                  return std::string(h);
                              }() + ")").c_str() : "",
                              fl.downedCalls, fl.downedRefused, fl.dormantDrives, fl.dormantRefused, fl.piecesListed,
                              fl.raceGo, fl.crashEmits, fl.crashEmitsDeclined, fl.stampResults, fl.resets,
                              fl.viewEvents, fl.bursts, fl.sprays, fl.effectRecordsBusy);
                log += camLine;
                if (fl.padControls.mount >= 2) { // the PORTED pad reader region, off the bike (pad_product.h)
                    char padLine[320];
                    // the rider record's box +0xB8 and speed +0x1E0 (the thrown rider's slide)
                    const uint8_t* ram = session.ArenaRam();
                    const uint32_t ro = session.Player().ownerAddress & 0x1FFFFFu;
                    auto rs32 = [&](uint32_t o) {
                        int32_t v;
                        std::memcpy(&v, ram + ro + o, 4);
                        return v;
                    };
                    std::snprintf(padLine, sizeof(padLine),
                                  "        pad exit=0x%08X mount=%u rider+0x228=0x%08X rider-bike %.2f | rider box=%.2f,%.2f,%.2f "
                                  "+0x1E0=%d\n",
                                  fl.padControls.exit, fl.padControls.mount, fl.padControls.riderFlags,
                                  fl.padControls.riderToBike, rs32(0xB8) / 65536.0, rs32(0xBC) / 65536.0,
                                  rs32(0xC0) / 65536.0, rs32(0x1E0));
                    log += padLine;
                }
                {
                    char tl[200];
                    std::snprintf(tl, sizeof(tl),
                                  "        traffic spawner=%d%s pass=%d%s cars=%d copsout=%d drawn=%zu\n",
                                  fl.spawnerRan ? 1 : 0, fl.spawnerDeclined ? "(declined)" : "", fl.trafficRan ? 1 : 0,
                                  fl.trafficDeclined ? "(declined)" : "", fl.carsLive, fl.copsOut, raceRender.counters.trafficDrawn);
                    log += tl;
                    // each live car: road key, along, direction, lane, speed, flags +0x1FD
                    const uint8_t* carRam = session.ArenaRam();
                    auto carWord = [carRam](uint32_t a) {
                        uint32_t v;
                        std::memcpy(&v, carRam + (a & 0x1FFFFFu), 4);
                        return v;
                    };
                    for (uint32_t s = 0; s < 16; ++s) {
                        const uint32_t car = 0x800CF660u + 512u * s;
                        if ((carWord(car + 0xAC) & 0xFFFFu) == 0) continue;
                        char one[200];
                        std::snprintf(one, sizeof(one),
                                      "          car %u: live=%u road=0x%X along=%.1f dir=%d lane=%d speed=%.2f "
                                      "flags=0x%02X box=%.1f,%.1f\n",
                                      s, carWord(car + 0x140) & 0xFFFFu, carWord(car + 0x168),
                                      static_cast<int32_t>(carWord(car + 0x170)) / 65536.0,
                                      static_cast<int32_t>(carWord(car + 0x16C)), static_cast<int8_t>(carRam[(car + 0x1FC) & 0x1FFFFFu]),
                                      static_cast<int32_t>(carWord(car + 0x1E0)) / 65536.0, carRam[(car + 0x1FD) & 0x1FFFFFu],
                                      static_cast<int32_t>(carWord(car + 0xB8)) / 65536.0,
                                      static_cast<int32_t>(carWord(car + 0xC0)) / 65536.0);
                        log += one;
                    }
                }
                char pl[1100];
                std::snprintf(pl, sizeof(pl),
                              "        player box=%.2f,%.2f,%.2f facing=%d,%d,%d heading=%d,%d,%d netaccel=%d "
                              "throttle=%d leanF=%d latF=%d | +0x168=0x%08X +0x16C=%d +0x170=%.2f +0x158=%.2f "
                              "+0x184=0x%X +0x144=%d route=0x%08X slice=0x%08X idx=%d | ground q=%zu hit=%zu "
                              "exh=%zu miss=%zu decl=%zu +0x218=0x%08X +0x112=%d,%d,%d +0x216=%u +0x2C=%d | "
                              "cells %zu (+%zu -%zu) | rider stance=%u mount=%d anim frame=%u flags=0x%X | "
                              "flags %08X %08X %08X rider+0x228 %08X list 0x%08X\n",
                              fl.playerBox[0] / 65536.0, fl.playerBox[1] / 65536.0, fl.playerBox[2] / 65536.0,
                              fl.playerFacing[0], fl.playerFacing[1], fl.playerFacing[2], fl.playerHeading[0],
                              fl.playerHeading[1], fl.playerHeading[2], fl.playerNetAccel, fl.playerThrottleAmt,
                              fl.playerLeanF, fl.playerLatForce, fl.playerRoadWord, fl.playerDirection,
                              fl.playerAlong / 65536.0, fl.playerLateralRoad / 65536.0, fl.playerFlags184,
                              fl.playerProgress, fl.playerRoute, fl.playerSlice, fl.playerSliceIndex,
                              fl.groundQueries, fl.groundHits, fl.groundExhausted, fl.groundMisses,
                              fl.groundDeclined, fl.playerGroundAnswer, fl.playerGroundNormal[0],
                              fl.playerGroundNormal[1], fl.playerGroundNormal[2], fl.playerSurface,
                              fl.playerViewDistance, fl.cellsResident, fl.cellLoads, fl.cellUnloads,
                              static_cast<unsigned>(fl.playerStance), fl.playerMount, fl.playerAnimFrame,
                              fl.playerAnimFlags, fl.playerFlags[0], fl.playerFlags[1], fl.playerFlags[2],
                              fl.playerRiderFlags, fl.playerList);
                log += pl;
                if (session.Sounds().EngineMode()) {
                    const auto& snd = session.Sounds();
                    const uint32_t E = snd.ArenaWord(0x8005AC8C + 1952);
                    const uint32_t h0 = E ? snd.ArenaWord(E + 0x10) : 0u;
                    char note[300];
                    std::snprintf(note, sizeof(note),
                                  "        engine note frames=%zu vsyncs=%zu spuwrites=%zu keyons=%u level=%d "
                                  "target=%d load=%d L0 ch %u pitch 0x%04X peak=%d clipped=%llu\n",
                                  snd.EngineFrames(), snd.VSyncs(), snd.SpuWrites(), snd.Spu()->KeyOns(),
                                  E ? static_cast<int32_t>(snd.ArenaWord(E + 0x30)) : 0,
                                  E ? static_cast<int32_t>(snd.ArenaWord(E + 0x34)) : 0,
                                  E ? static_cast<int32_t>(snd.ArenaWord(E + 0x60)) : 0, h0 >> 27,
                                  static_cast<unsigned>(snd.Spu()->Read(16u * (h0 >> 27) + 4u)), snd.Spu()->Peak(),
                                  static_cast<unsigned long long>(snd.Spu()->Clipped()));
                    log += note;
                    // Every sounding voice as the SPU registers hold it: channel, volume L/R (the
                    // 15-bit fixed-mode registers), pitch, ADSR1/2, and the main volume L/R.
                    std::string voices = "        spu voices";
                    for (int ch = 0; ch < rr::audio::SpuVoices::kChannels; ++ch) {
                        if (!snd.Spu()->State(ch).on) continue;
                        const uint32_t b = 16u * static_cast<uint32_t>(ch);
                        char v[96];
                        std::snprintf(v, sizeof(v), " %d:%04X/%04X p%04X a%04X/%04X", ch,
                                      snd.Spu()->Read(b + 0), snd.Spu()->Read(b + 2), snd.Spu()->Read(b + 4),
                                      snd.Spu()->Read(b + 8), snd.Spu()->Read(b + 10));
                        voices += v;
                    }
                    // The headroom: the sum of the sounding voices' |volume| per side (the fixed-mode
                    // register is volume/2, so full scale is 0x4000), i.e. how many times full scale the
                    // voice sum can reach when every voice's sample is at full amplitude.
                    double sumL = 0.0, sumR = 0.0;
                    for (int ch = 0; ch < rr::audio::SpuVoices::kChannels; ++ch) {
                        if (!snd.Spu()->State(ch).on) continue;
                        auto mag = [](uint16_t r) {
                            if (r & 0x8000u) return 0.0;
                            const int32_t v = static_cast<int32_t>((r & 0x7FFFu) ^ 0x4000u) - 0x4000;
                            return static_cast<double>(v < 0 ? -v : v) / 16384.0;
                        };
                        sumL += mag(snd.Spu()->Read(16u * static_cast<uint32_t>(ch)));
                        sumR += mag(snd.Spu()->Read(16u * static_cast<uint32_t>(ch) + 2u));
                    }
                    char mv[128];
                    std::snprintf(mv, sizeof(mv), " | sum|vol| L %.2f R %.2f of full scale | main %04X/%04X\n", sumL,
                                  sumR, snd.Spu()->Read(0x180), snd.Spu()->Read(0x182));
                    log += voices + mv;
                    // The hit sounds: every key-on of this frame with the registers it started
                    // from, and the emitter calls (sound index, x, z) the race made through SoundRuntime.
                    std::string kon = HitSoundTally::Describe(frameKeyOns);
                    for (const auto& c : framePlayed) {
                        char v[64];
                        std::snprintf(v, sizeof(v), " #%d(%d,%d)", c.id, c.x >> 16, c.z >> 16);
                        kon += v;
                    }
                    if (!kon.empty()) log += "        spu keyons/sound3d" + kon + "\n";
                }
            }
            // Launches, landings and the return to the riding list, on every frame.
            if (!logPath.empty() && !fl.airNote.empty()) log += fl.airNote;
            if (!logPath.empty() && !raceRender.counters.vanishNote.empty()) log += raceRender.counters.vanishNote;
            raceRender.counters.vanishNote.clear();

            ++frames;
            if (heartbeat != nullptr) {
                std::fprintf(heartbeat, "frame %ld\n", frames);
                std::fflush(heartbeat);
            }
            const bool lastFrame = frameLimit >= 0 && frames >= frameLimit;
            if (lastFrame && !shotPath.empty() && vr == nullptr) {
                // Before the swap: the back buffer is undefined afterwards and a shot taken after
                // it comes out black.
                glFinish();
                lastShot = ReadFrame(width, height);
                lastWidth = width;
                lastHeight = height;
            }
            host.Present();
            frameDone(); // the profiler's clock, the frame cap
            if (lastFrame) break;
            // A race the front end started goes back to it (src\game\shell\handover.h).
            // Esc is the pause's Start now (pause_product.h); RRJB_PAUSE=off: the old quit
            if (rr::shell::RaceLeaves(session.RaceOver(), !rr::game::PauseOn() && keys[VK_ESCAPE])) break;
            if (rr::game::PauseOn() && session.GameStateByte() == 5) break; // the menu's RESTART (main 0x80012360)
        }
        if (heartbeat != nullptr) std::fclose(heartbeat);
        // the menu's RESTART (state 5): the race again, the handover as the front end left it (pause_product.h)
        const bool restartRace = rr::game::PauseOn() && session.GameStateByte() == 5;
        if (!restartRace)
            rr::shell::NoteRaceReturn(session.ArenaRam(), session.Player().riderDefAddress, session.GameStateByte());
        if (!arenaDumpPath.empty()) { // DEVELOPMENT: the arena as the run left it (a guest RAM image; game data -> work\)
            if (FILE* f = std::fopen(arenaDumpPath.c_str(), "wb")) {
                std::fwrite(session.ArenaRam(), 1, 2u * 1024u * 1024u, f);
                std::fclose(f);
            }
        }

        if (!lastShot.empty()) SaveShot(shotPath, lastWidth, lastHeight, lastShot);

        // ---- what the run actually leaned on. This is the honest part: every unported callee the
        // frames asked for, named by its address, printed and written into the log.
        std::string seamReport;
        seamReport += "\nSEAMS - what this run asked the original for and did not have:\n";
        for (const std::string& seam : session.Seams()) seamReport += "  * " + seam + "\n";
        char tail[1024];
        {
            const rr::game::FrameLog& fl = session.Log();
            std::snprintf(tail, sizeof(tail),
                          "\nframes run %ld, race clock %d ticks (%.2f s at 1/300 s per tick)\n"
                          "the PORTED per-bike step RASHCDG 0x80075EE0 on the last frame: ran %d, declined %d; "
                          "region G integrated %zu bike(s) (%zu appended to the active list), region H ran the "
                          "ground frame %zu time(s) (moved +0x1F8 on %zu) and the contact response %zu time(s) "
                          "(%zu PORTED re-binds), region I posed %zu, region J ran the PORTED activation "
                          "pass %zu and downed-rider pass %zu time(s) and the PORTED EndRace %zu time(s); the "
                          "list migration ran %zu time(s)\n"
                          "opponents with a non-zero steering angle on the last frame: %zu of %zu\n"
                          "the race %s end: riders finished %zu of %zu, finish tests this frame %zu (declined %zu), "
                          "game_state+0x00 = %u%s\n",
                          frames, fl.raceClock, static_cast<double>(fl.raceClock) / 300.0, fl.stepRan ? 1 : 0,
                          fl.stepDeclined ? 1 : 0, fl.integratorRan, fl.integratorListed, fl.groundFrames,
                          fl.groundMoved, fl.contactRan, fl.contactRebinds, fl.riderPoses, fl.activationSeams,
                          fl.downedSeams, fl.endRaces, fl.stepMigrations, fl.aiSteered, session.Bikes().size() - 1,
                          session.CanFinish() ? "CAN" : "cannot", fl.ridersFinished, session.Bikes().size(),
                          fl.finishTestsRequested, fl.finishTestsDeclined,
                          static_cast<unsigned>(session.GameStateByte()),
                          session.RaceOver() ? "  <- THE RACE IS OVER" : "");
        }
        seamReport += tail;
        {
            char b[320];
            std::snprintf(b, sizeof(b),
                          "the rivals' draw range (cell_view.h InDrawRange, ModelVisible 0x80067AC4)%s: %zu rival-frame(s) "
                          "of a live rival past it; rivals that vanished in the air while drawn: %zu\n",
                          drawRangeOff ? " SWITCHED OFF (--draw-range-off, a negative control)" : "",
                          raceRender.counters.rivalFramesPastRange, raceRender.counters.rivalsVanishedInAir);
            seamReport += b;
            // the original's subdividers (scene_cell.md 13.8) on the near road and fine near groups
            char sb[512];
            std::snprintf(sb, sizeof(sb),
                          "subdiv: %s; road strips %zu -> %zu pieces (0x80069CF0), fine near primitives %zu, %zu cut "
                          "-> %zu pieces (0x80069784 / 0x8006929C), %zu off-screen, %zu refused; the cell emitters' "
                          "back-face test (NCLIP) %s: %zu fine near primitive(s) facing away dropped\n",
                          !scene.Affine() ? "SWITCHED OFF (RRJB_AFFINE=off: perspective-correct, not cut)"
                          : !scene.Subdivides() ? "affine, NOT cut (RRJB_SUBDIV=off)" : "affine and cut as the original",
                          raceRender.counters.subdiv.roadStrips, raceRender.counters.subdiv.roadPieces, raceRender.counters.subdiv.nearPrims, raceRender.counters.subdiv.splitPrims,
                          raceRender.counters.subdiv.pieces, raceRender.counters.subdiv.culledPrims, raceRender.counters.subdiv.refused,
                          cellNclip ? "on" : "OFF (RRJB_CELL_NCLIP=off, the control)", raceRender.counters.subdiv.backPrims);
            seamReport += sb;
            seamReport += scene.GteTotals() + (rr::render::GteProjOn() ? "" : " - SWITCHED OFF (RRJB_PROJ=float, the control)") + "\n";
            seamReport += rr::render::EdgeTotals() + "\n"; // edge_rule.h
            // The 3D scene in the original's ordering-table order
            std::snprintf(sb, sizeof(sb),
                          "otsort: the 3D scene %s in %zu of %ld frame(s), two tables (SLUS 0x80035958) in %zu; last "
                          "frame's tables near %d shift %d last slot %d / near %d shift %d last slot %d; cell depth "
                          "ranges within 16 of the capture's (--parity) %zu of %zu (worst %d), last table's map equal to the capture's: %s; %zu vertex "
                          "source binding(s), %zu shadow packet(s) at their own slot\n",
                          otZbuffer ? "drawn with the depth buffer (RRJB_OT_ORDER=zbuffer, the negative control)"
                                    : "drawn in the original's ordering-table order",
                          raceRender.counters.otViews, frames, scene.OtChecked().twoPassFrames, scene.OtPass(1).nearOffset,
                          scene.OtPass(1).shift, scene.OtPass(1).used ? scene.OtPass(1).maxSlot : -1,
                          scene.OtPass(2).nearOffset, scene.OtPass(2).shift,
                          scene.OtPass(2).used ? scene.OtPass(2).maxSlot : -1, scene.OtChecked().depthsEqual,
                          scene.OtChecked().cells, scene.OtChecked().worst, scene.OtChecked().mapEqual ? "yes" : "no", scene.OtSources(),
                          scene.OtShadowQuads());
            seamReport += sb;
            if (renderTimed && renderFrames > 0) {
                std::snprintf(sb, sizeof(sb), "render time: the 3D views %.3f ms a frame on average, %.3f ms at worst, over %ld frame(s)\n",
                              1000.0 * renderSeconds / static_cast<double>(renderFrames), 1000.0 * renderWorst, renderFrames);
                seamReport += sb;
            }
            // What the frame drew the original's way, with each negative control
            std::snprintf(b, sizeof(b),
                          "parity: model light %s in %zu of %ld frame(s); rival machines drawn as their own models %zu "
                          "time(s)%s; sun yaw %d%s; bike frame %s; one-player picture %s\n",
                          modelLightOff ? "SWITCHED OFF (RRJB_MODEL_LIGHT=off)" : "on", raceRender.counters.modelLightViews, frames,
                          scene.RivalMachinesDrawn(), RivalsAsPlayer() ? " (RRJB_RIVALS=player: all as the player's)" : "",
                          sunOff ? 0 : scene.SunYaw(), sunOff ? " (RRJB_SUN=off)" : "",
                          BikeFrameFromGround() ? "the ground frame (RRJB_BIKE_FRAME=ground)" : "+0x1B0 (the model draw's)",
                          !parityState.empty() ? "the whole draw area (--parity)"
                          : shownOff ? "the whole 240 lines (RRJB_SHOWN=off)" : "the shown 365 x 224");
            seamReport += b;
            if (!shadowCheckCsv.empty()) { // the frame's shadow packets against the original's
                const std::string line = rr::game::ShadowPacketCheck(models.shadows, shadowCheckCsv);
                std::printf("%s", line.c_str());
                seamReport += line;
            }
        }
        seamReport += session.CollisionTotals();
        seamReport += rrgame::RiderPlacementLine(); // race_render.h
        if (headCamLoaded) { // the head camera (head_camera.h)
            const auto& ht = headCam.Totals();
            char b[600];
            std::snprintf(b, sizeof(b),
                          "head camera: %zu frame(s) drawn from it; %zu update(s), %zu seated, %zu invalid; posed head centre "
                          "against the PORTED model draw's head vertices: %zu frame(s), mean %.4f max %.4f world units (the previous "
                          "frame's posed head: %zu, mean %.4f max %.4f); eye "
                          "(%.0f, %.0f, %.0f) in head part %d, model units\n",
                          ht.drawn, ht.updates, ht.seated, ht.invalid, ht.compared,
                          ht.compared ? ht.sumDist / static_cast<double>(ht.compared) : 0.0, ht.maxDist, ht.comparedPrev,
                          ht.comparedPrev ? ht.sumPrev / static_cast<double>(ht.comparedPrev) : 0.0, ht.maxPrev,
                          headCam.EyeLocal()[0], headCam.EyeLocal()[1], headCam.EyeLocal()[2], rr::game::kHeadPart);
            seamReport += b;
        }
        if (vr != nullptr) { // game_host_vr.h
            char b[400];
            std::snprintf(b, sizeof(b),
                          "vr: %zu stereo frame(s) from the rider's head, %zu from the chase camera, %zu with the pause menu "
                          "on the theatre quad; %s\n",
                          vrHeadFrames, vrChaseFrames, vrPauseQuadFrames, vr->Display().Describe().c_str());
            seamReport += b;
            seamReport += vrBars.Totals() + "\n"; // the Handlebars mode (vr_handlebars.h)
            seamReport += vrMelee.Totals() + "\n"; // physical combat (vr_melee.h)
            seamReport += vrHolsters.Totals() + "\n"; // the holsters (vr_holsters.h)
            seamReport += rrgame::ComfortTotals();             // vr_comfort.h
            seamReport += rrgame::ProductMotionMeter().Totals();
            seamReport += rrgame::ProductViewPitch().Totals() + rrgame::ProductNearMeter().Totals() +
                          rrgame::ProductRivalSmooth().Totals(); // vr_horizon.h
            seamReport += rrgame::ProductPacing().Totals(); // vr_pacing.h
            seamReport += rrgame::ProductBikeShakeMeter().Totals() + rrgame::ProductBikeShake().Totals(); // vr_bike_shake.h
            if (!mockHitFrames.empty()) seamReport += "vr mock: HitRumble's motors at frame(s)" + mockHitFrames + "\n";
        }
        if (vr == nullptr && !rrgame::ProductBikeShakeMeter().Totals().empty()) // the desktop head camera
            seamReport += rrgame::ProductBikeShakeMeter().Totals() + rrgame::ProductBikeShake().Totals();
        if (!twoPlayers && rr::game::PlayerHandling().Engaged()) // Original, no test input: no line
            seamReport += rr::game::PlayerHandling().Summary() + "\n" +
                          (rrgame::HandlingView().Frames() > 0 ? rrgame::HandlingView().Summary() + "\n" : std::string());
        if (!twoPlayers && rr::game::PlayerWheelie().Engaged()) // off, no test input: no line
            seamReport += rr::game::PlayerWheelie().Summary() + "\n" + rrgame::WheelieBarsTotals();
        seamReport += session.RecoverTotals();

        seamReport += rr::game::WeaponTotals(); // weapon_session.h
        if (!bundleLine.empty())                // level_bundle.h: the resident bundle (RASHCDI 0x80061A98)
            seamReport += bundleLine + "; races answered with the resident bundle in this process: " +
                          std::to_string(rr::ResidentLevelBundle().sentinelRaces) + "\n";
        seamReport += rr::game::SolidTotals(); // solid_product.h
        seamReport += rr::game::AnimObjTotals(); // animobj_product.h
        seamReport += rr::game::JunctionTotals(); // junction_product.h
        seamReport += rr::game::TakedownTotals(); // takedown_product.h
        seamReport += rr::game::PadGateTotals(); // fight_session.h (the pad combat gate)
        seamReport += rr::game::FightTotals();   // fight_session.h (the fight damage gate)
        seamReport += rr::game::PhysicalBlowTotals(); // fight_session.h (VR physical combat)
        if (rr::game::CheatReport()) seamReport += rr::game::CheatTotals(); // cheats.h; --cheat-report
        seamReport += g_hitSounds.Line(session.Sounds().AnimSoundCalls());
        if (autoSteerOn)                         // the test script's own counters (ai_race.h, pad_product.h)
            seamReport += "the autosteer script (OURS): pole dodges " +
                          std::to_string(autoSteer.poleDodges) + " frame(s) (RRJB_DODGE=off: 0), on foot with no key " +
                          "held (RiderRecover's own walk) " + std::to_string(rr::game::WalkFreeFrames()) +
                          " frame(s) (RRJB_WALKFREE=off: 0)\n";
        seamReport += rr::game::StrikeTotals(); // strike_product.h
        rr::game::RumbleRaceEnd(session.MutableArenaRam(), 0x8005AC8Cu); // LeaveRace's motors off
        seamReport += rr::game::RumbleTotals();  // rumble_product.h
        seamReport += rr::game::PauseTotals();  // pause_product.h
        if (session.Players() == 2)
            seamReport += "player 2 Start (mp_input.h): " + std::to_string(p2StartFrames) + " frame(s) with Start on pad 2 " +
                          (pad2Start ? "" : "(RRJB_P2_START=off) ") + "; pauses by pad 1: " + std::to_string(pausesBy[0]) +
                          ", by pad 2: " + std::to_string(pausesBy[1]) + "\n";
        if (!pauseCheckCsv.empty()) { // the menu's GP0 commands against the original's
            bool pausePass = false;
            const std::string line = rr::game::PausePacketCheck(session, pauseCheckCsv, pauseCheckMutate, &pausePass);
            std::printf("%s", line.c_str());
            seamReport += line;
        }
        seamReport += models.Totals(); // model_runtime.h
        {   // shadow_product.h: the PORTED model shadow
            char b[400];
            std::snprintf(b, sizeof(b),
                          "look: the model shadow SLUS 0x80025EE0 %s: %zu call(s), %zu packet(s), %zu object-draw(s) with a "
                          "shadow, %zu refused (first 0x%08X), %zu two-player (0x80026960: the mp2 line); "
                          "drawn in %zu view-frame(s)\n",
                          rr::game::ShadowPortOn() ? "PORTED" : "SWITCHED OFF (RRJB_SHADOW=ours: our approximation)",
                          models.shadows.calls, models.shadows.packets, models.shadows.objects, models.shadows.refused,
                          models.shadows.firstRefused, models.shadows.twoPlayer, scene.PortedShadowDraws());
            seamReport += b;
            seamReport += rr::game::Mp2Report() + "\n";
            std::snprintf(b, sizeof(b),
                          "look: machines from the PORTED model draw's vertices %s: %zu group draw(s), %zu capture(s) not of "
                          "the model / LOD drawn%s\n",
                          std::getenv("RRJB_POSE_FROM") ? "SWITCHED OFF (RRJB_POSE_FROM=ours)" : "on", scene.CapturedDraws(),
                          scene.CapturedMissed(), RaceScene::LookLightOff() ? "; the other objects' model light SWITCHED OFF (RRJB_LOOK_LIGHT=off)" : "");
            seamReport += b;
            std::snprintf(b, sizeof(b), "look: the frame drawn from the render camera record (+0x5C / +0x1C) in %zu view-frame(s), "
                          "from the view record in %zu%s%s", rrgame::g_renderCameraFrames, rrgame::g_renderCameraFallbacks,
                          std::getenv("RRJB_GL_CAMERA") ? " (RRJB_GL_CAMERA=view: SWITCHED OFF)" : "", "\n");
            seamReport += b;
        }
        {   // the settings the run drew with and, with --profile-log, its totals
            seamReport += gfx.Describe() + "\n";
            char gb[640];
            std::snprintf(gb, sizeof(gb),
                          "graphics: maximum detail %s: %zu placement-record prop(s) offered to the views (drawn where "
                          "their cell is drawn and no pool holds them); %s\n",
                          gfx.maxDetail ? "ON" : "off", raceRender.counters.recordPropsAdded, SmoothTotals().c_str());
            seamReport += gb;
            if (scene.LayeredPrimitives() > 0) { // only a run that drew with the depth buffer builds them
                std::snprintf(gb, sizeof(gb),
                              "graphics: coplanar layers (race_scene_pc.cpp): %zu overlapping coplanar pair(s) in the "
                              "cell soup, %zu primitive(s) drawn again on top with the depth buffer, deepest layer %d; models "
                              "(props, player machine): %zu pair(s), %zu decal primitive(s)\n",
                              scene.CoplanarPairs(), scene.LayeredPrimitives(), scene.LayerMax(), scene.ModelPairs(),
                              scene.ModelWinners());
                seamReport += gb;
            }
            if (!profileLog.empty() && ptotals.frames > 0) {
                const double n = static_cast<double>(ptotals.frames);
                std::snprintf(gb, sizeof(gb),
                              "profile: %ld frame(s) past the first 60 at %dx%d, MSAA %dx: %.1f FPS (mean frame %.2f ms, worst "
                              "%.1f ms), CPU %.2f ms a frame of which the views %.2f ms, GPU %.2f ms (%ld timed), %.0f draw "
                              "calls and %.0f triangles a frame; cell runs + road pieces drawn %.0f / culled %.0f, props %.0f / "
                              "%.0f, machines, cars, pedestrians %.0f / %.0f a frame\n",
                              ptotals.frames, ptotals.renderW, ptotals.renderH, ptotals.samples, 1000.0 * n / ptotals.frameMs,
                              ptotals.frameMs / n, ptotals.worstMs, ptotals.cpuMs / n, ptotals.renderMs / n,
                              ptotals.gpuFrames ? ptotals.gpuMs / static_cast<double>(ptotals.gpuFrames) : -1.0, ptotals.gpuFrames,
                              static_cast<double>(ptotals.drawCalls) / n, static_cast<double>(ptotals.triangles) / n,
                              static_cast<double>(ptotals.cellRuns) / n, static_cast<double>(ptotals.cellRunsCulled) / n,
                              static_cast<double>(ptotals.props) / n, static_cast<double>(ptotals.propsCulled) / n,
                              static_cast<double>(ptotals.objects) / n, static_cast<double>(ptotals.objectsCulled) / n);
                seamReport += gb;
                if (FILE* pf = std::fopen(profileLog.c_str(), "wb")) {
                    std::fputs(gb, pf);
                    std::fclose(pf);
                }
            }
        }
        seamReport += rr::game::CellDrawTotals(); // cell_view.h
        seamReport += rr::game::CellSortTotals(); // cell_sort_product.h
        {
            char cb[300];
            std::snprintf(cb, sizeof(cb),
                          "cellsort: effect packets linked into their entity's cell's table (0x800674D4 -> 0x80067770): the "
                          "first %zu, the second %zu, one table (no arena sort) %zu, left out (the cell in neither) %zu",
                          fxDraw.otTableCounts[0], fxDraw.otTableCounts[1], fxDraw.otTableCounts[2], fxDraw.otTableCounts[3]);
            seamReport += cb;
            seamReport += '\n';
        }
        seamReport += rr::game::WorldTotalsLine(); // world_pop_product.h
        seamReport += rr::game::HazardTotals(); // hazard_product.h (the hazard objects)
        seamReport += rr::game::PedTotalsLine(); // peds_product.h
        if (gfx.maxDetail || std::getenv("RRJB_ANIM_DETAIL_TRACE")) seamReport += rr::game::AnimDetailLine(); // anim_detail.h
        seamReport += rr::game::PassTotalsLine(); // passes_product.h
        seamReport += rr::game::LoaderTotalsLine() + '\n'; // loader_product.h
        seamReport += rr::game::Loader2Line() + '\n';     // loader_product.h
        seamReport += rr::game::RouteLine() + '\n';       // route_product.h
        seamReport += rr::game::StreamFilesLine() + '\n'; // stream_files_product.h
        seamReport += rr::game::StreamTotals(); // stream_product.h
        seamReport += rr::game::SkyTotals() + '\n'; // sky_product.h
        { // the passes' comparison line: the race clock and the finishing order (place : slot @ finish stamp)
            std::string order;
            std::vector<std::pair<int, size_t>> done;
            const auto& bk = session.Bikes();
            for (size_t i = 0; i < bk.size(); ++i) {
                int32_t stamp = 0;
                std::memcpy(&stamp, bk[i].riderDef.p + 0x28, 4);
                if (stamp != 0) done.push_back({static_cast<int>(bk[i].riderDef.p[0x27]), i});
            }
            std::sort(done.begin(), done.end());
            for (const auto& [place, i] : done) {
                int32_t stamp = 0;
                std::memcpy(&stamp, bk[i].riderDef.p + 0x28, 4);
                order += " " + std::to_string(place) + ":s" + std::to_string(i) + "@" + std::to_string(stamp);
            }
            order += " | unfinished places:";
            for (size_t i = 0; i < bk.size(); ++i) {
                int32_t stamp = 0;
                std::memcpy(&stamp, bk[i].riderDef.p + 0x28, 4);
                if (stamp == 0) order += " s" + std::to_string(i) + "=" + std::to_string(bk[i].riderDef.p[0x27]);
            }
            seamReport += "passes compare: race clock " + std::to_string(session.Log().raceClock) + " ticks; finished" +
                          order + "\n";
        }
        seamReport += "the props drawn from pools 4 / 5" + std::string(propsFromRecords ? " SWITCHED OFF (--props-from-records)" : "") +
                      ": at most " + std::to_string(raceRender.counters.worldPropsMax) + " in a frame, in " + std::to_string(raceRender.counters.worldPropFrames) +
                      " frame(s); car-frames past ModelVisible's kind-3 range (not drawn): " + std::to_string(raceRender.counters.carsPastRange) + "\n";
        seamReport += "the cars drawn by level of detail (traffic_draw.h, car +0x08): LOD 0 " + std::to_string(traffic.LodDraws(0)) +
                      ", 1 " + std::to_string(traffic.LodDraws(1)) + ", 2 " + std::to_string(traffic.LodDraws(2)) +
                      "; the weapon in hand (weapon_draw.h): " + std::to_string(weapons.DrawnTotal()) + " draw(s), " +
                      std::to_string(weapons.PosedTotal()) + " with posed parts, sheet " +
                      (weapons.Textured() ? "textured" : "NOT loaded") + "\n";
        seamReport += "\n" + session.ModeTotals() + "\n"; // race_modes.cpp (the game modes)
        if (scene.HasSidecar()) {
            char c[400];
            std::snprintf(c, sizeof(c),
                          "the sidecar rig (race_scene_sidecar.cpp)%s: %zu frame(s) drawn from its %zu part slots; frames "
                          "a slot changed: fork 1 %zu, pitch 2 %zu, wheel 3 %zu, wheel 4 %zu, sidecar wheel 5 %zu\n",
                          rigOff ? " SWITCHED OFF (RRJB_RIG=off: the rest pose)" : "", raceRender.counters.rigPosed, scene.SidecarParts(),
                          raceRender.counters.rigMoved[1], raceRender.counters.rigMoved[2], raceRender.counters.rigMoved[3], raceRender.counters.rigMoved[4], raceRender.counters.rigMoved[5]);
            seamReport += c;
            std::snprintf(c, sizeof(c),
                          "look3: the sidecar rig and its passenger from the PORTED model draw's vertices%s: rig %zu group "
                          "draw(s), passenger %zu, %zu capture(s) not of the model / LOD drawn\n",
                          std::getenv("RRJB_POSE_FROM") ? " SWITCHED OFF (RRJB_POSE_FROM=ours)" : "", scene.SidecarCaptured(),
                          scene.PassengerCaptured(), scene.SidecarCaptureMissed());
            seamReport += c;
        }
        seamReport += '\n';
        {
            char c[1400];
            std::snprintf(c, sizeof(c),
                          "the PORTED camera ViewUpdate RASHCDG 0x800881B4 on view record 0: %zu frame(s), %zu refused; "
                          "the eye's distance to the player's box centre while racing %.2f .. %.2f (mean %.2f) world "
                          "units over %zu frame(s); the player's +0x2C below 1280 (the ground query's FINE mesh) on %zu "
                          "of them; CameraCollide 0x800A421C PORTED (camera_collide.h: the frame log's collide-pushed)\n"
                          "the PORTED population passes: live bikes %zu .. %zu, dormant at most %zu; %zu transition(s) "
                          "made, %zu refused (last refused callee 0x%08X) and restored; the downed-rider pass %zu call(s), "
                          "%zu refused; DormantDrive %zu call(s)\n"
                          "the PORTED spine seams: RaceGo %zu, CrashEmit %zu (%zu not run), StampResult %zu, "
                          "ResetBikeState %zu, ViewEvent %zu; RoadNote's spawners: %zu burst(s), %zu spray(s), at most "
                          "%zu effect record(s) in use\n",
                          cam.frames, cam.declined, cam.racing ? cam.minDist : 0.0, cam.maxDist,
                          cam.racing ? cam.sumDist / static_cast<double>(cam.racing) : 0.0, cam.racing, cam.fine,
                          cam.minLive, cam.maxLive, cam.maxDormant, cam.transitions, cam.refused, cam.lastRefusal,
                          cam.downed, cam.downedRefused, cam.dormantDrives, cam.raceGo, cam.crashEmits,
                          cam.crashDeclined, cam.stamps, cam.resets, cam.viewEvents, cam.bursts, cam.sprays,
                          cam.maxBusy);
            seamReport += c;
            std::snprintf(c, sizeof(c),
                          "the eye's distance to the camera's subject (the player's rider box while he is off the bike, "
                          "+0x25C >= 2, on %zu frame(s); the bike's box otherwise) while racing: max %.2f at frame %ld, "
                          "mean %.2f world units\n",
                          cam.offFrames, cam.subjMax, cam.subjMaxFrame,
                          cam.racing ? cam.subjSum / static_cast<double>(cam.racing) : 0.0);
            seamReport += c;
            std::snprintf(c, sizeof(c),
                          "the PORTED traffic: SpawnerPass 0x8008CD88 %zu frame(s), %zu declined; TrafficPass 0x8009A298 "
                          "%zu frame(s), %zu declined; pool 3 live at most %zu, cops out at most %zu. The cars drawn "
                          "(traffic_draw.h, models of %s): at most %zu in a frame, on screen in %zu frame(s); %zu live "
                          "car-frames with a model id not in that file\n",
                          spawnerFrames, spawnerDeclined, trafficFrames, trafficDeclined, trafficMaxLive, trafficMaxCops,
                          traffic.FileName().c_str(), raceRender.counters.trafficMaxDrawn, raceRender.counters.trafficFramesWithCars, raceRender.counters.trafficUnknown);
            seamReport += c;
            std::snprintf(c, sizeof(c),
                          "the PORTED effect pass (fx_runtime.h): %zu frame(s), %zu refused; %zu packet(s) linked, %zu of "
                          "them drawable (3..6 with the model draw's capture, model_runtime.h); at most %zu drawn in a frame, drawn in %zu frame(s); at most %zu "
                          "effect record(s) live; %zu siren call(s) not run; packets by state: 1 spark %zu, 2 spray %zu, "
                          "3 streak %zu, 4 nitro flame %zu, 5 police light %zu, 6 weapon trail %zu, 7 burst %zu; the "
                          "emitter's glow sprites (fx_glow.h, no record) %zu\n",
                          fx.frames, fx.refusals, fx.packetsTotal, fx.packetsDrawn, raceRender.counters.fxDrawnMax, raceRender.counters.fxFramesDrawn, fx.maxLive,
                          fx.sirens, fx.byState[1], fx.byState[2], fx.byState[3], fx.byState[4], fx.byState[5],
                          fx.byState[6], fx.byState[7], fx.byState[0]);
            seamReport += c;
        }
        {
            const rr::game::FrameLog& fl = session.Log();
            std::string stats = "stat blocks (entity[+0x22C]):";
            for (size_t r = 0; r < session.Bikes().size(); ++r) {
                const rr::game::RaceBike& b = session.Bikes()[r];
                int32_t e0 = 0;
                if (b.stats != nullptr) std::memcpy(&e0, b.stats + 0xE0, 4);
                char one[96];
                std::snprintf(one, sizeof(one), " %zu:block %d @0x%08X +0xE0=%d", r, b.statsBlock,
                              b.statsAddress, e0);
                stats += one;
            }
            seamReport += stats + "\n";
            char route[400];
            std::snprintf(route, sizeof(route),
                          "the route arena: %d route record(s) + the start record, finish record at 0x%08X = "
                          "{road %d, line %.2f, side %d, key %d}; the player's +0x1AC = 0x%08X, +0x144 = %d "
                          "(the PORTED ProgressPass 0x8003B520)\n",
                          session.RouteRecordCount(), session.FinishRecord(),
                          static_cast<int32_t>(session.ArenaWord(session.FinishRecord())),
                          static_cast<int32_t>(session.ArenaWord(session.FinishRecord() + 4)) / 65536.0,
                          static_cast<int32_t>(session.ArenaWord(session.FinishRecord() + 8)),
                          static_cast<int32_t>(session.ArenaWord(session.FinishRecord() + 12)), fl.playerRoute,
                          fl.playerProgress);
            seamReport += route;
            char road[900];
            std::snprintf(road, sizeof(road),
                          "the PORTED road layer inside region H over the run: bikes off the road (+0x184 bit 0) "
                          "on %zu frame(s), at most %zu at once, the player on %zu frame(s). The PORTED ground "
                          "query RASHCDG 0x800A7BF8: %zu call(s), %zu hit(s), %zu lists-exhausted, %zu -1, %zu "
                          "declined; cells: %zu loaded, %zu freed, at most %zu resident; the player's last "
                          "terrain hit 0x%08X normal +0x112 = (%d, %d, %d)\n"
                          "the player's rider: stance +0x220 = %u, mount +0x25C = %d, animation frame %u, flags "
                          "0x%X; the PORTED stance event ran %zu time(s) on the last frame, the rider layer %zu\n",
                          totals.offRoadFrames, totals.maxOffRoad, totals.playerOffRoadFrames, totals.queries,
                          totals.hits, totals.exhausted, totals.misses, totals.qDeclined, totals.loads,
                          totals.unloads, totals.maxResident, totals.lastHitAnswer, totals.lastHitNormal[0],
                          totals.lastHitNormal[1], totals.lastHitNormal[2], static_cast<unsigned>(fl.playerStance),
                          fl.playerMount, fl.playerAnimFrame, fl.playerAnimFlags, fl.stanceEvents,
                          fl.riderLayerCalls);
            seamReport += road;
            char lod[300];
            std::snprintf(lod, sizeof(lod),
                          "the ground query's level of detail (answer bit 11): %zu of %zu hit(s) on the "
                          "FINE mesh; the player's: %zu of %zu\n",
                          totals.fineHits, totals.hits, totals.playerFineHits, totals.playerHits);
            seamReport += lod;
            if (session.Sounds().EngineMode()) {
                const auto& snd = session.Sounds();
                int sounding = 0;
                for (int ch = 0; ch < rr::audio::SpuVoices::kChannels; ++ch)
                    if (snd.Spu()->State(ch).on) ++sounding;
                char note[512];
                std::snprintf(note, sizeof(note),
                              "the PORTED engine note: %zu game frame(s) through EngineNote/RoadNote, %zu "
                              "vblank(s) through AudioVSyncTick/SoundService, %zu SPU register write(s), "
                              "%u key-on(s), %d voice(s) sounding at the end, %zu effect spawn(s) not run; "
                              "layer 0's pitch register while racing %u..%u (0x%04X..0x%04X); SPU model "
                              "peak %d of 32767; %s\n",
                              snd.EngineFrames(), snd.VSyncs(), snd.SpuWrites(), snd.Spu()->KeyOns(),
                              sounding, snd.EffectsNotRun(), totals.minPitch == 0xFFFF ? 0u : totals.minPitch,
                              totals.maxPitch, totals.minPitch == 0xFFFF ? 0u : totals.minPitch,
                              totals.maxPitch, snd.Spu()->Peak(),
                              snd.EngineFault().empty() ? "no fault" : snd.EngineFault().c_str());
                seamReport += note;
                { // the service's clock against the console's (one vblank per 5 ticks = 60 a second)
                    const double seconds = static_cast<double>(soundTicks) / 300.0;
                    const double perSecond = seconds > 0.0 ? static_cast<double>(snd.VSyncs()) / seconds : 0.0;
                    const bool ok = soundTicks >= 300 && std::fabs(perSecond - 60.0) <= 60.0 * 5.0 / static_cast<double>(soundTicks);
                    char pace[512];
                    std::snprintf(pace, sizeof(pace),
                                  "the sound service's clock: %zu vblank(s) through AudioVSyncTick/"
                                  "SoundService (%zu asked by the loop%s) in %lld tick(s) of game time (%.3f s) = %.2f "
                                  "per simulated second; the console's VSync callback SLUS 0x8001B700: one per 5 ticks = "
                                  "60 -> %s\n",
                                  snd.VSyncs(), hostVsyncs, vsyncTwoPerFrame ? ", RRJB_SOUND_VSYNC=frame: two a frame" : "",
                                  soundTicks, seconds, perSecond, ok ? "PASS" : "FAIL");
                    seamReport += pace;
                }
                // saturation measured where it happens (the voice SUM leaving 16 bits,
                // before the main volume), and the object slots AudioFrame fills in the world mode
                int slotVoices = 0;
                const int slots = snd.ObjectSlotsBusy(&slotVoices);
                std::snprintf(note, sizeof(note),
                              "the sound mix: %s; %llu of %llu output sample(s) saturated in the "
                              "voice sum (%.4f%%), %d voice(s) audible at the end (envelope > 0, volume > 0), "
                              "player 0's object slots: %d busy, %d with a voice\n",
                              snd.Attached() ? "AudioFrame PORTED on the world" : "own arena (no AudioFrame)",
                              static_cast<unsigned long long>(snd.Spu()->Clipped()),
                              static_cast<unsigned long long>(snd.Spu()->Rendered()),
                              snd.Spu()->Rendered() ? 100.0 * static_cast<double>(snd.Spu()->Clipped()) /
                                                          static_cast<double>(snd.Spu()->Rendered())
                                                    : 0.0,
                              snd.Spu()->SoundingVoices(), slots, slotVoices);
                seamReport += note;
                seamReport += rr::game::MpSoundLine(snd, session.Players()); // mp_arena.h
            }
        }
        // Sound, counted at every step of the chain, so "the game makes a sound" is a measurement
        // rather than a claim: how many calls the SIMULATION made into the ported emitter, how
        // many of those reached the allocator and keyed a voice on, how many the service pass
        // programmed with real samples, and how many the MIXER actually accepted and is playing.
        char audio[512];
        std::snprintf(audio, sizeof(audio),
                      "sound: %zu call(s) into the PORTED emitter SLUS 0x80017BA0, %zu of them "
                      "keyed a voice on, %zu voice(s) programmed from DATA\\RASHNZ_E.DAT, %zu "
                      "accepted by the mixer; %zu distinct sample(s) decoded; device %s rendered "
                      "%zu frame(s) with peak amplitude %d of 32767 (%zu sample(s) at the 16-bit rail)\n",
                      session.Sounds().Requested(), session.Sounds().Allocated(),
                      session.Sounds().Programmed(), sfxPlayed, session.Sounds().LoadedSounds(),
                      audioDevice ? audioDevice->BackendName() : "none",
                      sink.framesRendered.load(), sink.Peak(), sink.samplesAtRail.load());
        seamReport += audio;
        {   // the riders' voices (speech_session.cpp)
            char sp[320];
            std::snprintf(sp, sizeof(sp),
                          "voices: %d speech slot(s) loaded from DATA\\AUDTAUNT.STR, %zu RiderSpeech call(s) (%zu L2 "
                          "taunt(s)), %zu line(s) started, %zu grudge(s) written, %zu provocation(s) pushed; music went on to %d next "
                          "track(s), now track %d%s%s\n",
                          session.Sounds().SpeechSlotsLoaded(), session.Sounds().SpeechCalls(), session.Taunts(),
                          session.Sounds().SpeechLines(), session.Grudges(), session.Sounds().SpeechPushes(),
                          session.Sounds().MusicTracksChained(), session.Sounds().MusicTrack(),
                          session.Sounds().SpeechFault().empty() ? "" : "; speech stopped: ",
                          session.Sounds().SpeechFault().c_str());
            seamReport += sp;
            seamReport += "voices: the speech slots " + session.Sounds().SpeechTable() + "\n";
            seamReport += "spu heap: " + session.Sounds().SpuHeapLine() + "\n"; // sound_loader.cpp (sim\spu_heap.h)
        }
        if (!soundWav.empty()) {
            // the whole run's sound, rendered in step with the game (no device, no
            // wall clock), measured and written as a 16-bit stereo WAV
            const auto& spu = *session.Sounds().Spu();
            int32_t peak = 0;
            uint64_t sum2 = 0;
            for (int16_t v : soundRecord) {
                const int32_t a = v < 0 ? -static_cast<int32_t>(v) : v;
                peak = std::max(peak, a);
                sum2 += static_cast<uint64_t>(static_cast<int64_t>(v) * v);
            }
            const double rms = soundRecord.empty() ? 0.0 : std::sqrt(static_cast<double>(sum2) / static_cast<double>(soundRecord.size()));
            char w[400];
            std::snprintf(w, sizeof(w),
                          "sound record: %zu frame(s) at 44100 Hz (%.2f s), peak %d, RMS %.0f, %llu of %llu sample(s) "
                          "saturated in the voice sum (%.4f%%), %d voice(s) audible at the end, music track %d "
                          "(%llu unit(s) streamed); %s\n",
                          soundRecord.size() / 2, static_cast<double>(soundRecord.size() / 2) / 44100.0, peak, rms,
                          static_cast<unsigned long long>(spu.Clipped()), static_cast<unsigned long long>(spu.Rendered()),
                          spu.Rendered() ? 100.0 * static_cast<double>(spu.Clipped()) / static_cast<double>(spu.Rendered()) : 0.0,
                          spu.SoundingVoices(), session.Sounds().MusicTrack(),
                          static_cast<unsigned long long>(session.Sounds().MusicUnits()), soundWav.c_str());
            seamReport += w;
            if (FILE* out = std::fopen(soundWav.c_str(), "wb")) {
                const uint32_t data = static_cast<uint32_t>(soundRecord.size() * 2u);
                auto u32 = [out](uint32_t v) { std::fwrite(&v, 4, 1, out); };
                auto u16 = [out](uint16_t v) { std::fwrite(&v, 2, 1, out); };
                std::fwrite("RIFF", 1, 4, out); u32(36u + data); std::fwrite("WAVEfmt ", 1, 8, out);
                u32(16); u16(1); u16(2); u32(44100); u32(44100u * 4u); u16(4); u16(16);
                std::fwrite("data", 1, 4, out); u32(data);
                std::fwrite(soundRecord.data(), 2, soundRecord.size(), out);
                std::fclose(out);
            }
        }
        std::fputs(seamReport.c_str(), stdout);
        if (!logPath.empty()) {
            log += seamReport;
            if (FILE* out = std::fopen(logPath.c_str(), "wb")) {
                std::fwrite(log.data(), 1, log.size(), out);
                std::fclose(out);
                std::printf("frame log: %s\n", logPath.c_str());
            }
        }
        if (audioDevice) audioDevice->Stop();
        if (restartRace) {
            std::printf("pause: RESTART GAME - the race again (state 5: main 0x8001247C -> 0x800122FC)\n");
            return rr::game::kRaceRestart;
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}

// rrgame <disc.bin> opens the front end; any other command line goes straight to RaceMain.
// The menu's RESTART (pause_product.h): RaceMain again with the same arguments while it asks for it.
int RaceMainLoop(int argc, char** argv) {
    for (rr::game::RaceAttempt() = 0;; ++rr::game::RaceAttempt()) {
        const int rc = RaceMain(argc, argv);
        if (rc != rr::game::kRaceRestart) return rc;
    }
}

// `main` (the disc lookup, the host): main_desktop.cpp on Windows, main_android.cpp's android_main on the Quest.
