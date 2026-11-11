#pragma once

#include "CoreMinimal.h"
#include "STTrace.h"
#include "STTypes.h"
#include "STViews.h"

class AFGBuildableTrainPlatformCargo;
class AFGTrain;
class AFGTrainStationIdentifier;
class ASTSubsystem;
class USTRemoteCallObject;
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
		// It already had a role: proposed as it is and left alone when applying.
		bool bKept = false;
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
		// When its one item wasn't settled by its own stock: the items it could have, best first, so platforms serving
		// the same machines can share them out (ShareOutItems).
		TArray<TSubclassOf<class UFGItemDescriptor>> SpreadChoices;
		// How much of each of SpreadChoices its machines use or make per minute (same order).
		TArray<float> SpreadRates;
		// The machines its connection trace reached.
		TArray<TWeakObjectPtr<const AActor>> Machines;
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
		// The name the station set-up gives it (STSetup::NameStation), and how it was worked out.
		FString NewName;
		FString NameNote;
	};

	struct FTrainProposal
	{
		TWeakObjectPtr<AFGTrain> Train;
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
		FString DttNote;
		FString CsvPath;
		double MadeAt = 0.0;
	};

	// Platforms whose one item wasn't settled by their own stock share out the items their traces found, within groups
	// of same-role platforms reaching a machine in common.
	void ShareOutItems(FPreview& Preview);

	// Names the last preview's stations, a few per call (about BudgetMs); true when done or none are pending.
	bool StepNaming(ASTSubsystem* Subsystem, UWorld* World, double BudgetMs);
	// The window that asked for the last preview gets the names when they are ready.
	void SetRequester(UWorld* World, USTRemoteCallObject* Requester);
	// True while the last preview's station names are still being worked out.
	bool IsNaming(UWorld* World);

	// Builds a preview: every station's platforms from what is attached (STSetup::ProposeStation) and every train that
	// isn't managed yet; writes the CSV report and returns chat lines. Station names follow (StepNaming).
	TArray<FString> Preview(ASTSubsystem* Subsystem, UWorld* World);

	// Applies the last preview. Selection: "all" (unflagged stations and preselected trains), or "stations 1,2" and/or
	// "trains 3,4" (numbers from the preview); "rename" also renames the stations. Returns chat lines.
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
