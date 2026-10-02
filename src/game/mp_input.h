#pragma once
// Player 2's controls in a two-player race. Header-only, like the
// gamepad it reads. The original reads the second pad port the same way as the first (SLUS 0x8001CB3C
// loops over the pad records 0x800D7128 + 192p), so player 2 gets the same PadState the session hands to
// the ported pad reader, from:
//   * the keyboard's right-hand layout: arrows = d-pad (Up throttle / Cross, Down brake / Square, Left /
//     Right steer), M = R1 (punch), comma = L1, period = R2 (kick), K / L = d-pad Up / Down held with
//     them, slash = Select (the next chase camera), semicolon = look behind;
//   * the SECOND game controller (platform/gamepad_win32.h Poll(1)), mapped as player 1's is;
//   * a script: --hold2 <TBLRV>, --autosteer2 and --taunt2 N (rrgame), as --hold / --autosteer / --taunt
//     are player 1's.
// The taunt (L2) is N or the second controller's taunt binding (L3 by default): the pad reader's L2 arm runs for
// player 2's record as for player 1's (SLUS 0x8001D548, s2 = the loop's pad record, s3 = its bike;
// speech_session.cpp TauntPad). F6 switches the SECOND controller's ANALOG mode (a 0x73 pad, as F5 does
// the first's: left stick steers, right stick / triggers throttle), which the PORTED region reads for
// record 1 and the axes 0x800CE548.
// In a two-player race player 1 keeps the left-hand layout only (W A S D and its keys) and the first
// controller: the arrows are player 2's.
// Start (control 1, slot 12 - the pause test reads every pad record): H, the second
// controller's Start or the script's p2start, through rrgame's second PauseKeys (pause_product.h); when
// player 2 paused (gp+260 = 1) the pause menu reads record 1, and player 2's pad is then the menu pad:
// the arrows / d-pad, M or Cross = Cross, N or Triangle = Triangle, H or Start = Start (script p2up p2down
// p2left p2right p2x p2t). RRJB_P2_START=off is the negative control.
#include "game/race_session.h"
#include "platform/gamepad_state.h" // GamepadState and the key codes (portable)

#include <cstdio>

namespace rr::game {

struct SecondPlayerInput {
    bool cameraWas = false, padCameraWas = false;
    bool analogMode = false, f6Was = false;

    PadState Read(const bool* key, const rr::platform::GamepadState& gp, const PadState& hold) {
        PadState p;
        p.throttle = hold.throttle || key[VK_UP] || gp.throttle;
        p.brake = hold.brake || key[VK_DOWN] || gp.brake;
        p.left = hold.left || key[VK_LEFT] || gp.left;
        p.right = hold.right || key[VK_RIGHT] || gp.right;
        p.lookBack = hold.lookBack || key[VK_OEM_1] || gp.lookBack;
        p.r1 = key['M'] != 0 || gp.r1; // the second controller's combat bits: its bindings
        p.l1 = key[VK_OEM_COMMA] != 0 || gp.l1;
        p.r2 = key[VK_OEM_PERIOD] != 0 || gp.r2;
        p.padUp = key['K'] != 0 || gp.padUp;
        p.padDown = key['L'] != 0 || gp.padDown;
        const bool camera = key[VK_OEM_2] != 0;
        p.cameraNext = (camera && !cameraWas) || (gp.camera && !padCameraWas);
        cameraWas = camera;
        padCameraWas = gp.camera;
        p.taunt = hold.taunt || key['N'] || gp.taunt;
        const bool f6 = key[VK_F6] != 0;
        if (f6 && !f6Was) {
            analogMode = !analogMode;
            std::printf("pad 2: %s mode\n", analogMode ? "ANALOGUE (0x73: left stick steers, right stick / triggers "
                                                         "throttle)" : "digital (0x41)");
        }
        f6Was = f6;
        p.device.analog = analogMode && gp.connected;
        if (p.device.analog) {
            p.device.lx = gp.lx;
            p.device.ly = gp.ly;
            p.device.ry = gp.ry;
        }
        return p;
    }
};

} // namespace rr::game
