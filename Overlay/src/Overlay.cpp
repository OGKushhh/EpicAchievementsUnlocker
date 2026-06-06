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

HWND                    gWindow           = nullptr;
ID3D11Device*           gD3D11Device      = nullptr;
ID3D11DeviceContext*    gContext          = nullptr;
ID3D11RenderTargetView* gRenderTargetView = nullptr;

bool bKieroInit              = false;
bool bInit                   = false;
bool bShowInitPopup          = true;
bool bShowAchievementManager = false;

// Saved cursor clip rect — restored when overlay closes
static RECT g_savedClipRect = {};
static bool g_clipSaved     = false;

// Mouse wheel accumulator — drained in UpdateImGuiMouseInput
static float g_MouseWheelDelta = 0.0f;

Achievements*              achievements      = nullptr;
UnlockAchievementFunction* unlockAchievement = nullptr;

// ── Manual mouse input — ImGui 1.92.9 event API ───────────────────────────────
static void UpdateImGuiMouseInput() {
    ImGuiIO& io = ImGui::GetIO();

    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(gWindow, &pt);
    io.AddMousePosEvent((float)pt.x, (float)pt.y);

    io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON)  & 0x8000) != 0);
    io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON)  & 0x8000) != 0);
    io.AddMouseButtonEvent(2, (GetAsyncKeyState(VK_MBUTTON)  & 0x8000) != 0);
    io.AddMouseButtonEvent(3, (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0);
    io.AddMouseButtonEvent(4, (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0);

    io.AddMouseWheelEvent(0.0f, g_MouseWheelDelta);
    g_MouseWheelDelta = 0.0f;
}

// ── Cursor clip management ────────────────────────────────────────────────────
void OverlayOpen() {
    g_clipSaved = (GetClipCursor(&g_savedClipRect) != FALSE);
    ClipCursor(nullptr);
    ReleaseCapture();
    ::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
    Logger::ovrly("[OVERLAY] Opened — cursor clip released");
}

void OverlayClose() {
    if (g_clipSaved)
        ClipCursor(&g_savedClipRect);
    else
        ClipCursor(nullptr);
    g_clipSaved = false;
    Logger::ovrly("[OVERLAY] Closed — cursor clip restored");
}

// ── Win32 subclass WndProc ────────────────────────────────────────────────────
// Shift+F5 handled EXCLUSIVELY by HotkeyHandler (raw input) to prevent double-fire.
LRESULT WINAPI WindowProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {

    // ── Hotkeys (always active) ───────────────────────────────────────────────
    if (uMsg == WM_KEYDOWN) {
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

    // Accumulate wheel delta — drained in UpdateImGuiMouseInput
    if (uMsg == WM_MOUSEWHEEL) {
        g_MouseWheelDelta += (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;
        return 0;
    }

    // ── Overlay input ─────────────────────────────────────────────────────────
    if (bShowAchievementManager) {
        // Prevent game from hiding cursor
        if (uMsg == WM_SETCURSOR) {
            ::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
            return TRUE;
        }

        // Forward messages to ImGui event queue
        ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam);

        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse || io.WantCaptureKeyboard)
            return 0;
    }

    return CallWindowProc(originalWindowProc, hWnd, uMsg, wParam, lParam);
}

// ── DX11 hooked Present ───────────────────────────────────────────────────────
HRESULT WINAPI hookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (!bInit) {
        Logger::ovrly("hookedPresent: Initializing DX11 overlay");
        if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&gD3D11Device))) {
            gD3D11Device->GetImmediateContext(&gContext);
            DXGI_SWAP_CHAIN_DESC sd;
            pSwapChain->GetDesc(&sd);
            gWindow = sd.OutputWindow;
            Logger::ovrly("hookedPresent: Got D3D11 device, window: %p", gWindow);

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
            Logger::error("hookedPresent: Failed to get D3D11 device");
            bInit = true;
        }
    }

    if (gRenderTargetView) {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (bShowAchievementManager)
            UpdateImGuiMouseInput();

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

        bool hasDX12 = GetModuleHandle(TEXT("d3d12.dll")) != NULL;
        bool hasDX11 = GetModuleHandle(TEXT("d3d11.dll")) != NULL;
        Logger::ovrly("Overlay::Init: d3d12.dll=%s  d3d11.dll=%s",
                      hasDX12 ? "YES" : "NO", hasDX11 ? "YES" : "NO");

        // DX12 path — only if explicitly enabled in config
        if (hasDX12 && Config::EnableDX12Hook()) {
            Logger::ovrly("Overlay::Init: Initializing D3D12Hook");
            D3D12Hook::Init();
            bKieroInit = true;
            static auto hidePopup = std::async(std::launch::async, [] {
                Sleep(POPUP_DURATION_MS); bShowInitPopup = false;
            });
            return;
        }

        // DX11 path — direct kiero init, no window waiting
        if (hasDX11) {
            Logger::ovrly("Overlay::Init: Calling kiero::init");
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
    if (hasDX12 && Config::EnableDX12Hook()) {
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
