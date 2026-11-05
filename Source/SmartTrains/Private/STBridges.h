#pragma once

#include "CoreMinimal.h"
#include "FGRailroadTimeTable.h"

class AFGBuildableRailroadStation;
class AFGRailroadVehicle;
class AFGTrain;
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

// Auto Trains' API (its Docs/API.md), called by reflection so it stays optional. Server game thread only; times are
// seconds. Each call returns false (or -1) when Auto Trains isn't loaded.
namespace STAt
{
	// Returns true if Auto Trains with the API is loaded in World.
	bool IsAvailable(const UWorld* World);

	// The trains Train waits for, the one holding it up first (parked and undriven trains included), how long it has
	// been held up and why. Returns false if Auto Trains doesn't drive Train now or isn't loaded.
	bool GetWaitingFor(const UWorld* World, AFGTrain* Train, TArray<AFGTrain*>& OutWaitingFor, float& OutWaitedSeconds, FString& OutReason);

	// Seconds until the last train docked at Station or heading there is expected to have left; 0 if none, -1 if one
	// has no plan or Auto Trains isn't loaded.
	float GetStationQueue(const UWorld* World, AFGBuildableRailroadStation* Station, TArray<AFGTrain*>& OutTrains);

	// Seconds until Train reaches OutGoal by Auto Trains' plan, around every booking and queue (for a docked train its
	// next stop, the stay included); -1 without such a plan or without Auto Trains.
	float GetPlannedArrival(const UWorld* World, AFGTrain* Train, AFGBuildableRailroadStation*& OutGoal);

	// Seconds from From to To on the shortest way the signals allow, without other traffic; -1 if there is no way or
	// Auto Trains isn't loaded. A search over the whole network: call sparingly.
	float EstimateTravelSeconds(const UWorld* World, AFGTrain* Train, AFGBuildableRailroadStation* From, AFGBuildableRailroadStation* To);

	struct FTroubled
	{
		AFGTrain* Train = nullptr;
		// Deadlock, Stuck, Lost or Stalled.
		FString Kind;
		float Seconds = 0.f;
	};
	// Trains in trouble now; empty if none or Auto Trains isn't loaded.
	TArray<FTroubled> GetTroubledTrains(const UWorld* World);

	// Tells Auto Trains that Train's timetable changed, so it predicts the next leg from the new one.
	void TimeTableChanged(const UWorld* World, AFGTrain* Train);

	enum class ELegCheck : uint8
	{
		// Auto Trains isn't loaded or has no ExternalCheckLeg.
		Unavailable,
		// Being planned; ask again with the same station.
		Pending,
		// The train could go now; OutSeconds until it would arrive.
		Clear,
		// No plan gets it there now; OutReason says what is in the way.
		Blocked,
		// It can't be asked (driving, no path, traffic control off); OutReason says why.
		NotAsked,
	};
	// Asks whether Train, standing in a station (docked, or parked with self-driving off), could drive to To now,
	// planned after every other train. Asynchronous: a first call usually answers Pending.
	ELegCheck CheckLeg(const UWorld* World, AFGTrain* Train, AFGBuildableRailroadStation* To, float& OutSeconds, FString& OutReason);
}
