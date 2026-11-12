// Car patterns (Docs/Architecture.md, "Car patterns"): every managed train has a pattern, the items each of its
// freight cars carries, and only takes trips to consumer stations whose demand platforms line up with it. Every trip is
// built around one such consumer. A train that finds no trip in its pattern for a while may change group: cars whose
// cargo doesn't line up with the new consumer are unloaded first.

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
	// A train that left a station this recently doesn't start a trip with that station. [s]
	constexpr double GatherJustLeftSeconds = 60.0;
	// The first train of every dispatch round gets at least this much planning time. [ms]
	constexpr double GatherMinTurnMs = 15.0;
	constexpr double PatternIdleSeconds = STPatternIdleSeconds;
	// A train's pattern is checked against every consumer station at most this often while it finds nothing. [s]
	constexpr double PatternHomeCheckSeconds = 60.0;

	// True if A and B give every car the same items, front to back or back to front (a train turned round).
	bool SamePattern(const TArray<TArray<UClass*>>& A, const TArray<TArray<UClass*>>& B)
	{
		auto SameCar = [](const TArray<UClass*>& X, const TArray<UClass*>& Y)
		{
			return X.Num() == Y.Num() && !X.ContainsByPredicate([&Y](UClass* Item) { return !Y.Contains(Item); });
		};
		if (A.Num() != B.Num())
		{
			return false;
		}
		bool bForward = true;
		bool bBackward = true;
		for (int32 Car = 0; Car < A.Num(); ++Car)
		{
			bForward &= SameCar(A[Car], B[Car]);
			bBackward &= SameCar(A[Car], B[A.Num() - 1 - Car]);
		}
		return bForward || bBackward;
	}

	// True if some consumer station's demand platforms take Pattern: a car with pattern items beside a demand platform
	// for the same items, and no car beside one for other items.
	bool HasPatternHome(const FSTTripPlanner& Planner, const TArray<FSTNode>& Nodes, const TArray<TArray<UClass*>>& Pattern)
	{
		TSet<const AFGTrainStationIdentifier*> Seen;
		for (const FSTNode& Node : Nodes)
		{
			const AFGTrainStationIdentifier* Station = Node.Station.Get();
			if (Node.Role != ESTRole::Demand || Node.bAnyItem || !Station || Seen.Contains(Station))
			{
				continue;
			}
			Seen.Add(Station);
			TArray<TArray<UClass*>> Allowed;
			int32 Mismatched = 0;
			int32 Matched = 0;
			if (Planner.MakeCarItems(Station, Pattern, false, Allowed, Mismatched, &Matched) && Matched > 0)
			{
				return true;
			}
		}
		return false;
	}

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
		const bool bOpen = State.IsPatternOpen();
		// A train may change group when it has none yet or found nothing in its own for a while.
		const bool bMayChange = bOpen || (State.PatternIdleSince > 0.0 && Time - State.PatternIdleSince >= PatternIdleSeconds);
		Planner.SetSinksForCargo(bMayChange);
		const bool bBegun = Planner.BeginTrain(Each.TrainIndex, Each.Anchor, Each.FreeAt, Each.bDocked);
		Planner.SetSinksForCargo(false);
		if (!bBegun)
		{
			continue;
		}
		if (State.CarSets.Num() != Planner.GetCarCount())
		{
			if (State.CarSets.Num() > 0)
			{
				UE_LOG(LogSmartTrains, Log, TEXT("PATTERN %s: its cars changed (%d -> %d); pattern starts open"), *STLayout::Name(State.Train), State.CarSets.Num(),
					Planner.GetCarCount());
			}
			State.CarSets.Init(FSTCarItems(), Planner.GetCarCount());
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
		// bUnserved: only consumer stations no train serves (a train choosing a pattern tries those first).
		auto TryConsumers = [&](bool bIgnorePattern, bool bUnserved)
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
				if (!Planner.MakeCarItems(Consumer, Pattern, bIgnorePattern, Allowed, Mismatched))
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
				Planner.SetOnlyStation(Consumer);
				FSTPlanResult Result = Planner.Improve(Stops, 0);
				Planner.SetOnlyStation(nullptr);
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
		// Choosing a pattern (open, or changing group): stations no train serves first, then any.
		bool bUnservedChosen = false;
		if (bOpen)
		{
			TryConsumers(false, true);
			bUnservedChosen = BestConsumer != nullptr;
		}
		if (!BestConsumer)
		{
			TryConsumers(false, false);
		}
		// A pattern no consumer station takes any more (a player changed the platforms it came from) has no group to wait
		// for: the train changes group at once.
		bool bHomeless = false;
		if (!BestConsumer && !bOpen && !bMayChange && Tried == 0 && Blocked == 0)
		{
			if (Time - State.PatternHomeCheckedAt >= PatternHomeCheckSeconds)
			{
				State.PatternHomeCheckedAt = Time;
				State.bPatternHomeless = !HasPatternHome(Planner, Nodes, Pattern);
				if (State.bPatternHomeless)
				{
					UE_LOG(LogSmartTrains, Log, TEXT("PATTERN %s: no consumer station takes %s any more; it may change group now"), *STLayout::Name(State.Train),
						*State.DescribePattern());
				}
			}
			bHomeless = State.bPatternHomeless;
		}
		// A spare train: its cars are empty, nothing in its own group is worth a trip, and another managed train keeps the
		// group. It may move at once, but only to a consumer station no train serves; else it waits as before.
		bool bSpare = false;
		if (!BestConsumer && !bOpen && !bMayChange && !bHomeless && GetCargo(State.Train).Num() == 0)
		{
			bSpare = mTrains.ContainsByPredicate([&](const FSTTrainState& Other)
			{
				return Other.bManaged && Other.Train != State.Train && IsValid(Other.Train) && SamePattern(Other.GetPattern(), Pattern);
			});
		}
		bool bChanging = false;
		if (!BestConsumer && !bOpen && (bMayChange || bHomeless || bSpare))
		{
			bChanging = true;
			TryConsumers(true, true);
			bUnservedChosen = BestConsumer != nullptr;
			if (!BestConsumer && !bSpare)
			{
				TryConsumers(true, false);
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
			UE_LOG(LogSmartTrains, Verbose, TEXT("GATHER %s: %d consumer stations tried (%d of other patterns, %d blocked by cargo), best %s (%d stops, %d with pickups), score %.4f"),
				*STLayout::Name(State.Train), Tried, OtherPattern, Blocked, *STLayout::Name(BestConsumer), BestStops.Num(), Pickups, BestResult.Score);
			const FString Before = State.DescribePattern();
			State.CarSets = PatternAfter(State, BestAllowed, Planner.GetCarCount(), !bChanging);
			State.PatternIdleSince = 0.0;
			State.bPatternHomeless = false;
			State.PatternHomeCheckedAt = -1.0e9;
			const FString After = State.DescribePattern();
			if (After != Before)
			{
				UE_LOG(LogSmartTrains, Log, TEXT("PATTERN %s: %s -> %s (%s%s)"), *STLayout::Name(State.Train), *Before, *After,
					bChanging ? (bHomeless ? TEXT("changes group: no consumer station takes its pattern any more")
						: (bMayChange ? TEXT("changes group: nothing to do in its own for a while") : TEXT("changes group: spare, empty with nothing to do, another train keeps its group")))
						: TEXT("a trip gives open cars their items"),
					bUnservedChosen ? *FString::Printf(TEXT("; %s had no train"), *STLayout::Name(BestConsumer)) : TEXT(""));
			}
			CommitTrip(State, Planner, BestStops, BestResult, true, MostUnloaded(BestResult), BestConsumer);
			continue;
		}
		if (!bOpen && State.PatternIdleSince <= 0.0)
		{
			State.PatternIdleSince = Time;
		}
		// Consumers need work its cargo is in the way of: it unloads the cars first (at consumers of the cargo, else at a
		// sink), then takes trips to any consumer that lines up.
		if ((bMayChange || bHomeless) && Blocked > 0 && GetCargo(State.Train).Num() > 0 && !(Tried > 0 && Planner.IsOverBudget()))
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
				UE_LOG(LogSmartTrains, Log, TEXT("PATTERN %s: %s -> open; unloads its cars first (%d consumers it could serve need other items in them)"),
					*STLayout::Name(State.Train), *State.DescribePattern(), Blocked);
				State.CarSets.Init(FSTCarItems(), Planner.GetCarCount());
				State.PatternIdleSince = 0.0;
				State.bPatternHomeless = false;
				CommitTrip(State, Planner, Stops, Result, true, MostUnloaded(Result), nullptr);
				continue;
			}
		}
		// Nothing anywhere: it waits (a free depot, else parked where it is).
		if (GetCargo(State.Train).Num() > 0 && State.UnwantedCargoSince <= 0.0)
		{
			State.UnwantedCargoSince = Time;
			UE_LOG(LogSmartTrains, Log, TEXT("%s carries cargo no trip wants now"), *STLayout::Name(State.Train));
		}
		const FString Problems = Planner.DescribeProblems();
		FString Why = FString::Printf(TEXT("no consumer in its pattern (%s) is worth a trip now (%d tried, %d of other patterns"), *State.DescribePattern(), Tried,
			OtherPattern);
		if (Blocked > 0)
		{
			Why += FString::Printf(TEXT(", %d where cargo on board doesn't line up"), Blocked);
		}
		if (bHomeless)
		{
			Why += TEXT("; no consumer station takes its pattern any more, so it may change group now");
		}
		else if (!bOpen && !bMayChange)
		{
			Why += FString::Printf(TEXT("; may change group in %.0f min"), FMath::Max(0.0, PatternIdleSeconds - (Time - State.PatternIdleSince)) / 60.0);
		}
		Why += TEXT(")");
		NoteNoTrip(State, Problems.IsEmpty() ? Why : FString::Printf(TEXT("%s; trips tried were rejected: %s"), *Why, *Problems));
		if (!State.bHasJob && State.Phase != ESTTrainPhase::Parked && !(State.Train->IsPlayerDriven() && !State.Train->IsSelfDrivingEnabled()))
		{
			SettleIdle(State, TEXT("nothing to move"), false);
		}
	}
	UE_LOG(LogSmartTrains, Verbose, TEXT("DISPATCH: %d free trains, %d planned, %d trips (%d gathering from several suppliers), %.1f ms"), Free.Num(), Turns, Planned, Gathering,
		Planner.GetElapsedMs());
}
