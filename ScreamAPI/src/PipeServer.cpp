// PipeServer.cpp
// Named pipe server running inside the ScreamAPI DLL.
// Accepts one GUI client at a time; sends achievement list on connect;
// forwards unlock commands to AchievementManager; pushes state updates.

#include "pch.h"
#include "PipeServer.h"
#include "pipe_protocol.h"
#include "achievement_manager.h"
#include "Overlay_types.h"
#include "Overlay.h"
#include "eos_hooks.h"

#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include <mutex>
#include <map>

// Defined in eos_ecom_entitlements.cpp — returns a thread-safe snapshot of the
// catalog cache populated by DlcCatalog::fetch() during QueryEntitlements.
extern std::map<std::string, std::string> GetCatalogSnapshot();

namespace PipeServer {

static std::atomic<bool>  s_running{ false };
static std::thread        s_thread;
static HANDLE             s_pipe    = INVALID_HANDLE_VALUE;
static std::mutex         s_pipeMtx;  // guards s_pipe for NotifyUnlock
static std::wstring       s_logPath;

void SetLogPath(const std::wstring& path) {
    s_logPath = path;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static bool WriteAll(HANDLE pipe, const void* buf, DWORD len) {
    DWORD written = 0, total = 0;
    while (total < len) {
        if (!WriteFile(pipe, (const char*)buf + total, len - total, &written, nullptr) || written == 0)
            return false;
        total += written;
    }
    return true;
}

static bool ReadAll(HANDLE pipe, void* buf, DWORD len) {
    DWORD read = 0, total = 0;
    while (total < len) {
        if (!ReadFile(pipe, (char*)buf + total, len - total, &read, nullptr) || read == 0)
            return false;
        total += read;
    }
    return true;
}

static bool SendPacket(HANDLE pipe, PktType type, const void* payload, uint32_t size) {
    PktHeader hdr{ EPIC_MAGIC, type, size };
    if (!WriteAll(pipe, &hdr, sizeof(hdr))) return false;
    if (size > 0 && payload)
        if (!WriteAll(pipe, payload, size)) return false;
    return true;
}

// ── Send full achievement list to newly connected client ──────────────────────
static void SendAchList(HANDLE pipe) {
    // Snapshot under mutex to avoid data race with EOS callback thread
    std::vector<Overlay_Achievement> snapshot;
    {
        std::lock_guard<std::mutex> lk(AchievementManager::GetAchievementsMutex());
        if (Overlay::achievements && !Overlay::achievements->empty())
            snapshot = *Overlay::achievements;
    }

    if (snapshot.empty()) {
        std::vector<uint8_t> payload(sizeof(AchListHeader), 0);
        SendPacket(pipe, PktType::AchList, payload.data(), (uint32_t)payload.size());
        return;
    }

    // Build string blob
    std::string blob;
    std::vector<AchEntry> entries;
    entries.reserve(snapshot.size());

    for (auto& a : snapshot) {
        AchEntry e{};
        auto addStr = [&](const char* s) -> uint32_t {
            uint32_t off = (uint32_t)blob.size();
            if (s) blob.append(s);
            blob.push_back('\0');
            return off;
        };
        e.idOff   = addStr(a.AchievementId);
        e.nameOff = addStr(a.UnlockedDisplayName);
        e.descOff = addStr(a.UnlockedDescription);
        e.isHidden = a.IsHidden ? 1 : 0;
        e.state    = static_cast<WireUnlockState>(static_cast<int>(a.UnlockState));
        entries.push_back(e);
    }

    AchListHeader lh{};
    lh.count    = (uint32_t)entries.size();
    lh.blobSize = (uint32_t)blob.size();

    uint32_t payloadSize = sizeof(AchListHeader)
                         + (uint32_t)(entries.size() * sizeof(AchEntry))
                         + (uint32_t)blob.size();

    std::vector<uint8_t> payload;
    payload.resize(payloadSize);
    uint8_t* p = payload.data();
    memcpy(p, &lh, sizeof(lh));               p += sizeof(lh);
    memcpy(p, entries.data(), entries.size() * sizeof(AchEntry));
    p += entries.size() * sizeof(AchEntry);
    memcpy(p, blob.data(), blob.size());

    SendPacket(pipe, PktType::AchList, payload.data(), payloadSize);
    Logger::info("[PIPE] Sent %u achievements to GUI", lh.count);
}

// ── Send DLC catalog (id→title map) to newly connected client ─────────────────
static void SendDlcCatalog(HANDLE pipe) {
    auto catalog = GetCatalogSnapshot();
    if (catalog.empty()) {
        Logger::info("[PIPE] DLC catalog not yet fetched — skipping DlcCatalog packet");
        return;
    }

    // Build string blob using the same pattern as AchList
    std::string blob;
    std::vector<DlcCatalogEntry> entries;
    entries.reserve(catalog.size());

    for (auto& [id, title] : catalog) {
        DlcCatalogEntry e{};
        e.idOff    = (uint32_t)blob.size(); blob.append(id);    blob.push_back('\0');
        e.titleOff = (uint32_t)blob.size(); blob.append(title); blob.push_back('\0');
        entries.push_back(e);
    }

    DlcCatalogHeader hdr{ (uint32_t)entries.size(), (uint32_t)blob.size() };
    uint32_t payloadSize = sizeof(DlcCatalogHeader)
                         + (uint32_t)(entries.size() * sizeof(DlcCatalogEntry))
                         + (uint32_t)blob.size();

    std::vector<uint8_t> payload(payloadSize);
    uint8_t* p = payload.data();
    memcpy(p, &hdr,            sizeof(hdr));                                p += sizeof(hdr);
    memcpy(p, entries.data(),  entries.size() * sizeof(DlcCatalogEntry));   p += entries.size() * sizeof(DlcCatalogEntry);
    memcpy(p, blob.data(),     blob.size());

    SendPacket(pipe, PktType::DlcCatalog, payload.data(), payloadSize);
    Logger::info("[PIPE] Sent DLC catalog: %u entries", hdr.count);
}

// ── Handle commands from connected client ────────────────────────────────────
static void HandleClient(HANDLE pipe) {
    // Send log path first so GUI can start tailing immediately
    if (!s_logPath.empty()) {
        LogPathPkt lpp{};
        WideCharToMultiByte(CP_UTF8, 0, s_logPath.c_str(), -1, lpp.path, MAX_PATH - 1, nullptr, nullptr);
        SendPacket(pipe, PktType::LogPath, &lpp, sizeof(lpp));
        Logger::info("[PIPE] Sent log path: %S", s_logPath.c_str());
    }
    SendAchList(pipe);
    SendDlcCatalog(pipe);  // send catalog titles after achievement list

    while (s_running) {
        PktHeader hdr{};
        if (!ReadAll(pipe, &hdr, sizeof(hdr))) break;
        if (hdr.magic != EPIC_MAGIC || hdr.payloadSize > 1024u * 1024u) {
            Logger::error("[PIPE] Invalid packet magic/size — disconnecting");
            break;
        }

        std::vector<uint8_t> payload(hdr.payloadSize);
        if (hdr.payloadSize > 0)
            if (!ReadAll(pipe, payload.data(), hdr.payloadSize)) break;

        switch (hdr.type) {
        case PktType::CmdUnlock: {
            if (hdr.payloadSize < sizeof(CmdUnlockPkt)) break;
            auto* cmd = reinterpret_cast<CmdUnlockPkt*>(payload.data());
            cmd->id[127] = '\0';
            Logger::info("[PIPE] GUI requests unlock: %s", cmd->id);
            EOS_Hooks::QueueUnlock(cmd->id);  // queued — drained on game thread in Platform_Tick
            break;
        }
        case PktType::CmdUnlockAll:
            Logger::info("[PIPE] GUI requests unlock all");
            {
                std::lock_guard<std::mutex> lk(AchievementManager::GetAchievementsMutex());
                if (Overlay::achievements) {
                    for (auto& a : *Overlay::achievements)
                        if (a.UnlockState == UnlockState::Locked)
                            EOS_Hooks::QueueUnlock(a.AchievementId);
                }
            }
            break;
        case PktType::CmdRefresh:
            Logger::info("[PIPE] GUI requests refresh");
            AchievementManager::refresh();
            // SendAchList intentionally removed — refresh() is async.
            // SendUpdatedList() will be called from queryPlayerAchievementsComplete when ready.
            break;
        default:
            Logger::warn("[PIPE] Unknown command type 0x%02X", (int)hdr.type);
            break;
        }
    }
}

// ── Server loop ───────────────────────────────────────────────────────────────
static void ServerLoop() {
    while (s_running) {
        HANDLE pipe = CreateNamedPipeW(
            EPIC_PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,          // max instances — one GUI at a time
            65536, 65536,
            0,
            nullptr
        );
        if (pipe == INVALID_HANDLE_VALUE) {
            Logger::error("[PIPE] CreateNamedPipe failed (%d)", GetLastError());
            Sleep(1000);
            continue;
        }

        Logger::info("[PIPE] Waiting for GUI connection...");
        BOOL connected = ConnectNamedPipe(pipe, nullptr)
                         ? TRUE
                         : (GetLastError() == ERROR_PIPE_CONNECTED);

        if (connected && s_running) {
            Logger::info("[PIPE] GUI connected");
            {
                std::lock_guard<std::mutex> lk(s_pipeMtx);
                s_pipe = pipe;
            }
            HandleClient(pipe);
            {
                std::lock_guard<std::mutex> lk(s_pipeMtx);
                s_pipe = INVALID_HANDLE_VALUE;
            }
            Logger::info("[PIPE] GUI disconnected");
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

// ── Public API ────────────────────────────────────────────────────────────────
void Start() {
    if (s_running.exchange(true)) return;
    s_thread = std::thread(ServerLoop);
    s_thread.detach();
    Logger::info("[PIPE] Server started on %s", EPIC_PIPE_NAME_A);
}

void Stop() {
    s_running = false;
    // Unblock ConnectNamedPipe by opening+closing a dummy client
    HANDLE dummy = CreateFileW(EPIC_PIPE_NAME, GENERIC_READ, 0,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (dummy != INVALID_HANDLE_VALUE) CloseHandle(dummy);
    // Close any active connection
    std::lock_guard<std::mutex> lk(s_pipeMtx);
    if (s_pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(s_pipe, nullptr);
        CloseHandle(s_pipe);
        s_pipe = INVALID_HANDLE_VALUE;
    }
}

void SendUpdatedList() {
    std::lock_guard<std::mutex> lk(s_pipeMtx);
    if (s_pipe == INVALID_HANDLE_VALUE) return;
    SendAchList(s_pipe);
    Logger::info("[PIPE] SendUpdatedList: sent refreshed achievement list to GUI");
}

void NotifyUnlock(const char* achievementId) {
    std::lock_guard<std::mutex> lk(s_pipeMtx);
    if (s_pipe == INVALID_HANDLE_VALUE) return;

    AchUpdatePkt upd{};
    strncpy_s(upd.id, achievementId, 127);
    upd.state = WireUnlockState::Unlocked;
    SendPacket(s_pipe, PktType::AchUpdate,
               &upd, sizeof(upd));
}

} // namespace PipeServer
