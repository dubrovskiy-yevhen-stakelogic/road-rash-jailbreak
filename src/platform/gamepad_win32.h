#pragma once
// A game controller for `rrgame`, mapped onto the original's pad bits through the rebindable bindings
// (input_bindings.h). Three sources, tried in this order for the n-th controller:
//   * XInput (Xbox-layout pads), loaded at run time from xinput1_4.dll / xinput9_1_0.dll - no import library;
//   * a Sony DualSense / DualSense Edge read natively over HID, USB or Bluetooth (dualsense.h): its buttons,
//     sticks and analogue triggers, its motors and its adaptive triggers - no Steam Input needed;
//   * the WinMM joystick API (`joyGetPosEx`): every other DirectInput-class game controller (a DualSense the
//     native path has open is skipped here, so it is not counted twice).
// Each source is decoded BY POSITION into a PhysicalPad (pad_decode.h), and MapPad turns that into the
// original's pad word with the active bindings. The race reads the bound fields (throttle ... r1 / l1 / r2 ...);
// the menus read the physical ones (up / down / cross / circle / square / triangle), as a PlayStation pad's
// positions: Cross confirms, Triangle goes back.
//
// The sticks: in the digital mode (the 0x41 device, the default) the left stick is a second d-pad. In the
// ANALOGUE mode (rrgame's F5, the DualShock's ANALOG button: a 0x73 device) lx / ly / ry become the three stick
// bytes the driver stores - left X (steering), right Y (throttle / brake, what the original's analogue on-bike
// branch reads) and left Y - and the PORTED pad reader turns them into the axes 0x800CE540 (game\pad_product.h).
// OURS: the triggers bound to throttle / brake are folded into the right stick's Y (throttle up, brake down).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <mmsystem.h>

#include "platform/dualsense.h"
#include "platform/gamepad_state.h" // GamepadState, StateFromPhysical (portable)
#include "platform/input_bindings.h"
#include "platform/pad_decode.h"

#include <cstdint>
#include <memory>

namespace rr::platform {

class Gamepad {
public:
    // `nth`: which connected controller (0 = the first; a two-player race polls 1 for player 2). XInput pads
    // come first, then the native DualSense controllers, then the other WinMM joysticks.
    GamepadState Poll(int nth = 0) {
        nth_ = nth < 0 ? 0 : nth;
        dualsense_.reset();
        PhysicalPad p;
        int skip = nth_;
        if (!PollXInput(p, skip) && !PollDualSense(p, skip)) PollWinMM(p, skip);
        GamepadState s = StateFromPhysical(p, ActiveBindings());
        s.hasMotors = (user_ >= 0 && setState_ != nullptr) || dualsense_ != nullptr;
        return s;
    }

    // THE MOTORS (game\rumble_product.h): the DualShock's two motors on the pad the last Poll
    // used. XInput: the large motor's strength byte 0..255 on the low-frequency (left) motor as byte * 257, the
    // small motor's on / off on the high-frequency (right) one as 65535 / 0. A DualSense: its compatible-rumble
    // motors (dualsense.h). A WinMM pad has no force feedback. Returns true when the value changed (a device
    // write). The destructor stops the motors.
    bool Vibrate(bool small, uint8_t large) {
        if (dualsense_) {
            dualsense_->Motors(small ? 1 : 0, large); // every frame: the device switches off 500 ms after the last
            const bool changed = small != dsSmall_ || large != dsLarge_;
            dsSmall_ = small;
            dsLarge_ = large;
            return changed;
        }
        if (user_ < 0 || !setState_) return false;
        const WORD left = static_cast<WORD>(large * 257u), right = small ? 0xFFFFu : 0u;
        if (left == lastLeft_ && right == lastRight_) return false;
        XInputVibration v{left, right};
        setState_(static_cast<DWORD>(user_), &v);
        lastLeft_ = left;
        lastRight_ = right;
        return true;
    }
    // The DualSense's adaptive triggers (input_bindings.h TriggerResistance): call every frame; `racing` false
    // (menus, the pause, a hidden run) releases them.
    void Triggers(bool racing) {
        if (!dualsense_) return;
        uint8_t right = 0, left = 0;
        if (racing) TriggerResistance(ActiveBindings(), right, left);
        dualsense_->Triggers(right, left);
    }
    ~Gamepad() {
        if (user_ >= 0 && setState_ && (lastLeft_ != 0 || lastRight_ != 0)) {
            XInputVibration v{0, 0};
            setState_(static_cast<DWORD>(user_), &v);
        }
        if (dualsense_) {
            dualsense_->Motors(0, 0);
            dualsense_->Triggers(0, 0);
        }
    }

private:
    struct XInputState {
        DWORD dwPacketNumber;
        XInputRaw Gamepad; // XINPUT_GAMEPAD's layout
    };
    static_assert(sizeof(XInputRaw) == 12, "XInputRaw must match XINPUT_GAMEPAD");
    using GetStateFn = DWORD(WINAPI*)(DWORD, XInputState*);
    struct XInputVibration {
        WORD wLeftMotorSpeed, wRightMotorSpeed;
    };
    using SetStateFn = DWORD(WINAPI*)(DWORD, XInputVibration*);

    bool PollXInput(PhysicalPad& p, int& skip) {
        if (!xinputTried_) {
            xinputTried_ = true;
            for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
                if (HMODULE m = LoadLibraryA(dll)) {
                    getState_ = reinterpret_cast<GetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")));
                    setState_ = reinterpret_cast<SetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputSetState")));
                    if (getState_) break;
                }
            }
        }
        user_ = -1;
        if (!getState_) return false;
        for (DWORD user = 0; user < 4; ++user) {
            XInputState st{};
            if (getState_(user, &st) != ERROR_SUCCESS) continue;
            if (skip-- > 0) continue; // an earlier player's controller
            user_ = static_cast<int>(user);
            p = DecodeXInput(st.Gamepad);
            return true;
        }
        return false;
    }

    bool PollDualSense(PhysicalPad& p, int& skip) {
        DualSenseHub& hub = DualSenseHub::Get();
        const size_t n = hub.Count();
        dualsenseOpen_ = n > 0;
        for (size_t i = 0; i < n; ++i) {
            std::shared_ptr<DualSenseDevice> d = hub.At(i);
            if (!d) continue;
            if (skip-- > 0) continue;
            PhysicalPad in;
            const int r = d->ReadPad(in);
            if (r < 0) continue;
            if (r == 0) { // open, nothing received yet (a Bluetooth pad sends on a change): connected and neutral
                in = PhysicalPad{};
                in.connected = true;
                in.style = PadStyle::PlayStation;
                in.source = d->Bluetooth() ? "DualSense Bluetooth" : "DualSense USB";
            }
            p = in;
            dualsense_ = std::move(d);
            return true;
        }
        return false;
    }

    // WinMM: asking an absent joystick id costs 1..2.5 ms (16 ids: 40 ms, then 18 ms; rrgame --dualsenseprobe), so the
    // ids 0..kJoyIds-1 are probed round robin, one every 500 ms, after one full pass at the first poll; a present
    // one is read every frame. A DualSense the native path has open is skipped.
    static constexpr UINT kJoyIds = 4;
    void PollWinMM(PhysicalPad& p, int skip) {
        const DWORD now = GetTickCount();
        if (!joyProbed_ || now - lastJoyProbe_ >= 500) {
            const UINT first = joyProbed_ ? joyNext_ : 0, count = joyProbed_ ? 1 : kJoyIds;
            joyProbed_ = true;
            lastJoyProbe_ = now;
            for (UINT k = 0; k < count; ++k) {
                const UINT id = (first + k) % kJoyIds;
                JOYINFOEX info{};
                info.dwSize = sizeof(info);
                info.dwFlags = JOY_RETURNBUTTONS;
                joyPresent_[id] = joyGetPosEx(id, &info) == JOYERR_NOERROR;
                JOYCAPSA caps{};
                const bool haveCaps = joyPresent_[id] && joyGetDevCapsA(id, &caps, sizeof(caps)) == JOYERR_NOERROR;
                joySony_[id] = haveCaps && caps.wMid == 0x054C;
                joyDualSense_[id] = joySony_[id] && (caps.wPid == 0x0CE6 || caps.wPid == 0x0DF2);
            }
            joyNext_ = (first + count) % kJoyIds;
        }
        UINT id = 0;
        for (; id < kJoyIds; ++id) {
            if (!joyPresent_[id] || (joyDualSense_[id] && dualsenseOpen_)) continue;
            if (skip-- == 0) break;
        }
        if (id >= kJoyIds) return;
        JOYINFOEX info{};
        info.dwSize = sizeof(info);
        info.dwFlags = JOY_RETURNALL;
        if (joyGetPosEx(id, &info) != JOYERR_NOERROR) {
            joyPresent_[id] = false; // gone
            return;
        }
        WinMMRaw raw;
        raw.buttons = info.dwButtons;
        raw.pov = info.dwPOV;
        raw.x = info.dwXpos;
        raw.y = info.dwYpos;
        raw.z = info.dwZpos;
        raw.r = info.dwRpos;
        p = DecodeWinMM(raw, joySony_[id]);
    }

    int nth_ = 0;
    bool xinputTried_ = false;
    GetStateFn getState_ = nullptr;
    SetStateFn setState_ = nullptr;
    int user_ = -1;
    WORD lastLeft_ = 0, lastRight_ = 0;
    std::shared_ptr<DualSenseDevice> dualsense_;
    bool dualsenseOpen_ = false, dsSmall_ = false;
    uint8_t dsLarge_ = 0;
    bool joyProbed_ = false;
    DWORD lastJoyProbe_ = 0;
    UINT joyNext_ = 0;
    bool joyPresent_[kJoyIds] = {}, joySony_[kJoyIds] = {}, joyDualSense_[kJoyIds] = {};
};

} // namespace rr::platform
