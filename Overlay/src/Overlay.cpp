#include "pch.h"
#include "Overlay.h"
#include "Loader.h"
#include "achievement_manager_ui.h"
#include "achievement_manager.h"
#include "HotkeyHandler.h"
#include "Config.h"   // added for EnableOverlay check
#include <thread>
#include <future>
#include <fstream>
#include <string>
#include <algorithm>
#include <cctype>

#define POPUP_DURATION_MS	4500

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Overlay {

HRESULT(WINAPI* originalPresent) (IDXGISwapChain*, UINT, UINT);
HRESULT(WINAPI* originalResizeBuffers) (IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
LRESULT(WINAPI* originalWindowProc)(HWND, UINT, WPARAM, LPARAM);

HWND gWindow = nullptr;
ID3D11Device* gD3D11Device = nullptr;
ID3D11DeviceContext* gContext = nullptr;
ID3D11RenderTargetView* gRenderTargetView = nullptr;

bool bKieroInit = false;
bool bInit = false;
bool bShowInitPopup = true;
bool bShowAchievementManager = false;

static float g_MouseWheelDelta = 0.0f;

Achievements* achievements = nullptr;
UnlockAchievementFunction* unlockAchievement = nullptr;

LRESULT WINAPI WindowProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_KEYDOWN) {
        if (GetKeyState(VK_SHIFT) & 0x8000 && wParam == VK_F5) {
            bShowInitPopup = false;
            bShowAchievementManager = !bShowAchievementManager;
            Logger::info("[HOTKEY] Shift+F5 pressed, overlay toggled");
            if (bShowAchievementManager) {
                AchievementManagerUI::RequestFocus();
            }
            return 0;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000) && wParam == 'U') {
            Logger::info("[HOTKEY] Ctrl+Shift+U pressed - unlocking all");
            if (achievements) {
                int count = 0;
                for (auto& ach : *achievements) {
                    if (ach.UnlockState == UnlockState::Locked) {
                        unlockAchievement(&ach);
                        count++;
                    }
                }
                Logger::info("[HOTKEY] Unlocked %d achievements.", count);
            } else {
                Logger::error("[HOTKEY] Achievements list is NULL");
            }
            return 0;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000) && wParam == 'L') {
            Logger::info("[HOTKEY] Ctrl+Shift+L pressed - reading unlock list");
            char path[MAX_PATH];
            GetModuleFileNameA(NULL, path, MAX_PATH);
            std::string exePath(path);
            std::string dir = exePath.substr(0, exePath.find_last_of("\\/"));
            std::string listFile = dir + "\\unlock_list.txt";
            
            std::ifstream file(listFile);
            if (file.is_open()) {
                std::string line;
                int count = 0;
                while (std::getline(file, line)) {
                    line.erase(0, line.find_first_not_of(" \t\r\n"));
                    line.erase(line.find_last_not_of(" \t\r\n") + 1);
                    if (line.empty()) continue;
                    AchievementManager::findAchievement(line.c_str(), [&](Overlay_Achievement& ach) {
                        if (ach.UnlockState == UnlockState::Locked) {
                            unlockAchievement(&ach);
                            count++;
                        }
                    });
                }
                file.close();
                Logger::info("[HOTKEY] Unlocked %d achievements from %s", count, listFile.c_str());
            } else {
                Logger::error("[HOTKEY] Could not open %s", listFile.c_str());
            }
            return 0;
        }
    }

    if (bShowAchievementManager) {
        UINT translatedMsg = uMsg;
        switch (uMsg) {
        case WM_POINTERDOWN: translatedMsg = WM_LBUTTONDOWN; break;
        case WM_POINTERUP:   translatedMsg = WM_LBUTTONUP;   break;
        case WM_POINTERWHEEL:translatedMsg = WM_MOUSEWHEEL;  break;
        case WM_POINTERUPDATE:translatedMsg = WM_SETCURSOR;  break;
        default: break;
        }
        ImGui_ImplWin32_WndProcHandler(hWnd, translatedMsg, wParam, lParam);
        
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureKeyboard || io.WantCaptureMouse) {
            return 0;
        }
    }
    return CallWindowProc(originalWindowProc, hWnd, uMsg, wParam, lParam);
}

void UpdateImGuiMouseInput() {
    ImGuiIO& io = ImGui::GetIO();
    POINT mousePos;
    GetCursorPos(&mousePos);
    ScreenToClient(gWindow, &mousePos);
    io.MousePos.x = (float)mousePos.x;
    io.MousePos.y = (float)mousePos.y;
    io.MouseDown[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    io.MouseDown[1] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    io.MouseDown[2] = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
    io.MouseWheel = g_MouseWheelDelta;
    g_MouseWheelDelta = 0.0f;
}

void AccumulateMouseWheel(float delta) {
    g_MouseWheelDelta += delta;
}

HRESULT WINAPI hookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (!bInit) {
        Logger::ovrly("hookedPresent: Initializing overlay");
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&gD3D11Device))) {
            gD3D11Device->GetImmediateContext(&gContext);
            DXGI_SWAP_CHAIN_DESC sd;
            pSwapChain->GetDesc(&sd);
            gWindow = sd.OutputWindow;
            Logger::ovrly("hookedPresent: Got D3D11 device, window handle: %p", gWindow);

            ID3D11Texture2D* pBackBuffer;
            if (SUCCEEDED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer))) {
                if (SUCCEEDED(gD3D11Device->CreateRenderTargetView(pBackBuffer, NULL, &gRenderTargetView))) {
                    Logger::ovrly("hookedPresent: Created render target view");
                } else {
                    Logger::error("hookedPresent: Failed to create render target view");
                }
                pBackBuffer->Release();
            } else {
                Logger::error("hookedPresent: Failed to get back buffer");
                return originalPresent(pSwapChain, SyncInterval, Flags);
            }

            originalWindowProc = (WNDPROC)SetWindowLongPtr(gWindow, GWLP_WNDPROC, (LONG_PTR)WindowProc);
            if (originalWindowProc == NULL) {
                Logger::error("Failed to SetWindowLongPtr. Error code: %d", GetLastError());
                return originalPresent(pSwapChain, SyncInterval, Flags);
            }

            Logger::ovrly("hookedPresent: Initializing ImGui");
            AchievementManagerUI::InitImGui(gWindow, gD3D11Device, gContext);
            bInit = true;
            Logger::ovrly("hookedPresent: Overlay initialization complete");
            Loader::AsyncLoadIcons();
        }
        else {
            Logger::error("hookedPresent: Failed to get D3D11 device");
            // Hotkeys already started in Init() — nothing extra needed here
            bInit = true;
            return originalPresent(pSwapChain, SyncInterval, Flags);
        }
    }

    if (gRenderTargetView) {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        UpdateImGuiMouseInput();
        ImGui::GetIO().MouseDrawCursor = bShowAchievementManager;
        if (bShowInitPopup)
            AchievementManagerUI::DrawInitPopup();
        if (bShowAchievementManager)
            AchievementManagerUI::DrawAchievementList();
        ImGui::Render();
        gContext->OMSetRenderTargets(1, &gRenderTargetView, NULL);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    return originalPresent(pSwapChain, SyncInterval, Flags);
}

HRESULT WINAPI hookedResizeBuffer(IDXGISwapChain* pThis, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    Logger::debug("hookedResizeBuffer");
    if (bInit) {
        AchievementManagerUI::ShutdownImGui();
        if (originalWindowProc) {
            SetWindowLongPtr(gWindow, GWLP_WNDPROC, (LONG_PTR)originalWindowProc);
        }
        if (gRenderTargetView) {
            gRenderTargetView->Release();
            gRenderTargetView = nullptr;
        }
        bInit = false;
    }
    return originalResizeBuffers(pThis, BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

#define D3D11_Present      8
#define D3D11_ResizeBuffers 13

static std::mutex initMutex;

void Init(HMODULE hMod, Achievements* pAchievements, UnlockAchievementFunction* fnUnlockAchievement) {
    achievements = pAchievements;
    unlockAchievement = fnUnlockAchievement;

    // Hotkeys always start regardless of EnableOverlay — the INI flag controls
    // the ImGui window only, not the Ctrl+Shift+U / Ctrl+Shift+L functionality.
    HotkeyHandler::Start(); // internally guarded: safe to call multiple times

    Logger::ovrly("Overlay::Init: Starting async initialization");
    static auto initJob = std::async(std::launch::async, []() {
        Logger::ovrly("Overlay::Init: Async init thread started");
        {
            std::lock_guard<std::mutex> guard(initMutex);
            if (bKieroInit) {
                Logger::ovrly("Overlay::Init: Already initialized");
                return;
            }
            Logger::ovrly("Overlay::Init: Calling kiero::init");
            auto result = kiero::init(kiero::RenderType::D3D11);
            if (result != kiero::Status::Success) {
                Logger::debug("Kiero: result code = %d", result);
                if (result == kiero::Status::ModuleNotFoundError)
                    Logger::error("Failed to initialize kiero. Are you sure you are running a DirectX 11 game?");
                else
                    Logger::error("Failed to initialize kiero. Error code: %d", result);
                // Hotkeys already started, no overlay needed
                return;
            }
            bKieroInit = true;
            Logger::ovrly("Kiero: Successfully initialized");
            kiero::bind(D3D11_Present, (void**)&originalPresent, hookedPresent);
            Logger::ovrly("Kiero: Successfully hooked Present");
            kiero::bind(D3D11_ResizeBuffers, (void**)&originalResizeBuffers, hookedResizeBuffer);
            Logger::ovrly("Kiero: Successfully hooked ResizeBuffers");
            static auto hidePopupJob = std::async(std::launch::async, [&]() {
                Sleep(POPUP_DURATION_MS);
                bShowInitPopup = false;
            });
        }
    });
}

void Shutdown() {
    AchievementManagerUI::ShutdownImGui();
    if (originalWindowProc) {
        SetWindowLongPtr(gWindow, GWLP_WNDPROC, (LONG_PTR)originalWindowProc);
    }
    HotkeyHandler::Stop();
    kiero::shutdown();
    Logger::ovrly("Kiero: Shutdown");
}

} // namespace Overlay