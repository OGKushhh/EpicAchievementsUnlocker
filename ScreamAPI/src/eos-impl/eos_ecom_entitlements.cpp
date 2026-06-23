#include "pch.h"
#include "ScreamAPI.h"
#include "eos-sdk/eos_ecom.h"
#include "dlc_catalog.h"
#include "util.h"
#include <map>
#include <mutex>
#include <vector>

namespace {

// id -> title. Rebuilt on every QueryEntitlements call.
std::map<std::string, std::string> entitlement_map;

// Ordered ID list for index-based lookups.
std::vector<std::string> entitlement_ids;

// Catalog cache — fetched once per session.
// Protected by s_cache_mutex because PipeServer reads it from a different thread.
std::mutex s_cache_mutex;
std::map<std::string, std::string> s_catalog_cache;
bool s_catalog_fetched = false;

void auto_fetch_entitlements(){
    // Prefer the runtime-captured SandboxId. Fall back to the config override.
    std::string ns = Util::g_namespace_id;
    if(ns.empty()){
        ns = Config::NamespaceId();
        if(!ns.empty())
            Logger::debug("DLC auto-fetch: using NamespaceId from config: %s", ns.c_str());
    }
    if(ns.empty()){
        Logger::warn("DLC auto-fetch: namespace_id unavailable. "
            "Set NamespaceId= in [ScreamAPI] if ScreamAPI loads after EOS_Platform_Create.");
        return;
    }

    if(!s_catalog_fetched){
        s_catalog_fetched = true;
        auto result = DlcCatalog::fetch(ns);
        if(result.has_value()){
            std::lock_guard<std::mutex> lk(s_cache_mutex);
            s_catalog_cache = std::move(*result);
            Logger::dlc("Auto-fetch: cached %zu entries", s_catalog_cache.size());
        } else {
            Logger::warn("Auto-fetch: failed to retrieve catalog from Epic's API");
        }
    }

    // original_unlocked=false: auto-fetched items are not originally owned,
    // so DLC_Override=original means skip them.
    std::lock_guard<std::mutex> lk(s_cache_mutex);
    for(auto& [id, title] : s_catalog_cache){
        if(Config::IsDlcUnlocked(id, false)){
            Logger::debug("  Auto-fetch adding: %s - \"%s\"", id.c_str(), title.c_str());
            entitlement_map[id] = title;
        }
    }
}

void inject_extra_entitlements(){
    for(auto& [id, title] : Config::ExtraEntitlements()){
        if(Config::IsDlcUnlocked(id, true)){
            Logger::debug("  Config adding: %s - \"%s\"", id.c_str(), title.c_str());
            entitlement_map[id] = title;
        }
    }
}

// Builds a fake EOS_Ecom_Entitlement* from our map. Caller must delete it.
// char* pointers point into entitlement_map/entitlement_ids — do NOT free them.
EOS_Ecom_Entitlement* make_entitlement(const std::string& id, const std::string& title){
    auto* e = new EOS_Ecom_Entitlement{};
    e->ApiVersion    = EOS_ECOM_ENTITLEMENT_API_LATEST;
    e->EntitlementId   = id.c_str();
    e->CatalogItemId   = id.c_str();
    e->EntitlementName = title.c_str();
    e->bRedeemed   = false;
    e->EndTimestamp = -1;
    e->ServerIndex  = -1;
    return e;
}

} // namespace

// ---------------------------------------------------------------------------
// EOS_Ecom_QueryEntitlements
// ---------------------------------------------------------------------------
EOS_DECLARE_FUNC(void) EOS_Ecom_QueryEntitlements(
	EOS_HEcom Handle,
	const EOS_Ecom_QueryEntitlementsOptions* Options,
	void* ClientData,
	const EOS_Ecom_OnQueryEntitlementsCallback CompletionDelegate
){
	Logger::debug(__func__);

	if(!Config::EnableEntitlementUnlocker()){
		static auto proxy = ScreamAPI::proxyFunction(&EOS_Ecom_QueryEntitlements, __func__);
		proxy(Handle, Options, ClientData, CompletionDelegate);
		return;
	}

	entitlement_map.clear();
	entitlement_ids.clear();

	Logger::dlc("Game queried %d entitlement(s):", Options->EntitlementNameCount);
	for(uint32_t i = 0; i < Options->EntitlementNameCount; i++){
		const char* id = Options->EntitlementNames[i];
		Logger::dlc("  %s", id);
		// original_unlocked=true: game is explicitly querying these
		if(Config::IsDlcUnlocked(std::string(id), true))
			entitlement_map[id] = "Unknown Title";
	}

	auto container = new ScreamAPI::OriginalDataContainer(ClientData, CompletionDelegate);
	static auto proxy = ScreamAPI::proxyFunction(&EOS_Ecom_QueryEntitlements, __func__);
	proxy(Handle, Options, container,
		[](const EOS_Ecom_QueryEntitlementsCallbackInfo* Data){
			ScreamAPI::proxyCallback<EOS_Ecom_QueryEntitlementsCallbackInfo>(
				Data, &Data->ClientData,
				[](EOS_Ecom_QueryEntitlementsCallbackInfo* mData){
					try {
						auto_fetch_entitlements();
						inject_extra_entitlements();

						entitlement_ids.clear();
						for(auto& [id, title] : entitlement_map)
							entitlement_ids.push_back(id);

						Logger::dlc("Responding with %zu entitlement(s):", entitlement_map.size());
						for(auto& [id, title] : entitlement_map)
							Logger::dlc("  %s = \"%s\"", id.c_str(), title.c_str());

						mData->ResultCode = EOS_EResult::EOS_Success;
					} catch(const std::exception& e){
						Logger::error("QueryEntitlements callback error: %s", e.what());
					}
				}
			);
		}
	);
}

// ---------------------------------------------------------------------------
// EOS_Ecom_GetEntitlementsCount
// ---------------------------------------------------------------------------
EOS_DECLARE_FUNC(uint32_t) EOS_Ecom_GetEntitlementsCount(
	EOS_HEcom Handle,
	const EOS_Ecom_GetEntitlementsCountOptions* Options
){
	Logger::debug(__func__);
	if(!Config::EnableEntitlementUnlocker()){
		static auto proxy = ScreamAPI::proxyFunction(&EOS_Ecom_GetEntitlementsCount, __func__);
		return proxy(Handle, Options);
	}
	const auto count = (uint32_t)entitlement_map.size();
	Logger::dlc("GetEntitlementsCount: %u", count);
	return count;
}

// ---------------------------------------------------------------------------
// EOS_Ecom_GetEntitlementsByNameCount
// Returns how many entitlements we have matching a given name (id).
// ---------------------------------------------------------------------------
EOS_DECLARE_FUNC(uint32_t) EOS_Ecom_GetEntitlementsByNameCount(
	EOS_HEcom Handle,
	const EOS_Ecom_GetEntitlementsByNameCountOptions* Options
){
	Logger::debug(__func__);
	if(!Config::EnableEntitlementUnlocker()){
		static auto proxy = ScreamAPI::proxyFunction(&EOS_Ecom_GetEntitlementsByNameCount, __func__);
		return proxy(Handle, Options);
	}
	// In our model, each entitlement name appears at most once.
	const char* name = Options->EntitlementName;
	uint32_t count = entitlement_map.count(std::string(name)) ? 1u : 0u;
	Logger::dlc("GetEntitlementsByNameCount '%s': %u", name, count);
	return count;
}

// ---------------------------------------------------------------------------
// EOS_Ecom_CopyEntitlementByIndex
// ---------------------------------------------------------------------------
EOS_DECLARE_FUNC(EOS_EResult) EOS_Ecom_CopyEntitlementByIndex(
	EOS_HEcom Handle,
	const EOS_Ecom_CopyEntitlementByIndexOptions* Options,
	EOS_Ecom_Entitlement** OutEntitlement
){
	Logger::debug(__func__);
	if(!Config::EnableEntitlementUnlocker()){
		static auto proxy = ScreamAPI::proxyFunction(&EOS_Ecom_CopyEntitlementByIndex, __func__);
		return proxy(Handle, Options, OutEntitlement);
	}
	const auto index = Options->EntitlementIndex;
	if(index >= entitlement_ids.size()){
		Logger::warn("CopyEntitlementByIndex: index %u out of bounds (%zu)", index, entitlement_ids.size());
		return EOS_EResult::EOS_NotFound;
	}
	const auto& id    = entitlement_ids[index];
	const auto& title = entitlement_map.at(id);
	Logger::dlc("CopyEntitlementByIndex[%u]: %s", index, id.c_str());
	*OutEntitlement = make_entitlement(id, title);
	return EOS_EResult::EOS_Success;
}

// ---------------------------------------------------------------------------
// EOS_Ecom_CopyEntitlementByNameAndIndex
// Games that use named queries call this instead of ByIndex.
// ---------------------------------------------------------------------------
EOS_DECLARE_FUNC(EOS_EResult) EOS_Ecom_CopyEntitlementByNameAndIndex(
	EOS_HEcom Handle,
	const EOS_Ecom_CopyEntitlementByNameAndIndexOptions* Options,
	EOS_Ecom_Entitlement** OutEntitlement
){
	Logger::debug(__func__);
	if(!Config::EnableEntitlementUnlocker()){
		static auto proxy = ScreamAPI::proxyFunction(&EOS_Ecom_CopyEntitlementByNameAndIndex, __func__);
		return proxy(Handle, Options, OutEntitlement);
	}
	const std::string name = Options->EntitlementName;
	auto it = entitlement_map.find(name);
	if(it == entitlement_map.end() || Options->Index > 0){
		Logger::warn("CopyEntitlementByNameAndIndex: '%s'[%u] not found", name.c_str(), Options->Index);
		return EOS_EResult::EOS_NotFound;
	}
	Logger::dlc("CopyEntitlementByNameAndIndex: %s", name.c_str());
	*OutEntitlement = make_entitlement(it->first, it->second);
	return EOS_EResult::EOS_Success;
}

// ---------------------------------------------------------------------------
// EOS_Ecom_CopyEntitlementById
// Direct ID lookup - some games use this after getting the ID from the server.
// ---------------------------------------------------------------------------
EOS_DECLARE_FUNC(EOS_EResult) EOS_Ecom_CopyEntitlementById(
	EOS_HEcom Handle,
	const EOS_Ecom_CopyEntitlementByIdOptions* Options,
	EOS_Ecom_Entitlement** OutEntitlement
){
	Logger::debug(__func__);
	if(!Config::EnableEntitlementUnlocker()){
		static auto proxy = ScreamAPI::proxyFunction(&EOS_Ecom_CopyEntitlementById, __func__);
		return proxy(Handle, Options, OutEntitlement);
	}
	const std::string id = Options->EntitlementId;
	auto it = entitlement_map.find(id);
	if(it == entitlement_map.end()){
		Logger::warn("CopyEntitlementById: '%s' not found", id.c_str());
		return EOS_EResult::EOS_NotFound;
	}
	Logger::dlc("CopyEntitlementById: %s", id.c_str());
	*OutEntitlement = make_entitlement(it->first, it->second);
	return EOS_EResult::EOS_Success;
}

// ---------------------------------------------------------------------------
// EOS_Ecom_Entitlement_Release
// ---------------------------------------------------------------------------
EOS_DECLARE_FUNC(void) EOS_Ecom_Entitlement_Release(EOS_Ecom_Entitlement* Entitlement){
	if(!Config::EnableEntitlementUnlocker()){
		static auto proxy = ScreamAPI::proxyFunction(&EOS_Ecom_Entitlement_Release, __func__);
		proxy(Entitlement);
		return;
	}
	if(Entitlement){
		Logger::debug("Releasing entitlement: %s", Entitlement->EntitlementId);
		// char* pointers inside belong to entitlement_map/entitlement_ids — do NOT free them.
		delete Entitlement;
	} else {
		Logger::warn("Game attempted to release a null entitlement");
	}
}

// ---------------------------------------------------------------------------
// EOS_Ecom_QueryEntitlementToken
// Token-based entitlement check. Stub — force success.
// ---------------------------------------------------------------------------
EOS_DECLARE_FUNC(void) EOS_Ecom_QueryEntitlementToken(
	EOS_HEcom Handle,
	const EOS_Ecom_QueryEntitlementTokenOptions* Options,
	void* ClientData,
	const EOS_Ecom_OnQueryEntitlementTokenCallback CompletionDelegate
){
	Logger::debug(__func__);
	static auto proxy = ScreamAPI::proxyFunction(&EOS_Ecom_QueryEntitlementToken, __func__);
	auto container = new ScreamAPI::OriginalDataContainer(ClientData, CompletionDelegate);
	proxy(Handle, Options, container,
		[](const EOS_Ecom_QueryEntitlementTokenCallbackInfo* Data){
			ScreamAPI::proxyCallback<EOS_Ecom_QueryEntitlementTokenCallbackInfo>(
				Data, &Data->ClientData,
				[](EOS_Ecom_QueryEntitlementTokenCallbackInfo* mData){
					Logger::dlc("QueryEntitlementToken result: %s — forcing success",
						EOS_EResult_ToString(mData->ResultCode));
					mData->ResultCode = EOS_EResult::EOS_Success;
				}
			);
		}
	);
}

// ---------------------------------------------------------------------------
// EOS_Ecom_RedeemEntitlements
// Games call this to "consume" entitlements after purchase. Stub — force success.
// ---------------------------------------------------------------------------
EOS_DECLARE_FUNC(void) EOS_Ecom_RedeemEntitlements(
	EOS_HEcom Handle,
	const EOS_Ecom_RedeemEntitlementsOptions* Options,
	void* ClientData,
	const EOS_Ecom_OnRedeemEntitlementsCallback CompletionDelegate
){
	Logger::debug(__func__);
	static auto proxy = ScreamAPI::proxyFunction(&EOS_Ecom_RedeemEntitlements, __func__);
	auto container = new ScreamAPI::OriginalDataContainer(ClientData, CompletionDelegate);
	proxy(Handle, Options, container,
		[](const EOS_Ecom_RedeemEntitlementsCallbackInfo* Data){
			ScreamAPI::proxyCallback<EOS_Ecom_RedeemEntitlementsCallbackInfo>(
				Data, &Data->ClientData,
				[](EOS_Ecom_RedeemEntitlementsCallbackInfo* mData){
					Logger::dlc("RedeemEntitlements result: %s — forcing success",
						EOS_EResult_ToString(mData->ResultCode));
					mData->ResultCode = EOS_EResult::EOS_Success;
				}
			);
		}
	);
}

// ── Catalog snapshot for PipeServer ──────────────────────────────────────────
// Called from PipeServer thread on GUI connect; mutex guards the cache.
// Returns a copy so the caller doesn't need to hold the lock.
// Called from eos_hooks.cpp (QueryOwnership path) so the catalog is populated
// regardless of which EOS ecom function the game happens to call first.
void EnsureCatalogFetched() {
    if (s_catalog_fetched) return;

    std::string ns = Util::g_namespace_id;
    if (ns.empty()) {
        ns = Config::NamespaceId();
        if (!ns.empty())
            Logger::debug("EnsureCatalogFetched: using NamespaceId from config: %s", ns.c_str());
    }
    if (ns.empty()) {
        Logger::warn("EnsureCatalogFetched: namespace_id unavailable.");
        return;
    }

    s_catalog_fetched = true;
    auto result = DlcCatalog::fetch(ns);
    if (result.has_value()) {
        std::lock_guard<std::mutex> lk(s_cache_mutex);
        s_catalog_cache = std::move(*result);
        Logger::dlc("EnsureCatalogFetched: cached %zu entries", s_catalog_cache.size());
    } else {
        Logger::warn("EnsureCatalogFetched: failed to retrieve catalog from Epic's API");
    }
}

std::map<std::string, std::string> GetCatalogSnapshot() {
    std::lock_guard<std::mutex> lk(s_cache_mutex);
    return s_catalog_cache;
}
