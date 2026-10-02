// Android playback backend, AAudio (the Quest build). The same contract as
// audio_device_winmm.cpp: 16-bit stereo pulled from the AudioSink at the device's pace. Model: the sibling project's
// src/game/audio/audio_device_aaudio.cpp (the gt2-play project, MIT).
//
// AAudio calls back on its own high-priority thread, so there is no pump thread here: the callback asks the sink for
// exactly the frames AAudio wants. The stream is opened at the mixer's rate; when the device insists on another one
// (the Quest's own mix runs at 48 kHz) the callback resamples linearly from blocks it renders at the mixer's rate
// rather than pretending the rates match. RRJB_AUDIO=silent gives the silent device of the Windows file (a real-time
// pull that is thrown away).
#include "platform/audio_device.h"

#include <aaudio/AAudio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

namespace rr::platform {
namespace {

class AAudioDevice final : public AudioDevice {
public:
    AAudioDevice(AudioSink& sink, int sampleRate, size_t bufferFrames) : sink_(sink), sampleRate_(sampleRate) {
        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder)
            throw std::runtime_error("audio device: AAudio_createStreamBuilder failed");
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setSampleRate(builder, sampleRate);
        AAudioStreamBuilder_setChannelCount(builder, 2);
        AAudioStreamBuilder_setDataCallback(builder, &AAudioDevice::Callback, this);
        const aaudio_result_t opened = AAudioStreamBuilder_openStream(builder, &stream_);
        AAudioStreamBuilder_delete(builder);
        if (opened != AAUDIO_OK || !stream_)
            throw std::runtime_error(std::string("audio device: AAudioStreamBuilder_openStream failed (") +
                                     AAudio_convertResultToText(opened) + ")");
        if (AAudioStream_getFormat(stream_) != AAUDIO_FORMAT_PCM_I16 || AAudioStream_getChannelCount(stream_) != 2) {
            AAudioStream_close(stream_);
            throw std::runtime_error("audio device: the AAudio stream is not 16-bit stereo");
        }
        deviceRate_ = AAudioStream_getSampleRate(stream_);
        if (deviceRate_ <= 0) {
            AAudioStream_close(stream_);
            throw std::runtime_error("audio device: the AAudio stream reports no sample rate");
        }
        block_.assign(std::max<size_t>(bufferFrames, 64) * 2, 0);
        std::printf("audio: AAudio %d Hz stereo 16-bit (the mixer runs at %d Hz%s)\n", deviceRate_, sampleRate_,
                    deviceRate_ == sampleRate_ ? "" : ", resampled linearly in the callback");
    }

    ~AAudioDevice() override {
        Stop();
        if (stream_) {
            std::printf("audio: AAudio underruns %d\n", AAudioStream_getXRunCount(stream_));
            AAudioStream_close(stream_); // waits for a running callback to return
        }
    }

    void Start() override {
        if (running_.exchange(true)) return;
        const aaudio_result_t r = AAudioStream_requestStart(stream_);
        if (r != AAUDIO_OK) {
            running_.store(false);
            throw std::runtime_error(std::string("audio device: AAudioStream_requestStart failed (") + AAudio_convertResultToText(r) + ")");
        }
    }

    void Stop() override {
        if (!running_.exchange(false)) return;
        AAudioStream_requestStop(stream_);
    }

    int SampleRate() const override { return sampleRate_; }
    const char* BackendName() const override { return "AAudio"; }

private:
    static aaudio_data_callback_result_t Callback(AAudioStream*, void* user, void* data, int32_t frames) {
        auto* self = static_cast<AAudioDevice*>(user);
        int16_t* out = static_cast<int16_t*>(data);
        if (!self || !self->running_.load()) {
            std::memset(out, 0, size_t(frames) * 4);
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        self->Fill(out, frames);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    // One source frame out of the sink's stream, fetched block by block.
    void NextSource(int16_t frame[2]) {
        if (blockPos_ >= blockFrames_) {
            blockFrames_ = block_.size() / 2;
            sink_.Render(block_.data(), blockFrames_);
            blockPos_ = 0;
        }
        frame[0] = block_[2 * blockPos_];
        frame[1] = block_[2 * blockPos_ + 1];
        ++blockPos_;
    }

    void Fill(int16_t* out, int32_t frames) {
        if (deviceRate_ == sampleRate_) {
            sink_.Render(out, size_t(frames));
            return;
        }
        const double step = double(sampleRate_) / double(deviceRate_);
        for (int32_t i = 0; i < frames; ++i) {
            while (phase_ >= 1.0) {
                prev_[0] = next_[0];
                prev_[1] = next_[1];
                NextSource(next_);
                phase_ -= 1.0;
            }
            for (int c = 0; c < 2; ++c)
                out[2 * i + c] = static_cast<int16_t>(std::lround(prev_[c] + (next_[c] - prev_[c]) * phase_));
            phase_ += step;
        }
    }

    AudioSink& sink_;
    int sampleRate_;
    int deviceRate_ = 0;
    AAudioStream* stream_ = nullptr;
    std::atomic<bool> running_{false};
    std::vector<int16_t> block_;
    size_t blockFrames_ = 0, blockPos_ = 0;
    int16_t prev_[2] = {0, 0}, next_[2] = {0, 0};
    double phase_ = 1.0;
};

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
    const char* BackendName() const override { return "silent (RRJB_AUDIO=silent)"; }

private:
    AudioSink& sink_;
    int sampleRate_;
    std::vector<int16_t> pcm_;
    size_t bufferFrames_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace

std::unique_ptr<AudioDevice> OpenAudioDevice(AudioSink& sink, int sampleRate, size_t bufferFrames) {
    if (sampleRate <= 0) throw std::runtime_error("audio device: bad sample rate");
    if (bufferFrames < 64) bufferFrames = 64;
    const char* mode = std::getenv("RRJB_AUDIO");
    if (mode && std::strcmp(mode, "silent") == 0) return std::make_unique<SilentDevice>(sink, sampleRate, bufferFrames);
    return std::make_unique<AAudioDevice>(sink, sampleRate, bufferFrames);
}

} // namespace rr::platform
