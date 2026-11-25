#pragma once

#include "CoreMinimal.h"
#include "STTypes.h"
#include "Resources/FGItemDescriptor.h"
#include "STViews.generated.h"

class AFGBuildableTrainPlatformCargo;
class AFGTrain;
class AFGTrainStationIdentifier;

// One car of a train's pattern, as the UI draws it: its item, what it holds of it and how much fits.
USTRUCT()
struct SMARTTRAINS_API FSTCarView
{
	GENERATED_BODY()

	UPROPERTY()
	TSubclassOf<UFGItemDescriptor> Item;

	UPROPERTY()
	int32 Amount = 0;

	UPROPERTY()
	int32 Capacity = 0;

	UPROPERTY()
	bool bFluid = false;

	// Its cargo moves now (docked where it loads or unloads).
	UPROPERTY()
	bool bActive = false;
};

// One stop of a train's loop.
USTRUCT()
struct SMARTTRAINS_API FSTStopView
{
	GENERATED_BODY()

	UPROPERTY()
	FString Station;

	// 0 picks up, 1 delivers, 2 drives there.
	UPROPERTY()
	uint8 Kind = 0;

	UPROPERTY()
	TArray<TSubclassOf<UFGItemDescriptor>> Items;

	UPROPERTY()
	bool bCurrent = false;

	UPROPERTY()
	bool bDocked = false;
};

// One platform of a station, in order from the station.
USTRUCT()
struct SMARTTRAINS_API FSTPlatformChip
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Index = 0;

	UPROPERTY()
	ESTRole Role = ESTRole::Off;

	UPROPERTY()
	TArray<TSubclassOf<UFGItemDescriptor>> Items;

	UPROPERTY()
	bool bFluid = false;
};

// Where a consumer's item comes from, or what a supplier station offers of one item.
USTRUCT()
struct SMARTTRAINS_API FSTSourceView
{
	GENERATED_BODY()

	UPROPERTY()
	FString Station;

	UPROPERTY()
	TSubclassOf<UFGItemDescriptor> Item;

	// Travel from there to the consumer, -1 if not known. [s]
	UPROPERTY()
	float Seconds = -1.f;

	// A supplier's most rate, -1 if not known, and what loops take of it. [car loads/min]
	UPROPERTY()
	float Rate = -1.f;

	UPROPERTY()
	float Taken = 0.f;
};

// What the server sends a player's menu about one platform (Docs/Architecture.md, "UI").
USTRUCT()
struct SMARTTRAINS_API FSTPlatformView
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<AFGBuildableTrainPlatformCargo> Platform = nullptr;

	// "Freight platform 2 of Iron Hub".
	UPROPERTY()
	FString Title;

	UPROPERTY()
	bool bOnStation = false;

	UPROPERTY()
	bool bFluid = false;

	UPROPERTY()
	ESTRole Role = ESTRole::Off;

	UPROPERTY()
	TArray<FSTItemRule> Items;

	// One readout per entry of Items: stock, rate, batch or target.
	UPROPERTY()
	TArray<FString> ItemLines;

	// Trains on the way, recent jobs, and why nothing is coming.
	UPROPERTY()
	TArray<FString> Lines;

	UPROPERTY()
	FString EditedBy;

	UPROPERTY()
	bool bFcc = false;

	// Its item's most rate here (supplied, or used by the consumer), -1 if not known. [per minute]
	UPROPERTY()
	float RatePerMinute = -1.f;

	// Loops taking from it (Supply) or trains on its station's loop (Demand).
	UPROPERTY()
	int32 Loops = 0;

	// The next train to work here, empty if none, and its arrival, -1 if not known (0 docked). [s]
	UPROPERTY()
	FString NextTrain;

	UPROPERTY()
	float NextSeconds = -1.f;

	// Last edit number of this player the server had applied when it made the view.
	UPROPERTY()
	int32 AppliedEdit = 0;
};

// What the server sends the locomotive menu about one train.
// Platform settings held by a player's clipboard, one per cargo form (as the game keeps one per building kind).
USTRUCT()
struct SMARTTRAINS_API FSTPlatformSettings
{
	GENERATED_BODY()

	UPROPERTY()
	bool bValid = false;

	UPROPERTY()
	ESTRole Role = ESTRole::Off;

	UPROPERTY()
	TArray<FSTItemRule> Items;

};

USTRUCT()
struct SMARTTRAINS_API FSTTrainView
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<AFGTrain> Train = nullptr;

	UPROPERTY()
	bool bManaged = false;

	UPROPERTY()
	bool bKeepVanilla = false;

	// Held by a player (Hold / Resume), and whether it has a job to cancel.
	UPROPERTY()
	bool bHeld = false;

	UPROPERTY()
	bool bHasJob = false;

	// Why it can't be managed, empty if it can.
	UPROPERTY()
	FString CannotManage;

	// One line: phase and job.
	UPROPERTY()
	FString Status;

	// Job legs and cargo.
	UPROPERTY()
	TArray<FString> Lines;

	// 0 not managed, 1 on its loop, 2 switching patterns, 3 held, 4 no loop, 5 at or heading to a depot signal.
	UPROPERTY()
	uint8 State = 0;

	// Its consumer, empty without one.
	UPROPERTY()
	FString Consumer;

	UPROPERTY()
	TArray<FSTCarView> Cars;

	UPROPERTY()
	TArray<FSTStopView> Stops;

	// Its consumer's loop: trains on it, the share it should have, how many wait at a depot signal now.
	UPROPERTY()
	int32 PatternTrains = 0;

	UPROPERTY()
	int32 PatternTarget = 0;

	UPROPERTY()
	int32 PatternAtDepot = 0;

	// Its consumer's most rate [car loads/min], the loop's round trip [s] and the share of its machines that want input.
	UPROPERTY()
	float Rate = 0.f;

	UPROPERTY()
	float RoundTrip = 0.f;

	UPROPERTY()
	float Use = 1.f;

	// Until it gets to its current stop, -1 if not known, 0 docked there. [s]
	UPROPERTY()
	float NextSeconds = -1.f;

	UPROPERTY()
	int32 AppliedEdit = 0;
};

// What the server sends the station menu about one station and the network.
USTRUCT()
struct SMARTTRAINS_API FSTStationView
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<AFGTrainStationIdentifier> Station = nullptr;


	// Layout and platform roles.
	UPROPERTY()
	TArray<FString> Lines;

	// Fleet status and advice.
	UPROPERTY()
	TArray<FString> NetworkLines;

	// Station set-up: the last preview, or what the last apply changed.
	UPROPERTY()
	TArray<FString> SetupLines;

	// A preview was made in the last two minutes and can be applied as shown: 1 the name, 2 the platforms.
	UPROPERTY()
	uint8 SetupReady = 0;

	UPROPERTY()
	bool bBetaTools = false;

	UPROPERTY()
	TArray<FSTPlatformChip> Platforms;

	// It is a consumer (its loop's numbers below), and a supplier (Supplies).
	UPROPERTY()
	bool bConsumer = false;

	UPROPERTY()
	float Rate = 0.f;

	UPROPERTY()
	float RoundTrip = 0.f;

	// Its share of all managed trains' need, -1 if it has none. [0..1]
	UPROPERTY()
	float Share = -1.f;

	UPROPERTY()
	float Use = 1.f;

	UPROPERTY()
	int32 Trains = 0;

	UPROPERTY()
	int32 Target = 0;

	UPROPERTY()
	int32 AtDepot = 0;

	UPROPERTY()
	FString Problem;

	UPROPERTY()
	TArray<FSTSourceView> Suppliers;

	UPROPERTY()
	TArray<FSTSourceView> Supplies;

	UPROPERTY()
	int32 AppliedEdit = 0;
};

// One platform of a station in a migration preview.
USTRUCT()
struct SMARTTRAINS_API FSTMigrationPlatformRow
{
	GENERATED_BODY()

	// Position in the station, counted from 1.
	UPROPERTY()
	int32 Number = 0;

	UPROPERTY()
	ESTRole Role = ESTRole::Off;

	UPROPERTY()
	TArray<TSubclassOf<UFGItemDescriptor>> Items;

	// high, medium, low or unclear; empty for platforms without a role.
	UPROPERTY()
	FString Confidence;

	// Where the items came from and what else was found.
	UPROPERTY()
	FString Note;

	// Why the platform needs a look, if it does.
	UPROPERTY()
	FString Conflict;

	// A fluid platform (liquids and gases), for the item picker.
	UPROPERTY()
	bool bFluid = false;

	// False for a piece that can't move freight or fluid: shown, not settable.
	UPROPERTY()
	bool bCargo = true;
};

// One station or train of a migration preview.
USTRUCT()
struct SMARTTRAINS_API FSTMigrationRow
{
	GENERATED_BODY()

	UPROPERTY()
	bool bStation = true;

	// Number in the preview, counted from 1.
	UPROPERTY()
	int32 Number = 0;

	// One line, as chat shows it.
	UPROPERTY()
	FString Text;

	// The station or train name.
	UPROPERTY()
	FString Title;

	// One line per platform (stations) or the route and note (trains).
	UPROPERTY()
	TArray<FString> Details;

	// Why a station isn't ticked by default, if it isn't.
	UPROPERTY()
	FString Flag;

	// Ticked by default: a station without a flag, or a train the preview proposes to manage.
	UPROPERTY()
	bool bProposed = false;

	// Stations: their platforms.
	UPROPERTY()
	TArray<FSTMigrationPlatformRow> Platforms;

	// Stations: the trains or groups that use them. Trains: their route.
	UPROPERTY()
	FString Users;

	// Stations: the station set-up name (empty: kept), used when Rename stations is on.
	UPROPERTY()
	FString NewName;

	// Trains: why they are ticked or not.
	UPROPERTY()
	FString Note;
};

// The migration window's content.
USTRUCT()
struct SMARTTRAINS_API FSTMigrationView
{
	GENERATED_BODY()

	UPROPERTY()
	bool bHasPreview = false;

	UPROPERTY()
	TArray<FSTMigrationRow> Rows;

	// Result of the last step, as the chat command would print it.
	UPROPERTY()
	TArray<FString> Lines;

	// No test copy or backup was made this session, so applying asks to confirm.
	UPROPERTY()
	bool bNeedsConfirm = false;

	UPROPERTY()
	bool bCanUndo = false;

	UPROPERTY()
	bool bBetaTools = false;

	// Counts the server's answers, so the window notices each new one.
	UPROPERTY()
	int32 Serial = 0;
};

// Steps the migration window asks for.
UENUM()
enum class ESTMigrationStep : uint8
{
	Refresh,
	Preview,
	Apply,
	Undo,
	TestCopy,
	Backup,
};

// Train changes the locomotive menu asks for.
UENUM()
enum class ESTTrainAction : uint8
{
	Manage,
	Unmanage,
	KeepVanillaOn,
	KeepVanillaOff,
	Hold,
	Resume,
};

// One row of the monitor: a consumer, supplier or train, or an overview tile.
USTRUCT()
struct SMARTTRAINS_API FSTMonitorRow
{
	GENERATED_BODY()

	// Station or train name; a tile's label.
	UPROPERTY()
	FString Title;

	// The item whose icon the row shows, if any.
	UPROPERTY()
	TSubclassOf<UFGItemDescriptor> Item;

	// Consumers: Served or Unserved. Suppliers: Used, Not used or Over its rate. Trains: their state. Tiles: the value.
	UPROPERTY()
	FString Verdict;

	// 0 fine, 1 worth a look, 2 needs a look.
	UPROPERTY()
	uint8 Severity = 0;

	// Table cells, in the order of the tab's column headings.
	UPROPERTY()
	TArray<FString> Columns;

	// What to do about it, if anything (shown in the overview).
	UPROPERTY()
	FString Advice;

	UPROPERTY()
	TArray<FString> Details;

	// Consumers: their pattern (one item per slot), trains, target, how many wait at a depot signal and their use.
	UPROPERTY()
	TArray<TSubclassOf<UFGItemDescriptor>> Pattern;

	UPROPERTY()
	int32 Trains = 0;

	UPROPERTY()
	int32 Target = 0;

	UPROPERTY()
	int32 AtDepot = 0;

	UPROPERTY()
	float Use = 1.f;
};

// The monitor's content ("/st monitor"): the loop plan as last worked out.
USTRUCT()
struct SMARTTRAINS_API FSTMonitorView
{
	GENERATED_BODY()

	// Overview tiles: label in Title, value in Verdict.
	UPROPERTY()
	TArray<FSTMonitorRow> Tiles;

	// Fleet lines.
	UPROPERTY()
	TArray<FString> Summary;

	// Consumer stations, unserved first. Columns: rate (car loads/min), round trip, share of the need, trains.
	UPROPERTY()
	TArray<FSTMonitorRow> Consumers;

	// Supplier stations per item. Columns: item, rate, taken by loops (car loads/min).
	UPROPERTY()
	TArray<FSTMonitorRow> Suppliers;

	// Managed trains: state in Verdict, consumer in Columns, what it does in Details.
	UPROPERTY()
	TArray<FSTMonitorRow> Trains;
};
