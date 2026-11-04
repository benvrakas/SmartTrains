#include "Buildables/FGBuildableRailroadStation.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGFreightWagon.h"
#include "FGInventoryComponent.h"
#include "FGTrain.h"
#include "FGTrainStationIdentifier.h"
#include "Resources/FGItemDescriptor.h"
#include "SmartTrains.h"
#include "STLayout.h"
#include "STSubsystem.h"

// Milk runs: two pickups that together fill one train, or one load split between two consumers (Docs/Architecture.md,
// "Milk runs").

namespace
{
	// Score factor of a milk run against a direct job of the same worth.
	constexpr double MilkRunFactor = 0.9;

	struct FDemandAt
	{
		double Projected = 0.0;
		double Target = 0.0;
		int32 Room = 0;
		double Urgency = 1.0;
	};

	FDemandAt GetDemandAt(const FSTNode& Consumer, double Arrive, double Time)
	{
		FDemandAt Out;
		Out.Projected = Consumer.Stock + Consumer.Reserved - Consumer.Rate * (Arrive - Time);
		Out.Target = Consumer.Capacity * Consumer.TargetPercent / 100.0;
		Out.Room = Consumer.Capacity - FMath::Max(0, static_cast<int32>(Out.Projected));
		if (Out.Target > 0.0)
		{
			Out.Urgency += FMath::Clamp((Out.Target - Out.Projected) / Out.Target, 0.0, 1.0) * 2.0;
			Out.Urgency += (Consumer.bStarved || Out.Projected <= 0.0) ? 2.0 : 0.0;
		}
		return Out;
	}

	double GetAvailableAt(const FSTNode& Supplier, double Arrive, double Time)
	{
		return FMath::Min<double>(Supplier.Capacity, Supplier.Stock + Supplier.Rate * (Arrive - Time)) - Supplier.Reserved;
	}

	double GetSendAtShare(const FSTNode& Supplier)
	{
		return Supplier.SendAtPercent > 0.f ? Supplier.SendAtPercent / 100.0 : 1.0;
	}

	void AddCars(TArray<TObjectPtr<AFGRailroadVehicle>>& Out, const TArray<AFGFreightWagon*>& Cars)
	{
		for (AFGFreightWagon* Car : Cars)
		{
			Out.AddUnique(Car);
		}
	}
}

FSTFit ASTSubsystem::CheckDropOnTheWay(const AFGTrain* Train, const AFGTrainStationIdentifier* Station, TSubclassOf<UFGItemDescriptor> Item,
	const TArray<AFGFreightWagon*>& Cars) const
{
	FSTFit Fit;
	const AFGBuildableRailroadStation* Building = Station ? Station->GetStation() : nullptr;
	const TArray<TArray<AFGRailroadVehicle*>> Maps = GetDockMaps(Train, Station);
	if (!Building || Maps.Num() == 0)
	{
		Fit.Problem = TEXT("the train has no locomotive that can dock");
		return Fit;
	}
	const TArray<AFGBuildableTrainPlatform*> Platforms = STLayout::GetPlatforms(Building);
	const bool bFcc = IsFccAvailable();
	for (AFGFreightWagon* Car : Cars)
	{
		bool bAlwaysDemands = true;
		bool bMayUnload = false;
		for (const TArray<AFGRailroadVehicle*>& Map : Maps)
		{
			const int32 Index = Map.IndexOfByKey(Car);
			const AFGBuildableTrainPlatformCargo* Platform = Platforms.IsValidIndex(Index) ? Cast<AFGBuildableTrainPlatformCargo>(Platforms[Index]) : nullptr;
			const FSTPlatformConfig* Config = Platform ? FindPlatformConfig(Platform) : nullptr;
			const bool bTypeOk = Platform && STLayout::ItemFitsCargoType(Item, STLayout::GetPlatformCargoType(Platform));
			const bool bDemands = bTypeOk && Config && Config->Role == ESTRole::Demand && Config->FindRule(Item);
			bAlwaysDemands &= bDemands;
			bMayUnload |= bTypeOk && !bDemands && !Platform->GetIsInLoadMode();
		}
		if (bAlwaysDemands)
		{
			Fit.Cars.Add(Car);
			Fit.Capacity += STLayout::GetCapacity(Car->GetFreightInventory(), Item);
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
					*STNames::Item(Item), *STLayout::Name(Station));
			}
		}
	}
	if (Fit.Cars.Num() == 0 && Fit.Problem.IsEmpty())
	{
		Fit.Problem = FString::Printf(TEXT("no car stops beside a demand platform for %s at %s"), *STNames::Item(Item), *STLayout::Name(Station));
	}
	return Fit;
}

void ASTSubsystem::AddMilkRuns(int32 TrainIndex, const AFGTrainStationIdentifier* Anchor, double FreeAt, const TArray<FSTNode>& Nodes,
	const TMap<const AFGTrainStationIdentifier*, double>& BusyUntil, TArray<FCandidate>& OutCandidates) const
{
	const FSTTrainState& State = mTrains[TrainIndex];
	const AFGTrain* Train = State.Train;
	const double Time = Now();
	int32 Added = 0;

	TMap<TPair<int32, int32>, FSTFit> Fits;
	auto GetFit = [&](int32 From, int32 To) -> FSTFit
	{
		const TPair<int32, int32> Key(From, To);
		if (const FSTFit* Found = Fits.Find(Key))
		{
			return *Found;
		}
		return Fits.Add(Key, ComputeFit(Train, Nodes[From].Station.Get(), Nodes[To].Station.Get(), Nodes[From].Item));
	};
	auto Busy = [&BusyUntil](const FSTNode& Node) { return BusyUntil.FindRef(Node.Station.Get()); };

	TArray<int32> Suppliers;
	TArray<int32> Consumers;
	for (int32 Index = 0; Index < Nodes.Num(); ++Index)
	{
		const FSTNode& Node = Nodes[Index];
		if (!Node.Item || !CanServe(State, Node.Station.Get()))
		{
			continue;
		}
		if (Node.Role == ESTRole::Supply)
		{
			Suppliers.Add(Index);
		}
		else if (Node.Role == ESTRole::Demand && !Node.bAnyItem)
		{
			Consumers.Add(Index);
		}
	}

	// Two pickups, then one delivery: neither supplier has a batch alone, both together do.
	for (const int32 To : Consumers)
	{
		const FSTNode& Consumer = Nodes[To];
		for (const int32 First : Suppliers)
		{
			for (const int32 Second : Suppliers)
			{
				const FSTNode& A = Nodes[First];
				const FSTNode& B = Nodes[Second];
				if (First == Second || A.Item != Consumer.Item || B.Item != Consumer.Item || A.Station == B.Station
					|| A.Station == Consumer.Station || B.Station == Consumer.Station)
				{
					continue;
				}
				const float ToA = EstimateTravelSeconds(Anchor, A.Station.Get());
				const float AToB = EstimateTravelSeconds(A.Station.Get(), B.Station.Get());
				const float BToC = EstimateTravelSeconds(B.Station.Get(), Consumer.Station.Get());
				if (ToA < 0.f || AToB < 0.f || BToC < 0.f)
				{
					continue;
				}
				const FSTFit FitA = GetFit(First, To);
				const FSTFit FitB = GetFit(Second, To);
				if (FitA.Capacity <= 0 || FitB.Capacity <= 0)
				{
					continue;
				}
				const double ArriveA = FMath::Max(FreeAt + ToA, Busy(A));
				const double ArriveB = FMath::Max(ArriveA + EstimateStaySeconds(A.Station.Get()) + AToB, Busy(B));
				const double ArriveC = FMath::Max(ArriveB + EstimateStaySeconds(B.Station.Get()) + BToC, Busy(Consumer));
				const double AvailableA = GetAvailableAt(A, ArriveA, Time);
				const double AvailableB = GetAvailableAt(B, ArriveB, Time);
				// A supplier with a batch of its own gets a direct job instead.
				if (AvailableA <= 0.0 || AvailableB <= 0.0 || AvailableA >= FitA.Capacity * GetSendAtShare(A) || AvailableB >= FitB.Capacity * GetSendAtShare(B))
				{
					continue;
				}
				TArray<TObjectPtr<AFGRailroadVehicle>> Cars;
				AddCars(Cars, FitA.Cars);
				AddCars(Cars, FitB.Cars);
				int32 Capacity = 0;
				for (AFGRailroadVehicle* Car : Cars)
				{
					const AFGFreightWagon* Wagon = Cast<AFGFreightWagon>(Car);
					Capacity += Wagon ? STLayout::GetCapacity(Wagon->GetFreightInventory(), Consumer.Item) : 0;
				}
				const bool bCritical = Consumer.Priority == ESTPriority::Critical;
				const double Batch = bCritical ? FMath::Min<double>(Capacity, UFGItemDescriptor::GetStackSize(Consumer.Item))
					: Capacity * FMath::Max(GetSendAtShare(A), GetSendAtShare(B));
				if (AvailableA + AvailableB < Batch)
				{
					continue;
				}
				const FDemandAt Demand = GetDemandAt(Consumer, ArriveC, Time);
				if (Demand.Projected >= Demand.Target)
				{
					continue;
				}
				const int32 Amount = FMath::Min3(Capacity, static_cast<int32>(AvailableA + AvailableB), Demand.Room);
				if (Amount <= 0 || (!bCritical && Amount < Batch * 0.5))
				{
					continue;
				}

				FCandidate Candidate;
				Candidate.TrainIndex = TrainIndex;
				Candidate.From = First;
				Candidate.To = To;
				Candidate.Item = Consumer.Item;
				Candidate.Amount = Amount;
				Candidate.ArriveFrom = ArriveA;
				Candidate.ArriveTo = ArriveC;
				Candidate.Fit = FitA;
				FSTJobLeg& PickA = Candidate.Legs.AddDefaulted_GetRef();
				PickA.Station = A.Station.Get();
				PickA.Item = Consumer.Item;
				PickA.bLoad = true;
				PickA.bPartial = true;
				PickA.PlannedAmount = FMath::Min(static_cast<int32>(AvailableA), Amount);
				PickA.PlannedArrival = ArriveA;
				AddCars(PickA.Cars, FitA.Cars);
				AddCars(PickA.SkipCars, FitA.SkipAtPickup);
				FSTJobLeg& PickB = Candidate.Legs.AddDefaulted_GetRef();
				PickB.Station = B.Station.Get();
				PickB.Item = Consumer.Item;
				PickB.bLoad = true;
				PickB.PlannedAmount = Amount - PickA.PlannedAmount;
				PickB.PlannedArrival = ArriveB;
				AddCars(PickB.Cars, FitB.Cars);
				AddCars(PickB.SkipCars, FitB.SkipAtPickup);
				FSTJobLeg& Drop = Candidate.Legs.AddDefaulted_GetRef();
				Drop.Station = Consumer.Station.Get();
				Drop.Item = Consumer.Item;
				Drop.PlannedAmount = Amount;
				Drop.PlannedArrival = ArriveC;
				Drop.Cars = Cars;
				Candidate.LegNodes = {First, Second, To};

				const double Duration = FMath::Max(1.0, ArriveC - Time + EstimateStaySeconds(Consumer.Station.Get()));
				const double Weight = STNames::PriorityWeight(Consumer.Priority);
				Candidate.Score = MilkRunFactor * Weight * Demand.Urgency * Amount / Duration;
				Candidate.Why = FString::Printf(TEXT("milk run, two pickups: weight %.1f x urgency %.2f x %d / %.0f s"), Weight, Demand.Urgency, Amount, Duration);
				OutCandidates.Add(MoveTemp(Candidate));
				++Added;
			}
		}
	}

	// One pickup, then two deliveries: neither consumer has room for the load alone, both together do.
	for (const int32 From : Suppliers)
	{
		const FSTNode& Supplier = Nodes[From];
		const float ToS = EstimateTravelSeconds(Anchor, Supplier.Station.Get());
		if (ToS < 0.f)
		{
			continue;
		}
		const double ArriveS = FMath::Max(FreeAt + ToS, Busy(Supplier));
		const double Available = GetAvailableAt(Supplier, ArriveS, Time);
		if (Available <= 0.0)
		{
			continue;
		}
		for (const int32 First : Consumers)
		{
			for (const int32 Last : Consumers)
			{
				const FSTNode& A = Nodes[First];
				const FSTNode& B = Nodes[Last];
				if (First == Last || A.Item != Supplier.Item || B.Item != Supplier.Item || A.Station == B.Station
					|| A.Station == Supplier.Station || B.Station == Supplier.Station)
				{
					continue;
				}
				const float SToA = EstimateTravelSeconds(Supplier.Station.Get(), A.Station.Get());
				const float AToB = EstimateTravelSeconds(A.Station.Get(), B.Station.Get());
				if (SToA < 0.f || AToB < 0.f)
				{
					continue;
				}
				// Every loaded car must be able to unload at the last stop; the first stop takes what its platforms can.
				const FSTFit FitB = GetFit(From, Last);
				if (FitB.Capacity <= 0)
				{
					continue;
				}
				const FSTFit OnTheWay = CheckDropOnTheWay(Train, A.Station.Get(), Supplier.Item, FitB.Cars);
				if (!OnTheWay.Problem.IsEmpty())
				{
					continue;
				}
				const bool bCritical = A.Priority == ESTPriority::Critical || B.Priority == ESTPriority::Critical;
				const double Batch = bCritical ? FMath::Min<double>(FitB.Capacity, UFGItemDescriptor::GetStackSize(Supplier.Item))
					: FitB.Capacity * GetSendAtShare(Supplier);
				if (Available < Batch)
				{
					continue;
				}
				const double ArriveA = FMath::Max(ArriveS + EstimateStaySeconds(Supplier.Station.Get()) + SToA, Busy(A));
				const double ArriveB = FMath::Max(ArriveA + EstimateStaySeconds(A.Station.Get()) + AToB, Busy(B));
				const FDemandAt DemandA = GetDemandAt(A, ArriveA, Time);
				const FDemandAt DemandB = GetDemandAt(B, ArriveB, Time);
				const int32 Load = FMath::Min(FitB.Capacity, static_cast<int32>(Available));
				// Both must want it, and a direct job to either can't take the whole load.
				if (DemandA.Projected >= DemandA.Target || DemandB.Projected >= DemandB.Target || DemandA.Room <= 0 || DemandB.Room <= 0
					|| FMath::Max(DemandA.Room, DemandB.Room) >= Load)
				{
					continue;
				}
				const int32 AmountA = FMath::Min3(DemandA.Room, OnTheWay.Capacity, Load);
				const int32 Amount = FMath::Min(Load, AmountA + DemandB.Room);
				if (Amount <= 0 || (!bCritical && Amount < Batch * 0.5))
				{
					continue;
				}

				FCandidate Candidate;
				Candidate.TrainIndex = TrainIndex;
				Candidate.From = From;
				Candidate.To = Last;
				Candidate.Item = Supplier.Item;
				Candidate.Amount = Amount;
				Candidate.ArriveFrom = ArriveS;
				Candidate.ArriveTo = ArriveB;
				Candidate.Fit = FitB;
				FSTJobLeg& Pick = Candidate.Legs.AddDefaulted_GetRef();
				Pick.Station = Supplier.Station.Get();
				Pick.Item = Supplier.Item;
				Pick.bLoad = true;
				Pick.PlannedAmount = Amount;
				Pick.PlannedArrival = ArriveS;
				AddCars(Pick.Cars, FitB.Cars);
				AddCars(Pick.SkipCars, FitB.SkipAtPickup);
				FSTJobLeg& DropA = Candidate.Legs.AddDefaulted_GetRef();
				DropA.Station = A.Station.Get();
				DropA.Item = Supplier.Item;
				DropA.bPartial = true;
				DropA.PlannedAmount = FMath::Min(AmountA, Amount);
				DropA.PlannedArrival = ArriveA;
				AddCars(DropA.Cars, OnTheWay.Cars);
				AddCars(DropA.SkipCars, OnTheWay.SkipAtDrop);
				FSTJobLeg& DropB = Candidate.Legs.AddDefaulted_GetRef();
				DropB.Station = B.Station.Get();
				DropB.Item = Supplier.Item;
				DropB.PlannedAmount = Amount - DropA.PlannedAmount;
				DropB.PlannedArrival = ArriveB;
				AddCars(DropB.Cars, FitB.Cars);
				AddCars(DropB.SkipCars, FitB.SkipAtDrop);
				Candidate.LegNodes = {From, First, Last};

				const double Duration = FMath::Max(1.0, ArriveB - Time + EstimateStaySeconds(B.Station.Get()));
				const double WeightA = STNames::PriorityWeight(A.Priority);
				const double WeightB = STNames::PriorityWeight(B.Priority);
				const double Worth = WeightA * DemandA.Urgency * DropA.PlannedAmount + WeightB * DemandB.Urgency * DropB.PlannedAmount;
				Candidate.Score = MilkRunFactor * Worth / Duration;
				Candidate.Why = FString::Printf(TEXT("milk run, two deliveries: %d + %d, worth %.0f / %.0f s"), DropA.PlannedAmount, DropB.PlannedAmount, Worth, Duration);
				OutCandidates.Add(MoveTemp(Candidate));
				++Added;
			}
		}
	}
	if (Added > 0)
	{
		UE_LOG(LogSmartTrains, Verbose, TEXT("%s: %d milk run candidates"), *STLayout::Name(Train), Added);
	}
}

bool ASTSubsystem::RecheckMilkRun(FCandidate& Candidate, const TArray<FSTNode>& Nodes) const
{
	const double Time = Now();
	int32 Pickups = 0;
	int32 Drops = 0;
	TArray<int32> Limits;
	for (int32 Index = 0; Index < Candidate.Legs.Num(); ++Index)
	{
		const FSTJobLeg& Leg = Candidate.Legs[Index];
		const FSTNode& Node = Nodes[Candidate.LegNodes[Index]];
		int32 Limit = Leg.PlannedAmount;
		if (Leg.bLoad)
		{
			Limit = FMath::Min(Limit, static_cast<int32>(GetAvailableAt(Node, Leg.PlannedArrival, Time)));
			Pickups += FMath::Max(0, Limit);
		}
		else
		{
			Limit = FMath::Min(Limit, GetDemandAt(Node, Leg.PlannedArrival, Time).Room);
			Drops += FMath::Max(0, Limit);
		}
		Limits.Add(FMath::Max(0, Limit));
	}
	const int32 Amount = FMath::Min(Pickups, Drops);
	if (Amount <= 0 || Amount < Candidate.Amount / 2)
	{
		return false;
	}
	// Spreads Amount over the pickups and over the drops in order; the last of each takes the rest.
	int32 LeftToLoad = Amount;
	int32 LeftToDrop = Amount;
	int32 LastLoad = INDEX_NONE;
	int32 LastDrop = INDEX_NONE;
	for (int32 Index = 0; Index < Candidate.Legs.Num(); ++Index)
	{
		FSTJobLeg& Leg = Candidate.Legs[Index];
		int32& Left = Leg.bLoad ? LeftToLoad : LeftToDrop;
		Leg.PlannedAmount = FMath::Min(Limits[Index], Left);
		Left -= Leg.PlannedAmount;
		(Leg.bLoad ? LastLoad : LastDrop) = Index;
	}
	if (LastLoad != INDEX_NONE)
	{
		Candidate.Legs[LastLoad].PlannedAmount += LeftToLoad;
	}
	if (LastDrop != INDEX_NONE)
	{
		Candidate.Legs[LastDrop].PlannedAmount += LeftToDrop;
	}
	Candidate.Amount = Amount;
	return true;
}
