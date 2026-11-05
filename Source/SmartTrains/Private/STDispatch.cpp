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
#include "STSubsystem.h"

namespace
{
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
		if (Leg.Item && Leg.bPartial)
		{
			Rules.DockingDefinition = ETrainDockingDefinition::TDD_LoadUnloadOnce;
			Rules.DockForDuration = 0.f;
			Rules.LoadFilterDescriptors = {Leg.bLoad ? Leg.Item : None};
			Rules.UnloadFilterDescriptors = {Leg.bLoad ? None : Leg.Item};
		}
		else if (!Leg.Item)
		{
			Rules.DockingDefinition = ETrainDockingDefinition::TDD_LoadUnloadOnce;
			Rules.DockForDuration = 0.f;
			Rules.LoadFilterDescriptors = {None};
			Rules.UnloadFilterDescriptors = {None};
		}
		else if (Leg.bLoad)
		{
			Rules.DockingDefinition = ETrainDockingDefinition::TDD_FullyLoadUnload;
			Rules.DockForDuration = Settings.LoadWaitSeconds;
			Rules.LoadFilterDescriptors = {Leg.Item};
			Rules.UnloadFilterDescriptors = {None};
		}
		else
		{
			Rules.DockingDefinition = ETrainDockingDefinition::TDD_FullyLoadUnload;
			Rules.DockForDuration = Settings.UnloadWaitSeconds;
			Rules.LoadFilterDescriptors = {None};
			Rules.UnloadFilterDescriptors = {Leg.Item};
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
			if (!Each.Item)
			{
				continue;
			}
			int32 Index = FindNode(OutNodes, Each.Station, Each.Item, Each.bLoad ? ESTRole::Supply : ESTRole::Demand);
			if (Index == INDEX_NONE && !Each.bLoad)
			{
				Index = FindNode(OutNodes, Each.Station, nullptr, ESTRole::Demand);
			}
			if (Index != INDEX_NONE)
			{
				OutNodes[Index].Reserved += Each.PlannedAmount;
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
	const int32 Stack = Item ? UFGItemDescriptor::GetStackSize(Item) : 0;
	for (const TWeakObjectPtr<AFGBuildableTrainPlatformCargo>& Platform : Node.Platforms)
	{
		const UFGInventoryComponent* Inventory = Platform.IsValid() ? Platform->GetInventory() : nullptr;
		if (!Inventory || !STLayout::ItemFitsCargoType(Item, STLayout::GetPlatformCargoType(Platform.Get())))
		{
			continue;
		}
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
	TArray<TArray<AFGRailroadVehicle*>> Maps;
	if (bLearned)
	{
		*bLearned = false;
	}
	if (const FSTDockMap* Seen = mDockMaps.FindByPredicate([&](const FSTDockMap& Map) { return Map.Train == Train && Map.Station == Station; }))
	{
		// A seen map is only used while the train's vehicles are still the ones it saw.
		const TArray<AFGRailroadVehicle*> Vehicles = STLayout::GetVehicles(Train);
		bool bValid = true;
		TArray<AFGRailroadVehicle*> Map;
		for (const TObjectPtr<AFGRailroadVehicle>& Vehicle : Seen->Vehicles)
		{
			bValid &= !Vehicle || Vehicles.Contains(Vehicle.Get());
			Map.Add(Vehicle);
		}
		if (bValid)
		{
			if (bLearned)
			{
				*bLearned = true;
			}
			Maps.Add(Map);
			return Maps;
		}
	}
	const AFGBuildableRailroadStation* Building = Station ? Station->GetStation() : nullptr;
	const int32 PlatformCount = STLayout::GetPlatforms(Building).Num();
	for (AFGLocomotive* Locomotive : STLayout::GetPossibleDockingLocomotives(Train))
	{
		TArray<AFGRailroadVehicle*> Behind = STLayout::GetVehiclesBehind(Locomotive);
		Behind.SetNum(FMath::Min(Behind.Num(), PlatformCount));
		if (!Maps.Contains(Behind))
		{
			Maps.Add(Behind);
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
		const bool bEmpty = STLayout::GetCargoType(Car) == EFreightCargoType::FCT_NONE;
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
					!bEmpty ? TEXT("already carries something") : *FString::Printf(TEXT("doesn't stop beside a demand platform for it at %s (needs Freight Car Control to skip that car)"), *STLayout::Name(To)));
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
	if (Job.bPark && Job.Legs.Num() == 1 && !Job.Legs[0].Item)
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
	if (!HasAuthority())
	{
		return;
	}
	TArray<FSTNode> Nodes;
	BuildNodes(Nodes);
	if (Nodes.Num() == 0)
	{
		return;
	}
	const double Time = Now();

	// When each station's bay is taken until, by jobs already running.
	TMap<const AFGTrainStationIdentifier*, double> BusyUntil;
	for (const FSTTrainState& State : mTrains)
	{
		if (!State.bManaged || !State.bHasJob)
		{
			continue;
		}
		for (int32 Leg = State.Job.Leg; Leg < State.Job.Legs.Num(); ++Leg)
		{
			const FSTJobLeg& Each = State.Job.Legs[Leg];
			double& Until = BusyUntil.FindOrAdd(Each.Station.Get(), 0.0);
			Until = FMath::Max(Until, Each.PlannedArrival + EstimateStaySeconds(Each.Station));
		}
	}

	TArray<FCandidate> Candidates;
	for (int32 TrainIndex = 0; TrainIndex < mTrains.Num(); ++TrainIndex)
	{
		const FSTTrainState& State = mTrains[TrainIndex];
		AFGTrainStationIdentifier* Anchor = nullptr;
		double FreeAt = 0.0;
		if (!GetFreeAt(State, Anchor, FreeAt))
		{
			continue;
		}
		const AFGTrain* Train = State.Train;
		const TMap<UClass*, int32> Cargo = GetCargo(Train);

		if (Cargo.Num() > 0)
		{
			// A loaded train only delivers what it carries.
			for (const TPair<UClass*, int32>& Carried : Cargo)
			{
				for (int32 To = 0; To < Nodes.Num(); ++To)
				{
					const FSTNode& Consumer = Nodes[To];
					if (Consumer.Role != ESTRole::Demand || (Consumer.Item.Get() != Carried.Key && !Consumer.bAnyItem) || !CanServe(State, Consumer.Station.Get()))
					{
						continue;
					}
					const float Travel = EstimateTravelSeconds(Anchor, Consumer.Station.Get());
					if (Travel < 0.f)
					{
						continue;
					}
					FCandidate Candidate;
					Candidate.TrainIndex = TrainIndex;
					Candidate.To = To;
					Candidate.Item = Carried.Key;
					Candidate.bDeliveryOnly = true;
					Candidate.Fit = ComputeDeliveryFit(Train, Consumer.Station.Get(), Carried.Key);
					if (Candidate.Fit.Capacity <= 0)
					{
						continue;
					}
					Candidate.ArriveTo = FMath::Max(FreeAt + Travel, BusyUntil.FindRef(Consumer.Station.Get()));
					const int32 Room = Consumer.bAnyItem ? GetSinkRoom(Consumer, Carried.Key)
						: Consumer.Capacity - FMath::Max(0, static_cast<int32>(Consumer.Stock + Consumer.Reserved - Consumer.Rate * (Candidate.ArriveTo - Time)));
					Candidate.Amount = FMath::Min(Candidate.Fit.Capacity, Room);
					if (Candidate.Amount <= 0)
					{
						continue;
					}
					const double Weight = Consumer.bAnyItem ? 0.1 : STNames::PriorityWeight(Consumer.Priority);
					Candidate.Score = Weight * 3.0 * Candidate.Amount / FMath::Max(1.0, Candidate.ArriveTo - Time + EstimateStaySeconds(Consumer.Station.Get()));
					Candidate.Why = FString::Printf(TEXT("deliver carried cargo, weight %.1f, %.0f s"), Weight, Candidate.ArriveTo - Time);
					Candidates.Add(MoveTemp(Candidate));
				}
			}
			continue;
		}

		for (int32 From = 0; From < Nodes.Num(); ++From)
		{
			const FSTNode& Supplier = Nodes[From];
			if (Supplier.Role != ESTRole::Supply || !CanServe(State, Supplier.Station.Get()))
			{
				continue;
			}
			const float ToSupplier = EstimateTravelSeconds(Anchor, Supplier.Station.Get());
			if (ToSupplier < 0.f)
			{
				continue;
			}
			const double ArriveFrom = FMath::Max(FreeAt + ToSupplier, BusyUntil.FindRef(Supplier.Station.Get()));
			const double SupplierStay = EstimateStaySeconds(Supplier.Station.Get());
			const double Available = FMath::Min<double>(Supplier.Capacity, Supplier.Stock + Supplier.Rate * (ArriveFrom - Time)) - Supplier.Reserved;
			const bool bSupplyFull = Supplier.Capacity > 0 && (Supplier.Stock >= 0.9 * Supplier.Capacity
				|| (Supplier.Rate > 0.0 && (Supplier.Capacity - Supplier.Stock + Supplier.Reserved) / Supplier.Rate < ArriveFrom - Time));
			for (int32 To = 0; To < Nodes.Num(); ++To)
			{
				const FSTNode& Consumer = Nodes[To];
				if (Consumer.Role != ESTRole::Demand || (Consumer.Item != Supplier.Item && !Consumer.bAnyItem) || Consumer.Station == Supplier.Station
					|| !CanServe(State, Consumer.Station.Get()))
				{
					continue;
				}
				if (Consumer.bAnyItem && !bSupplyFull)
				{
					continue;
				}
				const float Leg = EstimateTravelSeconds(Supplier.Station.Get(), Consumer.Station.Get());
				if (Leg < 0.f)
				{
					continue;
				}
				FCandidate Candidate;
				Candidate.TrainIndex = TrainIndex;
				Candidate.From = From;
				Candidate.To = To;
				Candidate.Item = Supplier.Item;
				Candidate.ArriveFrom = ArriveFrom;
				Candidate.ArriveTo = FMath::Max(ArriveFrom + SupplierStay + Leg, BusyUntil.FindRef(Consumer.Station.Get()));
				Candidate.Fit = ComputeFit(Train, Supplier.Station.Get(), Consumer.Station.Get(), Supplier.Item);
				if (Candidate.Fit.Capacity <= 0)
				{
					UE_LOG(LogSmartTrains, VeryVerbose, TEXT("NO FIT %s for %s %s -> %s: %s"), *STLayout::Name(Train), *STNames::Item(Supplier.Item),
						*STLayout::Name(Supplier.Station.Get()), *STLayout::Name(Consumer.Station.Get()), *Candidate.Fit.Problem);
					continue;
				}
				const bool bCritical = Consumer.Priority == ESTPriority::Critical;
				const double Batch = bCritical ? FMath::Min<double>(Candidate.Fit.Capacity, UFGItemDescriptor::GetStackSize(Supplier.Item))
					: Candidate.Fit.Capacity * (Supplier.SendAtPercent > 0.f ? Supplier.SendAtPercent / 100.0 : 1.0);
				if (Available < Batch)
				{
					continue;
				}
				double Projected = 0.0;
				double Target = 0.0;
				int32 Room = 0;
				if (Consumer.bAnyItem)
				{
					Room = GetSinkRoom(Consumer, Supplier.Item);
				}
				else
				{
					Projected = Consumer.Stock + Consumer.Reserved - Consumer.Rate * (Candidate.ArriveTo - Time);
					Target = Consumer.Capacity * Consumer.TargetPercent / 100.0;
					Room = Consumer.Capacity - FMath::Max(0, static_cast<int32>(Projected));
					if (Projected >= Target && !bSupplyFull)
					{
						continue;
					}
				}
				Candidate.Amount = FMath::Min3(Candidate.Fit.Capacity, static_cast<int32>(Available), Room);
				if (Candidate.Amount <= 0 || (!bCritical && Candidate.Amount < Batch * 0.5))
				{
					continue;
				}
				double Urgency = 1.0;
				if (!Consumer.bAnyItem && Target > 0.0)
				{
					Urgency += FMath::Clamp((Target - Projected) / Target, 0.0, 1.0) * 2.0;
					Urgency += (Consumer.bStarved || Projected <= 0.0) ? 2.0 : 0.0;
				}
				Urgency = FMath::Max(Urgency, bSupplyFull ? 1.5 : 1.0);
				const double* Planned = mPlannedPairs.Find(PairKey(Supplier, Consumer));
				const double PlanFactor = Consumer.bAnyItem ? 0.1 : (mPlannedPairs.Num() == 0 || (Planned && *Planned > 0.0) ? 1.0 : 0.6);
				const double Duration = FMath::Max(1.0, Candidate.ArriveTo - Time + EstimateStaySeconds(Consumer.Station.Get()));
				const double Weight = STNames::PriorityWeight(Consumer.Priority);
				Candidate.Score = Weight * Urgency * PlanFactor * Candidate.Amount / Duration;
				Candidate.Why = FString::Printf(TEXT("weight %.1f x urgency %.2f x plan %.1f x %d / %.0f s%s%s"), Weight, Urgency, PlanFactor, Candidate.Amount, Duration,
					bSupplyFull ? TEXT(", supplier nearly full") : TEXT(""), Consumer.bStarved ? TEXT(", consumer starved") : TEXT(""));
				Candidates.Add(MoveTemp(Candidate));
			}
		}
		if (mSettings.bMilkRuns)
		{
			AddMilkRuns(TrainIndex, Anchor, FreeAt, Nodes, BusyUntil, Candidates);
		}
	}

	Candidates.Sort([](const FCandidate& A, const FCandidate& B) { return A.Score > B.Score; });
	TSet<int32> Assigned;
	for (FCandidate& Candidate : Candidates)
	{
		if (Assigned.Contains(Candidate.TrainIndex))
		{
			continue;
		}
		if (Candidate.Legs.Num() > 0)
		{
			if (!RecheckMilkRun(Candidate, Nodes))
			{
				continue;
			}
			Assigned.Add(Candidate.TrainIndex);
			TArray<FString> Stops;
			for (int32 Index = 0; Index < Candidate.Legs.Num(); ++Index)
			{
				Nodes[Candidate.LegNodes[Index]].Reserved += Candidate.Legs[Index].PlannedAmount;
				Stops.Add(STLayout::Name(Candidate.Legs[Index].Station.Get()));
			}
			FSTJob Job;
			Job.Id = mNextJobId++;
			Job.StartedAt = Time;
			Job.LegStartedAt = Time;
			Job.Legs = Candidate.Legs;
			Job.Summary = FString::Printf(TEXT("%s %s"), *STNames::Item(Candidate.Item), *FString::Join(Stops, TEXT(" -> ")));
			FSTTrainState& State = mTrains[Candidate.TrainIndex];
			const FString Line = FString::Printf(TEXT("%s: %s, %s with %d cars, score %.2f (%s)"), *STLayout::Name(State.Train), *Job.Summary,
				*STNames::Amount(Candidate.Item, Candidate.Amount), Job.Legs.Last().Cars.Num(), Candidate.Score, *Candidate.Why);
			CommitJob(State, MoveTemp(Job), Line);
			continue;
		}
		FSTNode& Consumer = Nodes[Candidate.To];
		FSTNode* Supplier = Candidate.From != INDEX_NONE ? &Nodes[Candidate.From] : nullptr;
		// Earlier assignments this round may have used up the supply or the room.
		if (Supplier)
		{
			const double Available = FMath::Min<double>(Supplier->Capacity, Supplier->Stock + Supplier->Rate * (Candidate.ArriveFrom - Time)) - Supplier->Reserved;
			Candidate.Amount = FMath::Min(Candidate.Amount, static_cast<int32>(Available));
		}
		if (!Consumer.bAnyItem)
		{
			const double Projected = Consumer.Stock + Consumer.Reserved - Consumer.Rate * (Candidate.ArriveTo - Time);
			Candidate.Amount = FMath::Min(Candidate.Amount, Consumer.Capacity - FMath::Max(0, static_cast<int32>(Projected)));
		}
		if (Candidate.Amount <= 0)
		{
			continue;
		}
		Assigned.Add(Candidate.TrainIndex);
		if (Supplier)
		{
			Supplier->Reserved += Candidate.Amount;
		}
		Consumer.Reserved += Candidate.Amount;

		FSTTrainState& State = mTrains[Candidate.TrainIndex];
		FSTJob Job;
		Job.Id = mNextJobId++;
		Job.StartedAt = Time;
		Job.LegStartedAt = Time;
		if (Supplier)
		{
			FSTJobLeg& Pickup = Job.Legs.AddDefaulted_GetRef();
			Pickup.Station = Supplier->Station.Get();
			Pickup.Item = Candidate.Item;
			Pickup.bLoad = true;
			Pickup.PlannedAmount = Candidate.Amount;
			Pickup.PlannedArrival = Candidate.ArriveFrom;
			for (AFGFreightWagon* Car : Candidate.Fit.Cars)
			{
				Pickup.Cars.Add(Car);
			}
			for (AFGFreightWagon* Car : Candidate.Fit.SkipAtPickup)
			{
				Pickup.SkipCars.Add(Car);
			}
		}
		FSTJobLeg& Drop = Job.Legs.AddDefaulted_GetRef();
		Drop.Station = Consumer.Station.Get();
		Drop.Item = Candidate.Item;
		Drop.bLoad = false;
		Drop.PlannedAmount = Candidate.Amount;
		Drop.PlannedArrival = Candidate.ArriveTo;
		for (AFGFreightWagon* Car : Candidate.Fit.Cars)
		{
			Drop.Cars.Add(Car);
		}
		for (AFGFreightWagon* Car : Candidate.Fit.SkipAtDrop)
		{
			Drop.SkipCars.Add(Car);
		}
		Job.Summary = Supplier
			? FString::Printf(TEXT("%s %s -> %s"), *STNames::Item(Candidate.Item), *STLayout::Name(Supplier->Station.Get()), *STLayout::Name(Consumer.Station.Get()))
			: FString::Printf(TEXT("%s -> %s"), *STNames::Item(Candidate.Item), *STLayout::Name(Consumer.Station.Get()));
		const FString Line = FString::Printf(TEXT("%s: %s, %s with %d cars, score %.2f (%s)"), *STLayout::Name(State.Train), *Job.Summary,
			*STNames::Amount(Candidate.Item, Candidate.Amount), Candidate.Fit.Cars.Num(), Candidate.Score, *Candidate.Why);
		CommitJob(State, MoveTemp(Job), Line);
	}

	if (!mSettings.bObserveOnly)
	{
		ClearNeededBays();
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
	WriteJob(State, TEXT("new job"));
}

void ASTSubsystem::SettleIdle(FSTTrainState& State, const TCHAR* Why)
{
	if (mSettings.bObserveOnly || !State.bManaged || !IsValid(State.Train))
	{
		return;
	}
	Dispatch();
	if (State.bHasJob)
	{
		return;
	}
	AFGTrainStationIdentifier* Depot = FindDepot(State, State.LastStation.Get());
	if (Depot && Depot != State.LastStation)
	{
		SendToStation(State, Depot, Why);
		return;
	}
	// A train still at speed isn't stopped on the line: it drives on to the stop it was heading for and parks there.
	const AFGRailroadVehicle* First = State.Train->GetFirstVehicle();
	const bool bMoving = First && First->GetVelocity().Size() > 300.0;
	AFGRailroadTimeTable* TimeTable = State.Train->GetTimeTable();
	AFGTrainStationIdentifier* Heading = TimeTable && TimeTable->IsValidStop(TimeTable->GetCurrentStop()) ? TimeTable->GetStop(TimeTable->GetCurrentStop()).Station.Get() : nullptr;
	if (bMoving)
	{
		Heading = Heading ? Heading : FindNearestStation(State.Train);
		if (Heading)
		{
			SendToStation(State, Heading, *FString::Printf(TEXT("%s, still moving"), Why));
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
		if (Leg.Item && !Leg.bLoad)
		{
			Moved += Leg.MovedAmount;
			Planned += Leg.PlannedAmount;
		}
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
	Leg.PlannedArrival = Now() + FMath::Max(0.f, EstimateTravelSeconds(State.LastStation.Get(), Station));
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
		const float Time = From ? EstimateTravelSeconds(From, Config.Station) : 0.f;
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
	TSet<const AFGTrainStationIdentifier*> Needed;
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
	for (FSTTrainState& State : mTrains)
	{
		const AFGTrainStationIdentifier* Here = State.LastStation.Get();
		if (!State.bManaged || State.Phase != ESTTrainPhase::Parked || !Here || !Needed.Contains(Here) || !IsValid(State.Train))
		{
			continue;
		}
		AFGTrainStationIdentifier* Target = FindDepot(State, Here);
		if (!Target || Target == Here)
		{
			float BestTime = TNumericLimits<float>::Max();
			Target = nullptr;
			TArray<AFGTrainStationIdentifier*> Stations;
			if (AFGRailroadSubsystem* Railroad = AFGRailroadSubsystem::Get(GetWorld()))
			{
				Railroad->GetAllTrainStations(Stations);
			}
			for (AFGTrainStationIdentifier* Candidate : Stations)
			{
				if (!Candidate || Candidate == Here || Needed.Contains(Candidate) || !CanServe(State, Candidate))
				{
					continue;
				}
				const float Time = EstimateTravelSeconds(Here, Candidate);
				if (Time >= 0.f && Time < BestTime)
				{
					BestTime = Time;
					Target = Candidate;
				}
			}
		}
		if (Target)
		{
			SendToStation(State, Target, *FString::Printf(TEXT("making room at %s"), *STLayout::Name(Here)));
		}
		else
		{
			UE_LOG(LogSmartTrains, Warning, TEXT("%s is parked at %s, which a job needs, and there is nowhere to move it: add a depot (/st station depot on)"),
				*STLayout::Name(State.Train), *STLayout::Name(Here));
		}
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
	State.WrittenStations.Reset();
	for (int32 Index = State.Job.Leg; Index < State.Job.Legs.Num(); ++Index)
	{
		const FSTJobLeg& Leg = State.Job.Legs[Index];
		FTimeTableStop& Stop = Stops.AddDefaulted_GetRef();
		Stop.Station = Leg.Station;
		Stop.DockingRuleSet = MakeRules(Leg, mSettings);
		State.WrittenStations.Add(Leg.Station);
		for (AFGRailroadVehicle* Car : Leg.SkipCars)
		{
			SkipStops.Add(Stops.Num() - 1);
			SkipVehicles.Add(Car);
		}
	}
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
		Leg.Item = Item;
		Leg.bLoad = Pass == 0;
		Leg.PlannedAmount = Fit.Capacity;
		Leg.PlannedArrival = Now() + EstimateTravelSeconds(State->LastStation.Get(), From) + (Pass == 1 ? EstimateStaySeconds(From) + EstimateTravelSeconds(From, To) : 0.f);
		for (AFGFreightWagon* Car : Fit.Cars)
		{
			Leg.Cars.Add(Car);
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
					const float Time = EstimateTravelSeconds(Nodes[From].Station.Get(), Nodes[To].Station.Get());
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
	const float Back = EstimateTravelSeconds(Consumer.Station.Get(), Supplier.Station.Get());
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
