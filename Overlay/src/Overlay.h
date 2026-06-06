#pragma once
#include "pch.h"
#include "Overlay_types.h"

namespace Overlay {

extern ID3D11Device*          gD3D11Device;
extern ID3D11DeviceContext*   gContext;
extern HWND                   gWindow;
extern Achievements*          achievements;
extern UnlockAchievementFunction* unlockAchievement;

// UI state — read by both DX11 render path and D3D12Hook
extern bool bShowInitPopup;
extern bool bShowAchievementManager;

// WndProc — exposed so D3D12Hook's DX11 fallback can subclass the window
extern LRESULT(WINAPI* originalWindowProc)(HWND, UINT, WPARAM, LPARAM);
LRESULT WINAPI WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

void Init(HMODULE hMod, Achievements* pAchievements, UnlockAchievementFunction* pUnlockAchievement);
void Shutdown();
void OverlayOpen();
void OverlayClose();

} // namespace Overlay
