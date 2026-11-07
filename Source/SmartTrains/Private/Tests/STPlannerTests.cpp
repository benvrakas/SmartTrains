// Automation tests for the trip planner, the flow plan and the assignment (Docs/Testing.md, "Automated tests").
// Run in the editor: Tools/BuildScripts/test-smarttrains.bat, or "Automation RunTests SmartTrains" in its console.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGTrainStationIdentifier.h"
#include "Misc/AutomationTest.h"
#include "STMinCostFlow.h"
#include "STSubsystem.h"
#include "STTripPlanner.h"
#include "Tests/STTestItems.h"

// Builds planner situations without a game world: stations, learned trip times, platforms, cars and the platform
// each car meets for every way the train may dock.
struct FSTTestAccess
{
	using FCarInfo = FSTTripPlanner::FCarInfo;
	using FPlatformInfo = FSTTripPlanner::FPlatformInfo;
	using FCarPlatforms = TMap<const AFGTrainStationIdentifier*, TArray<TArray<int32>>>;

	static ASTSubsystem* MakeSubsystem()
	{
		ASTSubsystem* Subsystem = NewObject<ASTSubsystem>(GetTransientPackage());
		Subsystem->mSettings = FSTSettings();
		Subsystem->mSettings.MaxTripMinutes = 120.f;
		Subsystem->mSettings.ArrivalGapSeconds = 30.f;
		Subsystem->mTrains.AddDefaulted();
		return Subsystem;
	}

	static void SetQueueTrains(ASTSubsystem& Subsystem, float Trains)
	{
		Subsystem.mSettings.bLeaveAtOnce = true;
		Subsystem.mSettings.QueueTrains = Trains;
	}

	static AFGTrainStationIdentifier* MakeStation()
	{
		return NewObject<AFGTrainStationIdentifier>(GetTransientPackage());
	}

	static void SetTrip(ASTSubsystem& Subsystem, AFGTrainStationIdentifier* From, AFGTrainStationIdentifier* To, float Seconds)
	{
		FSTTripTime& Trip = Subsystem.mTrips.AddDefaulted_GetRef();
		Trip.From = From;
		Trip.To = To;
		Trip.Seconds = Seconds;
		Trip.Samples = 1;
	}

	static void SetTrain(FSTTripPlanner& Planner, const AFGTrainStationIdentifier* Anchor, bool bFcc, bool bPushPull, const TArray<FCarInfo>& Cars,
		const TArray<FPlatformInfo>& Platforms, const FCarPlatforms& CarPlatforms)
	{
		Planner.mTrainIndex = 0;
		Planner.mAnchor = Anchor;
		Planner.mFreeAt = Planner.mTime;
		Planner.bAnchorDocked = false;
		Planner.bFcc = bFcc;
		Planner.bPushPull = bPushPull;
		Planner.bRelaxed = false;
		Planner.mCars = Cars;
		Planner.mPlatforms = Platforms;
		Planner.mCarPlatforms = CarPlatforms;
		Planner.mProblems.Reset();
		Planner.mPlatformIndex.Reset();
		for (int32 Index = 0; Index < Platforms.Num(); ++Index)
		{
			if (Platforms[Index].Platform)
			{
				Planner.mPlatformIndex.Add(Platforms[Index].Platform, Index);
			}
		}
	}

	static double FirstFreeArrival(const FSTTripPlanner& Planner, const AFGTrainStationIdentifier* Station, double Arrive, double Stay)
	{
		return Planner.FirstFreeArrival(Station, Arrive, Stay);
	}

	static void ServeAll(FSTTripPlanner& Planner)
	{
		Planner.bServeAll = true;
	}

	static FPlatformInfo Platform(UClass* Item, bool bSupply, double Stock, int32 Occupied)
	{
		FPlatformInfo Info;
		Info.bConfigured = true;
		Info.bSupply = bSupply;
		Info.bDemand = !bSupply;
		Info.bLoadMode = bSupply;
		Info.bFluid = STNames::IsFluid(Item);
		Info.Items.Add(Item);
		if (Stock > 0.0)
		{
			Info.Stock.Add(Item, static_cast<int32>(Stock));
		}
		Info.Slots = 1;
		Info.Occupied = Occupied;
		return Info;
	}

	static FCarInfo Car(UClass* Carried = nullptr, double Amount = 0.0)
	{
		FCarInfo Info;
		Info.Slots = 1;
		if (Carried && Amount > 0.0)
		{
			Info.Cargo.Emplace(Carried, Amount);
		}
		return Info;
	}

	static FSTNode Node(AFGTrainStationIdentifier* Station, UClass* Item, ESTRole Role, int32 Stock, int32 Capacity, int32 Platforms)
	{
		FSTNode Node;
		Node.Station = Station;
		Node.Item = Item;
		Node.Role = Role;
		Node.Stock = Stock;
		Node.Capacity = Capacity;
		Node.TargetPercent = 50.f;
		Node.SendAtPercent = 1.f;
		Node.bRateKnown = true;
		Node.Platforms.SetNum(Platforms);
		return Node;
	}
};

namespace
{
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	// A fluid platform or car in the tests: one slot of the test fluid's stack.
	constexpr int32 FluidSlot = 2400000;
}

// Bookings with a start and an end: the first slot that keeps the arrival gap to every other train's booking.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTFirstFreeArrivalTest, "SmartTrains.Planner.FirstFreeArrival", TestFlags)
bool FSTFirstFreeArrivalTest::RunTest(const FString& Parameters)
{
	ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
	AFGTrainStationIdentifier* Station = FSTTestAccess::MakeStation();
	TArray<FSTNode> Nodes;
	TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;
	Bookings.Add(Station, {{5, 100.0, 200.0}, {6, 400.0, 500.0}});
	FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);
	FSTTestAccess::SetTrain(Planner, Station, true, false, {}, {}, {});

	TestEqual(TEXT("fits before the first booking"), FSTTestAccess::FirstFreeArrival(Planner, Station, 0.0, 50.0), 0.0);
	TestEqual(TEXT("waits for the first booking plus the gap"), FSTTestAccess::FirstFreeArrival(Planner, Station, 150.0, 50.0), 230.0);
	TestEqual(TEXT("too long for the gap between bookings"), FSTTestAccess::FirstFreeArrival(Planner, Station, 300.0, 80.0), 530.0);
	TestEqual(TEXT("free station"), FSTTestAccess::FirstFreeArrival(Planner, FSTTestAccess::MakeStation(), 42.0, 50.0), 42.0);
	return true;
}

// A push-pull train at identical platforms meets a different platform in each way round: every car still works, and
// each platform's stock and room count once per way round (the water shuttle bugs of 2026-11-05).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTPushPullIdenticalPlatformsTest, "SmartTrains.Planner.PushPullIdenticalPlatforms", TestFlags)
bool FSTPushPullIdenticalPlatformsTest::RunTest(const FString& Parameters)
{
	UClass* Water = USTTestFluid::StaticClass();
	ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
	AFGTrainStationIdentifier* Start = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Source = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Sink = FSTTestAccess::MakeStation();
	FSTTestAccess::SetTrip(*Subsystem, Start, Source, 60.f);
	FSTTestAccess::SetTrip(*Subsystem, Source, Sink, 60.f);

	TArray<FSTNode> Nodes = {FSTTestAccess::Node(Source, Water, ESTRole::Supply, 4 * 1000000, 4 * FluidSlot, 4),
		FSTTestAccess::Node(Sink, Water, ESTRole::Demand, 0, 4 * FluidSlot, 4)};
	TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;
	FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);

	TArray<FSTTestAccess::FPlatformInfo> Platforms;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Platforms.Add(FSTTestAccess::Platform(Water, true, 1000000.0, 1));
	}
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Platforms.Add(FSTTestAccess::Platform(Water, false, 0.0, 0));
	}
	FSTTestAccess::FCarPlatforms CarPlatforms;
	CarPlatforms.Add(Source, {{0, 1, 2, 3}, {3, 2, 1, 0}});
	CarPlatforms.Add(Sink, {{4, 5, 6, 7}, {7, 6, 5, 4}});
	FSTTestAccess::SetTrain(Planner, Start, true, true, {FSTTestAccess::Car(), FSTTestAccess::Car(), FSTTestAccess::Car(), FSTTestAccess::Car()}, Platforms,
		CarPlatforms);

	FSTPlanStop Load;
	Load.Station = Source;
	Load.LoadItems.Add(Water);
	FSTPlanStop Unload;
	Unload.Station = Sink;
	Unload.UnloadItems.Add(Water);
	const FSTPlanResult Result = Planner.Simulate({Load, Unload});
	if (!TestTrue(FString::Printf(TEXT("trip works (%s)"), *Result.Problem), Result.bValid))
	{
		return false;
	}
	for (int32 CarIndex = 0; CarIndex < 4; ++CarIndex)
	{
		const FSTPlanCarStep& AtSource = Result.Steps[0][CarIndex];
		const FSTPlanCarStep& AtSink = Result.Steps[1][CarIndex];
		TestFalse(FString::Printf(TEXT("car %d isn't skipped at the source"), CarIndex + 1), AtSource.bSkip);
		TestEqual(FString::Printf(TEXT("car %d loads its platform's stock"), CarIndex + 1), AtSource.Loaded.Num() > 0 ? AtSource.Loaded[0].Value : 0, 1000000);
		TestEqual(FString::Printf(TEXT("car %d unloads it all"), CarIndex + 1), AtSink.Unloaded.Num() > 0 ? AtSink.Unloaded[0].Value : 0, 1000000);
	}
	return true;
}

// Without Freight Car Control no car can skip: a car beside a loading platform that isn't a supply platform for the
// item makes the trip invalid. With it, that car skips the stop.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTWrongPlatformTest, "SmartTrains.Planner.WrongPlatformNeedsSkip", TestFlags)
bool FSTWrongPlatformTest::RunTest(const FString& Parameters)
{
	UClass* Water = USTTestFluid::StaticClass();
	ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
	AFGTrainStationIdentifier* Start = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Source = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Sink = FSTTestAccess::MakeStation();
	FSTTestAccess::SetTrip(*Subsystem, Start, Source, 60.f);
	FSTTestAccess::SetTrip(*Subsystem, Source, Sink, 60.f);
	TArray<FSTNode> Nodes = {FSTTestAccess::Node(Source, Water, ESTRole::Supply, 1000000, FluidSlot, 1),
		FSTTestAccess::Node(Sink, Water, ESTRole::Demand, 0, 2 * FluidSlot, 2)};
	TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;

	// Platform 1 loads water from its stock but has no supply role.
	TArray<FSTTestAccess::FPlatformInfo> Platforms = {FSTTestAccess::Platform(Water, true, 1000000.0, 1), FSTTestAccess::Platform(Water, true, 500000.0, 1),
		FSTTestAccess::Platform(Water, false, 0.0, 0), FSTTestAccess::Platform(Water, false, 0.0, 0)};
	Platforms[1].bSupply = false;
	Platforms[1].bConfigured = false;
	Platforms[1].Items.Reset();
	FSTTestAccess::FCarPlatforms CarPlatforms;
	CarPlatforms.Add(Source, {{0, 1}});
	CarPlatforms.Add(Sink, {{2, 3}});
	FSTPlanStop Load;
	Load.Station = Source;
	Load.LoadItems.Add(Water);
	FSTPlanStop Unload;
	Unload.Station = Sink;
	Unload.UnloadItems.Add(Water);

	{
		FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);
		FSTTestAccess::SetTrain(Planner, Start, false, false, {FSTTestAccess::Car(), FSTTestAccess::Car()}, Platforms, CarPlatforms);
		const FSTPlanResult Result = Planner.Simulate({Load, Unload});
		TestFalse(TEXT("without Freight Car Control the trip is invalid"), Result.bValid);
		TestTrue(TEXT("and says it needs Freight Car Control"), Result.Problem.Contains(TEXT("Freight Car Control")));
	}
	{
		FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);
		FSTTestAccess::SetTrain(Planner, Start, true, false, {FSTTestAccess::Car(), FSTTestAccess::Car()}, Platforms, CarPlatforms);
		const FSTPlanResult Result = Planner.Simulate({Load, Unload});
		if (TestTrue(FString::Printf(TEXT("with Freight Car Control the trip works (%s)"), *Result.Problem), Result.bValid))
		{
			TestTrue(TEXT("the car beside the unconfigured platform skips"), Result.Steps[0][1].bSkip);
			TestEqual(TEXT("the other car loads"), Result.Steps[0][0].Loaded.Num() > 0 ? Result.Steps[0][0].Loaded[0].Value : 0, 1000000);
		}
	}
	return true;
}

// A car holding one item never loads another into its free slots, and never stops beside an unloading platform
// that would take cargo it didn't plan to unload there: both leave mixed cars (the aluminium and concrete cars of
// 2026-11-05). Empty cars still load.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTNoMixedCarsTest, "SmartTrains.Planner.NoMixedCars", TestFlags)
bool FSTNoMixedCarsTest::RunTest(const FString& Parameters)
{
	UClass* Ingot = USTTestSolid::StaticClass();
	UClass* Concrete = USTTestSolidB::StaticClass();
	ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
	AFGTrainStationIdentifier* Start = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Source = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Sink = FSTTestAccess::MakeStation();
	FSTTestAccess::SetTrip(*Subsystem, Start, Source, 60.f);
	FSTTestAccess::SetTrip(*Subsystem, Source, Sink, 60.f);
	TArray<FSTNode> Nodes = {FSTTestAccess::Node(Source, Concrete, ESTRole::Supply, 100000, 100000, 2),
		FSTTestAccess::Node(Sink, Concrete, ESTRole::Demand, 0, 100000, 2)};
	TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;
	TArray<FSTTestAccess::FPlatformInfo> Platforms = {FSTTestAccess::Platform(Concrete, true, 50000.0, 1), FSTTestAccess::Platform(Concrete, true, 50000.0, 1),
		FSTTestAccess::Platform(Concrete, false, 0.0, 0), FSTTestAccess::Platform(Concrete, false, 0.0, 0)};
	for (FSTTestAccess::FPlatformInfo& Platform : Platforms)
	{
		Platform.Slots = 10;
	}
	FSTTestAccess::FCarPlatforms CarPlatforms;
	CarPlatforms.Add(Source, {{0, 1}});
	CarPlatforms.Add(Sink, {{2, 3}});
	FSTPlanStop Load;
	Load.Station = Source;
	Load.LoadItems.Add(Concrete);
	FSTPlanStop Unload;
	Unload.Station = Sink;
	Unload.UnloadItems.Add(Concrete);

	// Car 1 is empty; car 2 holds one stack of ingots and has a free slot.
	FSTTestAccess::FCarInfo Empty = FSTTestAccess::Car();
	Empty.Slots = 2;
	FSTTestAccess::FCarInfo Holding = FSTTestAccess::Car(Ingot, 100.0);
	Holding.Slots = 2;
	{
		FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);
		FSTTestAccess::SetTrain(Planner, Start, true, false, {Empty, Holding}, Platforms, CarPlatforms);
		const FSTPlanResult Result = Planner.Simulate({Load, Unload});
		if (TestTrue(FString::Printf(TEXT("the trip works with Freight Car Control (%s)"), *Result.Problem), Result.bValid))
		{
			TestEqual(TEXT("the empty car loads two stacks of concrete"), Result.Steps[0][0].Loaded.Num() > 0 ? Result.Steps[0][0].Loaded[0].Value : 0, 1000);
			TestTrue(TEXT("the car holding ingots skips the pickup"), Result.Steps[0][1].bSkip);
			TestTrue(TEXT("and skips the concrete consumer, which would take its ingots too"), Result.Steps[1][1].bSkip);
		}
	}
	{
		FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);
		FSTTestAccess::SetTrain(Planner, Start, false, false, {Empty, Holding}, Platforms, CarPlatforms);
		const FSTPlanResult Result = Planner.Simulate({Load, Unload});
		TestFalse(TEXT("without Freight Car Control the car holding ingots makes the trip invalid"), Result.bValid);
	}
	return true;
}

// Pickups promised at one platform don't use up another platform's stock: a train whose car stops beside a full
// platform loads from it though other trains have promised the station's stock at its other platform (the sulfur
// stations of 2026-11-06, where most trains' 4th car was a fluid car).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTPlatformReservationsTest, "SmartTrains.Planner.PlatformReservations", TestFlags)
bool FSTPlatformReservationsTest::RunTest(const FString& Parameters)
{
	UClass* Sulfur = USTTestSolid::StaticClass();
	ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
	AFGTrainStationIdentifier* Start = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Source = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Sink = FSTTestAccess::MakeStation();
	FSTTestAccess::SetTrip(*Subsystem, Start, Source, 60.f);
	FSTTestAccess::SetTrip(*Subsystem, Source, Sink, 60.f);
	AFGBuildableTrainPlatformCargo* Near = NewObject<AFGBuildableTrainPlatformCargo>(GetTransientPackage());
	AFGBuildableTrainPlatformCargo* Far = NewObject<AFGBuildableTrainPlatformCargo>(GetTransientPackage());
	// The station holds 1000 in all, all of it at the far platform; another train has promised 1000 at the near one.
	TArray<FSTNode> Nodes = {FSTTestAccess::Node(Source, Sulfur, ESTRole::Supply, 1000, 2000, 0), FSTTestAccess::Node(Sink, Sulfur, ESTRole::Demand, 0, 2000, 1)};
	Nodes[0].Platforms = {Near, Far};
	Nodes[0].Reserved = 1000;
	TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;
	TArray<FSTTestAccess::FPlatformInfo> Platforms = {FSTTestAccess::Platform(Sulfur, true, 0.0, 0), FSTTestAccess::Platform(Sulfur, true, 1000.0, 10),
		FSTTestAccess::Platform(Sulfur, false, 0.0, 0)};
	Platforms[0].Platform = Near;
	Platforms[1].Platform = Far;
	for (FSTTestAccess::FPlatformInfo& Platform : Platforms)
	{
		Platform.Slots = 10;
	}
	FSTTestAccess::FCarPlatforms CarPlatforms;
	CarPlatforms.Add(Source, {{1}});
	CarPlatforms.Add(Sink, {{2}});
	FSTPlanStop Load;
	Load.Station = Source;
	Load.LoadItems.Add(Sulfur);
	FSTPlanStop Unload;
	Unload.Station = Sink;
	Unload.UnloadItems.Add(Sulfur);
	FSTTestAccess::FCarInfo Car = FSTTestAccess::Car();
	Car.Slots = 10;

	// The promise made at the near platform: the car beside the far one still loads its stock.
	Nodes[0].PlatformReserved.Add(Near, 1000);
	{
		FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);
		FSTTestAccess::SetTrain(Planner, Start, true, false, {Car}, Platforms, CarPlatforms);
		const FSTPlanResult Result = Planner.Simulate({Load, Unload});
		if (TestTrue(FString::Printf(TEXT("the trip works (%s)"), *Result.Problem), Result.bValid))
		{
			TestEqual(TEXT("the car loads the far platform's stock"), Result.Steps[0][0].Loaded.Num() > 0 ? Result.Steps[0][0].Loaded[0].Value : 0, 1000);
		}
	}
	// A promise at no known platform (older saves) is shared out as before: the station's 1000 are all promised.
	Nodes[0].PlatformReserved.Reset();
	{
		FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);
		FSTTestAccess::SetTrain(Planner, Start, true, false, {Car}, Platforms, CarPlatforms);
		const FSTPlanResult Result = Planner.Simulate({Load, Unload});
		TestTrue(TEXT("without a platform the promise still counts"), !Result.bValid || Result.Steps[0][0].Loaded.Num() == 0 || Result.Steps[0][0].Loaded[0].Value == 0);
	}
	return true;
}

// Deliveries promised to one platform use up its room only: a car beside a platform another train is already
// filling unloads nothing there, and one beside a platform with room delivers.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTDeliveryReservationsTest, "SmartTrains.Planner.DeliveryReservations", TestFlags)
bool FSTDeliveryReservationsTest::RunTest(const FString& Parameters)
{
	UClass* Ingot = USTTestSolid::StaticClass();
	ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
	AFGTrainStationIdentifier* Start = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Sink = FSTTestAccess::MakeStation();
	FSTTestAccess::SetTrip(*Subsystem, Start, Sink, 60.f);
	AFGBuildableTrainPlatformCargo* Filled = NewObject<AFGBuildableTrainPlatformCargo>(GetTransientPackage());
	AFGBuildableTrainPlatformCargo* Open = NewObject<AFGBuildableTrainPlatformCargo>(GetTransientPackage());
	TArray<FSTNode> Nodes = {FSTTestAccess::Node(Sink, Ingot, ESTRole::Demand, 0, 2000, 0)};
	Nodes[0].Platforms = {Filled, Open};
	Nodes[0].Reserved = 1000;
	Nodes[0].PlatformReserved.Add(Filled, 1000);
	TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;
	TArray<FSTTestAccess::FPlatformInfo> Platforms = {FSTTestAccess::Platform(Ingot, false, 0.0, 0), FSTTestAccess::Platform(Ingot, false, 0.0, 0)};
	Platforms[0].Platform = Filled;
	Platforms[1].Platform = Open;
	for (FSTTestAccess::FPlatformInfo& Platform : Platforms)
	{
		Platform.Slots = 10;
	}
	FSTPlanStop Unload;
	Unload.Station = Sink;
	Unload.UnloadItems.Add(Ingot);
	FSTTestAccess::FCarInfo Car = FSTTestAccess::Car(Ingot, 1000.0);
	Car.Slots = 10;
	for (int32 Beside = 0; Beside < 2; ++Beside)
	{
		FSTTestAccess::FCarPlatforms CarPlatforms;
		CarPlatforms.Add(Sink, {{Beside}});
		FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);
		FSTTestAccess::SetTrain(Planner, Start, true, false, {Car}, Platforms, CarPlatforms);
		const FSTPlanResult Result = Planner.Simulate({Unload});
		const int32 Unloaded = Result.bValid && Result.Steps[0][0].Unloaded.Num() > 0 ? Result.Steps[0][0].Unloaded[0].Value : 0;
		if (Beside == 0)
		{
			TestEqual(TEXT("beside the platform another train fills, nothing is unloaded"), Unloaded, 0);
		}
		else
		{
			TestEqual(TEXT("beside the platform with room, the car delivers"), Unloaded, 1000);
		}
	}
	return true;
}

// A full car is worth the same whatever it carries: fluids count in litres, so a fluid car once outscored a solid one
// about 500 times (server log of 2026-11-06).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTSlotValueTest, "SmartTrains.Planner.SlotValue", TestFlags)
bool FSTSlotValueTest::RunTest(const FString& Parameters)
{
	ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
	AFGTrainStationIdentifier* Start = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Sink = FSTTestAccess::MakeStation();
	FSTTestAccess::SetTrip(*Subsystem, Start, Sink, 60.f);
	auto ValueOf = [&](UClass* Item, double Full)
	{
		TArray<FSTNode> Nodes = {FSTTestAccess::Node(Sink, Item, ESTRole::Demand, 0, 100000000, 1)};
		TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;
		FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);
		FSTTestAccess::FPlatformInfo Platform = FSTTestAccess::Platform(Item, false, 0.0, 0);
		Platform.Slots = STNames::IsFluid(Item) ? 1 : 48;
		FSTTestAccess::FCarInfo Car = FSTTestAccess::Car(Item, Full);
		Car.Slots = 32;
		FSTTestAccess::FCarPlatforms CarPlatforms;
		CarPlatforms.Add(Sink, {{0}});
		FSTTestAccess::SetTrain(Planner, Start, true, false, {Car}, {Platform}, CarPlatforms);
		FSTPlanStop Unload;
		Unload.Station = Sink;
		Unload.UnloadItems.Add(Item);
		const FSTPlanResult Result = Planner.Simulate({Unload});
		return Result.bValid ? Result.Value : -1.0;
	};
	const double Solid = ValueOf(USTTestSolid::StaticClass(), 32 * 100.0);
	const double Fluid = ValueOf(USTTestFluid::StaticClass(), 1600000.0);
	TestTrue(FString::Printf(TEXT("a full solid car is worth something (%.3f)"), Solid), Solid > 0.0);
	TestTrue(FString::Printf(TEXT("a full fluid car is worth the same as a full solid car (%.3f and %.3f)"), Fluid, Solid), FMath::IsNearlyEqual(Solid, Fluid, Solid * 0.01));
	return true;
}

// Leaving at once, the queue limit counts the trains a train would wait behind, not the seconds: one docked train and
// one waiting ahead are fine with the default of 2; a third is too many.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTQueueTrainsTest, "SmartTrains.Planner.QueueTrains", TestFlags)
bool FSTQueueTrainsTest::RunTest(const FString& Parameters)
{
	UClass* Ingot = USTTestSolid::StaticClass();
	ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
	FSTTestAccess::SetQueueTrains(*Subsystem, 2.f);
	AFGTrainStationIdentifier* Start = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Sink = FSTTestAccess::MakeStation();
	FSTTestAccess::SetTrip(*Subsystem, Start, Sink, 60.f);
	auto Try = [&](int32 Ahead)
	{
		TArray<FSTNode> Nodes = {FSTTestAccess::Node(Sink, Ingot, ESTRole::Demand, 0, 100000, 1)};
		TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;
		for (int32 Index = 0; Index < Ahead; ++Index)
		{
			// Back to back from now: the first is docked, the others wait for it.
			Bookings.FindOrAdd(Sink).Add({10 + Index, Index * 100.0, Index * 100.0 + 90.0});
		}
		FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);
		FSTTestAccess::FPlatformInfo Platform = FSTTestAccess::Platform(Ingot, false, 0.0, 0);
		Platform.Slots = 48;
		FSTTestAccess::FCarInfo Car = FSTTestAccess::Car(Ingot, 3200.0);
		Car.Slots = 32;
		FSTTestAccess::FCarPlatforms CarPlatforms;
		CarPlatforms.Add(Sink, {{0}});
		FSTTestAccess::SetTrain(Planner, Start, true, false, {Car}, {Platform}, CarPlatforms);
		FSTPlanStop Unload;
		Unload.Station = Sink;
		Unload.UnloadItems.Add(Ingot);
		return Planner.Simulate({Unload});
	};
	const FSTPlanResult Two = Try(2);
	TestTrue(FString::Printf(TEXT("behind two trains the trip works (%s)"), *Two.Problem), Two.bValid);
	const FSTPlanResult Three = Try(3);
	TestFalse(TEXT("behind three trains the trip would queue"), Three.bValid);
	return true;
}

// The exact assignment beats the greedy choice.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTAssignmentTest, "SmartTrains.Planner.Assignment", TestFlags)
bool FSTAssignmentTest::RunTest(const FString& Parameters)
{
	// Greedy takes 10 + 1 = 11; the best is 9 + 9 = 18.
	const TArray<int32> Assigned = STAssignment::Solve({{10.0, 9.0}, {9.0, 1.0}});
	TestEqual(TEXT("two rows"), Assigned.Num(), 2);
	if (Assigned.Num() == 2)
	{
		TestEqual(TEXT("row 1 takes column 2"), Assigned[0], 1);
		TestEqual(TEXT("row 2 takes column 1"), Assigned[1], 0);
	}
	const TArray<int32> Nothing = STAssignment::Solve({{0.0}, {-1.0}});
	TestTrue(TEXT("rows with nothing worth more than 0 get nothing"), Nothing.Num() == 2 && Nothing[0] == INDEX_NONE && Nothing[1] == INDEX_NONE);
	return true;
}

// The flow plan's minimum-cost flow fills the cheap route first and sends all it can.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTMinCostFlowTest, "SmartTrains.FlowPlan.MinCostFlow", TestFlags)
bool FSTMinCostFlowTest::RunTest(const FString& Parameters)
{
	// 0 source, 1 sink; two suppliers (2, 3) of 5 each, one consumer (4) taking 8, the route from 2 cheaper.
	FSTMinCostFlow Flow(5);
	Flow.AddEdge(0, 2, 5.0, 0.0);
	Flow.AddEdge(0, 3, 5.0, 0.0);
	const int32 Cheap = Flow.AddEdge(2, 4, 100.0, 1.0);
	const int32 Dear = Flow.AddEdge(3, 4, 100.0, 5.0);
	Flow.AddEdge(4, 1, 8.0, 0.0);
	TestEqual(TEXT("sends all the consumer takes"), Flow.Solve(0, 1), 8.0);
	TestEqual(TEXT("the cheap route is full"), Flow.GetFlow(Cheap), 5.0);
	TestEqual(TEXT("the dear route takes the rest"), Flow.GetFlow(Dear), 3.0);
	return true;
}

// A request: the car that fits the requesting platform holds another item, so the trip empties it first (at a consumer
// of that item, or at a sink when none takes it), then picks up and delivers, even less than a batch; its car loads
// nothing else.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTRequestEmptiesCarTest, "SmartTrains.Planner.RequestEmptiesCar", TestFlags)
bool FSTRequestEmptiesCarTest::RunTest(const FString& Parameters)
{
	UClass* Wanted = USTTestSolid::StaticClass();
	UClass* Other = USTTestSolidB::StaticClass();
	for (int32 Case = 0; Case < 2; ++Case)
	{
		const bool bSinkOnly = Case == 1;
		ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
		AFGTrainStationIdentifier* Start = FSTTestAccess::MakeStation();
		AFGTrainStationIdentifier* Dump = FSTTestAccess::MakeStation();
		AFGTrainStationIdentifier* Source = FSTTestAccess::MakeStation();
		AFGTrainStationIdentifier* Drop = FSTTestAccess::MakeStation();
		const TArray<AFGTrainStationIdentifier*> Stations = {Start, Dump, Source, Drop};
		for (AFGTrainStationIdentifier* From : Stations)
		{
			for (AFGTrainStationIdentifier* To : Stations)
			{
				if (From != To)
				{
					FSTTestAccess::SetTrip(*Subsystem, From, To, 60.f);
				}
			}
		}
		TArray<AFGBuildableTrainPlatformCargo*> Objects;
		for (int32 Index = 0; Index < 3; ++Index)
		{
			Objects.Add(NewObject<AFGBuildableTrainPlatformCargo>(GetTransientPackage()));
		}
		FSTNode DumpNode = FSTTestAccess::Node(Dump, bSinkOnly ? nullptr : Other, ESTRole::Demand, 0, 5000, 0);
		DumpNode.bAnyItem = bSinkOnly;
		DumpNode.Platforms = {Objects[0]};
		// A fifth of a load at a supplier that wants a full one.
		FSTNode SourceNode = FSTTestAccess::Node(Source, Wanted, ESTRole::Supply, 200, 1000, 0);
		SourceNode.SendAtPercent = 100.f;
		SourceNode.Platforms = {Objects[1]};
		FSTNode DropNode = FSTTestAccess::Node(Drop, Wanted, ESTRole::Demand, 0, 1000, 0);
		DropNode.Platforms = {Objects[2]};
		TArray<FSTNode> Nodes = {DumpNode, SourceNode, DropNode};
		TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;
		TArray<FSTTestAccess::FPlatformInfo> Platforms = {FSTTestAccess::Platform(Other, false, 0.0, 0), FSTTestAccess::Platform(Wanted, true, 200.0, 2),
			FSTTestAccess::Platform(Wanted, false, 0.0, 0)};
		if (bSinkOnly)
		{
			Platforms[0].Items.Reset();
			Platforms[0].bAnyItem = true;
		}
		for (int32 Index = 0; Index < Platforms.Num(); ++Index)
		{
			Platforms[Index].Platform = Objects[Index];
			Platforms[Index].Slots = 10;
		}
		FSTTestAccess::FCarInfo Car = FSTTestAccess::Car(Other, 1000.0);
		Car.Slots = 10;
		FSTTestAccess::FCarPlatforms CarPlatforms;
		CarPlatforms.Add(Dump, {{0}});
		CarPlatforms.Add(Source, {{1}});
		CarPlatforms.Add(Drop, {{2}});
		FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);
		FSTTestAccess::SetTrain(Planner, Start, true, false, {Car}, Platforms, CarPlatforms);
		FSTTestAccess::ServeAll(Planner);

		const TCHAR* Where = bSinkOnly ? TEXT("sink") : TEXT("consumer");
		TArray<FSTPlanStop> Stops;
		FSTPlanResult Result;
		bool bDelivers = false;
		FString Why;
		TArray<int32> Cars;
		const bool bPlanned = Planner.PlanRequest({2, Objects[2]}, Stops, Result, bDelivers, Cars, Why);
		TestTrue(FString::Printf(TEXT("%s: a request trip is found (%s)"), Where, *Why), bPlanned);
		TestTrue(FString::Printf(TEXT("%s: it delivers"), Where), bDelivers);
		if (bPlanned && Stops.Num() == 3)
		{
			TestTrue(FString::Printf(TEXT("%s: the car is emptied first"), Where), Stops[0].Station == Dump);
			TestTrue(FString::Printf(TEXT("%s: then the pickup"), Where), Stops[1].Station == Source);
			TestEqual(FString::Printf(TEXT("%s: all of the other item is unloaded"), Where), FSTTripPlanner::CountUnloaded(Stops, Result, Other, Dump), 1000);
			TestTrue(FString::Printf(TEXT("%s: the wanted item reaches the platform, less than a batch"), Where),
				FSTTripPlanner::CountUnloadedInto(Result, Wanted, Objects[2]) > 0);
			TestTrue(FString::Printf(TEXT("%s: the request car loads only the wanted item"), Where), Planner.KeepsRequestCars(Stops, Result, Cars, Wanted));
			TestFalse(FString::Printf(TEXT("%s: an emptying-only check rejects its pickup"), Where), Planner.KeepsRequestCars(Stops, Result, Cars, nullptr));
		}
		else
		{
			AddError(FString::Printf(TEXT("%s: expected 3 stops, got %d"), Where, Stops.Num()));
		}
	}
	return true;
}

// A request empties the car by whatever is soonest: two near consumers that take it all beat a far sink, and a near
// sink beats two far consumers.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTRequestSoonestEmptyingTest, "SmartTrains.Planner.RequestSoonestEmptying", TestFlags)
bool FSTRequestSoonestEmptyingTest::RunTest(const FString& Parameters)
{
	UClass* Wanted = USTTestSolid::StaticClass();
	UClass* Other = USTTestSolidB::StaticClass();
	for (int32 Case = 0; Case < 2; ++Case)
	{
		const bool bSinkNear = Case == 1;
		ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
		AFGTrainStationIdentifier* Start = FSTTestAccess::MakeStation();
		AFGTrainStationIdentifier* First = FSTTestAccess::MakeStation();
		AFGTrainStationIdentifier* Second = FSTTestAccess::MakeStation();
		AFGTrainStationIdentifier* Sink = FSTTestAccess::MakeStation();
		AFGTrainStationIdentifier* Source = FSTTestAccess::MakeStation();
		AFGTrainStationIdentifier* Drop = FSTTestAccess::MakeStation();
		const TArray<AFGTrainStationIdentifier*> Stations = {Start, First, Second, Sink, Source, Drop};
		auto IsFar = [&](const AFGTrainStationIdentifier* Station) { return bSinkNear ? (Station == First || Station == Second) : Station == Sink; };
		for (AFGTrainStationIdentifier* From : Stations)
		{
			for (AFGTrainStationIdentifier* To : Stations)
			{
				if (From != To)
				{
					FSTTestAccess::SetTrip(*Subsystem, From, To, IsFar(From) || IsFar(To) ? 600.f : 60.f);
				}
			}
		}
		TArray<AFGBuildableTrainPlatformCargo*> Objects;
		for (int32 Index = 0; Index < 5; ++Index)
		{
			Objects.Add(NewObject<AFGBuildableTrainPlatformCargo>(GetTransientPackage()));
		}
		FSTNode FirstNode = FSTTestAccess::Node(First, Other, ESTRole::Demand, 0, 100000, 0);
		FirstNode.Platforms = {Objects[0]};
		FSTNode SecondNode = FSTTestAccess::Node(Second, Other, ESTRole::Demand, 0, 100000, 0);
		SecondNode.Platforms = {Objects[1]};
		FSTNode SinkNode = FSTTestAccess::Node(Sink, nullptr, ESTRole::Demand, 0, 100000, 0);
		SinkNode.bAnyItem = true;
		SinkNode.Platforms = {Objects[2]};
		FSTNode SourceNode = FSTTestAccess::Node(Source, Wanted, ESTRole::Supply, 1000, 1000, 0);
		SourceNode.Platforms = {Objects[3]};
		FSTNode DropNode = FSTTestAccess::Node(Drop, Wanted, ESTRole::Demand, 0, 1000, 0);
		DropNode.Platforms = {Objects[4]};
		TArray<FSTNode> Nodes = {FirstNode, SecondNode, SinkNode, SourceNode, DropNode};
		TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;
		TArray<FSTTestAccess::FPlatformInfo> Platforms = {FSTTestAccess::Platform(Other, false, 0.0, 0), FSTTestAccess::Platform(Other, false, 0.0, 0),
			FSTTestAccess::Platform(Other, false, 0.0, 0), FSTTestAccess::Platform(Wanted, true, 1000.0, 10), FSTTestAccess::Platform(Wanted, false, 0.0, 0)};
		Platforms[2].Items.Reset();
		Platforms[2].bAnyItem = true;
		for (int32 Index = 0; Index < Platforms.Num(); ++Index)
		{
			Platforms[Index].Platform = Objects[Index];
			// The consumers take half the load each (2 slots of 500); the sink and the rest take it all.
			Platforms[Index].Slots = Index < 2 ? 2 : 10;
		}
		FSTTestAccess::FCarInfo Car = FSTTestAccess::Car(Other, 2000.0);
		Car.Slots = 10;
		FSTTestAccess::FCarPlatforms CarPlatforms;
		for (int32 Index = 0; Index < 5; ++Index)
		{
			CarPlatforms.Add(Stations[Index + 1], {{Index}});
		}
		FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);
		FSTTestAccess::SetTrain(Planner, Start, true, false, {Car}, Platforms, CarPlatforms);
		FSTTestAccess::ServeAll(Planner);

		const TCHAR* Where = bSinkNear ? TEXT("near sink") : TEXT("near consumers");
		TArray<int32> Cars;
		TArray<FSTPlanStop> Stops;
		FSTPlanResult Result;
		bool bDelivers = false;
		FString Why;
		TestTrue(FString::Printf(TEXT("%s: a request trip is found (%s)"), Where, *Why), Planner.PlanRequest({4, Objects[4]}, Stops, Result, bDelivers, Cars, Why) && bDelivers);
		const bool bUsesSink = Stops.ContainsByPredicate([Sink](const FSTPlanStop& Stop) { return Stop.Station == Sink; });
		if (bSinkNear)
		{
			TestEqual(TEXT("near sink: one stop at the sink, then the pickup and the delivery"), Stops.Num(), 3);
			TestTrue(TEXT("near sink: the sink empties the car"), bUsesSink);
		}
		else
		{
			TestEqual(TEXT("near consumers: two consumers, then the pickup and the delivery"), Stops.Num(), 4);
			TestFalse(TEXT("near consumers: the far sink isn't used"), bUsesSink);
		}
		TestTrue(FString::Printf(TEXT("%s: the wanted item reaches the platform"), Where), FSTTripPlanner::CountUnloaded(Stops, Result, Wanted, Drop) > 0);
	}
	return true;
}

// Two platforms of one station ask: one trip serves both, each with the car that stops beside it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTRequestMergeTest, "SmartTrains.Planner.RequestMerge", TestFlags)
bool FSTRequestMergeTest::RunTest(const FString& Parameters)
{
	UClass* Wanted = USTTestSolid::StaticClass();
	ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
	AFGTrainStationIdentifier* Start = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Source = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Drop = FSTTestAccess::MakeStation();
	for (AFGTrainStationIdentifier* From : {Start, Source, Drop})
	{
		for (AFGTrainStationIdentifier* To : {Start, Source, Drop})
		{
			if (From != To)
			{
				FSTTestAccess::SetTrip(*Subsystem, From, To, 60.f);
			}
		}
	}
	TArray<AFGBuildableTrainPlatformCargo*> Objects;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Objects.Add(NewObject<AFGBuildableTrainPlatformCargo>(GetTransientPackage()));
	}
	FSTNode SourceNode = FSTTestAccess::Node(Source, Wanted, ESTRole::Supply, 2000, 2000, 0);
	SourceNode.Platforms = {Objects[0], Objects[1]};
	FSTNode DropNode = FSTTestAccess::Node(Drop, Wanted, ESTRole::Demand, 0, 2000, 0);
	DropNode.Platforms = {Objects[2], Objects[3]};
	TArray<FSTNode> Nodes = {SourceNode, DropNode};
	TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;
	TArray<FSTTestAccess::FPlatformInfo> Platforms = {FSTTestAccess::Platform(Wanted, true, 1000.0, 10), FSTTestAccess::Platform(Wanted, true, 1000.0, 10),
		FSTTestAccess::Platform(Wanted, false, 0.0, 0), FSTTestAccess::Platform(Wanted, false, 0.0, 0)};
	for (int32 Index = 0; Index < Platforms.Num(); ++Index)
	{
		Platforms[Index].Platform = Objects[Index];
		Platforms[Index].Slots = 10;
	}
	FSTTestAccess::FCarInfo First = FSTTestAccess::Car();
	First.Slots = 10;
	FSTTestAccess::FCarInfo Second = FSTTestAccess::Car();
	Second.Slots = 10;
	FSTTestAccess::FCarPlatforms CarPlatforms;
	CarPlatforms.Add(Source, {{0, 1}});
	CarPlatforms.Add(Drop, {{2, 3}});
	FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);
	FSTTestAccess::SetTrain(Planner, Start, true, false, {First, Second}, Platforms, CarPlatforms);
	FSTTestAccess::ServeAll(Planner);

	TArray<FSTPlanStop> Stops;
	FSTPlanResult Result;
	bool bDelivers = false;
	TArray<int32> Cars;
	FString Why;
	TestTrue(FString::Printf(TEXT("the first request is planned (%s)"), *Why), Planner.PlanRequest({1, Objects[2]}, Stops, Result, bDelivers, Cars, Why) && bDelivers);
	TestTrue(TEXT("the first request uses the car beside its platform"), Cars == TArray<int32>{0});
	TArray<int32> More;
	const bool bAdded = Planner.AddRequest(Stops, Result, {1, Objects[3]}, Cars, More, [](const TArray<FSTPlanStop>&, const FSTPlanResult&) { return true; });
	TestTrue(TEXT("the second request joins the trip"), bAdded);
	TestEqual(TEXT("in the same two stops"), Stops.Num(), 2);
	TestTrue(TEXT("with the other car"), More == TArray<int32>{1});
	TestTrue(TEXT("both platforms get the item"), FSTTripPlanner::CountUnloadedInto(Result, Wanted, Objects[2]) > 0 && FSTTripPlanner::CountUnloadedInto(Result, Wanted, Objects[3]) > 0);
	return true;
}

#endif
