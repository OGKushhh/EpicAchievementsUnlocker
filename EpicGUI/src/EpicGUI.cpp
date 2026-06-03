// EpicGUI.cpp — Epic Achievement Unlocker
// Modern dark UI with custom-drawn controls, toast notifications, icon branding.

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <string>
#include <vector>
#include <algorithm>
#include <deque>
#include "pipe_protocol.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ── Colours ───────────────────────────────────────────────────────────────────
#define C_BG         RGB(15,  17,  22)
#define C_SURFACE    RGB(22,  26,  35)
#define C_SURFACE2   RGB(30,  35,  48)
#define C_BORDER     RGB(45,  52,  70)
#define C_TEXT       RGB(220, 225, 235)
#define C_TEXTDIM    RGB(110, 120, 145)
#define C_ACCENT     RGB(59,  130, 246)
#define C_ACCENT2    RGB(99,  160, 255)
#define C_GREEN      RGB(52,  199, 120)
#define C_YELLOW     RGB(251, 191,  36)
#define C_RED        RGB(239,  68,  68)

// ── IDs ───────────────────────────────────────────────────────────────────────
#define IDC_LIST        100
#define IDC_UNLOCK      101
#define IDC_UNLOCK_ALL  102
#define IDC_REFRESH     103
#define IDC_FILTER      104
#define IDC_COMBO       105
#define IDC_STATUS      106
#define IDC_INFO        107
#define IDT_PIPE          1
#define IDT_TOAST         2

// ── Toast ─────────────────────────────────────────────────────────────────────
// CHANGED: added sub (subtitle) and barColor fields
struct Toast {
    std::wstring text;
    std::wstring sub;
    DWORD        born;
    int          alpha;
    COLORREF     barColor;
};
static std::deque<Toast> g_toasts;
static HWND g_toastWnd = nullptr;

// ── Data ──────────────────────────────────────────────────────────────────────
struct Ach {
    std::wstring id, name, desc;
    bool isHidden;
    WireUnlockState state;
};

static HWND  g_hwnd     = nullptr;
static HWND  g_list     = nullptr;
static HWND  g_filter   = nullptr;
static HWND  g_combo    = nullptr;
static HWND  g_status   = nullptr;
static HWND  g_btnUnlock  = nullptr;
static HWND  g_btnAll     = nullptr;
static HWND  g_btnRefresh = nullptr;
static HWND  g_btnInfo    = nullptr;

static HFONT g_fontUI   = nullptr;
static HFONT g_fontBold = nullptr;
static HBRUSH g_brBg    = nullptr;
static HBRUSH g_brSurf  = nullptr;
static HBRUSH g_brSurf2 = nullptr;

static std::vector<Ach>     g_achs;
static std::vector<int>     g_view;
static HANDLE               g_pipe    = INVALID_HANDLE_VALUE;
static std::vector<uint8_t> g_pipeBuf;

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

static void SetStatus(const wchar_t* msg) { SetWindowTextW(g_status, msg); }

static void UpdateStats() {
    int total = (int)g_achs.size(), unlocked = 0;
    for (auto& a : g_achs) if (a.state == WireUnlockState::Unlocked) unlocked++;
    wchar_t buf[160];
    swprintf_s(buf, L"  %d / %d unlocked  ·  showing %d", unlocked, total, (int)g_view.size());
    SetStatus(buf);
}

// ── Toast system ──────────────────────────────────────────────────────────────
static LRESULT CALLBACK ToastProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// CHANGED: ShowToast → PushToast with subtitle + color
static void PushToast(const wchar_t* line1, const wchar_t* line2, COLORREF barColor) {
    Toast t;
    t.text     = line1;
    t.sub      = line2;
    t.born     = GetTickCount();
    t.alpha    = 255;
    t.barColor = barColor;
    g_toasts.push_back(t);

    RECT r; GetWindowRect(g_hwnd, &r);
    int tw = 340, th = 58;
    int tx = r.right - tw - 16;
    int totalH = (int)g_toasts.size() * (th + 6);
    int ty = r.bottom - totalH - 16;

    if (!g_toastWnd) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc   = ToastProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"EpicToast";
        wc.hbrBackground = nullptr;
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        RegisterClassExW(&wc);
        g_toastWnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            L"EpicToast", nullptr, WS_POPUP,
            tx, ty, tw, totalH,
            g_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    SetWindowPos(g_toastWnd, HWND_TOPMOST, tx, ty, tw, totalH, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    SetLayeredWindowAttributes(g_toastWnd, 0, 245, LWA_ALPHA);
    InvalidateRect(g_toastWnd, nullptr, TRUE);
    SetTimer(g_hwnd, IDT_TOAST, 50, nullptr);
}

// CHANGED: ToastProc now draws two lines + colored accent bar per toast
static LRESULT CALLBACK ToastProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);
        int th = 58;
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, cr.right, cr.bottom);
        SelectObject(mem, bmp);

        HBRUSH bg = CreateSolidBrush(RGB(18, 22, 32));
        FillRect(mem, &cr, bg);
        DeleteObject(bg);

        SetBkMode(mem, TRANSPARENT);

        for (int i = 0; i < (int)g_toasts.size(); i++) {
            int y = i * (th + 6);
            RECT tr = { 0, y, cr.right, y + th };

            // Card background
            HBRUSH cb = CreateSolidBrush(C_SURFACE2);
            FillRect(mem, &tr, cb);
            DeleteObject(cb);

            // Colored accent bar
            HBRUSH ab = CreateSolidBrush(g_toasts[i].barColor);
            RECT bar = { 0, y, 4, y + th };
            FillRect(mem, &bar, ab);
            DeleteObject(ab);

            // Border
            HPEN pen = CreatePen(PS_SOLID, 1, C_BORDER);
            HPEN old = (HPEN)SelectObject(mem, pen);
            HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);
            HBRUSH ob = (HBRUSH)SelectObject(mem, nb);
            Rectangle(mem, 0, y, cr.right, y + th);
            SelectObject(mem, old); SelectObject(mem, ob);
            DeleteObject(pen);

            // Trophy icon (colored to match bar)
            SetTextColor(mem, g_toasts[i].barColor);
            SelectObject(mem, g_fontUI ? g_fontUI : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
            RECT ir = { 10, y, 38, y + th };
            DrawTextW(mem, L"🏆", -1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            // Main text (bold, white)
            SelectObject(mem, g_fontBold ? g_fontBold : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
            SetTextColor(mem, C_TEXT);
            RECT nr = { 42, y + 8, cr.right - 8, y + th / 2 + 4 };
            DrawTextW(mem, g_toasts[i].text.c_str(), -1, &nr,
                      DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

            // Sub label (dimmed)
            SelectObject(mem, g_fontUI ? g_fontUI : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
            SetTextColor(mem, C_TEXTDIM);
            RECT sr2 = { 42, y + th / 2 + 2, cr.right - 8, y + th - 6 };
            DrawTextW(mem, g_toasts[i].sub.c_str(), -1, &sr2,
                      DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        BitBlt(hdc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
        DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Custom button draw ────────────────────────────────────────────────────────
static void DrawButton(HDC hdc, RECT r, const wchar_t* txt, bool hot, bool pressed, bool enabled) {
    COLORREF bg = pressed  ? C_ACCENT
                : hot      ? C_ACCENT2
                : enabled  ? RGB(38, 44, 60)
                           : RGB(28, 32, 44);
    COLORREF fg = enabled  ? C_TEXT : C_TEXTDIM;
    COLORREF br = enabled  ? C_BORDER : RGB(35, 40, 55);

    HBRUSH hbr = CreateSolidBrush(bg);
    FillRect(hdc, &r, hbr);
    DeleteObject(hbr);

    HPEN pen = CreatePen(PS_SOLID, 1, br);
    HPEN op  = (HPEN)SelectObject(hdc, pen);
    HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, nb);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, 6, 6);
    SelectObject(hdc, op); SelectObject(hdc, ob);
    DeleteObject(pen);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fg);
    SelectObject(hdc, g_fontUI ? g_fontUI : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
    DrawTextW(hdc, txt, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

struct BtnState { bool hot = false, pressed = false; };

static LRESULT CALLBACK BtnProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                  UINT_PTR, DWORD_PTR dwRefData) {
    auto* s = reinterpret_cast<BtnState*>(dwRefData);
    switch (msg) {
    case WM_MOUSEMOVE:
        if (!s->hot) { s->hot = true; TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd }; TrackMouseEvent(&tme); InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    case WM_MOUSELEAVE: s->hot = false; s->pressed = false; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: s->pressed = true; InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_LBUTTONUP:
        s->pressed = false; InvalidateRect(hwnd, nullptr, FALSE);
        if (s->hot) SendMessageW(GetParent(hwnd), WM_COMMAND, GetDlgCtrlID(hwnd), (LPARAM)hwnd);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT r; GetClientRect(hwnd, &r);
        wchar_t txt[64] = {}; GetWindowTextW(hwnd, txt, 63);
        bool en = IsWindowEnabled(hwnd) != 0;
        DrawButton(hdc, r, txt, s->hot && en, s->pressed && en, en);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_NCDESTROY: delete s; return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ── Disconnect ────────────────────────────────────────────────────────────────
static void Disconnect() {
    if (g_pipe != INVALID_HANDLE_VALUE) { CloseHandle(g_pipe); g_pipe = INVALID_HANDLE_VALUE; }
    g_pipeBuf.clear();
    EnableWindow(g_btnUnlock,  FALSE);
    EnableWindow(g_btnAll,     FALSE);
    EnableWindow(g_btnRefresh, FALSE);
    if (g_btnUnlock)  InvalidateRect(g_btnUnlock,  nullptr, FALSE);
    if (g_btnAll)     InvalidateRect(g_btnAll,     nullptr, FALSE);
    if (g_btnRefresh) InvalidateRect(g_btnRefresh, nullptr, FALSE);
    SetStatus(L"  ● Not connected — launch game first");
}

// ── Send packet ───────────────────────────────────────────────────────────────
static bool SendPkt(PktType type, const void* payload = nullptr, uint32_t size = 0) {
    if (g_pipe == INVALID_HANDLE_VALUE) return false;
    PktHeader hdr{ EPIC_MAGIC, type, size };
    DWORD written;
    if (!WriteFile(g_pipe, &hdr, sizeof(hdr), &written, nullptr)) { Disconnect(); return false; }
    if (size && payload)
        if (!WriteFile(g_pipe, payload, size, &written, nullptr)) { Disconnect(); return false; }
    return true;
}

// ── Rebuild list view ─────────────────────────────────────────────────────────
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
            if (nl.find(fl) == std::wstring::npos && il.find(fl) == std::wstring::npos) continue;
        }
        g_view.push_back(i);
    }

    SendMessage(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);
    for (int vi = 0; vi < (int)g_view.size(); vi++) {
        auto& a = g_achs[g_view[vi]];
        LVITEM lvi{};
        lvi.mask   = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem  = vi;
        lvi.lParam = g_view[vi];
        const wchar_t* st =
            a.state == WireUnlockState::Unlocked  ? L"✔  Unlocked"  :
            a.state == WireUnlockState::Unlocking ? L"⟳  Unlocking" : L"—  Locked";
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

// ── Parse achievement list ────────────────────────────────────────────────────
static void HandleAchList(const uint8_t* data, uint32_t size) {
    if (size < sizeof(AchListHeader)) return;
    auto* lh = reinterpret_cast<const AchListHeader*>(data);
    if (lh->count == 0) { RebuildView(); return; }
    size_t eb = lh->count * sizeof(AchEntry);
    if (size < sizeof(AchListHeader) + eb + lh->blobSize) return;
    auto* entries = reinterpret_cast<const AchEntry*>(data + sizeof(AchListHeader));
    const char* blob = reinterpret_cast<const char*>(data + sizeof(AchListHeader) + eb);
    g_achs.clear(); g_achs.reserve(lh->count);
    for (uint32_t i = 0; i < lh->count; i++) {
        auto& e = entries[i];
        if (e.idOff >= lh->blobSize || e.nameOff >= lh->blobSize || e.descOff >= lh->blobSize) continue;
        Ach a;
        a.id = Utf8ToWide(blob + e.idOff);
        a.name = Utf8ToWide(blob + e.nameOff);
        a.desc = Utf8ToWide(blob + e.descOff);
        a.isHidden = e.isHidden != 0;
        a.state = e.state;
        g_achs.push_back(std::move(a));
    }
    RebuildView();
}

// ── Process packet ────────────────────────────────────────────────────────────
static void ProcessPacket(const PktHeader& hdr, const uint8_t* payload) {
    if (hdr.type == PktType::AchList) {
        HandleAchList(payload, hdr.payloadSize);
        EnableWindow(g_btnUnlock,  TRUE);
        EnableWindow(g_btnAll,     TRUE);
        EnableWindow(g_btnRefresh, TRUE);
        InvalidateRect(g_btnUnlock,  nullptr, FALSE);
        InvalidateRect(g_btnAll,     nullptr, FALSE);
        InvalidateRect(g_btnRefresh, nullptr, FALSE);
        // CHANGED: better status + blue info toast
        wchar_t sb[128];
        swprintf_s(sb, L"  ✔ Received %d achievements from game", (int)g_achs.size());
        SetStatus(sb);
        PushToast(L"Achievements loaded", sb + 2, C_ACCENT);
    } else if (hdr.type == PktType::AchUpdate) {
        if (hdr.payloadSize < sizeof(AchUpdatePkt)) return;
        auto* upd = reinterpret_cast<const AchUpdatePkt*>(payload);
        std::wstring wid = Utf8ToWide(upd->id);
        for (auto& a : g_achs) {
            if (a.id == wid) {
                a.state = upd->state;
                if (upd->state == WireUnlockState::Unlocked) {
                    // CHANGED: green toast = confirmed by game
                    PushToast(a.name.c_str(), L"Achievement Unlocked ✔", C_GREEN);
                    wchar_t sb2[200];
                    swprintf_s(sb2, L"  ✔ Unlocked: %s", a.name.c_str());
                    SetStatus(sb2);
                } else if (upd->state == WireUnlockState::Unlocking) {
                    // CHANGED: yellow toast = game received command, processing
                    PushToast(a.name.c_str(), L"Unlock received by game ⟳", C_YELLOW);
                }
                break;
            }
        }
        RebuildView();
    }
}

// ── Timer-driven pipe read ────────────────────────────────────────────────────
static void PipeTimerTick() {
    if (g_pipe == INVALID_HANDLE_VALUE) {
        HANDLE p = CreateFileW(EPIC_PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (p == INVALID_HANDLE_VALUE) return;
        DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
        if (!SetNamedPipeHandleState(p, &mode, nullptr, nullptr)) { CloseHandle(p); return; }
        g_pipe = p;
        g_pipeBuf.clear();
        SetStatus(L"  ● Connected — receiving achievements...");
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
        auto* hdr = reinterpret_cast<PktHeader*>(g_pipeBuf.data());
        if (hdr->magic != EPIC_MAGIC) { SetStatus(L"  ✖ Protocol mismatch — rebuild both projects"); Disconnect(); return; }
        if (hdr->payloadSize > EPIC_MAX_PAYLOAD) { Disconnect(); return; }
        uint32_t total = (uint32_t)sizeof(PktHeader) + hdr->payloadSize;
        if (g_pipeBuf.size() < total) break;
        ProcessPacket(*hdr, g_pipeBuf.data() + sizeof(PktHeader));
        g_pipeBuf.erase(g_pipeBuf.begin(), g_pipeBuf.begin() + total);
    }
}

// ── Toast timer tick ──────────────────────────────────────────────────────────
static void ToastTick() {
    if (g_toasts.empty()) { KillTimer(g_hwnd, IDT_TOAST); if (g_toastWnd) { ShowWindow(g_toastWnd, SW_HIDE); } return; }
    DWORD now = GetTickCount();
    bool changed = false;
    for (auto it = g_toasts.begin(); it != g_toasts.end(); ) {
        DWORD age = now - it->born;
        if (age > 3500) { it = g_toasts.erase(it); changed = true; }
        else if (age > 2800) { it->alpha = (int)(255 * (1.0f - (age - 2800) / 700.0f)); ++it; changed = true; }
        else ++it;
    }
    if (g_toastWnd && !g_toasts.empty()) {
        int th = 58;
        int totalH = (int)g_toasts.size() * (th + 6);
        RECT r; GetWindowRect(g_hwnd, &r);
        SetWindowPos(g_toastWnd, HWND_TOPMOST,
                     r.right - 356, r.bottom - totalH - 16,
                     340, totalH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        BYTE alpha = g_toasts.empty() ? 0 : (BYTE)g_toasts.back().alpha;
        SetLayeredWindowAttributes(g_toastWnd, 0, alpha, LWA_ALPHA);
        InvalidateRect(g_toastWnd, nullptr, TRUE);
    }
    if (g_toasts.empty() && g_toastWnd) ShowWindow(g_toastWnd, SW_HIDE);
}

// ── Layout ────────────────────────────────────────────────────────────────────
static void Layout(int cx, int cy) {
    const int P=10, BH=34, BW=136, FH=32, CW=120, SH=28, TOPBAR=46;
    // Info button in header top-right
    SetWindowPos(g_btnInfo, nullptr, cx - 36, 4, 32, 32, SWP_NOZORDER);
    // Top bar: filter + combo
    SetWindowPos(g_filter,     nullptr, P, TOPBAR, cx - P*4 - CW, FH, SWP_NOZORDER);
    SetWindowPos(g_combo,      nullptr, cx - P - CW, TOPBAR, CW, FH + 130, SWP_NOZORDER);
    // Button row
    int y = TOPBAR + FH + P, x = P;
    SetWindowPos(g_btnRefresh, nullptr, x, y, BW, BH, SWP_NOZORDER); x += BW + P;
    SetWindowPos(g_btnUnlock,  nullptr, x, y, BW, BH, SWP_NOZORDER); x += BW + P;
    SetWindowPos(g_btnAll,     nullptr, x, y, BW + 10, BH, SWP_NOZORDER);
    // Status bar
    SetWindowPos(g_status,     nullptr, 0, cy - SH, cx, SH, SWP_NOZORDER);
    // List
    int listTop = y + BH + P;
    SetWindowPos(g_list,       nullptr, 0, listTop, cx, cy - listTop - SH, SWP_NOZORDER);
}

// ── WndProc ───────────────────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE: {
        // Dark mode title bar (Windows 10 1809+)
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));

        // Fonts
        g_fontUI   = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                         DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_fontBold = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                         DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Semibold");
        g_brBg    = CreateSolidBrush(C_BG);
        g_brSurf  = CreateSolidBrush(C_SURFACE);
        g_brSurf2 = CreateSolidBrush(C_SURFACE2);

        auto mkBtn = [&](LPCWSTR txt, int id) {
            HWND h = CreateWindowW(L"BUTTON", txt,
                WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
                0,0,0,0, hwnd, (HMENU)(INT_PTR)id,
                GetModuleHandleW(nullptr), nullptr);
            SetWindowSubclass(h, BtnProc, 0, (DWORD_PTR)new BtnState());
            return h;
        };

        g_btnRefresh = mkBtn(L"↺  Refresh",         IDC_REFRESH);
        g_btnUnlock  = mkBtn(L"⚡  Unlock Selected", IDC_UNLOCK);
        g_btnAll     = mkBtn(L"🏆  Unlock All",      IDC_UNLOCK_ALL);
        g_btnInfo    = mkBtn(L"ℹ",                   IDC_INFO);
        EnableWindow(g_btnUnlock,  FALSE);
        EnableWindow(g_btnAll,     FALSE);
        EnableWindow(g_btnRefresh, FALSE);

        // Filter edit with dark background
        g_filter = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            0,0,0,0, hwnd, (HMENU)IDC_FILTER, GetModuleHandleW(nullptr), nullptr);
        SendMessage(g_filter, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
        SendMessage(g_filter, EM_SETCUEBANNER, TRUE, (LPARAM)L"  Search achievements...");

        // Combo
        g_combo = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
            0,0,0,0, hwnd, (HMENU)IDC_COMBO, GetModuleHandleW(nullptr), nullptr);
        SendMessage(g_combo, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
        for (auto* s : { L"All", L"Locked", L"Unlocked", L"Hidden" })
            SendMessage(g_combo, CB_ADDSTRING, 0, (LPARAM)s);
        SendMessage(g_combo, CB_SETCURSEL, 0, 0);

        // Status bar
        g_status = CreateWindowW(L"STATIC", L"  ● Not connected",
            WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTERIMAGE,
            0,0,0,0, hwnd, (HMENU)IDC_STATUS, GetModuleHandleW(nullptr), nullptr);
        SendMessage(g_status, WM_SETFONT, (WPARAM)g_fontUI, TRUE);

        // List view
        g_list = CreateWindowExW(0, WC_LISTVIEW, nullptr,
            WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SHOWSELALWAYS|LVS_SINGLESEL,
            0,0,0,0, hwnd, (HMENU)IDC_LIST, GetModuleHandleW(nullptr), nullptr);
        SendMessage(g_list, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
        ListView_SetExtendedListViewStyle(g_list,
            LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        // Dark theme on list
        SetWindowTheme(g_list, L"DarkMode_Explorer", nullptr);
        ListView_SetBkColor(g_list,    C_SURFACE);
        ListView_SetTextBkColor(g_list, C_SURFACE);
        ListView_SetTextColor(g_list,  C_TEXT);

        LVCOLUMN lvc{}; lvc.mask=LVCF_TEXT|LVCF_WIDTH|LVCF_FMT; lvc.fmt=LVCFMT_LEFT;
        lvc.pszText=(LPWSTR)L"State";       lvc.cx=100; ListView_InsertColumn(g_list,0,&lvc);
        lvc.pszText=(LPWSTR)L"Name";        lvc.cx=240; ListView_InsertColumn(g_list,1,&lvc);
        lvc.pszText=(LPWSTR)L"Description"; lvc.cx=340; ListView_InsertColumn(g_list,2,&lvc);
        lvc.pszText=(LPWSTR)L"ID";          lvc.cx=220; ListView_InsertColumn(g_list,3,&lvc);

        SetTimer(hwnd, IDT_PIPE, 250, nullptr);
        return 0;
    }

    case WM_TIMER:
        if (wp == IDT_PIPE)  PipeTimerTick();
        if (wp == IDT_TOAST) ToastTick();
        return 0;

    case WM_SIZE:
        Layout(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wp;
        RECT r; GetClientRect(hwnd, &r);
        FillRect(hdc, &r, g_brBg);
        // Header bar
        RECT hdr = { r.left, r.top, r.right, 40 };
        FillRect(hdc, &hdr, g_brSurf);
        // Accent line under header
        RECT line = { r.left, 39, r.right, 40 };
        HBRUSH ab = CreateSolidBrush(C_ACCENT);
        FillRect(hdc, &line, ab);
        DeleteObject(ab);
        return 1;
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp;
        SetBkColor(hdc, C_SURFACE2);
        SetTextColor(hdc, C_TEXT);
        return (LRESULT)g_brSurf2;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        HWND ctrl = (HWND)lp;
        SetBkMode(hdc, TRANSPARENT);
        if (ctrl == g_status) {
            SetTextColor(hdc, C_TEXTDIM);
            return (LRESULT)g_brSurf;
        }
        SetTextColor(hdc, C_TEXT);
        return (LRESULT)g_brBg;
    }

    case WM_CTLCOLORLISTBOX: {
        HDC hdc = (HDC)wp;
        SetBkColor(hdc, C_SURFACE2);
        SetTextColor(hdc, C_TEXT);
        return (LRESULT)g_brSurf2;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        // Title text in header
        RECT r; GetClientRect(hwnd, &r);
        RECT tr = { 14, 0, r.right, 40 };
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, C_TEXT);
        SelectObject(hdc, g_fontBold);
        DrawTextW(hdc, L"🏆  Epic Achievement Unlocker", -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, IDT_PIPE);
        KillTimer(hwnd, IDT_TOAST);
        Disconnect();
        if (g_fontUI)   DeleteObject(g_fontUI);
        if (g_fontBold) DeleteObject(g_fontBold);
        if (g_brBg)     DeleteObject(g_brBg);
        if (g_brSurf)   DeleteObject(g_brSurf);
        if (g_brSurf2)  DeleteObject(g_brSurf2);
        PostQuitMessage(0);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_INFO:
            MessageBoxW(hwnd,
                L"⚠  Fullscreen Exclusive Games\n\n"
                L"If your game runs in fullscreen exclusive mode, clicking the\n"
                L"Unlock button will minimize the game (Windows limitation).\n\n"
                L"Solutions:\n"
                L"  • Switch the game to Borderless Windowed mode\n"
                L"  • Double-click an achievement in the list to unlock it\n"
                L"    (works the same way without minimizing the game)",
                L"How to Use", MB_OK | MB_ICONINFORMATION);
            break;
        case IDC_FILTER:
            if (HIWORD(wp) == EN_CHANGE) RebuildView();
            break;
        case IDC_COMBO:
            if (HIWORD(wp) == CBN_SELCHANGE) RebuildView();
            break;
        case IDC_REFRESH:
            // CHANGED: better status + blue toast
            if (SendPkt(PktType::CmdRefresh)) {
                SetStatus(L"  ↺ Refresh sent — waiting for game...");
                PushToast(L"Refresh requested", L"Waiting for game to respond ↺", C_ACCENT);
            }
            break;
        case IDC_UNLOCK: {
            int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
            if (sel < 0) { SetStatus(L"  ✖ Select an achievement first"); break; }
            LVITEM lvi{}; lvi.mask = LVIF_PARAM; lvi.iItem = sel;
            ListView_GetItem(g_list, &lvi);
            int idx = (int)lvi.lParam;
            if (idx < 0 || idx >= (int)g_achs.size()) break;
            if (g_achs[idx].state != WireUnlockState::Locked) { SetStatus(L"  ✖ Already unlocked"); break; }
            CmdUnlockPkt cmd{};
            WideCharToMultiByte(CP_UTF8, 0, g_achs[idx].id.c_str(), -1, cmd.id, 127, nullptr, nullptr);
            if (SendPkt(PktType::CmdUnlock, &cmd, sizeof(cmd))) {
                wchar_t sb[200];
                swprintf_s(sb, L"  ⚡ Unlock sent: %s", g_achs[idx].name.c_str());
                SetStatus(sb);
                // CHANGED: yellow toast = command sent, waiting for game confirmation
                PushToast(g_achs[idx].name.c_str(), L"Unlock command sent to game ⚡", C_YELLOW);
            }
            break;
        }
        case IDC_UNLOCK_ALL:
            if (MessageBoxW(hwnd, L"Unlock ALL achievements?", L"Confirm",
                            MB_YESNO | MB_ICONQUESTION) == IDYES) {
                if (SendPkt(PktType::CmdUnlockAll)) {
                    SetStatus(L"  ⚡ Unlock All sent — waiting for confirmations...");
                    // CHANGED: yellow toast for unlock all
                    PushToast(L"Unlock All sent", L"Waiting for game confirmations ⚡", C_YELLOW);
                }
            }
            break;
        }
        return 0;

    case WM_NOTIFY: {
        auto* nhdr = reinterpret_cast<NMHDR*>(lp);
        if (nhdr->hwndFrom == g_list) {
            if (nhdr->code == NM_DBLCLK) {
                auto* ni = reinterpret_cast<NMITEMACTIVATE*>(lp);
                if (ni->iItem >= 0) {
                    LVITEM lvi{}; lvi.mask = LVIF_PARAM; lvi.iItem = ni->iItem;
                    ListView_GetItem(g_list, &lvi);
                    int idx = (int)lvi.lParam;
                    if (idx >= 0 && idx < (int)g_achs.size() && g_achs[idx].state == WireUnlockState::Locked) {
                        CmdUnlockPkt cmd{};
                        WideCharToMultiByte(CP_UTF8, 0, g_achs[idx].id.c_str(), -1, cmd.id, 127, nullptr, nullptr);
                        if (SendPkt(PktType::CmdUnlock, &cmd, sizeof(cmd))) {
                            wchar_t sb[200];
                            swprintf_s(sb, L"  ⚡ Unlock sent: %s", g_achs[idx].name.c_str());
                            SetStatus(sb);
                            // CHANGED: yellow toast on double-click unlock too
                            PushToast(g_achs[idx].name.c_str(), L"Unlock command sent to game ⚡", C_YELLOW);
                        }
                    }
                }
            }
            // Custom draw for row colors
            if (nhdr->code == NM_CUSTOMDRAW) {
                auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    LVITEM lvi{}; lvi.mask = LVIF_PARAM; lvi.iItem = (int)cd->nmcd.dwItemSpec;
                    ListView_GetItem(g_list, &lvi);
                    int idx = (int)lvi.lParam;
                    if (idx >= 0 && idx < (int)g_achs.size()) {
                        auto& a = g_achs[idx];
                        cd->clrTextBk = (cd->nmcd.dwItemSpec % 2 == 0) ? C_SURFACE : C_SURFACE2;
                        cd->clrText   =  a.state == WireUnlockState::Unlocked  ? C_GREEN
                                       : a.state == WireUnlockState::Unlocking ? C_YELLOW
                                       : C_TEXT;
                    }
                    return CDRF_DODEFAULT;
                }
            }
        }
        break;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Entry ─────────────────────────────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow) {
    // Single instance check
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"EpicAchievementUnlocker_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Epic Achievement Unlocker is already running.", L"Already Running", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
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
                 WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 980, 660,
                 nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
