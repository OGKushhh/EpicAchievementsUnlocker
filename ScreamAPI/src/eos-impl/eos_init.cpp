#include "pch.h"
#include "eos-sdk/eos_init.h"
#include "eos-sdk/eos_types.h"
#include "ScreamAPI.h"
#include "achievement_manager.h"
#include "util.h"

EOS_DECLARE_FUNC(EOS_HPlatform) EOS_Platform_Create(const EOS_Platform_Options* Options){
	Logger::info("EOS_Platform_Create called - setting hPlatform");
	Logger::debug("EOS_Platform_Create: Options pointer: %p", Options);
	if(Options != nullptr) {
		Logger::debug("EOS_Platform_Create: Options->ApiVersion: %d", Options->ApiVersion);
		Logger::debug("EOS_Platform_Create: Options->Flags: %llu", Options->Flags);

		// Capture SandboxId as namespace_id for DLC catalog auto-fetch.
		if(Options->SandboxId && Options->SandboxId[0] != '\0'){
			Util::g_namespace_id = Options->SandboxId;
			Logger::info("Captured namespace_id: %s", Options->SandboxId);
			Logger::info("DLC database: https://scream-db.web.app/games/%s", Options->SandboxId);
		} else {
			Logger::warn("EOS_Platform_Create: SandboxId is empty - DLC auto-fetch may need NamespaceId set in config");
		}
	}

	if(Config::ForceEpicOverlay()){
		Logger::debug("[EOS_Platform_Options]");
		Logger::debug("\t""ApiVersion: %d", Options->ApiVersion);
		Logger::debug("\t""Flags: %llu", Options->Flags);
		auto mOptions = const_cast<EOS_Platform_Options*>(Options);
		mOptions->Flags = 0;
	}

	static auto proxy = ScreamAPI::proxyFunction(&EOS_Platform_Create, __func__);
	auto result = proxy(Options);

	Util::hPlatform = result;
	Logger::info("EOS_Platform_Create result: %p", result);

	if(result == nullptr){
		Logger::error("EOS_Platform_Create returned NULL - initialization will fail!");
		return result;
	}

	Logger::info("EOS Platform successfully created by game - initializing achievement manager");

	if(ScreamAPI::originalDLL != nullptr){
		Logger::debug("EOS_Platform_Create: Calling SetApplicationStatus and SetNetworkStatus (v1.15+ mandatory)");

		typedef void (*EOS_Platform_SetApplicationStatus_t)(EOS_HPlatform, EOS_EApplicationStatus);
		auto SetAppStatusFunc = (EOS_Platform_SetApplicationStatus_t)GetProcAddress(
			(HMODULE)ScreamAPI::originalDLL, "EOS_Platform_SetApplicationStatus");
		if(SetAppStatusFunc){
			try { SetAppStatusFunc(result, EOS_EApplicationStatus::EOS_AS_BackgroundConstrained); }
			catch(...){ Logger::warn("Exception calling EOS_Platform_SetApplicationStatus"); }
		} else {
			Logger::debug("EOS_Platform_SetApplicationStatus not found (older SDK version)");
		}

		typedef void (*EOS_Platform_SetNetworkStatus_t)(EOS_HPlatform, EOS_ENetworkStatus);
		auto SetNetStatusFunc = (EOS_Platform_SetNetworkStatus_t)GetProcAddress(
			(HMODULE)ScreamAPI::originalDLL, "EOS_Platform_SetNetworkStatus");
		if(SetNetStatusFunc){
			try { SetNetStatusFunc(result, EOS_ENetworkStatus::EOS_NS_Online); }
			catch(...){ Logger::warn("Exception calling EOS_Platform_SetNetworkStatus"); }
		} else {
			Logger::debug("EOS_Platform_SetNetworkStatus not found (older SDK version)");
		}
	}

	if(Config::EnableOverlay()){
		std::thread([](){
			Sleep(500);
			Logger::info("[INIT] Platform detected - triggering achievement manager initialization");
			AchievementManager::init();
		}).detach();
	}

	return result;
}
