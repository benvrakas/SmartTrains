#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildableRailroadStation.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGFreightWagon.h"
#include "FGInventoryComponent.h"
#include "FGLocomotive.h"
#include "FGRailroadSubsystem.h"
#include "FGRailroadTimeTable.h"
#include "FGRecipe.h"
#include "FGTrain.h"
#include "FGTrainStationIdentifier.h"
#include "Resources/FGItemDescriptor.h"
#include "SmartTrains.h"
#include "STBridges.h"
#include "STLayout.h"
#include "STLoopMath.h"
#include "STStationSetup.h"
#include "STSubsystem.h"

namespace
{
	// How often the loops are checked; the consumers and suppliers are worked out again at most this often. [s]
	constexpr float LoopSeconds = 5.f;
	constexpr double ModelSeconds = 30.0;
	// A station's factory is scanned again for its most rates after this long. [s]
	constexpr double RateScanSeconds = 1800.0;
	// Auto Trains travel-time questions per loop check. [calls]
	constexpr int32 AtTravelCallsPerCheck = 4;

	// What one written stop is: station, rules and the cars that skip it.
	FString StopKey(const FTimeTableStop& Stop, const TArray<TObjectPtr<AFGRailroadVehicle>>& SkipCars)
	{
		const FTrainDockingRuleSet& Rules = Stop.DockingRuleSet;
		FString Key = FString::Printf(TEXT("%p|%d|%.1f|%d|%d|"), ToRawPtr(Stop.Station), static_cast<int32>(Rules.DockingDefinition), Rules.DockForDuration,
			Rules.IsDurationAndRule ? 1 : 0, Rules.IgnoreFullLoadUnloadIfTransferBlockedByFilters ? 1 : 0);
		for (const TSubclassOf<UFGItemDescriptor>& Item : Rules.LoadFilterDescriptors)
		{
			Key += FString::Printf(TEXT("L%p"), Item.Get());
		}
		for (const TSubclassOf<UFGItemDescriptor>& Item : Rules.UnloadFilterDescriptors)
		{
			Key += FString::Printf(TEXT("U%p"), Item.Get());
		}
		TArray<UPTRINT> Cars;
		for (const TObjectPtr<AFGRailroadVehicle>& Car : SkipCars)
		{
			Cars.Add(reinterpret_cast<UPTRINT>(Car.Get()));
		}
		Cars.Sort();
		for (const UPTRINT Car : Cars)
		{
			Key += FString::Printf(TEXT("S%llx"), static_cast<uint64>(Car));
		}
		return Key;
	}

	// What Machines' recipes make (bProducts) or use of Item per minute at their clock speed. [/min]
	double GetRecipeRate(const TSet<AActor*>& Machines, UClass* Item, bool bProducts)
	{
		double PerMinute = 0.0;
		for (const AActor* Machine : Machines)
		{
			const AFGBuildableManufacturer* Manufacturer = Cast<AFGBuildableManufacturer>(Machine);
			const TSubclassOf<UFGRecipe> Recipe = Manufacturer ? Manufacturer->GetCurrentRecipe() : nullptr;
			const float Duration = Recipe ? UFGRecipe::GetManufacturingDuration(Recipe) : 0.f;
			if (Duration <= 0.f)
			{
				continue;
			}
			const float Clock = Manufacturer->GetCurrentPotential() > 0.f ? Manufacturer->GetCurrentPotential() : 1.f;
			// Production boost (somersloops) multiplies what a cycle makes, not what it uses.
			const float Boost = bProducts && Manufacturer->GetCurrentProductionBoost() > 0.f ? Manufacturer->GetCurrentProductionBoost() : 1.f;
			const TArray<FItemAmount> Amounts = bProducts ? UFGRecipe::GetProducts(Recipe)
				: UFGRecipe::GetIngredients(const_cast<AFGBuildableManufacturer*>(Manufacturer), Recipe);
			for (const FItemAmount& Amount : Amounts)
			{
				PerMinute += Amount.ItemClass == Item ? 60.0 * Amount.Amount * Clock * Boost / Duration : 0.0;
			}
		}
		return PerMinute;
	}

	// Car loads per minute of a rate in the item's inventory unit per minute (litres for fluids).
	double ToCarLoads(UClass* Item, double PerMinute)
	{
		const double Capacity = Item ? STLayout::GetCarCapacity(nullptr, Item) : 0.0;
		return Capacity > 0.0 ? PerMinute / Capacity : 0.0;
	}

	// Freight cars of Train, front to back.
	TArray<AFGFreightWagon*> GetCars(const AFGTrain* Train)
	{
		TArray<AFGFreightWagon*> Cars;
		for (AFGRailroadVehicle* Vehicle : STLayout::GetVehicles(Train))
		{
			if (AFGFreightWagon* Car = Cast<AFGFreightWagon>(Vehicle))
			{
				Cars.Add(Car);
			}
		}
		return Cars;
	}

	// The items held in Car, by class.
	TMap<UClass*, int32> GetCarCargo(const AFGFreightWagon* Car)
	{
		TMap<UClass*, int32> Held;
		const UFGInventoryComponent* Inventory = Car ? Car->GetFreightInventory() : nullptr;
		if (!Inventory)
		{
			return Held;
		}
		TArray<FInventoryStack> Stacks;
		Inventory->GetInventoryStacks(Stacks);
		for (const FInventoryStack& Stack : Stacks)
		{
			if (Stack.HasItems() && Stack.Item.GetItemClass())
			{
				Held.FindOrAdd(Stack.Item.GetItemClass()) += Stack.NumItems;
			}
		}
		return Held;
	}

	// Rules for a stop: only its items move, once, and the train goes on with what it has; with bWaitFull it leaves
	// only with full (loading) or empty (unloading) cars.
	FTrainDockingRuleSet MakeRules(const FSTJobLeg& Leg, bool bWaitFull)
	{
		FTrainDockingRuleSet Rules;
		// A car only moves its filtered items, so cargo kept for a later stop never counts against "fully".
		Rules.IgnoreFullLoadUnloadIfTransferBlockedByFilters = true;
		const TSubclassOf<UFGItemDescriptor> None = nullptr;
		for (UClass* Item : Leg.GetItems(true))
		{
			Rules.LoadFilterDescriptors.Add(Item);
		}
		for (UClass* Item : Leg.GetItems(false))
		{
			Rules.UnloadFilterDescriptors.Add(Item);
		}
		// An empty filter means everything to the game, so "nothing" is the none descriptor.
		if (Rules.LoadFilterDescriptors.Num() == 0)
		{
			Rules.LoadFilterDescriptors.Add(None);
		}
		if (Rules.UnloadFilterDescriptors.Num() == 0)
		{
			Rules.UnloadFilterDescriptors.Add(None);
		}
		Rules.DockForDuration = 0.f;
		const bool bWait = bWaitFull && Leg.HasWork();
		Rules.DockingDefinition = bWait ? ETrainDockingDefinition::TDD_FullyLoadUnload : ETrainDockingDefinition::TDD_LoadUnloadOnce;
		// With the wait, both the (zero) duration and the rule must hold.
		Rules.IsDurationAndRule = bWait;
		return Rules;
	}

	// One car's load or unload at a stop.
	FSTCarWork MakeWork(AFGFreightWagon* Car, UClass* Item, bool bLoad, int32 Amount, AFGBuildableTrainPlatformCargo* Platform)
	{
		FSTCarWork Work;
		Work.Car = Car;
		Work.Item = Item;
		Work.bLoad = bLoad;
		Work.PlannedAmount = Amount;
		Work.Platform = Platform;
		return Work;
	}

	// Picks the entry for the Rank-th of Count trains from Weights by largest remainder (STLoopMath::Split), in order.
	int32 PickByRank(const TArray<double>& Weights, int32 Count, int32 Rank)
	{
		const TArray<int32> Split = STLoopMath::Split(Weights, FMath::Max(Count, 1));
		int32 Seen = 0;
		for (int32 Index = 0; Index < Split.Num(); ++Index)
		{
			Seen += Split[Index];
			if (Rank < Seen)
			{
				return Index;
			}
		}
		// Fewer trains than entries, or a rank past the split: the largest weight.
		int32 Best = INDEX_NONE;
		for (int32 Index = 0; Index < Weights.Num(); ++Index)
		{
			Best = Weights[Index] > 0.0 && (Best == INDEX_NONE || Weights[Index] > Weights[Best]) ? Index : Best;
		}
		return Best;
	}
}

//~ The plan

void ASTSubsystem::CheckLoops()
{
	ReadSettings();
	mAtTravelCallsLeft = AtTravelCallsPerCheck;
	StepRateScan();
	const double Time = Now();
	if (bCheckSoon || Time - mModelAt >= ModelSeconds || mConsumers.Num() == 0)
	{
		mModelAt = Time;
		BuildLoopModel();
	}
	bCheckSoon = false;
	AssignTrains();
	UpdateUse();
	for (FSTTrainState& State : mTrains)
	{
		UpdateTrainJob(State);
	}
	if (Time - mLastMonitorLog >= 300.0 && mTrains.ContainsByPredicate([](const FSTTrainState& State) { return State.bManaged; }))
	{
		mLastMonitorLog = Time;
		for (const FString& Line : DescribeMonitor())
		{
			UE_LOG(LogSmartTrains, Log, TEXT("MONITOR %s"), *Line);
		}
	}
}

void ASTSubsystem::StepRateScan()
{
	TSet<AFGTrainStationIdentifier*> Stations;
	for (const FSTPlatformConfig& Config : mPlatforms)
	{
		const AFGBuildableRailroadStation* Building = IsValid(Config.Platform) && (Config.Role == ESTRole::Supply || Config.Role == ESTRole::Demand)
			? STLayout::FindStation(Config.Platform) : nullptr;
		if (AFGTrainStationIdentifier* Station = STLayout::GetIdentifier(Building))
		{
			Stations.Add(Station);
		}
	}
	const double Time = Now();
	AFGTrainStationIdentifier* Pick = nullptr;
	double PickAt = TNumericLimits<double>::Max();
	for (AFGTrainStationIdentifier* Station : Stations)
	{
		const FStationRates* Rates = mStationRates.Find(Station);
		const double At = mRatesDirty.Contains(Station) || !Rates ? -1.0e12 : Rates->ScannedAt;
		if (At < PickAt && (At < -1.0e11 || Time - At >= RateScanSeconds))
		{
			Pick = Station;
			PickAt = At;
		}
	}
	if (!Pick)
	{
		return;
	}
	mRatesDirty.Remove(Pick);
	const double Started = FPlatformTime::Seconds();
	const STMigration::FStationProposal Proposal = STSetup::ProposeStation(*this, Pick, nullptr, true, false);
	const STSetup::FFactory Factory = STSetup::AnalyzeFactory(*this, Proposal, true);
	FStationRates& Rates = mStationRates.FindOrAdd(Pick);
	FStationRates Before = Rates;
	Rates.In.Reset();
	Rates.Out.Reset();
	Rates.ScannedAt = Time;
	// The scan counts fluids in m³; inventories count them in litres.
	auto InUnits = [](UClass* Item, double PerMinute) { return PerMinute * (STNames::IsFluid(Item) ? 1000.0 : 1.0); };
	TArray<FString> Parts;
	for (const TPair<TSubclassOf<UFGItemDescriptor>, double>& Each : Factory.InputRates)
	{
		Rates.In.Add(Each.Key.Get(), InUnits(Each.Key.Get(), Each.Value));
		Parts.Add(FString::Printf(TEXT("uses %s %s/min"), *STNames::Item(Each.Key.Get()), *STSetup::FormatRate(Each.Value)));
	}
	if (Factory.Product && Factory.Rate > 0.0)
	{
		Rates.Out.Add(Factory.Product.Get(), InUnits(Factory.Product.Get(), Factory.Rate));
		Parts.Add(FString::Printf(TEXT("makes %s %s/min"), *STNames::Item(Factory.Product.Get()), *STSetup::FormatRate(Factory.Rate)));
	}
	const bool bChanged = Before.In.OrderIndependentCompareEqual(Rates.In) == false || Before.Out.OrderIndependentCompareEqual(Rates.Out) == false;
	UE_LOG(LogSmartTrains, Log, TEXT("FACTORY %s: %s (limited by %s; %.0f ms)%s"), *STLayout::Name(Pick), Parts.Num() > 0 ? *FString::Join(Parts, TEXT(", ")) : TEXT("no rate found"),
		Factory.Limit.IsEmpty() ? TEXT("nothing found") : *Factory.Limit, 1000.0 * (FPlatformTime::Seconds() - Started), bChanged ? TEXT("") : TEXT(", unchanged"));
	if (bChanged)
	{
		bCheckSoon = true;
	}
}

void ASTSubsystem::BuildLoopModel()
{
	mConsumers.Reset();
	mSuppliers.Reset();
	// Platforms by station.
	TMap<AFGTrainStationIdentifier*, TArray<TPair<int32, const FSTPlatformConfig*>>> ByStation;
	for (const FSTPlatformConfig& Config : mPlatforms)
	{
		if (!IsValid(Config.Platform) || Config.Role == ESTRole::Off)
		{
			continue;
		}
		int32 Index = INDEX_NONE;
		const AFGBuildableRailroadStation* Building = STLayout::FindStation(Config.Platform, &Index);
		if (AFGTrainStationIdentifier* Station = STLayout::GetIdentifier(Building))
		{
			ByStation.FindOrAdd(Station).Emplace(Index, &Config);
		}
	}
	// The machines behind a station's platforms of one role and item, each once (platforms may share them).
	auto MachinesOf = [](const TArray<TPair<int32, const FSTPlatformConfig*>>& Platforms, ESTRole WantRole, UClass* Item)
	{
		TSet<AActor*> Machines;
		for (const TPair<int32, const FSTPlatformConfig*>& Platform : Platforms)
		{
			if (Platform.Value->Role == WantRole && Platform.Value->FindRule(Item))
			{
				for (const TObjectPtr<AActor>& Machine : Platform.Value->Machines)
				{
					Machines.Add(Machine.Get());
				}
			}
		}
		Machines.Remove(nullptr);
		return Machines;
	};
	// Suppliers, per station and item.
	for (TPair<AFGTrainStationIdentifier*, TArray<TPair<int32, const FSTPlatformConfig*>>>& Each : ByStation)
	{
		const FStationRates* Rates = mStationRates.Find(Each.Key);
		TSet<UClass*> Items;
		for (const TPair<int32, const FSTPlatformConfig*>& Platform : Each.Value)
		{
			for (const FSTItemRule& Rule : Platform.Value->Items)
			{
				if (Platform.Value->Role == ESTRole::Supply && Rule.Item)
				{
					Items.Add(Rule.Item.Get());
				}
			}
		}
		for (UClass* Item : Items)
		{
			FSTSupplier& Supplier = mSuppliers.AddDefaulted_GetRef();
			Supplier.Station = Each.Key;
			Supplier.Item = Item;
			const double* Scanned = Rates ? Rates->Out.Find(Item) : nullptr;
			const double PerMinute = Scanned && *Scanned > 0.0 ? *Scanned : GetRecipeRate(MachinesOf(Each.Value, ESTRole::Supply, Item), Item, true);
			Supplier.Rate = PerMinute > 0.0 ? ToCarLoads(Item, PerMinute) : -1.0;
		}
	}
	// Consumers: each station with Demand platforms, its slots in platform order.
	for (TPair<AFGTrainStationIdentifier*, TArray<TPair<int32, const FSTPlatformConfig*>>>& Each : ByStation)
	{
		Each.Value.Sort([](const TPair<int32, const FSTPlatformConfig*>& A, const TPair<int32, const FSTPlatformConfig*>& B) { return A.Key < B.Key; });
		FSTConsumer Consumer;
		Consumer.Station = Each.Key;
		Consumer.Priority = GetStationPriority(Each.Key);
		TMap<UClass*, int32> SlotsOf;
		for (const TPair<int32, const FSTPlatformConfig*>& Platform : Each.Value)
		{
			if (Platform.Value->Role != ESTRole::Demand)
			{
				continue;
			}
			FSTLoopSlot& Slot = Consumer.Slots.AddDefaulted_GetRef();
			Slot.Index = Platform.Key;
			Slot.Platform = Platform.Value->Platform.Get();
			Slot.bFluid = STLayout::GetPlatformCargoType(Platform.Value->Platform) == EFreightCargoType::FCT_Liquid;
			for (const FSTItemRule& Rule : Platform.Value->Items)
			{
				if (Rule.Item)
				{
					Slot.Items.AddUnique(Rule.Item.Get());
					++SlotsOf.FindOrAdd(Rule.Item.Get());
				}
			}
			if (Slot.Items.Num() == 0)
			{
				Consumer.Slots.Pop();
			}
		}
		if (Consumer.Slots.Num() == 0)
		{
			continue;
		}
		// Rates: the station's use of each item (the scan's, else its machines' recipes) shared over the slots that take it.
		const FStationRates* Rates = mStationRates.Find(Each.Key);
		for (FSTLoopSlot& Slot : Consumer.Slots)
		{
			for (const TWeakObjectPtr<UClass>& Item : Slot.Items)
			{
				const double* Scanned = Rates ? Rates->In.Find(Item) : nullptr;
				const double AtStation = Scanned && *Scanned > 0.0 ? *Scanned : GetRecipeRate(MachinesOf(Each.Value, ESTRole::Demand, Item.Get()), Item.Get(), false);
				const double PerMinute = AtStation / FMath::Max(1, SlotsOf.FindRef(Item.Get()));
				Slot.ItemRates.Add(ToCarLoads(Item.Get(), PerMinute));
				Slot.Rate += Slot.ItemRates.Last();
			}
			Consumer.Rate = FMath::Max(Consumer.Rate, Slot.Rate);
		}
		Consumer.bRateKnown = Consumer.Rate > 0.0;
		mConsumers.Add(MoveTemp(Consumer));
	}
	// Biggest consumers take the nearest suppliers first.
	mConsumers.Sort([](const FSTConsumer& A, const FSTConsumer& B) { return A.Rate > B.Rate; });
	for (FSTSupplier& Supplier : mSuppliers)
	{
		Supplier.Taken = 0.0;
	}
	for (FSTConsumer& Consumer : mConsumers)
	{
		AFGTrainStationIdentifier* Station = Consumer.Station.Get();
		const AFGBuildableRailroadStation* Building = Station ? Station->GetStation() : nullptr;
		TArray<FString> Missing;
		TSet<AFGTrainStationIdentifier*> Visit;
		for (const FSTLoopSlot& Slot : Consumer.Slots)
		{
			for (int32 Item = 0; Item < Slot.Items.Num(); ++Item)
			{
				TArray<int32> Candidates;
				TArray<STLoopMath::FOffer> Offers;
				for (int32 Index = 0; Index < mSuppliers.Num(); ++Index)
				{
					const FSTSupplier& Supplier = mSuppliers[Index];
					const AFGBuildableRailroadStation* From = Supplier.Station.IsValid() ? Supplier.Station->GetStation() : nullptr;
					if (Supplier.Item != Slot.Items[Item] || !From || !Building || From->GetTrackGraphID() != Building->GetTrackGraphID() || Supplier.Station == Consumer.Station)
					{
						continue;
					}
					const float Travel = EstimateTravelSeconds(Supplier.Station.Get(), Station);
					if (Travel < 0.f)
					{
						continue;
					}
					Candidates.Add(Index);
					Offers.Add({Travel, Supplier.Rate < 0.0 ? -1.0 : FMath::Max(0.0, Supplier.Rate - Supplier.Taken)});
				}
				if (Candidates.Num() == 0)
				{
					Missing.AddUnique(STNames::Item(Slot.Items[Item].Get()));
					continue;
				}
				const TArray<double> Taken = STLoopMath::Allocate(FMath::Max(Slot.ItemRates[Item], 1e-6), Offers);
				for (int32 Offer = 0; Offer < Taken.Num(); ++Offer)
				{
					if (Taken[Offer] > 0.0)
					{
						FSTSupplier& Supplier = mSuppliers[Candidates[Offer]];
						Supplier.Taken += Taken[Offer];
						Consumer.Sources.Add({Slot.Items[Item], Candidates[Offer], Taken[Offer]});
						Visit.Add(Supplier.Station.Get());
					}
				}
			}
		}
		// Round trip: from the consumer to its nearest supplier still to visit, and so on, then back, with the stays.
		double RoundTrip = EstimateStaySeconds(Station);
		AFGTrainStationIdentifier* At = Station;
		while (Visit.Num() > 0)
		{
			AFGTrainStationIdentifier* Next = nullptr;
			float Best = TNumericLimits<float>::Max();
			for (AFGTrainStationIdentifier* Candidate : Visit)
			{
				const float Travel = EstimateTravelSeconds(At, Candidate);
				if (Travel >= 0.f && Travel < Best)
				{
					Best = Travel;
					Next = Candidate;
				}
			}
			if (!Next)
			{
				break;
			}
			RoundTrip += Best + EstimateStaySeconds(Next);
			Visit.Remove(Next);
			At = Next;
		}
		RoundTrip += FMath::Max(0.f, EstimateTravelSeconds(At, Station));
		Consumer.RoundTrip = RoundTrip;
		Consumer.Need = Consumer.Sources.Num() > 0 ? Consumer.Rate * RoundTrip / 60.0 : 0.0;
		Consumer.Problem = !Consumer.bRateKnown ? FString(TEXT("no rate known: set the station up from its factory (station menu) so its machines are traced"))
			: Consumer.Sources.Num() == 0 ? FString::Printf(TEXT("no supplier of %s on its rail network"), *FString::Join(Missing, TEXT(", ")))
			: Missing.Num() > 0 ? FString::Printf(TEXT("no supplier of %s on its rail network; its loop brings the rest"), *FString::Join(Missing, TEXT(", ")))
			: FString();
		Consumer.Cause = !Consumer.bRateKnown ? FString(TEXT("rate not known"))
			: Missing.Num() > 0 ? FString::Printf(TEXT("no %s supplier"), *FString::Join(Missing, TEXT(", ")))
			: FString();
		Consumer.ModelProblem = Consumer.Problem;
		Consumer.ModelCause = Consumer.Cause;
	}
	UE_LOG(LogSmartTrains, Verbose, TEXT("LOOPS model: %d consumers, %d supplier items"), mConsumers.Num(), mSuppliers.Num());
}

const FSTConsumer* ASTSubsystem::FindConsumer(const AFGTrainStationIdentifier* Station) const
{
	return Station ? mConsumers.FindByPredicate([Station](const FSTConsumer& Each) { return Each.Station.Get() == Station; }) : nullptr;
}

bool ASTSubsystem::FitsConsumer(const AFGTrain* Train, const FSTConsumer& Consumer, TArray<AFGFreightWagon*>& OutCars, FString& OutWhy) const
{
	OutCars.Reset();
	const AFGTrainStationIdentifier* Station = Consumer.Station.Get();
	if (!CanServe(Train, Station))
	{
		OutWhy = TEXT("not on its rail network");
		return false;
	}
	const TArray<TArray<AFGRailroadVehicle*>> Maps = GetDockMaps(Train, Station);
	if (Maps.Num() == 0)
	{
		OutWhy = TEXT("it has no locomotive to dock with");
		return false;
	}
	// The first map: the way round it was seen docking, else its lead locomotive's.
	const TArray<AFGRailroadVehicle*>& Map = Maps[0];
	for (const FSTLoopSlot& Slot : Consumer.Slots)
	{
		AFGFreightWagon* Car = Map.IsValidIndex(Slot.Index) ? Cast<AFGFreightWagon>(Map[Slot.Index]) : nullptr;
		if (!Car)
		{
			OutWhy = FString::Printf(TEXT("no freight car beside platform %d"), Slot.Index + 1);
			OutCars.Reset();
			return false;
		}
		if ((STLayout::GetCargoType(Car) == EFreightCargoType::FCT_Liquid) != Slot.bFluid)
		{
			OutWhy = FString::Printf(TEXT("the car beside platform %d is a %s car, the platform a %s one"), Slot.Index + 1, Slot.bFluid ? TEXT("solid") : TEXT("fluid"),
				Slot.bFluid ? TEXT("fluid") : TEXT("solid"));
			OutCars.Reset();
			return false;
		}
		OutCars.Add(Car);
	}
	return true;
}

void ASTSubsystem::AssignTrains()
{
	// Trains that can take a loop: managed, not held, not driven by hand.
	TArray<FSTTrainState*> Available;
	for (FSTTrainState& State : mTrains)
	{
		if (State.bManaged && IsValid(State.Train) && !State.bHeld && !(State.Train->IsPlayerDriven() && !State.Train->IsSelfDrivingEnabled()))
		{
			Available.Add(&State);
		}
	}
	// Which consumers each train fits.
	TMap<const FSTTrainState*, TArray<int32>> Fits;
	TArray<int32> Fitting;
	Fitting.Init(0, mConsumers.Num());
	for (const FSTTrainState* State : Available)
	{
		TArray<int32>& List = Fits.Add(State);
		for (int32 Index = 0; Index < mConsumers.Num(); ++Index)
		{
			TArray<AFGFreightWagon*> Cars;
			FString Why;
			if (FitsConsumer(State->Train, mConsumers[Index], Cars, Why))
			{
				List.Add(Index);
				++Fitting[Index];
			}
		}
	}
	// A new split only when what it depends on changed: the consumers, their slots and rates, the trains and what they fit.
	FString Inputs;
	for (int32 Index = 0; Index < mConsumers.Num(); ++Index)
	{
		const FSTConsumer& Consumer = mConsumers[Index];
		Inputs += FString::Printf(TEXT("%p:%.3g:%d:%d;"), Consumer.Station.Get(), Consumer.Rate, Consumer.Sources.Num(), static_cast<int32>(Consumer.Priority));
	}
	for (const FSTTrainState* State : Available)
	{
		Inputs += FString::Printf(TEXT("%p"), State->Train.Get());
		for (const int32 Index : Fits.FindRef(State))
		{
			Inputs += FString::Printf(TEXT(",%d"), Index);
		}
		Inputs += TEXT(";");
	}
	const bool bResplit = Inputs != mSplitInputs;
	// The targets: all trains that fit some consumer, split by need over the consumers some train fits.
	TArray<double> Needs;
	TArray<uint8> Tiers;
	for (int32 Index = 0; Index < mConsumers.Num(); ++Index)
	{
		FSTConsumer& Consumer = mConsumers[Index];
		// The split's own reasons are worked out afresh each time.
		Consumer.Problem = Consumer.ModelProblem;
		Consumer.Cause = Consumer.ModelCause;
		Tiers.Add(static_cast<uint8>(Consumer.Priority));
		if (Fitting[Index] == 0 && Consumer.Problem.IsEmpty())
		{
			// The cars it needs: one of the right kind beside each unloading platform.
			TArray<FString> Cars;
			for (const FSTLoopSlot& Slot : Consumer.Slots)
			{
				Cars.Add(FString::Printf(TEXT("a %s car beside platform %d"), Slot.bFluid ? TEXT("fluid") : TEXT("freight"), Slot.Index + 1));
			}
			Consumer.Problem = FString::Printf(TEXT("no managed train fits its pattern: it needs %s"), *FString::Join(Cars, TEXT(", ")));
			Consumer.Cause = TEXT("no train has the right cars");
		}
		Needs.Add(Fitting[Index] > 0 ? Consumer.Need : 0.0);
	}
	int32 Usable = 0;
	for (const FSTTrainState* State : Available)
	{
		Usable += Fits.FindRef(State).Num() > 0 ? 1 : 0;
	}
	const TArray<int32> Targets = STLoopMath::SplitByPriority(Needs, Tiers, Fitting, Usable);
	TArray<TArray<FSTTrainState*>> On;
	On.SetNum(mConsumers.Num());
	if (!bResplit)
	{
		// Unchanged: every train keeps its consumer.
		for (FSTTrainState* State : Available)
		{
			const int32 Index = mConsumers.IndexOfByPredicate([State](const FSTConsumer& Each) { return Each.Station.Get() == State->LoopConsumer; });
			if (On.IsValidIndex(Index))
			{
				On[Index].Add(State);
			}
		}
	}
	else
	{
		mSplitInputs = Inputs;
		// Each train keeps its consumer while it fits there and the consumer isn't over its target.
		TArray<FSTTrainState*> Free;
		for (FSTTrainState* State : Available)
		{
			const int32 Index = mConsumers.IndexOfByPredicate([State](const FSTConsumer& Each) { return Each.Station.Get() == State->LoopConsumer; });
			if (On.IsValidIndex(Index) && Fits.FindRef(State).Contains(Index) && On[Index].Num() < Targets[Index])
			{
				On[Index].Add(State);
			}
			else
			{
				Free.Add(State);
			}
		}
		// Consumers short of their target take free trains that fit them: empty ones first, then the nearest.
		auto Distance = [this](const FSTTrainState* State, const FSTConsumer& Consumer)
		{
			const float Travel = State->LastStation ? EstimateTravelSeconds(State->LastStation.Get(), Consumer.Station.Get(), IsPushPull(State->Train)) : -1.f;
			return Travel >= 0.f ? Travel : TNumericLimits<float>::Max() / 2.f;
		};
		for (bool bFilled = true; bFilled;)
		{
			bFilled = false;
			int32 Best = INDEX_NONE;
			for (int32 Index = 0; Index < mConsumers.Num(); ++Index)
			{
				const int32 Short = Targets[Index] - On[Index].Num();
				const bool bAnyFits = Free.ContainsByPredicate([&](const FSTTrainState* State) { return Fits.FindRef(State).Contains(Index); });
				if (Short > 0 && bAnyFits && (Best == INDEX_NONE || Short > Targets[Best] - On[Best].Num()))
				{
					Best = Index;
				}
			}
			if (Best == INDEX_NONE)
			{
				break;
			}
			FSTTrainState* Pick = nullptr;
			for (FSTTrainState* State : Free)
			{
				if (!Fits.FindRef(State).Contains(Best))
				{
					continue;
				}
				const bool bEmpty = GetCargo(State->Train).Num() == 0;
				const bool bPickEmpty = Pick && GetCargo(Pick->Train).Num() == 0;
				if (!Pick || (bEmpty && !bPickEmpty) || (bEmpty == bPickEmpty && Distance(State, mConsumers[Best]) < Distance(Pick, mConsumers[Best])))
				{
					Pick = State;
				}
			}
			On[Best].Add(Pick);
			Free.Remove(Pick);
			bFilled = true;
		}
		// Trains left over (their fitting consumers are full): each goes where it fits that is furthest below its share.
		for (FSTTrainState* State : Free)
		{
			int32 Best = INDEX_NONE;
			for (const int32 Index : Fits.FindRef(State))
			{
				const double Ratio = Needs[Index] / (On[Index].Num() + 1.0);
				if (Needs[Index] > 0.0 && (Best == INDEX_NONE || Ratio > Needs[Best] / (On[Best].Num() + 1.0)))
				{
					Best = Index;
				}
			}
			if (Best != INDEX_NONE)
			{
				On[Best].Add(State);
			}
		}
	}
	// The result into the trains and the consumers.
	for (FSTTrainState* State : Available)
	{
		AFGTrainStationIdentifier* Before = State->LoopConsumer.Get();
		AFGTrainStationIdentifier* After = nullptr;
		for (int32 Index = 0; Index < mConsumers.Num() && !After; ++Index)
		{
			After = On[Index].Contains(State) ? mConsumers[Index].Station.Get() : nullptr;
		}
		State->LoopConsumer = After;
		State->IdleReason = After ? FString() : Fits.FindRef(State).Num() == 0 ? FString(TEXT("its cars line up with no consumer's pattern"))
			: FString(TEXT("no consumer it fits has a known need"));
		if (Before != After)
		{
			UE_LOG(LogSmartTrains, Log, TEXT("LOOP %s: %s (was %s)"), *STLayout::Name(State->Train), After ? *FString::Printf(TEXT("serves %s"), *STLayout::Name(After))
				: *FString::Printf(TEXT("no loop: %s"), *State->IdleReason), Before ? *STLayout::Name(Before) : TEXT("none"));
		}
	}
	for (int32 Index = 0; Index < mConsumers.Num(); ++Index)
	{
		FSTConsumer& Consumer = mConsumers[Index];
		Consumer.Target = Targets[Index];
		Consumer.Trains.Reset();
		for (const FSTTrainState* State : On[Index])
		{
			Consumer.Trains.Add(State->Train.Get());
		}
	}
	for (FSTConsumer& Consumer : mConsumers)
	{
		if (Consumer.Trains.Num() == 0 && Consumer.Problem.IsEmpty())
		{
			const bool bOutranked = mConsumers.ContainsByPredicate([&Consumer](const FSTConsumer& Each) { return Each.Priority > Consumer.Priority && Each.Trains.Num() > 0; });
			Consumer.Problem = bOutranked ? TEXT("unserved: higher-priority loops take the trains it fits until they have their full need")
				: TEXT("unserved: fewer trains than consumers, and its share is among the smallest");
			Consumer.Cause = bOutranked ? TEXT("higher priority loops come first") : TEXT("fewer trains than consumers");
		}
	}
	if (bResplit)
	{
		TArray<FString> Parts;
		for (const FSTConsumer& Consumer : mConsumers)
		{
			Parts.Add(FString::Printf(TEXT("%s %d (need %.2f%s)"), *STLayout::Name(Consumer.Station.Get()), Consumer.Trains.Num(), Consumer.Need,
				Consumer.Priority == ESTPriority::Medium ? TEXT("") : *FString::Printf(TEXT(", %s"), STNames::Priority(Consumer.Priority))));
		}
		UE_LOG(LogSmartTrains, Log, TEXT("SPLIT %d trains over %d consumers: %s"), Usable, mConsumers.Num(), *FString::Join(Parts, TEXT(", ")));
	}
}

//~ Jobs

bool ASTSubsystem::MakeLoopJob(const FSTTrainState& State, const FSTConsumer& Consumer, FSTJob& OutJob, FString& OutWhy) const
{
	OutJob = FSTJob();
	AFGTrain* Train = State.Train;
	TArray<AFGFreightWagon*> Cars;
	if (!FitsConsumer(Train, Consumer, Cars, OutWhy))
	{
		return false;
	}
	// This train's rank on its loop picks its item of an item group and its supplier where there are several, so the
	// loop's trains share them by rate.
	const int32 Count = FMath::Max(1, Consumer.Trains.Num());
	const int32 Rank = FMath::Max(0, Consumer.Trains.IndexOfByKey(Train));
	TMap<AFGTrainStationIdentifier*, FSTJobLeg> Pickups;
	TArray<AFGTrainStationIdentifier*> PickupOrder;
	FSTJobLeg Delivery;
	Delivery.Station = Consumer.Station.Get();
	TArray<FString> Unused;
	for (int32 Slot = 0; Slot < Consumer.Slots.Num(); ++Slot)
	{
		const FSTLoopSlot& Each = Consumer.Slots[Slot];
		AFGFreightWagon* Car = Cars[Slot];
		const int32 ItemIndex = Each.Items.Num() == 1 ? 0 : PickByRank(Each.ItemRates, Count, Rank);
		UClass* Item = Each.Items.IsValidIndex(ItemIndex) ? Each.Items[ItemIndex].Get() : nullptr;
		if (!Item)
		{
			continue;
		}
		// Its suppliers by rate taken; this train takes one, or failing that any supplier of the item it fits at.
		TArray<int32> Suppliers;
		TArray<double> Taken;
		for (const FSTLoopSource& Source : Consumer.Sources)
		{
			if (Source.Item.Get() == Item)
			{
				Suppliers.Add(Source.Supplier);
				Taken.Add(Source.Taken);
			}
		}
		TArray<int32> Order;
		const int32 First = PickByRank(Taken, Count, Rank);
		if (Suppliers.IsValidIndex(First))
		{
			Order.Add(Suppliers[First]);
		}
		for (const int32 Supplier : Suppliers)
		{
			Order.AddUnique(Supplier);
		}
		AFGTrainStationIdentifier* From = nullptr;
		AFGBuildableTrainPlatformCargo* FromPlatform = nullptr;
		for (const int32 Supplier : Order)
		{
			AFGTrainStationIdentifier* Station = mSuppliers.IsValidIndex(Supplier) ? mSuppliers[Supplier].Station.Get() : nullptr;
			const TArray<TArray<AFGRailroadVehicle*>> Maps = Station ? GetDockMaps(Train, Station) : TArray<TArray<AFGRailroadVehicle*>>();
			const int32 Position = Maps.Num() > 0 ? Maps[0].IndexOfByKey(Car) : INDEX_NONE;
			const TArray<AFGBuildableTrainPlatform*> Platforms = STLayout::GetPlatforms(Station ? Station->GetStation() : nullptr);
			AFGBuildableTrainPlatformCargo* Cargo = Platforms.IsValidIndex(Position) ? Cast<AFGBuildableTrainPlatformCargo>(Platforms[Position]) : nullptr;
			const FSTPlatformConfig* Config = FindPlatformConfig(Cargo);
			if (Config && Config->Role == ESTRole::Supply && Config->FindRule(Item))
			{
				From = Station;
				FromPlatform = Cargo;
				break;
			}
		}
		if (!From)
		{
			Unused.Add(FString::Printf(TEXT("car %d (%s: no supplier where it stands beside a platform of it)"), GetCars(Train).IndexOfByKey(Car) + 1, *STNames::Item(Item)));
			continue;
		}
		if (!Pickups.Contains(From))
		{
			Pickups.Add(From).Station = From;
			PickupOrder.Add(From);
		}
		const int32 Amount = static_cast<int32>(STLayout::GetCarCapacity(Car, Item));
		Pickups[From].Work.Add(MakeWork(Car, Item, true, Amount, FromPlatform));
		const TArray<AFGBuildableTrainPlatform*> Here = STLayout::GetPlatforms(Delivery.Station ? Delivery.Station->GetStation() : nullptr);
		Delivery.Work.Add(MakeWork(Car, Item, false, Amount, Here.IsValidIndex(Each.Index) ? Cast<AFGBuildableTrainPlatformCargo>(Here[Each.Index]) : nullptr));
	}
	if (PickupOrder.Num() == 0)
	{
		OutWhy = Unused.Num() > 0 ? FString::Printf(TEXT("no car can load: %s"), *FString::Join(Unused, TEXT("; "))) : FString(TEXT("no supplier for its pattern"));
		return false;
	}
	// The pickups in the order of the shortest way from the consumer, each next the nearest.
	TArray<AFGTrainStationIdentifier*> Ordered;
	AFGTrainStationIdentifier* At = Delivery.Station.Get();
	while (PickupOrder.Num() > 0)
	{
		int32 Best = 0;
		float BestTravel = TNumericLimits<float>::Max();
		for (int32 Index = 0; Index < PickupOrder.Num(); ++Index)
		{
			const float Travel = EstimateTravelSeconds(At, PickupOrder[Index], IsPushPull(Train));
			if (Travel >= 0.f && Travel < BestTravel)
			{
				BestTravel = Travel;
				Best = Index;
			}
		}
		At = PickupOrder[Best];
		Ordered.Add(At);
		PickupOrder.RemoveAt(Best);
	}
	// Every car that doesn't work at a stop skips it.
	const TArray<AFGFreightWagon*> All = GetCars(Train);
	auto Skips = [&All](FSTJobLeg& Leg)
	{
		for (AFGFreightWagon* Car : All)
		{
			if (!Leg.Work.ContainsByPredicate([Car](const FSTCarWork& Work) { return Work.Car == Car; }))
			{
				Leg.SkipCars.Add(Car);
			}
		}
	};
	for (AFGTrainStationIdentifier* Station : Ordered)
	{
		FSTJobLeg Leg = Pickups[Station];
		Skips(Leg);
		OutJob.Legs.Add(MoveTemp(Leg));
	}
	Skips(Delivery);
	OutJob.Legs.Add(MoveTemp(Delivery));
	OutJob.Kind = ESTJobKind::Loop;
	OutJob.AnchorStation = Consumer.Station.Get();
	OutJob.Summary = SummarizeLegs(OutJob.Legs);
	OutWhy = Unused.Num() > 0 ? FString::Printf(TEXT("unused: %s"), *FString::Join(Unused, TEXT("; "))) : FString();
	return true;
}

bool ASTSubsystem::HoldsOtherCargo(const FSTTrainState& State, const FSTJob& Loop) const
{
	for (AFGFreightWagon* Car : GetCars(State.Train))
	{
		for (const TPair<UClass*, int32>& Held : GetCarCargo(Car))
		{
			const bool bCarried = Loop.Legs.ContainsByPredicate([&](const FSTJobLeg& Leg)
			{
				return Leg.Work.ContainsByPredicate([&](const FSTCarWork& Work) { return Work.Car == Car && Work.Item == Held.Key; });
			});
			if (!bCarried)
			{
				return true;
			}
		}
	}
	return false;
}

bool ASTSubsystem::MakeUnloadJob(const FSTTrainState& State, AFGTrainStationIdentifier* Station, ESTJobKind Kind, FSTJob& OutJob) const
{
	OutJob = FSTJob();
	AFGTrain* Train = State.Train;
	if (!Station || !CanServe(Train, Station))
	{
		return false;
	}
	const TArray<TArray<AFGRailroadVehicle*>> Maps = GetDockMaps(Train, Station);
	if (Maps.Num() == 0)
	{
		return false;
	}
	const TArray<AFGBuildableTrainPlatform*> Platforms = STLayout::GetPlatforms(Station->GetStation());
	FSTJobLeg Leg;
	Leg.Station = Station;
	for (AFGFreightWagon* Car : GetCars(Train))
	{
		const int32 Position = Maps[0].IndexOfByKey(Car);
		AFGBuildableTrainPlatformCargo* Cargo = Platforms.IsValidIndex(Position) ? Cast<AFGBuildableTrainPlatformCargo>(Platforms[Position]) : nullptr;
		const FSTPlatformConfig* Config = FindPlatformConfig(Cargo);
		if (!Config || Config->Role != (Kind == ESTJobKind::Sink ? ESTRole::Sink : ESTRole::Demand))
		{
			continue;
		}
		for (const TPair<UClass*, int32>& Held : GetCarCargo(Car))
		{
			if (Config->FindRule(Held.Key) && STLayout::ItemFitsCargoType(Held.Key, STLayout::GetPlatformCargoType(Cargo)))
			{
				Leg.Work.Add(MakeWork(Car, Held.Key, false, Held.Value, Cargo));
			}
		}
	}
	if (!Leg.HasWork())
	{
		return false;
	}
	for (AFGFreightWagon* Car : GetCars(Train))
	{
		if (!Leg.Work.ContainsByPredicate([Car](const FSTCarWork& Work) { return Work.Car == Car; }))
		{
			Leg.SkipCars.Add(Car);
		}
	}
	OutJob.Legs.Add(MoveTemp(Leg));
	OutJob.Kind = Kind;
	OutJob.AnchorStation = Station;
	OutJob.Summary = FString::Printf(TEXT("%s %s"), Kind == ESTJobKind::Sink ? TEXT("to sink at") : TEXT("deliver what it holds at"), *STLayout::Name(Station));
	return true;
}

AFGTrainStationIdentifier* ASTSubsystem::FindSinkStation(const FSTTrainState& State) const
{
	AFGTrainStationIdentifier* Best = nullptr;
	float BestTravel = TNumericLimits<float>::Max();
	TSet<AFGTrainStationIdentifier*> Tried;
	for (const FSTPlatformConfig& Config : mPlatforms)
	{
		AFGTrainStationIdentifier* Station = IsValid(Config.Platform) && Config.Role == ESTRole::Sink ? STLayout::GetIdentifier(STLayout::FindStation(Config.Platform)) : nullptr;
		bool bTried = false;
		Tried.Add(Station, &bTried);
		if (!Station || bTried)
		{
			continue;
		}
		FSTJob Job;
		if (!MakeUnloadJob(State, Station, ESTJobKind::Sink, Job))
		{
			continue;
		}
		const float Travel = State.LastStation ? EstimateTravelSeconds(State.LastStation.Get(), Station, IsPushPull(State.Train)) : 0.f;
		if (Travel >= 0.f && Travel < BestTravel)
		{
			BestTravel = Travel;
			Best = Station;
		}
	}
	return Best;
}

void ASTSubsystem::UpdateTrainJob(FSTTrainState& State, ESTJobKind After)
{
	AFGTrain* Train = State.Train;
	if (!State.bManaged || !IsValid(Train) || State.bHeld || (Train->IsPlayerDriven() && !Train->IsSelfDrivingEnabled()))
	{
		return;
	}
	// Switching patterns: its Deliver or Sink stop comes first.
	if (State.bHasJob && State.Job.Kind != ESTJobKind::Loop)
	{
		return;
	}
	const FSTConsumer* Consumer = FindConsumer(State.LoopConsumer.Get());
	// At a depot signal while its consumer is still its own; given another consumer (or none), back on the line first.
	if (State.bDepot)
	{
		if (Consumer && Consumer->Trains.Contains(Train))
		{
			CheckDepotDrive(State);
			return;
		}
		WakeFromDepot(State, Consumer ? TEXT("it serves another consumer now") : TEXT("its consumer has no loop any more"));
	}
	FSTJob Loop;
	FString Why;
	if (!Consumer || !MakeLoopJob(State, *Consumer, Loop, Why))
	{
		const FString Reason = Consumer ? Why : State.IdleReason;
		if (State.bHasJob)
		{
			EndJob(State, *Reason);
		}
		if (State.Phase != ESTTrainPhase::Parked)
		{
			Park(State, *Reason);
		}
		State.IdleReason = Reason;
		return;
	}
	// The same loop as it runs: nothing to do.
	auto SameLegs = [](const FSTJob& A, const FSTJob& B)
	{
		if (A.Legs.Num() != B.Legs.Num())
		{
			return false;
		}
		for (int32 Leg = 0; Leg < A.Legs.Num(); ++Leg)
		{
			const FSTJobLeg& X = A.Legs[Leg];
			const FSTJobLeg& Y = B.Legs[Leg];
			if (X.Station != Y.Station || X.Work.Num() != Y.Work.Num() || X.SkipCars != Y.SkipCars)
			{
				return false;
			}
			for (int32 Work = 0; Work < X.Work.Num(); ++Work)
			{
				if (X.Work[Work].Car != Y.Work[Work].Car || X.Work[Work].Item != Y.Work[Work].Item || X.Work[Work].bLoad != Y.Work[Work].bLoad)
				{
					return false;
				}
			}
		}
		return true;
	};
	if (State.bHasJob && State.Job.Kind == ESTJobKind::Loop && SameLegs(State.Job, Loop))
	{
		return;
	}
	// Cargo its new loop doesn't carry in that car goes first: back to the consumer it served, then to a sink.
	if (After != ESTJobKind::Sink && HoldsOtherCargo(State, Loop))
	{
		AFGTrainStationIdentifier* Old = State.bHasJob && State.Job.Kind == ESTJobKind::Loop ? State.Job.AnchorStation.Get() : nullptr;
		FSTJob Unload;
		if (After != ESTJobKind::Deliver)
		{
			// Without a loop before (just managed, or loaded from an older save): any consumer whose platforms take it.
			if (!Old || !MakeUnloadJob(State, Old, ESTJobKind::Deliver, Unload))
			{
				float Best = TNumericLimits<float>::Max();
				for (const FSTConsumer& Each : mConsumers)
				{
					FSTJob Job;
					const float Travel = State.LastStation ? EstimateTravelSeconds(State.LastStation.Get(), Each.Station.Get(), IsPushPull(Train)) : 0.f;
					if (Travel >= 0.f && Travel < Best && MakeUnloadJob(State, Each.Station.Get(), ESTJobKind::Deliver, Job))
					{
						Best = Travel;
						Unload = MoveTemp(Job);
					}
				}
			}
			if (Unload.Legs.Num() > 0)
			{
				Unload.Id = mNextJobId++;
				Unload.StartedAt = Now();
				Unload.LegStartedAt = Unload.StartedAt;
				CommitJob(State, MoveTemp(Unload), FString::Printf(TEXT("%s: switching patterns to %s; %s first"), *STLayout::Name(Train),
					*STLayout::Name(Consumer->Station.Get()), *Unload.Summary));
				return;
			}
		}
		if (AFGTrainStationIdentifier* Sink = FindSinkStation(State); Sink && MakeUnloadJob(State, Sink, ESTJobKind::Sink, Unload))
		{
			Unload.Id = mNextJobId++;
			Unload.StartedAt = Now();
			Unload.LegStartedAt = Unload.StartedAt;
			CommitJob(State, MoveTemp(Unload), FString::Printf(TEXT("%s: switching patterns to %s; what no consumer took goes to a sink first (%s)"),
				*STLayout::Name(Train), *STLayout::Name(Consumer->Station.Get()), *Unload.Summary));
			return;
		}
		UE_LOG(LogSmartTrains, Warning, TEXT("LOOP %s: holds cargo its new loop doesn't carry and no consumer or Sink platform takes it; it starts its loop with it (/st emptycars clears it)"),
			*STLayout::Name(Train));
	}
	// The pattern its cars carry on the loop.
	State.CarSets.Reset();
	for (AFGFreightWagon* Car : GetCars(Train))
	{
		FSTCarItems& Items = State.CarSets.AddDefaulted_GetRef();
		for (const FSTCarWork& Work : Loop.Legs.Last().Work)
		{
			if (Work.Car == Car)
			{
				Items.Items.AddUnique(Work.Item);
			}
		}
	}
	Loop.Id = mNextJobId++;
	Loop.StartedAt = Now();
	Loop.LegStartedAt = Loop.StartedAt;
	State.IdleReason = Why;
	CommitJob(State, MoveTemp(Loop), FString::Printf(TEXT("%s: loop %s%s"), *STLayout::Name(Train), *Loop.Summary, Why.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Why)));
}

void ASTSubsystem::CommitJob(FSTTrainState& State, FSTJob&& Job, const FString& Line)
{
	if (mSettings.bObserveOnly)
	{
		FString& Last = mObserved.FindOrAdd(State.Train.Get());
		if (Last != Job.Summary)
		{
			Last = Job.Summary;
			UE_LOG(LogSmartTrains, Log, TEXT("WOULD JOB %s"), *Line);
		}
		return;
	}
	if (State.bHasJob)
	{
		UE_LOG(LogSmartTrains, Log, TEXT("JOB %d %s replaced: %s"), State.Job.Id, *STLayout::Name(State.Train), *State.Job.Summary);
	}
	// Docked at a stop of the new job: that stop is the one it is at, so the job goes on from the next.
	if (State.Train->IsDocked() && State.DockedStation.IsValid())
	{
		const int32 Here = Job.Legs.IndexOfByPredicate([&State](const FSTJobLeg& Leg) { return Leg.Station == State.DockedStation.Get(); });
		if (Here != INDEX_NONE)
		{
			Job.Leg = Here;
		}
	}
	State.Job = MoveTemp(Job);
	State.bHasJob = true;
	UE_LOG(LogSmartTrains, Log, TEXT("JOB %d %s"), State.Job.Id, *Line);
	WriteJob(State, TEXT("new job"));
}

void ASTSubsystem::WriteJob(FSTTrainState& State, const TCHAR* Why)
{
	AFGTrain* Train = State.Train;
	if (!IsValid(Train) || mSettings.bObserveOnly || !State.bHasJob || State.Job.Legs.Num() == 0)
	{
		return;
	}
	// Stops whose station was dismantled are left out; with none left the job ends.
	for (int32 Leg = State.Job.Legs.Num() - 1; Leg >= 0; --Leg)
	{
		if (!IsValid(State.Job.Legs[Leg].Station) || !State.Job.Legs[Leg].Station->GetStation())
		{
			UE_LOG(LogSmartTrains, Warning, TEXT("%s job %d: a stop's station is gone; left out"), *STLayout::Name(Train), State.Job.Id);
			State.Job.Legs.RemoveAt(Leg);
			State.Job.Leg = Leg < State.Job.Leg ? State.Job.Leg - 1 : State.Job.Leg;
		}
	}
	if (State.Job.Legs.Num() == 0)
	{
		State.bHasJob = false;
		State.Phase = ESTTrainPhase::Idle;
		State.WrittenStations.Reset();
		bCheckSoon = true;
		return;
	}
	State.Job.Leg = FMath::Clamp(State.Job.Leg, 0, State.Job.Legs.Num() - 1);
	AFGRailroadTimeTable* TimeTable = Train->GetTimeTable();
	if (!TimeTable)
	{
		TimeTable = Train->NewTimeTable();
	}
	if (!TimeTable)
	{
		UE_LOG(LogSmartTrains, Error, TEXT("%s: no timetable could be made; job %d dropped"), *STLayout::Name(Train), State.Job.Id);
		State.bHasJob = false;
		return;
	}
	// The stops from the one it drives to next. Docked at its current leg, that is the following one: the game moved its
	// timetable on when it docked (Docs/Architecture.md, "Timetables"). A loop writes all its stops, which the timetable
	// repeats; other jobs only the ones left.
	const bool bDockedHere = Train->IsDocked() && State.DockedStation.Get() == State.Job.Legs[State.Job.Leg].Station;
	const int32 Count = State.Job.Legs.Num();
	const int32 Start = bDockedHere ? State.Job.Leg + 1 : State.Job.Leg;
	TArray<int32> Order;
	if (State.Job.Kind == ESTJobKind::Loop)
	{
		for (int32 Offset = 0; Offset < Count; ++Offset)
		{
			Order.Add((Start + Offset) % Count);
		}
	}
	else
	{
		for (int32 Index = Start; Index < Count; ++Index)
		{
			Order.Add(Index);
		}
	}
	if (Order.Num() == 0)
	{
		// Docked at the last stop of a one-off job: nothing left to write; it ends when it leaves.
		return;
	}
	TArray<FTimeTableStop> Stops;
	TArray<int32> SkipStops;
	TArray<AFGRailroadVehicle*> SkipVehicles;
	TArray<FString> Keys;
	for (const int32 Index : Order)
	{
		const FSTJobLeg& Leg = State.Job.Legs[Index];
		FTimeTableStop& Stop = Stops.AddDefaulted_GetRef();
		Stop.Station = Leg.Station;
		Stop.DockingRuleSet = MakeRules(Leg, mSettings.bWaitForFullCars && State.Job.Kind == ESTJobKind::Loop);
		for (AFGRailroadVehicle* Car : Leg.SkipCars)
		{
			SkipStops.Add(Stops.Num() - 1);
			SkipVehicles.Add(Car);
		}
		Keys.Add(StopKey(Stop, Leg.SkipCars));
	}
	// The same stops it already drives: rewriting would only make the game (and Auto Trains) plan its route again.
	if (Train->IsSelfDrivingEnabled() && State.WrittenStopKeys == Keys && TimeTable->GetNumStops() == Keys.Num())
	{
		UE_LOG(LogSmartTrains, Verbose, TEXT("WRITE %s job %d: timetable already has these %d stops; not rewritten (%s)"), *STLayout::Name(Train), State.Job.Id, Keys.Num(), Why);
		State.Phase = ESTTrainPhase::Working;
		UpdateTrainName(State);
		return;
	}
	State.WrittenStations.Reset();
	for (const FTimeTableStop& Stop : Stops)
	{
		State.WrittenStations.Add(Stop.Station);
	}
	State.WrittenStopKeys = Keys;
	bool bByFcc = false;
	if (IsFccAvailable())
	{
		bByFcc = STFcc::SetStops(GetWorld(), TimeTable, Stops, SkipStops, SkipVehicles, TEXT("SmartTrains"));
		if (!bByFcc)
		{
			UE_LOG(LogSmartTrains, Warning, TEXT("%s: Freight Car Control's ExternalSetStops failed; writing the timetable without car rules"), *STLayout::Name(Train));
		}
	}
	if (!bByFcc)
	{
		if (SkipVehicles.Num() > 0)
		{
			UE_LOG(LogSmartTrains, Warning, TEXT("%s: job %d needs %d cars to skip but Freight Car Control isn't available"), *STLayout::Name(Train), State.Job.Id, SkipVehicles.Num());
		}
		TimeTable->SetStops(Stops);
	}
	TimeTable->SetCurrentStop(0);
	if (!Train->IsSelfDrivingEnabled() && !State.bHeld)
	{
		Train->SetSelfDrivingEnabled(true);
	}
	// Standing in its first stop's station, ready to dock: it docks where it stands (the game doesn't dock a train already
	// standing in the station it drives to; Docs/Architecture.md, "Docking in place").
	if (!Train->IsDocked() && Train->GetDockingState() == ETrainDockingState::TDS_ReadyToDock && STLayout::IsTrainAt(Train, Stops[0].Station))
	{
		Train->Dock();
		UE_LOG(LogSmartTrains, Log, TEXT("DOCK %s at %s: docks where it stands for job %d (docking state now %d)"), *STLayout::Name(Train), *STLayout::Name(Stops[0].Station),
			State.Job.Id, static_cast<int32>(Train->GetDockingState()));
	}
	State.Phase = ESTTrainPhase::Working;
	State.LastCommandAt = Now();
	FString Listed;
	for (const FTimeTableStop& Stop : Stops)
	{
		Listed += (Listed.IsEmpty() ? TEXT("") : TEXT(", ")) + STLayout::Name(Stop.Station);
	}
	UE_LOG(LogSmartTrains, Log, TEXT("WRITE %s job %d: [%s]%s, %d skipped cars (%s)"), *STLayout::Name(Train), State.Job.Id, *Listed,
		bByFcc ? TEXT(" through Freight Car Control") : TEXT(""), SkipVehicles.Num(), Why);
	STAt::TimeTableChanged(GetWorld(), Train);
	UpdateTrainName(State);
}

void ASTSubsystem::FinishJob(FSTTrainState& State)
{
	const FSTJob Done = State.Job;
	UE_LOG(LogSmartTrains, Log, TEXT("JOB %d %s DONE: %s in %.0f s, moved %d"), Done.Id, *STLayout::Name(State.Train), *Done.Summary, Now() - Done.StartedAt,
		Done.Legs.Num() > 0 ? Done.Legs.Last().GetMoved(false) : 0);
	State.bHasJob = false;
	State.Phase = ESTTrainPhase::Idle;
	UpdateTrainJob(State, Done.Kind);
}

void ASTSubsystem::Park(FSTTrainState& State, const TCHAR* Why)
{
	AFGTrain* Train = State.Train;
	if (!IsValid(Train) || mSettings.bObserveOnly)
	{
		return;
	}
	State.Phase = ESTTrainPhase::Parked;
	State.bHasJob = false;
	if (Train->IsSelfDrivingEnabled())
	{
		Train->SetSelfDrivingEnabled(false);
	}
	UE_LOG(LogSmartTrains, Log, TEXT("PARK %s at %s (%s)"), *STLayout::Name(Train), *STLayout::Name(State.LastStation.Get()), Why);
	UpdateTrainName(State);
}

bool ASTSubsystem::CanServe(const AFGTrain* Train, const AFGTrainStationIdentifier* Station) const
{
	const AFGBuildableRailroadStation* Building = Station ? Station->GetStation() : nullptr;
	return Building && IsValid(Train) && Building->GetTrackGraphID() == Train->GetTrackGraphID();
}

FString ASTSubsystem::SummarizeLegs(const TArray<FSTJobLeg>& Legs)
{
	TArray<FString> Items;
	TArray<FString> Stations;
	for (const FSTJobLeg& Leg : Legs)
	{
		for (const FSTCarWork& Work : Leg.Work)
		{
			Items.AddUnique(STNames::Item(Work.Item));
		}
		Stations.Add(STLayout::Name(Leg.Station.Get()));
	}
	return FString::Printf(TEXT("%s: %s"), *FString::Join(Items, TEXT(", ")), *FString::Join(Stations, TEXT(" -> ")));
}

//~ Fit model

TArray<TArray<AFGRailroadVehicle*>> ASTSubsystem::GetDockMaps(const AFGTrain* Train, const AFGTrainStationIdentifier* Station, bool* bLearned) const
{
	TArray<TArray<AFGRailroadVehicle*>> Learned;
	TArray<const AFGLocomotive*> LearnedBy;
	const TArray<AFGRailroadVehicle*> Vehicles = STLayout::GetVehicles(Train);
	for (const FSTDockMap& Seen : mDockMaps)
	{
		if (Seen.Train != Train || Seen.Station != Station)
		{
			continue;
		}
		// A seen map is only used while the train is made of the same vehicles in the same order.
		bool bValid = true;
		if (Seen.Consist.Num() > 0)
		{
			bValid = Seen.Consist.Num() == Vehicles.Num();
			for (int32 Index = 0; bValid && Index < Vehicles.Num(); ++Index)
			{
				bValid = Seen.Consist[Index] == Vehicles[Index];
			}
		}
		TArray<AFGRailroadVehicle*> Map;
		for (const TObjectPtr<AFGRailroadVehicle>& Vehicle : Seen.Vehicles)
		{
			bValid &= !Vehicle || Vehicles.Contains(Vehicle.Get());
			Map.Add(Vehicle);
		}
		if (bValid)
		{
			Learned.Add(MoveTemp(Map));
			LearnedBy.Add(Seen.Locomotive.Get());
		}
	}
	if (bLearned)
	{
		*bLearned = Learned.Num() > 0;
	}
	TArray<TArray<AFGRailroadVehicle*>> Predicted;
	TArray<const AFGLocomotive*> PredictedBy;
	const AFGBuildableRailroadStation* Building = Station ? Station->GetStation() : nullptr;
	const int32 PlatformCount = STLayout::GetPlatforms(Building).Num();
	for (AFGLocomotive* Locomotive : STLayout::GetPossibleDockingLocomotives(Train))
	{
		TArray<AFGRailroadVehicle*> Behind = STLayout::GetVehiclesBehind(Locomotive);
		Behind.SetNum(FMath::Min(Behind.Num(), PlatformCount));
		if (!Predicted.Contains(Behind))
		{
			Predicted.Add(MoveTemp(Behind));
			PredictedBy.Add(Locomotive);
		}
	}
	if (Learned.Num() == 0)
	{
		return Predicted;
	}
	// Maps that know their docking locomotive: each replaces the prediction for that locomotive; the other ways round
	// stay predicted until seen.
	if (!LearnedBy.Contains(nullptr))
	{
		TArray<TArray<AFGRailroadVehicle*>> Maps = Learned;
		for (int32 Index = 0; Index < Predicted.Num(); ++Index)
		{
			if (!LearnedBy.Contains(PredictedBy[Index]))
			{
				Maps.Add(Predicted[Index]);
			}
		}
		return Maps;
	}
	// A learned map stands for the way round it was seen; the other ways round stay predicted until seen. If a learned
	// map matches no prediction, the predictions are wrong for this station and only what was seen is used.
	auto SameWayRound = [](const TArray<AFGRailroadVehicle*>& A, const TArray<AFGRailroadVehicle*>& B)
	{
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (A[Index])
			{
				return B.IsValidIndex(Index) && B[Index] == A[Index];
			}
		}
		return false;
	};
	TArray<TArray<AFGRailroadVehicle*>> Maps = Learned;
	bool bAllMatch = true;
	for (const TArray<AFGRailroadVehicle*>& Seen : Learned)
	{
		bAllMatch &= Predicted.ContainsByPredicate([&](const TArray<AFGRailroadVehicle*>& Each) { return SameWayRound(Seen, Each); });
	}
	if (bAllMatch)
	{
		for (const TArray<AFGRailroadVehicle*>& Each : Predicted)
		{
			if (!Learned.ContainsByPredicate([&](const TArray<AFGRailroadVehicle*>& Seen) { return SameWayRound(Seen, Each); }))
			{
				Maps.Add(Each);
			}
		}
	}
	return Maps;
}

//~ Readouts of the plan

FString ASTSubsystem::DescribeWork(const FSTTrainState& State) const
{
	if (!State.bManaged)
	{
		return FString();
	}
	if (State.bHeld)
	{
		return TEXT("held");
	}
	if (State.bDepot)
	{
		const FSTConsumer* Consumer = FindConsumer(State.LoopConsumer.Get());
		return FString::Printf(TEXT("%s a depot signal: %s uses %.0f %% of its machines now"), State.Phase == ESTTrainPhase::Parked ? TEXT("waiting at") : TEXT("on its way to"),
			Consumer ? *STLayout::Name(Consumer->Station.Get()) : TEXT("its consumer"), Consumer ? 100.0 * Consumer->Use : 0.0);
	}
	if (!State.bHasJob)
	{
		return FString::Printf(TEXT("no loop: %s"), State.IdleReason.IsEmpty() ? TEXT("not planned yet") : *State.IdleReason);
	}
	const FSTJob& Job = State.Job;
	const FSTJobLeg* Leg = Job.Legs.IsValidIndex(Job.Leg) ? &Job.Legs[Job.Leg] : nullptr;
	FString Where;
	if (Leg)
	{
		Where = FString::Printf(TEXT(", to %s"), *STLayout::Name(Leg->Station));
	}
	switch (Job.Kind)
	{
	case ESTJobKind::Deliver:
		return FString::Printf(TEXT("switching patterns: delivering what it holds at %s"), *STLayout::Name(Job.AnchorStation.Get()));
	case ESTJobKind::Sink:
		return FString::Printf(TEXT("switching patterns: unloading what no consumer took at %s"), *STLayout::Name(Job.AnchorStation.Get()));
	default:
		return FString::Printf(TEXT("loop for %s (round %d, stop %d of %d%s)"), *STLayout::Name(Job.AnchorStation.Get()), Job.Rounds + 1, Job.Leg + 1, Job.Legs.Num(), *Where);
	}
}

FSTMonitorView ASTSubsystem::MakeMonitorView() const
{
	FSTMonitorView View;
	int32 Managed = 0;
	int32 Looping = 0;
	int32 Switching = 0;
	int32 Idle = 0;
	for (const FSTTrainState& State : mTrains)
	{
		if (!State.bManaged || !IsValid(State.Train))
		{
			continue;
		}
		++Managed;
		Looping += State.bHasJob && State.Job.Kind == ESTJobKind::Loop ? 1 : 0;
		Switching += State.bHasJob && State.Job.Kind != ESTJobKind::Loop ? 1 : 0;
		Idle += State.bHasJob ? 0 : 1;
		FSTMonitorRow& Row = View.Trains.AddDefaulted_GetRef();
		Row.Title = STLayout::Name(State.Train);
		Row.Verdict = State.bHeld ? TEXT("Held") : State.bDepot ? TEXT("At a depot") : !State.bHasJob ? TEXT("No loop") : State.Job.Kind == ESTJobKind::Loop ? TEXT("On its loop") : TEXT("Switching");
		Row.Severity = !State.bHasJob && !State.bHeld ? 1 : 0;
		Row.Columns.Add(State.LoopConsumer ? STLayout::Name(State.LoopConsumer.Get()) : FString(TEXT("-")));
		Row.Details.Add(DescribeWork(State));
	}
	View.Trains.Sort([](const FSTMonitorRow& A, const FSTMonitorRow& B) { return A.Severity != B.Severity ? A.Severity > B.Severity : A.Title < B.Title; });
	int32 Unserved = 0;
	double TotalNeed = 0.0;
	for (const FSTConsumer& Consumer : mConsumers)
	{
		TotalNeed += Consumer.Trains.Num() > 0 || Consumer.Target > 0 ? Consumer.Need : 0.0;
	}
	for (const FSTConsumer& Consumer : mConsumers)
	{
		FSTMonitorRow& Row = View.Consumers.AddDefaulted_GetRef();
		Row.Title = STLayout::Name(Consumer.Station.Get());
		TArray<FString> Pattern;
		for (const FSTLoopSlot& Slot : Consumer.Slots)
		{
			TArray<FString> Names;
			for (const TWeakObjectPtr<UClass>& Item : Slot.Items)
			{
				Names.Add(STNames::Item(Item.Get()));
			}
			Pattern.Add(FString::Printf(TEXT("P%d %s"), Slot.Index + 1, *FString::Join(Names, TEXT(" + "))));
		}
		const bool bServed = Consumer.Trains.Num() > 0;
		Unserved += bServed ? 0 : 1;
		Row.Verdict = FString(bServed ? TEXT("Served") : TEXT("Unserved"))
			+ (Consumer.Priority == ESTPriority::Medium ? FString() : FString::Printf(TEXT(" · %s priority"), STNames::Priority(Consumer.Priority)))
			+ (Consumer.Cause.IsEmpty() ? FString() : TEXT(" · ") + Consumer.Cause);
		Row.Severity = bServed ? (Consumer.Problem.IsEmpty() ? 0 : 1) : 2;
		Row.Columns.Add(FString::Printf(TEXT("%.2f"), Consumer.Rate));
		Row.Columns.Add(FString::Printf(TEXT("%.1f min"), Consumer.RoundTrip / 60.0));
		const bool bCounted = bServed || Consumer.Target > 0;
		Row.Columns.Add(bCounted && TotalNeed > 0.0 && Consumer.Need > 0.0 ? FString::Printf(TEXT("%.0f%%"), 100.0 * Consumer.Need / TotalNeed) : FString(TEXT("-")));
		Row.Columns.Add(FString::Printf(TEXT("%d"), Consumer.Trains.Num()));
		Row.Details.Add(FString::Printf(TEXT("Pattern: %s"), *FString::Join(Pattern, TEXT(" | "))));
		for (const FSTLoopSlot& Slot : Consumer.Slots)
		{
			Row.Pattern.Add(Slot.Items.Num() > 0 ? TSubclassOf<UFGItemDescriptor>(Slot.Items[0].Get()) : TSubclassOf<UFGItemDescriptor>());
		}
		Row.Trains = Consumer.Trains.Num();
		Row.Target = Consumer.Target;
		Row.AtDepot = Consumer.AtDepot;
		Row.Use = static_cast<float>(Consumer.Use);
		if (Consumer.AtDepot > 0)
		{
			Row.Details.Add(FString::Printf(TEXT("%d of its trains wait at depot signals: its machines want %.0f %% of their input now"), Consumer.AtDepot, 100.0 * Consumer.Use));
		}
		TArray<FString> Sources;
		for (const FSTLoopSource& Source : Consumer.Sources)
		{
			if (mSuppliers.IsValidIndex(Source.Supplier))
			{
				Sources.Add(FString::Printf(TEXT("%s from %s (%.2f car loads/min)"), *STNames::Item(Source.Item.Get()), *STLayout::Name(mSuppliers[Source.Supplier].Station.Get()), Source.Taken));
			}
		}
		if (Sources.Num() > 0)
		{
			Row.Details.Add(FString::Printf(TEXT("Suppliers: %s"), *FString::Join(Sources, TEXT(", "))));
		}
		if (!Consumer.Problem.IsEmpty())
		{
			Row.Details.Add(Consumer.Problem);
			Row.Advice = Consumer.Problem;
		}
	}
	View.Consumers.Sort([](const FSTMonitorRow& A, const FSTMonitorRow& B) { return A.Severity != B.Severity ? A.Severity > B.Severity : A.Title < B.Title; });
	for (const FSTSupplier& Supplier : mSuppliers)
	{
		FSTMonitorRow& Row = View.Suppliers.AddDefaulted_GetRef();
		Row.Title = STLayout::Name(Supplier.Station.Get());
		Row.Item = Supplier.Item.Get();
		const bool bOver = Supplier.Rate >= 0.0 && Supplier.Taken > Supplier.Rate * 1.01;
		Row.Verdict = Supplier.Taken <= 0.0 ? TEXT("Not used") : bOver ? TEXT("Over its rate") : TEXT("Used");
		Row.Severity = bOver ? 1 : 0;
		Row.Columns.Add(STNames::Item(Supplier.Item.Get()));
		Row.Columns.Add(Supplier.Rate >= 0.0 ? FString::Printf(TEXT("%.2f"), Supplier.Rate) : FString(TEXT("not known")));
		Row.Columns.Add(FString::Printf(TEXT("%.2f"), Supplier.Taken));
	}
	View.Suppliers.Sort([](const FSTMonitorRow& A, const FSTMonitorRow& B) { return A.Severity != B.Severity ? A.Severity > B.Severity : A.Title < B.Title; });
	auto Tile = [&View](const TCHAR* Label, int32 Value, uint8 Severity)
	{
		FSTMonitorRow& Row = View.Tiles.AddDefaulted_GetRef();
		Row.Title = Label;
		Row.Verdict = FString::FromInt(Value);
		Row.Severity = Severity;
	};
	Tile(TEXT("managed trains"), Managed, 0);
	Tile(TEXT("on their loop"), Looping, 0);
	Tile(TEXT("switching patterns"), Switching, 0);
	Tile(TEXT("without a loop"), Idle, Idle > 0 ? 1 : 0);
	int32 AtDepots = 0;
	for (const FSTTrainState& State : mTrains)
	{
		AtDepots += State.bManaged && State.bDepot ? 1 : 0;
	}
	Tile(TEXT("at depot signals"), AtDepots, 0);
	Tile(TEXT("consumers"), mConsumers.Num(), 0);
	Tile(TEXT("unserved consumers"), Unserved, Unserved > 0 ? 2 : 0);
	View.Summary.Add(FString::Printf(TEXT("%d managed trains: %d on their loop, %d switching patterns, %d without a loop"), Managed, Looping, Switching, Idle));
	View.Summary.Add(FString::Printf(TEXT("%d consumers (%d unserved), %d supplier items"), mConsumers.Num(), Unserved, mSuppliers.Num()));
	return View;
}

TArray<FString> ASTSubsystem::DescribeMonitor() const
{
	const FSTMonitorView View = MakeMonitorView();
	TArray<FString> Lines = View.Summary;
	for (const FSTMonitorRow& Row : View.Consumers)
	{
		Lines.Add(FString::Printf(TEXT("Consumer %s: %s, %s car loads/min, round trip %s, share %s, %s trains"), *Row.Title, *Row.Verdict, *Row.Columns[0], *Row.Columns[1],
			*Row.Columns[2], *Row.Columns[3]));
		for (const FString& Detail : Row.Details)
		{
			Lines.Add(TEXT("  ") + Detail);
		}
	}
	for (const FSTMonitorRow& Row : View.Trains)
	{
		if (Row.Severity > 0)
		{
			Lines.Add(FString::Printf(TEXT("Train %s: %s"), *Row.Title, Row.Details.Num() > 0 ? *Row.Details[0] : *Row.Verdict));
		}
	}
	return Lines;
}

TArray<FString> ASTSubsystem::DescribeJobs() const
{
	TArray<FString> Lines;
	for (const FSTTrainState& State : mTrains)
	{
		if (State.bManaged && IsValid(State.Train))
		{
			Lines.Add(FString::Printf(TEXT("%s: %s%s"), *STLayout::Name(State.Train), *DescribeWork(State),
				State.bHasJob ? *FString::Printf(TEXT(" [%s]"), *State.Job.Summary) : TEXT("")));
		}
	}
	if (Lines.Num() == 0)
	{
		Lines.Add(TEXT("No managed trains."));
	}
	return Lines;
}

TArray<FString> ASTSubsystem::DescribeFit(const AFGTrain* Train) const
{
	TArray<FString> Lines;
	if (!IsValid(Train))
	{
		Lines.Add(TEXT("No train."));
		return Lines;
	}
	Lines.Add(FString::Printf(TEXT("%s, cars %s"), *STLayout::Name(Train), *STLayout::GetLayoutKey(Train)));
	for (const FSTConsumer& Consumer : mConsumers)
	{
		TArray<AFGFreightWagon*> Cars;
		FString Why;
		const bool bFits = FitsConsumer(Train, Consumer, Cars, Why);
		Lines.Add(FString::Printf(TEXT("%s: %s"), *STLayout::Name(Consumer.Station.Get()), bFits ? TEXT("fits") : *FString::Printf(TEXT("doesn't fit (%s)"), *Why)));
	}
	if (mConsumers.Num() == 0)
	{
		Lines.Add(TEXT("No consumers yet: give a station's unloading platforms the Demand role."));
	}
	return Lines;
}

TArray<FString> ASTSubsystem::DescribePatterns() const
{
	TArray<FString> Lines;
	for (const FSTConsumer& Consumer : mConsumers)
	{
		TArray<FString> Pattern;
		for (const FSTLoopSlot& Slot : Consumer.Slots)
		{
			TArray<FString> Names;
			for (const TWeakObjectPtr<UClass>& Item : Slot.Items)
			{
				Names.Add(STNames::Item(Item.Get()));
			}
			Pattern.Add(FString::Printf(TEXT("P%d %s"), Slot.Index + 1, *FString::Join(Names, TEXT(" + "))));
		}
		TArray<FString> Trains;
		for (const TWeakObjectPtr<AFGTrain>& Train : Consumer.Trains)
		{
			Trains.Add(STLayout::Name(Train.Get()));
		}
		Lines.Add(FString::Printf(TEXT("%s [%s]: %d of %d trains%s"), *STLayout::Name(Consumer.Station.Get()), *FString::Join(Pattern, TEXT(" | ")), Consumer.Trains.Num(),
			Consumer.Target, Trains.Num() > 0 ? *FString::Printf(TEXT(": %s"), *FString::Join(Trains, TEXT(", "))) : TEXT("")));
	}
	if (Lines.Num() == 0)
	{
		Lines.Add(TEXT("No consumers yet: give a station's unloading platforms the Demand role."));
	}
	return Lines;
}

//~ Train readout layout

float ASTSubsystem::SecondsToStop(const FSTTrainState& State) const
{
	const FSTJob& Job = State.Job;
	const AFGTrainStationIdentifier* Next = State.bHasJob && Job.Legs.IsValidIndex(Job.Leg) ? Job.Legs[Job.Leg].Station.Get() : nullptr;
	if (!Next || !IsValid(State.Train))
	{
		return -1.f;
	}
	if (State.Train->IsDocked() && State.DockedStation.Get() == Next)
	{
		return 0.f;
	}
	const float Travel = State.LastStation ? EstimateTravelSeconds(State.LastStation.Get(), Next, IsPushPull(State.Train)) : -1.f;
	return Travel < 0.f ? -1.f : FMath::Max(0.f, Travel - static_cast<float>(Now() - State.LastDepartureAt));
}

void ASTSubsystem::FillTrainLayout(const FSTTrainState* State, FSTTrainView& View) const
{
	if (!State || !State->bManaged)
	{
		View.State = 0;
		return;
	}
	View.State = State->bHeld ? 3 : State->bDepot ? 5 : !State->bHasJob ? 4 : State->Job.Kind == ESTJobKind::Loop ? 1 : 2;
	const FSTConsumer* Consumer = FindConsumer(State->LoopConsumer.Get());
	if (Consumer)
	{
		View.Consumer = STLayout::Name(Consumer->Station.Get());
		View.PatternTrains = Consumer->Trains.Num();
		View.PatternTarget = Consumer->Target;
		View.PatternAtDepot = Consumer->AtDepot;
		View.Rate = static_cast<float>(Consumer->Rate);
		View.RoundTrip = static_cast<float>(Consumer->RoundTrip);
		View.Use = static_cast<float>(Consumer->Use);
	}
	AFGTrain* Train = State->Train;
	const bool bDocked = IsValid(Train) && Train->IsDocked();
	const FSTJob& Job = State->Job;
	const FSTJobLeg* Here = State->bHasJob && Job.Legs.IsValidIndex(Job.Leg) ? &Job.Legs[Job.Leg] : nullptr;
	// The cars of its pattern in train order: the item each delivers on its loop (or carries).
	for (AFGFreightWagon* Car : GetCars(Train))
	{
		UClass* Item = nullptr;
		if (State->bHasJob && Job.Legs.Num() > 0)
		{
			for (const FSTCarWork& Work : Job.Legs.Last().Work)
			{
				Item = Work.Car == Car ? Work.Item.Get() : Item;
			}
		}
		if (!Item)
		{
			for (const TPair<UClass*, int32>& Held : GetCarCargo(Car))
			{
				Item = Held.Key;
			}
		}
		if (!Item)
		{
			continue;
		}
		FSTCarView& Each = View.Cars.AddDefaulted_GetRef();
		Each.Item = Item;
		Each.Amount = CountInCar(Car, Item);
		Each.Capacity = static_cast<int32>(STLayout::GetCarCapacity(Car, Item));
		Each.bFluid = STLayout::GetCargoType(Car) == EFreightCargoType::FCT_Liquid;
		Each.bActive = bDocked && Here && State->DockedStation.Get() == Here->Station.Get()
			&& Here->Work.ContainsByPredicate([Car](const FSTCarWork& Work) { return Work.Car == Car; });
	}
	for (int32 Leg = 0; State->bHasJob && Leg < Job.Legs.Num(); ++Leg)
	{
		const FSTJobLeg& Each = Job.Legs[Leg];
		FSTStopView& Stop = View.Stops.AddDefaulted_GetRef();
		Stop.Station = STLayout::Name(Each.Station.Get());
		Stop.Kind = Each.GetItems(true).Num() > 0 ? 0 : Each.GetItems(false).Num() > 0 ? 1 : 2;
		for (UClass* Item : Each.GetItems(Stop.Kind == 0))
		{
			Stop.Items.Add(Item);
		}
		Stop.bCurrent = Leg == Job.Leg;
		Stop.bDocked = Stop.bCurrent && bDocked && State->DockedStation.Get() == Each.Station.Get();
	}
	View.NextSeconds = SecondsToStop(*State);
}
