#pragma once

#include "CoreMinimal.h"
#include "STTypes.h"
#include "STViews.generated.h"

class AFGBuildableTrainPlatformCargo;
class AFGTrain;
class AFGTrainStationIdentifier;

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

	// Why it can't be managed, empty if it can.
	UPROPERTY()
	FString CannotManage;

	// One line: phase and job.
	UPROPERTY()
	FString Status;

	// Job legs and cargo.
	UPROPERTY()
	TArray<FString> Lines;

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

	UPROPERTY()
	bool bDepot = false;

	// Layout and platform roles.
	UPROPERTY()
	TArray<FString> Lines;

	// Fleet status and advice.
	UPROPERTY()
	TArray<FString> NetworkLines;

	UPROPERTY()
	bool bBetaTools = false;

	UPROPERTY()
	int32 AppliedEdit = 0;
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
};

// One row of the throughput monitor: an item, station, platform or train, or an overview tile.
USTRUCT()
struct SMARTTRAINS_API FSTMonitorRow
{
	GENERATED_BODY()

	// Item, station or train name; a tile's label.
	UPROPERTY()
	FString Title;

	// The item whose icon the row shows, if any.
	UPROPERTY()
	TSubclassOf<UFGItemDescriptor> Item;

	// Items: what limits it (OK, Measuring, Supply short, Trains short, No train fits, Congested, Blocked, No demand,
	// No supply). Stations: Congested or OK. Platforms: Starved or Backing up. Trains: their state. Tiles: the value.
	UPROPERTY()
	FString Verdict;

	// 0 fine, 1 worth a look, 2 limits deliveries.
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
};

// The throughput monitor's content ("/st monitor").
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

	// Per item, the ones that limit deliveries first. Columns: supply, demand, trains, below target.
	UPROPERTY()
	TArray<FSTMonitorRow> Items;

	// Stations with roles, congested ones first. Columns: docked, arrivals, average lateness.
	UPROPERTY()
	TArray<FSTMonitorRow> Stations;

	// Starved demand platforms and backed-up supply platforms. Columns: share of the time.
	UPROPERTY()
	TArray<FSTMonitorRow> Platforms;

	// Managed trains: state in Verdict, trip or idle reason in Details.
	UPROPERTY()
	TArray<FSTMonitorRow> Trains;

	// Idle managed trains grouped by why they got no trip.
	UPROPERTY()
	TArray<FString> Idle;

	// Minutes of history behind the verdicts.
	UPROPERTY()
	int32 WindowMinutes = 0;
};
