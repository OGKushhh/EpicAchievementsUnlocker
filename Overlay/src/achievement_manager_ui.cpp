#include "pch.h"
#include "achievement_manager_ui.h"
#include "Overlay.h"
#include "achievement_manager.h"
#include "Config.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace AchievementManagerUI {

// Fonts
ImFont* bigFont = nullptr;
ImFont* regularFont = nullptr;
ImFont* achievementNameFont = nullptr;
ImFont* descriptionFont = nullptr;
ImFont* instructionFont = nullptr;

constexpr float regularFontSize = 16.0f;
constexpr float achievementNameFontSize = 18.0f;
constexpr float descriptionFontSize = 17.0f;
constexpr float instructionFontSize = 22.0f;
constexpr float bigFontSize = 28.0f;

static const char* fontPaths[] = {
    "C:\\Windows\\Fonts\\Bahnschrift.ttf",
    "C:\\Windows\\Fonts\\SegoeUI-Variable.ttf",
    "C:\\Windows\\Fonts\\Segoeui.ttf",
    "C:\\Windows\\Fonts\\Arial.ttf"
};

// UI state
static char searchBuffer[128] = "";
static int filterMode = 0; // 0 = All, 1 = Locked only, 2 = Unlocked only, 3 = Hidden only
static int currentPage = 0;
static int itemsPerPage = 50;
static const int PAGINATION_THRESHOLD = 200;
static bool initialized = false;

// Focus flags
static bool s_focusWindow = false;
static bool s_focusSearch = false;

// Cached filtered indices for performance
static std::vector<int> s_filteredIndices;
static std::string s_lastSearch;
static int s_lastFilterMode = -1;
static bool s_cacheValid = false;

// ----------------------------------------------------------------------------
// Helper: case‑insensitive substring
// ----------------------------------------------------------------------------
static bool containsIgnoreCase(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char ch1, char ch2) { return std::toupper(ch1) == std::toupper(ch2); });
    return it != haystack.end();
}

// ----------------------------------------------------------------------------
// Helper: copy text to clipboard
// ----------------------------------------------------------------------------
static void CopyToClipboard(const char* text) {
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        size_t len = strlen(text) + 1;
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
        if (hMem) {
            memcpy(GlobalLock(hMem), text, len);
            GlobalUnlock(hMem);
            SetClipboardData(CF_TEXT, hMem);
        }
        CloseClipboard();
        Logger::info("Copied to clipboard: %s", text);
    } else {
        Logger::error("Failed to open clipboard");
    }
}

// ----------------------------------------------------------------------------
// Helper: centred text with scaling
// ----------------------------------------------------------------------------
static void FitTextToWindow(ImFont* font, const ImVec4 color, const char* text) {
    ImGui::PushFont(font);
    ImVec2 sz = ImGui::CalcTextSize(text);
    ImGui::PopFont();
    float canvasWidth = ( ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x );
    float scale = canvasWidth / sz.x;
    if (scale > 1.0f) scale = 1.0f;
    font->Scale = scale;
    ImGui::PushFont(font);
    ImGui::TextColored(color, "%s", text);
    ImGui::PopFont();
    font->Scale = 1.0f;
}

// ----------------------------------------------------------------------------
// Public: request focus on next draw
// ----------------------------------------------------------------------------
void RequestFocus() {
    s_focusWindow = true;
    s_focusSearch = true;
    Logger::ovrly("Focus requested for achievement overlay");
}

// ----------------------------------------------------------------------------
// Invalidate filter cache when search/filter changes
// ----------------------------------------------------------------------------
static void InvalidateFilterCache() {
    s_cacheValid = false;
}

// ----------------------------------------------------------------------------
// Rebuild filtered indices if needed
// ----------------------------------------------------------------------------
static void UpdateFilteredIndices() {
    std::string currentSearch(searchBuffer);
    if (!s_cacheValid || currentSearch != s_lastSearch || filterMode != s_lastFilterMode) {
        s_lastSearch = currentSearch;
        s_lastFilterMode = filterMode;
        s_filteredIndices.clear();

        auto* achievements = Overlay::achievements;
        if (!achievements || achievements->empty()) return;

        std::string searchLower = currentSearch;
        std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(), ::tolower);

        for (int i = 0; i < (int)achievements->size(); i++) {
            const auto& ach = achievements->at(i);
            if (filterMode == 1 && ach.UnlockState != UnlockState::Locked) continue;
            if (filterMode == 2 && ach.UnlockState != UnlockState::Unlocked) continue;
            if (filterMode == 3 && !ach.IsHidden) continue;   // Hidden filter
            if (!searchLower.empty()) {
                std::string name = ach.UnlockedDisplayName;
                std::string desc = ach.UnlockedDescription;
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                std::transform(desc.begin(), desc.end(), desc.begin(), ::tolower);
                if (name.find(searchLower) == std::string::npos && desc.find(searchLower) == std::string::npos)
                    continue;
            }
            s_filteredIndices.push_back(i);
        }
        s_cacheValid = true;
    }
}

// ----------------------------------------------------------------------------
// ImGui initialisation
// ----------------------------------------------------------------------------
// Shared style + font init — called by both DX11 and DX12 paths
void InitImGuiStyle() {
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    if (Config::EnableKeyboardNavigation()) {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        Logger::ovrly("Keyboard navigation enabled");
    } else {
        Logger::ovrly("Keyboard navigation disabled");
    }

    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_WindowBg]      = ImVec4(0.08f, 0.08f, 0.10f, 0.96f);
    style.Colors[ImGuiCol_ChildBg]       = ImVec4(0.10f, 0.10f, 0.12f, 0.85f);
    style.Colors[ImGuiCol_Text]          = ImVec4(0.95f, 0.95f, 0.97f, 1.00f);
    style.Colors[ImGuiCol_TextDisabled]  = ImVec4(0.55f, 0.55f, 0.60f, 1.00f);
    style.Colors[ImGuiCol_Button]        = ImVec4(0.20f, 0.45f, 0.80f, 0.80f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.55f, 0.95f, 0.95f);
    style.Colors[ImGuiCol_ButtonActive]  = ImVec4(0.15f, 0.35f, 0.70f, 1.00f);
    style.Colors[ImGuiCol_Header]        = ImVec4(0.20f, 0.45f, 0.80f, 0.50f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.55f, 0.95f, 0.65f);
    style.Colors[ImGuiCol_FrameBg]       = ImVec4(0.16f, 0.16f, 0.18f, 1.00f);
    style.Colors[ImGuiCol_FrameBgHovered]= ImVec4(0.22f, 0.22f, 0.24f, 1.00f);
    style.Colors[ImGuiCol_SliderGrab]    = ImVec4(0.20f, 0.45f, 0.80f, 0.80f);
    style.Colors[ImGuiCol_ScrollbarBg]   = ImVec4(0.10f, 0.10f, 0.12f, 0.60f);
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.25f, 0.25f, 0.30f, 0.80f);
    style.Colors[ImGuiCol_Separator]     = ImVec4(0.25f, 0.25f, 0.30f, 0.80f);

    style.ChildRounding    = 8.0f;
    style.FrameRounding    = 6.0f;
    style.PopupRounding    = 8.0f;
    style.ScrollbarSize    = 10.0f;
    style.ScrollbarRounding= 6.0f;
    style.GrabRounding     = 4.0f;
    style.WindowRounding   = 10.0f;
    style.WindowPadding    = ImVec2(12, 12);
    style.ItemSpacing      = ImVec2(8, 8);
    style.ItemInnerSpacing = ImVec2(6, 6);

    io.Fonts->Clear();
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 2;
    fontConfig.PixelSnapH  = true;
    static const ImWchar ranges[] = { 0x0020, 0xFFFF, 0 };

    bool fontLoaded = false;
    for (const char* path : fontPaths) {
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            regularFont = io.Fonts->AddFontFromFileTTF(path, regularFontSize, &fontConfig, ranges);
            if (regularFont) {
                fontConfig.SizePixels = achievementNameFontSize;
                achievementNameFont = io.Fonts->AddFontFromFileTTF(path, achievementNameFontSize, &fontConfig, ranges);
                fontConfig.SizePixels = descriptionFontSize;
                descriptionFont = io.Fonts->AddFontFromFileTTF(path, descriptionFontSize, &fontConfig, ranges);
                fontConfig.SizePixels = instructionFontSize;
                instructionFont = io.Fonts->AddFontFromFileTTF(path, instructionFontSize, &fontConfig, ranges);
                fontConfig.SizePixels = bigFontSize;
                bigFont = io.Fonts->AddFontFromFileTTF(path, bigFontSize, &fontConfig, ranges);
                fontLoaded = true;
                Logger::ovrly("Loaded font: %s", path);
                break;
            }
        }
    }
    if (!fontLoaded) {
        regularFont = achievementNameFont = descriptionFont =
            instructionFont = bigFont = io.Fonts->AddFontDefault();
        Logger::ovrly("Using default font");
    }
    initialized = true;
    Logger::ovrly("AchievementManagerUI style+fonts initialized");
}

// DX11 path
void InitImGui(HWND hWnd, ID3D11Device* device, ID3D11DeviceContext* context) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    InitImGuiStyle();
    ImGui_ImplWin32_Init(hWnd);
    ImGui_ImplDX11_Init(device, context);
    Logger::ovrly("AchievementManagerUI initialized (DX11)");
}

// ----------------------------------------------------------------------------
// Shutdown
// ----------------------------------------------------------------------------
void ShutdownImGui() {
    if (initialized) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        initialized = false;
        Logger::ovrly("AchievementManagerUI shut down");
    }
}

// ----------------------------------------------------------------------------
// Draw startup popup
// ----------------------------------------------------------------------------
void DrawInitPopup() {
    if (!Overlay::bShowInitPopup) return;

    static float popupTimer = 0.0f;
    popupTimer += ImGui::GetIO().DeltaTime;
    if (popupTimer >= 4.5f) {
        Overlay::bShowInitPopup = false;
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(560, 140), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.10f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.85f, 0.0f, 1.0f));
    ImGui::Begin("InitPopup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleColor(2);

    ImGui::PushFont(bigFont);
    ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1.0f), "Epic Achievements Unlocker is");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.0f, 0.85f, 0.0f, 1.0f), "online");
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushFont(instructionFont);
    ImGui::PushTextWrapPos(540.0f);
    ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1.0f), "Press [Shift] + [F5] to open the achievement panel.");
    ImGui::PopTextWrapPos();
    ImGui::PopFont();

    ImGui::End();
}

// ----------------------------------------------------------------------------
// Main achievement list – top pagination only (sticky)
// ----------------------------------------------------------------------------
void DrawAchievementList() {
    ImGuiIO& io = ImGui::GetIO();

    // Fallback symbol for rows when icon texture is missing
    static const char* fallbackSymbol = "♥";

    if (s_focusWindow) {
        ImGui::SetNextWindowFocus();
        s_focusWindow = false;
    }

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.10f, 0.96f));
    ImGui::Begin("AchievementManagerUI", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleColor();

    if (s_focusSearch) {
        ImGui::SetKeyboardFocusHere(0);
        s_focusSearch = false;
    }

    ImGui::SetWindowPos(ImVec2(0, 0));
    ImGui::SetWindowSize(ImVec2(450, io.DisplaySize.y));

    // ============================================================
    // Modern centered title with red hearts (extra large)
    // ============================================================
    const char* heartSymbol = "♥";
    const char* titleText = " Epic Unlocker ";
    
    ImGui::PushFont(bigFont);
    float heartWidth = ImGui::CalcTextSize(heartSymbol).x;
    float titleWidth = ImGui::CalcTextSize(titleText).x;
    float totalWidth = heartWidth * 2 + titleWidth + 10.0f; // 5px spacing each side
    ImGui::PopFont();

    float windowWidth = ImGui::GetWindowWidth();
    float startX = (windowWidth - totalWidth) * 0.5f;
    ImGui::SetCursorPosX(startX);

    // Left heart (red)
    ImGui::PushFont(bigFont);
    ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.3f, 1.0f), "%s", heartSymbol);
    ImGui::PopFont();
    ImGui::SameLine(0.0f, 5.0f);

    // Title text (white, extra large)
    ImGui::PushFont(bigFont);
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", titleText);
    ImGui::PopFont();
    ImGui::SameLine(0.0f, 5.0f);

    // Right heart (red)
    ImGui::PushFont(bigFont);
    ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.3f, 1.0f), "%s", heartSymbol);
    ImGui::PopFont();

    ImGui::Separator();
    ImGui::Spacing();

    // ------------------------------------------------------------------
    // Statistics + progress bar
    // ------------------------------------------------------------------
    if (Overlay::achievements && Overlay::achievements->size() > 0) {
        int total = (int)Overlay::achievements->size();
        int unlocked = 0;
        for (const auto& a : *Overlay::achievements) {
            if (a.UnlockState == UnlockState::Unlocked) unlocked++;
        }
        float percent = (total > 0) ? (unlocked * 100.0f / total) : 0.0f;
        
        ImGui::PushFont(regularFont);
        ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.2f, 1), "%d / %d unlocked (%.1f%%)", unlocked, total, percent);
        ImGui::PopFont();
        ImGui::ProgressBar(percent / 100.0f, ImVec2(-1, 8), "");
        ImGui::Spacing();
    }

    // ------------------------------------------------------------------
    // Filter controls + Refresh button
    // ------------------------------------------------------------------
    bool filterChanged = false;
    ImGui::BeginGroup();
    ImGui::PushItemWidth(150);
    if (ImGui::InputTextWithHint("##search", "Filter...", searchBuffer, sizeof(searchBuffer))) {
        filterChanged = true;
        currentPage = 0;
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("All")) { filterMode = 0; filterChanged = true; currentPage = 0; }
    ImGui::SameLine();
    if (ImGui::Button("Locked")) { filterMode = 1; filterChanged = true; currentPage = 0; }
    ImGui::SameLine();
    if (ImGui::Button("Unlocked")) { filterMode = 2; filterChanged = true; currentPage = 0; }
    ImGui::SameLine();
    if (ImGui::Button("Hidden")) { filterMode = 3; filterChanged = true; currentPage = 0; }
    if (strlen(searchBuffer) > 0 || filterMode != 0) {
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            searchBuffer[0] = '\0';
            filterMode = 0;
            filterChanged = true;
            currentPage = 0;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        AchievementManager::refresh();
        filterChanged = true;
    }
    ImGui::EndGroup();
    ImGui::Spacing();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 4.0f);

    if (filterChanged) {
        InvalidateFilterCache();
    }

    // ------------------------------------------------------------------
    // Top pagination (sticky)
    // ------------------------------------------------------------------
    UpdateFilteredIndices();
    int totalFiltered = (int)s_filteredIndices.size();
    bool usePagination = (totalFiltered > PAGINATION_THRESHOLD);
    if (usePagination) {
        int totalPages = (totalFiltered + itemsPerPage - 1) / itemsPerPage;
        if (currentPage >= totalPages) currentPage = totalPages - 1;
        if (currentPage < 0) currentPage = 0;
        
        ImGui::BeginGroup();
        ImGui::Text("Page %d / %d", currentPage + 1, totalPages);
        ImGui::SameLine();
        if (ImGui::Button("<<")) currentPage = 0;
        ImGui::SameLine();
        if (ImGui::Button("<") && currentPage > 0) currentPage--;
        ImGui::SameLine();
        if (ImGui::Button(">") && currentPage < totalPages - 1) currentPage++;
        ImGui::SameLine();
        if (ImGui::Button(">>")) currentPage = totalPages - 1;
        ImGui::SameLine();
        ImGui::Text("  (%d achievements)", totalFiltered);
        ImGui::EndGroup();
        ImGui::Spacing();
    }

    // ------------------------------------------------------------------
    // Achievement list (virtual scrolling)
    // ------------------------------------------------------------------
    ImGui::BeginChild("AchievementList", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

    if (Overlay::achievements == nullptr || Overlay::achievements->size() == 0) {
        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Loading achievements...");
    } else {
        if (totalFiltered == 0) {
            ImGui::TextColored(ImVec4(1, 1, 0, 1), "No achievements match the filter.");
        } else {
            int startIdx = usePagination ? currentPage * itemsPerPage : 0;
            int endIdx = usePagination ? (std::min)(startIdx + itemsPerPage, totalFiltered) : totalFiltered;

            float rowHeight = 70.0f;
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            ImVec2 startPos = ImGui::GetCursorScreenPos();

            for (int row = startIdx; row < endIdx; row++) {
                int idx = s_filteredIndices[row];
                auto& achievement = Overlay::achievements->at(idx);
                ImGui::PushID(idx);

                ImVec2 rowMin = ImVec2(startPos.x, startPos.y + (row - startIdx) * rowHeight);
                ImVec2 rowMax = ImVec2(rowMin.x + ImGui::GetWindowWidth(), rowMin.y + rowHeight);
                ImU32 bgColor = (row % 2 == 0) ? IM_COL32(30, 30, 35, 230) : IM_COL32(25, 25, 30, 230);
                drawList->AddRectFilled(rowMin, rowMax, bgColor, 4.0f);

                ImGui::SetCursorScreenPos(rowMin);

                // Achievement icon (if loaded) or fallback heart
                float iconHeight = 50.0f;
                float iconYOffset = (rowHeight - iconHeight) / 2.0f;
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + iconYOffset);

                if (achievement.IconTexture) {
                    ImGui::Image(achievement.IconTexture, ImVec2(iconHeight, iconHeight));
                } else {
                    ImGui::PushFont(bigFont);
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "%s", fallbackSymbol);
                    ImGui::PopFont();
                }
                ImGui::SameLine();

                // Text area
                float rightColumnWidth = 85.0f;
                float iconWidth = 50.0f;
                float padding = 15.0f;
                float textWidth = ImGui::GetWindowWidth() - iconWidth - rightColumnWidth - padding;
                if (textWidth < 100.0f) textWidth = 100.0f;

                ImGui::BeginGroup();
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textWidth);

                ImGui::PushFont(achievementNameFont);
                ImGui::TextColored(ImVec4(1, 1, 1, 1), achievement.UnlockedDisplayName);
                ImGui::PopFont();

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.90f, 1.0f));
                ImGui::PushFont(descriptionFont);
                ImGui::TextWrapped(achievement.UnlockedDescription);
                ImGui::PopFont();
                ImGui::PopStyleColor();

                ImGui::PopTextWrapPos();
                ImGui::EndGroup();

                // Right column
                float rightContentHeight = 0.0f;
                if (achievement.IsHidden) rightContentHeight += 20.0f;
                if (achievement.UnlockState == UnlockState::Locked) rightContentHeight += 28.0f;
                else rightContentHeight += 20.0f;
                
                float rightX = ImGui::GetWindowWidth() - rightColumnWidth;
                float rightY = rowMin.y + (rowHeight - rightContentHeight) / 2.0f;
                ImGui::SetCursorScreenPos(ImVec2(rightX, rightY));

                ImGui::BeginGroup();
                if (achievement.IsHidden) {
                    ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.2f, 1), "Hidden");
                }
                switch (achievement.UnlockState) {
                case UnlockState::Unlocked:
                    ImGui::TextColored(ImVec4(0, 0.9f, 0, 1), "Unlocked");
                    break;
                case UnlockState::Unlocking:
                    ImGui::TextColored(ImVec4(0.9f, 0.9f, 0, 1), "Unlocking...");
                    break;
                case UnlockState::Locked:
                    if (ImGui::Button("Unlock", ImVec2(70, 28))) {
                        Overlay::unlockAchievement(&achievement);
                    }
                    break;
                }
                ImGui::EndGroup();

                // Right-click context menu
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    ImGui::OpenPopup("AchievementContextMenu");
                }
                if (ImGui::BeginPopup("AchievementContextMenu")) {
                    if (ImGui::Selectable("Copy Achievement ID")) {
                        CopyToClipboard(achievement.AchievementId);
                    }
                    ImGui::EndPopup();
                }

                ImGui::PopID();
            }
            ImGui::SetCursorScreenPos(ImVec2(startPos.x, startPos.y + (endIdx - startIdx) * rowHeight));
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace AchievementManagerUI