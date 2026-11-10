#include "STMigration.h"
#include "Buildables/FGBuildableRailroadStation.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "Engine/World.h"
#include "FGInventoryComponent.h"
#include "FGRailroadSubsystem.h"
#include "FGRailroadTimeTable.h"
#include "FGTrain.h"
#include "FGTrainStationIdentifier.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SmartTrains.h"
#include "STBridges.h"
#include "STLayout.h"
#include "STSubsystem.h"
#include "STRemoteCallObject.h"
#include "STStationSetup.h"
#include "STTrace.h"

namespace
{
	TMap<TWeakObjectPtr<UWorld>, STMigration::FPreview> GPreviews;

	FString ItemList(const TArray<TSubclassOf<UFGItemDescriptor>>& Items)
	{
		FString Text;
		for (const TSubclassOf<UFGItemDescriptor>& Item : Items)
		{
			Text += (Text.IsEmpty() ? TEXT("") : TEXT(" + ")) + STNames::Item(Item);
		}
		return Text.IsEmpty() ? FString(TEXT("?")) : Text;
	}

	// "Name: P1=Supply[Iron Ore] P2=...  [flag]" for one proposed station.
	FString StationText(const STMigration::FStationProposal& Station)
	{
		FString Layout;
		for (const STMigration::FPlatformProposal& Platform : Station.Platforms)
		{
			Layout += FString::Printf(TEXT(" P%d=%s[%s]%s"), Platform.Index + 1, STNames::Role(Platform.Role), *ItemList(Platform.Items),
				Platform.Confidence.IsEmpty() || Platform.Confidence == TEXT("none") || Platform.Confidence == TEXT("high") ? TEXT("") : *FString::Printf(TEXT("(%s)"), *Platform.Confidence));
		}
		return FString::Printf(TEXT("%s:%s%s"), *STLayout::Name(Station.Station.Get()), *Layout,
			Station.Flag.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("  [%s]"), *Station.Flag));
	}

	// "Name: route (note)" for one proposed train.
	FString TrainText(const STMigration::FTrainProposal& Train)
	{
		return FString::Printf(TEXT("%s: %s (%s)"), *STLayout::Name(Train.Train.Get()), *Train.Route, *Train.Note);
	}

	// One line per platform for the migration window: "P1  Supply  Plastic  - high".
	FString PlatformLine(const STMigration::FPlatformProposal& Platform)
	{
		FString Line = FString::Printf(TEXT("P%d   %s   %s"), Platform.Index + 1, STNames::Role(Platform.Role), *ItemList(Platform.Items));
		if (Platform.Role != ESTRole::Off && !Platform.Confidence.IsEmpty())
		{
			Line += FString::Printf(TEXT("   - %s"), *Platform.Confidence);
		}
		// The trace summary without its path (the CSV has the path); the notes stay.
		FString Brief = Platform.TraceText;
		const int32 Via = Brief.Find(TEXT(" via "));
		if (Via != INDEX_NONE)
		{
			const FString Rest = Brief.Mid(Via);
			const int32 Notes = Rest.Find(TEXT(" ("));
			Brief = Brief.Left(Via) + (Notes != INDEX_NONE ? Rest.Mid(Notes) : FString());
		}
		Brief.TrimEndInline();
		if (!Brief.IsEmpty())
		{
			Line += FString::Printf(TEXT("   (trace %s)"), *Brief);
		}
		if (!Platform.Note.IsEmpty())
		{
			Line += TEXT("   ") + Platform.Note;
		}
		if (!Platform.Conflict.IsEmpty())
		{
			Line += TEXT("   ! ") + Platform.Conflict;
		}
		return Line;
	}

	FString Csv(const FString& Text)
	{
		return TEXT("\"") + Text.Replace(TEXT("\""), TEXT("\"\"")) + TEXT("\"");
	}

	// Parses "1,3,5" or "1 3 5" into numbers counted from 1.
	TArray<int32> ParseNumbers(const TArray<FString>& Words, int32 Start)
	{
		TArray<int32> Numbers;
		for (int32 Index = Start; Index < Words.Num(); ++Index)
		{
			TArray<FString> Parts;
			Words[Index].ParseIntoArray(Parts, TEXT(","));
			for (const FString& Part : Parts)
			{
				if (Part.IsNumeric())
				{
					Numbers.Add(FCString::Atoi(*Part));
				}
			}
		}
		return Numbers;
	}
}

// Platforms whose one item wasn't settled by their own stock share out the items their traces found. A group is the
// platforms of one role whose traces reach a machine in common, across stations. A platform the trace reached no
// machine from groups with its station's platforms of that role. Each platform takes the best item no other
// platform of its group has yet. Groups with more platforms than items give the rest their best item again.
void STMigration::ShareOutItems(FPreview& Preview)
{
	struct FRef
	{
		int32 Station = 0;
		int32 Platform = 0;
	};
	TArray<FRef> Refs;
	for (int32 StationIndex = 0; StationIndex < Preview.Stations.Num(); ++StationIndex)
	{
		const TArray<STMigration::FPlatformProposal>& Platforms = Preview.Stations[StationIndex].Platforms;
		for (int32 PlatformIndex = 0; PlatformIndex < Platforms.Num(); ++PlatformIndex)
		{
			const STMigration::FPlatformProposal& Each = Platforms[PlatformIndex];
			if (Each.bCargo && Each.Role != ESTRole::Off && Each.Items.Num() > 0)
			{
				Refs.Add({StationIndex, PlatformIndex});
			}
		}
	}
	auto Get = [&Preview](const FRef& Ref) -> STMigration::FPlatformProposal& { return Preview.Stations[Ref.Station].Platforms[Ref.Platform]; };

	// Union-find over Refs.
	TArray<int32> Parent;
	for (int32 Index = 0; Index < Refs.Num(); ++Index)
	{
		Parent.Add(Index);
	}
	TFunction<int32(int32)> Find = [&Parent, &Find](int32 Index) { return Parent[Index] == Index ? Index : (Parent[Index] = Find(Parent[Index])); };
	auto Join = [&](int32 A, int32 B) { Parent[Find(A)] = Find(B); };
	TMap<TPair<const AActor*, ESTRole>, int32> ByMachine;
	TMap<TPair<int32, ESTRole>, TArray<int32>> ByStation;
	for (int32 Index = 0; Index < Refs.Num(); ++Index)
	{
		const STMigration::FPlatformProposal& Each = Get(Refs[Index]);
		ByStation.FindOrAdd(TPair<int32, ESTRole>(Refs[Index].Station, Each.Role)).Add(Index);
		for (const TWeakObjectPtr<const AActor>& Machine : Each.Machines)
		{
			if (const int32* First = ByMachine.Find(TPair<const AActor*, ESTRole>(Machine.Get(), Each.Role)))
			{
				Join(Index, *First);
			}
			else if (Machine.IsValid())
			{
				ByMachine.Add(TPair<const AActor*, ESTRole>(Machine.Get(), Each.Role), Index);
			}
		}
	}
	for (int32 Index = 0; Index < Refs.Num(); ++Index)
	{
		const STMigration::FPlatformProposal& Each = Get(Refs[Index]);
		if (Each.Machines.Num() == 0 && Each.SpreadChoices.Num() > 0)
		{
			for (int32 Other : ByStation.FindRef(TPair<int32, ESTRole>(Refs[Index].Station, Each.Role)))
			{
				Join(Index, Other);
			}
		}
	}

	TMap<int32, TSet<UClass*>> Taken;
	for (int32 Index = 0; Index < Refs.Num(); ++Index)
	{
		const STMigration::FPlatformProposal& Each = Get(Refs[Index]);
		if (Each.SpreadChoices.Num() == 0)
		{
			for (const TSubclassOf<UFGItemDescriptor>& Item : Each.Items)
			{
				Taken.FindOrAdd(Find(Index)).Add(Item.Get());
			}
		}
	}
	int32 Changed = 0;
	for (int32 Index = 0; Index < Refs.Num(); ++Index)
	{
		STMigration::FPlatformProposal& Each = Get(Refs[Index]);
		if (Each.SpreadChoices.Num() < 2 || Each.Items.Num() != 1)
		{
			continue;
		}
		TSet<UClass*>& Used = Taken.FindOrAdd(Find(Index));
		const TSubclassOf<UFGItemDescriptor>* Free = Each.SpreadChoices.FindByPredicate([&Used](const TSubclassOf<UFGItemDescriptor>& Item) { return !Used.Contains(Item.Get()); });
		const TSubclassOf<UFGItemDescriptor> Pick = Free ? *Free : Each.Items[0];
		Used.Add(Pick.Get());
		if (Pick == Each.Items[0])
		{
			continue;
		}
		TArray<FString> Others;
		for (const TSubclassOf<UFGItemDescriptor>& Item : Each.SpreadChoices)
		{
			if (Item != Pick)
			{
				Others.Add(STNames::Item(Item));
			}
		}
		const FString Old = Each.OneItemNote;
		Each.OneItemNote = FString::Printf(TEXT("belts also carry %s; one item per platform: one no other platform for the same %s has yet; check it"),
			*FString::Join(Others, TEXT(", ")), Each.Machines.Num() > 0 ? TEXT("machines") : TEXT("station"));
		Each.Note = Old.IsEmpty() || !Each.Note.Contains(Old) ? (Each.Note.IsEmpty() ? Each.OneItemNote : Each.OneItemNote + TEXT("; ") + Each.Note)
			: Each.Note.Replace(*Old, *Each.OneItemNote);
		Each.Confidence = TEXT("medium");
		UE_LOG(LogSmartTrains, Log, TEXT("MIGRATION %s platform %d: %s instead of %s, which another platform for the same machines already has"),
			*STLayout::Name(Preview.Stations[Refs[Index].Station].Station.Get()), Each.Index + 1, *STNames::Item(Pick), *STNames::Item(Each.Items[0]));
		Each.Items = {Pick};
		++Changed;
	}
	UE_LOG(LogSmartTrains, Log, TEXT("MIGRATION shared out items: %d platforms changed"), Changed);
}

namespace
{
	// Station names for the last preview, worked out a few stations per step (StepNaming) so the server keeps running.
	struct FNamingState
	{
		double MadeAt = -1.0;
		int32 Next = 0;
		TSet<FString> Taken;
		TWeakObjectPtr<USTRemoteCallObject> Requester;
		double StartedAt = 0.0;
		// The longest one station took, and which. [s]
		double LongestSeconds = 0.0;
		FString Longest;
	};
	TMap<TWeakObjectPtr<UWorld>, FNamingState> GNaming;
}

void STMigration::SetRequester(UWorld* World, USTRemoteCallObject* Requester)
{
	if (FNamingState* State = GNaming.Find(World))
	{
		State->Requester = Requester;
	}
}

bool STMigration::IsNaming(UWorld* World)
{
	const FPreview* Preview = GPreviews.Find(World);
	const FNamingState* State = GNaming.Find(World);
	return Preview && State && State->MadeAt == Preview->MadeAt && State->Next < Preview->Stations.Num();
}

bool STMigration::StepNaming(ASTSubsystem* Subsystem, UWorld* World, double BudgetMs)
{
	FPreview* Preview = GPreviews.Find(World);
	FNamingState* State = GNaming.Find(World);
	if (!Subsystem || !Preview || !State)
	{
		return true;
	}
	// A new preview starts the names again.
	if (State->MadeAt != Preview->MadeAt)
	{
		const TWeakObjectPtr<USTRemoteCallObject> Requester = State->Requester;
		*State = FNamingState();
		State->MadeAt = Preview->MadeAt;
		State->Requester = Requester;
		State->StartedAt = FPlatformTime::Seconds();
		if (AFGRailroadSubsystem* Railroad = AFGRailroadSubsystem::Get(World))
		{
			TArray<AFGTrainStationIdentifier*> All;
			Railroad->GetAllTrainStations(All);
			for (const AFGTrainStationIdentifier* Identifier : All)
			{
				if (IsValid(Identifier))
				{
					State->Taken.Add(Identifier->GetStationName().ToString());
				}
			}
		}
	}
	if (State->Next >= Preview->Stations.Num())
	{
		return true;
	}
	const double Until = FPlatformTime::Seconds() + BudgetMs / 1000.0;
	bool bFirst = true;
	while (State->Next < Preview->Stations.Num() && (bFirst || FPlatformTime::Seconds() < Until))
	{
		bFirst = false;
		FStationProposal& Station = Preview->Stations[State->Next++];
		const FString Old = Station.Station.IsValid() ? Station.Station->GetStationName().ToString() : FString();
		State->Taken.Remove(Old);
		const double StationStart = FPlatformTime::Seconds();
		STSetup::NameStation(*Subsystem, Station, State->Taken);
		const double Took = FPlatformTime::Seconds() - StationStart;
		if (Took > State->LongestSeconds)
		{
			State->LongestSeconds = Took;
			State->Longest = Old;
		}
		if (Station.NewName.IsEmpty())
		{
			State->Taken.Add(Old);
		}
	}
	if (State->Next < Preview->Stations.Num())
	{
		return false;
	}
	// Done: the names go to the log and the report, and to the window that asked for the preview.
	FString Report;
	int32 Renamed = 0;
	for (int32 Index = 0; Index < Preview->Stations.Num(); ++Index)
	{
		const FStationProposal& Station = Preview->Stations[Index];
		const FString Old = STLayout::Name(Station.Station.Get());
		Report += FString::Printf(TEXT("name,%d,%s,,,,%s,%s,,,\n"), Index + 1, *Csv(Old), *Csv(Station.NewName), *Csv(Station.NameNote));
		if (!Station.NewName.IsEmpty() && Station.NewName != Old)
		{
			++Renamed;
			UE_LOG(LogSmartTrains, Verbose, TEXT("MIGRATION S%d %s: name %s (%s)"), Index + 1, *Old, *Station.NewName, *Station.NameNote);
		}
	}
	if (!Preview->CsvPath.IsEmpty())
	{
		FFileHelper::SaveStringToFile(Report, *Preview->CsvPath, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
	}
	const FString Summary = FString::Printf(TEXT("Station names ready: %d of %d stations would get a new name with Rename stations on (%.1f s)."), Renamed, Preview->Stations.Num(),
		FPlatformTime::Seconds() - State->StartedAt);
	UE_LOG(LogSmartTrains, Log, TEXT("MIGRATION naming: the longest station took %.0f ms (%s)"), State->LongestSeconds * 1000.0, *State->Longest);
	UE_LOG(LogSmartTrains, Log, TEXT("MIGRATION %s"), *Summary);
	if (USTRemoteCallObject* Requester = State->Requester.Get())
	{
		Requester->PushMigrationView({Summary});
	}
	return true;
}

TArray<FString> STMigration::Preview(ASTSubsystem* Subsystem, UWorld* World)
{
	TArray<FString> Lines;
	AFGRailroadSubsystem* Railroad = World ? AFGRailroadSubsystem::Get(World) : nullptr;
	if (!Subsystem || !Railroad)
	{
		Lines.Add(TEXT("Migration: no railroad subsystem."));
		return Lines;
	}
	FPreview Preview;
	Preview.MadeAt = World->GetTimeSeconds();
	Preview.DttNote = STDtt::IsInstalled(World)
		? TEXT("DynamicTimeTable is installed: it rewrites managed trains' timetables, so uninstall it before managing trains")
		: TEXT("every train that can dock is ticked to join the managed pool; untick any you want to keep on its own timetable");

	// Trains: every one with a timetable that isn't managed yet, ticked when it can dock and isn't kept vanilla.
	TArray<AFGTrain*> Trains;
	Railroad->GetAllTrains(Trains);
	TMap<const AFGTrainStationIdentifier*, TArray<int32>> UsersOf;
	for (AFGTrain* Train : Trains)
	{
		AFGRailroadTimeTable* TimeTable = Train ? Train->GetTimeTable() : nullptr;
		const FSTTrainState* State = Subsystem->FindTrainState(Train);
		if (!TimeTable || (State && State->bManaged))
		{
			continue;
		}
		TArray<FTimeTableStop> Stops;
		TimeTable->GetStops(Stops);
		if (Stops.Num() == 0)
		{
			continue;
		}
		FTrainProposal Proposal;
		Proposal.Train = Train;
		for (const FTimeTableStop& Stop : Stops)
		{
			Proposal.Route += (Proposal.Route.IsEmpty() ? TEXT("") : TEXT(" > ")) + STLayout::Name(Stop.Station);
			Proposal.Stops.Add(Stop.Station);
		}
		const bool bKeepVanilla = State && State->bKeepVanilla;
		const bool bCanDock = STLayout::GetPossibleDockingLocomotives(Train).Num() > 0;
		Proposal.bPreselected = !bKeepVanilla && bCanDock;
		Proposal.Note = bKeepVanilla ? TEXT("kept vanilla") : (!bCanDock ? TEXT("no locomotive that can dock") : TEXT("joins the managed pool"));
		const int32 TrainIndex = Preview.Trains.Add(Proposal);
		for (const FTimeTableStop& Stop : Stops)
		{
			UsersOf.FindOrAdd(Stop.Station).AddUnique(TrainIndex);
		}
	}

	// Stations: each one's platforms set from what is attached, as the station menu's Autodetect does.
	TArray<AFGTrainStationIdentifier*> Stations;
	Railroad->GetAllTrainStations(Stations);
	for (AFGTrainStationIdentifier* Identifier : Stations)
	{
		if (!IsValid(Identifier))
		{
			continue;
		}
		FStationProposal Station = STSetup::ProposeStation(*Subsystem, Identifier);
		if (!Station.Platforms.ContainsByPredicate([](const FPlatformProposal& Each) { return Each.bCargo; }))
		{
			continue;
		}
		for (int32 TrainIndex : UsersOf.FindRef(Identifier))
		{
			Station.Users.Add(STLayout::Name(Preview.Trains[TrainIndex].Train.Get()));
		}
		for (const FPlatformProposal& Platform : Station.Platforms)
		{
			if (!Platform.Conflict.IsEmpty())
			{
				Station.Flag = FString::Printf(TEXT("platform %d: %s"), Platform.Index + 1, *Platform.Conflict);
				break;
			}
		}
		Preview.Stations.Add(MoveTemp(Station));
	}

	// Names from the station set-up (factory product, map area, number and rate) come a few stations per step
	// (StepNaming), so the server keeps running; the window gets them when they are ready.
	GNaming.FindOrAdd(World).MadeAt = -1.0;
	Subsystem->StartSetupSteps();

	// The report.
	FString Report = TEXT("Kind,Number,Station or train,Platform,Role,Items,Evidence,Flag or note,Used by,Confidence,Connection trace\n");
	int32 Ready = 0;
	for (int32 Index = 0; Index < Preview.Stations.Num(); ++Index)
	{
		const FStationProposal& Station = Preview.Stations[Index];
		Ready += Station.Flag.IsEmpty() ? 1 : 0;
		for (const FPlatformProposal& Platform : Station.Platforms)
		{
			Report += FString::Printf(TEXT("station,%d,%s,%d,%s,%s,%s,%s,%s,%s,%s\n"), Index + 1, *Csv(STLayout::Name(Station.Station.Get())), Platform.Index + 1,
				STNames::Role(Platform.Role), *Csv(ItemList(Platform.Items)), *Csv(Platform.Evidence), *Csv(!Platform.Conflict.IsEmpty() ? Platform.Conflict : Platform.Note),
				*Csv(FString::Join(Station.Users, TEXT("; "))), *Platform.Confidence, *Csv(Platform.TraceText));
		}
		Lines.Add(FString::Printf(TEXT("S%d %s"), Index + 1, *StationText(Station)));
	}
	int32 Ticked = 0;
	for (int32 Index = 0; Index < Preview.Trains.Num(); ++Index)
	{
		const FTrainProposal& Train = Preview.Trains[Index];
		Ticked += Train.bPreselected ? 1 : 0;
		Report += FString::Printf(TEXT("train,%d,%s,,%s,,%s,%s,,,\n"), Index + 1, *Csv(STLayout::Name(Train.Train.Get())), Train.bPreselected ? TEXT("manage") : TEXT("keep"),
			*Csv(Train.Route), *Csv(Train.Note));
		Lines.Add(FString::Printf(TEXT("T%d %s %s"), Index + 1, Train.bPreselected ? TEXT("[x]") : TEXT("[ ]"), *TrainText(Train)));
	}
	Preview.CsvPath = FPaths::Combine(FPaths::ProjectLogDir(), FString::Printf(TEXT("SmartTrains-migration-%s.csv"), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"))));
	const bool bWritten = FFileHelper::SaveStringToFile(Report, *Preview.CsvPath);
	Lines.Insert(FString::Printf(TEXT("Migration preview: %d stations (%d ready), %d trains (%d ticked); %s. Station names follow in a moment. Report %s %s"),
		Preview.Stations.Num(), Ready, Preview.Trains.Num(), Ticked, *Preview.DttNote, bWritten ? TEXT("written to") : TEXT("NOT written to"), *Preview.CsvPath), 0);
	Lines.Add(TEXT("Apply with /st migrate apply all [rename | namesonly], or /st migrate apply stations 1,2 trains 3. /st migrate undo puts it back."));
	UE_LOG(LogSmartTrains, Log, TEXT("MIGRATION preview: %d stations (%d ready), %d trains (%d ticked)"), Preview.Stations.Num(), Ready, Preview.Trains.Num(), Ticked);
	for (const FString& Line : Lines)
	{
		UE_LOG(LogSmartTrains, Verbose, TEXT("MIGRATION %s"), *Line);
	}
	GPreviews.Add(World, MoveTemp(Preview));
	return Lines;
}

TArray<FString> STMigration::Apply(ASTSubsystem* Subsystem, UWorld* World, const TArray<FString>& Selection, const FString& Cause)
{
	TArray<FString> Lines;
	FPreview* Preview = GPreviews.Find(World);
	if (!Subsystem || !Preview)
	{
		Lines.Add(TEXT("Run /st migrate preview first."));
		return Lines;
	}
	const bool bConfirmed = Selection.ContainsByPredicate([](const FString& Word) { return Word.Equals(TEXT("confirm"), ESearchCase::IgnoreCase); });
	if (!Subsystem->bMadeBackupThisSession && !bConfirmed)
	{
		Lines.Add(TEXT("Tip: /st testcopy saves this world as a separate session to try the migration in first (or /st backup for a backup)."));
		Lines.Add(TEXT("To go ahead here anyway, add 'confirm': /st migrate apply ... confirm"));
		return Lines;
	}

	TSet<int32> Stations;
	TSet<int32> Trains;
	bool bAll = false;
	for (int32 Index = 0; Index < Selection.Num(); ++Index)
	{
		const FString& Word = Selection[Index];
		if (Word.Equals(TEXT("all"), ESearchCase::IgnoreCase))
		{
			bAll = true;
		}
		else if (Word.StartsWith(TEXT("station"), ESearchCase::IgnoreCase) || Word.StartsWith(TEXT("train"), ESearchCase::IgnoreCase))
		{
			TArray<FString> Rest;
			for (int32 Next = Index + 1; Next < Selection.Num() && (Selection[Next].IsNumeric() || Selection[Next].Contains(TEXT(","))); ++Next)
			{
				Rest.Add(Selection[Next]);
			}
			for (int32 Number : ParseNumbers(Rest, 0))
			{
				(Word.StartsWith(TEXT("station"), ESearchCase::IgnoreCase) ? Stations : Trains).Add(Number - 1);
			}
		}
	}
	if (bAll)
	{
		for (int32 Index = 0; Index < Preview->Stations.Num(); ++Index)
		{
			if (Preview->Stations[Index].Flag.IsEmpty())
			{
				Stations.Add(Index);
			}
		}
		for (int32 Index = 0; Index < Preview->Trains.Num(); ++Index)
		{
			if (Preview->Trains[Index].bPreselected)
			{
				Trains.Add(Index);
			}
		}
	}
	if (Stations.Num() == 0 && Trains.Num() == 0)
	{
		Lines.Add(TEXT("Nothing selected: /st migrate apply all, or stations 1,2 and/or trains 3,4."));
		return Lines;
	}

	FSTMigrationUndo& Undo = Subsystem->GetMigrationUndo();
	Undo = FSTMigrationUndo();
	Undo.bValid = true;
	Undo.AppliedBy = Cause;
	int32 PlatformsSet = 0;
	int32 Renamed = 0;
	auto Has = [&Selection](const TCHAR* Wanted) { return Selection.ContainsByPredicate([Wanted](const FString& Word) { return Word.Equals(Wanted, ESearchCase::IgnoreCase); }); };
	// "namesonly": the station names alone (it implies rename); without "rename": the platforms alone.
	const bool bNamesOnly = Has(TEXT("namesonly"));
	const bool bRename = bNamesOnly || Has(TEXT("rename"));
	// Names still being worked out are finished first.
	if (bRename)
	{
		StepNaming(Subsystem, World, 1.0e9);
	}
	for (int32 Index : Stations)
	{
		if (!Preview->Stations.IsValidIndex(Index))
		{
			continue;
		}
		FStationProposal Proposal = Preview->Stations[Index];
		if (bNamesOnly)
		{
			Proposal.Platforms.Reset();
		}
		const TArray<FString> Done = STSetup::ApplyProposal(*Subsystem, Proposal, bRename, Cause + TEXT(" (migration)"), &Undo);
		for (const FString& Line : Done)
		{
			if (Line.StartsWith(TEXT("Renamed")))
			{
				++Renamed;
			}
			else
			{
				++PlatformsSet;
			}
		}
	}
	int32 TrainsManaged = 0;
	for (int32 Index : Trains)
	{
		AFGTrain* Train = Preview->Trains.IsValidIndex(Index) ? Preview->Trains[Index].Train.Get() : nullptr;
		FString Why;
		if (Train && Subsystem->SetTrainManaged(Train, true, Cause + TEXT(" (migration)"), Why))
		{
			Undo.Trains.Add(Train);
			++TrainsManaged;
		}
		else if (Train)
		{
			Lines.Add(FString::Printf(TEXT("%s not managed: %s"), *STLayout::Name(Train), *Why));
		}
	}
	Lines.Add(FString::Printf(TEXT("Migrated: %d platforms set on %d stations, %d stations renamed, %d trains managed. /st migrate undo puts it back."),
		PlatformsSet, Stations.Num(), Renamed, TrainsManaged));
	UE_LOG(LogSmartTrains, Log, TEXT("MIGRATION applied by %s: %d platforms, %d stations, %d renamed, %d trains managed"), *Cause, PlatformsSet, Stations.Num(), Renamed,
		TrainsManaged);
	return Lines;
}

TArray<FString> STMigration::Undo(ASTSubsystem* Subsystem, UWorld* World, const FString& Cause)
{
	TArray<FString> Lines;
	if (!Subsystem || !Subsystem->GetMigrationUndo().bValid)
	{
		Lines.Add(TEXT("Nothing to undo."));
		return Lines;
	}
	FSTMigrationUndo& Undo = Subsystem->GetMigrationUndo();
	int32 Trains = 0;
	for (AFGTrain* Train : Undo.Trains)
	{
		FString Why;
		if (IsValid(Train) && Subsystem->SetTrainManaged(Train, false, Cause + TEXT(" (migration undo)"), Why))
		{
			++Trains;
		}
	}
	int32 Platforms = 0;
	for (const FSTPlatformConfig& Before : Undo.Platforms)
	{
		AFGBuildableTrainPlatformCargo* Platform = Before.Platform;
		if (!IsValid(Platform))
		{
			continue;
		}
		Subsystem->SetPlatformConfig(Platform, Before.Role, Before.Items, Cause + TEXT(" (migration undo)"));
		if (Before.Role == ESTRole::Off && Before.bHasOriginalLoadMode)
		{
			Platform->SetIsInLoadMode(Before.bOriginalLoadMode);
		}
		++Platforms;
	}
	int32 Names = 0;
	for (int32 Index = 0; Index < Undo.RenamedStations.Num() && Index < Undo.OldNames.Num(); ++Index)
	{
		if (AFGTrainStationIdentifier* Station = Undo.RenamedStations[Index]; IsValid(Station))
		{
			Station->SetStationName(FText::FromString(Undo.OldNames[Index]));
			++Names;
		}
	}
	if (Names > 0)
	{
		Lines.Add(FString::Printf(TEXT("%d station names put back."), Names));
	}
	Undo = FSTMigrationUndo();
	Lines.Add(FString::Printf(TEXT("Migration undone: %d platforms and %d trains put back (keep-vanilla marks stay; clear them with /st train keepvanilla off)."), Platforms, Trains));
	UE_LOG(LogSmartTrains, Log, TEXT("MIGRATION undone by %s: %d platforms, %d trains"), *Cause, Platforms, Trains);
	return Lines;
}

FSTMigrationView STMigration::MakeView(ASTSubsystem* Subsystem, UWorld* World)
{
	FSTMigrationView View;
	View.bNeedsConfirm = Subsystem && !Subsystem->bMadeBackupThisSession;
	View.bCanUndo = Subsystem && Subsystem->GetMigrationUndo().bValid;
	View.bBetaTools = Subsystem && Subsystem->GetSettings().bBetaTools;
	const FPreview* Preview = GPreviews.Find(World);
	View.bHasPreview = Preview != nullptr;
	if (!Preview)
	{
		return View;
	}
	for (int32 Index = 0; Index < Preview->Stations.Num(); ++Index)
	{
		FSTMigrationRow& Row = View.Rows.AddDefaulted_GetRef();
		Row.bStation = true;
		Row.Number = Index + 1;
		const FStationProposal& Station = Preview->Stations[Index];
		Row.Text = StationText(Station);
		Row.Title = STLayout::Name(Station.Station.Get());
		Row.Flag = Station.Flag;
		for (const FPlatformProposal& Platform : Station.Platforms)
		{
			Row.Details.Add(PlatformLine(Platform));
			FSTMigrationPlatformRow& Each = Row.Platforms.AddDefaulted_GetRef();
			Each.Number = Platform.Index + 1;
			Each.Role = Platform.Role;
			Each.Items = Platform.Items;
			Each.Confidence = Platform.Role != ESTRole::Off ? Platform.Confidence : FString();
			Each.Note = Platform.Note;
			Each.Conflict = Platform.Conflict;
			const AFGBuildableTrainPlatformCargo* Cargo = Platform.Platform.Get();
			Each.bFluid = Cargo && STLayout::GetPlatformCargoType(Cargo) == EFreightCargoType::FCT_Liquid;
			Each.bCargo = Platform.bCargo;
		}
		Row.NewName = Station.NewName != Row.Title ? Station.NewName : FString();
		if (!Station.NewName.IsEmpty())
		{
			Row.Details.Add(Station.NewName == Row.Title ? FString::Printf(TEXT("Name stays %s (%s)"), *Row.Title, *Station.NameNote)
				: FString::Printf(TEXT("New name with Rename stations on: %s (%s)"), *Station.NewName, *Station.NameNote));
		}
		if (Station.Users.Num() > 0)
		{
			Row.Details.Add(TEXT("Used by: ") + FString::Join(Station.Users, TEXT(", ")));
			Row.Users = FString::Join(Station.Users, TEXT(", "));
		}
		Row.bProposed = Station.Flag.IsEmpty();
	}
	for (int32 Index = 0; Index < Preview->Trains.Num(); ++Index)
	{
		FSTMigrationRow& Row = View.Rows.AddDefaulted_GetRef();
		Row.bStation = false;
		Row.Number = Index + 1;
		const FTrainProposal& Train = Preview->Trains[Index];
		Row.Text = TrainText(Train);
		Row.Title = STLayout::Name(Train.Train.Get());
		Row.Details.Add(Train.Route);
		if (!Train.Note.IsEmpty())
		{
			Row.Details.Add(Train.Note);
		}
		Row.Users = Train.Route;
		Row.Note = Train.Note;
		Row.bProposed = Train.bPreselected;
	}
	return View;
}
