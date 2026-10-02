#pragma once
// The Touch controllers' action set (internal to the OpenXR layer: xr_session_gl.cpp owns one). Model: the sibling
// project's src/platform/xr/xr_actions.cpp (gt2-play, MIT), mapped onto this game's pad
// (xr_pad.h) instead of GT2's.
//
// Suggested bindings for /interaction_profiles/oculus/touch_controller (Quest 1/2/3 Touch, and what Link / Virtual
// Desktop / SteamVR expose for them) and, for runtimes that only speak the generic profile,
// /interaction_profiles/khr/simple_controller (select = the trigger, menu = Start).
#include <openxr/openxr.h>

#include "platform/xr/haptic_pulse.h"
#include "platform/xr/xr_pad.h"

namespace rr::xr {

class ControllerActions {
public:
    ControllerActions(XrInstance instance, XrSession session, PFN_xrGetInstanceProcAddr get);
    ~ControllerActions();
    ControllerActions(const ControllerActions&) = delete;
    ControllerActions& operator=(const ControllerActions&) = delete;

    // xrSyncActions and the pad. `focused`: the session is FOCUSED (the runtime gives input to one application only;
    // otherwise the pad is released - all zero, connected = false).
    bool Poll(XrPad& pad, bool focused);
    // The two hands' grip and aim poses in `base` at `time` (valid flags false when untracked).
    void Locate(XrSpace base, XrTime time, HandPose hands[2]);
    // The DualShock's two motors on the Touch haptics: amplitude 0..1 per hand, called every frame; a pulse only when
    // the level starts or moves and a renewal before it runs out (haptic_pulse.h); 0 stops that hand.
    // Only in a FOCUSED session (unfocused = 0).
    void Vibrate(float left, float right, bool focused);

private:
    XrInstance instance_;
    XrSession session_;
    XrActionSet set_ = XR_NULL_HANDLE;
    XrAction stick_ = XR_NULL_HANDLE, trigger_ = XR_NULL_HANDLE, grip_ = XR_NULL_HANDLE, click_ = XR_NULL_HANDLE;
    XrAction a_ = XR_NULL_HANDLE, b_ = XR_NULL_HANDLE, x_ = XR_NULL_HANDLE, y_ = XR_NULL_HANDLE, menu_ = XR_NULL_HANDLE;
    XrAction haptic_ = XR_NULL_HANDLE, gripPose_ = XR_NULL_HANDLE, aimPose_ = XR_NULL_HANDLE;
    XrPath hands_[2]{};
    XrSpace gripSpaces_[2]{}, aimSpaces_[2]{};
    HapticPulse pulse_[2];
#define RRXR_ACTION_API(X)                                                                                              \
    X(xrStringToPath) X(xrCreateActionSet) X(xrDestroyActionSet) X(xrCreateAction) X(xrSuggestInteractionProfileBindings) \
    X(xrAttachSessionActionSets) X(xrSyncActions) X(xrCreateActionSpace) X(xrDestroySpace) X(xrLocateSpace)               \
    X(xrGetActionStatePose) X(xrGetActionStateBoolean) X(xrGetActionStateFloat) X(xrGetActionStateVector2f)               \
    X(xrApplyHapticFeedback) X(xrStopHapticFeedback)
#define RRXR_DECLARE(n) PFN_##n n = nullptr;
    RRXR_ACTION_API(RRXR_DECLARE)
#undef RRXR_DECLARE
};

} // namespace rr::xr
