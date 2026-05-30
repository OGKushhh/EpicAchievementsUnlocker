#include "pch.h"
#include "Overlay.h"
#include "Loader.h"
#include "achievement_manager_ui.h"
#include "achievement_manager.h"
#include "HotkeyHandler.h"
#include "d3d12hook.h"
#include "Config.h"
#include <thread>
#include <future>
#include <fstream>
#include <string>
#include <algorithm>
#include <cctype>

#define POPUP_DURATION_MS 4500

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace Overlay {

HRESULT(WINAPI* originalPresent)(IDXGISwapChain*, UINT, UINT);
HRESULT(WINAPI* originalResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
LRESULT(WINAPI* originalWindowProc)(HWND, UINT, WPARAM, LPARAM);

HWND                  gWindow             = nullptr;
ID3D11Device*         gD3D11Device        = nullptr;
ID3D11DeviceContext*  gContext            = nullptr;
ID3D11RenderTargetView* gRenderTargetView = nullptr;

bool bKieroInit              = false;
bool bInit                   = false;
bool bShowInitPopup          = true;
bool bShowAchievementManager = false;

static float g_MouseWheelDelta = 0.0f;

Achievements*             achievements      = nullptr;
UnlockAchievementFunction* unlockAchievement = nullptr;

// ── Win32 subclass WndProc (DX11 path) ───────────────────────────────────────
LRESULT WINAPI WindowProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_KEYDOWN) {
        if (GetKeyState(VK_SHIFT) & 0x8000 && wParam == VK_F5) {
            bShowInitPopup = false;
            bShowAchievementManager = !bShowAchievementManager;
            Logger::info("[HOTKEY] Shift+F5 pressed, overlay toggled");
            if (bShowAchievementManager)
                AchievementManagerUI::RequestFocus();
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
            }
            return 0;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000) && wParam == 'L') {
            Logger::info("[HOTKEY] Ctrl+Shift+L pressed");
            char path[MAX_PATH];
            GetModuleFileNameA(NULL, path, MAX_PATH);
            std::string dir = std::string(path).substr(0, std::string(path).find_last_of("\\/"));
            std::ifstream file(dir + "\\unlock_list.txt");
            if (file.is_open()) {
                std::string line; int count = 0;
                while (std::getline(file, line)) {
                    line.erase(0, line.find_first_not_of(" \t\r\n"));
                    line.erase(line.find_last_not_of(" \t\r\n") + 1);
                    if (line.empty()) continue;
                    AchievementManager::findAchievement(line.c_str(), [&](Overlay_Achievement& ach) {
                        if (ach.UnlockState == UnlockState::Locked) { unlockAchievement(&ach); count++; }
                    });
                }
                Logger::info("[HOTKEY] Unlocked %d achievements from list", count);
            }
            return 0;
        }
    }

    if (bShowAchievementManager) {
        UINT translatedMsg = uMsg;
        switch (uMsg) {
        case WM_POINTERDOWN:   translatedMsg = WM_LBUTTONDOWN; break;
        case WM_POINTERUP:     translatedMsg = WM_LBUTTONUP;   break;
        case WM_POINTERWHEEL:  translatedMsg = WM_MOUSEWHEEL;  break;
        case WM_POINTERUPDATE: translatedMsg = WM_SETCURSOR;   break;
        default: break;
        }
        ImGui_ImplWin32_WndProcHandler(hWnd, translatedMsg, wParam, lParam);
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureKeyboard || io.WantCaptureMouse)
            return 0;
    }
    return CallWindowProc(originalWindowProc, hWnd, uMsg, wParam, lParam);
}

void UpdateImGuiMouseInput() {
    ImGuiIO& io = ImGui::GetIO();
    POINT mousePos;
    GetCursorPos(&mousePos);
    ScreenToClient(gWindow, &mousePos);
    io.MousePos    = { (float)mousePos.x, (float)mousePos.y };
    io.MouseDown[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    io.MouseDown[1] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    io.MouseDown[2] = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
    io.MouseWheel   = g_MouseWheelDelta;
    g_MouseWheelDelta = 0.f;
}

void AccumulateMouseWheel(float delta) { g_MouseWheelDelta += delta; }

// ── DX11 hooked Present ───────────────────────────────────────────────────────
HRESULT WINAPI hookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (!bInit) {
        Logger::ovrly("hookedPresent: Initializing DX11 overlay");
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&gD3D11Device))) {
            gD3D11Device->GetImmediateContext(&gContext);
            DXGI_SWAP_CHAIN_DESC sd;
            pSwapChain->GetDesc(&sd);
            gWindow = sd.OutputWindow;

            ID3D11Texture2D* pBB;
            if (SUCCEEDED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBB))) {
                gD3D11Device->CreateRenderTargetView(pBB, NULL, &gRenderTargetView);
                pBB->Release();
            }

            originalWindowProc = (WNDPROC)SetWindowLongPtr(gWindow, GWLP_WNDPROC, (LONG_PTR)WindowProc);
            AchievementManagerUI::InitImGui(gWindow, gD3D11Device, gContext);
            bInit = true;
            Logger::ovrly("hookedPresent: DX11 overlay ready");
            Loader::AsyncLoadIcons();
        } else {
            bInit = true;
        }
    }

    if (gRenderTargetView) {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        UpdateImGuiMouseInput();
        ImGui::GetIO().MouseDrawCursor = bShowAchievementManager;
        if (bShowInitPopup)          AchievementManagerUI::DrawInitPopup();
        if (bShowAchievementManager) AchievementManagerUI::DrawAchievementList();
        ImGui::Render();
        gContext->OMSetRenderTargets(1, &gRenderTargetView, NULL);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    return originalPresent(pSwapChain, SyncInterval, Flags);
}

HRESULT WINAPI hookedResizeBuffer(IDXGISwapChain* pThis, UINT BufferCount,
    UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    if (bInit) {
        AchievementManagerUI::ShutdownImGui();
        if (originalWindowProc)
            SetWindowLongPtr(gWindow, GWLP_WNDPROC, (LONG_PTR)originalWindowProc);
        if (gRenderTargetView) { gRenderTargetView->Release(); gRenderTargetView = nullptr; }
        bInit = false;
    }
    return originalResizeBuffers(pThis, BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

#define D3D11_Present       8
#define D3D11_ResizeBuffers 13

static std::mutex initMutex;

// ── Init ──────────────────────────────────────────────────────────────────────
void Init(HMODULE hMod, Achievements* pAchievements, UnlockAchievementFunction* fnUnlock) {
    achievements      = pAchievements;
    unlockAchievement = fnUnlock;

    HotkeyHandler::Start();

    Logger::ovrly("Overlay::Init: Starting async initialization");
    static auto initJob = std::async(std::launch::async, []() {
        std::lock_guard<std::mutex> guard(initMutex);
        if (bKieroInit) return;

        // ── Wait for game window ─────────────────────────────────────────────
        // Launcher-based games (cmd → game.exe) load graphics DLLs before the
        // game window exists. We wait up to 10s for a visible non-console window.
        {
            DWORD pid = GetCurrentProcessId();
            Logger::ovrly("Overlay::Init: Waiting for game window (pid %u)...", pid);
            for (int i = 0; i < 200; i++) {
                struct Finder { DWORD pid; HWND found; };
                Finder f{ pid, nullptr };
                EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
                    auto* f = reinterpret_cast<Finder*>(lp);
                    DWORD wp = 0;
                    GetWindowThreadProcessId(hwnd, &wp);
                    if (wp == f->pid && IsWindowVisible(hwnd)) {
                        wchar_t cls[64]{};
                        GetClassNameW(hwnd, cls, 64);
                        if (wcscmp(cls, L"ConsoleWindowClass") != 0) {
                            f->found = hwnd; return FALSE;
                        }
                    }
                    return TRUE;
                }, (LPARAM)&f);
                if (f.found) { Logger::ovrly("Overlay::Init: Game window found (%p)", f.found); break; }
                Sleep(50);
            }
        }

        bool hasDX12 = GetModuleHandle(TEXT("d3d12.dll")) != NULL;
        bool hasDX11 = GetModuleHandle(TEXT("d3d11.dll")) != NULL;
        Logger::ovrly("Overlay::Init: d3d12.dll=%s  d3d11.dll=%s",
                      hasDX12 ? "YES" : "NO", hasDX11 ? "YES" : "NO");

        // ── DX12 path ────────────────────────────────────────────────────────
        if (hasDX12) {
            Logger::ovrly("Overlay::Init: Initializing D3D12Hook");
            D3D12Hook::Init();
            bKieroInit = true;

            static auto hidePopup = std::async(std::launch::async, [] {
                Sleep(POPUP_DURATION_MS); bShowInitPopup = false;
            });
            return; // DX12 games often also load d3d11.dll — don't double-hook
        }

        // ── DX11 path (kiero) ────────────────────────────────────────────────
        if (hasDX11) {
            auto result = kiero::init(kiero::RenderType::D3D11);
            if (result != kiero::Status::Success) {
                Logger::error("Kiero DX11 init failed: %d", result);
                return;
            }
            bKieroInit = true;
            Logger::ovrly("Kiero: DX11 initialized");
            kiero::bind(D3D11_Present,       (void**)&originalPresent,       hookedPresent);
            kiero::bind(D3D11_ResizeBuffers, (void**)&originalResizeBuffers, hookedResizeBuffer);
            Logger::ovrly("Kiero: Hooked Present and ResizeBuffers");

            static auto hidePopup = std::async(std::launch::async, [] {
                Sleep(POPUP_DURATION_MS); bShowInitPopup = false;
            });
        }
    });
}

// ── Shutdown ──────────────────────────────────────────────────────────────────
void Shutdown() {
    bool hasDX12 = GetModuleHandle(TEXT("d3d12.dll")) != NULL;
    if (hasDX12) {
        D3D12Hook::Shutdown();
    } else {
        AchievementManagerUI::ShutdownImGui();
        if (originalWindowProc)
            SetWindowLongPtr(gWindow, GWLP_WNDPROC, (LONG_PTR)originalWindowProc);
        kiero::shutdown();
        Logger::ovrly("Kiero: Shutdown");
    }
    HotkeyHandler::Stop();
}

} // namespace Overlay
