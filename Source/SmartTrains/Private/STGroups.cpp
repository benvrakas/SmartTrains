// Item groups (Docs/Architecture.md, "Groups"): a readout of the flow plan. Every item the plan routes is a group of its
// routes (supplier, consumer, flow, trains needed) and the managed trains that fit them. Trips are planned by
// GatherRound (STGather.cpp); groups only tell the monitor and readouts how many trains each item needs and whether it
// is short of them.

#include "Buildables/FGBuildableRailroadStation.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGTrain.h"
#include "FGTrainStationIdentifier.h"
#include "SmartTrains.h"
#include "STLayout.h"
#include "STSubsystem.h"

namespace
{
	// A group wants ceil(needed - this) trains, so a need just over a whole number doesn't call for another train.
	constexpr double GroupTrainSlack = 0.15;
	// A group whose routes have left the flow plan is kept this long: flows dip out for a few minutes when a consumer
	// reaches its target or rates are measured again after a load. [s]
	constexpr double GroupKeepSeconds = 900.0;
	// Weight of the latest flow plan in a group's need, so a need that swings between plans reads steadily.
	constexpr double NeededSmoothing = 0.35;

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
		const bool bEnds = !Group.Item.IsValid() || (Group.Routes.Num() == 0 && Time - Group.LastRouteAt > GroupKeepSeconds);
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
}

int32 ASTSubsystem::GetGroupTarget(const FSTGroup& Group) const
{
	return FMath::Max(0, FMath::CeilToInt(Group.Needed - GroupTrainSlack));
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
	const int32 Missing = GetGroupTarget(Group) - CountWorkingFor(Group);
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

//~ Readouts

TArray<FString> ASTSubsystem::DescribeGroups() const
{
	TArray<FString> Lines;
	for (const FSTGroup& Group : mGroups)
	{
		const int32 Short = GetGroupShortfall(Group);
		Lines.Add(FString::Printf(TEXT("%s: needs %.1f trains, %d working on it now, %d route%s%s"), *DescribeGroup(Group), Group.Needed, CountWorkingFor(Group),
			Group.Routes.Num(), Group.Routes.Num() == 1 ? TEXT("") : TEXT("s"), Short > 0 ? *FString::Printf(TEXT(" [short %d]"), Short) : TEXT("")));
	}
	if (Lines.Num() == 0)
	{
		Lines.Add(TEXT("No groups yet: the flow plan has no flows a managed train can carry."));
	}
	return Lines;
}

FString ASTSubsystem::DescribeWork(const FSTTrainState& State) const
{
	if (!State.bManaged)
	{
		return FString();
	}
	const FString Cars = FString::Printf(TEXT(" Cars: %s."), *State.DescribePattern());
	if (State.bHasJob && !State.Job.bPark)
	{
		return (State.Job.AnchorStation ? FString::Printf(TEXT("Trip for %s."), *STLayout::Name(State.Job.AnchorStation.Get()))
			: FString(TEXT("Unloading cars to change group."))) + Cars;
	}
	const FString* Why = mIdleReasons.Find(State.Train.Get());
	return (Why ? TEXT("Waiting: ") + *Why + TEXT(".") : FString(TEXT("Waiting for a trip."))) + Cars;
}

TArray<FString> ASTSubsystem::DescribePatterns() const
{
	TArray<FString> Lines;
	TMap<FString, TArray<FString>> Groups;
	for (const FSTTrainState& State : mTrains)
	{
		if (State.bManaged && IsValid(State.Train))
		{
			Groups.FindOrAdd(State.DescribePattern()).Add(STLayout::Name(State.Train));
		}
	}
	Groups.KeySort([](const FString& A, const FString& B) { return A < B; });
	Lines.Add(TEXT("Train groups (item per freight car, front to back):"));
	for (const TPair<FString, TArray<FString>>& Group : Groups)
	{
		Lines.Add(FString::Printf(TEXT("  %s: %d train%s (%s)"), *Group.Key, Group.Value.Num(), Group.Value.Num() == 1 ? TEXT("") : TEXT("s"),
			*FString::Join(Group.Value, TEXT(", "))));
	}
	// Consumer stations: their platforms from the station building outward.
	TMap<FString, TArray<FString>> Stations;
	for (const FSTPlatformConfig& Config : mPlatforms)
	{
		if (!IsValid(Config.Platform) || !STNames::Unloads(Config.Role))
		{
			continue;
		}
		const AFGBuildableRailroadStation* Building = STLayout::FindStation(Config.Platform);
		const AFGTrainStationIdentifier* Identifier = STLayout::GetIdentifier(const_cast<AFGBuildableRailroadStation*>(Building));
		if (!Identifier || Stations.Contains(STLayout::Name(Identifier)))
		{
			continue;
		}
		TArray<FString> Slots;
		for (AFGBuildableTrainPlatform* Platform : STLayout::GetPlatforms(Building))
		{
			const AFGBuildableTrainPlatformCargo* Cargo = Cast<AFGBuildableTrainPlatformCargo>(Platform);
			const FSTPlatformConfig* Each = Cargo ? FindPlatformConfig(Cargo) : nullptr;
			TArray<FString> Items;
			if (Each && STNames::Unloads(Each->Role))
			{
				for (const FSTItemRule& Rule : Each->Items)
				{
					Items.Add(Rule.Item ? STNames::Item(Rule.Item) : FString(TEXT("any (sink)")));
				}
			}
			Slots.Add(Items.Num() > 0 ? FString::Join(Items, TEXT("/")) : FString(TEXT("-")));
		}
		Stations.Add(STLayout::Name(Identifier), Slots);
	}
	Stations.KeySort([](const FString& A, const FString& B) { return A < B; });
	Lines.Add(TEXT("Consumer stations (what each platform takes, from the station building outward):"));
	for (const TPair<FString, TArray<FString>>& Station : Stations)
	{
		Lines.Add(FString::Printf(TEXT("  %s: %s"), *Station.Key, *FString::Join(Station.Value, TEXT(" | "))));
	}
	return Lines;
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
		Lines.Add(FString::Printf(TEXT("%s: %s %s/min in the flow plan; %d trains working on it now."), *DescribeGroup(*Group), bSupply ? TEXT("to") : TEXT("from"),
			*FString::Join(Partners, TEXT(", ")), CountWorkingFor(*Group)));
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
