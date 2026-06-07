// pipe_protocol.h
// Shared between ScreamAPI DLL and EpicGUI.exe
// Copy this file into both projects.
// Named pipe: \\.\pipe\EpicGUI

#pragma once
#include <cstdint>

#define EPIC_PIPE_NAME      L"\\\\.\\pipe\\EpicGUI"
#define EPIC_PIPE_NAME_A     "\\\\.\\pipe\\EpicGUI"

// ── Packet types ──────────────────────────────────────────────────────────────
enum class PktType : uint8_t {
    // DLL → GUI
    AchList     = 0x01,  // full achievement list dump
    AchUpdate   = 0x02,  // single achievement state changed
    LogPath     = 0x03,  // absolute path to ScreamAPI.log (UTF-8)
    DlcCatalog  = 0x04,  // full id→title map from Epic's GraphQL catalog

    // GUI → DLL
    CmdUnlock    = 0x10,  // unlock one achievement by id
    CmdUnlockAll = 0x11,  // unlock all locked achievements
    CmdRefresh   = 0x12,  // re-query definitions + player achievements
};

enum class WireUnlockState : uint8_t {
    Locked    = 0,
    Unlocked  = 1,
    Unlocking = 2,
};

#pragma pack(push, 1)

// Every packet starts with this header
struct PktHeader {
    uint32_t magic;        // always EPIC_MAGIC — detects packing/endian mismatches
    PktType  type;
    uint32_t payloadSize;  // bytes following this header
};
static constexpr uint32_t EPIC_MAGIC       = 0xABD04E21u;
static constexpr uint32_t EPIC_MAX_PAYLOAD = 8u * 1024u * 1024u; // 8 MB sanity cap

// ── AchList payload ───────────────────────────────────────────────────────────
// Header: PktType::AchList
// Payload: AchListHeader, then `count` AchEntry records, then string blob.
// String blob: null-terminated UTF-8 strings packed together.
// Each AchEntry holds byte offsets into the blob.

struct AchListHeader {
    uint32_t count;
    uint32_t blobSize;
};

struct AchEntry {
    uint32_t idOff;
    uint32_t nameOff;
    uint32_t descOff;
    uint8_t  isHidden;
    WireUnlockState state;
};

// ── AchUpdate payload ─────────────────────────────────────────────────────────
// Header: PktType::AchUpdate
// Payload: AchUpdatePkt

struct AchUpdatePkt {
    char            id[128];
    WireUnlockState state;
};

// ── CmdUnlock payload ─────────────────────────────────────────────────────────
// Header: PktType::CmdUnlock
// Payload: CmdUnlockPkt

struct CmdUnlockPkt {
    char id[128];
};

// ── CmdUnlockAll / CmdRefresh — no payload (payloadSize = 0) ──────────────────

// ── LogPath payload ───────────────────────────────────────────────────────────
// Header: PktType::LogPath
// Payload: LogPathPkt — null-terminated UTF-8 absolute path to ScreamAPI.log

struct LogPathPkt {
    char path[MAX_PATH];
};

// ── DlcCatalog payload ────────────────────────────────────────────────────────
// Header: PktType::DlcCatalog
// Payload: DlcCatalogHeader, then `count` DlcCatalogEntry records, then string blob.
// Same blob pattern as AchList: null-terminated UTF-8 strings packed together.
// Sent once on connect after AchList. Re-sent if catalog is populated later.

struct DlcCatalogHeader {
    uint32_t count;
    uint32_t blobSize;
};

struct DlcCatalogEntry {
    uint32_t idOff;
    uint32_t titleOff;
};

#pragma pack(pop)
