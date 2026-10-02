#pragma once
// The VR proof's frame loop, shared by the Windows executable (main_win32.cpp) and the Quest APK (main_android.cpp):
// the OpenXR frame at the runtime's pace, the two eyes of ProofScene in stereo with head tracking, the theatre quad
// beside them, the Touch controllers read into the PlayStation pad model (xr_pad.h) with the triggers on the haptics.
#include "platform/xr/xr_session_gl.h"
#include "vr_scene.h"

#include <functional>
#include <string>

namespace rrvr {

struct LoopOptions {
    long long frames = -1;       // stop after this many submitted frames (-1: until the runtime or the host quits)
    float unitsPerMetre = 1.0f;  // world units per metre (one world unit is one metre)
    float eyeHeight = 1.25f;     // metres above the road for the LOCAL space's origin (a seated rider's eyes)
    bool ps1Look = false;        // the 15-bit PS1 quantisation over each eye (RaceScene::PostProcess, no dither)
    bool quad = true;            // the theatre quad beside the world
    std::string shotDir;         // when set: eye 0, eye 1 and the quad written there as PNG at frame `shotFrame`
    long long shotFrame = 240;
    std::function<bool()> keepGoing; // the host's lifecycle (Android: not destroyed); may be empty
    std::function<void()> idle;      // called while the session is not running (the host services its events)
};

// Runs until the frame budget, the runtime (EXITING / LOSS_PENDING) or the host ends it; returns the process code.
int RunLoop(ProofScene& scene, rr::xr::GlSession& session, const LoopOptions& options);

// Without OpenXR (a GL context is enough): both eyes of a fixed synthetic head (seated, 64 mm IPD, a Quest-3-like
// asymmetric field) side by side in an offscreen w x h target, written to `png` - the SAME code path on desktop GL
// and on the Quest's ES, so the two pictures can be compared pixel for pixel.
bool MockPair(ProofScene& scene, int w, int h, const std::string& png, const LoopOptions& options);
// `frames` stereo frames of 2 x eyeW x eyeH offscreen, glFinish-bounded: milliseconds per stereo frame.
double MockTiming(ProofScene& scene, int eyeW, int eyeH, int frames, const LoopOptions& options);

} // namespace rrvr
