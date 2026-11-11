// Car patterns (Docs/Architecture.md, "0.18.0: car patterns"): every managed train has a pattern, the item each of its
// freight cars carries, and only takes trips to consumer stations whose demand platforms line up with it. Every trip is
// built around one such consumer. A train that finds no trip in its pattern for a while may change group: cars whose
// cargo doesn't line up with the new consumer are unloaded first.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "FGTrain.h"
#include "FGTrainStationIdentifier.h"
#include "SmartTrains.h"
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
	// A train may change group once it has found no trip in its pattern for this long. [s]
	constexpr double PatternIdleSeconds = 300.0;

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

	// The pattern after a trip: per car the item it loads, else the cargo it holds, else (bKeep) its item before.
	TArray<TSubclassOf<UFGItemDescriptor>> PatternAfter(const FSTTrainState& State, const FSTTripPlanner& Planner, const FSTPlanResult& Result, bool bKeep)
	{
		TArray<TSubclassOf<UFGItemDescriptor>> Next;
		for (int32 CarIndex = 0; CarIndex < Planner.GetCarCount(); ++CarIndex)
		{
			UClass* Item = nullptr;
			for (const TArray<FSTPlanCarStep>& Stop : Result.Steps)
			{
				if (!Item && Stop.IsValidIndex(CarIndex))
				{
					for (const TPair<UClass*, int32>& Each : Stop[CarIndex].Loaded)
					{
						if (Each.Value > 0)
						{
							Item = Each.Key;
							break;
						}
					}
				}
			}
			if (!Item)
			{
				Item = Planner.GetCarCargo(CarIndex);
			}
			if (!Item && bKeep && State.CarPattern.IsValidIndex(CarIndex))
			{
				Item = State.CarPattern[CarIndex].Get();
			}
			Next.Add(Item);
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
	for (int32 TrainIndex = 0; TrainIndex < mTrains.Num(); ++TrainIndex)
	{
		FSTTrainState& State = mTrains[TrainIndex];
		FFree Each;
		Each.TrainIndex = TrainIndex;
		if (!GetFreeAt(State, Each.Anchor, Each.FreeAt))
		{
			continue;
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
		const bool bOpen = !State.CarPattern.ContainsByPredicate([](const TSubclassOf<UFGItemDescriptor>& Item) { return Item != nullptr; });
		// A train may change group when it has none yet or found nothing in its own for a while.
		const bool bMayChange = bOpen || (State.PatternIdleSince > 0.0 && Time - State.PatternIdleSince >= PatternIdleSeconds);
		Planner.SetSinksForCargo(bMayChange);
		const bool bBegun = Planner.BeginTrain(Each.TrainIndex, Each.Anchor, Each.FreeAt, Each.bDocked);
		Planner.SetSinksForCargo(false);
		if (!bBegun)
		{
			continue;
		}
		if (State.CarPattern.Num() != Planner.GetCarCount())
		{
			if (State.CarPattern.Num() > 0)
			{
				UE_LOG(LogSmartTrains, Log, TEXT("PATTERN %s: its cars changed (%d -> %d); pattern starts open"), *STLayout::Name(State.Train), State.CarPattern.Num(),
					Planner.GetCarCount());
			}
			State.CarPattern.Init(nullptr, Planner.GetCarCount());
		}
		const TArray<UClass*> Pattern = State.GetPattern();
		// One trip per consumer station whose platforms line up with the cars: pickups for the cars that line up there,
		// added while the trip's value per second rises. The best one is run.
		TArray<FSTPlanStop> BestStops;
		FSTPlanResult BestResult;
		AFGTrainStationIdentifier* BestConsumer = nullptr;
		int32 Tried = 0;
		int32 OtherPattern = 0;
		int32 Blocked = 0;
		auto TryConsumers = [&](bool bIgnorePattern)
		{
			for (const AFGTrainStationIdentifier* Consumer : Planner.GetConsumerStations())
			{
				if (Tried > 0 && Planner.IsOverBudget())
				{
					break;
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
				}
			}
		};
		TryConsumers(false);
		bool bChanging = false;
		if (!BestConsumer && !bOpen && bMayChange)
		{
			bChanging = true;
			TryConsumers(true);
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
			State.CarPattern = PatternAfter(State, Planner, BestResult, !bChanging);
			State.PatternIdleSince = 0.0;
			const FString After = State.DescribePattern();
			if (After != Before)
			{
				UE_LOG(LogSmartTrains, Log, TEXT("PATTERN %s: %s -> %s (%s)"), *STLayout::Name(State.Train), *Before, *After,
					bChanging ? TEXT("changes group: nothing to do in its own for a while") : TEXT("a trip gives open cars their item"));
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
		if (bMayChange && Blocked > 0 && GetCargo(State.Train).Num() > 0 && !(Tried > 0 && Planner.IsOverBudget()))
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
				State.CarPattern.Init(nullptr, Planner.GetCarCount());
				State.PatternIdleSince = 0.0;
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
		if (!bOpen && !bMayChange)
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
