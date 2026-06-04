#include "pch.h"
#include "Config.h"
#include "ConfigUtils.h"
#include <map>
#include <set>

namespace Config{

// ScreamAPI
bool bEnableItemUnlocker = true;
bool bEnableEntitlementUnlocker = true;
bool bEnableLogging = false;
bool bEnableOverlay = false;
bool bForceAchievementsConfig = false;
bool bEnableKeyboardNavigation = true;   // <-- NEW
// Logging
std::string sLogLevel = "INFO";
std::string sLogFilename = "ScreamAPI.log";
bool bLogDLCQueries = true;
bool bLogAchievementQueries = false;
bool bLogOverlay = false;
// Overlay
bool bLoadIcons = true;
bool bCacheIcons = true;
bool bValidateIcons = true;
bool bForceEpicOverlay = false;
bool bEnableDX12Hook = false;
// DLC
bool bUnlockAllDLC = true;
bool bForceSuccess = true;
// DLC_List
std::vector<std::string> vDLC_List;
// Custom path
std::string sCustomEOSPath = "";

// Set of configuration keys that are strings (not booleans)
static const std::set<std::string> stringKeys = {
    "LogLevel",
    "LogFilename",
    "CustomEOSPath"
};

std::map<std::string, std::map<std::string, void*>> configMap = {
    {"ScreamAPI", {
        {"EnableItemUnlocker", &bEnableItemUnlocker},
        {"EnableEntitlementUnlocker", &bEnableEntitlementUnlocker},
        {"EnableLogging", &bEnableLogging},
        {"EnableOverlay", &bEnableOverlay},
        {"ForceAchievementsConfig", &bForceAchievementsConfig},
        {"EnableKeyboardNavigation", &bEnableKeyboardNavigation},
        {"EnableDX12Hook", &bEnableDX12Hook},
        {"CustomEOSPath", &sCustomEOSPath},
    }},
    {"Logging", {
        {"LogLevel", &sLogLevel},
        {"LogFilename", &sLogFilename},
        {"LogDLCQueries", &bLogDLCQueries},
        {"LogAchievementQueries", &bLogAchievementQueries},
        {"LogOverlay", &bLogOverlay}
    }},
    {"Overlay",{
        {"LoadIcons", &bLoadIcons},
        {"CacheIcons", &bCacheIcons},
        {"ValidateIcons", &bValidateIcons},
        {"ForceEpicOverlay", &bForceEpicOverlay},
    }},
    {"DLC", {
        {"UnlockAllDLC", &bUnlockAllDLC},
        {"ForceSuccess", &bForceSuccess},
    }},
};

int iniHandler(void* user, const char* section_raw, const char* name_raw, const char* value_raw){
    std::string section = section_raw;
    std::string name = name_raw;
    std::string value = value_raw;

    try{
        if(section == "DLC_List"){
            if(stringToBool(value))
                vDLC_List.push_back(name);
            return TRUE;
        } else{
            try{
                auto sectionMap = configMap.at(section);
                try{
                    auto varPtr = sectionMap.at(name);

                    // Unified logic: check if this key is a string
                    if(stringKeys.find(name) != stringKeys.end()){
                        auto stringPtr = static_cast<std::string*>(varPtr);
                        *stringPtr = value;
                    } else{
                        auto boolPtr = static_cast<bool*>(varPtr);
                        *boolPtr = stringToBool(value);
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

// ScreamAPI
bool EnableItemUnlocker(){ return bEnableItemUnlocker; }
bool EnableEntitlementUnlocker(){ return bEnableEntitlementUnlocker; }
bool EnableOverlay(){ return bEnableOverlay; }
bool ForceAchievementsConfig(){ return bForceAchievementsConfig; }
bool EnableKeyboardNavigation(){ return bEnableKeyboardNavigation; }   // <-- NEW
// Logging
bool EnableLogging(){ return bEnableLogging; }
std::string LogLevel(){ return sLogLevel; }
std::string LogFilename() { return sLogFilename; }
bool LogDLCQueries(){ return bLogDLCQueries; }
bool LogAchievementQueries(){ return bLogAchievementQueries; }
bool LogOverlay(){ return bLogOverlay; }
// Overlay
bool LoadIcons() { return bLoadIcons; }
bool CacheIcons() { return bCacheIcons; }
bool ValidateIcons(){ return bValidateIcons; }
bool ForceEpicOverlay(){ return bForceEpicOverlay; }
bool EnableDX12Hook(){ return bEnableDX12Hook; }
// DLC
bool UnlockAllDLC(){ return bUnlockAllDLC; }
bool ForceSuccess(){ return bForceSuccess; }
// DLC_List
std::vector<std::string> DLC_List(){ return vDLC_List; }
// Custom path
std::string GetCustomEOSPath(){ return sCustomEOSPath; }

}