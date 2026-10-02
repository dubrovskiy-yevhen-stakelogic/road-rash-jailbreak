#include "platform/xr/xr_actions.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace rr::xr {
namespace {

void Check(XrResult r, const char* where) {
    if (XR_FAILED(r)) throw std::runtime_error(std::string("OpenXR actions: ") + where + " failed (" + std::to_string(int(r)) + ")");
}

uint8_t StickByte(float v, bool invert) {
    const float c = std::clamp(v, -1.0f, 1.0f) * (invert ? -1.0f : 1.0f);
    return static_cast<uint8_t>(std::lround(127.5f + c * 127.5f));
}

} // namespace

ControllerActions::ControllerActions(XrInstance instance, XrSession session, PFN_xrGetInstanceProcAddr get)
    : instance_(instance), session_(session) {
#define RRXR_LOAD(n) Check(get(instance, #n, reinterpret_cast<PFN_xrVoidFunction*>(&n)), #n);
    RRXR_ACTION_API(RRXR_LOAD)
#undef RRXR_LOAD
    auto path = [&](const std::string& text) {
        XrPath p{};
        Check(xrStringToPath(instance, text.c_str(), &p), text.c_str());
        return p;
    };
    hands_[0] = path("/user/hand/left");
    hands_[1] = path("/user/hand/right");
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(si.actionSetName, sizeof(si.actionSetName), "%s", "riding");
    std::snprintf(si.localizedActionSetName, sizeof(si.localizedActionSetName), "%s", "Riding and menus");
    Check(xrCreateActionSet(instance, &si, &set_), "xrCreateActionSet");
    auto action = [&](const char* name, XrActionType type) {
        XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
        std::snprintf(ci.actionName, sizeof(ci.actionName), "%s", name);
        std::snprintf(ci.localizedActionName, sizeof(ci.localizedActionName), "%s", name);
        ci.actionType = type;
        ci.countSubactionPaths = 2;
        ci.subactionPaths = hands_;
        XrAction a = XR_NULL_HANDLE;
        Check(xrCreateAction(set_, &ci, &a), name);
        return a;
    };
    stick_ = action("stick", XR_ACTION_TYPE_VECTOR2F_INPUT);
    trigger_ = action("trigger", XR_ACTION_TYPE_FLOAT_INPUT);
    grip_ = action("grip", XR_ACTION_TYPE_FLOAT_INPUT);
    click_ = action("stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT);
    a_ = action("button_a", XR_ACTION_TYPE_BOOLEAN_INPUT);
    b_ = action("button_b", XR_ACTION_TYPE_BOOLEAN_INPUT);
    x_ = action("button_x", XR_ACTION_TYPE_BOOLEAN_INPUT);
    y_ = action("button_y", XR_ACTION_TYPE_BOOLEAN_INPUT);
    menu_ = action("menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
    haptic_ = action("rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT);
    gripPose_ = action("grip_pose", XR_ACTION_TYPE_POSE_INPUT);
    aimPose_ = action("aim_pose", XR_ACTION_TYPE_POSE_INPUT);

    { // Touch
        std::vector<XrActionSuggestedBinding> b;
        auto bind = [&](XrAction a, const std::string& p) { b.push_back({a, path(p)}); };
        for (const char* hand : {"left", "right"}) {
            const std::string h = std::string("/user/hand/") + hand;
            bind(gripPose_, h + "/input/grip/pose");
            bind(aimPose_, h + "/input/aim/pose");
            bind(stick_, h + "/input/thumbstick");
            bind(trigger_, h + "/input/trigger/value");
            bind(grip_, h + "/input/squeeze/value");
            bind(click_, h + "/input/thumbstick/click");
            bind(haptic_, h + "/output/haptic");
        }
        bind(a_, "/user/hand/right/input/a/click");
        bind(b_, "/user/hand/right/input/b/click");
        bind(x_, "/user/hand/left/input/x/click");
        bind(y_, "/user/hand/left/input/y/click");
        bind(menu_, "/user/hand/left/input/menu/click");
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
        sb.countSuggestedBindings = static_cast<uint32_t>(b.size());
        sb.suggestedBindings = b.data();
        Check(xrSuggestInteractionProfileBindings(instance, &sb), "Touch bindings");
    }
    { // the generic profile every runtime supports
        std::vector<XrActionSuggestedBinding> b;
        auto bind = [&](XrAction a, const std::string& p) { b.push_back({a, path(p)}); };
        for (const char* hand : {"left", "right"}) {
            const std::string h = std::string("/user/hand/") + hand;
            bind(gripPose_, h + "/input/grip/pose");
            bind(aimPose_, h + "/input/aim/pose");
            bind(trigger_, h + "/input/select/click");
            bind(menu_, h + "/input/menu/click");
            bind(haptic_, h + "/output/haptic");
        }
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = path("/interaction_profiles/khr/simple_controller");
        sb.countSuggestedBindings = static_cast<uint32_t>(b.size());
        sb.suggestedBindings = b.data();
        const XrResult r = xrSuggestInteractionProfileBindings(instance, &sb);
        if (XR_FAILED(r)) std::printf("xr: simple_controller bindings refused (%d)\n", int(r));
    }
    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &set_;
    Check(xrAttachSessionActionSets(session_, &ai), "xrAttachSessionActionSets");
    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ci.subactionPath = hands_[h];
        ci.poseInActionSpace.orientation.w = 1.0f;
        ci.action = gripPose_;
        Check(xrCreateActionSpace(session_, &ci, &gripSpaces_[h]), "grip space");
        ci.action = aimPose_;
        Check(xrCreateActionSpace(session_, &ci, &aimSpaces_[h]), "aim space");
    }
}

ControllerActions::~ControllerActions() {
    for (int h = 0; h < 2; ++h) {
        if (gripSpaces_[h] != XR_NULL_HANDLE) xrDestroySpace(gripSpaces_[h]);
        if (aimSpaces_[h] != XR_NULL_HANDLE) xrDestroySpace(aimSpaces_[h]);
    }
    if (set_ != XR_NULL_HANDLE) xrDestroyActionSet(set_);
}

bool ControllerActions::Poll(XrPad& pad, bool focused) {
    pad = XrPad{};
    if (!focused) {
        pad.Derive();
        return false;
    }
    XrActiveActionSet active{set_, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(session_, &sync))) {
        pad.Derive();
        return false;
    }
    bool any = false;
    auto boolean = [&](XrAction a, int hand) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        gi.subactionPath = hands_[hand];
        XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_FAILED(xrGetActionStateBoolean(session_, &gi, &st)) || !st.isActive) return false;
        any = true;
        return st.currentState == XR_TRUE;
    };
    auto scalar = [&](XrAction a, int hand) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = a;
        gi.subactionPath = hands_[hand];
        XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_FAILED(xrGetActionStateFloat(session_, &gi, &st)) || !st.isActive) return 0.0f;
        any = true;
        return std::clamp(st.currentState, 0.0f, 1.0f);
    };
    float sx[2] = {0, 0}, sy[2] = {0, 0};
    for (int h = 0; h < 2; ++h) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = stick_;
        gi.subactionPath = hands_[h];
        XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(session_, &gi, &st)) && st.isActive) {
            any = true;
            sx[h] = st.currentState.x;
            sy[h] = st.currentState.y;
        }
    }
    for (int h = 0; h < 2; ++h) {
        pad.stick[h][0] = sx[h];
        pad.stick[h][1] = sy[h];
    }
    pad.leftTrigger = scalar(trigger_, 0);
    pad.rightTrigger = scalar(trigger_, 1);
    pad.leftGrip = scalar(grip_, 0);
    pad.rightGrip = scalar(grip_, 1);
    // the sticks as DualShock bytes: y up is 0x00; the right stick's Y folds the triggers in as XInput's does
    // (gamepad_win32.h: ry = right Y + RT - LT)
    pad.lx = StickByte(sx[0], false);
    pad.ly = StickByte(sy[0], true);
    pad.rx = StickByte(sx[1], false);
    pad.ry = StickByte(std::clamp(sy[1] + pad.rightTrigger - pad.leftTrigger, -1.0f, 1.0f), true);
    uint16_t b = 0;
    constexpr float kStick = 1.0f / 3.0f; // past a third of the travel, as the desktop pad's left stick
    constexpr float kPress = 0.25f;
    // the raw buttons, each read once (the phase-1 PlayStation mapping below is built from them)
    uint16_t t = 0;
    if (boolean(a_, 1)) t |= kTouchA;
    if (boolean(b_, 1)) t |= kTouchB;
    if (boolean(x_, 0)) t |= kTouchX;
    if (boolean(y_, 0)) t |= kTouchY;
    if (boolean(menu_, 0)) t |= kTouchMenu;
    if (boolean(click_, 0)) t |= kTouchLeftStick;
    if (boolean(click_, 1)) t |= kTouchRightStick;
    pad.touch = t;
    if (sx[0] < -kStick) b |= kPadLeft;
    if (sx[0] > kStick) b |= kPadRight;
    if (sy[0] > kStick) b |= kPadUp;
    if (sy[0] < -kStick) b |= kPadDown;
    if (pad.rightTrigger > kPress || (t & kTouchA)) b |= kPadCross;
    if (pad.leftTrigger > kPress || (t & kTouchX)) b |= kPadSquare;
    if (t & kTouchB) b |= kPadCircle;
    if (t & kTouchY) b |= kPadTriangle;
    if (pad.leftGrip > 0.5f) b |= kPadL1;
    if (pad.rightGrip > 0.5f) b |= kPadR1;
    if (t & kTouchLeftStick) b |= kPadL2;
    if (t & kTouchRightStick) b |= kPadSelect;
    if (t & kTouchMenu) b |= kPadStart;
    pad.buttons = b;
    pad.connected = any;
    pad.Derive();
    return any;
}

void ControllerActions::Locate(XrSpace base, XrTime time, HandPose hands[2]) {
    auto locate = [&](XrAction action, XrSpace space, int h, float out[7]) {
        if (time <= 0) return false;
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = action;
        gi.subactionPath = hands_[h];
        XrActionStatePose st{XR_TYPE_ACTION_STATE_POSE};
        if (XR_FAILED(xrGetActionStatePose(session_, &gi, &st)) || !st.isActive) return false;
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        constexpr XrSpaceLocationFlags kValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (XR_FAILED(xrLocateSpace(space, base, time, &loc)) || (loc.locationFlags & kValid) != kValid) return false;
        out[0] = loc.pose.position.x; out[1] = loc.pose.position.y; out[2] = loc.pose.position.z;
        out[3] = loc.pose.orientation.x; out[4] = loc.pose.orientation.y; out[5] = loc.pose.orientation.z;
        out[6] = loc.pose.orientation.w;
        return true;
    };
    for (int h = 0; h < 2; ++h) {
        hands[h].gripValid = locate(gripPose_, gripSpaces_[h], h, hands[h].grip);
        hands[h].aimValid = locate(aimPose_, aimSpaces_[h], h, hands[h].aim);
    }
}

void ControllerActions::Vibrate(float left, float right, bool focused) {
    // A pulse when the level starts or moves, a renewal before it runs out, a stop on 0 - not a 40 ms
    // pulse re-sent on every call (haptic_pulse.h)
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    const float amp[2] = {focused ? left : 0.0f, focused ? right : 0.0f};
    for (int h = 0; h < 2; ++h) {
        const HapticCommand c = pulse_[h].Update(amp[h], now);
        if (c.kind == HapticCommand::kNone) continue;
        XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
        hi.action = haptic_;
        hi.subactionPath = hands_[h];
        if (c.kind == HapticCommand::kApply) {
            XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
            v.amplitude = c.amplitude;
            v.duration = c.durationNs;
            v.frequency = XR_FREQUENCY_UNSPECIFIED;
            xrApplyHapticFeedback(session_, &hi, reinterpret_cast<const XrHapticBaseHeader*>(&v));
        } else {
            xrStopHapticFeedback(session_, &hi);
        }
    }
}

} // namespace rr::xr
