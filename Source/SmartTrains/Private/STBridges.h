#pragma once

#include "CoreMinimal.h"
#include "FGRailroadTimeTable.h"

class AFGRailroadVehicle;
class AFGTrainStationIdentifier;
class UWorld;

// Freight Car Control, called by reflection so it stays optional (Docs/Architecture.md, "Freight Car Control").
namespace STFcc
{
	// Returns true if Freight Car Control with the API SmartTrains needs is loaded in World.
	bool IsAvailable(const UWorld* World);

	// Replaces TimeTable's stops and, in the same step, makes the cars in SkipVehicles skip the stops at the matching
	// SkipStops indices. A non-empty Owner locks the rules in Freight Car Control's menus; an empty one unlocks them
	// and puts back the player's own rules if they still line up with Stops. Returns false if the call failed.
	bool SetStops(const UWorld* World, AFGRailroadTimeTable* TimeTable, const TArray<FTimeTableStop>& Stops, const TArray<int32>& SkipStops,
		const TArray<AFGRailroadVehicle*>& SkipVehicles, const FString& Owner);
}

// DynamicTimeTable station groups, read by reflection (Docs/Migration.md, "DynamicTimeTable").
namespace STDtt
{
	struct FGroup
	{
		FString Name;
		TArray<AFGTrainStationIdentifier*> Stations;
		TArray<int32> TargetSlots;
	};

	// Returns true if DynamicTimeTable is loaded in World.
	bool IsInstalled(const UWorld* World);

	// Reads every group; returns false with OutError set if DynamicTimeTable is missing or its data couldn't be read.
	bool ReadGroups(const UWorld* World, TArray<FGroup>& OutGroups, FString& OutError);
}
