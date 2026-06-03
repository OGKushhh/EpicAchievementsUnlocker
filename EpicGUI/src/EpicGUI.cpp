// EpicGUI.cpp — Epic Achievement Unlocker
// Standalone Win32 GUI. Connects to ScreamAPI DLL via \\.\pipe\EpicGUI.
// Architecture: single-threaded. A timer drives pipe reads via PeekNamedPipe
// so we never block the message loop and never need cross-thread PostMessage.

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include "pipe_protocol.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ── IDs ───────────────────────────────────────────────────────────────────────
#define IDC_LIST        100
#define IDC_UNLOCK      101
#define IDC_UNLOCK_ALL  102
#define IDC_REFRESH     103
#define IDC_FILTER      104
#define IDC_COMBO       105
#define IDC_STATUS      106
#define IDT_PIPE        1

// ── Data ──────────────────────────────────────────────────────────────────────
struct Ach {
    std::wstring id, name, desc;
    bool isHidden;
    WireUnlockState state;
};

static HWND  g_hwnd    = nullptr;
static HWND  g_list    = nullptr;
static HWND  g_filter  = nullptr;
static HWND  g_combo   = nullptr;
static HWND  g_status  = nullptr;
static HWND  g_btnUnlock = nullptr;
static HWND  g_btnAll    = nullptr;
static HWND  g_btnRefresh= nullptr;

static std::vector<Ach> g_achs;
static std::vector<int> g_view;
static HANDLE g_pipe   = INVALID_HANDLE_VALUE;

// Accumulation buffer for partial pipe reads
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
    wchar_t buf[128];
    swprintf_s(buf, L"%d / %d unlocked — showing %d", unlocked, total, (int)g_view.size());
    SetStatus(buf);
}

// ── Disconnect ────────────────────────────────────────────────────────────────
static void Disconnect() {
    if (g_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
    g_pipeBuf.clear();
    EnableWindow(g_btnUnlock,  FALSE);
    EnableWindow(g_btnAll,     FALSE);
    EnableWindow(g_btnRefresh, FALSE);
    SetStatus(L"Not connected — launch game first");
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

    // Rebuild list content
    SendMessage(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);
    for (int vi = 0; vi < (int)g_view.size(); vi++) {
        auto& a = g_achs[g_view[vi]];
        LVITEM lvi{};
        lvi.mask    = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem   = vi;
        lvi.lParam  = g_view[vi];

        const wchar_t* stateStr =
            a.state == WireUnlockState::Unlocked  ? L"Unlocked"  :
            a.state == WireUnlockState::Unlocking ? L"Unlocking" : L"Locked";
        lvi.pszText = (LPWSTR)stateStr;
        ListView_InsertItem(g_list, &lvi);
        ListView_SetItemText(g_list, vi, 1, (LPWSTR)a.name.c_str());
        ListView_SetItemText(g_list, vi, 2, (LPWSTR)a.desc.c_str());
        ListView_SetItemText(g_list, vi, 3, (LPWSTR)a.id.c_str());
    }
    SendMessage(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, nullptr, TRUE);
    UpdateStats();
}

// ── Parse incoming AchList packet ─────────────────────────────────────────────
static void HandleAchList(const uint8_t* data, uint32_t size) {
    if (size < sizeof(AchListHeader)) return;
    auto* lh = reinterpret_cast<const AchListHeader*>(data);
    if (lh->count == 0) { RebuildView(); return; }

    size_t entryBytes = lh->count * sizeof(AchEntry);
    if (size < sizeof(AchListHeader) + entryBytes + lh->blobSize) {
        SetStatus(L"Received malformed achievement list"); return;
    }

    auto* entries = reinterpret_cast<const AchEntry*>(data + sizeof(AchListHeader));
    const char* blob = reinterpret_cast<const char*>(data + sizeof(AchListHeader) + entryBytes);

    g_achs.clear();
    g_achs.reserve(lh->count);
    for (uint32_t i = 0; i < lh->count; i++) {
        auto& e = entries[i];
        if (e.idOff >= lh->blobSize || e.nameOff >= lh->blobSize || e.descOff >= lh->blobSize) continue;
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

// ── Process a single complete packet from g_pipeBuf ──────────────────────────
static void ProcessPacket(const PktHeader& hdr, const uint8_t* payload) {
    if (hdr.type == PktType::AchList) {
        HandleAchList(payload, hdr.payloadSize);
        EnableWindow(g_btnUnlock,  TRUE);
        EnableWindow(g_btnAll,     TRUE);
        EnableWindow(g_btnRefresh, TRUE);
        SetStatus(L"Connected");
        UpdateStats();
    } else if (hdr.type == PktType::AchUpdate) {
        if (hdr.payloadSize < sizeof(AchUpdatePkt)) return;
        auto* upd = reinterpret_cast<const AchUpdatePkt*>(payload);
        std::wstring wid = Utf8ToWide(upd->id);
        for (auto& a : g_achs)
            if (a.id == wid) { a.state = upd->state; break; }
        RebuildView();
    }
}

// ── Timer-driven pipe read (non-blocking) ─────────────────────────────────────
static void PipeTimerTick() {
    if (g_pipe == INVALID_HANDLE_VALUE) {
        // Try to connect
        HANDLE p = CreateFileW(EPIC_PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (p == INVALID_HANDLE_VALUE) return;
        DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
        if (!SetNamedPipeHandleState(p, &mode, nullptr, nullptr)) {
            CloseHandle(p); return;
        }
        g_pipe = p;
        g_pipeBuf.clear();
        SetStatus(L"Connected — waiting for achievements...");
        EnableWindow(g_btnRefresh, TRUE);
    }

    // Read whatever is available without blocking
    uint8_t tmp[4096];
    DWORD got = 0;
    while (true) {
        BOOL ok = ReadFile(g_pipe, tmp, sizeof(tmp), &got, nullptr);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_NO_DATA) break;       // nothing available right now — fine
            if (err == ERROR_MORE_DATA) {           // partial — append and continue
                g_pipeBuf.insert(g_pipeBuf.end(), tmp, tmp + got);
                continue;
            }
            Disconnect(); return;                   // real error — pipe broken
        }
        if (got == 0) break;
        g_pipeBuf.insert(g_pipeBuf.end(), tmp, tmp + got);
    }

    // Process all complete packets in g_pipeBuf
    while (g_pipeBuf.size() >= sizeof(PktHeader)) {
        auto* hdr = reinterpret_cast<PktHeader*>(g_pipeBuf.data());
        if (hdr->magic != EPIC_MAGIC) {
            SetStatus(L"Protocol mismatch — rebuild both DLL and EpicGUI with same pipe_protocol.h");
            Disconnect(); return;
        }
        if (hdr->payloadSize > EPIC_MAX_PAYLOAD) {
            SetStatus(L"Oversized packet — disconnecting");
            Disconnect(); return;
        }
        uint32_t total = (uint32_t)sizeof(PktHeader) + hdr->payloadSize;
        if (g_pipeBuf.size() < total) break; // incomplete packet — wait for more data

        ProcessPacket(*hdr, g_pipeBuf.data() + sizeof(PktHeader));
        g_pipeBuf.erase(g_pipeBuf.begin(), g_pipeBuf.begin() + total);
    }
}

// ── Layout ────────────────────────────────────────────────────────────────────
static void Layout(int cx, int cy) {
    const int P = 8, BH = 28, BW = 120, FH = 24, CW = 110, SH = 22;
    int y = P;
    SetWindowPos(g_filter,     nullptr, P, y, cx - P*3 - CW, FH, SWP_NOZORDER);
    SetWindowPos(g_combo,      nullptr, cx - P - CW, y, CW, FH + 120, SWP_NOZORDER);
    y += FH + P;
    int x = P;
    SetWindowPos(g_btnRefresh, nullptr, x, y, BW, BH, SWP_NOZORDER); x += BW + P;
    SetWindowPos(g_btnUnlock,  nullptr, x, y, BW, BH, SWP_NOZORDER); x += BW + P;
    SetWindowPos(g_btnAll,     nullptr, x, y, BW + 20, BH, SWP_NOZORDER);
    y += BH + P;
    SetWindowPos(g_status,     nullptr, 0, cy - SH, cx, SH, SWP_NOZORDER);
    SetWindowPos(g_list,       nullptr, 0, y, cx, cy - y - SH, SWP_NOZORDER);
}

// ── WndProc ───────────────────────────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        auto mkWnd = [&](LPCWSTR cls, LPCWSTR txt, DWORD style, int id) {
            HWND h = CreateWindowW(cls, txt, WS_CHILD|WS_VISIBLE|style,
                                   0,0,0,0, hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
            SendMessage(h, WM_SETFONT, (WPARAM)f, TRUE);
            return h;
        };
        g_filter  = mkWnd(L"EDIT",   L"", ES_AUTOHSCROLL, IDC_FILTER);
        SendMessage(g_filter, EM_SETCUEBANNER, TRUE, (LPARAM)L"Filter by name or ID...");

        g_combo   = CreateWindowW(L"COMBOBOX", nullptr,
                        WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,
                        0,0,0,0, hwnd, (HMENU)IDC_COMBO, GetModuleHandleW(nullptr), nullptr);
        SendMessage(g_combo, WM_SETFONT, (WPARAM)f, TRUE);
        SendMessage(g_combo, CB_ADDSTRING, 0, (LPARAM)L"All");
        SendMessage(g_combo, CB_ADDSTRING, 0, (LPARAM)L"Locked");
        SendMessage(g_combo, CB_ADDSTRING, 0, (LPARAM)L"Unlocked");
        SendMessage(g_combo, CB_ADDSTRING, 0, (LPARAM)L"Hidden");
        SendMessage(g_combo, CB_SETCURSEL, 0, 0);

        g_btnRefresh = mkWnd(L"BUTTON", L"Refresh",         BS_PUSHBUTTON, IDC_REFRESH);
        g_btnUnlock  = mkWnd(L"BUTTON", L"Unlock Selected", BS_PUSHBUTTON, IDC_UNLOCK);
        g_btnAll     = mkWnd(L"BUTTON", L"Unlock All",      BS_PUSHBUTTON, IDC_UNLOCK_ALL);
        g_status     = mkWnd(L"STATIC", L"Not connected",   SS_SUNKEN,     IDC_STATUS);

        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEW, nullptr,
                     WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SHOWSELALWAYS|LVS_SINGLESEL,
                     0,0,0,0, hwnd, (HMENU)IDC_LIST, GetModuleHandleW(nullptr), nullptr);
        SendMessage(g_list, WM_SETFONT, (WPARAM)f, TRUE);
        ListView_SetExtendedListViewStyle(g_list,
            LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES|LVS_EX_DOUBLEBUFFER);

        LVCOLUMN lvc{}; lvc.mask = LVCF_TEXT|LVCF_WIDTH|LVCF_FMT; lvc.fmt = LVCFMT_LEFT;
        lvc.pszText=(LPWSTR)L"State";       lvc.cx=85;  ListView_InsertColumn(g_list,0,&lvc);
        lvc.pszText=(LPWSTR)L"Name";        lvc.cx=240; ListView_InsertColumn(g_list,1,&lvc);
        lvc.pszText=(LPWSTR)L"Description"; lvc.cx=340; ListView_InsertColumn(g_list,2,&lvc);
        lvc.pszText=(LPWSTR)L"ID";          lvc.cx=220; ListView_InsertColumn(g_list,3,&lvc);

        EnableWindow(g_btnUnlock,  FALSE);
        EnableWindow(g_btnAll,     FALSE);
        EnableWindow(g_btnRefresh, FALSE);

        SetTimer(hwnd, IDT_PIPE, 250, nullptr); // poll pipe every 250ms
        return 0;
    }
    case WM_TIMER:
        if (wp == IDT_PIPE) PipeTimerTick();
        return 0;

    case WM_SIZE:
        Layout(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_PIPE);
        Disconnect();
        PostQuitMessage(0);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_FILTER:
            if (HIWORD(wp) == EN_CHANGE) RebuildView();
            break;
        case IDC_COMBO:
            if (HIWORD(wp) == CBN_SELCHANGE) RebuildView();
            break;
        case IDC_REFRESH:
            SendPkt(PktType::CmdRefresh);
            break;
        case IDC_UNLOCK: {
            int sel = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
            if (sel < 0) { SetStatus(L"Select an achievement first"); break; }
            LVITEM lvi{}; lvi.mask = LVIF_PARAM; lvi.iItem = sel;
            ListView_GetItem(g_list, &lvi);
            int idx = (int)lvi.lParam;
            if (idx < 0 || idx >= (int)g_achs.size()) break;
            if (g_achs[idx].state != WireUnlockState::Locked) { SetStatus(L"Already unlocked"); break; }
            CmdUnlockPkt cmd{};
            WideCharToMultiByte(CP_UTF8, 0, g_achs[idx].id.c_str(), -1, cmd.id, 127, nullptr, nullptr);
            SendPkt(PktType::CmdUnlock, &cmd, sizeof(cmd));
            break;
        }
        case IDC_UNLOCK_ALL:
            if (MessageBoxW(hwnd, L"Unlock ALL achievements?", L"Confirm",
                            MB_YESNO|MB_ICONQUESTION) == IDYES)
                SendPkt(PktType::CmdUnlockAll);
            break;
        }
        return 0;

    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lp);
        if (hdr->hwndFrom == g_list && hdr->code == NM_DBLCLK) {
            auto* ni = reinterpret_cast<NMITEMACTIVATE*>(lp);
            if (ni->iItem >= 0) {
                LVITEM lvi{}; lvi.mask = LVIF_PARAM; lvi.iItem = ni->iItem;
                ListView_GetItem(g_list, &lvi);
                int idx = (int)lvi.lParam;
                if (idx >= 0 && idx < (int)g_achs.size() && g_achs[idx].state == WireUnlockState::Locked) {
                    CmdUnlockPkt cmd{};
                    WideCharToMultiByte(CP_UTF8, 0, g_achs[idx].id.c_str(), -1, cmd.id, 127, nullptr, nullptr);
                    SendPkt(PktType::CmdUnlock, &cmd, sizeof(cmd));
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
    INITCOMMONCONTROLSEX ice{ sizeof(ice), ICC_LISTVIEW_CLASSES|ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&ice);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"EpicGUI";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, L"EpicGUI", L"Epic Achievement Unlocker",
                 WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 960, 620,
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
