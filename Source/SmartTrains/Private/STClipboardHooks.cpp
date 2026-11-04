#include "STClipboardHooks.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGPlayerController.h"
#include "FGPlayerState.h"
#include "Patching/NativeHookManager.h"
#include "SmartTrains.h"
#include "STLayout.h"
#include "STRemoteCallObject.h"

namespace
{
	// Returns the local player's remote call object if Self belongs to the local player, else nullptr.
	USTRemoteCallObject* GetLocalRCO(const AFGPlayerState* Self)
	{
		AFGPlayerController* Controller = Self ? Cast<AFGPlayerController>(Self->GetPlayerController()) : nullptr;
		if (!Controller || !Controller->IsLocalController())
		{
			return nullptr;
		}
		return Controller->GetRemoteCallObjectOfClass<USTRemoteCallObject>();
	}
}

void STClipboardHooks::Install()
{
#if WITH_EDITOR
	UE_LOG(LogSmartTrains, Log, TEXT("Editor build: clipboard hooks not installed"));
#else
	static bool bInstalled = false;
	if (bInstalled)
	{
		return;
	}
	bInstalled = true;

	SUBSCRIBE_METHOD_AFTER(AFGPlayerState::CopyFactoryClipboard, [](AFGPlayerState* Self, UObject* Object)
	{
		AFGBuildableTrainPlatformCargo* Platform = Cast<AFGBuildableTrainPlatformCargo>(Object);
		if (!Platform)
		{
			return;
		}
		if (USTRemoteCallObject* RCO = GetLocalRCO(Self))
		{
			UE_LOG(LogSmartTrains, Log, TEXT("Game clipboard: copy from %s"), *Platform->GetName());
			RCO->Server_CopyPlatformSettings(Platform, true);
		}
	});

	SUBSCRIBE_METHOD_AFTER(AFGPlayerState::PasteFactoryClipboard, [](AFGPlayerState* Self, UObject* Object)
	{
		AFGBuildableTrainPlatformCargo* Platform = Cast<AFGBuildableTrainPlatformCargo>(Object);
		if (!Platform)
		{
			return;
		}
		if (USTRemoteCallObject* RCO = GetLocalRCO(Self); RCO && RCO->HasClipboard(STLayout::GetPlatformCargoType(Platform) == EFreightCargoType::FCT_Liquid))
		{
			const int32 Edit = RCO->NextEdit();
			UE_LOG(LogSmartTrains, Log, TEXT("Game clipboard: paste onto %s (edit %d)"), *Platform->GetName(), Edit);
			RCO->Server_PastePlatformSettings(Platform, true, Edit);
		}
	});
	UE_LOG(LogSmartTrains, Log, TEXT("Clipboard hooks installed"));
#endif
}
