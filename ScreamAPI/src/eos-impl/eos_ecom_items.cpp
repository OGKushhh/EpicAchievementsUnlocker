#include "pch.h"
#include "ScreamAPI.h"
#include "eos-sdk/eos_ecom.h"

// ---------------------------------------------------------------------------
// EOS_Ecom_GetItemReleaseCount
// Returns the number of releases for a given catalog item.
// Games rarely depend on this for DLC unlocking — stub it to return 0
// so the game doesn't try to iterate releases and get garbage data.
// ---------------------------------------------------------------------------
EOS_DECLARE_FUNC(uint32_t) EOS_Ecom_GetItemReleaseCount(
	EOS_HEcom Handle,
	const EOS_Ecom_GetItemReleaseCountOptions* Options
){
	Logger::debug(__func__);
	Logger::dlc("GetItemReleaseCount for item: %s -> returning 0", Options->ItemId);
	return 0;
}
