#include "pch.h"
#include "util.h"
#include "ScreamAPI.h"
#include "eos-sdk/eos_auth.h"
#include "Logger.h"

namespace Util{

EOS_HPlatform hPlatform = nullptr;

std::string g_namespace_id;

std::filesystem::path getDLLparentDir(HMODULE hModule){
	WCHAR modulePathBuffer[MAX_PATH];
	GetModuleFileName(hModule, modulePathBuffer, MAX_PATH);

	std::filesystem::path modulePath = modulePathBuffer;
	return modulePath.parent_path();
}

EOS_HPlatform getHPlatform(){
	return hPlatform;
}

EOS_HAuth getHAuth(){
	// Don't cache - platform might be created after first call
	auto result = EOS_Platform_GetAuthInterface(getHPlatform());
	if(result == nullptr) {
		Logger::debug("[UTIL] getHAuth: Returned NULL");
	}
	return result;
}

EOS_HConnect getHConnect(){
	// Don't cache - platform might be created after first call
	auto result = EOS_Platform_GetConnectInterface(getHPlatform());
	if(result == nullptr) {
		Logger::debug("[UTIL] getHConnect: Returned NULL");
	}
	return result;
}

EOS_HAchievements getHAchievements(){
	// Don't cache - platform might be created after first call
	auto result = EOS_Platform_GetAchievementsInterface(getHPlatform());
	if(result == nullptr) {
		Logger::debug("[UTIL] getHAchievements: Returned NULL (Platform=%p)", getHPlatform());
	} else {
		Logger::debug("[UTIL] getHAchievements: Success (Handle=%p)", result);
	}
	return result;
}

bool isEOSPlatformReady(){
	auto platform = getHPlatform();
	if(platform == nullptr) {
		return false;
	}
	
	auto hAchievements = EOS_Platform_GetAchievementsInterface(platform);
	return (hAchievements != nullptr);
}

void logPlatformStatus(){
	auto platform = getHPlatform();
	
	Logger::debug("[UTIL] ========== EOS Platform Status ==========");
	Logger::debug("[UTIL] Platform Handle:     %p", platform);
	
	if(platform == nullptr) {
		Logger::error("[UTIL] Platform is NULL - cannot check interfaces");
		Logger::debug("[UTIL] ==========================================");
		return;
	}
	
	auto hConnect = EOS_Platform_GetConnectInterface(platform);
	auto hAuth = EOS_Platform_GetAuthInterface(platform);
	auto hAchievements = EOS_Platform_GetAchievementsInterface(platform);
	
	Logger::debug("[UTIL] Connect Interface:   %p %s", hConnect, hConnect ? "OK" : "NULL");
	Logger::debug("[UTIL] Auth Interface:      %p %s", hAuth, hAuth ? "OK" : "NULL");
	Logger::debug("[UTIL] Achievements Int:    %p %s", hAchievements, hAchievements ? "OK" : "NULL");
	
	auto productUserId = getProductUserId();
	auto epicAccountId = getEpicAccountId();
	
	Logger::debug("[UTIL] Product User ID:     %p %s", productUserId, productUserId ? "OK" : "NULL");
	Logger::debug("[UTIL] Epic Account ID:     %p %s", epicAccountId, epicAccountId ? "OK" : "NULL");
	Logger::debug("[UTIL] ==========================================");
}

EOS_EpicAccountId getEpicAccountId(){
	// Don't cache - get fresh value each time in case user logs in later
	auto hAuth = getHAuth();
	if(hAuth == nullptr) {
		Logger::debug("[UTIL] getEpicAccountId: HAuth is NULL");
		return nullptr;
	}
	auto result = EOS_Auth_GetLoggedInAccountByIndex(hAuth, 0);
	if(result == nullptr) {
		Logger::debug("[UTIL] getEpicAccountId: No logged-in account found at index 0");
	} else {
		Logger::debug("[UTIL] getEpicAccountId: Found account (Handle=%p)", result);
	}
	return result;
}

EOS_ProductUserId getProductUserId(){
	// Don't cache - get fresh value each time in case user logs in later
	auto hConnect = getHConnect();
	if(hConnect == nullptr) {
		Logger::debug("[UTIL] getProductUserId: HConnect is NULL");
		return nullptr;
	}
	auto result = EOS_Connect_GetLoggedInUserByIndex(hConnect, 0);
	if(result == nullptr) {
		Logger::debug("[UTIL] getProductUserId: No logged-in user found at index 0");
	} else {
		Logger::debug("[UTIL] getProductUserId: Found user (Handle=%p)", result);
	}
	return result;
}

/**
 * A small utility function that copies the c string into a newly allocated memory
 * @return Pointer to the new string
 */
char* copy_c_string(const char* c_string){
	// Get string size
	auto string_size = strlen(c_string) + 1;// +1 for null terminator

	// Allocate enough memory for the new string
	char* new_string = new char[string_size];

	// Copy the string contents
	strcpy_s(new_string, string_size, c_string);

	return new_string;
}

}
