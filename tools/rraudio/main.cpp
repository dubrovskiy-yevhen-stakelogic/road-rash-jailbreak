// rraudio - decode and play the audio of Road Rash: Jailbreak (USA, SLUS_01053).
//
// Reads the player's own disc extract; writes nothing back to it. `decode` is the cross-check surface:
// when the output path is a directory it writes exactly the file names tools\scout\audio.py writes, so
// the two decoders can be diffed sample-for-sample.
#include "game/audio/mixer.h"
#include "game/audio/sound_runtime.h"
#include "platform/audio_device.h"
#include "rrformats/audio.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

int Usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  rraudio scan <dir|file>...                        inventory of every audio file\n"
                 "  rraudio list <file> [--limit N]                   chunks / units / records\n"
                 "  rraudio decode <file> <out.wav|outdir> [opts]     -> 16-bit PCM WAV\n"
                 "  rraudio play <file> [opts]                        real playback (winmm)\n"
                 "  rraudio mix <out.wav> <file>... [opts]            offline mix, no device\n"
                 "  rraudio tracks <SLUS_010.53>                      ALBUM.ALB track table\n"
                 "  rraudio engine-probe <disc extract> --ram <ram.bin> [--seconds N] [--wav out.wav]\n"
                 "                    [--play]   the PORTED engine note through the product's sound\n"
                 "                    runtime, driven by a synthetic rev sweep on a captured bike\n"
                 "  rraudio spu-test                                  the SPU voice model's rules\n"
                 "                                                   , exit 0 = all hold\n"
                 "\n"
                 "options:\n"
                 "  --start N     .ALB: first 16 KiB unit; AUDTAUNT: first record\n"
                 "  --track N     ALBUM.ALB/FEALBUM.ALB: one track (needs --exe for ALBUM.ALB)\n"
                 "  --exe PATH    the game's SLUS_010.53, which carries the ALBUM.ALB track table\n"
                 "  --units N     .ALB: number of units (0 = to the end)\n"
                 "  --record N    AUDTAUNT: one record\n"
                 "  --sample N    AUDTAUNT: which sample of the record (0 or 1; default: all of them)\n"
                 "  --all         AUDTAUNT: every record (decode: one WAV each, into a directory)\n"
                 "  --probe-compat  AUDTAUNT decode: reproduce the span and the file names of\n"
                 "                  tools\\scout\\audio.py, for the sample-for-sample cross-check\n"
                 "  --rate N      play: output device rate (default 44100; the source is 22050)\n"
                 "  --pitch N     play: Q12 pitch, 4096 = the source's own rate\n"
                 "  --volume N    play: Q15 volume, 32768 = unity\n"
                 "  --pan N       play: Q15 pan, -32768 left .. +32768 right\n"
                 "  --seconds N   play: stop after N seconds\n"
                 "  --loop        play: loop the source\n");
    return 2;
}

struct Options {
    size_t start = 0;
    size_t units = 0;
    long record = -1;
    long sample = -1;
    long track = -1;
    std::string exe;
    bool all = false;
    bool loop = false;
    bool probeCompat = false;
    int rate = 44100;
    int pitch = rr::audio::kPitchUnit;
    int volume = rr::audio::kVolumeUnit;
    int pan = 0;
    double seconds = 0.0;
    int limit = 12;
    std::string ram;
    std::string wav;
    bool play = false;
};

std::vector<uint8_t> ReadWholeFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (size && !in.read(reinterpret_cast<char*>(data.data()), size))
        throw std::runtime_error("short read on " + path.string());
    return data;
}

void WriteWav(const fs::path& path, const rr::PcmBuffer& pcm) {
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    const uint32_t dataBytes = static_cast<uint32_t>(pcm.samples.size() * sizeof(int16_t));
    const uint16_t channels = static_cast<uint16_t>(pcm.channels);
    const uint32_t rate = static_cast<uint32_t>(pcm.sampleRate);
    const uint16_t blockAlign = static_cast<uint16_t>(channels * 2);
    const uint32_t byteRate = rate * blockAlign;
    auto u32 = [&out](uint32_t v) {
        const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                              static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
        out.write(reinterpret_cast<const char*>(b), 4);
    };
    auto u16 = [&out](uint16_t v) {
        const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
        out.write(reinterpret_cast<const char*>(b), 2);
    };
    out.write("RIFF", 4);
    u32(36 + dataBytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(channels);
    u32(rate);
    u32(byteRate);
    u16(blockAlign);
    u16(16);
    out.write("data", 4);
    u32(dataBytes);
    out.write(reinterpret_cast<const char*>(pcm.samples.data()), dataBytes);
    if (!out) throw std::runtime_error("write failed on " + path.string());
}

std::string Stem(const fs::path& path) { return path.stem().string(); }

bool LooksLikeWavPath(const fs::path& path) {
    std::string ext = path.extension().string();
    for (char& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return ext == ".WAV";
}

// ------------------------------------------------------------------------------------------------
// Streaming source for .ALB, so `play ALBUM.ALB` does not need 222 MB of decoded PCM.
// ------------------------------------------------------------------------------------------------

class AlbFileSource final : public rr::audio::PcmSource {
public:
    AlbFileSource(const fs::path& path, size_t startUnit, size_t units)
        : file_(path, std::ios::binary), startUnit_(startUnit) {
        if (!file_) throw std::runtime_error("cannot open " + path.string());
        const size_t total = rr::AlbUnitCount(static_cast<size_t>(fs::file_size(path)));
        if (startUnit_ > total) throw std::runtime_error(".ALB start unit past the end");
        unitCount_ = units == 0 ? total - startUnit_ : std::min(units, total - startUnit_);
        Rewind();
    }

    int Channels() const override { return 2; }
    int SourceRate() const override { return rr::kAlbSampleRate; }
    bool CanRewind() const override { return true; }

    void Rewind() override {
        unitsDone_ = 0;
        pcm_.clear();
        pos_ = 0;
        left_ = rr::AdpcmState();
        right_ = rr::AdpcmState();
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(startUnit_ * rr::kAlbUnitSize), std::ios::beg);
    }

    size_t Read(int16_t* dst, size_t frames) override {
        size_t done = 0;
        while (done < frames) {
            if (pos_ >= pcm_.size()) {
                if (!DecodeNextUnit()) break;
            }
            const size_t available = (pcm_.size() - pos_) / 2;
            const size_t n = std::min(available, frames - done);
            std::memcpy(dst + done * 2, pcm_.data() + pos_, n * 2 * sizeof(int16_t));
            pos_ += n * 2;
            done += n;
        }
        return done;
    }

    size_t Units() const { return unitCount_; }

private:
    bool DecodeNextUnit() {
        if (unitsDone_ >= unitCount_) return false;
        uint8_t unit[rr::kAlbUnitSize];
        if (!file_.read(reinterpret_cast<char*>(unit), rr::kAlbUnitSize)) return false;
        pcm_.clear();
        pos_ = 0;
        rr::DecodeAlbUnit(std::span<const uint8_t>(unit, rr::kAlbUnitSize), left_, right_, pcm_);
        ++unitsDone_;
        return !pcm_.empty();
    }

    std::ifstream file_;
    size_t startUnit_ = 0;
    size_t unitCount_ = 0;
    size_t unitsDone_ = 0;
    std::vector<int16_t> pcm_;
    size_t pos_ = 0;
    rr::AdpcmState left_, right_;
};

class MixerSink final : public rr::platform::AudioSink {
public:
    explicit MixerSink(rr::audio::Mixer& mixer) : mixer_(mixer) {}
    void Render(int16_t* out, size_t frames) override { mixer_.Mix(out, frames); }

private:
    rr::audio::Mixer& mixer_;
};

// ------------------------------------------------------------------------------------------------
// commands
// ------------------------------------------------------------------------------------------------

void ScanOne(const fs::path& path) {
    const std::vector<uint8_t> data = ReadWholeFile(path);
    const std::span<const uint8_t> file(data);
    const rr::AudioFamily family = rr::DetectAudioFamily(path.string(), file);
    const std::string name = path.filename().string();
    switch (family) {
        case rr::AudioFamily::Wve: {
            rr::WveAudioInfo info;
            const rr::PcmBuffer pcm = rr::DecodeWve(file, &info);
            std::printf("%-14s %10zu %-5s %8.2fs %10zu  %zu au chunks, %zu video frames, "
                        "%.3f samples/frame, counterMismatch=%u, clipped L=%llu R=%llu\n",
                        name.c_str(), data.size(), "wve", pcm.Seconds(), pcm.Frames(), info.chunks,
                        info.videoFrames,
                        info.videoFrames ? static_cast<double>(pcm.Frames()) / static_cast<double>(info.videoFrames) : 0.0,
                        info.counterMismatch,
                        static_cast<unsigned long long>(info.left.clipped),
                        static_cast<unsigned long long>(info.right.clipped));
            break;
        }
        case rr::AudioFamily::Alb: {
            const size_t units = rr::AlbUnitCount(data.size());
            const size_t frames = units * rr::kAlbSamplesPerUnit;
            const size_t ends = rr::AlbLoopEndBlocks(file).size();
            std::printf("%-14s %10zu %-5s %8.2fs %10zu  %zu units of 16 KiB, %zu loop-end blocks "
                        "@ %d Hz\n",
                        name.c_str(), data.size(), "alb",
                        static_cast<double>(frames) / rr::kAlbSampleRate, frames, units, ends,
                        rr::kAlbSampleRate);
            break;
        }
        case rr::AudioFamily::Taunt: {
            const std::vector<rr::TauntRecord> records = rr::TauntRecords(file);
            size_t frames = 0, samples = 0, single = 0;
            double seconds = 0.0;
            for (const rr::TauntRecord& r : records) {
                if (r.samples.size() == 1) ++single;
                for (const rr::TauntSample& s : r.samples) {
                    const size_t n = s.blocks * rr::kAdpcmSamplesPerBlock;
                    frames += n;
                    seconds += static_cast<double>(n) / s.sampleRate;
                    ++samples;
                }
            }
            std::printf("%-14s %10zu %-5s %8.2fs %10zu  %zu records -> %zu samples "
                        "(%zu records hold only one)\n",
                        name.c_str(), data.size(), "taunt", seconds, frames, records.size(), samples,
                        single);
            break;
        }
        default:
            std::printf("%-14s %10zu %-5s %8s %10s  not an audio container\n", name.c_str(),
                        data.size(), "?", "-", "-");
            break;
    }
}

bool IsAudioName(const fs::path& path) {
    std::string upper = path.filename().string();
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return upper.size() > 4 &&
           (upper.compare(upper.size() - 4, 4, ".WVE") == 0 ||
            upper.compare(upper.size() - 4, 4, ".ALB") == 0 || upper == "AUDTAUNT.STR");
}

int CmdScan(const std::vector<std::string>& paths) {
    std::printf("%-14s %10s %-5s %8s %10s  %s\n", "file", "bytes", "fam", "seconds", "frames", "notes");
    for (const std::string& p : paths) {
        const fs::path path(p);
        if (fs::is_directory(path)) {
            std::vector<fs::path> files;
            for (const fs::directory_entry& e : fs::directory_iterator(path))
                if (e.is_regular_file() && IsAudioName(e.path())) files.push_back(e.path());
            std::sort(files.begin(), files.end());
            for (const fs::path& f : files) ScanOne(f);
        } else {
            ScanOne(path);
        }
    }
    return 0;
}

int CmdList(const fs::path& path, const Options& opt) {
    const std::vector<uint8_t> data = ReadWholeFile(path);
    const std::span<const uint8_t> file(data);
    const rr::AudioFamily family = rr::DetectAudioFamily(path.string(), file);
    std::printf("=== %s (%zu bytes) family=%s ===\n", path.string().c_str(), data.size(),
                rr::AudioFamilyName(family));
    switch (family) {
        case rr::AudioFamily::Wve: {
            const std::vector<rr::WveAudioChunk> chunks = rr::WveAudioChunks(file);
            std::printf("  %zu audio chunks\n", chunks.size());
            bool havePrev = false;
            uint32_t prev = 0;
            for (size_t i = 0; i < chunks.size() && i < static_cast<size_t>(opt.limit); ++i) {
                const rr::WveAudioChunk& c = chunks[i];
                std::printf("  0x%08zX %s size=%-6u payload=%-6zu blocks=%-5zu half=%-5zu rem=%-2zu "
                            "first=%-9u%s +0x0C=%08X\n",
                            c.offset, c.tag.c_str(), c.size, c.payloadSize, c.blocks, c.halfBlocks,
                            c.payloadSize - c.blocks * rr::kWveBlockSize, c.firstSample,
                            havePrev ? (" (+" + std::to_string(c.firstSample - prev) + ")").c_str() : "",
                            c.field0C);
                prev = c.firstSample;
                havePrev = true;
            }
            if (chunks.size() > static_cast<size_t>(opt.limit))
                std::printf("  ... %zu more\n", chunks.size() - static_cast<size_t>(opt.limit));
            break;
        }
        case rr::AudioFamily::Alb: {
            const size_t units = rr::AlbUnitCount(data.size());
            std::printf("  %zu units of 16 KiB = %zu blocks, %zu frames/channel, %.2f s @ %d Hz\n",
                        units, data.size() / rr::kSpuBlockSize, units * rr::kAlbSamplesPerUnit,
                        static_cast<double>(units * rr::kAlbSamplesPerUnit) / rr::kAlbSampleRate,
                        rr::kAlbSampleRate);
            const std::vector<size_t> ends = rr::AlbLoopEndBlocks(file);
            std::printf("  %zu loop-end flagged blocks", ends.size());
            for (size_t i = 0; i < ends.size() && i < static_cast<size_t>(opt.limit); ++i)
                std::printf("%s0x%zX", i ? ", " : ": ", ends[i]);
            std::printf("\n");
            break;
        }
        case rr::AudioFamily::Taunt: {
            const std::vector<rr::TauntRecord> records = rr::TauntRecords(file);
            std::printf("  %zu records of 0x%zX bytes\n", records.size(), rr::kTauntRecordSize);
            for (size_t i = 0; i < records.size() && i < static_cast<size_t>(opt.limit); ++i) {
                const rr::TauntRecord& r = records[i];
                std::printf("    #%-4zu @0x%06zX sound=%-3u voice=0x%02X id20=%-3u id24=%-3u "
                            "vol=0x%04X second=%-6u %zu sample(s)\n",
                            r.index, r.offset, r.soundId, r.voiceId, r.id20, r.id24, r.volume,
                            r.secondSampleOffset, r.samples.size());
                for (size_t k = 0; k < r.samples.size(); ++k) {
                    const rr::TauntSample& s = r.samples[k];
                    std::printf("        [%zu] id=%-3u @0x%06zX blocks=%-5zu pitch=0x%04X "
                                "%5d Hz  %.3f s\n",
                                k, s.id, s.offset - r.offset, s.blocks, s.pitch, s.sampleRate,
                                static_cast<double>(s.blocks * rr::kAdpcmSamplesPerBlock) / s.sampleRate);
                }
            }
            if (records.size() > static_cast<size_t>(opt.limit))
                std::printf("    ... %zu more\n", records.size() - static_cast<size_t>(opt.limit));
            break;
        }
        default:
            std::printf("  not an audio container\n");
            return 1;
    }
    return 0;
}

std::string Upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// `--track N` is just a (start unit, unit count) pair, resolved from the table in the game's EXE for
// ALBUM.ALB / ALBUM2.ALB and from the fixed 0x70000 stride for FEALBUM.ALB.
Options ApplyTrack(const fs::path& path, Options opt) {
    if (opt.track < 0) return opt;
    const std::string name = Upper(path.filename().string());
    if (name == "FEALBUM.ALB") {
        if (static_cast<size_t>(opt.track) >= rr::kFeAlbumTrackCount)
            throw std::runtime_error("FEALBUM.ALB has " + std::to_string(rr::kFeAlbumTrackCount) +
                                     " tracks");
        opt.start = static_cast<size_t>(opt.track) * (rr::kFeAlbumTrackSize / rr::kAlbUnitSize);
        opt.units = rr::kFeAlbumTrackSize / rr::kAlbUnitSize;
        return opt;
    }
    if (opt.exe.empty())
        throw std::runtime_error("--track on " + name +
                                 " needs --exe <path to SLUS_010.53>: the track table lives in the "
                                 "game's EXE, not in the .ALB");
    const std::vector<rr::AlbTrack> tracks = rr::ParseAlbumTrackTable(ReadWholeFile(opt.exe));
    if (static_cast<size_t>(opt.track) >= tracks.size())
        throw std::runtime_error("the album has " + std::to_string(tracks.size()) + " tracks");
    opt.start = tracks[static_cast<size_t>(opt.track)].StartUnit();
    opt.units = tracks[static_cast<size_t>(opt.track)].Units();
    return opt;
}

int CmdTracks(const fs::path& exe) {
    const std::vector<rr::AlbTrack> tracks = rr::ParseAlbumTrackTable(ReadWholeFile(exe));
    std::printf("ALBUM.ALB track table from %s (+0x%zX): %zu tracks\n", exe.string().c_str(),
                rr::kAlbumTrackTableFileOffset, tracks.size());
    std::printf("%3s %12s %12s %8s %8s %10s %10s %s\n", "n", "start", "length", "unit", "units",
                "start s", "length s", "flags");
    double at = 0.0;
    uint64_t total = 0;
    for (const rr::AlbTrack& t : tracks) {
        const double seconds =
            static_cast<double>(t.Units() * rr::kAlbSamplesPerUnit) / rr::kAlbSampleRate;
        std::printf("%3zu   0x%08X   0x%08X %8zu %8zu %10.2f %10.2f 0x%X\n", t.index, t.start,
                    t.length, t.StartUnit(), t.Units(), at, seconds, t.flags);
        at += seconds;
        total += t.length;
    }
    std::printf("total %llu bytes = %.2f s at %d Hz\n", static_cast<unsigned long long>(total), at,
                rr::kAlbSampleRate);
    return 0;
}

int CmdDecode(const fs::path& path, const fs::path& out, const Options& optIn) {
    const Options opt = ApplyTrack(path, optIn);
    const std::vector<uint8_t> data = ReadWholeFile(path);
    const std::span<const uint8_t> file(data);
    const rr::AudioFamily family = rr::DetectAudioFamily(path.string(), file);
    const bool outIsDir = !LooksLikeWavPath(out);
    const std::string stem = Stem(path);

    switch (family) {
        case rr::AudioFamily::Wve: {
            rr::WveAudioInfo info;
            const rr::PcmBuffer pcm = rr::DecodeWve(file, &info);
            const fs::path target = outIsDir ? out / (stem + ".wav") : out;
            WriteWav(target, pcm);
            std::printf("%s  stereo %d Hz  %zu frames  %.2f s  chunks=%zu counterMismatch=%u "
                        "clipped L=%llu R=%llu\n",
                        target.string().c_str(), pcm.sampleRate, pcm.Frames(), pcm.Seconds(),
                        info.chunks, info.counterMismatch,
                        static_cast<unsigned long long>(info.left.clipped),
                        static_cast<unsigned long long>(info.right.clipped));
            return 0;
        }
        case rr::AudioFamily::Alb: {
            rr::AdpcmState left, right;
            const rr::PcmBuffer pcm = rr::DecodeAlb(file, opt.start, opt.units, &left, &right);
            const size_t units = opt.units == 0 ? rr::AlbUnitCount(data.size()) - opt.start
                                                : std::min(opt.units, rr::AlbUnitCount(data.size()) - opt.start);
            char name[256];
            std::snprintf(name, sizeof(name), "%s_u%05zu_n%zu.wav", stem.c_str(), opt.start, units);
            const fs::path target = outIsDir ? out / name : out;
            WriteWav(target, pcm);
            std::printf("%s  stereo %d Hz  units %zu..%zu  %zu frames  %.2f s  clipped L=%llu R=%llu\n",
                        target.string().c_str(), pcm.sampleRate, opt.start,
                        units ? opt.start + units - 1 : opt.start, pcm.Frames(), pcm.Seconds(),
                        static_cast<unsigned long long>(left.clipped),
                        static_cast<unsigned long long>(right.clipped));
            return 0;
        }
        case rr::AudioFamily::Taunt: {
            const std::vector<rr::TauntRecord> records = rr::TauntRecords(file);
            std::vector<size_t> selected;
            if (opt.record >= 0) {
                if (static_cast<size_t>(opt.record) >= records.size())
                    throw std::runtime_error("record index past the end");
                selected.push_back(static_cast<size_t>(opt.record));
            } else if (opt.all) {
                const size_t count = opt.units ? opt.units : records.size();
                for (size_t i = opt.start; i < records.size() && i < opt.start + count; ++i)
                    selected.push_back(i);
            } else {
                selected.push_back(opt.start);
            }
            if (!outIsDir && selected.size() != 1)
                throw std::runtime_error("AUDTAUNT: several records need a directory as the output");
            for (size_t index : selected) {
                const rr::TauntRecord& r = records[index];
                char name[256];
                if (opt.probeCompat) {
                    // Reproduce tools\scout\audio.py exactly: its span, its names, and its habit of
                    // skipping the records whose +0x5C field is 0.
                    if (!r.secondSampleOffset) continue;
                    rr::AdpcmState state;
                    const rr::PcmBuffer pcm = rr::DecodeTauntProbeSpan(file, r, &state);
                    std::snprintf(name, sizeof(name), "%s_%03zu_s%02u_v%02X.wav", stem.c_str(),
                                  r.index, r.soundId, r.voiceId);
                    const fs::path target = outIsDir ? out / name : out;
                    WriteWav(target, pcm);
                    std::printf("%s  mono %d Hz  %zu frames  %.3f s  clipped=%llu\n",
                                target.string().c_str(), pcm.sampleRate, pcm.Frames(), pcm.Seconds(),
                                static_cast<unsigned long long>(state.clipped));
                    continue;
                }
                for (size_t k = 0; k < r.samples.size(); ++k) {
                    if (opt.sample >= 0 && static_cast<size_t>(opt.sample) != k) continue;
                    const rr::TauntSample& s = r.samples[k];
                    rr::AdpcmState state;
                    const rr::PcmBuffer pcm = rr::DecodeTaunt(file, s, &state);
                    std::snprintf(name, sizeof(name), "%s_%03zu_%zu_id%02u_v%02X.wav", stem.c_str(),
                                  r.index, k, s.id, r.voiceId);
                    const fs::path target = outIsDir ? out / name : out;
                    WriteWav(target, pcm);
                    std::printf("%s  mono %d Hz  %zu frames  %.3f s  clipped=%llu\n",
                                target.string().c_str(), pcm.sampleRate, pcm.Frames(), pcm.Seconds(),
                                static_cast<unsigned long long>(state.clipped));
                }
            }
            return 0;
        }
        default:
            std::fprintf(stderr, "%s: not an audio container\n", path.string().c_str());
            return 1;
    }
}

// Builds a playable source out of any of the three families. .ALB is streamed off disk (decoding all
// of ALBUM.ALB would be 222 MB of PCM); the other two fit in memory.
std::shared_ptr<rr::audio::PcmSource> MakeSource(const fs::path& path, const Options& optIn,
                                                 std::string& what) {
    const Options opt = ApplyTrack(path, optIn);
    std::shared_ptr<rr::audio::PcmSource> source;

    const std::vector<uint8_t> probe = [&] {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("cannot open " + path.string());
        std::vector<uint8_t> head(4, 0);
        in.read(reinterpret_cast<char*>(head.data()), 4);
        return head;
    }();
    const rr::AudioFamily family = rr::DetectAudioFamily(path.string(), probe);

    if (family == rr::AudioFamily::Alb) {
        auto alb = std::make_shared<AlbFileSource>(path, opt.start, opt.units);
        what = "streaming " + std::to_string(alb->Units()) + " units from unit " +
               std::to_string(opt.start);
        source = alb;
    } else if (family == rr::AudioFamily::Wve) {
        const std::vector<uint8_t> data = ReadWholeFile(path);
        rr::WveAudioInfo info;
        rr::PcmBuffer pcm = rr::DecodeWve(data, &info);
        what = std::to_string(info.chunks) + " au chunks, " + std::to_string(pcm.Frames()) + " frames";
        source = std::make_shared<rr::audio::MemorySource>(std::move(pcm.samples), 2, pcm.sampleRate);
    } else if (family == rr::AudioFamily::Taunt) {
        const std::vector<uint8_t> data = ReadWholeFile(path);
        const std::vector<rr::TauntRecord> records = rr::TauntRecords(data);
        std::vector<size_t> selected;
        if (opt.record >= 0) selected.push_back(static_cast<size_t>(opt.record));
        else if (opt.all) {
            const size_t count = opt.units ? opt.units : records.size();
            for (size_t i = opt.start; i < records.size() && i < opt.start + count; ++i)
                selected.push_back(i);
        } else selected.push_back(opt.start);
        std::vector<int16_t> joined;
        int rate = 0;
        size_t count = 0;
        for (size_t index : selected) {
            if (index >= records.size()) throw std::runtime_error("record index past the end");
            const rr::TauntRecord& r = records[index];
            for (size_t k = 0; k < r.samples.size(); ++k) {
                if (opt.sample >= 0 && static_cast<size_t>(opt.sample) != k) continue;
                const rr::TauntSample& s = r.samples[k];
                // The two rates in this file never appear in the same playback request in practice;
                // refuse rather than silently play one of them at the wrong speed.
                if (rate && rate != s.sampleRate)
                    throw std::runtime_error("AUDTAUNT: the selected samples do not share one rate (" +
                                             std::to_string(rate) + " vs " +
                                             std::to_string(s.sampleRate) + ")");
                rate = s.sampleRate;
                const rr::PcmBuffer pcm = rr::DecodeTaunt(data, s);
                joined.insert(joined.end(), pcm.samples.begin(), pcm.samples.end());
                joined.insert(joined.end(), static_cast<size_t>(rate / 4), static_cast<int16_t>(0));
                ++count;
            }
        }
        if (!rate) throw std::runtime_error("AUDTAUNT: nothing selected");
        what = std::to_string(count) + " sample(s), " + std::to_string(joined.size()) + " frames @ " +
               std::to_string(rate) + " Hz";
        source = std::make_shared<rr::audio::MemorySource>(std::move(joined), 1, rate);
    } else {
        throw std::runtime_error(path.string() + ": not an audio container");
    }
    return source;
}

int CmdPlay(const fs::path& path, const Options& opt) {
    std::string what;
    const std::shared_ptr<rr::audio::PcmSource> source = MakeSource(path, opt, what);

    rr::audio::Mixer mixer(opt.rate, 8);
    rr::audio::VoiceDesc desc;
    desc.source = source;
    desc.pitch = opt.pitch;
    desc.volume = opt.volume;
    desc.pan = opt.pan;
    desc.loop = opt.loop;
    const rr::audio::VoiceId id = mixer.Play(desc);
    if (!id) {
        std::fprintf(stderr, "mixer refused the source (empty?)\n");
        return 1;
    }

    MixerSink sink(mixer);
    std::unique_ptr<rr::platform::AudioDevice> device =
        rr::platform::OpenAudioDevice(sink, opt.rate, 1024);
    std::printf("playing %s (%s) via %s at %d Hz, source %d Hz, pitch %d/4096\n",
                path.filename().string().c_str(), what.c_str(), device->BackendName(), opt.rate,
                source->SourceRate(), opt.pitch);
    device->Start();

    const auto began = std::chrono::steady_clock::now();
    while (mixer.IsPlaying(id)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        if (opt.seconds > 0.0 && elapsed >= opt.seconds) break;
    }
    // Let the buffers already handed to the driver drain before the device closes.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    device->Stop();
    std::printf("done\n");
    return 0;
}

// Offline mix: the same Mixer the game will use, driven without any audio device. Useful as an
// artifact (one WAV holding several sources at once) and as a determinism check - two runs of the
// same command have to produce byte-identical files.
int CmdMix(const fs::path& out, const std::vector<std::string>& inputs, const Options& opt) {
    rr::audio::Mixer mixer(opt.rate, std::max<size_t>(inputs.size(), 1));
    const int spread = inputs.size() > 1 ? 2 * rr::audio::kVolumeUnit / static_cast<int>(inputs.size() - 1) : 0;
    for (size_t i = 0; i < inputs.size(); ++i) {
        std::string what;
        rr::audio::VoiceDesc desc;
        desc.source = MakeSource(inputs[i], opt, what);
        desc.pitch = opt.pitch;
        desc.volume = opt.volume / static_cast<int>(inputs.size());
        desc.pan = inputs.size() > 1
                       ? std::clamp(-rr::audio::kVolumeUnit + spread * static_cast<int>(i),
                                    -rr::audio::kVolumeUnit, rr::audio::kVolumeUnit)
                       : opt.pan;
        desc.loop = opt.loop;
        if (!mixer.Play(desc)) throw std::runtime_error("mixer refused " + inputs[i]);
        std::printf("voice %zu: %s (%s) pan %d volume %d\n", i + 1, inputs[i].c_str(), what.c_str(),
                    desc.pan, desc.volume);
    }

    rr::PcmBuffer pcm;
    pcm.channels = 2;
    pcm.sampleRate = opt.rate;
    const double seconds = opt.seconds > 0.0 ? opt.seconds : 10.0;
    const size_t total = static_cast<size_t>(seconds * opt.rate);
    constexpr size_t kBlock = 1024;
    std::vector<int16_t> block(kBlock * 2);
    for (size_t done = 0; done < total && mixer.ActiveVoices(); done += kBlock) {
        const size_t n = std::min(kBlock, total - done);
        mixer.Mix(block.data(), n);
        pcm.samples.insert(pcm.samples.end(), block.begin(), block.begin() + static_cast<long>(n * 2));
    }
    WriteWav(out, pcm);
    std::printf("%s  stereo %d Hz  %zu frames  %.2f s\n", out.string().c_str(), pcm.sampleRate,
                pcm.Frames(), pcm.Seconds());
    return 0;
}

// ---------------------------------------------------------------- engine-probe
//
// Measures, rather than claims, that the product's sound runtime makes an engine note: the ported
// EngineSetup / EngineNote / RoadNote / AudioVSyncTick / SoundService of src\game\sim\sound_engine.h
// run in `rr::game::SoundRuntime`'s arena on the player's own SLUS_010.53 and RASHNZ_E.DAT, every SPU
// register write goes to the SPU voice model, and the model is rendered.
//
// The INPUT is part real, part synthetic, and says so: the bike entity, the object at its +0x354
// and its stat block are the player's, lifted out of a captured RAM image; the three fields the
// note reads to decide what to play - revs +0x25C, drive demand +0x24C and gear +0x351 - are then
// driven by a scripted sweep (idle, full throttle through four gears, release), because the
// per-bike step that produces them in the game is not ported. Two vsyncs per game frame, 30 frames
// per second, as a one-player race runs on the console.
namespace engine_probe {

uint32_t Word(const std::vector<uint8_t>& ram, uint32_t a) {
    const size_t o = a & 0x1FFFFFu;
    if (o + 4 > ram.size()) return 0;
    return static_cast<uint32_t>(ram[o]) | (static_cast<uint32_t>(ram[o + 1]) << 8) |
           (static_cast<uint32_t>(ram[o + 2]) << 16) | (static_cast<uint32_t>(ram[o + 3]) << 24);
}
std::vector<uint8_t> Block(const std::vector<uint8_t>& ram, uint32_t a, size_t n) {
    std::vector<uint8_t> out(n, 0);
    for (size_t i = 0; i < n; ++i) {
        const size_t o = (a + i) & 0x1FFFFFu;
        if (o < ram.size()) out[i] = ram[o];
    }
    return out;
}
void Put32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    for (int k = 0; k < 4; ++k) b[off + static_cast<size_t>(k)] = static_cast<uint8_t>(v >> (8 * k));
}

int Run(const fs::path& disc, const Options& opt) {
    if (opt.ram.empty()) throw std::runtime_error("engine-probe needs --ram <ram.bin>");
    const std::vector<uint8_t> slus = ReadWholeFile(disc / "SLUS_010.53");
    const std::vector<uint8_t> rashnz = ReadWholeFile(disc / "DATA" / "RASHNZ_E.DAT");
    const std::vector<uint8_t> ram = ReadWholeFile(opt.ram);
    auto exeAt = [&slus](uint32_t a) { return slus.data() + (a - 0x80010000u + 0x800u); };

    rr::game::SoundRuntime rt;
    std::string error;
    if (!rt.LoadBanks(rashnz, error)) throw std::runtime_error(error);
    rt.SetExecutable(slus);
    rt.Reset();
    std::vector<int32_t> atan(20);
    std::memcpy(atan.data(), exeAt(0x8005285C), 20 * 4);
    std::vector<int16_t> sincos(8192);
    std::memcpy(sincos.data(), exeAt(0x8005624C), 8192 * 2);
    std::vector<uint8_t> surface(exeAt(0x800525C0), exeAt(0x800525C0) + 52);
    rt.SetTables(atan.data(), sincos.data(), surface.data());
    std::vector<uint8_t> gameState = Block(ram, Word(ram, 0x8005B2F8), 256);
    rt.SetGameState(gameState.data());

    const uint32_t bikeAt = Word(ram, 0x8005B268);
    std::vector<uint8_t> bike = Block(ram, bikeAt, 1096);
    const std::vector<uint8_t> owner = Block(ram, Word(ram, bikeAt + 0x354), 0x260);
    const std::vector<uint8_t> stats = Block(ram, Word(ram, bikeAt + 0x22C), 0x1C0);
    uint32_t seed = Word(ram, 0x8005AC8C + 2076);
    std::printf("engine-probe: bike 0x%08X of %s, owner +0x25C = %u, +0x220 = %u; model %u\n", bikeAt,
                opt.ram.c_str(), Word(ram, Word(ram, bikeAt + 0x354) + 0x25C),
                Word(ram, Word(ram, bikeAt + 0x354) + 0x220) & 0xFFFFu, Word(ram, Word(ram, 0x8005B2F8) + 0x48));

    const double seconds = opt.seconds > 0.0 ? opt.seconds : 8.0;
    const int frames = static_cast<int>(seconds * 30.0);
    std::shared_ptr<rr::audio::SpuVoices> spu = rt.Spu();
    rr::PcmBuffer pcm;
    pcm.channels = 2;
    pcm.sampleRate = rr::audio::SpuVoices::kRate;
    std::vector<int16_t> chunk(735 * 2);
    int32_t phasePeak[3] = {0, 0, 0};
    size_t clipped[3] = {0, 0, 0};
    const int idleEnd = frames / 8, throttleEnd = frames * 5 / 8;
    for (int f = 0; f < frames; ++f) {
        // the scripted inputs (see the header): revs in the bike's own 16.16 units
        const int phase = f < idleEnd ? 0 : (f < throttleEnd ? 1 : 2);
        int32_t revs = 19619280, load = 0, gear = 0;
        if (phase == 1) {
            const int k = f - idleEnd, n = throttleEnd - idleEnd;
            gear = std::min(3, 4 * k / n);
            const int inGear = k - gear * n / 4;
            revs = 30000000 + static_cast<int32_t>((75000000LL * inGear) / (n / 4));
            load = 2000000;
        } else if (phase == 2) {
            const int k = f - throttleEnd, n = frames - throttleEnd;
            gear = 3;
            revs = 100000000 - static_cast<int32_t>((80000000LL * k) / n);
        }
        Put32(bike, 0x25C, static_cast<uint32_t>(revs));
        Put32(bike, 0x24C, static_cast<uint32_t>(load));
        bike[0x351] = static_cast<uint8_t>(gear);
        // AudioFrame's on-the-bike arm: position AND velocity of the player's bike,
        // so the doppler ratio EngineNote asks for is exactly 1.0, as in every capture
        rt.SetListener(0, static_cast<int32_t>(Word(bike, 0x800000B8u)), static_cast<int32_t>(Word(bike, 0x800000C0u)),
                       static_cast<int32_t>(Word(bike, 0x800001C8u)), static_cast<int32_t>(Word(bike, 0x800001D0u)), 0);
        if (!rt.EngineFrame(bike, owner, stats, seed) && !rt.EngineFault().empty())
            throw std::runtime_error("the ported engine code faulted: " + rt.EngineFault());
        for (int v = 0; v < 2; ++v) {
            rt.VSync();
            spu->Render(chunk.data(), 735);
            for (int16_t x : chunk) {
                phasePeak[phase] = std::max(phasePeak[phase], std::min(32767, std::abs(static_cast<int32_t>(x))));
                if (x == 32767 || x == -32768) ++clipped[phase];
            }
            pcm.samples.insert(pcm.samples.end(), chunk.begin(), chunk.end());
        }
        if (f == idleEnd - 1 || f == (idleEnd + throttleEnd) / 2 || f == throttleEnd - 1 || f == frames - 1) {
            const uint32_t E = rt.ArenaWord(0x8005AC8C + 1952);
            const uint32_t h0 = rt.ArenaWord(E + 0x10), h3 = rt.ArenaWord(E + 0x1C), h4 = rt.ArenaWord(E + 0x20);
            std::printf("  frame %4d  level E+0x30 %5d  load E+0x60 %4d  L0 ch %2u pitch reg 0x%04X  L3 ch %2u  L4 ch %2u"
                        "  PMON 0x%06X\n",
                        f, static_cast<int32_t>(rt.ArenaWord(E + 0x30)), static_cast<int32_t>(rt.ArenaWord(E + 0x60)),
                        h0 >> 27, spu->Read(16u * (h0 >> 27) + 4u), h3 >> 27, h4 >> 27,
                        static_cast<uint32_t>(spu->Read(0x190)) | (static_cast<uint32_t>(spu->Read(0x192)) << 16));
        }
    }
    int on = 0;
    for (int ch = 0; ch < rr::audio::SpuVoices::kChannels; ++ch) on += spu->State(ch).on ? 1 : 0;
    uint64_t hash = 1469598103934665603ull;
    for (int16_t x : pcm.samples) hash = (hash ^ static_cast<uint16_t>(x)) * 1099511628211ull;
    std::printf("engine-probe: %zu game frame(s) through the PORTED EngineNote/RoadNote, %zu vsync(s) through the "
                "PORTED AudioVSyncTick/SoundService\n", rt.EngineFrames(), rt.VSyncs());
    std::printf("              %zu SPU register write(s), %u key-on(s), %d voice(s) sounding at the end, "
                "%zu effect spawn(s) not run (not ported)\n",
                rt.SpuWrites(), spu->KeyOns(), on, rt.EffectsNotRun());
    std::printf("              peak amplitude of 32767: idle %d, throttle %d, release %d (samples at the rail: "
                "%zu, %zu, %zu); %zu frames rendered, FNV-1a 0x%016llX\n",
                phasePeak[0], phasePeak[1], phasePeak[2], clipped[0], clipped[1], clipped[2], pcm.Frames(),
                static_cast<unsigned long long>(hash));
    if (!opt.wav.empty()) {
        WriteWav(opt.wav, pcm);
        std::printf("              wrote %s\n", opt.wav.c_str());
    }
    if (opt.play) {
        rr::audio::Mixer mixer(rr::audio::SpuVoices::kRate, 2);
        rr::audio::VoiceDesc desc;
        desc.source = std::make_shared<rr::audio::MemorySource>(pcm.samples, 2, rr::audio::SpuVoices::kRate);
        const rr::audio::VoiceId id = mixer.Play(desc);
        MixerSink sink(mixer);
        std::unique_ptr<rr::platform::AudioDevice> device =
            rr::platform::OpenAudioDevice(sink, rr::audio::SpuVoices::kRate, 1024);
        device->Start();
        while (mixer.IsPlaying(id)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        device->Stop();
        std::printf("              played through %s\n", device->BackendName());
    }
    return (on > 0 && phasePeak[1] > 0) ? 0 : 1;
}

} // namespace engine_probe

// ---------------------------------------------------------------- spu-test
//
// The deterministic test of two SPU features: pitch modulation (PMON) from
// the channel below and a repeat address written while the voice plays. Every sample below is a
// hand-built SPU-ADPCM block with filter 0 and shift 0, so a nibble `s` decodes to exactly s*4096
// and every level is known in advance. No game data is involved.
namespace spu_test {

void Block(std::vector<uint8_t>& ram, uint32_t at, uint8_t flags, int nibble) {
    ram[at] = 0x00; // filter 0, shift 0
    ram[at + 1] = flags;
    const uint8_t n = static_cast<uint8_t>(nibble & 0xF);
    for (uint32_t i = 2; i < 16; ++i) ram[at + i] = static_cast<uint8_t>(n | (n << 4));
}
// A two-block loop: flags 6 (loop start + repeat) then 3 (loop end + repeat), as the engine samples.
void Loop(std::vector<uint8_t>& ram, uint32_t at, int nibble) {
    Block(ram, at, 6, nibble);
    Block(ram, at + 16, 3, nibble);
}
// Every voice gets ADSR1 0x000F / ADSR2 0x0000 unless a test says otherwise: the fastest linear
// attack (0 -> 0x7FFF in three samples), sustain level 0xF (so no decay), a sustain that only
// rises - i.e. the envelope sits at 0x7FFF from the third sample on - and the main volume every
// capture holds, 0x3FFF (x 0x7FFE / 0x8000). So a sample s is heard as
// ((s * 0x7FFF >> 15) * 0x7FFE >> 15) * 0x7FFE >> 15: nibble 1 (4096) -> 4093, nibble 2 -> 8189.
void Voice(rr::audio::SpuVoices& spu, int ch, uint16_t pitch, uint16_t vol, uint32_t start,
           uint16_t adsr1 = 0x000F, uint16_t adsr2 = 0x0000) {
    const uint32_t b = 16u * static_cast<uint32_t>(ch);
    spu.Write(b + 0x0, vol);
    spu.Write(b + 0x2, vol);
    spu.Write(b + 0x4, pitch);
    spu.Write(b + 0x6, static_cast<uint16_t>(start / 8u));
    spu.Write(b + 0x8, adsr1);
    spu.Write(b + 0xA, adsr2);
    spu.Write(0x180, 0x3FFF);
    spu.Write(0x182, 0x3FFF);
}
void KeyOn(rr::audio::SpuVoices& spu, uint32_t mask) {
    spu.Write(0x188, static_cast<uint16_t>(mask));
    spu.Write(0x18A, static_cast<uint16_t>(mask >> 16));
}
std::vector<int16_t> Render(rr::audio::SpuVoices& spu, size_t frames) {
    std::vector<int16_t> out(frames * 2);
    spu.Render(out.data(), frames);
    return out;
}

int failures = 0;
void Check(bool ok, const char* what, long long got, long long want) {
    std::printf("  %-4s %-72s got %lld, want %lld\n", ok ? "ok" : "FAIL", what, got, want);
    if (!ok) ++failures;
}

// Loop A (nibble 1 -> 4096) at 0x1000, loop B (nibble 2 -> 8192) at 0x1020; the voice at unity
// volume (0x3FFF, i.e. 0x7FFE) is heard as 4093 for A and 8189 for B (see Voice).
int SwitchAt(bool writeRepeat, int framesBeforeWrite) {
    rr::audio::SpuVoices spu;
    Loop(spu.Ram(), 0x1000, 1);
    Loop(spu.Ram(), 0x1020, 2);
    Voice(spu, 5, 0x1000, 0x3FFF, 0x1000);
    KeyOn(spu, 1u << 5);
    Render(spu, static_cast<size_t>(framesBeforeWrite));
    if (writeRepeat) spu.Write(16u * 5u + 0xE, 0x1020 / 8);
    const std::vector<int16_t> out = Render(spu, 400);
    for (size_t i = 0; i < 400; ++i)
        if (out[2 * i] != 4093) return static_cast<int>(i);
    return -1;
}

// Stage B4, the envelope. Loop A on channel 6, the envelope observed through State().
struct EnvRun {
    rr::audio::SpuVoices spu;
    EnvRun(uint16_t adsr1, uint16_t adsr2) {
        Loop(spu.Ram(), 0x1000, 1);
        Voice(spu, 6, 0x1000, 0x3FFF, 0x1000, adsr1, adsr2);
        KeyOn(spu, 1u << 6);
    }
    rr::audio::SpuVoices::VoiceState After(size_t frames) {
        Render(spu, frames);
        return spu.State(6);
    }
};

void EnvelopeTests() {
    using Phase = rr::audio::SpuVoices::Phase;
    std::printf("B4, the ADSR envelope:\n");
    {
        // linear attack, shift 10, step +7: one step per sample of 7 << 1 = 14; 14 * 2340 = 32760,
        // the 2341st step reaches the top
        EnvRun e(static_cast<uint16_t>((10u << 10) | 0xFu), 0x0000);
        const auto a = e.After(2340);
        Check(a.level == 32760 && a.phase == Phase::Attack, "linear attack, shift 10: 2340 samples -> level 32760, still attacking", a.level, 32760);
        const auto b = e.After(1);
        Check(b.level == 32767 && b.phase == Phase::Sustain,
              "... the 2341st reaches 0x7FFF; sustain level 0xF (0x8000): no decay step, straight to sustain", b.level, 32767);
        const auto c = e.After(500);
        Check(c.phase == Phase::Sustain && c.level == 32767, "... and a rising sustain holds the top", c.level, 32767);
    }
    {
        // exponential attack, same rate: a step computed at a level above 0x6000 waits 4 samples.
        // Steps 1..1756 land on samples 0..1755 and leave 24584 (> 0x6000); the next 585
        // (24584 + 585*14 >= 32767) land every fourth sample from 1759: the last on
        // 1759 + 584*4 = 4095, i.e. after 4096 samples (linear: 2341)
        EnvRun e(static_cast<uint16_t>(0x8000u | (10u << 10) | 0xFu), 0x0000);
        const auto a = e.After(4095);
        Check(a.phase == Phase::Attack && a.level == 32760, "exponential attack: 4095 samples in, level 32760 (x4 above 0x6000)", a.level, 32760);
        const auto b = e.After(1);
        Check(b.phase == Phase::Sustain && b.level == 32767, "... and at the top after 4096 (linear: 2341)", b.level, 32767);
    }
    {
        // decay to a sustain level: ADSR1 sustain 7 -> 0x4000, decay shift 0 (exponential, -8 << 11
        // scaled by level / 0x8000, i.e. about halving per sample); ADSR2 sustain shift 31 holds it
        EnvRun e(0x0007, static_cast<uint16_t>(0x1Fu << 8));
        const auto a = e.After(200);
        Check(a.phase == Phase::Sustain && a.level <= 0x4000 && a.level > 0x1000, "decay stops at or below the sustain level (7 -> 0x4000) and holds", a.level, 0x4000);
        const auto b = e.After(2000);
        Check(b.level == a.level, "a sustain step of 1 << (31 - 11) samples does not move in 2000", b.level, a.level);
    }
    {
        // linear release, shift 11: -8 per sample from 0x7FFF: 4095 samples leave 7, the 4096th ends
        // the voice
        EnvRun e(0x000F, 0x000B);
        e.After(10);
        e.spu.Write(0x18C, 1u << 6); // KOFF
        const auto a = e.After(4095);
        Check(a.on && a.level == 7 && a.phase == Phase::Release, "key-off: linear release, shift 11, 4095 samples -> level 7", a.level, 7);
        const auto b = e.After(1);
        Check(!b.on && b.phase == Phase::Off, "... the 4096th sample ends the voice", b.on ? 1 : 0, 0);
    }
    {
        // the main volume scales the clamped sum: 0x2000 is 0x4000 = one half
        rr::audio::SpuVoices spu;
        Loop(spu.Ram(), 0x1000, 1);
        Voice(spu, 6, 0x1000, 0x3FFF, 0x1000);
        spu.Write(0x180, 0x2000);
        spu.Write(0x182, 0x2000);
        KeyOn(spu, 1u << 6);
        const std::vector<int16_t> out = Render(spu, 50);
        Check(out[2 * 49] == 2047, "main volume 0x2000: 4094 after the voice volume -> 2047", out[2 * 49], 2047);
    }
    {
        // saturation is counted where the SUM leaves 16 bits: two voices of nibble 7 (28672) at full
        // volume sum to 57340; the counter sees every frame after the attack, on both sides
        rr::audio::SpuVoices spu;
        Loop(spu.Ram(), 0x1000, 7);
        Voice(spu, 1, 0x1000, 0x3FFF, 0x1000);
        Voice(spu, 2, 0x1000, 0x3FFF, 0x1000);
        KeyOn(spu, (1u << 1) | (1u << 2));
        const std::vector<int16_t> out = Render(spu, 100);
        Check(out[2 * 99] == 32765 && spu.Clipped() >= 2u * 97u,
              "a saturating sum is clamped, then scaled: 32767 * 0x7FFE >> 15 = 32765; counted", static_cast<long long>(spu.Clipped()), 2 * 97);
        rr::audio::SpuVoices quiet;
        Loop(quiet.Ram(), 0x1000, 3);
        Voice(quiet, 1, 0x1000, 0x3FFF, 0x1000);
        KeyOn(quiet, 1u << 1);
        Render(quiet, 100);
        Check(quiet.Clipped() == 0, "one voice of 12288 never saturates (control)", static_cast<long long>(quiet.Clipped()), 0);
    }
}

int Run() {
    std::printf("rraudio spu-test - SPU checks: ADSR and main volume, PMON, repeat address\n");
    std::printf("B6, the repeat address written while the voice plays:\n");
    // Written 10 samples into block A0: A0 has 18 samples left and A1 28, so the voice must play
    // exactly 46 more samples of loop A and switch to loop B at sample 46 - at the loop END, not
    // at the write.
    Check(SwitchAt(true, 10) == 46, "a write in A0 takes effect at A1's loop end (first B sample)", SwitchAt(true, 10), 46);
    Check(SwitchAt(true, 40) == 16, "a write in A1 takes effect at the same loop end", SwitchAt(true, 40), 16);
    Check(SwitchAt(false, 10) == -1, "no write: loop A repeats for all 400 samples (control)", SwitchAt(false, 10), -1);
    {
        rr::audio::SpuVoices spu;
        Loop(spu.Ram(), 0x1000, 1);
        Loop(spu.Ram(), 0x1020, 2);
        Voice(spu, 5, 0x1000, 0x3FFF, 0x1000);
        KeyOn(spu, 1u << 5);
        Render(spu, 10);
        spu.Write(16u * 5u + 0xE, 0x1020 / 8);
        Render(spu, 46);
        const std::vector<int16_t> out = Render(spu, 2000);
        int notB = 0;
        for (size_t i = 0; i < 2000; ++i) notB += (out[2 * i] != 8189) ? 1 : 0;
        Check(notB == 0, "after the switch the voice STAYS on loop B (its own loop-start block)", notB, 0);
        Check(spu.State(5).repeat == 0x1020, "the repeat register holds loop B's start", spu.State(5).repeat, 0x1020);
    }
    {
        rr::audio::SpuVoices spu;
        Block(spu.Ram(), 0x2000, 0, 3);
        Block(spu.Ram(), 0x2010, 1, 3); // loop end WITHOUT repeat: a one-shot
        Voice(spu, 2, 0x1000, 0x3FFF, 0x2000);
        KeyOn(spu, 1u << 2);
        const std::vector<int16_t> out = Render(spu, 100);
        int heard = 0;
        for (size_t i = 0; i < 100; ++i) heard += out[2 * i] != 0 ? 1 : 0;
        Check(heard == 56 && !spu.State(2).on, "a block with loop end and no repeat ends the voice after 56 samples", heard, 56);
    }

    std::printf("B5, pitch modulation from the channel below:\n");
    // Modulator on channel 3 at volume 0 holding a constant level m; carrier on channel 4 at pitch
    // 0x0800 (half a sample per output sample). With PMON bit 4 the carrier's step is
    // 0x0800 * (m + 0x8000) >> 15.
    auto carrier = [](int modNibble, uint32_t pmon, int carrierCh, int modCh, size_t frames, int16_t* firstOut) {
        rr::audio::SpuVoices spu;
        Loop(spu.Ram(), 0x3000, modNibble);
        Loop(spu.Ram(), 0x3020, 1);
        Voice(spu, modCh, 0x1000, 0x0000, 0x3000); // volume 0: heard only through the carrier
        Voice(spu, carrierCh, 0x0800, 0x3FFF, 0x3020);
        spu.Write(0x190, static_cast<uint16_t>(pmon));
        spu.Write(0x192, static_cast<uint16_t>(pmon >> 16));
        KeyOn(spu, (1u << modCh) | (1u << carrierCh));
        const std::vector<int16_t> out = Render(spu, frames);
        if (firstOut) *firstOut = out[2 * (frames - 1)];
        return static_cast<long long>(spu.State(carrierCh).advanced);
    };
    int16_t o = 0;
    // With the envelope the modulator's output is 16384 * 0x7FFF >> 15 = 16383 once its attack is
    // done, so the step is 0x0800 * 49151 >> 15 = 3071, not 3072, and the first two samples ramp
    // (steps 2496 and 2944): 2496 + 2944 + 998 * 3071 = 3 070 298 -> 749 samples in 1000 frames.
    // A negative level floors (-16384 * 0x7FFF >> 15 = -16384), so the x0.5 case stays at 250.
    Check(carrier(4, 1u << 4, 4, 3, 1000, &o) == 749, "m = +16384: step x1.5 (after the envelope), 1000 frames advance 749 samples", carrier(4, 1u << 4, 4, 3, 1000, nullptr), 749);
    Check(o == 4093, "the volume-0 modulator adds nothing to the mix (carrier level alone)", o, 4093);
    Check(carrier(-4, 1u << 4, 4, 3, 1000, nullptr) == 250, "m = -16384: step x0.5, 250 samples", carrier(-4, 1u << 4, 4, 3, 1000, nullptr), 250);
    Check(carrier(0, 1u << 4, 4, 3, 1000, nullptr) == 500, "m = 0: step unchanged, 500 samples", carrier(0, 1u << 4, 4, 3, 1000, nullptr), 500);
    Check(carrier(4, 0, 4, 3, 1000, nullptr) == 500, "PMON clear: no modulation (control), 500 samples", carrier(4, 0, 4, 3, 1000, nullptr), 500);
    Check(carrier(4, 1u << 4, 4, 2, 1000, nullptr) == 500, "the modulator must be the channel BELOW: ch 2 does not modulate ch 4", carrier(4, 1u << 4, 4, 2, 1000, nullptr), 500);
    Check(carrier(4, 1u << 0, 0, 1, 1000, nullptr) == 500, "PMON bit 0 is ignored: channel 0 has no channel below it", carrier(4, 1u << 0, 0, 1, 1000, nullptr), 500);
    {
        rr::audio::SpuVoices spu;
        Loop(spu.Ram(), 0x4000, 1);
        Voice(spu, 7, 0x5000, 0x3FFF, 0x4000);
        KeyOn(spu, 1u << 7);
        Render(spu, 100);
        Check(spu.State(7).advanced == 400, "a step above 0x3FFF is clamped to 0x4000 (4 samples per frame)", static_cast<long long>(spu.State(7).advanced), 400);
    }
    {
        auto scenario = []() {
            rr::audio::SpuVoices spu;
            Loop(spu.Ram(), 0x3000, 5);
            Loop(spu.Ram(), 0x3020, -3);
            Voice(spu, 10, 0x0321, 0x0000, 0x3000);
            Voice(spu, 11, 0x0F00, 0x2345, 0x3020);
            spu.Write(0x190, 1u << 11);
            KeyOn(spu, (1u << 10) | (1u << 11));
            std::vector<int16_t> a = Render(spu, 3000);
            spu.Write(16u * 11u + 0xE, 0x3000 / 8);
            std::vector<int16_t> b = Render(spu, 3000);
            a.insert(a.end(), b.begin(), b.end());
            return a;
        };
        Check(scenario() == scenario(), "determinism: the same register writes render the same 6000 frames", 1, 1);
    }
    EnvelopeTests();
    std::printf("spu-test: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}

} // namespace spu_test

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return Usage();
    const std::string command = argv[1];
    Options opt;
    std::vector<std::string> positional;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + arg);
            return argv[++i];
        };
        if (arg == "--start") opt.start = std::stoull(next());
        else if (arg == "--units") opt.units = std::stoull(next());
        else if (arg == "--record") opt.record = std::stol(next());
        else if (arg == "--limit") opt.limit = std::stoi(next());
        else if (arg == "--rate") opt.rate = std::stoi(next());
        else if (arg == "--pitch") opt.pitch = std::stoi(next());
        else if (arg == "--volume") opt.volume = std::stoi(next());
        else if (arg == "--pan") opt.pan = std::stoi(next());
        else if (arg == "--seconds") opt.seconds = std::stod(next());
        else if (arg == "--sample") opt.sample = std::stol(next());
        else if (arg == "--track") opt.track = std::stol(next());
        else if (arg == "--exe") opt.exe = next();
        else if (arg == "--all") opt.all = true;
        else if (arg == "--probe-compat") opt.probeCompat = true;
        else if (arg == "--loop") opt.loop = true;
        else if (arg == "--ram") opt.ram = next();
        else if (arg == "--wav") opt.wav = next();
        else if (arg == "--play") opt.play = true;
        else if (arg.rfind("--", 0) == 0) { std::fprintf(stderr, "unknown option %s\n", arg.c_str()); return Usage(); }
        else positional.push_back(arg);
    }

    try {
        if (command == "scan") {
            if (positional.empty()) return Usage();
            return CmdScan(positional);
        }
        if (command == "list") {
            if (positional.size() != 1) return Usage();
            return CmdList(positional[0], opt);
        }
        if (command == "decode") {
            if (positional.size() != 2) return Usage();
            return CmdDecode(positional[0], positional[1], opt);
        }
        if (command == "play") {
            if (positional.size() != 1) return Usage();
            return CmdPlay(positional[0], opt);
        }
        if (command == "tracks") {
            if (positional.size() != 1) return Usage();
            return CmdTracks(positional[0]);
        }
        if (command == "spu-test") return spu_test::Run();
        if (command == "engine-probe") {
            if (positional.size() != 1) return Usage();
            return engine_probe::Run(positional[0], opt);
        }
        if (command == "mix") {
            if (positional.size() < 2) return Usage();
            return CmdMix(positional[0], {positional.begin() + 1, positional.end()}, opt);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return Usage();
}
