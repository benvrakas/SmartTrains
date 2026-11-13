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
		// Cargo no trip has delivered for LeftoverMinutes may also go to a Sink platform while a consumer of it exists.
		const bool bUnwanted = bCarrying && State.UnwantedCargoSince > 0.0 && Time - State.UnwantedCargoSince > mSettings.LeftoverMinutes * 60.0;
		Planner.SetSinksForCargo(bUnwanted);
		const bool bBegun = Planner.BeginTrain(Each.TrainIndex, Each.Anchor, Each.FreeAt, Each.bDocked);
		Planner.SetSinksForCargo(false);
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
		// An empty train goes first to a consumer station no train serves, so every station with work gets a train before
		// any gets a second; then, like a train holding cargo, to the best trip its cars line up with.
		bool bUnservedChosen = false;
		if (!bCarrying)
		{
			TryConsumers(true);
			bUnservedChosen = BestConsumer != nullptr;
		}
		if (!BestConsumer)
		{
			TryConsumers(false);
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
			State.CarSets = PatternAfter(State, BestAllowed, Planner.GetCarCount(), true);
			const FString After = State.DescribePattern();
			if (After != Before)
			{
				UE_LOG(LogSmartTrains, Log, TEXT("PATTERN %s: %s -> %s (a trip to %s gives its cars their items%s)"), *STLayout::Name(State.Train), *Before, *After,
					*STLayout::Name(BestConsumer), bUnservedChosen ? TEXT("; it had no train") : TEXT(""));
			}
			CommitTrip(State, Planner, BestStops, BestResult, true, MostUnloaded(BestResult), BestConsumer);
			continue;
		}
		// Holding cargo with no trip in its pattern: it only delivers what it holds (to consumers of it; to a Sink
		// platform when nothing consumes it, or once it counts as unwanted), loading nothing, and keeps its pattern.
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
			Why += bUnwanted ? TEXT("; nothing takes the cargo it holds, not even a Sink platform")
				: TEXT("; the consumers of the cargo it holds have no room now, so it keeps it");
		}
		Why += TEXT(")");
		NoteNoTrip(State, Problems.IsEmpty() ? Why : FString::Printf(TEXT("%s; trips tried were rejected: %s"), *Why, *Problems));
		// Parked in the very station its trips would start at: it can't dock where it stands, so it parks elsewhere and
		// comes back.
		if (State.Phase == ESTTrainPhase::Parked && Planner.WasKeptFromOwnStation())
		{
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
