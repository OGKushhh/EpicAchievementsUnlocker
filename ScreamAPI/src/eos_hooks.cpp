#include "pch.h"
#include "eos_hooks.h"
#include "ScreamAPI.h"
#include "achievement_manager.h"
#include "util.h"
#include "eos_compat.h"
#include "Logger.h"
#include "MinHook.h"
#include "dlc_catalog.h"
#include <mutex>
#include <vector>
#include <queue>
#include <thread>
#include <atomic>
#include <map>

namespace EOS_Hooks {

static bool hooksInitialized = false;
static HMODULE originalEOSDLL = nullptr;
static std::atomic<bool> g_bAchievementsConfigured{false};
static std::mutex g_configMutex;

// Original function pointers (filled by MinHook)
namespace Original {
    // Platform
    decltype(&EOS_Platform_Create) Platform_Create = nullptr;
    decltype(&EOS_Platform_Release) Platform_Release = nullptr;
    decltype(&EOS_Platform_Tick) Platform_Tick = nullptr;
    decltype(&EOS_Platform_GetConnectInterface) Platform_GetConnectInterface = nullptr;
    decltype(&EOS_Platform_GetAuthInterface) Platform_GetAuthInterface = nullptr;
    decltype(&EOS_Platform_GetAchievementsInterface) Platform_GetAchievementsInterface = nullptr;
    decltype(&EOS_Platform_GetEcomInterface) Platform_GetEcomInterface = nullptr;
    // Optional
    decltype(&EOS_Platform_GetUIInterface) Platform_GetUIInterface = nullptr;

    // Achievements
    decltype(&EOS_Achievements_QueryDefinitions) Achievements_QueryDefinitions = nullptr;
    decltype(&EOS_Achievements_QueryPlayerAchievements) Achievements_QueryPlayerAchievements = nullptr;
    decltype(&EOS_Achievements_UnlockAchievements) Achievements_UnlockAchievements = nullptr;
    decltype(&EOS_Achievements_AddNotifyAchievementsUnlockedV2) Achievements_AddNotifyAchievementsUnlockedV2 = nullptr;
    // Deprecated version
    decltype(&EOS_Achievements_AddNotifyAchievementsUnlocked) Achievements_AddNotifyAchievementsUnlocked = nullptr;

    // Ecom
    decltype(&EOS_Ecom_QueryOwnership) Ecom_QueryOwnership = nullptr;
    decltype(&EOS_Ecom_QueryOwnershipBySandboxIds) Ecom_QueryOwnershipBySandboxIds = nullptr;
    decltype(&EOS_Ecom_QueryOwnershipToken) Ecom_QueryOwnershipToken = nullptr;
    decltype(&EOS_Ecom_QueryEntitlements) Ecom_QueryEntitlements = nullptr;
    decltype(&EOS_Ecom_GetEntitlementsCount) Ecom_GetEntitlementsCount = nullptr;
    decltype(&EOS_Ecom_CopyEntitlementByIndex) Ecom_CopyEntitlementByIndex = nullptr;
    decltype(&EOS_Ecom_Entitlement_Release) Ecom_Entitlement_Release = nullptr;

    // Connect
    decltype(&EOS_Connect_Login) Connect_Login = nullptr;
    decltype(&EOS_Connect_GetLoggedInUserByIndex) Connect_GetLoggedInUserByIndex = nullptr;
    // Optional
    decltype(&EOS_Connect_AddNotifyLoginStatusChanged) Connect_AddNotifyLoginStatusChanged = nullptr;

    // Auth
    decltype(&EOS_Auth_Login) Auth_Login = nullptr;
    decltype(&EOS_Auth_GetLoggedInAccountByIndex) Auth_GetLoggedInAccountByIndex = nullptr;
    // Optional
    decltype(&EOS_Auth_AddNotifyLoginStatusChanged) Auth_AddNotifyLoginStatusChanged = nullptr;
}

// Helper to get the decorated name for 32-bit
static std::string GetDecoratedName(const char* baseName) {
#ifdef _WIN64
    return std::string(baseName);
#else
    std::stringstream ss;
    ss << "_" << baseName << "@";
    if (strcmp(baseName, "EOS_Platform_Create") == 0) ss << "4";
    else if (strcmp(baseName, "EOS_Platform_Release") == 0) ss << "4";
    else if (strcmp(baseName, "EOS_Platform_Tick") == 0) ss << "4";
    else if (strcmp(baseName, "EOS_Platform_GetConnectInterface") == 0) ss << "4";
    else if (strcmp(baseName, "EOS_Platform_GetAuthInterface") == 0) ss << "4";
    else if (strcmp(baseName, "EOS_Platform_GetAchievementsInterface") == 0) ss << "4";
    else if (strcmp(baseName, "EOS_Platform_GetEcomInterface") == 0) ss << "4";
    else if (strcmp(baseName, "EOS_Platform_GetUIInterface") == 0) ss << "4";
    else if (strcmp(baseName, "EOS_Achievements_QueryDefinitions") == 0) ss << "16";
    else if (strcmp(baseName, "EOS_Achievements_QueryPlayerAchievements") == 0) ss << "16";
    else if (strcmp(baseName, "EOS_Achievements_UnlockAchievements") == 0) ss << "16";
    else if (strcmp(baseName, "EOS_Achievements_AddNotifyAchievementsUnlockedV2") == 0) ss << "16";
    else if (strcmp(baseName, "EOS_Achievements_AddNotifyAchievementsUnlocked") == 0) ss << "16";
    else if (strcmp(baseName, "EOS_Ecom_QueryOwnership") == 0) ss << "16";
    else if (strcmp(baseName, "EOS_Ecom_QueryOwnershipBySandboxIds") == 0) ss << "16";
    else if (strcmp(baseName, "EOS_Ecom_QueryOwnershipToken") == 0) ss << "16";
    else if (strcmp(baseName, "EOS_Ecom_QueryEntitlements") == 0) ss << "16";
    else if (strcmp(baseName, "EOS_Ecom_GetEntitlementsCount") == 0) ss << "8";
    else if (strcmp(baseName, "EOS_Ecom_CopyEntitlementByIndex") == 0) ss << "12";
    else if (strcmp(baseName, "EOS_Ecom_Entitlement_Release") == 0) ss << "4";
    else if (strcmp(baseName, "EOS_Connect_Login") == 0) ss << "16";
    else if (strcmp(baseName, "EOS_Connect_GetLoggedInUserByIndex") == 0) ss << "8";
    else if (strcmp(baseName, "EOS_Connect_AddNotifyLoginStatusChanged") == 0) ss << "16";
    else if (strcmp(baseName, "EOS_Auth_Login") == 0) ss << "16";
    else if (strcmp(baseName, "EOS_Auth_GetLoggedInAccountByIndex") == 0) ss << "8";
    else if (strcmp(baseName, "EOS_Auth_AddNotifyLoginStatusChanged") == 0) ss << "16";
    else {
        Logger::warn("[HOOK] Unknown decorated name for: %s", baseName);
        ss << "16";
    }
    return ss.str();
#endif
}

#define INSTALL_HOOK(module, funcName, hookFunc, originalPtr) \
    do { \
        std::string targetName = GetDecoratedName(#funcName); \
        void* targetFunc = GetProcAddress(module, targetName.c_str()); \
        if (targetFunc) { \
            MH_STATUS status = MH_CreateHook(targetFunc, (void*)&hookFunc, (void**)&originalPtr); \
            if (status == MH_OK) { \
                status = MH_EnableHook(targetFunc); \
                if (status == MH_OK) { \
                    Logger::info("[HOOK] Successfully hooked: %s", targetName.c_str()); \
                } else { \
                    Logger::error("[HOOK] Failed to enable hook for %s: %d", targetName.c_str(), status); \
                    return false; \
                } \
            } else { \
                Logger::error("[HOOK] Failed to create hook for %s: %d", targetName.c_str(), status); \
                return false; \
            } \
        } else { \
            Logger::warn("[HOOK] Function not found (may be optional): %s", targetName.c_str()); \
        } \
    } while(0)

#define INSTALL_HOOK_OPTIONAL(module, funcName, hookFunc, originalPtr) \
    do { \
        std::string targetName = GetDecoratedName(#funcName); \
        void* targetFunc = GetProcAddress(module, targetName.c_str()); \
        if (targetFunc) { \
            MH_STATUS status = MH_CreateHook(targetFunc, (void*)&hookFunc, (void**)&originalPtr); \
            if (status == MH_OK) { \
                status = MH_EnableHook(targetFunc); \
                if (status == MH_OK) { \
                    Logger::info("[HOOK] Successfully hooked (optional): %s", targetName.c_str()); \
                } else { \
                    Logger::warn("[HOOK] Failed to enable optional hook for %s: %d (continuing)", targetName.c_str(), status); \
                } \
            } else { \
                Logger::warn("[HOOK] Failed to create optional hook for %s: %d (continuing)", targetName.c_str(), status); \
            } \
        } else { \
            Logger::warn("[HOOK] Optional function not found: %s (continuing)", targetName.c_str()); \
        } \
    } while(0)

// ------------------------------------------------------------------
// Pending unlock storage and retry mechanism
// ------------------------------------------------------------------
struct PendingUnlock {
    EOS_HAchievements Handle;
    EOS_Achievements_UnlockAchievementsOptions Options;
    void* ClientData;
    EOS_Achievements_OnUnlockAchievementsCompleteCallback CompletionDelegate;
};
static std::vector<PendingUnlock> g_pendingUnlocks;
static std::mutex g_pendingMutex;
static std::atomic<int> g_forcedQueriesPending{0};

// GUI unlock queue — drained on the game thread inside Platform_Tick
static std::queue<std::string> g_guiUnlockQueue;
static std::mutex              g_guiQueueMutex;

static void RetryPendingUnlocks() {
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    for (auto& p : g_pendingUnlocks) {
        Logger::info("[HOOK] Retrying pending unlock for achievement: %s", p.Options.AchievementIds[0]);
        Original::Achievements_UnlockAchievements(p.Handle, &p.Options, p.ClientData, p.CompletionDelegate);
    }
    g_pendingUnlocks.clear();
}

static void ForceAchievementsConfiguration(EOS_HAchievements handle, EOS_ProductUserId userId) {
    if (g_bAchievementsConfigured) return;
    std::lock_guard<std::mutex> lock(g_configMutex);
    if (g_bAchievementsConfigured) return;

    Logger::info("[HOOK] Forcing achievements configuration (QueryDefinitions + QueryPlayerAchievements)");

    g_forcedQueriesPending = 2;

    EOS_Achievements_QueryDefinitionsOptions defOpts = {1, userId, nullptr, nullptr, 0};
    Original::Achievements_QueryDefinitions(handle, &defOpts, nullptr,
        [](const EOS_Achievements_OnQueryDefinitionsCompleteCallbackInfo* Data) {
            Logger::debug("[HOOK] Forced QueryDefinitions result: %s", EOS_EResult_ToString(Data->ResultCode));
            if (--g_forcedQueriesPending == 0) {
                g_bAchievementsConfigured = true;
                RetryPendingUnlocks();
            }
        });

    EOS_Achievements_QueryPlayerAchievementsOptions playerOpts = {1, userId};
    Original::Achievements_QueryPlayerAchievements(handle, &playerOpts, nullptr,
        [](const EOS_Achievements_OnQueryPlayerAchievementsCompleteCallbackInfo* Data) {
            Logger::debug("[HOOK] Forced QueryPlayerAchievements result: %s", EOS_EResult_ToString(Data->ResultCode));
            if (--g_forcedQueriesPending == 0) {
                g_bAchievementsConfigured = true;
                RetryPendingUnlocks();
            }
        });
}

// ============================================================================
// HOOK IMPLEMENTATIONS
// ============================================================================

namespace Hooks {

// Platform hooks
EOS_HPlatform EOS_CALL Platform_Create(const EOS_Platform_Options* Options) {
    Logger::info("[HOOK] EOS_Platform_Create called");
    if (Options) {
        Logger::debug("[HOOK]   ApiVersion: %d", Options->ApiVersion);
        Logger::debug("[HOOK]   ProductId: %s", Options->ProductId ? Options->ProductId : "NULL");
        Logger::debug("[HOOK]   Flags: %llu", Options->Flags);
        if (Options->SandboxId && Options->SandboxId[0] != '\0') {
            Util::g_namespace_id = Options->SandboxId;
            Logger::info("[HOOK]   Captured namespace_id: %s", Options->SandboxId);
        }
        if (Config::ForceEpicOverlay()) {
            auto mOptions = const_cast<EOS_Platform_Options*>(Options);
            mOptions->Flags = 0;
            Logger::debug("[HOOK]   Disabled Epic Overlay (Flags set to 0)");
        }
    }
    EOS_HPlatform result = Original::Platform_Create(Options);
    Util::hPlatform = result;
    Logger::info("[HOOK] Platform created: %p", result);
    if (result && Config::EnableOverlay()) {
        std::thread([]() {
            Sleep(500);
            Logger::info("[HOOK] Triggering achievement manager initialization");
            AchievementManager::init();
        }).detach();
    }
    return result;
}

void EOS_CALL Platform_Release(EOS_HPlatform Handle) {
    Logger::info("[HOOK] EOS_Platform_Release called");
    Original::Platform_Release(Handle);
    Util::hPlatform = nullptr;
}

void EOS_CALL Platform_Tick(EOS_HPlatform Handle) {
    // Drain GUI unlock queue on the game thread (safe to call EOS here)
    {
        std::lock_guard<std::mutex> lk(g_guiQueueMutex);
        while (!g_guiUnlockQueue.empty()) {
            const std::string& id = g_guiUnlockQueue.front();
            Logger::info("[HOOK] Platform_Tick: draining GUI unlock: %s", id.c_str());
            AchievementManager::findAchievement(id.c_str(), [](Overlay_Achievement& a) {
                if (a.UnlockState == UnlockState::Locked)
                    AchievementManager::unlockAchievement(&a);
            });
            g_guiUnlockQueue.pop();
        }
    }
    Original::Platform_Tick(Handle);
}

EOS_HConnect EOS_CALL Platform_GetConnectInterface(EOS_HPlatform Handle) {
    auto result = Original::Platform_GetConnectInterface(Handle);
    if (result) Logger::debug("[HOOK] EOS_Platform_GetConnectInterface -> %p", result);
    return result;
}

EOS_HAuth EOS_CALL Platform_GetAuthInterface(EOS_HPlatform Handle) {
    auto result = Original::Platform_GetAuthInterface(Handle);
    if (result) Logger::debug("[HOOK] EOS_Platform_GetAuthInterface -> %p", result);
    return result;
}

EOS_HAchievements EOS_CALL Platform_GetAchievementsInterface(EOS_HPlatform Handle) {
    auto result = Original::Platform_GetAchievementsInterface(Handle);
    if (result) Logger::debug("[HOOK] EOS_Platform_GetAchievementsInterface -> %p", result);
    return result;
}

EOS_HEcom EOS_CALL Platform_GetEcomInterface(EOS_HPlatform Handle) {
    auto result = Original::Platform_GetEcomInterface(Handle);
    if (result) Logger::debug("[HOOK] EOS_Platform_GetEcomInterface -> %p", result);
    return result;
}

EOS_HUI EOS_CALL Platform_GetUIInterface(EOS_HPlatform Handle) {
    auto result = Original::Platform_GetUIInterface(Handle);
    if (result) Logger::debug("[HOOK] EOS_Platform_GetUIInterface -> %p", result);
    return result;
}

// Achievements hooks
void EOS_CALL Achievements_QueryDefinitions(EOS_HAchievements Handle, const EOS_Achievements_QueryDefinitionsOptions* Options, void* ClientData, const EOS_Achievements_OnQueryDefinitionsCompleteCallback CompletionDelegate) {
    Logger::debug("[HOOK] EOS_Achievements_QueryDefinitions called");
    Original::Achievements_QueryDefinitions(Handle, Options, ClientData, CompletionDelegate);
}

void EOS_CALL Achievements_QueryPlayerAchievements(EOS_HAchievements Handle, const EOS_Achievements_QueryPlayerAchievementsOptions* Options, void* ClientData, const EOS_Achievements_OnQueryPlayerAchievementsCompleteCallback CompletionDelegate) {
    Logger::debug("[HOOK] EOS_Achievements_QueryPlayerAchievements called");
    Original::Achievements_QueryPlayerAchievements(Handle, Options, ClientData, CompletionDelegate);
}

void EOS_CALL Achievements_UnlockAchievements(EOS_HAchievements Handle, const EOS_Achievements_UnlockAchievementsOptions* Options, void* ClientData, const EOS_Achievements_OnUnlockAchievementsCompleteCallback CompletionDelegate) {
    Logger::info("[HOOK] EOS_Achievements_UnlockAchievements called");
    if (Options) {
        Logger::info("[HOOK]   ApiVersion: %d", Options->ApiVersion);
        Logger::info("[HOOK]   UserId: %p", Options->UserId);
        Logger::info("[HOOK]   AchievementsCount: %u", Options->AchievementsCount);
        for (uint32_t i = 0; i < Options->AchievementsCount; i++) {
            Logger::info("[HOOK]     Achievement ID: %s", Options->AchievementIds[i]);
        }
    } else {
        Logger::warn("[HOOK]   Options is NULL!");
    }

    auto currentUserId = Util::getProductUserId();
    auto hAchievements = Util::getHAchievements();
    Logger::info("[HOOK]   Current Util::getProductUserId() = %p", currentUserId);
    Logger::info("[HOOK]   Current Util::getHAchievements() = %p", hAchievements);
    Logger::info("[HOOK]   Handle passed to hook = %p", Handle);

    if (Options && Options->UserId != currentUserId) {
        Logger::warn("[HOOK]   UserId mismatch! Hook Options->UserId (%p) != current Util::getProductUserId() (%p)", Options->UserId, currentUserId);
    }

    // If achievements not yet configured AND the config option is enabled, force configuration and postpone unlock
    if (!g_bAchievementsConfigured && Options && Options->UserId && Config::ForceAchievementsConfig()) {
        Logger::warn("[HOOK] Achievements not configured yet – forcing configuration and postponing unlock");
        ForceAchievementsConfiguration(Handle, Options->UserId);
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        g_pendingUnlocks.push_back({Handle, *Options, ClientData, CompletionDelegate});
        return;
    }

    // Otherwise proceed normally
    Original::Achievements_UnlockAchievements(Handle, Options, ClientData, CompletionDelegate);
}

EOS_NotificationId EOS_CALL Achievements_AddNotifyAchievementsUnlockedV2(EOS_HAchievements Handle, const EOS_Achievements_AddNotifyAchievementsUnlockedV2Options* Options, void* ClientData, const EOS_Achievements_OnAchievementsUnlockedCallbackV2 NotificationFn) {
    Logger::debug("[HOOK] EOS_Achievements_AddNotifyAchievementsUnlockedV2 called");
    return Original::Achievements_AddNotifyAchievementsUnlockedV2(Handle, Options, ClientData, NotificationFn);
}

EOS_NotificationId EOS_CALL Achievements_AddNotifyAchievementsUnlocked(EOS_HAchievements Handle, const EOS_Achievements_AddNotifyAchievementsUnlockedOptions* Options, void* ClientData, const EOS_Achievements_OnAchievementsUnlockedCallback NotificationFn) {
    Logger::debug("[HOOK] EOS_Achievements_AddNotifyAchievementsUnlocked (deprecated) called");
    return Original::Achievements_AddNotifyAchievementsUnlocked(Handle, Options, ClientData, NotificationFn);
}

// ============================================================================
// Ecom hooks — actual DLC logic (proxy mode equivalent, using Original:: trampolines)
// ============================================================================

// Pre-built ownership list for the ForceSuccess fallback path.
static std::vector<EOS_Ecom_ItemOwnership> g_ownerships;

// Entitlement state — rebuilt on every QueryEntitlements call.
static std::map<std::string, std::string> g_entitlement_map;
static std::vector<std::string>           g_entitlement_ids;

// DLC catalog cache — fetched once per session.
static std::map<std::string, std::string> g_catalog_cache;
static bool                               g_catalog_fetched = false;

static void AutoFetchEntitlements() {
    std::string ns = Util::g_namespace_id;
    if (ns.empty()) {
        ns = Config::NamespaceId();
        if (!ns.empty())
            Logger::debug("[HOOK] DLC auto-fetch: using NamespaceId from config: %s", ns.c_str());
    }
    if (ns.empty()) {
        Logger::warn("[HOOK] DLC auto-fetch: namespace_id unavailable. "
            "Set NamespaceId= in [ScreamAPI] if needed.");
        return;
    }
    if (!g_catalog_fetched) {
        g_catalog_fetched = true;
        auto result = DlcCatalog::fetch(ns);
        if (result.has_value()) {
            g_catalog_cache = std::move(*result);
            Logger::dlc("[HOOK] Auto-fetch: cached %zu entries", g_catalog_cache.size());
        } else {
            Logger::warn("[HOOK] Auto-fetch: failed to retrieve catalog from Epic's API");
        }
    }
    for (auto& [id, title] : g_catalog_cache) {
        if (Config::IsDlcUnlocked(id, false)) {
            Logger::debug("[HOOK]   Auto-fetch adding: %s", id.c_str());
            g_entitlement_map[id] = title;
        }
    }
}

static void InjectExtraEntitlements() {
    for (auto& [id, title] : Config::ExtraEntitlements()) {
        if (Config::IsDlcUnlocked(id, true)) {
            Logger::debug("[HOOK]   Config adding: %s", id.c_str());
            g_entitlement_map[id] = title;
        }
    }
}

static EOS_Ecom_Entitlement* MakeEntitlement(const std::string& id, const std::string& title) {
    auto* e = new EOS_Ecom_Entitlement{};
    e->ApiVersion      = EOS_ECOM_ENTITLEMENT_API_LATEST;
    e->EntitlementId   = id.c_str();
    e->CatalogItemId   = id.c_str();
    e->EntitlementName = title.c_str();
    e->bRedeemed       = false;
    e->EndTimestamp    = -1;
    e->ServerIndex     = -1;
    return e;
}

void EOS_CALL Ecom_QueryOwnership(EOS_HEcom Handle, const EOS_Ecom_QueryOwnershipOptions* Options, void* ClientData, const EOS_Ecom_OnQueryOwnershipCallback CompletionDelegate) {
    Logger::info("[HOOK] EOS_Ecom_QueryOwnership called");

    g_ownerships.clear();
    if (Options) {
        Logger::dlc("[HOOK] Game queried ownership of %d item(s):", Options->CatalogItemIdCount);
        for (uint32_t i = 0; i < Options->CatalogItemIdCount; i++) {
            const char* id = Options->CatalogItemIds[i];
            Logger::dlc("[HOOK]   Item ID: %s", id);
            bool unlocked = Config::IsDlcUnlocked(std::string(id), true);
            g_ownerships.emplace_back(EOS_Ecom_ItemOwnership{
                EOS_ECOM_ITEMOWNERSHIP_API_LATEST,
                Util::copy_c_string(id),
                unlocked ? EOS_EOwnershipStatus::EOS_OS_Owned : EOS_EOwnershipStatus::EOS_OS_NotOwned
            });
        }
    } else {
        Logger::warn("[HOOK] Game queried DLC ownership without Options parameter");
    }

    if (Config::EnableOwnershipUnlocker()) {
        struct Container {
            void* ClientData;
            EOS_Ecom_OnQueryOwnershipCallback CompletionDelegate;
        };
        auto* container = new Container{ClientData, CompletionDelegate};
        Original::Ecom_QueryOwnership(Handle, Options, container,
            [](const EOS_Ecom_QueryOwnershipCallbackInfo* Data) {
                auto* c = static_cast<Container*>(Data->ClientData);
                auto* mData = const_cast<EOS_Ecom_QueryOwnershipCallbackInfo*>(Data);

                if (mData->ResultCode != EOS_EResult::EOS_Success) {
                    Logger::warn("[HOOK] EOS_Ecom_QueryOwnership failed: %s",
                        EOS_EResult_ToString(mData->ResultCode));
                    if (Config::ForceSuccess()) {
                        Logger::warn("[HOOK] Forcing EOS_Success");
                        mData->ItemOwnershipCount = (uint32_t)g_ownerships.size();
                        mData->ItemOwnership      = g_ownerships.data();
                        mData->ResultCode         = EOS_EResult::EOS_Success;
                    }
                }

                Logger::dlc("[HOOK] Responding with %d ownership(s):", mData->ItemOwnershipCount);
                for (uint32_t i = 0; i < mData->ItemOwnershipCount; i++) {
                    auto* item = const_cast<EOS_Ecom_ItemOwnership*>(mData->ItemOwnership + i);
                    bool original = (item->OwnershipStatus == EOS_EOwnershipStatus::EOS_OS_Owned);
                    bool unlocked = Config::IsDlcUnlocked(std::string(item->Id), original);
                    item->OwnershipStatus = unlocked
                        ? EOS_EOwnershipStatus::EOS_OS_Owned
                        : EOS_EOwnershipStatus::EOS_OS_NotOwned;
                    Logger::dlc("[HOOK]   [%s] %s", unlocked ? "Owned" : "Not Owned", item->Id);
                }

                mData->ClientData = c->ClientData;
                c->CompletionDelegate(Data);
                delete c;
            }
        );
    } else {
        Original::Ecom_QueryOwnership(Handle, Options, ClientData, CompletionDelegate);
    }
}

void EOS_CALL Ecom_QueryEntitlements(EOS_HEcom Handle, const EOS_Ecom_QueryEntitlementsOptions* Options, void* ClientData, const EOS_Ecom_OnQueryEntitlementsCallback CompletionDelegate) {
    Logger::info("[HOOK] EOS_Ecom_QueryEntitlements called");

    if (!Config::EnableEntitlementUnlocker()) {
        Original::Ecom_QueryEntitlements(Handle, Options, ClientData, CompletionDelegate);
        return;
    }

    g_entitlement_map.clear();
    g_entitlement_ids.clear();

    Logger::dlc("[HOOK] Game queried %d entitlement(s):", Options->EntitlementNameCount);
    for (uint32_t i = 0; i < Options->EntitlementNameCount; i++) {
        const char* id = Options->EntitlementNames[i];
        Logger::dlc("[HOOK]   %s", id);
        if (Config::IsDlcUnlocked(std::string(id), true))
            g_entitlement_map[id] = "Unknown Title";
    }

    struct Container {
        void* ClientData;
        EOS_Ecom_OnQueryEntitlementsCallback CompletionDelegate;
    };
    auto* container = new Container{ClientData, CompletionDelegate};
    Original::Ecom_QueryEntitlements(Handle, Options, container,
        [](const EOS_Ecom_QueryEntitlementsCallbackInfo* Data) {
            auto* c = static_cast<Container*>(Data->ClientData);
            auto* mData = const_cast<EOS_Ecom_QueryEntitlementsCallbackInfo*>(Data);
            try {
                AutoFetchEntitlements();
                InjectExtraEntitlements();

                g_entitlement_ids.clear();
                for (auto& [id, title] : g_entitlement_map)
                    g_entitlement_ids.push_back(id);

                Logger::dlc("[HOOK] Responding with %zu entitlement(s):", g_entitlement_map.size());
                for (auto& [id, title] : g_entitlement_map)
                    Logger::dlc("[HOOK]   %s = \"%s\"", id.c_str(), title.c_str());

                mData->ResultCode = EOS_EResult::EOS_Success;
            } catch (const std::exception& e) {
                Logger::error("[HOOK] QueryEntitlements callback error: %s", e.what());
            }
            mData->ClientData = c->ClientData;
            c->CompletionDelegate(Data);
            delete c;
        }
    );
}

uint32_t EOS_CALL Ecom_GetEntitlementsCount(EOS_HEcom Handle, const EOS_Ecom_GetEntitlementsCountOptions* Options) {
    Logger::debug("[HOOK] EOS_Ecom_GetEntitlementsCount called");
    if (!Config::EnableEntitlementUnlocker()) {
        return Original::Ecom_GetEntitlementsCount(Handle, Options);
    }
    const auto count = (uint32_t)g_entitlement_map.size();
    Logger::debug("[HOOK] GetEntitlementsCount: %u", count);
    return count;
}

EOS_EResult EOS_CALL Ecom_CopyEntitlementByIndex(EOS_HEcom Handle, const EOS_Ecom_CopyEntitlementByIndexOptions* Options, EOS_Ecom_Entitlement** OutEntitlement) {
    Logger::debug("[HOOK] EOS_Ecom_CopyEntitlementByIndex called");
    if (!Config::EnableEntitlementUnlocker()) {
        return Original::Ecom_CopyEntitlementByIndex(Handle, Options, OutEntitlement);
    }
    const auto index = Options->EntitlementIndex;
    if (index >= g_entitlement_ids.size()) {
        Logger::warn("[HOOK] CopyEntitlementByIndex: index %u out of bounds (%zu)", index, g_entitlement_ids.size());
        return EOS_EResult::EOS_NotFound;
    }
    const auto& id    = g_entitlement_ids[index];
    const auto& title = g_entitlement_map.at(id);
    Logger::dlc("[HOOK] CopyEntitlementByIndex[%u]: %s", index, id.c_str());
    *OutEntitlement = MakeEntitlement(id, title);
    return EOS_EResult::EOS_Success;
}

void EOS_CALL Ecom_Entitlement_Release(EOS_Ecom_Entitlement* Entitlement) {
    if (Entitlement) {
        Logger::debug("[HOOK] EOS_Ecom_Entitlement_Release: %s", Entitlement->EntitlementId);
        delete Entitlement;
    } else {
        Logger::warn("[HOOK] EOS_Ecom_Entitlement_Release: null entitlement");
    }
}

void EOS_CALL Ecom_QueryOwnershipBySandboxIds(EOS_HEcom Handle, const EOS_Ecom_QueryOwnershipBySandboxIdsOptions* Options, void* ClientData, const EOS_Ecom_OnQueryOwnershipBySandboxIdsCallback CompletionDelegate) {
    Logger::debug("[HOOK] EOS_Ecom_QueryOwnershipBySandboxIds called");
    Original::Ecom_QueryOwnershipBySandboxIds(Handle, Options, ClientData, CompletionDelegate);
}

void EOS_CALL Ecom_QueryOwnershipToken(EOS_HEcom Handle, const EOS_Ecom_QueryOwnershipTokenOptions* Options, void* ClientData, const EOS_Ecom_OnQueryOwnershipTokenCallback CompletionDelegate) {
    Logger::debug("[HOOK] EOS_Ecom_QueryOwnershipToken called");
    Original::Ecom_QueryOwnershipToken(Handle, Options, ClientData, CompletionDelegate);
}

// Connect hooks
void EOS_CALL Connect_Login(EOS_HConnect Handle, const EOS_Connect_LoginOptions* Options, void* ClientData, const EOS_Connect_OnLoginCallback CompletionDelegate) {
    Logger::info("[HOOK] EOS_Connect_Login called");
    Original::Connect_Login(Handle, Options, ClientData, CompletionDelegate);
}

EOS_ProductUserId EOS_CALL Connect_GetLoggedInUserByIndex(EOS_HConnect Handle, int32_t Index) {
    auto result = Original::Connect_GetLoggedInUserByIndex(Handle, Index);
    if (result) Logger::debug("[HOOK] EOS_Connect_GetLoggedInUserByIndex[%d] -> %p", Index, result);
    return result;
}

void EOS_CALL Connect_AddNotifyLoginStatusChanged(EOS_HConnect Handle, const EOS_Connect_AddNotifyLoginStatusChangedOptions* Options, void* ClientData, const EOS_Connect_OnLoginStatusChangedCallback NotificationFn) {
    Logger::debug("[HOOK] EOS_Connect_AddNotifyLoginStatusChanged called");
    Original::Connect_AddNotifyLoginStatusChanged(Handle, Options, ClientData, NotificationFn);
}

// Auth hooks
void EOS_CALL Auth_Login(EOS_HAuth Handle, const EOS_Auth_LoginOptions* Options, void* ClientData, const EOS_Auth_OnLoginCallback CompletionDelegate) {
    Logger::info("[HOOK] EOS_Auth_Login called");
    Original::Auth_Login(Handle, Options, ClientData, CompletionDelegate);
}

EOS_EpicAccountId EOS_CALL Auth_GetLoggedInAccountByIndex(EOS_HAuth Handle, int32_t Index) {
    auto result = Original::Auth_GetLoggedInAccountByIndex(Handle, Index);
    if (result) Logger::debug("[HOOK] EOS_Auth_GetLoggedInAccountByIndex[%d] -> %p", Index, result);
    return result;
}

void EOS_CALL Auth_AddNotifyLoginStatusChanged(EOS_HAuth Handle, const EOS_Auth_AddNotifyLoginStatusChangedOptions* Options, void* ClientData, const EOS_Auth_OnLoginStatusChangedCallback NotificationFn) {
    Logger::debug("[HOOK] EOS_Auth_AddNotifyLoginStatusChanged called");
    Original::Auth_AddNotifyLoginStatusChanged(Handle, Options, ClientData, NotificationFn);
}

} // namespace Hooks

// ============================================================================
// HOOK INITIALIZATION
// ============================================================================

bool InitializeHooks(HMODULE originalDLL) {
    Logger::debug("[HOOK] Entering InitializeHooks");
    if (hooksInitialized) {
        Logger::warn("[HOOK] Hooks already initialized");
        return true;
    }
    if (!originalDLL) {
        Logger::error("[HOOK] Invalid DLL handle");
        return false;
    }
    originalEOSDLL = originalDLL;

    Logger::info("[HOOK] ========================================");
    Logger::info("[HOOK] Initializing MinHook-based EOS hooking");
    Logger::info("[HOOK] ========================================");

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        Logger::error("[HOOK] MH_Initialize failed: %d", status);
        return false;
    }
    Logger::info("[HOOK] MinHook initialized successfully");

    Logger::info("[HOOK] Installing Platform hooks...");
    INSTALL_HOOK(originalDLL, EOS_Platform_Create, Hooks::Platform_Create, Original::Platform_Create);
    INSTALL_HOOK(originalDLL, EOS_Platform_Release, Hooks::Platform_Release, Original::Platform_Release);
    INSTALL_HOOK(originalDLL, EOS_Platform_Tick, Hooks::Platform_Tick, Original::Platform_Tick);
    INSTALL_HOOK(originalDLL, EOS_Platform_GetConnectInterface, Hooks::Platform_GetConnectInterface, Original::Platform_GetConnectInterface);
    INSTALL_HOOK(originalDLL, EOS_Platform_GetAuthInterface, Hooks::Platform_GetAuthInterface, Original::Platform_GetAuthInterface);
    INSTALL_HOOK(originalDLL, EOS_Platform_GetAchievementsInterface, Hooks::Platform_GetAchievementsInterface, Original::Platform_GetAchievementsInterface);
    INSTALL_HOOK(originalDLL, EOS_Platform_GetEcomInterface, Hooks::Platform_GetEcomInterface, Original::Platform_GetEcomInterface);
    INSTALL_HOOK(originalDLL, EOS_Platform_GetUIInterface, Hooks::Platform_GetUIInterface, Original::Platform_GetUIInterface);

    Logger::info("[HOOK] Installing Achievement hooks...");
    INSTALL_HOOK(originalDLL, EOS_Achievements_QueryDefinitions, Hooks::Achievements_QueryDefinitions, Original::Achievements_QueryDefinitions);
    INSTALL_HOOK(originalDLL, EOS_Achievements_QueryPlayerAchievements, Hooks::Achievements_QueryPlayerAchievements, Original::Achievements_QueryPlayerAchievements);
    INSTALL_HOOK(originalDLL, EOS_Achievements_UnlockAchievements, Hooks::Achievements_UnlockAchievements, Original::Achievements_UnlockAchievements);
    INSTALL_HOOK(originalDLL, EOS_Achievements_AddNotifyAchievementsUnlockedV2, Hooks::Achievements_AddNotifyAchievementsUnlockedV2, Original::Achievements_AddNotifyAchievementsUnlockedV2);
    INSTALL_HOOK(originalDLL, EOS_Achievements_AddNotifyAchievementsUnlocked, Hooks::Achievements_AddNotifyAchievementsUnlocked, Original::Achievements_AddNotifyAchievementsUnlocked);

    Logger::info("[HOOK] Installing Ecom hooks...");
    INSTALL_HOOK(originalDLL, EOS_Ecom_QueryOwnership, Hooks::Ecom_QueryOwnership, Original::Ecom_QueryOwnership);
    INSTALL_HOOK_OPTIONAL(originalDLL, EOS_Ecom_QueryOwnershipBySandboxIds, Hooks::Ecom_QueryOwnershipBySandboxIds, Original::Ecom_QueryOwnershipBySandboxIds);
    INSTALL_HOOK_OPTIONAL(originalDLL, EOS_Ecom_QueryOwnershipToken, Hooks::Ecom_QueryOwnershipToken, Original::Ecom_QueryOwnershipToken);
    INSTALL_HOOK(originalDLL, EOS_Ecom_QueryEntitlements, Hooks::Ecom_QueryEntitlements, Original::Ecom_QueryEntitlements);
    INSTALL_HOOK(originalDLL, EOS_Ecom_GetEntitlementsCount, Hooks::Ecom_GetEntitlementsCount, Original::Ecom_GetEntitlementsCount);
    INSTALL_HOOK(originalDLL, EOS_Ecom_CopyEntitlementByIndex, Hooks::Ecom_CopyEntitlementByIndex, Original::Ecom_CopyEntitlementByIndex);
    INSTALL_HOOK(originalDLL, EOS_Ecom_Entitlement_Release, Hooks::Ecom_Entitlement_Release, Original::Ecom_Entitlement_Release);

    Logger::info("[HOOK] Installing Connect hooks...");
    INSTALL_HOOK(originalDLL, EOS_Connect_Login, Hooks::Connect_Login, Original::Connect_Login);
    INSTALL_HOOK(originalDLL, EOS_Connect_GetLoggedInUserByIndex, Hooks::Connect_GetLoggedInUserByIndex, Original::Connect_GetLoggedInUserByIndex);
    INSTALL_HOOK_OPTIONAL(originalDLL, EOS_Connect_AddNotifyLoginStatusChanged, Hooks::Connect_AddNotifyLoginStatusChanged, Original::Connect_AddNotifyLoginStatusChanged);

    Logger::info("[HOOK] Installing Auth hooks...");
    INSTALL_HOOK(originalDLL, EOS_Auth_Login, Hooks::Auth_Login, Original::Auth_Login);
    INSTALL_HOOK(originalDLL, EOS_Auth_GetLoggedInAccountByIndex, Hooks::Auth_GetLoggedInAccountByIndex, Original::Auth_GetLoggedInAccountByIndex);
    INSTALL_HOOK_OPTIONAL(originalDLL, EOS_Auth_AddNotifyLoginStatusChanged, Hooks::Auth_AddNotifyLoginStatusChanged, Original::Auth_AddNotifyLoginStatusChanged);

    hooksInitialized = true;

    Logger::info("[HOOK] ========================================");
    Logger::info("[HOOK] All hooks installed successfully!");
    Logger::info("[HOOK] ========================================");

    return true;
}

void ShutdownHooks() {
    if (!hooksInitialized) return;
    Logger::info("[HOOK] Shutting down hooks...");
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    hooksInitialized = false;
    Logger::info("[HOOK] Hooks shutdown complete");
}

bool AreHooksActive() {
    return hooksInitialized;
}

void QueueUnlock(const char* id) {
    std::lock_guard<std::mutex> lk(g_guiQueueMutex);
    g_guiUnlockQueue.push(id);
    Logger::info("[HOOK] QueueUnlock: queued %s", id);
}

} // namespace EOS_Hooks