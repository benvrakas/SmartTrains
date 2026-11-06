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
#include "STTrace.h"

namespace
{
	TMap<TWeakObjectPtr<UWorld>, STMigration::FPreview> GPreviews;

	// A platform set by hand in the migration window.
	struct FHandSetting
	{
		ESTRole Role = ESTRole::Off;
		TArray<TSubclassOf<UFGItemDescriptor>> Items;
		FString Who;
	};
	TMap<TWeakObjectPtr<UWorld>, TMap<TWeakObjectPtr<AFGBuildableTrainPlatformCargo>, FHandSetting>> GHandSettings;

	const TCHAR* ByHand = TEXT("by hand");

	void UseHandSetting(STMigration::FPlatformProposal& Platform, const FHandSetting& Setting)
	{
		Platform.Role = Setting.Role;
		Platform.Items = Setting.Role == ESTRole::Off ? TArray<TSubclassOf<UFGItemDescriptor>>() : Setting.Items;
		Platform.Confidence = ByHand;
		Platform.Note = FString::Printf(TEXT("set by hand (%s)"), *Setting.Who);
		Platform.Conflict = Platform.Role != ESTRole::Off && Platform.Items.Num() == 0 ? FString(TEXT("no item chosen")) : FString();
	}

	// After a platform changed: a station flagged for a platform's problem takes the next platform's problem, or none.
	void UpdatePlatformFlag(STMigration::FStationProposal& Station)
	{
		if (!Station.Flag.IsEmpty() && !Station.Flag.StartsWith(TEXT("platform ")))
		{
			return;
		}
		Station.Flag.Reset();
		for (const STMigration::FPlatformProposal& Platform : Station.Platforms)
		{
			if (!Platform.Conflict.IsEmpty())
			{
				Station.Flag = FString::Printf(TEXT("platform %d: %s"), Platform.Index + 1, *Platform.Conflict);
				return;
			}
		}
	}

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

namespace
{
	const TCHAR* NoItemConflict = TEXT("no item found: the trace reached no machine, and it is empty and no timetable names one");

	// Route inference: a platform the other evidence left without items gets the items that trains stopping there
	// carry from or to the other stations on their timetables (loads elsewhere for a demand platform, deliveries
	// elsewhere for a supply platform). Repeats while it fills platforms, so items carry along chains of routes.
	// Platforms whose one item wasn't settled by their own stock share out the items their traces found. A group is the
	// platforms of one role whose traces reach a machine in common, across stations. A platform the trace reached no
	// machine from groups with its station's platforms of that role. Each platform takes the best item no other
	// platform of its group has yet. Groups with more platforms than items give the rest their best item again.
	void ShareOutItems(STMigration::FPreview& Preview)
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

	void InferFromRoutes(STMigration::FPreview& Preview, bool bOneItem)
	{
		TMap<const AFGTrainStationIdentifier*, int32> StationIndex;
		for (int32 Index = 0; Index < Preview.Stations.Num(); ++Index)
		{
			StationIndex.Add(Preview.Stations[Index].Station.Get(), Index);
		}
		int32 Filled = 0;
		for (int32 Pass = 0; Pass < 4; ++Pass)
		{
			int32 FilledThisPass = 0;
			for (const STMigration::FTrainProposal& Train : Preview.Trains)
			{
				// What the route's other stations supply and take, counted by platforms.
				TMap<UClass*, int32> Loaded;
				TMap<UClass*, int32> Delivered;
				for (const TWeakObjectPtr<AFGTrainStationIdentifier>& Stop : Train.Stops)
				{
					const int32* Index = StationIndex.Find(Stop.Get());
					if (!Index)
					{
						continue;
					}
					for (const STMigration::FPlatformProposal& Platform : Preview.Stations[*Index].Platforms)
					{
						for (const TSubclassOf<UFGItemDescriptor>& Item : Platform.Items)
						{
							if (Platform.Role == ESTRole::Supply)
							{
								++Loaded.FindOrAdd(Item.Get());
							}
							else if (Platform.Role == ESTRole::Demand)
							{
								++Delivered.FindOrAdd(Item.Get());
							}
						}
					}
				}
				for (const TWeakObjectPtr<AFGTrainStationIdentifier>& Stop : Train.Stops)
				{
					const int32* Index = StationIndex.Find(Stop.Get());
					if (!Index)
					{
						continue;
					}
					for (STMigration::FPlatformProposal& Platform : Preview.Stations[*Index].Platforms)
					{
						if (Platform.Role == ESTRole::Off || Platform.Items.Num() > 0 || Platform.Conflict != NoItemConflict || !Platform.Platform.IsValid())
						{
							continue;
						}
						const EFreightCargoType CargoType = STLayout::GetPlatformCargoType(Platform.Platform.Get());
						TArray<TPair<UClass*, int32>> Candidates;
						for (const TPair<UClass*, int32>& Each : Platform.Role == ESTRole::Demand ? Loaded : Delivered)
						{
							if (STLayout::ItemFitsCargoType(Each.Key, CargoType))
							{
								Candidates.Emplace(Each.Key, Each.Value);
							}
						}
						if (Candidates.Num() == 0)
						{
							continue;
						}
						Candidates.Sort([](const TPair<UClass*, int32>& A, const TPair<UClass*, int32>& B) { return A.Value > B.Value; });
						const int32 Take = bOneItem ? 1 : Candidates.Num();
						for (int32 Each = 0; Each < Take; ++Each)
						{
							Platform.Items.Add(Candidates[Each].Key);
						}
						Platform.Conflict.Reset();
						Platform.Confidence = TEXT("medium");
						Platform.Note = FString::Printf(TEXT("inferred from %s's route (%s)%s"), *STLayout::Name(Train.Train.Get()), *Train.Route,
							bOneItem && Candidates.Num() > 1 ? TEXT("; one item per platform: the one most platforms on the route move, check it") : TEXT(""));
						Platform.Evidence += TEXT(", route inference");
						UE_LOG(LogSmartTrains, Verbose, TEXT("MIGRATION route inference: %s platform %d gets %s from %s"), *STLayout::Name(Preview.Stations[*Index].Station.Get()),
							Platform.Index + 1, *STNames::Item(Platform.Items[0]), *STLayout::Name(Train.Train.Get()));
						++FilledThisPass;
					}
				}
			}
			Filled += FilledThisPass;
			if (FilledThisPass == 0)
			{
				break;
			}
		}
		if (Filled == 0)
		{
			return;
		}
		// Flags that came from a platform conflict are worked out again.
		for (STMigration::FStationProposal& Station : Preview.Stations)
		{
			if (!Station.Flag.StartsWith(TEXT("platform ")))
			{
				continue;
			}
			Station.Flag.Reset();
			for (const STMigration::FPlatformProposal& Platform : Station.Platforms)
			{
				if (!Platform.Conflict.IsEmpty())
				{
					Station.Flag = FString::Printf(TEXT("platform %d: %s"), Platform.Index + 1, *Platform.Conflict);
					break;
				}
			}
		}
		UE_LOG(LogSmartTrains, Log, TEXT("MIGRATION route inference filled %d platforms"), Filled);
	}
}

TArray<FString> STMigration::Preview(ASTSubsystem* Subsystem, UWorld* World, int32 TraceLimit, bool bOneItem)
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
		Preview.DttNote = TEXT("DynamicTimeTable not installed: every train that can dock is ticked to join the managed pool; untick any you want to keep on its own timetable");
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
			Proposal.Stops.Add(Stop.Station);
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
		const bool bCanDock = STLayout::GetPossibleDockingLocomotives(Proposal.Train.Get()).Num() > 0;
		// With DynamicTimeTable groups, pull-based trains are ticked and dedicated ones kept. Without them every train
		// joins the managed pool (a train that used to shuttle simply takes other trips when its route is stocked).
		Proposal.bPreselected = !bKeepVanilla && bCanDock && (Preview.bDttRead ? Proposal.bPullBased : true);
		Proposal.Note = bKeepVanilla ? TEXT("kept vanilla") : (!bCanDock ? TEXT("no locomotive that can dock")
			: (Proposal.bPullBased ? TEXT("pull-based (stops in a DynamicTimeTable group)")
			: (Preview.bDttRead ? TEXT("dedicated: kept as is") : TEXT("joins the managed pool"))));
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
			if (!STLayout::CanMoveCargo(Cargo))
			{
				Platform.bCargo = false;
				Platform.Evidence = FString::Printf(TEXT("%s: no inventory or no belt or pipe connectors, can't move cargo"), *Cargo->GetClass()->GetName());
				UE_LOG(LogSmartTrains, Log, TEXT("MIGRATION %s platform %d (%s) can't move cargo: listed without a role"), *STLayout::Name(Identifier), Index + 1,
					*Cargo->GetClass()->GetName());
				Station.Platforms.Add(Platform);
				continue;
			}
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
			// Backups: what the platform holds now, and what timetable filters move here.
			TSet<UClass*> Backup;
			TMap<UClass*, int32> Held;
			if (const UFGInventoryComponent* Inventory = Cargo->GetInventory())
			{
				TArray<FInventoryStack> Stacks;
				Inventory->GetInventoryStacks(Stacks);
				for (const FInventoryStack& Stack : Stacks)
				{
					if (Stack.HasItems() && Stack.Item.GetItemClass())
					{
						Backup.Add(Stack.Item.GetItemClass());
						Held.FindOrAdd(Stack.Item.GetItemClass()) += Stack.NumItems;
					}
				}
			}
			const TSet<UClass*> Scheduled = (bLoadMode ? LoadItemsAt : UnloadItemsAt).FindRef(Identifier);
			// One item per platform: from several, the one it holds most of, then one its timetables send, then the
			// largest traced flow. Returns how it was chosen.
			auto ChooseOne = [&Held, &Scheduled](const TArray<UClass*>& Choices, const TMap<UClass*, float>& Flow, UClass*& OutItem) -> FString
			{
				OutItem = nullptr;
				int32 Most = 0;
				for (UClass* Item : Choices)
				{
					if (Held.FindRef(Item) > Most)
					{
						Most = Held.FindRef(Item);
						OutItem = Item;
					}
				}
				if (OutItem)
				{
					return TEXT("the one it holds most of");
				}
				for (UClass* Item : Choices)
				{
					if (Scheduled.Contains(Item))
					{
						OutItem = Item;
						return TEXT("the one its timetables send");
					}
				}
				float Largest = -1.f;
				for (UClass* Item : Choices)
				{
					if (Flow.FindRef(Item) > Largest)
					{
						Largest = Flow.FindRef(Item);
						OutItem = Item;
					}
				}
				return TEXT("the largest flow; check it");
			};
			// The choices in ChooseOne's order after stock: the ones its timetables send, then by traced flow.
			auto SpreadOrder = [&Scheduled](const TArray<UClass*>& Choices, const TMap<UClass*, float>& Flow)
			{
				TArray<UClass*> Order = Choices;
				Order.StableSort([&](const UClass& A, const UClass& B)
				{
					const bool bA = Scheduled.Contains(&A);
					const bool bB = Scheduled.Contains(&B);
					return bA != bB ? bA : Flow.FindRef(&A) > Flow.FindRef(&B);
				});
				return TArray<TSubclassOf<UFGItemDescriptor>>(Order);
			};
			const int32 FromInventory = Backup.Num();
			for (UClass* Item : (bLoadMode ? LoadItemsAt : UnloadItemsAt).FindRef(Identifier))
			{
				if (STLayout::ItemFitsCargoType(Item, STLayout::GetPlatformCargoType(Cargo)))
				{
					Backup.Add(Item);
				}
			}
			const int32 FromFilters = Backup.Num() - FromInventory;

			// The connection trace is the source of truth: what the machines behind the belts and pipes make or use.
			// The backups are only used when the trace finds nothing.
			TSet<UClass*> Traced;
			FString Source = TEXT("none");
			bool bChoseByFlow = false;
			if (Platform.Role != ESTRole::Off)
			{
				const STTrace::FResult Trace = STTrace::Trace(Cargo, bLoadMode, TraceLimit);
				Platform.TraceText = Trace.Summary();
				const EFreightCargoType CargoType = STLayout::GetPlatformCargoType(Cargo);
				for (const TSubclassOf<UFGItemDescriptor>& Item : Trace.Items())
				{
					if (Item && STLayout::ItemFitsCargoType(Item.Get(), CargoType))
					{
						Traced.Add(Item.Get());
					}
				}
				if (Traced.Num() > 0)
				{
					Source = TEXT("trace");
					Platform.Machines = Trace.MachineActors;
					if (bOneItem && Traced.Num() > 1)
					{
						TMap<UClass*, float> Flow;
						for (const STTrace::FFlow& Each : Trace.Flows)
						{
							Flow.FindOrAdd(Each.Item.Get()) += Each.PerMinute;
						}
						UClass* Chosen = nullptr;
						const FString How = ChooseOne(Traced.Array(), Flow, Chosen);
						if (!How.StartsWith(TEXT("the one it holds")))
						{
							Platform.SpreadChoices = SpreadOrder(Traced.Array(), Flow);
						}
						TArray<FString> Others;
						for (UClass* Item : Traced)
						{
							if (Item != Chosen)
							{
								Others.Add(STNames::Item(Item));
							}
						}
						Platform.Items.Add(Chosen);
						Platform.OneItemNote = FString::Printf(TEXT("belts also carry %s; one item per platform: %s"), *FString::Join(Others, TEXT(", ")), *How);
						Traced = {Chosen};
						bChoseByFlow = How.StartsWith(TEXT("the largest"));
					}
					else
					{
						for (UClass* Item : Traced)
						{
							Platform.Items.Add(Item);
						}
					}
					TArray<FString> Ignored;
					for (UClass* Item : Backup)
					{
						if (!Traced.Contains(Item))
						{
							Ignored.Add(STNames::Item(Item));
						}
					}
					bool bConfirmed = false;
					for (UClass* Item : Traced)
					{
						bConfirmed |= Backup.Contains(Item);
					}
					// A clean trace is high; an unclean one the platform's stock or timetables confirm is high too.
					Platform.Confidence = bChoseByFlow ? TEXT("medium")
						: (Trace.Confidence == STTrace::EConfidence::High || bConfirmed ? TEXT("high") : STTrace::ConfidenceName(Trace.Confidence));
					if (Ignored.Num() > 0)
					{
						Platform.Note = FString::Printf(TEXT("also holds or is sent %s, not on its belts: ignored"), *FString::Join(Ignored, TEXT(", ")));
					}
					if (!Platform.OneItemNote.IsEmpty())
					{
						Platform.Note = Platform.Note.IsEmpty() ? Platform.OneItemNote : Platform.OneItemNote + TEXT("; ") + Platform.Note;
					}
				}
				else if (Backup.Num() > 0)
				{
					Source = TEXT("backup");
					Platform.Confidence = TEXT("medium");
					Platform.Note = TEXT("trace found nothing; items from its inventory and timetables");
					if (bOneItem && Backup.Num() > 1)
					{
						UClass* Chosen = nullptr;
						const FString How = ChooseOne(Backup.Array(), TMap<UClass*, float>(), Chosen);
						if (!How.StartsWith(TEXT("the one it holds")))
						{
							Platform.SpreadChoices = SpreadOrder(Backup.Array(), TMap<UClass*, float>());
						}
						Platform.Items.Add(Chosen);
						Platform.Note += FString::Printf(TEXT("; one item per platform: %s"), *How);
					}
					else
					{
						for (UClass* Item : Backup)
						{
							Platform.Items.Add(Item);
						}
					}
				}
				else if (Trace.PipeFluidHint && STLayout::ItemFitsCargoType(Trace.PipeFluidHint.Get(), CargoType))
				{
					Source = TEXT("pipe contents");
					Platform.Items.Add(Trace.PipeFluidHint.Get());
					Platform.Confidence = TEXT("medium");
					Platform.Note = TEXT("only the fluid now in its pipes");
				}
				else
				{
					Platform.Confidence = TEXT("unclear");
				}
			}
			Platform.Evidence += FString::Printf(TEXT(", items from: %s (trace %d, inventory %d, timetable filters %d)"), *Source, Traced.Num(), FromInventory, FromFilters);
			if (Platform.Role != ESTRole::Off && Platform.Items.Num() == 0 && Platform.Conflict.IsEmpty())
			{
				Platform.Conflict = NoItemConflict;
			}
			Station.Platforms.Add(Platform);
		}
		if (!Station.Platforms.ContainsByPredicate([](const FPlatformProposal& Each) { return Each.bCargo; }))
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

	ShareOutItems(Preview);
	InferFromRoutes(Preview, bOneItem);

	// Platforms set by hand in the migration window keep that setting.
	if (const TMap<TWeakObjectPtr<AFGBuildableTrainPlatformCargo>, FHandSetting>* Hand = GHandSettings.Find(World))
	{
		int32 Used = 0;
		for (FStationProposal& Station : Preview.Stations)
		{
			bool bChanged = false;
			for (FPlatformProposal& Platform : Station.Platforms)
			{
				if (const FHandSetting* Setting = Platform.bCargo ? Hand->Find(Platform.Platform) : nullptr)
				{
					UseHandSetting(Platform, *Setting);
					bChanged = true;
					++Used;
				}
			}
			if (bChanged)
			{
				UpdatePlatformFlag(Station);
			}
		}
		UE_LOG(LogSmartTrains, Log, TEXT("MIGRATION %d platforms keep their setting by hand"), Used);
	}

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
				STNames::Role(Platform.Role), *Csv(ItemList(Platform.Items)), *Csv(Platform.Evidence), *Csv(!Platform.Conflict.IsEmpty() ? Platform.Conflict : (!Platform.Note.IsEmpty() ? Platform.Note : Station.Flag)),
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
	Lines.Insert(FString::Printf(TEXT("Migration preview: %d stations (%d ready), %d trains (%d ticked). %s. Report %s %s"), Preview.Stations.Num(), Ready,
		Preview.Trains.Num(), Ticked, *Preview.DttNote, bWritten ? TEXT("written to") : TEXT("NOT written to"), *Preview.CsvPath), 0);
	Lines.Add(TEXT("Apply with /st migrate apply all, or /st migrate apply stations 1,2 trains 3. /st migrate undo puts it back."));
	UE_LOG(LogSmartTrains, Log, TEXT("MIGRATION trace limit %d buildings and belts per platform, %s"), TraceLimit, bOneItem ? TEXT("one item per platform") : TEXT("all traced items"));
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
			Subsystem->SetPlatformConfig(Cargo, Platform.Role, Rules, Cause + TEXT(" (migration)"));
			++PlatformsSet;
		}
		// Applied: its platforms' settings by hand are done with.
		if (TMap<TWeakObjectPtr<AFGBuildableTrainPlatformCargo>, FHandSetting>* Hand = GHandSettings.Find(World))
		{
			for (const FPlatformProposal& Platform : Preview->Stations[Index].Platforms)
			{
				Hand->Remove(Platform.Platform);
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
		Subsystem->SetPlatformConfig(Platform, Before.Role, Before.Items, Cause + TEXT(" (migration undo)"));
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

bool STMigration::SetPlatform(UWorld* World, int32 Station, int32 Platform, ESTRole Role, const TArray<TSubclassOf<UFGItemDescriptor>>& Items, const FString& Who,
	FString& OutMessage)
{
	FPreview* Preview = GPreviews.Find(World);
	FStationProposal* Proposal = Preview && Preview->Stations.IsValidIndex(Station - 1) ? &Preview->Stations[Station - 1] : nullptr;
	FPlatformProposal* Target = Proposal ? Proposal->Platforms.FindByPredicate([Platform](const FPlatformProposal& Each) { return Each.Index + 1 == Platform; }) : nullptr;
	AFGBuildableTrainPlatformCargo* Cargo = Target ? Target->Platform.Get() : nullptr;
	if (!Cargo)
	{
		OutMessage = FString::Printf(TEXT("Station %d platform %d isn't in the preview any more; press Preview again."), Station, Platform);
		return false;
	}
	if (!Target->bCargo)
	{
		OutMessage = FString::Printf(TEXT("Station %d platform %d can't move freight or fluid, so it can't have a role."), Station, Platform);
		return false;
	}
	const EFreightCargoType CargoType = STLayout::GetPlatformCargoType(Cargo);
	FHandSetting Setting;
	Setting.Role = Role;
	Setting.Who = Who;
	for (const TSubclassOf<UFGItemDescriptor>& Item : Items)
	{
		if (!Item && Role != ESTRole::Demand)
		{
			OutMessage = TEXT("'Any item' only works on a demand platform (a sink).");
			return false;
		}
		if (Item && !STLayout::ItemFitsCargoType(Item, CargoType))
		{
			OutMessage = FString::Printf(TEXT("%s doesn't fit this kind of platform."), *STNames::Item(Item));
			return false;
		}
		Setting.Items.AddUnique(Item);
	}
	UseHandSetting(*Target, Setting);
	UpdatePlatformFlag(*Proposal);
	GHandSettings.FindOrAdd(World).Add(Cargo, Setting);
	const FString Name = STLayout::Name(Proposal->Station.Get());
	OutMessage = FString::Printf(TEXT("S%d %s, platform %d: %s %s (by hand; applied with its station)."), Station, *Name, Platform, STNames::Role(Target->Role),
		Target->Items.Num() > 0 ? *ItemList(Target->Items) : TEXT("with no item"));
	UE_LOG(LogSmartTrains, Log, TEXT("MIGRATION %s set S%d %s platform %d by hand: %s %s"), *Who, Station, *Name, Platform, STNames::Role(Target->Role), *ItemList(Target->Items));
	return true;
}

int32 STMigration::ParseTraceLimit(const TArray<FString>& Words, int32 Default)
{
	for (int32 Index = 0; Index + 1 < Words.Num(); ++Index)
	{
		if (Words[Index].Equals(TEXT("tracelimit"), ESearchCase::IgnoreCase) && Words[Index + 1].IsNumeric())
		{
			return FMath::Clamp(FCString::Atoi(*Words[Index + 1]), STTrace::MinLimit, STTrace::MaxLimit);
		}
	}
	return Default;
}

bool STMigration::ParseOneItem(const TArray<FString>& Words)
{
	return !Words.ContainsByPredicate([](const FString& Word) { return Word.Equals(TEXT("allitems"), ESearchCase::IgnoreCase); });
}
