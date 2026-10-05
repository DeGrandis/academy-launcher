#include "Hle.h"

#include "Log.h"

#include <windows.h>
#include <Xinput.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace cw::d3d {
HWND gameWindow();
unsigned long long scriptMilliseconds();
}

namespace cw::hle {

namespace {

constexpr std::uint32_t kGamepadDeviceType = 0x0032B878;
constexpr DWORD kErrorDeviceNotConnected = 1167;

#pragma pack(push, 2)
struct XboxGamepad {
    WORD buttons;
    BYTE analogButtons[8];
    SHORT thumbLX;
    SHORT thumbLY;
    SHORT thumbRX;
    SHORT thumbRY;
};

struct XboxInputState {
    DWORD packetNumber;
    XboxGamepad gamepad;
};

struct XboxCapabilities {
    BYTE subType;
    WORD reserved;
    XboxGamepad in;
    WORD rumbleLeft;
    WORD rumbleRight;
};
#pragma pack(pop)

struct XboxFeedback {
    DWORD status;
    HANDLE event;
    BYTE reserved[58];
    WORD rumbleLeft;
    WORD rumbleRight;
};

enum XboxButton : WORD {
    kDpadUp = 0x01, kDpadDown = 0x02, kDpadLeft = 0x04, kDpadRight = 0x08,
    kStart = 0x10, kBack = 0x20, kLeftThumb = 0x40, kRightThumb = 0x80,
};
enum XboxAnalog { kA, kB, kX, kY, kBlack, kWhite, kLeftTrigger, kRightTrigger };

constexpr int kPorts = 4;
DWORD g_packetNumber[kPorts] = {};
XboxGamepad g_lastState[kPorts] = {};

// Virtual pads (setVirtualPad): ports a plugin drives. The device-type block's first two dwords are the connected
// and "changed since last XGetDeviceChanges" port masks.
std::mutex g_virtualMutex;
PadFilter g_padFilter = nullptr;
bool g_virtualPresent[kPorts] = {};
XboxGamepad g_virtualState[kPorts] = {};

volatile DWORD* gamepadDeviceMasks() {
    return reinterpret_cast<volatile DWORD*>(static_cast<std::uintptr_t>(kGamepadDeviceType));
}

// CW_INPUT_SCRIPT="ms:button[:holdms],..." presses a button at each time offset, for 150 ms unless a hold time is given.
// Buttons: start, back, a, b, x, y, black, white, lt, rt, up, down, left, right (d-pad), lup, ldown, lleft, lright (left stick),
// rup, rdown, rleft, rright (right stick), lthumb, rthumb (stick clicks).
void applyScript(const std::string& script, ULONGLONG elapsed, XboxGamepad& pad);

void applyScriptedInput(XboxGamepad& pad) {
    static const std::string script = [] {
        const char* value = std::getenv("CW_INPUT_SCRIPT");
        return value == nullptr ? std::string() : std::string(value);
    }();
    static const ULONGLONG start = cw::d3d::scriptMilliseconds();
    if (!script.empty()) {
        applyScript(script, cw::d3d::scriptMilliseconds() - start, pad);
    }
    // CW_MISSION_INPUT: the same format, timed (wall clock) from the first frame in play (screen 4) of each mission,
    // for automated runs that drive the player after spawning.
    static const std::string missionScript = [] {
        const char* value = std::getenv("CW_MISSION_INPUT");
        return value == nullptr ? std::string() : std::string(value);
    }();
    if (!missionScript.empty()) {
        static ULONGLONG playingSince = 0;
        const bool playing = *reinterpret_cast<volatile std::int32_t*>(0x0038F7A8) == 4;
        if (!playing) {
            playingSince = 0;
        } else {
            if (playingSince == 0) {
                playingSince = GetTickCount64();
            }
            applyScript(missionScript, GetTickCount64() - playingSince, pad);
        }
    }
}

void applyScript(const std::string& script, ULONGLONG elapsed, XboxGamepad& pad) {
    std::size_t position = 0;
    while (position < script.size()) {
        const std::size_t end = std::min(script.find(',', position), script.size());
        const std::string item = script.substr(position, end - position);
        const std::size_t colon = item.find(':');
        if (colon != std::string::npos) {
            const ULONGLONG at = std::strtoull(item.c_str(), nullptr, 10);
            const std::size_t holdColon = item.find(':', colon + 1);
            const std::string button = item.substr(colon + 1, holdColon == std::string::npos ? std::string::npos : holdColon - colon - 1);
            const ULONGLONG hold = holdColon == std::string::npos ? 150 : std::strtoull(item.c_str() + holdColon + 1, nullptr, 10);
            if (elapsed >= at && elapsed < at + hold) {
                if (button == "start") pad.buttons |= kStart;
                else if (button == "back") pad.buttons |= kBack;
                else if (button == "up") pad.buttons |= kDpadUp;
                else if (button == "down") pad.buttons |= kDpadDown;
                else if (button == "left") pad.buttons |= kDpadLeft;
                else if (button == "right") pad.buttons |= kDpadRight;
                else if (button == "a") pad.analogButtons[kA] = 255;
                else if (button == "b") pad.analogButtons[kB] = 255;
                else if (button == "x") pad.analogButtons[kX] = 255;
                else if (button == "y") pad.analogButtons[kY] = 255;
                else if (button == "black") pad.analogButtons[kBlack] = 255;
                else if (button == "white") pad.analogButtons[kWhite] = 255;
                else if (button == "lt") pad.analogButtons[kLeftTrigger] = 255;
                else if (button == "rt") pad.analogButtons[kRightTrigger] = 255;
                else if (button == "lup") pad.thumbLY = 32767;
                else if (button == "ldown") pad.thumbLY = -32768;
                else if (button == "lleft") pad.thumbLX = -32768;
                else if (button == "lright") pad.thumbLX = 32767;
                else if (button == "rup") pad.thumbRY = 32767;
                else if (button == "rdown") pad.thumbRY = -32768;
                else if (button == "rleft") pad.thumbRX = -32768;
                else if (button == "rright") pad.thumbRX = 32767;
                else if (button == "lthumb") pad.buttons |= kLeftThumb;
                else if (button == "rthumb") pad.buttons |= kRightThumb;
            }
        }
        position = end + 1;
    }
}

bool keyDown(int key) {
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

void readKeyboardAndMouse(XboxGamepad& pad) {
    HWND window = d3d::gameWindow();
    if (window == nullptr || GetForegroundWindow() != window) {
        return;
    }
    auto button = [&](bool pressed, WORD flag) {
        if (pressed) {
            pad.buttons |= flag;
        }
    };
    auto analog = [&](bool pressed, XboxAnalog index) {
        if (pressed) {
            pad.analogButtons[index] = 255;
        }
    };
    button(keyDown(VK_UP) || keyDown('W'), kDpadUp);
    button(keyDown(VK_DOWN) || keyDown('S'), kDpadDown);
    button(keyDown(VK_LEFT) || keyDown('A'), kDpadLeft);
    button(keyDown(VK_RIGHT) || keyDown('D'), kDpadRight);
    button(keyDown(VK_SPACE), kStart);
    button(keyDown(VK_TAB), kBack);
    analog(keyDown(VK_RETURN) || keyDown(VK_LBUTTON), kA);
    analog(keyDown(VK_ESCAPE) || keyDown(VK_BACK) || keyDown(VK_RBUTTON), kB);
    analog(keyDown('X'), kX);
    analog(keyDown('Y'), kY);
    analog(keyDown('Q'), kLeftTrigger);
    analog(keyDown('E'), kRightTrigger);
    analog(keyDown('R'), kBlack);  // host right shoulder
    analog(keyDown('F'), kWhite);  // host left shoulder
    if (pad.buttons & kDpadUp) pad.thumbLY = 32767;
    if (pad.buttons & kDpadDown) pad.thumbLY = -32768;
    if (pad.buttons & kDpadLeft) pad.thumbLX = -32768;
    if (pad.buttons & kDpadRight) pad.thumbLX = 32767;
}

void readHostController(DWORD port, XboxGamepad& pad) {
    XINPUT_STATE state{};
    if (XInputGetState(port, &state) != ERROR_SUCCESS) {
        return;
    }
    const XINPUT_GAMEPAD& host = state.Gamepad;
    auto button = [&](WORD hostFlag, WORD flag) {
        if (host.wButtons & hostFlag) {
            pad.buttons |= flag;
        }
    };
    auto analog = [&](WORD hostFlag, XboxAnalog index) {
        if (host.wButtons & hostFlag) {
            pad.analogButtons[index] = 255;
        }
    };
    button(XINPUT_GAMEPAD_DPAD_UP, kDpadUp);
    button(XINPUT_GAMEPAD_DPAD_DOWN, kDpadDown);
    button(XINPUT_GAMEPAD_DPAD_LEFT, kDpadLeft);
    button(XINPUT_GAMEPAD_DPAD_RIGHT, kDpadRight);
    button(XINPUT_GAMEPAD_START, kStart);
    button(XINPUT_GAMEPAD_BACK, kBack);
    button(XINPUT_GAMEPAD_LEFT_THUMB, kLeftThumb);
    button(XINPUT_GAMEPAD_RIGHT_THUMB, kRightThumb);
    analog(XINPUT_GAMEPAD_A, kA);
    analog(XINPUT_GAMEPAD_B, kB);
    analog(XINPUT_GAMEPAD_X, kX);
    analog(XINPUT_GAMEPAD_Y, kY);
    // Xbox Black/White buttons map to the host shoulder buttons.
    analog(XINPUT_GAMEPAD_RIGHT_SHOULDER, kBlack);
    analog(XINPUT_GAMEPAD_LEFT_SHOULDER, kWhite);
    pad.analogButtons[kLeftTrigger] = std::max(pad.analogButtons[kLeftTrigger], host.bLeftTrigger);
    pad.analogButtons[kRightTrigger] = std::max(pad.analogButtons[kRightTrigger], host.bRightTrigger);
    pad.thumbLX = host.sThumbLX;
    pad.thumbLY = host.sThumbLY;
    pad.thumbRX = host.sThumbRX;
    pad.thumbRY = host.sThumbRY;
}

DWORD portOf(HANDLE device) {
    return static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(device)) - 1;
}

void __stdcall xXInitDevices(DWORD preallocTypeCount, void* preallocTypes) {
    // Mark a gamepad as inserted in port 0; XGetDeviceChanges reports it as a new insertion.
    DWORD mask = 1;
    {
        std::lock_guard lock(g_virtualMutex);
        for (int port = 1; port < kPorts; ++port) {
            mask |= g_virtualPresent[port] ? 1u << port : 0u;
        }
    }
    auto* gamepads = gamepadDeviceMasks();
    gamepads[0] = mask;
    gamepads[1] = mask;
    gamepads[2] = 0;
    logf("input: XInitDevices -> gamepads attached (port mask %lX; port 1 = keyboard/mouse/XInput)", mask);
}

HANDLE __stdcall xXInputOpen(void* deviceType, DWORD port, DWORD slot, void* pollingParameters) {
    if (reinterpret_cast<std::uintptr_t>(deviceType) != kGamepadDeviceType || port > 3) {
        SetLastError(kErrorDeviceNotConnected);
        return nullptr;
    }
    logf("input: XInputOpen(port %lu)", port);
    return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(port + 1));
}

void __stdcall xXInputClose(HANDLE device) {
}

DWORD __stdcall xXInputGetCapabilities(HANDLE device, XboxCapabilities* capabilities) {
    std::memset(capabilities, 0, sizeof(*capabilities));
    capabilities->subType = 1;
    std::memset(&capabilities->in, 0xFF, sizeof(capabilities->in));
    capabilities->rumbleLeft = 0xFFFF;
    capabilities->rumbleRight = 0xFFFF;
    return ERROR_SUCCESS;
}

DWORD __stdcall xXInputGetState(HANDLE device, XboxInputState* state) {
    const DWORD port = portOf(device);
    if (port >= kPorts) {
        return kErrorDeviceNotConnected;
    }
    XboxGamepad pad{};
    bool isVirtual = false;
    {
        std::lock_guard lock(g_virtualMutex);
        if (g_virtualPresent[port]) {
            pad = g_virtualState[port];
            isVirtual = true;
        }
    }
    if (!isVirtual) {
        readHostController(port, pad);
    }
    if (port == 0) {
        readKeyboardAndMouse(pad);
        applyScriptedInput(pad);
    }
    if (const PadFilter filter = g_padFilter) {
        filter(port, &pad);
    }
    if (std::memcmp(&pad, &g_lastState[port], sizeof(pad)) != 0) {
        g_lastState[port] = pad;
        ++g_packetNumber[port];
    }
    state->packetNumber = g_packetNumber[port];
    state->gamepad = pad;
    return ERROR_SUCCESS;
}

DWORD __stdcall xXInputSetState(HANDLE device, XboxFeedback* feedback) {
    XINPUT_VIBRATION vibration{feedback->rumbleLeft, feedback->rumbleRight};
    XInputSetState(portOf(device), &vibration);
    feedback->status = ERROR_SUCCESS;
    if (feedback->event != nullptr) {
        SetEvent(feedback->event);
    }
    return ERROR_SUCCESS;
}

} // namespace

void setVirtualPad(std::uint32_t port, const void* pad) {
    if (port == 0 || port >= kPorts) {
        return;
    }
    static_assert(sizeof(XboxGamepad) == 18, "CwPad layout");
    std::lock_guard lock(g_virtualMutex);
    const bool wasPresent = g_virtualPresent[port];
    g_virtualPresent[port] = pad != nullptr;
    if (pad != nullptr) {
        std::memcpy(&g_virtualState[port], pad, sizeof(XboxGamepad));
    }
    if (wasPresent != g_virtualPresent[port]) {
        // Plugged in or out: update the connected mask and flag the change for XGetDeviceChanges.
        auto* gamepads = gamepadDeviceMasks();
        gamepads[0] = pad != nullptr ? (gamepads[0] | (1u << port)) : (gamepads[0] & ~(1u << port));
        gamepads[1] |= 1u << port;
        logf("input: virtual gamepad %s port %u", pad != nullptr ? "plugged into" : "unplugged from", port + 1);
    }
}

void setPadFilter(PadFilter filter) {
    g_padFilter = filter;
}

void installInputHooks() {
    hookFunction(0x0032BDF8, reinterpret_cast<const void*>(&xXInitDevices), "XInitDevices");
    hookFunction(0x003332F7, reinterpret_cast<const void*>(&xXInputOpen), "XInputOpen");
    hookFunction(0x0033334D, reinterpret_cast<const void*>(&xXInputClose), "XInputClose");
    hookFunction(0x00333359, reinterpret_cast<const void*>(&xXInputGetCapabilities), "XInputGetCapabilities");
    hookFunction(0x00333537, reinterpret_cast<const void*>(&xXInputGetState), "XInputGetState");
    hookFunction(0x003335A3, reinterpret_cast<const void*>(&xXInputSetState), "XInputSetState");
}

} // namespace cw::hle
