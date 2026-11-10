#pragma once

#include "CoreMinimal.h"
#include "FGRailroadTimeTable.h"
#include "Templates/SubclassOf.h"
#include "STTypes.generated.h"

class AFGBuildableTrainPlatformCargo;
class AFGRailroadVehicle;
class AFGFreightWagon;
class AFGTrain;
class AFGTrainStationIdentifier;
class AFGLocomotive;
class UFGItemDescriptor;

// What a freight or fluid platform does for SmartTrains.
UENUM()
enum class ESTRole : uint8
{
	// Vanilla: the platform keeps its own load/unload setting and SmartTrains ignores it.
	Off,
	// The platform's items are picked up by managed trains.
	Supply,
	// Managed trains deliver the platform's items.
	Demand,
	// Managed trains unload cargo here that no consumer takes; any item.
	Sink,
};

// How urgent a platform item is.
UENUM()
enum class ESTPriority : uint8
{
	Low,
	Normal,
	High,
	// Sent before anything else, even as a part load.
	Critical,
};

// What a managed train is doing.
UENUM()
enum class ESTTrainPhase : uint8
{
	// Waiting for a job; self-driving is left as it is.
	Idle,
	// Following a job's legs.
	Working,
	// Standing still with self-driving switched off by SmartTrains until it gets a job.
	Parked,
};

namespace STNames
{
	SMARTTRAINS_API const TCHAR* Role(ESTRole Role);
	// True for roles whose platforms managed trains unload at (Demand, Sink).
	SMARTTRAINS_API bool Unloads(ESTRole Role);
	SMARTTRAINS_API const TCHAR* Priority(ESTPriority Priority);
	SMARTTRAINS_API const TCHAR* Phase(ESTTrainPhase Phase);
	SMARTTRAINS_API TOptional<ESTRole> ParseRole(const FString& Text);
	SMARTTRAINS_API TOptional<ESTPriority> ParsePriority(const FString& Text);
	// Returns the score multiplier of Priority.
	SMARTTRAINS_API float PriorityWeight(ESTPriority Priority);
	// Send-at share of a load for a rule's SendAtPercent (0: the default). [%]
	constexpr float DefaultSendAtPercent = 50.f;
	inline float SendAtPercent(float Percent) { return Percent > 0.f ? Percent : DefaultSendAtPercent; }
	// Returns the item's display name, or "any item" for null.
	SMARTTRAINS_API FString Item(TSubclassOf<UFGItemDescriptor> Item);
	// Returns true for liquids and gases.
	SMARTTRAINS_API bool IsFluid(TSubclassOf<UFGItemDescriptor> Item);
	// Formats an amount in the item's unit: items, or m³ for fluids (inventories count fluids in litres).
	SMARTTRAINS_API FString Amount(TSubclassOf<UFGItemDescriptor> Item, double Amount);
}

// One item a platform supplies or demands. A null Item on a Demand platform accepts any item (a sink).
USTRUCT()
struct SMARTTRAINS_API FSTItemRule
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	TSubclassOf<UFGItemDescriptor> Item;

	// Supply: a train is sent once the station holds this share of the train's load; 0 means the default
	// (STNames::DefaultSendAtPercent). [%]
	UPROPERTY(SaveGame)
	float SendAtPercent = 0.f;

	// Demand: the platform is kept stocked at this share of its capacity. [%]
	UPROPERTY(SaveGame)
	float TargetPercent = 50.f;

	UPROPERTY(SaveGame)
	ESTPriority Priority = ESTPriority::Normal;

	// Rate used instead of the measured one when above 0, in the item's inventory unit per minute.
	UPROPERTY(SaveGame)
	float ManualRatePerMinute = 0.f;
};

namespace STNames
{
	// Returns a one-line summary such as "Supply: Iron Plate, Copper Sheet", or "Off".
	SMARTTRAINS_API FString Settings(ESTRole InRole, const TArray<FSTItemRule>& InItems);
}

// A platform's SmartTrains settings.
USTRUCT()
struct SMARTTRAINS_API FSTPlatformConfig
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	TObjectPtr<AFGBuildableTrainPlatformCargo> Platform = nullptr;

	UPROPERTY(SaveGame)
	ESTRole Role = ESTRole::Off;

	UPROPERTY(SaveGame)
	TArray<FSTItemRule> Items;

	// Load/unload setting the platform had before it got a role, put back when the role is removed.
	UPROPERTY(SaveGame)
	bool bOriginalLoadMode = false;

	UPROPERTY(SaveGame)
	bool bHasOriginalLoadMode = false;

	// Who last changed the settings, for the log and readouts.
	UPROPERTY(SaveGame)
	FString EditedBy;

	// Returns the rule for Item, or the any-item rule of a sink, or nullptr.
	const FSTItemRule* FindRule(TSubclassOf<UFGItemDescriptor> Item) const;
};

// A station's SmartTrains settings.
USTRUCT()
struct SMARTTRAINS_API FSTStationConfig
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	TObjectPtr<AFGTrainStationIdentifier> Station = nullptr;

	// Idle managed trains park here.
	UPROPERTY(SaveGame)
	bool bDepot = false;
};

// A player's settings for one item's group (Docs/Architecture.md, "Groups"). Without one, the group takes the trains it
// needs.
USTRUCT()
struct SMARTTRAINS_API FSTGroupConfig
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	TSubclassOf<UFGItemDescriptor> Item;

	// Fewest and most trains whose home it is (0: automatic).
	UPROPERTY(SaveGame)
	int32 MinTrains = 0;

	UPROPERTY(SaveGame)
	int32 MaxTrains = 0;
};

// What one car loads or unloads at one stop of a job.
USTRUCT()
struct SMARTTRAINS_API FSTCarWork
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	TObjectPtr<AFGRailroadVehicle> Car = nullptr;

	UPROPERTY(SaveGame)
	TSubclassOf<UFGItemDescriptor> Item;

	UPROPERTY(SaveGame)
	bool bLoad = false;

	UPROPERTY(SaveGame)
	int32 PlannedAmount = 0;

	// The platform the car was planned to load from or unload into, so the promise counts against that platform only.
	UPROPERTY(SaveGame)
	TObjectPtr<AFGBuildableTrainPlatformCargo> Platform = nullptr;

	// Amount actually moved, filled in when the train leaves.
	UPROPERTY(SaveGame)
	int32 MovedAmount = 0;
};

// One stop of a job.
USTRUCT()
struct SMARTTRAINS_API FSTJobLeg
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	TObjectPtr<AFGTrainStationIdentifier> Station = nullptr;

	// Per car: what it loads or unloads here. Empty for a stop that moves nothing (depot, out of the way).
	UPROPERTY(SaveGame)
	TArray<FSTCarWork> Work;

	// Load/unload once and go on, instead of waiting for full or empty cars.
	UPROPERTY(SaveGame)
	bool bPartial = false;

	// Cars told to skip this stop through Freight Car Control.
	UPROPERTY(SaveGame)
	TArray<TObjectPtr<AFGRailroadVehicle>> SkipCars;

	// Expected arrival, in game seconds.
	UPROPERTY(SaveGame)
	double PlannedArrival = 0.0;

	// Items loaded (bLoad) or unloaded here, each once.
	TArray<UClass*> GetItems(bool bWantLoad) const;
	// Total planned or moved amount of the loads or unloads here.
	int32 GetPlanned(bool bWantLoad) const;
	int32 GetMoved(bool bWantLoad) const;
	bool HasWork() const { return Work.Num() > 0; }
};

// A managed train's work: stops in order, written into its timetable.
USTRUCT()
struct SMARTTRAINS_API FSTJob
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	int32 Id = 0;

	UPROPERTY(SaveGame)
	TArray<FSTJobLeg> Legs;

	// Index of the leg the train is heading to or docked at.
	UPROPERTY(SaveGame)
	int32 Leg = 0;

	UPROPERTY(SaveGame)
	double StartedAt = 0.0;

	UPROPERTY(SaveGame)
	double LegStartedAt = 0.0;

	// Park after the last leg instead of waiting for the next job where it stands.
	UPROPERTY(SaveGame)
	bool bPark = false;

	// The item group the trip was planned for: the train's home group, or the one it was lent to.
	UPROPERTY(SaveGame)
	TSubclassOf<UFGItemDescriptor> GroupItem;

	// One line for logs and readouts.
	UPROPERTY(SaveGame)
	FString Summary;
};

// SmartTrains' state for one train.
// A move that moved nothing: not planned again for the train until Until (runtime only).
struct FSTNoProgress
{
	TWeakObjectPtr<AFGTrainStationIdentifier> Station;
	TWeakObjectPtr<UClass> Item;
	bool bLoad = false;
	double Until = 0.0;
};

USTRUCT()
struct SMARTTRAINS_API FSTTrainState
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	TObjectPtr<AFGTrain> Train = nullptr;

	UPROPERTY(SaveGame)
	bool bManaged = false;

	// Never managed and never proposed by the migration tool.
	UPROPERTY(SaveGame)
	bool bKeepVanilla = false;

	UPROPERTY(SaveGame)
	ESTTrainPhase Phase = ESTTrainPhase::Idle;

	UPROPERTY(SaveGame)
	bool bHasJob = false;

	UPROPERTY(SaveGame)
	FSTJob Job;

	// Station the train last docked at, and when it left (game seconds).
	UPROPERTY(SaveGame)
	TObjectPtr<AFGTrainStationIdentifier> LastStation = nullptr;

	UPROPERTY(SaveGame)
	double LastDepartureAt = 0.0;

	// Timetable and autopilot the train had before it was managed, put back when management ends.
	UPROPERTY(SaveGame)
	TArray<FTimeTableStop> SavedStops;

	UPROPERTY(SaveGame)
	int32 SavedCurrentStop = 0;

	UPROPERTY(SaveGame)
	bool bSavedSelfDriving = false;

	UPROPERTY(SaveGame)
	bool bHasSaved = false;

	// Name before SmartTrains showed jobs in it.
	UPROPERTY(SaveGame)
	FText OriginalName;

	UPROPERTY(SaveGame)
	bool bRenamed = false;

	// Stations of the timetable SmartTrains last wrote, to notice changes made by others.
	UPROPERTY(SaveGame)
	TArray<TObjectPtr<AFGTrainStationIdentifier>> WrittenStations;

	// When the train began carrying cargo no trip wanted (game seconds), 0 if it doesn't.
	UPROPERTY(SaveGame)
	double UnwantedCargoSince = 0.0;

	// Runtime only: moves that moved nothing at their station lately.
	TArray<FSTNoProgress> NoProgress;

	// Runtime only: the name SmartTrains last gave the train, to notice a player renaming it.
	FString LastWrittenName;
	// Runtime only: one key per stop SmartTrains last wrote (station, rules, skipped cars), to skip rewriting the same.
	TArray<FString> WrittenStopKeys;
	// Runtime only: the rest of the trip is re-planned in the next dispatch.
	bool bReplan = false;
	// Runtime only: that re-plan starts from scratch (from the cargo on board), not from the stops it has.
	bool bReplanFresh = false;
	// Runtime only: when the trip was last planned again because a parked train couldn't leave a station it needs.
	double LastRerouted = -1000.0;
	// Its home group's item (null: none) and the player's Stay in this group: allocation doesn't move it.
	UPROPERTY(SaveGame)
	TSubclassOf<UFGItemDescriptor> HomeItem;

	UPROPERTY(SaveGame)
	bool bHomeStay = false;

	// Runtime only: docking state at the last check, when the current dock began, and where.
	bool bWasDocked = false;
	double DockedAt = 0.0;
	TWeakObjectPtr<AFGTrainStationIdentifier> DockedStation;
	// Runtime only: when SmartTrains last told the train to drive, for the stuck report.
	double LastCommandAt = 0.0;
	// Runtime only: the game's self-driving error at the last check, logged when it changes (0 = none).
	uint8 LastSelfDrivingError = 0;
	// Runtime only: the self-driving error now, since when, and whether it was logged (only once it lasts a while).
	uint8 SelfDrivingError = 0;
	double SelfDrivingErrorSince = 0.0;
	// Runtime only: the lasting no-way error was acted on (left out its station).
	bool bUnreachableHandled = false;
	// Runtime only: stations the game found no way to for this train, left out of its trips until then.
	TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, double> Unreachable;
	// Runtime only: a player in the driver's seat at the last check (0 none, 1 with autopilot on, 2 driving by hand).
	uint8 PlayerAboard = 0;
	// Runtime only, for the current dock: the stay was ended early, the cargo moved so far, and when it last changed.
	bool bDockReleased = false;
	int32 DockMoved = 0;
	double DockProgressAt = 0.0;
	// Runtime only: the job was replaced while the train was docked, so the coming departure ends the old job's dock.
	bool bChainedWhileDocked = false;
	// Runtime only: that job's timetable is written as the train leaves; written during the dock, the game moves it
	// on by one stop at departure.
	bool bWriteOnUndock = false;
	// Runtime only: timetable changes by others this minute, and when the count started.
	int32 ExternalChanges = 0;
	double ExternalChangesSince = 0.0;
};

// Which vehicle stood beside each platform of a station the last time a train docked there.
USTRUCT()
struct SMARTTRAINS_API FSTDockMap
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	TObjectPtr<AFGTrain> Train = nullptr;

	UPROPERTY(SaveGame)
	TObjectPtr<AFGTrainStationIdentifier> Station = nullptr;

	// Index = position of the platform counted from the station (0 = next to it); null where no vehicle docked.
	UPROPERTY(SaveGame)
	TArray<TObjectPtr<AFGRailroadVehicle>> Vehicles;

	// The train's vehicles in order when it was seen; the map is only used while they are the same. Empty in maps
	// from 0.1.0 to 0.3.0 saves, which are checked by vehicle set only.
	UPROPERTY(SaveGame)
	TArray<TObjectPtr<AFGRailroadVehicle>> Consist;

	// The locomotive that docked (the train's lead), which says which way round the train stood. Null in maps from
	// saves before 0.4.0, which are told apart by the first docked vehicle.
	UPROPERTY(SaveGame)
	TObjectPtr<AFGLocomotive> Locomotive = nullptr;
};

// Learned travel time between two stations.
USTRUCT()
struct SMARTTRAINS_API FSTTripTime
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	TObjectPtr<AFGTrainStationIdentifier> From = nullptr;

	UPROPERTY(SaveGame)
	TObjectPtr<AFGTrainStationIdentifier> To = nullptr;

	UPROPERTY(SaveGame)
	float Seconds = 0.f;

	UPROPERTY(SaveGame)
	int32 Samples = 0;
};

// Learned stay at a station.
USTRUCT()
struct SMARTTRAINS_API FSTDwellTime
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	TObjectPtr<AFGTrainStationIdentifier> Station = nullptr;

	UPROPERTY(SaveGame)
	float Seconds = 0.f;

	UPROPERTY(SaveGame)
	int32 Samples = 0;
};

// What the last migration changed, so it can be undone.
USTRUCT()
struct SMARTTRAINS_API FSTMigrationUndo
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	bool bValid = false;

	// Platform settings before the migration (Role Off where there were none).
	UPROPERTY(SaveGame)
	TArray<FSTPlatformConfig> Platforms;

	// Trains the migration put under management.
	UPROPERTY(SaveGame)
	TArray<TObjectPtr<AFGTrain>> Trains;

	UPROPERTY(SaveGame)
	FString AppliedBy;
};
