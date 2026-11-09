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
#include "STThroughput.h"
#include "STTravel.h"
#include "Subsystem/SubsystemActorManager.h"
#include "TimerManager.h"

ASTSubsystem::ASTSubsystem()
{
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnServer;
	mTravel = new FSTTravel();
	mThroughput = new FSTThroughput();
}

ASTSubsystem::~ASTSubsystem()
{
	delete mTravel;
	mTravel = nullptr;
	delete mThroughput;
	mThroughput = nullptr;
}

namespace
{
	// How long a move that moved nothing isn't planned again for that train. [min]
	constexpr double NoProgressMinutes = 10.0;
	// An unloading platform that takes less than this share of what was planned counts as slow draining for SlowDrainMinutes.
	constexpr double SlowDrainShare = 0.5;
	constexpr double SlowDrainMinutes = 30.0;
	// A known trip pair's sample counts at most this many times its estimate (queues and deadlocks are outliers).
	constexpr float TripOutlierFactor = 1.5f;
	// A self-driving error is logged once it has lasted this long; shorter ones are the game re-routing. [s]
	constexpr double SelfDrivingErrorLogSeconds = 5.0;
	// A train the game finds no way for this long leaves out the station it was heading for, for UnreachableMinutes.
	constexpr double UnreachableSeconds = 20.0;
	constexpr double UnreachableMinutes = 30.0;
	// A docked train's stay ends once nothing has moved for this long, after at least the first transfer. [s]
	constexpr double DockIdleSeconds = 20.0;
	constexpr double DockFirstTransferSeconds = 30.0;
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
	mStartTime = Now();
	FTimerManager& Timers = GetWorldTimerManager();
	Timers.SetTimer(mMonitorTimer, this, &ASTSubsystem::MonitorTrains, 0.25f, true, 1.f);
	Timers.SetTimer(mSampleTimer, this, &ASTSubsystem::SamplePlatforms, 1.f, true, 1.f);
	Timers.SetTimer(mTravelTimer, this, &ASTSubsystem::StepTravel, 0.5f, true, 2.f);
	Timers.SetTimer(mThroughputTimer, FTimerDelegate::CreateWeakLambda(this, [this]()
	{
		if (!mThroughput)
		{
			return;
		}
		mThroughput->Sample(*this);
		// The report goes to the log every 5 minutes while trains are managed.
		const double Time = Now();
		if (Time - mLastMonitorLog >= 300.0 && mTrains.ContainsByPredicate([](const FSTTrainState& State) { return State.bManaged; }))
		{
			mLastMonitorLog = Time;
			for (const FString& Line : DescribeMonitor())
			{
				UE_LOG(LogSmartTrains, Log, TEXT("MONITOR %s"), *Line);
			}
		}
	}), static_cast<float>(FSTThroughput::SampleSeconds), true, 5.f);
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
	mClockAtSave = Now();
	mPlatforms.RemoveAll([](const FSTPlatformConfig& Config) { return !IsValid(Config.Platform) || (Config.Role == ESTRole::Off && Config.Items.Num() == 0); });
	mStations.RemoveAll([](const FSTStationConfig& Config) { return !IsValid(Config.Station) || !Config.bDepot; });
	mTrains.RemoveAll([](const FSTTrainState& State) { return !IsValid(State.Train) || (!State.bManaged && !State.bKeepVanilla); });
	mDockMaps.RemoveAll([](const FSTDockMap& Map) { return !IsValid(Map.Train) || !IsValid(Map.Station); });
	mTrips.RemoveAll([](const FSTTripTime& Trip) { return !IsValid(Trip.From) || !IsValid(Trip.To); });
	mStays.RemoveAll([](const FSTDwellTime& Stay) { return !IsValid(Stay.Station); });
}

void ASTSubsystem::PostLoadGame_Implementation(int32 SaveVersion, int32 GameVersion)
{
	// The clock carries on from the save. A save without the clock gets the latest time its trains hold, so nothing
	// saved lies in the future.
	double Saved = mClockAtSave;
	if (Saved <= 0.0)
	{
		for (const FSTTrainState& State : mTrains)
		{
			Saved = FMath::Max(Saved, FMath::Max(State.LastDepartureAt, State.UnwantedCargoSince));
			Saved = FMath::Max(Saved, FMath::Max(State.Job.StartedAt, State.Job.LegStartedAt));
		}
	}
	const UWorld* World = GetWorld();
	mClockBase = Saved - (World ? World->GetTimeSeconds() : 0.0);
	mStartTime = Now();
	UE_LOG(LogSmartTrains, Log, TEXT("Clock continues at %.0f s%s"), Saved, mClockAtSave > 0.0 ? TEXT("") : TEXT(" (save without a clock: latest saved train time)"));
	for (FSTStationConfig& Config : mStations)
	{
		if (!Config.Pool.IsEmpty())
		{
			UE_LOG(LogSmartTrains, Log, TEXT("Station %s had pool tag '%s'; pool tags are gone, so it now serves every managed train"), *STLayout::Name(Config.Station), *Config.Pool);
			Config.Pool.Reset();
		}
	}
	// Before 0.6.9 a sink was a demand platform whose only item was "any item".
	for (FSTPlatformConfig& Config : mPlatforms)
	{
		if (Config.Role == ESTRole::Demand && Config.Items.Num() == 1 && !Config.Items[0].Item)
		{
			Config.Role = ESTRole::Sink;
			UE_LOG(LogSmartTrains, Log, TEXT("PLATFORM %s: demand for any item is now the Sink role"), IsValid(Config.Platform) ? *Config.Platform->GetName() : TEXT("?"));
		}
	}
	// Requests whose platform, station or item is gone are dropped.
	for (FSTTrainState& State : mTrains)
	{
		State.Requests.RemoveAll([](const FSTRequestClaim& Claim) { return !IsValid(Claim.Station) || !IsValid(Claim.Platform) || !Claim.Item; });
	}
	int32 Converted = 0;
	for (FSTTrainState& State : mTrains)
	{
		for (FSTJobLeg& Leg : State.Job.Legs)
		{
			Converted += Leg.Item ? 1 : 0;
			Leg.ConvertOldWork();
		}
	}
	if (Converted > 0)
	{
		UE_LOG(LogSmartTrains, Log, TEXT("Converted %d job stops from an older save to per-car work"), Converted);
	}
	for (FSTTrainState& State : mTrains)
	{
		if (!State.Pool.IsEmpty())
		{
			UE_LOG(LogSmartTrains, Log, TEXT("Train %s had pool tag '%s'; pool tags are gone, so it now serves every station"), *STLayout::Name(State.Train), *State.Pool);
			State.Pool.Reset();
		}
	}
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
	return mClockBase + (World ? World->GetTimeSeconds() : 0.0);
}

void ASTSubsystem::ReadSettings()
{
	const FSTSettings Before = mSettings;
	if (!USTConfig::Read(this, mSettings))
	{
		if (!bSettingsRead)
		{
			UE_LOG(LogSmartTrains, Warning, TEXT("Settings: Mods menu configuration not found; defaults used"));
			bSettingsRead = true;
		}
		return;
	}
	const TArray<FString> Old = USTConfig::Describe(Before);
	const TArray<FString> New = USTConfig::Describe(mSettings);
	if (!bSettingsRead)
	{
		UE_LOG(LogSmartTrains, Log, TEXT("Settings: %s"), *FString::Join(New, TEXT(", ")));
	}
	else
	{
		for (int32 Index = 0; Index < New.Num(); ++Index)
		{
			if (!Old.IsValidIndex(Index) || Old[Index] != New[Index])
			{
				UE_LOG(LogSmartTrains, Log, TEXT("Settings changed: %s (was %s)"), *New[Index], Old.IsValidIndex(Index) ? *Old[Index] : TEXT("?"));
			}
		}
	}
	if (!bSettingsRead || Before.bDetailedLog != mSettings.bDetailedLog)
	{
		LogSmartTrains.SetVerbosity(mSettings.bDetailedLog ? ELogVerbosity::Verbose : ELogVerbosity::Log);
		UE_LOG(LogSmartTrains, Log, TEXT("Detailed log %s"), mSettings.bDetailedLog ? TEXT("on") : TEXT("off"));
	}
	bSettingsRead = true;
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
	if (NewRole != ESTRole::Off && !STLayout::CanMoveCargo(Platform))
	{
		UE_LOG(LogSmartTrains, Warning, TEXT("PLATFORM %s (%s) can't move cargo: role %s from %s refused"), *Platform->GetName(), *Platform->GetClass()->GetName(),
			STNames::Role(NewRole), *Cause);
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
	if (NewRole == ESTRole::Sink)
	{
		// A sink takes any item; its one rule has no item.
		Config->Items = {FSTItemRule()};
	}
	Config->EditedBy = Cause;
	if (NewRole == ESTRole::Supply && !Platform->GetIsInLoadMode())
	{
		Platform->SetIsInLoadMode(true);
	}
	else if (STNames::Unloads(NewRole) && Platform->GetIsInLoadMode())
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
	bReplanAll = true;
}

bool ASTSubsystem::SetPlatformRequest(AFGBuildableTrainPlatformCargo* Platform, ESTRequest Request, const FString& Cause, FString& OutError)
{
	FSTPlatformConfig* Config = Platform ? FindPlatformConfig(Platform) : nullptr;
	if (!Config || Config->Role != ESTRole::Demand)
	{
		OutError = TEXT("Only a demand platform asks for a train to be emptied for it.");
		return false;
	}
	const ESTRequest Old = Config->Request;
	Config->Request = Request;
	Config->EditedBy = Cause;
	UE_LOG(LogSmartTrains, Log, TEXT("PLATFORM %s: request %s -> %s by %s"), *Platform->GetName(), STNames::Request(Old), STNames::Request(Request), *Cause);
	return true;
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
		// Docks are normally begun by the dock hook (NoteDockStarted); this catches any it missed.
		if (bDocked && !State.bWasDocked)
		{
			AFGTrainStationIdentifier* Station = nullptr;
			if (TimeTable && TimeTable->GetNumStops() > 0)
			{
				const int32 Current = TimeTable->GetCurrentStop();
				Station = TimeTable->GetStop((Current == 0 ? TimeTable->GetNumStops() : Current) - 1).Station;
			}
			UE_LOG(LogSmartTrains, Verbose, TEXT("%s docked at %s without the dock hook"), *STLayout::Name(Train), *STLayout::Name(Station));
			BeginDock(State, Station);
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
		if (bDocked && !mSettings.bObserveOnly)
		{
			ReleaseDockWhenDone(State, Time);
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
					UE_LOG(LogSmartTrains, Warning, TEXT("EXTERNAL TIMETABLE CHANGE on %s: stops are now [%s]; writing job %d back (%s)"),
						*STLayout::Name(Train), *Listed, State.Job.Id, STDtt::IsInstalled(GetWorld())
						? TEXT("DynamicTimeTable is installed and rewrites stops at stations in its groups, managed trains included: take those stations out of its groups, or uninstall it")
						: TEXT("another mod or a player changed it"));
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
		// The game's self-driving error (no path, station unreachable, ...), logged when it changes.
		// Logged once it has lasted SelfDrivingErrorLogSeconds, and when a logged one clears.
		const uint8 Error = Train->IsSelfDrivingEnabled() ? static_cast<uint8>(Train->GetSelfDrivingError()) : 0;
		if (Error != State.SelfDrivingError)
		{
			State.SelfDrivingError = Error;
			State.SelfDrivingErrorSince = Time;
			State.bUnreachableHandled = false;
		}
		const bool bNoWay = Error == static_cast<uint8>(ESelfDrivingLocomotiveError::SDLE_NoPath) || Error == static_cast<uint8>(ESelfDrivingLocomotiveError::SDLE_StationUnreachable)
			|| Error == static_cast<uint8>(ESelfDrivingLocomotiveError::SDLE_StationUnreachableWithSignals);
		if (bNoWay && !State.bUnreachableHandled && State.bManaged && State.bHasJob && Time - State.SelfDrivingErrorSince >= UnreachableSeconds
			&& State.Job.Legs.IsValidIndex(State.Job.Leg) && IsValid(State.Job.Legs[State.Job.Leg].Station))
		{
			State.bUnreachableHandled = true;
			AFGTrainStationIdentifier* Station = State.Job.Legs[State.Job.Leg].Station.Get();
			State.Unreachable.Add(Station, Time + UnreachableMinutes * 60.0);
			UE_LOG(LogSmartTrains, Warning, TEXT("UNREACHABLE %s: the game finds no way to %s (%s for %.0f s, job %d); this train leaves it out for %.0f minutes. Check which way the station faces (a train arrives in its direction; a station built backwards reads as unreachable with signals), then the signals on the way in (a train may not pass a one-way signal from behind)."),
				*STLayout::Name(Train), *STLayout::Name(Station), *StaticEnum<ESelfDrivingLocomotiveError>()->GetNameStringByValue(static_cast<int64>(Error)),
				Time - State.SelfDrivingErrorSince, State.Job.Id, UnreachableMinutes);
			if (State.Job.bPark || State.Job.bRepeat)
			{
				State.bHasJob = false;
				State.Phase = ESTTrainPhase::Idle;
				SettleIdle(State, TEXT("the station it was heading for is unreachable"), true);
			}
			else
			{
				State.bReplan = true;
				State.bReplanFresh = true;
				bDispatchSoon = true;
			}
		}
		const bool bLasting = Error == 0 || Time - State.SelfDrivingErrorSince >= SelfDrivingErrorLogSeconds;
		if (Error != State.LastSelfDrivingError && bLasting)
		{
			UE_LOG(LogSmartTrains, Log, TEXT("SELFDRIVING %s: %s%s"), *STLayout::Name(Train),
				*StaticEnum<ESelfDrivingLocomotiveError>()->GetNameStringByValue(static_cast<int64>(Error)),
				Error != 0 && State.bHasJob ? *FString::Printf(TEXT(" for %.0f s (job %d, leg %d)"), Time - State.SelfDrivingErrorSince, State.Job.Id, State.Job.Leg + 1) : TEXT(""));
			State.LastSelfDrivingError = Error;
		}
		// A player in the driver's seat, logged when it changes: driving by hand holds the train (no new trips).
		const uint8 Aboard = !Train->IsPlayerDriven() ? 0 : (Train->IsSelfDrivingEnabled() ? 1 : 2);
		if (Aboard != State.PlayerAboard)
		{
			UE_LOG(LogSmartTrains, Log, TEXT("PLAYER %s: %s"), *STLayout::Name(Train), Aboard == 0 ? TEXT("no player driving it now")
				: (Aboard == 1 ? TEXT("a player is in the driver's seat with autopilot on; it keeps getting trips")
				: TEXT("a player drives it by hand (autopilot off); no new trips until autopilot is on")));
			State.PlayerAboard = Aboard;
		}
		if (State.Phase == ESTTrainPhase::Parked && Train->IsSelfDrivingEnabled() && !bDocked)
		{
			UE_LOG(LogSmartTrains, Log, TEXT("%s: autopilot switched on while parked; it gets the next job that fits"), *STLayout::Name(Train));
			State.Phase = ESTTrainPhase::Idle;
			SettleIdle(State, TEXT("autopilot switched on by someone"));
		}
	}
}

int32 ASTSubsystem::EmptyTrainCars(AFGTrain* Train, const FString& Cause)
{
	if (!IsValid(Train))
	{
		return 0;
	}
	int64 Removed = 0;
	TArray<FString> Items;
	for (const TPair<UClass*, int32>& Each : GetCargo(Train))
	{
		const bool bFluid = STNames::IsFluid(Each.Key);
		Removed += bFluid ? Each.Value / 1000 : Each.Value;
		Items.Add(FString::Printf(TEXT("%d %s"), bFluid ? Each.Value / 1000 : Each.Value, *STNames::Item(Each.Key)));
	}
	for (AFGRailroadVehicle* Vehicle : STLayout::GetVehicles(Train))
	{
		if (AFGFreightWagon* Car = Cast<AFGFreightWagon>(Vehicle))
		{
			if (UFGInventoryComponent* Inventory = Car->GetFreightInventory())
			{
				Inventory->Empty();
			}
		}
	}
	UE_LOG(LogSmartTrains, Log, TEXT("EMPTY %s by %s: removed %s"), *STLayout::Name(Train), *Cause, Items.Num() > 0 ? *FString::Join(Items, TEXT(", ")) : TEXT("nothing"));
	if (FSTTrainState* State = mTrains.FindByPredicate([Train](const FSTTrainState& Each) { return Each.Train == Train; }); State && State->bManaged && State->bHasJob && !State->Job.bPark)
	{
		State->bReplan = true;
		State->bReplanFresh = true;
		bDispatchSoon = true;
	}
	return static_cast<int32>(FMath::Min<int64>(Removed, MAX_int32));
}

TArray<AFGTrain*> ASTSubsystem::GetManagedTrains() const
{
	TArray<AFGTrain*> Trains;
	for (const FSTTrainState& State : mTrains)
	{
		if (State.bManaged && IsValid(State.Train))
		{
			Trains.Add(State.Train);
		}
	}
	return Trains;
}

FString ASTSubsystem::DescribeStationPlatforms(const AFGTrainStationIdentifier* Station) const
{
	TArray<FString> Lines;
	const TArray<AFGBuildableTrainPlatform*> Platforms = STLayout::GetPlatforms(Station ? Station->GetStation() : nullptr);
	for (int32 Index = 0; Index < Platforms.Num(); ++Index)
	{
		const AFGBuildableTrainPlatformCargo* Cargo = Cast<AFGBuildableTrainPlatformCargo>(Platforms[Index]);
		if (!Cargo || !STLayout::CanMoveCargo(Cargo))
		{
			continue;
		}
		const UFGInventoryComponent* Inventory = Cargo->GetInventory();
		TMap<UClass*, int32> Held;
		int32 Used = 0;
		if (Inventory)
		{
			TArray<FInventoryStack> Stacks;
			Inventory->GetInventoryStacks(Stacks);
			for (const FInventoryStack& Stack : Stacks)
			{
				if (Stack.HasItems() && Stack.Item.GetItemClass())
				{
					Held.FindOrAdd(Stack.Item.GetItemClass()) += Stack.NumItems;
					++Used;
				}
			}
		}
		TArray<FString> Items;
		for (const TPair<UClass*, int32>& Each : Held)
		{
			Items.Add(FString::Printf(TEXT("%d %s"), STNames::IsFluid(Each.Key) ? Each.Value / 1000 : Each.Value, *STNames::Item(Each.Key)));
		}
		const FSTPlatformConfig* Config = FindPlatformConfig(Cargo);
		Lines.Add(FString::Printf(TEXT("P%d %s, %s, %d of %d slots used%s%s"), Index + 1, Cargo->GetIsInLoadMode() ? TEXT("load") : TEXT("unload"),
			Config ? *STNames::Settings(Config->Role, Config->Items) : TEXT("no role"), Used, Inventory ? Inventory->GetSizeLinear() : 0,
			Items.Num() > 0 ? TEXT(": ") : TEXT(", empty"), *FString::Join(Items, TEXT(", "))));
	}
	return Lines.Num() > 0 ? FString::Join(Lines, TEXT("; ")) : FString(TEXT("none found"));
}

void ASTSubsystem::ReleaseDockWhenDone(FSTTrainState& State, double Time)
{
	AFGTrainStationIdentifier* Station = State.DockedStation.Get();
	if (State.bDockReleased || !State.bHasJob || State.Job.bPark || !Station || !State.Job.Legs.IsValidIndex(State.Job.Leg)
		|| State.Job.Legs[State.Job.Leg].Station != Station)
	{
		return;
	}
	const FSTJobLeg& Leg = State.Job.Legs[State.Job.Leg];
	if (!Leg.HasWork())
	{
		return;
	}
	const TArray<int32> Before = mDockStartAmounts.FindRef(State.Train);
	int32 Moved = 0;
	bool bAllDone = true;
	for (int32 Index = 0; Index < Leg.Work.Num(); ++Index)
	{
		const FSTCarWork& Work = Leg.Work[Index];
		const int32 Start = Before.IsValidIndex(Index) ? Before[Index] : 0;
		const int32 Done = FMath::Abs(CountInCar(Work.Car, Work.Item) - Start);
		Moved += Done;
		bAllDone &= Done >= Work.PlannedAmount;
	}
	if (Moved != State.DockMoved)
	{
		State.DockMoved = Moved;
		State.DockProgressAt = Time;
	}
	const bool bIdle = Time - State.DockedAt >= DockFirstTransferSeconds && Time - State.DockProgressAt >= DockIdleSeconds;
	if (!bAllDone && !bIdle)
	{
		return;
	}
	AFGBuildableRailroadStation* Building = Station->GetStation();
	if (!Building)
	{
		return;
	}
	State.bDockReleased = true;
	Building->ClearRuleSetTimeRequirement();
	UE_LOG(LogSmartTrains, Log, TEXT("DOCK %s at %s: %s after %.0f s (%d moved); the stay ends now"), *STLayout::Name(State.Train), *STLayout::Name(Station),
		bAllDone ? TEXT("planned transfer done") : *FString::Printf(TEXT("nothing moved for %.0f s"), Time - State.DockProgressAt), Time - State.DockedAt, Moved);
}

void ASTSubsystem::NoteDockStarted(AFGTrain* Train, AFGTrainStationIdentifier* Station)
{
	FSTTrainState* State = Train ? FindTrainState(Train) : nullptr;
	if (!State || !State->bManaged || State->bWasDocked)
	{
		return;
	}
	BeginDock(*State, Station);
}

void ASTSubsystem::BeginDock(FSTTrainState& State, AFGTrainStationIdentifier* Station)
{
	const double Time = Now();
	State.bWasDocked = true;
	State.DockedAt = Time;
	State.DockedStation = Station;
	State.bDockReleased = false;
	State.DockMoved = 0;
	State.DockProgressAt = Time;
	OnDocked(State, Station);
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
	if (State.Job.Legs[State.Job.Leg].Station != Station)
	{
		// Docked at a later stop of the trip: the stops in between were missed; carry on from this one.
		int32 Later = INDEX_NONE;
		for (int32 Index = State.Job.Leg + 1; Index < State.Job.Legs.Num() && Later == INDEX_NONE; ++Index)
		{
			Later = State.Job.Legs[Index].Station == Station ? Index : INDEX_NONE;
		}
		if (Later == INDEX_NONE)
		{
			UE_LOG(LogSmartTrains, Warning, TEXT("%s docked at %s, which job %d doesn't stop at (next is leg %d, %s); the rest of the trip is planned again"),
				*STLayout::Name(Train), *STLayout::Name(Station), State.Job.Id, State.Job.Leg + 1, *STLayout::Name(State.Job.Legs[State.Job.Leg].Station));
			State.bReplan = true;
			bDispatchSoon = true;
			return;
		}
		UE_LOG(LogSmartTrains, Warning, TEXT("%s docked at %s, leg %d of job %d; legs %d to %d were missed"), *STLayout::Name(Train), *STLayout::Name(Station), Later + 1,
			State.Job.Id, State.Job.Leg + 1, Later);
		State.Job.Leg = Later;
	}
	const FSTJobLeg& Leg = State.Job.Legs[State.Job.Leg];
	if (State.LastStation && State.LastDepartureAt > 0.0)
	{
		LearnTrip(State.LastStation, Station, Now() - State.LastDepartureAt);
	}
	TArray<int32> Amounts;
	for (const FSTCarWork& Work : Leg.Work)
	{
		Amounts.Add(CountInCar(Work.Car, Work.Item));
	}
	mDockStartAmounts.Add(Train, Amounts);
	const double Late = Now() - Leg.PlannedArrival;
	if (mThroughput && !State.Job.bPark && !State.Job.bRepeat)
	{
		mThroughput->RecordArrival(Station, Late, Now());
	}
	UE_LOG(LogSmartTrains, Log, TEXT("JOB %d %s: docked at %s for leg %d/%d (%s), %+.0f s against plan"), State.Job.Id, *STLayout::Name(Train), *STLayout::Name(Station),
		State.Job.Leg + 1, State.Job.Legs.Num(), Leg.HasWork() ? *DescribeLeg(Train, Leg, false) : TEXT("no transfer"), Late);
	if (State.Job.Leg == State.Job.Legs.Num() - 1 && !State.Job.bPark)
	{
		bDispatchSoon = true;
	}
}

void ASTSubsystem::OnUndocked(FSTTrainState& State, AFGTrainStationIdentifier* Station, double Stay)
{
	AFGTrain* Train = State.Train;
	mPendingDockMaps.Remove(Train);
	State.bRequestRecheck = State.bHasJob && State.Job.bRequest && State.Requests.Num() > 0;
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
		if (State.bWriteOnUndock)
		{
			State.bWriteOnUndock = false;
			WriteJob(State, TEXT("next job, written as it left"));
		}
		return;
	}
	if (!State.Job.Legs.IsValidIndex(State.Job.Leg) || State.Job.Legs[State.Job.Leg].Station != Station)
	{
		UE_LOG(LogSmartTrains, Log, TEXT("%s left %s after %.0f s; job %d continues at leg %d"), *STLayout::Name(Train), *STLayout::Name(Station), Stay,
			State.Job.Id, State.Job.Leg + 1);
		return;
	}
	FSTJobLeg& Leg = State.Job.Legs[State.Job.Leg];
	const TArray<int32> Before = mDockStartAmounts.FindRef(Train);
	for (int32 Index = 0; Index < Leg.Work.Num(); ++Index)
	{
		FSTCarWork& Work = Leg.Work[Index];
		const int32 Start = Before.IsValidIndex(Index) ? Before[Index] : 0;
		Work.MovedAmount = FMath::Abs(CountInCar(Work.Car, Work.Item) - Start);
		if (!Work.bLoad && Work.MovedAmount > 0)
		{
			mLastServed.Add(TPair<TWeakObjectPtr<AFGTrainStationIdentifier>, UClass*>(Station, Work.Item.Get()), Now());
		}
		if (!Work.bLoad && Work.Platform && Work.PlannedAmount > 0 && Work.MovedAmount < SlowDrainShare * Work.PlannedAmount)
		{
			mSlowDrainUntil.Add(Work.Platform.Get(), Now() + SlowDrainMinutes * 60.0);
			UE_LOG(LogSmartTrains, Log, TEXT("SLOWDRAIN %s took %d of %d planned %s from %s; its room counts no drain for %.0f minutes"),
				*STLayout::DescribePlatform(Work.Platform.Get()), Work.MovedAmount, Work.PlannedAmount, *STNames::Item(Work.Item.Get()), *STLayout::Name(Train), SlowDrainMinutes);
		}
	}
	if (Leg.HasWork())
	{
		LearnStay(Station, Stay);
	}
	// Planned moves that moved nothing at all: not planned again for this train for a while, so it doesn't loop.
	TMap<TPair<UClass*, bool>, TPair<int32, int32>> PerItem;
	for (const FSTCarWork& Work : Leg.Work)
	{
		TPair<int32, int32>& Sum = PerItem.FindOrAdd(TPair<UClass*, bool>(Work.Item.Get(), Work.bLoad));
		Sum.Key += Work.PlannedAmount;
		Sum.Value += Work.MovedAmount;
	}
	for (const TPair<TPair<UClass*, bool>, TPair<int32, int32>>& Each : PerItem)
	{
		if (Each.Value.Key > 0 && Each.Value.Value == 0 && Each.Key.Key)
		{
			State.NoProgress.RemoveAll([&](const FSTNoProgress& Old) { return Old.Until <= Now() || (Old.Station == Station && Old.Item == Each.Key.Key && Old.bLoad == Each.Key.Value); });
			State.NoProgress.Add({Station, Each.Key.Key, Each.Key.Value, Now() + NoProgressMinutes * 60.0});
			const FString Text = FString::Printf(TEXT("NOPROGRESS %s %s no %s at %s (%d planned) in %.0f s; not planned again for %.0f minutes. Platforms there: %s"), *STLayout::Name(Train),
				Each.Key.Value ? TEXT("loaded") : TEXT("unloaded"), *STNames::Item(Each.Key.Key), *STLayout::Name(Station), Each.Value.Key, Stay, NoProgressMinutes,
				*DescribeStationPlatforms(Station));
			// A repeat run moving nothing is expected (nothing was worth a new trip); only the detailed log shows it.
			if (State.Job.bRepeat)
			{
				UE_LOG(LogSmartTrains, Verbose, TEXT("%s"), *Text);
			}
			else
			{
				UE_LOG(LogSmartTrains, Log, TEXT("%s"), *Text);
			}
		}
	}
	const FString Line = FString::Printf(TEXT("JOB %d %s: left %s after %.0f s: %s"), State.Job.Id, *STLayout::Name(Train), *STLayout::Name(Station), Stay,
		Leg.HasWork() ? *DescribeLeg(Train, Leg, true) : TEXT("no transfer"));
	UE_LOG(LogSmartTrains, Log, TEXT("%s"), *Line);
	if (Leg.HasWork())
	{
		AddHistory(Station, Line);
	}
	++State.Job.Leg;
	State.Job.LegStartedAt = Now();
	if (State.Job.Leg >= State.Job.Legs.Num())
	{
		FinishJob(State);
	}
	else if (State.Job.bRepeat)
	{
		// A repeat is looked at again at every stop: a new trip, or the rest of it again if it still has work.
		FinishJob(State);
	}
	else if (!State.Job.bPark)
	{
		// The rest of the trip is planned again from what the cars really hold now.
		State.bReplan = true;
		bDispatchSoon = true;
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
	for (AFGRailroadVehicle* Vehicle : STLayout::GetVehicles(Train))
	{
		Seen.Consist.Add(Vehicle);
	}
	Seen.Locomotive = Train->GetMultipleUnitMaster();
	// One map per way round, told apart by the locomotive that docked (older maps by the first docked vehicle): the
	// one of the same way round is replaced, maps of another consist go.
	int32 FirstIndex = Seen.Vehicles.IndexOfByPredicate([](const TObjectPtr<AFGRailroadVehicle>& Vehicle) { return Vehicle != nullptr; });
	mDockMaps.RemoveAll([&](const FSTDockMap& Map)
	{
		if (Map.Train != Train || Map.Station != Station)
		{
			return false;
		}
		const bool bSameWayRound = Map.Locomotive && Seen.Locomotive ? Map.Locomotive == Seen.Locomotive
			: Map.Vehicles.IsValidIndex(FirstIndex) && Map.Vehicles[FirstIndex] == Seen.Vehicles[FirstIndex];
		return bSameWayRound || Map.Consist != Seen.Consist;
	});
	mDockMaps.Add(Seen);
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
	// A trip held up in a queue or a deadlock says little about the next one: once the pair is known, one sample counts
	// at most TripOutlierFactor times the estimate.
	const float Counted = Trip->Samples >= 2 ? FMath::Min(Seconds, Trip->Seconds * TripOutlierFactor) : Seconds;
	Trip->Seconds = Trip->Samples == 0 ? Counted : FMath::Lerp(Trip->Seconds, Counted, 0.3f);
	++Trip->Samples;
	UE_LOG(LogSmartTrains, Log, TEXT("TRIP %s -> %s took %.0f s (estimate was %.0f s, now %.0f s from %d trips%s)"), *STLayout::Name(From), *STLayout::Name(To), Seconds,
		Estimate, Trip->Seconds, Trip->Samples, Counted < Seconds ? *FString::Printf(TEXT("; counted as %.0f s"), Counted) : TEXT(""));
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

bool ASTSubsystem::IsPushPull(const AFGTrain* Train)
{
	return STLayout::GetPossibleDockingLocomotives(Train).Num() > 1;
}

float ASTSubsystem::EstimateTravelSeconds(const AFGTrainStationIdentifier* From, const AFGTrainStationIdentifier* To, bool bPushPull) const
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
	float Distance = mTravel ? mTravel->GetDistance(From, To, &bReversing, bPushPull) : -1.f;
	// Auto Trains' time for a pair not driven yet: the way the signals allow, at real speeds and acceleration. Asked a
	// few pairs per dispatch round and kept 10 minutes; SmartTrains' own estimate stands in meanwhile.
	const TPair<TWeakObjectPtr<AFGTrainStationIdentifier>, TWeakObjectPtr<AFGTrainStationIdentifier>> Key(const_cast<AFGTrainStationIdentifier*>(From),
		const_cast<AFGTrainStationIdentifier*>(To));
	TPair<float, double>* Cached = mAtTravel.Find(Key);
	if ((!Cached || Now() - Cached->Value > 600.0) && mAtTravelCallsLeft > 0 && STAt::IsAvailable(GetWorld()))
	{
		--mAtTravelCallsLeft;
		const float AtSeconds = STAt::EstimateTravelSeconds(GetWorld(), nullptr, const_cast<AFGBuildableRailroadStation*>(From->GetStation()),
			const_cast<AFGBuildableRailroadStation*>(To->GetStation()));
		if (!Cached && AtSeconds >= 0.f && Distance == -2.f)
		{
			UE_LOG(LogSmartTrains, Log, TEXT("TRAVEL %s -> %s: SmartTrains' search finds no way, Auto Trains does (%.0f s); using that"), *STLayout::Name(From),
				*STLayout::Name(To), AtSeconds);
		}
		Cached = &mAtTravel.Add(Key, TPair<float, double>(AtSeconds, Now()));
	}
	// Auto Trains may leave either way, so a way only it finds is for push-pull trains.
	if (Cached && Cached->Key >= 0.f && (Distance != -2.f || bPushPull))
	{
		// Plus time to start and stop at the stations.
		return Cached->Key + 20.f;
	}
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

bool ASTSubsystem::IsNoProgress(const FSTTrainState& State, const AFGTrainStationIdentifier* Station, const UClass* Item, bool bLoad) const
{
	const double Time = Now();
	return State.NoProgress.ContainsByPredicate([&](const FSTNoProgress& Each)
	{
		return Each.Until > Time && Each.Station.Get() == Station && Each.Item.Get() == Item && Each.bLoad == bLoad;
	});
}

int32 ASTSubsystem::GetWaitingRoom(const AFGTrainStationIdentifier* Station, float TrainLength) const
{
	// Room a waiting train leaves to the train ahead and to the switch. [cm]
	constexpr float WaitClearance = 500.f;
	FSTTravel::FApproach Approach;
	if (!mTravel || TrainLength <= 0.f || !Station || !mTravel->GetApproach(Station, Approach))
	{
		return -1;
	}
	return FMath::FloorToInt(Approach.WaitLength / (TrainLength + WaitClearance));
}

const void* ASTSubsystem::GetJunction(const AFGTrainStationIdentifier* Station) const
{
	FSTTravel::FApproach Approach;
	return mTravel && Station && mTravel->GetApproach(Station, Approach) ? Approach.Junction : nullptr;
}

bool ASTSubsystem::IsDeadEnd(const AFGTrainStationIdentifier* Station) const
{
	return mTravel && Station && mTravel->IsDeadEnd(Station);
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
	const FString Current = Train->GetTrainName().ToString();
	if (!State.bRenamed)
	{
		State.OriginalName = Train->GetTrainName();
		State.bRenamed = true;
	}
	else if (!State.LastWrittenName.IsEmpty() && Current != State.LastWrittenName && !Current.StartsWith(TEXT("ST: ")))
	{
		// Renamed by a player since: that name is the one to put back later.
		State.OriginalName = Train->GetTrainName();
		UE_LOG(LogSmartTrains, Log, TEXT("%s was renamed by a player; that name comes back when SmartTrains stops naming it"), *Current);
	}
	// Idle trains carry where they stand, so they can be told apart.
	FString Name = State.bHasJob ? FString::Printf(TEXT("ST: %s"), *State.Job.Summary) : FString::Printf(TEXT("ST: %s"), STNames::Phase(State.Phase));
	if (!State.bHasJob && State.LastStation)
	{
		Name += FString::Printf(TEXT(" at %s"), *STLayout::Name(State.LastStation));
	}
	if (Current != Name)
	{
		Train->SetTrainName(FText::FromString(Name));
	}
	State.LastWrittenName = Name;
}

//~ Readouts

TArray<FString> ASTSubsystem::DescribePlatform(const AFGBuildableTrainPlatformCargo* Platform) const
{
	TArray<FString> Lines;
	const FSTPlatformView View = MakePlatformView(Platform, 0);
	Lines.Add(FString::Printf(TEXT("%s: %s, %s mode"), *View.Title, STNames::Role(View.Role), Platform && Platform->GetIsInLoadMode() ? TEXT("load") : TEXT("unload")));
	if (View.Role == ESTRole::Off)
	{
		Lines.Add(TEXT("  Vanilla. Set a role with /st platform supply|demand <item>, or /st platform sink."));
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
	Lines.Add(FString::Printf(TEXT("Station %s%s"), *STLayout::Name(Station), Config && Config->bDepot ? TEXT(" (depot)") : TEXT("")));
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
			Piece += FString::Printf(TEXT("-%s[%s]"), STNames::Role(PlatformConfig->Role), *Items);
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
			const FString* Reason = mIdleReasons.Find(State.Train.Get());
			Lines.Add(FString::Printf(TEXT("%s: %s%s%s"), *STLayout::Name(State.Train), STNames::Phase(State.Phase),
				Observed ? *FString::Printf(TEXT(" - would: %s"), **Observed) : TEXT(""), Reason ? *FString::Printf(TEXT(" - no trip: %s"), **Reason) : TEXT("")));
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
	int32 Sinks = 0;
	for (const FSTPlatformConfig& Config : mPlatforms)
	{
		Supply += Config.Role == ESTRole::Supply ? 1 : 0;
		Demand += Config.Role == ESTRole::Demand ? 1 : 0;
		Sinks += Config.Role == ESTRole::Sink ? 1 : 0;
	}
	Lines.Add(FString::Printf(TEXT("SmartTrains%s: %d managed trains (%d working), %d supply, %d demand and %d sink platforms, %d stations searched for distances, Freight Car Control %s"),
		mSettings.bObserveOnly ? TEXT(" (observe only)") : TEXT(""), Managed, Working, Supply, Demand, Sinks, mTravel ? mTravel->GetSearchedCount() : 0,
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
	int32 Unwanted = 0;
	for (const FSTTrainState& State : mTrains)
	{
		Unwanted += State.bManaged && State.UnwantedCargoSince > 0.0 ? 1 : 0;
	}
	if (Managed > 0 && Unwanted * 2 > Managed)
	{
		Lines.Add(FString::Printf(TEXT("  Warning: %d of %d managed trains carry cargo no consumer wants. Check demand targets, or add an any-item sink."), Unwanted, Managed));
	}
	return Lines;
}

FSTMonitorView ASTSubsystem::MakeMonitorView() const
{
	return mThroughput ? mThroughput->MakeView(*this) : FSTMonitorView();
}

TArray<FString> ASTSubsystem::DescribeMonitor() const
{
	return FSTThroughput::Describe(MakeMonitorView());
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

double ASTSubsystem::GetLastServed(const AFGTrainStationIdentifier* Station, UClass* Item) const
{
	const double* Found = mLastServed.Find(TPair<TWeakObjectPtr<AFGTrainStationIdentifier>, UClass*>(const_cast<AFGTrainStationIdentifier*>(Station), Item));
	return Found ? *Found : mStartTime;
}

int32 ASTSubsystem::CountInCar(const AFGRailroadVehicle* Car, TSubclassOf<UFGItemDescriptor> Item)
{
	const AFGFreightWagon* Wagon = Cast<AFGFreightWagon>(Car);
	const UFGInventoryComponent* Inventory = Wagon ? Wagon->GetFreightInventory() : nullptr;
	return Inventory && Item ? Inventory->GetNumItems(Item) : 0;
}

FString ASTSubsystem::DescribeLeg(const AFGTrain* Train, const FSTJobLeg& Leg, bool bMoved) const
{
	TArray<FString> Parts;
	for (const FSTCarWork& Work : Leg.Work)
	{
		const FString Amount = bMoved
			? FString::Printf(TEXT("%s of %s"), *STNames::Amount(Work.Item, Work.MovedAmount), *STNames::Amount(Work.Item, Work.PlannedAmount))
			: STNames::Amount(Work.Item, Work.PlannedAmount);
		Parts.Add(FString::Printf(TEXT("%s %s %s %s"), *STLayout::DescribeVehicle(Train, Work.Car), Work.bLoad ? TEXT("loads") : TEXT("unloads"), *Amount,
			*STNames::Item(Work.Item)));
	}
	return FString::Join(Parts, TEXT(", "));
}
