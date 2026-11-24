#include "STDockHooks.h"
#include "Buildables/FGBuildableRailroadStation.h"
#include "Patching/NativeHookManager.h"
#include "SmartTrains.h"
#include "STSubsystem.h"
#include "FGLocomotive.h"
#include "FGTrain.h"

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
	// A drive to park ends ready to dock in the station, not docked: docking and leaving again would leave it standing
	// there unable to dock, so a trip starting there could not (Docs/Architecture.md, "Parking ready to dock").
	SUBSCRIBE_METHOD(AFGTrain::Dock, [](auto& Scope, AFGTrain* Self)
	{
		if (!Self || !Self->HasAuthority())
		{
			return;
		}
		ASTSubsystem* Subsystem = ASTSubsystem::Get(Self);
		if (Subsystem && Subsystem->KeepFromDocking(Self))
		{
			Scope.Cancel();
		}
	});
	UE_LOG(LogSmartTrains, Log, TEXT("Dock hook installed"));
#endif
}
