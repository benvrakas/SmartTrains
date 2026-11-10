// Item groups (Docs/Architecture.md, "Groups"): every item the flow plan moves is a group of its suppliers and
// consumers. Each managed train has a home group, given in proportion to what the groups need. A free train takes its
// home group's work first (and may add other work on the way); when its group has nothing, it is lent to any work it
// fits; with nothing anywhere it parks.

#include "Buildables/FGBuildableRailroadStation.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
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
	// A group wants ceil(needed - this) home trains, so a need just over a whole number doesn't take another train.
	constexpr double GroupTrainSlack = 0.15;
	// A group whose routes have left the flow plan keeps its homes this long: flows dip out for a few minutes when a
	// consumer reaches its target or rates are measured again after a load. [s]
	constexpr double GroupKeepSeconds = 900.0;
	// Weight of the latest flow plan in a group's need, so a need that swings between plans doesn't move trains back
	// and forth.
	constexpr double NeededSmoothing = 0.35;
	// A train that left a station this recently doesn't start a trip with that station. [s]
	constexpr double JustLeftAnchorSeconds = 60.0;

	// What a simulated trip loads and unloads of Item.
	int32 CountMoved(const FSTPlanResult& Result, const UClass* Item)
	{
		int32 Moved = 0;
		for (const TArray<FSTPlanCarStep>& Stop : Result.Steps)
		{
			for (const FSTPlanCarStep& Step : Stop)
			{
				for (const TPair<UClass*, int32>& Each : Step.Loaded)
				{
					Moved += Each.Key == Item ? Each.Value : 0;
				}
				for (const TPair<UClass*, int32>& Each : Step.Unloaded)
				{
					Moved += Each.Key == Item ? Each.Value : 0;
				}
			}
		}
		return Moved;
	}

	// The item a trip mostly delivers, in car loads (null if it delivers nothing).
	UClass* MainItem(const FSTPlanResult& Result)
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

	bool JobMoves(const FSTJob& Job, const UClass* Item)
	{
		for (int32 Leg = FMath::Max(0, Job.Leg); Leg < Job.Legs.Num(); ++Leg)
		{
			for (const FSTCarWork& Work : Job.Legs[Leg].Work)
			{
				if (Work.Item.Get() == Item)
				{
					return true;
				}
			}
		}
		return false;
	}
}

//~ Groups

const FSTGroup* ASTSubsystem::FindGroup(const UClass* Item) const
{
	return Item ? mGroups.FindByPredicate([Item](const FSTGroup& Group) { return Group.Item.Get() == Item; }) : nullptr;
}

const FSTGroupConfig* ASTSubsystem::FindGroupConfig(const UClass* Item) const
{
	return Item ? mGroupConfigs.FindByPredicate([Item](const FSTGroupConfig& Config) { return Config.Item.Get() == Item; }) : nullptr;
}

FString ASTSubsystem::DescribeGroup(const FSTGroup& Group)
{
	return STNames::Item(Group.Item.Get());
}

void ASTSubsystem::UpdateGroups(TArray<FSTFlowRoute>&& Routes)
{
	const double Time = Now();
	TMap<UClass*, TArray<FSTFlowRoute>> ByItem;
	for (FSTFlowRoute& Route : Routes)
	{
		if (Route.Item.IsValid())
		{
			ByItem.FindOrAdd(Route.Item.Get()).Add(MoveTemp(Route));
		}
	}
	auto NeedOf = [](const TArray<FSTFlowRoute>& Each)
	{
		double Need = 0.0;
		for (const FSTFlowRoute& Route : Each)
		{
			Need += Route.Trains;
		}
		return Need;
	};
	for (FSTGroup& Group : mGroups)
	{
		TArray<FSTFlowRoute>* Latest = ByItem.Find(Group.Item.Get());
		Group.Needed = FMath::Lerp(Group.Needed, Latest ? NeedOf(*Latest) : 0.0, NeededSmoothing);
		if (Latest)
		{
			Group.Routes = MoveTemp(*Latest);
			Group.LastRouteAt = Time;
			ByItem.Remove(Group.Item.Get());
		}
		else
		{
			Group.Routes.Reset();
		}
	}
	mGroups.RemoveAll([&](const FSTGroup& Group)
	{
		const FSTGroupConfig* Config = FindGroupConfig(Group.Item.Get());
		const bool bEnds = !Group.Item.IsValid() || (Group.Routes.Num() == 0 && Time - Group.LastRouteAt > GroupKeepSeconds && !(Config && Config->MinTrains > 0));
		if (bEnds)
		{
			UE_LOG(LogSmartTrains, Log, TEXT("GROUP %s ends: no route in the flow plan for %.0f min"), *DescribeGroup(Group), GroupKeepSeconds / 60.0);
		}
		return bEnds;
	});
	for (TPair<UClass*, TArray<FSTFlowRoute>>& Pair : ByItem)
	{
		FSTGroup& Group = mGroups.AddDefaulted_GetRef();
		Group.Item = Pair.Key;
		Group.Needed = NeedOf(Pair.Value);
		Group.Routes = MoveTemp(Pair.Value);
		Group.LastRouteAt = Time;
		UE_LOG(LogSmartTrains, Log, TEXT("GROUP %s new: %d route%s, needs %.2f trains"), *DescribeGroup(Group), Group.Routes.Num(), Group.Routes.Num() == 1 ? TEXT("") : TEXT("s"),
			Group.Needed);
	}
	// A group without routes now keeps the trains that fitted its last ones.
	for (FSTGroup& Group : mGroups)
	{
		if (Group.Routes.Num() == 0)
		{
			continue;
		}
		Group.Fitting.Reset();
		for (const FSTFlowRoute& Route : Group.Routes)
		{
			for (const TWeakObjectPtr<AFGTrain>& Train : Route.Fitting)
			{
				Group.Fitting.AddUnique(Train);
			}
		}
	}
	mGroups.Sort([](const FSTGroup& A, const FSTGroup& B) { return DescribeGroup(A) < DescribeGroup(B); });
	AllocateHomes();
}

void ASTSubsystem::AllocateHomes()
{
	// Seats go one at a time to the group with the most need per seat (needed / (seats + 1), as in the D'Hondt method),
	// so with too few trains every group gets its share, and with more than enough the spares spread the same way.
	// Trains that stay and each group's least come first; a group's own trains are its first choice for a seat.
	const int32 GroupCount = mGroups.Num();
	TArray<TSet<const AFGTrain*>> Fits;
	Fits.SetNum(GroupCount);
	for (int32 Group = 0; Group < GroupCount; ++Group)
	{
		for (const TWeakObjectPtr<AFGTrain>& Train : mGroups[Group].Fitting)
		{
			Fits[Group].Add(Train.Get());
		}
	}
	struct FCandidate
	{
		int32 TrainIndex = INDEX_NONE;
		int32 Current = INDEX_NONE;
		TArray<int32> Groups;
		int32 Assigned = INDEX_NONE;
	};
	TArray<FCandidate> Candidates;
	for (int32 TrainIndex = 0; TrainIndex < mTrains.Num(); ++TrainIndex)
	{
		const FSTTrainState& State = mTrains[TrainIndex];
		if (!State.bManaged || State.bKeepVanilla || !IsValid(State.Train))
		{
			continue;
		}
		FCandidate& Each = Candidates.AddDefaulted_GetRef();
		Each.TrainIndex = TrainIndex;
		for (int32 Group = 0; Group < GroupCount; ++Group)
		{
			if (Fits[Group].Contains(State.Train.Get()))
			{
				Each.Groups.Add(Group);
				Each.Current = mGroups[Group].Item.Get() == State.HomeItem.Get() ? Group : Each.Current;
			}
		}
	}
	TArray<int32> Seats;
	Seats.SetNumZeroed(GroupCount);
	auto MaxOf = [&](int32 Group)
	{
		const FSTGroupConfig* Config = FindGroupConfig(mGroups[Group].Item.Get());
		return Config && Config->MaxTrains > 0 ? Config->MaxTrains : MAX_int32;
	};
	// The train to seat in Group: its own first, then one without a home, then any; among those, one already holding the
	// item, then the one fitting the fewest groups (it has the fewest other places to go).
	auto Pick = [&](int32 Group)
	{
		UClass* Item = mGroups[Group].Item.Get();
		int32 Best = INDEX_NONE;
		TTuple<int32, int32, int32> BestKey(MAX_int32, MAX_int32, MAX_int32);
		for (int32 Index = 0; Index < Candidates.Num(); ++Index)
		{
			const FCandidate& Each = Candidates[Index];
			// A train that stays keeps its home, even when that group is gone or doesn't fit it now.
			if (Each.Assigned != INDEX_NONE || !Each.Groups.Contains(Group) || mTrains[Each.TrainIndex].bHomeStay)
			{
				continue;
			}
			const int32 Tier = Each.Current == Group ? 0 : (Each.Current == INDEX_NONE ? 1 : 2);
			const int32 Holds = GetCargo(mTrains[Each.TrainIndex].Train).Contains(Item) ? 0 : 1;
			const TTuple<int32, int32, int32> Key(Tier, Holds, Each.Groups.Num());
			if (Key < BestKey)
			{
				BestKey = Key;
				Best = Index;
			}
		}
		return Best;
	};
	auto Seat = [&](int32 Candidate, int32 Group)
	{
		Candidates[Candidate].Assigned = Group;
		++Seats[Group];
	};
	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		if (mTrains[Candidates[Index].TrainIndex].bHomeStay && Candidates[Index].Current != INDEX_NONE)
		{
			Seat(Index, Candidates[Index].Current);
		}
	}
	for (int32 Group = 0; Group < GroupCount; ++Group)
	{
		const FSTGroupConfig* Config = FindGroupConfig(mGroups[Group].Item.Get());
		while (Config && Seats[Group] < FMath::Min(Config->MinTrains, MaxOf(Group)))
		{
			const int32 Candidate = Pick(Group);
			if (Candidate == INDEX_NONE)
			{
				break;
			}
			Seat(Candidate, Group);
		}
	}
	while (true)
	{
		int32 BestGroup = INDEX_NONE;
		int32 BestCandidate = INDEX_NONE;
		double BestQuotient = 0.0;
		for (int32 Group = 0; Group < GroupCount; ++Group)
		{
			const double Quotient = mGroups[Group].Needed / (Seats[Group] + 1);
			if (Quotient <= BestQuotient || Seats[Group] >= MaxOf(Group))
			{
				continue;
			}
			const int32 Candidate = Pick(Group);
			if (Candidate != INDEX_NONE)
			{
				BestQuotient = Quotient;
				BestGroup = Group;
				BestCandidate = Candidate;
			}
		}
		if (BestGroup == INDEX_NONE)
		{
			break;
		}
		Seat(BestCandidate, BestGroup);
	}
	// Apply and log the changes per group.
	TArray<TArray<FString>> Joins;
	TArray<TArray<FString>> Leaves;
	Joins.SetNum(GroupCount);
	Leaves.SetNum(GroupCount);
	for (FSTGroup& Group : mGroups)
	{
		Group.Homes.Reset();
	}
	for (const FCandidate& Each : Candidates)
	{
		FSTTrainState& State = mTrains[Each.TrainIndex];
		// A train that stays keeps its home even while that group is gone or doesn't fit it now.
		if (State.bHomeStay && Each.Assigned == INDEX_NONE)
		{
			continue;
		}
		UClass* NewHome = Each.Assigned != INDEX_NONE ? mGroups[Each.Assigned].Item.Get() : nullptr;
		if (Each.Assigned != INDEX_NONE)
		{
			mGroups[Each.Assigned].Homes.Add(State.Train.Get());
		}
		if (NewHome == State.HomeItem.Get())
		{
			continue;
		}
		const int32 Old = mGroups.IndexOfByPredicate([&](const FSTGroup& Group) { return Group.Item.Get() == State.HomeItem.Get(); });
		if (Old != INDEX_NONE)
		{
			Leaves[Old].Add(STLayout::Name(State.Train));
		}
		if (Each.Assigned != INDEX_NONE)
		{
			Joins[Each.Assigned].Add(STLayout::Name(State.Train));
		}
		else if (State.HomeItem)
		{
			UE_LOG(LogSmartTrains, Log, TEXT("GROUP %s: no home now (its group is gone, or the groups it fits have the trains they need)"), *STLayout::Name(State.Train));
		}
		State.HomeItem = NewHome;
	}
	for (int32 Group = 0; Group < GroupCount; ++Group)
	{
		if (Joins[Group].Num() > 0 || Leaves[Group].Num() > 0)
		{
			UE_LOG(LogSmartTrains, Log, TEXT("GROUP %s: %d home train%s (needs %.2f)%s%s"), *DescribeGroup(mGroups[Group]), mGroups[Group].Homes.Num(),
				mGroups[Group].Homes.Num() == 1 ? TEXT("") : TEXT("s"), mGroups[Group].Needed,
				Joins[Group].Num() > 0 ? *FString::Printf(TEXT("; joins: %s"), *FString::Join(Joins[Group], TEXT(", "))) : TEXT(""),
				Leaves[Group].Num() > 0 ? *FString::Printf(TEXT("; leaves: %s"), *FString::Join(Leaves[Group], TEXT(", "))) : TEXT(""));
		}
	}
}

int32 ASTSubsystem::GetGroupTarget(const FSTGroup& Group) const
{
	int32 Want = FMath::Max(0, FMath::CeilToInt(Group.Needed - GroupTrainSlack));
	if (const FSTGroupConfig* Config = FindGroupConfig(Group.Item.Get()))
	{
		Want = FMath::Max(Want, Config->MinTrains);
		Want = Config->MaxTrains > 0 ? FMath::Min(Want, Config->MaxTrains) : Want;
	}
	return Want;
}

int32 ASTSubsystem::CountWorkingFor(const FSTGroup& Group) const
{
	int32 Count = 0;
	for (const FSTTrainState& State : mTrains)
	{
		Count += State.bManaged && State.bHasJob && !State.Job.bPark && JobMoves(State.Job, Group.Item.Get()) ? 1 : 0;
	}
	return Count;
}

int32 ASTSubsystem::GetGroupShortfall(const FSTGroup& Group) const
{
	// Trains working on it from other groups count too.
	int32 LentIn = 0;
	for (const FSTTrainState& State : mTrains)
	{
		LentIn += State.bManaged && State.bHasJob && !State.Job.bPark && State.HomeItem.Get() != Group.Item.Get() && JobMoves(State.Job, Group.Item.Get()) ? 1 : 0;
	}
	const int32 Missing = GetGroupTarget(Group) - Group.Homes.Num() - LentIn;
	if (Missing <= 0)
	{
		return 0;
	}
	// A train that fits it and has nothing to do would take its work: then it is short of work, not of trains.
	for (const TWeakObjectPtr<AFGTrain>& Train : Group.Fitting)
	{
		const FSTTrainState* State = mTrains.FindByPredicate([&](const FSTTrainState& Each) { return Each.Train.Get() == Train.Get(); });
		if (State && State->bManaged && (!State->bHasJob || (State->Job.bPark && State->Job.Legs.Num() == 1 && !State->Job.Legs[0].HasWork())))
		{
			return 0;
		}
	}
	return Missing;
}

//~ Dispatching

void ASTSubsystem::GroupRound(FSTTripPlanner& Planner, TArray<FSTNode>& Nodes)
{
	const double Time = Now();
	struct FFree
	{
		int32 TrainIndex = INDEX_NONE;
		AFGTrainStationIdentifier* Anchor = nullptr;
		double FreeAt = 0.0;
		bool bDocked = false;
		bool bPlanned = false;
	};
	TArray<FFree> Free;
	TArray<TSharedPtr<FJsonValue>> SnapshotTrains;
	for (int32 TrainIndex = 0; TrainIndex < mTrains.Num() && !Planner.IsOverBudget(); ++TrainIndex)
	{
		FSTTrainState& State = mTrains[TrainIndex];
		FFree Each;
		Each.TrainIndex = TrainIndex;
		if (!GetFreeAt(State, Each.Anchor, Each.FreeAt))
		{
			continue;
		}
		const bool bJustLeftAnchor = State.LastStation.Get() == Each.Anchor && Time - State.LastDepartureAt < JustLeftAnchorSeconds
			&& !(State.Phase == ESTTrainPhase::Parked && !State.Train->IsSelfDrivingEnabled());
		Each.bDocked = (State.Train->IsDocked() && State.DockedStation.Get() == Each.Anchor) || bJustLeftAnchor;
		if (Planner.BeginTrain(TrainIndex, Each.Anchor, Each.FreeAt, Each.bDocked))
		{
			if (bSnapshotNext)
			{
				SnapshotTrains.Add(MakeShared<FJsonValueObject>(Planner.DescribeForReplay()));
			}
			Free.Add(Each);
		}
	}
	if (bSnapshotNext)
	{
		bSnapshotNext = false;
		WriteSnapshot(Nodes, Planner.GetBookings(), SnapshotTrains);
	}
	// The trains free soonest plan first.
	Free.Sort([](const FFree& A, const FFree& B) { return A.FreeAt != B.FreeAt ? A.FreeAt < B.FreeAt : A.TrainIndex < B.TrainIndex; });
	auto Resume = [&](const FFree& Each)
	{
		return Planner.ResumeTrain(Each.TrainIndex, Each.Anchor, Each.FreeAt, Each.bDocked) || Planner.BeginTrain(Each.TrainIndex, Each.Anchor, Each.FreeAt, Each.bDocked);
	};
	// Worth running: what it gains in the forecasts beats the train's time.
	auto Worth = [&](const FFree& Each, const TArray<FSTPlanStop>& Stops, const FSTPlanResult& Result)
	{
		return Result.bValid && Stops.Num() > 0 && Result.Moved > 0 && Result.Value - STForecast::TrainSecondCost * FMath::Max(0.0, Result.EndTime - Each.FreeAt) > 0.0;
	};
	auto Finish = [&](TArray<FSTPlanStop>& Stops, FSTPlanResult& Result)
	{
		FSTTripPlanner::Trim(Stops, Result, 0);
		Result = Planner.Simulate(Stops);
	};
	int32 Home = 0;
	int32 Lent = 0;
	// Home work first: a trip that starts with the home group's moves (or delivers what the train carries), to which
	// any other work may be added on the way.
	for (FFree& Each : Free)
	{
		FSTTrainState& State = mTrains[Each.TrainIndex];
		UClass* HomeItem = State.HomeItem.Get();
		if (Planner.IsOverBudget() || !HomeItem || !Resume(Each))
		{
			continue;
		}
		TArray<FSTPlanStop> Stops;
		Planner.SetOnlyItem(HomeItem);
		FSTPlanResult Result = Planner.Improve(Stops, 0);
		Planner.SetOnlyItem(nullptr);
		if (!Result.bValid || CountMoved(Result, HomeItem) <= 0)
		{
			continue;
		}
		Result = Planner.Improve(Stops, 0);
		Finish(Stops, Result);
		if (Worth(Each, Stops, Result) && CountMoved(Result, HomeItem) > 0)
		{
			Each.bPlanned = true;
			++Home;
			CommitTrip(State, Planner, Stops, Result, true, HomeItem);
		}
	}
	// Then any work: trains whose group has nothing now are lent to whatever is worth running.
	for (FFree& Each : Free)
	{
		if (Each.bPlanned || Planner.IsOverBudget() || !Resume(Each))
		{
			continue;
		}
		FSTTrainState& State = mTrains[Each.TrainIndex];
		TArray<FSTPlanStop> Stops;
		FSTPlanResult Result = Planner.Improve(Stops, 0);
		Finish(Stops, Result);
		if (Worth(Each, Stops, Result))
		{
			Each.bPlanned = true;
			UClass* Item = MainItem(Result);
			if (State.HomeItem && Item && Item != State.HomeItem.Get())
			{
				++Lent;
				UE_LOG(LogSmartTrains, Log, TEXT("LEND %s: its group %s has nothing to move now; lent to %s"), *STLayout::Name(State.Train), *STNames::Item(State.HomeItem),
					*STNames::Item(Item));
			}
			CommitTrip(State, Planner, Stops, Result, true, Item);
			continue;
		}
		// Nothing anywhere: it waits (a free depot, else parked where it is).
		if (GetCargo(State.Train).Num() > 0 && State.UnwantedCargoSince <= 0.0)
		{
			State.UnwantedCargoSince = Time;
			UE_LOG(LogSmartTrains, Log, TEXT("%s carries cargo no trip wants now"), *STLayout::Name(State.Train));
		}
		const FString Problems = Planner.DescribeProblems();
		NoteNoTrip(State, FString::Printf(TEXT("%s%s"), State.HomeItem ? *FString::Printf(TEXT("its group %s has nothing to move now, nor has any other it fits"),
			*STNames::Item(State.HomeItem)) : TEXT("nothing it fits is worth running now"), Problems.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("; trips tried were rejected: %s"), *Problems)));
		if (!State.bHasJob && State.Phase != ESTTrainPhase::Parked && !(State.Train->IsPlayerDriven() && !State.Train->IsSelfDrivingEnabled()))
		{
			SettleIdle(State, TEXT("nothing to move"), false);
		}
	}
	UE_LOG(LogSmartTrains, Verbose, TEXT("DISPATCH: %d free trains, %d home trips, %d lent, %.1f ms"), Free.Num(), Home, Lent, Planner.GetElapsedMs());
}

//~ Readouts

TArray<FString> ASTSubsystem::DescribeGroups() const
{
	TArray<FString> Lines;
	for (const FSTGroup& Group : mGroups)
	{
		const int32 Short = GetGroupShortfall(Group);
		const FSTGroupConfig* Config = FindGroupConfig(Group.Item.Get());
		Lines.Add(FString::Printf(TEXT("%s: %d home train%s (needs %.1f), %d working on it now, %d route%s%s%s"), *DescribeGroup(Group), Group.Homes.Num(),
			Group.Homes.Num() == 1 ? TEXT("") : TEXT("s"), Group.Needed, CountWorkingFor(Group), Group.Routes.Num(), Group.Routes.Num() == 1 ? TEXT("") : TEXT("s"),
			Config && (Config->MinTrains > 0 || Config->MaxTrains > 0) ? *FString::Printf(TEXT(", trains %d to %s"), Config->MinTrains,
				Config->MaxTrains > 0 ? *FString::FromInt(Config->MaxTrains) : TEXT("any")) : TEXT(""),
			Short > 0 ? *FString::Printf(TEXT(" [short %d]"), Short) : TEXT("")));
	}
	if (Lines.Num() == 0)
	{
		Lines.Add(TEXT("No groups yet: the flow plan has no flows a managed train can carry."));
	}
	return Lines;
}

FString ASTSubsystem::DescribeHome(const FSTTrainState& State) const
{
	if (!State.bManaged)
	{
		return FString();
	}
	UClass* HomeItem = State.HomeItem.Get();
	FString Text;
	if (!HomeItem)
	{
		const bool bFitsAny = mGroups.ContainsByPredicate([&](const FSTGroup& Group) { return Group.Fitting.Contains(State.Train.Get()); });
		Text = bFitsAny ? TEXT("No home group: the groups it fits have the trains they need; it takes any work it fits")
			: TEXT("No home group: no group's routes fit its cars; it takes any work it fits");
	}
	else
	{
		const FSTGroup* Group = FindGroup(HomeItem);
		Text = Group ? FString::Printf(TEXT("Home group: %s (%d home trains, needs %.1f)"), *DescribeGroup(*Group), Group->Homes.Num(), Group->Needed)
			: FString::Printf(TEXT("Home group: %s (no routes in the flow plan now)"), *STNames::Item(HomeItem));
		Text += State.bHomeStay ? TEXT(", stays there") : TEXT("");
	}
	if (State.bHasJob && !State.Job.bPark)
	{
		UClass* For = State.Job.GroupItem.Get();
		Text += HomeItem && For && For != HomeItem ? FString::Printf(TEXT("; lent to %s"), *STNames::Item(For)) : FString(TEXT("; working"));
	}
	else if (const FString* Why = mIdleReasons.Find(State.Train.Get()))
	{
		Text += TEXT("; waiting: ") + *Why;
	}
	return Text + TEXT(".");
}

TArray<FString> ASTSubsystem::DescribePlatformGroups(const AFGBuildableTrainPlatformCargo* Platform) const
{
	TArray<FString> Lines;
	const AFGTrainStationIdentifier* Station = STLayout::GetIdentifier(STLayout::FindStation(const_cast<AFGBuildableTrainPlatformCargo*>(Platform)));
	const FSTPlatformConfig* Config = FindPlatformConfig(Platform);
	if (!Station || !Config || (Config->Role != ESTRole::Supply && Config->Role != ESTRole::Demand))
	{
		return Lines;
	}
	const bool bSupply = Config->Role == ESTRole::Supply;
	for (const FSTItemRule& Rule : Config->Items)
	{
		const FSTGroup* Group = FindGroup(Rule.Item.Get());
		TArray<FString> Partners;
		for (const FSTFlowRoute& Route : Group ? Group->Routes : TArray<FSTFlowRoute>())
		{
			if ((bSupply ? Route.From : Route.To).Get() == Station)
			{
				Partners.Add(FString::Printf(TEXT("%s %s"), *STLayout::Name((bSupply ? Route.To : Route.From).Get()), *STNames::Amount(Rule.Item, Route.Flow * 60.0)));
			}
		}
		if (!Group || Partners.Num() == 0)
		{
			Lines.Add(FString::Printf(TEXT("%s: no route here in the flow plan now (nothing short, nothing to fetch, or no managed train fits it and reaches a partner)."),
				*STNames::Item(Rule.Item)));
			continue;
		}
		Lines.Add(FString::Printf(TEXT("Group %s: %s %s/min; %d home trains, %d working on it now."), *DescribeGroup(*Group), bSupply ? TEXT("to") : TEXT("from"),
			*FString::Join(Partners, TEXT(", ")), Group->Homes.Num(), CountWorkingFor(*Group)));
	}
	return Lines;
}

TArray<FString> ASTSubsystem::DescribeStationGroups(const AFGTrainStationIdentifier* Station) const
{
	TArray<FString> Lines;
	for (const FSTGroup& Group : mGroups)
	{
		double Out = 0.0;
		double In = 0.0;
		for (const FSTFlowRoute& Route : Group.Routes)
		{
			Out += Route.From.Get() == Station ? Route.Flow : 0.0;
			In += Route.To.Get() == Station ? Route.Flow : 0.0;
		}
		if (Out > 0.0 || In > 0.0)
		{
			Lines.Add(FString::Printf(TEXT("Group %s: %s%s%s/min here."), *DescribeGroup(Group), Out > 0.0 ? TEXT("loads ") : TEXT("drops "),
				*STNames::Amount(Group.Item.Get(), (Out > 0.0 ? Out : In) * 60.0), Out > 0.0 && In > 0.0 ? *FString::Printf(TEXT(", drops %s"),
					*STNames::Amount(Group.Item.Get(), In * 60.0)) : TEXT("")));
		}
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

//~ Player settings

bool ASTSubsystem::SetTrainHomeStay(AFGTrain* Train, bool bOn, const FString& Cause)
{
	FSTTrainState* State = FindTrainState(Train);
	if (!State)
	{
		return false;
	}
	State->bHomeStay = bOn;
	UE_LOG(LogSmartTrains, Log, TEXT("GROUP %s: stay in its group %s (home %s; %s)"), *STLayout::Name(Train), bOn ? TEXT("on") : TEXT("off"),
		State->HomeItem ? *STNames::Item(State->HomeItem) : TEXT("none"), *Cause);
	AllocateHomes();
	return true;
}

bool ASTSubsystem::SetGroupTrains(UClass* Item, int32 MinTrains, int32 MaxTrains, const FString& Cause, FString& OutResult)
{
	if (!Item)
	{
		OutResult = TEXT("Name an item, e.g. /st group sulfur trains 2 4.");
		return false;
	}
	MinTrains = FMath::Clamp(MinTrains, 0, 50);
	MaxTrains = FMath::Clamp(MaxTrains, 0, 50);
	if (MaxTrains > 0 && MinTrains > MaxTrains)
	{
		Swap(MinTrains, MaxTrains);
	}
	const int32 Existing = mGroupConfigs.IndexOfByPredicate([Item](const FSTGroupConfig& Config) { return Config.Item.Get() == Item; });
	if (MinTrains == 0 && MaxTrains == 0)
	{
		if (Existing != INDEX_NONE)
		{
			mGroupConfigs.RemoveAt(Existing);
		}
		OutResult = FString::Printf(TEXT("%s: trains automatic again."), *STNames::Item(Item));
	}
	else
	{
		FSTGroupConfig& Config = Existing != INDEX_NONE ? mGroupConfigs[Existing] : mGroupConfigs.AddDefaulted_GetRef();
		Config.Item = Item;
		Config.MinTrains = MinTrains;
		Config.MaxTrains = MaxTrains;
		OutResult = FString::Printf(TEXT("%s: at least %d home train%s, at most %s."), *STNames::Item(Item), MinTrains, MinTrains == 1 ? TEXT("") : TEXT("s"),
			MaxTrains > 0 ? *FString::FromInt(MaxTrains) : TEXT("as many as it needs"));
	}
	UE_LOG(LogSmartTrains, Log, TEXT("GROUP %s (%s)"), *OutResult, *Cause);
	AllocateHomes();
	return true;
}
