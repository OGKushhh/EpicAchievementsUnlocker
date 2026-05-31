// d3d12hook.cpp
// DX12 overlay hook using Sh0ckFR's architecture (no kiero).
// Hooks IDXGISwapChain::Present and ID3D12CommandQueue::ExecuteCommandLists
// via vtable pointer theft from a temporary dummy device+swapchain.
// All DX12 rendering is self-contained: own RTV/SRV heaps, own command
// allocator+list per frame, own fence for GPU sync.
//
// Calls AchievementManagerUI::Draw* for the actual overlay content.
//
// FIX: If gCommandQueue is still NULL after 300 frames (~5s at 60fps), the
// game loaded d3d12.dll but actually renders with DX11 (e.g. Dying Light).
// In that case we initialize a DX11 ImGui overlay directly from the already-
// hooked Present, without needing a separate kiero hook.

#include "pch.h"
#include "d3d12hook.h"
#include "Overlay.h"
#include "achievement_manager_ui.h"
#include "Loader.h"
#include "minhook/include/MinHook.h"

#include <dxgi1_4.h>
#include <d3d12.h>
#include <d3d11.h>
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// ── ImGui backends ────────────────────────────────────────────────────────────
#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx12.h"
#include "imgui/imgui_impl_dx11.h"

namespace D3D12Hook {

// ── Types ─────────────────────────────────────────────────────────────────────
using PresentFn              = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using ExecuteCommandListsFn  = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using ResizeBuffersFn        = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static PresentFn             oPresent             = nullptr;
static ExecuteCommandListsFn oExecuteCommandLists = nullptr;
static ResizeBuffersFn       oResizeBuffers        = nullptr;

// ── DX12 globals ──────────────────────────────────────────────────────────────
static ID3D12Device*              gDevice        = nullptr;
static ID3D12CommandQueue*        gCommandQueue  = nullptr;
static ID3D12DescriptorHeap*      gHeapRTV       = nullptr;
static ID3D12DescriptorHeap*      gHeapSRV       = nullptr;
static ID3D12GraphicsCommandList* gCommandList   = nullptr;
static ID3D12Fence*               gFence         = nullptr;
static HANDLE                     gFenceEvent    = nullptr;
static UINT64                     gFenceValue    = 0;
static UINT                       gBufferCount   = 0;
static HWND                       gWindow        = nullptr;

struct FrameCtx {
    ID3D12CommandAllocator* allocator   = nullptr;
    ID3D12Resource*         renderTarget= nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{};
};
static FrameCtx* gFrames = nullptr;

static bool gInitialized      = false;
static bool gShutdown         = false;
static bool gAfterFirstPresent= false;

// ── DX11 fallback globals (used when game has d3d12.dll but renders DX11) ────
static bool                    gFallbackAttempted  = false;
static bool                    gFallbackToDX11     = false;
static bool                    gFallbackInited     = false;
static ID3D11RenderTargetView* gFbRTV              = nullptr;

// ── SRV descriptor allocator callbacks (required by ImGui 1.92+) ─────────────
static void SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    *cpu = gHeapSRV->GetCPUDescriptorHandleForHeapStart();
    *gpu = gHeapSRV->GetGPUDescriptorHandleForHeapStart();
}
static void SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {
    // Single-slot allocator — nothing to free
}

// ── Release all DX12 overlay resources ───────────────────────────────────────
static void ReleaseResources() {
    if (gInitialized && ImGui::GetCurrentContext()) {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        gInitialized = false;
    }
    if (gCommandList)  { gCommandList->Release();  gCommandList  = nullptr; }
    if (gHeapRTV)      { gHeapRTV->Release();      gHeapRTV      = nullptr; }
    if (gHeapSRV)      { gHeapSRV->Release();      gHeapSRV      = nullptr; }
    if (gFence)        { gFence->Release();         gFence        = nullptr; }
    if (gFenceEvent)   { CloseHandle(gFenceEvent);  gFenceEvent   = nullptr; }
    if (gFrames) {
        for (UINT i = 0; i < gBufferCount; i++) {
            if (gFrames[i].renderTarget) gFrames[i].renderTarget->Release();
            if (gFrames[i].allocator)    gFrames[i].allocator->Release();
        }
        delete[] gFrames;
        gFrames = nullptr;
    }
    if (gCommandQueue) { gCommandQueue->Release();  gCommandQueue = nullptr; }
    if (gDevice)       { gDevice->Release();        gDevice       = nullptr; }
    gBufferCount = 0;
}

// ── Release DX11 fallback resources ──────────────────────────────────────────
static void ReleaseFallbackResources() {
    if (gFallbackInited && ImGui::GetCurrentContext()) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        gFallbackInited = false;
    }
    if (gFbRTV) { gFbRTV->Release(); gFbRTV = nullptr; }
    if (Overlay::gContext)     { Overlay::gContext->Release();     Overlay::gContext     = nullptr; }
    if (Overlay::gD3D11Device) { Overlay::gD3D11Device->Release(); Overlay::gD3D11Device = nullptr; }
}

// ── Hooked ExecuteCommandLists — captures the real command queue ──────────────
void STDMETHODCALLTYPE HookedExecuteCommandLists(
    ID3D12CommandQueue* pQueue, UINT count, ID3D12CommandList* const* lists)
{
    if (!gCommandQueue && gAfterFirstPresent) {
        D3D12_COMMAND_QUEUE_DESC desc = pQueue->GetDesc();
        Logger::ovrly("[DX12] ECL called — queue type=%d priority=%d (DIRECT=0,COMPUTE=1,COPY=2)",
                      (int)desc.Type, (int)desc.Priority);
        if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            pQueue->AddRef();
            gCommandQueue = pQueue;
            Logger::ovrly("[DX12] CommandQueue captured: %p", pQueue);
        }
    }
    oExecuteCommandLists(pQueue, count, lists);
}

// ── Per-frame DX12 render helper ─────────────────────────────────────────────
static void RenderFrame(IDXGISwapChain* pSwapChain) {
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();

    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.DisplaySize.x <= 0.f || io.DisplaySize.y <= 0.f) {
            DXGI_SWAP_CHAIN_DESC sd{};
            pSwapChain->GetDesc(&sd);
            io.DisplaySize.x = (float)sd.BufferDesc.Width;
            io.DisplaySize.y = (float)sd.BufferDesc.Height;
        }
    }

    ImGui::NewFrame();

    {
        ImGuiIO& io = ImGui::GetIO();
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(gWindow, &pt);
        io.MousePos      = { (float)pt.x, (float)pt.y };
        io.MouseDown[0]  = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        io.MouseDown[1]  = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        io.MouseDown[2]  = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
        io.MouseDrawCursor = Overlay::bShowAchievementManager;
    }

    if (Overlay::bShowInitPopup)          AchievementManagerUI::DrawInitPopup();
    if (Overlay::bShowAchievementManager) AchievementManagerUI::DrawAchievementList();

    ImGui::Render();

    IDXGISwapChain3* sc3 = nullptr;
    pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3));
    UINT frameIdx = sc3 ? sc3->GetCurrentBackBufferIndex() : 0;
    if (sc3) sc3->Release();

    FrameCtx& ctx = gFrames[frameIdx];

    if (gFence->GetCompletedValue() < gFenceValue) {
        gFence->SetEventOnCompletion(gFenceValue, gFenceEvent);
        WaitForSingleObject(gFenceEvent, 2000);
    }

    ctx.allocator->Reset();
    if (!gCommandList) {
        gDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                   ctx.allocator, nullptr, IID_PPV_ARGS(&gCommandList));
        gCommandList->Close();
    }
    gCommandList->Reset(ctx.allocator, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = ctx.renderTarget;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    gCommandList->ResourceBarrier(1, &barrier);

    gCommandList->OMSetRenderTargets(1, &ctx.rtvHandle, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = { gHeapSRV };
    gCommandList->SetDescriptorHeaps(1, heaps);

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), gCommandList);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    gCommandList->ResourceBarrier(1, &barrier);
    gCommandList->Close();

    oExecuteCommandLists(gCommandQueue, 1,
                         reinterpret_cast<ID3D12CommandList* const*>(&gCommandList));
    gCommandQueue->Signal(gFence, ++gFenceValue);
}

// ── Hooked Present ────────────────────────────────────────────────────────────
HRESULT WINAPI HookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    gAfterFirstPresent = true;

    if (!gCommandQueue) {
        static int missedFrames = 0;
        if (++missedFrames % 60 == 0)
            Logger::ovrly("[DX12] Present called but gCommandQueue still NULL (frame %d)", missedFrames);

        // ── DX11 fallback ─────────────────────────────────────────────────────
        // After ~5 seconds with no DIRECT queue, this game loaded d3d12.dll but
        // actually renders with DX11 (common with some launchers / middleware).
        // We detect this by trying to QueryInterface a DX11 device from the
        // swap chain, then stand up the DX11 ImGui backend in-place.
        if (missedFrames >= 300 && !gFallbackAttempted) {
            gFallbackAttempted = true;
            ID3D11Device* d3d11 = nullptr;
            if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&d3d11))) {
                Logger::ovrly("[DX12->DX11] CommandQueue never appeared — game is DX11. Switching to DX11 overlay.");
                Overlay::gD3D11Device = d3d11;
                d3d11->GetImmediateContext(&Overlay::gContext);
                gFallbackToDX11 = true;
            } else {
                Logger::error("[DX12->DX11] Not a DX11 game either — overlay disabled.");
            }
        }

        // ── First-time DX11 fallback init ─────────────────────────────────────
        if (gFallbackToDX11 && !gFallbackInited && Overlay::gD3D11Device) {
            DXGI_SWAP_CHAIN_DESC sd{};
            pSwapChain->GetDesc(&sd);
            Overlay::gWindow = sd.OutputWindow;

            ID3D11Texture2D* pBB = nullptr;
            if (SUCCEEDED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBB))) {
                Overlay::gD3D11Device->CreateRenderTargetView(pBB, nullptr, &gFbRTV);
                pBB->Release();
            }

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            AchievementManagerUI::InitImGuiStyle();
            ImGui_ImplWin32_Init(Overlay::gWindow);
            ImGui_ImplDX11_Init(Overlay::gD3D11Device, Overlay::gContext);

            // Subclass the window so hotkeys and mouse input work
            Overlay::originalWindowProc = (WNDPROC)SetWindowLongPtr(
                Overlay::gWindow, GWLP_WNDPROC, (LONG_PTR)Overlay::WindowProc);

            Loader::AsyncLoadIcons();
            gFallbackInited = true;
            Logger::ovrly("[DX12->DX11] DX11 overlay initialized via fallback");
        }

        // ── DX11 fallback render ──────────────────────────────────────────────
        if (gFallbackInited && gFbRTV && !gShutdown) {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            {
                ImGuiIO& io = ImGui::GetIO();
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(Overlay::gWindow, &pt);
                io.MousePos        = { (float)pt.x, (float)pt.y };
                io.MouseDown[0]    = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
                io.MouseDown[1]    = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
                io.MouseDown[2]    = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
                io.MouseDrawCursor = Overlay::bShowAchievementManager;
            }
            if (Overlay::bShowInitPopup)          AchievementManagerUI::DrawInitPopup();
            if (Overlay::bShowAchievementManager) AchievementManagerUI::DrawAchievementList();
            ImGui::Render();
            Overlay::gContext->OMSetRenderTargets(1, &gFbRTV, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }

        return oPresent(pSwapChain, SyncInterval, Flags);
    }

    // ── Normal DX12 path ─────────────────────────────────────────────────────
    if (!gInitialized) {
        Logger::ovrly("[DX12] Initializing ImGui on first Present");

        if (FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&gDevice)))) {
            Logger::error("[DX12] GetDevice failed");
            return oPresent(pSwapChain, SyncInterval, Flags);
        }

        DXGI_SWAP_CHAIN_DESC sd{};
        pSwapChain->GetDesc(&sd);
        gBufferCount = sd.BufferCount;
        gWindow      = sd.OutputWindow;
        Logger::ovrly("[DX12] BufferCount=%u %ux%u window=%p",
                      gBufferCount, sd.BufferDesc.Width, sd.BufferDesc.Height, gWindow);

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = gBufferCount;
        if (FAILED(gDevice->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&gHeapRTV)))) {
            Logger::error("[DX12] CreateDescriptorHeap RTV failed"); return oPresent(pSwapChain, SyncInterval, Flags);
        }

        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 64;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(gDevice->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&gHeapSRV)))) {
            Logger::error("[DX12] CreateDescriptorHeap SRV failed"); return oPresent(pSwapChain, SyncInterval, Flags);
        }

        gFrames = new FrameCtx[gBufferCount]{};
        UINT rtvStride = gDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = gHeapRTV->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < gBufferCount; i++) {
            gDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&gFrames[i].allocator));
            pSwapChain->GetBuffer(i, IID_PPV_ARGS(&gFrames[i].renderTarget));
            gDevice->CreateRenderTargetView(gFrames[i].renderTarget, nullptr, rtvHandle);
            gFrames[i].rtvHandle = rtvHandle;
            rtvHandle.ptr += rtvStride;
        }

        gDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gFence));
        gFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        ImGui::CreateContext();
        AchievementManagerUI::InitImGuiStyle();
        ImGui_ImplWin32_Init(gWindow);

        ImGui_ImplDX12_InitInfo dx12info{};
        dx12info.Device            = gDevice;
        dx12info.CommandQueue      = gCommandQueue;
        dx12info.NumFramesInFlight = (int)gBufferCount;
        dx12info.RTVFormat         = sd.BufferDesc.Format;
        dx12info.SrvDescriptorHeap = gHeapSRV;
        dx12info.SrvDescriptorAllocFn = SrvAlloc;
        dx12info.SrvDescriptorFreeFn  = SrvFree;
        ImGui_ImplDX12_Init(&dx12info);

        Logger::ovrly("[DX12] ImGui initialized");
        Loader::AsyncLoadIcons();
        gInitialized = true;
    }

    if (!gShutdown)
        RenderFrame(pSwapChain);

    return oPresent(pSwapChain, SyncInterval, Flags);
}

// ── Hooked ResizeBuffers ──────────────────────────────────────────────────────
HRESULT WINAPI HookedResizeBuffers(IDXGISwapChain* pSwapChain,
    UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT Flags)
{
    Logger::ovrly("[DX12] ResizeBuffers %ux%u", Width, Height);
    if (gFallbackInited) {
        // Tear down DX11 fallback RTV; re-init happens next Present
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        if (gFbRTV) { gFbRTV->Release(); gFbRTV = nullptr; }
        gFallbackInited = false;
    } else {
        ReleaseResources();
    }
    return oResizeBuffers(pSwapChain, BufferCount, Width, Height, Format, Flags);
}

// ── Vtable theft ──────────────────────────────────────────────────────────────
static bool GrabVtablePointers(void** outPresent, void** outECL, void** outResize) {
    WNDCLASSEX wc{ sizeof(wc) };
    wc.lpfnWndProc   = DefWindowProc;
    wc.hInstance     = GetModuleHandle(nullptr);
    wc.lpszClassName = L"D3D12HookDummy";
    RegisterClassEx(&wc);
    HWND hwnd = CreateWindowEx(0, wc.lpszClassName, L"", WS_OVERLAPPED,
                               0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return false;

    bool ok = false;
    IDXGIFactory4*  factory = nullptr;
    ID3D12Device*   device  = nullptr;
    IDXGISwapChain* sc      = nullptr;

    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) goto cleanup;

    {
        IDXGIAdapter* adapter = nullptr;
        factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
        D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        if (adapter) adapter->Release();
    }
    if (!device) D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (!device) goto cleanup;

    {
        D3D12_COMMAND_QUEUE_DESC cqd{ D3D12_COMMAND_LIST_TYPE_DIRECT };
        ID3D12CommandQueue* queue = nullptr;
        device->CreateCommandQueue(&cqd, IID_PPV_ARGS(&queue));
        if (!queue) goto cleanup;

        DXGI_SWAP_CHAIN_DESC scd{};
        scd.BufferCount       = 2;
        scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.OutputWindow      = hwnd;
        scd.SampleDesc.Count  = 1;
        scd.Windowed          = TRUE;
        scd.SwapEffect        = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        factory->CreateSwapChain(queue, &scd, &sc);
        queue->Release();
    }
    if (!sc) goto cleanup;

    {
        void** scVtbl   = *reinterpret_cast<void***>(sc);
        *outPresent     = scVtbl[8];
        *outResize      = scVtbl[13];

        ID3D12Device* dev2 = nullptr;
        sc->GetDevice(IID_PPV_ARGS(&dev2));
        D3D12_COMMAND_QUEUE_DESC cqd{ D3D12_COMMAND_LIST_TYPE_DIRECT };
        ID3D12CommandQueue* q = nullptr;
        if (dev2) { dev2->CreateCommandQueue(&cqd, IID_PPV_ARGS(&q)); dev2->Release(); }
        if (q) {
            void** qVtbl = *reinterpret_cast<void***>(q);
            *outECL = qVtbl[10];
            q->Release();
            ok = true;
        }
    }

cleanup:
    if (sc)      sc->Release();
    if (device)  device->Release();
    if (factory) factory->Release();
    DestroyWindow(hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);
    return ok;
}

// ── Public Init / Shutdown ────────────────────────────────────────────────────
void Init() {
    Logger::ovrly("[DX12] Grabbing vtable pointers...");

    void* pPresent = nullptr, *pECL = nullptr, *pResize = nullptr;
    if (!GrabVtablePointers(&pPresent, &pECL, &pResize)) {
        Logger::error("[DX12] Failed to grab vtable pointers");
        return;
    }

    if (MH_CreateHook(pPresent, &HookedPresent,
                      reinterpret_cast<void**>(&oPresent)) != MH_OK ||
        MH_EnableHook(pPresent) != MH_OK) {
        Logger::error("[DX12] Failed to hook Present");
        return;
    }
    Logger::ovrly("[DX12] Hooked Present");

    if (MH_CreateHook(pECL, &HookedExecuteCommandLists,
                      reinterpret_cast<void**>(&oExecuteCommandLists)) != MH_OK ||
        MH_EnableHook(pECL) != MH_OK) {
        Logger::error("[DX12] Failed to hook ExecuteCommandLists");
        return;
    }
    Logger::ovrly("[DX12] Hooked ExecuteCommandLists");

    if (MH_CreateHook(pResize, &HookedResizeBuffers,
                      reinterpret_cast<void**>(&oResizeBuffers)) != MH_OK ||
        MH_EnableHook(pResize) != MH_OK) {
        Logger::error("[DX12] Failed to hook ResizeBuffers");
    }
    Logger::ovrly("[DX12] Hooked ResizeBuffers");
}

void Shutdown() {
    gShutdown = true;
    if (gFallbackInited) {
        ReleaseFallbackResources();
        if (Overlay::originalWindowProc)
            SetWindowLongPtr(Overlay::gWindow, GWLP_WNDPROC, (LONG_PTR)Overlay::originalWindowProc);
    } else {
        // Wait for GPU to finish before releasing DX12 resources
        if (gCommandQueue && gFence && gFenceEvent) {
            gCommandQueue->Signal(gFence, ++gFenceValue);
            gFence->SetEventOnCompletion(gFenceValue, gFenceEvent);
            WaitForSingleObject(gFenceEvent, 2000);
        }
        ReleaseResources();
    }
    if (oPresent)             MH_DisableHook(reinterpret_cast<void*>(oPresent));
    if (oExecuteCommandLists) MH_DisableHook(reinterpret_cast<void*>(oExecuteCommandLists));
    if (oResizeBuffers)       MH_DisableHook(reinterpret_cast<void*>(oResizeBuffers));
    Logger::ovrly("[DX12] Shutdown complete");
}

} // namespace D3D12Hook
