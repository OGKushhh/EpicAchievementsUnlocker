# EOS SDK Version Detection & Compatibility Strategy
## Dynamic Runtime Adaptation for ScreamAPI

---

## Problem Statement

Your ScreamAPI is compiled against EOS SDK **v1.13.0** (June 2021), but games may use:
- v1.13.x (your current target)
- v1.14.x (2021-2022) - Added external auth providers
- v1.15.x (2023) - Added desktop crossplay, network timeouts
- v1.16.x (2024) - Added QueryOwnershipBySandboxIds, persistent items
- **v1.17.x+ (future)** - Unknown changes

**Current failure mode**: Hard-coded struct sizes and API versions cause:
- Stack corruption
- NULL pointer returns
- Failed function calls
- Complete initialization failure

---

## Solution: Runtime SDK Version Detection

Instead of compiling against one SDK version, **detect the game's SDK version at runtime** and adapt.

### Implementation Strategy

```cpp
// Global variable to store detected version
namespace EOS_Compat {
    int detectedMajor = 0;
    int detectedMinor = 0;
    int detectedPatch = 0;
    bool versionDetected = false;
}

/**
 * Detect the EOS SDK version used by the game's DLL
 * Returns: true if successfully detected, false otherwise
 */
bool detectGameEOSSDKVersion(HMODULE eosDLL) {
    // Method 1: Try to call EOS_GetVersion() if available
    typedef const char* (EOS_CALL *EOS_GetVersion_Func)();
    auto getVersion = (EOS_GetVersion_Func)GetProcAddress(eosDLL, "EOS_GetVersion");
    
    if(getVersion) {
        const char* versionStr = getVersion();
        Logger::info("Game EOS SDK version (from DLL): %s", versionStr);
        
        // Parse version string "1.16.3" or "1.16.3-CL123456"
        if(sscanf(versionStr, "%d.%d.%d", 
                  &EOS_Compat::detectedMajor, 
                  &EOS_Compat::detectedMinor, 
                  &EOS_Compat::detectedPatch) >= 2) {
            EOS_Compat::versionDetected = true;
            Logger::info("Parsed EOS SDK version: %d.%d.%d", 
                        EOS_Compat::detectedMajor,
                        EOS_Compat::detectedMinor, 
                        EOS_Compat::detectedPatch);
            return true;
        }
    }
    
    // Method 2: Probe for version-specific functions
    Logger::warn("EOS_GetVersion not available, probing for version-specific functions...");
    
    // v1.16.0+ has EOS_Connect_Logout
    if(GetProcAddress(eosDLL, "EOS_Connect_Logout")) {
        Logger::info("Found EOS_Connect_Logout - SDK is v1.16.0+");
        EOS_Compat::detectedMajor = 1;
        EOS_Compat::detectedMinor = 16;
        EOS_Compat::versionDetected = true;
        return true;
    }
    
    // v1.15.0+ has EOS_Platform_GetDesktopCrossplayStatus
    if(GetProcAddress(eosDLL, "EOS_Platform_GetDesktopCrossplayStatus")) {
        Logger::info("Found EOS_Platform_GetDesktopCrossplayStatus - SDK is v1.15.x");
        EOS_Compat::detectedMajor = 1;
        EOS_Compat::detectedMinor = 15;
        EOS_Compat::versionDetected = true;
        return true;
    }
    
    // v1.14.0+ has EOS_Ecom_QueryOwnershipBySandboxIds
    if(GetProcAddress(eosDLL, "EOS_Ecom_QueryOwnershipBySandboxIds")) {
        Logger::info("Found EOS_Ecom_QueryOwnershipBySandboxIds - SDK is v1.14.x");
        EOS_Compat::detectedMajor = 1;
        EOS_Compat::detectedMinor = 14;
        EOS_Compat::versionDetected = true;
        return true;
    }
    
    // Assume v1.13.x if no newer features found
    Logger::warn("No version-specific functions found, assuming v1.13.x");
    EOS_Compat::detectedMajor = 1;
    EOS_Compat::detectedMinor = 13;
    EOS_Compat::versionDetected = true;
    return true;
}

/**
 * Get the appropriate API version constant based on detected SDK version
 */
int getCompatibleApiVersion(const char* apiName) {
    if(!EOS_Compat::versionDetected) {
        Logger::warn("SDK version not detected, using default API versions");
        return -1;  // Use compile-time constant
    }
    
    int major = EOS_Compat::detectedMajor;
    int minor = EOS_Compat::detectedMinor;
    
    // QueryPlayerAchievements API versions:
    // v1.13.0: API_LATEST = 2
    // v1.16.0: API_LATEST = 3 (added EpicUserId_DEPRECATED field)
    if(strcmp(apiName, "QueryPlayerAchievements") == 0) {
        if(major == 1 && minor >= 16) {
            Logger::debug("Using QueryPlayerAchievements ApiVersion 3 for v1.%d", minor);
            return 3;
        } else {
            Logger::debug("Using QueryPlayerAchievements ApiVersion 2 for v1.%d", minor);
            return 2;
        }
    }
    
    // CopyPlayerAchievementByIndex API versions:
    // v1.13.0: API_LATEST = 2
    // v1.16.0: API_LATEST = 3 (added LocalUserId field)
    if(strcmp(apiName, "CopyPlayerAchievementByIndex") == 0) {
        if(major == 1 && minor >= 16) {
            return 3;
        } else {
            return 2;
        }
    }
    
    // Platform Options API versions:
    // v1.13.0: API_LATEST = 11
    // v1.14.0: API_LATEST = 12 (added RTCOptions)
    // v1.15.0: API_LATEST = 13 (added TickBudgetInMilliseconds)
    if(strcmp(apiName, "PlatformOptions") == 0) {
        if(major == 1 && minor >= 15) {
            return 13;
        } else if(major == 1 && minor >= 14) {
            return 12;
        } else {
            return 11;
        }
    }
    
    return -1;  // Unknown API
}

/**
 * Check if a specific EOS feature/function is available in the detected SDK
 */
bool isFeatureAvailable(const char* featureName) {
    int major = EOS_Compat::detectedMajor;
    int minor = EOS_Compat::detectedMinor;
    
    // External auth providers (Apple, Google, Oculus, itch.io)
    if(strcmp(featureName, "ExternalAuthProviders") == 0) {
        return (major == 1 && minor >= 14);
    }
    
    // Desktop crossplay status
    if(strcmp(featureName, "DesktopCrossplay") == 0) {
        return (major == 1 && minor >= 15);
    }
    
    // Connect Logout
    if(strcmp(featureName, "ConnectLogout") == 0) {
        return (major == 1 && minor >= 16);
    }
    
    // Hidden achievements support
    if(strcmp(featureName, "HiddenAchievements") == 0) {
        return (major == 1 && minor >= 15);
    }
    
    return false;
}
```

---

## Usage in Your Code

### Example 1: QueryPlayerAchievements with Version Adaptation

```cpp
// OLD CODE (BROKEN):
EOS_Achievements_QueryPlayerAchievementsOptions QueryAchievementsOptions = {
    EOS_ACHIEVEMENTS_QUERYPLAYERACHIEVEMENTS_API_LATEST,  // Compile-time constant
    getProductUserId()
};

// NEW CODE (ADAPTIVE):
void queryPlayerAchievements() {
    int apiVersion = getCompatibleApiVersion("QueryPlayerAchievements");
    
    if(apiVersion == 3) {
        // v1.16.0+ structure (4 fields)
        struct QueryOptions_v3 {
            int32_t ApiVersion;
            EOS_ProductUserId TargetUserId;
            EOS_ProductUserId LocalUserId;
            EOS_EpicAccountId EpicUserId_DEPRECATED;
        } options = {
            3,
            getProductUserId(),
            getProductUserId(),
            nullptr
        };
        
        Logger::debug("Using v1.16+ QueryPlayerAchievements structure (4 fields)");
        EOS_Achievements_QueryPlayerAchievements(
            getHAchievements(),
            (EOS_Achievements_QueryPlayerAchievementsOptions*)&options,
            nullptr,
            queryPlayerAchievementsComplete
        );
    } 
    else if(apiVersion == 2) {
        // v1.13.0 structure (3 fields)
        struct QueryOptions_v2 {
            int32_t ApiVersion;
            EOS_ProductUserId TargetUserId;
            EOS_ProductUserId LocalUserId;
        } options = {
            2,
            getProductUserId(),
            getProductUserId()
        };
        
        Logger::debug("Using v1.13 QueryPlayerAchievements structure (3 fields)");
        EOS_Achievements_QueryPlayerAchievements(
            getHAchievements(),
            (EOS_Achievements_QueryPlayerAchievementsOptions*)&options,
            nullptr,
            queryPlayerAchievementsComplete
        );
    }
    else {
        // Fallback: Use compile-time constant
        Logger::warn("Unknown SDK version, using compile-time default");
        EOS_Achievements_QueryPlayerAchievementsOptions options = {
            EOS_ACHIEVEMENTS_QUERYPLAYERACHIEVEMENTS_API_LATEST,
            getProductUserId(),
            getProductUserId()
        };
        
        EOS_Achievements_QueryPlayerAchievements(
            getHAchievements(),
            &options,
            nullptr,
            queryPlayerAchievementsComplete
        );
    }
}
```

### Example 2: Platform Creation with Version Detection

```cpp
EOS_HPlatform createPlatformWithVersionDetection(HMODULE eosDLL) {
    // Detect game's SDK version
    if(!detectGameEOSSDKVersion(eosDLL)) {
        Logger::error("Failed to detect game's EOS SDK version");
    }
    
    // Get compatible API version
    int platformApiVer = getCompatibleApiVersion("PlatformOptions");
    if(platformApiVer == -1) {
        platformApiVer = 11;  // Default to v1.13 baseline
    }
    
    // Build platform options with appropriate size
    if(platformApiVer >= 13) {
        // v1.15.0+ structure
        struct PlatformOptions_v13 {
            int32_t ApiVersion;
            void* Reserved;
            const char* ProductId;
            const char* SandboxId;
            EOS_Platform_ClientCredentials ClientCredentials;
            EOS_Bool bIsServer;
            const char* EncryptionKey;
            const char* OverrideCountryCode;
            const char* OverrideLocaleCode;
            const char* DeploymentId;
            EOS_Platform_Flags Flags;
            const char* CacheDirectory;
            uint32_t TickBudgetInMilliseconds;  // NEW in v1.15
        } options = {};
        
        options.ApiVersion = 13;
        // ... fill other fields ...
        options.TickBudgetInMilliseconds = 0;  // Default/disabled
        
        Logger::info("Creating platform with v1.15+ options (ApiVersion 13)");
        return EOS_Platform_Create((EOS_Platform_Options*)&options);
    }
    else {
        // v1.13.0 structure
        EOS_Platform_Options options = {};
        options.ApiVersion = platformApiVer;
        // ... fill other fields ...
        
        Logger::info("Creating platform with v1.13 options (ApiVersion %d)", platformApiVer);
        return EOS_Platform_Create(&options);
    }
}
```

---

## Integration into ScreamAPI

### Step 1: Detect Version on DLL Load

In your `dllmain.cpp` or wherever you load the original EOS DLL:

```cpp
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if(ul_reason_for_call == DLL_PROCESS_ATTACH) {
        // Load the original EOS DLL
        HMODULE originalDLL = LoadLibrary("EOSSDK-Win32-Shipping_o.dll");
        
        if(originalDLL) {
            // Detect the game's SDK version
            bool detected = detectGameEOSSDKVersion(originalDLL);
            
            if(detected) {
                Logger::info("Game uses EOS SDK v%d.%d.%d",
                            EOS_Compat::detectedMajor,
                            EOS_Compat::detectedMinor,
                            EOS_Compat::detectedPatch);
                
                // Log available features
                Logger::info("Feature availability:");
                Logger::info("  Hidden Achievements: %s", 
                            isFeatureAvailable("HiddenAchievements") ? "YES" : "NO");
                Logger::info("  External Auth: %s",
                            isFeatureAvailable("ExternalAuthProviders") ? "YES" : "NO");
                Logger::info("  Connect Logout: %s",
                            isFeatureAvailable("ConnectLogout") ? "YES" : "NO");
            }
        }
        
        // ... rest of init ...
    }
    
    return TRUE;
}
```

### Step 2: Use Detected Version in All API Calls

Replace all hardcoded API version constants with runtime detection:

```cpp
// BEFORE:
#define EOS_ACHIEVEMENTS_QUERYPLAYERACHIEVEMENTS_API_LATEST 2

// AFTER:
#define EOS_ACHIEVEMENTS_QUERYPLAYERACHIEVEMENTS_API_V2 2
#define EOS_ACHIEVEMENTS_QUERYPLAYERACHIEVEMENTS_API_V3 3

// Use in code:
int apiVersion = getCompatibleApiVersion("QueryPlayerAchievements");
if(apiVersion == -1) {
    apiVersion = EOS_ACHIEVEMENTS_QUERYPLAYERACHIEVEMENTS_API_V2;  // Fallback
}
```

---

## Struct Size Calculation

For maximum compatibility, dynamically calculate struct sizes:

```cpp
size_t getQueryPlayerAchievementsOptionsSize() {
    int apiVersion = getCompatibleApiVersion("QueryPlayerAchievements");
    
    if(apiVersion == 3) {
        // v1.16.0+: 4 fields
        return sizeof(int32_t)              // ApiVersion
             + sizeof(EOS_ProductUserId)    // TargetUserId
             + sizeof(EOS_ProductUserId)    // LocalUserId
             + sizeof(EOS_EpicAccountId);   // EpicUserId_DEPRECATED
    } 
    else if(apiVersion == 2) {
        // v1.13.0: 3 fields
        return sizeof(int32_t)              // ApiVersion
             + sizeof(EOS_ProductUserId)    // TargetUserId
             + sizeof(EOS_ProductUserId);   // LocalUserId
    }
    
    // Fallback
    return sizeof(EOS_Achievements_QueryPlayerAchievementsOptions);
}

// Use when allocating or copying structs:
void* optionsBuffer = alloca(getQueryPlayerAchievementsOptionsSize());
memset(optionsBuffer, 0, getQueryPlayerAchievementsOptionsSize());
```

---

## Testing Matrix

After implementing version detection, test against multiple SDK versions:

| Game SDK Version | Expected ApiVersion | Test Result |
|------------------|---------------------|-------------|
| v1.13.0          | 2                   | [ ]         |
| v1.14.x          | 2                   | [ ]         |
| v1.15.x          | 2                   | [ ]         |
| v1.16.0          | 3                   | [ ]         |
| v1.16.3 (Beholder)| 3                  | [ ]         |

---

## Backward Compatibility Safety

To ensure ScreamAPI works with OLD games (v1.13 or earlier):

```cpp
// Always provide fallback to v1.13 behavior
if(!EOS_Compat::versionDetected) {
    Logger::warn("SDK version detection failed, assuming v1.13.0 compatibility mode");
    EOS_Compat::detectedMajor = 1;
    EOS_Compat::detectedMinor = 13;
    EOS_Compat::detectedPatch = 0;
    EOS_Compat::versionDetected = true;
}
```

This ensures ScreamAPI **never regresses** - it always works with v1.13 games, and **adds** support for newer versions.

---

## Quick Implementation Checklist

- [ ] Add `detectGameEOSSDKVersion()` function
- [ ] Add `getCompatibleApiVersion()` function
- [ ] Add `isFeatureAvailable()` function
- [ ] Call version detection in `DllMain`
- [ ] Replace all hardcoded API versions with runtime calls
- [ ] Test with Beholder (v1.16.3)
- [ ] Test with older game (v1.13.x)
- [ ] Add version detection to log file header

---

## Expected Log Output (Success)

```
[INFO]  ScreamAPI v1.13.0-1
[INFO]  Loading original EOS SDK: EOSSDK-Win32-Shipping_o.dll
[INFO]  Game EOS SDK version (from DLL): 1.16.3
[INFO]  Parsed EOS SDK version: 1.16.3
[INFO]  Feature availability:
[INFO]    Hidden Achievements: YES
[INFO]    External Auth: YES
[INFO]    Connect Logout: YES
[DEBUG] Using QueryPlayerAchievements ApiVersion 3 for v1.16
[DEBUG] Using v1.16+ QueryPlayerAchievements structure (4 fields)
[INFO]  Achievement query successful
```

---

## Long-term Benefits

1. **Future-proof**: Automatically adapts to v1.17, v1.18, etc.
2. **Backward compatible**: Still works with old games
3. **Debuggable**: Version detection logged for troubleshooting
4. **Maintainable**: One codebase for all SDK versions
5. **Testable**: Can simulate different SDK versions

---

## Alternative: Compile Multiple Versions

If runtime detection is too complex, you could build multiple DLLs:

- `ScreamAPI_v113.dll` - For SDK v1.13.x games
- `ScreamAPI_v116.dll` - For SDK v1.16.x games
- `ScreamAPI_auto.dll` - With version detection (recommended)

But this is **not recommended** because:
- Users must know which version to use
- More maintenance burden
- Harder to debug
- Version detection is not that complex

**Recommendation**: Implement runtime version detection. It's the most robust solution.

---

## 1.18.x support (added May 2026)

### New probe added to `detectSDKVersion()`

`EOS_PresenceModification_SetTemplateId` was added as a probe for 1.18.0+ games. It is the function backing the new Localized Presence feature introduced in 1.18 and is absent from all 1.17 and earlier DLLs. It sits at the top of the probe chain, above the existing `EOS_Connect_CopyIdToken` (1.17.0+) entry.

Updated probe order:

| Export present | Minimum version inferred |
|---|---|
| `EOS_PresenceModification_SetTemplateId` | 1.18.0 |
| `EOS_Connect_CopyIdToken` | 1.17.0 |
| `EOS_Connect_Logout` | 1.16.0 |
| `EOS_Platform_GetDesktopCrossplayStatus` | 1.15.0 |
| `EOS_Ecom_QueryOwnershipBySandboxIds` | 1.14.0 |
| *(none of the above)* | 1.13.0 |

### New feature: `"LocalizedPresence"`

`isFeatureAvailable("LocalizedPresence")` returns `true` for games on SDK 1.18.0+. It is logged in the compatibility block and can be used to gate any future Localized Presence forwarding logic.

### Updated compatibility block output (1.18 game)

```
[INFO]  [COMPAT] ========================================
[INFO]  [COMPAT] EOS SDK Compatibility Information
[INFO]  [COMPAT] ========================================
[INFO]  [COMPAT] ScreamAPI SDK Version: v1.18.1.2 (headers)
[INFO]  [COMPAT] Game SDK Version:      v1.18.1.2
[INFO]  [COMPAT]
[INFO]  [COMPAT] Feature Availability:
[INFO]  [COMPAT]   Connect Logout:          YES
[INFO]  [COMPAT]   Desktop Crossplay:       YES
[INFO]  [COMPAT]   External Auth Providers: YES
[INFO]  [COMPAT]   Hidden Achievements:     YES
[INFO]  [COMPAT]   RTC Options:             YES
[INFO]  [COMPAT]   Tick Budget:             YES
[INFO]  [COMPAT]   Integrated Platform:     YES
[INFO]  [COMPAT]   Task Network Timeout:    YES
[INFO]  [COMPAT]   Localized Presence:      YES
[INFO]  [COMPAT]
[INFO]  [COMPAT] API Versions:
[INFO]  [COMPAT]   PlatformOptions:         14
[INFO]  [COMPAT]   QueryPlayerAchievements: 2
[INFO]  [COMPAT]   CopyAchievementByIndex:  2
[INFO]  [COMPAT]
[INFO]  [COMPAT] Status: COMPATIBLE (Game >= ScreamAPI)
[INFO]  [COMPAT] ========================================
```
