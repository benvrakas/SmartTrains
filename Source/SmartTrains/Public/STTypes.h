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
	// Managed trains deliver the platform's items; its station's unloading platforms make a car pattern.
	Demand,
	// Managed trains switching patterns unload cargo here that their old consumer had no room for; any item.
	Sink,
};

// How a consumer station gets trains when there aren't enough for every loop's full need (Docs/Architecture.md,
// "Priority"). Set per station, since its whole pattern is one loop.
UENUM()
enum class ESTPriority : uint8
{
	// Trains only once every Medium and High loop has its full need.
	Low,
	// The default: shares what High loops leave, by need.
	Medium,
	// Takes trains from Medium and Low loops until it has its full need.
	High,
};

// What a managed train is doing.
UENUM()
enum class ESTTrainPhase : uint8
{
	// No loop: it stands where it is with its timetable as it was.
	Idle,
	// Following its job's stops.
	Working,
	// Standing still with self-driving switched off by SmartTrains (held, or no consumer it fits).
	Parked,
};

// What a job is for.
UENUM()
enum class ESTJobKind : uint8
{
	// The train's loop: its suppliers, then its consumer, repeated by its timetable.
	Loop,
	// Switching patterns: what it holds goes to the consumer it served before.
	Deliver,
	// Switching patterns: what its old consumer had no room for goes to a Sink platform.
	Sink,
};

namespace STNames
{
	SMARTTRAINS_API const TCHAR* Role(ESTRole Role);
	SMARTTRAINS_API const TCHAR* Priority(ESTPriority Priority);
	// True for roles whose platforms managed trains unload at (Demand, Sink).
	SMARTTRAINS_API bool Unloads(ESTRole Role);
	SMARTTRAINS_API const TCHAR* Phase(ESTTrainPhase Phase);
	SMARTTRAINS_API const TCHAR* JobKind(ESTJobKind Kind);
	SMARTTRAINS_API TOptional<ESTRole> ParseRole(const FString& Text);
	// Returns the item's display name, or "any item" for null.
	SMARTTRAINS_API FString Item(TSubclassOf<UFGItemDescriptor> Item);
	// Returns true for liquids and gases.
	SMARTTRAINS_API bool IsFluid(TSubclassOf<UFGItemDescriptor> Item);
	// Formats an amount in the item's unit: items, or m³ for fluids (inventories count fluids in litres).
	SMARTTRAINS_API FString Amount(TSubclassOf<UFGItemDescriptor> Item, double Amount);
}

// One item a platform supplies or demands. A Sink platform has one rule with no item (it takes anything).
USTRUCT()
struct SMARTTRAINS_API FSTItemRule
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	TSubclassOf<UFGItemDescriptor> Item;
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

	// Several items on a Demand platform make its car's slot an item group: the car carries any one of them.
	UPROPERTY(SaveGame)
	TArray<FSTItemRule> Items;

	// Load/unload setting the platform had before it got a role, put back when the role is removed.
	UPROPERTY(SaveGame)
	bool bOriginalLoadMode = false;

	UPROPERTY(SaveGame)
	bool bHasOriginalLoadMode = false;

	// The machines its belts or pipes reached when it was last set up from them (Autodetect, station set-up or the
	// migration): their recipes give its rate when the factory scan gives none.
	UPROPERTY(SaveGame)
	TArray<TObjectPtr<AActor>> Machines;

	// Who last changed the settings, for the log and readouts.
	UPROPERTY(SaveGame)
	FString EditedBy;

	// Returns the rule for Item, or the any-item rule of a sink, or nullptr.
	const FSTItemRule* FindRule(TSubclassOf<UFGItemDescriptor> Item) const;
};

// A station's own SmartTrains settings.
USTRUCT()
struct SMARTTRAINS_API FSTStationConfig
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	TObjectPtr<AFGTrainStationIdentifier> Station = nullptr;

	// As a consumer: how its loop gets trains.
	UPROPERTY(SaveGame)
	ESTPriority Priority = ESTPriority::Medium;
};

// How the station set-up names stations (Docs/Architecture.md, "Station set-up"), per world. {area}, {product},
// {role}, {n} and {rate} are filled in; empty ones and their brackets drop out.
USTRUCT()
struct SMARTTRAINS_API FSTNamingConfig
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FString Format = TEXT("{area} {product} {role} {n} ({rate})");

	// {role} for a station that only loads, one that unloads several items (a factory's inputs), one that unloads one
	// item, and one that does both.
	UPROPERTY(SaveGame)
	FString SupplyWord = TEXT("Out");

	UPROPERTY(SaveGame)
	FString FactoryWord = TEXT("In");

	UPROPERTY(SaveGame)
	FString ConsumerWord = TEXT("In");

	UPROPERTY(SaveGame)
	FString MixedWord;

	// {product} for a station whose cargo platforms are all Sinks (it has no {role} or {rate}).
	UPROPERTY(SaveGame)
	FString SinkWord = TEXT("Sink");
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

	// A full car of the item.
	UPROPERTY(SaveGame)
	int32 PlannedAmount = 0;

	// The platform the car stands beside there.
	UPROPERTY(SaveGame)
	TObjectPtr<AFGBuildableTrainPlatformCargo> Platform = nullptr;

	// Amount actually moved at the last visit, filled in when the train leaves.
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

	// Per car: what it loads or unloads here.
	UPROPERTY(SaveGame)
	TArray<FSTCarWork> Work;

	// Cars told to skip this stop through Freight Car Control.
	UPROPERTY(SaveGame)
	TArray<TObjectPtr<AFGRailroadVehicle>> SkipCars;

	// Items loaded (bLoad) or unloaded here, each once.
	TArray<UClass*> GetItems(bool bWantLoad) const;
	// Total planned or moved amount of the loads or unloads here.
	int32 GetPlanned(bool bWantLoad) const;
	int32 GetMoved(bool bWantLoad) const;
	bool HasWork() const { return Work.Num() > 0; }
};

// The items one freight car carries in its train's pattern (empty: none).
USTRUCT()
struct SMARTTRAINS_API FSTCarItems
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	TArray<TSubclassOf<UFGItemDescriptor>> Items;
};

// A managed train's work: stops in order, written into its timetable. A Loop repeats; Deliver and Sink end after their
// last stop.
USTRUCT()
struct SMARTTRAINS_API FSTJob
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	int32 Id = 0;

	UPROPERTY(SaveGame)
	ESTJobKind Kind = ESTJobKind::Loop;

	UPROPERTY(SaveGame)
	TArray<FSTJobLeg> Legs;

	// Index of the leg the train is heading to or docked at.
	UPROPERTY(SaveGame)
	int32 Leg = 0;

	UPROPERTY(SaveGame)
	double StartedAt = 0.0;

	UPROPERTY(SaveGame)
	double LegStartedAt = 0.0;

	// Loop: its consumer station. Deliver: the consumer the cargo goes back to.
	UPROPERTY(SaveGame)
	TObjectPtr<AFGTrainStationIdentifier> AnchorStation = nullptr;

	// Loops run so far.
	UPROPERTY(SaveGame)
	int32 Rounds = 0;

	// One line for logs and readouts.
	UPROPERTY(SaveGame)
	FString Summary;
};

// SmartTrains' state for one train.
USTRUCT()
struct SMARTTRAINS_API FSTTrainState
{
	GENERATED_BODY()

	// CarSets as plain classes, one list per car.
	TArray<TArray<UClass*>> GetPattern() const
	{
		TArray<TArray<UClass*>> Out;
		for (const FSTCarItems& Car : CarSets)
		{
			TArray<UClass*>& Items = Out.AddDefaulted_GetRef();
			for (const TSubclassOf<UFGItemDescriptor>& Item : Car.Items)
			{
				Items.Add(Item.Get());
			}
		}
		return Out;
	}
	// True when no car has items given.
	bool IsPatternOpen() const
	{
		return !CarSets.ContainsByPredicate([](const FSTCarItems& Car) { return Car.Items.Num() > 0; });
	}
	// "Iron Plate | Wire | - | Wire", or "open" when no car has items.
	FString DescribePattern() const
	{
		TArray<FString> Parts;
		for (const FSTCarItems& Car : CarSets)
		{
			TArray<FString> Names;
			for (const TSubclassOf<UFGItemDescriptor>& Item : Car.Items)
			{
				Names.Add(STNames::Item(Item));
			}
			Parts.Add(Names.Num() > 0 ? FString::Join(Names, TEXT(" + ")) : FString(TEXT("-")));
		}
		return IsPatternOpen() ? FString(TEXT("open")) : FString::Join(Parts, TEXT(" | "));
	}

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

	// Held by a player (locomotive menu Hold): autopilot off, no loop given, until Resume.
	UPROPERTY(SaveGame)
	bool bHeld = false;

	UPROPERTY(SaveGame)
	FSTJob Job;

	// The consumer station whose loop the train is given (Docs/Architecture.md, "Loops"); null if none.
	UPROPERTY(SaveGame)
	TObjectPtr<AFGTrainStationIdentifier> LoopConsumer = nullptr;

	// Its consumer's use doesn't need it now: it waits at a depot signal (or is on its way there), still on that loop.
	UPROPERTY(SaveGame)
	bool bDepot = false;

	// Station the train last docked at, and when it left (game seconds).
	UPROPERTY(SaveGame)
	TObjectPtr<AFGTrainStationIdentifier> LastStation = nullptr;

	UPROPERTY(SaveGame)
	double LastDepartureAt = 0.0;

	// Switching patterns: when it started clearing cargo its new loop doesn't carry (game seconds), -1 when not.
	UPROPERTY(SaveGame)
	double ClearingSince = -1.0;

	// Timetable and autopilot the train had before it was managed, put back when management ends.
	UPROPERTY(SaveGame)
	TArray<FTimeTableStop> SavedStops;

	UPROPERTY(SaveGame)
	int32 SavedCurrentStop = 0;

	UPROPERTY(SaveGame)
	bool bSavedSelfDriving = false;

	UPROPERTY(SaveGame)
	bool bHasSaved = false;

	// Name before SmartTrains showed its loop in it.
	UPROPERTY(SaveGame)
	FText OriginalName;

	UPROPERTY(SaveGame)
	bool bRenamed = false;

	// Stations of the timetable SmartTrains last wrote, to notice changes made by others.
	UPROPERTY(SaveGame)
	TArray<TObjectPtr<AFGTrainStationIdentifier>> WrittenStations;

	// The items each freight car carries on its loop, front to back (Docs/Architecture.md, "Car patterns").
	UPROPERTY(SaveGame)
	TArray<FSTCarItems> CarSets;

	// Runtime only: the name SmartTrains last gave the train, to notice a player renaming it.
	FString LastWrittenName;
	// Runtime only: one key per stop SmartTrains last wrote (station, rules, skipped cars), to skip rewriting the same.
	TArray<FString> WrittenStopKeys;
	// Runtime only: docking state at the last check, when the current dock began, and where.
	bool bWasDocked = false;
	double DockedAt = 0.0;
	TWeakObjectPtr<AFGTrainStationIdentifier> DockedStation;
	// Runtime only: the cars that skip the stop the train is docked at, to write that stop again ahead of a new job.
	TArray<TObjectPtr<AFGRailroadVehicle>> DockedSkipCars;
	// Runtime only: when SmartTrains last told the train to drive, for the stuck report.
	double LastCommandAt = 0.0;
	// Runtime only: the game's self-driving error now, since when, and the one last logged (0 = none).
	uint8 SelfDrivingError = 0;
	double SelfDrivingErrorSince = 0.0;
	uint8 LastSelfDrivingError = 0;
	// Runtime only: a lasting invalid-stop error was acted on (its timetable written again).
	bool bInvalidStopHandled = false;
	// Runtime only: a player in the driver's seat at the last check (0 none, 1 with autopilot on, 2 driving by hand).
	uint8 PlayerAboard = 0;
	// Runtime only: timetable changes by others this minute, and when the count started.
	int32 ExternalChanges = 0;
	double ExternalChangesSince = 0.0;
	// Runtime only: why it has no loop, for readouts; empty when it has one.
	FString IdleReason;
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

	// Stations the migration renamed, and their names before.
	UPROPERTY(SaveGame)
	TArray<TObjectPtr<AFGTrainStationIdentifier>> RenamedStations;

	UPROPERTY(SaveGame)
	TArray<FString> OldNames;

	UPROPERTY(SaveGame)
	FString AppliedBy;
};
