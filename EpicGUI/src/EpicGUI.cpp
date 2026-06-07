// EpicGUI.cpp — Epic Achievement Unlocker
// Dark UI, tabbed: Achievements | Log (RichEdit, colored) | DLC (with catalog titles)

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <richedit.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <deque>
#include <sstream>
#include <map>
#include "pipe_protocol.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// ── Colours ───────────────────────────────────────────────────────────────────
#define C_BG        RGB(13,  15,  20)
#define C_SURFACE   RGB(20,  24,  33)
#define C_SURFACE2  RGB(28,  33,  46)
#define C_SURFACE3  RGB(36,  42,  58)
#define C_BORDER    RGB(48,  56,  76)
#define C_BORDER2   RGB(60,  70,  95)
#define C_TEXT      RGB(218, 224, 236)
#define C_TEXTDIM   RGB(100, 112, 140)
#define C_TEXTSUB   RGB(140, 152, 178)
#define C_ACCENT    RGB(56,  128, 244)
#define C_ACCENT2   RGB(86,  156, 255)
#define C_ACCENTDIM RGB(36,  80,  160)
#define C_GREEN     RGB(46,  196, 112)
#define C_YELLOW    RGB(248, 188,  32)
#define C_RED       RGB(236,  64,  64)
#define C_PURPLE    RGB(162,  80,  244)
#define C_ORANGE    RGB(246, 112,  20)
#define C_HEADER    RGB(16,  20,  28)

// ── Control IDs ───────────────────────────────────────────────────────────────
#define IDC_LIST            100
#define IDC_UNLOCK          101
#define IDC_UNLOCK_ALL      102
#define IDC_REFRESH         103
#define IDC_FILTER          104
#define IDC_COMBO           105
#define IDC_STATUS          106
#define IDC_INFO            107
#define IDC_TAB             108
#define IDC_LOG_EDIT        109
#define IDC_DLC_LIST        110
#define IDC_LOG_FILTER      111
#define IDC_LOG_CLEAR       112
#define IDC_LOG_AUTOSCROLL  113
#define IDC_LOG_OPEN        114

// ── Timers ────────────────────────────────────────────────────────────────────
#define IDT_PIPE      1
#define IDT_TOAST     2
#define IDT_LOGTAIL   3

// ── Tabs ──────────────────────────────────────────────────────────────────────
#define TAB_ACH   0
#define TAB_LOG   1
#define TAB_DLC   2
#define TAB_COUNT 3

// ── Layout constants ──────────────────────────────────────────────────────────
#define HEADER_H  48
#define TAB_H     36
#define STATUS_H  26
#define BTN_H     32
#define EDIT_H    30
#define PAD       10

// ── Log limits ────────────────────────────────────────────────────────────────
static const int LOG_MAX_LINES = 20000;
static const int LOG_TRIM_TO   = 15000;

// ── Tab icon/label definitions ────────────────────────────────────────────────
// Icons use Segoe UI Symbol (monochrome, recolorable via SetTextColor).
// Label uses Segoe UI. Both are rendered side-by-side in the draw functions.
struct TabDef { const wchar_t* icon; const wchar_t* label; };
static const TabDef TAB_DEFS[TAB_COUNT] = {
    { L"\u2605", L"Achievements" },  // ★  solid star
    { L"\u2630", L"Logs"         },  // ☰  three horizontal bars
    { L"\u229E", L"DLC"          },  // ⊞  squared plus
};

// ── Button text convention ────────────────────────────────────────────────────
// Format: L"ICON\tLabel"
// DrawBtn splits on \t: icon drawn with g_fontSymbol, label with g_fontUI.
// Buttons with no \t (e.g. info "ℹ") are drawn entirely with g_fontSymbol.
#define BTN_REFRESH      L"\u21BA\tRefresh"           // ↺
#define BTN_UNLOCK       L"\u26A1\tUnlock Selected"   // ⚡
#define BTN_UNLOCK_ALL   L"\u2726\tUnlock All"         // ✦
#define BTN_LOG_CLEAR    L"\u2715\tClear"              // ✕
#define BTN_AUTOSCROLL   L"\u21E9\tAuto-scroll"        // ⇩
#define BTN_OPEN_FILE    L"\u2197\tOpen File"          // ↗
#define BTN_INFO         L"\u2139"                     // ℹ  (icon only)

// ── Data structures ───────────────────────────────────────────────────────────
struct Toast {
    std::wstring text, sub;
    DWORD        born;
    int          alpha;
    COLORREF     barColor;
};

struct Ach {
    std::wstring id, name, desc;
    bool         isHidden;
    WireUnlockState state;
};

struct DlcItem {
    std::wstring id;
    int  timesQueried = 0;   // appearances in "Item ID:" log lines
    int  timesOwned   = 0;   // appearances in "[Owned]" response lines
    bool currentOwned = false;
};

struct LogLine {
    std::wstring text;
    COLORREF     color;
};

// ── Globals ───────────────────────────────────────────────────────────────────
static HWND g_hwnd   = nullptr;
static int  g_curTab = TAB_ACH;
static bool g_connected = false;

// Achievement tab
static HWND g_list       = nullptr;
static HWND g_filter     = nullptr;
static HWND g_combo      = nullptr;
static HWND g_btnUnlock  = nullptr;
static HWND g_btnAll     = nullptr;
static HWND g_btnRefresh = nullptr;

// Log tab
static HWND g_logEdit          = nullptr;
static HWND g_logFilter        = nullptr;
static HWND g_btnLogClear      = nullptr;
static HWND g_btnLogAutoScroll = nullptr;
static HWND g_btnLogOpen       = nullptr;

// DLC tab
static HWND g_dlcList = nullptr;

// Header & status (always visible)
static HWND g_tabBar    = nullptr;
static HWND g_statusBar = nullptr;
static HWND g_btnInfo   = nullptr;

// Fonts — UI is Segoe UI, Symbol is Segoe UI Symbol (monochrome icon glyphs)
static HFONT  g_fontUI     = nullptr;
static HFONT  g_fontBold   = nullptr;
static HFONT  g_fontSmall  = nullptr;
static HFONT  g_fontSymbol = nullptr;  // Segoe UI Symbol, same point size as g_fontUI

// Brushes
static HBRUSH g_brBg      = nullptr;
static HBRUSH g_brSurf    = nullptr;
static HBRUSH g_brSurf2   = nullptr;
static HBRUSH g_brSurf3   = nullptr;
static HBRUSH g_brHeader  = nullptr;

// Data
static std::vector<Ach>            g_achs;
static std::vector<int>            g_view;
static std::vector<DlcItem>        g_dlcItems;
static std::map<std::wstring, int> g_dlcIndex;
static std::map<std::wstring, std::wstring> g_dlcTitles;  // id → title from DlcCatalog packet
static std::vector<LogLine>        g_logLines;
static std::deque<Toast>           g_toasts;
static HWND                        g_toastWnd = nullptr;

// Pipe
static HANDLE               g_pipe    = INVALID_HANDLE_VALUE;
static std::vector<uint8_t> g_pipeBuf;

// Log state
static std::wstring g_logPath;
static long long    g_logFilePos       = 0;
static int          g_logDisplayedCount = 0;
static bool         g_logAutoScroll    = true;
static int          g_entitlementCount = -1;

// Status
static std::wstring g_statusText  = L"Not connected  —  launch game first";
static COLORREF     g_statusColor = C_TEXTDIM;

// RichEdit module
static HMODULE g_hRichEdit = nullptr;

// ── Forward declarations ──────────────────────────────────────────────────────
static void Disconnect();
static void RebuildView();
static void RebuildDlcList();
static void RebuildLogEdit();
static void AppendNewLogLines(int fromIdx);
static void UpdateStats();
static void UpdateDlcStats();
static void ShowTab(int tab);

// ── Helpers ───────────────────────────────────────────────────────────────────
static std::wstring Utf8ToWide(const char* s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    if (!w.empty() && w.back() == 0) w.pop_back();
    return w;
}

static void SetStatusText(const wchar_t* msg, COLORREF color) {
    g_statusText  = msg;
    g_statusColor = color;
    if (g_statusBar) InvalidateRect(g_statusBar, nullptr, TRUE);
}

static void UpdateStats() {
    if (g_curTab != TAB_ACH) return;
    int total = (int)g_achs.size(), unlocked = 0;
    for (auto& a : g_achs) if (a.state == WireUnlockState::Unlocked) unlocked++;
    wchar_t buf[160];
    swprintf_s(buf, L"%d / %d unlocked  \u00B7  showing %d", unlocked, total, (int)g_view.size());
    SetStatusText(buf, C_TEXTDIM);
}

static void UpdateDlcStats() {
    if (g_curTab != TAB_DLC) return;
    int owned = 0;
    for (auto& d : g_dlcItems) if (d.currentOwned) owned++;
    wchar_t buf[240];
    int catalogSize = (int)g_dlcTitles.size();
    if (g_entitlementCount >= 0 && catalogSize > 0)
        swprintf_s(buf, L"%d DLCs queried  \u00B7  %d owned  \u00B7  Catalog: %d titles  \u00B7  Entitlements: %d",
                   (int)g_dlcItems.size(), owned, catalogSize, g_entitlementCount);
    else if (catalogSize > 0)
        swprintf_s(buf, L"%d DLCs queried  \u00B7  %d owned  \u00B7  Catalog: %d titles",
                   (int)g_dlcItems.size(), owned, catalogSize);
    else if (g_entitlementCount >= 0)
        swprintf_s(buf, L"%d DLCs queried  \u00B7  %d owned  \u00B7  Entitlements: %d",
                   (int)g_dlcItems.size(), owned, g_entitlementCount);
    else
        swprintf_s(buf, L"%d DLCs queried  \u00B7  %d owned", (int)g_dlcItems.size(), owned);
    SetStatusText(buf, C_TEXTDIM);
}

static COLORREF LogLineColor(const std::wstring& line) {
    if (line.find(L"[ERROR]") != std::wstring::npos) return C_RED;
    if (line.find(L"[WARN]")  != std::wstring::npos) return C_YELLOW;
    if (line.find(L"[DLC]")   != std::wstring::npos) return C_PURPLE;
    if (line.find(L"[ACH]")   != std::wstring::npos) return C_GREEN;
    if (line.find(L"[OVRLY]") != std::wstring::npos) return C_ORANGE;
    if (line.find(L"[HOOK]")  != std::wstring::npos) return C_ACCENT2;
    if (line.find(L"[DEBUG]") != std::wstring::npos) return C_TEXTDIM;
    return C_TEXT;
}

// ── GDI helpers ───────────────────────────────────────────────────────────────
static void FillRectColor(HDC hdc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c); FillRect(hdc, &r, b); DeleteObject(b);
}

static void DrawRoundRect(HDC hdc, RECT r, int rx, COLORREF fill, COLORREF border) {
    HBRUSH fb = CreateSolidBrush(fill);
    HPEN   fp = CreatePen(PS_SOLID, 1, border);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, fb);
    HPEN   op = (HPEN)SelectObject(hdc, fp);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, rx, rx);
    SelectObject(hdc, ob); SelectObject(hdc, op);
    DeleteObject(fb); DeleteObject(fp);
}

static void DrawHLine(HDC hdc, int x0, int x1, int y, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HPEN o = (HPEN)SelectObject(hdc, p);
    MoveToEx(hdc, x0, y, nullptr); LineTo(hdc, x1, y);
    SelectObject(hdc, o); DeleteObject(p);
}

static void DrawVLine(HDC hdc, int x, int y0, int y1, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HPEN o = (HPEN)SelectObject(hdc, p);
    MoveToEx(hdc, x, y0, nullptr); LineTo(hdc, x, y1);
    SelectObject(hdc, o); DeleteObject(p);
}

// ── Draw text with split icon (Segoe UI Symbol) + label (Segoe UI) ────────────
// text format: L"ICON\tLabel"  — icon drawn with g_fontSymbol at iconColor,
//                                label drawn with g_fontUI   at labelColor.
// If no \t, entire text is drawn with g_fontSymbol at iconColor (icon-only buttons).
static void DrawIconLabel(HDC hdc, RECT r, const wchar_t* text,
                          COLORREF iconColor, COLORREF labelColor) {
    const wchar_t* sep = wcschr(text, L'\t');
    SetBkMode(hdc, TRANSPARENT);

    if (!sep || !g_fontSymbol) {
        // Icon-only or fallback: draw everything centered with fontSymbol (or fontUI)
        SelectObject(hdc, g_fontSymbol ? g_fontSymbol : g_fontUI);
        SetTextColor(hdc, iconColor);
        DrawTextW(hdc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    std::wstring icon(text, sep);
    const wchar_t* label = sep + 1;

    // Measure each piece with its font
    SIZE  iconSz{}, labelSz{};
    TEXTMETRIC tmIcon{}, tmLabel{};

    SelectObject(hdc, g_fontSymbol);
    GetTextExtentPoint32W(hdc, icon.c_str(), (int)icon.size(), &iconSz);
    GetTextMetrics(hdc, &tmIcon);

    SelectObject(hdc, g_fontUI);
    GetTextExtentPoint32W(hdc, label, (int)wcslen(label), &labelSz);
    GetTextMetrics(hdc, &tmLabel);

    // Center the whole block horizontally
    const int gap    = 5;
    const int totalW = iconSz.cx + gap + labelSz.cx;
    const int cx2    = (r.left + r.right)  / 2;
    const int cy2    = (r.top  + r.bottom) / 2;
    int x = cx2 - totalW / 2;

    // Draw icon — vertically centered per its own ascent
    SelectObject(hdc, g_fontSymbol);
    SetTextColor(hdc, iconColor);
    TextOutW(hdc, x, cy2 - tmIcon.tmAscent / 2, icon.c_str(), (int)icon.size());
    x += iconSz.cx + gap;

    // Draw label — vertically centered per its own ascent
    SelectObject(hdc, g_fontUI);
    SetTextColor(hdc, labelColor);
    TextOutW(hdc, x, cy2 - tmLabel.tmAscent / 2, label, (int)wcslen(label));
}

// ── Custom tab bar ────────────────────────────────────────────────────────────
static int g_tabHot = -1;

static RECT GetTabRect(HWND hwnd, int i) {
    RECT cr; GetClientRect(hwnd, &cr);
    int tw = cr.right / TAB_COUNT;
    return { i * tw, 0, i * tw + tw, cr.bottom };
}

static LRESULT CALLBACK TabBarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);
        FillRectColor(hdc, cr, C_SURFACE);
        SetBkMode(hdc, TRANSPARENT);

        for (int i = 0; i < TAB_COUNT; i++) {
            RECT tr = GetTabRect(hwnd, i);
            bool active = (i == g_curTab), hot = (i == g_tabHot);
            FillRectColor(hdc, tr, active ? C_BG : hot ? C_SURFACE2 : C_SURFACE);
            if (active) {
                RECT ul = { tr.left + 2, tr.bottom - 2, tr.right - 2, tr.bottom };
                FillRectColor(hdc, ul, C_ACCENT);
            }
            if (i < TAB_COUNT - 1)
                DrawVLine(hdc, tr.right - 1, tr.top + 6, tr.bottom - 6, C_BORDER);

            // Build "ICON\tLabel" for split-font rendering
            wchar_t combined[128];
            swprintf_s(combined, L"%s\t%s", TAB_DEFS[i].icon, TAB_DEFS[i].label);
            COLORREF iconCol = active ? C_ACCENT2 : hot ? C_TEXTSUB : C_TEXTDIM;
            COLORREF textCol = active ? C_TEXT    : hot ? C_TEXTSUB : C_TEXTDIM;
            DrawIconLabel(hdc, tr, combined, iconCol, textCol);
        }
        DrawHLine(hdc, 0, cr.right, cr.bottom - 1, C_BORDER);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp);
        for (int i = 0; i < TAB_COUNT; i++) {
            RECT tr = GetTabRect(hwnd, i);
            if (x >= tr.left && x < tr.right && i != g_curTab) {
                g_curTab = i;
                InvalidateRect(hwnd, nullptr, TRUE);
                SendMessageW(GetParent(hwnd), WM_APP + 1, i, 0);
                break;
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), newHot = -1;
        for (int i = 0; i < TAB_COUNT; i++) {
            RECT tr = GetTabRect(hwnd, i);
            if (x >= tr.left && x < tr.right) { newHot = i; break; }
        }
        if (newHot != g_tabHot) {
            g_tabHot = newHot;
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd };
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    }
    case WM_MOUSELEAVE: g_tabHot = -1; InvalidateRect(hwnd, nullptr, TRUE); return 0;
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Custom status bar ─────────────────────────────────────────────────────────
static LRESULT CALLBACK StatusBarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);
        FillRectColor(hdc, cr, C_SURFACE);
        DrawHLine(hdc, 0, cr.right, 0, C_BORDER);
        COLORREF dot = g_connected ? C_GREEN : C_TEXTDIM;
        HBRUSH db = CreateSolidBrush(dot); HPEN dp = CreatePen(PS_SOLID, 1, dot);
        HBRUSH ob = (HBRUSH)SelectObject(hdc, db);
        HPEN   op = (HPEN)SelectObject(hdc, dp);
        int cy2 = cr.bottom / 2;
        Ellipse(hdc, 10, cy2 - 4, 18, cy2 + 4);
        SelectObject(hdc, ob); SelectObject(hdc, op);
        DeleteObject(db); DeleteObject(dp);
        SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, g_statusColor);
        SelectObject(hdc, g_fontUI);
        RECT tr = { 24, 0, cr.right - 8, cr.bottom };
        DrawTextW(hdc, g_statusText.c_str(), -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        EndPaint(hwnd, &ps); return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Custom button ─────────────────────────────────────────────────────────────
struct BtnState { bool hot = false, pressed = false; };

static void DrawBtn(HDC hdc, RECT r, const wchar_t* txt,
                    bool hot, bool pressed, bool enabled,
                    bool primary = false, bool active = false) {
    COLORREF bg, iconCol, labelCol, border;
    if (!enabled) {
        bg = C_SURFACE2; iconCol = C_TEXTDIM; labelCol = C_TEXTDIM; border = C_BORDER;
    } else if (primary) {
        bg       = pressed ? C_ACCENTDIM : hot ? C_ACCENT2 : C_ACCENT;
        iconCol  = RGB(255, 255, 255);
        labelCol = RGB(255, 255, 255);
        border   = bg;
    } else if (active) {
        bg = C_ACCENTDIM; iconCol = C_ACCENT2; labelCol = C_ACCENT2; border = C_ACCENT;
    } else {
        bg       = pressed ? C_SURFACE3 : hot ? C_SURFACE3 : C_SURFACE2;
        iconCol  = hot ? C_ACCENT2  : C_TEXTDIM;
        labelCol = hot ? C_TEXT     : C_TEXTSUB;
        border   = hot ? C_BORDER2  : C_BORDER;
    }
    DrawRoundRect(hdc, r, 6, bg, border);
    DrawIconLabel(hdc, r, txt, iconCol, labelCol);
}

static LRESULT CALLBACK BtnProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                 UINT_PTR, DWORD_PTR ref) {
    auto* s = (BtnState*)ref;
    switch (msg) {
    case WM_MOUSEMOVE:
        if (!s->hot) {
            s->hot = true;
            TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, hwnd };
            TrackMouseEvent(&t);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE:
        s->hot = false; s->pressed = false;
        InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN:
        s->pressed = true; SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_LBUTTONUP:
        ReleaseCapture(); s->pressed = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        if (s->hot)
            PostMessageW(GetParent(hwnd), WM_COMMAND,
                         MAKEWPARAM(GetDlgCtrlID(hwnd), BN_CLICKED), (LPARAM)hwnd);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT r; GetClientRect(hwnd, &r);
        wchar_t txt[64] = {}; GetWindowTextW(hwnd, txt, 63);
        bool en      = IsWindowEnabled(hwnd) != 0;
        int  id      = GetDlgCtrlID(hwnd);
        bool primary = (id == IDC_UNLOCK_ALL || id == IDC_UNLOCK);
        bool actv    = (id == IDC_LOG_AUTOSCROLL && g_logAutoScroll);
        DrawBtn(hdc, r, txt, s->hot && en, s->pressed && en, en, primary, actv);
        EndPaint(hwnd, &ps); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_NCDESTROY:  delete s; return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ── Edit subclass (focus ring) ────────────────────────────────────────────────
struct EditState { bool focused = false; };
static LRESULT CALLBACK EditSubProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR, DWORD_PTR ref) {
    auto* s = (EditState*)ref;
    if (msg == WM_SETFOCUS)  { s->focused = true;  InvalidateRect(GetParent(hwnd), nullptr, FALSE); }
    if (msg == WM_KILLFOCUS) { s->focused = false; InvalidateRect(GetParent(hwnd), nullptr, FALSE); }
    if (msg == WM_NCDESTROY) { delete s; return 0; }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ── Toast ─────────────────────────────────────────────────────────────────────
static LRESULT CALLBACK ToastProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);
        const int th = 56;
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
        SelectObject(mem, bmp);
        FillRectColor(mem, cr, C_BG);
        SetBkMode(mem, TRANSPARENT);
        for (int i = 0; i < (int)g_toasts.size(); i++) {
            int y = i * (th + 6);
            RECT tr = { 2, y + 2, cr.right - 2, y + th - 2 };
            DrawRoundRect(mem, tr, 6, C_SURFACE2, C_BORDER2);
            RECT bar = { 2, y + 2, 6, y + th - 2 };
            FillRectColor(mem, bar, g_toasts[i].barColor);
            SelectObject(mem, g_fontBold);
            SetTextColor(mem, C_TEXT);
            RECT nr = { 14, y + 8, cr.right - 8, y + th / 2 + 2 };
            DrawTextW(mem, g_toasts[i].text.c_str(), -1, &nr,
                      DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(mem, g_fontSmall);
            SetTextColor(mem, C_TEXTDIM);
            RECT sr = { 14, y + th / 2 + 2, cr.right - 8, y + th - 8 };
            DrawTextW(mem, g_toasts[i].sub.c_str(), -1, &sr,
                      DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        BitBlt(hdc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
        DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps); return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void PushToast(const wchar_t* line1, const wchar_t* line2, COLORREF barColor) {
    Toast t{ line1, line2, GetTickCount(), 255, barColor };
    if (g_toasts.size() >= 4) g_toasts.pop_front();
    g_toasts.push_back(t);
    const int tw = 320, th = 56;
    int totalH = (int)g_toasts.size() * (th + 6);
    RECT r; GetWindowRect(g_hwnd, &r);
    if (!g_toastWnd) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = ToastProc;
        wc.hInstance   = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"EpicToast";
        RegisterClassExW(&wc);
        g_toastWnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            L"EpicToast", nullptr, WS_POPUP,
            r.right - tw - 16, r.bottom - totalH - 36, tw, totalH,
            g_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    SetWindowPos(g_toastWnd, HWND_TOPMOST,
        r.right - tw - 16, r.bottom - totalH - 36, tw, totalH,
        SWP_SHOWWINDOW | SWP_NOACTIVATE);
    SetLayeredWindowAttributes(g_toastWnd, 0, 240, LWA_ALPHA);
    InvalidateRect(g_toastWnd, nullptr, TRUE);
    SetTimer(g_hwnd, IDT_TOAST, 50, nullptr);
}

static void ToastTick() {
    if (g_toasts.empty()) {
        KillTimer(g_hwnd, IDT_TOAST);
        if (g_toastWnd) ShowWindow(g_toastWnd, SW_HIDE);
        return;
    }
    DWORD now = GetTickCount();
    bool changed = false;
    for (auto it = g_toasts.begin(); it != g_toasts.end(); ) {
        DWORD age = now - it->born;
        if (age > 3500) { it = g_toasts.erase(it); changed = true; }
        else if (age > 2800) {
            it->alpha = (int)(255.0f * (1.0f - (float)(age - 2800) / 700.0f));
            ++it; changed = true;
        } else ++it;
    }
    if (!g_toasts.empty() && g_toastWnd) {
        const int th = 56;
        int totalH = (int)g_toasts.size() * (th + 6);
        RECT r; GetWindowRect(g_hwnd, &r);
        BYTE alpha = (BYTE)max(0, g_toasts.back().alpha);
        SetWindowPos(g_toastWnd, HWND_TOPMOST,
            r.right - 336, r.bottom - totalH - 36, 320, totalH,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetLayeredWindowAttributes(g_toastWnd, 0, alpha, LWA_ALPHA);
        if (changed) InvalidateRect(g_toastWnd, nullptr, TRUE);
    }
    if (g_toasts.empty() && g_toastWnd) ShowWindow(g_toastWnd, SW_HIDE);
}

// ── Pipe send ─────────────────────────────────────────────────────────────────
static bool SendPkt(PktType type, const void* payload = nullptr, uint32_t size = 0) {
    if (g_pipe == INVALID_HANDLE_VALUE) return false;
    PktHeader hdr{ EPIC_MAGIC, type, size };
    DWORD w;
    if (!WriteFile(g_pipe, &hdr, sizeof(hdr), &w, nullptr)) { Disconnect(); return false; }
    if (size && payload && !WriteFile(g_pipe, payload, size, &w, nullptr)) { Disconnect(); return false; }
    return true;
}

// ── RichEdit log helpers ──────────────────────────────────────────────────────
static void AppendColoredLine(const LogLine& ll) {
    int len = GetWindowTextLengthW(g_logEdit);
    SendMessage(g_logEdit, EM_SETSEL, len, len);
    CHARFORMAT2W cf{};
    cf.cbSize      = sizeof(cf);
    cf.dwMask      = CFM_COLOR | CFM_FACE | CFM_SIZE | CFM_CHARSET;
    cf.crTextColor = ll.color;
    cf.yHeight     = 180;   // ~9pt in twips
    cf.bCharSet    = DEFAULT_CHARSET;
    wcscpy_s(cf.szFaceName, L"Consolas");
    SendMessage(g_logEdit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    std::wstring line = ll.text + L"\r\n";
    SendMessage(g_logEdit, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
}

static void AppendNewLogLines(int fromIdx) {
    if (!g_logEdit || fromIdx >= (int)g_logLines.size()) return;
    SendMessage(g_logEdit, WM_SETREDRAW, FALSE, 0);
    for (int i = fromIdx; i < (int)g_logLines.size(); i++)
        AppendColoredLine(g_logLines[i]);
    g_logDisplayedCount = (int)g_logLines.size();
    SendMessage(g_logEdit, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_logEdit, nullptr, FALSE);
    if (g_logAutoScroll) {
        SendMessage(g_logEdit, EM_SETSEL, -1, -1);
        SendMessage(g_logEdit, EM_SCROLLCARET, 0, 0);
    }
}

static void RebuildLogEdit() {
    if (!g_logEdit) return;
    wchar_t fbuf[256] = {};
    GetWindowTextW(g_logFilter, fbuf, 255);
    std::wstring fs = fbuf;
    std::transform(fs.begin(), fs.end(), fs.begin(), ::towlower);
    SendMessage(g_logEdit, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(g_logEdit, L"");
    for (auto& ll : g_logLines) {
        if (!fs.empty()) {
            std::wstring lo = ll.text;
            std::transform(lo.begin(), lo.end(), lo.begin(), ::towlower);
            if (lo.find(fs) == std::wstring::npos) continue;
        }
        AppendColoredLine(ll);
    }
    g_logDisplayedCount = (int)g_logLines.size();
    SendMessage(g_logEdit, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_logEdit, nullptr, FALSE);
    if (g_logAutoScroll) {
        SendMessage(g_logEdit, EM_SETSEL, -1, -1);
        SendMessage(g_logEdit, EM_SCROLLCARET, 0, 0);
    }
}

// ── DLC log parser ────────────────────────────────────────────────────────────
static void ParseDlcLine(const std::wstring& line) {
    // "Item ID: <id>" — game queried this DLC
    auto pos = line.find(L"Item ID: ");
    if (pos != std::wstring::npos) {
        std::wstring id = line.substr(pos + 9);
        while (!id.empty() && (id.back() == L'\r' || id.back() == L'\n' || id.back() == L' '))
            id.pop_back();
        if (!id.empty()) {
            if (g_dlcIndex.find(id) == g_dlcIndex.end()) {
                g_dlcIndex[id] = (int)g_dlcItems.size();
                g_dlcItems.push_back({ id });
            }
            g_dlcItems[g_dlcIndex[id]].timesQueried++;
        }
        return;
    }

    // "[Owned] <id>" / "[Not Owned] <id>" — DLL ownership response
    bool owned    = line.find(L"[Owned] ")     != std::wstring::npos;
    bool notOwned = line.find(L"[Not Owned] ") != std::wstring::npos;
    if (owned || notOwned) {
        size_t start = line.find(owned ? L"[Owned] " : L"[Not Owned] ");
        if (start != std::wstring::npos) {
            start += owned ? 8u : 12u;
            std::wstring id = line.substr(start);
            while (!id.empty() && (id.back() == L'\r' || id.back() == L'\n' || id.back() == L' '))
                id.pop_back();
            auto it = g_dlcIndex.find(id);
            if (it != g_dlcIndex.end()) {
                auto& d = g_dlcItems[it->second];
                d.currentOwned = owned;
                if (owned) d.timesOwned++;
            }
        }
        return;
    }

    // "GetEntitlementsCount: N"
    auto pos2 = line.find(L"GetEntitlementsCount:");
    if (pos2 != std::wstring::npos) {
        std::wstring rest = line.substr(pos2 + 21);
        size_t ns = rest.find_first_not_of(L" \t");
        if (ns != std::wstring::npos)
            g_entitlementCount = _wtoi(rest.c_str() + ns);
    }
}

static void RebuildDlcList() {
    if (!g_dlcList) return;
    SendMessage(g_dlcList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_dlcList);
    for (int i = 0; i < (int)g_dlcItems.size(); i++) {
        auto& d = g_dlcItems[i];

        // Look up title from catalog packet, fall back to empty
        std::wstring title;
        auto it = g_dlcTitles.find(d.id);
        if (it != g_dlcTitles.end()) title = it->second;

        LVITEM lvi{}; lvi.mask = LVIF_TEXT; lvi.iItem = i;
        lvi.pszText = (LPWSTR)d.id.c_str();
        ListView_InsertItem(g_dlcList, &lvi);
        ListView_SetItemText(g_dlcList, i, 1, (LPWSTR)(title.empty() ? L"\u2014" : title.c_str()));
        wchar_t buf[16];
        swprintf_s(buf, L"%d", d.timesQueried);
        ListView_SetItemText(g_dlcList, i, 2, buf);
        swprintf_s(buf, L"%d", d.timesOwned);
        ListView_SetItemText(g_dlcList, i, 3, buf);
        ListView_SetItemText(g_dlcList, i, 4,
            (LPWSTR)(d.currentOwned ? L"Owned" : L"Not Owned"));
    }
    SendMessage(g_dlcList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_dlcList, nullptr, TRUE);
    UpdateDlcStats();
}

// ── Log tail ──────────────────────────────────────────────────────────────────
static void TailLogFile() {
    if (g_logPath.empty()) return;
    HANDLE hf = CreateFileW(g_logPath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER fs{}; GetFileSizeEx(hf, &fs);
    if (fs.QuadPart <= g_logFilePos) { CloseHandle(hf); return; }
    LARGE_INTEGER sp{}; sp.QuadPart = g_logFilePos;
    SetFilePointerEx(hf, sp, nullptr, FILE_BEGIN);
    std::string raw; raw.resize((size_t)(fs.QuadPart - g_logFilePos));
    DWORD got = 0; ReadFile(hf, raw.data(), (DWORD)raw.size(), &got, nullptr);
    CloseHandle(hf);
    g_logFilePos += got;
    if (!got) return;

    int prevCount = (int)g_logLines.size();
    bool anyDlc = false;
    std::istringstream ss(raw); std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::wstring wl = Utf8ToWide(line.c_str());
        g_logLines.push_back({ wl, LogLineColor(wl) });
        if (wl.find(L"[DLC]") != std::wstring::npos) { ParseDlcLine(wl); anyDlc = true; }
    }

    // Rolling cap to avoid unbounded memory growth
    if ((int)g_logLines.size() > LOG_MAX_LINES) {
        int remove = (int)g_logLines.size() - LOG_TRIM_TO;
        g_logLines.erase(g_logLines.begin(), g_logLines.begin() + remove);
        g_logDisplayedCount = 0;   // force full rebuild
    }

    bool hasFilter = GetWindowTextLengthW(g_logFilter) > 0;
    if (g_curTab == TAB_LOG) {
        if (hasFilter || g_logDisplayedCount == 0)
            RebuildLogEdit();
        else
            AppendNewLogLines(prevCount);
    }
    // Don't touch hidden controls — RebuildLogEdit/RebuildDlcList are called
    // from ShowTab when the user actually switches to those tabs.
    if (anyDlc && g_curTab == TAB_DLC) RebuildDlcList();
}

// ── Achievement list ──────────────────────────────────────────────────────────
static void RebuildView() {
    if (!g_list) return;
    wchar_t fbuf[256] = {};
    GetWindowTextW(g_filter, fbuf, 256);
    std::wstring fl = fbuf;
    std::transform(fl.begin(), fl.end(), fl.begin(), ::towlower);
    int combo = (int)SendMessage(g_combo, CB_GETCURSEL, 0, 0);

    g_view.clear();
    for (int i = 0; i < (int)g_achs.size(); i++) {
        auto& a = g_achs[i];
        if (combo == 1 && a.state != WireUnlockState::Locked)   continue;
        if (combo == 2 && a.state != WireUnlockState::Unlocked) continue;
        if (combo == 3 && !a.isHidden)                          continue;
        if (!fl.empty()) {
            std::wstring nl = a.name, il = a.id;
            std::transform(nl.begin(), nl.end(), nl.begin(), ::towlower);
            std::transform(il.begin(), il.end(), il.begin(), ::towlower);
            if (nl.find(fl) == std::wstring::npos && il.find(fl) == std::wstring::npos)
                continue;
        }
        g_view.push_back(i);
    }

    SendMessage(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);
    for (int vi = 0; vi < (int)g_view.size(); vi++) {
        auto& a = g_achs[g_view[vi]];
        LVITEM lvi{}; lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = vi; lvi.lParam = g_view[vi];
        const wchar_t* st = a.state == WireUnlockState::Unlocked  ? L"\u2714  Unlocked"
                          : a.state == WireUnlockState::Unlocking ? L"\u27F3  Unlocking"
                          : L"\u2014  Locked";
        lvi.pszText = (LPWSTR)st;
        ListView_InsertItem(g_list, &lvi);
        ListView_SetItemText(g_list, vi, 1, (LPWSTR)a.name.c_str());
        ListView_SetItemText(g_list, vi, 2, (LPWSTR)a.desc.c_str());
        ListView_SetItemText(g_list, vi, 3, (LPWSTR)a.id.c_str());
    }
    SendMessage(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, nullptr, TRUE);
    UpdateStats();
}

static void HandleAchList(const uint8_t* data, uint32_t size) {
    if (size < sizeof(AchListHeader)) return;
    auto* lh = (const AchListHeader*)data;
    if (lh->count == 0) { RebuildView(); return; }
    size_t eb = lh->count * sizeof(AchEntry);
    if (size < sizeof(AchListHeader) + eb + lh->blobSize) return;
    auto*  entries = (const AchEntry*)(data + sizeof(AchListHeader));
    const char* blob = (const char*)(data + sizeof(AchListHeader) + eb);
    g_achs.clear(); g_achs.reserve(lh->count);
    for (uint32_t i = 0; i < lh->count; i++) {
        auto& e = entries[i];
        if (e.idOff >= lh->blobSize || e.nameOff >= lh->blobSize || e.descOff >= lh->blobSize)
            continue;
        Ach a;
        a.id       = Utf8ToWide(blob + e.idOff);
        a.name     = Utf8ToWide(blob + e.nameOff);
        a.desc     = Utf8ToWide(blob + e.descOff);
        a.isHidden = e.isHidden != 0;
        a.state    = e.state;
        g_achs.push_back(std::move(a));
    }
    RebuildView();
}

static void HandleDlcCatalog(const uint8_t* data, uint32_t size) {
    if (size < sizeof(DlcCatalogHeader)) return;
    auto* hdr = (const DlcCatalogHeader*)data;
    if (hdr->count == 0) return;
    size_t eb = hdr->count * sizeof(DlcCatalogEntry);
    if (size < sizeof(DlcCatalogHeader) + eb + hdr->blobSize) return;
    auto*       entries = (const DlcCatalogEntry*)(data + sizeof(DlcCatalogHeader));
    const char* blob    = (const char*)(data + sizeof(DlcCatalogHeader) + eb);
    g_dlcTitles.clear();
    for (uint32_t i = 0; i < hdr->count; i++) {
        auto& e = entries[i];
        if (e.idOff >= hdr->blobSize || e.titleOff >= hdr->blobSize) continue;
        std::wstring id    = Utf8ToWide(blob + e.idOff);
        std::wstring title = Utf8ToWide(blob + e.titleOff);
        if (!id.empty()) g_dlcTitles[id] = title;
    }
    // Refresh DLC list if already populated from log parsing and tab is visible
    if (g_curTab == TAB_DLC && !g_dlcItems.empty()) RebuildDlcList();
    // Toast only when catalog has entries
    if (!g_dlcTitles.empty()) {
        wchar_t sb[128];
        swprintf_s(sb, L"Received %d DLC titles from Epic catalog", (int)g_dlcTitles.size());
        PushToast(L"DLC Catalog received", sb, C_PURPLE);
    }
}

// ── Pipe ──────────────────────────────────────────────────────────────────────
static void Disconnect() {
    if (g_pipe != INVALID_HANDLE_VALUE) { CloseHandle(g_pipe); g_pipe = INVALID_HANDLE_VALUE; }
    g_pipeBuf.clear();
    g_connected = false;
    EnableWindow(g_btnUnlock,  FALSE);
    EnableWindow(g_btnAll,     FALSE);
    EnableWindow(g_btnRefresh, FALSE);
    InvalidateRect(g_btnUnlock,  nullptr, FALSE);
    InvalidateRect(g_btnAll,     nullptr, FALSE);
    InvalidateRect(g_btnRefresh, nullptr, FALSE);
    SetStatusText(L"Not connected  \u2014  launch game first", C_TEXTDIM);
}

static void ProcessPacket(const PktHeader& hdr, const uint8_t* payload) {
    switch (hdr.type) {

    case PktType::LogPath: {
        if (hdr.payloadSize < sizeof(LogPathPkt)) return;
        auto* lpp = (const LogPathPkt*)payload;
        g_logPath = Utf8ToWide(lpp->path);
        g_logFilePos = 0;
        g_logLines.clear(); g_logDisplayedCount = 0;
        g_dlcItems.clear(); g_dlcIndex.clear();
        g_entitlementCount = -1;
        if (g_logEdit) SetWindowTextW(g_logEdit, L"");
        TailLogFile();
        SetTimer(g_hwnd, IDT_LOGTAIL, 500, nullptr);
        break;
    }

    case PktType::AchList:
        HandleAchList(payload, hdr.payloadSize);
        EnableWindow(g_btnUnlock,  TRUE);
        EnableWindow(g_btnAll,     TRUE);
        EnableWindow(g_btnRefresh, TRUE);
        InvalidateRect(g_btnUnlock,  nullptr, FALSE);
        InvalidateRect(g_btnAll,     nullptr, FALSE);
        InvalidateRect(g_btnRefresh, nullptr, FALSE);
        {
            wchar_t sb[128];
            swprintf_s(sb, L"Connected  \u2014  %d achievements loaded", (int)g_achs.size());
            SetStatusText(sb, C_GREEN);
            PushToast(L"Achievements loaded", sb, C_ACCENT);
        }
        break;

    case PktType::AchUpdate: {
        if (hdr.payloadSize < sizeof(AchUpdatePkt)) return;
        auto* upd = (const AchUpdatePkt*)payload;
        std::wstring wid = Utf8ToWide(upd->id);
        for (auto& a : g_achs) {
            if (a.id != wid) continue;
            a.state = upd->state;
            if (upd->state == WireUnlockState::Unlocked) {
                wchar_t sb[200]; swprintf_s(sb, L"Unlocked: %s", a.name.c_str());
                SetStatusText(sb, C_GREEN);
                PushToast(a.name.c_str(), L"Achievement unlocked \u2714", C_GREEN);
            } else if (upd->state == WireUnlockState::Unlocking) {
                PushToast(a.name.c_str(), L"Unlock pending...", C_YELLOW);
            }
            break;
        }
        RebuildView();
        break;
    }

    case PktType::DlcCatalog:
        HandleDlcCatalog(payload, hdr.payloadSize);
        break;

    default: break;
    }
}

static void PipeTimerTick() {
    if (g_pipe == INVALID_HANDLE_VALUE) {
        HANDLE p = CreateFileW(EPIC_PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (p == INVALID_HANDLE_VALUE) return;
        DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
        if (!SetNamedPipeHandleState(p, &mode, nullptr, nullptr)) { CloseHandle(p); return; }
        g_pipe = p; g_pipeBuf.clear(); g_connected = true;
        SetStatusText(L"Connected  \u2014  receiving data...", C_YELLOW);
        EnableWindow(g_btnRefresh, TRUE);
        InvalidateRect(g_btnRefresh, nullptr, FALSE);
    }
    uint8_t tmp[4096]; DWORD got = 0;
    while (true) {
        BOOL ok = ReadFile(g_pipe, tmp, sizeof(tmp), &got, nullptr);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_NO_DATA) break;
            if (err == ERROR_MORE_DATA) { g_pipeBuf.insert(g_pipeBuf.end(), tmp, tmp + got); continue; }
            Disconnect(); return;
        }
        if (!got) break;
        g_pipeBuf.insert(g_pipeBuf.end(), tmp, tmp + got);
    }
    while (g_pipeBuf.size() >= sizeof(PktHeader)) {
        auto* h = (PktHeader*)g_pipeBuf.data();
        if (h->magic != EPIC_MAGIC) {
            SetStatusText(L"Protocol mismatch \u2014 rebuild both projects", C_RED);
            Disconnect(); return;
        }
        if (h->payloadSize > EPIC_MAX_PAYLOAD) { Disconnect(); return; }
        uint32_t total = (uint32_t)sizeof(PktHeader) + h->payloadSize;
        if (g_pipeBuf.size() < total) break;
        ProcessPacket(*h, g_pipeBuf.data() + sizeof(PktHeader));
        g_pipeBuf.erase(g_pipeBuf.begin(), g_pipeBuf.begin() + total);
    }
}

// ── Show/hide tab content ─────────────────────────────────────────────────────
static void ShowTab(int tab) {
    g_curTab = tab;
    bool a = (tab == TAB_ACH), lo = (tab == TAB_LOG), d = (tab == TAB_DLC);
    ShowWindow(g_list,             a  ? SW_SHOW : SW_HIDE);
    ShowWindow(g_filter,           a  ? SW_SHOW : SW_HIDE);
    ShowWindow(g_combo,            a  ? SW_SHOW : SW_HIDE);
    ShowWindow(g_btnUnlock,        a  ? SW_SHOW : SW_HIDE);
    ShowWindow(g_btnAll,           a  ? SW_SHOW : SW_HIDE);
    ShowWindow(g_btnRefresh,       a  ? SW_SHOW : SW_HIDE);
    ShowWindow(g_logEdit,          lo ? SW_SHOW : SW_HIDE);
    ShowWindow(g_logFilter,        lo ? SW_SHOW : SW_HIDE);
    ShowWindow(g_btnLogClear,      lo ? SW_SHOW : SW_HIDE);
    ShowWindow(g_btnLogAutoScroll, lo ? SW_SHOW : SW_HIDE);
    ShowWindow(g_btnLogOpen,       lo ? SW_SHOW : SW_HIDE);
    ShowWindow(g_dlcList,          d  ? SW_SHOW : SW_HIDE);
    if (a)  UpdateStats();
    if (lo) {
        // Append only lines that arrived while the tab was hidden.
        // Full rebuild only when: first open (count==0), after a trim reset,
        // or when a filter is active (filter needs to re-scan all lines).
        bool hasFilter = g_logFilter && GetWindowTextLengthW(g_logFilter) > 0;
        if (g_logDisplayedCount == 0 || hasFilter)
            RebuildLogEdit();
        else
            AppendNewLogLines(g_logDisplayedCount);  // O(new lines only)
    }
    if (d)  RebuildDlcList();
}

// ── Layout ────────────────────────────────────────────────────────────────────
static void Layout(int cx, int cy) {
    const int P           = PAD;
    const int CONTENT_TOP = HEADER_H + TAB_H;
    const int CONTENT_BOT = cy - STATUS_H;

    // Info button — header top-right, clear of all content-area controls
    SetWindowPos(g_btnInfo,    nullptr, cx - P - 32, (HEADER_H - 32) / 2, 32, 32, SWP_NOZORDER);
    SetWindowPos(g_tabBar,    nullptr, 0, HEADER_H, cx, TAB_H, SWP_NOZORDER);
    SetWindowPos(g_statusBar, nullptr, 0, CONTENT_BOT, cx, STATUS_H, SWP_NOZORDER);

    const int BW = 140;

    // ── Achievement tab ────────────────────────────────────────────────────────
    int filterW = cx - P * 3 - 130;
    SetWindowPos(g_filter,     nullptr, P,                  CONTENT_TOP + P, filterW, EDIT_H,       SWP_NOZORDER);
    SetWindowPos(g_combo,      nullptr, cx - P - 130,       CONTENT_TOP + P, 130,     EDIT_H + 130, SWP_NOZORDER);
    int by = CONTENT_TOP + P + EDIT_H + P;
    SetWindowPos(g_btnRefresh, nullptr, P,                  by, BW,      BTN_H, SWP_NOZORDER);
    SetWindowPos(g_btnUnlock,  nullptr, P + BW + P,         by, BW,      BTN_H, SWP_NOZORDER);
    SetWindowPos(g_btnAll,     nullptr, P + BW * 2 + P * 2, by, BW + 10, BTN_H, SWP_NOZORDER);
    int listTop = by + BTN_H + P;
    SetWindowPos(g_list,       nullptr, 0, listTop, cx, CONTENT_BOT - listTop, SWP_NOZORDER);

    // ── Log tab ────────────────────────────────────────────────────────────────
    // [Filter___________________] [Clear] [⇩ Auto-scroll] [↗ Open File]
    const int bClear = 80, bAuto = 120, bOpen = 100;
    int filterLogW = cx - P * 5 - bClear - bAuto - bOpen;
    int lfy = CONTENT_TOP + P;
    SetWindowPos(g_logFilter,        nullptr, P, lfy, filterLogW, EDIT_H, SWP_NOZORDER);
    int lx = P + filterLogW + P;
    SetWindowPos(g_btnLogClear,      nullptr, lx, lfy, bClear, BTN_H, SWP_NOZORDER); lx += bClear + P;
    SetWindowPos(g_btnLogAutoScroll, nullptr, lx, lfy, bAuto,  BTN_H, SWP_NOZORDER); lx += bAuto  + P;
    SetWindowPos(g_btnLogOpen,       nullptr, lx, lfy, bOpen,  BTN_H, SWP_NOZORDER);
    int logEditTop = lfy + EDIT_H + P;
    SetWindowPos(g_logEdit, nullptr, 0, logEditTop, cx, CONTENT_BOT - logEditTop, SWP_NOZORDER);

    // ── DLC tab ────────────────────────────────────────────────────────────────
    SetWindowPos(g_dlcList, nullptr, 0, CONTENT_TOP, cx, CONTENT_BOT - CONTENT_TOP, SWP_NOZORDER);
}

// ── WndProc ───────────────────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE: {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

        // Load RichEdit before creating controls
        g_hRichEdit = LoadLibraryW(L"riched20.dll");

        // Segoe UI for text, Segoe UI Symbol for icons (monochrome, recolorable)
        g_fontUI     = CreateFontW(-14, 0,0,0, FW_NORMAL,  0,0,0, DEFAULT_CHARSET,0,0, CLEARTYPE_QUALITY,0, L"Segoe UI");
        g_fontBold   = CreateFontW(-14, 0,0,0, FW_SEMIBOLD,0,0,0, DEFAULT_CHARSET,0,0, CLEARTYPE_QUALITY,0, L"Segoe UI");
        g_fontSmall  = CreateFontW(-12, 0,0,0, FW_NORMAL,  0,0,0, DEFAULT_CHARSET,0,0, CLEARTYPE_QUALITY,0, L"Segoe UI");
        g_fontSymbol = CreateFontW(-14, 0,0,0, FW_NORMAL,  0,0,0, DEFAULT_CHARSET,0,0, CLEARTYPE_QUALITY,0, L"Segoe UI Symbol");

        g_brBg     = CreateSolidBrush(C_BG);
        g_brSurf   = CreateSolidBrush(C_SURFACE);
        g_brSurf2  = CreateSolidBrush(C_SURFACE2);
        g_brSurf3  = CreateSolidBrush(C_SURFACE3);
        g_brHeader = CreateSolidBrush(C_HEADER);

        auto mkBtn = [&](LPCWSTR txt, int id) {
            HWND h = CreateWindowW(L"BUTTON", txt,
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                0,0,10,10, hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
            SetWindowSubclass(h, BtnProc, 0, (DWORD_PTR)new BtnState());
            return h;
        };

        auto mkEdit = [&](int id, LPCWSTR cue) {
            HWND h = CreateWindowExW(0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                0,0,10,10, hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
            SendMessage(h, WM_SETFONT,   (WPARAM)g_fontUI, TRUE);
            SendMessage(h, EM_SETCUEBANNER, TRUE, (LPARAM)cue);
            SetWindowSubclass(h, EditSubProc, 0, (DWORD_PTR)new EditState());
            return h;
        };

        // Tab bar
        WNDCLASSEXW wct{ sizeof(wct) };
        wct.lpfnWndProc = TabBarProc; wct.hInstance = GetModuleHandleW(nullptr);
        wct.lpszClassName = L"EpicTabBar"; wct.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassExW(&wct);
        g_tabBar = CreateWindowW(L"EpicTabBar", nullptr, WS_CHILD | WS_VISIBLE,
            0,0,10,10, hwnd, (HMENU)IDC_TAB, GetModuleHandleW(nullptr), nullptr);

        // Status bar
        WNDCLASSEXW wcs{ sizeof(wcs) };
        wcs.lpfnWndProc = StatusBarProc; wcs.hInstance = GetModuleHandleW(nullptr);
        wcs.lpszClassName = L"EpicStatus"; wcs.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassExW(&wcs);
        g_statusBar = CreateWindowW(L"EpicStatus", nullptr, WS_CHILD | WS_VISIBLE,
            0,0,10,10, hwnd, (HMENU)IDC_STATUS, GetModuleHandleW(nullptr), nullptr);

        // Header info button (always visible, positioned in header)
        g_btnInfo = mkBtn(BTN_INFO, IDC_INFO);

        // Achievement tab
        g_btnRefresh = mkBtn(BTN_REFRESH,    IDC_REFRESH);
        g_btnUnlock  = mkBtn(BTN_UNLOCK,     IDC_UNLOCK);
        g_btnAll     = mkBtn(BTN_UNLOCK_ALL, IDC_UNLOCK_ALL);
        EnableWindow(g_btnUnlock,  FALSE);
        EnableWindow(g_btnAll,     FALSE);
        EnableWindow(g_btnRefresh, FALSE);

        g_filter = mkEdit(IDC_FILTER, L"Search achievements...");

        g_combo = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            0,0,10,10, hwnd, (HMENU)IDC_COMBO, GetModuleHandleW(nullptr), nullptr);
        SendMessage(g_combo, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
        for (auto* s : { L"All", L"Locked", L"Unlocked", L"Hidden" })
            SendMessage(g_combo, CB_ADDSTRING, 0, (LPARAM)s);
        SendMessage(g_combo, CB_SETCURSEL, 0, 0);

        g_list = CreateWindowExW(0, WC_LISTVIEW, nullptr,
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
            0,0,10,10, hwnd, (HMENU)IDC_LIST, GetModuleHandleW(nullptr), nullptr);
        SendMessage(g_list, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        SetWindowTheme(g_list, L"DarkMode_Explorer", nullptr);
        ListView_SetBkColor(g_list,     C_SURFACE);
        ListView_SetTextBkColor(g_list, C_SURFACE);
        ListView_SetTextColor(g_list,   C_TEXT);
        {
            LVCOLUMN c{}; c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT; c.fmt = LVCFMT_LEFT;
            c.pszText=(LPWSTR)L"State";       c.cx=110; ListView_InsertColumn(g_list,0,&c);
            c.pszText=(LPWSTR)L"Name";        c.cx=240; ListView_InsertColumn(g_list,1,&c);
            c.pszText=(LPWSTR)L"Description"; c.cx=340; ListView_InsertColumn(g_list,2,&c);
            c.pszText=(LPWSTR)L"ID";          c.cx=200; ListView_InsertColumn(g_list,3,&c);
        }

        // Log tab
        g_logFilter        = mkEdit(IDC_LOG_FILTER, L"Filter log lines...");
        g_btnLogClear      = mkBtn(BTN_LOG_CLEAR, IDC_LOG_CLEAR);
        g_btnLogAutoScroll = mkBtn(BTN_AUTOSCROLL, IDC_LOG_AUTOSCROLL);
        g_btnLogOpen       = mkBtn(BTN_OPEN_FILE,  IDC_LOG_OPEN);

        g_logEdit = CreateWindowExW(0,
            g_hRichEdit ? L"RichEdit20W" : L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0,0,10,10, hwnd, (HMENU)IDC_LOG_EDIT, GetModuleHandleW(nullptr), nullptr);
        if (g_hRichEdit) {
            SendMessage(g_logEdit, EM_SETBKGNDCOLOR, 0, (LPARAM)C_SURFACE);
            SendMessage(g_logEdit, EM_SETLIMITTEXT, (WPARAM)(16 * 1024 * 1024), 0);
        } else {
            SendMessage(g_logEdit, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
        }

        // DLC tab — 5 columns: ID | Title | Queried | Times Owned | Status
        g_dlcList = CreateWindowExW(0, WC_LISTVIEW, nullptr,
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
            0,0,10,10, hwnd, (HMENU)IDC_DLC_LIST, GetModuleHandleW(nullptr), nullptr);
        SendMessage(g_dlcList, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
        ListView_SetExtendedListViewStyle(g_dlcList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        SetWindowTheme(g_dlcList, L"DarkMode_Explorer", nullptr);
        ListView_SetBkColor(g_dlcList,     C_SURFACE);
        ListView_SetTextBkColor(g_dlcList, C_SURFACE);
        ListView_SetTextColor(g_dlcList,   C_TEXT);
        {
            LVCOLUMN c{}; c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT; c.fmt = LVCFMT_LEFT;
            c.pszText=(LPWSTR)L"Item ID";        c.cx=280; ListView_InsertColumn(g_dlcList,0,&c);
            c.pszText=(LPWSTR)L"Title";          c.cx=240; ListView_InsertColumn(g_dlcList,1,&c);
            c.fmt = LVCFMT_CENTER;
            c.pszText=(LPWSTR)L"Times Queried";  c.cx=110; ListView_InsertColumn(g_dlcList,2,&c);
            c.pszText=(LPWSTR)L"Times Owned";    c.cx=110; ListView_InsertColumn(g_dlcList,3,&c);
            c.pszText=(LPWSTR)L"Status";         c.cx=110; ListView_InsertColumn(g_dlcList,4,&c);
        }

        // Row height via shared imagelist
        HIMAGELIST hil = ImageList_Create(1, 24, ILC_COLOR, 1, 1);
        ListView_SetImageList(g_list,    hil, LVSIL_SMALL);
        ListView_SetImageList(g_dlcList, hil, LVSIL_SMALL);

        ShowTab(TAB_ACH);
        SetTimer(hwnd, IDT_PIPE, 250, nullptr);
        return 0;
    }

    case WM_APP + 1:  // tab selected from TabBarProc
        ShowTab((int)wp);
        { RECT r; GetClientRect(hwnd, &r); Layout(r.right, r.bottom); }
        return 0;

    case WM_TIMER:
        if (wp == IDT_PIPE)    PipeTimerTick();
        if (wp == IDT_TOAST)   ToastTick();
        if (wp == IDT_LOGTAIL) TailLogFile();
        return 0;

    case WM_SIZE:
        // SIZE_MINIMIZED passes lp=0 — Layout with zero dimensions produces
        // negative control heights that persist until the next valid resize.
        if (wp != SIZE_MINIMIZED)
            Layout(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wp; RECT r; GetClientRect(hwnd, &r);
        FillRect(hdc, &r, g_brBg);
        RECT hdr = { 0, 0, r.right, HEADER_H };
        FillRect(hdc, &hdr, g_brHeader);
        RECT al  = { 0, HEADER_H - 2, r.right, HEADER_H };
        FillRectColor(hdc, al, C_ACCENT);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        SetBkMode(hdc, TRANSPARENT);
        SelectObject(hdc, g_fontBold);
        SetTextColor(hdc, C_TEXT);
        RECT tr = { 16, 6, 500, 30 };
        DrawTextW(hdc, L"Epic Achievement Unlocker", -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE);
        SelectObject(hdc, g_fontSmall);
        SetTextColor(hdc, C_TEXTDIM);
        RECT vr = { 16, 28, 500, HEADER_H - 4 };
        DrawTextW(hdc, L"Epic GUI", -1, &vr, DT_LEFT | DT_TOP | DT_SINGLELINE);
        EndPaint(hwnd, &ps); return 0;
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp; HWND ctrl = (HWND)lp;
        if (ctrl == g_logEdit && g_hRichEdit) break;
        SetBkColor(hdc, C_SURFACE2); SetTextColor(hdc, C_TEXT);
        return (LRESULT)g_brSurf2;
    }
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = (HDC)wp;
        SetBkColor(hdc, C_SURFACE2); SetTextColor(hdc, C_TEXT);
        return (LRESULT)g_brSurf2;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, C_TEXT);
        return (LRESULT)g_brBg;
    }

    case WM_DESTROY:
        KillTimer(hwnd, IDT_PIPE);
        KillTimer(hwnd, IDT_TOAST);
        KillTimer(hwnd, IDT_LOGTAIL);
        Disconnect();
        DeleteObject(g_fontUI); DeleteObject(g_fontBold);
        DeleteObject(g_fontSmall); DeleteObject(g_fontSymbol);
        DeleteObject(g_brBg); DeleteObject(g_brSurf); DeleteObject(g_brSurf2);
        DeleteObject(g_brSurf3); DeleteObject(g_brHeader);
        if (g_hRichEdit) FreeLibrary(g_hRichEdit);
        PostQuitMessage(0); return 0;

    case WM_NOTIFY: {
        auto* nhdr = (NMHDR*)lp;

        if (nhdr->hwndFrom == g_list) {
            if (nhdr->code == NM_DBLCLK) {
                auto* ni = (NMITEMACTIVATE*)lp;
                if (ni->iItem >= 0 && ni->iItem < (int)g_view.size()) {
                    int idx = g_view[ni->iItem];
                    if (g_achs[idx].state == WireUnlockState::Locked) {
                        CmdUnlockPkt cmd{};
                        WideCharToMultiByte(CP_UTF8, 0, g_achs[idx].id.c_str(), -1,
                                            cmd.id, 127, nullptr, nullptr);
                        if (SendPkt(PktType::CmdUnlock, &cmd, sizeof(cmd))) {
                            wchar_t sb[200];
                            swprintf_s(sb, L"Unlock sent: %s", g_achs[idx].name.c_str());
                            SetStatusText(sb, C_YELLOW);
                            PushToast(g_achs[idx].name.c_str(), L"Unlock command sent", C_YELLOW);
                        }
                    }
                }
            }
            if (nhdr->code == NM_CUSTOMDRAW) {
                auto* cd = (NMLVCUSTOMDRAW*)lp;
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    int vi = (int)cd->nmcd.dwItemSpec;
                    cd->clrTextBk = (vi % 2 == 0) ? C_SURFACE : C_SURFACE2;
                    if (vi >= 0 && vi < (int)g_view.size()) {
                        auto& a = g_achs[g_view[vi]];
                        cd->clrText = a.state == WireUnlockState::Unlocked  ? C_GREEN
                                    : a.state == WireUnlockState::Unlocking ? C_YELLOW
                                    : C_TEXTSUB;
                    }
                    return CDRF_DODEFAULT;
                }
            }
        }

        if (nhdr->hwndFrom == g_dlcList && nhdr->code == NM_CUSTOMDRAW) {
            auto* cd = (NMLVCUSTOMDRAW*)lp;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                int idx = (int)cd->nmcd.dwItemSpec;
                cd->clrTextBk = (idx % 2 == 0) ? C_SURFACE : C_SURFACE2;
                if (idx >= 0 && idx < (int)g_dlcItems.size())
                    cd->clrText = g_dlcItems[idx].currentOwned ? C_GREEN : C_TEXTSUB;
                return CDRF_DODEFAULT;
            }
        }
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_INFO:
            MessageBoxW(hwnd,
                L"\u2605  Achievements tab\r\n"
                L"Double-click or select + Unlock to unlock an achievement.\r\n"
                L"In fullscreen exclusive mode, use double-click.\r\n\r\n"
                L"\u2630  Logs tab\r\n"
                L"Live tail of ScreamAPI.log. Color-coded by level and category.\r\n"
                L"[ERROR]=red  [WARN]=yellow  [DLC]=purple  [ACH]=green\r\n"
                L"Use the filter box to search. Toggle Auto-scroll to pin the view.\r\n\r\n"
                L"\u229E  DLC tab\r\n"
                L"Item ID, title (from Epic catalog if available), times queried,\r\n"
                L"times responded as Owned, and current status. Parsed from the log.",
                L"How to Use", MB_OK | MB_ICONINFORMATION);
            break;
        case IDC_FILTER:
            if (HIWORD(wp) == EN_CHANGE) RebuildView();
            break;
        case IDC_COMBO:
            if (HIWORD(wp) == CBN_SELCHANGE) RebuildView();
            break;
        case IDC_REFRESH:
            if (SendPkt(PktType::CmdRefresh)) {
                SetStatusText(L"\u21BA  Refresh sent  \u2014  waiting for game...", C_YELLOW);
                PushToast(L"Refresh requested", L"Waiting for game response", C_ACCENT);
            }
            break;
        case IDC_UNLOCK: {
            int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
            if (sel < 0 || sel >= (int)g_view.size()) {
                SetStatusText(L"Select an achievement first", C_TEXTDIM); break;
            }
            int idx = g_view[sel];
            if (g_achs[idx].state != WireUnlockState::Locked) {
                SetStatusText(L"Already unlocked", C_TEXTDIM); break;
            }
            CmdUnlockPkt cmd{};
            WideCharToMultiByte(CP_UTF8, 0, g_achs[idx].id.c_str(), -1,
                                cmd.id, 127, nullptr, nullptr);
            if (SendPkt(PktType::CmdUnlock, &cmd, sizeof(cmd))) {
                wchar_t sb[200]; swprintf_s(sb, L"Unlock sent: %s", g_achs[idx].name.c_str());
                SetStatusText(sb, C_YELLOW);
                PushToast(g_achs[idx].name.c_str(), L"Unlock command sent", C_YELLOW);
            }
            break;
        }
        case IDC_UNLOCK_ALL:
            if (MessageBoxW(hwnd, L"Unlock ALL achievements?", L"Confirm",
                            MB_YESNO | MB_ICONQUESTION) == IDYES) {
                if (SendPkt(PktType::CmdUnlockAll)) {
                    SetStatusText(L"\u26A1  Unlock All sent  \u2014  waiting for confirmations...", C_YELLOW);
                    PushToast(L"Unlock All sent", L"Waiting for confirmations", C_YELLOW);
                }
            }
            break;
        case IDC_LOG_FILTER:
            if (HIWORD(wp) == EN_CHANGE) RebuildLogEdit();
            break;
        case IDC_LOG_CLEAR:
            g_logLines.clear(); g_logDisplayedCount = 0;
            if (g_logEdit) SetWindowTextW(g_logEdit, L"");
            break;
        case IDC_LOG_AUTOSCROLL:
            g_logAutoScroll = !g_logAutoScroll;
            InvalidateRect(g_btnLogAutoScroll, nullptr, FALSE);
            if (g_logAutoScroll) {
                SendMessage(g_logEdit, EM_SETSEL, -1, -1);
                SendMessage(g_logEdit, EM_SCROLLCARET, 0, 0);
            }
            break;
        case IDC_LOG_OPEN:
            if (!g_logPath.empty())
                ShellExecuteW(hwnd, L"open", g_logPath.c_str(), nullptr, nullptr, SW_SHOW);
            else
                SetStatusText(L"No log path yet  \u2014  connect to game first", C_TEXTDIM);
            break;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Entry ─────────────────────────────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow) {
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"EpicAchievementUnlocker_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Epic Achievement Unlocker is already running.",
                    L"Already Running", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    (void)hMutex;

    INITCOMMONCONTROLSEX ice{ sizeof(ice), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&ice);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"EpicGUI";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm       = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hbrBackground = CreateSolidBrush(C_BG);
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, L"EpicGUI", L"Epic Achievement Unlocker",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 700,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    return (int)m.wParam;
}
