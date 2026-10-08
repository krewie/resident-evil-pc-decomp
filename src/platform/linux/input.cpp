// input.cpp - Linux input backend (Phase 5).
//
// Counterpart of src/marni/MarniInput.cpp + MarniXInput.cpp. Reproduces the
// mask contract exactly (docs/GAMEPAD_INPUT.md): a 32-bit word where
//   bits 0-3  = axis directions (up, down, left, right)
//   bits 4-7  = POV hat - deliberately unused, g_JoyRemapTbl[1] zeroes them
//   bits 8-19 = buttons (MARNI_XI_BTN_*)
// published into joysticks[0] with the same prev/curr/newPress transitions as
// the WinMM path, so nothing downstream can tell the backends apart.
//
// There is no WinMM on Linux, so the device sweep is SDL's game-controller API
// instead of joyGetPosEx; the bit assignments are identical.
#include "Globals.h"
#include "platform/platform.h"
#include "marni/MarniInput.h"
#include "marni/MarniXInput.h"

#include <SDL2/SDL.h>

// ---------------------------------------------------------------------------
// Pad state
// ---------------------------------------------------------------------------

// port-addition: Support multiple controllers controlling Player 1.
// Keep controller handles outside the original game's memory structures.
static const int MAX_PADS = 8;
static SDL_GameController* s_pads[MAX_PADS] = {};

static Uint32 s_nextRescan = 0;

static const int kDeadzone = 10000;                 // MARNI_XI_DEFAULT_DEADZONE
static const int kTriggerThreshold = 3855;          // XInput's 30/255, scaled

// port-addition: Check whether at least one controller is connected.
static bool HasConnectedPad(void)
{
    for (int i = 0; i < MAX_PADS; ++i) {
        if (s_pads[i] != NULL &&
            SDL_GameControllerGetAttached(s_pads[i])) {
            return true;
        }
    }

    return false;
}

static void TryOpenPad(void)
{

    int count = SDL_NumJoysticks();

    if (getenv("RE1_INPUT_DEBUG") != NULL) {
        fprintf(stderr, "[PAD] rescan: %d joystick(s)\n", count);
    }


    // Release controllers that have been disconnected.
    for (int i = 0; i < MAX_PADS; ++i) {
        if (s_pads[i] && !SDL_GameControllerGetAttached(s_pads[i])) {
            SDL_GameControllerClose(s_pads[i]);
            s_pads[i] = NULL;
        }
    }

    // Throttle device enumeration to once per second.
    Uint32 now = SDL_GetTicks();
    if (now < s_nextRescan)
        return;

    s_nextRescan = now + 1000;

    for (int i = 0; i < count; ++i) {
        if (!SDL_IsGameController(i))
            continue;

        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);

        // Check whether this controller is already registered.
        bool alreadyOpen = false;

        for (int j = 0; j < MAX_PADS; ++j) {
            if (!s_pads[j])
                continue;

            SDL_Joystick* joystick =
                SDL_GameControllerGetJoystick(s_pads[j]);

            if (SDL_JoystickInstanceID(joystick) == id) {
                alreadyOpen = true;
                break;
            }
        }

        if (alreadyOpen)
            continue;

        // Register the new controller in the first available slot.
        for (int j = 0; j < MAX_PADS; ++j) {
            if (s_pads[j] != NULL)
                continue;

            SDL_GameController* pad = SDL_GameControllerOpen(i);

            if (pad) {
                s_pads[j] = pad;

                fprintf(stderr, "[PAD] opened: %s\n",
                        SDL_GameControllerName(pad));
            }

            break;
        }
    }
}

// Port addition: Combine input from all connected SDL controllers.
//
// Every controller contributes to the same input mask.
// allowing Steam Deck controls and external gamepads to work simultaneously. as an example
static DWORD BuildPadMask(void)
{
    DWORD mask = 0;

    for (int i = 0; i < MAX_PADS; ++i) {
        SDL_GameController* pad = s_pads[i];

        // Ignore unused slots and controllers that have disconnected.
        if (pad == NULL || !SDL_GameControllerGetAttached(pad))
            continue;

        Sint16 lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        Sint16 ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);

        // D-pad and left stick both produce the original directional bits.
        // SDL's positive Y direction points down.
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP)    || ly < -kDeadzone) mask |= 1;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)  || ly >  kDeadzone) mask |= 2;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)  || lx < -kDeadzone) mask |= 4;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || lx >  kDeadzone) mask |= 8;

        // Merge face buttons, shoulders, Start/Back and stick buttons.
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A))             mask |= MARNI_XI_BTN_A;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B))             mask |= MARNI_XI_BTN_B;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X))             mask |= MARNI_XI_BTN_X;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y))             mask |= MARNI_XI_BTN_Y;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  mask |= MARNI_XI_BTN_LB;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) mask |= MARNI_XI_BTN_RB;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK))          mask |= MARNI_XI_BTN_BACK;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START))         mask |= MARNI_XI_BTN_START;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK))     mask |= MARNI_XI_BTN_LTHUMB;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK))    mask |= MARNI_XI_BTN_RTHUMB;

        // Convert analog triggers to digital buttons using the existing
        // XInput-compatible thresholds.
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > kTriggerThreshold)
            mask |= MARNI_XI_BTN_LTRIGGER;

        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > kTriggerThreshold)
            mask |= MARNI_XI_BTN_RTRIGGER;
    }

    // Resolve contradictory directions after merging all controllers.
    // Preserve the original priority: UP over DOWN, LEFT over RIGHT.
    if (mask & 1) mask &= ~2u;
    if (mask & 4) mask &= ~8u;

    return mask;
}

// ---------------------------------------------------------------------------
// CMarniDirectInput
// ---------------------------------------------------------------------------

void CMarniDirectInput::UpdateKeyboardInputState(MasterInputState* pState)
{
    // 0x004202f0: zero keyboardCurr, then test all 32 mapped keys.
    pState->keyboardCurr = 0;
    for (int i = 0; i < 32; i++) {
        if (plat_key_state(pState->keyMap[i]) & 0x8000) {
            pState->keyboardCurr |= (1u << i);
        }
    }
}

void CMarniDirectInput::UpdateAllInputStates(MasterInputState* pState)
{
    // 0x0042057a-0x004206e7: keyboard state transitions.
    UpdateKeyboardInputState(pState);

    DWORD oldPrev = pState->keyboardPrev;
    pState->keyboardRepeat   = oldPrev;
    pState->keyboardPrev     = pState->keyboardCurr;
    pState->frameFlag        = 1;
    pState->keyboardNewPress = (~oldPrev) & pState->keyboardCurr;

    // --- Pad publishes into joysticks[0] ---
    // Port addition: Publish the combined input from all connected controllers
    // into the original Player 1 joystick slot.
    TryOpenPad();

    JoystickEntry* pJoy = &pState->joysticks[0];
    DWORD padMask = BuildPadMask();

    if (HasConnectedPad()) {
        pJoy->enabled   = 1;
        pJoy->prevPress = pJoy->currPress;
        pJoy->currPress = padMask;
        pJoy->newPress  = (~pJoy->prevPress) & padMask;
    } else {
        // No pad: release the slot and clear any latched press state.
        pJoy->enabled   = 0;
        pJoy->prevPress = 0;
        pJoy->currPress = 0;
        pJoy->newPress  = 0;
    }

    // The OG still sees one logical controller.
    pState->joystickCount = HasConnectedPad() ? 1u : 0u;
}

void CMarniDirectInput::InitJoysticks(MasterInputState* pState)
{
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);

    // Port addition: Discover all controllers, but expose a single logical
    // Player 1 joystick to the original input system.
    TryOpenPad();

    pState->joystickCount = HasConnectedPad() ? 1u : 0u;
}

// ---------------------------------------------------------------------------
// Capability queries used by InputUpdate
// ---------------------------------------------------------------------------

// Port addition: Report a connected gamepad when at least one SDL
// controller is attached, regardless of which physical device it is.
bool MarniPadIsConnected(void)
{
    return HasConnectedPad();
}

// Port addition: Release all registered controller handles before
// shutting down SDL's game-controller subsystem.
static void CloseAllPads(void)
{
    for (int i = 0; i < MAX_PADS; ++i) {
        if (s_pads[i] != NULL) {
            SDL_GameControllerClose(s_pads[i]);
            s_pads[i] = NULL;
        }
    }
}

// port-addition: Clean up SDL controller resources on game exit.
void MarniPadShutdown(void)
{
    CloseAllPads();
}

namespace MarniXInput {

bool IsConnected()
{
    return HasConnectedPad();
}

}
