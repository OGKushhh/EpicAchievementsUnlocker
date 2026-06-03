#include "pch.h"
#include "achievement_manager.h"
#include "ScreamAPI.h"
#include "util.h"
#include "eos-sdk/eos_achievements.h"
#include "PipeServer.h"
#include "Overlay.h"
#include <future>
#include <atomic>
#include <thread>
#include <mutex>

using namespace Util;

namespace AchievementManager {

// Forward declaration
void queryAchievementDefinitions();
void queryAchievementDefinitionsWithRetry(int delayMs = 0);

Achievements achievements;
static std::mutex g_achMutex;  // guards achievements vector

std::mutex& GetAchievementsMutex() { return g_achMutex; }

// Track if we've already retried due to missing user
static std::atomic<bool> waitingForUser{false};
static std::atomic<bool> definitionsQueried{false};
static std::atomic<bool> playerAchievementsQueried{false};

// ----------------------------------------------------------------------------
// Helper: Log achievement statistics to the log file
// ----------------------------------------------------------------------------
static void LogAchievementStats() {
    if (achievements.empty()) {
        Logger::ach("No achievements loaded yet.");
        return;
    }

    int total = (int)achievements.size();
    int unlocked = 0;
    for (const auto& ach : achievements) {
        if (ach.UnlockState == UnlockState::Unlocked) unlocked++;
    }
    float percent = (total > 0) ? (unlocked * 100.0f / total) : 0.0f;

    Logger::ach("========================================");
    Logger::ach("Achievement Statistics");
    Logger::ach("  Total achievements: %d", total);
    Logger::ach("  Unlocked: %d", unlocked);
    Logger::ach("  Locked: %d", total - unlocked);
    Logger::ach("  Progress: %.1f%%", percent);
    Logger::ach("========================================");
}

void printAchievementDefinition(EOS_Achievements_DefinitionV2* definition) {
    if (definition == nullptr) {
        Logger::ach("Invalid Achievement Definition");
    } else {
        std::stringstream ss;
        ss
            << "[Achievement Definition]\n"
            << "\t\t\t""AchievementId: " << definition->AchievementId << "\n"
            << "\t\t\t""IsHidden: " << definition->bIsHidden << "\n"
            << "\t\t\t""FlavorText: " << definition->FlavorText << "\n"
            << "\t\t\t""LockedDescription: " << definition->LockedDescription << "\n"
            << "\t\t\t""LockedDisplayName: " << definition->LockedDisplayName << "\n"
            << "\t\t\t""LockedIconURL: " << definition->LockedIconURL << "\n"
            << "\t\t\t""UnlockedDescription: " << definition->UnlockedDescription << "\n"
            << "\t\t\t""UnlockedDisplayName: " << definition->UnlockedDisplayName << "\n"
            << "\t\t\t""UnlockedIconURL: " << definition->UnlockedIconURL << "\n"
            << "\t\t\t""StatThresholdsCount: " << definition->StatThresholdsCount << "\n";

        for (unsigned int i = 0; i < definition->StatThresholdsCount; i++) {
            ss
                << "\t\t\t\t""[StatThreshold] "
                << "Name: " << definition->StatThresholds[i].Name << "; "
                << "Threshold: " << definition->StatThresholds[i].Threshold;
        }
        Logger::ach("%s", ss.str().c_str());
    }
}

void printPlayerAchievement(EOS_Achievements_PlayerAchievement* achievement) {
    if (achievement == nullptr) {
        Logger::ach("Invalid Player Achievement");
    } else {
        std::stringstream ss;
        ss
            << "[Player Achievement]\n"
            << "\t\t\t""AchievementId: " << achievement->AchievementId << "\n"
            << "\t\t\t""Description: " << achievement->Description << "\n"
            << "\t\t\t""DisplayName: " << achievement->DisplayName << "\n"
            << "\t\t\t""FlavorText: " << achievement->FlavorText << "\n"
            << "\t\t\t""IconURL: " << achievement->IconURL << "\n"
            << "\t\t\t""Progress: " << achievement->Progress << "\n"
            << "\t\t\t""StatInfoCount: " << achievement->StatInfoCount << "\n";

        for (int i = 0; i < achievement->StatInfoCount; i++) {
            ss
                << "\t\t\t\t""[StatInfo] "
                << "Name: " << achievement->StatInfo[i].Name << "; "
                << "CurrentValue: " << achievement->StatInfo[i].CurrentValue << "; "
                << "ThresholdValue: " << achievement->StatInfo[i].ThresholdValue;
        }
        Logger::ach("%s", ss.str().c_str());
    }
}

void findAchievement(const char* achievementID, std::function<void(Overlay_Achievement&)> callback) {
    for (auto& achievement : achievements) {
        if (!strcmp(achievement.AchievementId, achievementID)) {
            callback(achievement);
            return;
        }
    }
    Logger::error("Could not find achievement with id: %s", achievementID);
}

// ----------------------------------------------------------------------------
// Static callbacks for achievement operations
// ----------------------------------------------------------------------------

static void EOS_CALL OnUnlockAchievementsComplete(const EOS_Achievements_OnUnlockAchievementsCompleteCallbackInfo* Data) {
    auto achievement = (Overlay_Achievement*)Data->ClientData;
    if (Data->ResultCode == EOS_EResult::EOS_Success) {
        Logger::info("Successfully unlocked the achievement: %s", achievement->AchievementId);
    } else {
        achievement->UnlockState = UnlockState::Locked;
        Logger::error("Failed to unlock the achievement: %s. Error string: %s",
            achievement->AchievementId,
            EOS_EResult_ToString(Data->ResultCode));
    }
}

static void EOS_CALL OnAchievementsUnlockedV2(const EOS_Achievements_OnAchievementsUnlockedCallbackV2Info* Data) {
    findAchievement(Data->AchievementId, [](Overlay_Achievement& achievement) {
        achievement.UnlockState = UnlockState::Unlocked;
    });
    PipeServer::NotifyUnlock(Data->AchievementId);
}

static void EOS_CALL OnAchievementsUnlocked(const EOS_Achievements_OnAchievementsUnlockedCallbackInfo* Data) {
    for (uint32_t i = 0; i < Data->AchievementsCount; i++) {
        findAchievement(Data->AchievementIds[i], [](Overlay_Achievement& achievement) {
            achievement.UnlockState = UnlockState::Unlocked;
        });
        PipeServer::NotifyUnlock(Data->AchievementIds[i]);
    }
}

void unlockAchievement(Overlay_Achievement* achievement) {
    achievement->UnlockState = UnlockState::Unlocking;

    EOS_Achievements_UnlockAchievementsOptions Options = {
        EOS_ACHIEVEMENTS_UNLOCKACHIEVEMENTS_API_LATEST,
        getProductUserId(),
        &achievement->AchievementId,
        1
    };

    EOS_Achievements_UnlockAchievements(getHAchievements(), &Options, achievement, OnUnlockAchievementsComplete);
}

// ----------------------------------------------------------------------------
// Player achievements query callback (with retry on authentication errors)
// ----------------------------------------------------------------------------

void EOS_CALL queryPlayerAchievementsComplete(const EOS_Achievements_OnQueryPlayerAchievementsCompleteCallbackInfo* Data) {
    Logger::debug("queryPlayerAchievementsComplete");

    if (Data->ResultCode != EOS_EResult::EOS_Success) {
        Logger::error("Failed to query player achievements. Result string: %s",
            EOS_EResult_ToString(Data->ResultCode));

        // If authentication failed, try again later
        if (Data->ResultCode == EOS_EResult::EOS_InvalidUser || Data->ResultCode == EOS_EResult::EOS_InvalidAuth) {
            Logger::debug("User not authenticated – will retry in 5 seconds");
            std::thread([]() {
                Sleep(5000);
                EOS_Achievements_QueryPlayerAchievementsOptions QueryOptions = {
                    1,                             // ApiVersion (legacy)
                    getProductUserId()             // UserId
                };
                EOS_Achievements_QueryPlayerAchievements(
                    getHAchievements(),
                    &QueryOptions,
                    nullptr,
                    queryPlayerAchievementsComplete
                );
            }).detach();
        }
        return;
    }

    playerAchievementsQueried = true;
    Logger::debug("[ACH] Player achievements query succeeded");
    PipeServer::Start(); // achievements list is now complete — open pipe for GUI

    static EOS_Achievements_GetPlayerAchievementCountOptions GetCountOptions{
        EOS_ACHIEVEMENTS_GETPLAYERACHIEVEMENTCOUNT_API_LATEST,
        getProductUserId()
    };
    auto playerAchievementsCount = EOS_Achievements_GetPlayerAchievementCount(getHAchievements(), &GetCountOptions);
    for (unsigned int i = 0; i < playerAchievementsCount; i++) {
        EOS_Achievements_CopyPlayerAchievementByIndexOptions CopyAchievementOptions{
            EOS_ACHIEVEMENTS_COPYPLAYERACHIEVEMENTBYINDEX_API_LATEST,
            getProductUserId(),      // TargetUserId
            i,                       // AchievementIndex
            getProductUserId()       // LocalUserId (for modern SDK)
        };
        EOS_Achievements_PlayerAchievement* OutAchievement;
        auto result = EOS_Achievements_CopyPlayerAchievementByIndex(
            getHAchievements(),
            &CopyAchievementOptions,
            &OutAchievement
        );
        if (result != EOS_EResult::EOS_Success) {
            Logger::error("Failed to copy player achievement by index %d. "
                "Result string: %s", i, EOS_EResult_ToString(result));
            continue;
        }

        printPlayerAchievement(OutAchievement);

        if (OutAchievement->UnlockTime != -1) {
            findAchievement(OutAchievement->AchievementId, [](Overlay_Achievement& achievement) {
                achievement.UnlockState = UnlockState::Unlocked;
            });
        }

        EOS_Achievements_PlayerAchievement_Release(OutAchievement);
    }

    // Log achievement statistics after processing
    LogAchievementStats();

    // Notify GUI that the updated list is ready
    PipeServer::SendUpdatedList();
}

// ----------------------------------------------------------------------------
// Definitions query callback (with retry on authentication errors)
// ----------------------------------------------------------------------------

void EOS_CALL queryDefinitionsComplete(const EOS_Achievements_OnQueryDefinitionsCompleteCallbackInfo* Data) {
    Logger::debug("[ACH] queryDefinitionsComplete called with ResultCode: %d", Data->ResultCode);

    if (Data->ResultCode != EOS_EResult::EOS_Success) {
        Logger::error("[ACH] Failed to query achievement definitions. Result: %s",
            EOS_EResult_ToString(Data->ResultCode));

        // If authentication failed, retry later
        if (Data->ResultCode == EOS_EResult::EOS_InvalidAuth || Data->ResultCode == EOS_EResult::EOS_InvalidUser) {
            Logger::debug("[ACH] Authentication required – will retry in 5 seconds");
            std::thread([]() {
                Sleep(5000);
                queryAchievementDefinitions();
            }).detach();
        }
        return;
    }

    definitionsQueried = true;
    Logger::debug("[ACH] Achievement definitions query succeeded");

    // Clear existing achievements before repopulating (in case of retry)
    achievements.clear();

    static EOS_Achievements_GetAchievementDefinitionCountOptions GetCountOptions{
        EOS_ACHIEVEMENTS_GETACHIEVEMENTDEFINITIONCOUNT_API_LATEST
    };
    auto achievementDefinitionCount = EOS_Achievements_GetAchievementDefinitionCount(getHAchievements(), &GetCountOptions);
    for (uint32_t i = 0; i < achievementDefinitionCount; i++) {
        static bool useDeprecated = false;

        if (!useDeprecated) {
            try {
                EOS_Achievements_CopyAchievementDefinitionV2ByIndexOptions DefinitionOptions{
                    EOS_ACHIEVEMENTS_COPYACHIEVEMENTDEFINITIONV2BYINDEX_API_LATEST,
                    i
                };
                EOS_Achievements_DefinitionV2* OutDefinition;
                auto result = EOS_Achievements_CopyAchievementDefinitionV2ByIndex(getHAchievements(), &DefinitionOptions, &OutDefinition);
                if (result != EOS_EResult::EOS_Success) {
                    Logger::error("Failed to copy achievement definition by index %d. "
                        "Result string: %s", i, EOS_EResult_ToString(result));
                    continue;
                }

                printAchievementDefinition(OutDefinition);

                achievements.push_back(
                    {
                        copy_c_string(OutDefinition->AchievementId),
                        (bool)OutDefinition->bIsHidden,
                        UnlockState::Locked,
                        copy_c_string(OutDefinition->UnlockedDescription),
                        copy_c_string(OutDefinition->UnlockedDisplayName),
                        copy_c_string(OutDefinition->UnlockedIconURL),
                        nullptr
                    }
                );

                EOS_Achievements_DefinitionV2_Release(OutDefinition);
                continue;
            } catch (ScreamAPI::FunctionNotFoundException) {
                useDeprecated = true;
            }
        }

        EOS_Achievements_CopyAchievementDefinitionByIndexOptions DefinitionOptions{
            EOS_ACHIEVEMENTS_COPYDEFINITIONBYINDEX_API_LATEST,
            i
        };
        EOS_Achievements_Definition* OutDefinition;
        auto result = EOS_Achievements_CopyAchievementDefinitionByIndex(getHAchievements(), &DefinitionOptions, &OutDefinition);
        if (result != EOS_EResult::EOS_Success) {
            Logger::error("Failed to copy (deprecated) achievement definition by index %d. "
                "Result string: %s", i, EOS_EResult_ToString(result));
            continue;
        }

        achievements.push_back(
            {
                copy_c_string(OutDefinition->AchievementId),
                (bool)OutDefinition->bIsHidden,
                UnlockState::Locked,
                copy_c_string(OutDefinition->CompletionDescription),
                copy_c_string(OutDefinition->DisplayName),
                copy_c_string(OutDefinition->UnlockedIconId),
                nullptr
            }
        );

        EOS_Achievements_Definition_Release(OutDefinition);
    }

    // Query player achievements – use legacy version (1) to allow NULL user ID
    EOS_Achievements_QueryPlayerAchievementsOptions QueryAchievementsOptions = {
        1,                             // ApiVersion (legacy)
        getProductUserId()             // UserId (TargetUserId)
    };

    EOS_Achievements_QueryPlayerAchievements(
        getHAchievements(),
        &QueryAchievementsOptions,
        nullptr,
        queryPlayerAchievementsComplete
    );
}

// ----------------------------------------------------------------------------
// Query definitions (legacy API version) with retry if user not logged in
// ----------------------------------------------------------------------------

void queryAchievementDefinitions() {
    auto productUserId = getProductUserId();
    auto epicAccountId = getEpicAccountId();
    auto hAchievements = getHAchievements();

    Logger::debug("[ACH] queryAchievementDefinitions called");
    Logger::debug("[ACH]   ProductUserId: %p", productUserId);
    Logger::debug("[ACH]   EpicAccountId: %p", epicAccountId);
    Logger::debug("[ACH]   HAchievements: %p", hAchievements);

    if (hAchievements == nullptr) {
        Logger::error("[ACH] Cannot query achievements - HAchievements is NULL");
        return;
    }

    // If no user is logged in yet, schedule a retry
    if (productUserId == nullptr && epicAccountId == nullptr) {
        if (!waitingForUser.exchange(true)) {
            Logger::warn("[ACH] Both ProductUserId and EpicAccountId are NULL - user not logged in yet");
            Logger::warn("[ACH] Will retry definitions query every 5 seconds until login");
            queryAchievementDefinitionsWithRetry(5000);
        } else {
            Logger::debug("[ACH] Already waiting for user login, skipping duplicate retry schedule");
        }
        return;
    }

    // User exists – proceed with query
    waitingForUser = false;
    definitionsQueried = false;

    // Legacy API version (1) – matches v1.10.2 behavior
    EOS_Achievements_QueryDefinitionsOptions QueryDefinitionsOptions = {
        1,                             // ApiVersion
        productUserId,                 // TargetUserId (LocalUserId in modern)
        epicAccountId,                 // EpicUserId_DEPRECATED
        nullptr,                       // HiddenAchievementIds_DEPRECATED
        0                              // HiddenAchievementsCount_DEPRECATED
    };

    Logger::debug("[ACH] Calling EOS_Achievements_QueryDefinitions");
    EOS_Achievements_QueryDefinitions(
        hAchievements,
        &QueryDefinitionsOptions,
        nullptr,
        queryDefinitionsComplete
    );
}

// Retry helper: sleeps and then calls queryAchievementDefinitions again
void queryAchievementDefinitionsWithRetry(int delayMs) {
    std::thread([delayMs]() {
        Sleep(delayMs);
        // Check again if user is now logged in
        auto productUserId = getProductUserId();
        auto epicAccountId = getEpicAccountId();
        if (productUserId != nullptr || epicAccountId != nullptr) {
            Logger::info("[ACH] User logged in detected, retrying achievement definitions query");
            queryAchievementDefinitions();
        } else {
            // Still no user – keep retrying
            Logger::debug("[ACH] User still not logged in, will retry definitions again in 5 seconds");
            queryAchievementDefinitionsWithRetry(5000);
        }
    }).detach();
}

// ----------------------------------------------------------------------------
// Notifications
// ----------------------------------------------------------------------------

void subscribeToAchievementNotifications() {
    static bool useDeprecated = false;

    if (!useDeprecated) {
        try {
            EOS_Achievements_AddNotifyAchievementsUnlockedV2Options NotifyOptions = {
                EOS_ACHIEVEMENTS_ADDNOTIFYACHIEVEMENTSUNLOCKEDV2_API_LATEST
            };
            EOS_Achievements_AddNotifyAchievementsUnlockedV2(getHAchievements(), &NotifyOptions, nullptr, OnAchievementsUnlockedV2);
            Logger::debug("[ACH] Subscribed to V2 unlock notifications");
            return;
        } catch (ScreamAPI::FunctionNotFoundException) {
            useDeprecated = true;
        }
    }

    EOS_Achievements_AddNotifyAchievementsUnlockedOptions NotifyOptions = {
        EOS_ACHIEVEMENTS_ADDNOTIFYACHIEVEMENTSUNLOCKED_API_LATEST
    };
    EOS_Achievements_AddNotifyAchievementsUnlocked(getHAchievements(), &NotifyOptions, nullptr, OnAchievementsUnlocked);
    Logger::debug("[ACH] Subscribed to deprecated unlock notifications");
}

// ----------------------------------------------------------------------------
// Initialization
// ----------------------------------------------------------------------------

void init() {
    static bool init = false;

    if (!init && Config::EnableOverlay()) {
        init = true;
        Logger::ovrly("Achievement Manager: Initializing...");
        Logger::debug("[ACH] init(): Starting achievement manager initialization");

        queryAchievementDefinitions();
        subscribeToAchievementNotifications();
        Logger::debug("[ACH] init(): Achievement manager initialization complete");
    }
}

// Public function to manually retry (call from login hook if needed)
void retryAchievementQueries() {
    if (!definitionsQueried) {
        Logger::info("[ACH] Manual retry triggered – re-querying definitions");
        queryAchievementDefinitions();
    } else if (!playerAchievementsQueried) {
        Logger::info("[ACH] Manual retry triggered – re-querying player achievements");
        EOS_Achievements_QueryPlayerAchievementsOptions QueryOptions = {
            1,
            getProductUserId()
        };
        EOS_Achievements_QueryPlayerAchievements(
            getHAchievements(),
            &QueryOptions,
            nullptr,
            queryPlayerAchievementsComplete
        );
    } else {
        Logger::debug("[ACH] Manual retry called but both definitions and player achievements already loaded");
    }
}

void refresh() {
    Logger::info("[ACH] Manual refresh triggered");

    // Reset internal state flags so that a fresh query can run
    definitionsQueried = false;
    playerAchievementsQueried = false;
    waitingForUser = false;

    // Clear existing achievements – the query callback will repopulate
    achievements.clear();

    // Start a fresh definitions query
    queryAchievementDefinitions();
}

void initWithOverlay(void* hModule) {
    Logger::ovrly("Achievement Manager: initWithOverlay called");
    Overlay::Init((HMODULE)hModule, &achievements, unlockAchievement);
    init();
    Logger::ovrly("Achievement Manager: initWithOverlay complete");
}

} // namespace AchievementManager