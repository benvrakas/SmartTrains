#include "STSubsystem.h"
#include "Buildables/FGBuildableRailroadStation.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "Engine/World.h"
#include "FGFreightWagon.h"
#include "FGInventoryComponent.h"
#include "FGLocomotive.h"
#include "FGRailroadSubsystem.h"
#include "FGRailroadTimeTable.h"
#include "FGTrain.h"
#include "FGTrainStationIdentifier.h"
#include "Resources/FGItemDescriptor.h"
#include "SmartTrains.h"
#include "STBridges.h"
#include "STLayout.h"
#include "STTravel.h"
#include "Subsystem/SubsystemActorManager.h"
#include "TimerManager.h"

ASTSubsystem::ASTSubsystem()
{
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer;
	mTravel = new FSTTravel();
}

ASTSubsystem::~ASTSubsystem()
{
	delete mTravel;
	mTravel = nullptr;
}

ASTSubsystem* ASTSubsystem::Get(const UObject* WorldContext)
{
	UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	USubsystemActorManager* Manager = World ? World->GetSubsystem<USubsystemActorManager>() : nullptr;
	return Manager ? Manager->GetSubsystemActor<ASTSubsystem>() : nullptr;
}

void ASTSubsystem::BeginPlay()
{
	Super::BeginPlay();
	if (!HasAuthority())
	{
		return;
	}
	ReadSettings();
	FTimerManager& Timers = GetWorldTimerManager();
	Timers.SetTimer(mMonitorTimer, this, &ASTSubsystem::MonitorTrains, 0.25f, true, 1.f);
	Timers.SetTimer(mSampleTimer, this, &ASTSubsystem::SamplePlatforms, 1.f, true, 1.f);
	Timers.SetTimer(mTravelTimer, this, &ASTSubsystem::StepTravel, 0.5f, true, 2.f);
	Timers.SetTimer(mDispatchTimer, FTimerDelegate::CreateWeakLambda(this, [this]()
	{
		ReadSettings();
		const double Time = Now();
		if (Time - mLastPlan >= mSettings.PlanSeconds)
		{
			mLastPlan = Time;
			PlanFlows();
		}
		if (bDispatchSoon || Time - mLastDispatch >= mSettings.DispatchSeconds)
		{
			bDispatchSoon = false;
			mLastDispatch = Time;
			Dispatch();
		}
	}), 1.f, true, 5.f);
	int32 Managed = 0;
	for (const FSTTrainState& State : mTrains)
	{
		Managed += State.bManaged ? 1 : 0;
	}
	UE_LOG(LogSmartTrains, Log, TEXT("Subsystem ready: %d platform settings, %d stations, %d trains (%d managed), %d dock maps, %d timed trips; Freight Car Control %s; DynamicTimeTable %s"),
		mPlatforms.Num(), mStations.Num(), mTrains.Num(), Managed, mDockMaps.Num(), mTrips.Num(),
		IsFccAvailable() ? TEXT("available") : TEXT("not available"), STDtt::IsInstalled(GetWorld()) ? TEXT("installed") : TEXT("not installed"));
}

void ASTSubsystem::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearAllTimersForObject(this);
	}
	Super::EndPlay(EndPlayReason);
}

void ASTSubsystem::PreSaveGame_Implementation(int32 SaveVersion, int32 GameVersion)
{
	mPlatforms.RemoveAll([](const FSTPlatformConfig& Config) { return !IsValid(Config.Platform) || (Config.Role == ESTRole::Off && Config.Items.Num() == 0); });
	mStations.RemoveAll([](const FSTStationConfig& Config) { return !IsValid(Config.Station) || (!Config.bDepot && Config.Pool.IsEmpty()); });
	mTrains.RemoveAll([](const FSTTrainState& State) { return !IsValid(State.Train) || (!State.bManaged && !State.bKeepVanilla && State.Pool.IsEmpty()); });
	mDockMaps.RemoveAll([](const FSTDockMap& Map) { return !IsValid(Map.Train) || !IsValid(Map.Station); });
	mTrips.RemoveAll([](const FSTTripTime& Trip) { return !IsValid(Trip.From) || !IsValid(Trip.To); });
	mStays.RemoveAll([](const FSTDwellTime& Stay) { return !IsValid(Stay.Station); });
}

void ASTSubsystem::PostLoadGame_Implementation(int32 SaveVersion, int32 GameVersion)
{
	PreSaveGame_Implementation(SaveVersion, GameVersion);
	for (FSTTrainState& State : mTrains)
	{
		State.bWasDocked = false;
	}
	UE_LOG(LogSmartTrains, Log, TEXT("Loaded: %d platform settings, %d stations, %d trains, %d dock maps, %d timed trips, %d timed stays"),
		mPlatforms.Num(), mStations.Num(), mTrains.Num(), mDockMaps.Num(), mTrips.Num(), mStays.Num());
}

double ASTSubsystem::Now() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetTimeSeconds() : 0.0;
}

void ASTSubsystem::ReadSettings()
{
	const FSTSettings Before = mSettings;
	if (!USTConfig::Read(this, mSettings))
	{
		return;
	}
	if (Before.bObserveOnly != mSettings.bObserveOnly)
	{
		UE_LOG(LogSmartTrains, Log, TEXT("Settings: observe only %s"), mSettings.bObserveOnly ? TEXT("on") : TEXT("off"));
	}
	if (Before.bJobNames != mSettings.bJobNames)
	{
		for (FSTTrainState& State : mTrains)
		{
			UpdateTrainName(State);
		}
	}
}

bool ASTSubsystem::IsFccAvailable() const
{
	return STFcc::IsAvailable(GetWorld());
}

//~ Settings

FSTPlatformConfig* ASTSubsystem::FindPlatformConfig(const AFGBuildableTrainPlatformCargo* Platform)
{
	return mPlatforms.FindByPredicate([Platform](const FSTPlatformConfig& Config) { return Config.Platform == Platform; });
}

const FSTPlatformConfig* ASTSubsystem::FindPlatformConfig(const AFGBuildableTrainPlatformCargo* Platform) const
{
	return mPlatforms.FindByPredicate([Platform](const FSTPlatformConfig& Config) { return Config.Platform == Platform; });
}

FSTStationConfig* ASTSubsystem::FindStationConfig(const AFGTrainStationIdentifier* Station)
{
	return mStations.FindByPredicate([Station](const FSTStationConfig& Config) { return Config.Station == Station; });
}

FSTTrainState* ASTSubsystem::FindTrainState(const AFGTrain* Train)
{
	return mTrains.FindByPredicate([Train](const FSTTrainState& State) { return State.Train == Train; });
}

FSTTrainState& ASTSubsystem::FindOrAddTrainState(AFGTrain* Train)
{
	if (FSTTrainState* Found = FindTrainState(Train))
	{
		return *Found;
	}
	FSTTrainState& State = mTrains.AddDefaulted_GetRef();
	State.Train = Train;
	return State;
}

void ASTSubsystem::SetPlatformConfig(AFGBuildableTrainPlatformCargo* Platform, ESTRole NewRole, const TArray<FSTItemRule>& Items, const FString& Cause)
{
	if (!Platform)
	{
		return;
	}
	FSTPlatformConfig* Config = FindPlatformConfig(Platform);
	if (!Config)
	{
		Config = &mPlatforms.AddDefaulted_GetRef();
		Config->Platform = Platform;
	}
	const ESTRole OldRole = Config->Role;
	FString OldItems;
	for (const FSTItemRule& Rule : Config->Items)
	{
		OldItems += (OldItems.IsEmpty() ? TEXT("") : TEXT(", ")) + STNames::Item(Rule.Item);
	}
	if (NewRole != ESTRole::Off && !Config->bHasOriginalLoadMode)
	{
		Config->bOriginalLoadMode = Platform->GetIsInLoadMode();
		Config->bHasOriginalLoadMode = true;
	}
	Config->Role = NewRole;
	Config->Items = Items;
	Config->EditedBy = Cause;
	if (NewRole == ESTRole::Supply && !Platform->GetIsInLoadMode())
	{
		Platform->SetIsInLoadMode(true);
	}
	else if (NewRole == ESTRole::Demand && Platform->GetIsInLoadMode())
	{
		Platform->SetIsInLoadMode(false);
	}
	else if (NewRole == ESTRole::Off && Config->bHasOriginalLoadMode)
	{
		Platform->SetIsInLoadMode(Config->bOriginalLoadMode);
		Config->bHasOriginalLoadMode = false;
	}
	FString NewItems;
	for (const FSTItemRule& Rule : Items)
	{
		NewItems += FString::Printf(TEXT("%s%s (%s, target %.0f%%, send at %.0f%%)"), NewItems.IsEmpty() ? TEXT("") : TEXT(", "), *STNames::Item(Rule.Item),
			STNames::Priority(Rule.Priority), Rule.TargetPercent, Rule.SendAtPercent);
	}
	int32 Index = INDEX_NONE;
	const AFGBuildableRailroadStation* Station = STLayout::FindStation(Platform, &Index);
	UE_LOG(LogSmartTrains, Log, TEXT("PLATFORM %s #%d (%s): %s [%s] -> %s [%s] by %s"), *STLayout::Name(Station), Index + 1, *Platform->GetName(),
		STNames::Role(OldRole), *OldItems, STNames::Role(NewRole), *NewItems, *Cause);
	if (!Station)
	{
		UE_LOG(LogSmartTrains, Warning, TEXT("PLATFORM %s is not on the output side of any station; trains can't use it"), *Platform->GetName());
	}
	bDispatchSoon = true;
}

void ASTSubsystem::SetStationDepot(AFGTrainStationIdentifier* Station, bool bDepot, const FString& Cause)
{
	if (!Station)
	{
		return;
	}
	FSTStationConfig* Config = FindStationConfig(Station);
	if (!Config)
	{
		Config = &mStations.AddDefaulted_GetRef();
		Config->Station = Station;
	}
	Config->bDepot = bDepot;
	UE_LOG(LogSmartTrains, Log, TEXT("STATION %s: depot %s by %s"), *STLayout::Name(Station), bDepot ? TEXT("on") : TEXT("off"), *Cause);
}

void ASTSubsystem::SetStationPool(AFGTrainStationIdentifier* Station, const FString& Pool, const FString& Cause)
{
	if (!Station)
	{
		return;
	}
	FSTStationConfig* Config = FindStationConfig(Station);
	if (!Config)
	{
		Config = &mStations.AddDefaulted_GetRef();
		Config->Station = Station;
	}
	Config->Pool = Pool;
	UE_LOG(LogSmartTrains, Log, TEXT("STATION %s: pool '%s' by %s"), *STLayout::Name(Station), *Pool, *Cause);
}

bool ASTSubsystem::SetTrainManaged(AFGTrain* Train, bool bManaged, const FString& Cause, FString& OutWhy)
{
	if (!Train)
	{
		OutWhy = TEXT("no train");
		return false;
	}
	FSTTrainState& State = FindOrAddTrainState(Train);
	if (State.bManaged == bManaged)
	{
		OutWhy = bManaged ? TEXT("already managed") : TEXT("not managed");
		return true;
	}
	AFGRailroadTimeTable* TimeTable = Train->GetTimeTable();
	if (bManaged)
	{
		if (State.bKeepVanilla)
		{
			OutWhy = TEXT("it is set to keep vanilla (/st train keepvanilla off first)");
			return false;
		}
		if (STLayout::GetPossibleDockingLocomotives(Train).Num() == 0)
		{
			OutWhy = TEXT("it has no locomotive");
			return false;
		}
		State.SavedStops.Reset();
		if (TimeTable)
		{
			TimeTable->GetStops(State.SavedStops);
			State.SavedCurrentStop = TimeTable->GetCurrentStop();
		}
		State.bSavedSelfDriving = Train->IsSelfDrivingEnabled();
		State.bHasSaved = true;
		State.bManaged = true;
		State.bHasJob = false;
		State.Phase = ESTTrainPhase::Idle;
		State.WrittenStations.Reset();
		UE_LOG(LogSmartTrains, Log, TEXT("MANAGE %s by %s: saved %d stops (current %d), autopilot %s, %s"), *STLayout::Name(Train), *Cause,
			State.SavedStops.Num(), State.SavedCurrentStop, State.bSavedSelfDriving ? TEXT("on") : TEXT("off"), Train->IsDocked() ? TEXT("docked") : TEXT("not docked"));
		if (!Train->IsDocked())
		{
			if (Train->IsSelfDrivingEnabled())
			{
				SettleIdle(State, TEXT("just managed"));
			}
			else
			{
				State.Phase = ESTTrainPhase::Parked;
			}
		}
		bDispatchSoon = true;
		return true;
	}

	State.bManaged = false;
	State.bHasJob = false;
	State.Phase = ESTTrainPhase::Idle;
	if (TimeTable && State.bHasSaved && !mSettings.bObserveOnly)
	{
		if (!STFcc::SetStops(GetWorld(), TimeTable, State.SavedStops, {}, {}, FString()))
		{
			TimeTable->SetStops(State.SavedStops);
		}
		if (State.SavedStops.Num() > 0)
		{
			TimeTable->SetCurrentStop(FMath::Clamp(State.SavedCurrentStop, 0, State.SavedStops.Num() - 1));
		}
		Train->SetSelfDrivingEnabled(State.bSavedSelfDriving);
	}
	State.bHasSaved = false;
	State.WrittenStations.Reset();
	UpdateTrainName(State);
	UE_LOG(LogSmartTrains, Log, TEXT("UNMANAGE %s by %s: timetable of %d stops and autopilot %s put back"), *STLayout::Name(Train), *Cause,
		State.SavedStops.Num(), State.bSavedSelfDriving ? TEXT("on") : TEXT("off"));
	return true;
}

void ASTSubsystem::SetTrainKeepVanilla(AFGTrain* Train, bool bKeepVanilla, const FString& Cause)
{
	if (!Train)
	{
		return;
	}
	FSTTrainState& State = FindOrAddTrainState(Train);
	if (bKeepVanilla && State.bManaged)
	{
		FString Why;
		SetTrainManaged(Train, false, Cause, Why);
	}
	State.bKeepVanilla = bKeepVanilla;
	UE_LOG(LogSmartTrains, Log, TEXT("TRAIN %s: keep vanilla %s by %s"), *STLayout::Name(Train), bKeepVanilla ? TEXT("on") : TEXT("off"), *Cause);
}

void ASTSubsystem::SetTrainPool(AFGTrain* Train, const FString& Pool, const FString& Cause)
{
	if (!Train)
	{
		return;
	}
	FindOrAddTrainState(Train).Pool = Pool;
	UE_LOG(LogSmartTrains, Log, TEXT("TRAIN %s: pool '%s' by %s"), *STLayout::Name(Train), *Pool, *Cause);
}

//~ Monitoring

void ASTSubsystem::MonitorTrains()
{
	const double Time = Now();
	for (FSTTrainState& State : mTrains)
	{
		AFGTrain* Train = State.Train;
		if (!State.bManaged || !IsValid(Train))
		{
			continue;
		}
		AFGRailroadTimeTable* TimeTable = Train->GetTimeTable();
		const bool bDocked = Train->IsDocked();
		if (bDocked && !State.bWasDocked)
		{
			AFGTrainStationIdentifier* Station = nullptr;
			if (TimeTable && TimeTable->GetNumStops() > 0)
			{
				const int32 Current = TimeTable->GetCurrentStop();
				Station = TimeTable->GetStop((Current == 0 ? TimeTable->GetNumStops() : Current) - 1).Station;
			}
			State.DockedAt = Time;
			State.DockedStation = Station;
			OnDocked(State, Station);
		}
		else if (!bDocked && State.bWasDocked)
		{
			OnUndocked(State, State.DockedStation.Get(), Time - State.DockedAt);
		}
		State.bWasDocked = bDocked;

		if (bDocked && mPendingDockMaps.Contains(Train))
		{
			LearnDockMap(Train, State.DockedStation.Get());
		}

		if (mSettings.bObserveOnly || !TimeTable)
		{
			continue;
		}
		if (State.Phase == ESTTrainPhase::Working && State.WrittenStations.Num() > 0)
		{
			TArray<FTimeTableStop> Stops;
			TimeTable->GetStops(Stops);
			bool bSame = Stops.Num() == State.WrittenStations.Num();
			for (int32 Index = 0; bSame && Index < Stops.Num(); ++Index)
			{
				bSame = Stops[Index].Station == State.WrittenStations[Index];
			}
			if (!bSame)
			{
				FString Listed;
				for (const FTimeTableStop& Stop : Stops)
				{
					Listed += (Listed.IsEmpty() ? TEXT("") : TEXT(", ")) + STLayout::Name(Stop.Station);
				}
				if (Time - State.ExternalChangesSince > 60.0)
				{
					State.ExternalChanges = 0;
					State.ExternalChangesSince = Time;
				}
				if (++State.ExternalChanges <= 3)
				{
					UE_LOG(LogSmartTrains, Warning, TEXT("EXTERNAL TIMETABLE CHANGE on %s: stops are now [%s]; writing job %d back (another mod such as DynamicTimeTable, or a player, changed it)"),
						*STLayout::Name(Train), *Listed, State.Job.Id);
					WriteJob(State, TEXT("written back after an external change"));
				}
				else
				{
					UE_LOG(LogSmartTrains, Error, TEXT("EXTERNAL TIMETABLE CHANGE on %s keeps happening (stops now [%s]): job %d dropped. If DynamicTimeTable does this, take these stations out of its groups."),
						*STLayout::Name(Train), *Listed, State.Job.Id);
					State.bHasJob = false;
					State.Phase = ESTTrainPhase::Idle;
					State.WrittenStations.Reset();
				}
			}
		}
		if (State.Phase == ESTTrainPhase::Parked && Train->IsSelfDrivingEnabled() && !bDocked)
		{
			UE_LOG(LogSmartTrains, Log, TEXT("%s: autopilot switched on while parked; it gets the next job that fits"), *STLayout::Name(Train));
			State.Phase = ESTTrainPhase::Idle;
			SettleIdle(State, TEXT("autopilot switched on by someone"));
		}
	}
}

void ASTSubsystem::OnDocked(FSTTrainState& State, AFGTrainStationIdentifier* Station)
{
	AFGTrain* Train = State.Train;
	mPendingDockMaps.Add(Train);
	LearnDockMap(Train, Station);
	if (!State.bHasJob || !State.Job.Legs.IsValidIndex(State.Job.Leg))
	{
		UE_LOG(LogSmartTrains, Log, TEXT("%s docked at %s (%s, no job)"), *STLayout::Name(Train), *STLayout::Name(Station), STNames::Phase(State.Phase));
		return;
	}
	const FSTJobLeg& Leg = State.Job.Legs[State.Job.Leg];
	if (Leg.Station != Station)
	{
		UE_LOG(LogSmartTrains, Warning, TEXT("%s docked at %s but job %d leg %d is %s"), *STLayout::Name(Train), *STLayout::Name(Station), State.Job.Id,
			State.Job.Leg + 1, *STLayout::Name(Leg.Station));
		return;
	}
	if (State.LastStation && State.LastDepartureAt > 0.0)
	{
		LearnTrip(State.LastStation, Station, Now() - State.LastDepartureAt);
	}
	int32 Amount = 0;
	for (AFGRailroadVehicle* Car : Leg.Cars)
	{
		const AFGFreightWagon* Wagon = Cast<AFGFreightWagon>(Car);
		const UFGInventoryComponent* Inventory = Wagon ? Wagon->GetFreightInventory() : nullptr;
		Amount += Inventory && Leg.Item ? Inventory->GetNumItems(Leg.Item) : 0;
	}
	mDockStartAmounts.Add(Train, Amount);
	const double Late = Now() - Leg.PlannedArrival;
	UE_LOG(LogSmartTrains, Log, TEXT("JOB %d %s: docked at %s for leg %d/%d (%s %s), %+.0f s against plan"), State.Job.Id, *STLayout::Name(Train), *STLayout::Name(Station),
		State.Job.Leg + 1, State.Job.Legs.Num(), Leg.Item ? (Leg.bLoad ? TEXT("load") : TEXT("unload")) : TEXT("no transfer"), *STNames::Item(Leg.Item), Late);
	if (State.Job.Leg == State.Job.Legs.Num() - 1 && !State.Job.bPark)
	{
		bDispatchSoon = true;
	}
}

void ASTSubsystem::OnUndocked(FSTTrainState& State, AFGTrainStationIdentifier* Station, double Stay)
{
	AFGTrain* Train = State.Train;
	mPendingDockMaps.Remove(Train);
	State.LastStation = Station;
	State.LastDepartureAt = Now();
	if (!State.bHasJob)
	{
		UE_LOG(LogSmartTrains, Log, TEXT("%s left %s after %.0f s (no job)"), *STLayout::Name(Train), *STLayout::Name(Station), Stay);
		if (!mSettings.bObserveOnly)
		{
			SettleIdle(State, TEXT("left a station without a job"));
		}
		return;
	}
	if (State.bChainedWhileDocked)
	{
		State.bChainedWhileDocked = false;
		UE_LOG(LogSmartTrains, Log, TEXT("%s left %s after %.0f s, on to job %d"), *STLayout::Name(Train), *STLayout::Name(Station), Stay, State.Job.Id);
		return;
	}
	if (!State.Job.Legs.IsValidIndex(State.Job.Leg) || State.Job.Legs[State.Job.Leg].Station != Station)
	{
		UE_LOG(LogSmartTrains, Log, TEXT("%s left %s after %.0f s; job %d continues at leg %d"), *STLayout::Name(Train), *STLayout::Name(Station), Stay,
			State.Job.Id, State.Job.Leg + 1);
		return;
	}
	FSTJobLeg& Leg = State.Job.Legs[State.Job.Leg];
	int32 Amount = 0;
	for (AFGRailroadVehicle* Car : Leg.Cars)
	{
		const AFGFreightWagon* Wagon = Cast<AFGFreightWagon>(Car);
		const UFGInventoryComponent* Inventory = Wagon ? Wagon->GetFreightInventory() : nullptr;
		Amount += Inventory && Leg.Item ? Inventory->GetNumItems(Leg.Item) : 0;
	}
	const int32 Before = mDockStartAmounts.FindRef(Train);
	Leg.MovedAmount = FMath::Abs(Amount - Before);
	if (Leg.Item)
	{
		LearnStay(Station, Stay);
	}
	const FString Line = FString::Printf(TEXT("JOB %d %s: left %s after %.0f s, %s %s %s (planned %s)"), State.Job.Id, *STLayout::Name(Train), *STLayout::Name(Station), Stay,
		Leg.bLoad ? TEXT("loaded") : TEXT("unloaded"), *STNames::Amount(Leg.Item, Leg.MovedAmount), *STNames::Item(Leg.Item), *STNames::Amount(Leg.Item, Leg.PlannedAmount));
	UE_LOG(LogSmartTrains, Log, TEXT("%s"), *Line);
	if (Leg.Item)
	{
		AddHistory(Station, Line);
	}
	++State.Job.Leg;
	State.Job.LegStartedAt = Now();
	if (State.Job.Leg >= State.Job.Legs.Num())
	{
		FinishJob(State);
	}
}

void ASTSubsystem::LearnDockMap(AFGTrain* Train, AFGTrainStationIdentifier* Station)
{
	AFGBuildableRailroadStation* Building = Station ? Station->GetStation() : nullptr;
	if (!Train || !Building)
	{
		mPendingDockMaps.Remove(Train);
		return;
	}
	FSTDockMap Seen;
	Seen.Train = Train;
	Seen.Station = Station;
	bool bAny = false;
	for (AFGBuildableTrainPlatform* Platform : STLayout::GetPlatforms(Building))
	{
		const AFGBuildableTrainPlatformCargo* Cargo = Cast<AFGBuildableTrainPlatformCargo>(Platform);
		AFGRailroadVehicle* Vehicle = Cargo ? Cargo->GetDockedActor() : nullptr;
		Seen.Vehicles.Add(Vehicle);
		bAny |= Vehicle != nullptr;
	}
	if (!bAny)
	{
		return;
	}
	mPendingDockMaps.Remove(Train);

	bool bLearned = false;
	const TArray<TArray<AFGRailroadVehicle*>> Expected = GetDockMaps(Train, Station, &bLearned);
	bool bMatches = false;
	for (const TArray<AFGRailroadVehicle*>& Map : Expected)
	{
		bool bSame = true;
		for (int32 Index = 0; Index < Seen.Vehicles.Num() && bSame; ++Index)
		{
			if (Seen.Vehicles[Index] && (!Map.IsValidIndex(Index) || Map[Index] != Seen.Vehicles[Index]))
			{
				bSame = false;
			}
		}
		bMatches |= bSame;
	}
	FString Line;
	for (int32 Index = 0; Index < Seen.Vehicles.Num(); ++Index)
	{
		Line += FString::Printf(TEXT(" P%d=%s"), Index + 1, *STLayout::DescribeVehicle(Train, Seen.Vehicles[Index]));
	}
	if (!bMatches)
	{
		UE_LOG(LogSmartTrains, Warning, TEXT("DOCK MAP MISMATCH %s at %s (%s): seen%s"), *STLayout::Name(Train), *STLayout::Name(Station),
			bLearned ? TEXT("differs from last time") : TEXT("differs from the prediction"), *Line);
	}
	else
	{
		UE_LOG(LogSmartTrains, Verbose, TEXT("Dock map %s at %s:%s"), *STLayout::Name(Train), *STLayout::Name(Station), *Line);
	}
	FSTDockMap* Existing = mDockMaps.FindByPredicate([&](const FSTDockMap& Map) { return Map.Train == Train && Map.Station == Station; });
	if (Existing)
	{
		*Existing = Seen;
	}
	else
	{
		mDockMaps.Add(Seen);
	}
}

void ASTSubsystem::LearnTrip(AFGTrainStationIdentifier* From, AFGTrainStationIdentifier* To, float Seconds)
{
	if (!From || !To || From == To || Seconds <= 0.f)
	{
		return;
	}
	FSTTripTime* Trip = mTrips.FindByPredicate([&](const FSTTripTime& Each) { return Each.From == From && Each.To == To; });
	if (!Trip)
	{
		Trip = &mTrips.AddDefaulted_GetRef();
		Trip->From = From;
		Trip->To = To;
	}
	const float Estimate = Trip->Samples > 0 ? Trip->Seconds : EstimateTravelSeconds(From, To);
	Trip->Seconds = Trip->Samples == 0 ? Seconds : FMath::Lerp(Trip->Seconds, Seconds, 0.3f);
	++Trip->Samples;
	UE_LOG(LogSmartTrains, Log, TEXT("TRIP %s -> %s took %.0f s (estimate was %.0f s, now %.0f s from %d trips)"), *STLayout::Name(From), *STLayout::Name(To), Seconds,
		Estimate, Trip->Seconds, Trip->Samples);
}

void ASTSubsystem::LearnStay(AFGTrainStationIdentifier* Station, float Seconds)
{
	if (!Station || Seconds <= 0.f)
	{
		return;
	}
	FSTDwellTime* Stay = mStays.FindByPredicate([&](const FSTDwellTime& Each) { return Each.Station == Station; });
	if (!Stay)
	{
		Stay = &mStays.AddDefaulted_GetRef();
		Stay->Station = Station;
	}
	Stay->Seconds = Stay->Samples == 0 ? Seconds : FMath::Lerp(Stay->Seconds, Seconds, 0.3f);
	++Stay->Samples;
}

void ASTSubsystem::AddHistory(const AFGTrainStationIdentifier* Station, const FString& Line)
{
	TArray<FString>& Lines = mHistory.FindOrAdd(const_cast<AFGTrainStationIdentifier*>(Station));
	Lines.Add(Line);
	while (Lines.Num() > 5)
	{
		Lines.RemoveAt(0);
	}
}

float ASTSubsystem::EstimateTravelSeconds(const AFGTrainStationIdentifier* From, const AFGTrainStationIdentifier* To) const
{
	if (!From || !To)
	{
		return -1.f;
	}
	if (From == To)
	{
		return 0.f;
	}
	if (const FSTTripTime* Trip = mTrips.FindByPredicate([&](const FSTTripTime& Each) { return Each.From == From && Each.To == To; }))
	{
		return Trip->Seconds;
	}
	const float Speed = FMath::Max(1.f, mSettings.AverageSpeedKmh / 3.6f) * 100.f;
	bool bReversing = false;
	float Distance = mTravel ? mTravel->GetDistance(From, To, &bReversing) : -1.f;
	if (Distance == -2.f)
	{
		return -1.f;
	}
	if (Distance < 0.f)
	{
		const AFGBuildableRailroadStation* A = From->GetStation();
		const AFGBuildableRailroadStation* B = To->GetStation();
		if (!A || !B)
		{
			return -1.f;
		}
		Distance = FVector::Dist(A->GetActorLocation(), B->GetActorLocation()) * 1.4f;
	}
	return Distance / Speed + 20.f + (bReversing ? 30.f : 0.f);
}

float ASTSubsystem::EstimateStaySeconds(const AFGTrainStationIdentifier* Station) const
{
	if (const FSTDwellTime* Stay = mStays.FindByPredicate([&](const FSTDwellTime& Each) { return Each.Station == Station; }))
	{
		return Stay->Seconds;
	}
	return mSettings.DefaultStaySeconds;
}

void ASTSubsystem::StepTravel()
{
	AFGRailroadSubsystem* Railroad = AFGRailroadSubsystem::Get(GetWorld());
	if (!Railroad || !mTravel)
	{
		return;
	}
	TArray<AFGTrainStationIdentifier*> Stations;
	Railroad->GetAllTrainStations(Stations);
	Stations.RemoveAll([](const AFGTrainStationIdentifier* Station) { return !IsValid(Station) || !Station->GetStation(); });
	mTravel->Step(Stations);
}

//~ Measuring

void ASTSubsystem::SamplePlatforms()
{
	const double Time = Now();
	const double Step = mLastSample > 0.0 ? FMath::Clamp(Time - mLastSample, 0.01, 10.0) : 1.0;
	mLastSample = Time;
	const double Window = FMath::Max(60.0, mSettings.RateWindowMinutes * 60.0);
	for (const FSTPlatformConfig& Config : mPlatforms)
	{
		AFGBuildableTrainPlatformCargo* Platform = Config.Platform;
		const UFGInventoryComponent* Inventory = IsValid(Platform) ? Platform->GetInventory() : nullptr;
		if (!Inventory || Config.Role == ESTRole::Off)
		{
			continue;
		}
		FPlatformStats& Stats = mStats.FindOrAdd(Platform);
		AFGRailroadVehicle* Docked = Platform->GetDockedActor();
		const AFGFreightWagon* Car = Cast<AFGFreightWagon>(Docked);
		const UFGInventoryComponent* CarInventory = Car ? Car->GetFreightInventory() : nullptr;
		for (const FSTItemRule& Rule : Config.Items)
		{
			if (!Rule.Item)
			{
				continue;
			}
			FItemStats& Item = Stats.Items.FindOrAdd(Rule.Item.Get());
			const int32 Stock = Inventory->GetNumItems(Rule.Item);
			const int32 CarItems = CarInventory ? CarInventory->GetNumItems(Rule.Item) : -1;
			if (Item.LastStock >= 0)
			{
				const int32 StockDelta = Stock - Item.LastStock;
				const int32 CarDelta = (CarItems >= 0 && Item.LastCar.Get() == Docked && Item.LastCarItems >= 0) ? CarItems - Item.LastCarItems : 0;
				// Supply: belts in = stock change + taken by the car. Demand: belts out = given by the car - stock change.
				const double Flow = Config.Role == ESTRole::Supply ? StockDelta + CarDelta : -CarDelta - StockDelta;
				const double Sample = FMath::Max(0.0, Flow) / Step;
				Item.Measured += Step;
				const double Weight = FMath::Min(1.0, Step / FMath::Min(Window, FMath::Max(Item.Measured, Step)));
				Item.Rate += (Sample - Item.Rate) * Weight;
			}
			Item.EmptySeconds = Stock == 0 ? Item.EmptySeconds + Step : 0.0;
			Item.LastStock = Stock;
			Item.LastCarItems = CarItems;
			Item.LastCar = Docked;
		}
	}
	for (auto It = mStats.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}
}

double ASTSubsystem::GetMeasuredRate(const AFGBuildableTrainPlatformCargo* Platform, TSubclassOf<UFGItemDescriptor> Item, bool* bKnown) const
{
	const FPlatformStats* Stats = mStats.Find(const_cast<AFGBuildableTrainPlatformCargo*>(Platform));
	const FItemStats* ItemStats = Stats ? Stats->Items.Find(Item.Get()) : nullptr;
	if (bKnown)
	{
		*bKnown = ItemStats && ItemStats->Measured >= FMath::Min(120.0, mSettings.RateWindowMinutes * 60.0);
	}
	return ItemStats ? ItemStats->Rate : 0.0;
}

//~ Names

void ASTSubsystem::UpdateTrainName(FSTTrainState& State)
{
	AFGTrain* Train = State.Train;
	if (!IsValid(Train) || !Train->HasAuthority())
	{
		return;
	}
	if (!mSettings.bJobNames || !State.bManaged)
	{
		if (State.bRenamed)
		{
			Train->SetTrainName(State.OriginalName);
			State.bRenamed = false;
			UE_LOG(LogSmartTrains, Log, TEXT("%s: original name put back"), *State.OriginalName.ToString());
		}
		return;
	}
	if (!State.bRenamed)
	{
		State.OriginalName = Train->GetTrainName();
		State.bRenamed = true;
	}
	FString Name;
	if (State.bHasJob)
	{
		Name = FString::Printf(TEXT("ST: %s"), *State.Job.Summary);
	}
	else
	{
		Name = FString::Printf(TEXT("ST: %s (%s)"), STNames::Phase(State.Phase), *State.OriginalName.ToString());
	}
	if (Train->GetTrainName().ToString() != Name)
	{
		Train->SetTrainName(FText::FromString(Name));
	}
}

//~ Readouts

TArray<FString> ASTSubsystem::DescribePlatform(const AFGBuildableTrainPlatformCargo* Platform) const
{
	TArray<FString> Lines;
	const FSTPlatformView View = MakePlatformView(Platform, 0);
	Lines.Add(FString::Printf(TEXT("%s: %s, %s mode"), *View.Title, STNames::Role(View.Role), Platform && Platform->GetIsInLoadMode() ? TEXT("load") : TEXT("unload")));
	if (View.Role == ESTRole::Off)
	{
		Lines.Add(TEXT("  Vanilla. Set a role with /st platform supply|demand <item>."));
		return Lines;
	}
	for (int32 Index = 0; Index < View.Items.Num() && Index < View.ItemLines.Num(); ++Index)
	{
		Lines.Add(FString::Printf(TEXT("  %s: %s"), *STNames::Item(View.Items[Index].Item), *View.ItemLines[Index]));
	}
	for (const FString& Line : View.Lines)
	{
		Lines.Add(TEXT("  ") + Line);
	}
	return Lines;
}

TArray<FString> ASTSubsystem::DescribeStation(const AFGTrainStationIdentifier* Station) const
{
	TArray<FString> Lines;
	const AFGBuildableRailroadStation* Building = Station ? Station->GetStation() : nullptr;
	const FSTStationConfig* Config = mStations.FindByPredicate([Station](const FSTStationConfig& Each) { return Each.Station == Station; });
	Lines.Add(FString::Printf(TEXT("Station %s%s%s"), *STLayout::Name(Station), Config && Config->bDepot ? TEXT(" (depot)") : TEXT(""),
		Config && !Config->Pool.IsEmpty() ? *FString::Printf(TEXT(" pool '%s'"), *Config->Pool) : TEXT("")));
	FString Layout;
	for (const AFGBuildableTrainPlatform* Platform : STLayout::GetPlatforms(Building))
	{
		const AFGBuildableTrainPlatformCargo* Cargo = Cast<AFGBuildableTrainPlatformCargo>(Platform);
		const FSTPlatformConfig* PlatformConfig = Cargo ? FindPlatformConfig(Cargo) : nullptr;
		FString Piece = !Cargo ? TEXT("E") : (STLayout::GetPlatformCargoType(Cargo) == EFreightCargoType::FCT_Liquid ? TEXT("F") : TEXT("S"));
		if (PlatformConfig && PlatformConfig->Role != ESTRole::Off)
		{
			FString Items;
			for (const FSTItemRule& Rule : PlatformConfig->Items)
			{
				Items += (Items.IsEmpty() ? TEXT("") : TEXT("+")) + STNames::Item(Rule.Item);
			}
			Piece += FString::Printf(TEXT("-%s[%s]"), PlatformConfig->Role == ESTRole::Supply ? TEXT("Supply") : TEXT("Demand"), *Items);
		}
		Layout += (Layout.IsEmpty() ? TEXT("") : TEXT(" ")) + Piece;
	}
	Lines.Add(TEXT("  Layout (from the station): ") + (Layout.IsEmpty() ? FString(TEXT("no platforms")) : Layout));
	return Lines;
}

TArray<FString> ASTSubsystem::DescribeTrain(const AFGTrain* Train) const
{
	TArray<FString> Lines;
	const FSTTrainState* State = mTrains.FindByPredicate([Train](const FSTTrainState& Each) { return Each.Train == Train; });
	FString Consist;
	for (const AFGRailroadVehicle* Vehicle : STLayout::GetVehicles(Train))
	{
		Consist += Vehicle && Vehicle->IsA<AFGLocomotive>() ? TEXT("L") : TEXT("C");
	}
	Lines.Add(FString::Printf(TEXT("%s [%s]: %s%s%s"), *STLayout::Name(Train), *Consist,
		State && State->bManaged ? TEXT("managed, ") : TEXT("not managed"), State && State->bManaged ? STNames::Phase(State->Phase) : TEXT(""),
		State && State->bKeepVanilla ? TEXT(", keep vanilla") : TEXT("")));
	if (State && !State->Pool.IsEmpty())
	{
		Lines.Add(FString::Printf(TEXT("  Pool '%s'"), *State->Pool));
	}
	if (State && State->bHasJob)
	{
		Lines.Add(FString::Printf(TEXT("  Job %d: %s (leg %d of %d)"), State->Job.Id, *State->Job.Summary, State->Job.Leg + 1, State->Job.Legs.Num()));
	}
	TMap<UClass*, int32> Cargo = GetCargo(Train);
	for (const TPair<UClass*, int32>& Pair : Cargo)
	{
		Lines.Add(FString::Printf(TEXT("  Carries %s %s"), *STNames::Amount(Pair.Key, Pair.Value), *STNames::Item(Pair.Key)));
	}
	return Lines;
}

TArray<FString> ASTSubsystem::DescribeJobs() const
{
	TArray<FString> Lines;
	for (const FSTTrainState& State : mTrains)
	{
		if (!State.bManaged || !IsValid(State.Train))
		{
			continue;
		}
		if (!State.bHasJob)
		{
			const FString* Observed = mObserved.Find(State.Train.Get());
			Lines.Add(FString::Printf(TEXT("%s: %s%s"), *STLayout::Name(State.Train), STNames::Phase(State.Phase),
				Observed ? *FString::Printf(TEXT(" - would: %s"), **Observed) : TEXT("")));
			continue;
		}
		const FSTJobLeg* Leg = State.Job.Legs.IsValidIndex(State.Job.Leg) ? &State.Job.Legs[State.Job.Leg] : nullptr;
		Lines.Add(FString::Printf(TEXT("%s: job %d %s, leg %d/%d at %s, ETA %.0f s"), *STLayout::Name(State.Train), State.Job.Id, *State.Job.Summary,
			State.Job.Leg + 1, State.Job.Legs.Num(), Leg ? *STLayout::Name(Leg->Station) : TEXT("?"), Leg ? FMath::Max(0.0, Leg->PlannedArrival - Now()) : 0.0));
	}
	if (Lines.Num() == 0)
	{
		Lines.Add(TEXT("No managed trains. Drive or look at a train and use /st train manage."));
	}
	return Lines;
}

TArray<FString> ASTSubsystem::DescribeStatus() const
{
	TArray<FString> Lines;
	int32 Managed = 0;
	int32 Working = 0;
	for (const FSTTrainState& State : mTrains)
	{
		Managed += State.bManaged ? 1 : 0;
		Working += State.bManaged && State.bHasJob ? 1 : 0;
	}
	int32 Supply = 0;
	int32 Demand = 0;
	for (const FSTPlatformConfig& Config : mPlatforms)
	{
		Supply += Config.Role == ESTRole::Supply ? 1 : 0;
		Demand += Config.Role == ESTRole::Demand ? 1 : 0;
	}
	Lines.Add(FString::Printf(TEXT("SmartTrains%s: %d managed trains (%d working), %d supply and %d demand platforms, %d stations searched for distances, Freight Car Control %s"),
		mSettings.bObserveOnly ? TEXT(" (observe only)") : TEXT(""), Managed, Working, Supply, Demand, mTravel ? mTravel->GetSearchedCount() : 0,
		IsFccAvailable() ? TEXT("on") : TEXT("off")));
	for (const TPair<UClass*, FAdvice>& Pair : mAdvice)
	{
		const FAdvice& Advice = Pair.Value;
		Lines.Add(FString::Printf(TEXT("  %s: planned %s/min over %d routes, needs %.1f trains, %d managed trains fit"), *STNames::Item(Pair.Key),
			*STNames::Amount(Pair.Key, Advice.FlowPerMinute), Advice.Pairs, Advice.TrainsNeeded, Advice.TrainsFitting));
	}
	if (mAdvice.Num() == 0)
	{
		Lines.Add(TEXT("  No flow plan yet: it needs supply and demand platforms for the same item with measured rates."));
	}
	return Lines;
}

TArray<FString> ASTSubsystem::DescribeFit(const AFGTrain* Train) const
{
	TArray<FString> Lines;
	TSet<const AFGTrainStationIdentifier*> Stations;
	for (const FSTPlatformConfig& Config : mPlatforms)
	{
		if (Config.Role != ESTRole::Off)
		{
			Stations.Add(STLayout::GetIdentifier(STLayout::FindStation(Config.Platform)));
		}
	}
	for (const AFGTrainStationIdentifier* Station : Stations)
	{
		if (!Station)
		{
			continue;
		}
		bool bLearned = false;
		const TArray<TArray<AFGRailroadVehicle*>> Maps = GetDockMaps(Train, Station, &bLearned);
		for (const TArray<AFGRailroadVehicle*>& Map : Maps)
		{
			FString Line;
			for (int32 Index = 0; Index < Map.Num(); ++Index)
			{
				Line += FString::Printf(TEXT(" P%d=%s"), Index + 1, *STLayout::DescribeVehicle(Train, Map[Index]));
			}
			Lines.Add(FString::Printf(TEXT("%s (%s%s):%s"), *STLayout::Name(Station), bLearned ? TEXT("seen") : TEXT("predicted"),
				Maps.Num() > 1 ? TEXT(", either way round") : TEXT(""), *Line));
		}
	}
	if (Lines.Num() == 0)
	{
		Lines.Add(TEXT("No stations with platform roles yet."));
	}
	return Lines;
}

TMap<UClass*, int32> ASTSubsystem::GetCargo(const AFGTrain* Train)
{
	TMap<UClass*, int32> Cargo;
	for (const AFGRailroadVehicle* Vehicle : STLayout::GetVehicles(Train))
	{
		const AFGFreightWagon* Car = Cast<AFGFreightWagon>(Vehicle);
		const UFGInventoryComponent* Inventory = Car ? Car->GetFreightInventory() : nullptr;
		if (!Inventory)
		{
			continue;
		}
		TArray<FInventoryStack> Stacks;
		Inventory->GetInventoryStacks(Stacks);
		for (const FInventoryStack& Stack : Stacks)
		{
			if (Stack.HasItems() && Stack.Item.GetItemClass())
			{
				Cargo.FindOrAdd(Stack.Item.GetItemClass()) += Stack.NumItems;
			}
		}
	}
	return Cargo;
}
