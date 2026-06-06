#include "pch.h"
#include "HotkeyHandler.h"
#include "achievement_manager.h"
#include "achievement_manager_ui.h"
#include "Overlay.h"
#include <fstream>
#include <string>
#include <atomic>
#include <thread>

namespace HotkeyHandler {

static HWND hwndHotkey = nullptr;
static const UINT HOTKEY_ID_UNLOCK_ALL  = 1;
static const UINT HOTKEY_ID_UNLOCK_LIST = 2;
static std::atomic<bool> keepRunning{false};
static std::thread messageThread;

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

static void UnlockAll() {
    if (!Overlay::achievements) { Logger::error("[HOTKEY] achievements NULL"); return; }
    int n = 0;
    for (auto& ach : *Overlay::achievements)
        if (ach.UnlockState == UnlockState::Locked) { Overlay::unlockAchievement(&ach); n++; }
    Logger::info("[HOTKEY] Unlocked %d achievements.", n);
}

static void UnlockFromFile() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string dir = std::string(path).substr(0, std::string(path).find_last_of("\\/"));
    std::ifstream file(dir + "\\unlock_list.txt");
    if (!file.is_open()) { Logger::error("[HOTKEY] Could not open unlock_list.txt"); return; }
    std::string line; int n = 0;
    while (std::getline(file, line)) {
        std::string id = trim(line);
        if (id.empty()) continue;
        AchievementManager::findAchievement(id.c_str(), [&](Overlay_Achievement& ach) {
            if (ach.UnlockState == UnlockState::Locked) { Overlay::unlockAchievement(&ach); n++; }
        });
    }
    Logger::info("[HOTKEY] Unlocked %d achievements from file.", n);
}

// ── Raw keyboard handler ──────────────────────────────────────────────────────
// RIDEV_INPUTSINK delivers WM_INPUT to our message window even when it is not
// the foreground window — completely bypassing the game's message loop.
// This is the only reliable way to intercept Shift+F5 in an injected DLL.
static bool sShiftDown = false;
static bool sF5Down    = false;

static void HandleRawKeyboard(RAWKEYBOARD& kb) {
    bool down = !(kb.Flags & RI_KEY_BREAK);
    USHORT vk = kb.VKey;
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) sShiftDown = down;
    if (vk == VK_F5) {
        if (down && !sF5Down && sShiftDown) {
            Overlay::bShowInitPopup          = false;
            Overlay::bShowAchievementManager = !Overlay::bShowAchievementManager;
            Logger::info("[HOTKEY] Shift+F5 (raw) — overlay %s",
                         Overlay::bShowAchievementManager ? "shown" : "hidden");
            if (Overlay::bShowAchievementManager) {
                Overlay::OverlayOpen();
                AchievementManagerUI::RequestFocus();
            } else {
                Overlay::OverlayClose();
            }
        }
        sF5Down = down;
    }
}

LRESULT CALLBACK HotkeyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_HOTKEY) {
        if      (wParam == HOTKEY_ID_UNLOCK_ALL)  UnlockAll();
        else if (wParam == HOTKEY_ID_UNLOCK_LIST) UnlockFromFile();
        return 0;
    }
    if (msg == WM_INPUT) {
        UINT size = 0;
        GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
        if (size > 0) {
            RAWINPUT* raw = (RAWINPUT*)_alloca(size);
            if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, raw, &size, sizeof(RAWINPUTHEADER)) == size)
                if (raw->header.dwType == RIM_TYPEKEYBOARD)
                    HandleRawKeyboard(raw->data.keyboard);
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static void MessageLoop() {
    WNDCLASSEX wc{ sizeof(wc) };
    wc.lpfnWndProc   = HotkeyWndProc;
    wc.hInstance     = GetModuleHandle(NULL);
    wc.lpszClassName = L"ScreamAPI_HotkeyWindow";
    if (!RegisterClassEx(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            Logger::error("[HOTKEY] RegisterClassEx failed"); keepRunning = false; return;
        }
    }

    hwndHotkey = CreateWindowEx(0, wc.lpszClassName, L"HotkeyWindow", 0,
                                0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (!hwndHotkey) {
        Logger::error("[HOTKEY] CreateWindowEx failed"); keepRunning = false; return;
    }

    // Ctrl+Shift+U / Ctrl+Shift+L via RegisterHotKey (system-level, reliable)
    if (RegisterHotKey(hwndHotkey, HOTKEY_ID_UNLOCK_ALL,  MOD_CONTROL|MOD_SHIFT|MOD_NOREPEAT, 'U'))
        Logger::info("[HOTKEY] Ctrl+Shift+U registered.");
    else Logger::error("[HOTKEY] Ctrl+Shift+U failed (%d)", GetLastError());

    if (RegisterHotKey(hwndHotkey, HOTKEY_ID_UNLOCK_LIST, MOD_CONTROL|MOD_SHIFT|MOD_NOREPEAT, 'L'))
        Logger::info("[HOTKEY] Ctrl+Shift+L registered.");
    else Logger::error("[HOTKEY] Ctrl+Shift+L failed (%d)", GetLastError());

    // Shift+F5 via raw input with RIDEV_INPUTSINK
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01; // HID_USAGE_PAGE_GENERIC
    rid.usUsage     = 0x06; // HID_USAGE_GENERIC_KEYBOARD
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = hwndHotkey;
    if (RegisterRawInputDevices(&rid, 1, sizeof(rid)))
        Logger::info("[HOTKEY] Raw keyboard input registered (RIDEV_INPUTSINK).");
    else Logger::error("[HOTKEY] RegisterRawInputDevices failed (%d)", GetLastError());

    MSG msg;
    while (keepRunning) {
        BOOL ret = GetMessage(&msg, NULL, 0, 0);
        if (ret == 0 || ret == -1) break;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnregisterHotKey(hwndHotkey, HOTKEY_ID_UNLOCK_ALL);
    UnregisterHotKey(hwndHotkey, HOTKEY_ID_UNLOCK_LIST);
    RAWINPUTDEVICE ridRemove{ 0x01, 0x06, RIDEV_REMOVE, nullptr };
    RegisterRawInputDevices(&ridRemove, 1, sizeof(ridRemove));
    DestroyWindow(hwndHotkey);
    hwndHotkey = nullptr;
    Logger::info("[HOTKEY] Stopped.");
}

void Start() {
    if (keepRunning.exchange(true)) return;
    messageThread = std::thread(MessageLoop);
    messageThread.detach();
    Logger::info("[HOTKEY] Message loop thread started.");
}

void Stop() {
    keepRunning = false;
    if (hwndHotkey) PostMessage(hwndHotkey, WM_QUIT, 0, 0);
}

} // namespace HotkeyHandler
