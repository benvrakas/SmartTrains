#include "STMigration.h"
#include "Buildables/FGBuildableRailroadStation.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "Engine/World.h"
#include "FGInventoryComponent.h"
#include "FGRailroadSubsystem.h"
#include "FGRailroadTimeTable.h"
#include "FGTrain.h"
#include "FGTrainStationIdentifier.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SmartTrains.h"
#include "STBridges.h"
#include "STLayout.h"
#include "STSubsystem.h"

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
			Layout += FString::Printf(TEXT(" P%d=%s[%s]"), Platform.Index + 1, STNames::Role(Platform.Role), *ItemList(Platform.Items));
		}
		return FString::Printf(TEXT("%s:%s%s"), *STLayout::Name(Station.Station.Get()), *Layout,
			Station.Flag.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("  [%s]"), *Station.Flag));
	}

	// "Name: route (note)" for one proposed train.
	FString TrainText(const STMigration::FTrainProposal& Train)
	{
		return FString::Printf(TEXT("%s: %s (%s)"), *STLayout::Name(Train.Train.Get()), *Train.Route, *Train.Note);
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

	// DynamicTimeTable groups tell pull-based trains from dedicated ones.
	TSet<const AFGTrainStationIdentifier*> GroupMembers;
	TMap<const AFGTrainStationIdentifier*, FString> GroupOf;
	if (STDtt::IsInstalled(World))
	{
		TArray<STDtt::FGroup> Groups;
		FString Error;
		Preview.bDttRead = STDtt::ReadGroups(World, Groups, Error);
		Preview.DttNote = Preview.bDttRead ? FString::Printf(TEXT("%d DynamicTimeTable groups"), Groups.Num()) : TEXT("DynamicTimeTable installed but unreadable: ") + Error;
		for (const STDtt::FGroup& Group : Groups)
		{
			for (const AFGTrainStationIdentifier* Station : Group.Stations)
			{
				GroupMembers.Add(Station);
				GroupOf.Add(Station, Group.Name);
			}
		}
	}
	else
	{
		Preview.DttNote = TEXT("DynamicTimeTable not installed: no train is ticked, pick them with /st migrate apply trains <numbers>");
	}

	TArray<AFGTrain*> Trains;
	Railroad->GetAllTrains(Trains);
	TMap<const AFGTrainStationIdentifier*, TArray<int32>> UsersOf;
	TMap<const AFGTrainStationIdentifier*, TSet<UClass*>> LoadItemsAt;
	TMap<const AFGTrainStationIdentifier*, TSet<UClass*>> UnloadItemsAt;
	for (AFGTrain* Train : Trains)
	{
		AFGRailroadTimeTable* TimeTable = Train ? Train->GetTimeTable() : nullptr;
		if (!TimeTable)
		{
			continue;
		}
		const FSTTrainState* State = Subsystem->FindTrainState(Train);
		if (State && State->bManaged)
		{
			continue;
		}
		TArray<FTimeTableStop> Stops;
		TimeTable->GetStops(Stops);
		FTrainProposal Proposal;
		Proposal.Train = Train;
		for (const FTimeTableStop& Stop : Stops)
		{
			Proposal.Route += (Proposal.Route.IsEmpty() ? TEXT("") : TEXT(" > ")) + STLayout::Name(Stop.Station);
			Proposal.bPullBased |= GroupMembers.Contains(Stop.Station);
			for (const TSubclassOf<UFGItemDescriptor>& Item : Stop.DockingRuleSet.LoadFilterDescriptors)
			{
				if (Item)
				{
					LoadItemsAt.FindOrAdd(Stop.Station).Add(Item.Get());
				}
			}
			for (const TSubclassOf<UFGItemDescriptor>& Item : Stop.DockingRuleSet.UnloadFilterDescriptors)
			{
				if (Item)
				{
					UnloadItemsAt.FindOrAdd(Stop.Station).Add(Item.Get());
				}
			}
		}
		if (Stops.Num() == 0)
		{
			continue;
		}
		const bool bKeepVanilla = State && State->bKeepVanilla;
		Proposal.bPreselected = Preview.bDttRead && Proposal.bPullBased && !bKeepVanilla;
		Proposal.Note = bKeepVanilla ? TEXT("kept vanilla") : (Proposal.bPullBased ? TEXT("pull-based (stops in a DynamicTimeTable group)")
			: (Preview.bDttRead ? TEXT("dedicated: kept as is") : TEXT("not ticked (no DynamicTimeTable to tell)")));
		const int32 TrainIndex = Preview.Trains.Add(Proposal);
		for (const FTimeTableStop& Stop : Stops)
		{
			UsersOf.FindOrAdd(Stop.Station).AddUnique(TrainIndex);
		}
	}

	TArray<AFGTrainStationIdentifier*> Stations;
	Railroad->GetAllTrainStations(Stations);
	for (AFGTrainStationIdentifier* Identifier : Stations)
	{
		AFGBuildableRailroadStation* Building = Identifier ? Identifier->GetStation() : nullptr;
		const TArray<AFGBuildableTrainPlatform*> Platforms = STLayout::GetPlatforms(Building);
		FStationProposal Station;
		Station.Station = Identifier;
		bool bAnyPull = false;
		bool bAnyDedicated = false;
		for (int32 TrainIndex : UsersOf.FindRef(Identifier))
		{
			const FTrainProposal& User = Preview.Trains[TrainIndex];
			bAnyPull |= User.bPullBased;
			bAnyDedicated |= !User.bPullBased;
			Station.Users.Add(STLayout::Name(User.Train.Get()));
		}
		for (int32 Index = 0; Index < Platforms.Num(); ++Index)
		{
			AFGBuildableTrainPlatformCargo* Cargo = Cast<AFGBuildableTrainPlatformCargo>(Platforms[Index]);
			if (!Cargo)
			{
				continue;
			}
			FPlatformProposal Platform;
			Platform.Platform = Cargo;
			Platform.Index = Index;
			const bool bLoadMode = Cargo->GetIsInLoadMode();
			int32 Inputs = 0;
			int32 Outputs = 0;
			FSTPlatformAccess::CountConnections(Cargo, Inputs, Outputs);
			Platform.Evidence = FString::Printf(TEXT("%s mode, %d inputs and %d outputs connected"), bLoadMode ? TEXT("load") : TEXT("unload"), Inputs, Outputs);
			if (Inputs == 0 && Outputs == 0)
			{
				Platform.Role = ESTRole::Off;
				Platform.Evidence += TEXT(", nothing attached");
			}
			else
			{
				Platform.Role = bLoadMode ? ESTRole::Supply : ESTRole::Demand;
				if (bLoadMode && Inputs == 0)
				{
					Platform.Conflict = TEXT("set to load but nothing feeds it");
				}
				else if (!bLoadMode && Outputs == 0)
				{
					Platform.Conflict = TEXT("set to unload but nothing takes from it");
				}
			}
			TSet<UClass*> Items;
			if (const UFGInventoryComponent* Inventory = Cargo->GetInventory())
			{
				TArray<FInventoryStack> Stacks;
				Inventory->GetInventoryStacks(Stacks);
				for (const FInventoryStack& Stack : Stacks)
				{
					if (Stack.HasItems() && Stack.Item.GetItemClass())
					{
						Items.Add(Stack.Item.GetItemClass());
					}
				}
			}
			const int32 FromInventory = Items.Num();
			for (UClass* Item : (bLoadMode ? LoadItemsAt : UnloadItemsAt).FindRef(Identifier))
			{
				if (STLayout::ItemFitsCargoType(Item, STLayout::GetPlatformCargoType(Cargo)))
				{
					Items.Add(Item);
				}
			}
			for (UClass* Item : Items)
			{
				Platform.Items.Add(Item);
			}
			Platform.Evidence += FString::Printf(TEXT(", items: %d in its inventory, %d from timetable filters"), FromInventory, Items.Num() - FromInventory);
			if (Platform.Role != ESTRole::Off && Platform.Items.Num() == 0 && Platform.Conflict.IsEmpty())
			{
				Platform.Conflict = TEXT("no item found (empty and no train filter names one)");
			}
			Station.Platforms.Add(Platform);
		}
		if (Station.Platforms.Num() == 0)
		{
			continue;
		}
		if (Preview.bDttRead)
		{
			if (!bAnyPull && bAnyDedicated)
			{
				Station.bDedicatedOnly = true;
				Station.Flag = TEXT("used only by dedicated trains: kept vanilla");
			}
			else if (bAnyPull && bAnyDedicated)
			{
				Station.Flag = TEXT("shared by pull-based and dedicated trains: left unset");
			}
			else if (!bAnyPull)
			{
				Station.Flag = TEXT("no train stops here");
			}
		}
		for (const FPlatformProposal& Platform : Station.Platforms)
		{
			if (!Platform.Conflict.IsEmpty() && Station.Flag.IsEmpty())
			{
				Station.Flag = FString::Printf(TEXT("platform %d: %s"), Platform.Index + 1, *Platform.Conflict);
			}
		}
		if (const FString* Group = GroupOf.Find(Identifier))
		{
			Station.Users.Insert(FString::Printf(TEXT("group '%s'"), **Group), 0);
		}
		Preview.Stations.Add(MoveTemp(Station));
	}

	// The report.
	FString Report = TEXT("Kind,Number,Station or train,Platform,Role,Items,Evidence,Flag or note,Used by\n");
	int32 Ready = 0;
	for (int32 Index = 0; Index < Preview.Stations.Num(); ++Index)
	{
		const FStationProposal& Station = Preview.Stations[Index];
		Ready += Station.Flag.IsEmpty() ? 1 : 0;
		for (const FPlatformProposal& Platform : Station.Platforms)
		{
			Report += FString::Printf(TEXT("station,%d,%s,%d,%s,%s,%s,%s,%s\n"), Index + 1, *Csv(STLayout::Name(Station.Station.Get())), Platform.Index + 1,
				STNames::Role(Platform.Role), *Csv(ItemList(Platform.Items)), *Csv(Platform.Evidence), *Csv(Platform.Conflict.IsEmpty() ? Station.Flag : Platform.Conflict),
				*Csv(FString::Join(Station.Users, TEXT("; "))));
		}
		Lines.Add(FString::Printf(TEXT("S%d %s"), Index + 1, *StationText(Station)));
	}
	int32 Ticked = 0;
	for (int32 Index = 0; Index < Preview.Trains.Num(); ++Index)
	{
		const FTrainProposal& Train = Preview.Trains[Index];
		Ticked += Train.bPreselected ? 1 : 0;
		Report += FString::Printf(TEXT("train,%d,%s,,%s,,%s,%s,\n"), Index + 1, *Csv(STLayout::Name(Train.Train.Get())), Train.bPreselected ? TEXT("manage") : TEXT("keep"),
			*Csv(Train.Route), *Csv(Train.Note));
		Lines.Add(FString::Printf(TEXT("T%d %s %s"), Index + 1, Train.bPreselected ? TEXT("[x]") : TEXT("[ ]"), *TrainText(Train)));
	}
	Preview.CsvPath = FPaths::Combine(FPaths::ProjectLogDir(), FString::Printf(TEXT("SmartTrains-migration-%s.csv"), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"))));
	const bool bWritten = FFileHelper::SaveStringToFile(Report, *Preview.CsvPath);
	Lines.Insert(FString::Printf(TEXT("Migration preview: %d stations (%d ready), %d trains (%d ticked). %s. Report %s %s"), Preview.Stations.Num(), Ready,
		Preview.Trains.Num(), Ticked, *Preview.DttNote, bWritten ? TEXT("written to") : TEXT("NOT written to"), *Preview.CsvPath), 0);
	Lines.Add(TEXT("Apply with /st migrate apply all, or /st migrate apply stations 1,2 trains 3. /st migrate undo puts it back."));
	UE_LOG(LogSmartTrains, Log, TEXT("MIGRATION preview: %d stations (%d ready), %d trains (%d ticked), %s, report %s"), Preview.Stations.Num(), Ready,
		Preview.Trains.Num(), Ticked, *Preview.DttNote, *Preview.CsvPath);
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
	for (int32 Index : Stations)
	{
		if (!Preview->Stations.IsValidIndex(Index))
		{
			continue;
		}
		for (const FPlatformProposal& Platform : Preview->Stations[Index].Platforms)
		{
			AFGBuildableTrainPlatformCargo* Cargo = Platform.Platform.Get();
			if (!Cargo || Platform.Role == ESTRole::Off || Platform.Items.Num() == 0)
			{
				continue;
			}
			FSTPlatformConfig Before;
			if (const FSTPlatformConfig* Existing = Subsystem->FindPlatformConfig(Cargo))
			{
				Before = *Existing;
			}
			Before.Platform = Cargo;
			Before.bOriginalLoadMode = Cargo->GetIsInLoadMode();
			Before.bHasOriginalLoadMode = true;
			Undo.Platforms.Add(Before);
			TArray<FSTItemRule> Rules;
			for (const TSubclassOf<UFGItemDescriptor>& Item : Platform.Items)
			{
				FSTItemRule& Rule = Rules.AddDefaulted_GetRef();
				Rule.Item = Item;
			}
			Subsystem->SetPlatformConfig(Cargo, Platform.Role, Rules, Cause + TEXT(" (migration)"), false);
			++PlatformsSet;
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
	int32 KeptVanilla = 0;
	if (Preview->bDttRead)
	{
		for (const FTrainProposal& Proposal : Preview->Trains)
		{
			const FSTTrainState* State = Subsystem->FindTrainState(Proposal.Train.Get());
			if (Proposal.Train.IsValid() && !Proposal.bPullBased && !(State && State->bManaged))
			{
				Subsystem->SetTrainKeepVanilla(Proposal.Train.Get(), true, Cause + TEXT(" (migration: dedicated train)"));
				++KeptVanilla;
			}
		}
	}
	Lines.Add(FString::Printf(TEXT("Migrated: %d platforms set on %d stations, %d trains managed, %d dedicated trains marked keep vanilla. /st migrate undo puts it back."),
		PlatformsSet, Stations.Num(), TrainsManaged, KeptVanilla));
	UE_LOG(LogSmartTrains, Log, TEXT("MIGRATION applied by %s: %d platforms, %d stations, %d trains managed, %d kept vanilla"), *Cause, PlatformsSet, Stations.Num(),
		TrainsManaged, KeptVanilla);

	// DynamicTimeTable groups whose members are all SmartTrains stations now can be deleted by the player.
	if (Preview->bDttRead)
	{
		TSet<const AFGTrainStationIdentifier*> Converted;
		for (int32 Index : Stations)
		{
			if (Preview->Stations.IsValidIndex(Index))
			{
				Converted.Add(Preview->Stations[Index].Station.Get());
			}
		}
		TArray<STDtt::FGroup> Groups;
		FString Error;
		STDtt::ReadGroups(World, Groups, Error);
		for (const STDtt::FGroup& Group : Groups)
		{
			bool bAllConverted = Group.Stations.Num() > 0;
			for (const AFGTrainStationIdentifier* Member : Group.Stations)
			{
				bAllConverted &= Converted.Contains(Member);
			}
			if (bAllConverted)
			{
				Lines.Add(FString::Printf(TEXT("DynamicTimeTable group '%s' is no longer needed; you can delete it in DynamicTimeTable."), *Group.Name));
			}
		}
	}
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
		Subsystem->SetPlatformConfig(Platform, Before.Role, Before.Items, Cause + TEXT(" (migration undo)"), false);
		if (Before.Role == ESTRole::Off && Before.bHasOriginalLoadMode)
		{
			Platform->SetIsInLoadMode(Before.bOriginalLoadMode);
		}
		++Platforms;
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
		Row.Text = StationText(Preview->Stations[Index]);
		Row.bProposed = Preview->Stations[Index].Flag.IsEmpty();
	}
	for (int32 Index = 0; Index < Preview->Trains.Num(); ++Index)
	{
		FSTMigrationRow& Row = View.Rows.AddDefaulted_GetRef();
		Row.bStation = false;
		Row.Number = Index + 1;
		Row.Text = TrainText(Preview->Trains[Index]);
		Row.bProposed = Preview->Trains[Index].bPreselected;
	}
	return View;
}
