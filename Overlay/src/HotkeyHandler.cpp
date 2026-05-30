#include "pch.h"
#include "HotkeyHandler.h"
#include "achievement_manager.h"
#include "Overlay.h"
#include <fstream>
#include <string>
#include <algorithm>
#include <cctype>
#include <atomic>
#include <thread>

namespace HotkeyHandler {

    static HWND hwndHotkey = nullptr;
    static const UINT HOTKEY_ID_UNLOCK_ALL  = 1;
    static const UINT HOTKEY_ID_UNLOCK_LIST = 2;
    static std::atomic<bool> keepRunning{false};
    static std::thread messageThread;

    static std::string trim(const std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    static void UnlockAllAchievements() {
        if (Overlay::achievements) {
            int count = 0;
            for (auto& ach : *Overlay::achievements) {
                if (ach.UnlockState == UnlockState::Locked) {
                    Overlay::unlockAchievement(&ach);
                    count++;
                    Logger::info("[HOTKEY] Unlocking: %s", ach.AchievementId);
                }
            }
            Logger::info("[HOTKEY] Unlocked %d achievements.", count);
        } else {
            Logger::error("[HOTKEY] Achievements list not available.");
        }
    }

    static void UnlockFromFile() {
        char path[MAX_PATH];
        GetModuleFileNameA(NULL, path, MAX_PATH);
        std::string exePath(path);
        std::string dir = exePath.substr(0, exePath.find_last_of("\\/"));
        std::string listFile = dir + "\\unlock_list.txt";

        std::ifstream file(listFile);
        if (!file.is_open()) {
            Logger::error("[HOTKEY] Could not open %s", listFile.c_str());
            return;
        }

        std::string line;
        int count = 0;
        while (std::getline(file, line)) {
            std::string id = trim(line);
            if (id.empty()) continue;
            AchievementManager::findAchievement(id.c_str(), [&](Overlay_Achievement& ach) {
                if (ach.UnlockState == UnlockState::Locked) {
                    Overlay::unlockAchievement(&ach);
                    count++;
                    Logger::info("[HOTKEY] Unlocking specific: %s", ach.AchievementId);
                } else {
                    Logger::info("[HOTKEY] Already unlocked: %s", ach.AchievementId);
                }
            });
        }
        file.close();
        Logger::info("[HOTKEY] Unlocked %d achievements from %s", count, listFile.c_str());
    }

    LRESULT CALLBACK HotkeyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_HOTKEY) {
            if (wParam == HOTKEY_ID_UNLOCK_ALL) {
                UnlockAllAchievements();
                return 0;
            } else if (wParam == HOTKEY_ID_UNLOCK_LIST) {
                UnlockFromFile();
                return 0;
            }
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    // FIX (Bug 3): The window, RegisterHotKey, and GetMessage must all live on the
    // same thread. We do all three here inside the message loop thread itself.
    static void MessageLoop() {
        // Register window class
        WNDCLASSEX wc = {};
        wc.cbSize        = sizeof(WNDCLASSEX);
        wc.lpfnWndProc   = HotkeyWndProc;
        wc.hInstance     = GetModuleHandle(NULL);
        wc.lpszClassName = L"ScreamAPI_HotkeyWindow";
        if (!RegisterClassEx(&wc)) {
            DWORD err = GetLastError();
            // ERROR_CLASS_ALREADY_EXISTS (1410) is fine if Start() was somehow called twice
            if (err != ERROR_CLASS_ALREADY_EXISTS) {
                Logger::error("[HOTKEY] Failed to register window class (error %d)", err);
                keepRunning = false;
                return;
            }
        }

        hwndHotkey = CreateWindowEx(0, wc.lpszClassName, L"HotkeyWindow", 0,
                                    0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
        if (!hwndHotkey) {
            Logger::error("[HOTKEY] Failed to create message window (error %d)", GetLastError());
            keepRunning = false;
            return;
        }

        // RegisterHotKey on THIS thread — WM_HOTKEY will arrive in this thread's queue
        if (!RegisterHotKey(hwndHotkey, HOTKEY_ID_UNLOCK_ALL, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'U'))
            Logger::error("[HOTKEY] Failed to register Ctrl+Shift+U (error %d)", GetLastError());
        else
            Logger::info("[HOTKEY] Ctrl+Shift+U registered.");

        if (!RegisterHotKey(hwndHotkey, HOTKEY_ID_UNLOCK_LIST, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'L'))
            Logger::error("[HOTKEY] Failed to register Ctrl+Shift+L (error %d)", GetLastError());
        else
            Logger::info("[HOTKEY] Ctrl+Shift+L registered.");

        MSG msg;
        while (keepRunning) {
            // GetMessage blocks until a message arrives for THIS thread
            BOOL ret = GetMessage(&msg, NULL, 0, 0);
            if (ret == 0 || ret == -1) break;   // WM_QUIT or error
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        UnregisterHotKey(hwndHotkey, HOTKEY_ID_UNLOCK_ALL);
        UnregisterHotKey(hwndHotkey, HOTKEY_ID_UNLOCK_LIST);
        DestroyWindow(hwndHotkey);
        hwndHotkey = nullptr;
        Logger::info("[HOTKEY] Message loop exited.");
    }

    void Start() {
        // FIX (Bug 2): guard with keepRunning so double-calls are silently ignored
        if (keepRunning.exchange(true)) {
            Logger::info("[HOTKEY] Already running, ignoring duplicate Start().");
            return;
        }
        messageThread = std::thread(MessageLoop);
        messageThread.detach();
        Logger::info("[HOTKEY] Message loop thread started.");
    }

    void Stop() {
        keepRunning = false;
        if (hwndHotkey) {
            PostMessage(hwndHotkey, WM_QUIT, 0, 0);
        }
        Logger::info("[HOTKEY] Stopped.");
    }

}
