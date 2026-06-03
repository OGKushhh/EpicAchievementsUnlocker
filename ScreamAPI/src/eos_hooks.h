#pragma once

// Include EOS SDK headers
#include "eos-sdk/eos_sdk.h"
#include "eos-sdk/eos_achievements.h"
#include "eos-sdk/eos_auth.h"
#include "eos-sdk/eos_connect.h"
#include "eos-sdk/eos_ecom.h"
#include "MinHook.h"

namespace EOS_Hooks {

// Original function pointers (trampolines to call original functions)
namespace Original {
    // Platform
    extern decltype(&EOS_Platform_Create) Platform_Create;
    extern decltype(&EOS_Platform_Release) Platform_Release;
    extern decltype(&EOS_Platform_Tick) Platform_Tick;
    extern decltype(&EOS_Platform_GetConnectInterface) Platform_GetConnectInterface;
    extern decltype(&EOS_Platform_GetAuthInterface) Platform_GetAuthInterface;
    extern decltype(&EOS_Platform_GetAchievementsInterface) Platform_GetAchievementsInterface;
    extern decltype(&EOS_Platform_GetEcomInterface) Platform_GetEcomInterface;
    // Optional platform function
    extern decltype(&EOS_Platform_GetUIInterface) Platform_GetUIInterface;
    
    // Achievements
    extern decltype(&EOS_Achievements_QueryDefinitions) Achievements_QueryDefinitions;
    extern decltype(&EOS_Achievements_QueryPlayerAchievements) Achievements_QueryPlayerAchievements;
    extern decltype(&EOS_Achievements_UnlockAchievements) Achievements_UnlockAchievements;
    extern decltype(&EOS_Achievements_AddNotifyAchievementsUnlockedV2) Achievements_AddNotifyAchievementsUnlockedV2;
    // Optional deprecated achievement notification
    extern decltype(&EOS_Achievements_AddNotifyAchievementsUnlocked) Achievements_AddNotifyAchievementsUnlocked;
    
    // Ecom
    extern decltype(&EOS_Ecom_QueryOwnership) Ecom_QueryOwnership;
    extern decltype(&EOS_Ecom_QueryEntitlements) Ecom_QueryEntitlements;
    extern decltype(&EOS_Ecom_GetEntitlementsCount) Ecom_GetEntitlementsCount;
    extern decltype(&EOS_Ecom_CopyEntitlementByIndex) Ecom_CopyEntitlementByIndex;
    
    // Connect
    extern decltype(&EOS_Connect_Login) Connect_Login;
    extern decltype(&EOS_Connect_GetLoggedInUserByIndex) Connect_GetLoggedInUserByIndex;
    // Optional connect login status notification
    extern decltype(&EOS_Connect_AddNotifyLoginStatusChanged) Connect_AddNotifyLoginStatusChanged;
    
    // Auth
    extern decltype(&EOS_Auth_Login) Auth_Login;
    extern decltype(&EOS_Auth_GetLoggedInAccountByIndex) Auth_GetLoggedInAccountByIndex;
    // Optional auth login status notification
    extern decltype(&EOS_Auth_AddNotifyLoginStatusChanged) Auth_AddNotifyLoginStatusChanged;
}

// Hook function declarations (our implementations)
namespace Hooks {
    // Platform
    EOS_HPlatform EOS_CALL Platform_Create(const EOS_Platform_Options* Options);
    void EOS_CALL Platform_Release(EOS_HPlatform Handle);
    void EOS_CALL Platform_Tick(EOS_HPlatform Handle);
    EOS_HConnect EOS_CALL Platform_GetConnectInterface(EOS_HPlatform Handle);
    EOS_HAuth EOS_CALL Platform_GetAuthInterface(EOS_HPlatform Handle);
    EOS_HAchievements EOS_CALL Platform_GetAchievementsInterface(EOS_HPlatform Handle);
    EOS_HEcom EOS_CALL Platform_GetEcomInterface(EOS_HPlatform Handle);
    // Optional platform function
    EOS_HUI EOS_CALL Platform_GetUIInterface(EOS_HPlatform Handle);
    
    // Achievements
    void EOS_CALL Achievements_QueryDefinitions(EOS_HAchievements Handle, const EOS_Achievements_QueryDefinitionsOptions* Options, void* ClientData, const EOS_Achievements_OnQueryDefinitionsCompleteCallback CompletionDelegate);
    void EOS_CALL Achievements_QueryPlayerAchievements(EOS_HAchievements Handle, const EOS_Achievements_QueryPlayerAchievementsOptions* Options, void* ClientData, const EOS_Achievements_OnQueryPlayerAchievementsCompleteCallback CompletionDelegate);
    void EOS_CALL Achievements_UnlockAchievements(EOS_HAchievements Handle, const EOS_Achievements_UnlockAchievementsOptions* Options, void* ClientData, const EOS_Achievements_OnUnlockAchievementsCompleteCallback CompletionDelegate);
    EOS_NotificationId EOS_CALL Achievements_AddNotifyAchievementsUnlockedV2(EOS_HAchievements Handle, const EOS_Achievements_AddNotifyAchievementsUnlockedV2Options* Options, void* ClientData, const EOS_Achievements_OnAchievementsUnlockedCallbackV2 NotificationFn);
    // Optional deprecated achievement notification
    EOS_NotificationId EOS_CALL Achievements_AddNotifyAchievementsUnlocked(EOS_HAchievements Handle, const EOS_Achievements_AddNotifyAchievementsUnlockedOptions* Options, void* ClientData, const EOS_Achievements_OnAchievementsUnlockedCallback NotificationFn);
    
    // Ecom
    void EOS_CALL Ecom_QueryOwnership(EOS_HEcom Handle, const EOS_Ecom_QueryOwnershipOptions* Options, void* ClientData, const EOS_Ecom_OnQueryOwnershipCallback CompletionDelegate);
    void EOS_CALL Ecom_QueryEntitlements(EOS_HEcom Handle, const EOS_Ecom_QueryEntitlementsOptions* Options, void* ClientData, const EOS_Ecom_OnQueryEntitlementsCallback CompletionDelegate);
    uint32_t EOS_CALL Ecom_GetEntitlementsCount(EOS_HEcom Handle, const EOS_Ecom_GetEntitlementsCountOptions* Options);
    EOS_EResult EOS_CALL Ecom_CopyEntitlementByIndex(EOS_HEcom Handle, const EOS_Ecom_CopyEntitlementByIndexOptions* Options, EOS_Ecom_Entitlement** OutEntitlement);
    
    // Connect
    void EOS_CALL Connect_Login(EOS_HConnect Handle, const EOS_Connect_LoginOptions* Options, void* ClientData, const EOS_Connect_OnLoginCallback CompletionDelegate);
    EOS_ProductUserId EOS_CALL Connect_GetLoggedInUserByIndex(EOS_HConnect Handle, int32_t Index);
    // Optional connect login status notification
    void EOS_CALL Connect_AddNotifyLoginStatusChanged(EOS_HConnect Handle, const EOS_Connect_AddNotifyLoginStatusChangedOptions* Options, void* ClientData, const EOS_Connect_OnLoginStatusChangedCallback NotificationFn);
    
    // Auth
    void EOS_CALL Auth_Login(EOS_HAuth Handle, const EOS_Auth_LoginOptions* Options, void* ClientData, const EOS_Auth_OnLoginCallback CompletionDelegate);
    EOS_EpicAccountId EOS_CALL Auth_GetLoggedInAccountByIndex(EOS_HAuth Handle, int32_t Index);
    // Optional auth login status notification
    void EOS_CALL Auth_AddNotifyLoginStatusChanged(EOS_HAuth Handle, const EOS_Auth_AddNotifyLoginStatusChangedOptions* Options, void* ClientData, const EOS_Auth_OnLoginStatusChangedCallback NotificationFn);
}

// Initialization and cleanup
bool InitializeHooks(HMODULE originalDLL);
void ShutdownHooks();
bool AreHooksActive();

// GUI thread-safe unlock queue — call from any thread, drained on game thread
void QueueUnlock(const char* id);

} // namespace EOS_Hooks