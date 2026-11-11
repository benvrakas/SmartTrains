#include "STStationSetup.h"
#include "Buildables/FGBuildableConveyorBase.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildablePipeline.h"
#include "Buildables/FGBuildableRailroadStation.h"
#include "Buildables/FGBuildableResourceExtractor.h"
#include "Buildables/FGBuildableResourceExtractorBase.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FGFactoryConnectionComponent.h"
#include "FGInventoryComponent.h"
#include "FGMapArea.h"
#include "FGMapAreaTexture.h"
#include "FGMinimapCaptureActor.h"
#include "FGPipeConnectionComponent.h"
#include "FGRailroadSubsystem.h"
#include "FGTrain.h"
#include "FGRecipe.h"
#include "FGTrainStationIdentifier.h"
#include "FGVolumeMapArea.h"
#include "FGWorldSettings.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "Resources/FGExtractableResourceInterface.h"
#include "SmartTrains.h"
#include "STLayout.h"
#include "STSubsystem.h"
#include "STTrace.h"
#include "UObject/UnrealType.h"

namespace
{
	// Buildings and belt segments one flood fill through a factory may visit.
	constexpr int32 FactoryFloodLimit = 50000;
	// An item the factory makes upstream of its unloading platforms' machines at least this share as fast as those
	// machines use it is made there, not brought by train.
	constexpr double LocalSupplyShare = 0.9;
	// Recipe chains deeper than this are cut off in the rate search (loops of recipes).
	constexpr int32 MaxRecipeDepth = 12;
	// Steps of the rate search.
	constexpr int32 RateSearchSteps = 40;
	// A preview is applied as shown if confirmed within this long. [s]
	constexpr double PreviewKeepSeconds = 120.0;

	bool SetupIsFluid(UClass* Item)
	{
		return STNames::IsFluid(Item);
	}

	// Recipe amounts count fluids in litres; rates here use m³.
	double RecipeUnits(const FItemAmount& Amount)
	{
		return SetupIsFluid(Amount.ItemClass) ? Amount.Amount / 1000.0 : static_cast<double>(Amount.Amount);
	}

	// Machines of one recipe (or extractors of one resource) in a factory.
	struct FSetupMachineGroup
	{
		FString Name;
		int32 Count = 0;
		// Runs per minute when all of them run.
		double Capacity = 0.0;
		TMap<UClass*, double> Products;
		TMap<UClass*, double> Ingredients;
	};

	// What can pass through a platform's belts or pipes per minute, out of it (bOut) or into it; OutText says how many
	// belts or pipes and of what. 0 when nothing is attached.
	double SetupThroughput(const AFGBuildableTrainPlatformCargo* Platform, bool bOut, FString& OutText)
	{
		double PerMinute = 0.0;
		int32 Belts = 0;
		int32 Pipes = 0;
		int32 Direct = 0;
		TInlineComponentArray<UFGFactoryConnectionComponent*> Connections;
		Platform->GetComponents(Connections);
		const EFactoryConnectionDirection Wanted = bOut ? EFactoryConnectionDirection::FCD_OUTPUT : EFactoryConnectionDirection::FCD_INPUT;
		for (const UFGFactoryConnectionComponent* Connection : Connections)
		{
			if (!Connection || !Connection->IsConnected() || Connection->GetDirection() != Wanted)
			{
				continue;
			}
			const UFGFactoryConnectionComponent* Other = Connection->GetConnection();
			const AFGBuildableConveyorBase* Conveyor = Other ? Cast<AFGBuildableConveyorBase>(Other->GetOuterBuildable()) : nullptr;
			if (Conveyor)
			{
				// Belt speed is in cm/s; items sit ITEM_SPACING apart.
				PerMinute += Conveyor->GetSpeed() * 60.0 / AFGBuildableConveyorBase::ITEM_SPACING;
				++Belts;
			}
			else
			{
				++Direct;
			}
		}
		for (const UFGPipeConnectionComponent* Pipe : FSTPlatformAccess::GetPipeConnections(Platform, !bOut))
		{
			const UFGPipeConnectionComponentBase* Other = Pipe ? Pipe->GetConnection() : nullptr;
			const AFGBuildablePipeline* Pipeline = Other ? Cast<AFGBuildablePipeline>(Other->GetOwner()) : nullptr;
			if (!Pipeline)
			{
				++Direct;
				continue;
			}
			// The flow limit is in m³ per second.
			const float Limit = Pipeline->GetFlowLimit();
			static bool bLogged = false;
			if (!bLogged)
			{
				bLogged = true;
				UE_LOG(LogSmartTrains, Log, TEXT("SETUP pipe flow limit as the game reports it: %.3f (read as m3/s)"), Limit);
			}
			PerMinute += Limit * 60.0;
			++Pipes;
		}
		TArray<FString> Parts;
		if (Belts > 0)
		{
			Parts.Add(FString::Printf(TEXT("%d belt%s"), Belts, Belts == 1 ? TEXT("") : TEXT("s")));
		}
		if (Pipes > 0)
		{
			Parts.Add(FString::Printf(TEXT("%d pipe%s"), Pipes, Pipes == 1 ? TEXT("") : TEXT("s")));
		}
		if (Direct > 0)
		{
			Parts.Add(FString::Printf(TEXT("%d direct connection%s (not counted)"), Direct, Direct == 1 ? TEXT("") : TEXT("s")));
		}
		OutText = FString::Join(Parts, TEXT(" and "));
		return PerMinute;
	}

	// Reads one machine into Groups: its recipe's products and ingredients per run and its runs per minute.
	void SetupAddMachine(const AActor* Machine, TMap<FString, FSetupMachineGroup>& Groups)
	{
		if (const AFGBuildableManufacturer* Manufacturer = Cast<AFGBuildableManufacturer>(Machine))
		{
			const TSubclassOf<UFGRecipe> Recipe = Manufacturer->GetCurrentRecipe();
			const float Duration = Recipe ? UFGRecipe::GetManufacturingDuration(Recipe) : 0.f;
			if (Duration <= 0.f)
			{
				return;
			}
			// The cycle at its clock speed; production boost (somersloops) multiplies what each cycle makes.
			const float Clock = Manufacturer->GetCurrentPotential() > 0.f ? Manufacturer->GetCurrentPotential() : 1.f;
			const float Cycle = Manufacturer->GetProductionCycleTime() > 0.f ? Manufacturer->GetProductionCycleTime() : Duration / Clock;
			const float Boost = Manufacturer->GetCurrentProductionBoost() > 0.f ? Manufacturer->GetCurrentProductionBoost() : 1.f;
			const int32 BoostKey = FMath::RoundToInt(Boost * 100.f);
			FSetupMachineGroup& Group = Groups.FindOrAdd(FString::Printf(TEXT("%s|%d"), *Recipe->GetPathName(), BoostKey));
			if (Group.Count == 0)
			{
				Group.Name = UFGRecipe::GetRecipeName(Recipe).ToString() + (BoostKey != 100 ? FString::Printf(TEXT(" (boosted %d%%)"), BoostKey) : FString());
				for (const FItemAmount& Amount : UFGRecipe::GetProducts(Recipe))
				{
					Group.Products.FindOrAdd(Amount.ItemClass) += RecipeUnits(Amount) * Boost;
				}
				for (const FItemAmount& Amount : UFGRecipe::GetIngredients(const_cast<AFGBuildableManufacturer*>(Manufacturer), Recipe))
				{
					Group.Ingredients.FindOrAdd(Amount.ItemClass) += RecipeUnits(Amount);
				}
			}
			Group.Capacity += 60.0 / Cycle;
			++Group.Count;
			return;
		}
		if (const AFGBuildableResourceExtractor* Extractor = Cast<AFGBuildableResourceExtractor>(Machine))
		{
			IFGExtractableResourceInterface* Resource = Extractor->GetExtractableResource().GetInterface();
			UClass* Item = Resource ? Resource->GetResourceClass().Get() : nullptr;
			if (!Item)
			{
				return;
			}
			// What it extracts now: node purity, extractor tier and clock speed included. Fluids may come in litres.
			double PerMinute = Extractor->GetExtractionPerMinute();
			if (SetupIsFluid(Item) && PerMinute > 10000.0)
			{
				PerMinute /= 1000.0;
			}
			static TSet<FString> Logged;
			if (!Logged.Contains(Machine->GetClass()->GetName()))
			{
				Logged.Add(Machine->GetClass()->GetName());
				UE_LOG(LogSmartTrains, Log, TEXT("SETUP extractor %s: %.2f %s per minute (as the game reports it: %.2f)"), *Machine->GetClass()->GetName(), PerMinute,
					*STNames::Item(Item), Extractor->GetExtractionPerMinute());
			}
			FSetupMachineGroup& Group = Groups.FindOrAdd(TEXT("extract:") + Item->GetPathName());
			if (Group.Count == 0)
			{
				Group.Name = FString::Printf(TEXT("%s extractors"), *STNames::Item(Item));
				Group.Products.Add(Item, 1.0);
			}
			Group.Capacity += PerMinute;
			++Group.Count;
		}
	}

	// The item most platforms of Role carry in Proposal (null if none).
	UClass* SetupMostCommonItem(const STMigration::FStationProposal& Proposal, ESTRole Role)
	{
		TMap<UClass*, int32> Counts;
		for (const STMigration::FPlatformProposal& Platform : Proposal.Platforms)
		{
			if (Platform.Role == Role)
			{
				for (const TSubclassOf<UFGItemDescriptor>& Item : Platform.Items)
				{
					++Counts.FindOrAdd(Item.Get());
				}
			}
		}
		UClass* Best = nullptr;
		int32 Most = 0;
		for (const TPair<UClass*, int32>& Pair : Counts)
		{
			if (Pair.Value > Most || (Pair.Value == Most && Best && STNames::Item(Pair.Key) < STNames::Item(Best)))
			{
				Most = Pair.Value;
				Best = Pair.Key;
			}
		}
		return Best;
	}

	FString SetupCollapseSpaces(const FString& Text)
	{
		FString Out;
		bool bSpace = false;
		for (const TCHAR Char : Text)
		{
			if (FChar::IsWhitespace(Char))
			{
				bSpace = true;
				continue;
			}
			if (bSpace && !Out.IsEmpty())
			{
				Out.AppendChar(TEXT(' '));
			}
			bSpace = false;
			Out.AppendChar(Char);
		}
		return Out;
	}

	// Every station on the railroad.
	TArray<AFGTrainStationIdentifier*> SetupStations(const UWorld* World)
	{
		TArray<AFGTrainStationIdentifier*> Stations;
		if (AFGRailroadSubsystem* Railroad = World ? AFGRailroadSubsystem::Get(const_cast<UWorld*>(World)) : nullptr)
		{
			Railroad->GetAllTrainStations(Stations);
		}
		Stations.RemoveAll([](const AFGTrainStationIdentifier* Station) { return !IsValid(Station); });
		return Stations;
	}

	FString StationNameOf(const AFGTrainStationIdentifier* Station)
	{
		return Station ? Station->GetStationName().ToString() : FString();
	}

	// "Supply Iron Plate" for a platform's settings now.
	FString SetupSettingsOf(const ASTSubsystem& Subsystem, const AFGBuildableTrainPlatformCargo* Platform)
	{
		const FSTPlatformConfig* Config = Subsystem.FindPlatformConfig(Platform);
		return Config ? STNames::Settings(Config->Role, Config->Items) : FString(TEXT("Off"));
	}

	// True if Platform is already set to Role with exactly Items.
	bool SetupIsSetTo(const ASTSubsystem& Subsystem, const AFGBuildableTrainPlatformCargo* Platform, ESTRole Role, const TArray<TSubclassOf<UFGItemDescriptor>>& Items)
	{
		const FSTPlatformConfig* Config = Subsystem.FindPlatformConfig(Platform);
		if (!Config)
		{
			return Role == ESTRole::Off;
		}
		if (Config->Role != Role || Config->Items.Num() != Items.Num())
		{
			return false;
		}
		for (const FSTItemRule& Rule : Config->Items)
		{
			if (!Items.Contains(Rule.Item))
			{
				return false;
			}
		}
		return true;
	}

	FString SetupCsv(const FString& Text)
	{
		return TEXT("\"") + Text.Replace(TEXT("\""), TEXT("\"\"")) + TEXT("\"");
	}
}

TArray<FString> STSetup::ApplyProposal(ASTSubsystem& Subsystem, const STMigration::FStationProposal& Proposal, bool bRename, const FString& Cause,
	FSTMigrationUndo* Undo)
{
	TArray<FString> Done;
	AFGTrainStationIdentifier* Station = Proposal.Station.Get();
	for (const STMigration::FPlatformProposal& Platform : Proposal.Platforms)
	{
		AFGBuildableTrainPlatformCargo* Cargo = Platform.Platform.Get();
		if (!Cargo || !Platform.bCargo || Platform.bKept)
		{
			continue;
		}
		if (Platform.Role != ESTRole::Off && Platform.Items.Num() == 0)
		{
			UE_LOG(LogSmartTrains, Log, TEXT("SETUP %s P%d: no item found, left as %s"), *STLayout::Name(Station), Platform.Index + 1, *SetupSettingsOf(Subsystem, Cargo));
			continue;
		}
		if (SetupIsSetTo(Subsystem, Cargo, Platform.Role, Platform.Items))
		{
			Subsystem.SetPlatformMachines(Cargo, Platform.Machines);
			continue;
		}
		if (Undo)
		{
			FSTPlatformConfig Before;
			if (const FSTPlatformConfig* Existing = Subsystem.FindPlatformConfig(Cargo))
			{
				Before = *Existing;
			}
			Before.Platform = Cargo;
			Before.bOriginalLoadMode = Cargo->GetIsInLoadMode();
			Before.bHasOriginalLoadMode = true;
			Undo->Platforms.Add(Before);
		}
		TArray<FSTItemRule> Rules;
		for (const TSubclassOf<UFGItemDescriptor>& Item : Platform.Items)
		{
			Rules.AddDefaulted_GetRef().Item = Item;
		}
		const FString Before = SetupSettingsOf(Subsystem, Cargo);
		Subsystem.SetPlatformConfig(Cargo, Platform.Role, Rules, Cause + TEXT(" (station set-up)"));
		Subsystem.SetPlatformMachines(Cargo, Platform.Machines);
		const FString After = SetupSettingsOf(Subsystem, Cargo);
		Done.Add(FString::Printf(TEXT("P%d: %s -> %s"), Platform.Index + 1, *Before, *After));
		UE_LOG(LogSmartTrains, Log, TEXT("SETUP %s P%d: %s -> %s (%s)"), *STLayout::Name(Station), Platform.Index + 1, *Before, *After, *Platform.Evidence);
	}
	const FString Old = StationNameOf(Station);
	if (bRename && Station && !Proposal.NewName.IsEmpty() && Proposal.NewName != Old)
	{
		if (Undo)
		{
			Undo->RenamedStations.Add(Station);
			Undo->OldNames.Add(Old);
		}
		Station->SetStationName(FText::FromString(Proposal.NewName));
		Done.Add(FString::Printf(TEXT("Renamed %s -> %s"), *Old, *Proposal.NewName));
		UE_LOG(LogSmartTrains, Log, TEXT("SETUP renamed %s -> %s (%s; %s)"), *Old, *Proposal.NewName, *Proposal.NameNote, *Cause);
	}
	return Done;
}

//~ Platforms

STMigration::FStationProposal STSetup::ProposeStation(const ASTSubsystem& Subsystem, AFGTrainStationIdentifier* Station, const AFGBuildableTrainPlatformCargo* Redo, bool bShare)
{
	STMigration::FStationProposal Proposal;
	Proposal.Station = Station;
	const AFGBuildableRailroadStation* Building = Station ? Station->GetStation() : nullptr;
	const TArray<AFGBuildableTrainPlatform*> Platforms = STLayout::GetPlatforms(Building);
	// What each traced platform's belts or pipes reach, for choosing its item once the whole station is known.
	struct FTraced
	{
		int32 Proposal = INDEX_NONE;
		bool bSupply = false;
		TMap<UClass*, float> Flow;
		TMap<UClass*, int32> Held;
		FString Summary;
		FString Confidence;
		EFreightCargoType CargoType = EFreightCargoType::FCT_Standard;
		TSubclassOf<UFGItemDescriptor> PipeFluidHint;
	};
	TArray<FTraced> Traced;
	for (int32 Index = 0; Index < Platforms.Num(); ++Index)
	{
		AFGBuildableTrainPlatformCargo* Cargo = Cast<AFGBuildableTrainPlatformCargo>(Platforms[Index]);
		if (!Cargo)
		{
			continue;
		}
		STMigration::FPlatformProposal& Platform = Proposal.Platforms.AddDefaulted_GetRef();
		Platform.Platform = Cargo;
		Platform.Index = Index;
		if (!STLayout::CanMoveCargo(Cargo))
		{
			Platform.bCargo = false;
			Platform.Evidence = TEXT("can't move cargo");
			continue;
		}
		// A platform a player (or an earlier set-up) gave a role keeps it.
		if (const FSTPlatformConfig* Config = Subsystem.FindPlatformConfig(Cargo); Cargo != Redo && Config && Config->Role != ESTRole::Off)
		{
			Platform.bKept = true;
			for (const TObjectPtr<AActor>& Machine : Config->Machines)
			{
				Platform.Machines.Add(Machine.Get());
			}
			Platform.Role = Config->Role;
			for (const FSTItemRule& Rule : Config->Items)
			{
				if (Rule.Item)
				{
					Platform.Items.Add(Rule.Item);
				}
			}
			Platform.Confidence = TEXT("set up");
			Platform.Evidence = TEXT("already set up; kept as it is");
			continue;
		}
		int32 Inputs = 0;
		int32 Outputs = 0;
		FSTPlatformAccess::CountConnections(Cargo, Inputs, Outputs);
		// Belts or pipes only into it: it loads trains; only out of it: it unloads them; both: as it is set now.
		if (Inputs == 0 && Outputs == 0)
		{
			Platform.Role = ESTRole::Off;
			Platform.Evidence = TEXT("nothing attached");
			continue;
		}
		const bool bSupply = Outputs == 0 || (Inputs > 0 && Cargo->GetIsInLoadMode());
		Platform.Role = bSupply ? ESTRole::Supply : ESTRole::Demand;
		Platform.Evidence = FString::Printf(TEXT("%d in, %d out%s"), Inputs, Outputs, Inputs > 0 && Outputs > 0 ? TEXT(", both: its load mode decides") : TEXT(""));
		FTraced& Each = Traced.AddDefaulted_GetRef();
		Each.Proposal = Proposal.Platforms.Num() - 1;
		Each.bSupply = bSupply;
		Each.CargoType = STLayout::GetPlatformCargoType(Cargo);
		if (const UFGInventoryComponent* Inventory = Cargo->GetInventory())
		{
			TArray<FInventoryStack> Stacks;
			Inventory->GetInventoryStacks(Stacks);
			for (const FInventoryStack& Stack : Stacks)
			{
				if (Stack.HasItems() && Stack.Item.GetItemClass())
				{
					Each.Held.FindOrAdd(Stack.Item.GetItemClass()) += Stack.NumItems;
				}
			}
		}
		const STTrace::FResult Trace = STTrace::Trace(Cargo, bSupply);
		Platform.TraceText = Trace.Summary();
		Platform.Machines = Trace.MachineActors;
		Each.Summary = Trace.Summary();
		Each.Confidence = STTrace::ConfidenceName(Trace.Confidence);
		Each.PipeFluidHint = Trace.PipeFluidHint;
		for (const STTrace::FFlow& Flow : Trace.Flows)
		{
			if (Flow.Item && STLayout::ItemFitsCargoType(Flow.Item.Get(), Each.CargoType))
			{
				Each.Flow.FindOrAdd(Flow.Item.Get()) += Flow.PerMinute;
			}
		}
	}

	// Items the factory makes for itself: what the machines upstream of its unloading platforms' machines make (in its
	// own machines, or extracted from resource nodes) at least as fast as those machines use it. Trains don't bring those.
	TSet<UClass*> MadeHere;
	{
		TArray<const AActor*> Seeds;
		for (const FTraced& Each : Traced)
		{
			if (!Each.bSupply)
			{
				for (const TWeakObjectPtr<const AActor>& Machine : Proposal.Platforms[Each.Proposal].Machines)
				{
					if (Machine.IsValid())
					{
						Seeds.AddUnique(Machine.Get());
					}
				}
			}
		}
		if (Seeds.Num() > 0)
		{
			TMap<FString, FSetupMachineGroup> Users;
			for (const AActor* Machine : Seeds)
			{
				SetupAddMachine(Machine, Users);
			}
			TMap<UClass*, double> Used;
			for (const TPair<FString, FSetupMachineGroup>& Pair : Users)
			{
				for (const TPair<UClass*, double>& Ingredient : Pair.Value.Ingredients)
				{
					Used.FindOrAdd(Ingredient.Key) += Ingredient.Value * Pair.Value.Capacity;
				}
			}
			const STTrace::FFlood Up = STTrace::Flood(Seeds, true, FactoryFloodLimit);
			TMap<FString, FSetupMachineGroup> Makers;
			for (const AActor* Machine : Up.Machines)
			{
				SetupAddMachine(Machine, Makers);
			}
			TMap<UClass*, double> Made;
			for (const TPair<FString, FSetupMachineGroup>& Pair : Makers)
			{
				for (const TPair<UClass*, double>& Product : Pair.Value.Products)
				{
					Made.FindOrAdd(Product.Key) += Product.Value * Pair.Value.Capacity;
				}
			}
			for (const TPair<UClass*, double>& Pair : Used)
			{
				if (Pair.Value > 0.0 && Made.FindRef(Pair.Key) >= LocalSupplyShare * Pair.Value)
				{
					MadeHere.Add(Pair.Key);
					UE_LOG(LogSmartTrains, Verbose, TEXT("SETUP %s: %s made here (%.1f/min made upstream, %.1f/min used by the machines its platforms feed); not a train input"),
						*STLayout::Name(Station), *STNames::Item(Pair.Key), Made.FindRef(Pair.Key), Pair.Value);
				}
			}
		}
	}

	for (const FTraced& Each : Traced)
	{
		STMigration::FPlatformProposal& Platform = Proposal.Platforms[Each.Proposal];
		// What it holds decides; the trace only gives an empty platform its item.
		UClass* HeldItem = nullptr;
		int32 HeldMost = 0;
		for (const TPair<UClass*, int32>& Pair : Each.Held)
		{
			if (Pair.Value > HeldMost && STLayout::ItemFitsCargoType(Pair.Key, Each.CargoType))
			{
				HeldMost = Pair.Value;
				HeldItem = Pair.Key;
			}
		}
		if (HeldItem)
		{
			Platform.Items.Add(HeldItem);
			Platform.Confidence = TEXT("high");
			Platform.Evidence += TEXT(", item from what it holds");
			if (Each.Flow.Num() > 0 && !Each.Flow.Contains(HeldItem))
			{
				TArray<FString> TracedItems;
				for (const TPair<UClass*, float>& Pair : Each.Flow)
				{
					TracedItems.Add(STNames::Item(Pair.Key));
				}
				Platform.Note = FString::Printf(TEXT("holds %s; its machines %s %s"), *STNames::Item(HeldItem), Each.bSupply ? TEXT("make") : TEXT("take"),
					*FString::Join(TracedItems, TEXT(", ")));
				Platform.Evidence += TEXT(" (") + Platform.Note + TEXT(")");
			}
			continue;
		}
		TMap<UClass*, float> Flow = Each.Flow;
		TArray<FString> Local;
		if (!Each.bSupply)
		{
			for (auto It = Flow.CreateIterator(); It; ++It)
			{
				if (MadeHere.Contains(It.Key()) && Flow.Num() > 1)
				{
					Local.Add(STNames::Item(It.Key()));
					It.RemoveCurrent();
				}
			}
		}
		if (Flow.Num() > 0)
		{
			TArray<UClass*> Order;
			Flow.GetKeys(Order);
			Order.Sort([&Flow](const UClass& A, const UClass& B) { return Flow.FindRef(const_cast<UClass*>(&A)) > Flow.FindRef(const_cast<UClass*>(&B)); });
			Platform.Items.Add(Order[0]);
			Platform.Confidence = Each.Confidence;
			Platform.Evidence += TEXT(", items from its machines: ") + Each.Summary;
			if (Local.Num() > 0)
			{
				Platform.Evidence += FString::Printf(TEXT("; made in the factory itself, so left out: %s"), *FString::Join(Local, TEXT(", ")));
			}
			if (Order.Num() > 1)
			{
				for (UClass* Item : Order)
				{
					Platform.SpreadChoices.Add(Item);
					Platform.SpreadRates.Add(Flow.FindRef(Item));
				}
				Platform.OneItemNote = TEXT("several items on its belts; shared out by rate");
				Platform.Note = Platform.OneItemNote;
			}
			continue;
		}
		// Empty and no machine reached: the fluid now in its pipes.
		if (Each.PipeFluidHint && STLayout::ItemFitsCargoType(Each.PipeFluidHint.Get(), Each.CargoType))
		{
			Platform.Items.Add(Each.PipeFluidHint.Get());
			Platform.Confidence = TEXT("medium");
			Platform.Evidence += TEXT(", no machine reached; fluid now in its pipes");
		}
		else
		{
			Platform.Confidence = TEXT("unclear");
			Platform.Conflict = TEXT("no item found");
			Platform.Evidence += TEXT(", no item found: ") + Each.Summary;
		}
	}
	if (!bShare)
	{
		return Proposal;
	}
	// Platforms of one role serving the same machines share the items out.
	STMigration::FPreview One;
	One.Stations.Add(MoveTemp(Proposal));
	STMigration::ShareOutItems(One);
	return MoveTemp(One.Stations[0]);
}

//~ Factory

STSetup::FFactory STSetup::AnalyzeFactory(const ASTSubsystem& Subsystem, const STMigration::FStationProposal& Proposal)
{
	FFactory Factory;
	auto GroupsOf = [](const TArray<const AActor*>& Machines)
	{
		TMap<FString, FSetupMachineGroup> GroupMap;
		for (const AActor* Machine : Machines)
		{
			SetupAddMachine(Machine, GroupMap);
		}
		TArray<FSetupMachineGroup> Out;
		GroupMap.GenerateValueArray(Out);
		return Out;
	};
	auto MakesItem = [](const AActor* Machine, UClass* Item)
	{
		TMap<FString, FSetupMachineGroup> One;
		SetupAddMachine(Machine, One);
		for (const TPair<FString, FSetupMachineGroup>& Each : One)
		{
			if (Each.Value.Products.Contains(Item))
			{
				return true;
			}
		}
		return false;
	};

	const bool bAnySupply = Proposal.Platforms.ContainsByPredicate([](const STMigration::FPlatformProposal& Each) { return Each.Role == ESTRole::Supply && Each.Items.Num() > 0; });
	const bool bAnyDemand = Proposal.Platforms.ContainsByPredicate([](const STMigration::FPlatformProposal& Each) { return Each.Role == ESTRole::Demand && Each.Items.Num() > 0; });
	bool bCut = false;

	// The product. A station that only loads: what it loads. Otherwise: downstream from its unloading platforms (one
	// flood fill through belts, pipes and machines), what the machines make and don't use themselves, preferring what
	// goes on to a train platform, then the one made by the most machines.
	UClass* Product = nullptr;
	STTrace::FFlood Down;
	if (bAnySupply && !bAnyDemand)
	{
		Product = SetupMostCommonItem(Proposal, ESTRole::Supply);
	}
	else
	{
		TArray<const AActor*> Seeds;
		for (const STMigration::FPlatformProposal& Platform : Proposal.Platforms)
		{
			if (Platform.bCargo && Platform.Role == ESTRole::Demand)
			{
				for (const TWeakObjectPtr<const AActor>& Each : Platform.Machines)
				{
					if (Each.IsValid())
					{
						Seeds.AddUnique(Each.Get());
					}
				}
			}
		}
		Down = STTrace::Flood(Seeds, false, FactoryFloodLimit);
		bCut |= Down.bCut;
		const TArray<FSetupMachineGroup> Groups = GroupsOf(Down.Machines);
		TMap<UClass*, int32> Makers;
		TSet<UClass*> Used;
		for (const FSetupMachineGroup& Group : Groups)
		{
			for (const TPair<UClass*, double>& Each : Group.Products)
			{
				Makers.FindOrAdd(Each.Key) += Group.Count;
			}
			for (const TPair<UClass*, double>& Each : Group.Ingredients)
			{
				Used.Add(Each.Key);
			}
		}
		TSet<UClass*> ToPlatforms;
		for (const TPair<const AActor*, TArray<const AActor*>>& Pair : Down.PlatformsByMachine)
		{
			TMap<FString, FSetupMachineGroup> One;
			SetupAddMachine(Pair.Key, One);
			for (const TPair<FString, FSetupMachineGroup>& Each : One)
			{
				for (const TPair<UClass*, double>& Made : Each.Value.Products)
				{
					ToPlatforms.Add(Made.Key);
				}
			}
		}
		int32 BestKey = -1;
		for (const TPair<UClass*, int32>& Pair : Makers)
		{
			const int32 Key = (Used.Contains(Pair.Key) ? 0 : 2000000) + (ToPlatforms.Contains(Pair.Key) ? 1000000 : 0) + Pair.Value;
			if (Key > BestKey)
			{
				BestKey = Key;
				Product = Pair.Key;
			}
		}
		if (!Product)
		{
			Product = SetupMostCommonItem(Proposal, bAnyDemand ? ESTRole::Demand : ESTRole::Supply);
		}
	}
	Factory.Product = Product;
	if (!Product)
	{
		return Factory;
	}

	// The factory for the rate: upstream from the machines that make the product (for a station that only loads, those
	// its platforms reach), through their inputs, until train platforms and extractors.
	TArray<const AActor*> MakerSeeds;
	if (bAnySupply && !bAnyDemand)
	{
		for (const STMigration::FPlatformProposal& Platform : Proposal.Platforms)
		{
			if (Platform.bCargo && Platform.Role == ESTRole::Supply && Platform.Items.Contains(Product))
			{
				for (const TWeakObjectPtr<const AActor>& Each : Platform.Machines)
				{
					if (Each.IsValid())
					{
						MakerSeeds.AddUnique(Each.Get());
					}
				}
			}
		}
	}
	else
	{
		for (const AActor* Machine : Down.Machines)
		{
			if (MakesItem(Machine, Product))
			{
				MakerSeeds.Add(Machine);
			}
		}
	}
	const STTrace::FFlood Up = STTrace::Flood(MakerSeeds, true, FactoryFloodLimit);
	bCut |= Up.bCut;
	const TArray<const AActor*>& Machines = Up.Machines;
	TSet<const AFGBuildableTrainPlatformCargo*> InputPlatforms;
	for (const AActor* Each : Up.Platforms)
	{
		if (const AFGBuildableTrainPlatformCargo* Cargo = Cast<AFGBuildableTrainPlatformCargo>(Each))
		{
			InputPlatforms.Add(Cargo);
		}
	}
	Factory.Machines = Machines.Num();
	if (bCut)
	{
		Factory.Notes.Add(FString::Printf(TEXT("the factory is larger than %d buildings and belts; the rate counts what was reached"), FactoryFloodLimit));
	}
	TArray<FSetupMachineGroup> Groups = GroupsOf(Machines);
	TMap<UClass*, TArray<int32>> Producers;
	for (int32 Index = 0; Index < Groups.Num(); ++Index)
	{
		for (const TPair<UClass*, double>& Each : Groups[Index].Products)
		{
			Producers.FindOrAdd(Each.Key).Add(Index);
		}
	}

	// What trains bring: each unloading platform's belts or pipes out of it, per item it carries (proposed here, set by
	// a player, else what its own trace finds).
	TMap<UClass*, double> Caps;
	TMap<UClass*, double> OwnCaps;
	TMap<UClass*, FString> CapText;
	for (const AFGBuildableTrainPlatformCargo* Cargo : InputPlatforms)
	{
		TArray<UClass*> Items;
		const STMigration::FPlatformProposal* Own = Proposal.Platforms.FindByPredicate([Cargo](const STMigration::FPlatformProposal& Each) { return Each.Platform.Get() == Cargo; });
		if (Own)
		{
			for (const TSubclassOf<UFGItemDescriptor>& Item : Own->Items)
			{
				Items.AddUnique(Item.Get());
			}
		}
		else if (const FSTPlatformConfig* Config = Subsystem.FindPlatformConfig(Cargo); Config && Config->Role == ESTRole::Demand && Config->Items.Num() > 0)
		{
			for (const FSTItemRule& Rule : Config->Items)
			{
				Items.AddUnique(Rule.Item.Get());
			}
		}
		else
		{
			const EFreightCargoType CargoType = STLayout::GetPlatformCargoType(Cargo);
			for (const STTrace::FFlow& Flow : STTrace::Trace(Cargo, false).Flows)
			{
				if (Flow.Item && STLayout::ItemFitsCargoType(Flow.Item.Get(), CargoType))
				{
					Items.AddUnique(Flow.Item.Get());
				}
			}
		}
		FString Text;
		const double PerMinute = SetupThroughput(Cargo, true, Text);
		if (PerMinute <= 0.0)
		{
			continue;
		}
		const AFGTrainStationIdentifier* Station = STLayout::GetIdentifier(STLayout::FindStation(const_cast<AFGBuildableTrainPlatformCargo*>(Cargo)));
		for (UClass* Item : Items)
		{
			Caps.FindOrAdd(Item) += PerMinute;
			OwnCaps.FindOrAdd(Item) += Own ? PerMinute : 0.0;
			FString& Where = CapText.FindOrAdd(Item);
			Where += FString::Printf(TEXT("%s%s out of %s"), Where.IsEmpty() ? TEXT("") : TEXT(" + "), *Text, *STLayout::Name(Station));
		}
	}

	Factory.bManufactured = Producers.Contains(Product);

	// This station's own belts or pipes for Item: out of its unloading platforms (bOut) or into its loading ones.
	auto OwnThroughput = [&](UClass* Item, bool bOut, FString& OutText)
	{
		double PerMinute = 0.0;
		TArray<FString> Parts;
		for (const STMigration::FPlatformProposal& Platform : Proposal.Platforms)
		{
			if (Platform.Role == (bOut ? ESTRole::Demand : ESTRole::Supply) && Platform.Items.Contains(Item) && Platform.Platform.IsValid())
			{
				FString Text;
				PerMinute += SetupThroughput(Platform.Platform.Get(), bOut, Text);
				if (!Text.IsEmpty())
				{
					Parts.Add(FString::Printf(TEXT("P%d %s"), Platform.Index + 1, *Text));
				}
			}
		}
		OutText = FString::Printf(TEXT("this station's %s"), *FString::Join(Parts, TEXT(", ")));
		return PerMinute;
	};

	// An unloading station whose platforms all take one item: that item and how much of it the factory can use.
	TArray<UClass*> InputItems;
	for (const STMigration::FPlatformProposal& Platform : Proposal.Platforms)
	{
		if (Platform.Role == ESTRole::Demand)
		{
			for (const TSubclassOf<UFGItemDescriptor>& Item : Platform.Items)
			{
				InputItems.AddUnique(Item.Get());
			}
		}
	}
	if (bAnyDemand && !bAnySupply && InputItems.Num() == 1)
	{
		Factory.InputItem = InputItems[0];
		FString Text;
		Factory.InputRate = OwnThroughput(InputItems[0], true, Text);
		Factory.InputLimit = Text;
	}

	if (!Factory.bManufactured)
	{
		// Unloading into machines that make nothing (power plants, storage): its own belts or pipes. Loading what no
		// machine it reaches makes: unknown.
		if (!bAnyDemand)
		{
			Factory.Limit = TEXT("no machine reached");
			return Factory;
		}
		FString Text;
		Factory.Rate = OwnThroughput(Product, true, Text);
		Factory.Limit = Factory.Rate > 0.0 ? Text : FString(TEXT("nothing measurable attached"));
		return Factory;
	}

	// The most of Product per minute: a search over the rate. Each step shares the need for an item out over the machines
	// making it, in proportion to the capacity they have left; what they make besides goes to later needs (canisters,
	// water); what trains bring is used first. A step fails when the machines making an item, or the belts bringing it,
	// can't keep up.
	double Upper = 0.0;
	for (int32 Index : Producers[Product])
	{
		Upper += Groups[Index].Capacity * Groups[Index].Products[Product];
	}
	TSet<UClass*> External;
	auto Check = [&](double Target, FString* OutLimit, TMap<UClass*, double>* OutImported) -> bool
	{
		TArray<double> Runs;
		Runs.SetNumZeroed(Groups.Num());
		TMap<UClass*, double> Imported;
		TMap<UClass*, double> Spare;
		bool bOk = true;
		auto Fail = [&](const FString& Why)
		{
			if (bOk && OutLimit)
			{
				*OutLimit = Why;
			}
			bOk = false;
		};
		TFunction<void(UClass*, double, int32)> Need = [&](UClass* Item, double Amount, int32 Depth)
		{
			if (Depth > MaxRecipeDepth || Amount <= 1e-9 || !bOk)
			{
				return;
			}
			if (double* Left = Spare.Find(Item))
			{
				const double Take = FMath::Min(Amount, *Left);
				*Left -= Take;
				Amount -= Take;
			}
			if (const double* Cap = Caps.Find(Item))
			{
				double& Used = Imported.FindOrAdd(Item);
				const double Take = FMath::Min(Amount, FMath::Max(0.0, *Cap - Used));
				Used += Take;
				Amount -= Take;
			}
			if (Amount <= 1e-9)
			{
				return;
			}
			const TArray<int32>* Makers = Producers.Find(Item);
			if (!Makers)
			{
				if (Caps.Contains(Item))
				{
					Fail(FString::Printf(TEXT("%s: %s (%s/min)"), *STNames::Item(Item), *CapText.FindRef(Item), *STSetup::FormatRate(Caps[Item])));
				}
				else
				{
					External.Add(Item);
				}
				return;
			}
			double Room = 0.0;
			int32 Count = 0;
			for (int32 Index : *Makers)
			{
				Room += FMath::Max(0.0, Groups[Index].Capacity - Runs[Index]) * Groups[Index].Products[Item];
				Count += Groups[Index].Count;
			}
			if (Amount > Room * (1.0 + 1e-6))
			{
				Fail(Makers->Num() == 1 ? FString::Printf(TEXT("%d machine%s running %s"), Groups[(*Makers)[0]].Count, Groups[(*Makers)[0]].Count == 1 ? TEXT("") : TEXT("s"),
					*Groups[(*Makers)[0]].Name) : FString::Printf(TEXT("%d machines making %s"), Count, *STNames::Item(Item)));
				return;
			}
			for (int32 Index : *Makers)
			{
				const FSetupMachineGroup& Group = Groups[Index];
				const double Free = FMath::Max(0.0, Group.Capacity - Runs[Index]);
				const double GroupRuns = Room > 0.0 ? Amount * Free / Room : 0.0;
				if (GroupRuns <= 0.0)
				{
					continue;
				}
				Runs[Index] += GroupRuns;
				for (const TPair<UClass*, double>& Made : Group.Products)
				{
					if (Made.Key != Item)
					{
						Spare.FindOrAdd(Made.Key) += GroupRuns * Made.Value;
					}
				}
				for (const TPair<UClass*, double>& Ingredient : Group.Ingredients)
				{
					Need(Ingredient.Key, GroupRuns * Ingredient.Value, Depth + 1);
				}
			}
		};
		Need(Product, Target, 0);
		if (OutImported)
		{
			*OutImported = Imported;
		}
		return bOk;
	};
	double Rate = 0.0;
	TMap<UClass*, double> ImportedAtRate;
	if (Check(Upper, nullptr, &ImportedAtRate))
	{
		Rate = Upper;
		const int32 Count = [&]() { int32 Sum = 0; for (int32 Index : Producers[Product]) { Sum += Groups[Index].Count; } return Sum; }();
		Factory.Limit = FString::Printf(TEXT("%d machines making %s"), Count, *STNames::Item(Product));
	}
	else
	{
		double Low = 0.0;
		double High = Upper;
		for (int32 Step = 0; Step < RateSearchSteps; ++Step)
		{
			const double Mid = 0.5 * (Low + High);
			(Check(Mid, nullptr, nullptr) ? Low : High) = Mid;
		}
		Rate = Low;
		Check(Low, nullptr, &ImportedAtRate);
		Check(High, &Factory.Limit, nullptr);
	}

	// A station that only loads gets its share of what the factory makes: by its belts or pipes into its platforms,
	// against all loading platforms for the product that the factory's machines feed (two stations sharing one factory).
	if (bAnySupply && !bAnyDemand)
	{
		FString Text;
		const double Into = OwnThroughput(Product, false, Text);
		double AllInto = 0.0;
		TArray<const AActor*> Makers;
		for (const AActor* Machine : Machines)
		{
			if (MakesItem(Machine, Product))
			{
				Makers.Add(Machine);
			}
		}
		const STTrace::FFlood Fed = STTrace::Flood(Makers, false, FactoryFloodLimit);
		for (const AActor* Each : Fed.Platforms)
		{
			const AFGBuildableTrainPlatformCargo* Cargo = Cast<AFGBuildableTrainPlatformCargo>(Each);
			if (!Cargo)
			{
				continue;
			}
			const STMigration::FPlatformProposal* Own = Proposal.Platforms.FindByPredicate([Cargo](const STMigration::FPlatformProposal& P) { return P.Platform.Get() == Cargo; });
			const FSTPlatformConfig* Config = Subsystem.FindPlatformConfig(Cargo);
			const bool bCarries = Own ? Own->Role == ESTRole::Supply && Own->Items.Contains(Product)
				: (Config && Config->Role == ESTRole::Supply && Config->Items.ContainsByPredicate([&](const FSTItemRule& Rule) { return Rule.Item.Get() == Product; }));
			if (bCarries)
			{
				FString Unused;
				AllInto += SetupThroughput(Cargo, false, Unused);
			}
		}
		if (Into > 0.0 && AllInto > Into * 1.001)
		{
			Rate *= Into / AllInto;
			Factory.Notes.Add(FString::Printf(TEXT("this station loads %.0f %% of what the factory sends to train platforms"), 100.0 * Into / AllInto));
		}
		if (Into > 0.0 && Into < Rate)
		{
			Rate = Into;
			Factory.Limit = Text;
		}
	}
	Factory.Rate = Rate;
	if (Factory.InputItem)
	{
		// How much of its one item the factory uses at its most, this station's share of it, within its own belts.
		const double Total = Caps.FindRef(Factory.InputItem.Get());
		const double Share = Total > 0.0 ? OwnCaps.FindRef(Factory.InputItem.Get()) / Total : 1.0;
		const double Used = ImportedAtRate.FindRef(Factory.InputItem.Get()) * Share;
		if (Used > 0.0 && Used < Factory.InputRate * 0.999)
		{
			Factory.InputRate = Used;
			Factory.InputLimit = FString::Printf(TEXT("what the factory uses at its most (%s/min of %s, limited by %s)"), *STSetup::FormatRate(Rate),
				*STNames::Item(Product), *Factory.Limit);
		}
	}
	for (UClass* Item : External)
	{
		Factory.Notes.Add(FString::Printf(TEXT("assumes enough %s arrives by belt from outside the factory"), *STNames::Item(Item)));
	}
	return Factory;
}


//~ Names

FString STSetup::GetMapArea(const UWorld* World, const FVector& Location, FString& OutSource)
{
	OutSource = TEXT("none");
	if (!World)
	{
		return FString();
	}
	auto NameOf = [](TSubclassOf<UFGMapArea> Area)
	{
		const FString User = UFGMapArea::GetUserSetAreaDisplayName(Area).ToString();
		return User.IsEmpty() ? UFGMapArea::GetAreaDisplayName(Area).ToString() : User;
	};
	const AFGWorldSettings* Settings = Cast<AFGWorldSettings>(World->GetWorldSettings());
	AFGMinimapCaptureActor* Capture = Settings ? Settings->GetMinimapCaptureActor() : nullptr;
	if (!Capture)
	{
		for (TActorIterator<AFGMinimapCaptureActor> It(const_cast<UWorld*>(World)); It && !Capture; ++It)
		{
			Capture = *It;
		}
	}
	UFGMapAreaTexture* Texture = Capture ? Capture->GetMapAreaTexture() : nullptr;
	if (Texture)
	{
		TSubclassOf<UFGMapArea> Area = Texture->GetMapAreaForWorldLocation(Location);
		// The texture learns the map's scale and offset when the capture actor starts, which a dedicated server may not
		// have done.
		static TWeakObjectPtr<UFGMapAreaTexture> Initialised;
		if (!Area && Initialised.Get() != Texture)
		{
			Initialised = Texture;
			Texture->InitMapAreaTexture(Capture);
			Area = Texture->GetMapAreaForWorldLocation(Location);
			UE_LOG(LogSmartTrains, Log, TEXT("SETUP map areas: texture initialised here, lookup %s"), Area ? TEXT("works") : TEXT("still finds nothing"));
		}
		if (Area)
		{
			OutSource = TEXT("map area texture");
			return NameOf(Area);
		}
	}
	static bool bLogged = false;
	if (!bLogged)
	{
		bLogged = true;
		UE_LOG(LogSmartTrains, Log, TEXT("SETUP map areas: world settings %s, minimap capture actor %s, area texture %s, at %s"), Settings ? TEXT("found") : TEXT("missing"),
			Capture ? TEXT("found") : TEXT("missing"), Texture ? TEXT("found") : TEXT("missing"), *Location.ToString());
	}
	for (TActorIterator<AFGVolumeMapArea> It(const_cast<UWorld*>(World)); It; ++It)
	{
		if (!It->EncompassesPoint(Location))
		{
			continue;
		}
		static const FClassProperty* AreaProperty = CastField<FClassProperty>(AFGVolumeMapArea::StaticClass()->FindPropertyByName(TEXT("mMapArea")));
		UClass* Area = AreaProperty ? Cast<UClass>(AreaProperty->GetObjectPropertyValue_InContainer(*It)) : nullptr;
		if (Area)
		{
			OutSource = TEXT("map area volume");
			return NameOf(Area);
		}
	}
	return FString();
}

FString STSetup::FormatRate(double PerMinute)
{
	if (PerMinute <= 0.0)
	{
		return FString();
	}
	if (PerMinute < 10.0)
	{
		const double Rounded = FMath::RoundToDouble(PerMinute * 10.0) / 10.0;
		return FMath::IsNearlyEqual(Rounded, FMath::RoundToDouble(Rounded)) ? FString::Printf(TEXT("%d"), FMath::RoundToInt(Rounded)) : FString::Printf(TEXT("%.1f"), Rounded);
	}
	return FText::AsNumber(FMath::RoundToInt64(PerMinute)).ToString();
}

FString STSetup::FormatName(const FString& Format, const FString& Area, const FString& Product, const FString& Role, int32 Number, const FString& Rate)
{
	FString Name = Format;
	Name.ReplaceInline(TEXT("{area}"), *Area, ESearchCase::IgnoreCase);
	Name.ReplaceInline(TEXT("{product}"), *Product, ESearchCase::IgnoreCase);
	Name.ReplaceInline(TEXT("{role}"), *Role, ESearchCase::IgnoreCase);
	Name.ReplaceInline(TEXT("{n}"), Number > 0 ? *FString::FromInt(Number) : TEXT(""), ESearchCase::IgnoreCase);
	Name.ReplaceInline(TEXT("{rate}"), *Rate, ESearchCase::IgnoreCase);
	// Brackets left empty by a missing value drop out.
	for (const TCHAR* Empty : {TEXT("( )"), TEXT("()"), TEXT("[ ]"), TEXT("[]")})
	{
		Name.ReplaceInline(Empty, TEXT(""));
	}
	Name = SetupCollapseSpaces(Name);
	// Separators left at either end ("- Grass Fields").
	while (Name.StartsWith(TEXT("- ")) || Name.StartsWith(TEXT(", ")))
	{
		Name.RightChopInline(2);
	}
	while (Name.EndsWith(TEXT(" -")) || Name.EndsWith(TEXT(",")))
	{
		Name.LeftChopInline(Name.EndsWith(TEXT(",")) ? 1 : 2);
	}
	return Name.TrimStartAndEnd();
}

bool STSetup::MatchesName(const FString& Name, const FString& Format, const FString& Area, const FString& Product, const FString& Role, int32 Number)
{
	if (Name.Equals(FormatName(Format, Area, Product, Role, Number, FString()), ESearchCase::IgnoreCase))
	{
		return true;
	}
	if (!Format.Contains(TEXT("{rate}")))
	{
		return false;
	}
	// With a rate: the text around the rate must match, and the rate itself is digits, separators and dots.
	const FString Marker = TEXT("\x1F");
	const FString WithMarker = FormatName(Format, Area, Product, Role, Number, Marker);
	FString Left;
	FString Right;
	if (!WithMarker.Split(Marker, &Left, &Right))
	{
		return false;
	}
	if (Name.Len() <= Left.Len() + Right.Len() || !Name.StartsWith(Left, ESearchCase::IgnoreCase) || !Name.EndsWith(Right, ESearchCase::IgnoreCase))
	{
		return false;
	}
	const FString Middle = Name.Mid(Left.Len(), Name.Len() - Left.Len() - Right.Len());
	for (const TCHAR Char : Middle)
	{
		if (!FChar::IsDigit(Char) && Char != TEXT(',') && Char != TEXT('.') && Char != TEXT(' ') && Char != 0xA0 && Char != 0x202F)
		{
			return false;
		}
	}
	return true;
}

int32 STSetup::PickNumber(const FString& Format, const FString& Area, const FString& Product, const FString& Role, const FString& Current, const TSet<FString>& Taken)
{
	if (!Format.Contains(TEXT("{n}")))
	{
		return 0;
	}
	auto IsTaken = [&](int32 Number)
	{
		for (const FString& Other : Taken)
		{
			if (MatchesName(Other, Format, Area, Product, Role, Number))
			{
				return true;
			}
		}
		return false;
	};
	for (int32 Number = 1; Number <= 999; ++Number)
	{
		if (MatchesName(Current, Format, Area, Product, Role, Number) && !IsTaken(Number))
		{
			return Number;
		}
	}
	for (int32 Number = 1; Number <= 999; ++Number)
	{
		if (!IsTaken(Number))
		{
			return Number;
		}
	}
	return 0;
}

void STSetup::NameStation(const ASTSubsystem& Subsystem, STMigration::FStationProposal& Proposal, TSet<FString>& Taken)
{
	const AFGTrainStationIdentifier* Station = Proposal.Station.Get();
	const AFGBuildableRailroadStation* Building = Station ? Station->GetStation() : nullptr;
	if (!Building)
	{
		return;
	}
	const FFactory Factory = AnalyzeFactory(Subsystem, Proposal);
	if (!Factory.Product)
	{
		Proposal.NameNote = TEXT("no product found (nothing attached, or no item on its platforms): name kept");
		return;
	}
	const bool bSupply = Proposal.Platforms.ContainsByPredicate([](const STMigration::FPlatformProposal& Each) { return Each.Role == ESTRole::Supply && Each.Items.Num() > 0; });
	const bool bDemand = Proposal.Platforms.ContainsByPredicate([](const STMigration::FPlatformProposal& Each) { return Each.Role == ESTRole::Demand && Each.Items.Num() > 0; });
	const FSTNamingConfig& Naming = Subsystem.mNaming;
	// One item unloaded: named by that item and how much of it the factory uses. Several: by what the factory makes.
	const bool bOneInput = Factory.InputItem != nullptr;
	const FString Role = bSupply && bDemand ? Naming.MixedWord : (bSupply ? Naming.SupplyWord : (bOneInput ? Naming.ConsumerWord : Naming.FactoryWord));
	FString AreaSource;
	const FString Area = GetMapArea(Building->GetWorld(), Building->GetActorLocation(), AreaSource);
	const FString Product = STNames::Item(bOneInput ? Factory.InputItem : Factory.Product);
	const double NameRate = bOneInput ? Factory.InputRate : Factory.Rate;
	const FString Old = StationNameOf(Station);
	const int32 Number = PickNumber(Naming.Format, Area, Product, Role, Old, Taken);
	Proposal.NewName = FormatName(Naming.Format, Area, Product, Role, Number, FormatRate(NameRate));
	Taken.Add(Proposal.NewName);
	TArray<FString> Parts;
	if (bOneInput)
	{
		Parts.Add(FString::Printf(TEXT("unloads %s for %s"), *Product, Factory.bManufactured ? *FString::Printf(TEXT("a factory making %s"), *STNames::Item(Factory.Product))
			: TEXT("machines that make nothing")));
		if (NameRate > 0.0)
		{
			Parts.Add(FString::Printf(TEXT("at most %s/min, limited by %s"), *FormatRate(NameRate), *Factory.InputLimit));
		}
	}
	else
	{
		Parts.Add(Factory.bManufactured ? FString::Printf(TEXT("%d machines, product %s"), Factory.Machines, *Product) : FString::Printf(TEXT("no machine makes it, item %s"), *Product));
		if (Factory.Rate > 0.0)
		{
			Parts.Add(FString::Printf(TEXT("at most %s/min, limited by %s"), *FormatRate(Factory.Rate), *Factory.Limit));
		}
	}
	Parts.Add(Area.IsEmpty() ? FString(TEXT("no map area found")) : FString::Printf(TEXT("area from the %s"), *AreaSource));
	Parts.Append(Factory.Notes);
	Proposal.NameNote = FString::Join(Parts, TEXT("; "));
}

TArray<FString> STSetup::Describe(const STMigration::FStationProposal& Proposal)
{
	TArray<FString> Lines;
	const FString Old = StationNameOf(Proposal.Station.Get());
	Lines.Add(Proposal.NewName.IsEmpty() ? FString::Printf(TEXT("Name: %s (kept: %s)"), *Old, *Proposal.NameNote)
		: Proposal.NewName == Old ? FString::Printf(TEXT("Name: %s (unchanged)"), *Old) : FString::Printf(TEXT("Name: %s -> %s"), *Old, *Proposal.NewName));
	if (!Proposal.NewName.IsEmpty())
	{
		Lines.Add(TEXT("  ") + Proposal.NameNote);
	}
	for (const STMigration::FPlatformProposal& Platform : Proposal.Platforms)
	{
		if (!Platform.bCargo)
		{
			Lines.Add(FString::Printf(TEXT("P%d: can't move cargo"), Platform.Index + 1));
			continue;
		}
		TArray<FString> Items;
		for (const TSubclassOf<UFGItemDescriptor>& Item : Platform.Items)
		{
			Items.Add(STNames::Item(Item));
		}
		Lines.Add(FString::Printf(TEXT("P%d: %s%s%s (%s)"), Platform.Index + 1, STNames::Role(Platform.Role), Items.Num() > 0 ? TEXT(" ") : TEXT(""),
			*FString::Join(Items, TEXT(", ")), Platform.Note.IsEmpty() ? *Platform.Evidence : *(Platform.Note + TEXT("; ") + Platform.Evidence)));
	}
	return Lines;
}

//~ Subsystem

TArray<FString> ASTSubsystem::SetUpStation(AFGTrainStationIdentifier* Station, bool bApply, bool bRename, bool bPlatforms, const FString& Cause)
{
	TArray<FString> Lines;
	if (!IsValid(Station))
	{
		Lines.Add(TEXT("No station."));
		return Lines;
	}
	STMigration::FStationProposal Proposal = STSetup::ProposeStation(*this, Station);
	if (bRename)
	{
		TSet<FString> Taken;
		for (const AFGTrainStationIdentifier* Other : SetupStations(GetWorld()))
		{
			if (Other != Station)
			{
				Taken.Add(StationNameOf(Other));
			}
		}
		STSetup::NameStation(*this, Proposal, Taken);
	}
	// A preview shows what the action changes: the name (rename), the platforms (autodetect), or both.
	for (const FString& Line : STSetup::Describe(Proposal))
	{
		const bool bNameLine = Line.StartsWith(TEXT("Name:")) || Line.StartsWith(TEXT("  "));
		if ((bNameLine && bRename) || (!bNameLine && bPlatforms))
		{
			Lines.Add(Line);
		}
	}
	UE_LOG(LogSmartTrains, Log, TEXT("SETUP %s%s%s %s (%s): %s"), bApply ? TEXT("apply") : TEXT("preview"), bRename ? TEXT(" name") : TEXT(""), bPlatforms ? TEXT(" platforms") : TEXT(""),
		*STLayout::Name(Station), *Cause, *FString::Join(Lines, TEXT(" | ")));
	if (bApply)
	{
		if (!bPlatforms)
		{
			Proposal.Platforms.Reset();
		}
		const TArray<FString> Done = STSetup::ApplyProposal(*this, Proposal, bRename, Cause);
		Lines.Reset();
		Lines.Add(Done.Num() > 0 ? FString::Printf(TEXT("Done: %d change%s."), Done.Num(), Done.Num() == 1 ? TEXT("") : TEXT("s")) : FString(TEXT("Nothing to change.")));
		Lines.Append(Done);
		mSetupPreviewAt.Remove(Station);
		mSetupPreviewMode.Remove(Station);
	}
	else
	{
		mSetupPreviewAt.Add(Station, Now());
		mSetupPreviewMode.Add(Station, (bRename ? 1 : 0) | (bPlatforms ? 2 : 0));
	}
	mSetupLines.Add(Station, Lines);
	return Lines;
}

FString ASTSubsystem::AutodetectPlatform(AFGBuildableTrainPlatformCargo* Platform, const FString& Cause)
{
	AFGTrainStationIdentifier* Station = STLayout::GetIdentifier(STLayout::FindStation(Platform));
	if (!Station)
	{
		return TEXT("This platform isn't part of a station.");
	}
	STMigration::FStationProposal Proposal = STSetup::ProposeStation(*this, Station, Platform);
	const STMigration::FPlatformProposal* Own = Proposal.Platforms.FindByPredicate([Platform](const STMigration::FPlatformProposal& Each) { return Each.Platform.Get() == Platform; });
	if (!Own)
	{
		return TEXT("This platform can't be set up.");
	}
	const STMigration::FPlatformProposal Kept = *Own;
	Proposal.Platforms = {Kept};
	const TArray<FString> Done = STSetup::ApplyProposal(*this, Proposal, false, Cause);
	TArray<FString> Items;
	for (const TSubclassOf<UFGItemDescriptor>& Item : Kept.Items)
	{
		Items.Add(STNames::Item(Item));
	}
	const FString Found = FString::Printf(TEXT("%s%s%s"), STNames::Role(Kept.Role), Items.Num() > 0 ? TEXT(" ") : TEXT(""), *FString::Join(Items, TEXT(", ")));
	const FString Message = Kept.Role != ESTRole::Off && Kept.Items.Num() == 0 ? FString::Printf(TEXT("Autodetect: %s, but no item found (%s); left as it was."), STNames::Role(Kept.Role), *Kept.Evidence)
		: Done.Num() > 0 ? FString::Printf(TEXT("Autodetect: %s (%s)."), *Found, *Kept.Evidence) : FString::Printf(TEXT("Autodetect: already %s."), *Found);
	UE_LOG(LogSmartTrains, Log, TEXT("SETUP platform %s P%d: %s (%s)"), *STLayout::Name(Station), Kept.Index + 1, *Message, *Cause);
	return Message;
}

namespace
{
	// /st setup all, a few stations per step so the server keeps running (StepSetupAll).
	struct FSetupAllJob
	{
		TArray<TWeakObjectPtr<AFGTrainStationIdentifier>> Stations;
		int32 Next = 0;
		TSet<FString> Taken;
		FString Report;
		bool bApply = false;
		bool bRename = true;
		FString Cause;
		int32 Done = 0;
		int32 Renamed = 0;
		int32 Changes = 0;
		double StartedAt = 0.0;
	};
	TMap<TWeakObjectPtr<UWorld>, FSetupAllJob> GSetupAll;
}

TArray<FString> ASTSubsystem::SetUpAllStations(bool bApply, bool bRename, const FString& Cause)
{
	TArray<FString> Lines;
	if (GSetupAll.Contains(GetWorld()))
	{
		Lines.Add(TEXT("A set-up of every station is already running; its report follows in the log (SETUP lines)."));
		return Lines;
	}
	FSetupAllJob& Job = GSetupAll.Add(GetWorld());
	for (AFGTrainStationIdentifier* Station : SetupStations(GetWorld()))
	{
		Job.Stations.Add(Station);
		Job.Taken.Add(StationNameOf(Station));
	}
	Job.Report = TEXT("station,old name,new name,how,platforms\n");
	Job.bApply = bApply;
	Job.bRename = bRename;
	Job.Cause = Cause;
	Job.StartedAt = FPlatformTime::Seconds();
	StartSetupSteps();
	Lines.Add(FString::Printf(TEXT("Station set-up %s for %d stations started, a few per moment so the server keeps running. The report goes to Saved/SmartTrains (SETUP lines in the log)."),
		bApply ? TEXT("apply") : TEXT("preview"), Job.Stations.Num()));
	UE_LOG(LogSmartTrains, Log, TEXT("SETUP %s (%s)"), *Lines[0], *Cause);
	return Lines;
}

bool ASTSubsystem::StepSetupAll(double BudgetMs)
{
	FSetupAllJob* Job = GSetupAll.Find(GetWorld());
	if (!Job)
	{
		return true;
	}
	const double Until = FPlatformTime::Seconds() + BudgetMs / 1000.0;
	bool bFirst = true;
	while (Job->Next < Job->Stations.Num() && (bFirst || FPlatformTime::Seconds() < Until))
	{
		bFirst = false;
		AFGTrainStationIdentifier* Station = Job->Stations[Job->Next++].Get();
		if (!IsValid(Station))
		{
			continue;
		}
		STMigration::FStationProposal Proposal = STSetup::ProposeStation(*this, Station);
		if (!Proposal.Platforms.ContainsByPredicate([](const STMigration::FPlatformProposal& Each) { return Each.bCargo; }))
		{
			continue;
		}
		++Job->Done;
		const FString Old = StationNameOf(Station);
		Job->Taken.Remove(Old);
		STSetup::NameStation(*this, Proposal, Job->Taken);
		if (Proposal.NewName.IsEmpty())
		{
			Job->Taken.Add(Old);
		}
		const TArray<FString> Described = STSetup::Describe(Proposal);
		TArray<FString> PlatformLines;
		for (const FString& Line : Described)
		{
			if (Line.StartsWith(TEXT("P")))
			{
				PlatformLines.Add(Line);
			}
		}
		Job->Report += FString::Printf(TEXT("%s,%s,%s,%s,%s\n"), *SetupCsv(STLayout::Name(Station)), *SetupCsv(Old), *SetupCsv(Proposal.NewName), *SetupCsv(Proposal.NameNote),
			*SetupCsv(FString::Join(PlatformLines, TEXT(" | "))));
		UE_LOG(LogSmartTrains, Log, TEXT("SETUP %s %s: %s"), Job->bApply ? TEXT("apply") : TEXT("preview"), *Old, *FString::Join(Described, TEXT(" | ")));
		Job->Renamed += !Proposal.NewName.IsEmpty() && Proposal.NewName != Old ? 1 : 0;
		if (Job->bApply)
		{
			Job->Changes += STSetup::ApplyProposal(*this, Proposal, Job->bRename, Job->Cause).Num();
		}
	}
	if (Job->Next < Job->Stations.Num())
	{
		return false;
	}
	const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SmartTrains"), FString::Printf(TEXT("StationSetup-%s.csv"), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"))));
	const bool bWritten = FFileHelper::SaveStringToFile(Job->Report, *Path);
	UE_LOG(LogSmartTrains, Log, TEXT("SETUP Station set-up %s: %d stations, %d new names%s, %.1f s. Report %s %s. (%s)"), Job->bApply ? TEXT("applied") : TEXT("preview"), Job->Done,
		Job->Renamed, Job->bApply ? *FString::Printf(TEXT(", %d changes"), Job->Changes) : TEXT(""), FPlatformTime::Seconds() - Job->StartedAt,
		bWritten ? TEXT("written to") : TEXT("NOT written to"), *Path, *Job->Cause);
	GSetupAll.Remove(GetWorld());
	return true;
}

bool ASTSubsystem::StepMachineFill(double BudgetMs)
{
	if (bMachinesFilled || mMachineFillQueue.Num() == 0)
	{
		return true;
	}
	const double Until = FPlatformTime::Seconds() + BudgetMs / 1000.0;
	do
	{
		AFGBuildableTrainPlatformCargo* Platform = mMachineFillQueue.Pop(EAllowShrinking::No).Get();
		const FSTPlatformConfig* Config = Platform ? FindPlatformConfig(Platform) : nullptr;
		if (Config && (Config->Role == ESTRole::Supply || Config->Role == ESTRole::Demand) && Config->Machines.Num() == 0)
		{
			SetPlatformMachines(Platform, STTrace::Trace(Platform, Config->Role == ESTRole::Supply).MachineActors);
		}
	}
	while (mMachineFillQueue.Num() > 0 && FPlatformTime::Seconds() < Until);
	if (mMachineFillQueue.Num() > 0)
	{
		return false;
	}
	bMachinesFilled = true;
	UE_LOG(LogSmartTrains, Log, TEXT("SETUP platforms set up before 0.14.0 traced once; from now on only Autodetect, /st setup and the migration trace platforms"));
	return true;
}

void ASTSubsystem::StartSetupSteps()
{
	FTimerManager& Timers = GetWorldTimerManager();
	if (Timers.IsTimerActive(mSetupTimer))
	{
		return;
	}
	Timers.SetTimer(mSetupTimer, FTimerDelegate::CreateWeakLambda(this, [this]()
	{
		// Each step takes about this long, at least one station. [ms]
		constexpr double StepMs = 15.0;
		const bool bNamesDone = STMigration::StepNaming(this, GetWorld(), StepMs);
		const bool bAllDone = StepSetupAll(StepMs);
		const bool bFillDone = StepMachineFill(StepMs);
		if (bNamesDone && bAllDone && bFillDone)
		{
			GetWorldTimerManager().ClearTimer(mSetupTimer);
		}
	}), 0.05f, FTimerManagerTimerParameters{.bLoop = true, .bMaxOncePerFrame = true});
}

bool ASTSubsystem::SetNaming(const TArray<FString>& Words, const FString& Cause, FString& OutMessage)
{
	// Words: "format <text...>" or "words <supply> <factory> <consumer> [mixed]" ("-" for none).
	if (Words.Num() >= 2 && Words[0].Equals(TEXT("format"), ESearchCase::IgnoreCase))
	{
		TArray<FString> Rest = Words;
		Rest.RemoveAt(0);
		const FString Format = FString::Join(Rest, TEXT(" ")).TrimQuotes();
		if (!Format.Contains(TEXT("{product}")))
		{
			OutMessage = TEXT("The format needs {product}; also {area}, {role}, {n} and {rate}, e.g. {area} {product} {role} {n} ({rate}).");
			return false;
		}
		mNaming.Format = Format;
	}
	else if (Words.Num() >= 4 && Words[0].Equals(TEXT("words"), ESearchCase::IgnoreCase))
	{
		auto Word = [](const FString& Text) { return Text == TEXT("-") ? FString() : Text; };
		mNaming.SupplyWord = Word(Words[1]);
		mNaming.FactoryWord = Word(Words[2]);
		mNaming.ConsumerWord = Word(Words[3]);
		mNaming.MixedWord = Words.Num() >= 5 ? Word(Words[4]) : FString();
	}
	else if (Words.Num() >= 1 && Words[0].Equals(TEXT("default"), ESearchCase::IgnoreCase))
	{
		mNaming = FSTNamingConfig();
	}
	else
	{
		OutMessage = TEXT("Use /st setup format <text>, /st setup words <loads> <unloads several> <unloads one> [both] (- for none), or /st setup default.");
		return false;
	}
	OutMessage = FString::Printf(TEXT("Station names: %s; {role}: loads %s, unloads several items %s, unloads one item %s, both %s. Example: %s"), *mNaming.Format,
		*mNaming.SupplyWord, *mNaming.FactoryWord, *mNaming.ConsumerWord, mNaming.MixedWord.IsEmpty() ? TEXT("(none)") : *mNaming.MixedWord,
		*STSetup::FormatName(mNaming.Format, TEXT("Grass Fields"), TEXT("Modular Frame"), mNaming.FactoryWord, 1, TEXT("288")));
	UE_LOG(LogSmartTrains, Log, TEXT("SETUP naming: %s (%s)"), *OutMessage, *Cause);
	return true;
}

//~ Console

namespace
{
	// SmartTrains.Setup [apply] [keepname]: every station's set-up from the server console (dedicated servers).
	void RunSetupCommand(const TArray<FString>& Args, UWorld* World)
	{
		ASTSubsystem* Subsystem = ASTSubsystem::Get(World);
		if (!Subsystem)
		{
			UE_LOG(LogSmartTrains, Warning, TEXT("SmartTrains.Setup: no SmartTrains subsystem in this world"));
			return;
		}
		auto Has = [&Args](const TCHAR* Word) { return Args.ContainsByPredicate([Word](const FString& Each) { return Each.Equals(Word, ESearchCase::IgnoreCase); }); };
		for (const FString& Line : Subsystem->SetUpAllStations(Has(TEXT("apply")), !Has(TEXT("keepname")), TEXT("server console")))
		{
			UE_LOG(LogSmartTrains, Log, TEXT("%s"), *Line);
		}
	}

	// SmartTrains.EmptyCars: deletes the cargo of every managed train (as /st emptycars all confirm), for tests that
	// start from empty trains on a dedicated server.
	void RunEmptyCarsCommand(const TArray<FString>& Args, UWorld* World)
	{
		ASTSubsystem* Subsystem = ASTSubsystem::Get(World);
		if (!Subsystem)
		{
			UE_LOG(LogSmartTrains, Warning, TEXT("SmartTrains.EmptyCars: no SmartTrains subsystem in this world"));
			return;
		}
		int32 Removed = 0;
		int32 Trains = 0;
		for (AFGTrain* Train : Subsystem->GetManagedTrains())
		{
			Removed += Subsystem->EmptyTrainCars(Train, TEXT("server console"));
			++Trains;
		}
		UE_LOG(LogSmartTrains, Log, TEXT("SmartTrains.EmptyCars: %d managed trains emptied, %d items and litres removed"), Trains, Removed);
	}

	FAutoConsoleCommandWithWorldAndArgs GEmptyCarsCommand(TEXT("SmartTrains.EmptyCars"),
		TEXT("Deletes the cargo of every managed train (no undo), as /st emptycars all confirm."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&RunEmptyCarsCommand));

	// SmartTrains.MigratePreview: the migration preview from the server console (names follow a few stations per step).
	void RunMigratePreviewCommand(const TArray<FString>& Args, UWorld* World)
	{
		ASTSubsystem* Subsystem = ASTSubsystem::Get(World);
		if (!Subsystem)
		{
			UE_LOG(LogSmartTrains, Warning, TEXT("SmartTrains.MigratePreview: no SmartTrains subsystem in this world"));
			return;
		}
		const double Started = FPlatformTime::Seconds();
		const TArray<FString> Lines = STMigration::Preview(Subsystem, World);
		UE_LOG(LogSmartTrains, Log, TEXT("SmartTrains.MigratePreview: %s (%.1f s)"), Lines.Num() > 0 ? *Lines[0] : TEXT("no lines"), FPlatformTime::Seconds() - Started);
	}

	FAutoConsoleCommandWithWorldAndArgs GMigratePreviewCommand(TEXT("SmartTrains.MigratePreview"),
		TEXT("Runs the migration preview, as /st migrate preview."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&RunMigratePreviewCommand));

	FAutoConsoleCommandWithWorldAndArgs GSetupCommand(TEXT("SmartTrains.Setup"),
		TEXT("Station set-up for every station: preview (default) or apply [keepname]. Report in Saved/SmartTrains."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&RunSetupCommand));
}
