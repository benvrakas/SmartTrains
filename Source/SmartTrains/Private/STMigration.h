#pragma once

#include "CoreMinimal.h"
#include "STTrace.h"
#include "STTypes.h"
#include "STViews.h"

class AFGBuildableTrainPlatformCargo;
class AFGTrain;
class AFGTrainStationIdentifier;
class ASTSubsystem;
class UWorld;

// The migration tool: proposes platform roles and managed trains from the current setup (Docs/Migration.md).
namespace STMigration
{
	struct FPlatformProposal
	{
		TWeakObjectPtr<AFGBuildableTrainPlatformCargo> Platform;
		int32 Index = 0;
		// False for a piece that can't move freight or fluid (STLayout::CanMoveCargo): listed, never given a role.
		bool bCargo = true;
		ESTRole Role = ESTRole::Off;
		TArray<TSubclassOf<class UFGItemDescriptor>> Items;
		FString Evidence;
		FString Conflict;
		// What the belt or pipe trace to the machines found (empty when it wasn't needed or reached nothing).
		FString TraceText;
		// "high", "medium" or "unclear": how far the items can be trusted.
		FString Confidence;
		// Why the items are what they are when it isn't plain (backup used, other items ignored).
		FString Note;
		// Which items were left out to keep one item per platform.
		FString OneItemNote;
	};

	struct FStationProposal
	{
		TWeakObjectPtr<AFGTrainStationIdentifier> Station;
		TArray<FPlatformProposal> Platforms;
		// Why the station isn't applied by "all", if it isn't.
		FString Flag;
		// Used only by dedicated trains: left vanilla.
		bool bDedicatedOnly = false;
		TArray<FString> Users;
	};

	struct FTrainProposal
	{
		TWeakObjectPtr<AFGTrain> Train;
		bool bPullBased = false;
		bool bPreselected = false;
		FString Route;
		FString Note;
		// The timetable's stations, in order.
		TArray<TWeakObjectPtr<AFGTrainStationIdentifier>> Stops;
	};

	struct FPreview
	{
		TArray<FStationProposal> Stations;
		TArray<FTrainProposal> Trains;
		bool bDttRead = false;
		FString DttNote;
		FString CsvPath;
		double MadeAt = 0.0;
	};

	// Gathers the evidence and builds a preview; writes the CSV report and returns chat lines.
	// TraceLimit: buildings and belt segments each platform's connection trace may visit.
	// bOneItem: each platform gets one item, so a machine with several inputs isn't flooded with one of them.
	TArray<FString> Preview(ASTSubsystem* Subsystem, UWorld* World, int32 TraceLimit = STTrace::DefaultLimit, bool bOneItem = true);

	// False if the words contain "allitems".
	bool ParseOneItem(const TArray<FString>& Words);

	// Reads "tracelimit <n>" from command words; Default when absent.
	int32 ParseTraceLimit(const TArray<FString>& Words, int32 Default = STTrace::DefaultLimit);

	// Applies the last preview. Selection: "all" (unflagged stations and preselected trains), or "stations 1,2" and/or
	// "trains 3,4" (numbers from the preview). Returns chat lines.
	TArray<FString> Apply(ASTSubsystem* Subsystem, UWorld* World, const TArray<FString>& Selection, const FString& Cause);

	// Puts back what the last apply changed. Returns chat lines.
	TArray<FString> Undo(ASTSubsystem* Subsystem, UWorld* World, const FString& Cause);

	// The migration window's rows from the last preview, and whether applying or undoing is possible.
	FSTMigrationView MakeView(ASTSubsystem* Subsystem, UWorld* World);

	// Sets a platform of the last preview by hand. Station is the preview's station number, Platform the platform's
	// number in its station (both from 1). The setting is kept for later previews until its station is applied.
	// Returns false with OutMessage saying why if it can't be set.
	bool SetPlatform(UWorld* World, int32 Station, int32 Platform, ESTRole Role, const TArray<TSubclassOf<UFGItemDescriptor>>& Items, const FString& Who,
		FString& OutMessage);
}

// Save copies for testing (Docs/Commands.md, "Beta tools").
namespace STSaveTools
{
	// Saves the world as a separate session named SessionName (default "<session> (SmartTrains test)"), then puts the
	// running world's session name back. Returns false with OutMessage set if it couldn't start.
	bool TestCopy(UWorld* World, const FString& SessionName, FString& OutMessage);

	// Makes a timestamped save in the current session.
	bool Backup(UWorld* World, FString& OutMessage);
}
