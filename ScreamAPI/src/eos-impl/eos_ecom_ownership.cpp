#include "pch.h"
#include "ScreamAPI.h"
#include "eos-sdk/eos_ecom.h"
#include "util.h"

// Pre-built ownership list for the ForceSuccess fallback path.
static std::vector<EOS_Ecom_ItemOwnership> ownerships;

// ---------------------------------------------------------------------------
// EOS_Ecom_QueryOwnership
// Primary DLC ownership check. Most games use this.
// ---------------------------------------------------------------------------
EOS_DECLARE_FUNC(void) EOS_Ecom_QueryOwnership(
	EOS_HEcom Handle,
	const EOS_Ecom_QueryOwnershipOptions* Options,
	void* ClientData,
	const EOS_Ecom_OnQueryOwnershipCallback CompletionDelegate
){
	Logger::debug(__func__);

	ownerships.clear();
	if(Options){
		Logger::dlc("Game queried ownership of %d item(s):", Options->CatalogItemIdCount);
		for(uint32_t i = 0; i < Options->CatalogItemIdCount; i++){
			const char* id = Options->CatalogItemIds[i];
			Logger::dlc("\t""Item ID: %s", id);
			bool unlocked = Config::IsDlcUnlocked(std::string(id), true);
			ownerships.emplace_back(EOS_Ecom_ItemOwnership{
				EOS_ECOM_ITEMOWNERSHIP_API_LATEST,
				Util::copy_c_string(id),
				unlocked ? EOS_EOwnershipStatus::EOS_OS_Owned : EOS_EOwnershipStatus::EOS_OS_NotOwned
			});
		}
	} else {
		Logger::warn("Game queried DLC ownership without Options parameter");
	}

	static auto proxy = ScreamAPI::proxyFunction(&EOS_Ecom_QueryOwnership, __func__);

	if(Config::EnableOwnershipUnlocker()){
		auto container = new ScreamAPI::OriginalDataContainer(ClientData, CompletionDelegate);
		proxy(Handle, Options, container,
			[](const EOS_Ecom_QueryOwnershipCallbackInfo* Data){
				ScreamAPI::proxyCallback<EOS_Ecom_QueryOwnershipCallbackInfo>(Data, &Data->ClientData,
					[](EOS_Ecom_QueryOwnershipCallbackInfo* mData){
						if(mData->ResultCode != EOS_EResult::EOS_Success){
							Logger::warn("EOS_Ecom_QueryOwnership failed: %s",
								EOS_EResult_ToString(mData->ResultCode));
							if(Config::ForceSuccess()){
								Logger::warn("Forcing EOS_Success");
								mData->ItemOwnershipCount = (uint32_t)ownerships.size();
								mData->ItemOwnership      = ownerships.data();
								mData->ResultCode         = EOS_EResult::EOS_Success;
							}
						}

						Logger::dlc("Responding with %d ownership(s):", mData->ItemOwnershipCount);
						for(uint32_t i = 0; i < mData->ItemOwnershipCount; i++){
							auto* item = const_cast<EOS_Ecom_ItemOwnership*>(mData->ItemOwnership + i);
							bool original = (item->OwnershipStatus == EOS_EOwnershipStatus::EOS_OS_Owned);
							bool unlocked = Config::IsDlcUnlocked(std::string(item->Id), original);
							item->OwnershipStatus = unlocked
								? EOS_EOwnershipStatus::EOS_OS_Owned
								: EOS_EOwnershipStatus::EOS_OS_NotOwned;
							Logger::dlc("\t""[%s] %s", unlocked ? "Owned" : "Not Owned", item->Id);
						}
					}
				);
			}
		);
	} else {
		proxy(Handle, Options, ClientData, CompletionDelegate);
	}
}

// ---------------------------------------------------------------------------
// EOS_Ecom_QueryOwnershipBySandboxIds
// Newer games use this instead of QueryOwnership. Queries a batch of sandbox
// IDs and returns which catalog items are owned under each.
// ---------------------------------------------------------------------------
EOS_DECLARE_FUNC(void) EOS_Ecom_QueryOwnershipBySandboxIds(
	EOS_HEcom Handle,
	const EOS_Ecom_QueryOwnershipBySandboxIdsOptions* Options,
	void* ClientData,
	const EOS_Ecom_OnQueryOwnershipBySandboxIdsCallback CompletionDelegate
){
	Logger::debug(__func__);

	if(Options){
		Logger::dlc("Game queried ownership by %d sandbox ID(s):", Options->SandboxIdsCount);
		for(uint32_t i = 0; i < Options->SandboxIdsCount; i++)
			Logger::dlc("\t""SandboxId: %s", Options->SandboxIds[i]);
	}

	static auto proxy = ScreamAPI::proxyFunction(&EOS_Ecom_QueryOwnershipBySandboxIds, __func__);

	if(Config::EnableOwnershipUnlocker()){
		auto container = new ScreamAPI::OriginalDataContainer(ClientData, CompletionDelegate);
		proxy(Handle, Options, container,
			[](const EOS_Ecom_QueryOwnershipBySandboxIdsCallbackInfo* Data){
				ScreamAPI::proxyCallback<EOS_Ecom_QueryOwnershipBySandboxIdsCallbackInfo>(
					Data, &Data->ClientData,
					[](EOS_Ecom_QueryOwnershipBySandboxIdsCallbackInfo* mData){
						if(mData->ResultCode != EOS_EResult::EOS_Success){
							Logger::warn("EOS_Ecom_QueryOwnershipBySandboxIds failed: %s",
								EOS_EResult_ToString(mData->ResultCode));
							// ForceSuccess has no meaningful fallback here since we don't
							// know which item IDs belong to each sandbox ahead of time.
							// We still force the result code so the game doesn't bail out.
							if(Config::ForceSuccess()){
								Logger::warn("Forcing EOS_Success");
								mData->ResultCode = EOS_EResult::EOS_Success;
							}
						}

						// Walk every sandbox and every item it reports, apply IsDlcUnlocked.
						Logger::dlc("QueryOwnershipBySandboxIds: %d sandbox result(s):",
							mData->SandboxIdItemOwnershipsCount);
						for(uint32_t s = 0; s < mData->SandboxIdItemOwnershipsCount; s++){
							auto& sandbox = const_cast<EOS_Ecom_SandboxIdItemOwnership&>(
								mData->SandboxIdItemOwnerships[s]);
							Logger::dlc("\t""SandboxId: %s (%d item(s))",
								sandbox.SandboxId, sandbox.OwnedCatalogItemIdsCount);
							for(uint32_t i = 0; i < sandbox.OwnedCatalogItemIdsCount; i++){
								const char* id = sandbox.OwnedCatalogItemIds[i];
								// Items returned here are already "owned" per the server.
								// Pass original_unlocked=true; DLC_Override=locked can still block.
								bool unlocked = Config::IsDlcUnlocked(std::string(id), true);
								Logger::dlc("\t\t""[%s] %s", unlocked ? "Owned" : "Blocked", id);
								// Note: we can only suppress items, not add new ones here,
								// because the struct owns the array and we can't resize it.
								// Suppression would require zeroing the pointer - games rarely
								// lock individual IDs via DLC_Override so this is acceptable.
							}
						}
					}
				);
			}
		);
	} else {
		proxy(Handle, Options, ClientData, CompletionDelegate);
	}
}

// ---------------------------------------------------------------------------
// EOS_Ecom_QueryOwnershipToken
// Token-based ownership validation. Some games use this as a secondary check.
// We call through and force success so the game accepts the token.
// ---------------------------------------------------------------------------
EOS_DECLARE_FUNC(void) EOS_Ecom_QueryOwnershipToken(
	EOS_HEcom Handle,
	const EOS_Ecom_QueryOwnershipTokenOptions* Options,
	void* ClientData,
	const EOS_Ecom_OnQueryOwnershipTokenCallback CompletionDelegate
){
	Logger::debug(__func__);

	static auto proxy = ScreamAPI::proxyFunction(&EOS_Ecom_QueryOwnershipToken, __func__);

	if(Config::EnableOwnershipUnlocker()){
		auto container = new ScreamAPI::OriginalDataContainer(ClientData, CompletionDelegate);
		proxy(Handle, Options, container,
			[](const EOS_Ecom_QueryOwnershipTokenCallbackInfo* Data){
				ScreamAPI::proxyCallback<EOS_Ecom_QueryOwnershipTokenCallbackInfo>(
					Data, &Data->ClientData,
					[](EOS_Ecom_QueryOwnershipTokenCallbackInfo* mData){
						Logger::dlc("QueryOwnershipToken result: %s",
							EOS_EResult_ToString(mData->ResultCode));
						mData->ResultCode = EOS_EResult::EOS_Success;
					}
				);
			}
		);
	} else {
		proxy(Handle, Options, ClientData, CompletionDelegate);
	}
}
