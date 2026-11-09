#include "STTripPlanner.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "STForecast.h"
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
	// Requests: suppliers and places to empty cars tried per train (nearest first).
	constexpr int32 MaxRequestSources = 3;
	constexpr int32 MaxRequestConsumers = 3;
	constexpr int32 MaxRequestSinks = 2;
	// Least a car unloads at a stop, in car slots (or all it holds when that is less). [slots]
	constexpr double MinUnloadSlots = 2.0;
	// Value a stop costs, once plus once per other train booked at that station near the visit. [car-load minutes]
	constexpr double StopCostValue = 1.0;
	// How far before and after a visit other trains' bookings count towards its stop cost. [s]
	constexpr double StopNearSeconds = 120.0;
	// What dumping a car load into a sink costs a train switching services: goods a consumer needs within the horizon,
	// and goods none does. [car-load minutes]
	constexpr double NeededDumpValue = 30.0;
	constexpr double SurplusDumpValue = 0.5;
	// A car loads an item only if it takes at least this share of a car of it.
	constexpr double MinLoadShare = 0.05;
	// Other trains' bookings count towards the queue limit only for arrivals this soon: later plans still change. [s]
	constexpr double QueueLookSeconds = 300.0;
	// Value of a car left holding cargo after a trip (a car emptied is worth the same). [car-load minutes]
	constexpr double TiedCarValue = 2.0;
	// Share of TiedCarValue for a car holding only items a demand platform needs within the horizon.
	constexpr double NeededSoonShare = 0.25;

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
	mMaxStops = FMath::Max(mMaxStops, STForecast::HorizonStops);
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
		if (Node.Role == ESTRole::Demand && !Node.bAnyItem && Node.Item && !mNeededSoon.Contains(Node.Item.Get())
			&& STForecast::Deadline(Node, Node.Planned, mTime, STLayout::GetCarCapacity(nullptr, Node.Item)) < mTime + STForecast::HorizonSeconds)
		{
			mNeededSoon.Add(Node.Item.Get());
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

int32 FSTTripPlanner::CountAhead(const AFGTrainStationIdentifier* Station, double Reach, double Dock) const
{
	int32 Ahead = 0;
	if (const TArray<FSTBooking>* Bookings = mBookings.Find(Station))
	{
		for (const FSTBooking& Each : *Bookings)
		{
			Ahead += Each.Train != mTrainIndex && Each.Until > Reach && Each.From < Dock ? 1 : 0;
		}
	}
	return Ahead;
}

int32 FSTTripPlanner::CountNear(const AFGTrainStationIdentifier* Station, double From, double Until) const
{
	// Other trains at the station, and at stations whose only way in is through the same switch: they cross it too.
	const void* Junction = mSubsystem.GetJunction(Station);
	int32 Near = 0;
	for (const TPair<const AFGTrainStationIdentifier*, TArray<FSTBooking>>& Pair : mBookings)
	{
		if (Pair.Key != Station && (!Junction || mSubsystem.GetJunction(Pair.Key) != Junction))
		{
			continue;
		}
		for (const FSTBooking& Each : Pair.Value)
		{
			Near += Each.Train != mTrainIndex && Each.Until > From && Each.From < Until ? 1 : 0;
		}
	}
	return Near;
}

TSharedRef<FJsonObject> FSTTripPlanner::DescribeForReplay() const
{
	TSharedRef<FJsonObject> Train = MakeShared<FJsonObject>();
	Train->SetStringField(TEXT("name"), TrainName());
	Train->SetNumberField(TEXT("index"), mTrainIndex);
	Train->SetStringField(TEXT("anchor"), GetNameSafe(mAnchor));
	Train->SetNumberField(TEXT("freeAt"), mFreeAt);
	Train->SetBoolField(TEXT("anchorDocked"), bAnchorDocked);
	Train->SetBoolField(TEXT("pushPull"), bPushPull);
	Train->SetBoolField(TEXT("fcc"), bFcc);
	TArray<TSharedPtr<FJsonValue>> Cars;
	for (const FCarInfo& Car : mCars)
	{
		TSharedRef<FJsonObject> Each = MakeShared<FJsonObject>();
		Each->SetNumberField(TEXT("slots"), Car.Slots);
		TSharedRef<FJsonObject> Cargo = MakeShared<FJsonObject>();
		for (const TPair<UClass*, double>& Held : Car.Cargo)
		{
			Cargo->SetNumberField(GetPathNameSafe(Held.Key), Held.Value);
		}
		Each->SetObjectField(TEXT("cargo"), Cargo);
		Cars.Add(MakeShared<FJsonValueObject>(Each));
	}
	Train->SetArrayField(TEXT("cars"), Cars);
	TArray<TSharedPtr<FJsonValue>> Platforms;
	for (const FPlatformInfo& Platform : mPlatforms)
	{
		TSharedRef<FJsonObject> Each = MakeShared<FJsonObject>();
		Each->SetStringField(TEXT("key"), GetNameSafe(Platform.Platform));
		Each->SetBoolField(TEXT("configured"), Platform.bConfigured);
		Each->SetBoolField(TEXT("supply"), Platform.bSupply);
		Each->SetBoolField(TEXT("demand"), Platform.bDemand);
		Each->SetBoolField(TEXT("anyItem"), Platform.bAnyItem);
		Each->SetBoolField(TEXT("loadMode"), Platform.bLoadMode);
		Each->SetBoolField(TEXT("fluid"), Platform.bFluid);
		Each->SetNumberField(TEXT("slots"), Platform.Slots);
		Each->SetNumberField(TEXT("occupied"), Platform.Occupied);
		TArray<TSharedPtr<FJsonValue>> Items;
		for (UClass* Item : Platform.Items)
		{
			Items.Add(MakeShared<FJsonValueString>(GetPathNameSafe(Item)));
		}
		Each->SetArrayField(TEXT("items"), Items);
		TSharedRef<FJsonObject> Stock = MakeShared<FJsonObject>();
		for (const TPair<UClass*, int32>& Held : Platform.Stock)
		{
			Stock->SetNumberField(GetPathNameSafe(Held.Key), Held.Value);
		}
		Each->SetObjectField(TEXT("stock"), Stock);
		Platforms.Add(MakeShared<FJsonValueObject>(Each));
	}
	Train->SetArrayField(TEXT("platforms"), Platforms);
	TSharedRef<FJsonObject> Maps = MakeShared<FJsonObject>();
	for (const TPair<const AFGTrainStationIdentifier*, TArray<TArray<int32>>>& Pair : mCarPlatforms)
	{
		TArray<TSharedPtr<FJsonValue>> Ways;
		for (const TArray<int32>& Way : Pair.Value)
		{
			TArray<TSharedPtr<FJsonValue>> Cells;
			for (const int32 Index : Way)
			{
				Cells.Add(MakeShared<FJsonValueNumber>(Index));
			}
			Ways.Add(MakeShared<FJsonValueArray>(Cells));
		}
		Maps->SetArrayField(GetNameSafe(Pair.Key), Ways);
	}
	Train->SetObjectField(TEXT("carPlatforms"), Maps);
	return Train;
}

void FSTTripPlanner::RaiseBudget(double Milliseconds)
{
	mDeadline = FMath::Max(mDeadline, FPlatformTime::Seconds() + Milliseconds / 1000.0);
}

TArray<double> FSTTripPlanner::AllocateServiceCars(const TArray<FSTPlanStop>& Stops, const TArray<FFlowRef>& Flows) const
{
	TArray<double> PerTrain;
	PerTrain.Init(0.0, Flows.Num());
	// Per flow: its pickup and drop positions in the loop, and the cars that line up at both.
	struct FSpan
	{
		int32 Pick = INDEX_NONE;
		int32 Drop = INDEX_NONE;
		TArray<int32> Cars;
	};
	TArray<FSpan> Spans;
	for (const FFlowRef& Flow : Flows)
	{
		FSpan& Span = Spans.AddDefaulted_GetRef();
		Span.Pick = Stops.IndexOfByPredicate([&](const FSTPlanStop& Stop) { return Stop.Station == Flow.From; });
		Span.Drop = Stops.IndexOfByPredicate([&](const FSTPlanStop& Stop) { return Stop.Station == Flow.To; });
		if (Span.Pick == INDEX_NONE || Span.Drop == INDEX_NONE || Span.Pick >= Span.Drop || !Flow.Item)
		{
			continue;
		}
		UClass* Item = Flow.Item;
		for (int32 CarIndex = 0; CarIndex < mCars.Num(); ++CarIndex)
		{
			const AFGFreightWagon* Wagon = mCars[CarIndex].Car;
			if (Wagon && !STLayout::ItemFitsCargoType(Item, STLayout::GetCargoType(Wagon)))
			{
				continue;
			}
			if (CarFitsAt(CarIndex, Flow.From, [Item](const FPlatformInfo& Platform) { return Platform.bSupply && Platform.bLoadMode && Platform.Items.Contains(Item); })
				&& CarFitsAt(CarIndex, Flow.To, [Item](const FPlatformInfo& Platform) { return Platform.bDemand && !Platform.bLoadMode && (Platform.bAnyItem || Platform.Items.Contains(Item)); }))
			{
				Span.Cars.Add(CarIndex);
			}
		}
	}
	// Each car's busy stretches of the loop, [pick, drop].
	TArray<TArray<TPair<int32, int32>>> Busy;
	Busy.SetNum(mCars.Num());
	auto IsFree = [&](int32 CarIndex, const FSpan& Span)
	{
		return !Busy[CarIndex].ContainsByPredicate([&](const TPair<int32, int32>& Each) { return Span.Pick < Each.Value && Each.Key < Span.Drop; });
	};
	auto Give = [&](int32 FlowIndex, int32 CarIndex)
	{
		Busy[CarIndex].Add({Spans[FlowIndex].Pick, Spans[FlowIndex].Drop});
		PerTrain[FlowIndex] += STLayout::GetCarCapacity(mCars[CarIndex].Car, Flows[FlowIndex].Item) * (mCars[CarIndex].Slots > 0 && !mCars[CarIndex].Car
			? static_cast<double>(mCars[CarIndex].Slots) / 32.0 : 1.0);
	};
	// A car for every flow first, the flows with the fewest cars to choose from before the others.
	TArray<int32> Order;
	for (int32 Index = 0; Index < Flows.Num(); ++Index)
	{
		Order.Add(Index);
	}
	Order.Sort([&](int32 A, int32 B) { return Spans[A].Cars.Num() < Spans[B].Cars.Num(); });
	for (const int32 FlowIndex : Order)
	{
		for (const int32 CarIndex : Spans[FlowIndex].Cars)
		{
			if (IsFree(CarIndex, Spans[FlowIndex]))
			{
				Give(FlowIndex, CarIndex);
				break;
			}
		}
	}
	// Then the rest, each to the flow that needs the most trains per car it has.
	for (bool bGave = true; bGave;)
	{
		bGave = false;
		int32 BestFlow = INDEX_NONE;
		int32 BestCar = INDEX_NONE;
		double BestNeed = 0.0;
		for (int32 FlowIndex = 0; FlowIndex < Flows.Num(); ++FlowIndex)
		{
			const double Need = Flows[FlowIndex].Flow / FMath::Max(1.0, PerTrain[FlowIndex]);
			if (Need <= BestNeed)
			{
				continue;
			}
			for (const int32 CarIndex : Spans[FlowIndex].Cars)
			{
				if (IsFree(CarIndex, Spans[FlowIndex]))
				{
					BestFlow = FlowIndex;
					BestCar = CarIndex;
					BestNeed = Need;
					break;
				}
			}
		}
		if (BestFlow != INDEX_NONE)
		{
			Give(BestFlow, BestCar);
			bGave = true;
		}
	}
	return PerTrain;
}

bool FSTTripPlanner::IsBookedNear(const AFGTrainStationIdentifier* Station, double Time, double Window, TFunctionRef<bool(int32)> Filter) const
{
	const TArray<FSTBooking>* Bookings = mBookings.Find(Station);
	return Bookings && Bookings->ContainsByPredicate([&](const FSTBooking& Each)
	{
		return Each.Train != mTrainIndex && FMath::Abs(Each.From - Time) < Window && Filter(Each.Train);
	});
}

bool FSTTripPlanner::CarHasConsumer(int32 CarIndex, UClass* Item) const
{
	for (const FSTNode& Node : mNodes)
	{
		if (Node.Role != ESTRole::Demand || Node.bAnyItem || Node.Item.Get() != Item || !CanServeHere(Node.Station.Get()))
		{
			continue;
		}
		if (CarFitsAt(CarIndex, Node.Station.Get(), [Item](const FPlatformInfo& Platform) { return Platform.bDemand && !Platform.bLoadMode && !Platform.bAnyItem && Platform.Items.Contains(Item); }))
		{
			return true;
		}
	}
	return false;
}

bool FSTTripPlanner::ItemHasConsumer(UClass* Item) const
{
	return mNodes.ContainsByPredicate([&](const FSTNode& Node)
	{
		return Node.Role == ESTRole::Demand && !Node.bAnyItem && Node.Item.Get() == Item && CanServeHere(Node.Station.Get());
	});
}

double FSTTripPlanner::GetTiedShare(const TArray<TPair<UClass*, double>>& Cargo) const
{
	bool bAny = false;
	for (const TPair<UClass*, double>& Each : Cargo)
	{
		if (Each.Value >= 1.0)
		{
			if (!mNeededSoon.Contains(Each.Key))
			{
				return 1.0;
			}
			bAny = true;
		}
	}
	return bAny ? NeededSoonShare : 0.0;
}

double FSTTripPlanner::GetAvailable(int32 Node, double Arrive) const
{
	const FSTNode& Supplier = mNodes[Node];
	const double Until = Arrive + mSubsystem.EstimateStaySeconds(Supplier.Station.Get()) - mTime;
	const double Whole = FMath::Min<double>(Supplier.Capacity, Supplier.Stock + Supplier.Rate * Until) - Supplier.Reserved;
	if (Supplier.PlatformReserved.Num() == 0)
	{
		return Whole;
	}
	// Pickups promised at one platform don't use up another's stock: a train whose cars can't reach a platform
	// leaves it for trains whose cars can.
	double Sum = 0.0;
	bool bAny = false;
	for (const TWeakObjectPtr<AFGBuildableTrainPlatformCargo>& Each : Supplier.Platforms)
	{
		const int32 Info = GetPlatformInfo(Each.Get());
		if (Info != INDEX_NONE)
		{
			bAny = true;
			Sum += GetPlatformAvailable(Node, Info, Until);
		}
	}
	return bAny ? FMath::Min(Sum, static_cast<double>(Supplier.Capacity)) : Whole;
}

double FSTTripPlanner::GetPlatformRoom(int32 Node, int32 Info, UClass* Item, double Arrive) const
{
	const FPlatformInfo& Platform = mPlatforms[Info];
	const int32 Stack = STLayout::GetSlotSize(Platform.Inventory, Item);
	const int32 Stock = Platform.Stock.FindRef(Item);
	const int32 Rest = Stock % Stack;
	// Free slots; for a fluid also the rest of its one slot. A train doesn't top up part-full solid stacks.
	const double Free = FMath::Max(0, Platform.Slots - Platform.Occupied) * static_cast<double>(Stack) + (Platform.bFluid && Rest > 0 ? Stack - Rest : 0);
	if (Node == INDEX_NONE || mNodes[Node].bAnyItem)
	{
		return Free;
	}
	const FSTNode& Consumer = mNodes[Node];
	const int32 Count = FMath::Max(1, Consumer.Platforms.Num());
	const double Share = Consumer.Stock > 0 ? static_cast<double>(Stock) / Consumer.Stock : 1.0 / Count;
	// What its machines take out of it before the train arrives (no more than it holds); none from a platform that
	// lately took much less than planned, since its belts don't drain it.
	const double Used = mSubsystem.IsSlowDrain(Platform.Platform) ? 0.0
		: FMath::Min(static_cast<double>(Stock), Share * Consumer.Rate * FMath::Max(0.0, Arrive - mTime));
	int32 Assigned = 0;
	for (const TPair<const AFGBuildableTrainPlatformCargo*, int32>& Each : Consumer.PlatformReserved)
	{
		Assigned += Each.Value;
	}
	const double Unassigned = FMath::Max(0, Consumer.Reserved - Assigned) / static_cast<double>(Count);
	return FMath::Max(0.0, Free + Used - Consumer.PlatformReserved.FindRef(Platform.Platform) - Unassigned);
}

double FSTTripPlanner::GetPlatformAvailable(int32 Node, int32 Info, double Until) const
{
	const FSTNode& Supplier = mNodes[Node];
	const FPlatformInfo& Platform = mPlatforms[Info];
	const double Stock = Platform.Stock.FindRef(Supplier.Item.Get());
	const double Share = Supplier.Stock > 0 ? Stock / Supplier.Stock : 1.0 / FMath::Max(1, Supplier.Platforms.Num());
	int32 Assigned = 0;
	for (const TPair<const AFGBuildableTrainPlatformCargo*, int32>& Each : Supplier.PlatformReserved)
	{
		Assigned += Each.Value;
	}
	const double Unassigned = FMath::Max(0, Supplier.Reserved - Assigned);
	return FMath::Max(0.0, Stock + Share * (Supplier.Rate * Until - Unassigned) - Supplier.PlatformReserved.FindRef(Platform.Platform));
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

bool FSTTripPlanner::IsNoProgressHere(const AFGTrainStationIdentifier* Station, UClass* Item, bool bLoad) const
{
	return mSubsystem.mTrains.IsValidIndex(mTrainIndex) && mSubsystem.IsNoProgress(mSubsystem.mTrains[mTrainIndex], Station, Item, bLoad);
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

bool FSTTripPlanner::HoldsOtherItem(const TArray<TPair<UClass*, double>>& Cargo, UClass* Item)
{
	return Cargo.ContainsByPredicate([Item](const TPair<UClass*, double>& Each) { return Each.Key != Item && Each.Value >= 1.0; });
}

double FSTTripPlanner::GetCarRoom(const FCarInfo& Car, const TArray<TPair<UClass*, double>>& Cargo, UClass* Item) const
{
	const bool bFluid = STNames::IsFluid(Item);
	if (bFluid)
	{
		// A car carries solids or one fluid, never both.
		for (const TPair<UClass*, double>& Each : Cargo)
		{
			if (Each.Key != Item)
			{
				return 0.0;
			}
		}
		return FMath::Max(0.0, STLayout::GetCarCapacity(Car.Car, Item) - CargoOf(Cargo, Item));
	}
	int32 Used = 0;
	double Partial = 0.0;
	for (const TPair<UClass*, double>& Each : Cargo)
	{
		if (STNames::IsFluid(Each.Key))
		{
			return 0.0;
		}
		const double Stack = STLayout::GetCarSlotAmount(Car.Car, Each.Key);
		Used += FMath::CeilToInt(Each.Value / Stack);
		if (Each.Key == Item)
		{
			const double Rest = FMath::Fmod(Each.Value, Stack);
			Partial = Rest > 0.0 ? Stack - Rest : 0.0;
		}
	}
	return FMath::Max(0, Car.Slots - Used) * STLayout::GetCarSlotAmount(Car.Car, Item) + Partial;
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
		Info.bDemand = STNames::Unloads(Config->Role);
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

bool FSTTripPlanner::CarFitsAt(int32 Index, const AFGTrainStationIdentifier* Station, TFunctionRef<bool(const FPlatformInfo&)> Test) const
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
}

void FSTTripPlanner::BuildMoves()
{
	mMoves.Reset();
	auto CarFits = [this](int32 Index, const AFGTrainStationIdentifier* Station, TFunctionRef<bool(const FPlatformInfo&)> Test)
	{
		return CarFitsAt(Index, Station, Test);
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
				if (Consumer.Role != ESTRole::Demand || (Consumer.Item.Get() != Carried.Key && !Consumer.bAnyItem) || !CanServeHere(Consumer.Station.Get()))
				{
					continue;
				}
				// A sink only takes what this car can't unload at any consumer of the item.
				if (Consumer.bAnyItem && !bSwitching && CarHasConsumer(CarIndex, Carried.Key))
				{
					continue;
				}
				if (IsNoProgressHere(Consumer.Station.Get(), Carried.Key, false))
				{
					UE_LOG(LogSmartTrains, Verbose, TEXT("PLANNER %s: no %s to %s for now (it unloaded none there lately)"), *TrainName(), *STNames::Item(Carried.Key),
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
		if (Supplier.Role != ESTRole::Supply || !Supplier.Item || !CanServeHere(Supplier.Station.Get()))
		{
			continue;
		}
		const float ToSupplier = mSubsystem.EstimateTravelSeconds(mAnchor, Supplier.Station.Get(), bPushPull);
		if (ToSupplier < 0.f || GetAvailable(From, mFreeAt + ToSupplier) <= 0.0)
		{
			continue;
		}
		UClass* Item = Supplier.Item.Get();
		if (IsNoProgressHere(Supplier.Station.Get(), Item, true))
		{
			UE_LOG(LogSmartTrains, Verbose, TEXT("PLANNER %s: no %s from %s for now (it loaded none there lately)"), *TrainName(), *STNames::Item(Item),
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
				|| !CanServeHere(Consumer.Station.Get()))
			{
				continue;
			}
			// A full supplier's goods go to a sink only if nothing on the network consumes them (overflow).
			if (Consumer.bAnyItem && (!bFull || ItemHasConsumer(Item)))
			{
				continue;
			}
			if (IsNoProgressHere(Consumer.Station.Get(), Item, false))
			{
				continue;
			}
			if (!Consumer.bAnyItem && !bFull)
			{
				// Only consumers that fall below their target within the forecast horizon.
				const float Leg = mSubsystem.EstimateTravelSeconds(Supplier.Station.Get(), Consumer.Station.Get(), bPushPull);
				const double CarCapacity = STLayout::GetCarCapacity(mCars[0].Car, Item);
				if (Leg < 0.f || STForecast::Deadline(Consumer, Consumer.Planned, mTime, CarCapacity) >= mTime + STForecast::HorizonSeconds)
				{
					continue;
				}
			}
			// A shuttle's route is its own, but for a full supplier.
			if (!bFull && TrainNow() && mSubsystem.IsShuttleRoute(TrainNow(), Supplier.Station.Get(), Consumer.Station.Get(), Item))
			{
				continue;
			}
			const bool bAnyCar = SupplyCars.ContainsByPredicate([&](int32 CarIndex) { return CarFits(CarIndex, Consumer.Station.Get(), Demands(Item)); });
			if (bAnyCar)
			{
				Moves.AddUnique({From, To, Item});
			}
		}
	}
	// Solo scores, then keep the best few (cargo on board always). With Detailed log: why deliveries to High and
	// Critical consumers score nothing by themselves.
	const bool bDetail = UE_LOG_ACTIVE(LogSmartTrains, Verbose);
	auto MoveName = [this](const FSTPlanMove& Move)
	{
		return FString::Printf(TEXT("%s %s -> %s"), *STNames::Item(Move.Item), Move.From == INDEX_NONE ? TEXT("on board") : *STLayout::Name(mNodes[Move.From].Station.Get()),
			*STLayout::Name(mNodes[Move.To].Station.Get()));
	};
	TArray<FString> Unscored;
	for (FSTPlanMove& Move : Moves)
	{
		TArray<FSTPlanStop> Stops;
		FSTPlanResult Result;
		double Second = 0.0;
		const TMap<FString, int32> Before = bDetail ? mProblems : TMap<FString, int32>();
		Move.SoloScore = InsertBest(Stops, Move, 0, Result, Second) ? Result.Score : 0.0;
		if (bDetail && Move.SoloScore <= 0.0 && static_cast<uint8>(mNodes[Move.To].Priority) >= static_cast<uint8>(ESTPriority::High))
		{
			FString Why = TEXT("no value");
			for (const TPair<FString, int32>& Each : mProblems)
			{
				if (Each.Value > Before.FindRef(Each.Key))
				{
					Why = Each.Key;
					break;
				}
			}
			Unscored.Add(FString::Printf(TEXT("%s (%s)"), *MoveName(Move), *Why));
		}
	}
	Moves.Sort([](const FSTPlanMove& A, const FSTPlanMove& B) { return A.SoloScore > B.SoloScore; });
	if (bDetail && Moves.Num() > 0)
	{
		TArray<FString> Best;
		for (int32 Index = 0; Index < Moves.Num() && Index < 3; ++Index)
		{
			Best.Add(FString::Printf(TEXT("%s %.3f"), *MoveName(Moves[Index]), Moves[Index].SoloScore));
		}
		UE_LOG(LogSmartTrains, Verbose, TEXT("PLANNER %s: best moves alone: %s%s%s"), *TrainName(), *FString::Join(Best, TEXT("; ")),
			Unscored.Num() > 0 ? TEXT("; scoring nothing alone: ") : TEXT(""), *FString::Join(TArray<FString>(Unscored.GetData(), FMath::Min(4, Unscored.Num())), TEXT("; ")));
	}
	int32 Kept = 0;
	for (const FSTPlanMove& Move : Moves)
	{
		if (Move.From == INDEX_NONE || Kept++ < MaxMovesPerTrain)
		{
			mMoves.Add(Move);
		}
	}
	UE_LOG(LogSmartTrains, Verbose, TEXT("PLANNER %s: %d moves (%d kept), %d cars, from %s at %+.0f s%s"), *TrainName(), Moves.Num(), mMoves.Num(),
		mCars.Num(), *STLayout::Name(mAnchor), mFreeAt - mTime, bRelaxed ? TEXT(", unwanted cargo may go anywhere") : TEXT(""));
}

const AFGTrain* FSTTripPlanner::TrainNow() const
{
	return mSubsystem.mTrains.IsValidIndex(mTrainIndex) ? mSubsystem.mTrains[mTrainIndex].Train.Get() : nullptr;
}

FString FSTTripPlanner::TrainName() const
{
	const AFGTrain* Train = TrainNow();
	return Train ? STLayout::Name(Train) : FString(TEXT("replayed train"));
}

bool FSTTripPlanner::PrepareCars(int32 TrainIndex, const AFGTrainStationIdentifier* Anchor, double FreeAt, bool bInAnchorDocked)
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
		Car.Slots = STLayout::GetWagonSlots(Wagon);
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
	mTrainLength = STLayout::GetTrainLength(Train);

	return true;
}

bool FSTTripPlanner::BeginTrain(int32 TrainIndex, const AFGTrainStationIdentifier* Anchor, double FreeAt, bool bInAnchorDocked)
{
	if (!PrepareCars(TrainIndex, Anchor, FreeAt, bInAnchorDocked))
	{
		return false;
	}
	BuildMoves();
	FTrainSetup& Setup = mSetups.FindOrAdd(TrainIndex);
	Setup.Anchor = Anchor;
	Setup.FreeAt = FreeAt;
	Setup.bAnchorDocked = bAnchorDocked;
	Setup.bRelaxed = bRelaxed;
	Setup.TrainLength = mTrainLength;
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
	mTrainLength = Setup->TrainLength;
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
	TMap<TPair<int32, UClass*>, double> Caps;
	FSTPlanResult Result;
	for (int32 Pass = 0; Pass <= mCars.Num(); ++Pass)
	{
		const int32 Before = Skips.Num();
		Result = SimulateWithSkips(Stops, Skips, Caps);
		if (Result.bValid || Skips.Num() == Before)
		{
			break;
		}
	}
	// A car loads only what the trip unloads again: what it would still hold of an item loaded on this trip is taken off
	// its pickups, and the trip simulated once more (leftovers tie cars and end up in sinks).
	if (Result.bValid && Caps.Num() == 0)
	{
		for (int32 CarIndex = 0; CarIndex < mCars.Num(); ++CarIndex)
		{
			TMap<UClass*, double> Loaded;
			TMap<UClass*, double> Unloaded;
			for (const TArray<FSTPlanCarStep>& Step : Result.Steps)
			{
				if (!Step.IsValidIndex(CarIndex))
				{
					continue;
				}
				for (const TPair<UClass*, int32>& Each : Step[CarIndex].Loaded)
				{
					Loaded.FindOrAdd(Each.Key) += Each.Value;
				}
				for (const TPair<UClass*, int32>& Each : Step[CarIndex].Unloaded)
				{
					Unloaded.FindOrAdd(Each.Key) += Each.Value;
				}
			}
			for (const TPair<UClass*, double>& Each : Loaded)
			{
				const double Before = CargoOf(mCars[CarIndex].Cargo, Each.Key);
				const double Left = Before + Each.Value - Unloaded.FindRef(Each.Key);
				const double Extra = FMath::Min(Each.Value, Left - Before);
				if (Extra >= 1.0)
				{
					Caps.Add(TPair<int32, UClass*>(CarIndex, Each.Key), FMath::Max(0.0, Each.Value - Extra));
				}
			}
		}
		if (Caps.Num() > 0)
		{
			Skips.Reset();
			for (int32 Pass = 0; Pass <= mCars.Num(); ++Pass)
			{
				const int32 Before = Skips.Num();
				Result = SimulateWithSkips(Stops, Skips, Caps);
				if (Result.bValid || Skips.Num() == Before)
				{
					break;
				}
			}
		}
	}
	return Result;
}

FSTPlanResult FSTTripPlanner::SimulateWithSkips(const TArray<FSTPlanStop>& Stops, TSet<TPair<int32, int32>>& InOutSkips, const TMap<TPair<int32, UClass*>, double>& Caps) const
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

	// Times, and what the stops cost: each one holds its station from other trains.
	double StopCost = 0.0;
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
		// Leaving at once, a train that has to wait for a booked station waits on the line before it: so do all the trains
		// still there or not yet docked when it gets there, but the one docked (the waiting time itself only lowers the
		// score, since it counts as trip time).
		const FSTSettings& Settings = mSubsystem.mSettings;
		int32 Limit = Settings.QueueTrains > 0.f ? FMath::RoundToInt(Settings.QueueTrains) : MAX_int32;
		const int32 Room = mSubsystem.GetWaitingRoom(Station, mTrainLength);
		if (Room >= 0)
		{
			Limit = FMath::Min(Limit, Room);
		}
		if (Settings.bLeaveAtOnce && Limit < MAX_int32 && Result.Arrive[Index] > Clock + Travel && Clock + Travel - mTime <= QueueLookSeconds)
		{
			const int32 Ahead = CountAhead(Station, Clock + Travel, Result.Arrive[Index]);
			if (Ahead > Limit)
			{
				return Fail(Room >= 0 && Room == Limit
					? FString::Printf(TEXT("would wait before %s behind %d trains (room for %d before its switch)"), *STLayout::Name(Station), Ahead, Room)
					: FString::Printf(TEXT("would queue at %s behind %d trains (most %d waiting)"), *STLayout::Name(Station), Ahead, Limit));
			}
		}
		Clock = Result.Arrive[Index] + mSubsystem.EstimateStaySeconds(Station);
		StopCost += StopCostValue * (1 + CountNear(Station, Result.Arrive[Index] - StopNearSeconds, Clock + StopNearSeconds));
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
	// What this trip delivers to or picks up from each node, in order: later stops are valued on top of earlier ones.
	TMap<int32, TArray<FSTNodeEvent>> TripEvents;
	// The drop in Node's forecast cost when Amount of Item is delivered or picked up at Time (Docs/Architecture.md,
	// "Forecast cost").
	auto ForecastGain = [&](int32 Node, UClass* Item, double Time, double Amount)
	{
		TArray<FSTNodeEvent>& Mine = TripEvents.FindOrAdd(Node);
		TArray<FSTNodeEvent> Events = mNodes[Node].Planned;
		Events.Append(Mine);
		const FSTNodeEvent Added{Time, Amount};
		const double Gain = STForecast::Gain(mNodes[Node], Events, Added, mTime, STLayout::GetCarCapacity(mCars.Num() > 0 ? mCars[0].Car : nullptr, Item));
		Mine.Add(Added);
		return Gain;
	};
	TArray<TPair<TPair<int32, UClass*>, double>> PlatformPool;
	TArray<TPair<TPair<int32, UClass*>, double>> PlatformRoom;
	TArray<TPair<int32, double>> NodeSupply;
	// Per item: total loaded, the room of the cars loading it, the strictest send-at share, full supplier, critical consumer.
	struct FItemTotals
	{
		UClass* Item;
		double LoadedAmount = 0.0;
		double LoadRoom = 0.0;
		double Share = 0.0;
		bool bFromFull = false;
		bool bCritical = false;
		// Loaded for a request: no minimum batch.
		bool bNoBatch = false;
	};
	TArray<FItemTotals> Totals;
	// Loaded so far against each cap.
	TMap<TPair<int32, UClass*>, double> CapTaken;
	auto SlotAmount = [this](UClass* Item)
	{
		return STLayout::GetCarSlotAmount(mCars.Num() > 0 ? mCars[0].Car : nullptr, Item);
	};
	// Share of a car Amount of Item fills: a delivery's value counts only how full the car is.
	auto CarShare = [this](UClass* Item, double Amount)
	{
		return Amount / FMath::Max(1.0, STLayout::GetCarCapacity(mCars.Num() > 0 ? mCars[0].Car : nullptr, Item));
	};
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
						// Cars only come out mixed from one platform with several items: a car holding another item never takes
						// on a new one, and a configured platform only gives its own items.
						bool bWouldMix = false;
						for (UClass* Item : Stop.LoadItems)
						{
							if (IsNoProgressHere(Stop.Station, Item, true))
							{
								continue;
							}
							const bool bOffers = Platform.bConfigured ? Platform.Items.Contains(Item) : Platform.Stock.Contains(Item);
							if (bOffers && STNames::IsFluid(Item) == Platform.bFluid && GetCarRoom(mCars[CarIndex], Cargo[CarIndex], Item) > 0.0)
							{
								if (HoldsOtherItem(Cargo[CarIndex], Item))
								{
									bWouldMix = true;
									continue;
								}
								ThisItems.Add(Item);
							}
						}
						ThisEffect = bWouldMix ? EEffect::Harm : (ThisItems.Num() == 0 ? EEffect::None : (Platform.bSupply ? EEffect::Load : EEffect::Harm));
					}
					else
					{
						for (UClass* Item : Stop.UnloadItems)
						{
							if (IsNoProgressHere(Stop.Station, Item, false))
							{
								continue;
							}
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
						// An unloading platform takes everything the car holds, planned here or not.
						const bool bOthers = Cargo[CarIndex].ContainsByPredicate([&](const TPair<UClass*, double>& Each)
						{
							return Each.Value >= 1.0 && STNames::IsFluid(Each.Key) == Platform.bFluid && !ThisItems.Contains(Each.Key);
						});
						ThisEffect = bOthers ? EEffect::Harm : (ThisItems.Num() == 0 ? EEffect::None : (bTakesAll ? EEffect::Unload : EEffect::Harm));
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
						if (Node == INDEX_NONE)
						{
							return static_cast<double>(mPlatforms[PlatformIndex].Stock.FindRef(Item));
						}
						// What arrives up to the end of the stay, since the platform keeps filling while the train loads.
						const double Until = Arrive + mSubsystem.EstimateStaySeconds(Stop.Station) - mTime;
						return GetPlatformAvailable(Node, PlatformIndex, Until);
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
					double Amount = FMath::FloorToDouble(FMath::Min3(Room, Pool, FMath::Max(0.0, NodeLeft)));
					if (const double* Cap = Caps.Find(TPair<int32, UClass*>(CarIndex, Item)))
					{
						double& Taken = CapTaken.FindOrAdd(TPair<int32, UClass*>(CarIndex, Item));
						Amount = FMath::Min(Amount, FMath::FloorToDouble(FMath::Max(0.0, *Cap - Taken)));
						Taken += FMath::Max(0.0, Amount);
					}
					FItemTotals& Total = TotalsFor(Item);
					const bool bFirst = !Loaded.ContainsByPredicate([&](const FLoaded& Each) { return Each.Car == CarIndex && Each.Item == Item; });
					if (bFirst)
					{
						Total.LoadRoom += Room;
						Loaded.Add({CarIndex, Item, StopIndex, false});
					}
					if (Node != INDEX_NONE)
					{
						Total.Share = FMath::Max(Total.Share, STNames::SendAtPercent(mNodes[Node].SendAtPercent) / 100.0);
						Total.bFromFull |= IsSupplyFull(Node);
					}
					Total.bNoBatch |= Stop.NoBatchItems.Contains(Item);
					if (Amount <= 0.0 || Amount < MinLoadShare * GetCarRoom(mCars[CarIndex], {}, Item))
					{
						continue;
					}
					AddCargo(Cargo[CarIndex], Item, Amount);
					Step.Platform = Beside.Num() > 0 ? mPlatforms[Beside[0]].Platform : nullptr;
					for (double* Each : Pools)
					{
						*Each -= Amount;
					}
					NodeLeft -= Amount;
					Total.LoadedAmount += Amount;
					Step.Loaded.Emplace(Item, static_cast<int32>(Amount));
					Result.Moved += static_cast<int32>(Amount);
					if (Node != INDEX_NONE)
					{
						Result.Value += ForecastGain(Node, Item, Arrive, Amount);
					}
				}
			}
			else if (Effect == EEffect::Unload)
			{
				for (UClass* Item : Items)
				{
					const double Carried = CargoOf(Cargo[CarIndex], Item);
					const int32 RoomNode = FindNode(Stop.Station, Item, false);
					auto InitialSpace = [&](int32 PlatformIndex)
					{
						return GetPlatformRoom(RoomNode, PlatformIndex, Item, Arrive);
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
					// No small top-ups: the stay ends at once if the platform has no room the moment the train docks.
					const double Possible = FMath::FloorToDouble(FMath::Min(Carried, Space));
					const double Amount = Possible + 0.5 >= FMath::Min(Carried, MinUnloadSlots * SlotAmount(Item)) ? Possible : 0.0;
					for (FLoaded& Each : Loaded)
					{
						Each.bDelivered |= Each.Car == CarIndex && Each.Item == Item;
					}
					if (Amount <= 0.0)
					{
						continue;
					}
					AddCargo(Cargo[CarIndex], Item, -Amount);
					Step.Platform = Beside.Num() > 0 ? mPlatforms[Beside[0]].Platform : nullptr;
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
					if (mNodes[Node].bAnyItem && bSwitching)
					{
						// Dumped to switch services sooner: the cargo's value is lost, little for goods no consumer needs soon,
						// a lot for goods one does (so those are delivered).
						Result.Value -= CarShare(Item, Amount) * (mNeededSoon.Contains(Item) ? NeededDumpValue : SurplusDumpValue);
						continue;
					}
					if (mNodes[Node].bAnyItem && CarHasConsumer(CarIndex, Item))
					{
						return Fail(FString::Printf(TEXT("car %d would unload %s into the sink at %s, but it can deliver it to a consumer"), CarIndex + 1,
							*STNames::Item(Item), *STLayout::Name(Stop.Station)));
					}
					FItemTotals& Total = TotalsFor(Item);
					if (mNodes[Node].bAnyItem)
					{
						continue;
					}
					Total.bCritical |= mNodes[Node].Priority == ESTPriority::Critical;
					const double Gain = ForecastGain(Node, Item, Arrive, Amount);
					Result.Value += Gain;
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
		if (Total.LoadRoom > 0.0 && !Total.bCritical && !Total.bFromFull && !Total.bNoBatch && Total.LoadedAmount + 0.5 < Total.Share * Total.LoadRoom)
		{
			return Fail(FString::Printf(TEXT("less than a batch of %s"), *STNames::Item(Total.Item)));
		}
	}
	// Cars left holding cargo can only take that item next; cars emptied can take any.
	for (int32 CarIndex = 0; CarIndex < mCars.Num(); ++CarIndex)
	{
		Result.Value += TiedCarValue * (GetTiedShare(mCars[CarIndex].Cargo) - GetTiedShare(Cargo[CarIndex]));
	}
	Result.Value -= StopCost;
	Result.bValid = true;
	// One objective: what the trip is worth less what the train's time costs.
	Result.Score = Result.Value - STForecast::TrainSecondCost * FMath::Max(0.0, Result.EndTime - mFreeAt);
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
			if (Used.Contains(Index) || (bDeliveriesOnly && mMoves[Index].From != INDEX_NONE))
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

int32 FSTTripPlanner::CountUnloaded(const TArray<FSTPlanStop>& Stops, const FSTPlanResult& Result, UClass* Item, const AFGTrainStationIdentifier* Station)
{
	int32 Total = 0;
	for (int32 StopIndex = 0; StopIndex < Stops.Num() && StopIndex < Result.Steps.Num(); ++StopIndex)
	{
		if (Station && Stops[StopIndex].Station != Station)
		{
			continue;
		}
		for (const FSTPlanCarStep& Step : Result.Steps[StopIndex])
		{
			for (const TPair<UClass*, int32>& Each : Step.Unloaded)
			{
				Total += Each.Key == Item ? Each.Value : 0;
			}
		}
	}
	return Total;
}

int32 FSTTripPlanner::CountUnloadedInto(const FSTPlanResult& Result, UClass* Item, const AFGBuildableTrainPlatformCargo* Platform, int32* OutStop)
{
	int32 Total = 0;
	for (int32 StopIndex = 0; StopIndex < Result.Steps.Num(); ++StopIndex)
	{
		for (const FSTPlanCarStep& Step : Result.Steps[StopIndex])
		{
			if (Step.Platform != Platform)
			{
				continue;
			}
			for (const TPair<UClass*, int32>& Each : Step.Unloaded)
			{
				if (Each.Key == Item && Each.Value > 0)
				{
					Total += Each.Value;
					if (OutStop && *OutStop == INDEX_NONE)
					{
						*OutStop = StopIndex;
					}
				}
			}
		}
	}
	return Total;
}

bool FSTTripPlanner::KeepsRequestCars(const TArray<FSTPlanStop>& Stops, const FSTPlanResult& Result, const TArray<int32>& Cars, UClass* Item) const
{
	for (int32 StopIndex = 0; StopIndex < Stops.Num() && StopIndex < Result.Steps.Num(); ++StopIndex)
	{
		for (const int32 CarIndex : Cars)
		{
			if (!Result.Steps[StopIndex].IsValidIndex(CarIndex))
			{
				continue;
			}
			for (const TPair<UClass*, int32>& Each : Result.Steps[StopIndex][CarIndex].Loaded)
			{
				if (Each.Value > 0 && (Each.Key != Item || !Stops[StopIndex].NoBatchItems.Contains(Item)))
				{
					return false;
				}
			}
		}
	}
	return true;
}

bool FSTTripPlanner::CanServeHere(const AFGTrainStationIdentifier* Station) const
{
	return bServeAll || (mSubsystem.mTrains.IsValidIndex(mTrainIndex) && mSubsystem.CanServe(mSubsystem.mTrains[mTrainIndex], Station));
}

int32 FSTTripPlanner::FindCar(const AFGRailroadVehicle* Car) const
{
	return mCars.IndexOfByPredicate([Car](const FCarInfo& Each) { return Each.Car == Car; });
}

AFGFreightWagon* FSTTripPlanner::GetCar(int32 Index) const
{
	return mCars.IsValidIndex(Index) ? mCars[Index].Car : nullptr;
}

TArray<FSTTripPlanner::FRequestSource> FSTTripPlanner::FindRequestSources(const FSTRequestTarget& Target, const TArray<int32>& Taken, FString& OutWhy) const
{
	TArray<FRequestSource> Sources;
	const FSTNode& Consumer = mNodes[Target.Node];
	UClass* Item = Consumer.Item.Get();
	AFGTrainStationIdentifier* Drop = Consumer.Station.Get();
	if (!Item || !Drop || !Target.Platform || !CanServeHere(Drop))
	{
		OutWhy = TEXT("it can't serve the platform's station");
		return Sources;
	}
	if (IsNoProgressHere(Drop, Item, false))
	{
		OutWhy = TEXT("it unloaded none there lately");
		return Sources;
	}
	const AFGBuildableTrainPlatformCargo* Wanted = Target.Platform;
	auto IntoTarget = [Wanted, Item](const FPlatformInfo& Platform)
	{
		return Platform.Platform == Wanted && Platform.bDemand && !Platform.bLoadMode && (Platform.bAnyItem || Platform.Items.Contains(Item));
	};
	for (int32 From = 0; From < mNodes.Num(); ++From)
	{
		const FSTNode& Supplier = mNodes[From];
		AFGTrainStationIdentifier* Pick = Supplier.Station.Get();
		if (Supplier.Role != ESTRole::Supply || Supplier.Item.Get() != Item || !Pick || Pick == Drop || !CanServeHere(Pick) || IsNoProgressHere(Pick, Item, true))
		{
			continue;
		}
		const float ToPick = mAnchor == Pick ? 0.f : mSubsystem.EstimateTravelSeconds(mAnchor, Pick, bPushPull);
		const float ToDrop = mSubsystem.EstimateTravelSeconds(Pick, Drop, bPushPull);
		if (ToPick < 0.f || ToDrop < 0.f || GetAvailable(From, mFreeAt + ToPick) <= 0.0)
		{
			continue;
		}
		FRequestSource Source;
		Source.Station = Pick;
		Source.Seconds = ToPick + ToDrop;
		for (int32 CarIndex = 0; CarIndex < mCars.Num(); ++CarIndex)
		{
			if (!Taken.Contains(CarIndex)
				&& CarFitsAt(CarIndex, Pick, [Item](const FPlatformInfo& Platform) { return Platform.bSupply && Platform.bLoadMode && Platform.Items.Contains(Item); })
				&& CarFitsAt(CarIndex, Drop, IntoTarget))
			{
				Source.Cars.Add(CarIndex);
			}
		}
		if (Source.Cars.Num() > 0)
		{
			Sources.Add(MoveTemp(Source));
		}
	}
	if (Sources.Num() == 0)
	{
		OutWhy = FString::Printf(TEXT("no supplier of %s with stock where one of its cars stops beside the loader and, at %s, beside the platform"), *STNames::Item(Item),
			*STLayout::Name(Drop));
	}
	Sources.Sort([](const FRequestSource& A, const FRequestSource& B) { return A.Seconds < B.Seconds; });
	Sources.SetNum(FMath::Min(Sources.Num(), MaxRequestSources));
	return Sources;
}

bool FSTTripPlanner::PlanRequest(const FSTRequestTarget& Target, TArray<FSTPlanStop>& OutStops, FSTPlanResult& OutResult, bool& bOutDelivers, TArray<int32>& OutCars,
	FString& OutWhy) const
{
	OutStops.Reset();
	OutResult = FSTPlanResult();
	bOutDelivers = false;
	OutCars.Reset();
	if (!mNodes.IsValidIndex(Target.Node) || !mSubsystem.mTrains.IsValidIndex(mTrainIndex))
	{
		OutWhy = TEXT("no train set up");
		return false;
	}
	const TArray<FRequestSource> Sources = FindRequestSources(Target, {}, OutWhy);
	if (Sources.Num() == 0)
	{
		return false;
	}
	UClass* Item = mNodes[Target.Node].Item.Get();
	AFGTrainStationIdentifier* Drop = mNodes[Target.Node].Station.Get();
	auto DemandsItem = [](UClass* Wanted)
	{
		return [Wanted](const FPlatformInfo& Platform) { return Platform.bDemand && !Platform.bLoadMode && (Platform.bAnyItem || Platform.Items.Contains(Wanted)); };
	};

	// A place to empty cars at: the items it takes from them, and whether it is a sink.
	struct FDump
	{
		AFGTrainStationIdentifier* Station = nullptr;
		TArray<UClass*> Items;
		bool bSink = false;
		float Seconds = 0.f;
	};
	// A trip and how it empties the cars: whether they end up empty and how much is unloaded (Key: delivery arrival,
	// or the end of a trip that only empties).
	struct FBest
	{
		TArray<FSTPlanStop> Stops;
		FSTPlanResult Result;
		TArray<int32> Cars;
		bool bDrained = false;
		int32 Cleared = 0;
		double Key = TNumericLimits<double>::Max();
	};
	FBest Full;
	FBest Partial;
	// Whatever empties the cars soonest: emptied before partly emptied, then the earlier Key (a partly emptied one by
	// more unloaded first). Consumers and sinks count the same; only the time does.
	auto IsBetter = [](const FBest& Best, const FBest& New)
	{
		if (Best.Stops.Num() == 0)
		{
			return true;
		}
		if (New.bDrained != Best.bDrained)
		{
			return New.bDrained;
		}
		if (!New.bDrained && New.Cleared != Best.Cleared)
		{
			return New.Cleared > Best.Cleared;
		}
		return New.Key < Best.Key;
	};
	TMap<FString, int32> Rejected;
	for (const FRequestSource& Source : Sources)
	{
		// What the cars that fit both ends hold of other items.
		TArray<UClass*> Blocking;
		TArray<int32> Blocked;
		int32 BlockingAmount = 0;
		for (const int32 CarIndex : Source.Cars)
		{
			for (const TPair<UClass*, double>& Carried : mCars[CarIndex].Cargo)
			{
				if (Carried.Key != Item && Carried.Value >= 1.0)
				{
					Blocking.AddUnique(Carried.Key);
					Blocked.AddUnique(CarIndex);
					BlockingAmount += FMath::FloorToInt(Carried.Value);
				}
			}
		}
		TArray<FDump> Dumps;
		if (Blocking.Num() > 0)
		{
			for (int32 To = 0; To < mNodes.Num(); ++To)
			{
				const FSTNode& Node = mNodes[To];
				AFGTrainStationIdentifier* Station = Node.Station.Get();
				if (Node.Role != ESTRole::Demand || !Station || Station == Drop || Station == Source.Station || !CanServeHere(Station))
				{
					continue;
				}
				const float Seconds = mAnchor == Station ? 0.f : mSubsystem.EstimateTravelSeconds(mAnchor, Station, bPushPull);
				if (Seconds < 0.f)
				{
					continue;
				}
				TArray<UClass*> Takes;
				for (UClass* Other : Blocking)
				{
					if ((Node.bAnyItem || Node.Item.Get() == Other) && !IsNoProgressHere(Station, Other, false)
						&& Blocked.ContainsByPredicate([&](int32 CarIndex) { return CarFitsAt(CarIndex, Station, DemandsItem(Other)); }))
					{
						Takes.Add(Other);
					}
				}
				if (Takes.Num() == 0)
				{
					continue;
				}
				FDump* Same = Dumps.FindByPredicate([&](const FDump& Each) { return Each.Station == Station; });
				if (!Same)
				{
					Same = &Dumps.AddDefaulted_GetRef();
					Same->Station = Station;
					Same->Seconds = Seconds;
				}
				for (UClass* Each : Takes)
				{
					Same->Items.AddUnique(Each);
				}
				Same->bSink |= Node.bAnyItem;
			}
			// The nearest consumers and the nearest sinks, so a sink that takes it all is always among them.
			Dumps.Sort([](const FDump& A, const FDump& B) { return A.bSink != B.bSink ? !A.bSink : A.Seconds < B.Seconds; });
			const int32 FirstSink = Dumps.IndexOfByPredicate([](const FDump& Each) { return Each.bSink; });
			const int32 SinksFrom = FirstSink == INDEX_NONE ? Dumps.Num() : FirstSink;
			TArray<FDump> Kept;
			for (int32 Index = 0; Index < Dumps.Num(); ++Index)
			{
				if (Index < SinksFrom ? Index < MaxRequestConsumers : Index - SinksFrom < MaxRequestSinks)
				{
					Kept.Add(Dumps[Index]);
				}
			}
			Dumps = MoveTemp(Kept);
			if (Dumps.Num() == 0)
			{
				Rejected.FindOrAdd(FString::Printf(TEXT("nowhere to unload the %s its cars hold"), *STNames::Item(Blocking[0])))++;
				continue;
			}
		}
		// Places to empty cars at, in order: none, one, or two (both orders).
		TArray<TArray<int32>> Sequences;
		if (Blocking.Num() == 0)
		{
			Sequences.Add({});
		}
		for (int32 First = 0; First < Dumps.Num(); ++First)
		{
			Sequences.Add({First});
		}
		for (int32 First = 0; First < Dumps.Num(); ++First)
		{
			for (int32 Second = 0; Second < Dumps.Num(); ++Second)
			{
				if (First != Second)
				{
					Sequences.Add({First, Second});
				}
			}
		}
		for (const TArray<int32>& Sequence : Sequences)
		{
			if (IsOverBudget())
			{
				break;
			}
			TArray<FSTPlanStop> Emptying;
			for (const int32 Index : Sequence)
			{
				FSTPlanStop& Stop = Emptying.AddDefaulted_GetRef();
				Stop.Station = Dumps[Index].Station;
				Stop.UnloadItems = Dumps[Index].Items;
			}
			// Emptying, then the pickup (no minimum batch) and the delivery.
			if (Emptying.Num() + 2 <= mMaxStops)
			{
				TArray<FSTPlanStop> Stops = Emptying;
				FSTPlanStop& Pick = Stops.AddDefaulted_GetRef();
				Pick.Station = Source.Station;
				Pick.LoadItems.Add(Item);
				Pick.NoBatchItems.Add(Item);
				FSTPlanStop& Deliver = Stops.AddDefaulted_GetRef();
				Deliver.Station = Drop;
				Deliver.UnloadItems.Add(Item);
				FSTPlanResult Result = Simulate(Stops);
				int32 Into = INDEX_NONE;
				if (!Result.bValid)
				{
					Rejected.FindOrAdd(Result.Problem)++;
				}
				else if (CountUnloadedInto(Result, Item, Target.Platform, &Into) <= 0)
				{
					Rejected.FindOrAdd(FString::Printf(TEXT("its car beside the platform would still hold other cargo at %s"), *STLayout::Name(Source.Station)))++;
				}
				else
				{
					FBest New;
					New.bDrained = true;
					New.Key = Result.Arrive[Into];
					if (IsBetter(Full, New))
					{
						New.Stops = MoveTemp(Stops);
						New.Result = MoveTemp(Result);
						New.Cars = Source.Cars;
						Full = MoveTemp(New);
					}
				}
			}
			// Emptying alone, for when the delivery doesn't fit in this trip.
			if (Emptying.Num() > 0)
			{
				FSTPlanResult Result = Simulate(Emptying);
				int32 Cleared = 0;
				for (UClass* Other : Blocking)
				{
					Cleared += CountUnloaded(Emptying, Result, Other, nullptr);
				}
				if (!Result.bValid)
				{
					Rejected.FindOrAdd(Result.Problem)++;
				}
				else if (Cleared > 0)
				{
					FBest New;
					New.bDrained = Cleared >= BlockingAmount;
					New.Cleared = Cleared;
					New.Key = Result.EndTime;
					if (IsBetter(Partial, New))
					{
						New.Stops = MoveTemp(Emptying);
						New.Result = MoveTemp(Result);
						New.Cars = Source.Cars;
						Partial = MoveTemp(New);
					}
				}
			}
		}
	}
	FBest* Best = Full.Stops.Num() > 0 ? &Full : (Partial.Stops.Num() > 0 ? &Partial : nullptr);
	if (Best)
	{
		OutStops = MoveTemp(Best->Stops);
		OutResult = MoveTemp(Best->Result);
		OutCars = MoveTemp(Best->Cars);
		bOutDelivers = Best == &Full;
		return true;
	}
	TArray<FString> Reasons;
	for (const TPair<FString, int32>& Each : Rejected)
	{
		Reasons.Add(Each.Value > 1 ? FString::Printf(TEXT("%s (%d times)"), *Each.Key, Each.Value) : Each.Key);
	}
	OutWhy = Reasons.Num() > 0 ? FString::Join(Reasons, TEXT("; ")) : FString(TEXT("no trip found"));
	return false;
}

bool FSTTripPlanner::AddRequest(TArray<FSTPlanStop>& InOutStops, FSTPlanResult& InOutResult, const FSTRequestTarget& Target, const TArray<int32>& Taken,
	TArray<int32>& OutCars, TFunctionRef<bool(const TArray<FSTPlanStop>&, const FSTPlanResult&)> KeepsOthers) const
{
	OutCars.Reset();
	if (!mNodes.IsValidIndex(Target.Node))
	{
		return false;
	}
	FString Why;
	const TArray<FRequestSource> Sources = FindRequestSources(Target, Taken, Why);
	UClass* Item = mNodes[Target.Node].Item.Get();
	AFGTrainStationIdentifier* Drop = mNodes[Target.Node].Station.Get();
	TArray<FSTPlanStop> BestStops;
	FSTPlanResult BestResult;
	TArray<int32> BestCars;
	double BestEnd = TNumericLimits<double>::Max();
	// Adds a load (or unload) of Item at Station to Stops: at its stop if it has one there, else as a new stop at At.
	auto Place = [Item](TArray<FSTPlanStop>& Stops, AFGTrainStationIdentifier* Station, int32 At, bool bLoad)
	{
		int32 Index = Stops.IndexOfByPredicate([Station](const FSTPlanStop& Stop) { return Stop.Station == Station; });
		if (Index == INDEX_NONE)
		{
			Index = At;
			Stops.InsertDefaulted(Index);
			Stops[Index].Station = Station;
		}
		if (bLoad)
		{
			Stops[Index].LoadItems.AddUnique(Item);
			Stops[Index].NoBatchItems.AddUnique(Item);
		}
		else
		{
			Stops[Index].UnloadItems.AddUnique(Item);
		}
		return Index;
	};
	for (const FRequestSource& Source : Sources)
	{
		const bool bPickThere = InOutStops.ContainsByPredicate([&](const FSTPlanStop& Stop) { return Stop.Station == Source.Station; });
		for (int32 PickAt = bPickThere ? 0 : InOutStops.Num(); PickAt >= 0 && !IsOverBudget(); --PickAt)
		{
			TArray<FSTPlanStop> WithPick = InOutStops;
			const int32 Picked = Place(WithPick, Source.Station, PickAt, true);
			const bool bDropThere = WithPick.ContainsByPredicate([&](const FSTPlanStop& Stop) { return Stop.Station == Drop; });
			for (int32 DropAt = bDropThere ? Picked + 1 : WithPick.Num(); DropAt > Picked; --DropAt)
			{
				TArray<FSTPlanStop> Stops = WithPick;
				const int32 Dropped = Place(Stops, Drop, DropAt, false);
				if (Dropped <= Picked || Stops.Num() > mMaxStops)
				{
					continue;
				}
				const FSTPlanResult Result = Simulate(Stops);
				if (Result.bValid && Result.EndTime < BestEnd && CountUnloadedInto(Result, Item, Target.Platform) > 0 && KeepsRequestCars(Stops, Result, Source.Cars, Item)
					&& KeepsOthers(Stops, Result))
				{
					BestEnd = Result.EndTime;
					BestStops = Stops;
					BestResult = Result;
					BestCars = Source.Cars;
				}
				if (bDropThere)
				{
					break;
				}
			}
			if (bPickThere)
			{
				break;
			}
		}
	}
	if (BestStops.Num() == 0)
	{
		return false;
	}
	InOutStops = MoveTemp(BestStops);
	InOutResult = MoveTemp(BestResult);
	OutCars = MoveTemp(BestCars);
	return true;
}

void FSTTripPlanner::MakeLegs(const TArray<FSTPlanStop>& Stops, const FSTPlanResult& Result, TArray<FSTJobLeg>& OutLegs) const
{
	OutLegs.Reset();
	for (int32 StopIndex = 0; StopIndex < Stops.Num(); ++StopIndex)
	{
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
						Work.Platform = const_cast<AFGBuildableTrainPlatformCargo*>(Step->Platform);
						bWorks = true;
					}
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
		Leg.bPartial = bLaterSame || (Loads.Num() > 0 && Unloads.Num() > 0);
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
					mNodes[Node].Planned.Add({Result.Arrive.IsValidIndex(StopIndex) ? Result.Arrive[StopIndex] : mTime, static_cast<double>(Each.Value)});
					mNodes[Node].Reserved += Each.Value;
					if (Step.Platform)
					{
						mNodes[Node].PlatformReserved.FindOrAdd(Step.Platform) += Each.Value;
					}
				}
			}
			for (const TPair<UClass*, int32>& Each : Step.Unloaded)
			{
				const int32 Node = FindNode(Station, Each.Key, false);
				if (Node != INDEX_NONE)
				{
					mNodes[Node].Planned.Add({Result.Arrive.IsValidIndex(StopIndex) ? Result.Arrive[StopIndex] : mTime, static_cast<double>(Each.Value)});
					mNodes[Node].Reserved += Each.Value;
					if (Step.Platform)
					{
						mNodes[Node].PlatformReserved.FindOrAdd(Step.Platform) += Each.Value;
					}
				}
			}
		}
		if (Result.Arrive.IsValidIndex(StopIndex))
		{
			mBookings.FindOrAdd(Station).Add({mTrainIndex, Result.Arrive[StopIndex], Result.Arrive[StopIndex] + mSubsystem.EstimateStaySeconds(Station)});
		}
	}
}

void FSTTripPlanner::Unreserve(const TArray<FSTPlanStop>& Stops, const FSTPlanResult& Result)
{
	for (int32 StopIndex = 0; StopIndex < Stops.Num() && Result.Steps.IsValidIndex(StopIndex); ++StopIndex)
	{
		const AFGTrainStationIdentifier* Station = Stops[StopIndex].Station;
		const double Arrive = Result.Arrive.IsValidIndex(StopIndex) ? Result.Arrive[StopIndex] : mTime;
		for (const FSTPlanCarStep& Step : Result.Steps[StopIndex])
		{
			for (int32 Pass = 0; Pass < 2; ++Pass)
			{
				for (const TPair<UClass*, int32>& Each : Pass == 0 ? Step.Loaded : Step.Unloaded)
				{
					const int32 Node = FindNode(Station, Each.Key, Pass == 0);
					if (Node == INDEX_NONE)
					{
						continue;
					}
					FSTNode& Target = mNodes[Node];
					Target.Reserved = FMath::Max(0, Target.Reserved - Each.Value);
					if (int32* AtPlatform = Step.Platform ? Target.PlatformReserved.Find(Step.Platform) : nullptr)
					{
						*AtPlatform = FMath::Max(0, *AtPlatform - Each.Value);
					}
					const int32 Event = Target.Planned.IndexOfByPredicate([&](const FSTNodeEvent& Planned)
					{
						return FMath::IsNearlyEqual(Planned.Time, Arrive, 0.01) && FMath::IsNearlyEqual(Planned.Amount, static_cast<double>(Each.Value), 0.01);
					});
					if (Event != INDEX_NONE)
					{
						Target.Planned.RemoveAtSwap(Event);
					}
				}
			}
		}
		if (TArray<FSTBooking>* Booked = mBookings.Find(Station))
		{
			const int32 Booking = Booked->IndexOfByPredicate([&](const FSTBooking& Each) { return Each.Train == mTrainIndex && FMath::IsNearlyEqual(Each.From, Arrive, 0.01); });
			if (Booking != INDEX_NONE)
			{
				Booked->RemoveAt(Booking);
			}
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
				const int32 Event = mNodes[Node].Planned.IndexOfByPredicate([&](const FSTNodeEvent& Each)
				{
					return FMath::IsNearlyEqual(Each.Time, Legs[Index].PlannedArrival, 1.0) && FMath::IsNearlyEqual(Each.Amount, static_cast<double>(Work.PlannedAmount), 0.5);
				});
				if (Event != INDEX_NONE)
				{
					mNodes[Node].Planned.RemoveAtSwap(Event);
				}
				mNodes[Node].Reserved = FMath::Max(0, mNodes[Node].Reserved - Work.PlannedAmount);
				if (int32* AtPlatform = Work.Platform ? mNodes[Node].PlatformReserved.Find(Work.Platform.Get()) : nullptr)
				{
					*AtPlatform = FMath::Max(0, *AtPlatform - Work.PlannedAmount);
				}
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
