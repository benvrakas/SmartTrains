#include "STThroughput.h"
#include "Buildables/FGBuildableRailroadStation.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGFreightWagon.h"
#include "FGInventoryComponent.h"
#include "FGLocomotive.h"
#include "FGLocomotiveMovementComponent.h"
#include "FGTrain.h"
#include "FGTrainStationIdentifier.h"
#include "SmartTrains.h"
#include "STLayout.h"
#include "STBridges.h"
#include "STSubsystem.h"

namespace
{
	// Monitor: requests shown are those due within this, at most this many. [s]
	constexpr double RequestRowSeconds = 900.0;
	constexpr int32 MaxRequestRows = 10;
	// Share of time moving on trips or at stops from which the fleet counts as saturated.
	constexpr double SaturatedShare = 0.85;
	// Share of time trains stand still on trips from which the network counts as congested.
	constexpr double StandingShare = 0.15;
	// Below this a driving train counts as standing still. [cm/s]
	constexpr float StandingSpeed = 100.f;
	// Fewest samples before a verdict (one minute).
	constexpr int32 MinSamples = 6;
	// Shares of the window that make a verdict.
	constexpr double LowShare = 0.25;
	constexpr double FullShare = 0.3;
	constexpr double EmptyShare = 0.5;
	constexpr double BusyShare = 0.8;
	constexpr double CongestedShare = 0.85;
	// Average lateness of arrivals at a station that counts as congestion, and the fewest arrivals to judge it. [s]
	constexpr double CongestedLate = 120.0;
	// A station with this many managed trains on their way to it on average is a bottleneck: they queue before it.
	constexpr double QueueTrains = 2.5;
	constexpr int32 MinArrivals = 3;
	// Demand nodes whose serving trains are checked per sample, and the idle share of the fleet below which it is
	// short of trains.
	constexpr int32 ServeChecksPerSample = 6;
	constexpr double IdleShareForShort = 0.2;
	// Supply that takes longer than this to fill one freight car is low: a trip for it carries a nearly empty car. [min]
	constexpr double LowSupplyMinutes = 30.0;

	FString NodeKey(const AFGTrainStationIdentifier* Station, const UClass* Item, ESTRole Role)
	{
		return FString::Printf(TEXT("%p|%p|%d"), Station, Item, static_cast<int32>(Role));
	}

	template <typename SampleType>
	void TrimWindow(TArray<SampleType>& Samples, double Time)
	{
		Samples.RemoveAll([Time](const SampleType& Each) { return Time - Each.Time > FSTThroughput::WindowSeconds; });
	}

	FString Percent(double Share)
	{
		return FString::Printf(TEXT("%.0f%%"), Share * 100.0);
	}

}

double FSTThroughput::FNodeHistory::Share(uint8 Flag) const
{
	if (Samples.Num() == 0)
	{
		return 0.0;
	}
	int32 Count = 0;
	for (const FNodeSample& Each : Samples)
	{
		Count += (Each.Flags & Flag) ? 1 : 0;
	}
	return static_cast<double>(Count) / Samples.Num();
}

double FSTThroughput::FNodeHistory::ShareOf(uint8 Flag, uint8 Within) const
{
	int32 Count = 0;
	int32 Total = 0;
	for (const FNodeSample& Each : Samples)
	{
		if (Each.Flags & Within)
		{
			++Total;
			Count += (Each.Flags & Flag) ? 1 : 0;
		}
	}
	return Total > 0 ? static_cast<double>(Count) / Total : 0.0;
}

void FSTThroughput::Sample(const ASTSubsystem& Subsystem)
{
	const double Time = Subsystem.Now();

	FFleetSample Fleet;
	Fleet.Time = Time;
	TSet<const AFGTrainStationIdentifier*> Occupied;
	for (const FSTTrainState& State : Subsystem.mTrains)
	{
		if (!IsValid(State.Train))
		{
			continue;
		}
		const bool bDocked = State.Train->IsDocked();
		if (bDocked && State.DockedStation.IsValid())
		{
			Occupied.Add(State.DockedStation.Get());
		}
		if (!State.bManaged)
		{
			continue;
		}
		++Fleet.Managed;
		if (!State.bHasJob)
		{
			++Fleet.Idle;
			const FSTStationConfig* Here = Subsystem.mStations.FindByPredicate([&](const FSTStationConfig& Each) { return Each.Station == State.LastStation.Get(); });
			Fleet.ParkedOutside += State.Phase == ESTTrainPhase::Parked && !(Here && Here->bDepot) ? 1 : 0;
		}
		else if (State.Job.bRepeat)
		{
			++Fleet.Repeating;
		}
		else if (State.Job.bPark)
		{
			++Fleet.Parking;
		}
		else if (bDocked)
		{
			++Fleet.Docked;
		}
		else
		{
			++Fleet.Driving;
			const AFGLocomotive* Master = State.Train->GetMultipleUnitMaster();
			const UFGLocomotiveMovementComponent* Movement = Master ? Master->GetLocomotiveMovementComponent() : nullptr;
			Fleet.Standing += Movement && FMath::Abs(Movement->GetForwardSpeed()) < StandingSpeed ? 1 : 0;
		}
	}
	mFleet.Add(Fleet);
	TrimWindow(mFleet, Time);

	TArray<FSTNode> Nodes;
	Subsystem.BuildNodes(Nodes);
	TSet<FString> Seen;
	TSet<const AFGTrainStationIdentifier*> Stations;
	for (const FSTNode& Node : Nodes)
	{
		if (Node.bAnyItem || !Node.Item || (Node.Role != ESTRole::Supply && Node.Role != ESTRole::Demand))
		{
			continue;
		}
		const FString Key = NodeKey(Node.Station.Get(), Node.Item.Get(), Node.Role);
		Seen.Add(Key);
		Stations.Add(Node.Station.Get());
		FNodeHistory& History = mNodes.FindOrAdd(Key);
		History.Station = Node.Station;
		History.Item = Node.Item.Get();
		History.Role = Node.Role;
		History.Platforms = Node.Platforms.Num();
		History.PlatformList = Node.Platforms;
		History.Rate = Node.Rate;
		History.Stock = Node.Stock;
		History.Capacity = Node.Capacity;
		uint8 Flags = 0;
		if (Node.Role == ESTRole::Demand && Node.Stock < Node.Capacity * Node.TargetPercent / 100.0)
		{
			Flags |= BelowTarget;
		}
		if (Node.bStarved)
		{
			Flags |= Starved;
		}
		if (Node.Capacity > 0 && Node.Stock >= 0.9 * Node.Capacity)
		{
			Flags |= Full;
		}
		if (Node.Stock <= 0.1 * Node.Capacity)
		{
			Flags |= Empty;
		}
		if (Node.Role == ESTRole::Demand && Node.Reserved > 0)
		{
			Flags |= OnTheWay;
		}
		if (Node.Role == ESTRole::Demand && Node.Stock <= 0)
		{
			Flags |= AllEmpty;
		}
		History.Samples.Add({Time, Flags});
		TrimWindow(History.Samples, Time);
	}
	for (auto It = mNodes.CreateIterator(); It; ++It)
	{
		if (!Seen.Contains(It.Key()))
		{
			It.RemoveCurrent();
		}
	}
	for (auto It = mServe.CreateIterator(); It; ++It)
	{
		if (!Seen.Contains(It.Key()))
		{
			It.RemoveCurrent();
		}
	}

	// Can a managed train serve each consumer? The ones checked longest ago first, a few per sample.
	TArray<const FSTNode*> Consumers;
	for (const FSTNode& Node : Nodes)
	{
		if (Node.Role == ESTRole::Demand && !Node.bAnyItem && Node.Item)
		{
			Consumers.Add(&Node);
		}
	}
	Consumers.Sort([this](const FSTNode& A, const FSTNode& B)
	{
		const FServe* First = mServe.Find(NodeKey(A.Station.Get(), A.Item.Get(), A.Role));
		const FServe* Second = mServe.Find(NodeKey(B.Station.Get(), B.Item.Get(), B.Role));
		return (First ? First->CheckedAt : -1000.0) < (Second ? Second->CheckedAt : -1000.0);
	});
	for (int32 Index = 0; Index < Consumers.Num() && Index < ServeChecksPerSample; ++Index)
	{
		const FSTNode& Consumer = *Consumers[Index];
		FServe Serve;
		Serve.CheckedAt = Time;
		for (const FSTTrainState& State : Subsystem.mTrains)
		{
			if (!State.bManaged || !IsValid(State.Train) || Serve.bFits)
			{
				continue;
			}
			for (const FSTNode& Supplier : Nodes)
			{
				if (Supplier.Role != ESTRole::Supply || Supplier.Item != Consumer.Item || Supplier.Station == Consumer.Station
					|| !Subsystem.CanServe(State, Supplier.Station.Get()) || !Subsystem.CanServe(State, Consumer.Station.Get())
					|| Subsystem.EstimateTravelSeconds(Supplier.Station.Get(), Consumer.Station.Get(), ASTSubsystem::IsPushPull(State.Train)) < 0.f)
				{
					continue;
				}
				Serve.bReach = true;
				const FSTFit Fit = Subsystem.ComputeFit(State.Train, Supplier.Station.Get(), Consumer.Station.Get(), Consumer.Item, true);
				if (Fit.Capacity > 0)
				{
					Serve.bFits = true;
					break;
				}
				if (Serve.Problem.IsEmpty())
				{
					Serve.Problem = FString::Printf(TEXT("%s: %s"), *STLayout::Name(State.Train), *Fit.Problem);
				}
			}
		}
		mServe.Add(NodeKey(Consumer.Station.Get(), Consumer.Item.Get(), Consumer.Role), Serve);
	}

	for (const AFGTrainStationIdentifier* Station : Stations)
	{
		TArray<FStationSample>& Samples = mOccupied.FindOrAdd(const_cast<AFGTrainStationIdentifier*>(Station));
		int32 Heading = 0;
		for (const FSTTrainState& State : Subsystem.mTrains)
		{
			if (State.bManaged && State.bHasJob && IsValid(State.Train) && State.Job.Legs.IsValidIndex(State.Job.Leg) && State.Job.Legs[State.Job.Leg].Station == Station
				&& !(State.Train->IsDocked() && State.DockedStation.Get() == Station))
			{
				++Heading;
			}
		}
		Samples.Add({Time, Occupied.Contains(Station), Heading});
		TrimWindow(Samples, Time);
	}
	for (auto It = mOccupied.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid() || !Stations.Contains(It.Key().Get()))
		{
			It.RemoveCurrent();
		}
	}
	for (auto It = mArrivals.CreateIterator(); It; ++It)
	{
		TrimWindow(It.Value(), Time);
		if (!It.Key().IsValid() || It.Value().Num() == 0)
		{
			It.RemoveCurrent();
		}
	}
}

void FSTThroughput::RecordArrival(const AFGTrainStationIdentifier* Station, double Late, double Time)
{
	if (Station)
	{
		mArrivals.FindOrAdd(const_cast<AFGTrainStationIdentifier*>(Station)).Add({Time, Late});
	}
}

FSTMonitorView FSTThroughput::MakeView(const ASTSubsystem& Subsystem) const
{
	FSTMonitorView View;
	const double Time = Subsystem.Now();
	const double Span = mFleet.Num() > 0 ? Time - mFleet[0].Time : 0.0;
	View.WindowMinutes = FMath::RoundToInt(Span / 60.0);

	// Fleet.
	// Busy: moving on trips or at stops; standing still on a trip (queueing or at signals) is congestion, not work.
	double Busy = 0.0;
	double Standing = 0.0;
	double Managed = 0.0;
	for (const FFleetSample& Each : mFleet)
	{
		Busy += Each.Driving - Each.Standing + Each.Docked;
		Standing += Each.Standing;
		Managed += Each.Managed;
	}
	const double BusyShareNow = Managed > 0.0 ? Busy / Managed : 0.0;
	const double StandingShareNow = Managed > 0.0 ? Standing / Managed : 0.0;
	const double IdleShareNow = mFleet.Num() > 0 && mFleet.Last().Managed > 0
		? static_cast<double>(mFleet.Last().Idle + mFleet.Last().Repeating) / mFleet.Last().Managed : 0.0;
	if (mFleet.Num() > 0)
	{
		const FFleetSample& Last = mFleet.Last();
		View.Summary.Add(FString::Printf(TEXT("%d managed trains: %d driving a trip (%d of them standing still), %d at a stop, %d repeating their last job (free for new trips), %d going to park, %d idle."),
			Last.Managed, Last.Driving, Last.Standing, Last.Docked, Last.Repeating, Last.Parking, Last.Idle));
		View.Summary.Add(FString::Printf(TEXT("Busy on trips %s of the time over the last %d min (moving or at a stop); standing still on trips %s."), *Percent(BusyShareNow),
			FMath::Max(1, View.WindowMinutes), *Percent(StandingShareNow)));
		if (StandingShareNow >= StandingShare)
		{
			View.Summary.Add(TEXT("Congested: trains stand still on their trips much of the time, queueing for stations or held at signals. Fewer, fuller visits help (shuttles, stops that carry more); more trains would only queue too."));
		}
		else if (BusyShareNow >= SaturatedShare)
		{
			View.Summary.Add(TEXT("Fleet saturated: nearly every train is moving on a trip or at a stop all the time. Items waiting for trains (Trains busy elsewhere) need more trains; better planning can't make up for it."));
		}
		if (Subsystem.mServices.Num() > 0)
		{
			for (const FString& Line : Subsystem.DescribeServices())
			{
				View.Summary.Add(TEXT("Service ") + Line);
			}
		}
		if (Last.ParkedOutside > 0)
		{
			int32 Depots = 0;
			for (const FSTStationConfig& Config : Subsystem.mStations)
			{
				Depots += Config.bDepot && IsValid(Config.Station) ? 1 : 0;
			}
			View.Summary.Add(FString::Printf(TEXT("%d idle trains are parked in stations that aren't depots, because no depot is free (%d depots). They move when a trip needs the station, but a depot for each (one train per depot, best without supply or demand platforms, off the main line) keeps stations clear."),
				Last.ParkedOutside, Depots));
		}
	}
	else
	{
		View.Summary.Add(TEXT("Measuring: the first sample comes within 10 seconds."));
	}
	{
		TArray<FSTNode> ChainNodes;
		Subsystem.BuildNodes(ChainNodes);
		int32 Shown = 0;
		for (const FSTNode& Node : ChainNodes)
		{
			if (Node.WaitsOn && Shown++ < 6)
			{
				View.Summary.Add(FString::Printf(TEXT("Supply chain: %s at %s waits on %s there (starved), so deliveries of it count less until that arrives."),
					*STNames::Item(Node.Item), *STLayout::Name(Node.Station.Get()), *STNames::Item(Node.WaitsOn)));
			}
		}
	}
	View.Summary.Add(Subsystem.IsFccAvailable() ? TEXT("Freight Car Control: on (cars can skip stops).")
		: TEXT("Freight Car Control: off (every car must fit the platform it stops beside)."));

	// Idle trains, grouped by reason.
	TMap<FString, int32> Reasons;
	for (const FSTTrainState& State : Subsystem.mTrains)
	{
		if (State.bManaged && !State.bHasJob && IsValid(State.Train))
		{
			const FString* Reason = Subsystem.mIdleReasons.Find(State.Train.Get());
			++Reasons.FindOrAdd(Reason ? *Reason : FString(TEXT("not planned yet")));
		}
	}
	Reasons.ValueSort([](int32 A, int32 B) { return A > B; });
	for (const TPair<FString, int32>& Each : Reasons)
	{
		View.Idle.Add(FString::Printf(TEXT("%d %s: %s"), Each.Value, Each.Value == 1 ? TEXT("train") : TEXT("trains"), *Each.Key));
	}
	// Auto Trains' troubled trains: deadlocked, stuck, lost or stalled.
	TMap<const AFGTrain*, FString> Trouble;
	for (const STAt::FTroubled& Each : STAt::GetTroubledTrains(Subsystem.GetWorld()))
	{
		FString& Text = Trouble.FindOrAdd(Each.Train);
		Text += FString::Printf(TEXT("%s%s for %.0f s"), Text.IsEmpty() ? TEXT("") : TEXT(", "), *Each.Kind, Each.Seconds);
	}
	for (const FSTTrainState& State : Subsystem.mTrains)
	{
		if (!State.bManaged || !IsValid(State.Train))
		{
			continue;
		}
		FSTMonitorRow& Row = View.Trains.AddDefaulted_GetRef();
		// The name the game shows; with job names on it already says the trip.
		Row.Title = STLayout::Name(State.Train.Get());
		const bool bDocked = State.Train->IsDocked();
		if (!State.bHasJob)
		{
			const FString* Reason = Subsystem.mIdleReasons.Find(State.Train.Get());
			Row.Verdict = TEXT("Idle");
			Row.Severity = Reason ? 1 : 0;
			Row.Details.Add(Reason ? *Reason : FString(TEXT("not planned yet")));
		}
		else
		{
			Row.Verdict = State.Job.bRepeat ? TEXT("Repeating last job")
				: (State.Job.bPark ? TEXT("Going to park") : (bDocked ? TEXT("At a stop") : TEXT("Driving")));
			if (!Subsystem.mSettings.bJobNames)
			{
				Row.Details.Add(State.Job.Summary);
			}
		}
		if (const FSTRosterDuty* Duty = Subsystem.mRoster.Find(State.Train.Get()))
		{
			Row.Details.Add(FString::Printf(TEXT("Roster: %s (%.0f%% of the train)."), *Subsystem.DescribeDuty(*Duty), Duty->Load * 100.0));
		}
		if (State.Requests.Num() > 0)
		{
			const bool bBringing = State.Requests.ContainsByPredicate([&](const FSTRequestClaim& Claim)
			{
				return State.bHasJob && State.Job.bRequest && ASTSubsystem::JobDelivers(State.Job, Claim, State.Job.Leg);
			});
			Row.Verdict = bBringing ? TEXT("Bringing a request") : TEXT("Emptying for a request");
			for (const FSTRequestClaim& Claim : State.Requests)
			{
				Row.Details.Add(FString::Printf(TEXT("Request: %s."), *Subsystem.DescribeRequest(State, Claim)));
			}
		}
		Row.Columns.Add(State.LastStation ? STLayout::Name(State.LastStation.Get()) : FString(TEXT("-")));
		if (const FString* Problem = Trouble.Find(State.Train.Get()))
		{
			Row.Severity = 2;
			Row.Details.Insert(FString::Printf(TEXT("Auto Trains: %s."), **Problem), 0);
		}
	}
	View.Trains.Sort([](const FSTMonitorRow& A, const FSTMonitorRow& B)
	{
		return A.Severity != B.Severity ? A.Severity > B.Severity : A.Title < B.Title;
	});

	// Stations: occupancy and late arrivals.
	TSet<const AFGTrainStationIdentifier*> Congested;
	// Average lateness of arrivals per station (with enough arrivals to judge). [s]
	TMap<const AFGTrainStationIdentifier*, double> LateAt;
	for (const TPair<TWeakObjectPtr<AFGTrainStationIdentifier>, TArray<FStationSample>>& Pair : mOccupied)
	{
		if (!Pair.Key.IsValid() || Pair.Value.Num() < MinSamples)
		{
			continue;
		}
		int32 Count = 0;
		int32 HeadingSum = 0;
		int32 HeadingMax = 0;
		for (const FStationSample& Each : Pair.Value)
		{
			Count += Each.bOccupied ? 1 : 0;
			HeadingSum += Each.Heading;
			HeadingMax = FMath::Max(HeadingMax, Each.Heading);
		}
		const double Share = static_cast<double>(Count) / Pair.Value.Num();
		const double Heading = static_cast<double>(HeadingSum) / Pair.Value.Num();
		double Late = 0.0;
		int32 Arrivals = 0;
		if (const TArray<FArrival>* Found = mArrivals.Find(Pair.Key))
		{
			for (const FArrival& Each : *Found)
			{
				Late += Each.Late;
			}
			Arrivals = Found->Num();
			Late = Arrivals > 0 ? Late / Arrivals : 0.0;
		}
		const bool bLate = Arrivals >= MinArrivals && Late >= CongestedLate;
		if (Arrivals >= MinArrivals)
		{
			LateAt.Add(Pair.Key.Get(), Late);
		}
		FSTMonitorRow& Row = View.Stations.AddDefaulted_GetRef();
		Row.Title = STLayout::Name(Pair.Key.Get());
		Row.Columns = {Percent(Share), FString::FromInt(Arrivals), Arrivals > 0 ? FString::Printf(TEXT("%+.0f s"), Late) : FString(TEXT("-"))};
		if (Heading >= QueueTrains)
		{
			Congested.Add(Pair.Key.Get());
			Row.Verdict = TEXT("Bottleneck");
			Row.Severity = 2;
			Row.Details.Add(FString::Printf(TEXT("On average %.1f managed trains are on their way here at once (up to %d), so they queue before it."), Heading, HeadingMax));
			Row.Details.Add(FString::Printf(TEXT("A train is docked here %s of the time."), *Percent(Share)));
			Row.Advice = TEXT("Too many trips use this station: add bays or a second station for some of its items, or a waiting bay before it so queues don't block junctions.");
			continue;
		}
		if (Share < CongestedShare && !bLate)
		{
			Row.Verdict = TEXT("OK");
			continue;
		}
		Congested.Add(Pair.Key.Get());
		Row.Verdict = TEXT("Congested");
		Row.Severity = 2;
		Row.Details.Add(FString::Printf(TEXT("A train is docked here %s of the time."), *Percent(Share)));
		if (Arrivals > 0)
		{
			Row.Details.Add(FString::Printf(TEXT("%d arrivals, on average %.0f s later than planned."), Arrivals, Late));
		}
		Row.Advice = TEXT("More platforms, a second station for the same items, or a waiting bay before it help.");
	}

	// Items.
	struct FItemGroup
	{
		TArray<const FNodeHistory*> Suppliers;
		TArray<const FNodeHistory*> Consumers;
	};
	TMap<UClass*, FItemGroup> Groups;
	for (const TPair<FString, FNodeHistory>& Pair : mNodes)
	{
		if (UClass* Item = Pair.Value.Item.Get())
		{
			FItemGroup& Group = Groups.FindOrAdd(Item);
			(Pair.Value.Role == ESTRole::Supply ? Group.Suppliers : Group.Consumers).Add(&Pair.Value);
		}
	}
	// Average share of the window its consumers were empty.
	auto StarvedShareOf = [](const TArray<const FNodeHistory*>& Consumers)
	{
		double Share = 0.0;
		for (const FNodeHistory* Each : Consumers)
		{
			Share += Each->Share(ENodeFlag::Starved) / FMath::Max(1, Consumers.Num());
		}
		return Share;
	};
	// A platform group's row title: the station, then each of its platforms by position (P1 is the first from the
	// station building, as in the log) with how full it is now.
	auto GroupTitle = [](const FNodeHistory& History, UClass* Item)
	{
		const AFGTrainStationIdentifier* Station = History.Station.Get();
		const TArray<AFGBuildableTrainPlatform*> All = STLayout::GetPlatforms(Station ? Station->GetStation() : nullptr);
		TArray<TPair<int32, FString>> Parts;
		for (const TWeakObjectPtr<AFGBuildableTrainPlatformCargo>& Platform : History.PlatformList)
		{
			const UFGInventoryComponent* Inventory = Platform.IsValid() ? Platform->GetInventory() : nullptr;
			const int32 Capacity = Inventory ? STLayout::GetCapacity(Inventory, Item) : 0;
			const double Fill = Capacity > 0 ? static_cast<double>(Inventory->GetNumItems(Item)) / Capacity : 0.0;
			const int32 Position = Platform.IsValid() ? All.IndexOfByKey(Platform.Get()) + 1 : 0;
			Parts.Emplace(Position, FString::Printf(TEXT("P%d %s"), Position,
				Fill <= 0.0 ? TEXT("empty") : (Fill >= 0.995 ? TEXT("full") : *Percent(Fill))));
		}
		Parts.Sort([](const TPair<int32, FString>& A, const TPair<int32, FString>& B) { return A.Key < B.Key; });
		TArray<FString> Texts;
		for (const TPair<int32, FString>& Each : Parts)
		{
			Texts.Add(Each.Value);
		}
		return FString::Printf(TEXT("%s\n%d %s: %s"), *STLayout::Name(Station), Parts.Num(), Parts.Num() == 1 ? TEXT("platform") : TEXT("platforms"),
			*FString::Join(Texts, TEXT(", ")));
	};
	// A freight car of the fleet, for how long supply takes to fill one.
	const AFGFreightWagon* AnyWagon = nullptr;
	for (const FSTTrainState& State : Subsystem.mTrains)
	{
		for (AFGRailroadVehicle* Vehicle : IsValid(State.Train) && !AnyWagon ? STLayout::GetVehicles(State.Train) : TArray<AFGRailroadVehicle*>())
		{
			AnyWagon = AnyWagon ? AnyWagon : Cast<AFGFreightWagon>(Vehicle);
		}
	}
	for (const TPair<UClass*, FItemGroup>& Pair : Groups)
	{
		UClass* Item = Pair.Key;
		const FItemGroup& Group = Pair.Value;
		FSTMonitorRow& Row = View.Items.AddDefaulted_GetRef();
		Row.Title = STNames::Item(Item);
		Row.Item = Item;

		double SupplyRate = 0.0;
		double DemandRate = 0.0;
		int32 SupplyPlatforms = 0;
		int32 DemandPlatforms = 0;
		int32 Samples = TNumericLimits<int32>::Max();
		double SupplierFull = 0.0;
		double SupplierEmpty = 0.0;
		for (const FNodeHistory* Each : Group.Suppliers)
		{
			SupplyRate += Each->Rate * 60.0;
			SupplyPlatforms += Each->Platforms;
			Samples = FMath::Min(Samples, Each->Samples.Num());
			SupplierFull += Each->Share(ENodeFlag::Full) / Group.Suppliers.Num();
			SupplierEmpty += Each->Share(ENodeFlag::Empty) / Group.Suppliers.Num();
		}
		double Low = 0.0;
		double StarvedShare = 0.0;
		bool bAnyStarvedNow = false;
		const FNodeHistory* Worst = nullptr;
		for (const FNodeHistory* Each : Group.Consumers)
		{
			DemandRate += Each->Rate * 60.0;
			DemandPlatforms += Each->Platforms;
			Samples = FMath::Min(Samples, Each->Samples.Num());
			const double ThisLow = Each->Share(ENodeFlag::BelowTarget);
			Low += ThisLow / Group.Consumers.Num();
			StarvedShare += Each->Share(ENodeFlag::Starved) / Group.Consumers.Num();
			bAnyStarvedNow |= Each->Samples.Num() > 0 && (Each->Samples.Last().Flags & ENodeFlag::Starved);
			if (!Worst || ThisLow > Worst->Share(ENodeFlag::BelowTarget))
			{
				Worst = Each;
			}
		}
		// Full supply platforms stop their belts, so their measured rate only shows what trains took.
		const bool bSuppliersFull = SupplierFull >= FullShare;
		Row.Details.Add(FString::Printf(TEXT("Supply %s%s/min at %d platforms in %d stations; demand %s%s/min at %d platforms in %d stations."),
			bSuppliersFull ? TEXT("at least ") : TEXT(""), *STNames::Amount(Item, SupplyRate), SupplyPlatforms, Group.Suppliers.Num(),
			bAnyStarvedNow ? TEXT("at least ") : TEXT(""), *STNames::Amount(Item, DemandRate), DemandPlatforms, Group.Consumers.Num()));
		const ASTSubsystem::FAdvice* Advice = Subsystem.mAdvice.Find(Item);
		Row.Columns = {
			Group.Suppliers.Num() > 0 ? FString::Printf(TEXT("%s%s"), bSuppliersFull ? TEXT(">") : TEXT(""), *STNames::Amount(Item, SupplyRate)) : FString(TEXT("-")),
			Group.Consumers.Num() > 0 ? FString::Printf(TEXT("%s%s"), bAnyStarvedNow ? TEXT(">") : TEXT(""), *STNames::Amount(Item, DemandRate)) : FString(TEXT("-")),
			Advice && Advice->Pairs > 0 ? FString::Printf(TEXT("%.1f / %d"), Advice->TrainsNeeded, Advice->TrainsFitting) : FString(TEXT("-")),
			Group.Consumers.Num() > 0 ? Percent(Low) : FString(TEXT("-"))};
		if (Advice && Advice->Pairs > 0)
		{
			Row.Details.Add(FString::Printf(TEXT("Flow plan: %s/min over %d routes, needs about %.1f trains; %d managed trains could serve those routes."),
				*STNames::Amount(Item, Advice->FlowPerMinute), Advice->Pairs, Advice->TrainsNeeded, Advice->TrainsFitting));
		}
		if (Group.Consumers.Num() > 0 && Group.Suppliers.Num() > 0)
		{
			Row.Details.Add(FString::Printf(TEXT("Over %d min: consumers below target %s of the time (starved %s); suppliers full %s, nearly empty %s."),
				FMath::Max(1, View.WindowMinutes), *Percent(Low), *Percent(StarvedShare), *Percent(SupplierFull), *Percent(SupplierEmpty)));
		}

		// Starvation: deliveries on the way but late (congestion), or none on the way (no trips).
		double StarvedWaiting = 0.0;
		int32 StarvedConsumers = 0;
		for (const FNodeHistory* Each : Group.Consumers)
		{
			if (Each->Share(ENodeFlag::Starved) > 0.0)
			{
				StarvedWaiting += Each->ShareOf(ENodeFlag::OnTheWay, ENodeFlag::Starved);
				++StarvedConsumers;
			}
		}
		if (StarvedConsumers > 0)
		{
			Row.Details.Add(FString::Printf(TEXT("While its consumers were empty, a delivery was on its way %s of the time (late trains: congestion) and none %s (no trips: starvation)."),
				*Percent(StarvedWaiting / StarvedConsumers), *Percent(1.0 - StarvedWaiting / StarvedConsumers)));
		}

		// Verdict: what limits this item.
		bool bCongested = false;
		for (const FNodeHistory* Each : Group.Consumers)
		{
			bCongested |= Congested.Contains(Each->Station.Get());
		}
		for (const FNodeHistory* Each : Group.Suppliers)
		{
			bCongested |= Congested.Contains(Each->Station.Get());
		}
		if (Group.Consumers.Num() == 0)
		{
			Row.Verdict = TEXT("No demand");
			Row.Severity = SupplierFull >= FullShare ? 1 : 0;
			Row.Details.Add(TEXT("No demand platform takes it, so its supply platforms fill up."));
			Row.Advice = TEXT("Set a platform where it is used to Demand.");
		}
		else if (Group.Suppliers.Num() == 0)
		{
			Row.Verdict = TEXT("No supply");
			Row.Severity = 2;
			Row.Details.Add(TEXT("No supply platform offers it."));
			Row.Advice = TEXT("Set a platform where it is made to Supply.");
		}
		else if (Samples < MinSamples)
		{
			Row.Verdict = TEXT("Measuring");
			Row.Severity = 0;
		}
		else if (Low < LowShare)
		{
			Row.Verdict = TEXT("OK");
			Row.Severity = 0;
		}
		else if (const FServe* Serve = Worst ? mServe.Find(NodeKey(Worst->Station.Get(), Item, ESTRole::Demand)) : nullptr; Serve && !Serve->bFits)
		{
			// The consumer that waits most has no managed train that can serve it.
			Row.Verdict = Serve->bReach ? TEXT("No train fits") : TEXT("Unreachable");
			Row.Severity = 2;
			Row.Details.Add(Serve->bReach
				? FString::Printf(TEXT("No managed train fits a trip to %s from a supplier of it. First problem: %s"), *STLayout::Name(Worst->Station.Get()), *Serve->Problem)
				: FString::Printf(TEXT("No managed train can drive to %s from a supplier of it."), *STLayout::Name(Worst->Station.Get())));
			Row.Advice = Serve->bReach
				? FString::Printf(TEXT("Check the platform roles and items at %s, and the cars' order (/st fit on a train)."), *STLayout::Name(Worst->Station.Get()))
				: FString::Printf(TEXT("Connect %s to a supplier's track, or manage a train on its network."), *STLayout::Name(Worst->Station.Get()));
		}
		else if (SupplierEmpty >= EmptyShare || (SupplyRate < DemandRate * 0.9 && !bSuppliersFull))
		{
			Row.Verdict = TEXT("Supply short");
			Row.Severity = 2;
			Row.Details.Add(SupplyRate < DemandRate
				? FString::Printf(TEXT("Demand is %s/min more than supply."), *STNames::Amount(Item, DemandRate - SupplyRate))
				: FString(TEXT("Its supply platforms are mostly empty: trains take all that is made.")));
			Row.Advice = TEXT("Build more production, or add supply platforms where it is made.");
		}
		else if (StarvedShareOf(Group.Consumers) >= EmptyShare && !bSuppliersFull
			&& (SupplyRate <= 0.0 || STLayout::GetCarCapacity(AnyWagon, Item) / SupplyRate > LowSupplyMinutes))
		{
			// Starved consumers use nothing, so their demand can't be measured; the supply alone shows the limit.
			Row.Verdict = TEXT("Low supply");
			Row.Severity = 2;
			Row.Details.Add(SupplyRate > 0.0
				? FString::Printf(TEXT("Its supply makes %s/min; one freight car of it takes %.0f minutes to fill, so a trip for it carries a nearly empty car and is rarely worth a train."),
					*STNames::Amount(Item, SupplyRate), STLayout::GetCarCapacity(AnyWagon, Item) / SupplyRate)
				: FString(TEXT("Its supply platforms get none of it.")));
			Row.Advice = TEXT("Build more production, or set its demand platforms to a higher priority so smaller loads go.");
		}
		else if (Advice && Advice->TrainsNeeded > Advice->TrainsFitting + 0.5 && BusyShareNow >= BusyShare && IdleShareNow < IdleShareForShort)
		{
			// The flow plan needs more trains than fit these routes, and the fleet has none to spare.
			Row.Verdict = TEXT("Trains short");
			Row.Severity = 2;
			const int32 More = FMath::Max(1, FMath::CeilToInt(Advice->TrainsNeeded - Advice->TrainsFitting));
			Row.Details.Add(FString::Printf(TEXT("Its routes need about %.1f trains and %d fit; the fleet is busy %s of the time."), Advice->TrainsNeeded,
				Advice->TrainsFitting, *Percent(BusyShareNow)));
			Row.Advice = FString::Printf(TEXT("Add about %d %s whose cars fit these platforms."), More, More == 1 ? TEXT("train") : TEXT("trains"));
		}
		else if (SupplierFull >= FullShare && BusyShareNow >= BusyShare)
		{
			// Enough trains fit, but they are on other trips while this supply backs up.
			Row.Verdict = TEXT("Trains busy elsewhere");
			Row.Severity = 2;
			Row.Details.Add(FString::Printf(TEXT("%d managed trains fit its routes, but the fleet is busy %s of the time on other trips; its suppliers are full %s of the time."),
				Advice ? Advice->TrainsFitting : 0, *Percent(BusyShareNow), *Percent(SupplierFull)));
			Row.Advice = TEXT("Raise this item's priority on its demand platforms, or add a train whose cars fit them.");
		}
		else if (bCongested)
		{
			Row.Verdict = TEXT("Congested");
			Row.Severity = 2;
			Row.Details.Add(TEXT("One of its stations is congested (Stations tab)."));
			Row.Advice = TEXT("More platforms, a second station for the same items, or a waiting bay before it help.");
		}
		else
		{
			Row.Verdict = TEXT("No trips planned");
			Row.Severity = 2;
			Row.Details.Add(TEXT("Its supply is there and trains are free, but SmartTrains plans no trip for it."));
			Row.Advice = TEXT("Check its platforms' roles and items, and why free trains wait (Trains tab). With Detailed log on, PLANNER lines say why its trips score nothing.");
		}
		if (Worst && Row.Severity > 0 && Group.Consumers.Num() > 1)
		{
			Row.Details.Add(FString::Printf(TEXT("Waits most: %s (below target %s of the time)."), *STLayout::Name(Worst->Station.Get()), *Percent(Worst->Share(ENodeFlag::BelowTarget))));
		}

		// Platforms that need a look.
		for (const FNodeHistory* Each : Group.Consumers)
		{
			if (Each->Samples.Num() >= MinSamples && Each->Share(ENodeFlag::Starved) >= 0.5)
			{
				FSTMonitorRow& Platform = View.Platforms.AddDefaulted_GetRef();
				Platform.Title = GroupTitle(*Each, Item);
				Platform.Item = Item;
				const double Waiting = Each->ShareOf(ENodeFlag::OnTheWay, ENodeFlag::Starved);
				const double* Late = LateAt.Find(Each->Station.Get());
				// Starved: all its platforms empty most of the time; partly: some empty while others hold stock.
				const bool bAll = Each->Share(ENodeFlag::AllEmpty) >= 0.5;
				const FString Kind = bAll ? TEXT("Starved") : TEXT("Partly starved");
				Platform.Severity = 2;
				Platform.Columns = {Row.Title, Percent(Each->Share(ENodeFlag::Starved))};
				Platform.Details.Add(Each->Platforms > 1
					? FString::Printf(TEXT("Wants %s: some of its platforms were empty %s of the time, all of them %s."), *Row.Title,
						*Percent(Each->Share(ENodeFlag::Starved)), *Percent(Each->Share(ENodeFlag::AllEmpty)))
					: FString::Printf(TEXT("Wants %s and was empty %s of the time."), *Row.Title, *Percent(Each->Share(ENodeFlag::Starved))));
				if (Waiting >= 0.5)
				{
					// Deliveries are coming but don't arrive in time: trains are held up on the way or at the station.
					Platform.Verdict = Kind + TEXT(": trains held up");
					Platform.Details.Add(FString::Printf(TEXT("A delivery was on its way %s of the time it was empty, so trains are held up on the way or queue at the station%s."),
						*Percent(Waiting), Late ? *FString::Printf(TEXT(" (arrivals here %+.0f s against plan on average)"), *Late) : TEXT("")));
					Platform.Advice = TEXT("Congestion: see this station in the Stations tab. More platforms, a second station, or a waiting bay help.");
				}
				else
				{
					// No delivery is coming most of the time: no trip is planned for it.
					Platform.Verdict = Kind + TEXT(": no trips");
					Platform.Details.Add(FString::Printf(TEXT("No delivery was on its way %s of the time it was empty, so no train was sent."), *Percent(1.0 - Waiting)));
					Platform.Advice = Row.Severity > 0
						? FString::Printf(TEXT("Starvation: see %s in the Items tab (%s)."), *Row.Title, *Row.Verdict)
						: FString::Printf(TEXT("Other demand platforms for %s are fine; this one gets too few trips. Raise its priority, or check that trains' cars line up with it (/st fit)."), *Row.Title);
				}
			}
		}
		for (const FNodeHistory* Each : Group.Suppliers)
		{
			if (Each->Samples.Num() >= MinSamples && Each->Share(ENodeFlag::Full) >= 0.5)
			{
				FSTMonitorRow& Platform = View.Platforms.AddDefaulted_GetRef();
				Platform.Title = GroupTitle(*Each, Item);
				Platform.Item = Item;
				Platform.Verdict = TEXT("Backing up");
				Platform.Severity = 1;
				Platform.Columns = {Row.Title, Percent(Each->Share(ENodeFlag::Full))};
				Platform.Details.Add(FString::Printf(TEXT("Supplies %s and was full %s of the time, so its belts or pipes back up."), *Row.Title, *Percent(Each->Share(ENodeFlag::Full))));
			}
		}
	}
	// Requests: platforms a train is emptied for, and platforms that ask without a train yet.
	for (const FSTTrainState& State : Subsystem.mTrains)
	{
		for (const FSTRequestClaim& Claim : State.Requests)
		{
			FSTMonitorRow& Row = View.Platforms.AddDefaulted_GetRef();
			Row.Title = STLayout::DescribePlatform(Claim.Platform);
			Row.Item = Claim.Item;
			Row.Verdict = TEXT("Requested: train on it");
			Row.Severity = 1;
			Row.Columns = {STNames::Item(Claim.Item), TEXT("-")};
			Row.Details.Add(FString::Printf(TEXT("%s."), *Subsystem.DescribeRequest(State, Claim)));
		}
	}
	TArray<const FSTRequestNote*> Notes;
	for (const TPair<const AFGBuildableTrainPlatformCargo*, FSTRequestNote>& Pair : Subsystem.mRequestNotes)
	{
		if (Pair.Value.Platform.IsValid() && Pair.Value.Kind != TEXT("covered") && Pair.Value.BelowAt <= Time + RequestRowSeconds)
		{
			Notes.Add(&Pair.Value);
		}
	}
	Notes.Sort([](const FSTRequestNote& A, const FSTRequestNote& B) { return A.BelowAt < B.BelowAt; });
	Notes.SetNum(FMath::Min(Notes.Num(), MaxRequestRows));
	for (const FSTRequestNote* Each : Notes)
	{
		const FSTRequestNote& Note = *Each;
		FSTMonitorRow& Row = View.Platforms.AddDefaulted_GetRef();
		Row.Title = STLayout::DescribePlatform(Note.Platform.Get());
		Row.Item = Note.Item.Get();
		const bool bNoTrain = Note.Kind == TEXT("no train");
		Row.Verdict = bNoTrain ? TEXT("Requested: no train") : (Note.Kind == TEXT("later") ? TEXT("Requested: train later") : TEXT("Requested: covered"));
		Row.Severity = bNoTrain ? 2 : 0;
		Row.Columns = {STNames::Item(Note.Item.Get()), TEXT("-")};
		Row.Details.Add(Note.Text + TEXT("."));
		if (bNoTrain)
		{
			Row.Advice = TEXT("No free train has a car that lines up with this platform and can be emptied: give it a train whose cars line up, or a sink or consumer for what they hold.");
		}
	}
	AddUnreachable(Subsystem, View);
	auto BySeverity = [](const FSTMonitorRow& A, const FSTMonitorRow& B)
	{
		return A.Severity != B.Severity ? A.Severity > B.Severity : A.Title < B.Title;
	};
	View.Items.Sort(BySeverity);
	View.Stations.Sort(BySeverity);
	View.Platforms.Sort(BySeverity);

	auto AddTile = [&View](const FString& Label, const FString& Value, uint8 Severity)
	{
		FSTMonitorRow& Tile = View.Tiles.AddDefaulted_GetRef();
		Tile.Title = Label;
		Tile.Verdict = Value;
		Tile.Severity = Severity;
	};
	int32 Limited = 0;
	for (const FSTMonitorRow& Row : View.Items)
	{
		Limited += Row.Severity >= 2 ? 1 : 0;
	}
	const int32 ManagedNow = mFleet.Num() > 0 ? mFleet.Last().Managed : 0;
	const int32 IdleNow = mFleet.Num() > 0 ? mFleet.Last().Idle + mFleet.Last().Repeating : 0;
	AddTile(TEXT("Managed trains"), FString::FromInt(ManagedNow), 0);
	AddTile(TEXT("Busy on trips"), mFleet.Num() > 0 ? Percent(BusyShareNow) : FString(TEXT("-")), BusyShareNow >= BusyShare ? 1 : 0);
	AddTile(TEXT("Free now (idle or repeating)"), FString::FromInt(IdleNow), 0);
	AddTile(TEXT("Items limited"), FString::FromInt(Limited), Limited > 0 ? 2 : 0);
	AddTile(TEXT("Congested stations"), FString::FromInt(Congested.Num()), Congested.Num() > 0 ? 2 : 0);
	return View;
}

TArray<FString> FSTThroughput::Describe(const FSTMonitorView& View)
{
	TArray<FString> Lines = View.Summary;
	for (const FSTMonitorRow& Row : View.Items)
	{
		if (Row.Severity > 0)
		{
			Lines.Add(FString::Printf(TEXT("  %s: %s. %s%s%s"), *Row.Title, *Row.Verdict, *FString::Join(Row.Details, TEXT(" ")), Row.Advice.IsEmpty() ? TEXT("") : TEXT(" "),
				*Row.Advice));
		}
	}
	int32 Fine = 0;
	for (const FSTMonitorRow& Row : View.Items)
	{
		Fine += Row.Severity == 0 ? 1 : 0;
	}
	if (Fine > 0)
	{
		Lines.Add(FString::Printf(TEXT("  %d items OK or still measuring."), Fine));
	}
	for (const FSTMonitorRow& Row : View.Stations)
	{
		if (Row.Severity == 0)
		{
			continue;
		}
		Lines.Add(FString::Printf(TEXT("  Station %s: %s. %s"), *Row.Title, *Row.Verdict, *FString::Join(Row.Details, TEXT(" "))));
	}
	for (const FSTMonitorRow& Row : View.Platforms)
	{
		Lines.Add(FString::Printf(TEXT("  %s: %s %s. %s"), *Row.Verdict, *Row.Title.Replace(TEXT("\n"), TEXT(" - ")), Row.Columns.Num() > 0 ? *Row.Columns[0] : TEXT(""), *FString::Join(Row.Details, TEXT(" "))));
	}
	for (const FString& Line : View.Idle)
	{
		Lines.Add(TEXT("  Idle: ") + Line);
	}
	return Lines;
}

void FSTThroughput::AddUnreachable(const ASTSubsystem& Subsystem, FSTMonitorView& View) const
{
	// Per train and station, the vehicle beside each platform for every way it docks (computed once per pair).
	TMap<TPair<const AFGTrain*, const AFGTrainStationIdentifier*>, TArray<TArray<AFGRailroadVehicle*>>> Maps;
	for (const FSTPlatformConfig& Config : Subsystem.mPlatforms)
	{
		const AFGBuildableTrainPlatformCargo* Platform = Config.Platform;
		const UFGInventoryComponent* Inventory = IsValid(Platform) ? Platform->GetInventory() : nullptr;
		if (!Inventory || (Config.Role != ESTRole::Supply && Config.Role != ESTRole::Demand) || Config.Items.Num() == 0)
		{
			continue;
		}
		// Only where it matters: a supplier holding its items, a consumer that has run out.
		int32 Stock = 0;
		UClass* First = nullptr;
		for (const FSTItemRule& Rule : Config.Items)
		{
			if (Rule.Item)
			{
				Stock += Inventory->GetNumItems(Rule.Item);
				First = First ? First : Rule.Item.Get();
			}
		}
		if (!First || (Config.Role == ESTRole::Supply ? Stock <= 0 : Stock > 0))
		{
			continue;
		}
		int32 Index = INDEX_NONE;
		const AFGTrainStationIdentifier* Station = STLayout::GetIdentifier(STLayout::FindStation(Platform, &Index));
		if (!Station || Index == INDEX_NONE)
		{
			continue;
		}
		const EFreightCargoType Type = STLayout::GetPlatformCargoType(Platform);
		bool bReached = false;
		for (const FSTTrainState& State : Subsystem.mTrains)
		{
			if (bReached || !State.bManaged || !IsValid(State.Train) || !Subsystem.CanServe(State, Station))
			{
				continue;
			}
			const TPair<const AFGTrain*, const AFGTrainStationIdentifier*> Key(State.Train.Get(), Station);
			if (!Maps.Contains(Key))
			{
				Maps.Add(Key, Subsystem.GetDockMaps(State.Train, Station));
			}
			for (const TArray<AFGRailroadVehicle*>& Map : Maps[Key])
			{
				const AFGFreightWagon* Car = Map.IsValidIndex(Index) ? Cast<AFGFreightWagon>(Map[Index]) : nullptr;
				bReached |= Car && STLayout::GetCargoType(Car) == Type;
			}
		}
		if (bReached)
		{
			continue;
		}
		const bool bFluid = Type == EFreightCargoType::FCT_Liquid;
		FSTMonitorRow& Row = View.Platforms.AddDefaulted_GetRef();
		Row.Title = STLayout::DescribePlatform(Platform);
		Row.Item = First;
		Row.Verdict = TEXT("Unreachable");
		Row.Severity = 2;
		Row.Columns = {STNames::Item(First), TEXT("-")};
		Row.Details.Add(FString::Printf(TEXT("No managed train has a %s car that stops beside P%d, so %s %s never %s here."), bFluid ? TEXT("fluid") : TEXT("freight"), Index + 1,
			*STNames::Item(First), Config.Role == ESTRole::Supply ? TEXT("is") : TEXT("is"), Config.Role == ESTRole::Supply ? TEXT("picked up") : TEXT("delivered")));
		Row.Advice = FString::Printf(TEXT("Give a train a %s car in the position that stops at P%d, or move the %s to a platform the trains reach."), bFluid ? TEXT("fluid") : TEXT("freight"),
			Index + 1, bFluid ? TEXT("pipes") : TEXT("belts"));
	}
}
