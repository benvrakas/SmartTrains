#include "Buildables/FGBuildableRailroadStation.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGFreightWagon.h"
#include "FGInventoryComponent.h"
#include "FGLocomotive.h"
#include "FGRailroadSubsystem.h"
#include "FGRailroadTimeTable.h"
#include "FGTrain.h"
#include "FGTrainStationIdentifier.h"
#include "Resources/FGItemDescriptor.h"
#include "SmartTrains.h"
#include "STBridges.h"
#include "STLayout.h"
#include "STMinCostFlow.h"
#include "STTripPlanner.h"
#include "STSubsystem.h"

namespace
{
	// What one written stop is: station, rules and the cars that skip it.
	FString StopKey(const FTimeTableStop& Stop, const TArray<TObjectPtr<AFGRailroadVehicle>>& SkipCars)
	{
		const FTrainDockingRuleSet& Rules = Stop.DockingRuleSet;
		FString Key = FString::Printf(TEXT("%p|%d|%.1f|%d|"), ToRawPtr(Stop.Station), static_cast<int32>(Rules.DockingDefinition), Rules.DockForDuration,
			Rules.IsDurationAndRule ? 1 : 0);
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

	// A train that undocked this recently and still stands counts as in its station for holding. [s]
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
		if (!Leg.HasWork() || Leg.bPartial || bFluidDeliveryOnly)
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
			if (!Rule.Item && Config.Role != ESTRole::Demand)
			{
				continue;
			}
			int32 Index = FindNode(OutNodes, Identifier, Rule.Item, Config.Role);
			if (Index == INDEX_NONE)
			{
				Index = OutNodes.AddDefaulted();
				FSTNode& New = OutNodes[Index];
				New.Station = Identifier;
				New.Item = Rule.Item;
				New.Role = Config.Role;
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
	// Amounts promised by jobs still to come.
	for (const FSTTrainState& State : mTrains)
	{
		if (!State.bManaged || !State.bHasJob)
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
					OutNodes[Index].Reserved += Work.PlannedAmount;
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
	return Building && IsValid(State.Train) && Building->GetTrackGraphID() == State.Train->GetTrackGraphID();
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

FSTFit ASTSubsystem::ComputeFit(const AFGTrain* Train, const AFGTrainStationIdentifier* From, const AFGTrainStationIdentifier* To, TSubclassOf<UFGItemDescriptor> Item) const
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
			bFitsTo &= Platform && Config && Config->Role == ESTRole::Demand && Config->FindRule(Item)
				&& STLayout::ItemFitsCargoType(Item, STLayout::GetPlatformCargoType(Platform));
		}
		// Empty, or holding only this item (it tops up or delivers it first).
		bool bEmpty = true;
		if (const UFGInventoryComponent* Inventory = Car->GetFreightInventory())
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
			Fit.Capacity += STLayout::GetCapacity(Car->GetFreightInventory(), Item);
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
			const bool bDemands = bTypeOk && Config && Config->Role == ESTRole::Demand && Config->FindRule(Item);
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
	if (!State.bManaged || State.bKeepVanilla || !IsValid(Train) || Train->IsPlayerDriven() || Train->IsDerailed())
	{
		return false;
	}
	const double Time = Now();
	if (!State.bHasJob)
	{
		OutStation = Train->IsDocked() && State.DockedStation.IsValid() ? State.DockedStation.Get() : State.LastStation.Get();
		OutStation = OutStation ? OutStation : FindNearestStation(Train);
		OutTime = Train->IsDocked() ? FMath::Max(Time, State.DockedAt + EstimateStaySeconds(OutStation)) : Time;
		return OutStation != nullptr;
	}
	const FSTJob& Job = State.Job;
	if (Job.bPark && Job.Legs.Num() == 1 && !Job.Legs[0].HasWork())
	{
		// Heading for a depot or out of the way: free to take a job from where it last was.
		OutStation = State.LastStation.Get() ? State.LastStation.Get() : Job.Legs[0].Station.Get();
		OutTime = Time;
		return OutStation != nullptr;
	}
	if (Job.Leg == Job.Legs.Num() - 1 && Train->IsDocked() && State.DockedStation.Get() == Job.Legs.Last().Station)
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

void ASTSubsystem::DispatchRound()
{
	TArray<FSTNode> Nodes;
	BuildNodes(Nodes);
	const double Time = Now();

	mAtTravelCallsLeft = AtTravelCallsPerRound;
	mHeldFor.Reset();
	// Station bookings of the trips already running, from arrival to the end of the stay. A train docked for its current
	// stop holds the station from when it docked; one running late from now.
	TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;
	for (int32 TrainIndex = 0; TrainIndex < mTrains.Num(); ++TrainIndex)
	{
		const FSTTrainState& State = mTrains[TrainIndex];
		if (!State.bManaged || !State.bHasJob)
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
			Bookings.FindOrAdd(Each.Station.Get()).Add({TrainIndex, From, FMath::Max(Each.PlannedArrival, From) + Stay});
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
			Bookings.FindOrAdd(State.LastStation.Get()).Add({TrainIndex, Time, Time + Hold});
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
				Bookings.FindOrAdd(Station).Add({INDEX_NONE, Time, Time + Left});
			}
		}
	}
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
			if (!bWanted || !State.bManaged || !State.bHasJob || State.Job.bPark || !IsValid(State.Train) || State.Train->IsDocked())
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

	// Free trains and the moves each could make.
	struct FFree
	{
		int32 TrainIndex = INDEX_NONE;
		AFGTrainStationIdentifier* Anchor = nullptr;
		double FreeAt = 0.0;
		bool bDocked = false;
		TArray<FSTPlanMove> Moves;
	};
	TArray<FFree> Free;
	for (int32 TrainIndex = 0; TrainIndex < mTrains.Num() && !Planner.IsOverBudget(); ++TrainIndex)
	{
		const FSTTrainState& State = mTrains[TrainIndex];
		FFree Entry;
		Entry.TrainIndex = TrainIndex;
		if (!GetFreeAt(State, Entry.Anchor, Entry.FreeAt))
		{
			continue;
		}
		Entry.bDocked = State.Train->IsDocked() && State.DockedStation.Get() == Entry.Anchor;
		if (Planner.BeginTrain(TrainIndex, Entry.Anchor, Entry.FreeAt, Entry.bDocked) && Planner.GetMoves().Num() > 0)
		{
			Entry.Moves = Planner.GetMoves();
			Free.Add(MoveTemp(Entry));
		}
	}

	// Exact assignment of trains to first moves (two slots per move, so a big supplier can take two trains).
	constexpr int32 AnchorsPerTrain = 6;
	constexpr int32 SlotsPerMove = 2;
	TArray<FSTPlanMove> Anchors;
	for (const FFree& Entry : Free)
	{
		for (int32 Index = 0; Index < Entry.Moves.Num() && Index < AnchorsPerTrain; ++Index)
		{
			if (Entry.Moves[Index].SoloScore > 0.0)
			{
				Anchors.AddUnique(Entry.Moves[Index]);
			}
		}
	}
	TArray<TArray<double>> Value;
	for (const FFree& Entry : Free)
	{
		TArray<double>& Row = Value.AddDefaulted_GetRef();
		Row.Init(0.0, Anchors.Num() * SlotsPerMove);
		for (int32 Index = 0; Index < Anchors.Num(); ++Index)
		{
			if (const FSTPlanMove* Own = Entry.Moves.FindByKey(Anchors[Index]))
			{
				for (int32 Slot = 0; Slot < SlotsPerMove; ++Slot)
				{
					// The second slot is worth a little less, so one train per move is preferred when it is a tie.
					Row[Index * SlotsPerMove + Slot] = Own->SoloScore * (Slot == 0 ? 1.0 : 0.95);
				}
			}
		}
	}
	const TArray<int32> Assigned = STAssignment::Solve(Value);
	TArray<int32> Order;
	for (int32 Row = 0; Row < Free.Num(); ++Row)
	{
		Order.Add(Row);
	}
	Order.Sort([&](int32 A, int32 B)
	{
		const double ValueA = Assigned[A] != INDEX_NONE ? Value[A][Assigned[A]] : -1.0;
		const double ValueB = Assigned[B] != INDEX_NONE ? Value[B][Assigned[B]] : -1.0;
		return ValueA > ValueB;
	});

	// Records why a free train got no trip; logged when the reason changes.
	auto NoTrip = [this](const FSTTrainState& State, const FString& Reason)
	{
		FString& Last = mIdleReasons.FindOrAdd(State.Train.Get());
		if (Last != Reason)
		{
			Last = Reason;
			UE_LOG(LogSmartTrains, Log, TEXT("IDLE %s: no trip: %s"), *STLayout::Name(State.Train), *Reason);
		}
	};
	int32 Planned = 0;
	int32 Trips = 0;
	bool bOutOfTime = false;
	// Each train's trip: its assigned first move, then regret insertion; reservations go to the trains after it.
	for (const int32 Row : Order)
	{
		if (Planner.IsOverBudget())
		{
			bOutOfTime = true;
			break;
		}
		++Planned;
		const FFree& Entry = Free[Row];
		FSTTrainState& State = mTrains[Entry.TrainIndex];
		if (!Planner.ResumeTrain(Entry.TrainIndex, Entry.Anchor, Entry.FreeAt, Entry.bDocked)
			&& !Planner.BeginTrain(Entry.TrainIndex, Entry.Anchor, Entry.FreeAt, Entry.bDocked))
		{
			continue;
		}
		TArray<FSTPlanStop> Stops;
		if (Assigned[Row] != INDEX_NONE)
		{
			const FSTPlanMove& First = Anchors[Assigned[Row] / SlotsPerMove];
			if (const FSTPlanMove* Own = Planner.GetMoves().FindByKey(First))
			{
				FSTPlanResult Ignored;
				double Second = 0.0;
				Planner.InsertBest(Stops, *Own, 0, Ignored, Second);
			}
		}
		FSTPlanResult Result = Planner.Improve(Stops, 0);
		FSTTripPlanner::Trim(Stops, Result, 0);
		Result = Planner.Simulate(Stops);
		const bool bCarrying = GetCargo(State.Train).Num() > 0;
		if (Result.bValid && Stops.Num() > 0 && Result.Value > 0.0 && Result.Arrive.Num() > 0)
		{
			// A train standing in a station whose first stop is booked waits where it is, so it doesn't queue on the line.
			// In a station: docked, parked there, or just undocked and still standing there (its trip ended as it left).
			const AFGRailroadVehicle* First = State.Train->GetFirstVehicle();
			const bool bJustLeft = Time - State.LastDepartureAt < JustLeftSeconds && First && First->GetVelocity().Size() < 300.0;
			const bool bInStation = State.Train->IsDocked() || (State.Phase == ESTTrainPhase::Parked && !State.Train->IsSelfDrivingEnabled()) || bJustLeft;
			const float Travel = Stops[0].Station == Entry.Anchor ? 0.f : EstimateTravelSeconds(Entry.Anchor, Stops[0].Station, IsPushPull(State.Train));
			const double Wait = Result.Arrive[0] - (Entry.FreeAt + FMath::Max(0.f, Travel));
			if (bInStation && Travel >= 0.f && Wait > HoldAfterWaitSeconds)
			{
				mHeldFor.Add(Stops[0].Station);
				NoTrip(State, FString::Printf(TEXT("holding at %s: %s is booked by other trains, so it leaves later instead of queueing there"),
					*STLayout::Name(Entry.Anchor), *STLayout::Name(Stops[0].Station)));
				continue;
			}
			// With Auto Trains: the first leg must be drivable now, around every other train (single track included).
			if (bInStation && Stops[0].Station != Entry.Anchor && Stops[0].Station->GetStation())
			{
				float Seconds = -1.f;
				FString Reason;
				const STAt::ELegCheck Check = STAt::CheckLeg(GetWorld(), State.Train, Stops[0].Station->GetStation(), Seconds, Reason);
				if (Check == STAt::ELegCheck::Pending)
				{
					// Asked; the answer comes by the next round.
					continue;
				}
				if (Check == STAt::ELegCheck::Blocked)
				{
					mHeldFor.Add(Stops[0].Station);
					NoTrip(State, FString::Printf(TEXT("holding at %s: Auto Trains sees no way to %s now (%s)"), *STLayout::Name(Entry.Anchor),
						*STLayout::Name(Stops[0].Station), *Reason));
					continue;
				}
				if (Check == STAt::ELegCheck::NotAsked)
				{
					UE_LOG(LogSmartTrains, Verbose, TEXT("CHECK %s to %s not asked: %s"), *STLayout::Name(State.Train), *STLayout::Name(Stops[0].Station), *Reason);
				}
			}
		}
		if (!Result.bValid || Stops.Num() == 0 || Result.Value <= 0.0)
		{
			if (bCarrying && State.UnwantedCargoSince <= 0.0)
			{
				State.UnwantedCargoSince = Time;
				UE_LOG(LogSmartTrains, Log, TEXT("%s carries cargo no trip wants now"), *STLayout::Name(State.Train));
			}
			const FString Problems = Planner.DescribeProblems();
			NoTrip(State, !Problems.IsEmpty()
				? FString::Printf(TEXT("nothing worth running now; trips tried were rejected: %s"), *Problems)
				: FString(TEXT("nothing worth running now (consumers at target, or no batch ready)")));
			continue;
		}
		mIdleReasons.Remove(State.Train.Get());
		++Trips;
		FSTJob Job;
		Job.Id = mNextJobId++;
		Job.StartedAt = Time;
		Job.LegStartedAt = Time;
		Planner.MakeLegs(Stops, Result, Job.Legs);
		TArray<FString> Items;
		bool bDelivers = false;
		for (const FSTJobLeg& Leg : Job.Legs)
		{
			for (const FSTCarWork& Work : Leg.Work)
			{
				Items.AddUnique(STNames::Item(Work.Item));
				bDelivers |= !Work.bLoad;
			}
		}
		TArray<FString> Stations;
		for (const FSTPlanStop& Stop : Stops)
		{
			Stations.Add(STLayout::Name(Stop.Station));
		}
		Job.Summary = FString::Printf(TEXT("%s: %s"), *FString::Join(Items, TEXT(", ")), *FString::Join(Stations, TEXT(" -> ")));
		const FString Line = FString::Printf(TEXT("%s: %s [%s], %d moved, score %.3f"), *STLayout::Name(State.Train), *Job.Summary,
			*Planner.Describe(Stops, Result), Result.Moved, Result.Score);
		Planner.Reserve(Stops, Result);
		CommitJob(State, MoveTemp(Job), Line);
		UpdateUnwantedCargo(State);
	}

	// Free trains with no move at all.
	for (int32 TrainIndex = 0; TrainIndex < mTrains.Num(); ++TrainIndex)
	{
		const FSTTrainState& State = mTrains[TrainIndex];
		AFGTrainStationIdentifier* Anchor = nullptr;
		double FreeAt = 0.0;
		if (!State.bHasJob && GetFreeAt(State, Anchor, FreeAt) && !Free.ContainsByPredicate([TrainIndex](const FFree& Entry) { return Entry.TrainIndex == TrainIndex; }))
		{
			NoTrip(State, TEXT("no supplier and consumer that its cars stop beside at both ends, or nothing wanted (see the platform menus)"));
		}
	}
	UE_LOG(LogSmartTrains, Verbose, TEXT("DISPATCH: %d free trains with moves, %d assigned a first move, %d planned, %d trips, %.1f ms%s"), Free.Num(),
		Assigned.FilterByPredicate([](int32 Column) { return Column != INDEX_NONE; }).Num(), Planned, Trips, Planner.GetElapsedMs(),
		bOutOfTime ? TEXT(", out of time") : TEXT(""));
	if (bOutOfTime && Time - mLastBudgetWarning > 300.0)
	{
		mLastBudgetWarning = Time;
		UE_LOG(LogSmartTrains, Warning, TEXT("Dispatch: planning time (%.0f ms) ran out with %d free trains left; they are planned in the next rounds. Raise \"Planning time per round\" if this keeps happening."),
			mSettings.PlanBudgetMs, Free.Num() - Planned);
	}

	if (!mSettings.bObserveOnly)
	{
		ClearNeededBays();
	}
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
	for (int32 Leg = State.Job.Leg; !bFresh && Leg < State.Job.Legs.Num(); ++Leg)
	{
		const FSTJobLeg& Each = State.Job.Legs[Leg];
		FSTPlanStop& Stop = Stops.AddDefaulted_GetRef();
		Stop.Station = Each.Station;
		Stop.LoadItems = Each.GetItems(true);
		Stop.UnloadItems = Each.GetItems(false);
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
	Result = Planner.Improve(Stops, 0);
	FSTTripPlanner::Trim(Stops, Result, 0);
	Result = Planner.Simulate(Stops);
	// Stability: additions to a trip that still works must beat it by the re-plan threshold.
	if (bKeptWorks && KeptResult.bValid && Result.bValid && Result.Score < KeptResult.Score * FMath::Max(1.f, mSettings.ReplanThreshold))
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
	State.bChainedWhileDocked = State.Train->IsDocked();
	State.Job = MoveTemp(Job);
	State.bHasJob = true;
	UE_LOG(LogSmartTrains, Log, TEXT("JOB %d %s"), State.Job.Id, *Line);
	if (State.bChainedWhileDocked)
	{
		// The game moves a timetable on by one stop when the train departs, so the new one is written as it leaves.
		State.bWriteOnUndock = true;
		UE_LOG(LogSmartTrains, Verbose, TEXT("WRITE %s job %d: written when it leaves its stop"), *STLayout::Name(State.Train), State.Job.Id);
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
	AFGTrainStationIdentifier* Depot = FindDepot(State, State.LastStation.Get());
	if (Depot && Depot != State.LastStation)
	{
		SendToStation(State, Depot, Why);
		return;
	}
	// A train isn't parked on the line: one still at speed drives on to the stop it was heading for, and one that
	// isn't at a station (e.g. just managed out on the track) drives to the nearest station, and parks there.
	const AFGRailroadVehicle* First = State.Train->GetFirstVehicle();
	const bool bMoving = First && First->GetVelocity().Size() > 300.0;
	const bool bOnTheLine = !State.Train->IsDocked() && !State.LastStation;
	AFGRailroadTimeTable* TimeTable = State.Train->GetTimeTable();
	AFGTrainStationIdentifier* Heading = TimeTable && TimeTable->IsValidStop(TimeTable->GetCurrentStop()) ? TimeTable->GetStop(TimeTable->GetCurrentStop()).Station.Get() : nullptr;
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

AFGTrainStationIdentifier* ASTSubsystem::FindDepot(const FSTTrainState& State, const AFGTrainStationIdentifier* From) const
{
	AFGTrainStationIdentifier* Best = nullptr;
	float BestTime = TNumericLimits<float>::Max();
	for (const FSTStationConfig& Config : mStations)
	{
		if (!Config.bDepot || !IsValid(Config.Station) || !CanServe(State, Config.Station))
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
		AFGTrainStationIdentifier* Target = FindDepot(State, Here);
		if (!Target || Target == Here)
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
			Work.PlannedAmount = STLayout::GetCapacity(Car->GetFreightInventory(), Item);
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
	mPlannedPairs.Reset();
	mAdvice.Reset();
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
			const double Rate = Node.Role == ESTRole::Demand && Node.bStarved ? FMath::Max(Node.Rate, 1.0 / 60.0) * 1.5 : Node.Rate;
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
					Routes.Add({Flow.AddEdge(SupplierBase + S, ConsumerBase + (C - Start), TNumericLimits<double>::Max(), Time), From, To, Time});
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
	for (const FSTTrainState& State : mTrains)
	{
		if (!State.bManaged || !IsValid(State.Train))
		{
			continue;
		}
		const FSTFit Fit = ComputeFit(State.Train, Supplier.Station.Get(), Consumer.Station.Get(), Supplier.Item);
		if (Fit.Capacity > 0)
		{
			++Fitting;
			Load += Fit.Capacity;
		}
	}
	Advice.TrainsFitting = FMath::Max(Advice.TrainsFitting, Fitting);
	if (Fitting > 0)
	{
		Advice.TrainsNeeded += Flow * RoundTrip / (Load / Fitting);
	}
	UE_LOG(LogSmartTrains, Verbose, TEXT("PLAN %s: %s -> %s %s/min, %.0f s each way, %d trains fit"), *STNames::Item(Supplier.Item),
		*STLayout::Name(Supplier.Station.Get()), *STLayout::Name(Consumer.Station.Get()), *STNames::Amount(Supplier.Item, Flow * 60.0), Time, Fitting);
}
