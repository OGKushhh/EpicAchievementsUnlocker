# EOS SDK Migration Guide: v1.13.0 → v1.16.3
**ScreamAPI Achievement Manager Compatibility Update**

## Executive Summary

Your ScreamAPI tool with achievement manager overlay is currently using EOS SDK v1.13.0 (June 2021) but the game (Beholder 32-bit) uses v1.16.3 (August 2024). The main issues from your log:

```
[ERROR] EOS_Platform_Create returned NULL - platform initialization failed
[DEBUG] [UTIL] getHAchievements: Returned NULL (Platform=00000000)
[ERROR] [ACH] Cannot query achievements - HAchievements is NULL
```

**Root Cause**: The game's EOS SDK v1.16.3 has breaking changes in:
1. Achievement API structures (added fields)
2. Authentication flow (new external providers)
3. Platform initialization options
4. Connect interface changes

## Phase 1: Critical Achievements API Updates (v1.13.0 → v1.16.3)

### 1.1 QueryPlayerAchievements Structure Changes

**v1.13.0 (Current):**
```cpp
// EOS_ACHIEVEMENTS_QUERYPLAYERACHIEVEMENTS_API_LATEST = 2
EOS_Achievements_QueryPlayerAchievementsOptions {
    int32_t ApiVersion;             // = 2
    EOS_ProductUserId TargetUserId;
    EOS_ProductUserId LocalUserId;
}
```

**v1.16.3 (Target):**
```cpp
// EOS_ACHIEVEMENTS_QUERYPLAYERACHIEVEMENTS_API_LATEST = 3
EOS_Achievements_QueryPlayerAchievementsOptions {
    int32_t ApiVersion;             // = 3
    EOS_ProductUserId TargetUserId;
    EOS_ProductUserId LocalUserId;
    EOS_EpicAccountId EpicUserId_DEPRECATED;  // NEW - must be NULL
}
```

**Impact**: Your code at line 288-291 in `achievement_manager.cpp` needs updating:

**Current Code:**
```cpp
EOS_Achievements_QueryPlayerAchievementsOptions QueryAchievementsOptions = {
    EOS_ACHIEVEMENTS_QUERYPLAYERACHIEVEMENTS_API_LATEST,  // = 2
    getProductUserId()
};
```

**Fixed Code:**
```cpp
EOS_Achievements_QueryPlayerAchievementsOptions QueryAchievementsOptions = {
    3,  // EOS_ACHIEVEMENTS_QUERYPLAYERACHIEVEMENTS_API_LATEST in v1.16.3
    getProductUserId(),  // TargetUserId
    getProductUserId(),  // LocalUserId (was missing!)
    nullptr              // EpicUserId_DEPRECATED (new field)
};
```

### 1.2 CopyPlayerAchievementByIndex Structure Changes

**v1.13.0 (Current):**
```cpp
// EOS_ACHIEVEMENTS_COPYPLAYERACHIEVEMENTBYINDEX_API_LATEST = 2
EOS_Achievements_CopyPlayerAchievementByIndexOptions {
    int32_t ApiVersion;
    EOS_ProductUserId TargetUserId;
    uint32_t AchievementIndex;
}
```

**v1.16.3 (Target):**
```cpp
// EOS_ACHIEVEMENTS_COPYPLAYERACHIEVEMENTBYINDEX_API_LATEST = 3
EOS_Achievements_CopyPlayerAchievementByIndexOptions {
    int32_t ApiVersion;
    EOS_ProductUserId TargetUserId;
    uint32_t AchievementIndex;
    EOS_ProductUserId LocalUserId;  // NEW
}
```

**Impact**: Your code at lines 144-148 needs updating:

**Current Code:**
```cpp
EOS_Achievements_CopyPlayerAchievementByIndexOptions CopyAchievementOptions{
    EOS_ACHIEVEMENTS_COPYPLAYERACHIEVEMENTBYINDEX_API_LATEST,  // = 2
    getProductUserId(),
    i
};
```

**Fixed Code:**
```cpp
EOS_Achievements_CopyPlayerAchievementByIndexOptions CopyAchievementOptions{
    3,  // EOS_ACHIEVEMENTS_COPYPLAYERACHIEVEMENTBYINDEX_API_LATEST in v1.16.3
    getProductUserId(),  // TargetUserId
    i,                   // AchievementIndex
    getProductUserId()   // LocalUserId (new field)
};
```

### 1.3 Hidden Achievements Support

**v1.15.0 Added**: New achievement visibility API

**Changes Needed:**
- `EOS_Achievements_DefinitionV2` now properly supports `bIsHidden` field
- `QueryPlayerAchievements` returns ALL achievements (including hidden ones)

**What This Means**: Your current implementation at line 241 correctly handles hidden achievements:
```cpp
(bool)OutDefinition->bIsHidden,  // Already correct!
```

But you need to ensure the query includes hidden achievements. No code changes needed here, but be aware the behavior changed.

## Phase 2: Authentication & Platform Changes

### 2.1 New External Auth Providers (v1.14.0+)

Your screenshot shows v1.16.3 added:
- `EOS_Connect_Logout`
- `EOS_Connect_TransferDeviceIdAccount`
- Support for Apple, Google, Oculus, itch.io login

**Impact on ScreamAPI**: 
- If the game uses these newer auth methods, your proxy needs to intercept them
- Check if `EOS_Connect_Login` calls are using new `ExternalType` values

**Action Required**:
```cpp
// In eos_connect.cpp - verify these external types are handled:
EOS_EExternalCredentialType_APPLE_ID_TOKEN         // v1.14.0
EOS_EExternalCredentialType_GOOGLE_ID_TOKEN        // v1.14.0  
EOS_EExternalCredentialType_OCULUS_USERID_NONCE   // v1.14.0
EOS_EExternalCredentialType_ITCH_IO_JWT           // v1.14.1
```

### 2.2 Platform Initialization Issues

Your log shows:
```
[INFO]  Platform not initialized by game - attempting manual initialization
[ERROR] EOS_Platform_Create returned NULL - platform initialization failed
```

**Likely Cause**: The game's `EOS_Platform_Options` structure has changed.

**v1.13.0 Structure:**
```cpp
// EOS_PLATFORM_OPTIONS_API_LATEST = 11
EOS_Platform_Options {
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
    // 11 fields total
}
```

**v1.16.3 Structure (Estimated):**
```cpp
// EOS_PLATFORM_OPTIONS_API_LATEST = 13 (likely)
EOS_Platform_Options {
    // All previous fields...
    // Plus new fields for:
    uint32_t TickBudgetInMilliseconds;  // v1.15.0
    EOS_Platform_RTCOptions* RTCOptions; // v1.14.0
    // Possibly more...
}
```

**Action Required**: You need to update your platform creation code in `eos_init.cpp` to handle the new structure size and fields.

## Phase 3: Specific File Updates Needed

### File 1: `/ScreamAPI/src/achievement_manager.cpp`

**Line 288-291**: Update QueryPlayerAchievementsOptions
```cpp
EOS_Achievements_QueryPlayerAchievementsOptions QueryAchievementsOptions = {
    3,                   // ApiVersion (was 2)
    getProductUserId(),  // TargetUserId
    getProductUserId(),  // LocalUserId (NEW - was missing)
    nullptr              // EpicUserId_DEPRECATED (NEW field)
};
```

**Line 144-148**: Update CopyPlayerAchievementByIndexOptions
```cpp
EOS_Achievements_CopyPlayerAchievementByIndexOptions CopyAchievementOptions{
    3,                   // ApiVersion (was 2)
    getProductUserId(),  // TargetUserId
    i,                   // AchievementIndex
    getProductUserId()   // LocalUserId (NEW field)
};
```

**Line 321-327**: Update QueryDefinitionsOptions (May need changes)
```cpp
// Current uses ApiVersion = 1 (legacy)
// Check if v1.16.3 requires ApiVersion = 3
EOS_Achievements_QueryDefinitionsOptions QueryDefinitionsOptions = {
    3,  // Try updating from 1
    productUserId,
    epicAccountId,
    nullptr,
    0,
    nullptr  // May need new field: EOS_Bool bIncludeHidden
};
```

### File 2: `/ScreamAPI/src/eos-sdk/eos_achievements_types.h`

**Replace entire file** with v1.16.3 version from Epic's SDK download.

**Critical structures to verify**:
- `EOS_Achievements_QueryPlayerAchievementsOptions` (ApiVersion 3)
- `EOS_Achievements_CopyPlayerAchievementByIndexOptions` (ApiVersion 3)
- `EOS_Achievements_QueryDefinitionsOptions` (check latest version)
- `EOS_Achievements_PlayerAchievement` (check for new fields)

### File 3: `/ScreamAPI/src/eos-sdk/eos_achievements.h`

**Replace entire file** with v1.16.3 version.

**Functions added in v1.14.0-v1.16.3** that you may need to proxy:
- None for achievements specifically, but verify function signatures match

### File 4: `/ScreamAPI/src/eos-sdk/eos_version.h`

**Update version numbers:**
```cpp
#define EOS_MAJOR_VERSION   1
#define EOS_MINOR_VERSION   16  // Was 13
#define EOS_PATCH_VERSION   3   // Was 0
```

### File 5: `/ScreamAPI/src/eos-sdk/eos_connect_types.h`

**Add new external credential types** (v1.14.0+):
```cpp
EOS_DECLARE_FUNC(EOS_EResult) EOS_Connect_Logout(
    EOS_HConnect Handle,
    const EOS_Connect_LogoutOptions* Options,
    void* ClientData,
    const EOS_Connect_OnLogoutCallback CompletionDelegate
);

EOS_DECLARE_FUNC(EOS_EResult) EOS_Connect_TransferDeviceIdAccount(
    EOS_HConnect Handle,
    const EOS_Connect_TransferDeviceIdAccountOptions* Options,
    void* ClientData,
    const EOS_Connect_OnTransferDeviceIdAccountCallback CompletionDelegate
);
```

### File 6: `/ScreamAPI/src/eos-impl/eos_achievements.cpp`

**Update all structure initializations** to match new API versions.

**Example pattern to follow**:
```cpp
// Before proxying EOS_Achievements_QueryPlayerAchievements:
static void* EOS_CALL EOS_Achievements_QueryPlayerAchievements_Hook(
    EOS_HAchievements Handle,
    const EOS_Achievements_QueryPlayerAchievementsOptions* Options,
    void* ClientData,
    const EOS_Achievements_OnQueryPlayerAchievementsCompleteCallback CompletionDelegate
) {
    // Validate ApiVersion
    if(Options->ApiVersion != 3) {
        Logger::warn("Game using QueryPlayerAchievements ApiVersion %d, expected 3", 
                     Options->ApiVersion);
    }
    
    // Forward to original
    return EOS_Achievements_QueryPlayerAchievements_Original(
        Handle, Options, ClientData, CompletionDelegate
    );
}
```

## Phase 4: Testing Strategy

### Step 1: Incremental Updates
1. **Update ONLY the achievements types** (`eos_achievements_types.h`)
2. **Update ApiVersion numbers** in your code (3 instead of 2)
3. **Add missing struct fields** (LocalUserId, EpicUserId_DEPRECATED)
4. **Test** - Does it crash? Does it query?

### Step 2: Verify Structure Sizes
```cpp
// Add debug logging to verify struct alignment:
Logger::debug("QueryPlayerAchievementsOptions size: %d", 
              sizeof(EOS_Achievements_QueryPlayerAchievementsOptions));
Logger::debug("CopyPlayerAchievementByIndexOptions size: %d",
              sizeof(EOS_Achievements_CopyPlayerAchievementByIndexOptions));
```

Compare these sizes with the game's EOS DLL to ensure binary compatibility.

### Step 3: Platform Initialization Fix

The NULL platform is your **biggest problem**. Debug this first:

```cpp
// In eos_init.cpp or wherever you call EOS_Platform_Create:
EOS_Platform_Options PlatformOptions = { /* ... */ };

Logger::debug("Calling EOS_Platform_Create with ApiVersion: %d", PlatformOptions.ApiVersion);
Logger::debug("ProductId: %s", PlatformOptions.ProductId);
Logger::debug("SandboxId: %s", PlatformOptions.SandboxId);

EOS_HPlatform platform = EOS_Platform_Create(&PlatformOptions);

if(platform == nullptr) {
    // Try with different ApiVersion values
    for(int ver = 11; ver <= 13; ver++) {
        PlatformOptions.ApiVersion = ver;
        platform = EOS_Platform_Create(&PlatformOptions);
        if(platform != nullptr) {
            Logger::info("SUCCESS: Platform created with ApiVersion %d", ver);
            break;
        }
    }
}
```

## Phase 5: Quick Fix Patch (Apply This First)

Here's the minimal code change to test if API version is the issue:

### Patch 1: achievement_manager.cpp

```cpp
// Line 288-291 - ADD THE MISSING FIELD:
EOS_Achievements_QueryPlayerAchievementsOptions QueryAchievementsOptions = {
    EOS_ACHIEVEMENTS_QUERYPLAYERACHIEVEMENTS_API_LATEST,  // Keep as is
    getProductUserId(),   // TargetUserId
    getProductUserId(),   // LocalUserId - THIS WAS MISSING!!!
};

// Line 144-148 - ADD THE MISSING FIELD:
EOS_Achievements_CopyPlayerAchievementByIndexOptions CopyAchievementOptions{
    EOS_ACHIEVEMENTS_COPYPLAYERACHIEVEMENTBYINDEX_API_LATEST,  // Keep as is
    getProductUserId(),   // TargetUserId
    i,                    // AchievementIndex
    getProductUserId()    // LocalUserId - THIS WAS MISSING!!!
};
```

**Why this might fix it**: Your current code is passing only 2 fields when the structure expects 3 (in v1.13) or 4 (in v1.16.3). This causes stack corruption or parameter misalignment.

## Phase 6: Long-Term Solution

### Option A: Update to Full v1.16.3 SDK
1. Download EOS SDK v1.16.3 from Epic Developer Portal
2. Replace entire `/ScreamAPI/src/eos-sdk/` directory
3. Update all API version constants
4. Update all structure initializations
5. Test thoroughly

**Pros**: Full compatibility with modern games
**Cons**: More breaking changes, need to update all proxied functions

### Option B: Hybrid Approach (Recommended)
1. Keep v1.13.0 SDK headers for now
2. Add **conditional version detection** in your proxy
3. Dynamically adjust structure sizes based on game's SDK version
4. Use runtime checks instead of compile-time constants

**Example**:
```cpp
int detectEOSSDKVersion() {
    // Try to call a v1.16.3-only function
    auto func = GetProcAddress(gameDLL, "EOS_Connect_Logout");
    if(func != nullptr) return 163;  // v1.16.3
    
    // Try v1.15.0 function
    func = GetProcAddress(gameDLL, "EOS_Platform_GetDesktopCrossplayStatus");
    if(func != nullptr) return 150;
    
    return 130;  // Default to v1.13.0
}
```

## Critical Action Items (Priority Order)

### 🔴 CRITICAL - Do This First
1. **Fix LocalUserId missing field** in QueryPlayerAchievementsOptions (Line 288-291)
2. **Fix LocalUserId missing field** in CopyPlayerAchievementByIndexOptions (Line 144-148)
3. **Debug platform initialization** - Find out WHY it returns NULL

### 🟡 HIGH PRIORITY
4. Update `eos_achievements_types.h` to v1.16.3
5. Update ApiVersion constants (2 → 3)
6. Add new external auth provider support in connect proxy

### 🟢 MEDIUM PRIORITY  
7. Update `eos_version.h` to v1.16.3
8. Add runtime SDK version detection
9. Update platform options structure

### 🔵 LOW PRIORITY
10. Full SDK replacement (only if above doesn't work)
11. Implement v1.16.3-only features

## Expected Outcomes

After Phase 1 fixes (missing LocalUserId fields):
```
[INFO]  Successfully loaded original EOS SDK: EOSSDK-Win32-Shipping.dll
[INFO]  Platform initialized successfully
[DEBUG] [UTIL] getHAchievements: Returned 0x12345678 (Platform=0xABCDEF00)
[DEBUG] [ACH] Querying achievement definitions...
[INFO]  Found 25 achievement definitions
[INFO]  Achievement Manager: Ready
```

## Still Have Issues?

If after fixing the struct fields you still get NULL:

### Check 1: DLL Export Names
```bash
# Verify the game's EOS DLL exports the functions you expect:
dumpbin /exports EOSSDK-Win32-Shipping.dll | grep -i achievement
```

### Check 2: Calling Convention
- v1.13.0 uses `__stdcall` 
- Verify v1.16.3 didn't change to `__cdecl`

### Check 3: Structure Padding
```cpp
#pragma pack(push, 8)  // Ensure 8-byte alignment
struct EOS_Achievements_QueryPlayerAchievementsOptions { /* ... */ };
#pragma pack(pop)
```

## Next Steps

1. **Apply the quick fix patch first** (add LocalUserId fields)
2. **Test with Beholder** - Does it still return NULL?
3. **If still NULL**: Debug the platform initialization
4. **If platform OK but no achievements**: Update to v1.16.3 achievement types
5. **Report back** with new log output

---

**Note**: The fact that your overlay loads and renders correctly means the DLL injection and hooking works. The problem is purely with the EOS SDK API compatibility layer. Focus on structure alignment first, then platform init.

---

## 1.17.1.3 → 1.18.1.2 (May 2026)

### What changed in the headers

| File | Change |
|---|---|
| `eos_version.h` | Minor 17 → 18, Patch 1 → 1, Hotfix 3 → 2 |
| `eos_presence_localized_types.h` | **New file** — Localized Presence feature (unused by ScreamAPI directly) |
| All other headers | Identical structure layout, identical `_API_LATEST` constants |

### API version constants — before and after

Every constant ScreamAPI actually uses stayed the same:

| Constant | 1.17.1.3 | 1.18.1.2 |
|---|---|---|
| `EOS_ACHIEVEMENTS_QUERYDEFINITIONS_API_LATEST` | 3 | 3 |
| `EOS_ACHIEVEMENTS_QUERYPLAYERACHIEVEMENTS_API_LATEST` | 2 | 2 |
| `EOS_ACHIEVEMENTS_UNLOCKACHIEVEMENTS_API_LATEST` | 1 | 1 |
| `EOS_CONNECT_LOGIN_API_LATEST` | 2 | 2 |
| `EOS_INITIALIZE_API_LATEST` | 4 | 4 |

### Source code changes required

**None in ScreamAPI source.** No structs used by `eos_hooks.cpp`, `achievement_manager.cpp`, or `eos_compat.cpp` changed shape.

The only changes were:

- `eos_compat.cpp` — added `EOS_PresenceModification_SetTemplateId` probe to the fallback detection chain (identifies 1.18.0+ games that don't export `EOS_GetVersion`), added `"LocalizedPresence"` to `isFeatureAvailable()`, and added the corresponding log line in `logCompatibilityInfo()`
- Docs updated

The `EOS_Compat` runtime detection layer automatically logs `1.18.1.2` for games that ship with the new runtime DLL.

### Upgrade procedure

1. Delete the contents of `ScreamAPI/src/eos-sdk/`
2. Copy all files from the SDK download's `SDK/Include/` folder into `ScreamAPI/src/eos-sdk/`
3. Rebuild — no compiler errors expected

### How to get the SDK without an organization account

You do **not** need an organization account. A personal Epic Games account is sufficient:

1. Sign in at <https://dev.epicgames.com/portal>
2. Create any product (name doesn't matter)
3. Go to **Product Settings → SDK & Release Notes**
4. Download the Windows SDK zip

The `Include/` folder inside is all you need — the rest of the 0.5 GB is platform libs, binaries, and samples.

---

*See `UPGRADE_CHANGELOG_v1.17.3.md` for the full 1.13.0 → 1.17.1.3 history.*
