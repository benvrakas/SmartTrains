#include "Algo/StableSort.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildableRailroadStation.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGFreightWagon.h"
#include "FGInventoryComponent.h"
#include "FGLocomotive.h"
#include "FGRailroadSubsystem.h"
#include "FGRailroadTimeTable.h"
#include "FGTrain.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "FGRecipe.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "FGTrainStationIdentifier.h"
#include "Resources/FGItemDescriptor.h"
#include "SmartTrains.h"
#include "STBridges.h"
#include "STLayout.h"
#include "STMinCostFlow.h"
#include "STTripPlanner.h"
#include "STForecast.h"
#include "STSubsystem.h"

namespace
{
	// What one written stop is: station, rules and the cars that skip it.
	FString StopKey(const FTimeTableStop& Stop, const TArray<TObjectPtr<AFGRailroadVehicle>>& SkipCars)
	{
		const FTrainDockingRuleSet& Rules = Stop.DockingRuleSet;
		FString Key = FString::Printf(TEXT("%p|%d|%.1f|%d|%d|"), ToRawPtr(Stop.Station), static_cast<int32>(Rules.DockingDefinition), Rules.DockForDuration,
			Rules.IsDurationAndRule ? 1 : 0, Rules.IgnoreFullLoadUnloadIfTransferBlockedByFilters ? 1 : 0);
		for (const TSubclassOf<UFGItemDescriptor>& Item : Rules.LoadFilterDescriptors)
		{
			Key += FString::Printf(TEXT("L%p"), Item.Get());
		}
		for (const TSubclassOf<UFGItemDescriptor>& Item : Rules.UnloadFilterDescriptors)
		{
			Key += FString::Printf(TEXT("U%p"), Item.Get());
		}
		TArray<UPTRINT> Cars;
		for (const TObjectPtr<AFGRailroadVehicle>& Car : SkipCars)
		{
			Cars.Add(reinterpret_cast<UPTRINT>(Car.Get()));
		}
		Cars.Sort();
		for (const UPTRINT Car : Cars)
		{
			Key += FString::Printf(TEXT("S%llx"), static_cast<uint64>(Car));
		}
		return Key;
	}

	// A parked train's booking of its station: about how long moving it out takes. [s]
	constexpr double ParkedLeaveSeconds = 60.0;
	// A station counts as queued for repeats once this many managed trains are on their way to it.
	constexpr int32 QueuedTrains = 2;
	// A train that left a station this recently doesn't start a trip with that station again. [s]
	constexpr double JustLeftAnchorSeconds = 60.0;
	// Request setting On: a platform asks until it and its deliveries on the way reach this share of its capacity.
	constexpr double RequestFullShare = 0.9;
	constexpr double TrainSecondCost = STForecast::TrainSecondCost;
	// A consumer whose machines wait on another starved item counts its shortfall this much (Docs/Architecture.md,
	// "Supply chain").
	constexpr double WaitingCostScale = 0.25;

	// What Machines' recipes make (bProducts) or use of Item per second at their clock speed. [/s]
	double GetRecipeRate(const TArray<TWeakObjectPtr<const AActor>>& Machines, UClass* Item, bool bProducts)
	{
		double PerSecond = 0.0;
		for (const TWeakObjectPtr<const AActor>& Machine : Machines)
		{
			const AFGBuildableManufacturer* Manufacturer = Cast<AFGBuildableManufacturer>(Machine.Get());
			const TSubclassOf<UFGRecipe> Recipe = Manufacturer ? Manufacturer->GetCurrentRecipe() : nullptr;
			const float Duration = Recipe ? UFGRecipe::GetManufacturingDuration(Recipe) : 0.f;
			if (Duration <= 0.f)
			{
				continue;
			}
			const float Clock = Manufacturer->GetCurrentPotential() > 0.f ? Manufacturer->GetCurrentPotential() : 1.f;
			const TArray<FItemAmount> Amounts = bProducts ? UFGRecipe::GetProducts(Recipe)
				: UFGRecipe::GetIngredients(const_cast<AFGBuildableManufacturer*>(Manufacturer), Recipe);
			for (const FItemAmount& Amount : Amounts)
			{
				PerSecond += Amount.ItemClass == Item ? Amount.Amount * Clock / Duration : 0.0;
			}
		}
		return PerSecond;
	}
	// A re-plan after a stop replaces the rest of the trip only if it is worth this much more as well. [car-load minutes]
	constexpr double ReplanMarginValue = 5.0;
	// Roster: least share of a train worth giving a route; routes keep their trains while trains needed change by less
	// than this share; a roster trip runs if it delivers at least this share of a car.
	constexpr double RosterMinShare = 0.05;
	constexpr double RosterKeepShare = 0.25;
	constexpr double RosterMinCarShare = 0.25;
	// Roster: flow plans in a row a route must be in before it joins.
	constexpr int32 RosterStablePlans = 3;
	// A route gets a shuttle (a whole train running only it) while what it still needs is at least this much of that
	// train; it keeps one while it needs at least ShuttleKeepTrains. [trains]
	constexpr double ShuttleMinTrains = 0.7;
	constexpr double ShuttleKeepTrains = 0.5;
	// Most of the managed trains that may be shuttles.
	constexpr double ShuttleFleetShare = 0.6;
	// A service keeps a train it needs less than ShuttleKeepTrains of until this many roster builds in a row did.
	constexpr int32 ServiceDropBuilds = 6;
	// Share of its travel time a route of the last flow plan costs in the next one, so near-equal routes don't swap.
	constexpr double FlowKeepCost = 0.9;
	// A shuttle's roster trip runs if it delivers at least this share of what the train carries on that route.
	constexpr double ShuttleLoadShare = 0.5;
	// Flow plan: a supplier at least this full backs up, so its measured rate (what trains took) is only a lower
	// bound; it also offers its stock spread over FullStockSeconds. [s]
	constexpr double FullSupplyShare = 0.9;
	constexpr double FullStockSeconds = 1800.0;
	// Horizon search: stops after this many tries in a row that found nothing better.
	constexpr int32 HorizonStaleTries = 6;
	// Horizon planner: a train is emptied only for platforms that fall below target within this. [s]
	constexpr double HorizonEmptyingSeconds = 1800.0;
	// Requests: only platforms that fall below target within this (plus the margin) are planned for; the same note is
	// logged again after this long. [s]
	constexpr double RequestLookSeconds = 1800.0;
	constexpr double RequestNoteRepeatSeconds = 600.0;
	constexpr int32 HorizonShortTargets = 6;
	constexpr int32 HorizonMaxTries = 40;
	// Trip limit for request trips, which may stop to empty cars first. [s]
	constexpr double RequestTripSeconds = 3600.0;
	// How often a waiting train with a trip to repeat tries it again. [s]
	constexpr double RepeatRetrySeconds = 30.0;
	// A parked train in a dead-end station books it this far ahead, i.e. until it leaves (booked again every round). [s]
	constexpr double DeadEndHoldSeconds = 3600.0;
	// Auto Trains travel-time questions per dispatch round. [calls]
	constexpr int32 AtTravelCallsPerRound = 4;

	// With Auto Trains: a parked managed train moves when a train has waited this long for it. [s]
	constexpr float AtHeldUpSeconds = 20.f;
	// How often Auto Trains is asked for a station's queue. [s]
	constexpr double AtQueueSeconds = 15.0;

	// A trip is planned again because a parked train can't leave a station it needs at most this often. [s]
	constexpr double RerouteAgainSeconds = 60.0;

	// A train that undocked from a dead-end station this recently and still stands counts as in it for holding. [s]
	constexpr double JustLeftSeconds = 15.0;

	// A planned trip whose first stop is free this much later than the train could arrive is not started yet. [s]
	constexpr double HoldAfterWaitSeconds = 20.0;

	FString NodeKey(const FSTNode& Node)
	{
		return FString::Printf(TEXT("%p|%p|%d"), Node.Station.Get(), Node.Item.Get(), static_cast<int32>(Node.Role));
	}

	FString PairKey(const FSTNode& From, const FSTNode& To)
	{
		return NodeKey(From) + TEXT(">") + NodeKey(To);
	}

	// Freight cars of Train, front to back.
	TArray<AFGFreightWagon*> GetCars(const AFGTrain* Train)
	{
		TArray<AFGFreightWagon*> Cars;
		for (AFGRailroadVehicle* Vehicle : STLayout::GetVehicles(Train))
		{
			if (AFGFreightWagon* Car = Cast<AFGFreightWagon>(Vehicle))
			{
				Cars.Add(Car);
			}
		}
		return Cars;
	}

	// The cargo platform at Index of Platforms, or nullptr.
	AFGBuildableTrainPlatformCargo* CargoAt(const TArray<AFGBuildableTrainPlatform*>& Platforms, int32 Index)
	{
		return Platforms.IsValidIndex(Index) ? Cast<AFGBuildableTrainPlatformCargo>(Platforms[Index]) : nullptr;
	}

	FTrainDockingRuleSet MakeRules(const FSTJobLeg& Leg, const FSTSettings& Settings)
	{
		FTrainDockingRuleSet Rules;
		Rules.IsDurationAndRule = false;
		// A car only moves its filtered items, so cargo kept for a later stop never counts against "fully".
		Rules.IgnoreFullLoadUnloadIfTransferBlockedByFilters = true;
		const TSubclassOf<UFGItemDescriptor> None = nullptr;
		const TArray<UClass*> Loads = Leg.GetItems(true);
		const TArray<UClass*> Unloads = Leg.GetItems(false);
		for (UClass* Item : Loads)
		{
			Rules.LoadFilterDescriptors.Add(Item);
		}
		for (UClass* Item : Unloads)
		{
			Rules.UnloadFilterDescriptors.Add(Item);
		}
		// An empty filter means everything to the game, so "nothing" is the none descriptor.
		if (Rules.LoadFilterDescriptors.Num() == 0)
		{
			Rules.LoadFilterDescriptors.Add(None);
		}
		if (Rules.UnloadFilterDescriptors.Num() == 0)
		{
			Rules.UnloadFilterDescriptors.Add(None);
		}
		// A fluid car empties into its platform in one transfer; waiting longer only drains a little more into the pipes
		// while the train could be refilling.
		bool bFluidDeliveryOnly = Loads.Num() == 0 && Unloads.Num() > 0;
		for (UClass* Item : Unloads)
		{
			bFluidDeliveryOnly &= STNames::IsFluid(Item);
		}
		if (Settings.bLeaveAtOnce || !Leg.HasWork() || Leg.bPartial || bFluidDeliveryOnly)
		{
			Rules.DockingDefinition = ETrainDockingDefinition::TDD_LoadUnloadOnce;
			Rules.DockForDuration = 0.f;
		}
		else
		{
			Rules.DockingDefinition = ETrainDockingDefinition::TDD_FullyLoadUnload;
			Rules.DockForDuration = Loads.Num() > 0 ? Settings.LoadWaitSeconds : Settings.UnloadWaitSeconds;
		}
		return Rules;
	}
}

//~ Nodes

void ASTSubsystem::BuildNodes(TArray<FSTNode>& OutNodes) const
{
	OutNodes.Reset();
	for (const FSTPlatformConfig& Config : mPlatforms)
	{
		AFGBuildableTrainPlatformCargo* Platform = Config.Platform;
		if (Config.Role == ESTRole::Off || !IsValid(Platform))
		{
			continue;
		}
		AFGBuildableRailroadStation* Station = STLayout::FindStation(Platform);
		AFGTrainStationIdentifier* Identifier = STLayout::GetIdentifier(Station);
		if (!Identifier)
		{
			continue;
		}
		const UFGInventoryComponent* Inventory = Platform->GetInventory();
		for (const FSTItemRule& Rule : Config.Items)
		{
			if (!Rule.Item && !STNames::Unloads(Config.Role))
			{
				continue;
			}
			// A sink is a demand for any item.
			const ESTRole NodeRole = Config.Role == ESTRole::Sink ? ESTRole::Demand : Config.Role;
			int32 Index = FindNode(OutNodes, Identifier, Rule.Item, NodeRole);
			if (Index == INDEX_NONE)
			{
				Index = OutNodes.AddDefaulted();
				FSTNode& New = OutNodes[Index];
				New.Station = Identifier;
				New.Item = Rule.Item;
				New.Role = NodeRole;
				New.Request = ESTRequest::Off;
				New.bAnyItem = !Rule.Item;
				New.Priority = Rule.Priority;
				New.TargetPercent = Rule.TargetPercent;
				New.SendAtPercent = Rule.SendAtPercent;
				New.TrackGraph = Station->GetTrackGraphID();
				New.bRateKnown = true;
			}
			FSTNode& Node = OutNodes[Index];
			Node.Platforms.Add(Platform);
			Node.Priority = FMath::Max(Node.Priority, Rule.Priority);
			// The most eager platform's request setting: On, then Auto, then Off.
			if (NodeRole == ESTRole::Demand && Rule.Item && (Config.Request == ESTRequest::On || (Config.Request == ESTRequest::Auto && Node.Request == ESTRequest::Off)))
			{
				Node.Request = Config.Request;
			}
			if (!Rule.Item)
			{
				continue;
			}
			Node.Stock += Inventory ? Inventory->GetNumItems(Rule.Item) : 0;
			Node.Capacity += STLayout::GetCapacity(Inventory, Rule.Item);
			bool bKnown = false;
			Node.Rate += Rule.ManualRatePerMinute > 0.f ? Rule.ManualRatePerMinute / 60.0 : GetMeasuredRate(Platform, Rule.Item, &bKnown);
			Node.bRateKnown &= bKnown || Rule.ManualRatePerMinute > 0.f;
			const FPlatformStats* Stats = mStats.Find(Platform);
			const FItemStats* ItemStats = Stats ? Stats->Items.Find(Rule.Item.Get()) : nullptr;
			Node.bStarved |= Config.Role == ESTRole::Demand && ItemStats && ItemStats->EmptySeconds > 30.0;
		}
	}
	// Machines and recipe rates, from the traces of each node's platforms.
	for (FSTNode& Node : OutNodes)
	{
		if (!Node.Item || Node.bAnyItem)
		{
			continue;
		}
		for (const TWeakObjectPtr<AFGBuildableTrainPlatformCargo>& Platform : Node.Platforms)
		{
			if (const FTraceCache* Cache = mTraces.Find(Platform))
			{
				for (const TWeakObjectPtr<const AActor>& Machine : Cache->Machines)
				{
					Node.Machines.AddUnique(Machine);
				}
			}
		}
		Node.RecipeRate = GetRecipeRate(Node.Machines, Node.Item.Get(), Node.Role == ESTRole::Supply);
	}
	LinkSupplyChain(OutNodes);
	// Amounts promised by jobs still to come (repeats promise nothing).
	for (const FSTTrainState& State : mTrains)
	{
		if (!State.bManaged || !State.bHasJob || State.Job.bRepeat)
		{
			continue;
		}
		for (int32 Leg = State.Job.Leg; Leg < State.Job.Legs.Num(); ++Leg)
		{
			const FSTJobLeg& Each = State.Job.Legs[Leg];
			for (const FSTCarWork& Work : Each.Work)
			{
				int32 Index = FindNode(OutNodes, Each.Station, Work.Item, Work.bLoad ? ESTRole::Supply : ESTRole::Demand);
				if (Index == INDEX_NONE && !Work.bLoad)
				{
					Index = FindNode(OutNodes, Each.Station, nullptr, ESTRole::Demand);
				}
				if (Index != INDEX_NONE)
				{
					OutNodes[Index].Planned.Add({Each.PlannedArrival, static_cast<double>(Work.PlannedAmount)});
					OutNodes[Index].Reserved += Work.PlannedAmount;
					if (Work.Platform)
					{
						OutNodes[Index].PlatformReserved.FindOrAdd(Work.Platform.Get()) += Work.PlannedAmount;
					}
				}
			}
		}
	}
	for (FSTNode& Node : OutNodes)
	{
		if (const double* Planned = mPlannedPairs.Find(NodeKey(Node)))
		{
			Node.PlannedFlow = *Planned;
		}
	}
}

void ASTSubsystem::LinkSupplyChain(TArray<FSTNode>& Nodes)
{
	// A consumer with stock whose machines also wait on a starved item can't use what it gets, so its shortfall counts
	// less until that item arrives.
	for (FSTNode& Node : Nodes)
	{
		if (Node.Role != ESTRole::Demand || Node.bAnyItem || !Node.Item || Node.bStarved || Node.Machines.Num() == 0)
		{
			continue;
		}
		for (const FSTNode& Other : Nodes)
		{
			if (&Other != &Node && Other.Role == ESTRole::Demand && !Other.bAnyItem && Other.Item && Other.Item != Node.Item && Other.bStarved
				&& Other.Machines.ContainsByPredicate([&](const TWeakObjectPtr<const AActor>& Machine) { return Node.Machines.Contains(Machine); }))
			{
				Node.CostScale = WaitingCostScale;
				Node.WaitsOn = Other.Item;
				break;
			}
		}
	}
}

int32 ASTSubsystem::FindNode(const TArray<FSTNode>& Nodes, const AFGTrainStationIdentifier* Station, TSubclassOf<UFGItemDescriptor> Item, ESTRole NodeRole) const
{
	return Nodes.IndexOfByPredicate([&](const FSTNode& Node) { return Node.Station.Get() == Station && Node.Item == Item && Node.Role == NodeRole; });
}

int32 ASTSubsystem::GetSinkRoom(const FSTNode& Node, TSubclassOf<UFGItemDescriptor> Item) const
{
	int64 Room = 0;
	for (const TWeakObjectPtr<AFGBuildableTrainPlatformCargo>& Platform : Node.Platforms)
	{
		const UFGInventoryComponent* Inventory = Platform.IsValid() ? Platform->GetInventory() : nullptr;
		if (!Inventory || !STLayout::ItemFitsCargoType(Item, STLayout::GetPlatformCargoType(Platform.Get())))
		{
			continue;
		}
		const int32 Stack = STLayout::GetSlotSize(Inventory, Item);
		TArray<FInventoryStack> Stacks;
		Inventory->GetInventoryStacks(Stacks, true);
		for (const FInventoryStack& Each : Stacks)
		{
			if (!Each.HasItems())
			{
				Room += Stack;
			}
			else if (Each.Item.GetItemClass() == Item)
			{
				Room += FMath::Max(0, Stack - Each.NumItems);
			}
		}
	}
	return static_cast<int32>(FMath::Min<int64>(Room, MAX_int32));
}

bool ASTSubsystem::CanServe(const FSTTrainState& State, const AFGTrainStationIdentifier* Station) const
{
	const AFGBuildableRailroadStation* Building = Station ? Station->GetStation() : nullptr;
	if (!Building || !IsValid(State.Train) || Building->GetTrackGraphID() != State.Train->GetTrackGraphID())
	{
		return false;
	}
	const double* Until = State.Unreachable.Find(const_cast<AFGTrainStationIdentifier*>(Station));
	return !Until || *Until <= Now();
}

//~ Fit model

TArray<TArray<AFGRailroadVehicle*>> ASTSubsystem::GetDockMaps(const AFGTrain* Train, const AFGTrainStationIdentifier* Station, bool* bLearned) const
{
	TArray<TArray<AFGRailroadVehicle*>> Learned;
	TArray<const AFGLocomotive*> LearnedBy;
	const TArray<AFGRailroadVehicle*> Vehicles = STLayout::GetVehicles(Train);
	for (const FSTDockMap& Seen : mDockMaps)
	{
		if (Seen.Train != Train || Seen.Station != Station)
		{
			continue;
		}
		// A seen map is only used while the train is made of the same vehicles in the same order.
		bool bValid = true;
		if (Seen.Consist.Num() > 0)
		{
			bValid = Seen.Consist.Num() == Vehicles.Num();
			for (int32 Index = 0; bValid && Index < Vehicles.Num(); ++Index)
			{
				bValid = Seen.Consist[Index] == Vehicles[Index];
			}
		}
		TArray<AFGRailroadVehicle*> Map;
		for (const TObjectPtr<AFGRailroadVehicle>& Vehicle : Seen.Vehicles)
		{
			bValid &= !Vehicle || Vehicles.Contains(Vehicle.Get());
			Map.Add(Vehicle);
		}
		if (bValid)
		{
			Learned.Add(MoveTemp(Map));
			LearnedBy.Add(Seen.Locomotive.Get());
		}
	}
	if (bLearned)
	{
		*bLearned = Learned.Num() > 0;
	}
	TArray<TArray<AFGRailroadVehicle*>> Predicted;
	TArray<const AFGLocomotive*> PredictedBy;
	const AFGBuildableRailroadStation* Building = Station ? Station->GetStation() : nullptr;
	const int32 PlatformCount = STLayout::GetPlatforms(Building).Num();
	for (AFGLocomotive* Locomotive : STLayout::GetPossibleDockingLocomotives(Train))
	{
		TArray<AFGRailroadVehicle*> Behind = STLayout::GetVehiclesBehind(Locomotive);
		Behind.SetNum(FMath::Min(Behind.Num(), PlatformCount));
		if (!Predicted.Contains(Behind))
		{
			Predicted.Add(MoveTemp(Behind));
			PredictedBy.Add(Locomotive);
		}
	}
	if (Learned.Num() == 0)
	{
		return Predicted;
	}
	// Maps that know their docking locomotive: each replaces the prediction for that locomotive; the other ways round
	// stay predicted until seen.
	if (!LearnedBy.Contains(nullptr))
	{
		TArray<TArray<AFGRailroadVehicle*>> Maps = Learned;
		for (int32 Index = 0; Index < Predicted.Num(); ++Index)
		{
			if (!LearnedBy.Contains(PredictedBy[Index]))
			{
				Maps.Add(Predicted[Index]);
			}
		}
		return Maps;
	}
	// A learned map stands for the way round it was seen; the other ways round stay predicted until seen. If a learned
	// map matches no prediction, the predictions are wrong for this station and only what was seen is used.
	auto SameWayRound = [](const TArray<AFGRailroadVehicle*>& A, const TArray<AFGRailroadVehicle*>& B)
	{
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (A[Index])
			{
				return B.IsValidIndex(Index) && B[Index] == A[Index];
			}
		}
		return false;
	};
	TArray<TArray<AFGRailroadVehicle*>> Maps = Learned;
	bool bAllMatch = true;
	for (const TArray<AFGRailroadVehicle*>& Seen : Learned)
	{
		bAllMatch &= Predicted.ContainsByPredicate([&](const TArray<AFGRailroadVehicle*>& Each) { return SameWayRound(Seen, Each); });
	}
	if (bAllMatch)
	{
		for (const TArray<AFGRailroadVehicle*>& Each : Predicted)
		{
			if (!Learned.ContainsByPredicate([&](const TArray<AFGRailroadVehicle*>& Seen) { return SameWayRound(Seen, Each); }))
			{
				Maps.Add(Each);
			}
		}
	}
	return Maps;
}

FSTFit ASTSubsystem::ComputeFit(const AFGTrain* Train, const AFGTrainStationIdentifier* From, const AFGTrainStationIdentifier* To, TSubclassOf<UFGItemDescriptor> Item,
	bool bIgnoreCargo) const
{
	FSTFit Fit;
	const AFGBuildableRailroadStation* FromStation = From ? From->GetStation() : nullptr;
	const AFGBuildableRailroadStation* ToStation = To ? To->GetStation() : nullptr;
	if (!Train || !FromStation || !ToStation || !Item)
	{
		Fit.Problem = TEXT("missing train, station or item");
		return Fit;
	}
	const TArray<AFGBuildableTrainPlatform*> FromPlatforms = STLayout::GetPlatforms(FromStation);
	const TArray<AFGBuildableTrainPlatform*> ToPlatforms = STLayout::GetPlatforms(ToStation);
	const TArray<TArray<AFGRailroadVehicle*>> FromMaps = GetDockMaps(Train, From);
	const TArray<TArray<AFGRailroadVehicle*>> ToMaps = GetDockMaps(Train, To);
	if (FromMaps.Num() == 0 || ToMaps.Num() == 0)
	{
		Fit.Problem = TEXT("the train has no locomotive that can dock");
		return Fit;
	}
	const bool bFcc = IsFccAvailable();

	for (AFGFreightWagon* Car : GetCars(Train))
	{
		bool bFitsFrom = true;
		bool bMayLoad = false;
		for (const TArray<AFGRailroadVehicle*>& Map : FromMaps)
		{
			const int32 Index = Map.IndexOfByKey(Car);
			const AFGBuildableTrainPlatformCargo* Platform = CargoAt(FromPlatforms, Index);
			const FSTPlatformConfig* Config = Platform ? FindPlatformConfig(Platform) : nullptr;
			const bool bTypeOk = Platform && STLayout::ItemFitsCargoType(Item, STLayout::GetPlatformCargoType(Platform))
				&& STLayout::ItemFitsCargoType(Item, STLayout::GetCargoType(Car));
			const bool bSupplies = bTypeOk && Config && Config->Role == ESTRole::Supply && Config->FindRule(Item);
			bFitsFrom &= bSupplies;
			const bool bOtherLoader = bTypeOk && !bSupplies && Platform->GetIsInLoadMode() && Platform->GetInventory() && Platform->GetInventory()->GetNumItems(Item) > 0;
			bMayLoad |= bSupplies || bOtherLoader;
		}
		bool bFitsTo = true;
		for (const TArray<AFGRailroadVehicle*>& Map : ToMaps)
		{
			const int32 Index = Map.IndexOfByKey(Car);
			const AFGBuildableTrainPlatformCargo* Platform = CargoAt(ToPlatforms, Index);
			const FSTPlatformConfig* Config = Platform ? FindPlatformConfig(Platform) : nullptr;
			bFitsTo &= Platform && Config && STNames::Unloads(Config->Role) && Config->FindRule(Item)
				&& STLayout::ItemFitsCargoType(Item, STLayout::GetPlatformCargoType(Platform));
		}
		// Empty, or holding only this item (it tops up or delivers it first).
		bool bEmpty = true;
		if (const UFGInventoryComponent* Inventory = bIgnoreCargo ? nullptr : Car->GetFreightInventory())
		{
			TArray<FInventoryStack> Stacks;
			Inventory->GetInventoryStacks(Stacks, true);
			for (const FInventoryStack& Stack : Stacks)
			{
				if (!Stack.HasItems())
				{
					continue;
				}
				bEmpty &= Stack.Item.GetItemClass() == Item;
			}
		}
		if (bFitsFrom && bFitsTo && bEmpty)
		{
			Fit.Cars.Add(Car);
			Fit.Capacity += static_cast<int32>(STLayout::GetCarCapacity(Car, Item));
		}
		else if (bMayLoad)
		{
			if (bFcc)
			{
				Fit.SkipAtPickup.Add(Car);
			}
			else if (Fit.Problem.IsEmpty())
			{
				Fit.Problem = FString::Printf(TEXT("%s would load %s at %s but %s"), *STLayout::DescribeVehicle(Train, Car), *STNames::Item(Item), *STLayout::Name(From),
					!bEmpty ? TEXT("already carries another item") : *FString::Printf(TEXT("doesn't stop beside a demand platform for it at %s (needs Freight Car Control to skip that car)"), *STLayout::Name(To)));
			}
		}
	}
	if (Fit.Cars.Num() == 0 && Fit.Problem.IsEmpty())
	{
		Fit.Problem = FString::Printf(TEXT("no car stops beside a supply platform for %s at %s and a demand platform for it at %s"), *STNames::Item(Item),
			*STLayout::Name(From), *STLayout::Name(To));
	}
	if (!Fit.Problem.IsEmpty())
	{
		Fit.Capacity = 0;
	}
	return Fit;
}

FSTFit ASTSubsystem::ComputeDeliveryFit(const AFGTrain* Train, const AFGTrainStationIdentifier* To, TSubclassOf<UFGItemDescriptor> Item) const
{
	FSTFit Fit;
	const AFGBuildableRailroadStation* ToStation = To ? To->GetStation() : nullptr;
	if (!Train || !ToStation || !Item)
	{
		Fit.Problem = TEXT("missing train, station or item");
		return Fit;
	}
	const TArray<AFGBuildableTrainPlatform*> ToPlatforms = STLayout::GetPlatforms(ToStation);
	const TArray<TArray<AFGRailroadVehicle*>> ToMaps = GetDockMaps(Train, To);
	const bool bFcc = IsFccAvailable();
	for (AFGFreightWagon* Car : GetCars(Train))
	{
		const UFGInventoryComponent* Inventory = Car->GetFreightInventory();
		const int32 Carried = Inventory ? Inventory->GetNumItems(Item) : 0;
		if (Carried <= 0)
		{
			continue;
		}
		bool bFitsTo = ToMaps.Num() > 0;
		bool bMayUnload = false;
		for (const TArray<AFGRailroadVehicle*>& Map : ToMaps)
		{
			const AFGBuildableTrainPlatformCargo* Platform = CargoAt(ToPlatforms, Map.IndexOfByKey(Car));
			const FSTPlatformConfig* Config = Platform ? FindPlatformConfig(Platform) : nullptr;
			const bool bTypeOk = Platform && STLayout::ItemFitsCargoType(Item, STLayout::GetPlatformCargoType(Platform));
			const bool bDemands = bTypeOk && Config && STNames::Unloads(Config->Role) && Config->FindRule(Item);
			bFitsTo &= bDemands;
			bMayUnload |= bTypeOk && !bDemands && !Platform->GetIsInLoadMode();
		}
		if (bFitsTo)
		{
			Fit.Cars.Add(Car);
			Fit.Capacity += Carried;
		}
		else if (bMayUnload)
		{
			if (bFcc)
			{
				Fit.SkipAtDrop.Add(Car);
			}
			else if (Fit.Problem.IsEmpty())
			{
				Fit.Problem = FString::Printf(TEXT("%s would unload %s into a platform at %s that doesn't demand it"), *STLayout::DescribeVehicle(Train, Car),
					*STNames::Item(Item), *STLayout::Name(To));
			}
		}
	}
	if (Fit.Cars.Num() == 0 && Fit.Problem.IsEmpty())
	{
		Fit.Problem = FString::Printf(TEXT("no car carrying %s stops beside a demand platform for it at %s"), *STNames::Item(Item), *STLayout::Name(To));
	}
	if (!Fit.Problem.IsEmpty())
	{
		Fit.Capacity = 0;
	}
	return Fit;
}

//~ Dispatching

AFGTrainStationIdentifier* ASTSubsystem::FindNearestStation(const AFGTrain* Train) const
{
	const AFGRailroadVehicle* First = Train ? Train->GetFirstVehicle() : nullptr;
	AFGRailroadSubsystem* Railroad = First ? AFGRailroadSubsystem::Get(GetWorld()) : nullptr;
	if (!Railroad)
	{
		return nullptr;
	}
	TArray<AFGTrainStationIdentifier*> Stations;
	Railroad->GetTrainStations(Train->GetTrackGraphID(), Stations);
	AFGTrainStationIdentifier* Best = nullptr;
	double BestDistance = TNumericLimits<double>::Max();
	for (AFGTrainStationIdentifier* Station : Stations)
	{
		const AFGBuildableRailroadStation* Building = Station ? Station->GetStation() : nullptr;
		const double Distance = Building ? FVector::DistSquared(Building->GetActorLocation(), First->GetActorLocation()) : TNumericLimits<double>::Max();
		if (Distance < BestDistance)
		{
			BestDistance = Distance;
			Best = Station;
		}
	}
	return Best;
}

bool ASTSubsystem::GetFreeAt(const FSTTrainState& State, AFGTrainStationIdentifier*& OutStation, double& OutTime) const
{
	const AFGTrain* Train = State.Train;
	// A player in the driver's seat holds the train only while driving it by hand (autopilot off).
	if (!State.bManaged || State.bKeepVanilla || !IsValid(Train) || (Train->IsPlayerDriven() && !Train->IsSelfDrivingEnabled()) || Train->IsDerailed())
	{
		return false;
	}
	const double Time = Now();
	if (!State.bHasJob)
	{
		OutStation = Train->IsDocked() && State.DockedStation.IsValid() ? State.DockedStation.Get() : State.LastStation.Get();
		OutStation = OutStation ? OutStation : FindNearestStation(Train);
		// No job: nothing of its own to move here, so it is free now, docked or not.
		OutTime = Time;
		return OutStation != nullptr;
	}
	const FSTJob& Job = State.Job;
	if (Job.bRepeat)
	{
		// Running its last job again: free to take a new trip from where it is.
		OutStation = Train->IsDocked() && State.DockedStation.IsValid() ? State.DockedStation.Get() : State.LastStation.Get();
		OutStation = OutStation ? OutStation : FindNearestStation(Train);
		OutTime = Train->IsDocked() ? FMath::Max(Time, State.DockedAt + EstimateStaySeconds(OutStation)) : Time;
		return OutStation != nullptr;
	}
	if (Job.bPark && Job.Legs.Num() == 1 && !Job.Legs[0].HasWork())
	{
		// Heading for a depot or out of the way: free to take a job from where it last was.
		OutStation = State.LastStation.Get() ? State.LastStation.Get() : Job.Legs[0].Station.Get();
		OutTime = Time;
		return OutStation != nullptr;
	}
	// Docked at its last stop: free there only if that stop moves nothing. What a transfer moved is known once the
	// train leaves (platforms keep loading and unloading while it is docked), and its next trip is planned from that.
	if (Job.Leg == Job.Legs.Num() - 1 && Train->IsDocked() && State.DockedStation.Get() == Job.Legs.Last().Station && !Job.Legs.Last().HasWork())
	{
		OutStation = Job.Legs.Last().Station;
		OutTime = FMath::Max(Time, State.DockedAt + EstimateStaySeconds(OutStation));
		return true;
	}
	return false;
}

void ASTSubsystem::Dispatch()
{
	if (!HasAuthority() || bInDispatch)
	{
		return;
	}
	{
		TGuardValue<bool> Guard(bInDispatch, true);
		DispatchRound();
	}
	TArray<TWeakObjectPtr<AFGTrain>> Settle = MoveTemp(mToSettle);
	mToSettle.Reset();
	for (const TWeakObjectPtr<AFGTrain>& Train : Settle)
	{
		FSTTrainState* State = FindTrainState(Train.Get());
		if (State && State->bManaged && !State->bHasJob)
		{
			SettleIdle(*State, TEXT("no trip after a re-plan"), false);
		}
	}
}

void ASTSubsystem::BuildBookings(const TArray<FSTNode>& Nodes, double Time, TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>>& Out)
{
	// Station bookings of the trips already running, from arrival to the end of the stay. A train docked for its current
	// stop holds the station from when it docked; one running late from now.
	for (int32 TrainIndex = 0; TrainIndex < mTrains.Num(); ++TrainIndex)
	{
		const FSTTrainState& State = mTrains[TrainIndex];
		// A repeat books nothing: new trips go ahead of it.
		if (!State.bManaged || !State.bHasJob || State.Job.bRepeat)
		{
			continue;
		}
		for (int32 Leg = State.Job.Leg; Leg < State.Job.Legs.Num(); ++Leg)
		{
			const FSTJobLeg& Each = State.Job.Legs[Leg];
			const double Stay = EstimateStaySeconds(Each.Station);
			double From = Each.PlannedArrival;
			if (Leg == State.Job.Leg && IsValid(State.Train) && State.Train->IsDocked() && State.DockedStation.Get() == Each.Station)
			{
				From = State.DockedAt;
			}
			else if (Leg == State.Job.Leg)
			{
				From = FMath::Max(From, Time);
				AFGBuildableRailroadStation* Goal = nullptr;
				const float Eta = IsValid(State.Train) ? STAt::GetPlannedArrival(GetWorld(), State.Train, Goal) : -1.f;
				if (Eta >= 0.f && Goal && Goal == Each.Station->GetStation())
				{
					From = Time + Eta;
				}
			}
			Out.FindOrAdd(Each.Station.Get()).Add({TrainIndex, From, FMath::Max(Each.PlannedArrival, From) + Stay});
		}
	}
	// A parked train holds its station until it is moved out, which takes about ParkedLeaveSeconds once a trip needs it.
	for (int32 TrainIndex = 0; TrainIndex < mTrains.Num(); ++TrainIndex)
	{
		const FSTTrainState& State = mTrains[TrainIndex];
		if (State.bManaged && !State.bHasJob && State.Phase == ESTTrainPhase::Parked && State.LastStation && IsValid(State.Train) && !State.Train->IsSelfDrivingEnabled())
		{
			// In a dead end it holds the station until it leaves: a train arriving there could neither get in nor let it out.
			const double Hold = IsDeadEnd(State.LastStation.Get()) ? DeadEndHoldSeconds : ParkedLeaveSeconds;
			Out.FindOrAdd(State.LastStation.Get()).Add({TrainIndex, Time, Time + Hold});
		}
	}
	if (STAt::IsAvailable(GetWorld()))
	{
		TSet<const AFGTrainStationIdentifier*> Stations;
		for (const FSTNode& Node : Nodes)
		{
			Stations.Add(Node.Station.Get());
		}
		for (const AFGTrainStationIdentifier* Station : Stations)
		{
			TPair<float, double>& Cached = mAtQueues.FindOrAdd(const_cast<AFGTrainStationIdentifier*>(Station), TPair<float, double>(-1.f, -1000.0));
			if (Time - Cached.Value >= AtQueueSeconds && Station && Station->GetStation())
			{
				TArray<AFGTrain*> Queue;
				Cached = TPair<float, double>(STAt::GetStationQueue(GetWorld(), Station->GetStation(), Queue), Time);
			}
			const double Left = Cached.Key - (Time - Cached.Value);
			if (Cached.Key > 0.f && Left > 0.0)
			{
				Out.FindOrAdd(Station).Add({INDEX_NONE, Time, Time + Left});
			}
		}
	}
}

void ASTSubsystem::DispatchRound()
{
	TArray<FSTNode> Nodes;
	BuildNodes(Nodes);
	const double Time = Now();

	mAtTravelCallsLeft = AtTravelCallsPerRound;
	mHeldFor.Reset();
	TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;
	BuildBookings(Nodes, Time, Bookings);
	FSTTripPlanner Planner(*this, Nodes, Bookings);

	// A platform with settings that was dismantled: its trips are re-planned.
	const int32 Before = mPlatforms.Num();
	mPlatforms.RemoveAll([](const FSTPlatformConfig& Config) { return !IsValid(Config.Platform); });
	if (mPlatforms.Num() != Before)
	{
		UE_LOG(LogSmartTrains, Log, TEXT("%d platforms with settings were dismantled; trips under way are re-planned"), Before - mPlatforms.Num());
		bReplanAll = true;
	}
	// Trips under way that asked for a re-plan (after a stop, or after platform settings changed).
	if (!mSettings.bObserveOnly)
	{
		for (FSTTrainState& State : mTrains)
		{
			// A stop ahead whose station was dismantled.
			for (int32 Leg = State.Job.Leg; State.bHasJob && Leg < State.Job.Legs.Num(); ++Leg)
			{
				if (!IsValid(State.Job.Legs[Leg].Station))
				{
					State.bReplan = true;
				}
			}
		}
		for (FSTTrainState& State : mTrains)
		{
			const bool bWanted = State.bReplan || bReplanAll;
			if (!bWanted || !State.bManaged || !State.bHasJob || State.Job.bPark || State.Job.bRequest || !IsValid(State.Train) || State.Train->IsDocked())
			{
				continue;
			}
			State.bReplan = false;
			ReplanTrip(State, Planner);
		}
	}
	bReplanAll = false;
	if (Nodes.Num() == 0)
	{
		return;
	}
	// Every free train runs its service's next cycle.
	RunServices(Planner, Nodes);
	if (!mSettings.bObserveOnly)
	{
		ClearNeededBays();
	}
}

bool ASTSubsystem::IsFreeNow(const FSTTrainState& State)
{
	return !State.bHasJob || State.Job.bRepeat || (State.Job.bPark && State.Job.Legs.Num() == 1 && !State.Job.Legs[0].HasWork());
}

bool ASTSubsystem::JobDelivers(const FSTJob& Job, const FSTRequestClaim& Claim, int32 FromLeg)
{
	for (int32 Leg = FMath::Max(0, FromLeg); Leg < Job.Legs.Num(); ++Leg)
	{
		for (const FSTCarWork& Work : Job.Legs[Leg].Work)
		{
			if (!Work.bLoad && Work.Item == Claim.Item && Work.Platform == Claim.Platform)
			{
				return true;
			}
		}
	}
	return false;
}

bool ASTSubsystem::GetRequestNeed(const FSTNode& Node, const AFGBuildableTrainPlatformCargo* Platform, double& OutBelowAt, FString& OutWhy) const
{
	const FSTPlatformConfig* Config = Platform ? FindPlatformConfig(Platform) : nullptr;
	const FSTItemRule* Rule = Config && Node.Item ? Config->FindRule(Node.Item) : nullptr;
	const UFGInventoryComponent* Inventory = Platform ? Platform->GetInventory() : nullptr;
	if (!Config || Config->Role != ESTRole::Demand || Config->Request == ESTRequest::Off || !Rule || !Rule->Item || !Inventory)
	{
		return false;
	}
	const int32 Capacity = STLayout::GetCapacity(Inventory, Node.Item);
	if (Capacity <= 0)
	{
		return false;
	}
	// This platform's stock and its deliveries on the way: those promised to it, and its share of those promised to the
	// station without a platform.
	int32 Assigned = 0;
	for (const TPair<const AFGBuildableTrainPlatformCargo*, int32>& Each : Node.PlatformReserved)
	{
		Assigned += Each.Value;
	}
	const double Shared = FMath::Max(0, Node.Reserved - Assigned) / static_cast<double>(FMath::Max(1, Node.Platforms.Num()));
	const double Coming = Inventory->GetNumItems(Node.Item) + Node.PlatformReserved.FindRef(Platform) + Shared;
	const bool bOn = Config->Request == ESTRequest::On;
	const double Threshold = Capacity * (bOn ? RequestFullShare : Rule->TargetPercent / 100.0);
	const double Rate = Rule->ManualRatePerMinute > 0.f ? Rule->ManualRatePerMinute / 60.0 : GetMeasuredRate(Platform, Node.Item);
	double Seconds = 0.0;
	if (Coming >= Threshold)
	{
		if (Rate <= 0.0)
		{
			return false;
		}
		Seconds = (Coming - Threshold) / Rate;
	}
	if (Seconds > RequestTripSeconds)
	{
		return false;
	}
	OutBelowAt = Now() + Seconds;
	const TCHAR* Level = bOn ? TEXT("90%") : TEXT("its target");
	OutWhy = Seconds <= 0.0 ? FString::Printf(TEXT("below %s with what is on the way"), Level)
		: FString::Printf(TEXT("below %s in %.0f min at its use, with what is on the way"), Level, Seconds / 60.0);
	return true;
}

double ASTSubsystem::GetFittingDelivery(const FSTNode& Node, const AFGBuildableTrainPlatformCargo* Platform, const TArray<FSTNode>& Nodes, FString* OutTrain) const
{
	double Best = TNumericLimits<double>::Max();
	const AFGTrainStationIdentifier* Drop = Node.Station.Get();
	const AFGBuildableRailroadStation* Building = Drop ? Drop->GetStation() : nullptr;
	const int32 PlatformIndex = Building ? STLayout::GetPlatforms(Building).IndexOfByKey(Platform) : INDEX_NONE;
	if (PlatformIndex == INDEX_NONE)
	{
		return Best;
	}
	for (const FSTTrainState& State : mTrains)
	{
		if (!State.bManaged || State.bKeepVanilla || !IsValid(State.Train) || !CanServe(State, Drop)
			|| State.Requests.ContainsByPredicate([&](const FSTRequestClaim& Claim) { return Claim.Platform == Platform && Claim.Item == Node.Item; }))
		{
			continue;
		}
		AFGTrainStationIdentifier* Anchor = nullptr;
		double FreeAt = 0.0;
		if (!GetFreeAt(State, Anchor, FreeAt))
		{
			continue;
		}
		const TArray<TArray<AFGRailroadVehicle*>> DropMaps = GetDockMaps(State.Train, Drop);
		if (DropMaps.Num() == 0)
		{
			continue;
		}
		const bool bPushPull = IsPushPull(State.Train);
		for (const FSTNode& Supplier : Nodes)
		{
			const AFGTrainStationIdentifier* Pick = Supplier.Station.Get();
			if (Supplier.Role != ESTRole::Supply || Supplier.Item != Node.Item || !Pick || Pick == Drop || !CanServe(State, Pick))
			{
				continue;
			}
			const FString Key = FString::Printf(TEXT("%p|%p|%p|%p"), State.Train.Get(), Pick, Drop, Node.Item.Get());
			const FSTFit* Cached = mFitCache.Find(Key);
			const FSTFit Fit = Cached ? *Cached : mFitCache.Add(Key, ComputeFit(State.Train, Pick, Drop, Node.Item, false));
			// A car that fits, empty or holding the item, and stops beside this platform whichever way the train docks.
			const bool bBeside = Fit.Problem.IsEmpty() && Fit.Cars.ContainsByPredicate([&](const AFGFreightWagon* Car)
			{
				for (const TArray<AFGRailroadVehicle*>& Map : DropMaps)
				{
					if (!Map.IsValidIndex(PlatformIndex) || Map[PlatformIndex] != Car)
					{
						return false;
					}
				}
				return true;
			});
			if (!bBeside)
			{
				continue;
			}
			const float ToPick = Anchor == Pick ? 0.f : EstimateTravelSeconds(Anchor, Pick, bPushPull);
			const float ToDrop = EstimateTravelSeconds(Pick, Drop, bPushPull);
			if (ToPick < 0.f || ToDrop < 0.f)
			{
				continue;
			}
			const double Arrive = FreeAt + ToPick + EstimateStaySeconds(Pick) + ToDrop;
			if (Arrive < Best)
			{
				Best = Arrive;
				if (OutTrain)
				{
					*OutTrain = STLayout::Name(State.Train);
				}
			}
		}
	}
	return Best;
}

bool ASTSubsystem::RequestCarsHoldOther(const FSTTrainState& State)
{
	for (const FSTRequestClaim& Claim : State.Requests)
	{
		for (const AFGRailroadVehicle* Vehicle : Claim.Cars)
		{
			const AFGFreightWagon* Car = Cast<AFGFreightWagon>(Vehicle);
			const UFGInventoryComponent* Inventory = Car ? Car->GetFreightInventory() : nullptr;
			if (!Inventory)
			{
				continue;
			}
			TArray<FInventoryStack> Stacks;
			Inventory->GetInventoryStacks(Stacks, true);
			for (const FInventoryStack& Stack : Stacks)
			{
				if (Stack.HasItems() && Stack.Item.GetItemClass() != Claim.Item)
				{
					return true;
				}
			}
		}
	}
	return false;
}

void ASTSubsystem::NoteNoTrip(const FSTTrainState& State, const FString& Reason)
{
	FString& Last = mIdleReasons.FindOrAdd(State.Train.Get());
	if (Last != Reason)
	{
		Last = Reason;
		UE_LOG(LogSmartTrains, Log, TEXT("IDLE %s: no trip: %s"), *STLayout::Name(State.Train), *Reason);
	}
}

void ASTSubsystem::CommitTrip(FSTTrainState& State, FSTTripPlanner& Planner, const TArray<FSTPlanStop>& Stops, const FSTPlanResult& Result, bool bReserve, int32 ServiceId)
{
	const double Time = Now();
	mIdleReasons.Remove(State.Train.Get());
	FSTJob Job;
	Job.Id = mNextJobId++;
	Job.StartedAt = Time;
	Job.LegStartedAt = Time;
	Planner.MakeLegs(Stops, Result, Job.Legs);
	TArray<FString> Items;
	for (const FSTJobLeg& Leg : Job.Legs)
	{
		for (const FSTCarWork& Work : Leg.Work)
		{
			Items.AddUnique(STNames::Item(Work.Item));
		}
	}
	TArray<FString> Stations;
	for (const FSTPlanStop& Stop : Stops)
	{
		Stations.Add(STLayout::Name(Stop.Station));
	}
	Job.ServiceId = ServiceId;
	Job.Summary = FString::Printf(TEXT("%s%s: %s"), ServiceId > 0 ? *FString::Printf(TEXT("S%d "), ServiceId) : TEXT(""), *FString::Join(Items, TEXT(", ")),
		*FString::Join(Stations, TEXT(" -> ")));
	const FString Line = FString::Printf(TEXT("%s: %s [%s], %d moved, value %.2f, score %.4f"), *STLayout::Name(State.Train), *Job.Summary,
		*Planner.Describe(Stops, Result), Result.Moved, Result.Value, Result.Score);
	if (bReserve)
	{
		Planner.Reserve(Stops, Result);
	}
	CommitJob(State, MoveTemp(Job), Line);
	UpdateUnwantedCargo(State);
}

double ASTSubsystem::GetNetworkCost(const TArray<FSTNode>& Nodes) const
{
	const double Time = Now();
	double Cost = 0.0;
	for (const FSTNode& Node : Nodes)
	{
		if (!Node.bAnyItem && Node.Item)
		{
			Cost += STForecast::Cost(Node, Node.Planned, Time, STLayout::GetCarCapacity(nullptr, Node.Item));
		}
	}
	return Cost;
}

void ASTSubsystem::HorizonRound(FSTTripPlanner& Planner, TArray<FSTNode>& Nodes)
{
	const double Time = Now();
	// A free train's plan, its net value (forecast gain less train time) and where it starts.
	struct FPlan
	{
		int32 TrainIndex = INDEX_NONE;
		AFGTrainStationIdentifier* Anchor = nullptr;
		double FreeAt = 0.0;
		bool bDocked = false;
		TArray<FSTPlanStop> Stops;
		FSTPlanResult Result;
		double Net = 0.0;
		// Position in the train's duty of the roster route this plan runs, or INDEX_NONE.
		int32 RosterStep = INDEX_NONE;
		// It holds cargo, so emptying a car to serve a short platform is worth trying.
		bool bCarrying = false;
	};
	TArray<FPlan> Plans;
	TArray<TSharedPtr<FJsonValue>> SnapshotTrains;
	for (int32 TrainIndex = 0; TrainIndex < mTrains.Num() && !Planner.IsOverBudget(); ++TrainIndex)
	{
		FSTTrainState& State = mTrains[TrainIndex];
		State.Requests.Reset();
		FPlan Plan;
		Plan.TrainIndex = TrainIndex;
		if (!GetFreeAt(State, Plan.Anchor, Plan.FreeAt))
		{
			continue;
		}
		const bool bJustLeftAnchor = State.LastStation.Get() == Plan.Anchor && Time - State.LastDepartureAt < JustLeftAnchorSeconds
			&& !(State.Phase == ESTTrainPhase::Parked && !State.Train->IsSelfDrivingEnabled());
		Plan.bDocked = (State.Train->IsDocked() && State.DockedStation.Get() == Plan.Anchor) || bJustLeftAnchor;
		Plan.bCarrying = GetCargo(State.Train).Num() > 0;
		if (Planner.BeginTrain(TrainIndex, Plan.Anchor, Plan.FreeAt, Plan.bDocked))
		{
			if (bSnapshotNext)
			{
				SnapshotTrains.Add(MakeShared<FJsonValueObject>(Planner.DescribeForReplay()));
			}
			Plans.Add(MoveTemp(Plan));
		}
	}
	if (bSnapshotNext)
	{
		bSnapshotNext = false;
		WriteSnapshot(Nodes, Planner.GetBookings(), SnapshotTrains);
	}
	if (Plans.Num() == 0)
	{
		return;
	}
	auto Resume = [&](const FPlan& Plan)
	{
		return Planner.ResumeTrain(Plan.TrainIndex, Plan.Anchor, Plan.FreeAt, Plan.bDocked) || Planner.BeginTrain(Plan.TrainIndex, Plan.Anchor, Plan.FreeAt, Plan.bDocked);
	};
	auto NetOf = [&](const FPlan& Plan, const TArray<FSTPlanStop>& Stops, const FSTPlanResult& Result)
	{
		return Result.bValid && Stops.Num() > 0 && Result.Value > 0.0 ? Result.Value - TrainSecondCost * FMath::Max(0.0, Result.EndTime - Plan.FreeAt) : 0.0;
	};
	// The demand platforms the network is shortest of (forecast cost), for trips that empty a car to serve them.
	TArray<FSTRequestTarget> Short;
	{
		TArray<TPair<double, FSTRequestTarget>> Ranked;
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			const FSTNode& Node = Nodes[Index];
			if (Node.Role != ESTRole::Demand || Node.bAnyItem || !Node.Item)
			{
				continue;
			}
			const double CarCapacity = STLayout::GetCarCapacity(nullptr, Node.Item);
			if (STForecast::Deadline(Node, Node.Planned, Time, CarCapacity) > Time + HorizonEmptyingSeconds)
			{
				continue;
			}
			const double Cost = STForecast::Cost(Node, Node.Planned, Time, CarCapacity);
			for (const TWeakObjectPtr<AFGBuildableTrainPlatformCargo>& Platform : Node.Platforms)
			{
				if (Cost > 0.0 && Platform.IsValid())
				{
					Ranked.Add({Cost, FSTRequestTarget{Index, Platform.Get()}});
				}
			}
		}
		Ranked.Sort([](const TPair<double, FSTRequestTarget>& A, const TPair<double, FSTRequestTarget>& B) { return A.Key > B.Key; });
		for (int32 Index = 0; Index < Ranked.Num() && Index < HorizonShortTargets; ++Index)
		{
			Short.Add(Ranked[Index].Value);
		}
	}
	// The plan for one train given the others' reservations: regret insertion of moves, or a trip that empties its car
	// to serve one of the shortest platforms, whichever nets more.
	auto Build = [&](FPlan& Plan)
	{
		Plan.Stops.Reset();
		Plan.Result = FSTPlanResult();
		Plan.Net = 0.0;
		Plan.RosterStep = INDEX_NONE;
		if (!Resume(Plan))
		{
			return;
		}
		// A rostered train runs its routes in turn, the first that delivers at least a quarter of a car. A shuttle takes
		// the route whose trip delivers most, if that is at least ShuttleLoadShare of what it carries on that route.
		if (const FSTRosterDuty* Duty = mRoster.Find(mTrains[Plan.TrainIndex].Train.Get()))
		{
			const TWeakObjectPtr<AFGTrain> Train(mTrains[Plan.TrainIndex].Train.Get());
			int32 BestUnloaded = 0;
			for (int32 Step = 0; Step < Duty->Routes.Num(); ++Step)
			{
				const int32 RouteIndex = Duty->Routes[(Duty->Next + Step) % Duty->Routes.Num()];
				if (!mRosterRoutes.IsValidIndex(RouteIndex))
				{
					continue;
				}
				const FSTRosterRoute& Route = mRosterRoutes[RouteIndex];
				UClass* Item = Route.Item.Get();
				if (!Item || !Route.From.IsValid() || !Route.To.IsValid())
				{
					continue;
				}
				TArray<FSTPlanStop> Trip;
				FSTPlanStop& Pick = Trip.AddDefaulted_GetRef();
				Pick.Station = Route.From.Get();
				Pick.LoadItems.Add(Item);
				FSTPlanStop& Drop = Trip.AddDefaulted_GetRef();
				Drop.Station = Route.To.Get();
				Drop.UnloadItems.Add(Item);
				const FSTPlanResult TripResult = Planner.Simulate(Trip);
				const int32 Unloaded = TripResult.bValid ? FSTTripPlanner::CountUnloaded(Trip, TripResult, Item, Route.To.Get()) : 0;
				double Least = RosterMinCarShare * STLayout::GetCarCapacity(nullptr, Item);
				const int32 Fit = Route.Fitting.IndexOfByKey(Train);
				if (Duty->bShuttle && Route.Capacity.IsValidIndex(Fit))
				{
					Least = FMath::Max(Least, ShuttleLoadShare * Route.Capacity[Fit]);
				}
				if (!TripResult.bValid || Unloaded < Least || Unloaded <= BestUnloaded)
				{
					continue;
				}
				BestUnloaded = Unloaded;
				Plan.Stops = MoveTemp(Trip);
				Plan.Result = TripResult;
				Plan.RosterStep = Step;
				if (!Duty->bShuttle)
				{
					break;
				}
			}
			const UClass* ServiceItem = Duty->bShuttle && Duty->Routes.Num() > 0 && mRosterRoutes.IsValidIndex(Duty->Routes[0]) ? mRosterRoutes[Duty->Routes[0]].Item.Get() : nullptr;
			const FServiceInfo* Service = ServiceItem ? mServices.Find(const_cast<UClass*>(ServiceItem)) : nullptr;
			if (Plan.RosterStep != INDEX_NONE && Service && Service->Trains > 1 && Service->Headway > 0.0 && Plan.Result.Arrive.Num() > 0
				&& Planner.IsBookedNear(Plan.Stops[0].Station, Plan.Result.Arrive[0], Service->Headway * 0.5, [&](int32 Other)
				{
					const FSTRosterDuty* OtherDuty = mTrains.IsValidIndex(Other) ? GetShuttleDuty(mTrains[Other].Train) : nullptr;
					return OtherDuty && OtherDuty->Routes.Num() > 0 && mRosterRoutes.IsValidIndex(OtherDuty->Routes[0]) && mRosterRoutes[OtherDuty->Routes[0]].Item.Get() == ServiceItem;
				}))
			{
				// Another train of the service picks up there within half its spacing: this one keeps the rhythm and waits.
				Plan.Stops.Reset();
				Plan.Result = FSTPlanResult();
				Plan.RosterStep = INDEX_NONE;
				return;
			}
			if (Plan.RosterStep != INDEX_NONE)
			{
				Plan.Net = FMath::Max(NetOf(Plan, Plan.Stops, Plan.Result), 0.0);
				Planner.Reserve(Plan.Stops, Plan.Result);
				return;
			}
		}
		// A shuttle whose route trip doesn't work now only delivers what it carries, then waits for its route.
		const bool bShuttle = GetShuttleDuty(mTrains[Plan.TrainIndex].Train) != nullptr;
		TArray<FSTPlanStop> Stops;
		Planner.SetDeliveriesOnly(bShuttle);
		Planner.Improve(Stops, 0);
		Planner.SetDeliveriesOnly(false);
		FSTTripPlanner::Trim(Stops, Planner.Simulate(Stops), 0);
		FSTPlanResult Result = Planner.Simulate(Stops);
		double Net = NetOf(Plan, Stops, Result);
		for (const FSTRequestTarget& Target : Plan.bCarrying && !bShuttle ? Short : TArray<FSTRequestTarget>())
		{
			if (Planner.IsOverBudget())
			{
				break;
			}
			TArray<FSTPlanStop> Emptying;
			FSTPlanResult EmptyingResult;
			bool bDelivers = false;
			TArray<int32> Cars;
			FString Why;
			if (!Planner.PlanRequest(Target, Emptying, EmptyingResult, bDelivers, Cars, Why) || !bDelivers)
			{
				continue;
			}
			// Other cars may load on the way if the car serving the platform stays clean.
			TArray<FSTPlanStop> More = Emptying;
			const FSTPlanResult MoreResult = Planner.Improve(More, 0);
			UClass* Item = Nodes[Target.Node].Item.Get();
			if (MoreResult.bValid && Planner.KeepsRequestCars(More, MoreResult, Cars, Item)
				&& FSTTripPlanner::CountUnloadedInto(MoreResult, Item, Target.Platform) >= FSTTripPlanner::CountUnloadedInto(EmptyingResult, Item, Target.Platform))
			{
				Emptying = MoveTemp(More);
				EmptyingResult = MoreResult;
			}
			const double EmptyingNet = NetOf(Plan, Emptying, EmptyingResult);
			if (EmptyingNet > Net)
			{
				Net = EmptyingNet;
				Stops = MoveTemp(Emptying);
				Result = MoveTemp(EmptyingResult);
			}
		}
		if (Net <= 0.0)
		{
			return;
		}
		Plan.Stops = MoveTemp(Stops);
		Plan.Result = MoveTemp(Result);
		Plan.Net = Net;
		Planner.Reserve(Plan.Stops, Plan.Result);
	};
	auto Unbuild = [&](FPlan& Plan)
	{
		if (Plan.Stops.Num() > 0 && Resume(Plan))
		{
			Planner.Unreserve(Plan.Stops, Plan.Result);
		}
		Plan.Stops.Reset();
		Plan.Result = FSTPlanResult();
		Plan.Net = 0.0;
	};
	auto TotalCost = [&]()
	{
		double Cost = GetNetworkCost(Nodes);
		for (const FPlan& Plan : Plans)
		{
			Cost += Plan.Stops.Num() > 0 ? TrainSecondCost * FMath::Max(0.0, Plan.Result.EndTime - Plan.FreeAt) : 0.0;
		}
		return Cost;
	};

	// First plans, the trains free soonest first.
	Plans.Sort([](const FPlan& A, const FPlan& B) { return A.FreeAt != B.FreeAt ? A.FreeAt < B.FreeAt : A.TrainIndex < B.TrainIndex; });
	for (FPlan& Plan : Plans)
	{
		if (Planner.IsOverBudget())
		{
			break;
		}
		Build(Plan);
	}
	const double Start = TotalCost();
	double Best = Start;
	// Large neighbourhood search: plan one or two trains again in another order, keep it if the network costs less.
	// Seeded from the trains being planned, so the same situation gives the same plans. Roster trips stay as they are.
	uint32 Seed = GetTypeHash(Nodes.Num());
	TArray<int32> Searchable;
	for (int32 Index = 0; Index < Plans.Num(); ++Index)
	{
		Seed = HashCombine(Seed, GetTypeHash(Plans[Index].TrainIndex));
		if (Plans[Index].RosterStep == INDEX_NONE)
		{
			Searchable.Add(Index);
		}
	}
	FRandomStream Random(static_cast<int32>(Seed & 0x7fffffff));
	int32 Tries = 0;
	int32 Kept = 0;
	int32 Stale = 0;
	while (Searchable.Num() > 1 && !Planner.IsOverBudget() && Tries < HorizonMaxTries && Stale < HorizonStaleTries)
	{
		++Tries;
		++Stale;
		TArray<int32> Picked;
		// One train at random, and sometimes the one whose plan nets least, so a poor plan gets another chance.
		Picked.Add(Searchable[Random.RandRange(0, Searchable.Num() - 1)]);
		if (Random.FRand() < 0.5)
		{
			int32 Worst = INDEX_NONE;
			for (const int32 Index : Searchable)
			{
				if (!Picked.Contains(Index) && (Worst == INDEX_NONE || Plans[Index].Net < Plans[Worst].Net))
				{
					Worst = Index;
				}
			}
			if (Worst != INDEX_NONE)
			{
				Picked.Add(Worst);
			}
		}
		TArray<FPlan> Saved;
		for (const int32 Index : Picked)
		{
			Saved.Add(Plans[Index]);
			Unbuild(Plans[Index]);
		}
		// Rebuilt in reverse order, so each gets first pick this time.
		for (int32 Pick = Picked.Num() - 1; Pick >= 0; --Pick)
		{
			Build(Plans[Picked[Pick]]);
		}
		const double Cost = TotalCost();
		if (Cost < Best - 1e-6)
		{
			Best = Cost;
			++Kept;
			Stale = 0;
			continue;
		}
		for (const int32 Index : Picked)
		{
			Unbuild(Plans[Index]);
		}
		for (int32 Pick = 0; Pick < Picked.Num(); ++Pick)
		{
			FPlan& Plan = Plans[Picked[Pick]];
			Plan = Saved[Pick];
			if (Plan.Stops.Num() > 0 && Resume(Plan))
			{
				Planner.Reserve(Plan.Stops, Plan.Result);
			}
		}
	}

	// Commit: reservations are already in place.
	int32 Trips = 0;
	int32 Rostered = 0;
	for (const FPlan& Plan : Plans)
	{
		FSTTrainState& State = mTrains[Plan.TrainIndex];
		if (Plan.Stops.Num() == 0)
		{
			if (!State.bHasJob || State.Job.bRepeat)
			{
				const bool bCarrying = GetCargo(State.Train).Num() > 0;
				if (bCarrying && State.UnwantedCargoSince <= 0.0)
				{
					State.UnwantedCargoSince = Time;
					UE_LOG(LogSmartTrains, Log, TEXT("%s carries cargo no trip wants now"), *STLayout::Name(State.Train));
				}
				Resume(Plan);
				const FString Problems = Planner.DescribeProblems();
				NoteNoTrip(State, !Problems.IsEmpty() ? FString::Printf(TEXT("nothing worth running now; trips tried were rejected: %s"), *Problems)
					: FString(TEXT("nothing worth running now (nothing short within the next hour that it can serve)")));
			}
			continue;
		}
		if (!Resume(Plan))
		{
			continue;
		}
		++Trips;
		CommitTrip(State, Planner, Plan.Stops, Plan.Result, false);
		if (FSTRosterDuty* Duty = Plan.RosterStep != INDEX_NONE ? mRoster.Find(State.Train.Get()) : nullptr)
		{
			Duty->Next = (Duty->Next + Plan.RosterStep + 1) % FMath::Max(1, Duty->Routes.Num());
			++Rostered;
		}
	}
	UE_LOG(LogSmartTrains, Verbose, TEXT("HORIZON: %d trains planned, %d trips (%d from the roster), network cost %.1f -> %.1f car-load minutes, %d searches (%d kept), %.1f ms"),
		Plans.Num(), Trips, Rostered, Start, Best, Tries, Kept, Planner.GetElapsedMs());
}

void ASTSubsystem::WriteSnapshot(const TArray<FSTNode>& Nodes, const TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>>& Bookings,
	const TArray<TSharedPtr<FJsonValue>>& Trains) const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("version"), 1);
	Root->SetNumberField(TEXT("now"), Now());
	TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
	Settings->SetNumberField(TEXT("queueTrains"), mSettings.QueueTrains);
	Settings->SetBoolField(TEXT("leaveAtOnce"), mSettings.bLeaveAtOnce);
	Settings->SetNumberField(TEXT("maxStops"), mSettings.MaxStops);
	Settings->SetNumberField(TEXT("maxTripMinutes"), mSettings.MaxTripMinutes);
	Settings->SetNumberField(TEXT("arrivalGapSeconds"), mSettings.ArrivalGapSeconds);
	Root->SetObjectField(TEXT("settings"), Settings);
	TSet<UClass*> Items;
	TSet<const AFGTrainStationIdentifier*> Stations;
	TArray<TSharedPtr<FJsonValue>> NodeValues;
	for (const FSTNode& Node : Nodes)
	{
		TSharedRef<FJsonObject> Each = MakeShared<FJsonObject>();
		Each->SetStringField(TEXT("station"), GetNameSafe(Node.Station.Get()));
		Each->SetStringField(TEXT("item"), GetPathNameSafe(Node.Item.Get()));
		Each->SetNumberField(TEXT("role"), static_cast<int32>(Node.Role));
		Each->SetBoolField(TEXT("anyItem"), Node.bAnyItem);
		Each->SetNumberField(TEXT("stock"), Node.Stock);
		Each->SetNumberField(TEXT("capacity"), Node.Capacity);
		Each->SetNumberField(TEXT("rate"), Node.Rate);
		Each->SetBoolField(TEXT("starved"), Node.bStarved);
		Each->SetNumberField(TEXT("priority"), static_cast<int32>(Node.Priority));
		Each->SetNumberField(TEXT("target"), Node.TargetPercent);
		Each->SetNumberField(TEXT("sendAt"), Node.SendAtPercent);
		Each->SetNumberField(TEXT("reserved"), Node.Reserved);
		Each->SetNumberField(TEXT("plannedFlow"), Node.PlannedFlow);
		Each->SetNumberField(TEXT("recipeRate"), Node.RecipeRate);
		Each->SetNumberField(TEXT("costScale"), Node.CostScale);
		TArray<TSharedPtr<FJsonValue>> Platforms;
		for (const TWeakObjectPtr<AFGBuildableTrainPlatformCargo>& Platform : Node.Platforms)
		{
			Platforms.Add(MakeShared<FJsonValueString>(GetNameSafe(Platform.Get())));
		}
		Each->SetArrayField(TEXT("platforms"), Platforms);
		TSharedRef<FJsonObject> PlatformReserved = MakeShared<FJsonObject>();
		for (const TPair<const AFGBuildableTrainPlatformCargo*, int32>& Pair : Node.PlatformReserved)
		{
			PlatformReserved->SetNumberField(GetNameSafe(Pair.Key), Pair.Value);
		}
		Each->SetObjectField(TEXT("platformReserved"), PlatformReserved);
		TArray<TSharedPtr<FJsonValue>> Planned;
		for (const FSTNodeEvent& Event : Node.Planned)
		{
			Planned.Add(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(Event.Time), MakeShared<FJsonValueNumber>(Event.Amount)}));
		}
		Each->SetArrayField(TEXT("planned"), Planned);
		NodeValues.Add(MakeShared<FJsonValueObject>(Each));
		if (Node.Item)
		{
			Items.Add(Node.Item.Get());
		}
		Stations.Add(Node.Station.Get());
	}
	Root->SetArrayField(TEXT("nodes"), NodeValues);
	for (const FSTTrainState& State : mTrains)
	{
		Stations.Add(State.LastStation.Get());
		for (const TPair<UClass*, int32>& Held : GetCargo(State.Train))
		{
			Items.Add(Held.Key);
		}
	}
	for (const FSTPlatformConfig& Config : mPlatforms)
	{
		for (const FSTItemRule& Rule : Config.Items)
		{
			if (Rule.Item)
			{
				Items.Add(Rule.Item.Get());
			}
		}
	}
	Stations.Remove(nullptr);
	Items.Remove(nullptr);
	TArray<TSharedPtr<FJsonValue>> ItemValues;
	for (UClass* Item : Items)
	{
		TSharedRef<FJsonObject> Each = MakeShared<FJsonObject>();
		Each->SetStringField(TEXT("path"), GetPathNameSafe(Item));
		Each->SetStringField(TEXT("name"), STNames::Item(Item));
		Each->SetBoolField(TEXT("fluid"), STNames::IsFluid(Item));
		Each->SetNumberField(TEXT("stack"), UFGItemDescriptor::GetStackSize(Item));
		ItemValues.Add(MakeShared<FJsonValueObject>(Each));
	}
	Root->SetArrayField(TEXT("items"), ItemValues);
	TArray<TSharedPtr<FJsonValue>> StationValues;
	TArray<TSharedPtr<FJsonValue>> TravelValues;
	for (const AFGTrainStationIdentifier* From : Stations)
	{
		TSharedRef<FJsonObject> Each = MakeShared<FJsonObject>();
		Each->SetStringField(TEXT("key"), GetNameSafe(From));
		Each->SetStringField(TEXT("name"), STLayout::Name(From));
		Each->SetNumberField(TEXT("stay"), EstimateStaySeconds(From));
		StationValues.Add(MakeShared<FJsonValueObject>(Each));
		for (const AFGTrainStationIdentifier* To : Stations)
		{
			const float Seconds = From != To ? EstimateTravelSeconds(From, To, false) : -1.f;
			if (Seconds >= 0.f)
			{
				TravelValues.Add(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueString>(GetNameSafe(From)),
					MakeShared<FJsonValueString>(GetNameSafe(To)), MakeShared<FJsonValueNumber>(Seconds)}));
			}
		}
	}
	Root->SetArrayField(TEXT("stations"), StationValues);
	Root->SetArrayField(TEXT("travel"), TravelValues);
	TSharedRef<FJsonObject> BookingValues = MakeShared<FJsonObject>();
	for (const TPair<const AFGTrainStationIdentifier*, TArray<FSTBooking>>& Pair : Bookings)
	{
		TArray<TSharedPtr<FJsonValue>> List;
		for (const FSTBooking& Booking : Pair.Value)
		{
			List.Add(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(Booking.Train), MakeShared<FJsonValueNumber>(Booking.From),
				MakeShared<FJsonValueNumber>(Booking.Until)}));
		}
		BookingValues->SetArrayField(GetNameSafe(Pair.Key), List);
	}
	Root->SetObjectField(TEXT("bookings"), BookingValues);
	Root->SetArrayField(TEXT("trains"), Trains);
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Root, Writer);
	const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SmartTrains"), TEXT("Snapshots"),
		FString::Printf(TEXT("snapshot-%s.json"), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"))));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	const bool bSaved = FFileHelper::SaveStringToFile(Text, *Path);
	UE_LOG(LogSmartTrains, Log, TEXT("SNAPSHOT %s: %d nodes, %d free trains, %d stations, %d items%s"), *Path, Nodes.Num(), Trains.Num(), Stations.Num(), Items.Num(),
		bSaved ? TEXT("") : TEXT(" (could not be written)"));
}

void ASTSubsystem::ReplanTrip(FSTTrainState& State, FSTTripPlanner& Planner)
{
	const int32 TrainIndex = static_cast<int32>(&State - mTrains.GetData());
	AFGTrainStationIdentifier* From = State.LastStation.Get();
	if (!From || !Planner.BeginTrain(TrainIndex, From, Now(), true))
	{
		return;
	}
	Planner.Release(State.Job.Legs, State.Job.Leg);
	TArray<FSTPlanStop> Stops;
	FString Before;
	const bool bFresh = State.bReplanFresh;
	State.bReplanFresh = false;
	// A shuttle drops the pickups of the rest of its trip but its routes', and only delivers what it carries, so it is
	// free for its routes soon.
	const FSTRosterDuty* Shuttle = GetShuttleDuty(State.Train);
	auto IsShuttlePickup = [&](const UClass* Item, const AFGTrainStationIdentifier* Station)
	{
		return Shuttle->Routes.ContainsByPredicate([&](int32 Route)
		{
			return mRosterRoutes.IsValidIndex(Route) && mRosterRoutes[Route].Item.Get() == Item && mRosterRoutes[Route].From.Get() == Station;
		});
	};
	for (int32 Leg = State.Job.Leg; !bFresh && Leg < State.Job.Legs.Num(); ++Leg)
	{
		const FSTJobLeg& Each = State.Job.Legs[Leg];
		FSTPlanStop& Stop = Stops.AddDefaulted_GetRef();
		Stop.Station = Each.Station;
		Stop.LoadItems = Each.GetItems(true);
		Stop.UnloadItems = Each.GetItems(false);
		if (Shuttle)
		{
			Stop.LoadItems.RemoveAll([&](UClass* Item) { return !IsShuttlePickup(Item, Each.Station.Get()); });
		}
	}
	// The trip limit is for planning new trips: the rest of a trip under way is checked without it, and additions may
	// make the trip as long as its rest already is.
	Planner.RaiseTripLimit(1.0e9);
	FSTPlanResult Result = Planner.Simulate(Stops);
	Planner.ResetTripLimit(Result.bValid ? Result.EndTime - Now() + 120.0 : 0.0);
	Before = Planner.Describe(Stops, Result);
	if (!Result.bValid)
	{
		UE_LOG(LogSmartTrains, Log, TEXT("REPLAN %s job %d: the rest [%s] no longer works (%s); planning it again from the cargo on board"),
			*STLayout::Name(State.Train), State.Job.Id, *Before, *Result.Problem);
		Stops.Reset();
	}
	else
	{
		FSTTripPlanner::Trim(Stops, Result, 0);
	}
	const bool bKeptWorks = Result.bValid && Stops.Num() > 0;
	const TArray<FSTPlanStop> Kept = Stops;
	const FSTPlanResult KeptResult = bKeptWorks ? Planner.Simulate(Stops) : FSTPlanResult();
	if (State.Job.ServiceId == 0)
	{
		Planner.SetDeliveriesOnly(Shuttle != nullptr);
		Result = Planner.Improve(Stops, 0);
		Planner.SetDeliveriesOnly(false);
	}
	FSTTripPlanner::Trim(Stops, Result, 0);
	Result = Planner.Simulate(Stops);
	// Stability: additions to a trip that still works must beat it by the re-plan threshold.
	if (bKeptWorks && KeptResult.bValid && Result.bValid
		&& Result.Score - KeptResult.Score < ReplanMarginValue)
	{
		Stops = Kept;
		Result = KeptResult;
	}
	if (!Result.bValid || Stops.Num() == 0)
	{
		UE_LOG(LogSmartTrains, Log, TEXT("REPLAN %s job %d: nothing left to do after %s"), *STLayout::Name(State.Train), State.Job.Id, *STLayout::Name(From));
		State.Job.Legs.SetNum(State.Job.Leg);
		FinishJob(State);
		return;
	}
	TArray<FSTJobLeg> Rest;
	Planner.MakeLegs(Stops, Result, Rest);
	const FString After = Planner.Describe(Stops, Result);
	Planner.Reserve(Stops, Result);
	State.Job.Legs.SetNum(State.Job.Leg);
	State.Job.Legs.Append(Rest);
	if (After == Before)
	{
		UE_LOG(LogSmartTrains, Verbose, TEXT("REPLAN %s job %d: unchanged [%s]"), *STLayout::Name(State.Train), State.Job.Id, *After);
	}
	else
	{
		UE_LOG(LogSmartTrains, Log, TEXT("REPLAN %s job %d: [%s] -> [%s], score %.3f"), *STLayout::Name(State.Train), State.Job.Id, *Before, *After, Result.Score);
	}
	WriteJob(State, TEXT("re-planned after a stop"));
	UpdateUnwantedCargo(State);
}

void ASTSubsystem::UpdateUnwantedCargo(FSTTrainState& State)
{
	// Unwanted: an item on board that no stop still ahead unloads.
	bool bUnwanted = false;
	for (const TPair<UClass*, int32>& Carried : GetCargo(State.Train))
	{
		bool bDelivered = false;
		for (int32 Leg = State.Job.Leg; State.bHasJob && Leg < State.Job.Legs.Num() && !bDelivered; ++Leg)
		{
			bDelivered = State.Job.Legs[Leg].GetItems(false).Contains(Carried.Key);
		}
		bUnwanted |= !bDelivered;
	}
	if (!bUnwanted)
	{
		State.UnwantedCargoSince = 0.0;
	}
	else if (State.UnwantedCargoSince <= 0.0)
	{
		State.UnwantedCargoSince = Now();
	}
}

void ASTSubsystem::CommitJob(FSTTrainState& State, FSTJob&& Job, const FString& Line)
{
	if (mSettings.bObserveOnly)
	{
		FString& Last = mObserved.FindOrAdd(State.Train.Get());
		if (Last != Job.Summary)
		{
			Last = Job.Summary;
			UE_LOG(LogSmartTrains, Log, TEXT("WOULD JOB %s"), *Line);
		}
		return;
	}
	if (State.bHasJob)
	{
		UE_LOG(LogSmartTrains, Log, TEXT("JOB %d %s done: %s (next job chosen before leaving)"), State.Job.Id, *STLayout::Name(State.Train), *State.Job.Summary);
	}
	const bool bHadJob = State.bHasJob;
	State.bChainedWhileDocked = State.Train->IsDocked();
	State.Job = MoveTemp(Job);
	State.bHasJob = true;
	// A trip that picks up and delivers is the one to run again when nothing new is worth it, whole, as planned.
	if (!State.Job.bPark && !State.Job.bRepeat)
	{
		bool bLoads = false;
		bool bUnloads = false;
		for (const FSTJobLeg& Leg : State.Job.Legs)
		{
			bLoads |= Leg.GetItems(true).Num() > 0;
			bUnloads |= Leg.GetItems(false).Num() > 0;
		}
		if (bLoads && bUnloads)
		{
			State.LastWorkJob = State.Job;
		}
	}
	UE_LOG(LogSmartTrains, Log, TEXT("JOB %d %s"), State.Job.Id, *Line);
	if (State.bChainedWhileDocked)
	{
		// The game moves a timetable on by one stop when the train departs, so the new one is written as it leaves.
		State.bWriteOnUndock = true;
		UE_LOG(LogSmartTrains, Verbose, TEXT("WRITE %s job %d: written when it leaves its stop"), *STLayout::Name(State.Train), State.Job.Id);
		// Docked with no job before (just managed, or idle there): its stay isn't one of SmartTrains' stops, so it ends
		// now and the train starts its first trip, instead of waiting out the stop's old rules.
		AFGBuildableRailroadStation* Building = !bHadJob && State.DockedStation.IsValid() ? State.DockedStation->GetStation() : nullptr;
		if (Building)
		{
			Building->ClearRuleSetTimeRequirement();
			UE_LOG(LogSmartTrains, Log, TEXT("DOCK %s at %s: it had no job; the stay ends now so it starts job %d"), *STLayout::Name(State.Train),
				*STLayout::Name(State.DockedStation.Get()), State.Job.Id);
		}
		return;
	}
	WriteJob(State, TEXT("new job"));
}

void ASTSubsystem::SettleIdle(FSTTrainState& State, const TCHAR* Why, bool bDispatchFirst)
{
	if (mSettings.bObserveOnly || !State.bManaged || !IsValid(State.Train))
	{
		return;
	}
	// Serving requests: it waits for its next request trip (ServeRequests) instead of running or loading anything else.
	if (State.Requests.Num() > 0)
	{
		return;
	}
	if (bDispatchFirst)
	{
		if (bInDispatch)
		{
			mToSettle.AddUnique(State.Train.Get());
			return;
		}
		Dispatch();
		if (State.bHasJob)
		{
			return;
		}
	}
	// A service train without a cycle now waits where it is for its next one.
	if (FindService(State.ServiceId))
	{
		SettleServiceTrain(State, Why);
		return;
	}
	// Idle: to the nearest depot no other train is at or heading for. With none free it parks where it is, and moves
	// when a trip needs that station or (with Auto Trains) a train waits for it (ClearNeededBays).
	AFGTrainStationIdentifier* Depot = FindDepot(State, State.LastStation.Get());
	if (Depot && Depot != State.LastStation)
	{
		SendToStation(State, Depot, Why);
		return;
	}
	const AFGRailroadVehicle* First = State.Train->GetFirstVehicle();
	const bool bMoving = First && First->GetVelocity().Size() > 300.0;
	const bool bOnTheLine = !State.Train->IsDocked() && !State.LastStation;
	AFGRailroadTimeTable* TimeTable = State.Train->GetTimeTable();
	AFGTrainStationIdentifier* Heading = TimeTable && TimeTable->IsValidStop(TimeTable->GetCurrentStop()) ? TimeTable->GetStop(TimeTable->GetCurrentStop()).Station.Get() : nullptr;
	// A train isn't parked on the line: one still at speed drives on to the stop it was heading for, and one that
	// isn't at a station (e.g. just managed out on the track) drives to the nearest station, and parks there.
	if (bMoving || bOnTheLine)
	{
		Heading = Heading ? Heading : FindNearestStation(State.Train);
		if (Heading)
		{
			SendToStation(State, Heading, *FString::Printf(TEXT("%s, %s"), Why, bMoving ? TEXT("still moving") : TEXT("not at a station")));
			return;
		}
	}
	Park(State, Why);
}

bool ASTSubsystem::IsStationQueued(const AFGTrainStationIdentifier* Station, const AFGTrain* Except) const
{
	if (!Station)
	{
		return false;
	}
	int32 Heading = 0;
	for (const FSTTrainState& Other : mTrains)
	{
		if (Other.Train == Except || !Other.bManaged || !Other.bHasJob || !IsValid(Other.Train) || !Other.Job.Legs.IsValidIndex(Other.Job.Leg))
		{
			continue;
		}
		const bool bThere = Other.Train->IsDocked() && Other.DockedStation.Get() == Station;
		Heading += !bThere && Other.Job.Legs[Other.Job.Leg].Station == Station ? 1 : 0;
	}
	const TPair<float, double>* AtQueue = mAtQueues.Find(const_cast<AFGTrainStationIdentifier*>(Station));
	const bool bAtQueued = AtQueue && AtQueue->Key > 0.f && Now() - AtQueue->Value < AtQueue->Key;
	return Heading >= QueuedTrains || bAtQueued;
}

void ASTSubsystem::FinishJob(FSTTrainState& State)
{
	const FSTJob Done = State.Job;
	int32 Moved = 0;
	int32 Planned = 0;
	for (const FSTJobLeg& Leg : Done.Legs)
	{
		Moved += Leg.GetMoved(false);
		Planned += Leg.GetPlanned(false);
	}
	UE_LOG(LogSmartTrains, Log, TEXT("JOB %d %s DONE: %s in %.0f s, delivered %d of %d planned"), Done.Id, *STLayout::Name(State.Train), *Done.Summary,
		Now() - Done.StartedAt, Moved, Planned);
	State.bHasJob = false;
	State.Phase = ESTTrainPhase::Idle;
	// Requests it delivered are served; the others stay, and its next trip continues them.
	State.Requests.RemoveAll([&](const FSTRequestClaim& Claim)
	{
		int32 Delivered = 0;
		for (const FSTJobLeg& Leg : Done.Legs)
		{
			for (const FSTCarWork& Work : Leg.Work)
			{
				Delivered += !Work.bLoad && Work.Item == Claim.Item && Work.Platform == Claim.Platform ? Work.MovedAmount : 0;
			}
		}
		if (Delivered > 0)
		{
			UE_LOG(LogSmartTrains, Log, TEXT("REQUEST %s into %s: delivered %s by %s"), *STNames::Item(Claim.Item), *STLayout::DescribePlatform(Claim.Platform),
				*STNames::Amount(Claim.Item, Delivered), *STLayout::Name(State.Train));
		}
		return Delivered > 0;
	});
	if (Done.bPark)
	{
		Park(State, TEXT("arrived"));
		return;
	}
	SettleIdle(State, TEXT("job done"));
}

void ASTSubsystem::SendToStation(FSTTrainState& State, AFGTrainStationIdentifier* Station, const TCHAR* Why)
{
	FSTJob Job;
	Job.Id = mNextJobId++;
	Job.StartedAt = Now();
	Job.LegStartedAt = Job.StartedAt;
	Job.bPark = true;
	FSTJobLeg& Leg = Job.Legs.AddDefaulted_GetRef();
	Leg.Station = Station;
	Leg.PlannedArrival = Now() + FMath::Max(0.f, EstimateTravelSeconds(State.LastStation.Get(), Station, IsPushPull(State.Train)));
	Job.Summary = FString::Printf(TEXT("to %s"), *STLayout::Name(Station));
	State.Job = Job;
	State.bHasJob = true;
	UE_LOG(LogSmartTrains, Log, TEXT("JOB %d %s: drive %s and park (%s)"), Job.Id, *STLayout::Name(State.Train), *Job.Summary, Why);
	WriteJob(State, Why);
}

void ASTSubsystem::Park(FSTTrainState& State, const TCHAR* Why)
{
	AFGTrain* Train = State.Train;
	if (!IsValid(Train) || mSettings.bObserveOnly)
	{
		return;
	}
	State.Phase = ESTTrainPhase::Parked;
	State.bHasJob = false;
	if (Train->IsSelfDrivingEnabled())
	{
		Train->SetSelfDrivingEnabled(false);
	}
	UE_LOG(LogSmartTrains, Log, TEXT("PARK %s at %s (%s)"), *STLayout::Name(Train), *STLayout::Name(State.LastStation.Get()), Why);
	UpdateTrainName(State);
}

bool ASTSubsystem::IsDepotTaken(const AFGTrainStationIdentifier* Station, const AFGTrain* Except) const
{
	for (const FSTTrainState& Other : mTrains)
	{
		if (Other.Train == Except || !IsValid(Other.Train))
		{
			continue;
		}
		if ((Other.Train->IsDocked() && Other.DockedStation.Get() == Station) || (Other.bManaged && Other.Phase == ESTTrainPhase::Parked && Other.LastStation.Get() == Station))
		{
			return true;
		}
		for (int32 Leg = Other.Job.Leg; Other.bManaged && Other.bHasJob && Leg < Other.Job.Legs.Num(); ++Leg)
		{
			if (Other.Job.Legs[Leg].Station.Get() == Station)
			{
				return true;
			}
		}
	}
	return false;
}

AFGTrainStationIdentifier* ASTSubsystem::FindDepot(const FSTTrainState& State, const AFGTrainStationIdentifier* From, const AFGTrainStationIdentifier* Except) const
{
	AFGTrainStationIdentifier* Best = nullptr;
	float BestTime = TNumericLimits<float>::Max();
	for (const FSTStationConfig& Config : mStations)
	{
		if (!Config.bDepot || !IsValid(Config.Station) || Config.Station == Except || !CanServe(State, Config.Station) || IsDepotTaken(Config.Station, State.Train))
		{
			continue;
		}
		const float Time = From ? EstimateTravelSeconds(From, Config.Station, IsPushPull(State.Train)) : 0.f;
		if (Time >= 0.f && Time < BestTime)
		{
			BestTime = Time;
			Best = Config.Station;
		}
	}
	return Best;
}

void ASTSubsystem::ClearNeededBays()
{
	const double Clock = Now();
	TSet<const AFGTrainStationIdentifier*> Needed;
	for (const TWeakObjectPtr<AFGTrainStationIdentifier>& Station : mHeldFor)
	{
		Needed.Add(Station.Get());
	}
	for (const FSTTrainState& State : mTrains)
	{
		if (State.bManaged && State.bHasJob)
		{
			for (int32 Leg = State.Job.Leg; Leg < State.Job.Legs.Num(); ++Leg)
			{
				Needed.Add(State.Job.Legs[Leg].Station.Get());
			}
		}
	}

	// With Auto Trains: per train it reports as holding another up, the one held up and for how long.
	TMap<const AFGTrain*, TPair<const AFGTrain*, float>> HeldUp;
	AFGRailroadSubsystem* Railroad = AFGRailroadSubsystem::Get(GetWorld());
	if (Railroad && STAt::IsAvailable(GetWorld()))
	{
		TArray<AFGTrain*> Trains;
		Railroad->GetAllTrains(Trains);
		for (AFGTrain* Train : Trains)
		{
			if (!IsValid(Train) || !Train->IsSelfDrivingEnabled() || Train->IsDocked())
			{
				continue;
			}
			TArray<AFGTrain*> Blockers;
			float Waited = 0.f;
			FString Reason;
			STAt::GetWaitingFor(GetWorld(), Train, Blockers, Waited, Reason);
			if (Waited < AtHeldUpSeconds)
			{
				continue;
			}
			for (const AFGTrain* Blocker : Blockers)
			{
				TPair<const AFGTrain*, float>& Entry = HeldUp.FindOrAdd(Blocker, TPair<const AFGTrain*, float>(Train, 0.f));
				if (Waited > Entry.Value)
				{
					Entry = TPair<const AFGTrain*, float>(Train, Waited);
				}
			}
		}
	}

	for (FSTTrainState& State : mTrains)
	{
		const AFGTrainStationIdentifier* Here = State.LastStation.Get();
		if (!State.bManaged || State.Phase != ESTTrainPhase::Parked || !Here || !IsValid(State.Train))
		{
			continue;
		}
		// Why it has to move: a trip needs its station, or Auto Trains reports a train waiting for it.
		FString Why;
		if (Needed.Contains(Here))
		{
			Why = FString::Printf(TEXT("making room at %s"), *STLayout::Name(Here));
		}
		else if (const TPair<const AFGTrain*, float>* Held = HeldUp.Find(State.Train.Get()))
		{
			Why = FString::Printf(TEXT("%s has waited %.0f s for it (Auto Trains)"), *STLayout::Name(Held->Key), Held->Value);
			UE_LOG(LogSmartTrains, Log, TEXT("BLOCKING %s, parked at %s, holds a train up: %s; moving it"), *STLayout::Name(State.Train), *STLayout::Name(Here), *Why);
		}
		if (Why.IsEmpty())
		{
			continue;
		}
		AFGTrainStationIdentifier* Target = FindDepot(State, Here, Here);
		if (!Target)
		{
			float BestTime = TNumericLimits<float>::Max();
			Target = nullptr;
			TArray<AFGTrainStationIdentifier*> Stations;
			if (Railroad)
			{
				Railroad->GetAllTrainStations(Stations);
			}
			for (AFGTrainStationIdentifier* Candidate : Stations)
			{
				if (!Candidate || Candidate == Here || Needed.Contains(Candidate) || !CanServe(State, Candidate))
				{
					continue;
				}
				const float Time = EstimateTravelSeconds(Here, Candidate, IsPushPull(State.Train));
				if (Time >= 0.f && Time < BestTime)
				{
					BestTime = Time;
					Target = Candidate;
				}
			}
		}
		if (!Target)
		{
			UE_LOG(LogSmartTrains, Warning, TEXT("%s is parked at %s, which a job needs, and there is nowhere to move it: add a depot (/st station depot on)"),
				*STLayout::Name(State.Train), *STLayout::Name(Here));
			continue;
		}
		// With Auto Trains the way out is checked first: a train coming the other way on a single track would meet it
		// head on. Then it stays, and the trips heading here are planned again so they go elsewhere.
		if (Target->GetStation())
		{
			float Seconds = -1.f;
			FString Reason;
			const STAt::ELegCheck Check = STAt::CheckLeg(GetWorld(), State.Train, Target->GetStation(), Seconds, Reason);
			if (Check == STAt::ELegCheck::Pending)
			{
				continue;
			}
			if (Check == STAt::ELegCheck::Blocked)
			{
				TArray<FString> Rerouted;
				for (FSTTrainState& Other : mTrains)
				{
					if (&Other == &State || !Other.bManaged || !Other.bHasJob || Other.Job.bPark || !IsValid(Other.Train) || Other.Train->IsDocked()
						|| Clock - Other.LastRerouted < RerouteAgainSeconds)
					{
						continue;
					}
					bool bComing = false;
					for (int32 Leg = Other.Job.Leg; Leg < Other.Job.Legs.Num() && !bComing; ++Leg)
					{
						bComing = Other.Job.Legs[Leg].Station == Here;
					}
					if (bComing)
					{
						Other.LastRerouted = Clock;
						Other.bReplan = true;
						Other.bReplanFresh = true;
						Rerouted.Add(STLayout::Name(Other.Train));
					}
				}
				UE_LOG(LogSmartTrains, Log, TEXT("REROUTE %s, parked at %s, can't get out to %s now (%s)%s%s"), *STLayout::Name(State.Train), *STLayout::Name(Here),
					*STLayout::Name(Target), *Reason, Rerouted.Num() > 0 ? TEXT("; planned again: ") : TEXT(""), *FString::Join(Rerouted, TEXT(", ")));
				if (Rerouted.Num() > 0)
				{
					bDispatchSoon = true;
				}
				continue;
			}
		}
		SendToStation(State, Target, *Why);
	}
}

void ASTSubsystem::WriteJob(FSTTrainState& State, const TCHAR* Why)
{
	AFGTrain* Train = State.Train;
	if (!IsValid(Train) || mSettings.bObserveOnly || !State.bHasJob)
	{
		return;
	}
	AFGRailroadTimeTable* TimeTable = Train->GetTimeTable();
	if (!TimeTable)
	{
		TimeTable = Train->NewTimeTable();
	}
	if (!TimeTable)
	{
		UE_LOG(LogSmartTrains, Error, TEXT("%s: no timetable could be made; job %d dropped"), *STLayout::Name(Train), State.Job.Id);
		State.bHasJob = false;
		return;
	}
	TArray<FTimeTableStop> Stops;
	TArray<int32> SkipStops;
	TArray<AFGRailroadVehicle*> SkipVehicles;
	TArray<FString> Keys;
	for (int32 Index = State.Job.Leg; Index < State.Job.Legs.Num(); ++Index)
	{
		const FSTJobLeg& Leg = State.Job.Legs[Index];
		FTimeTableStop& Stop = Stops.AddDefaulted_GetRef();
		Stop.Station = Leg.Station;
		Stop.DockingRuleSet = MakeRules(Leg, mSettings);
		for (AFGRailroadVehicle* Car : Leg.SkipCars)
		{
			SkipStops.Add(Stops.Num() - 1);
			SkipVehicles.Add(Car);
		}
		Keys.Add(StopKey(Stop, Leg.SkipCars));
	}
	// The same stops as the rest of what the train already drives to: rewriting would only make the game (and Auto
	// Trains) plan its route again, possibly while it pulls out.
	const int32 Current = TimeTable->GetCurrentStop();
	if (Train->IsSelfDrivingEnabled() && State.WrittenStopKeys.Num() - Current == Keys.Num() && Current >= 0 && Keys.Num() > 0)
	{
		bool bSame = TimeTable->GetNumStops() == State.WrittenStopKeys.Num();
		for (int32 Index = 0; bSame && Index < Keys.Num(); ++Index)
		{
			bSame = State.WrittenStopKeys[Current + Index] == Keys[Index];
		}
		if (bSame)
		{
			UE_LOG(LogSmartTrains, Verbose, TEXT("WRITE %s job %d: timetable already has these %d stops from stop %d; not rewritten (%s)"), *STLayout::Name(Train),
				State.Job.Id, Keys.Num(), Current + 1, Why);
			UpdateTrainName(State);
			return;
		}
	}
	State.WrittenStations.Reset();
	for (const FTimeTableStop& Stop : Stops)
	{
		State.WrittenStations.Add(Stop.Station);
	}
	State.WrittenStopKeys = Keys;
	bool bByFcc = false;
	if (IsFccAvailable())
	{
		bByFcc = STFcc::SetStops(GetWorld(), TimeTable, Stops, SkipStops, SkipVehicles, TEXT("SmartTrains"));
		if (!bByFcc)
		{
			UE_LOG(LogSmartTrains, Warning, TEXT("%s: Freight Car Control's ExternalSetStops failed; writing the timetable without car rules"), *STLayout::Name(Train));
		}
	}
	if (!bByFcc)
	{
		if (SkipVehicles.Num() > 0)
		{
			UE_LOG(LogSmartTrains, Warning, TEXT("%s: job %d needs %d cars to skip but Freight Car Control isn't available"), *STLayout::Name(Train), State.Job.Id, SkipVehicles.Num());
		}
		TimeTable->SetStops(Stops);
	}
	TimeTable->SetCurrentStop(0);
	if (!Train->IsSelfDrivingEnabled())
	{
		Train->SetSelfDrivingEnabled(true);
	}
	State.Phase = ESTTrainPhase::Working;
	State.LastCommandAt = Now();
	FString Listed;
	for (const FTimeTableStop& Stop : Stops)
	{
		Listed += (Listed.IsEmpty() ? TEXT("") : TEXT(", ")) + STLayout::Name(Stop.Station);
	}
	UE_LOG(LogSmartTrains, Log, TEXT("WRITE %s job %d: [%s]%s, %d skipped cars (%s)"), *STLayout::Name(Train), State.Job.Id, *Listed,
		bByFcc ? TEXT(" through Freight Car Control") : TEXT(""), SkipVehicles.Num(), Why);
	STAt::TimeTableChanged(GetWorld(), Train);
	UpdateTrainName(State);
}

bool ASTSubsystem::AssignManualJob(AFGTrain* Train, AFGTrainStationIdentifier* From, AFGTrainStationIdentifier* To, TSubclassOf<UFGItemDescriptor> Item, FString& OutResult)
{
	FSTTrainState* State = FindTrainState(Train);
	if (!State || !State->bManaged)
	{
		OutResult = TEXT("the train isn't managed (/st train manage)");
		return false;
	}
	if (mSettings.bObserveOnly)
	{
		OutResult = TEXT("observe only is on in the Mods menu");
		return false;
	}
	const FSTFit Fit = ComputeFit(Train, From, To, Item);
	if (Fit.Capacity <= 0)
	{
		OutResult = TEXT("no fit: ") + Fit.Problem;
		return false;
	}
	FSTJob Job;
	Job.Id = mNextJobId++;
	Job.StartedAt = Now();
	Job.LegStartedAt = Job.StartedAt;
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		FSTJobLeg& Leg = Job.Legs.AddDefaulted_GetRef();
		Leg.Station = Pass == 0 ? From : To;
		Leg.PlannedArrival = Now() + EstimateTravelSeconds(State->LastStation.Get(), From) + (Pass == 1 ? EstimateStaySeconds(From) + EstimateTravelSeconds(From, To) : 0.f);
		for (AFGFreightWagon* Car : Fit.Cars)
		{
			FSTCarWork& Work = Leg.Work.AddDefaulted_GetRef();
			Work.Car = Car;
			Work.Item = Item;
			Work.bLoad = Pass == 0;
			Work.PlannedAmount = static_cast<int32>(STLayout::GetCarCapacity(Car, Item));
		}
		if (Pass == 0)
		{
			for (AFGFreightWagon* Car : Fit.SkipAtPickup)
			{
				Leg.SkipCars.Add(Car);
			}
		}
	}
	Job.Summary = FString::Printf(TEXT("%s %s -> %s"), *STNames::Item(Item), *STLayout::Name(From), *STLayout::Name(To));
	State->Job = Job;
	State->bHasJob = true;
	UE_LOG(LogSmartTrains, Log, TEXT("JOB %d %s: %s by hand, %d cars, up to %s"), Job.Id, *STLayout::Name(Train), *Job.Summary, Fit.Cars.Num(),
		*STNames::Amount(Item, Fit.Capacity));
	WriteJob(*State, TEXT("job given by hand"));
	OutResult = FString::Printf(TEXT("job %d: %s with %d cars (%d skipped), up to %s"), Job.Id, *Job.Summary, Fit.Cars.Num(), Fit.SkipAtPickup.Num(),
		*STNames::Amount(Item, Fit.Capacity));
	return true;
}

//~ Flow plan

void ASTSubsystem::PlanFlows()
{
	TArray<FSTNode> Nodes;
	BuildNodes(Nodes);
	const TMap<FString, double> PreviousPairs = MoveTemp(mPlannedPairs);
	mPlannedPairs.Reset();
	mAdvice.Reset();
	mPendingRoster.Reset();
	TSet<UClass*> Items;
	for (const FSTNode& Node : Nodes)
	{
		if (Node.Item)
		{
			Items.Add(Node.Item.Get());
		}
	}
	for (UClass* Item : Items)
	{
		// Suppliers and consumers of Item, with what is left of their rates.
		TArray<int32> Suppliers;
		TArray<int32> Consumers;
		TMap<int32, double> Left;
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			const FSTNode& Node = Nodes[Index];
			if (Node.Item.Get() != Item)
			{
				continue;
			}
			// A starved consumer and a full supplier can't measure their rates: their machines' recipes tell, else a guess.
			double Rate = Node.Rate;
			if (Node.Role == ESTRole::Demand && Node.bStarved)
			{
				Rate = Node.RecipeRate > 0.0 ? FMath::Max(Node.Rate, Node.RecipeRate) : FMath::Max(Node.Rate, 1.0 / 60.0) * 1.5;
			}
			if (Node.Role == ESTRole::Supply && Node.Capacity > 0 && Node.Stock >= FullSupplyShare * Node.Capacity)
			{
				Rate = FMath::Max(Rate, Node.RecipeRate) + Node.Stock / FullStockSeconds;
			}
			if (Rate <= 0.0)
			{
				continue;
			}
			(Node.Role == ESTRole::Supply ? Suppliers : Consumers).Add(Index);
			Left.Add(Index, Rate);
		}
		if (Suppliers.Num() == 0 || Consumers.Num() == 0)
		{
			continue;
		}
		FAdvice& Advice = mAdvice.FindOrAdd(Item);
		// One minimum-cost flow per priority tier, highest first: within a tier the total travel time of the planned
		// flow is as small as possible; later tiers get what is left of the suppliers.
		Consumers.Sort([&](int32 A, int32 B) { return Nodes[A].Priority > Nodes[B].Priority; });
		for (int32 Start = 0; Start < Consumers.Num();)
		{
			int32 End = Start;
			while (End < Consumers.Num() && Nodes[Consumers[End]].Priority == Nodes[Consumers[Start]].Priority)
			{
				++End;
			}
			// Graph: 0 source, 1 sink, then the suppliers, then this tier's consumers.
			const int32 SupplierBase = 2;
			const int32 ConsumerBase = SupplierBase + Suppliers.Num();
			FSTMinCostFlow Flow(ConsumerBase + (End - Start));
			for (int32 S = 0; S < Suppliers.Num(); ++S)
			{
				Flow.AddEdge(0, SupplierBase + S, Left[Suppliers[S]], 0.0);
			}
			for (int32 C = Start; C < End; ++C)
			{
				Flow.AddEdge(ConsumerBase + (C - Start), 1, Left[Consumers[C]], 0.0);
			}
			struct FRoute
			{
				int32 Edge;
				int32 From;
				int32 To;
				float Time;
			};
			TArray<FRoute> Routes;
			for (int32 S = 0; S < Suppliers.Num(); ++S)
			{
				const int32 From = Suppliers[S];
				for (int32 C = Start; C < End; ++C)
				{
					const int32 To = Consumers[C];
					if (Nodes[To].TrackGraph != Nodes[From].TrackGraph || Nodes[To].Station == Nodes[From].Station)
					{
						continue;
					}
					const float Time = EstimateTravelSeconds(Nodes[From].Station.Get(), Nodes[To].Station.Get(), true);
					if (Time < 0.f)
					{
						continue;
					}
					const double Cost = PreviousPairs.Contains(PairKey(Nodes[From], Nodes[To])) ? FlowKeepCost * Time : Time;
				Routes.Add({Flow.AddEdge(SupplierBase + S, ConsumerBase + (C - Start), TNumericLimits<double>::Max(), Cost), From, To, Time});
				}
			}
			const double Sent = Flow.Solve(0, 1);
			UE_LOG(LogSmartTrains, Verbose, TEXT("PLAN %s, priority %s: %d routes, %s/min planned"), *STNames::Item(Item),
				STNames::Priority(Nodes[Consumers[Start]].Priority), Routes.Num(), *STNames::Amount(Item, Sent * 60.0));
			for (const FRoute& Route : Routes)
			{
				const double Planned = Flow.GetFlow(Route.Edge);
				if (Planned > 1e-6)
				{
					Left[Route.From] -= Planned;
					Left[Route.To] -= Planned;
					AddPlannedRoute(Nodes, Route.From, Route.To, Planned, Route.Time, Advice);
				}
			}
			Start = End;
		}
	}
	mPlannedNodes = Nodes;
	mPendingRoster.Sort([](const FSTRosterRoute& A, const FSTRosterRoute& B) { return A.GetKey() < B.GetKey(); });
	DesignServices(MoveTemp(mPendingRoster));
	mPendingRoster.Reset();
}

void ASTSubsystem::BuildRoster(TArray<FSTRosterRoute>&& Candidates)
{
	// A route joins the roster once the flow plan has had it RosterStablePlans times in a row; routes on it stay while
	// the flow plan has them.
	TMap<FString, int32> Seen;
	TArray<FSTRosterRoute> Routes;
	for (FSTRosterRoute& Route : Candidates)
	{
		const FString Key = Route.GetKey();
		const int32 Count = mRouteSeen.FindRef(Key) + 1;
		Seen.Add(Key, Count);
		const bool bOnRoster = mRosterRoutes.ContainsByPredicate([&](const FSTRosterRoute& Each) { return Each.GetKey() == Key; });
		if (bOnRoster || Count >= RosterStablePlans)
		{
			Routes.Add(MoveTemp(Route));
		}
	}
	mRouteSeen = MoveTemp(Seen);
	Routes.Sort([](const FSTRosterRoute& A, const FSTRosterRoute& B) { return A.Trains != B.Trains ? A.Trains > B.Trains : A.GetKey() < B.GetKey(); });

	// The previous roster per route: its trains needed and each other train's share of it; and each item's shuttles.
	struct FOld
	{
		double Trains = 0.0;
		TArray<TPair<TWeakObjectPtr<AFGTrain>, double>> Shares;
	};
	TMap<UClass*, TArray<TWeakObjectPtr<AFGTrain>>> OldShuttles;
	TMap<FString, FOld> Old;
	for (int32 Index = 0; Index < mRosterRoutes.Num(); ++Index)
	{
		Old.FindOrAdd(mRosterRoutes[Index].GetKey()).Trains = mRosterRoutes[Index].Trains;
	}
	for (const TPair<TWeakObjectPtr<AFGTrain>, FSTRosterDuty>& Pair : mRoster)
	{
		if (Pair.Value.bShuttle && Pair.Value.Routes.Num() > 0 && mRosterRoutes.IsValidIndex(Pair.Value.Routes[0]))
		{
			OldShuttles.FindOrAdd(mRosterRoutes[Pair.Value.Routes[0]].Item.Get()).Add(Pair.Key);
			continue;
		}
		for (int32 Index = 0; Index < Pair.Value.Routes.Num(); ++Index)
		{
			if (mRosterRoutes.IsValidIndex(Pair.Value.Routes[Index]))
			{
				Old.FindOrAdd(mRosterRoutes[Pair.Value.Routes[Index]].GetKey()).Shares.Add({Pair.Key, Pair.Value.Shares[Index]});
			}
		}
	}
	// Whether Train had a share of any of Item's routes before.
	auto WasOnItem = [this](const UClass* Item, const TWeakObjectPtr<AFGTrain>& Train)
	{
		const FSTRosterDuty* Previous = mRoster.Find(Train);
		return Previous && Previous->Routes.ContainsByPredicate([&](int32 Route) { return mRosterRoutes.IsValidIndex(Route) && mRosterRoutes[Route].Item.Get() == Item; });
	};

	TMap<TWeakObjectPtr<AFGTrain>, FSTRosterDuty> Duties;
	auto LoadOf = [&Duties](const TWeakObjectPtr<AFGTrain>& Train)
	{
		const FSTRosterDuty* Duty = Duties.Find(Train);
		return Duty ? Duty->Load : 0.0;
	};
	auto Give = [&Duties](const TWeakObjectPtr<AFGTrain>& Train, int32 Route, double Share)
	{
		FSTRosterDuty& Duty = Duties.FindOrAdd(Train);
		const int32 Existing = Duty.Routes.IndexOfByKey(Route);
		if (Existing != INDEX_NONE)
		{
			Duty.Shares[Existing] += Share;
		}
		else
		{
			Duty.Routes.Add(Route);
			Duty.Shares.Add(Share);
		}
		Duty.Load += Share;
	};
	auto IsShuttle = [&Duties](const TWeakObjectPtr<AFGTrain>& Train)
	{
		const FSTRosterDuty* Duty = Duties.Find(Train);
		return Duty && Duty->bShuttle;
	};
	// Shuttles first, per item: an item whose roster routes together keep a whole train busy gets whole trains, each
	// running only the item's routes it fits whole (like a unit train for the item). An item keeps its shuttles from
	// before while it still needs ShuttleKeepTrains of each; then items, biggest first, take the trains that carry most
	// while they still need ShuttleMinTrains of one.
	int32 Managed = 0;
	for (const FSTTrainState& State : mTrains)
	{
		Managed += State.bManaged && IsValid(State.Train) ? 1 : 0;
	}
	const int32 MaxShuttles = FMath::FloorToInt(ShuttleFleetShare * Managed);
	int32 Shuttles = 0;
	TArray<double> Covered;
	Covered.SetNumZeroed(Routes.Num());
	// Per item: its routes (biggest first, as Routes is sorted), what they carry per round trip and what its shuttles
	// carry.
	struct FItemNeed
	{
		TArray<int32> Routes;
		double PerTrip = 0.0;
		double Covered = 0.0;
		int32 Shuttles = 0;
	};
	TMap<UClass*, FItemNeed> ItemNeeds;
	TArray<UClass*> ItemOrder;
	for (int32 Route = 0; Route < Routes.Num(); ++Route)
	{
		UClass* Item = Routes[Route].Item.Get();
		if (!Item)
		{
			continue;
		}
		if (!ItemNeeds.Contains(Item))
		{
			ItemOrder.Add(Item);
		}
		FItemNeed& Need = ItemNeeds.FindOrAdd(Item);
		Need.Routes.Add(Route);
		Need.PerTrip += Routes[Route].Flow * Routes[Route].RoundTrip;
	}
	Algo::StableSort(ItemOrder, [&](UClass* A, UClass* B) { return ItemNeeds[A].PerTrip > ItemNeeds[B].PerTrip; });
	auto IsWhole = [](const FSTRosterRoute& Each, int32 Fit)
	{
		return Each.Whole.IsValidIndex(Fit) && Each.Whole[Fit] && Each.Capacity.IsValidIndex(Fit);
	};
	// What Train carries as a shuttle of Item: its load on the item's biggest route, which it must fit whole; else 0.
	auto ShuttleCapacity = [&](UClass* Item, const TWeakObjectPtr<AFGTrain>& Train)
	{
		const FSTServiceConfig* Config = FindServiceConfig(Item);
		if (Config && Config->Trains.Num() > 0 && !Config->Trains.Contains(Train.Get()))
		{
			return 0;
		}
		const FSTRosterRoute& Biggest = Routes[ItemNeeds[Item].Routes[0]];
		const int32 Fit = Biggest.Fitting.IndexOfByKey(Train);
		return Fit != INDEX_NONE && Train.IsValid() && IsWhole(Biggest, Fit) ? FMath::Max(1, Biggest.Capacity[Fit]) : 0;
	};
	auto StillNeeds = [&](UClass* Item, int32 Capacity)
	{
		const FItemNeed& Need = ItemNeeds[Item];
		return (Need.PerTrip - Need.Covered) / FMath::Max(1, Capacity);
	};
	// Train's cars hold an item other than Item: a new shuttle would first have to get rid of it.
	auto HoldsOther = [](const TWeakObjectPtr<AFGTrain>& Train, const UClass* Item)
	{
		for (const TPair<UClass*, int32>& Each : GetCargo(Train.Get()))
		{
			if (Each.Key != Item && Each.Value > 0)
			{
				return true;
			}
		}
		return false;
	};
	// A shuttle's duty: the item's routes it fits whole, each with its part of what they carry.
	auto MakeShuttle = [&](UClass* Item, const TWeakObjectPtr<AFGTrain>& Train, int32 Capacity)
	{
		FItemNeed& Need = ItemNeeds[Item];
		FSTRosterDuty& Duty = Duties.FindOrAdd(Train);
		Duty = FSTRosterDuty();
		double Served = 0.0;
		for (const int32 Route : Need.Routes)
		{
			const int32 Fit = Routes[Route].Fitting.IndexOfByKey(Train);
			if (Fit != INDEX_NONE && IsWhole(Routes[Route], Fit))
			{
				Duty.Routes.Add(Route);
				Duty.Shares.Add(Routes[Route].Flow * Routes[Route].RoundTrip);
				Served += Duty.Shares.Last();
			}
		}
		for (double& Share : Duty.Shares)
		{
			Share = Served > 0.0 ? Share / Served : 1.0 / Duty.Shares.Num();
		}
		Duty.Load = 1.0;
		Duty.bShuttle = true;
		Need.Covered += Capacity;
		++Need.Shuttles;
		++Shuttles;
	};
	auto IsPinned = [&](const UClass* Item)
	{
		const FSTServiceConfig* Config = FindServiceConfig(Item);
		return Config && Config->bPinned;
	};
	auto IsPooled = [&](const UClass* Item)
	{
		const FSTServiceConfig* Config = FindServiceConfig(Item);
		return Config && Config->bPooled;
	};
	// A service keeps its trains while it needs at least ShuttleKeepTrains of each, a pinned one at least one train, and
	// a train it needs less is only let go after ServiceDropBuilds builds in a row, so short dips don't reshuffle it.
	TMap<TWeakObjectPtr<UClass>, int32> Low;
	for (UClass* Item : ItemOrder)
	{
		bool bLow = false;
		for (const TWeakObjectPtr<AFGTrain>& Train : IsPooled(Item) ? TArray<TWeakObjectPtr<AFGTrain>>() : OldShuttles.FindRef(Item))
		{
			const int32 Capacity = Duties.Contains(Train) ? 0 : ShuttleCapacity(Item, Train);
			if (Shuttles >= MaxShuttles || Capacity <= 0)
			{
				continue;
			}
			const bool bNeeded = StillNeeds(Item, Capacity) >= ShuttleKeepTrains || (IsPinned(Item) && ItemNeeds[Item].Shuttles == 0);
			if (bNeeded || mServiceLow.FindRef(Item) < ServiceDropBuilds)
			{
				bLow |= !bNeeded;
				MakeShuttle(Item, Train, Capacity);
			}
		}
		if (bLow)
		{
			Low.Add(Item, mServiceLow.FindRef(Item) + 1);
		}
	}
	mServiceLow = MoveTemp(Low);
	for (UClass* Item : ItemOrder)
	{
		const FSTRosterRoute& Biggest = Routes[ItemNeeds[Item].Routes[0]];
		while (Shuttles < MaxShuttles && !IsPooled(Item))
		{
			TWeakObjectPtr<AFGTrain> Best;
			int32 BestCapacity = 0;
			bool bBestPrevious = false;
			for (const TWeakObjectPtr<AFGTrain>& Train : Biggest.Fitting)
			{
				const int32 Capacity = Duties.Contains(Train) || HoldsOther(Train, Item) ? 0 : ShuttleCapacity(Item, Train);
				if (Capacity <= 0)
				{
					continue;
				}
				const bool bPrevious = WasOnItem(Item, Train);
				if (!Best.IsValid() || Capacity > BestCapacity || (Capacity == BestCapacity && bPrevious && !bBestPrevious))
				{
					Best = Train;
					BestCapacity = Capacity;
					bBestPrevious = bPrevious;
				}
			}
			if (!Best.IsValid() || (StillNeeds(Item, BestCapacity) < ShuttleMinTrains && !(IsPinned(Item) && ItemNeeds[Item].Shuttles == 0)))
			{
				break;
			}
			MakeShuttle(Item, Best, BestCapacity);
		}
	}
	// What an item's shuttles carry counts against the routes they run, in proportion to what each carries.
	for (const TPair<UClass*, FItemNeed>& Pair : ItemNeeds)
	{
		if (Pair.Value.Covered <= 0.0)
		{
			continue;
		}
		TSet<int32> Served;
		for (const TPair<TWeakObjectPtr<AFGTrain>, FSTRosterDuty>& Duty : Duties)
		{
			if (Duty.Value.bShuttle && Duty.Value.Routes.Num() > 0 && Routes[Duty.Value.Routes[0]].Item.Get() == Pair.Key)
			{
				Served.Append(Duty.Value.Routes);
			}
		}
		double ServedPerTrip = 0.0;
		for (const int32 Route : Served)
		{
			ServedPerTrip += Routes[Route].Flow * Routes[Route].RoundTrip;
		}
		for (const int32 Route : Served)
		{
			const double PerTrip = Routes[Route].Flow * Routes[Route].RoundTrip;
			Covered[Route] = ServedPerTrip > 0.0 ? FMath::Min(PerTrip, Pair.Value.Covered * PerTrip / ServedPerTrip) : 0.0;
		}
	}
	auto ShuttlesOf = [&](const UClass* Item)
	{
		const FItemNeed* Need = ItemNeeds.Find(const_cast<UClass*>(Item));
		return Need ? Need->Shuttles : 0;
	};
	// Then the shares routes already had, for what their shuttles don't carry: all of them while the trains a route
	// needs changed by less than RosterKeepShare and its shuttles are the same, else as many as it still needs. Then what
	// routes still need, from trains that fit them with room: their previous trains first, then the least loaded.
	TArray<double> Needed;
	for (int32 Route = 0; Route < Routes.Num(); ++Route)
	{
		const FSTRosterRoute& Each = Routes[Route];
		const double PerTrip = Each.Flow * Each.RoundTrip;
		double Need = PerTrip > 0.0 ? Each.Trains * FMath::Max(0.0, 1.0 - Covered[Route] / PerTrip) : Each.Trains;
		if (const FOld* Before = Old.Find(Each.GetKey()))
		{
			const bool bKeep = ShuttlesOf(Each.Item.Get()) == OldShuttles.FindRef(Each.Item.Get()).Num()
				&& FMath::Abs(Each.Trains - Before->Trains) <= RosterKeepShare * FMath::Max(Before->Trains, RosterMinShare);
			for (const TPair<TWeakObjectPtr<AFGTrain>, double>& Share : Before->Shares)
			{
				if (!Share.Key.IsValid() || !Each.Fitting.Contains(Share.Key) || IsShuttle(Share.Key) || (!bKeep && Need < RosterMinShare))
				{
					continue;
				}
				const double Kept = bKeep ? Share.Value : FMath::Min(Share.Value, Need);
				Give(Share.Key, Route, Kept);
				Need -= Kept;
			}
			if (bKeep)
			{
				Need = 0.0;
			}
		}
		Needed.Add(Need);
	}
	int32 Short = 0;
	for (int32 Route = 0; Route < Routes.Num(); ++Route)
	{
		const FSTRosterRoute& Each = Routes[Route];
		const FOld* Before = Old.Find(Each.GetKey());
		while (Needed[Route] >= RosterMinShare)
		{
			TWeakObjectPtr<AFGTrain> Best;
			double BestLoad = TNumericLimits<double>::Max();
			bool bBestPrevious = false;
			for (const TWeakObjectPtr<AFGTrain>& Candidate : Each.Fitting)
			{
				const double Load = LoadOf(Candidate);
				if (!Candidate.IsValid() || 1.0 - Load < RosterMinShare)
				{
					continue;
				}
				const bool bPrevious = Before && Before->Shares.ContainsByPredicate([&](const TPair<TWeakObjectPtr<AFGTrain>, double>& Share) { return Share.Key == Candidate; });
				if (!Best.IsValid() || (bPrevious && !bBestPrevious) || (bPrevious == bBestPrevious && Load < BestLoad))
				{
					Best = Candidate;
					BestLoad = Load;
					bBestPrevious = bPrevious;
				}
			}
			if (!Best.IsValid())
			{
				++Short;
				break;
			}
			const double Share = FMath::Min(Needed[Route], 1.0 - BestLoad);
			Give(Best, Route, Share);
			Needed[Route] -= Share;
		}
	}
	// A train keeps its place in its routes while they are the same ones.
	int32 Moved = 0;
	TSet<TWeakObjectPtr<AFGTrain>> Different;
	for (TPair<TWeakObjectPtr<AFGTrain>, FSTRosterDuty>& Pair : Duties)
	{
		FSTRosterDuty& Duty = Pair.Value;
		TArray<FString> Keys;
		for (const int32 Route : Duty.Routes)
		{
			Keys.Add(Routes[Route].GetKey());
		}
		TArray<FString> OldKeys;
		const FSTRosterDuty* Previous = mRoster.Find(Pair.Key);
		for (const int32 Route : Previous ? Previous->Routes : TArray<int32>())
		{
			OldKeys.Add(mRosterRoutes.IsValidIndex(Route) ? mRosterRoutes[Route].GetKey() : FString());
		}
		if (Previous && Keys == OldKeys && Previous->bShuttle == Duty.bShuttle)
		{
			Duty.Next = Previous->Next;
		}
		else
		{
			++Moved;
			Different.Add(Pair.Key);
		}
	}
	Moved += mRoster.Num() - mRoster.FilterByPredicate([&](const TPair<TWeakObjectPtr<AFGTrain>, FSTRosterDuty>& Pair) { return Duties.Contains(Pair.Key); }).Num();
	const bool bAnyChange = Moved > 0 || Routes.Num() != mRosterRoutes.Num();
	mRosterRoutes = MoveTemp(Routes);
	mRoster = MoveTemp(Duties);
	mServices.Reset();
	for (const TPair<UClass*, FItemNeed>& Pair : ItemNeeds)
	{
		if (Pair.Value.Shuttles == 0)
		{
			continue;
		}
		FServiceInfo& Service = mServices.Add(Pair.Key);
		Service.Trains = Pair.Value.Shuttles;
		double Weighted = 0.0;
		double Total = 0.0;
		for (const int32 Route : Pair.Value.Routes)
		{
			const FSTRosterRoute& Each = mRosterRoutes[Route];
			const double PerTrip = Each.Flow * Each.RoundTrip;
			Weighted += PerTrip * Each.RoundTrip;
			Total += PerTrip;
			Service.Routes.Add(FString::Printf(TEXT("%s -> %s"), *STLayout::Name(Each.From.Get()), *STLayout::Name(Each.To.Get())));
		}
		const FSTServiceConfig* Config = FindServiceConfig(Pair.Key);
		const double RoundTrip = Total > 0.0 ? Weighted / Total : 0.0;
		Service.Headway = Config && Config->RhythmMinutes > 0.f ? Config->RhythmMinutes * 60.0 : RoundTrip / FMath::Max(1, Service.Trains);
		Service.Needed = Pair.Value.Covered > 0.0 ? Pair.Value.PerTrip / (Pair.Value.Covered / Service.Trains) : 0.0;
	}
	mShuttles.Reset();
	for (const TPair<TWeakObjectPtr<AFGTrain>, FSTRosterDuty>& Pair : mRoster)
	{
		for (const int32 Route : Pair.Value.bShuttle ? Pair.Value.Routes : TArray<int32>())
		{
			mShuttles.FindOrAdd(mRosterRoutes[Route].GetKey()).Add(Pair.Key);
		}
	}
	if (!bAnyChange)
	{
		return;
	}
	UE_LOG(LogSmartTrains, Log, TEXT("ROSTER: %d routes on %d trains (%d service trains), %d trains' duties changed%s"), mRosterRoutes.Num(), mRoster.Num(), Shuttles, Moved,
		Short > 0 ? *FString::Printf(TEXT(", %d routes short of fitting trains (the horizon planner covers them)"), Short) : TEXT(""));
	for (const TPair<TWeakObjectPtr<AFGTrain>, FSTRosterDuty>& Pair : mRoster)
	{
		if (Different.Contains(Pair.Key))
		{
			UE_LOG(LogSmartTrains, Log, TEXT("ROSTER %s: %s (%.2f of a train)"), *STLayout::Name(Pair.Key.Get()), *DescribeDuty(Pair.Value), Pair.Value.Load);
		}
	}
}

bool ASTSubsystem::IsShuttleRoute(const AFGTrain* Train, const AFGTrainStationIdentifier* From, const AFGTrainStationIdentifier* To, const UClass* Item) const
{
	const TArray<TWeakObjectPtr<AFGTrain>>* Shuttles = mShuttles.Find(FSTRosterRoute::MakeKey(From, To, Item));
	if (!Shuttles || Shuttles->ContainsByPredicate([Train](const TWeakObjectPtr<AFGTrain>& Each) { return Each.Get() == Train; }))
	{
		return false;
	}
	for (const TWeakObjectPtr<AFGTrain>& Each : *Shuttles)
	{
		const FSTTrainState* State = mTrains.FindByPredicate([&](const FSTTrainState& Candidate) { return Candidate.Train == Each.Get(); });
		if (!State || !IsValid(State->Train))
		{
			continue;
		}
		if (!State->bHasJob)
		{
			return true;
		}
		for (int32 Leg = State->Job.Leg; Leg < State->Job.Legs.Num(); ++Leg)
		{
			const FSTJobLeg& Next = State->Job.Legs[Leg];
			if ((Next.Station == From && Next.GetItems(true).Contains(Item)) || (Next.Station == To && Next.GetItems(false).Contains(Item)))
			{
				return true;
			}
		}
	}
	return false;
}

const FSTServiceConfig* ASTSubsystem::FindServiceConfig(const UClass* Item) const
{
	return Item ? mServiceConfigs.FindByPredicate([Item](const FSTServiceConfig& Each) { return Each.Item.Get() == Item; }) : nullptr;
}

bool ASTSubsystem::SetService(UClass* Item, const FString& Verb, float Minutes, AFGTrain* Train, const FString& Cause, FString& OutResult)
{
	if (!Item)
	{
		OutResult = TEXT("Name an item.");
		return false;
	}
	FSTServiceConfig* Config = mServiceConfigs.FindByPredicate([Item](const FSTServiceConfig& Each) { return Each.Item.Get() == Item; });
	if (!Config)
	{
		Config = &mServiceConfigs.AddDefaulted_GetRef();
		Config->Item = Item;
	}
	const FString Name = STNames::Item(Item);
	if (Verb == TEXT("pin"))
	{
		Config->bPinned = true;
		Config->bPooled = false;
		OutResult = FString::Printf(TEXT("%s keeps a service train even when the flow plan needs less than one."), *Name);
	}
	else if (Verb == TEXT("unpin"))
	{
		Config->bPinned = false;
		OutResult = FString::Printf(TEXT("%s gets a service only while the flow plan needs one."), *Name);
	}
	else if (Verb == TEXT("pool"))
	{
		Config->bPooled = true;
		Config->bPinned = false;
		OutResult = FString::Printf(TEXT("%s gets no service; pool trains carry it."), *Name);
	}
	else if (Verb == TEXT("auto"))
	{
		*Config = FSTServiceConfig();
		Config->Item = Item;
		OutResult = FString::Printf(TEXT("%s's service is automatic again."), *Name);
	}
	else if (Verb == TEXT("rhythm"))
	{
		Config->RhythmMinutes = FMath::Clamp(Minutes, 0.f, 240.f);
		OutResult = Config->RhythmMinutes > 0.f ? FString::Printf(TEXT("%s's service picks up at most every %.1f minutes."), *Name, Config->RhythmMinutes)
			: FString::Printf(TEXT("%s's service spacing comes from its round trip again."), *Name);
	}
	else if ((Verb == TEXT("add") || Verb == TEXT("remove")) && Train)
	{
		if (Verb == TEXT("add"))
		{
			Config->Trains.AddUnique(Train);
		}
		else
		{
			Config->Trains.Remove(Train);
		}
		OutResult = FString::Printf(TEXT("%s's service may use %d chosen trains%s."), *Name, Config->Trains.Num(), Config->Trains.Num() == 0 ? TEXT(" (none: any that fit)") : TEXT(""));
	}
	else if (Verb == TEXT("dedicated"))
	{
		Config->bDedicated = true;
		OutResult = FString::Printf(TEXT("%s only runs in loops of its own, never merged with other items' loops."), *Name);
	}
	else if (Verb == TEXT("clear"))
	{
		Config->Trains.Reset();
		OutResult = FString::Printf(TEXT("%s's service may use any train whose cars all fit."), *Name);
	}
	else
	{
		OutResult = TEXT("Use dedicated or auto.");
		return false;
	}
	mServiceConfigs.RemoveAll([](const FSTServiceConfig& Each) { return !Each.Item || Each.IsDefault(); });
	for (FSTServiceConfig& Each : mServiceConfigs)
	{
		Each.Trains.RemoveAll([](const TObjectPtr<AFGTrain>& Each) { return !IsValid(Each); });
	}
	UE_LOG(LogSmartTrains, Log, TEXT("SERVICE %s: %s (%s)"), *Name, *OutResult, *Cause);
	PlanFlows();
	return true;
}

const FSTRosterDuty* ASTSubsystem::GetShuttleDuty(const AFGTrain* Train) const
{
	const FSTRosterDuty* Duty = Train ? mRoster.Find(const_cast<AFGTrain*>(Train)) : nullptr;
	return Duty && Duty->bShuttle && Duty->Routes.Num() > 0 ? Duty : nullptr;
}

bool ASTSubsystem::IsSlowDrain(const AFGBuildableTrainPlatformCargo* Platform) const
{
	const double* Until = Platform ? mSlowDrainUntil.Find(Platform) : nullptr;
	return Until && *Until > Now();
}

FString ASTSubsystem::DescribeDuty(const FSTRosterDuty& Duty) const
{
	TArray<FString> Parts;
	for (const int32 Route : Duty.Routes)
	{
		if (mRosterRoutes.IsValidIndex(Route))
		{
			const FSTRosterRoute& Each = mRosterRoutes[Route];
			Parts.Add(FString::Printf(TEXT("%s %s -> %s"), *STNames::Item(Each.Item.Get()), *STLayout::Name(Each.From.Get()), *STLayout::Name(Each.To.Get())));
		}
	}
	return (Duty.bShuttle ? FString(TEXT("service: ")) : FString()) + FString::Join(Parts, TEXT(", then "));
}

void ASTSubsystem::AddPlannedRoute(const TArray<FSTNode>& Nodes, int32 From, int32 To, double Flow, float Time, FAdvice& Advice)
{
	const FSTNode& Supplier = Nodes[From];
	const FSTNode& Consumer = Nodes[To];
	mPlannedPairs.FindOrAdd(PairKey(Supplier, Consumer)) += Flow;
	mPlannedPairs.FindOrAdd(NodeKey(Supplier)) += Flow;
	mPlannedPairs.FindOrAdd(NodeKey(Consumer)) += Flow;
	++Advice.Pairs;
	Advice.FlowPerMinute += Flow * 60.0;

	// Trains needed for this route: flow x round trip / load of the trains that fit.
	const float Back = EstimateTravelSeconds(Consumer.Station.Get(), Supplier.Station.Get(), true);
	const double RoundTrip = Time + FMath::Max(0.f, Back) + EstimateStaySeconds(Supplier.Station.Get()) + EstimateStaySeconds(Consumer.Station.Get());
	int32 Fitting = 0;
	double Load = 0.0;
	FSTRosterRoute Route;
	Route.From = Supplier.Station;
	Route.To = Consumer.Station;
	Route.Item = Supplier.Item.Get();
	Route.Flow = Flow;
	Route.RoundTrip = RoundTrip;
	for (const FSTTrainState& State : mTrains)
	{
		if (!State.bManaged || !IsValid(State.Train))
		{
			continue;
		}
		const FSTFit Fit = ComputeFit(State.Train, Supplier.Station.Get(), Consumer.Station.Get(), Supplier.Item, true);
		if (Fit.Capacity > 0)
		{
			++Fitting;
			Load += Fit.Capacity;
			Route.Fitting.Add(State.Train.Get());
			Route.Capacity.Add(Fit.Capacity);
			int32 Wagons = 0;
			for (AFGRailroadVehicle* Vehicle : STLayout::GetVehicles(State.Train))
			{
				Wagons += Cast<AFGFreightWagon>(Vehicle) ? 1 : 0;
			}
			Route.Whole.Add(Fit.Cars.Num() >= Wagons);
		}
	}
	if (Fitting > 0)
	{
		// The same route from another priority tier adds to it.
		Route.Trains = Flow * RoundTrip / (Load / Fitting);
		if (FSTRosterRoute* Same = mPendingRoster.FindByPredicate([&](const FSTRosterRoute& Each) { return Each.GetKey() == Route.GetKey(); }))
		{
			Same->Flow += Route.Flow;
			Same->Trains += Route.Trains;
		}
		else
		{
			mPendingRoster.Add(MoveTemp(Route));
		}
	}
	Advice.TrainsFitting = FMath::Max(Advice.TrainsFitting, Fitting);
	if (Fitting > 0)
	{
		Advice.TrainsNeeded += Flow * RoundTrip / (Load / Fitting);
	}
	UE_LOG(LogSmartTrains, Verbose, TEXT("PLAN %s: %s -> %s %s/min, %.0f s each way, round trip %.0f s, %d trains fit carrying %s each on average, %.2f trains needed"),
		*STNames::Item(Supplier.Item), *STLayout::Name(Supplier.Station.Get()), *STLayout::Name(Consumer.Station.Get()), *STNames::Amount(Supplier.Item, Flow * 60.0), Time,
		RoundTrip, Fitting, *STNames::Amount(Supplier.Item, Fitting > 0 ? Load / Fitting : 0.0), Fitting > 0 ? Flow * RoundTrip / (Load / Fitting) : 0.0);
}
