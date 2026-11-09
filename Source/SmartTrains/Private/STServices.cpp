// Services: loops made from the flow plan, with trains assigned and run as looping timetables (Docs/Architecture.md,
// "Services").

#include <algorithm>
#include "Algo/Sort.h"
#include "Buildables/FGBuildableRailroadStation.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGFreightWagon.h"
#include "FGTrain.h"
#include "FGTrainStationIdentifier.h"
#include "SmartTrains.h"
#include "STForecast.h"
#include "STLayout.h"
#include "STSubsystem.h"
#include "STTripPlanner.h"

namespace
{
	// A service loop has at most this many stops and lasts at most this long (merging stops there). [stops, s]
	constexpr int32 ServiceMaxStops = 8;
	constexpr double ServiceMaxSeconds = 3600.0;
	// Two loops merge into one when that saves at least this much of a train.
	constexpr double ServiceMergeSaving = 0.25;
	// A service whose flows are gone this many flow plans in a row ends.
	constexpr int32 ServiceRetirePlans = 3;
	// A service gets ceil(needed - this) trains, so a need just over a whole number doesn't take another train.
	constexpr double ServiceTrainSlack = 0.15;
	// Most pairs tried per merge round, and merge rounds per design.
	constexpr int32 ServiceMergePairs = 400;
	constexpr int32 ServiceMergeRounds = 20;
	// A cycle leaves its first stop at most this much earlier than its spacing asks. [s]
	constexpr double ServiceSpacingSlack = 5.0;
	// The way into a stop is jammed when Auto Trains has it booked this full with trains already waiting this long;
	// a service waits for it unless its last train left more than ServiceJamSpacings spacings ago. [load, s, spacings]
	constexpr float ServiceJamLoad = 1.f;
	constexpr float ServiceJamWaitSeconds = 60.f;
	constexpr double ServiceJamSpacings = 3.0;

	FString ServiceKey(const TArray<FSTServiceStop>& Stops)
	{
		FString Key;
		for (const FSTServiceStop& Stop : Stops)
		{
			Key += FString::Printf(TEXT("%p:"), Stop.Station.Get());
			for (const TSubclassOf<UFGItemDescriptor>& Item : Stop.LoadItems)
			{
				Key += FString::Printf(TEXT("+%p"), Item.Get());
			}
			for (const TSubclassOf<UFGItemDescriptor>& Item : Stop.UnloadItems)
			{
				Key += FString::Printf(TEXT("-%p"), Item.Get());
			}
			Key += TEXT("|");
		}
		return Key;
	}

	int32 FindStop(const TArray<FSTServiceStop>& Stops, const AFGTrainStationIdentifier* Station)
	{
		return Stops.IndexOfByPredicate([Station](const FSTServiceStop& Stop) { return Stop.Station.Get() == Station; });
	}
}

FString ASTSubsystem::DescribeService(const FSTService& Service) const
{
	TArray<FString> Items;
	TArray<FString> From;
	TArray<FString> To;
	for (const FSTServiceFlow& Flow : Service.Flows)
	{
		Items.AddUnique(STNames::Item(Flow.Item));
		From.AddUnique(STLayout::Name(Flow.From.Get()));
		To.AddUnique(STLayout::Name(Flow.To.Get()));
	}
	auto Short = [](const TArray<FString>& Names)
	{
		return Names.Num() <= 2 ? FString::Join(Names, TEXT(", ")) : FString::Printf(TEXT("%s, %s and %d more"), *Names[0], *Names[1], Names.Num() - 2);
	};
	return FString::Printf(TEXT("S%d %s: %s -> %s"), Service.Id, *Short(Items), *Short(From), *Short(To));
}

double ASTSubsystem::GetServiceSpacing(const FSTService& Service) const
{
	if (Service.SpacingMinutes > 0.f)
	{
		return Service.SpacingMinutes * 60.0;
	}
	return Service.CycleSeconds / FMath::Max(1, Service.Trains.Num());
}

FSTService* ASTSubsystem::FindService(int32 Id)
{
	return Id > 0 ? mNetworkServices.FindByPredicate([Id](const FSTService& Each) { return Each.Id == Id; }) : nullptr;
}

const FSTService* ASTSubsystem::FindService(int32 Id) const
{
	return Id > 0 ? mNetworkServices.FindByPredicate([Id](const FSTService& Each) { return Each.Id == Id; }) : nullptr;
}

double ASTSubsystem::GetLoopSeconds(const TArray<FSTServiceStop>& Stops, bool bPushPull) const
{
	double Seconds = 0.0;
	for (int32 Index = 0; Index < Stops.Num(); ++Index)
	{
		const AFGTrainStationIdentifier* From = Stops[Index].Station.Get();
		const AFGTrainStationIdentifier* To = Stops[(Index + 1) % Stops.Num()].Station.Get();
		const float Travel = From == To ? 0.f : EstimateTravelSeconds(From, To, bPushPull);
		if (Travel < 0.f)
		{
			return -1.0;
		}
		Seconds += Travel + EstimateStaySeconds(From);
	}
	return Seconds;
}

bool ASTSubsystem::RateLoop(FSTTripPlanner& Planner, int32 TrainIndex, TArray<FSTServiceStop>& Stops, TArray<FSTServiceFlow>& Flows, double& OutCycle,
	double& OutNeeded) const
{
	const FSTTrainState& State = mTrains[TrainIndex];
	if (Stops.Num() < 2 || Stops.Num() > ServiceMaxStops || !Planner.PrepareCars(TrainIndex, Stops[0].Station.Get(), Now(), false))
	{
		return false;
	}
	OutCycle = GetLoopSeconds(Stops, IsPushPull(State.Train));
	if (OutCycle <= 0.0 || OutCycle > ServiceMaxSeconds)
	{
		return false;
	}
	TArray<FSTPlanStop> PlanStops;
	for (const FSTServiceStop& Stop : Stops)
	{
		FSTPlanStop& Each = PlanStops.AddDefaulted_GetRef();
		Each.Station = Stop.Station.Get();
		for (const TSubclassOf<UFGItemDescriptor>& Item : Stop.LoadItems)
		{
			Each.LoadItems.Add(Item.Get());
		}
		for (const TSubclassOf<UFGItemDescriptor>& Item : Stop.UnloadItems)
		{
			Each.UnloadItems.Add(Item.Get());
		}
	}
	TArray<FSTTripPlanner::FFlowRef> Refs;
	for (const FSTServiceFlow& Flow : Flows)
	{
		Refs.Add({Flow.From.Get(), Flow.To.Get(), Flow.Item.Get(), Flow.Flow});
	}
	const TArray<double> PerTrain = Planner.AllocateServiceCars(PlanStops, Refs);
	OutNeeded = 0.0;
	for (int32 Index = 0; Index < Flows.Num(); ++Index)
	{
		Flows[Index].PerTrain = PerTrain.IsValidIndex(Index) ? PerTrain[Index] : 0.0;
		if (Flows[Index].PerTrain <= 0.0)
		{
			return false;
		}
		OutNeeded = FMath::Max(OutNeeded, Flows[Index].Flow * OutCycle / Flows[Index].PerTrain);
	}
	return true;
}

void ASTSubsystem::DesignServices(TArray<FSTRosterRoute>&& Routes)
{
	const double Started = FPlatformTime::Seconds();
	// Car layouts in the fleet, with a train of each to check loops with.
	TMap<FString, TArray<int32>> Layouts;
	for (int32 Index = 0; Index < mTrains.Num(); ++Index)
	{
		const FSTTrainState& State = mTrains[Index];
		if (State.bManaged && !State.bKeepVanilla && IsValid(State.Train))
		{
			Layouts.FindOrAdd(STLayout::GetLayoutKey(State.Train)).Add(Index);
		}
	}
	TArray<FSTNode> Nodes = mPlannedNodes;
	TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> NoBookings;
	FSTTripPlanner Planner(*this, Nodes, NoBookings);
	Planner.RaiseBudget(200.0);
	auto RouteKey = [](const AFGTrainStationIdentifier* From, const AFGTrainStationIdentifier* To, const UClass* Item)
	{
		return FSTRosterRoute::MakeKey(From, To, Item);
	};
	TMap<FString, const FSTRosterRoute*> RouteByKey;
	for (const FSTRosterRoute& Route : Routes)
	{
		RouteByKey.Add(Route.GetKey(), &Route);
	}

	// Services that exist keep their loops; their flows follow the flow plan, and one whose flows stay gone ends.
	TSet<FString> Covered;
	for (int32 Index = mNetworkServices.Num() - 1; Index >= 0; --Index)
	{
		FSTService& Service = mNetworkServices[Index];
		double Total = 0.0;
		for (FSTServiceFlow& Flow : Service.Flows)
		{
			const FString Key = RouteKey(Flow.From.Get(), Flow.To.Get(), Flow.Item.Get());
			const FSTRosterRoute* const* Route = RouteByKey.Find(Key);
			Flow.Flow = Route ? (*Route)->Flow : 0.0;
			Total += Flow.Flow;
			Covered.Add(Key);
		}
		const TArray<int32>* Trains = Layouts.Find(Service.Layout);
		Service.MissingPlans = Total > 0.0 || Service.bLocked ? 0 : Service.MissingPlans + 1;
		const bool bStationGone = Service.Stops.ContainsByPredicate([](const FSTServiceStop& Stop) { return !IsValid(Stop.Station) || !Stop.Station->GetStation(); });
		if (!Trains || Service.MissingPlans > ServiceRetirePlans || bStationGone)
		{
			UE_LOG(LogSmartTrains, Log, TEXT("SERVICE %s retired: %s"), *DescribeService(Service), bStationGone ? TEXT("a station of its loop is gone")
				: (!Trains ? TEXT("no managed train of its car layout") : TEXT("its flows are gone")));
			for (const FSTServiceFlow& Flow : Service.Flows)
			{
				Covered.Remove(RouteKey(Flow.From.Get(), Flow.To.Get(), Flow.Item.Get()));
			}
			for (const TObjectPtr<AFGTrain>& Train : Service.Trains)
			{
				if (FSTTrainState* State = FindTrainState(Train))
				{
					State->ServiceId = 0;
				}
			}
			mNetworkServices.RemoveAt(Index);
			continue;
		}
		double Cycle = 0.0;
		double Needed = 0.0;
		if (RateLoop(Planner, (*Trains)[0], Service.Stops, Service.Flows, Cycle, Needed))
		{
			Service.CycleSeconds = Cycle;
			Service.Needed = Needed;
		}
	}

	// Flows no service carries: a gathering loop per consumer station (its suppliers, then it), on the car layout that
	// carries most of them.
	TMap<const AFGTrainStationIdentifier*, TArray<const FSTRosterRoute*>> ByConsumer;
	for (const FSTRosterRoute& Route : Routes)
	{
		if (!Covered.Contains(Route.GetKey()) && Route.From.IsValid() && Route.To.IsValid() && Route.Item.IsValid())
		{
			ByConsumer.FindOrAdd(Route.To.Get()).Add(&Route);
		}
	}
	struct FCandidate
	{
		FSTService Service;
		bool bExisting = false;
	};
	TArray<FCandidate> Candidates;
	mServiceUncovered.Reset();
	for (const TPair<const AFGTrainStationIdentifier*, TArray<const FSTRosterRoute*>>& Pair : ByConsumer)
	{
		TArray<const FSTRosterRoute*> Left = Pair.Value;
		for (int32 Pass = 0; Pass < 3 && Left.Num() > 0; ++Pass)
		{
			// Suppliers in the order that makes the shortest loop back to the consumer (all orders up to 5 of them).
			TArray<const AFGTrainStationIdentifier*> Suppliers;
			for (const FSTRosterRoute* Route : Left)
			{
				Suppliers.AddUnique(Route->From.Get());
			}
			Suppliers.SetNum(FMath::Min(Suppliers.Num(), 5));
			FCandidate Best;
			double BestCovered = 0.0;
			for (const TPair<FString, TArray<int32>>& Layout : Layouts)
			{
				const int32 TrainIndex = Layout.Value[0];
				const bool bPushPull = IsPushPull(mTrains[TrainIndex].Train);
				TArray<const AFGTrainStationIdentifier*> Order = Suppliers;
				Algo::Sort(Order);
				TArray<const AFGTrainStationIdentifier*> BestOrder;
				double BestLoop = TNumericLimits<double>::Max();
				do
				{
					TArray<FSTServiceStop> Trial;
					for (const AFGTrainStationIdentifier* Supplier : Order)
					{
						Trial.AddDefaulted_GetRef().Station = const_cast<AFGTrainStationIdentifier*>(Supplier);
					}
					Trial.AddDefaulted_GetRef().Station = const_cast<AFGTrainStationIdentifier*>(Pair.Key);
					const double Loop = GetLoopSeconds(Trial, bPushPull);
					if (Loop > 0.0 && Loop < BestLoop)
					{
						BestLoop = Loop;
						BestOrder = Order;
					}
				}
				while (std::next_permutation(Order.GetData(), Order.GetData() + Order.Num()));
				if (BestOrder.Num() == 0)
				{
					continue;
				}
				FCandidate Candidate;
				FSTService& Service = Candidate.Service;
				Service.Layout = Layout.Key;
				for (const AFGTrainStationIdentifier* Supplier : BestOrder)
				{
					Service.Stops.AddDefaulted_GetRef().Station = const_cast<AFGTrainStationIdentifier*>(Supplier);
				}
				FSTServiceStop& Drop = Service.Stops.AddDefaulted_GetRef();
				Drop.Station = const_cast<AFGTrainStationIdentifier*>(Pair.Key);
				for (const FSTRosterRoute* Route : Left)
				{
					const int32 Pick = FindStop(Service.Stops, Route->From.Get());
					if (Pick == INDEX_NONE)
					{
						continue;
					}
					Service.Stops[Pick].LoadItems.AddUnique(Route->Item.Get());
					Drop.UnloadItems.AddUnique(Route->Item.Get());
					FSTServiceFlow& Flow = Service.Flows.AddDefaulted_GetRef();
					Flow.From = Route->From.Get();
					Flow.To = Route->To.Get();
					Flow.Item = Route->Item.Get();
					Flow.Flow = Route->Flow;
				}
				// Keep only the flows this layout can carry, then rate the loop again.
				double Cycle = 0.0;
				double Needed = 0.0;
				RateLoop(Planner, TrainIndex, Service.Stops, Service.Flows, Cycle, Needed);
				Service.Flows.RemoveAll([](const FSTServiceFlow& Flow) { return Flow.PerTrain <= 0.0; });
				for (FSTServiceStop& Stop : Service.Stops)
				{
					Stop.LoadItems.RemoveAll([&](const TSubclassOf<UFGItemDescriptor>& Item)
					{
						return !Service.Flows.ContainsByPredicate([&](const FSTServiceFlow& Flow) { return Flow.Item == Item && Flow.From.Get() == Stop.Station.Get(); });
					});
					Stop.UnloadItems.RemoveAll([&](const TSubclassOf<UFGItemDescriptor>& Item)
					{
						return !Service.Flows.ContainsByPredicate([&](const FSTServiceFlow& Flow) { return Flow.Item == Item && Flow.To.Get() == Stop.Station.Get(); });
					});
				}
				Service.Stops.RemoveAll([](const FSTServiceStop& Stop) { return Stop.LoadItems.Num() == 0 && Stop.UnloadItems.Num() == 0; });
				if (Service.Flows.Num() == 0 || !RateLoop(Planner, TrainIndex, Service.Stops, Service.Flows, Cycle, Needed))
				{
					continue;
				}
				Service.CycleSeconds = Cycle;
				Service.Needed = Needed;
				double Carried = 0.0;
				for (const FSTServiceFlow& Flow : Service.Flows)
				{
					Carried += Flow.Flow;
				}
				if (Carried > BestCovered + 1e-9 || (FMath::IsNearlyEqual(Carried, BestCovered) && Needed < Best.Service.Needed))
				{
					BestCovered = Carried;
					Best = MoveTemp(Candidate);
				}
			}
			if (Best.Service.Flows.Num() == 0)
			{
				for (const FSTRosterRoute* Route : Left)
				{
					mServiceUncovered.Add(FString::Printf(TEXT("%s %s -> %s: no car layout in the fleet lines up with its platforms at both ends, or the loop is too long"),
						*STNames::Item(Route->Item.Get()), *STLayout::Name(Route->From.Get()), *STLayout::Name(Route->To.Get())));
				}
				break;
			}
			Left.RemoveAll([&](const FSTRosterRoute* Route)
			{
				return Best.Service.Flows.ContainsByPredicate([&](const FSTServiceFlow& Flow)
				{
					return Flow.From.Get() == Route->From.Get() && Flow.To.Get() == Route->To.Get() && Flow.Item.Get() == Route->Item.Get();
				});
			});
			Candidates.Add(MoveTemp(Best));
		}
	}

	// Loops of the same car layout merge where one loop needs clearly fewer trains than two (a train carrying
	// something both ways instead of running back empty). Existing services may take new loops in, unless locked.
	for (FSTService& Service : mNetworkServices)
	{
		if (!Service.bLocked)
		{
			FCandidate& Existing = Candidates.AddDefaulted_GetRef();
			Existing.Service = Service;
			Existing.bExisting = true;
		}
	}
	auto IsDedicated = [this](const FSTService& Service)
	{
		return Service.Flows.ContainsByPredicate([this](const FSTServiceFlow& Flow)
		{
			const FSTServiceConfig* Config = FindServiceConfig(Flow.Item.Get());
			return Config && Config->bDedicated;
		});
	};
	for (int32 Round = 0; Round < ServiceMergeRounds && FPlatformTime::Seconds() - Started < 0.15; ++Round)
	{
		int32 BestA = INDEX_NONE;
		int32 BestB = INDEX_NONE;
		double BestSaving = ServiceMergeSaving;
		FSTService BestMerged;
		int32 Tried = 0;
		for (int32 A = 0; A < Candidates.Num() && Tried < ServiceMergePairs; ++A)
		{
			for (int32 B = A + 1; B < Candidates.Num() && Tried < ServiceMergePairs; ++B)
			{
				const FSTService& First = Candidates[A].Service;
				const FSTService& Second = Candidates[B].Service;
				if (First.Layout != Second.Layout || (Candidates[A].bExisting && Candidates[B].bExisting) || IsDedicated(First) || IsDedicated(Second)
					|| First.Stops.Num() + Second.Stops.Num() > ServiceMaxStops + 2)
				{
					continue;
				}
				++Tried;
				FSTService Merged = First;
				if (!MergeLoops(Merged, Second))
				{
					continue;
				}
				const TArray<int32>* Trains = Layouts.Find(Merged.Layout);
				double Cycle = 0.0;
				double Needed = 0.0;
				if (!Trains || !RateLoop(Planner, (*Trains)[0], Merged.Stops, Merged.Flows, Cycle, Needed))
				{
					continue;
				}
				const double Saving = First.Needed + Second.Needed - Needed;
				if (Saving > BestSaving)
				{
					BestSaving = Saving;
					BestA = A;
					BestB = B;
					Merged.CycleSeconds = Cycle;
					Merged.Needed = Needed;
					BestMerged = MoveTemp(Merged);
				}
			}
		}
		if (BestA == INDEX_NONE)
		{
			break;
		}
		// The existing service (if one of the two) keeps its number and trains.
		const bool bKeepB = Candidates[BestB].bExisting;
		FCandidate& Keep = Candidates[bKeepB ? BestB : BestA];
		const int32 Id = Keep.Service.Id;
		const TArray<TObjectPtr<AFGTrain>> Trains = Keep.Service.Trains;
		const double LastDeparture = Keep.Service.LastDeparture;
		const bool bExisting = Keep.bExisting;
		UE_LOG(LogSmartTrains, Log, TEXT("SERVICE merge: %s + %s saves %.2f trains"), *DescribeService(Candidates[BestA].Service), *DescribeService(Candidates[BestB].Service),
			BestSaving);
		Keep.Service = MoveTemp(BestMerged);
		Keep.Service.Id = Id;
		Keep.Service.Trains = Trains;
		Keep.Service.LastDeparture = LastDeparture;
		Keep.bExisting = bExisting;
		Candidates.RemoveAt(bKeepB ? BestA : BestB);
	}
	for (FCandidate& Candidate : Candidates)
	{
		if (Candidate.bExisting)
		{
			if (FSTService* Service = FindService(Candidate.Service.Id))
			{
				if (ServiceKey(Service->Stops) != ServiceKey(Candidate.Service.Stops))
				{
					++Candidate.Service.Version;
					UE_LOG(LogSmartTrains, Log, TEXT("SERVICE %s changed (version %d): %d stops, a loop of %.0f min"), *DescribeService(Candidate.Service), Candidate.Service.Version,
						Candidate.Service.Stops.Num(), Candidate.Service.CycleSeconds / 60.0);
				}
				*Service = Candidate.Service;
			}
			continue;
		}
		Candidate.Service.Id = mNextServiceId++;
		Candidate.Service.CreatedAt = Now();
		UE_LOG(LogSmartTrains, Log, TEXT("SERVICE %s new: %d stops, a loop of %.0f min, needs %.2f trains (layout %s)"), *DescribeService(Candidate.Service),
			Candidate.Service.Stops.Num(), Candidate.Service.CycleSeconds / 60.0, Candidate.Service.Needed, *Candidate.Service.Layout);
		mNetworkServices.Add(MoveTemp(Candidate.Service));
	}
	for (const FString& Line : mServiceUncovered)
	{
		UE_LOG(LogSmartTrains, Verbose, TEXT("SERVICE uncovered: %s"), *Line);
	}
	AllocateServiceTrains(Layouts);
	UE_LOG(LogSmartTrains, Verbose, TEXT("SERVICES: %d services, %d flows not covered, designed in %.1f ms"), mNetworkServices.Num(), mServiceUncovered.Num(),
		(FPlatformTime::Seconds() - Started) * 1000.0);
}

bool ASTSubsystem::MergeLoops(FSTService& Into, const FSTService& Other) const
{
	// Other's stops go in Other's order, each at the cheapest place after Other's previous stop; a station already in
	// the loop takes Other's items there.
	const bool bPushPull = false;
	int32 After = -1;
	for (const FSTServiceStop& Stop : Other.Stops)
	{
		const int32 Existing = FindStop(Into.Stops, Stop.Station.Get());
		if (Existing != INDEX_NONE)
		{
			if (Existing <= After)
			{
				return false;
			}
			for (const TSubclassOf<UFGItemDescriptor>& Item : Stop.LoadItems)
			{
				Into.Stops[Existing].LoadItems.AddUnique(Item);
			}
			for (const TSubclassOf<UFGItemDescriptor>& Item : Stop.UnloadItems)
			{
				Into.Stops[Existing].UnloadItems.AddUnique(Item);
			}
			After = Existing;
			continue;
		}
		int32 BestAt = INDEX_NONE;
		double BestAdded = TNumericLimits<double>::Max();
		for (int32 At = After + 1; At <= Into.Stops.Num(); ++At)
		{
			const AFGTrainStationIdentifier* Before = Into.Stops[(At - 1 + Into.Stops.Num()) % Into.Stops.Num()].Station.Get();
			const AFGTrainStationIdentifier* Next = Into.Stops[At % Into.Stops.Num()].Station.Get();
			const float In = EstimateTravelSeconds(Before, Stop.Station.Get(), bPushPull);
			const float Out = EstimateTravelSeconds(Stop.Station.Get(), Next, bPushPull);
			const float Was = Before == Next ? 0.f : EstimateTravelSeconds(Before, Next, bPushPull);
			if (In < 0.f || Out < 0.f)
			{
				continue;
			}
			const double Added = In + Out - FMath::Max(0.f, Was);
			if (Added < BestAdded)
			{
				BestAdded = Added;
				BestAt = At;
			}
		}
		if (BestAt == INDEX_NONE)
		{
			return false;
		}
		Into.Stops.Insert(Stop, BestAt);
		After = BestAt;
	}
	Into.Flows.Append(Other.Flows);
	// Every flow picks up before it drops, counting from the loop's first stop, so one cycle delivers what it loads.
	for (const FSTServiceFlow& Flow : Into.Flows)
	{
		const int32 Pick = FindStop(Into.Stops, Flow.From.Get());
		const int32 Drop = FindStop(Into.Stops, Flow.To.Get());
		if (Pick == INDEX_NONE || Drop == INDEX_NONE || Pick >= Drop)
		{
			return false;
		}
	}
	return Into.Stops.Num() <= ServiceMaxStops;
}

void ASTSubsystem::AllocateServiceTrains(const TMap<FString, TArray<int32>>& Layouts)
{
	// Trains each service gets: ceil(needed - slack), within the player's limits. Trains stay where they are when they
	// can; services short of trains take free trains of their layout, the best placed first; trains left over are
	// spread over the services of their layout, each to the one with the most to carry per train.
	TMap<int32, int32> Target;
	for (const FSTService& Service : mNetworkServices)
	{
		int32 Want = FMath::Max(1, FMath::CeilToInt(Service.Needed - ServiceTrainSlack));
		Want = FMath::Max(Want, Service.MinTrains);
		if (Service.MaxTrains > 0)
		{
			Want = FMath::Min(Want, Service.MaxTrains);
		}
		Target.Add(Service.Id, Want);
	}
	TMap<int32, TArray<int32>> Members;
	TArray<int32> Free;
	for (int32 Index = 0; Index < mTrains.Num(); ++Index)
	{
		FSTTrainState& State = mTrains[Index];
		if (!State.bManaged || State.bKeepVanilla || !IsValid(State.Train) || State.bServiceReserve)
		{
			State.ServiceId = 0;
			continue;
		}
		const FSTService* Service = FindService(State.ServiceId);
		if (Service && Service->Layout == STLayout::GetLayoutKey(State.Train))
		{
			Members.FindOrAdd(Service->Id).Add(Index);
		}
		else
		{
			Free.Add(Index);
		}
	}
	// Over target: the newest joiners go back to the free trains (unless told to stay).
	for (TPair<int32, TArray<int32>>& Pair : Members)
	{
		const int32 Want = Target.FindRef(Pair.Key);
		for (int32 Position = Pair.Value.Num() - 1; Position >= 0 && Pair.Value.Num() > Want; --Position)
		{
			if (!mTrains[Pair.Value[Position]].bServiceStay)
			{
				Free.Add(Pair.Value[Position]);
				Pair.Value.RemoveAt(Position);
			}
		}
	}
	// Short: free trains of the layout, those already holding the service's items first, then the emptiest.
	auto Fits = [&](const FSTService& Service, int32 TrainIndex)
	{
		return STLayout::GetLayoutKey(mTrains[TrainIndex].Train) == Service.Layout;
	};
	auto Preference = [&](const FSTService& Service, int32 TrainIndex)
	{
		double Belongs = 0.0;
		double Other = 0.0;
		for (const TPair<UClass*, int32>& Held : GetCargo(mTrains[TrainIndex].Train))
		{
			const bool bService = Service.Flows.ContainsByPredicate([&](const FSTServiceFlow& Flow) { return Flow.Item.Get() == Held.Key; });
			(bService ? Belongs : Other) += Held.Value / FMath::Max(1.0, STLayout::GetCarCapacity(nullptr, Held.Key));
		}
		return Belongs * 10.0 - Other + (mTrains[TrainIndex].LastServiceId == Service.Id ? 0.5 : 0.0);
	};
	for (FSTService& Service : mNetworkServices)
	{
		TArray<int32>& Mine = Members.FindOrAdd(Service.Id);
		while (Mine.Num() < Target.FindRef(Service.Id))
		{
			int32 Best = INDEX_NONE;
			double BestScore = -TNumericLimits<double>::Max();
			for (const int32 Candidate : Free)
			{
				const double Score = Fits(Service, Candidate) ? Preference(Service, Candidate) : -TNumericLimits<double>::Max();
				if (Fits(Service, Candidate) && Score > BestScore)
				{
					BestScore = Score;
					Best = Candidate;
				}
			}
			if (Best == INDEX_NONE)
			{
				break;
			}
			Free.Remove(Best);
			Mine.Add(Best);
		}
	}
	// Spares: to the service of their layout with the most to carry per train; a spare already in a service counts
	// there first, so spares don't swap services for nothing.
	for (const int32 Spare : TArray<int32>(Free))
	{
		int32 BestId = 0;
		double BestLoad = -1.0;
		for (const FSTService& Service : mNetworkServices)
		{
			if (!Fits(Service, Spare))
			{
				continue;
			}
			const double Load = Service.Needed / FMath::Max(1, Members.FindRef(Service.Id).Num()) + (mTrains[Spare].ServiceId == Service.Id ? 0.05 : 0.0);
			if (Load > BestLoad)
			{
				BestLoad = Load;
				BestId = Service.Id;
			}
		}
		if (BestId != 0)
		{
			Members.FindOrAdd(BestId).Add(Spare);
			Free.Remove(Spare);
		}
	}
	for (FSTService& Service : mNetworkServices)
	{
		const TArray<int32>& Mine = Members.FindRef(Service.Id);
		TArray<FString> Added;
		TArray<FString> Removed;
		for (const int32 Index : Mine)
		{
			if (!Service.Trains.Contains(mTrains[Index].Train))
			{
				Added.Add(STLayout::Name(mTrains[Index].Train));
			}
		}
		for (const TObjectPtr<AFGTrain>& Train : Service.Trains)
		{
			if (!Mine.ContainsByPredicate([&](int32 Index) { return mTrains[Index].Train == Train; }))
			{
				Removed.Add(IsValid(Train) ? STLayout::Name(Train) : FString(TEXT("a removed train")));
			}
		}
		Service.Trains.Reset();
		for (const int32 Index : Mine)
		{
			Service.Trains.Add(mTrains[Index].Train);
			if (mTrains[Index].ServiceId != Service.Id)
			{
				mTrains[Index].LastServiceId = mTrains[Index].ServiceId;
				mTrains[Index].ServiceId = Service.Id;
			}
		}
		if (Added.Num() > 0 || Removed.Num() > 0)
		{
			UE_LOG(LogSmartTrains, Log, TEXT("SERVICE %s trains: %d (needs %.2f)%s%s"), *DescribeService(Service), Service.Trains.Num(), Service.Needed,
				Added.Num() > 0 ? *FString::Printf(TEXT("; joins: %s"), *FString::Join(Added, TEXT(", "))) : TEXT(""),
				Removed.Num() > 0 ? *FString::Printf(TEXT("; leaves: %s"), *FString::Join(Removed, TEXT(", "))) : TEXT(""));
		}
	}
	for (const int32 Index : Free)
	{
		if (mTrains[Index].ServiceId != 0)
		{
			UE_LOG(LogSmartTrains, Log, TEXT("SERVICE %s leaves its service: no service of its car layout needs it"), *STLayout::Name(mTrains[Index].Train));
			mTrains[Index].LastServiceId = mTrains[Index].ServiceId;
			mTrains[Index].ServiceId = 0;
		}
	}
}

void ASTSubsystem::RunServices(FSTTripPlanner& Planner, const TArray<FSTNode>& Nodes)
{
	const double Time = Now();
	for (FSTService& Service : mNetworkServices)
	{
		TSet<UClass*> ServiceItems;
		for (const FSTServiceFlow& Flow : Service.Flows)
		{
			ServiceItems.Add(Flow.Item.Get());
		}
		for (const TObjectPtr<AFGTrain>& Train : Service.Trains)
		{
			FSTTrainState* State = FindTrainState(Train);
			if (!State || !State->bManaged || !IsValid(State->Train) || State->ServiceId != Service.Id || (State->Train->IsPlayerDriven() && !State->Train->IsSelfDrivingEnabled()))
			{
				continue;
			}
			AFGTrainStationIdentifier* Anchor = nullptr;
			double FreeAt = 0.0;
			if (!GetFreeAt(*State, Anchor, FreeAt))
			{
				continue;
			}
			const int32 TrainIndex = static_cast<int32>(State - mTrains.GetData());
			const bool bDocked = (State->Train->IsDocked() && State->DockedStation.Get() == Anchor)
				|| (State->LastStation.Get() == Anchor && Time - State->LastDepartureAt < 60.0 && State->Phase != ESTTrainPhase::Parked);
			// Joining (or switching): cargo that isn't the service's goes first, delivered or (if cheaper) dumped.
			bool bForeign = false;
			for (const TPair<UClass*, int32>& Held : GetCargo(State->Train))
			{
				bForeign |= !ServiceItems.Contains(Held.Key);
			}
			if (bForeign && Planner.BeginTrain(TrainIndex, Anchor, FreeAt, bDocked))
			{
				TArray<FSTPlanStop> Stops;
				Planner.SetDeliveriesOnly(true);
				Planner.SetSwitching(true);
				FSTPlanResult Result = Planner.Improve(Stops, 0);
				Planner.SetDeliveriesOnly(false);
				Planner.SetSwitching(false);
				FSTTripPlanner::Trim(Stops, Result, 0);
				Result = Planner.Simulate(Stops);
				if (Result.bValid && Stops.Num() > 0)
				{
					UE_LOG(LogSmartTrains, Log, TEXT("SERVICE %s: %s clears cargo that isn't the service's first"), *DescribeService(Service), *STLayout::Name(State->Train));
					CommitTrip(*State, Planner, Stops, Result, true, 0);
					continue;
				}
			}
			// A cycle of the loop from its first stop, with amounts from the forecasts; stops with nothing to move are left
			// out of this cycle.
			if (!Planner.PrepareCars(TrainIndex, Anchor, FreeAt, bDocked))
			{
				continue;
			}
			// The loop was checked against the service limits when it was designed, which are wider than a trip's: the
			// "Most stops per trip" and "Longest trip" settings must not stop a whole service from running.
			Planner.RaiseStopLimit(ServiceMaxStops);
			Planner.RaiseTripLimit(2.0 * ServiceMaxSeconds);
			TArray<FSTPlanStop> Stops;
			for (const FSTServiceStop& Stop : Service.Stops)
			{
				FSTPlanStop& Each = Stops.AddDefaulted_GetRef();
				Each.Station = Stop.Station.Get();
				for (const TSubclassOf<UFGItemDescriptor>& Item : Stop.LoadItems)
				{
					Each.LoadItems.Add(Item.Get());
				}
				for (const TSubclassOf<UFGItemDescriptor>& Item : Stop.UnloadItems)
				{
					Each.UnloadItems.Add(Item.Get());
				}
			}
			// A service keeps its spacing instead of waiting for a batch: a pickup that would load less than its
			// platform's Least per visit (of what the train carries of that flow) is left out of this cycle instead.
			for (FSTPlanStop& Stop : Stops)
			{
				Stop.NoBatchItems = Stop.LoadItems;
			}
			FSTPlanResult Result = Planner.Simulate(Stops);
			for (int32 Pass = 0; Pass < 3 && Result.bValid; ++Pass)
			{
				bool bDropped = false;
				for (int32 StopIndex = 0; StopIndex < Stops.Num(); ++StopIndex)
				{
					for (int32 ItemIndex = Stops[StopIndex].LoadItems.Num() - 1; ItemIndex >= 0; --ItemIndex)
					{
						UClass* Item = Stops[StopIndex].LoadItems[ItemIndex];
						const FSTServiceFlow* Flow = Service.Flows.FindByPredicate([&](const FSTServiceFlow& Each)
						{
							return Each.Item.Get() == Item && Each.From.Get() == Stops[StopIndex].Station;
						});
						const int32 Node = FindNode(Nodes, Stops[StopIndex].Station, Item, ESTRole::Supply);
						const double Share = Node != INDEX_NONE ? STNames::SendAtPercent(Nodes[Node].SendAtPercent) / 100.0 : 0.0;
						double Loaded = 0.0;
						for (const FSTPlanCarStep& Step : Result.Steps.IsValidIndex(StopIndex) ? Result.Steps[StopIndex] : TArray<FSTPlanCarStep>())
						{
							for (const TPair<UClass*, int32>& Each : Step.Loaded)
							{
								Loaded += Each.Key == Item ? Each.Value : 0;
							}
						}
						if (Flow && Loaded < Share * Flow->PerTrain)
						{
							Stops[StopIndex].LoadItems.RemoveAt(ItemIndex);
							bDropped = true;
						}
					}
				}
				if (!bDropped)
				{
					break;
				}
				// Drops of items no stop picks up any more go too.
				for (FSTPlanStop& Stop : Stops)
				{
					Stop.UnloadItems.RemoveAll([&](UClass* Item)
					{
						return !Stops.ContainsByPredicate([&](const FSTPlanStop& Each) { return Each.LoadItems.Contains(Item); })
							&& !GetCargo(State->Train).Contains(Item);
					});
				}
				Result = Planner.Simulate(Stops);
			}
			if (Result.bValid)
			{
				FSTTripPlanner::Trim(Stops, Result, 0);
				Result = Planner.Simulate(Stops);
			}
			if (!Result.bValid || Stops.Num() == 0 || Result.Moved <= 0)
			{
				NoteNoTrip(*State, FString::Printf(TEXT("%s: nothing to move this cycle%s"), *DescribeService(Service),
					Result.bValid ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Result.Problem)));
				SettleServiceTrain(*State, TEXT("its service has nothing to move now"));
				continue;
			}
			// Spacing: the cycle leaves the loop's first stop no sooner than the spacing after the last train did.
			const bool bFromFirst = Stops[0].Station == Service.Stops[0].Station.Get();
			const double Departure = Result.Arrive[0] + EstimateStaySeconds(Stops[0].Station);
			const double Due = Service.LastDeparture + GetServiceSpacing(Service);
			if (bFromFirst && Departure < Due - ServiceSpacingSlack)
			{
				NoteNoTrip(*State, FString::Printf(TEXT("%s: keeping the service's spacing"), *DescribeService(Service)));
				SettleServiceTrain(*State, TEXT("keeping its service's spacing"));
				continue;
			}
			float Waiting = 0.f;
			const float Load = GetTrackLoad(Stops[0].Station, &Waiting);
			if (Load >= ServiceJamLoad && Waiting >= ServiceJamWaitSeconds && Time - Service.LastDeparture < ServiceJamSpacings * GetServiceSpacing(Service))
			{
				NoteNoTrip(*State, FString::Printf(TEXT("%s: the way into %s is jammed (Auto Trains)"), *DescribeService(Service), *STLayout::Name(Stops[0].Station)));
				SettleServiceTrain(*State, TEXT("the way into its next stop is jammed"));
				continue;
			}
			if (bFromFirst)
			{
				Service.LastDeparture = Departure;
			}
			CommitTrip(*State, Planner, Stops, Result, true, Service.Id);
		}
	}
	// Trains no service uses wait: at a free depot, or parked where they are.
	for (FSTTrainState& State : mTrains)
	{
		if (State.bManaged && IsValid(State.Train) && !State.bHasJob && State.Phase != ESTTrainPhase::Parked && !FindService(State.ServiceId)
			&& !(State.Train->IsPlayerDriven() && !State.Train->IsSelfDrivingEnabled()))
		{
			SettleIdle(State, State.bServiceReserve ? TEXT("kept out of services") : TEXT("no service needs it"), false);
		}
	}
}

void ASTSubsystem::SettleServiceTrain(FSTTrainState& State, const TCHAR* Why)
{
	if (!State.bHasJob && State.Phase != ESTTrainPhase::Parked)
	{
		Park(State, Why);
	}
}

TArray<FString> ASTSubsystem::DescribeServices() const
{
	TArray<FString> Lines;
	for (const FSTService& Service : mNetworkServices)
	{
		TArray<FString> Stops;
		for (const FSTServiceStop& Stop : Service.Stops)
		{
			TArray<FString> Moves;
			for (const TSubclassOf<UFGItemDescriptor>& Item : Stop.LoadItems)
			{
				Moves.Add(TEXT("+") + STNames::Item(Item));
			}
			for (const TSubclassOf<UFGItemDescriptor>& Item : Stop.UnloadItems)
			{
				Moves.Add(TEXT("-") + STNames::Item(Item));
			}
			Stops.Add(FString::Printf(TEXT("%s (%s)"), *STLayout::Name(Stop.Station.Get()), *FString::Join(Moves, TEXT(" "))));
		}
		Lines.Add(FString::Printf(TEXT("%s: %d train%s (needs %.1f), one every %.1f min, a loop of %.0f min%s: %s"), *DescribeService(Service), Service.Trains.Num(),
			Service.Trains.Num() == 1 ? TEXT("") : TEXT("s"), Service.Needed, GetServiceSpacing(Service) / 60.0, Service.CycleSeconds / 60.0,
			Service.bLocked ? TEXT(" [locked]") : TEXT(""), *FString::Join(Stops, TEXT(" -> "))));
	}
	for (const FString& Line : mServiceUncovered)
	{
		Lines.Add(TEXT("Not covered: ") + Line);
	}
	if (Lines.Num() == 0)
	{
		Lines.Add(TEXT("No services yet: the flow plan has no flows a managed train can carry."));
	}
	return Lines;
}

int32 ASTSubsystem::GetServiceTarget(const FSTService& Service) const
{
	int32 Want = FMath::Max(1, FMath::CeilToInt(Service.Needed - ServiceTrainSlack));
	Want = FMath::Max(Want, Service.MinTrains);
	return Service.MaxTrains > 0 ? FMath::Min(Want, Service.MaxTrains) : Want;
}

FString ASTSubsystem::DescribeServiceRole(const FSTTrainState& State) const
{
	if (!State.bManaged)
	{
		return FString();
	}
	if (State.bServiceReserve)
	{
		return TEXT("Kept out of services (parked when idle).");
	}
	const FSTService* Service = FindService(State.ServiceId);
	if (!Service)
	{
		const FString Layout = STLayout::GetLayoutKey(State.Train);
		const bool bAnyOfLayout = mNetworkServices.ContainsByPredicate([&](const FSTService& Each) { return Each.Layout == Layout; });
		return bAnyOfLayout ? FString(TEXT("No service: the services of its car layout have the trains they need.")) :
			FString::Printf(TEXT("No service: none runs on its car layout (%s)."), *Layout);
	}
	const int32 Position = Service->Trains.IndexOfByKey(State.Train);
	const int32 Target = GetServiceTarget(*Service);
	FString Text = Position != INDEX_NONE && Position < Target ? FString::Printf(TEXT("Train %d of %d in %s"), Position + 1, Service->Trains.Num(), *DescribeService(*Service))
		: FString::Printf(TEXT("Spare in %s"), *DescribeService(*Service));
	if (State.bHasJob && State.Job.ServiceId == 0 && !State.Job.bPark)
	{
		Text += TEXT(", clearing cargo that isn't the service's first");
	}
	else if (!State.bHasJob)
	{
		Text += TEXT(", waiting for its next cycle");
	}
	return Text + FString::Printf(TEXT(" (one every %.1f min%s)."), GetServiceSpacing(*Service) / 60.0, State.bServiceStay ? TEXT(", stays in this service") : TEXT(""));
}

TArray<FString> ASTSubsystem::DescribePlatformServices(const AFGBuildableTrainPlatformCargo* Platform) const
{
	TArray<FString> Lines;
	const AFGTrainStationIdentifier* Station = STLayout::GetIdentifier(STLayout::FindStation(const_cast<AFGBuildableTrainPlatformCargo*>(Platform)));
	const FSTPlatformConfig* Config = FindPlatformConfig(Platform);
	if (!Station || !Config || Config->Role == ESTRole::Off || Config->Role == ESTRole::Sink)
	{
		return Lines;
	}
	for (const FSTService& Service : mNetworkServices)
	{
		for (const FSTServiceFlow& Flow : Service.Flows)
		{
			const bool bHere = Config->Role == ESTRole::Supply ? Flow.From.Get() == Station : Flow.To.Get() == Station;
			if (bHere && Config->Items.ContainsByPredicate([&](const FSTItemRule& Rule) { return Rule.Item == Flow.Item; }))
			{
				Lines.AddUnique(FString::Printf(TEXT("Served by %s: %s, a train every %.1f min."), *DescribeService(Service), *STNames::Item(Flow.Item),
					GetServiceSpacing(Service) / 60.0));
			}
		}
	}
	if (Lines.Num() == 0)
	{
		const FString Name = STLayout::Name(Station);
		for (const FString& Uncovered : mServiceUncovered)
		{
			if (Uncovered.Contains(Name))
			{
				Lines.Add(TEXT("Not served: ") + Uncovered);
			}
		}
		if (Lines.Num() == 0)
		{
			Lines.Add(TEXT("Not served by a service yet: the flow plan has no flow for it now (nothing short, nothing to fetch, or no reachable partner)."));
		}
	}
	return Lines;
}

TArray<FString> ASTSubsystem::DescribeStationServices(const AFGTrainStationIdentifier* Station) const
{
	TArray<FString> Lines;
	for (const FSTService& Service : mNetworkServices)
	{
		const int32 Stop = Service.Stops.IndexOfByPredicate([Station](const FSTServiceStop& Each) { return Each.Station.Get() == Station; });
		if (Stop == INDEX_NONE)
		{
			continue;
		}
		TArray<FString> Moves;
		for (const TSubclassOf<UFGItemDescriptor>& Item : Service.Stops[Stop].LoadItems)
		{
			Moves.Add(TEXT("loads ") + STNames::Item(Item));
		}
		for (const TSubclassOf<UFGItemDescriptor>& Item : Service.Stops[Stop].UnloadItems)
		{
			Moves.Add(TEXT("drops ") + STNames::Item(Item));
		}
		Lines.Add(FString::Printf(TEXT("%s %s here, %.1f trains an hour."), *DescribeService(Service), *FString::Join(Moves, TEXT(", ")),
			GetServiceSpacing(Service) > 0.0 ? 3600.0 / GetServiceSpacing(Service) : 0.0));
	}
	const AFGTrain* Any = mTrains.Num() > 0 ? mTrains[0].Train.Get() : nullptr;
	const int32 Room = GetWaitingRoom(Station, STLayout::GetTrainLength(Any));
	if (Room >= 0)
	{
		Lines.Add(FString::Printf(TEXT("Room to wait before its switch: %d train%s (of %s's length)."), Room, Room == 1 ? TEXT("") : TEXT("s"), *STLayout::Name(Any)));
	}
	if (const void* Junction = GetJunction(Station))
	{
		TArray<FString> Shared;
		for (const FSTStationConfig& Each : mStations)
		{
			if (Each.Station && Each.Station.Get() != Station && GetJunction(Each.Station) == Junction)
			{
				Shared.Add(STLayout::Name(Each.Station.Get()));
			}
		}
		for (const FSTPlatformConfig& Each : mPlatforms)
		{
			const AFGTrainStationIdentifier* Other = STLayout::GetIdentifier(STLayout::FindStation(Each.Platform));
			if (Other && Other != Station && GetJunction(Other) == Junction)
			{
				Shared.AddUnique(STLayout::Name(Other));
			}
		}
		if (Shared.Num() > 0)
		{
			Lines.Add(TEXT("Shares its way-in switch with: ") + FString::Join(Shared, TEXT(", ")) + TEXT("."));
		}
	}
	if (IsDeadEnd(Station))
	{
		Lines.Add(TEXT("A dead end: trains leave the way they came."));
	}
	float Waiting = 0.f;
	const float Load = GetTrackLoad(Station, &Waiting);
	if (Load > 0.f || Waiting > 0.f)
	{
		Lines.Add(FString::Printf(TEXT("Way in: its switches and crossings are booked %.0f%% of the next 3 min, %.0f s of waiting there now (Auto Trains)."),
			Load * 100.f, Waiting));
	}
	return Lines;
}

bool ASTSubsystem::SetTrainServiceFlags(AFGTrain* Train, bool bStay, bool bOn, const FString& Cause)
{
	FSTTrainState* State = FindTrainState(Train);
	if (!State)
	{
		return false;
	}
	(bStay ? State->bServiceStay : State->bServiceReserve) = bOn;
	UE_LOG(LogSmartTrains, Log, TEXT("SERVICE %s: %s %s (%s)"), *STLayout::Name(Train), bStay ? TEXT("stay in its service") : TEXT("kept out of services"),
		bOn ? TEXT("on") : TEXT("off"), *Cause);
	PlanFlows();
	return true;
}

bool ASTSubsystem::SetServiceSettings(int32 Id, const FString& Verb, float First, float Second, const FString& Cause, FString& OutResult)
{
	FSTService* Service = FindService(Id);
	if (!Service)
	{
		OutResult = FString::Printf(TEXT("There is no service S%d (/st service lists them)."), Id);
		return false;
	}
	if (Verb == TEXT("lock") || Verb == TEXT("unlock"))
	{
		Service->bLocked = Verb == TEXT("lock");
		OutResult = Service->bLocked ? TEXT("Its stops won't change by themselves, and it stays while its flows are gone.") : TEXT("It changes with the flow plan again.");
	}
	else if (Verb == TEXT("spacing"))
	{
		Service->SpacingMinutes = FMath::Clamp(First, 0.f, 240.f);
		OutResult = Service->SpacingMinutes > 0.f ? FString::Printf(TEXT("A train leaves its first stop at most every %.1f min."), Service->SpacingMinutes)
			: FString(TEXT("Its spacing is its loop divided over its trains again."));
	}
	else if (Verb == TEXT("trains"))
	{
		Service->MinTrains = FMath::Clamp(FMath::RoundToInt(First), 0, 50);
		Service->MaxTrains = FMath::Clamp(FMath::RoundToInt(Second), 0, 50);
		OutResult = FString::Printf(TEXT("Trains: at least %d, at most %s."), Service->MinTrains, Service->MaxTrains > 0 ? *FString::FromInt(Service->MaxTrains) : TEXT("as many as it needs"));
	}
	else
	{
		OutResult = TEXT("Use lock, unlock, spacing <minutes> or trains <least> <most>.");
		return false;
	}
	OutResult = DescribeService(*Service) + TEXT(": ") + OutResult;
	UE_LOG(LogSmartTrains, Log, TEXT("SERVICE %s (%s)"), *OutResult, *Cause);
	PlanFlows();
	return true;
}
