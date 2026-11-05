#include "STTripPlanner.h"
#include "Buildables/FGBuildableRailroadStation.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGFreightWagon.h"
#include "FGInventoryComponent.h"
#include "FGTrain.h"
#include "FGTrainStationIdentifier.h"
#include "HAL/PlatformTime.h"
#include "Resources/FGItemDescriptor.h"
#include "SmartTrains.h"
#include "STLayout.h"
#include "STSubsystem.h"
#include "STTypes.h"

namespace
{
	// Moves kept per train, best solo score first (cargo already on board is always kept).
	constexpr int32 MaxMovesPerTrain = 24;
	// Shortest trip time the score divides by, so very short trips don't get huge scores. [s]
	constexpr double MinScoreSeconds = 30.0;
	// Value weight of cargo delivered to a consumer above its target (unwanted cargo after the time limit).
	constexpr double OverTargetWeight = 0.2;
	// Value weight of cargo delivered to an any-item sink.
	constexpr double SinkWeight = 0.1;

	template <typename KeyType>
	double& FindOrAddValue(TArray<TPair<KeyType, double>>& Pairs, const KeyType& Key, TFunctionRef<double()> Initial)
	{
		for (TPair<KeyType, double>& Each : Pairs)
		{
			if (Each.Key == Key)
			{
				return Each.Value;
			}
		}
		return Pairs.Emplace_GetRef(Key, Initial()).Value;
	}

	// Key of a platform's pool in one way the train may dock (a dock map index).
	int32 PoolKey(int32 MapIndex, int32 PlatformIndex)
	{
		return (MapIndex << 20) | PlatformIndex;
	}

	double CargoOf(const TArray<TPair<UClass*, double>>& Cargo, UClass* Item)
	{
		for (const TPair<UClass*, double>& Each : Cargo)
		{
			if (Each.Key == Item)
			{
				return Each.Value;
			}
		}
		return 0.0;
	}

	void AddCargo(TArray<TPair<UClass*, double>>& Cargo, UClass* Item, double Amount)
	{
		for (int32 Index = 0; Index < Cargo.Num(); ++Index)
		{
			if (Cargo[Index].Key == Item)
			{
				Cargo[Index].Value += Amount;
				if (Cargo[Index].Value <= 0.5)
				{
					Cargo.RemoveAtSwap(Index);
				}
				return;
			}
		}
		if (Amount > 0.5)
		{
			Cargo.Emplace(Item, Amount);
		}
	}

	int32 FindStop(const TArray<FSTPlanStop>& Stops, const AFGTrainStationIdentifier* Station)
	{
		return Stops.IndexOfByPredicate([Station](const FSTPlanStop& Stop) { return Stop.Station == Station; });
	}
}

FSTTripPlanner::FSTTripPlanner(const ASTSubsystem& InSubsystem, TArray<FSTNode>& InNodes, TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>>& InBookings)
	: mSubsystem(InSubsystem), mNodes(InNodes), mBookings(InBookings)
{
	mTime = mSubsystem.Now();
	mStarted = FPlatformTime::Seconds();
	mDeadline = mStarted + FMath::Clamp(mSubsystem.mSettings.PlanBudgetMs, 5.f, 200.f) / 1000.0;
	bFcc = mSubsystem.IsFccAvailable();
	mMaxStops = FMath::Clamp(FMath::RoundToInt(mSubsystem.mSettings.MaxStops), 2, 8);
	mMaxTripSeconds = FMath::Max(60.0, mSubsystem.mSettings.MaxTripMinutes * 60.0);
	for (int32 Index = 0; Index < mNodes.Num(); ++Index)
	{
		const FSTNode& Node = mNodes[Index];
		if (Node.bAnyItem)
		{
			mAnyNode.Add(Node.Station.Get(), Index);
		}
		else
		{
			mNodeIndex.Add(MakeTuple(static_cast<const AFGTrainStationIdentifier*>(Node.Station.Get()), Node.Item.Get(), Node.Role == ESTRole::Supply), Index);
		}
	}
}

void FSTTripPlanner::RaiseTripLimit(double Seconds)
{
	mMaxTripSeconds = FMath::Max(mMaxTripSeconds, Seconds);
}

void FSTTripPlanner::ResetTripLimit(double AtLeast)
{
	mMaxTripSeconds = FMath::Max(FMath::Max(60.0, mSubsystem.mSettings.MaxTripMinutes * 60.0), AtLeast);
}

bool FSTTripPlanner::IsOverBudget() const
{
	return FPlatformTime::Seconds() > mDeadline;
}

double FSTTripPlanner::GetElapsedMs() const
{
	return (FPlatformTime::Seconds() - mStarted) * 1000.0;
}

int32 FSTTripPlanner::FindNode(const AFGTrainStationIdentifier* Station, UClass* Item, bool bSupply) const
{
	if (const int32* Found = mNodeIndex.Find(MakeTuple(Station, Item, bSupply)))
	{
		return *Found;
	}
	if (!bSupply)
	{
		if (const int32* Any = mAnyNode.Find(Station))
		{
			return *Any;
		}
	}
	return INDEX_NONE;
}

double FSTTripPlanner::FirstFreeArrival(const AFGTrainStationIdentifier* Station, double Arrive, double Stay) const
{
	const TArray<FSTBooking>* Bookings = mBookings.Find(Station);
	if (!Bookings)
	{
		return Arrive;
	}
	const double Gap = FMath::Max(0.0, static_cast<double>(mSubsystem.mSettings.ArrivalGapSeconds));
	TArray<FSTBooking> Others;
	for (const FSTBooking& Each : *Bookings)
	{
		if (Each.Train != mTrainIndex)
		{
			Others.Add(Each);
		}
	}
	Others.Sort([](const FSTBooking& A, const FSTBooking& B) { return A.From < B.From; });
	// The first slot that fits between bookings, gaps included.
	double Candidate = Arrive;
	for (const FSTBooking& Each : Others)
	{
		if (Candidate + Stay + Gap <= Each.From)
		{
			break;
		}
		Candidate = FMath::Max(Candidate, Each.Until + Gap);
	}
	return Candidate;
}

double FSTTripPlanner::GetAvailable(int32 Node, double Arrive) const
{
	const FSTNode& Supplier = mNodes[Node];
	const double Until = Arrive + mSubsystem.EstimateStaySeconds(Supplier.Station.Get()) - mTime;
	return FMath::Min<double>(Supplier.Capacity, Supplier.Stock + Supplier.Rate * Until) - Supplier.Reserved;
}

double FSTTripPlanner::GetRoom(int32 Node, double Arrive, bool& bOutWants) const
{
	const FSTNode& Consumer = mNodes[Node];
	const double Projected = Consumer.Stock + Consumer.Reserved - Consumer.Rate * (Arrive - mTime);
	bOutWants = Projected < Consumer.Capacity * Consumer.TargetPercent / 100.0;
	return Consumer.Capacity - FMath::Max(0.0, Projected);
}

double FSTTripPlanner::GetUrgency(int32 Node, double Arrive) const
{
	const FSTNode& Consumer = mNodes[Node];
	const double Projected = Consumer.Stock + Consumer.Reserved - Consumer.Rate * (Arrive - mTime);
	const double Target = Consumer.Capacity * Consumer.TargetPercent / 100.0;
	double Urgency = 1.0;
	if (Target > 0.0 && Projected < Target)
	{
		Urgency += FMath::Clamp((Target - Projected) / Target, 0.0, 1.0) * 2.0;
		Urgency += (Consumer.bStarved || Projected <= 0.0) ? 2.0 : 0.0;
		// Fairness: the longer since the consumer was last served, the more urgent (up to 2 after an hour).
		const double Since = mSubsystem.GetLastServed(Consumer.Station.Get(), Consumer.Item.Get());
		Urgency += FMath::Clamp((mTime - Since) / 1800.0, 0.0, 2.0);
	}
	return Urgency;
}

FString FSTTripPlanner::DescribeProblems() const
{
	TArray<TPair<FString, int32>> Sorted;
	for (const TPair<FString, int32>& Each : mProblems)
	{
		Sorted.Emplace(Each.Key, Each.Value);
	}
	Sorted.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B) { return A.Value > B.Value; });
	TArray<FString> Parts;
	for (int32 Index = 0; Index < Sorted.Num() && Index < 3; ++Index)
	{
		Parts.Add(Sorted[Index].Value > 1 ? FString::Printf(TEXT("%s (%d times)"), *Sorted[Index].Key, Sorted[Index].Value) : Sorted[Index].Key);
	}
	if (Sorted.Num() > 3)
	{
		Parts.Add(FString::Printf(TEXT("%d other reasons"), Sorted.Num() - 3));
	}
	return FString::Join(Parts, TEXT("; "));
}

bool FSTTripPlanner::IsSameKind(int32 A, int32 B) const
{
	if (A == B)
	{
		return true;
	}
	if (!mPlatforms.IsValidIndex(A) || !mPlatforms.IsValidIndex(B))
	{
		return false;
	}
	const FPlatformInfo& First = mPlatforms[A];
	const FPlatformInfo& Second = mPlatforms[B];
	if (First.bSupply != Second.bSupply || First.bDemand != Second.bDemand || First.bAnyItem != Second.bAnyItem || First.bLoadMode != Second.bLoadMode
		|| First.bFluid != Second.bFluid || First.Items.Num() != Second.Items.Num())
	{
		return false;
	}
	for (UClass* Item : First.Items)
	{
		if (!Second.Items.Contains(Item))
		{
			return false;
		}
	}
	return true;
}

bool FSTTripPlanner::IsSupplyFull(int32 Node) const
{
	const FSTNode& Supplier = mNodes[Node];
	return Supplier.Capacity > 0 && Supplier.Stock >= 0.9 * Supplier.Capacity;
}

double FSTTripPlanner::GetCarRoom(const FCarInfo& Car, const TArray<TPair<UClass*, double>>& Cargo, UClass* Item) const
{
	const bool bFluid = STNames::IsFluid(Item);
	int32 Used = 0;
	double Partial = 0.0;
	for (const TPair<UClass*, double>& Each : Cargo)
	{
		// A car carries solids or one fluid, never both.
		if (STNames::IsFluid(Each.Key) != bFluid || (bFluid && Each.Key != Item))
		{
			return 0.0;
		}
		const int32 Stack = STLayout::GetSlotSize(Car.Inventory, Each.Key);
		Used += FMath::CeilToInt(Each.Value / Stack);
		if (Each.Key == Item)
		{
			const double Rest = FMath::Fmod(Each.Value, static_cast<double>(Stack));
			Partial = Rest > 0.0 ? Stack - Rest : 0.0;
		}
	}
	return FMath::Max(0, Car.Slots - Used) * static_cast<double>(STLayout::GetSlotSize(Car.Inventory, Item)) + Partial;
}

int32 FSTTripPlanner::GetPlatformInfo(const AFGBuildableTrainPlatformCargo* Platform) const
{
	if (!Platform)
	{
		return INDEX_NONE;
	}
	if (const int32* Found = mPlatformIndex.Find(Platform))
	{
		return *Found;
	}
	FPlatformInfo Info;
	Info.Platform = Platform;
	Info.bLoadMode = Platform->GetIsInLoadMode();
	Info.bFluid = STLayout::GetPlatformCargoType(Platform) == EFreightCargoType::FCT_Liquid;
	if (const FSTPlatformConfig* Config = mSubsystem.FindPlatformConfig(Platform); Config && Config->Role != ESTRole::Off)
	{
		Info.bConfigured = true;
		Info.bSupply = Config->Role == ESTRole::Supply;
		Info.bDemand = Config->Role == ESTRole::Demand;
		for (const FSTItemRule& Rule : Config->Items)
		{
			if (Rule.Item)
			{
				Info.Items.AddUnique(Rule.Item.Get());
			}
			else
			{
				Info.bAnyItem = Info.bDemand;
			}
		}
	}
	if (const UFGInventoryComponent* Inventory = Platform->GetInventory())
	{
		Info.Slots = Inventory->GetSizeLinear();
		Info.Inventory = Inventory;
		TArray<FInventoryStack> Stacks;
		Inventory->GetInventoryStacks(Stacks, true);
		for (const FInventoryStack& Stack : Stacks)
		{
			if (Stack.HasItems())
			{
				++Info.Occupied;
				Info.Stock.FindOrAdd(Stack.Item.GetItemClass()) += Stack.NumItems;
			}
		}
	}
	const int32 Index = mPlatforms.Add(MoveTemp(Info));
	mPlatformIndex.Add(Platform, Index);
	return Index;
}

const TArray<TArray<int32>>& FSTTripPlanner::GetCarPlatforms(const AFGTrainStationIdentifier* Station) const
{
	if (const TArray<TArray<int32>>* Found = mCarPlatforms.Find(Station))
	{
		return *Found;
	}
	TArray<TArray<int32>> Out;
	const AFGTrain* Train = mSubsystem.mTrains[mTrainIndex].Train;
	const AFGBuildableRailroadStation* Building = Station ? Station->GetStation() : nullptr;
	if (Train && Building)
	{
		const TArray<AFGBuildableTrainPlatform*> Platforms = STLayout::GetPlatforms(Building);
		for (const TArray<AFGRailroadVehicle*>& Map : mSubsystem.GetDockMaps(Train, Station))
		{
			TArray<int32>& PerCar = Out.AddDefaulted_GetRef();
			for (const FCarInfo& Car : mCars)
			{
				const int32 Position = Map.IndexOfByKey(Car.Car);
				const AFGBuildableTrainPlatformCargo* Platform = Platforms.IsValidIndex(Position) ? Cast<AFGBuildableTrainPlatformCargo>(Platforms[Position]) : nullptr;
				PerCar.Add(GetPlatformInfo(Platform));
			}
		}
	}
	return mCarPlatforms.Add(Station, MoveTemp(Out));
}

bool FSTTripPlanner::BeginTrain(int32 TrainIndex, const AFGTrainStationIdentifier* Anchor, double FreeAt, bool bInAnchorDocked)
{
	mTrainIndex = TrainIndex;
	mMaxTripSeconds = FMath::Max(60.0, mSubsystem.mSettings.MaxTripMinutes * 60.0);
	mAnchor = Anchor;
	mFreeAt = FreeAt;
	bAnchorDocked = bInAnchorDocked;
	mCars.Reset();
	mMoves.Reset();
	mCarPlatforms.Reset();
	mProblems.Reset();
	bPushPull = ASTSubsystem::IsPushPull(mSubsystem.mTrains[TrainIndex].Train);
	const FSTTrainState& State = mSubsystem.mTrains[TrainIndex];
	const AFGTrain* Train = State.Train;
	if (!IsValid(Train) || !Anchor)
	{
		return false;
	}
	bool bCarrying = false;
	for (AFGRailroadVehicle* Vehicle : STLayout::GetVehicles(Train))
	{
		AFGFreightWagon* Wagon = Cast<AFGFreightWagon>(Vehicle);
		const UFGInventoryComponent* Inventory = Wagon ? Wagon->GetFreightInventory() : nullptr;
		if (!Inventory)
		{
			continue;
		}
		FCarInfo& Car = mCars.AddDefaulted_GetRef();
		Car.Car = Wagon;
		Car.Slots = Inventory->GetSizeLinear();
		Car.Inventory = Inventory;
		TArray<FInventoryStack> Stacks;
		Inventory->GetInventoryStacks(Stacks, true);
		for (const FInventoryStack& Stack : Stacks)
		{
			if (Stack.HasItems())
			{
				AddCargo(Car.Cargo, Stack.Item.GetItemClass(), Stack.NumItems);
				bCarrying = true;
			}
		}
	}
	if (mCars.Num() == 0)
	{
		return false;
	}
	bRelaxed = bCarrying && State.UnwantedCargoSince > 0.0 && mTime - State.UnwantedCargoSince > mSubsystem.mSettings.LeftoverMinutes * 60.0;

	// Does car Index stop beside a platform matching Test at Station, whichever way the train docks?
	auto CarFits = [this](int32 Index, const AFGTrainStationIdentifier* Station, TFunctionRef<bool(const FPlatformInfo&)> Test)
	{
		const TArray<TArray<int32>>& Maps = GetCarPlatforms(Station);
		if (Maps.Num() == 0)
		{
			return false;
		}
		for (const TArray<int32>& Map : Maps)
		{
			if (!Map.IsValidIndex(Index) || Map[Index] == INDEX_NONE || !Test(mPlatforms[Map[Index]]))
			{
				return false;
			}
		}
		return true;
	};
	auto Demands = [](UClass* Item)
	{
		return [Item](const FPlatformInfo& Platform) { return Platform.bDemand && !Platform.bLoadMode && (Platform.bAnyItem || Platform.Items.Contains(Item)); };
	};

	TArray<FSTPlanMove> Moves;
	// Cargo already on board.
	for (int32 CarIndex = 0; CarIndex < mCars.Num(); ++CarIndex)
	{
		for (const TPair<UClass*, double>& Carried : mCars[CarIndex].Cargo)
		{
			for (int32 To = 0; To < mNodes.Num(); ++To)
			{
				const FSTNode& Consumer = mNodes[To];
				if (Consumer.Role != ESTRole::Demand || (Consumer.Item.Get() != Carried.Key && !Consumer.bAnyItem) || !mSubsystem.CanServe(State, Consumer.Station.Get()))
				{
					continue;
				}
				if (Consumer.bAnyItem && !bRelaxed)
				{
					continue;
				}
				if (mSubsystem.IsNoProgress(State, Consumer.Station.Get(), Carried.Key, false))
				{
					UE_LOG(LogSmartTrains, Verbose, TEXT("PLANNER %s: no %s to %s for now (it unloaded none there lately)"), *STLayout::Name(Train), *STNames::Item(Carried.Key),
						*STLayout::Name(Consumer.Station.Get()));
					continue;
				}
				if (CarFits(CarIndex, Consumer.Station.Get(), Demands(Carried.Key)))
				{
					Moves.AddUnique({INDEX_NONE, To, Carried.Key});
				}
			}
		}
	}
	// Supplier -> consumer pairs that at least one car of the train fits at both ends.
	for (int32 From = 0; From < mNodes.Num(); ++From)
	{
		const FSTNode& Supplier = mNodes[From];
		if (Supplier.Role != ESTRole::Supply || !Supplier.Item || !mSubsystem.CanServe(State, Supplier.Station.Get()))
		{
			continue;
		}
		const float ToSupplier = mSubsystem.EstimateTravelSeconds(Anchor, Supplier.Station.Get(), bPushPull);
		if (ToSupplier < 0.f || GetAvailable(From, mFreeAt + ToSupplier) <= 0.0)
		{
			continue;
		}
		UClass* Item = Supplier.Item.Get();
		if (mSubsystem.IsNoProgress(State, Supplier.Station.Get(), Item, true))
		{
			UE_LOG(LogSmartTrains, Verbose, TEXT("PLANNER %s: no %s from %s for now (it loaded none there lately)"), *STLayout::Name(Train), *STNames::Item(Item),
				*STLayout::Name(Supplier.Station.Get()));
			continue;
		}
		const bool bFull = IsSupplyFull(From);
		TArray<int32> SupplyCars;
		for (int32 CarIndex = 0; CarIndex < mCars.Num(); ++CarIndex)
		{
			if (CarFits(CarIndex, Supplier.Station.Get(), [Item](const FPlatformInfo& Platform) { return Platform.bSupply && Platform.bLoadMode && Platform.Items.Contains(Item); }))
			{
				SupplyCars.Add(CarIndex);
			}
		}
		if (SupplyCars.Num() == 0)
		{
			continue;
		}
		for (int32 To = 0; To < mNodes.Num(); ++To)
		{
			const FSTNode& Consumer = mNodes[To];
			if (Consumer.Role != ESTRole::Demand || (Consumer.Item.Get() != Item && !Consumer.bAnyItem) || Consumer.Station == Supplier.Station
				|| !mSubsystem.CanServe(State, Consumer.Station.Get()))
			{
				continue;
			}
			if (Consumer.bAnyItem && !bFull)
			{
				continue;
			}
			if (!Consumer.bAnyItem && !bFull)
			{
				const float Leg = mSubsystem.EstimateTravelSeconds(Supplier.Station.Get(), Consumer.Station.Get(), bPushPull);
				bool bWants = false;
				GetRoom(To, mFreeAt + ToSupplier + mSubsystem.EstimateStaySeconds(Supplier.Station.Get()) + FMath::Max(0.f, Leg), bWants);
				if (!bWants || Leg < 0.f)
				{
					continue;
				}
			}
			const bool bAnyCar = SupplyCars.ContainsByPredicate([&](int32 CarIndex) { return CarFits(CarIndex, Consumer.Station.Get(), Demands(Item)); });
			if (bAnyCar)
			{
				Moves.AddUnique({From, To, Item});
			}
		}
	}
	// Solo scores, then keep the best few (cargo on board always).
	for (FSTPlanMove& Move : Moves)
	{
		TArray<FSTPlanStop> Stops;
		FSTPlanResult Result;
		double Second = 0.0;
		Move.SoloScore = InsertBest(Stops, Move, 0, Result, Second) ? Result.Score : 0.0;
	}
	Moves.Sort([](const FSTPlanMove& A, const FSTPlanMove& B) { return A.SoloScore > B.SoloScore; });
	int32 Kept = 0;
	for (const FSTPlanMove& Move : Moves)
	{
		if (Move.From == INDEX_NONE || Kept++ < MaxMovesPerTrain)
		{
			mMoves.Add(Move);
		}
	}
	UE_LOG(LogSmartTrains, Verbose, TEXT("PLANNER %s: %d moves (%d kept), %d cars, from %s at %+.0f s%s"), *STLayout::Name(Train), Moves.Num(), mMoves.Num(),
		mCars.Num(), *STLayout::Name(Anchor), FreeAt - mTime, bRelaxed ? TEXT(", unwanted cargo may go anywhere") : TEXT(""));
	FTrainSetup& Setup = mSetups.FindOrAdd(TrainIndex);
	Setup.Anchor = Anchor;
	Setup.FreeAt = FreeAt;
	Setup.bAnchorDocked = bAnchorDocked;
	Setup.bRelaxed = bRelaxed;
	Setup.bPushPull = bPushPull;
	Setup.Cars = mCars;
	Setup.Moves = mMoves;
	Setup.CarPlatforms = mCarPlatforms;
	Setup.Problems = mProblems;
	return true;
}

bool FSTTripPlanner::ResumeTrain(int32 TrainIndex, const AFGTrainStationIdentifier* Anchor, double FreeAt, bool bInAnchorDocked)
{
	const FTrainSetup* Setup = mSetups.Find(TrainIndex);
	if (!Setup || Setup->Anchor != Anchor || Setup->FreeAt != FreeAt || Setup->bAnchorDocked != bInAnchorDocked)
	{
		return false;
	}
	mTrainIndex = TrainIndex;
	mMaxTripSeconds = FMath::Max(60.0, mSubsystem.mSettings.MaxTripMinutes * 60.0);
	mAnchor = Anchor;
	mFreeAt = FreeAt;
	bAnchorDocked = bInAnchorDocked;
	bRelaxed = Setup->bRelaxed;
	bPushPull = Setup->bPushPull;
	mCars = Setup->Cars;
	mMoves = Setup->Moves;
	mCarPlatforms = Setup->CarPlatforms;
	mProblems = Setup->Problems;
	return true;
}

FSTPlanResult FSTTripPlanner::Simulate(const TArray<FSTPlanStop>& Stops) const
{
	TSet<TPair<int32, int32>> Skips;
	FSTPlanResult Result;
	for (int32 Pass = 0; Pass <= mCars.Num(); ++Pass)
	{
		const int32 Before = Skips.Num();
		Result = SimulateWithSkips(Stops, Skips);
		if (Result.bValid || Skips.Num() == Before)
		{
			break;
		}
	}
	return Result;
}

FSTPlanResult FSTTripPlanner::SimulateWithSkips(const TArray<FSTPlanStop>& Stops, TSet<TPair<int32, int32>>& InOutSkips) const
{
	FSTPlanResult Result;
	auto Fail = [&Result](const FString& Why)
	{
		Result.bValid = false;
		Result.Problem = Why;
		return Result;
	};
	if (Stops.Num() == 0)
	{
		Result.bValid = true;
		Result.EndTime = mFreeAt;
		return Result;
	}
	if (Stops.Num() > mMaxStops)
	{
		return Fail(TEXT("too many stops"));
	}
	if (bAnchorDocked && Stops[0].Station == mAnchor)
	{
		return Fail(TEXT("first stop is the station the train is docked at"));
	}

	// Times.
	Result.Arrive.SetNum(Stops.Num());
	const AFGTrainStationIdentifier* Previous = mAnchor;
	double Clock = mFreeAt;
	for (int32 Index = 0; Index < Stops.Num(); ++Index)
	{
		const AFGTrainStationIdentifier* Station = Stops[Index].Station;
		for (int32 Other = 0; Other < Index; ++Other)
		{
			if (Stops[Other].Station == Station)
			{
				return Fail(TEXT("a station twice"));
			}
		}
		const float Travel = Previous == Station ? 0.f : mSubsystem.EstimateTravelSeconds(Previous, Station, bPushPull);
		if (Travel < 0.f)
		{
			return Fail(FString::Printf(TEXT("%s can't be reached from %s%s"), *STLayout::Name(Station), *STLayout::Name(Previous),
				bPushPull ? TEXT(" (push-pull)") : TEXT(" (one way only)")));
		}
		Result.Arrive[Index] = FirstFreeArrival(Station, Clock + Travel, mSubsystem.EstimateStaySeconds(Station));
		Clock = Result.Arrive[Index] + mSubsystem.EstimateStaySeconds(Station);
		Previous = Station;
	}
	Result.EndTime = Clock;
	if (Stops.Num() > 2 && Result.EndTime - mFreeAt > mMaxTripSeconds)
	{
		return Fail(TEXT("longer than the trip limit"));
	}

	// Cars stop by stop.
	TArray<TArray<TPair<UClass*, double>>> Cargo;
	for (const FCarInfo& Car : mCars)
	{
		Cargo.Add(Car.Cargo);
	}
	// Per car: items loaded on this trip, with the stop of the first load and whether any of it was unloaded later.
	struct FLoaded
	{
		int32 Car;
		UClass* Item;
		int32 Stop;
		bool bDelivered;
	};
	TArray<FLoaded> Loaded;
	TArray<TPair<TPair<int32, UClass*>, double>> PlatformPool;
	TArray<TPair<TPair<int32, UClass*>, double>> PlatformRoom;
	TArray<TPair<int32, double>> NodeSupply;
	TArray<TPair<int32, double>> NodeRoom;
	// Per item: total loaded, the room of the cars loading it, the strictest send-at share, full supplier, critical consumer.
	struct FItemTotals
	{
		UClass* Item;
		double LoadedAmount = 0.0;
		double LoadRoom = 0.0;
		double Share = 0.0;
		bool bFromFull = false;
		bool bCritical = false;
	};
	TArray<FItemTotals> Totals;
	auto TotalsFor = [&Totals](UClass* Item) -> FItemTotals&
	{
		for (FItemTotals& Each : Totals)
		{
			if (Each.Item == Item)
			{
				return Each;
			}
		}
		FItemTotals& New = Totals.AddDefaulted_GetRef();
		New.Item = Item;
		return New;
	};

	Result.Steps.SetNum(Stops.Num());
	for (int32 StopIndex = 0; StopIndex < Stops.Num(); ++StopIndex)
	{
		const FSTPlanStop& Stop = Stops[StopIndex];
		const double Arrive = Result.Arrive[StopIndex];
		const TArray<TArray<int32>>& Maps = GetCarPlatforms(Stop.Station);
		if (Maps.Num() == 0)
		{
			return Fail(FString::Printf(TEXT("no locomotive can dock at %s"), *STLayout::Name(Stop.Station)));
		}
		Result.Steps[StopIndex].SetNum(mCars.Num());
		for (int32 CarIndex = 0; CarIndex < mCars.Num(); ++CarIndex)
		{
			FSTPlanCarStep& Step = Result.Steps[StopIndex][CarIndex];
			if (InOutSkips.Contains(TPair<int32, int32>(CarIndex, StopIndex)))
			{
				Step.bSkip = true;
				continue;
			}
			// What the car would do beside its platform, for every way the train may dock.
			enum class EEffect : uint8 { None, Load, Unload, Harm };
			EEffect Effect = EEffect::None;
			// The platform the car stops beside for each way the train may dock (equivalent ones when they differ).
			TArray<int32> Beside;
			TArray<UClass*> Items;
			bool bAgree = true;
			for (int32 MapIndex = 0; MapIndex < Maps.Num(); ++MapIndex)
			{
				const int32 Here = Maps[MapIndex].IsValidIndex(CarIndex) ? Maps[MapIndex][CarIndex] : INDEX_NONE;
				EEffect ThisEffect = EEffect::None;
				TArray<UClass*> ThisItems;
				if (Here != INDEX_NONE)
				{
					const FPlatformInfo& Platform = mPlatforms[Here];
					if (Platform.bLoadMode)
					{
						for (UClass* Item : Stop.LoadItems)
						{
							const bool bOffers = Platform.Items.Contains(Item) || Platform.Stock.Contains(Item);
							if (bOffers && STNames::IsFluid(Item) == Platform.bFluid && GetCarRoom(mCars[CarIndex], Cargo[CarIndex], Item) > 0.0)
							{
								ThisItems.Add(Item);
							}
						}
						ThisEffect = ThisItems.Num() == 0 ? EEffect::None : (Platform.bSupply ? EEffect::Load : EEffect::Harm);
					}
					else
					{
						for (UClass* Item : Stop.UnloadItems)
						{
							if (CargoOf(Cargo[CarIndex], Item) > 0.0 && STNames::IsFluid(Item) == Platform.bFluid)
							{
								ThisItems.Add(Item);
							}
						}
						bool bTakesAll = Platform.bDemand;
						for (UClass* Item : ThisItems)
						{
							bTakesAll &= Platform.bAnyItem || Platform.Items.Contains(Item);
						}
						ThisEffect = ThisItems.Num() == 0 ? EEffect::None : (bTakesAll ? EEffect::Unload : EEffect::Harm);
					}
				}
				if (MapIndex == 0)
				{
					Effect = ThisEffect;
					Items = ThisItems;
					if (Here != INDEX_NONE)
					{
						Beside.Add(Here);
					}
				}
				else if (ThisEffect == EEffect::Harm || Effect == EEffect::Harm)
				{
					Effect = EEffect::Harm;
				}
				else if (ThisEffect != Effect || (Effect != EEffect::None && (ThisItems != Items || !IsSameKind(Here, Beside[0]))))
				{
					bAgree = false;
				}
				else if (Effect != EEffect::None)
				{
					Beside.Add(Here);
				}
			}
			if (Effect == EEffect::Harm || !bAgree)
			{
				if (!bFcc)
				{
					return Fail(FString::Printf(TEXT("car %d would %s at %s (needs Freight Car Control to skip it)"), CarIndex + 1,
						bAgree ? TEXT("move an item its platform doesn't take or give") : TEXT("work differently depending on how the train docks"), *STLayout::Name(Stop.Station)));
				}
				Step.bSkip = true;
				continue;
			}
			if (Effect == EEffect::Load)
			{
				for (UClass* Item : Items)
				{
					const int32 Node = FindNode(Stop.Station, Item, true);
					double& NodeLeft = FindOrAddValue<int32>(NodeSupply, Node, [&]() { return Node != INDEX_NONE ? GetAvailable(Node, Arrive) : 0.0; });
					auto InitialPool = [&](int32 PlatformIndex)
					{
						const FPlatformInfo& Platform = mPlatforms[PlatformIndex];
						const double Stock = Platform.Stock.FindRef(Item);
						if (Node == INDEX_NONE)
						{
							return Stock;
						}
						const FSTNode& Supplier = mNodes[Node];
						const double Share = Supplier.Stock > 0 ? Stock / Supplier.Stock : 1.0 / FMath::Max(1, Supplier.Platforms.Num());
						// What arrives up to the end of the stay, since the platform keeps filling while the train loads.
						const double Until = Arrive + mSubsystem.EstimateStaySeconds(Stop.Station) - mTime;
						return FMath::Max(0.0, Stock + Share * (Supplier.Rate * Until - Supplier.Reserved));
					};
					// Each way the train may dock has its own platform pools; the car plans the smallest of its amounts.
					for (int32 MapIndex = 0; MapIndex < Beside.Num(); ++MapIndex)
					{
						FindOrAddValue<TPair<int32, UClass*>>(PlatformPool, TPair<int32, UClass*>(PoolKey(MapIndex, Beside[MapIndex]), Item),
							[&]() { return InitialPool(Beside[MapIndex]); });
					}
					TArray<double*> Pools;
					double Pool = TNumericLimits<double>::Max();
					for (int32 MapIndex = 0; MapIndex < Beside.Num(); ++MapIndex)
					{
						double& Each = FindOrAddValue<TPair<int32, UClass*>>(PlatformPool, TPair<int32, UClass*>(PoolKey(MapIndex, Beside[MapIndex]), Item), []() { return 0.0; });
						Pools.Add(&Each);
						Pool = FMath::Min(Pool, Each);
					}
					const double Room = GetCarRoom(mCars[CarIndex], Cargo[CarIndex], Item);
					const double Amount = FMath::FloorToDouble(FMath::Min3(Room, Pool, FMath::Max(0.0, NodeLeft)));
					FItemTotals& Total = TotalsFor(Item);
					const bool bFirst = !Loaded.ContainsByPredicate([&](const FLoaded& Each) { return Each.Car == CarIndex && Each.Item == Item; });
					if (bFirst)
					{
						Total.LoadRoom += Room;
						Loaded.Add({CarIndex, Item, StopIndex, false});
					}
					if (Node != INDEX_NONE)
					{
						Total.Share = FMath::Max(Total.Share, mNodes[Node].SendAtPercent > 0.f ? mNodes[Node].SendAtPercent / 100.0 : 1.0);
						Total.bFromFull |= IsSupplyFull(Node);
					}
					if (Amount <= 0.0)
					{
						continue;
					}
					AddCargo(Cargo[CarIndex], Item, Amount);
					for (double* Each : Pools)
					{
						*Each -= Amount;
					}
					NodeLeft -= Amount;
					Total.LoadedAmount += Amount;
					Step.Loaded.Emplace(Item, static_cast<int32>(Amount));
					Result.Moved += static_cast<int32>(Amount);
				}
			}
			else if (Effect == EEffect::Unload)
			{
				for (UClass* Item : Items)
				{
					const double Carried = CargoOf(Cargo[CarIndex], Item);
					auto InitialSpace = [&](int32 PlatformIndex)
					{
						const FPlatformInfo& Platform = mPlatforms[PlatformIndex];
						const int32 Stack = STLayout::GetSlotSize(Platform.Inventory, Item);
						const int32 Stock = Platform.Stock.FindRef(Item);
						const int32 Rest = Stock % Stack;
						return FMath::Max(0, Platform.Slots - Platform.Occupied) * static_cast<double>(Stack) + (Rest > 0 ? Stack - Rest : 0);
					};
					for (int32 MapIndex = 0; MapIndex < Beside.Num(); ++MapIndex)
					{
						FindOrAddValue<TPair<int32, UClass*>>(PlatformRoom, TPair<int32, UClass*>(PoolKey(MapIndex, Beside[MapIndex]), Item),
							[&]() { return InitialSpace(Beside[MapIndex]); });
					}
					TArray<double*> Spaces;
					double Space = TNumericLimits<double>::Max();
					for (int32 MapIndex = 0; MapIndex < Beside.Num(); ++MapIndex)
					{
						double& Each = FindOrAddValue<TPair<int32, UClass*>>(PlatformRoom, TPair<int32, UClass*>(PoolKey(MapIndex, Beside[MapIndex]), Item), []() { return 0.0; });
						Spaces.Add(&Each);
						Space = FMath::Min(Space, Each);
					}
					const double Amount = FMath::FloorToDouble(FMath::Min(Carried, Space));
					for (FLoaded& Each : Loaded)
					{
						Each.bDelivered |= Each.Car == CarIndex && Each.Item == Item;
					}
					if (Amount <= 0.0)
					{
						continue;
					}
					AddCargo(Cargo[CarIndex], Item, -Amount);
					for (double* Each : Spaces)
					{
						*Each -= Amount;
					}
					Step.Unloaded.Emplace(Item, static_cast<int32>(Amount));
					Result.Moved += static_cast<int32>(Amount);
					const int32 Node = FindNode(Stop.Station, Item, false);
					if (Node == INDEX_NONE)
					{
						continue;
					}
					FItemTotals& Total = TotalsFor(Item);
					if (mNodes[Node].bAnyItem)
					{
						Result.Value += (Total.bFromFull || bRelaxed) ? SinkWeight * Amount : 0.0;
						continue;
					}
					bool bWants = false;
					double& RoomLeft = FindOrAddValue<int32>(NodeRoom, Node, [&]() { return GetRoom(Node, Arrive, bWants); });
					GetRoom(Node, Arrive, bWants);
					const double Valued = FMath::Clamp(RoomLeft, 0.0, Amount);
					RoomLeft -= Amount;
					Total.bCritical |= mNodes[Node].Priority == ESTPriority::Critical;
					const double Weight = STNames::PriorityWeight(mNodes[Node].Priority);
					if (bWants)
					{
						Result.Value += Weight * GetUrgency(Node, Arrive) * Valued;
					}
					else if (bRelaxed || Total.bFromFull)
					{
						Result.Value += OverTargetWeight * Weight * Valued;
					}
				}
			}
		}
	}
	// Cargo loaded on this trip that is never unloaded: the car must skip that pickup (FCC), or the trip doesn't work.
	bool bStranded = false;
	for (const FLoaded& Each : Loaded)
	{
		if (!Each.bDelivered && Result.Steps[Each.Stop][Each.Car].Loaded.Num() > 0)
		{
			bStranded = true;
			if (bFcc)
			{
				InOutSkips.Add(TPair<int32, int32>(Each.Car, Each.Stop));
			}
		}
	}
	if (bStranded)
	{
		return Fail(bFcc ? TEXT("some cars would load cargo with nowhere to go") : TEXT("some cars would load cargo with nowhere to go (needs Freight Car Control to skip them)"));
	}
	// Send-at batch: a trip only goes when it picks up at least the send-at share of what its loading cars hold.
	for (const FItemTotals& Total : Totals)
	{
		if (Total.LoadRoom > 0.0 && !Total.bCritical && !Total.bFromFull && Total.LoadedAmount + 0.5 < Total.Share * Total.LoadRoom)
		{
			return Fail(FString::Printf(TEXT("less than a batch of %s"), *STNames::Item(Total.Item)));
		}
	}
	Result.bValid = true;
	Result.Score = Result.Value / FMath::Max(MinScoreSeconds, Result.EndTime - mTime);
	return Result;
}

bool FSTTripPlanner::InsertBest(TArray<FSTPlanStop>& InOutStops, const FSTPlanMove& Move, int32 FirstMovable, FSTPlanResult& OutResult, double& OutSecond) const
{
	double Best = -1.0;
	OutSecond = 0.0;
	TArray<FSTPlanStop> BestStops;
	auto Try = [&](TArray<FSTPlanStop>& Candidate)
	{
		if (Candidate.Num() > mMaxStops)
		{
			return;
		}
		FSTPlanResult Result = Simulate(Candidate);
		if (!Result.bValid)
		{
			++mProblems.FindOrAdd(Result.Problem);
			return;
		}
		if (Result.Score > Best)
		{
			OutSecond = FMath::Max(OutSecond, Best);
			Best = Result.Score;
			BestStops = Candidate;
			OutResult = MoveTemp(Result);
		}
		else
		{
			OutSecond = FMath::Max(OutSecond, Result.Score);
		}
	};
	AFGTrainStationIdentifier* DropStation = mNodes[Move.To].Station.Get();
	auto PlaceDrop = [&](TArray<FSTPlanStop> Base, int32 After)
	{
		const int32 Existing = FindStop(Base, DropStation);
		if (Existing != INDEX_NONE)
		{
			if (Existing > After && Existing >= FirstMovable)
			{
				Base[Existing].UnloadItems.AddUnique(Move.Item);
				Try(Base);
			}
			return;
		}
		for (int32 Position = FMath::Max(After + 1, FirstMovable); Position <= Base.Num(); ++Position)
		{
			TArray<FSTPlanStop> Candidate = Base;
			FSTPlanStop& New = Candidate.InsertDefaulted_GetRef(Position);
			New.Station = DropStation;
			New.UnloadItems.Add(Move.Item);
			Try(Candidate);
		}
	};
	if (Move.From == INDEX_NONE)
	{
		PlaceDrop(InOutStops, FirstMovable - 1);
	}
	else
	{
		AFGTrainStationIdentifier* PickStation = mNodes[Move.From].Station.Get();
		const int32 Existing = FindStop(InOutStops, PickStation);
		if (Existing != INDEX_NONE)
		{
			if (Existing >= FirstMovable)
			{
				TArray<FSTPlanStop> Base = InOutStops;
				Base[Existing].LoadItems.AddUnique(Move.Item);
				PlaceDrop(Base, Existing);
			}
		}
		else
		{
			for (int32 Position = FirstMovable; Position <= InOutStops.Num(); ++Position)
			{
				TArray<FSTPlanStop> Base = InOutStops;
				FSTPlanStop& New = Base.InsertDefaulted_GetRef(Position);
				New.Station = PickStation;
				New.LoadItems.Add(Move.Item);
				PlaceDrop(Base, Position);
			}
		}
	}
	if (Best < 0.0)
	{
		return false;
	}
	InOutStops = MoveTemp(BestStops);
	return true;
}

FSTPlanResult FSTTripPlanner::Improve(TArray<FSTPlanStop>& InOutStops, int32 FirstMovable) const
{
	FSTPlanResult Current = Simulate(InOutStops);
	if (!Current.bValid)
	{
		Current.Score = 0.0;
	}
	TSet<int32> Used;
	for (int32 Round = 0; Round < 2 * mMaxStops + 4 && !IsOverBudget(); ++Round)
	{
		int32 BestMove = INDEX_NONE;
		double BestKey = -1.0;
		TArray<FSTPlanStop> BestStops;
		FSTPlanResult BestResult;
		for (int32 Index = 0; Index < mMoves.Num(); ++Index)
		{
			if (Used.Contains(Index))
			{
				continue;
			}
			TArray<FSTPlanStop> Candidate = InOutStops;
			FSTPlanResult Result;
			double Second = 0.0;
			if (!InsertBest(Candidate, mMoves[Index], FirstMovable, Result, Second) || Result.Score <= Current.Score * 1.0001 + 1e-9)
			{
				continue;
			}
			// Regret: how much is lost if this move waits and only its second best place, or nothing, is left.
			const double Key = Result.Score - FMath::Max(Second, Current.Score) + 1e-3 * Result.Score;
			if (Key > BestKey)
			{
				BestKey = Key;
				BestMove = Index;
				BestStops = MoveTemp(Candidate);
				BestResult = MoveTemp(Result);
			}
		}
		if (BestMove == INDEX_NONE)
		{
			break;
		}
		Used.Add(BestMove);
		InOutStops = MoveTemp(BestStops);
		Current = MoveTemp(BestResult);
	}
	return Current;
}

void FSTTripPlanner::Trim(TArray<FSTPlanStop>& InOutStops, const FSTPlanResult& Result, int32 FirstMovable)
{
	if (!Result.bValid)
	{
		return;
	}
	TArray<FSTPlanStop> Kept;
	for (int32 StopIndex = 0; StopIndex < InOutStops.Num(); ++StopIndex)
	{
		FSTPlanStop Stop = InOutStops[StopIndex];
		if (StopIndex >= FirstMovable && Result.Steps.IsValidIndex(StopIndex))
		{
			auto Moves = [&](UClass* Item, bool bLoad)
			{
				for (const FSTPlanCarStep& Step : Result.Steps[StopIndex])
				{
					for (const TPair<UClass*, int32>& Each : bLoad ? Step.Loaded : Step.Unloaded)
					{
						if (Each.Key == Item && Each.Value > 0)
						{
							return true;
						}
					}
				}
				return false;
			};
			Stop.LoadItems.RemoveAll([&](UClass* Item) { return !Moves(Item, true); });
			Stop.UnloadItems.RemoveAll([&](UClass* Item) { return !Moves(Item, false); });
			if (Stop.LoadItems.Num() == 0 && Stop.UnloadItems.Num() == 0)
			{
				continue;
			}
		}
		Kept.Add(MoveTemp(Stop));
	}
	InOutStops = MoveTemp(Kept);
}

void FSTTripPlanner::MakeLegs(const TArray<FSTPlanStop>& Stops, const FSTPlanResult& Result, TArray<FSTJobLeg>& OutLegs) const
{
	OutLegs.Reset();
	TArray<TArray<TPair<UClass*, double>>> Cargo;
	for (const FCarInfo& Car : mCars)
	{
		Cargo.Add(Car.Cargo);
	}
	for (int32 StopIndex = 0; StopIndex < Stops.Num(); ++StopIndex)
	{
		bool bUnfinished = false;
		FSTJobLeg& Leg = OutLegs.AddDefaulted_GetRef();
		Leg.Station = Stops[StopIndex].Station;
		Leg.PlannedArrival = Result.Arrive.IsValidIndex(StopIndex) ? Result.Arrive[StopIndex] : mTime;
		for (int32 CarIndex = 0; CarIndex < mCars.Num(); ++CarIndex)
		{
			const FSTPlanCarStep* Step = Result.Steps.IsValidIndex(StopIndex) && Result.Steps[StopIndex].IsValidIndex(CarIndex) ? &Result.Steps[StopIndex][CarIndex] : nullptr;
			bool bWorks = false;
			if (Step)
			{
				for (int32 Pass = 0; Pass < 2; ++Pass)
				{
					for (const TPair<UClass*, int32>& Each : Pass == 0 ? Step->Loaded : Step->Unloaded)
					{
						FSTCarWork& Work = Leg.Work.AddDefaulted_GetRef();
						Work.Car = mCars[CarIndex].Car;
						Work.Item = Each.Key;
						Work.bLoad = Pass == 0;
						Work.PlannedAmount = Each.Value;
						bWorks = true;
						AddCargo(Cargo[CarIndex], Each.Key, Pass == 0 ? Each.Value : -Each.Value);
					}
				}
				// A car still holding what it unloads never meets the "fully unload" rule: the platform has no room for the rest.
				for (const TPair<UClass*, int32>& Each : Step->Unloaded)
				{
					bUnfinished |= CargoOf(Cargo[CarIndex], Each.Key) >= 1.0;
				}
			}
			// With FCC every car without work skips the stop, so nothing moves that wasn't planned.
			if (bFcc && !bWorks)
			{
				Leg.SkipCars.Add(mCars[CarIndex].Car);
			}
		}
		const TArray<UClass*> Loads = Leg.GetItems(true);
		const TArray<UClass*> Unloads = Leg.GetItems(false);
		bool bLaterSame = false;
		for (int32 Later = StopIndex + 1; Later < Stops.Num(); ++Later)
		{
			for (UClass* Item : Loads)
			{
				bLaterSame |= Stops[Later].LoadItems.Contains(Item);
			}
			for (UClass* Item : Unloads)
			{
				bLaterSame |= Stops[Later].UnloadItems.Contains(Item);
			}
		}
		Leg.bPartial = bLaterSame || bUnfinished || (Loads.Num() > 0 && Unloads.Num() > 0);
	}
}

void FSTTripPlanner::Reserve(const TArray<FSTPlanStop>& Stops, const FSTPlanResult& Result)
{
	for (int32 StopIndex = 0; StopIndex < Stops.Num() && Result.Steps.IsValidIndex(StopIndex); ++StopIndex)
	{
		const AFGTrainStationIdentifier* Station = Stops[StopIndex].Station;
		for (const FSTPlanCarStep& Step : Result.Steps[StopIndex])
		{
			for (const TPair<UClass*, int32>& Each : Step.Loaded)
			{
				const int32 Node = FindNode(Station, Each.Key, true);
				if (Node != INDEX_NONE)
				{
					mNodes[Node].Reserved += Each.Value;
				}
			}
			for (const TPair<UClass*, int32>& Each : Step.Unloaded)
			{
				const int32 Node = FindNode(Station, Each.Key, false);
				if (Node != INDEX_NONE)
				{
					mNodes[Node].Reserved += Each.Value;
				}
			}
		}
		if (Result.Arrive.IsValidIndex(StopIndex))
		{
			mBookings.FindOrAdd(Station).Add({mTrainIndex, Result.Arrive[StopIndex], Result.Arrive[StopIndex] + mSubsystem.EstimateStaySeconds(Station)});
		}
	}
}

void FSTTripPlanner::Release(const TArray<FSTJobLeg>& Legs, int32 From)
{
	for (int32 Index = FMath::Max(0, From); Index < Legs.Num(); ++Index)
	{
		for (const FSTCarWork& Work : Legs[Index].Work)
		{
			const int32 Node = FindNode(Legs[Index].Station, Work.Item.Get(), Work.bLoad);
			if (Node != INDEX_NONE)
			{
				mNodes[Node].Reserved = FMath::Max(0, mNodes[Node].Reserved - Work.PlannedAmount);
			}
		}
	}
}

FString FSTTripPlanner::Describe(const TArray<FSTPlanStop>& Stops, const FSTPlanResult& Result) const
{
	TArray<FString> Parts;
	for (int32 Index = 0; Index < Stops.Num(); ++Index)
	{
		TArray<FString> Items;
		for (UClass* Item : Stops[Index].LoadItems)
		{
			Items.Add(TEXT("+") + STNames::Item(Item));
		}
		for (UClass* Item : Stops[Index].UnloadItems)
		{
			Items.Add(TEXT("-") + STNames::Item(Item));
		}
		Parts.Add(FString::Printf(TEXT("%s (%s)"), *STLayout::Name(Stops[Index].Station), *FString::Join(Items, TEXT(" "))));
	}
	return FString::Join(Parts, TEXT(" -> "));
}

TArray<int32> STAssignment::Solve(const TArray<TArray<double>>& Value)
{
	// Hungarian method (Kuhn-Munkres with potentials), maximising the total value; rows <= columns after padding.
	const int32 Rows = Value.Num();
	int32 Columns = 0;
	for (const TArray<double>& Row : Value)
	{
		Columns = FMath::Max(Columns, Row.Num());
	}
	Columns = FMath::Max(Columns, Rows);
	TArray<int32> Assigned;
	Assigned.Init(INDEX_NONE, Rows);
	if (Rows == 0)
	{
		return Assigned;
	}
	double Highest = 0.0;
	for (const TArray<double>& Row : Value)
	{
		for (double Each : Row)
		{
			Highest = FMath::Max(Highest, Each);
		}
	}
	auto Cost = [&](int32 Row, int32 Column) { return Highest - (Value[Row].IsValidIndex(Column) ? Value[Row][Column] : 0.0); };
	const double Infinity = TNumericLimits<double>::Max() / 4.0;
	TArray<double> U, V, MinV;
	TArray<int32> Match, Way;
	U.Init(0.0, Rows + 1);
	V.Init(0.0, Columns + 1);
	Match.Init(0, Columns + 1);
	Way.Init(0, Columns + 1);
	TArray<bool> Used;
	for (int32 Row = 1; Row <= Rows; ++Row)
	{
		Match[0] = Row;
		int32 Column0 = 0;
		MinV.Init(Infinity, Columns + 1);
		Used.Init(false, Columns + 1);
		do
		{
			Used[Column0] = true;
			const int32 Row0 = Match[Column0];
			double Delta = Infinity;
			int32 Column1 = 0;
			for (int32 Column = 1; Column <= Columns; ++Column)
			{
				if (Used[Column])
				{
					continue;
				}
				const double Current = Cost(Row0 - 1, Column - 1) - U[Row0] - V[Column];
				if (Current < MinV[Column])
				{
					MinV[Column] = Current;
					Way[Column] = Column0;
				}
				if (MinV[Column] < Delta)
				{
					Delta = MinV[Column];
					Column1 = Column;
				}
			}
			for (int32 Column = 0; Column <= Columns; ++Column)
			{
				if (Used[Column])
				{
					U[Match[Column]] += Delta;
					V[Column] -= Delta;
				}
				else
				{
					MinV[Column] -= Delta;
				}
			}
			Column0 = Column1;
		}
		while (Match[Column0] != 0);
		do
		{
			const int32 Column1 = Way[Column0];
			Match[Column0] = Match[Column1];
			Column0 = Column1;
		}
		while (Column0 != 0);
	}
	for (int32 Column = 1; Column <= Columns; ++Column)
	{
		const int32 Row = Match[Column] - 1;
		if (Row >= 0 && Value[Row].IsValidIndex(Column - 1) && Value[Row][Column - 1] > 0.0)
		{
			Assigned[Row] = Column - 1;
		}
	}
	return Assigned;
}
