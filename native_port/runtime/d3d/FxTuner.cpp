#include "d3d/FxTuner.h"

#include "Log.h"

#include <windows.h>
#include <commctrl.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#pragma comment(lib, "comctl32.lib")

namespace cw::d3d {

namespace {

struct Entry {
    const char* key;  // name in fx_tuning.ini
    const char* label;
    float minimum, maximum, fallback;
};

// Defaults are the look tuned in play (fx_tuning.ini of 2026-10-04); multipliers apply on top of each planet's grade.
const Entry kEntries[FxSettingCount] = {
    {"exposure_day", "Brightness (day)", 0.3f, 1.5f, 0.9f},
    {"exposure_night", "Brightness (night)", 0.1f, 1.0f, 0.47f},
    {"contrast", "Contrast", 0.6f, 1.6f, 1.06f},
    {"saturation", "Color saturation", 0.0f, 2.0f, 1.1f},
    {"bloom_day", "Bloom glow (day)", 0.0f, 3.0f, 1.3f},
    {"bloom_night", "Bloom glow (night)", 0.0f, 3.0f, 0.9f},
    {"bloom_threshold_day", "Bloom starts at (day)", 0.2f, 1.2f, 0.75f},
    {"bloom_threshold_night", "Bloom starts at (night)", 0.1f, 1.2f, 0.57f},
    {"vignette_day", "Dark corners (day)", 0.0f, 0.6f, 0.18f},
    {"vignette_night", "Dark corners (night)", 0.0f, 0.6f, 0.3f},
    {"shadow_darkness_day", "Shadow lightness (day, 0 = black)", 0.2f, 1.2f, 0.85f},
    {"shadow_darkness_night", "Shadow lightness (night)", 0.2f, 1.2f, 0.83f},
    {"shadow_near", "Sharp shadow range", 20.0f, 200.0f, 60.0f},
    {"shadow_far", "Shadow distance", 100.0f, 1200.0f, 350.0f},
    {"occlusion_strength", "Contact shadows (occlusion)", 0.0f, 2.0f, 1.18f},
    {"occlusion_radius", "Contact shadow size", 1.0f, 20.0f, 6.57f},
    {"light_day", "Effect light strength (day)", 0.0f, 4.0f, 0.86f},
    {"light_night", "Effect light strength (night)", 0.0f, 6.0f, 2.18f},
    {"light_reach", "Effect light reach", 0.2f, 3.0f, 1.44f},
    {"light_brightness", "Halo / power-up light", 0.0f, 3.0f, 1.37f},
    {"glow_day", "Effect brightness (day)", 0.0f, 2.0f, 0.8f},
    {"glow_night", "Effect brightness (night)", 0.0f, 2.0f, 1.46f},
    {"player_glow", "Own tank's glow", 0.0f, 1.0f, 0.47f},
    {"bounce", "Bounce light (color from nearby surfaces)", 0.0f, 2.0f, 0.35f},
    {"reflections", "Reflections (ice, floors, metal)", 0.0f, 3.0f, 1.0f},
    {"reflection_reach", "Reflection distance", 20.0f, 800.0f, 300.0f},
};

std::atomic<float> g_values[FxSettingCount];
std::atomic<bool> g_loaded{false};
HWND g_window = nullptr;
HWND g_sliders[FxSettingCount] = {};
HWND g_texts[FxSettingCount] = {};
HWND g_status = nullptr;
constexpr int kSteps = 1000;
constexpr int kSave = 1, kReset = 2, kReload = 3;

std::string iniPath() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string directory = path;
    directory = directory.substr(0, directory.find_last_of("\\/") + 1);
    return directory + "fx_tuning.ini";
}

void resetDefaults() {
    for (int index = 0; index < FxSettingCount; ++index) {
        g_values[index] = kEntries[index].fallback;
    }
}

bool load() {
    FILE* file = std::fopen(iniPath().c_str(), "r");
    if (file == nullptr) {
        return false;
    }
    char line[256];
    while (std::fgets(line, sizeof(line), file)) {
        char key[128] = {};
        float value = 0;
        if (std::sscanf(line, " %127[^= ] = %f", key, &value) == 2) {
            for (int index = 0; index < FxSettingCount; ++index) {
                if (std::strcmp(key, kEntries[index].key) == 0) {
                    g_values[index] = std::fmin(std::fmax(value, kEntries[index].minimum), kEntries[index].maximum);
                }
            }
        }
    }
    std::fclose(file);
    return true;
}

bool save() {
    FILE* file = std::fopen(iniPath().c_str(), "w");
    if (file == nullptr) {
        return false;
    }
    std::fprintf(file, "; Frame effect settings (Direct3D 11 renderer). F10 in game opens the tuning window.\n");
    for (int index = 0; index < FxSettingCount; ++index) {
        std::fprintf(file, "%s = %.3f\n", kEntries[index].key, g_values[index].load());
    }
    std::fclose(file);
    return true;
}

void ensureLoaded() {
    bool expected = false;
    if (g_loaded.compare_exchange_strong(expected, true)) {
        resetDefaults();
        if (load()) {
            logf("d3d11: frame effect settings from %s", iniPath().c_str());
        }
    }
}

int toStep(int index, float value) {
    const Entry& entry = kEntries[index];
    return static_cast<int>(std::lround((value - entry.minimum) / (entry.maximum - entry.minimum) * kSteps));
}

void showValue(int index) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", g_values[index].load());
    SetWindowTextA(g_texts[index], text);
}

void syncControls() {
    for (int index = 0; index < FxSettingCount; ++index) {
        SendMessageW(g_sliders[index], TBM_SETPOS, TRUE, toStep(index, g_values[index]));
        showValue(index);
    }
}

void setStatus(const char* text) {
    SetWindowTextA(g_status, text);
}

LRESULT CALLBACK tunerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_HSCROLL:
        for (int index = 0; index < FxSettingCount; ++index) {
            if (reinterpret_cast<HWND>(lParam) == g_sliders[index]) {
                const int step = static_cast<int>(SendMessageW(g_sliders[index], TBM_GETPOS, 0, 0));
                const Entry& entry = kEntries[index];
                g_values[index] = entry.minimum + (entry.maximum - entry.minimum) * step / kSteps;
                showValue(index);
                setStatus("Changed (not saved yet)");
            }
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kSave: setStatus(save() ? ("Saved to " + iniPath()).c_str() : "Could not save"); break;
        case kReset: resetDefaults(); syncControls(); setStatus("Defaults restored (not saved yet)"); break;
        case kReload: resetDefaults(); load(); syncControls(); setStatus("Reloaded the saved settings"); break;
        default: break;
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        g_window = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

DWORD WINAPI tunerThread(void*) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = tunerProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = L"CloneWarsFxTuner";
    RegisterClassW(&windowClass);

    constexpr int kRow = 30, kLabel = 250, kSlider = 300, kValue = 60, kMargin = 12;
    const int height = kMargin * 2 + FxSettingCount * kRow + 80;
    RECT rect{0, 0, kMargin * 3 + kLabel + kSlider + kValue, height};
    AdjustWindowRect(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    g_window = CreateWindowW(windowClass.lpszClassName, L"Clone Wars - lighting and effects", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        40, 40, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, windowClass.hInstance, nullptr);
    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    auto child = [&](const char* type, const char* text, DWORD style, int x, int y, int w, int h, int id) {
        HWND control = CreateWindowExA(0, type, text,
            WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), windowClass.hInstance, nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return control;
    };
    for (int index = 0; index < FxSettingCount; ++index) {
        const int y = kMargin + index * kRow;
        child("STATIC", kEntries[index].label, SS_LEFT, kMargin, y + 6, kLabel, 20, 0);
        g_sliders[index] = child(TRACKBAR_CLASSA, "", TBS_HORZ | TBS_NOTICKS, kMargin * 2 + kLabel, y, kSlider, 26, 100 + index);
        SendMessageW(g_sliders[index], TBM_SETRANGE, TRUE, MAKELPARAM(0, kSteps));
        SendMessageW(g_sliders[index], TBM_SETPAGESIZE, 0, kSteps / 20);
        g_texts[index] = child("STATIC", "", SS_LEFT, kMargin * 3 + kLabel + kSlider - 10, y + 6, kValue, 20, 0);
    }
    const int buttons = kMargin + FxSettingCount * kRow + 10;
    child("BUTTON", "Save", BS_DEFPUSHBUTTON, kMargin, buttons, 110, 30, kSave);
    child("BUTTON", "Reset to defaults", BS_PUSHBUTTON, kMargin + 120, buttons, 150, 30, kReset);
    child("BUTTON", "Reload saved", BS_PUSHBUTTON, kMargin + 280, buttons, 130, 30, kReload);
    g_status = child("STATIC", "Drag a slider; the game updates at once.", SS_LEFT, kMargin, buttons + 40, kLabel + kSlider + kValue, 20, 0);
    syncControls();

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

}  // namespace

float fx(FxSetting setting) {
    ensureLoaded();
    return g_values[setting].load(std::memory_order_relaxed);
}

void toggleFxTuner() {
    ensureLoaded();
    if (g_window != nullptr) {
        PostMessageW(g_window, WM_CLOSE, 0, 0);
        return;
    }
    if (HANDLE thread = CreateThread(nullptr, 0, tunerThread, nullptr, 0, nullptr)) {
        CloseHandle(thread);
    }
}

void startFxTunerIfRequested() {
    if (const char* value = std::getenv("CW_FX_TUNER"); value != nullptr && std::strcmp(value, "0") != 0) {
        toggleFxTuner();
    }
}

}  // namespace cw::d3d
