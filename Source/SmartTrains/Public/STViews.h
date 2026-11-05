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

	// The station's linking setting, and whether an edit here applies to the whole station now.
	UPROPERTY()
	ESTPlatformLink Link = ESTPlatformLink::Auto;

	UPROPERTY()
	bool bLinked = false;

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

	UPROPERTY()
	FString Text;

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
