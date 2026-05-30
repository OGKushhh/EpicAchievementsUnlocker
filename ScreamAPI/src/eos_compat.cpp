#include "pch.h"
#include "eos_compat.h"
#include "ScreamAPI.h"

namespace EOS_Compat {

SDKVersion gameSDKVersion;

bool detectSDKVersion(HMODULE eosDLL) {
	if (!eosDLL) {
		Logger::error("[COMPAT] Cannot detect SDK version - DLL handle is NULL");
		return false;
	}

	// Method 1: Try to call EOS_GetVersion() if available
	typedef const char* (EOS_CALL* EOS_GetVersion_Func)();
	auto getVersion = (EOS_GetVersion_Func)GetProcAddress(eosDLL, "EOS_GetVersion");

	if (getVersion) {
		const char* versionStr = getVersion();
		Logger::info("[COMPAT] Game EOS SDK version (from DLL): %s", versionStr);

		// Parse version string "1.17.1.3" or "1.17.1.3-CL123456"
		int parsed = sscanf_s(versionStr, "%d.%d.%d.%d",
			&gameSDKVersion.major,
			&gameSDKVersion.minor,
			&gameSDKVersion.patch,
			&gameSDKVersion.hotfix);

		if (parsed >= 3) {
			gameSDKVersion.detected = true;
			Logger::info("[COMPAT] Parsed EOS SDK version: %d.%d.%d.%d",
				gameSDKVersion.major,
				gameSDKVersion.minor,
				gameSDKVersion.patch,
				gameSDKVersion.hotfix);
			return true;
		}
	}

	// Method 2: Probe for version-specific functions
	Logger::warn("[COMPAT] EOS_GetVersion not available, probing for version-specific functions...");

	// v1.18.0+ has EOS_PresenceModification_SetTemplateId (Localized Presence)
	if (GetProcAddress(eosDLL, "EOS_PresenceModification_SetTemplateId")) {
		Logger::info("[COMPAT] Found EOS_PresenceModification_SetTemplateId - SDK is v1.18.0+");
		gameSDKVersion.major = 1;
		gameSDKVersion.minor = 18;
		gameSDKVersion.patch = 0;
		gameSDKVersion.detected = true;
		return true;
	}

	// v1.17.0+ has EOS_Connect_CopyIdToken
	if (GetProcAddress(eosDLL, "EOS_Connect_CopyIdToken")) {
		Logger::info("[COMPAT] Found EOS_Connect_CopyIdToken - SDK is v1.17.0+");
		gameSDKVersion.major = 1;
		gameSDKVersion.minor = 17;
		gameSDKVersion.patch = 0;
		gameSDKVersion.detected = true;
		return true;
	}

	// v1.16.0+ has EOS_Connect_Logout
	if (GetProcAddress(eosDLL, "EOS_Connect_Logout")) {
		Logger::info("[COMPAT] Found EOS_Connect_Logout - SDK is v1.16.0+");
		gameSDKVersion.major = 1;
		gameSDKVersion.minor = 16;
		gameSDKVersion.patch = 0;
		gameSDKVersion.detected = true;
		return true;
	}

	// v1.15.0+ has EOS_Platform_GetDesktopCrossplayStatus
	if (GetProcAddress(eosDLL, "EOS_Platform_GetDesktopCrossplayStatus")) {
		Logger::info("[COMPAT] Found EOS_Platform_GetDesktopCrossplayStatus - SDK is v1.15.0+");
		gameSDKVersion.major = 1;
		gameSDKVersion.minor = 15;
		gameSDKVersion.patch = 0;
		gameSDKVersion.detected = true;
		return true;
	}

	// v1.14.0+ has EOS_Ecom_QueryOwnershipBySandboxIds
	if (GetProcAddress(eosDLL, "EOS_Ecom_QueryOwnershipBySandboxIds")) {
		Logger::info("[COMPAT] Found EOS_Ecom_QueryOwnershipBySandboxIds - SDK is v1.14.0+");
		gameSDKVersion.major = 1;
		gameSDKVersion.minor = 14;
		gameSDKVersion.patch = 0;
		gameSDKVersion.detected = true;
		return true;
	}

	// Assume v1.13.0 if no newer features found
	Logger::warn("[COMPAT] No version-specific functions found, assuming v1.13.0");
	gameSDKVersion.major = 1;
	gameSDKVersion.minor = 13;
	gameSDKVersion.patch = 0;
	gameSDKVersion.detected = true;
	return true;
}

const char* getVersionString() {
	static char versionStr[64];
	if (gameSDKVersion.detected) {
		sprintf_s(versionStr, "v%d.%d.%d.%d",
			gameSDKVersion.major,
			gameSDKVersion.minor,
			gameSDKVersion.patch,
			gameSDKVersion.hotfix);
	}
	else {
		strcpy_s(versionStr, "unknown");
	}
	return versionStr;
}

bool isVersionOrNewer(int major, int minor, int patch) {
	if (!gameSDKVersion.detected) return false;

	if (gameSDKVersion.major > major) return true;
	if (gameSDKVersion.major < major) return false;

	if (gameSDKVersion.minor > minor) return true;
	if (gameSDKVersion.minor < minor) return false;

	if (gameSDKVersion.patch >= patch) return true;
	return false;
}

int getApiVersion(const char* apiName) {
	if (!gameSDKVersion.detected) {
		return -1;  // Use compile-time constant
	}

	// Achievement API versions are stable across all versions
	if (strcmp(apiName, "QueryPlayerAchievements") == 0) {
		return 2;  // Stable since v1.13.0
	}

	if (strcmp(apiName, "CopyPlayerAchievementByIndex") == 0) {
		return 2;  // Stable since v1.13.0
	}

	if (strcmp(apiName, "QueryDefinitions") == 0) {
		return 2;  // Stable since v1.13.0
	}

	// Platform Options evolved significantly
	if (strcmp(apiName, "PlatformOptions") == 0) {
		if (isVersionOrNewer(1, 17, 0)) return 14;  // v1.17.0+
		if (isVersionOrNewer(1, 16, 0)) return 13;  // v1.16.0
		if (isVersionOrNewer(1, 15, 0)) return 13;  // v1.15.0 added TickBudget
		if (isVersionOrNewer(1, 14, 0)) return 12;  // v1.14.0 added RTCOptions
		return 11;  // v1.13.0
	}

	return -1;  // Unknown API
}

bool isFeatureAvailable(const char* featureName) {
	if (!gameSDKVersion.detected) return false;

	// Connect Logout (v1.16.0+)
	if (strcmp(featureName, "ConnectLogout") == 0) {
		return isVersionOrNewer(1, 16, 0);
	}

	// Desktop Crossplay (v1.15.0+)
	if (strcmp(featureName, "DesktopCrossplay") == 0) {
		return isVersionOrNewer(1, 15, 0);
	}

	// External Auth Providers - Apple, Google, Oculus, itch.io (v1.14.0+)
	if (strcmp(featureName, "ExternalAuthProviders") == 0) {
		return isVersionOrNewer(1, 14, 0);
	}

	// Hidden Achievements (v1.15.0+)
	if (strcmp(featureName, "HiddenAchievements") == 0) {
		return isVersionOrNewer(1, 15, 0);
	}

	// RTC Options in Platform (v1.14.0+)
	if (strcmp(featureName, "RTCOptions") == 0) {
		return isVersionOrNewer(1, 14, 0);
	}

	// Tick Budget (v1.15.0+)
	if (strcmp(featureName, "TickBudget") == 0) {
		return isVersionOrNewer(1, 15, 0);
	}

	// Integrated Platform (v1.17.0+)
	if (strcmp(featureName, "IntegratedPlatform") == 0) {
		return isVersionOrNewer(1, 17, 0);
	}

	// Task Network Timeout (v1.17.0+)
	if (strcmp(featureName, "TaskNetworkTimeout") == 0) {
		return isVersionOrNewer(1, 17, 0);
	}

	// Localized Presence (v1.18.0+)
	if (strcmp(featureName, "LocalizedPresence") == 0) {
		return isVersionOrNewer(1, 18, 0);
	}

	return false;
}

void logCompatibilityInfo() {
	Logger::info("[COMPAT] ========================================");
	Logger::info("[COMPAT] EOS SDK Compatibility Information");
	Logger::info("[COMPAT] ========================================");
	Logger::info("[COMPAT] ScreamAPI SDK Version: v%d.%d.%d.%d (headers)",
		EOS_MAJOR_VERSION, EOS_MINOR_VERSION, EOS_PATCH_VERSION, EOS_HOTFIX_VERSION);
	Logger::info("[COMPAT] Game SDK Version:      %s", getVersionString());

	if (!gameSDKVersion.detected) {
		Logger::error("[COMPAT] Game SDK version not detected!");
		Logger::info("[COMPAT] ========================================");
		return;
	}

	// --- 32-bit + SDK 1.19+ deprecation warning (official EOS 1.19.0.3) ---
	// This warning only appears in 32-bit builds. 64-bit is fully supported.
#ifdef _M_IX86
	if (isVersionOrNewer(1, 19, 0)) {
		Logger::warn("[COMPAT] This game uses an EOS SDK version (≥1.19.0) that no longer supports 32-bit Windows.");
		Logger::warn("[COMPAT] Official deprecation: https://dev.epicgames.com/docs/epic-online-services/whats-new#2026");
		Logger::warn("[COMPAT] Achievements overlay may not work. Use EOS SDK ≤1.18.1.2 for 32-bit compatibility.");
	}
#endif

	Logger::info("[COMPAT] ");
	Logger::info("[COMPAT] Feature Availability:");
	Logger::info("[COMPAT]   Connect Logout:          %s", isFeatureAvailable("ConnectLogout") ? "YES" : "NO");
	Logger::info("[COMPAT]   Desktop Crossplay:       %s", isFeatureAvailable("DesktopCrossplay") ? "YES" : "NO");
	Logger::info("[COMPAT]   External Auth Providers: %s", isFeatureAvailable("ExternalAuthProviders") ? "YES" : "NO");
	Logger::info("[COMPAT]   Hidden Achievements:     %s", isFeatureAvailable("HiddenAchievements") ? "YES" : "NO");
	Logger::info("[COMPAT]   RTC Options:             %s", isFeatureAvailable("RTCOptions") ? "YES" : "NO");
	Logger::info("[COMPAT]   Tick Budget:             %s", isFeatureAvailable("TickBudget") ? "YES" : "NO");
	Logger::info("[COMPAT]   Integrated Platform:     %s", isFeatureAvailable("IntegratedPlatform") ? "YES" : "NO");
	Logger::info("[COMPAT]   Task Network Timeout:    %s", isFeatureAvailable("TaskNetworkTimeout") ? "YES" : "NO");
	Logger::info("[COMPAT]   Localized Presence:      %s", isFeatureAvailable("LocalizedPresence") ? "YES" : "NO");

	Logger::info("[COMPAT] ");
	Logger::info("[COMPAT] API Versions:");
	Logger::info("[COMPAT]   PlatformOptions:         %d", getApiVersion("PlatformOptions"));
	Logger::info("[COMPAT]   QueryPlayerAchievements: %d", getApiVersion("QueryPlayerAchievements"));
	Logger::info("[COMPAT]   CopyAchievementByIndex:  %d", getApiVersion("CopyPlayerAchievementByIndex"));

	// Compatibility status
	if (isVersionOrNewer(EOS_MAJOR_VERSION, EOS_MINOR_VERSION, EOS_PATCH_VERSION)) {
		Logger::info("[COMPAT] ");
		Logger::info("[COMPAT] Status: COMPATIBLE (Game >= ScreamAPI)");
	}
	else {
		Logger::warn("[COMPAT] ");
		Logger::warn("[COMPAT] Status: PARTIAL (Game < ScreamAPI)");
		Logger::warn("[COMPAT] Game uses older SDK - some ScreamAPI features unavailable");
	}

	Logger::info("[COMPAT] ========================================");
}

} // namespace EOS_Compat