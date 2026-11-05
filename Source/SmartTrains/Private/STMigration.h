#pragma once

#include "CoreMinimal.h"
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
		ESTRole Role = ESTRole::Off;
		TArray<TSubclassOf<class UFGItemDescriptor>> Items;
		FString Evidence;
		FString Conflict;
		// What the belt or pipe trace to the machines found (empty when it wasn't needed or reached nothing).
		FString TraceText;
		// "high", "medium" or "unclear": how far the items can be trusted.
		FString Confidence;
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
	TArray<FString> Preview(ASTSubsystem* Subsystem, UWorld* World);

	// Applies the last preview. Selection: "all" (unflagged stations and preselected trains), or "stations 1,2" and/or
	// "trains 3,4" (numbers from the preview). Returns chat lines.
	TArray<FString> Apply(ASTSubsystem* Subsystem, UWorld* World, const TArray<FString>& Selection, const FString& Cause);

	// Puts back what the last apply changed. Returns chat lines.
	TArray<FString> Undo(ASTSubsystem* Subsystem, UWorld* World, const FString& Cause);

	// The migration window's rows from the last preview, and whether applying or undoing is possible.
	FSTMigrationView MakeView(ASTSubsystem* Subsystem, UWorld* World);
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
