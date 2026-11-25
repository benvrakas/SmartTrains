#include "STSubsystem.h"
#include "Buildables/FGBuildableRailroadStation.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "Engine/World.h"
#include "FGFreightWagon.h"
#include "FGInventoryComponent.h"
#include "FGLocomotive.h"
#include "FGRailroadSubsystem.h"
#include "FGRailroadTrackConnectionComponent.h"
#include "FGRailroadTimeTable.h"
#include "FGTrain.h"
#include "FGTrainStationIdentifier.h"
#include "Resources/FGItemDescriptor.h"
#include "SmartTrains.h"
#include "STBridges.h"
#include "STLayout.h"
#include "STTrace.h"
#include "STTravel.h"
#include "HAL/IConsoleManager.h"
#include "RailroadNavigation.h"
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

namespace
{
	// Manage new trains: a new train is managed once its vehicles haven't changed for this long. [s]
	constexpr double NewTrainSettleSeconds = 30.0;
	// A trip sample counts at most this many times the pair's estimate (queues and deadlocks are outliers); one this many
	// times faster than the learned time replaces it.
	constexpr float TripOutlierFactor = 1.5f;
	// A self-driving error is logged once it has lasted this long; shorter ones are the game re-routing. [s]
	constexpr double SelfDrivingErrorLogSeconds = 5.0;
	// How often the loops are checked (CheckLoops). [s]
	constexpr float LoopCheckSeconds = 5.f;

	// Share of the freight cars' room Train's cargo fills, 0..1 (each car by its own item).
	double GetFillShare(const AFGTrain* Train)
	{
		double Sum = 0.0;
		int32 Cars = 0;
		for (const AFGRailroadVehicle* Vehicle : STLayout::GetVehicles(Train))
		{
			const AFGFreightWagon* Car = Cast<AFGFreightWagon>(Vehicle);
			const UFGInventoryComponent* Inventory = Car ? Car->GetFreightInventory() : nullptr;
			if (!Inventory)
			{
				continue;
			}
			++Cars;
			TArray<FInventoryStack> Stacks;
			Inventory->GetInventoryStacks(Stacks);
			double Share = 0.0;
			for (const FInventoryStack& Stack : Stacks)
			{
				const double Capacity = Stack.HasItems() ? STLayout::GetCarCapacity(Car, Stack.Item.GetItemClass()) : 0.0;
				Share += Capacity > 0.0 ? Stack.NumItems / Capacity : 0.0;
			}
			Sum += FMath::Min(1.0, Share);
		}
		return Cars > 0 ? Sum / Cars : 0.0;
	}
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
	Timers.SetTimer(mNewTrainTimer, this, &ASTSubsystem::ManageNewTrains, 5.f, true, 10.f);
	Timers.SetTimer(mTravelTimer, this, &ASTSubsystem::StepTravel, 0.5f, true, 2.f);
	Timers.SetTimer(mLoopTimer, this, &ASTSubsystem::CheckLoops, LoopCheckSeconds, true, 5.f);
	// A save from before 0.14.0: its platforms' machines are traced once.
	if (!bMachinesFilled)
	{
		for (const FSTPlatformConfig& Config : mPlatforms)
		{
			if (IsValid(Config.Platform) && (Config.Role == ESTRole::Supply || Config.Role == ESTRole::Demand) && Config.Machines.Num() == 0)
			{
				mMachineFillQueue.Add(Config.Platform);
			}
		}
		if (mMachineFillQueue.Num() > 0)
		{
			UE_LOG(LogSmartTrains, Log, TEXT("SETUP %d platforms set up before 0.14.0: tracing each once for its recipe rate, a few at a time"), mMachineFillQueue.Num());
			StartSetupSteps();
		}
		else
		{
			bMachinesFilled = true;
		}
	}
	int32 Managed = 0;
	for (const FSTTrainState& State : mTrains)
	{
		Managed += State.bManaged ? 1 : 0;
	}
	UE_LOG(LogSmartTrains, Log, TEXT("Subsystem ready: %d platform settings, %d trains (%d managed), %d dock maps, %d timed trips; Freight Car Control %s; DynamicTimeTable %s"),
		mPlatforms.Num(), mTrains.Num(), Managed, mDockMaps.Num(), mTrips.Num(),
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
	mLoopsVersion = 1;
	mPlatforms.RemoveAll([](const FSTPlatformConfig& Config) { return !IsValid(Config.Platform) || (Config.Role == ESTRole::Off && Config.Items.Num() == 0); });
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
			Saved = FMath::Max(Saved, State.LastDepartureAt);
			Saved = FMath::Max(Saved, FMath::Max(State.Job.StartedAt, State.Job.LegStartedAt));
		}
	}
	const UWorld* World = GetWorld();
	mClockBase = Saved - (World ? World->GetTimeSeconds() : 0.0);
	UE_LOG(LogSmartTrains, Log, TEXT("Clock continues at %.0f s%s"), Saved, mClockAtSave > 0.0 ? TEXT("") : TEXT(" (save without a clock: latest saved train time)"));
	if (mLoopsVersion < 1)
	{
		// Jobs from before 0.50.0 are trips of the old dispatcher: dropped; the loop plan gives every train its loop.
		int32 Dropped = 0;
		for (FSTTrainState& State : mTrains)
		{
			Dropped += State.bHasJob ? 1 : 0;
			State.bHasJob = false;
			State.Job = FSTJob();
			State.LoopConsumer = nullptr;
			State.CarSets.Reset();
			State.WrittenStations.Reset();
			if (State.Phase == ESTTrainPhase::Working)
			{
				State.Phase = ESTTrainPhase::Idle;
			}
		}
		UE_LOG(LogSmartTrains, Log, TEXT("Loaded a save from before 0.50.0: %d trips of the old dispatcher dropped; each managed train gets its loop"), Dropped);
	}
	PreSaveGame_Implementation(SaveVersion, GameVersion);
	for (FSTTrainState& State : mTrains)
	{
		State.bWasDocked = false;
	}
	UE_LOG(LogSmartTrains, Log, TEXT("Loaded: %d platform settings, %d trains, %d dock maps, %d timed trips, %d timed stays"),
		mPlatforms.Num(), mTrains.Num(), mDockMaps.Num(), mTrips.Num(), mStays.Num());
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

FSTTrainState* ASTSubsystem::FindTrainState(const AFGTrain* Train)
{
	return mTrains.FindByPredicate([Train](const FSTTrainState& State) { return State.Train == Train; });
}

const FSTTrainState* ASTSubsystem::FindTrainState(const AFGTrain* Train) const
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
		NewItems += (NewItems.IsEmpty() ? TEXT("") : TEXT(", ")) + STNames::Item(Rule.Item);
	}
	int32 Index = INDEX_NONE;
	const AFGBuildableRailroadStation* Station = STLayout::FindStation(Platform, &Index);
	UE_LOG(LogSmartTrains, Log, TEXT("PLATFORM %s #%d (%s): %s [%s] -> %s [%s] by %s"), *STLayout::Name(Station), Index + 1, *Platform->GetName(),
		STNames::Role(OldRole), *OldItems, STNames::Role(NewRole), *NewItems, *Cause);
	if (!Station)
	{
		UE_LOG(LogSmartTrains, Warning, TEXT("PLATFORM %s is not on the output side of any station; trains can't use it"), *Platform->GetName());
	}
	if (AFGTrainStationIdentifier* Identifier = STLayout::GetIdentifier(Station))
	{
		mRatesDirty.Add(Identifier);
	}
	bCheckSoon = true;
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
		if (!Train->IsDocked() && !Train->IsSelfDrivingEnabled())
		{
			State.Phase = ESTTrainPhase::Parked;
		}
		bCheckSoon = true;
		return true;
	}

	State.bManaged = false;
	State.bHasJob = false;
	State.Phase = ESTTrainPhase::Idle;
	State.LoopConsumer = nullptr;
	bCheckSoon = true;
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
	LogFleet();
	for (FSTTrainState& State : mTrains)
	{
		AFGTrain* Train = State.Train;
		if (!State.bManaged || !IsValid(Train))
		{
			continue;
		}
		AFGRailroadTimeTable* TimeTable = Train->GetTimeTable();
		const bool bDocked = Train->IsDocked();
		// Docks are normally begun by the dock hook (NoteDockStarted); this catches any it missed. The game moved the
		// timetable on when it docked, so the stop it docked at is the one before the current.
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
		if (mSettings.bObserveOnly || !TimeTable)
		{
			continue;
		}
		// Someone else changed the timetable SmartTrains wrote: it is written back, unless that keeps happening.
		if (State.bHasJob && State.Phase == ESTTrainPhase::Working && State.WrittenStations.Num() > 0)
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
					State.WrittenStopKeys.Reset();
					WriteJob(State, TEXT("written back after an external change"));
				}
				else
				{
					UE_LOG(LogSmartTrains, Error, TEXT("EXTERNAL TIMETABLE CHANGE on %s keeps happening (stops now [%s]): job %d dropped. If DynamicTimeTable does this, take these stations out of its groups."),
						*STLayout::Name(Train), *Listed, State.Job.Id);
					EndJob(State, TEXT("its timetable keeps being changed by someone else"));
				}
			}
		}
		// The game's self-driving error (no path, station unreachable, ...), logged once it has lasted a while and when it
		// clears. An invalid next stop gets the timetable written again.
		const uint8 Error = Train->IsSelfDrivingEnabled() ? static_cast<uint8>(Train->GetSelfDrivingError()) : 0;
		if (Error != State.SelfDrivingError)
		{
			State.SelfDrivingError = Error;
			State.SelfDrivingErrorSince = Time;
			State.bInvalidStopHandled = false;
		}
		const bool bLasting = Error == 0 || Time - State.SelfDrivingErrorSince >= SelfDrivingErrorLogSeconds;
		if (Error == static_cast<uint8>(ESelfDrivingLocomotiveError::SDLE_InvalidNextStop) && !State.bInvalidStopHandled && State.bHasJob && bLasting)
		{
			State.bInvalidStopHandled = true;
			UE_LOG(LogSmartTrains, Warning, TEXT("%s: the game says its next stop is invalid (job %d); its timetable is written again"), *STLayout::Name(Train), State.Job.Id);
			State.WrittenStopKeys.Reset();
			WriteJob(State, TEXT("its next stop was invalid"));
		}
		if (Error != State.LastSelfDrivingError && bLasting)
		{
			UE_LOG(LogSmartTrains, Log, TEXT("SELFDRIVING %s: %s%s"), *STLayout::Name(Train),
				*StaticEnum<ESelfDrivingLocomotiveError>()->GetNameStringByValue(static_cast<int64>(Error)),
				Error != 0 && State.bHasJob ? *FString::Printf(TEXT(" for %.0f s (job %d, stop %d)"), Time - State.SelfDrivingErrorSince, State.Job.Id, State.Job.Leg + 1) : TEXT(""));
			State.LastSelfDrivingError = Error;
		}
		// A player in the driver's seat, logged when it changes: driving by hand takes it off its loop until autopilot is on.
		const uint8 Aboard = !Train->IsPlayerDriven() ? 0 : (Train->IsSelfDrivingEnabled() ? 1 : 2);
		if (Aboard != State.PlayerAboard)
		{
			UE_LOG(LogSmartTrains, Log, TEXT("PLAYER %s: %s"), *STLayout::Name(Train), Aboard == 0 ? TEXT("no player driving it now")
				: (Aboard == 1 ? TEXT("a player is in the driver's seat with autopilot on; it keeps its loop")
				: TEXT("a player drives it by hand (autopilot off); its loop waits until autopilot is on")));
			State.PlayerAboard = Aboard;
			bCheckSoon = true;
		}
		// A managed train with a job drives itself: autopilot off while it isn't held, parked, derailed or driven by a
		// player is a fault, put right here.
		if (State.bHasJob && !State.bHeld && State.Phase != ESTTrainPhase::Parked && !Train->IsSelfDrivingEnabled() && !Train->IsPlayerDriven() && !Train->IsDerailed())
		{
			UE_LOG(LogSmartTrains, Warning, TEXT("AUTOPILOT %s: off on a train with a job (job %d, stop %d); switched back on"), *STLayout::Name(Train), State.Job.Id,
				State.Job.Leg + 1);
			Train->SetSelfDrivingEnabled(true);
		}
		// A held train whose autopilot someone switched on again is no longer held.
		if (State.bHeld && Train->IsSelfDrivingEnabled())
		{
			UE_LOG(LogSmartTrains, Log, TEXT("HOLD %s: autopilot switched on by someone; no longer held"), *STLayout::Name(Train));
			State.bHeld = false;
			bCheckSoon = true;
		}
		if (State.Phase == ESTTrainPhase::Parked && Train->IsSelfDrivingEnabled() && !bDocked && !State.bHasJob)
		{
			UE_LOG(LogSmartTrains, Log, TEXT("%s: autopilot switched on while it stood without a loop; it is checked again"), *STLayout::Name(Train));
			State.Phase = ESTTrainPhase::Idle;
			bCheckSoon = true;
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
	bCheckSoon = true;
	return static_cast<int32>(FMath::Min<int64>(Removed, MAX_int32));
}

void ASTSubsystem::EndJob(FSTTrainState& State, const TCHAR* Why)
{
	if (!State.bHasJob)
	{
		return;
	}
	UE_LOG(LogSmartTrains, Log, TEXT("JOB %d %s ended: %s (%s)"), State.Job.Id, *STLayout::Name(State.Train), Why, *State.Job.Summary);
	State.bHasJob = false;
	State.Phase = ESTTrainPhase::Idle;
	State.WrittenStopKeys.Reset();
	bCheckSoon = true;
}

bool ASTSubsystem::SetTrainHeld(AFGTrain* Train, bool bHold, const FString& Cause)
{
	FSTTrainState* State = FindTrainState(Train);
	if (!State || !State->bManaged || !IsValid(Train) || State->bHeld == bHold)
	{
		return false;
	}
	State->bHeld = bHold;
	UE_LOG(LogSmartTrains, Log, TEXT("HOLD %s %s by %s%s"), *STLayout::Name(Train), bHold ? TEXT("held") : TEXT("resumed"), *Cause,
		State->bHasJob ? *FString::Printf(TEXT(" (job %d: %s)"), State->Job.Id, *State->Job.Summary) : TEXT(""));
	if (bHold)
	{
		if (Train->IsSelfDrivingEnabled())
		{
			Train->SetSelfDrivingEnabled(false);
		}
		bCheckSoon = true;
		return true;
	}
	if (State->bHasJob)
	{
		State->WrittenStopKeys.Reset();
		WriteJob(*State, TEXT("resumed"));
	}
	else
	{
		State->Phase = ESTTrainPhase::Idle;
	}
	bCheckSoon = true;
	return true;
}

void ASTSubsystem::ManageNewTrains()
{
	if (!mSettings.bManageNewTrains || mSettings.bObserveOnly || !HasAuthority())
	{
		mNewTrains.Reset();
		return;
	}
	AFGRailroadSubsystem* Railroad = AFGRailroadSubsystem::Get(this);
	if (!Railroad)
	{
		return;
	}
	TArray<AFGTrain*> Trains;
	Railroad->GetAllTrains(Trains);
	const double Time = Now();
	for (AFGTrain* Train : Trains)
	{
		// Only trains SmartTrains has never had a setting for (one switched off or kept vanilla has a state).
		if (!IsValid(Train) || FindTrainState(Train))
		{
			mNewTrains.Remove(Train);
			continue;
		}
		const AFGRailroadTimeTable* TimeTable = Train->GetTimeTable();
		const TArray<AFGRailroadVehicle*> Vehicles = STLayout::GetVehicles(Train);
		const bool bFreight = Vehicles.ContainsByPredicate([](const AFGRailroadVehicle* Each) { return Cast<AFGFreightWagon>(Each) != nullptr; });
		if ((TimeTable && TimeTable->GetNumStops() > 0) || !bFreight || STLayout::GetPossibleDockingLocomotives(Train).Num() == 0)
		{
			mNewTrains.Remove(Train);
			continue;
		}
		TPair<int32, double>& Seen = mNewTrains.FindOrAdd(Train, TPair<int32, double>(Vehicles.Num(), Time));
		if (Seen.Key != Vehicles.Num())
		{
			Seen = TPair<int32, double>(Vehicles.Num(), Time);
			continue;
		}
		if (Time - Seen.Value < NewTrainSettleSeconds)
		{
			continue;
		}
		mNewTrains.Remove(Train);
		FString Why;
		if (!SetTrainManaged(Train, true, TEXT("Manage new trains"), Why))
		{
			UE_LOG(LogSmartTrains, Log, TEXT("MANAGE %s: new train not managed: %s"), *STLayout::Name(Train), *Why);
		}
	}
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
	State.bWasDocked = true;
	State.DockedAt = Now();
	State.DockedStation = Station;
	OnDocked(State, Station);
}

void ASTSubsystem::OnDocked(FSTTrainState& State, AFGTrainStationIdentifier* Station)
{
	AFGTrain* Train = State.Train;
	mPendingDockMaps.Add(Train);
	LearnDockMap(Train, Station);
	if (State.LastStation && State.LastDepartureAt > 0.0)
	{
		LearnTrip(State.LastStation, Station, Now() - State.LastDepartureAt);
	}
	if (!State.bHasJob || State.Job.Legs.Num() == 0)
	{
		UE_LOG(LogSmartTrains, Log, TEXT("%s docked at %s (%s, no job)"), *STLayout::Name(Train), *STLayout::Name(Station), STNames::Phase(State.Phase));
		return;
	}
	// The stop it docked at: its current leg, else the next one of the job with that station (a loop counts round).
	const int32 Count = State.Job.Legs.Num();
	int32 Found = INDEX_NONE;
	for (int32 Offset = 0; Offset < Count && Found == INDEX_NONE; ++Offset)
	{
		const int32 Index = State.Job.Kind == ESTJobKind::Loop ? (State.Job.Leg + Offset) % Count : State.Job.Leg + Offset;
		Found = State.Job.Legs.IsValidIndex(Index) && State.Job.Legs[Index].Station == Station ? Index : INDEX_NONE;
	}
	if (Found == INDEX_NONE)
	{
		UE_LOG(LogSmartTrains, Warning, TEXT("%s docked at %s, which job %d doesn't stop at"), *STLayout::Name(Train), *STLayout::Name(Station), State.Job.Id);
		return;
	}
	if (Found != State.Job.Leg)
	{
		UE_LOG(LogSmartTrains, Warning, TEXT("%s docked at %s, stop %d of job %d; it was heading for stop %d"), *STLayout::Name(Train), *STLayout::Name(Station), Found + 1,
			State.Job.Id, State.Job.Leg + 1);
		State.Job.Leg = Found;
	}
	const FSTJobLeg& Leg = State.Job.Legs[State.Job.Leg];
	State.DockedSkipCars = Leg.SkipCars;
	TArray<int32> Amounts;
	for (const FSTCarWork& Work : Leg.Work)
	{
		Amounts.Add(CountInCar(Work.Car, Work.Item));
	}
	mDockStartAmounts.Add(Train, Amounts);
	UE_LOG(LogSmartTrains, Log, TEXT("JOB %d %s: docked at %s, stop %d of %d (%s)"), State.Job.Id, *STLayout::Name(Train), *STLayout::Name(Station), State.Job.Leg + 1,
		State.Job.Legs.Num(), Leg.HasWork() ? *DescribeLeg(Train, Leg, false) : TEXT("no transfer"));
}

void ASTSubsystem::OnUndocked(FSTTrainState& State, AFGTrainStationIdentifier* Station, double Stay)
{
	AFGTrain* Train = State.Train;
	mPendingDockMaps.Remove(Train);
	State.LastStation = Station;
	State.LastDepartureAt = Now();
	State.DockedSkipCars.Reset();
	if (!State.bHasJob || !State.Job.Legs.IsValidIndex(State.Job.Leg) || State.Job.Legs[State.Job.Leg].Station != Station)
	{
		UE_LOG(LogSmartTrains, Log, TEXT("%s left %s after %.0f s%s"), *STLayout::Name(Train), *STLayout::Name(Station), Stay, State.bHasJob ? TEXT("") : TEXT(" (no job)"));
		return;
	}
	FinishLeg(State, Station, Stay);
}

void ASTSubsystem::FinishLeg(FSTTrainState& State, AFGTrainStationIdentifier* Station, double Stay)
{
	AFGTrain* Train = State.Train;
	FSTJobLeg& Leg = State.Job.Legs[State.Job.Leg];
	const TArray<int32> Before = mDockStartAmounts.FindRef(Train);
	for (int32 Index = 0; Index < Leg.Work.Num(); ++Index)
	{
		FSTCarWork& Work = Leg.Work[Index];
		const int32 Start = Before.IsValidIndex(Index) ? Before[Index] : 0;
		Work.MovedAmount = FMath::Abs(CountInCar(Work.Car, Work.Item) - Start);
	}
	if (Leg.HasWork())
	{
		LearnStay(Station, Stay);
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
	if (State.Job.Leg < State.Job.Legs.Num())
	{
		return;
	}
	// A loop starts its next round (its timetable repeats by itself); other jobs end here.
	if (State.Job.Kind == ESTJobKind::Loop)
	{
		State.Job.Leg = 0;
		++State.Job.Rounds;
		UE_LOG(LogSmartTrains, Log, TEXT("LOOP %s: round %d done (%s)"), *STLayout::Name(Train), State.Job.Rounds, *State.Job.Summary);
		return;
	}
	FinishJob(State);
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
	const float Estimate = Trip && Trip->Samples > 0 ? Trip->Seconds : EstimateTravelSeconds(From, To);
	if (!Trip)
	{
		Trip = &mTrips.AddDefaulted_GetRef();
		Trip->From = From;
		Trip->To = To;
	}
	// Queues, deadlocks and standing parked only make a trip slower: a sample counts at most TripOutlierFactor times the
	// estimate, and a learned time that many times slower than a sample held such a wait, so the sample replaces it.
	const float Counted = Estimate > 0.f ? FMath::Min(Seconds, Estimate * TripOutlierFactor) : Seconds;
	Trip->Seconds = Trip->Samples == 0 || Counted * TripOutlierFactor < Trip->Seconds ? Counted : FMath::Lerp(Trip->Seconds, Counted, 0.3f);
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

void ASTSubsystem::CheckTravelAgainstGame() const
{
	AFGRailroadSubsystem* Railroad = AFGRailroadSubsystem::Get(GetWorld());
	if (!Railroad || !mTravel)
	{
		UE_LOG(LogSmartTrains, Warning, TEXT("TRAVELCHECK: no railroad subsystem or route search yet"));
		return;
	}
	TArray<AFGTrainStationIdentifier*> Stations;
	Railroad->GetAllTrainStations(Stations);
	const double Started = FPlatformTime::Seconds();
	int32 Trains = 0;
	int32 Pairs = 0;
	int32 OursOnly = 0;
	int32 GameOnly = 0;
	int32 SignalsOnly = 0;
	for (const FSTTrainState& State : mTrains)
	{
		AFGTrain* Train = State.Train.Get();
		// The station it is docked at: where the game's search starts from.
		const AFGTrainStationIdentifier* From = State.DockedStation.Get();
		if (!State.bManaged || !IsValid(Train) || !From || !Train->IsDocked())
		{
			continue;
		}
		++Trains;
		const bool bPushPull = IsPushPull(Train);
		const TArray<AFGLocomotive*> Locomotives = STLayout::GetPossibleDockingLocomotives(Train);
		for (AFGTrainStationIdentifier* To : Stations)
		{
			AFGBuildableRailroadStation* Building = To ? To->GetStation() : nullptr;
			if (!Building || To == From || Building->GetTrackGraphID() != Train->GetTrackGraphID())
			{
				continue;
			}
			++Pairs;
			bool bReversing = false;
			const float Ours = mTravel->GetDistance(From, To, &bReversing, bPushPull);
			bool bGame = false;
			bool bGameNoSignals = false;
			// The game's way, for a disagreement: which locomotive found it (and whether it leads now), and the tracks.
			FString GamePath;
			for (AFGLocomotive* Locomotive : Locomotives)
			{
				FRailroadGraphAStarFilter Filter;
				Filter.AcceptsPartialSolution = false;
				Filter.IgnoreSignals = false;
				const FRailroadPathFindingResult Found = FRailroadNavigation::FindPathSync(Locomotive, Building, Filter);
				if (!bGame && Found.Result == ERailroadPathFindingResult::RPFR_Success && Found.Path.IsValid())
				{
					TArray<FString> Points;
					for (const FRailroadPathPoint& Point : Found.Path->PathPoints)
					{
						const UFGRailroadTrackConnectionComponent* Connection = Point.TrackConnection.Get();
						const AFGBuildableRailroadTrack* Track = Connection ? Connection->GetTrack() : nullptr;
						if (Points.Num() < 24 && Track)
						{
							const FVector Where = Connection->GetConnectorLocation() / 100.0;
							Points.Add(FString::Printf(TEXT("%s(%.0f,%.0f)"), *Track->GetName().Right(10), Where.X, Where.Y));
						}
					}
					GamePath = FString::Printf(TEXT(" [locomotive %d of %d%s: %s]"), Locomotives.IndexOfByKey(Locomotive) + 1, Locomotives.Num(),
						Locomotive == Train->GetMultipleUnitMaster() ? TEXT(", leading") : TEXT(""), *FString::Join(Points, TEXT(" > ")));
				}
				bGame |= Found.Result == ERailroadPathFindingResult::RPFR_Success;
				if (!bGame)
				{
					Filter.IgnoreSignals = true;
					bGameNoSignals |= FRailroadNavigation::FindPathSync(Locomotive, Building, Filter).Result == ERailroadPathFindingResult::RPFR_Success;
				}
			}
			const bool bOurs = Ours >= 0.f;
			if (bOurs == bGame)
			{
				continue;
			}
			(bOurs ? OursOnly : GameOnly) += 1;
			SignalsOnly += bOurs && bGameNoSignals ? 1 : 0;
			UE_LOG(LogSmartTrains, Log, TEXT("TRAVELCHECK %s (%s, %d docking locomotives) from %s to %s: SmartTrains %s, the game %s%s"), *STLayout::Name(Train),
				bPushPull ? TEXT("push-pull") : TEXT("one way"), Locomotives.Num(), *STLayout::Name(From), *STLayout::Name(To),
				bOurs ? *FString::Printf(TEXT("finds %.0f m%s"), Ours / 100.f, bReversing ? TEXT(" backing out") : TEXT("")) : TEXT("finds no way"),
				bGame ? TEXT("finds a way") : (bGameNoSignals ? TEXT("finds a way only ignoring signals") : TEXT("finds no way even ignoring signals")), *GamePath);
			UE_LOG(LogSmartTrains, Log, TEXT("TRAVELCHECK   SmartTrains' view: %s"), *FSTTravel::Explain(From, To));
		}
	}
	UE_LOG(LogSmartTrains, Log, TEXT("TRAVELCHECK: %d docked trains, %d station pairs; SmartTrains alone finds %d (%d of them the game finds only ignoring signals), the game alone %d (%.0f ms)"),
		Trains, Pairs, OursOnly, SignalsOnly, GameOnly, 1000.0 * (FPlatformTime::Seconds() - Started));
}

namespace
{
	FAutoConsoleCommandWithWorldAndArgs GCheckTravelCommand(TEXT("SmartTrains.CheckTravel"),
		TEXT("Compares SmartTrains' route search with the game's pathfinder for every docked managed train (TRAVELCHECK lines)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (const ASTSubsystem* Subsystem = ASTSubsystem::Get(World))
			{
				Subsystem->CheckTravelAgainstGame();
			}
		}));
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
	// No way for this kind of train in SmartTrains' search is final: a learned time (from any train) or Auto Trains'
	// time never makes the pair reachable.
	bool bReversing = false;
	float Distance = mTravel ? mTravel->GetDistance(From, To, &bReversing, bPushPull) : -1.f;
	if (Distance == -2.f)
	{
		return -1.f;
	}
	if (const FSTTripTime* Trip = mTrips.FindByPredicate([&](const FSTTripTime& Each) { return Each.From == From && Each.To == To; }))
	{
		return Trip->Seconds;
	}
	const float Speed = FMath::Max(1.f, mSettings.AverageSpeedKmh / 3.6f) * 100.f;
	// Auto Trains' time for a pair not driven yet: the way the signals allow, at real speeds and acceleration. Asked a
	// few pairs per dispatch round and kept 10 minutes; SmartTrains' own estimate stands in meanwhile.
	const TPair<TWeakObjectPtr<AFGTrainStationIdentifier>, TWeakObjectPtr<AFGTrainStationIdentifier>> Key(const_cast<AFGTrainStationIdentifier*>(From),
		const_cast<AFGTrainStationIdentifier*>(To));
	TPair<float, double>* Cached = mAtTravel.Find(Key);
	if ((!Cached || Now() - Cached->Value > 600.0) && mAtTravelCallsLeft > 0 && STAt::IsAvailable(GetWorld()))
	{
		--mAtTravelCallsLeft;
		// Auto Trains times the drive at a train's own speeds (with none it assumes 18 km/h): a managed train's stand in.
		AFGTrain* Sample = nullptr;
		for (const FSTTrainState& State : mTrains)
		{
			if (State.bManaged && State.Train)
			{
				Sample = State.Train;
				break;
			}
		}
		const float AtSeconds = STAt::EstimateTravelSeconds(GetWorld(), Sample, const_cast<AFGBuildableRailroadStation*>(From->GetStation()),
			const_cast<AFGBuildableRailroadStation*>(To->GetStation()));
		Cached = &mAtTravel.Add(Key, TPair<float, double>(AtSeconds, Now()));
	}
	// Auto Trains' time replaces SmartTrains' estimate.
	if (Cached && Cached->Key >= 0.f)
	{
		// Plus time to start and stop at the stations.
		return Cached->Key + 20.f;
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

void ASTSubsystem::SetPlatformMachines(AFGBuildableTrainPlatformCargo* Platform, const TArray<TWeakObjectPtr<const AActor>>& Machines)
{
	FSTPlatformConfig* Config = FindPlatformConfig(Platform);
	if (!Config)
	{
		return;
	}
	Config->Machines.Reset();
	for (const TWeakObjectPtr<const AActor>& Machine : Machines)
	{
		if (Machine.IsValid())
		{
			Config->Machines.AddUnique(const_cast<AActor*>(Machine.Get()));
		}
	}
	UE_LOG(LogSmartTrains, Verbose, TEXT("PLATFORM %s: %d machines kept for its recipe rate"), *GetNameSafe(Platform), Config->Machines.Num());
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
	// Trains without a loop carry where they stand, so they can be told apart.
	FString Name = State.bHasJob ? FString::Printf(TEXT("ST: %s"), *State.Job.Summary) : FString(TEXT("ST: no loop"));
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
	Lines.Add(FString::Printf(TEXT("Station %s"), *STLayout::Name(Station)));
	if (const FSTConsumer* Consumer = FindConsumer(Station))
	{
		Lines.Add(FString::Printf(TEXT("  Consumer: %.2f car loads/min, round trip %.1f min, %d trains on its loop (target %d)%s"), Consumer->Rate, Consumer->RoundTrip / 60.0,
			Consumer->Trains.Num(), Consumer->Target, Consumer->Problem.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("; %s"), *Consumer->Problem)));
	}
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
	if (State && State->bManaged)
	{
		Lines.Add(DescribeWork(*State));
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

TArray<FString> ASTSubsystem::DescribeStatus() const
{
	TArray<FString> Lines;
	int32 Supply = 0;
	int32 Demand = 0;
	int32 Sinks = 0;
	for (const FSTPlatformConfig& Config : mPlatforms)
	{
		Supply += Config.Role == ESTRole::Supply ? 1 : 0;
		Demand += Config.Role == ESTRole::Demand ? 1 : 0;
		Sinks += Config.Role == ESTRole::Sink ? 1 : 0;
	}
	Lines.Add(FString::Printf(TEXT("SmartTrains%s: %d supply, %d demand and %d sink platforms, %d stations searched for distances, Freight Car Control %s"),
		mSettings.bObserveOnly ? TEXT(" (observe only)") : TEXT(""), Supply, Demand, Sinks, mTravel ? mTravel->GetSearchedCount() : 0, IsFccAvailable() ? TEXT("on") : TEXT("off")));
	for (const FString& Line : MakeMonitorView().Summary)
	{
		Lines.Add(TEXT("  ") + Line);
	}
	return Lines;
}

void ASTSubsystem::LogFleet()
{
	const double Time = Now();
	if (Time - mFleetLoggedAt < 60.0)
	{
		return;
	}
	mFleetLoggedAt = Time;
	enum EKind { MovingLoaded, MovingEmpty, Docked, Standing, Switching, NoLoop, Kinds };
	int32 Count[Kinds] = {};
	double Fill[Kinds] = {};
	for (const FSTTrainState& State : mTrains)
	{
		const AFGTrain* Train = State.Train.Get();
		if (!State.bManaged || !IsValid(Train))
		{
			continue;
		}
		const double Share = GetFillShare(Train);
		const bool bMoving = STLayout::GetTrainSpeed(Train) > 300.f;
		const EKind Kind = !State.bHasJob ? NoLoop : State.Job.Kind != ESTJobKind::Loop ? Switching : Train->IsDocked() ? Docked : !bMoving ? Standing
			: (Share > 0.001 ? MovingLoaded : MovingEmpty);
		++Count[Kind];
		Fill[Kind] += Share;
	}
	auto Avg = [&](EKind Kind) { return Count[Kind] > 0 ? 100.0 * Fill[Kind] / Count[Kind] : 0.0; };
	UE_LOG(LogSmartTrains, Log, TEXT("FLEET moving loaded %d (fill %.0f%%), moving empty %d, docked %d (fill %.0f%%), standing on the way %d, switching patterns %d, without a loop %d"),
		Count[MovingLoaded], Avg(MovingLoaded), Count[MovingEmpty], Count[Docked], Avg(Docked), Count[Standing], Count[Switching], Count[NoLoop]);
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
