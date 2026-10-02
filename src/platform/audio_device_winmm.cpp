// Windows playback backend, waveOut (WinMM).
//
// Why WinMM and not WASAPI: the mixer already produces exactly what waveOut takes - packed 16-bit
// stereo PCM at whatever rate we ask for - so this backend is an event, a thread and four buffers,
// with no COM apartment, no IMMDeviceEnumerator, and no shared-mode format negotiation (WASAPI shared
// mode hands you 32-bit float at the device's own rate, so it would need either
// AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM or our own resampler in the output path). The device sits behind
// rr::platform::AudioDevice, so a WASAPI backend can be added later for lower latency without any
// caller changing. Latency here is bufferFrames * kBuffers / rate, i.e. ~93 ms at 1024 frames /
// 22050 Hz, which is fine for streaming music and for this tool.
#include "platform/audio_device.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

namespace rr::platform {
namespace {

constexpr size_t kBuffers = 4;

std::string MmError(const char* what, MMRESULT mr) {
    char text[MAXERRORLENGTH] = {};
    if (waveOutGetErrorTextA(mr, text, MAXERRORLENGTH) != MMSYSERR_NOERROR) text[0] = '\0';
    return std::string(what) + ": MMRESULT " + std::to_string(static_cast<unsigned>(mr)) +
           (text[0] ? std::string(" (") + text + ")" : std::string());
}

class WinMmDevice final : public AudioDevice {
public:
    WinMmDevice(AudioSink& sink, int sampleRate, size_t bufferFrames)
        : sink_(sink), sampleRate_(sampleRate), bufferFrames_(bufferFrames) {
        WAVEFORMATEX fmt = {};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = static_cast<DWORD>(sampleRate_);
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = static_cast<WORD>(fmt.nChannels * fmt.wBitsPerSample / 8);
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        fmt.cbSize = 0;

        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) throw std::runtime_error("audio device: CreateEvent failed");

        const MMRESULT mr = waveOutOpen(&handle_, WAVE_MAPPER, &fmt,
                                        reinterpret_cast<DWORD_PTR>(event_), 0, CALLBACK_EVENT);
        if (mr != MMSYSERR_NOERROR) {
            CloseHandle(event_);
            event_ = nullptr;
            throw std::runtime_error(MmError("audio device: waveOutOpen failed", mr));
        }

        blocks_.resize(kBuffers);
        for (Block& b : blocks_) {
            b.pcm.assign(bufferFrames_ * 2, 0);
            b.header = {};
            b.header.lpData = reinterpret_cast<LPSTR>(b.pcm.data());
            b.header.dwBufferLength = static_cast<DWORD>(b.pcm.size() * sizeof(int16_t));
            const MMRESULT pr = waveOutPrepareHeader(handle_, &b.header, sizeof(WAVEHDR));
            if (pr != MMSYSERR_NOERROR) {
                Close();
                throw std::runtime_error(MmError("audio device: waveOutPrepareHeader failed", pr));
            }
            b.prepared = true;
            b.header.dwFlags |= WHDR_DONE; // free, so the pump picks it up on the first pass
        }
    }

    ~WinMmDevice() override {
        Stop();
        Close();
    }

    void Start() override {
        if (running_.exchange(true)) return;
        thread_ = std::thread(&WinMmDevice::Pump, this);
    }

    void Stop() override {
        if (!running_.exchange(false)) return;
        SetEvent(event_);
        if (thread_.joinable()) thread_.join();
        if (handle_) waveOutReset(handle_);
    }

    int SampleRate() const override { return sampleRate_; }
    const char* BackendName() const override { return "winmm/waveOut"; }

private:
    struct Block {
        std::vector<int16_t> pcm;
        WAVEHDR header = {};
        bool prepared = false;
    };

    void Close() {
        if (handle_) {
            waveOutReset(handle_);
            for (Block& b : blocks_)
                if (b.prepared) {
                    waveOutUnprepareHeader(handle_, &b.header, sizeof(WAVEHDR));
                    b.prepared = false;
                }
            waveOutClose(handle_);
            handle_ = nullptr;
        }
        if (event_) {
            CloseHandle(event_);
            event_ = nullptr;
        }
    }

    void Pump() {
        while (running_.load()) {
            bool queued = false;
            for (Block& b : blocks_) {
                if (!running_.load()) break;
                if (!(b.header.dwFlags & WHDR_DONE)) continue;
                sink_.Render(b.pcm.data(), bufferFrames_);
                b.header.dwFlags &= ~WHDR_DONE;
                const MMRESULT mr = waveOutWrite(handle_, &b.header, sizeof(WAVEHDR));
                if (mr != MMSYSERR_NOERROR) {
                    // Nothing sensible to do on the audio thread: stop pumping and let Stop() clean up.
                    running_.store(false);
                    break;
                }
                queued = true;
            }
            if (!queued) WaitForSingleObject(event_, 100);
        }
    }

    AudioSink& sink_;
    int sampleRate_;
    size_t bufferFrames_;
    HWAVEOUT handle_ = nullptr;
    HANDLE event_ = nullptr;
    std::vector<Block> blocks_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

// A device that pulls from the sink at the same real-time pace as the speakers would, and throws the
// samples away. Scripted and checking runs use it so they never make a sound,
// while everything measured on the sink side (frames rendered, peak, voices) stays exactly what a
// real device would have produced.
class SilentDevice final : public AudioDevice {
public:
    SilentDevice(AudioSink& sink, int sampleRate, size_t bufferFrames)
        : sink_(sink), sampleRate_(sampleRate), pcm_(bufferFrames * 2, 0), bufferFrames_(bufferFrames) {}
    ~SilentDevice() override { Stop(); }

    void Start() override {
        if (running_.exchange(true)) return;
        thread_ = std::thread([this] {
            const auto period = std::chrono::microseconds(
                static_cast<long long>(1'000'000.0 * static_cast<double>(bufferFrames_) / sampleRate_));
            auto next = std::chrono::steady_clock::now();
            while (running_.load()) {
                sink_.Render(pcm_.data(), bufferFrames_);
                next += period;
                std::this_thread::sleep_until(next);
            }
        });
    }
    void Stop() override {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
    }
    int SampleRate() const override { return sampleRate_; }
    const char* BackendName() const override { return "silent (hidden run or RRJB_AUDIO=silent)"; }

private:
    AudioSink& sink_;
    int sampleRate_;
    std::vector<int16_t> pcm_;
    size_t bufferFrames_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

// RRJB_AUDIO=silent forces the silent device and RRJB_AUDIO=device forces the speakers; unset, a
// run whose window is hidden (RRJB_WINDOW=hidden: the gates, scripted checks) is silent.
bool SilentRequested() {
    char buf[32] = {};
    if (GetEnvironmentVariableA("RRJB_AUDIO", buf, sizeof(buf)) > 0) return _stricmp(buf, "silent") == 0;
    if (GetEnvironmentVariableA("RRJB_WINDOW", buf, sizeof(buf)) > 0) return _stricmp(buf, "hidden") == 0;
    return false;
}

} // namespace

std::unique_ptr<AudioDevice> OpenAudioDevice(AudioSink& sink, int sampleRate, size_t bufferFrames) {
    if (sampleRate <= 0) throw std::runtime_error("audio device: bad sample rate");
    if (bufferFrames < 64) bufferFrames = 64;
    if (SilentRequested()) return std::make_unique<SilentDevice>(sink, sampleRate, bufferFrames);
    return std::make_unique<WinMmDevice>(sink, sampleRate, bufferFrames);
}

} // namespace rr::platform
