#pragma once
// The one place where the game talks to the operating system's audio output.
//
// Everything above this line is headless and testable: the mixer produces 16-bit stereo frames, and an
// AudioDevice is only a pump that asks for them on its own thread. Tests and the cross-check tools
// never open a device.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace rr::platform {

// Implemented by the mixer side. Render() is called on the device's own thread and must not block.
class AudioSink {
public:
    virtual ~AudioSink() = default;
    // `frames` stereo frames, interleaved L,R. The sink must fill all of them (silence if idle).
    virtual void Render(int16_t* out, size_t frames) = 0;
};

class AudioDevice {
public:
    virtual ~AudioDevice() = default;
    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual int SampleRate() const = 0;
    virtual const char* BackendName() const = 0;
};

// Opens the default output device at `sampleRate`, 2 channels, 16-bit. A hidden run
// (RRJB_WINDOW=hidden) or RRJB_AUDIO=silent gets a silent device instead: same real-time pull from the
// sink, nothing reaches the speakers. RRJB_AUDIO=device forces the speakers. `bufferFrames` is the size of
// one queued block; the backend keeps a few of them in flight. Throws std::runtime_error on failure.
std::unique_ptr<AudioDevice> OpenAudioDevice(AudioSink& sink, int sampleRate,
                                             size_t bufferFrames = 1024);

} // namespace rr::platform
