#include "STThroughput.h"
#include "FGTrain.h"
#include "FGTrainStationIdentifier.h"
#include "SmartTrains.h"
#include "STLayout.h"
#include "STBridges.h"
#include "STSubsystem.h"

namespace
{
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
	constexpr int32 MinArrivals = 3;
	// Demand nodes whose serving trains are checked per sample, and the idle share of the fleet below which it is
	// short of trains.
	constexpr int32 ServeChecksPerSample = 6;
	constexpr double IdleShareForShort = 0.2;

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
				const FSTFit Fit = Subsystem.ComputeFit(State.Train, Supplier.Station.Get(), Consumer.Station.Get(), Consumer.Item);
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
		Samples.Add({Time, Occupied.Contains(Station)});
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
	double Busy = 0.0;
	double Managed = 0.0;
	for (const FFleetSample& Each : mFleet)
	{
		Busy += Each.Driving + Each.Docked;
		Managed += Each.Managed;
	}
	const double BusyShareNow = Managed > 0.0 ? Busy / Managed : 0.0;
	const double IdleShareNow = mFleet.Num() > 0 && mFleet.Last().Managed > 0 ? static_cast<double>(mFleet.Last().Idle) / mFleet.Last().Managed : 0.0;
	if (mFleet.Num() > 0)
	{
		const FFleetSample& Last = mFleet.Last();
		View.Summary.Add(FString::Printf(TEXT("%d managed trains: %d driving a trip, %d at a stop, %d going to park, %d idle."), Last.Managed, Last.Driving,
			Last.Docked, Last.Parking, Last.Idle));
		View.Summary.Add(FString::Printf(TEXT("Busy on trips %s of the time over the last %d min."), *Percent(BusyShareNow), FMath::Max(1, View.WindowMinutes)));
	}
	else
	{
		View.Summary.Add(TEXT("Measuring: the first sample comes within 10 seconds."));
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
			Row.Verdict = State.Job.bPark ? TEXT("Going to park") : (bDocked ? TEXT("At a stop") : TEXT("Driving"));
			if (!Subsystem.mSettings.bJobNames)
			{
				Row.Details.Add(State.Job.Summary);
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
	for (const TPair<TWeakObjectPtr<AFGTrainStationIdentifier>, TArray<FStationSample>>& Pair : mOccupied)
	{
		if (!Pair.Key.IsValid() || Pair.Value.Num() < MinSamples)
		{
			continue;
		}
		int32 Count = 0;
		for (const FStationSample& Each : Pair.Value)
		{
			Count += Each.bOccupied ? 1 : 0;
		}
		const double Share = static_cast<double>(Count) / Pair.Value.Num();
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
		FSTMonitorRow& Row = View.Stations.AddDefaulted_GetRef();
		Row.Title = STLayout::Name(Pair.Key.Get());
		Row.Columns = {Percent(Share), FString::FromInt(Arrivals), Arrivals > 0 ? FString::Printf(TEXT("%+.0f s"), Late) : FString(TEXT("-"))};
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
			Row.Details.Add(FString::Printf(TEXT("Flow plan: %s/min over %d routes, needs about %.1f trains; %d managed trains fit those routes."),
				*STNames::Amount(Item, Advice->FlowPerMinute), Advice->Pairs, Advice->TrainsNeeded, Advice->TrainsFitting));
		}
		if (Group.Consumers.Num() > 0 && Group.Suppliers.Num() > 0)
		{
			Row.Details.Add(FString::Printf(TEXT("Over %d min: consumers below target %s of the time (starved %s); suppliers full %s, nearly empty %s."),
				FMath::Max(1, View.WindowMinutes), *Percent(Low), *Percent(StarvedShare), *Percent(SupplierFull), *Percent(SupplierEmpty)));
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
			Row.Verdict = TEXT("Blocked");
			Row.Severity = 2;
			Row.Details.Add(TEXT("Supply waits and trains are free, but no trip is made."));
			Row.Advice = TEXT("See why idle trains wait (Trains tab).");
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
				Platform.Title = STLayout::Name(Each->Station.Get());
				Platform.Item = Item;
				Platform.Verdict = TEXT("Starved");
				Platform.Severity = 2;
				Platform.Columns = {Row.Title, Percent(Each->Share(ENodeFlag::Starved))};
				Platform.Details.Add(FString::Printf(TEXT("Wants %s and was empty %s of the time."), *Row.Title, *Percent(Each->Share(ENodeFlag::Starved))));
			}
		}
		for (const FNodeHistory* Each : Group.Suppliers)
		{
			if (Each->Samples.Num() >= MinSamples && Each->Share(ENodeFlag::Full) >= 0.5)
			{
				FSTMonitorRow& Platform = View.Platforms.AddDefaulted_GetRef();
				Platform.Title = STLayout::Name(Each->Station.Get());
				Platform.Item = Item;
				Platform.Verdict = TEXT("Backing up");
				Platform.Severity = 1;
				Platform.Columns = {Row.Title, Percent(Each->Share(ENodeFlag::Full))};
				Platform.Details.Add(FString::Printf(TEXT("Supplies %s and was full %s of the time, so its belts or pipes back up."), *Row.Title, *Percent(Each->Share(ENodeFlag::Full))));
			}
		}
	}
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
	const int32 IdleNow = mFleet.Num() > 0 ? mFleet.Last().Idle : 0;
	AddTile(TEXT("Managed trains"), FString::FromInt(ManagedNow), 0);
	AddTile(TEXT("Busy on trips"), mFleet.Num() > 0 ? Percent(BusyShareNow) : FString(TEXT("-")), BusyShareNow >= BusyShare ? 1 : 0);
	AddTile(TEXT("Idle now"), FString::FromInt(IdleNow), 0);
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
		Lines.Add(FString::Printf(TEXT("  %s: %s %s. %s"), *Row.Verdict, *Row.Title, Row.Columns.Num() > 0 ? *Row.Columns[0] : TEXT(""), *FString::Join(Row.Details, TEXT(" "))));
	}
	for (const FString& Line : View.Idle)
	{
		Lines.Add(TEXT("  Idle: ") + Line);
	}
	return Lines;
}
