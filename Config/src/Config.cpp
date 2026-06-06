#include "pch.h"
#include "Config.h"
#include "ConfigUtils.h"
#include <map>
#include <set>

namespace Config{

// ScreamAPI
bool bEnableOwnershipUnlocker   = true;
bool bEnableEntitlementUnlocker = true;
bool bEnableLogging             = false;
bool bEnableOverlay             = false;
bool bForceAchievementsConfig   = false;
bool bEnableKeyboardNavigation  = true;
bool bBlockMetrics              = false;
// Logging
std::string sLogLevel    = "INFO";
std::string sLogFilename = "ScreamAPI.log";
bool bLogDLCQueries         = true;
bool bLogAchievementQueries = false;
bool bLogOverlay            = false;
// Overlay
bool bLoadIcons      = true;
bool bCacheIcons     = true;
bool bValidateIcons  = true;
bool bForceEpicOverlay = false;
bool bEnableDX12Hook = false;
// DLC
bool bUnlockAllDLC = true;
bool bForceSuccess = true;
// DLC_List (legacy)
std::vector<std::string> vDLC_List;
// Custom path
std::string sCustomEOSPath = "";
// Namespace
std::string sNamespaceId = "";

// Per-item DLC override (from [DLC_Override])
enum class DlcOverrideStatus { UNLOCKED, LOCKED, ORIGINAL };
std::map<std::string, DlcOverrideStatus> mDlcOverride;

// Extra entitlements (from [Extra_Entitlements])
std::map<std::string, std::string> mExtraEntitlements;

// -------------------------------------------------------------------------

static const std::set<std::string> stringKeys = {
    "LogLevel",
    "LogFilename",
    "CustomEOSPath",
    "NamespaceId",
};

std::map<std::string, std::map<std::string, void*>> configMap = {
    {"ScreamAPI", {
        // Current name
        {"EnableOwnershipUnlocker",   &bEnableOwnershipUnlocker},
        // Backward-compat alias for old config files
        {"EnableItemUnlocker",        &bEnableOwnershipUnlocker},
        {"EnableEntitlementUnlocker", &bEnableEntitlementUnlocker},
        {"EnableLogging",             &bEnableLogging},
        {"EnableOverlay",             &bEnableOverlay},
        {"ForceAchievementsConfig",   &bForceAchievementsConfig},
        {"EnableKeyboardNavigation",  &bEnableKeyboardNavigation},
        {"EnableDX12Hook",            &bEnableDX12Hook},
        {"BlockMetrics",              &bBlockMetrics},
        {"CustomEOSPath",             &sCustomEOSPath},
        {"NamespaceId",               &sNamespaceId},
    }},
    {"Logging", {
        {"LogLevel",              &sLogLevel},
        {"LogFilename",           &sLogFilename},
        {"LogDLCQueries",         &bLogDLCQueries},
        {"LogAchievementQueries", &bLogAchievementQueries},
        {"LogOverlay",            &bLogOverlay},
    }},
    {"Overlay", {
        {"LoadIcons",        &bLoadIcons},
        {"CacheIcons",       &bCacheIcons},
        {"ValidateIcons",    &bValidateIcons},
        {"ForceEpicOverlay", &bForceEpicOverlay},
    }},
    {"DLC", {
        {"UnlockAllDLC", &bUnlockAllDLC},
        {"ForceSuccess", &bForceSuccess},
    }},
};

int iniHandler(void* user, const char* section_raw, const char* name_raw, const char* value_raw){
    std::string section = section_raw;
    std::string name    = name_raw;
    std::string value   = value_raw;

    try{
        // [DLC_Override] - per-item 3-way status
        if(section == "DLC_Override"){
            DlcOverrideStatus status = DlcOverrideStatus::UNLOCKED;
            if(value == "locked")        status = DlcOverrideStatus::LOCKED;
            else if(value == "original") status = DlcOverrideStatus::ORIGINAL;
            else if(value != "unlocked"){
                showError("Invalid DLC_Override value '" + value + "' for: " + name
                          + "  (expected: unlocked | locked | original)");
                return FALSE;
            }
            mDlcOverride[name] = status;
            return TRUE;
        }

        // [Extra_Entitlements] - manually injected entitlements
        // name = entitlement ID, value = display title (plain string, no bool conversion)
        if(section == "Extra_Entitlements"){
            mExtraEntitlements[name] = value;
            return TRUE;
        }

        // [DLC_List] - legacy boolean list
        if(section == "DLC_List"){
            if(stringToBool(value))
                vDLC_List.push_back(name);
            return TRUE;
        }

        // All other known sections
        try{
            auto sectionMap = configMap.at(section);
            try{
                auto varPtr = sectionMap.at(name);
                if(stringKeys.find(name) != stringKeys.end()){
                    *static_cast<std::string*>(varPtr) = value;
                } else{
                    *static_cast<bool*>(varPtr) = stringToBool(value);
                }
                return TRUE;
            } catch(std::out_of_range&){
                showError("Invalid name (" + name + ") at section [" + section + "]");
                return FALSE;
            }
        } catch(std::out_of_range&){
            showError("Invalid section name: " + section);
            return FALSE;
        }
    } catch(InvalidBoolValue& ex){
        showError("Invalid boolean value (" + ex.value + ") for name [" + name + "]");
        return FALSE;
    }
}

void init(const std::wstring iniPath){
    int parseResult = ini_wparse(iniPath.c_str(), iniHandler, 0);
    if(parseResult != 0 && parseResult != -1){
        showError("Unexpected config parse result at line: " + std::to_string(parseResult));
        exit(1);
    }
}

// Accessors
bool EnableOwnershipUnlocker()    { return bEnableOwnershipUnlocker; }
bool EnableEntitlementUnlocker()  { return bEnableEntitlementUnlocker; }
bool EnableOverlay()              { return bEnableOverlay; }
bool ForceAchievementsConfig()    { return bForceAchievementsConfig; }
bool EnableKeyboardNavigation()   { return bEnableKeyboardNavigation; }
bool EnableLogging()              { return bEnableLogging; }
bool BlockMetrics()               { return bBlockMetrics; }
std::string LogLevel()            { return sLogLevel; }
std::string LogFilename()         { return sLogFilename; }
bool LogDLCQueries()              { return bLogDLCQueries; }
bool LogAchievementQueries()      { return bLogAchievementQueries; }
bool LogOverlay()                 { return bLogOverlay; }
bool LoadIcons()                  { return bLoadIcons; }
bool CacheIcons()                 { return bCacheIcons; }
bool ValidateIcons()              { return bValidateIcons; }
bool ForceEpicOverlay()           { return bForceEpicOverlay; }
bool EnableDX12Hook()             { return bEnableDX12Hook; }
bool UnlockAllDLC()               { return bUnlockAllDLC; }
bool ForceSuccess()               { return bForceSuccess; }
std::vector<std::string> DLC_List() { return vDLC_List; }
std::string GetCustomEOSPath()    { return sCustomEOSPath; }
std::string NamespaceId()         { return sNamespaceId; }

std::map<std::string, std::string> ExtraEntitlements(){
    return mExtraEntitlements;
}

bool IsDlcUnlocked(const std::string& id, bool original_unlocked){
    // 1. Check per-item override from [DLC_Override]
    auto it = mDlcOverride.find(id);
    if(it != mDlcOverride.end()){
        switch(it->second){
            case DlcOverrideStatus::UNLOCKED: return true;
            case DlcOverrideStatus::LOCKED:   return false;
            case DlcOverrideStatus::ORIGINAL: return original_unlocked;
        }
    }
    // 2. Legacy fallback — fully backward compatible
    bool inList = std::find(vDLC_List.begin(), vDLC_List.end(), id) != vDLC_List.end();
    return bUnlockAllDLC || inList;
}

}
