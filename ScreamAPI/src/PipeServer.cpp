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

namespace PipeServer {

static std::atomic<bool>  s_running{ false };
static std::thread        s_thread;
static HANDLE             s_pipe    = INVALID_HANDLE_VALUE;
static std::mutex         s_pipeMtx;  // guards s_pipe for NotifyUnlock

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

// ── Handle commands from connected client ────────────────────────────────────
static void HandleClient(HANDLE pipe) {
    SendAchList(pipe);

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
