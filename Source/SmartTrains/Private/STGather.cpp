// Consumer-anchored trips (Docs/Architecture.md, "0.12.0: consumer-anchored trips"): every trip is built around one
// consumer station. A free train fills the cars that line up with that station's demand platforms from the suppliers
// that pay best, and the trip ends there; the train takes the consumer whose trip is worth the most per second.

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
		if (!Planner.BeginTrain(Each.TrainIndex, Each.Anchor, Each.FreeAt, Each.bDocked))
		{
			continue;
		}
		// One trip per consumer station the train can deliver to: pickups for the cars that line up there, added while
		// the trip's value per second rises. The best one is run. A train with cargo on board first tries only the
		// consumers of that cargo, and only trips that deliver some of it, so it serves one factory at a time; any
		// consumer only when none of them is worth a trip now.
		TArray<FSTPlanStop> BestStops;
		FSTPlanResult BestResult;
		AFGTrainStationIdentifier* BestConsumer = nullptr;
		int32 Tried = 0;
		const TMap<UClass*, int32> Carried = GetCargo(State.Train);
		TSet<const AFGTrainStationIdentifier*> TakesCargo;
		for (const FSTNode& Node : Nodes)
		{
			if (Node.Role == ESTRole::Demand && !Node.bAnyItem && Node.Item && Carried.Contains(Node.Item.Get()))
			{
				TakesCargo.Add(Node.Station.Get());
			}
		}
		auto DeliversCargo = [&Carried](const FSTPlanResult& Result)
		{
			for (const TArray<FSTPlanCarStep>& Stop : Result.Steps)
			{
				for (const FSTPlanCarStep& Step : Stop)
				{
					for (const TPair<UClass*, int32>& Each : Step.Unloaded)
					{
						if (Each.Value > 0 && Carried.Contains(Each.Key))
						{
							return true;
						}
					}
				}
			}
			return false;
		};
		for (int32 Pass = TakesCargo.Num() > 0 ? 0 : 1; Pass < 2 && !BestConsumer; ++Pass)
		for (const AFGTrainStationIdentifier* Consumer : Planner.GetConsumerStations())
		{
			if (Pass == 0 && !TakesCargo.Contains(Consumer))
			{
				continue;
			}
			if (Tried > 0 && Planner.IsOverBudget())
			{
				break;
			}
			++Tried;
			TArray<FSTPlanStop> Stops;
			Planner.SetOnlyStation(Consumer);
			FSTPlanResult Result = Planner.Improve(Stops, 0);
			Planner.SetOnlyStation(nullptr);
			FSTTripPlanner::Trim(Stops, Result, 0);
			Result = Planner.Simulate(Stops);
			const bool bWorth = Result.bValid && Stops.Num() > 0 && Result.Moved > 0
				&& Result.Value - STForecast::TrainSecondCost * FMath::Max(0.0, Result.EndTime - Each.FreeAt) > 0.0
				&& (Pass > 0 || DeliversCargo(Result));
			if (bWorth && (!BestConsumer || Result.Score > BestResult.Score))
			{
				BestStops = MoveTemp(Stops);
				BestResult = MoveTemp(Result);
				BestConsumer = const_cast<AFGTrainStationIdentifier*>(Consumer);
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
			UE_LOG(LogSmartTrains, Verbose, TEXT("GATHER %s: %d consumer stations tried, best %s (%d stops, %d with pickups), score %.4f"), *STLayout::Name(State.Train), Tried,
				*STLayout::Name(BestConsumer), BestStops.Num(), Pickups, BestResult.Score);
			CommitTrip(State, Planner, BestStops, BestResult, true, MostUnloaded(BestResult), BestConsumer);
			continue;
		}
		// Nothing anywhere: it waits (a free depot, else parked where it is).
		if (GetCargo(State.Train).Num() > 0 && State.UnwantedCargoSince <= 0.0)
		{
			State.UnwantedCargoSince = Time;
			UE_LOG(LogSmartTrains, Log, TEXT("%s carries cargo no trip wants now"), *STLayout::Name(State.Train));
		}
		const FString Problems = Planner.DescribeProblems();
		NoteNoTrip(State, FString::Printf(TEXT("no consumer it can fill cars for is worth a trip now (%d tried)%s"), Tried,
			Problems.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("; trips tried were rejected: %s"), *Problems)));
		if (!State.bHasJob && State.Phase != ESTTrainPhase::Parked && !(State.Train->IsPlayerDriven() && !State.Train->IsSelfDrivingEnabled()))
		{
			SettleIdle(State, TEXT("nothing to move"), false);
		}
	}
	UE_LOG(LogSmartTrains, Verbose, TEXT("DISPATCH: %d free trains, %d planned, %d trips (%d gathering from several suppliers), %.1f ms"), Free.Num(), Turns, Planned, Gathering,
		Planner.GetElapsedMs());
}
