// Depot signals for a consumer's spare trains (Docs/Architecture.md, "Depot signals").
#include "STSubsystem.h"
#include "Buildables/FGBuildableFactory.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildableRailroadSignal.h"
#include "Buildables/FGBuildableRailroadTrack.h"
#include "EngineUtils.h"
#include "FGInventoryComponent.h"
#include "FGLocomotive.h"
#include "FGFreightWagon.h"
#include "FGRailroadTimeTable.h"
#include "FGRailroadTrackConnectionComponent.h"
#include "FGTrain.h"
#include "Resources/FGItemDescriptor.h"
#include "SmartTrains.h"
#include "STBridges.h"
#include "STDepotSignal.h"
#include "STLayout.h"

namespace
{
	// A train sent to a depot signal counts as parked once it stands still this long after being sent. [s]
	constexpr double DepotSettleSeconds = 5.0;
	// Own drive (no Auto Trains): standing slower than this near its goal is arrived, and a dropped path is given again
	// at most this often. [cm/s] [cm] [s]
	constexpr float DepotStillSpeed = 50.f;
	constexpr float DepotArriveDistance = 3000.f;
	constexpr double DepotPathRetrySeconds = 10.0;
	// A car counts as full from this share of what it holds at most (fluids and stacks rarely end exactly at the top).
	constexpr double FullShare = 0.98;

	// Whether Factory wants input now: running (not on standby), powered if it needs power, and its output not backed up.
	bool WantsInput(const AFGBuildableFactory* Factory)
	{
		if (!IsValid(Factory) || Factory->IsProductionPaused())
		{
			return false;
		}
		const AFGBuildableManufacturer* Maker = Cast<AFGBuildableManufacturer>(Factory);
		if (!Maker)
		{
			return true;
		}
		if (!Maker->HasPower())
		{
			return false;
		}
		const UFGInventoryComponent* Output = Maker->GetOutputInventory();
		for (int32 Index = 0; Output && Index < Output->GetSizeLinear(); ++Index)
		{
			FInventoryStack Stack;
			if (Output->GetStackFromIndex(Index, Stack) && Stack.HasItems())
			{
				const int32 Most = UFGItemDescriptor::GetStackSize(Stack.Item.GetItemClass());
				if (Most > 0 && Stack.NumItems >= Most)
				{
					return false;
				}
			}
		}
		return true;
	}
}

void ASTSubsystem::UpdateUse()
{
	const double Time = Now();
	const double Since = mUseSampledAt < 0.0 ? 0.0 : Time - mUseSampledAt;
	mUseSampledAt = Time;
	const double Blend = Since <= 0.0 ? 1.0 : 1.0 - FMath::Exp(-Since / (60.0 * FMath::Max(1.f, mSettings.UseAverageMinutes)));
	for (FSTConsumer& Consumer : mConsumers)
	{
		int32 Machines = 0;
		int32 Wanting = 0;
		TSet<const AActor*> Seen;
		for (const FSTLoopSlot& Slot : Consumer.Slots)
		{
			const FSTPlatformConfig* Config = FindPlatformConfig(Slot.Platform.Get());
			for (int32 Index = 0; Config && Index < Config->Machines.Num(); ++Index)
			{
				const AFGBuildableFactory* Factory = Cast<AFGBuildableFactory>(Config->Machines[Index].Get());
				if (!Factory || Seen.Contains(Factory))
				{
					continue;
				}
				Seen.Add(Factory);
				++Machines;
				Wanting += WantsInput(Factory) ? 1 : 0;
			}
		}
		// Without traced machines its use isn't known: it counts as using everything.
		const double Raw = Machines > 0 ? static_cast<double>(Wanting) / Machines : 1.0;
		double& Average = mUse.FindOrAdd(Consumer.Station, Raw);
		Average += (Raw - Average) * Blend;
		Consumer.Use = FMath::Clamp(Average, 0.0, 1.0);
		const int32 Count = Consumer.Trains.Num();
		Consumer.Wanted = Count == 0 ? 0 : FMath::Clamp(FMath::CeilToInt(Count * Consumer.Use - 0.05), 1, Count);
		Consumer.AtDepot = 0;
		for (const TWeakObjectPtr<AFGTrain>& Train : Consumer.Trains)
		{
			const FSTTrainState* State = FindTrainState(Train.Get());
			Consumer.AtDepot += State && State->bDepot ? 1 : 0;
		}
	}
	// Consumers whose use needs more of their trains running: depot trains go back to their loop, those with a clear way
	// out first.
	TOptional<TArray<FSTDepotSpot>> Spots;
	for (FSTConsumer& Consumer : mConsumers)
	{
		int32 Short = Consumer.Wanted - (Consumer.Trains.Num() - Consumer.AtDepot);
		for (const TWeakObjectPtr<AFGTrain>& Train : Consumer.Trains)
		{
			FSTTrainState* State = FindTrainState(Train.Get());
			if (Short <= 0 || !State || !State->bDepot)
			{
				continue;
			}
			if (!Spots.IsSet())
			{
				Spots = STDepots::Survey(GetWorld());
			}
			if (IsBoxedInAtDepot(*State, Spots.GetValue()))
			{
				continue;
			}
			WakeFromDepot(*State, *FString::Printf(TEXT("%s uses %.0f %% of its machines now and needs %d of its %d trains"), *STLayout::Name(Consumer.Station.Get()),
				100.0 * Consumer.Use, Consumer.Wanted, Consumer.Trains.Num()));
			--Consumer.AtDepot;
			--Short;
		}
	}
}

bool ASTSubsystem::IsLoopFull(const FSTTrainState& State) const
{
	bool bAny = false;
	for (int32 Leg = 0; Leg < State.Job.Legs.Num(); ++Leg)
	{
		for (const FSTCarWork& Work : State.Job.Legs[Leg].Work)
		{
			if (!Work.bLoad)
			{
				continue;
			}
			bAny = true;
			const double Capacity = STLayout::GetCarCapacity(Cast<AFGFreightWagon>(Work.Car.Get()), Work.Item);
			if (Capacity <= 0.0 || CountInCar(Work.Car, Work.Item) < Capacity * FullShare)
			{
				return false;
			}
		}
	}
	return bAny;
}

bool ASTSubsystem::ParkIfSpare(FSTTrainState& State)
{
	FSTConsumer* Consumer = mConsumers.FindByPredicate([&State](const FSTConsumer& Each) { return Each.Station.Get() == State.LoopConsumer.Get(); });
	if (!Consumer || mSettings.bObserveOnly)
	{
		return false;
	}
	// A spare train counts at the depot from the moment it is picked, so its consumer picks no more than it can spare.
	const bool bPicked = State.bDepot;
	if (!bPicked && Consumer->Trains.Num() - Consumer->AtDepot <= Consumer->Wanted)
	{
		return false;
	}
	auto Pick = [&](bool bOn)
	{
		if (State.bDepot != bOn)
		{
			State.bDepot = bOn;
			Consumer->AtDepot += bOn ? 1 : -1;
		}
	};
	const FString Why = FString::Printf(TEXT("%s uses %.0f %% of its machines now: %d of its %d trains are enough"), *STLayout::Name(Consumer->Station.Get()),
		100.0 * Consumer->Use, Consumer->Wanted, Consumer->Trains.Num());
	if (IsLoopFull(State))
	{
		Pick(true);
		if (SendToDepotSignal(State, *Why))
		{
			return true;
		}
		Pick(false);
		UE_LOG(LogSmartTrains, Log, TEXT("DEPOT %s: full but no free depot signal; it delivers as usual"), *STLayout::Name(State.Train));
		return false;
	}
	if (!SendToDepotSignal(State, *Why, true))
	{
		Pick(false);
		return false;
	}
	if (!bPicked)
	{
		UE_LOG(LogSmartTrains, Log, TEXT("DEPOT %s: spare (%s); fills up at its suppliers, then parks"), *STLayout::Name(State.Train), *Why);
	}
	Pick(true);
	State.Job.Leg = 0;
	WriteJob(State, TEXT("fills up before it parks"));
	return true;
}

bool ASTSubsystem::SendToDepotSignal(FSTTrainState& State, const TCHAR* Why, bool bCheckOnly)
{
	AFGTrain* Train = State.Train;
	AFGLocomotive* Locomotive = IsValid(Train) ? Cast<AFGLocomotive>(Train->GetMultipleUnitMaster()) : nullptr;
	const AFGBuildableRailroadTrack* Here = Locomotive ? Locomotive->GetTrackPosition().Track.Get() : nullptr;
	if (!Here)
	{
		return false;
	}
	const double Clock = Now();
	const float Needed = STLayout::GetTrainLength(Train) + STDepots::Margin;
	TSet<const UFGRailroadTrackConnectionComponent*> Goals;
	TMap<const UFGRailroadTrackConnectionComponent*, ASTBuildableDepotSignal*> ByGoal;
	const TArray<FSTDepotSpot> Spots = STDepots::Survey(GetWorld());
	// Taken: another train stands on the spot or is heading there.
	auto Taken = [&](int32 Index)
	{
		if (Spots[Index].Trains.ContainsByPredicate([Train](const AFGTrain* Each) { return Each != Train; }))
		{
			return true;
		}
		for (const TPair<TWeakObjectPtr<AFGTrain>, FDepotTrip>& Pair : mDepotTrips)
		{
			if (Pair.Key.IsValid() && Pair.Key.Get() != Train && Pair.Value.Signal.Get() == Spots[Index].Signal)
			{
				return true;
			}
		}
		return false;
	};
	// Passing sidings only: a train drives in and out the same way.
	auto Fits = [&](int32 Index)
	{
		const FSTDepotSiding& Siding = Spots[Index].Siding;
		return Siding.Problem.IsEmpty() && Siding.Goal && !Siding.bDeadEnd && Siding.Length >= Needed && Siding.TrackGraph == Here->GetTrackGraphID();
	};
	for (int32 Index = 0; Index < Spots.Num(); ++Index)
	{
		const double* NoWayUntil = mDepotNoWay.Find(TPair<const AFGTrain*, const ASTBuildableDepotSignal*>(Train, Spots[Index].Signal));
		if (!Fits(Index) || Taken(Index) || (NoWayUntil && *NoWayUntil > Clock))
		{
			continue;
		}
		// The way in is clear: no train on the spots between the siding's entrance and this one.
		bool bClear = true;
		for (int32 Behind = Spots[Index].Behind, Steps = 0; Behind != INDEX_NONE && bClear && Steps < Spots.Num(); Behind = Spots[Behind].Behind, ++Steps)
		{
			bClear = !Taken(Behind);
		}
		// Far end first: a deeper spot this train fits must be taken already.
		const int32 Ahead = Spots[Index].Ahead;
		if (!bClear || (Ahead != INDEX_NONE && Fits(Ahead) && !Taken(Ahead)))
		{
			continue;
		}
		Goals.Add(Spots[Index].Siding.Goal);
		ByGoal.Add(Spots[Index].Siding.Goal, Spots[Index].Signal);
	}
	if (bCheckOnly || Goals.Num() == 0)
	{
		return Goals.Num() > 0;
	}
	const UFGRailroadTrackConnectionComponent* Reached = nullptr;
	float Length = 0.f;
	const TArray<UFGRailroadTrackConnectionComponent*> Route = STDepots::FindRoute(Locomotive, Goals, Reached, Length);
	ASTBuildableDepotSignal* Signal = Reached ? ByGoal.FindRef(Reached) : nullptr;
	if (!Signal)
	{
		UE_LOG(LogSmartTrains, Log, TEXT("DEPOT %s: no way ahead to any of %d free depot signals that fit it; it stays on its loop"), *STLayout::Name(Train), Goals.Num());
		return false;
	}
	FDepotTrip Trip;
	Trip.Signal = Signal;
	Trip.SentAt = Clock;
	Trip.Goal = const_cast<UFGRailroadTrackConnectionComponent*>(Reached);
	for (UFGRailroadTrackConnectionComponent* Each : Route)
	{
		Trip.Route.Add(Each);
	}
	if (const FSTDepotSpot* Spot = Spots.FindByPredicate([Signal](const FSTDepotSpot& Each) { return Each.Signal == Signal; }))
	{
		Trip.StopSignal = const_cast<AFGBuildableRailroadSignal*>(Spot->Siding.StopSignal);
	}
	if (!StartDepotDrive(Train, Trip))
	{
		UE_LOG(LogSmartTrains, Warning, TEXT("DEPOT %s: can't drive it to the depot signal at %s (%s); it stays on its loop"), *STLayout::Name(Train),
			*Signal->GetActorLocation().ToCompactString(), Trip.bOwnDrive ? TEXT("the game refused the path") : TEXT("Auto Trains found no way"));
		mDepotNoWay.Add(TPair<const AFGTrain*, const ASTBuildableDepotSignal*>(Train, Signal), Clock + STDepots::NoWayRetrySeconds);
		return false;
	}
	State.bHasJob = false;
	State.Phase = ESTTrainPhase::Idle;
	State.WrittenStopKeys.Reset();
	mDepotTrips.Add(Train, MoveTemp(Trip));
	UE_LOG(LogSmartTrains, Log, TEXT("DEPOT %s: to the depot signal at %s, %.0f m along %d tracks, %s (%s)"), *STLayout::Name(Train),
		*Signal->GetActorLocation().ToCompactString(), Length / 100.f, Route.Num(), mDepotTrips[Train].bOwnDrive ? TEXT("along the signals") : TEXT("Auto Trains drives"), Why);
	UpdateTrainName(State);
	return true;
}

void ASTSubsystem::WakeFromDepot(FSTTrainState& State, const TCHAR* Why)
{
	AFGTrain* Train = State.Train;
	if (const FDepotTrip* Trip = mDepotTrips.Find(Train))
	{
		EndDepotDrive(Train, *Trip, true);
		mDepotTrips.Remove(Train);
	}
	State.bDepot = false;
	State.bHasJob = false;
	State.Phase = ESTTrainPhase::Idle;
	State.WrittenStopKeys.Reset();
	UE_LOG(LogSmartTrains, Log, TEXT("DEPOT %s: back to its loop (%s)"), *STLayout::Name(Train), Why);
	UpdateTrainName(State);
}

void ASTSubsystem::CheckDepotDrive(FSTTrainState& State)
{
	AFGTrain* Train = State.Train;
	FDepotTrip* Trip = mDepotTrips.Find(Train);
	// After a load the drive isn't known any more: it stands where it parked.
	if (!Trip || State.Phase == ESTTrainPhase::Parked || Now() - Trip->SentAt <= DepotSettleSeconds)
	{
		return;
	}
	const STDepots::EDriveState Drive = GetDepotDriveState(Train, *Trip);
	if (Drive == STDepots::EDriveState::Arrived)
	{
		State.Phase = ESTTrainPhase::Parked;
		if (Train->IsSelfDrivingEnabled())
		{
			Train->SetSelfDrivingEnabled(false);
		}
		UE_LOG(LogSmartTrains, Log, TEXT("DEPOT %s: parked at the depot signal at %s"), *STLayout::Name(Train),
			Trip->Signal.IsValid() ? *Trip->Signal->GetActorLocation().ToCompactString() : TEXT("?"));
		UpdateTrainName(State);
	}
	else if (Drive != STDepots::EDriveState::UnderWay)
	{
		mDepotNoWay.Add(TPair<const AFGTrain*, const ASTBuildableDepotSignal*>(Train, Trip->Signal.Get()), Now() + STDepots::NoWayRetrySeconds);
		WakeFromDepot(State, Drive == STDepots::EDriveState::NoWay ? TEXT("no way to the depot signal")
			: Drive == STDepots::EDriveState::None ? TEXT("the drive to the depot signal was dropped") : TEXT("Auto Trains isn't available for the drive"));
	}
}

bool ASTSubsystem::StartDepotDrive(AFGTrain* Train, FDepotTrip& Trip)
{
	if (!Train->IsSelfDrivingEnabled())
	{
		Train->SetSelfDrivingEnabled(true);
	}
	Trip.bOwnDrive = !STAt::IsAvailable(GetWorld());
	if (!Trip.bOwnDrive)
	{
		// Auto Trains plans and drives it, empties its timetable and stops it at the goal.
		return STAt::DriveTo(GetWorld(), Train, Trip.Goal.Get());
	}
	// The game follows a path with no station only with an empty timetable; the held stop signal makes it brake there.
	TArray<UFGRailroadTrackConnectionComponent*> Route;
	for (const TWeakObjectPtr<UFGRailroadTrackConnectionComponent>& Each : Trip.Route)
	{
		if (!Each.IsValid())
		{
			return false;
		}
		Route.Add(Each.Get());
	}
	if (AFGRailroadTimeTable* TimeTable = Train->GetTimeTable())
	{
		TimeTable->SetStops(TArray<FTimeTableStop>());
	}
	if (!FSTPlatformAccess::FollowPath(Train, Route))
	{
		return false;
	}
	Trip.PathSetAt = Now();
	STDepots::Hold(Trip.StopSignal.Get(), true);
	return true;
}

STDepots::EDriveState ASTSubsystem::GetDepotDriveState(AFGTrain* Train, FDepotTrip& Trip)
{
	if (!Trip.bOwnDrive)
	{
		return STAt::GetDriveState(GetWorld(), Train);
	}
	const UFGRailroadTrackConnectionComponent* Goal = Trip.Goal.Get();
	if (!Goal || !IsValid(Train))
	{
		return STDepots::EDriveState::NoWay;
	}
	// Arrived: standing with an end of the train near the goal.
	const AFGRailroadVehicle* First = Train->GetFirstVehicle();
	const AFGRailroadVehicle* Last = Train->GetLastVehicle();
	const FVector At = Goal->GetComponentLocation();
	const float Near = First && Last ? FMath::Min(FVector::Dist(First->GetActorLocation(), At), FVector::Dist(Last->GetActorLocation(), At)) : TNumericLimits<float>::Max();
	if (First && STLayout::GetTrainSpeed(Train) < DepotStillSpeed && Near < DepotArriveDistance)
	{
		return STDepots::EDriveState::Arrived;
	}
	// Not following its path to the goal (the game dropped it): given again, at most this often.
	if (!FSTPlatformAccess::IsFollowing(Train, Goal) && Now() - Trip.PathSetAt >= DepotPathRetrySeconds)
	{
		return StartDepotDrive(Train, Trip) ? STDepots::EDriveState::UnderWay : STDepots::EDriveState::NoWay;
	}
	return STDepots::EDriveState::UnderWay;
}

void ASTSubsystem::EndDepotDrive(AFGTrain* Train, const FDepotTrip& Trip, bool bCancel)
{
	STDepots::Hold(Trip.StopSignal.Get(), false);
	if (bCancel && !Trip.bOwnDrive)
	{
		STAt::CancelDrive(GetWorld(), Train);
	}
}

bool ASTSubsystem::IsBoxedInAtDepot(const FSTTrainState& State, const TArray<FSTDepotSpot>& Spots) const
{
	const AFGTrain* Train = State.Train;
	const int32 Here = Spots.IndexOfByPredicate([Train](const FSTDepotSpot& Spot) { return Spot.Trains.Contains(Train); });
	if (Here == INDEX_NONE)
	{
		return false;
	}
	// The way out of a passing siding is ahead, toward its exit.
	for (int32 Next = Spots[Here].Ahead, Steps = 0; Next != INDEX_NONE && Steps < Spots.Num(); Next = Spots[Next].Ahead, ++Steps)
	{
		if (Spots[Next].Trains.ContainsByPredicate([Train](const AFGTrain* Other) { return Other != Train; }))
		{
			return true;
		}
	}
	return false;
}

TArray<FString> ASTSubsystem::DescribeDepots() const
{
	TArray<FString> Lines;
	for (TActorIterator<ASTBuildableDepotSignal> It(GetWorld()); It; ++It)
	{
		FString User;
		for (const TPair<TWeakObjectPtr<AFGTrain>, FDepotTrip>& Pair : mDepotTrips)
		{
			if (Pair.Value.Signal.Get() == *It && Pair.Key.IsValid())
			{
				User = TEXT(" In use by ") + STLayout::Name(Pair.Key.Get()) + TEXT(".");
			}
		}
		Lines.Add(FString::Printf(TEXT("Depot signal at %s: %s%s"), *It->GetActorLocation().ToCompactString(), *STDepots::Describe(STDepots::Check(*It)), *User));
	}
	if (Lines.Num() == 0)
	{
		Lines.Add(TEXT("No depot signals built (build menu, Vehicles, next to the Block Signal; unlocked with it)."));
	}
	return Lines;
}
