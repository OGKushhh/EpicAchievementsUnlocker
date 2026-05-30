#pragma once
#include <Windows.h>
#include <d3d11.h>

namespace AchievementManagerUI {
    void InitImGuiStyle(); // shared font+style — call after ImGui::CreateContext()
    void InitImGui(HWND hWnd, ID3D11Device* device, ID3D11DeviceContext* context); // DX11
    void ShutdownImGui();
    void DrawInitPopup();
    void DrawAchievementList();
    void RequestFocus();
}
