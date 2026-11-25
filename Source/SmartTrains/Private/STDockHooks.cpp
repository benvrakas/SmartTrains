#include "STDockHooks.h"
#include "Buildables/FGBuildableRailroadStation.h"
#include "Patching/NativeHookManager.h"
#include "SmartTrains.h"
#include "STSubsystem.h"
#include "FGLocomotive.h"

void STDockHooks::Install()
{
#if WITH_EDITOR
	UE_LOG(LogSmartTrains, Log, TEXT("Editor build: dock hook not installed"));
#else
	static bool bInstalled = false;
	if (bInstalled)
	{
		return;
	}
	bInstalled = true;

	SUBSCRIBE_METHOD_AFTER(AFGBuildableRailroadStation::StartDocking, [](const bool& bDocked, AFGBuildableRailroadStation* Self, AFGLocomotive* Locomotive, float Offset)
	{
		if (!bDocked || !Self || !Locomotive || !Self->HasAuthority())
		{
			return;
		}
		if (ASTSubsystem* Subsystem = ASTSubsystem::Get(Self))
		{
			Subsystem->NoteDockStarted(Locomotive->GetTrain(), Self->GetStationIdentifier());
		}
	});
	UE_LOG(LogSmartTrains, Log, TEXT("Dock hook installed"));
#endif
}
