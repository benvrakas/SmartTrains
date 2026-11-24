// Car patterns (Docs/Architecture.md, "Car patterns"): every managed train has a pattern, the items each of its
// freight cars carries, and only takes trips to consumer stations whose demand platforms line up with it. Every trip is
// built around one such consumer. Cargo is what ties a train to its pattern: holding cargo it keeps it (and delivers
// what no trip in it takes); empty, it has none and takes the best trip anywhere.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "FGTrain.h"
#include "FGTrainStationIdentifier.h"
#include "SmartTrains.h"
#include "STDepotSignal.h"
#include "STForecast.h"
#include "STLayout.h"
#include "STSubsystem.h"
#include "STTripPlanner.h"

namespace
{
	// Backhaul: a pickup this close to a trip's last stop may be added after it, if every car it loads leaves at least
	// this full (a return load fills cars; a dribble only adds a docking). [s, share of a car]
	constexpr float BackhaulSeconds = 120.f;
	constexpr double BackhaulFullShare = 0.9;
	// A train that left a station this recently doesn't start a trip with that station. [s]
	constexpr double GatherJustLeftSeconds = 60.0;
	// The first train of every dispatch round gets at least this much planning time. [ms]
	constexpr double GatherMinTurnMs = 15.0;

	// The item a trip unloads most of, in car loads (null if it unloads nothing).
	UClass* MostUnloaded(const FSTPlanResult& Result)
	{
		TMap<UClass*, double> Loads;
		for (const TArray<FSTPlanCarStep>& Stop : Result.Steps)
		{
			for (const FSTPlanCarStep& Step : Stop)
			{
				for (const TPair<UClass*, int32>& Each : Step.Unloaded)
				{
					Loads.FindOrAdd(Each.Key) += Each.Value / FMath::Max(1.0, STLayout::GetCarCapacity(nullptr, Each.Key));
				}
			}
		}
		UClass* Best = nullptr;
		double Most = 0.0;
		for (const TPair<UClass*, double>& Pair : Loads)
		{
			if (Pair.Value > Most)
			{
				Most = Pair.Value;
				Best = Pair.Key;
			}
		}
		return Best;
	}

	// The pattern after a trip to a consumer: per car the items the trip allowed it there (its pattern items, the
	// demand platform's beside it, or what it holds), else (bKeep) its items before.
	TArray<FSTCarItems> PatternAfter(const FSTTrainState& State, const TArray<TArray<UClass*>>& Allowed, int32 Cars, bool bKeep)
	{
		TArray<FSTCarItems> Next;
		for (int32 CarIndex = 0; CarIndex < Cars; ++CarIndex)
		{
			FSTCarItems& Car = Next.AddDefaulted_GetRef();
			if (Allowed.IsValidIndex(CarIndex) && Allowed[CarIndex].Num() > 0)
			{
				for (UClass* Item : Allowed[CarIndex])
				{
					Car.Items.Add(Item);
				}
			}
			else if (bKeep && State.CarSets.IsValidIndex(CarIndex))
			{
				Car = State.CarSets[CarIndex];
			}
		}
		return Next;
	}
}

void ASTSubsystem::GatherRound(FSTTripPlanner& Planner, TArray<FSTNode>& Nodes)
{
	const double Time = Now();
	struct FFree
	{
		int32 TrainIndex = INDEX_NONE;
		AFGTrainStationIdentifier* Anchor = nullptr;
		double FreeAt = 0.0;
		bool bDocked = false;
		double LastTurn = -1.0e9;
	};
	TArray<FFree> Free;
	// Depot spots, surveyed once a round when a parked train needs them.
	TOptional<TArray<FSTDepotSpot>> Spots;
	for (int32 TrainIndex = 0; TrainIndex < mTrains.Num(); ++TrainIndex)
	{
		FSTTrainState& State = mTrains[TrainIndex];
		FFree Each;
		Each.TrainIndex = TrainIndex;
		if (!GetFreeAt(State, Each.Anchor, Each.FreeAt))
		{
			continue;
		}
		// A train parked behind another at a depot leaves after it.
		if (State.Phase == ESTTrainPhase::Parked && !State.bHasJob)
		{
			if (!Spots.IsSet())
			{
				Spots = STDepots::Survey(GetWorld());
			}
			FString Ahead;
			if (IsBoxedInAtDepot(State, Spots.GetValue(), Ahead))
			{
				NoteNoTrip(State, FString::Printf(TEXT("parked at a depot behind %s, which leaves first"), *Ahead));
				continue;
			}
		}
		const bool bJustLeftAnchor = State.LastStation.Get() == Each.Anchor && Time - State.LastDepartureAt < GatherJustLeftSeconds
			&& !(State.Phase == ESTTrainPhase::Parked && !State.Train->IsSelfDrivingEnabled());
		Each.bDocked = (State.Train->IsDocked() && State.DockedStation.Get() == Each.Anchor) || bJustLeftAnchor;
		Each.LastTurn = mPlanTurns.FindRef(State.Train.Get());
		Free.Add(Each);
	}
	if (bSnapshotNext)
	{
		bSnapshotNext = false;
		TArray<TSharedPtr<FJsonValue>> SnapshotTrains;
		for (const FFree& Each : Free)
		{
			if (Planner.BeginTrain(Each.TrainIndex, Each.Anchor, Each.FreeAt, Each.bDocked))
			{
				SnapshotTrains.Add(MakeShared<FJsonValueObject>(Planner.DescribeForReplay()));
			}
		}
		WriteSnapshot(Nodes, Planner.GetBookings(), SnapshotTrains);
	}
	// The train that waited longest for a turn goes first, then the soonest free; every round plans at least one.
	Free.Sort([](const FFree& A, const FFree& B)
	{
		return A.LastTurn != B.LastTurn ? A.LastTurn < B.LastTurn : (A.FreeAt != B.FreeAt ? A.FreeAt < B.FreeAt : A.TrainIndex < B.TrainIndex);
	});
	int32 Turns = 0;
	int32 Planned = 0;
	int32 Gathering = 0;
	for (const FFree& Each : Free)
	{
		if (Turns > 0 && Planner.IsOverBudget())
		{
			break;
		}
		if (Turns == 0)
		{
			Planner.GiveTime(GatherMinTurnMs);
		}
		++Turns;
		FSTTrainState& State = mTrains[Each.TrainIndex];
		mPlanTurns.Add(State.Train.Get(), Time);
		// Cargo is what ties a train to a group (Docs/Architecture.md, "Car patterns"): a train holding cargo keeps its
		// pattern and takes only trips its cars line up with, or delivers what it holds; an empty train has no group and
		// takes the best trip anywhere.
		const bool bCarrying = GetCargo(State.Train).Num() > 0;
		const bool bBegun = Planner.BeginTrain(Each.TrainIndex, Each.Anchor, Each.FreeAt, Each.bDocked);
		if (!bBegun)
		{
			continue;
		}
		if (State.CarSets.Num() != Planner.GetCarCount() || (!bCarrying && !State.IsPatternOpen()))
		{
			if (State.CarSets.Num() > 0 && State.CarSets.Num() != Planner.GetCarCount())
			{
				UE_LOG(LogSmartTrains, Log, TEXT("PATTERN %s: its cars changed (%d -> %d); pattern starts open"), *STLayout::Name(State.Train), State.CarSets.Num(),
					Planner.GetCarCount());
			}
			State.CarSets.Init(FSTCarItems(), Planner.GetCarCount());
		}
		// Cargo ties each car, not the train: a car holding none takes any item (Docs/Architecture.md, "Car patterns").
		for (int32 CarIndex = 0; CarIndex < State.CarSets.Num() && CarIndex < Planner.GetCarCount(); ++CarIndex)
		{
			if (Planner.GetCarCargo(CarIndex).Num() == 0)
			{
				State.CarSets[CarIndex].Items.Reset();
			}
		}
		const TArray<TArray<UClass*>> Pattern = State.GetPattern();
		// One trip per consumer station whose platforms line up with the cars: pickups for the cars that line up there,
		// added while the trip's value per second rises. The best one is run.
		TArray<FSTPlanStop> BestStops;
		FSTPlanResult BestResult;
		AFGTrainStationIdentifier* BestConsumer = nullptr;
		TArray<TArray<UClass*>> BestAllowed;
		int32 Tried = 0;
		int32 OtherPattern = 0;
		int32 Blocked = 0;
		// bUnserved: only consumer stations no train serves.
		auto TryConsumers = [&](bool bUnserved)
		{
			for (const AFGTrainStationIdentifier* Consumer : Planner.GetConsumerStations())
			{
				if (Tried > 0 && Planner.IsOverBudget())
				{
					break;
				}
				if (bUnserved && IsConsumerServed(Consumer))
				{
					continue;
				}
				// A parked train can't clear its door: no new trip there until it has.
				if (IsStationBlocked(Consumer))
				{
					continue;
				}
				TArray<TArray<UClass*>> Allowed;
				int32 Mismatched = 0;
				if (!Planner.MakeCarItems(Consumer, Pattern, false, Allowed, Mismatched))
				{
					++OtherPattern;
					continue;
				}
				if (Mismatched > 0)
				{
					++Blocked;
					continue;
				}
				++Tried;
				TArray<FSTPlanStop> Stops;
				const TArray<TArray<UClass*>> ThisAllowed = Allowed;
				Planner.SetCarItems(MoveTemp(Allowed));
				Planner.SetOnlyStations({Consumer});
				FSTPlanResult Result = Planner.Improve(Stops, 0);
				Planner.SetOnlyStations({});
				FSTTripPlanner::Trim(Stops, Result, 0);
				Result = Planner.Simulate(Stops);
				Planner.SetCarItems({});
				const bool bWorth = Result.bValid && Stops.Num() > 0 && Result.Moved > 0
					&& Result.Value - STForecast::TrainSecondCost * FMath::Max(0.0, Result.EndTime - Each.FreeAt) > 0.0;
				if (bWorth && (!BestConsumer || Result.Score > BestResult.Score))
				{
					BestStops = MoveTemp(Stops);
					BestResult = MoveTemp(Result);
					BestConsumer = const_cast<AFGTrainStationIdentifier*>(Consumer);
					BestAllowed = ThisAllowed;
				}
			}
		};
		// Push (empty trains): one trip per supplier station whose loading platforms some car stops beside, its full cars
		// to any consumers with room. The best one is run.
		auto TrySuppliers = [&]()
		{
			for (const AFGTrainStationIdentifier* Supplier : Planner.GetSupplierStations())
			{
				if (Tried > 0 && Planner.IsOverBudget())
				{
					break;
				}
				TArray<TArray<UClass*>> Allowed;
				if (IsStationBlocked(Supplier) || !Planner.MakeSupplyCarItems(Supplier, Allowed))
				{
					continue;
				}
				++Tried;
				TArray<FSTPlanStop> Stops;
				const TArray<TArray<UClass*>> ThisAllowed = Allowed;
				Planner.SetCarItems(MoveTemp(Allowed));
				Planner.SetOnlyFrom({Supplier});
				FSTPlanResult Result = Planner.Improve(Stops, 0);
				Planner.SetOnlyFrom({});
				FSTTripPlanner::Trim(Stops, Result, 0);
				Result = Planner.Simulate(Stops);
				Planner.SetCarItems({});
				const bool bWorth = Result.bValid && Stops.Num() > 0 && Result.Moved > 0
					&& Result.Value - STForecast::TrainSecondCost * FMath::Max(0.0, Result.EndTime - Each.FreeAt) > 0.0;
				if (bWorth && (!BestConsumer || Result.Score > BestResult.Score))
				{
					BestStops = MoveTemp(Stops);
					BestResult = MoveTemp(Result);
					BestConsumer = const_cast<AFGTrainStationIdentifier*>(Supplier);
					BestAllowed = ThisAllowed;
				}
			}
		};
		// Pull: an empty train goes first to a consumer station no train serves, so every station with work gets a train
		// before any gets a second; then, like a train holding cargo, to the best trip its cars line up with. Push: an
		// empty train goes to the best supplier; a train holding cargo delivers it as in pull.
		const bool bPushTrip = mSettings.bPush && !bCarrying;
		bool bUnservedChosen = false;
		if (bPushTrip)
		{
			TrySuppliers();
		}
		else
		{
			if (!bCarrying)
			{
				TryConsumers(true);
				bUnservedChosen = BestConsumer != nullptr;
			}
			if (!BestConsumer)
			{
				TryConsumers(false);
			}
		}
		// The trip's stations besides its anchor (BestConsumer: the consumer, or in push the supplier).
		TArray<AFGTrainStationIdentifier*> Others;
		// Several per trip: the cars the best trip leaves empty and unused may serve a second consumer (pull) or supplier
		// (push), with stops of their own (Docs/Architecture.md, "Several consumers per trip").
		if (BestConsumer && mSettings.bSeveralPerTrip && !Planner.IsOverBudget())
		{
			TArray<bool> FreeCar;
			int32 FreeCars = 0;
			for (int32 CarIndex = 0; CarIndex < Planner.GetCarCount(); ++CarIndex)
			{
				bool bUsed = Planner.GetCarCargo(CarIndex).Num() > 0;
				for (const TArray<FSTPlanCarStep>& Step : BestResult.Steps)
				{
					bUsed |= Step.IsValidIndex(CarIndex) && (Step[CarIndex].Loaded.Num() > 0 || Step[CarIndex].Unloaded.Num() > 0);
				}
				FreeCar.Add(!bUsed);
				FreeCars += bUsed ? 0 : 1;
			}
			TArray<FSTPlanStop> PairStops;
			FSTPlanResult PairResult;
			TArray<TArray<UClass*>> PairAllowed;
			AFGTrainStationIdentifier* Second = nullptr;
			const TArray<const AFGTrainStationIdentifier*> Candidates = FreeCars == 0 ? TArray<const AFGTrainStationIdentifier*>()
				: (bPushTrip ? Planner.GetSupplierStations() : Planner.GetConsumerStations());
			for (const AFGTrainStationIdentifier* Candidate : Candidates)
			{
				if (Candidate == BestConsumer || IsStationBlocked(Candidate) || Planner.IsOverBudget())
				{
					continue;
				}
				TArray<TArray<UClass*>> Theirs;
				int32 Mismatched = 0;
				if (bPushTrip ? !Planner.MakeSupplyCarItems(Candidate, Theirs) : !Planner.MakeCarItems(Candidate, {}, true, Theirs, Mismatched))
				{
					continue;
				}
				TArray<TArray<UClass*>> Merged = BestAllowed;
				bool bAny = false;
				for (int32 CarIndex = 0; CarIndex < Merged.Num() && CarIndex < Theirs.Num(); ++CarIndex)
				{
					if (FreeCar[CarIndex] && Theirs[CarIndex].Num() > 0)
					{
						Merged[CarIndex] = Theirs[CarIndex];
						bAny = true;
					}
				}
				if (!bAny)
				{
					continue;
				}
				TArray<FSTPlanStop> Stops = BestStops;
				Planner.SetCarItems(TArray<TArray<UClass*>>(Merged));
				if (bPushTrip)
				{
					Planner.SetOnlyFrom({BestConsumer, Candidate});
				}
				else
				{
					Planner.SetOnlyStations({BestConsumer, Candidate});
				}
				FSTPlanResult Result = Planner.Improve(Stops, 0);
				Planner.SetOnlyFrom({});
				Planner.SetOnlyStations({});
				FSTTripPlanner::Trim(Stops, Result, 0);
				Result = Planner.Simulate(Stops);
				Planner.SetCarItems({});
				const bool bServes = Stops.ContainsByPredicate([Candidate, bPushTrip](const FSTPlanStop& Stop)
				{
					return Stop.Station == Candidate && (bPushTrip ? Stop.LoadItems.Num() : Stop.UnloadItems.Num()) > 0;
				});
				const double Beat = PairStops.Num() > 0 ? PairResult.Score : BestResult.Score;
				if (Result.bValid && bServes && Result.Score > Beat * 1.0001 + 1e-6)
				{
					PairStops = MoveTemp(Stops);
					PairResult = MoveTemp(Result);
					PairAllowed = MoveTemp(Merged);
					Second = const_cast<AFGTrainStationIdentifier*>(Candidate);
				}
			}
			if (Second)
			{
				UE_LOG(LogSmartTrains, Log, TEXT("GATHER %s: %d free cars also serve %s %s (score %.4f -> %.4f)"), *STLayout::Name(State.Train), FreeCars,
					bPushTrip ? TEXT("supplier") : TEXT("consumer"), *STLayout::Name(Second), BestResult.Score, PairResult.Score);
				BestStops = MoveTemp(PairStops);
				BestResult = MoveTemp(PairResult);
				BestAllowed = MoveTemp(PairAllowed);
				Others.Add(Second);
			}
		}
		// Backhaul: after the trip's last stop, a pickup within BackhaulSeconds of it, delivered where it pays, so the run
		// back carries cargo (Docs/Architecture.md, "Backhaul").
		if (BestConsumer && BestStops.Num() > 0 && !Planner.IsOverBudget())
		{
			const AFGTrainStationIdentifier* Last = BestStops.Last().Station;
			const bool bPushPull = IsPushPull(State.Train);
			TArray<const AFGTrainStationIdentifier*> Near;
			for (const AFGTrainStationIdentifier* Supplier : Planner.GetSupplierStations())
			{
				const float Seconds = Supplier == Last ? 0.f : EstimateTravelSeconds(Last, Supplier, bPushPull);
				if (Seconds >= 0.f && Seconds <= BackhaulSeconds)
				{
					Near.Add(Supplier);
				}
			}
			if (Near.Num() > 0)
			{
				TArray<FSTPlanStop> Stops = BestStops;
				const int32 First = Stops.Num();
				Planner.SetOnlyFrom(Near);
				FSTPlanResult Result = Planner.Improve(Stops, First);
				Planner.SetOnlyFrom({});
				FSTTripPlanner::Trim(Stops, Result, First);
				Result = Planner.Simulate(Stops);
				bool bFull = true;
				for (int32 CarIndex = 0; Result.bValid && CarIndex < Planner.GetCarCount(); ++CarIndex)
				{
					TMap<UClass*, double> Loaded;
					for (int32 Index = First; Index < Result.Steps.Num(); ++Index)
					{
						for (const TPair<UClass*, int32>& Load : Result.Steps[Index].IsValidIndex(CarIndex) ? Result.Steps[Index][CarIndex].Loaded : TArray<TPair<UClass*, int32>>())
						{
							Loaded.FindOrAdd(Load.Key) += Load.Value;
						}
					}
					for (const TPair<UClass*, double>& Car : Loaded)
					{
						bFull &= Car.Value >= BackhaulFullShare * Planner.GetCarCapacity(CarIndex, Car.Key);
					}
				}
				if (Result.bValid && bFull && Stops.Num() > First && Result.Score > BestResult.Score * 1.0001 + 1e-6)
				{
					UE_LOG(LogSmartTrains, Log, TEXT("GATHER %s: backhaul from %s after %s (score %.4f -> %.4f)"), *STLayout::Name(State.Train),
						*STLayout::Name(Stops[First].Station), *STLayout::Name(Last), BestResult.Score, Result.Score);
					for (int32 Index = First; Index < Stops.Num(); ++Index)
					{
						Others.AddUnique(Stops[Index].Station);
					}
					BestStops = MoveTemp(Stops);
					BestResult = MoveTemp(Result);
				}
			}
		}
		if (BestConsumer)
		{
			++Planned;
			int32 Pickups = 0;
			for (const FSTPlanStop& Stop : BestStops)
			{
				Pickups += Stop.LoadItems.Num() > 0 ? 1 : 0;
			}
			Gathering += Pickups > 1 ? 1 : 0;
			// Each car's items for this trip: those it was planned with, and whatever it loads (backhaul).
			TArray<TArray<UClass*>> CarItems = BestAllowed;
			CarItems.SetNum(Planner.GetCarCount());
			for (const TArray<FSTPlanCarStep>& Step : BestResult.Steps)
			{
				for (int32 CarIndex = 0; CarIndex < Step.Num() && CarIndex < CarItems.Num(); ++CarIndex)
				{
					for (const TPair<UClass*, int32>& Load : Step[CarIndex].Loaded)
					{
						CarItems[CarIndex].AddUnique(Load.Key);
					}
				}
			}
			UE_LOG(LogSmartTrains, Verbose, TEXT("GATHER %s: %d %s stations tried (%d of other patterns, %d blocked by cargo), best %s (%d stops, %d with pickups), score %.4f"),
				*STLayout::Name(State.Train), Tried, bPushTrip ? TEXT("supplier") : TEXT("consumer"), OtherPattern, Blocked, *STLayout::Name(BestConsumer), BestStops.Num(),
				Pickups, BestResult.Score);
			const FString Before = State.DescribePattern();
			State.CarSets = PatternAfter(State, BestAllowed, Planner.GetCarCount(), true);
			const FString After = State.DescribePattern();
			if (After != Before)
			{
				UE_LOG(LogSmartTrains, Log, TEXT("PATTERN %s: %s -> %s (a trip %s %s gives its cars their items%s)"), *STLayout::Name(State.Train), *Before, *After,
					bPushTrip ? TEXT("from") : TEXT("to"), *STLayout::Name(BestConsumer), bUnservedChosen ? TEXT("; it had no train") : TEXT(""));
			}
			CommitTrip(State, Planner, BestStops, BestResult, true, MostUnloaded(BestResult), BestConsumer);
			if (State.bHasJob)
			{
				// Each car's share of the trip: planned fill, or what stood in its way at the anchor station.
				TArray<FString> Cars;
				int32 Used = 0;
				for (int32 CarIndex = 0; CarIndex < Planner.GetCarCount(); ++CarIndex)
				{
					double Share = 0.0;
					for (const TArray<FSTPlanCarStep>& Step : BestResult.Steps)
					{
						for (const TPair<UClass*, int32>& Load : Step.IsValidIndex(CarIndex) ? Step[CarIndex].Loaded : TArray<TPair<UClass*, int32>>())
						{
							const double Capacity = Planner.GetCarCapacity(CarIndex, Load.Key);
							Share += Capacity > 0.0 ? Load.Value / Capacity : 0.0;
						}
					}
					const bool bHolds = Planner.GetCarCargo(CarIndex).Num() > 0;
					Used += Share > 0.0 || bHolds ? 1 : 0;
					Cars.Add(Share > 0.0 ? FString::Printf(TEXT("car %d loads %.0f%%"), CarIndex + 1, 100.0 * FMath::Min(1.0, Share))
						: bHolds ? FString::Printf(TEXT("car %d holds cargo"), CarIndex + 1)
						: FString::Printf(TEXT("car %d unused: %s"), CarIndex + 1, *Planner.ExplainCar(BestConsumer, CarIndex)));
				}
				UE_LOG(LogSmartTrains, Log, TEXT("CARS %s trip %s %s: %d of %d cars used; %s"), *STLayout::Name(State.Train), bPushTrip ? TEXT("from") : TEXT("to"),
					*STLayout::Name(BestConsumer), Used, Planner.GetCarCount(), *FString::Join(Cars, TEXT("; ")));
				State.Job.bPushTrip = bPushTrip;
				for (AFGTrainStationIdentifier* Station : Others)
				{
					State.Job.OtherStations.Add(Station);
				}
				for (const TArray<UClass*>& Items : CarItems)
				{
					FSTCarItems& Car = State.Job.CarItems.AddDefaulted_GetRef();
					for (UClass* Item : Items)
					{
						Car.Items.Add(Item);
					}
				}
			}
			continue;
		}
		// Holding cargo with no trip in its pattern: it only delivers what it holds (to a consumer, or to a Sink platform
		// when that is worth more than keeping it), loading nothing, and keeps its pattern.
		bool bHeldByCargo = false;
		if (bCarrying && !(Tried > 0 && Planner.IsOverBudget()))
		{
			TArray<FSTPlanStop> Stops;
			Planner.SetNoLoads();
			FSTPlanResult Result = Planner.Improve(Stops, 0);
			FSTTripPlanner::Trim(Stops, Result, 0);
			Result = Planner.Simulate(Stops);
			Planner.SetCarItems({});
			if (Result.bValid && Stops.Num() > 0 && Result.Moved > 0)
			{
				++Planned;
				UE_LOG(LogSmartTrains, Log, TEXT("PATTERN %s: %s has no trip in its pattern and delivers the cargo it holds; empty, it is free for any station"),
					*STLayout::Name(State.Train), *State.DescribePattern());
				CommitTrip(State, Planner, Stops, Result, true, MostUnloaded(Result), nullptr);
				continue;
			}
			bHeldByCargo = true;
		}
		// Nothing anywhere: it waits (a free depot, else parked where it is).
		if (bCarrying && State.UnwantedCargoSince <= 0.0)
		{
			State.UnwantedCargoSince = Time;
			UE_LOG(LogSmartTrains, Log, TEXT("%s carries cargo no trip wants now"), *STLayout::Name(State.Train));
		}
		const FString Problems = Planner.DescribeProblems();
		FString Why = bCarrying
			? FString::Printf(TEXT("no consumer its cargo lines up with (%s) is worth a trip now (%d tried, %d need other items in its cars"), *State.DescribePattern(), Tried,
				OtherPattern + Blocked)
			: FString::Printf(TEXT("no consumer is worth a trip now (%d tried"), Tried);
		if (bHeldByCargo)
		{
			Why += TEXT("; no consumer has room for the cargo it holds now and a Sink platform isn't worth more than keeping it");
		}
		Why += TEXT(")");
		NoteNoTrip(State, Problems.IsEmpty() ? Why : FString::Printf(TEXT("%s; trips tried were rejected: %s"), *Why, *Problems));
		// Parked in the very station its trips would start at: it can't dock where it stands, so it parks elsewhere and
		// comes back.
		if (State.Phase == ESTTrainPhase::Parked && Planner.WasKeptFromOwnStation())
		{
			// What the game says about docking where it stands (whether a trip could start here by docking in place).
			UE_LOG(LogSmartTrains, Log, TEXT("OWNSTATION %s parked in %s: docking state %d (0 none, 1 ready to dock, 2 docked), self-driving %d, %.1f m/s"), *STLayout::Name(State.Train),
				*STLayout::Name(Each.Anchor), static_cast<int32>(State.Train->GetDockingState()), State.Train->IsSelfDrivingEnabled() ? 1 : 0, STLayout::GetTrainSpeed(State.Train) / 100.f);
			WantStation(Each.Anchor, TEXT("the train parked there has trips starting at it"));
		}
		if (!State.bHasJob && State.Phase != ESTTrainPhase::Parked && !(State.Train->IsPlayerDriven() && !State.Train->IsSelfDrivingEnabled()))
		{
			SettleIdle(State, TEXT("nothing to move"), false);
		}
	}
	UE_LOG(LogSmartTrains, Verbose, TEXT("DISPATCH: %d free trains, %d planned, %d trips (%d gathering from several suppliers), %.1f ms"), Free.Num(), Turns, Planned, Gathering,
		Planner.GetElapsedMs());
}
