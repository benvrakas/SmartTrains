// Automation tests for the trip planner, the flow plan and the item groups (Docs/Testing.md, "Automated tests").
// Run in the editor: Tools/BuildScripts/test-smarttrains.bat, or "Automation RunTests SmartTrains" in its console.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGTrain.h"
#include "FGTrainStationIdentifier.h"
#include "Misc/AutomationTest.h"
#include "STForecast.h"
#include "STMigration.h"
#include "STMinCostFlow.h"
#include "STSubsystem.h"
#include "STTripPlanner.h"
#include "Tests/STTestItems.h"
#include "Tests/STReplayItems.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

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

	static void AddManaged(ASTSubsystem& Subsystem, AFGTrain* Train)
	{
		FSTTrainState& State = Subsystem.mTrains.AddDefaulted_GetRef();
		State.Train = Train;
		State.bManaged = true;
	}

	static void BuildMoves(FSTTripPlanner& Planner)
	{
		Planner.BuildMoves();
	}

	static void SetStart(FSTTripPlanner& Planner, double FreeAt, bool bAnchorDocked)
	{
		Planner.mFreeAt = FreeAt;
		Planner.bAnchorDocked = bAnchorDocked;
	}

	static double GetTime(const FSTTripPlanner& Planner)
	{
		return Planner.mTime;
	}

	static FSTSettings& Settings(ASTSubsystem& Subsystem)
	{
		return Subsystem.mSettings;
	}

	static void SetStay(ASTSubsystem& Subsystem, AFGTrainStationIdentifier* Station, float Seconds)
	{
		FSTDwellTime& Stay = Subsystem.mStays.AddDefaulted_GetRef();
		Stay.Station = Station;
		Stay.Seconds = Seconds;
		Stay.Samples = 1;
	}

	static int32 CountMoves(const FSTTripPlanner& Planner)
	{
		return Planner.mMoves.Num();
	}

	// A group of Item needing Needed trains, fitted by Fitting, as the flow plan would make it.
	static void AddGroup(ASTSubsystem& Subsystem, UClass* Item, double Needed, const TArray<AFGTrain*>& Fitting)
	{
		FSTGroup& Group = Subsystem.mGroups.AddDefaulted_GetRef();
		Group.Item = Item;
		Group.Needed = Needed;
		for (AFGTrain* Train : Fitting)
		{
			Group.Fitting.Add(Train);
		}
	}

	static TArray<FSTGroup>& Groups(ASTSubsystem& Subsystem)
	{
		return Subsystem.mGroups;
	}

	static FSTTrainState* State(ASTSubsystem& Subsystem, const AFGTrain* Train)
	{
		return Subsystem.FindTrainState(Train);
	}


	static void LinkSupplyChain(TArray<FSTNode>& Nodes)
	{
		ASTSubsystem::LinkSupplyChain(Nodes);
	}

	static double GetTiedShare(const FSTTripPlanner& Planner, const TArray<TPair<UClass*, double>>& Cargo)
	{
		return Planner.GetTiedShare(Cargo);
	}

	static int32 CountNear(const FSTTripPlanner& Planner, const AFGTrainStationIdentifier* Station, double From, double Until)
	{
		return Planner.CountNear(Station, From, Until);
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
	auto Try = [&](int32 Ahead, bool bUnderWay = false)
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
		if (bUnderWay)
		{
			Planner.AllowQueue();
		}
		FSTPlanStop Unload;
		Unload.Station = Sink;
		Unload.UnloadItems.Add(Ingot);
		return Planner.Simulate({Unload});
	};
	const FSTPlanResult Two = Try(2);
	TestTrue(FString::Printf(TEXT("behind two trains the trip works (%s)"), *Two.Problem), Two.bValid);
	const FSTPlanResult Three = Try(3);
	TestFalse(TEXT("behind three trains the trip would queue"), Three.bValid);
	// The rest of a trip under way keeps its stop and waits: its cargo is on board (a trip used to be dropped after its first
	// pickup when the next station was busy, 2026-11-09).
	const FSTPlanResult UnderWay = Try(3, true);
	TestTrue(FString::Printf(TEXT("a trip under way may wait behind three trains (%s)"), *UnderWay.Problem), UnderWay.bValid);
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

// The forecast cost: a delivery is worth more the sooner it ends a shortfall, a pickup only where the supplier would be
// full, and the deadline is when the stock first falls below target.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTForecastTest, "SmartTrains.Planner.Forecast", TestFlags)
bool FSTForecastTest::RunTest(const FString& Parameters)
{
	UClass* Item = USTTestSolid::StaticClass();
	AFGTrainStationIdentifier* Station = FSTTestAccess::MakeStation();
	const double Now = 1000.0;
	const double Car = 1000.0;
	FSTNode Short = FSTTestAccess::Node(Station, Item, ESTRole::Demand, 0, 1000, 1);
	const double Soon = STForecast::Gain(Short, {}, {Now, 500.0}, Now, Car);
	const double Late = STForecast::Gain(Short, {}, {Now + 1800.0, 500.0}, Now, Car);
	TestTrue(TEXT("a delivery that ends a shortfall is worth something"), Late > 0.0);
	TestTrue(TEXT("sooner is worth more"), Soon > Late * 1.5);
	TestEqual(TEXT("an hour short of half a car load is 30 car-load minutes"), STForecast::Cost(Short, {}, Now, Car), 30.0, 0.01);
	const double Twice = STForecast::Gain(Short, {{Now, 500.0}}, {Now, 500.0}, Now, Car);
	TestEqual(TEXT("a second delivery to a platform already at target is worth nothing"), Twice, 0.0, 0.01);

	FSTNode Full = FSTTestAccess::Node(Station, Item, ESTRole::Supply, 1000, 1000, 1);
	Full.Rate = 1.0;
	FSTNode Empty = Full;
	Empty.Stock = 0;
	TestTrue(TEXT("a pickup at a full supplier is worth something"), STForecast::Gain(Full, {}, {Now, 500.0}, Now, Car) > 0.0);
	TestTrue(TEXT("more than at an empty one"),
		STForecast::Gain(Full, {}, {Now, 500.0}, Now, Car) > STForecast::Gain(Empty, {}, {Now, 500.0}, Now, Car));

	FSTNode Using = FSTTestAccess::Node(Station, Item, ESTRole::Demand, 800, 1000, 1);
	Using.Rate = 1.0;
	TestEqual(TEXT("falls below its target of 500 after 300 s"), STForecast::Deadline(Using, {}, Now, Car), Now + 300.0, 0.01);
	TestEqual(TEXT("a delivery of 200 then puts it off by 200 s"), STForecast::Deadline(Using, {{Now + 100.0, 200.0}}, Now, Car), Now + 500.0, 0.01);
	return true;
}

// Cars holding cargo tie the train down; less so when every item they hold is wanted soon. Busy stations: bookings of
// other trains near a visit count, the train's own don't.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTTiedCarsTest, "SmartTrains.Planner.TiedCarsAndBusyStations", TestFlags)
bool FSTTiedCarsTest::RunTest(const FString& Parameters)
{
	UClass* Water = USTTestFluid::StaticClass();
	UClass* Solid = USTTestSolid::StaticClass();
	ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
	AFGTrainStationIdentifier* Sink = FSTTestAccess::MakeStation();
	TArray<FSTNode> Nodes = {FSTTestAccess::Node(Sink, Water, ESTRole::Demand, 0, 2 * FluidSlot, 2)};
	TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;
	Bookings.Add(Sink, {{0, 100.0, 150.0}, {5, 100.0, 150.0}, {6, 400.0, 450.0}});
	FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);
	FSTTestAccess::SetTrain(Planner, Sink, true, false, {FSTTestAccess::Car()}, {}, {});

	TestEqual(TEXT("an empty car is free"), FSTTestAccess::GetTiedShare(Planner, {}), 0.0);
	TestEqual(TEXT("a car of an item nobody needs soon is tied down"), FSTTestAccess::GetTiedShare(Planner, {{Solid, 500.0}}), 1.0);
	TestTrue(TEXT("a car of an item a platform needs soon is tied down less"), FSTTestAccess::GetTiedShare(Planner, {{Water, 500.0}}) < 0.5);
	TestEqual(TEXT("one item nobody needs soon ties the car down"), FSTTestAccess::GetTiedShare(Planner, {{Water, 500.0}, {Solid, 10.0}}), 1.0);
	TestEqual(TEXT("other trains booked near the visit count, not this one"), FSTTestAccess::CountNear(Planner, Sink, 50.0, 200.0), 1);
	TestEqual(TEXT("bookings outside the window don't count"), FSTTestAccess::CountNear(Planner, Sink, 160.0, 390.0), 0);
	return true;
}

// Bookings only count towards the queue limit for arrivals within five minutes: plans further ahead still change.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTQueueLookAheadTest, "SmartTrains.Planner.QueueLookAhead", TestFlags)
bool FSTQueueLookAheadTest::RunTest(const FString& Parameters)
{
	UClass* Ingot = USTTestSolid::StaticClass();
	ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
	FSTTestAccess::SetQueueTrains(*Subsystem, 2.f);
	AFGTrainStationIdentifier* Start = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Sink = FSTTestAccess::MakeStation();
	FSTTestAccess::SetTrip(*Subsystem, Start, Sink, 900.f);
	TArray<FSTNode> Nodes = {FSTTestAccess::Node(Sink, Ingot, ESTRole::Demand, 0, 100000, 1)};
	TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Bookings.FindOrAdd(Sink).Add({10 + Index, 850.0 + Index * 100.0, 940.0 + Index * 100.0});
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
	const FSTPlanResult Result = Planner.Simulate({Unload});
	TestTrue(FString::Printf(TEXT("three bookings 15 minutes ahead don't reject the trip (%s)"), *Result.Problem), Result.bValid);
	TestTrue(TEXT("it still waits for them"), Result.bValid && Result.Arrive.Num() == 1 && Result.Arrive[0] > 900.0);
	return true;
}

// A car doesn't load less than a twentieth of a car of an item: on the server such pickups mostly moved nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTSmallLoadTest, "SmartTrains.Planner.SmallLoad", TestFlags)
bool FSTSmallLoadTest::RunTest(const FString& Parameters)
{
	UClass* Ingot = USTTestSolid::StaticClass();
	ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
	AFGTrainStationIdentifier* Start = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Source = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Sink = FSTTestAccess::MakeStation();
	FSTTestAccess::SetTrip(*Subsystem, Start, Source, 60.f);
	FSTTestAccess::SetTrip(*Subsystem, Source, Sink, 60.f);
	auto LoadedFrom = [&](int32 Stock)
	{
		TArray<FSTNode> Nodes = {FSTTestAccess::Node(Source, Ingot, ESTRole::Supply, Stock, 4800, 1), FSTTestAccess::Node(Sink, Ingot, ESTRole::Demand, 0, 100000, 1)};
		TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;
		FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);
		TArray<FSTTestAccess::FPlatformInfo> Platforms = {FSTTestAccess::Platform(Ingot, true, Stock, FMath::DivideAndRoundUp(Stock, 100)), FSTTestAccess::Platform(Ingot, false, 0.0, 0)};
		for (FSTTestAccess::FPlatformInfo& Platform : Platforms)
		{
			Platform.Slots = 48;
		}
		FSTTestAccess::FCarInfo Car = FSTTestAccess::Car();
		Car.Slots = 32;
		FSTTestAccess::FCarPlatforms CarPlatforms;
		CarPlatforms.Add(Source, {{0}});
		CarPlatforms.Add(Sink, {{1}});
		FSTTestAccess::SetTrain(Planner, Start, true, false, {Car}, Platforms, CarPlatforms);
		FSTPlanStop Load;
		Load.Station = Source;
		Load.LoadItems.Add(Ingot);
		FSTPlanStop Unload;
		Unload.Station = Sink;
		Unload.UnloadItems.Add(Ingot);
		const FSTPlanResult Result = Planner.Simulate({Load, Unload});
		AddInfo(FString::Printf(TEXT("stock %d: %s"), Stock, Result.bValid ? TEXT("valid") : *Result.Problem));
		return Result.bValid && Result.Steps.Num() > 0 && Result.Steps[0].Num() > 0 && Result.Steps[0][0].Loaded.Num() > 0 ? Result.Steps[0][0].Loaded[0].Value : 0;
	};
	TestEqual(TEXT("100 isn't loaded into a car of 3200"), LoadedFrom(100), 0);
	TestEqual(TEXT("1000 is"), LoadedFrom(1000), 1000);
	return true;
}

// A sink only gets what a car can't unload at any consumer of it: on the server leftovers of coal, sulfur and AI
// Limiters went into a depot's sink while their consumers waited.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTSinkNeededTest, "SmartTrains.Planner.SinkKeepsNeeded", TestFlags)
bool FSTSinkNeededTest::RunTest(const FString& Parameters)
{
	UClass* Ingot = USTTestSolid::StaticClass();
	ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
	AFGTrainStationIdentifier* Start = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Sink = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Consumer = FSTTestAccess::MakeStation();
	FSTTestAccess::SetTrip(*Subsystem, Start, Sink, 60.f);
	// bConsumer: a consumer of the ingots exists; bLinesUp: the car stops beside its platform there.
	auto Dump = [&](bool bConsumer, bool bLinesUp)
	{
		FSTNode SinkNode = FSTTestAccess::Node(Sink, nullptr, ESTRole::Demand, 0, 0, 1);
		SinkNode.bAnyItem = true;
		TArray<FSTNode> Nodes = {SinkNode};
		if (bConsumer)
		{
			Nodes.Add(FSTTestAccess::Node(Consumer, Ingot, ESTRole::Demand, 0, 100000, 1));
		}
		TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;
		FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);
		FSTTestAccess::FPlatformInfo SinkPlatform = FSTTestAccess::Platform(Ingot, false, 0.0, 0);
		SinkPlatform.bAnyItem = true;
		SinkPlatform.Items.Reset();
		SinkPlatform.Slots = 48;
		FSTTestAccess::FPlatformInfo ConsumerPlatform = FSTTestAccess::Platform(Ingot, false, 0.0, 0);
		ConsumerPlatform.Slots = 48;
		FSTTestAccess::FCarInfo Car = FSTTestAccess::Car(Ingot, 3200.0);
		Car.Slots = 32;
		FSTTestAccess::FCarPlatforms CarPlatforms;
		CarPlatforms.Add(Sink, {{0}});
		if (bLinesUp)
		{
			CarPlatforms.Add(Consumer, {{1}});
		}
		FSTTestAccess::SetTrain(Planner, Start, true, false, {Car}, {SinkPlatform, ConsumerPlatform}, CarPlatforms);
		FSTTestAccess::ServeAll(Planner);
		FSTPlanStop Unload;
		Unload.Station = Sink;
		Unload.UnloadItems.Add(Ingot);
		const FSTPlanResult Result = Planner.Simulate({Unload});
		AddInfo(FString::Printf(TEXT("consumer %d, lines up %d: %s"), bConsumer ? 1 : 0, bLinesUp ? 1 : 0, Result.bValid ? TEXT("valid") : *Result.Problem));
		return Result.bValid;
	};
	TestFalse(TEXT("ingots the car can deliver to a consumer don't go into the sink"), Dump(true, true));
	TestTrue(TEXT("ingots the car can never unload at their consumer may, so the car is freed"), Dump(true, false));
	TestTrue(TEXT("ingots no consumer takes may"), Dump(false, false));
	return true;
}

// Supply chain: a consumer whose machine also waits on a starved item counts its shortfall less.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTSupplyChainTest, "SmartTrains.Planner.SupplyChain", TestFlags)
bool FSTSupplyChainTest::RunTest(const FString& Parameters)
{
	AFGTrainStationIdentifier* Station = FSTTestAccess::MakeStation();
	const AActor* Machine = NewObject<AActor>(GetTransientPackage());
	FSTNode Ingots = FSTTestAccess::Node(Station, USTTestSolid::StaticClass(), ESTRole::Demand, 1000, 10000, 1);
	Ingots.Machines.Add(Machine);
	FSTNode Concrete = FSTTestAccess::Node(Station, USTTestSolidB::StaticClass(), ESTRole::Demand, 0, 10000, 1);
	Concrete.Machines.Add(Machine);
	Concrete.bStarved = true;
	TArray<FSTNode> Nodes = {Ingots, Concrete};
	FSTTestAccess::LinkSupplyChain(Nodes);
	TestTrue(TEXT("ingots wait on the starved concrete"), Nodes[0].WaitsOn == USTTestSolidB::StaticClass() && Nodes[0].CostScale < 1.0);
	TestTrue(TEXT("the starved concrete itself counts in full"), !Nodes[1].WaitsOn && FMath::IsNearlyEqual(Nodes[1].CostScale, 1.0));
	const double Waiting = STForecast::Cost(Nodes[0], {}, 0.0, 3200.0);
	const double Free = STForecast::Cost(Ingots, {}, 0.0, 3200.0);
	TestTrue(FString::Printf(TEXT("its shortfall costs less (%.1f against %.1f)"), Waiting, Free), Waiting < Free);
	return true;
}

// Replays every snapshot in Mods/SmartTrains/Tests/Snapshots (/st snapshot writes them on the server): each free train
// is planned again from the snapshot's inputs, and every plan must be valid. Passes with no snapshots.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTReplaySnapshotsTest, "SmartTrains.Replay.Snapshots", TestFlags)
bool FSTReplaySnapshotsTest::RunTest(const FString& Parameters)
{
	const FString Folder = FPaths::Combine(FPaths::ProjectDir(), TEXT("Mods"), TEXT("SmartTrains"), TEXT("Tests"), TEXT("Snapshots"));
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Folder, TEXT("*.json")), true, false);
	if (Files.Num() == 0)
	{
		AddInfo(FString::Printf(TEXT("no snapshots in %s"), *Folder));
		return true;
	}
	const TArray<UClass*> Stand = STReplayItemClasses();
	for (const FString& File : Files)
	{
		FString Text;
		TSharedPtr<FJsonObject> Root;
		if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(Folder, File)) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			AddError(FString::Printf(TEXT("%s can't be read"), *File));
			continue;
		}
		ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
		FSTSettings& Settings = FSTTestAccess::Settings(*Subsystem);
		const TSharedPtr<FJsonObject>* SettingsObject = nullptr;
		if (Root->TryGetObjectField(TEXT("settings"), SettingsObject))
		{
			Settings.QueueTrains = (*SettingsObject)->GetNumberField(TEXT("queueTrains"));
			Settings.bLeaveAtOnce = (*SettingsObject)->GetBoolField(TEXT("leaveAtOnce"));
			Settings.MaxStops = (*SettingsObject)->GetNumberField(TEXT("maxStops"));
			Settings.MaxTripMinutes = (*SettingsObject)->GetNumberField(TEXT("maxTripMinutes"));
			Settings.ArrivalGapSeconds = (*SettingsObject)->GetNumberField(TEXT("arrivalGapSeconds"));
		}
		// Items: each one a stand-in class with its form and stack size.
		TMap<FString, UClass*> Items;
		TMap<UClass*, FString> ItemNames;
		for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("items")))
		{
			const TSharedPtr<FJsonObject> Item = Value->AsObject();
			if (Items.Num() >= Stand.Num())
			{
				AddWarning(FString::Printf(TEXT("%s has more items than stand-ins (%d); the rest are left out"), *File, Stand.Num()));
				break;
			}
			UClass* Class = Stand[Items.Num()];
			UFGItemDescriptor* Default = Class->GetDefaultObject<UFGItemDescriptor>();
			const bool bFluid = Item->GetBoolField(TEXT("fluid"));
			const int32 Stack = static_cast<int32>(Item->GetNumberField(TEXT("stack")));
			Cast<USTReplayItemBase>(Default)->Configure(bFluid, Stack);
			Items.Add(Item->GetStringField(TEXT("path")), Class);
			ItemNames.Add(Class, Item->GetStringField(TEXT("name")));
		}
		auto ItemOf = [&](const FString& Path) { return Items.FindRef(Path); };
		// Stations, travel times and stays.
		TMap<FString, AFGTrainStationIdentifier*> Stations;
		TMap<FString, FString> StationNames;
		auto StationOf = [&](const FString& Key) -> AFGTrainStationIdentifier*
		{
			if (Key.IsEmpty() || Key == TEXT("None"))
			{
				return nullptr;
			}
			AFGTrainStationIdentifier*& Station = Stations.FindOrAdd(Key);
			if (!Station)
			{
				Station = FSTTestAccess::MakeStation();
			}
			return Station;
		};
		for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("stations")))
		{
			const TSharedPtr<FJsonObject> Station = Value->AsObject();
			const FString Key = Station->GetStringField(TEXT("key"));
			StationNames.Add(Key, Station->GetStringField(TEXT("name")));
			FSTTestAccess::SetStay(*Subsystem, StationOf(Key), Station->GetNumberField(TEXT("stay")));
		}
		for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("travel")))
		{
			const TArray<TSharedPtr<FJsonValue>>& Row = Value->AsArray();
			FSTTestAccess::SetTrip(*Subsystem, StationOf(Row[0]->AsString()), StationOf(Row[1]->AsString()), Row[2]->AsNumber());
		}
		// Times: the snapshot's clock moved onto the test planner's.
		const double SnapshotNow = Root->GetNumberField(TEXT("now"));
		TMap<FString, AFGBuildableTrainPlatformCargo*> PlatformObjects;
		auto PlatformOf = [&](const FString& Key)
		{
			AFGBuildableTrainPlatformCargo*& Platform = PlatformObjects.FindOrAdd(Key);
			if (!Platform)
			{
				Platform = NewObject<AFGBuildableTrainPlatformCargo>(GetTransientPackage());
			}
			return Platform;
		};
		TArray<FSTNode> BaseNodes;
		for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("nodes")))
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			FSTNode Node;
			Node.Station = StationOf(Object->GetStringField(TEXT("station")));
			Node.Item = ItemOf(Object->GetStringField(TEXT("item")));
			Node.Role = static_cast<ESTRole>(static_cast<int32>(Object->GetNumberField(TEXT("role"))));
			Node.bAnyItem = Object->GetBoolField(TEXT("anyItem"));
			if (!Node.Item && !Node.bAnyItem)
			{
				continue;
			}
			Node.Stock = static_cast<int32>(Object->GetNumberField(TEXT("stock")));
			Node.Capacity = static_cast<int32>(Object->GetNumberField(TEXT("capacity")));
			Node.Rate = Object->GetNumberField(TEXT("rate"));
			Node.bRateKnown = true;
			Node.bStarved = Object->GetBoolField(TEXT("starved"));
			Node.Priority = static_cast<ESTPriority>(static_cast<int32>(Object->GetNumberField(TEXT("priority"))));
			Node.TargetPercent = Object->GetNumberField(TEXT("target"));
			Node.SendAtPercent = Object->GetNumberField(TEXT("sendAt"));
			Node.Reserved = static_cast<int32>(Object->GetNumberField(TEXT("reserved")));
			Node.PlannedFlow = Object->GetNumberField(TEXT("plannedFlow"));
			Node.RecipeRate = Object->GetNumberField(TEXT("recipeRate"));
			Node.CostScale = Object->GetNumberField(TEXT("costScale"));
			for (const TSharedPtr<FJsonValue>& Platform : Object->GetArrayField(TEXT("platforms")))
			{
				Node.Platforms.Add(PlatformOf(Platform->AsString()));
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->GetObjectField(TEXT("platformReserved"))->Values)
			{
				Node.PlatformReserved.Add(PlatformOf(Pair.Key), static_cast<int32>(Pair.Value->AsNumber()));
			}
			for (const TSharedPtr<FJsonValue>& Event : Object->GetArrayField(TEXT("planned")))
			{
				const TArray<TSharedPtr<FJsonValue>>& Pair = Event->AsArray();
				Node.Planned.Add({Pair[0]->AsNumber() - SnapshotNow, Pair[1]->AsNumber()});
			}
			BaseNodes.Add(MoveTemp(Node));
		}
		int32 Planned = 0;
		int32 Trips = 0;
		for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("trains")))
		{
			const TSharedPtr<FJsonObject> Train = Value->AsObject();
			TArray<FSTNode> Nodes = BaseNodes;
			TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;
			FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);
			const double Shift = FSTTestAccess::GetTime(Planner) - SnapshotNow;
			for (FSTNode& Node : Nodes)
			{
				for (FSTNodeEvent& Event : Node.Planned)
				{
					Event.Time += SnapshotNow + Shift;
				}
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Root->GetObjectField(TEXT("bookings"))->Values)
			{
				for (const TSharedPtr<FJsonValue>& Booking : Pair.Value->AsArray())
				{
					const TArray<TSharedPtr<FJsonValue>>& Row = Booking->AsArray();
					Bookings.FindOrAdd(StationOf(Pair.Key)).Add({static_cast<int32>(Row[0]->AsNumber()) + 1000, Row[1]->AsNumber() + Shift, Row[2]->AsNumber() + Shift});
				}
			}
			TArray<FSTTestAccess::FPlatformInfo> Platforms;
			for (const TSharedPtr<FJsonValue>& PlatformValue : Train->GetArrayField(TEXT("platforms")))
			{
				const TSharedPtr<FJsonObject> Object = PlatformValue->AsObject();
				FSTTestAccess::FPlatformInfo& Info = Platforms.AddDefaulted_GetRef();
				Info.Platform = PlatformOf(Object->GetStringField(TEXT("key")));
				Info.bConfigured = Object->GetBoolField(TEXT("configured"));
				Info.bSupply = Object->GetBoolField(TEXT("supply"));
				Info.bDemand = Object->GetBoolField(TEXT("demand"));
				Info.bAnyItem = Object->GetBoolField(TEXT("anyItem"));
				Info.bLoadMode = Object->GetBoolField(TEXT("loadMode"));
				Info.bFluid = Object->GetBoolField(TEXT("fluid"));
				Info.Slots = static_cast<int32>(Object->GetNumberField(TEXT("slots")));
				Info.Occupied = static_cast<int32>(Object->GetNumberField(TEXT("occupied")));
				for (const TSharedPtr<FJsonValue>& Item : Object->GetArrayField(TEXT("items")))
				{
					if (UClass* Class = ItemOf(Item->AsString()))
					{
						Info.Items.Add(Class);
					}
				}
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->GetObjectField(TEXT("stock"))->Values)
				{
					if (UClass* Class = ItemOf(Pair.Key))
					{
						Info.Stock.Add(Class, static_cast<int32>(Pair.Value->AsNumber()));
					}
				}
			}
			TArray<FSTTestAccess::FCarInfo> Cars;
			for (const TSharedPtr<FJsonValue>& CarValue : Train->GetArrayField(TEXT("cars")))
			{
				const TSharedPtr<FJsonObject> Object = CarValue->AsObject();
				FSTTestAccess::FCarInfo& Car = Cars.AddDefaulted_GetRef();
				Car.Slots = static_cast<int32>(Object->GetNumberField(TEXT("slots")));
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->GetObjectField(TEXT("cargo"))->Values)
				{
					if (UClass* Class = ItemOf(Pair.Key))
					{
						Car.Cargo.Emplace(Class, Pair.Value->AsNumber());
					}
				}
			}
			FSTTestAccess::FCarPlatforms CarPlatforms;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Train->GetObjectField(TEXT("carPlatforms"))->Values)
			{
				TArray<TArray<int32>>& Ways = CarPlatforms.FindOrAdd(StationOf(Pair.Key));
				for (const TSharedPtr<FJsonValue>& Way : Pair.Value->AsArray())
				{
					TArray<int32>& Cells = Ways.AddDefaulted_GetRef();
					for (const TSharedPtr<FJsonValue>& Cell : Way->AsArray())
					{
						Cells.Add(static_cast<int32>(Cell->AsNumber()));
					}
				}
			}
			AFGTrainStationIdentifier* Anchor = StationOf(Train->GetStringField(TEXT("anchor")));
			if (!Anchor || Cars.Num() == 0)
			{
				continue;
			}
			FSTTestAccess::SetTrain(Planner, Anchor, Train->GetBoolField(TEXT("fcc")), Train->GetBoolField(TEXT("pushPull")), Cars, Platforms, CarPlatforms);
			FSTTestAccess::SetStart(Planner, Train->GetNumberField(TEXT("freeAt")) + Shift, Train->GetBoolField(TEXT("anchorDocked")));
			FSTTestAccess::ServeAll(Planner);
			FSTTestAccess::BuildMoves(Planner);
			TArray<FSTPlanStop> Stops;
			FSTPlanResult Result = Planner.Improve(Stops, 0);
			++Planned;
			if (Stops.Num() == 0)
			{
				AddInfo(FString::Printf(TEXT("%s: %s: no trip (%d moves)"), *File, *Train->GetStringField(TEXT("name")), FSTTestAccess::CountMoves(Planner)));
				continue;
			}
			Result = Planner.Simulate(Stops);
			++Trips;
			// The plan with the snapshot's names (the stand-ins have none).
			TArray<FString> Parts;
			for (const FSTPlanStop& Stop : Stops)
			{
				FString Name;
				for (const TPair<FString, AFGTrainStationIdentifier*>& Pair : Stations)
				{
					Name = Pair.Value == Stop.Station ? StationNames.FindRef(Pair.Key) : Name;
				}
				TArray<FString> Moves;
				for (UClass* Item : Stop.LoadItems)
				{
					Moves.Add(TEXT("+") + ItemNames.FindRef(Item));
				}
				for (UClass* Item : Stop.UnloadItems)
				{
					Moves.Add(TEXT("-") + ItemNames.FindRef(Item));
				}
				Parts.Add(FString::Printf(TEXT("%s (%s)"), *Name, *FString::Join(Moves, TEXT(" "))));
			}
			const FString Plan = FString::Printf(TEXT("%s, %d moved"), *FString::Join(Parts, TEXT(" -> ")), Result.Moved);
			TestTrue(FString::Printf(TEXT("%s: %s plans a valid trip (%s)"), *File, *Train->GetStringField(TEXT("name")), *Result.Problem), Result.bValid);
			AddInfo(FString::Printf(TEXT("%s: %s: %s, score %.2f"), *File, *Train->GetStringField(TEXT("name")), *Plan, Result.Score));
		}
		AddInfo(FString::Printf(TEXT("%s: %d free trains replayed, %d with a trip"), *File, Planned, Trips));
	}
	return true;
}

// The rest of a trip under way is checked against its own length, not the "Most stops per trip" setting: the re-plan
// after a stop must not drop a trip that is already longer (service S9 on the IB save, 2026-11-09).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTLongTripUnderWayTest, "SmartTrains.Planner.LongTripUnderWay", TestFlags)
bool FSTLongTripUnderWayTest::RunTest(const FString& Parameters)
{
	UClass* Ingot = USTTestSolid::StaticClass();
	ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
	AFGTrainStationIdentifier* Start = FSTTestAccess::MakeStation();
	TArray<AFGTrainStationIdentifier*> Stations;
	TArray<FSTNode> Nodes;
	TArray<FSTTestAccess::FPlatformInfo> Platforms;
	FSTTestAccess::FCarPlatforms CarPlatforms;
	TArray<FSTPlanStop> Stops;
	for (int32 Index = 0; Index < 8; ++Index)
	{
		const bool bSupply = Index % 2 == 0;
		AFGTrainStationIdentifier* Station = FSTTestAccess::MakeStation();
		FSTTestAccess::SetTrip(*Subsystem, Index == 0 ? Start : Stations.Last(), Station, 60.f);
		Stations.Add(Station);
		Nodes.Add(FSTTestAccess::Node(Station, Ingot, bSupply ? ESTRole::Supply : ESTRole::Demand, bSupply ? 100000 : 0, 100000, 1));
		FSTTestAccess::FPlatformInfo& Platform = Platforms.Add_GetRef(FSTTestAccess::Platform(Ingot, bSupply, bSupply ? 50000.0 : 0.0, bSupply ? 1 : 0));
		Platform.Slots = 10;
		CarPlatforms.Add(Station, {{Index}});
		FSTPlanStop& Stop = Stops.AddDefaulted_GetRef();
		Stop.Station = Station;
		(bSupply ? Stop.LoadItems : Stop.UnloadItems).Add(Ingot);
	}
	TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;
	FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);
	FSTTestAccess::FCarInfo Car = FSTTestAccess::Car();
	Car.Slots = 2;
	FSTTestAccess::SetTrain(Planner, Start, true, false, {Car}, Platforms, CarPlatforms);
	const FSTPlanResult Plain = Planner.Simulate(Stops);
	TestFalse(TEXT("an 8-stop trip is over the trip stop limit"), Plain.bValid);
	Planner.RaiseStopLimit(8);
	const FSTPlanResult UnderWay = Planner.Simulate(Stops);
	if (TestTrue(FString::Printf(TEXT("the rest of a trip under way may keep its 8 stops (%s)"), *UnderWay.Problem), UnderWay.bValid))
	{
		TestTrue(TEXT("and moves cargo"), UnderWay.Moved > 0);
	}
	return true;
}

// A group is short only while it has fewer trains than it wants and no train that fits it is idle: idle trains that
// fit mean there is too little work, not too few trains (the case that showed "short" with trains at the depot).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTGroupShortfallTest, "SmartTrains.Groups.Shortfall", TestFlags)
bool FSTGroupShortfallTest::RunTest(const FString& Parameters)
{
	UClass* Iron = USTTestSolid::StaticClass();
	ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
	AFGTrain* Train = NewObject<AFGTrain>(GetTransientPackage());
	FSTTestAccess::AddManaged(*Subsystem, Train);
	FSTTestAccess::AddGroup(*Subsystem, Iron, 3.0, {Train});
	TestEqual(TEXT("wants 3 (3.0 needed)"), Subsystem->GetGroupTarget(FSTTestAccess::Groups(*Subsystem)[0]), 3);
	TestEqual(TEXT("its one train is idle: not short"), Subsystem->GetGroupShortfall(FSTTestAccess::Groups(*Subsystem)[0]), 0);
	FSTTrainState* State = FSTTestAccess::State(*Subsystem, Train);
	State->bHasJob = true;
	State->Job.Legs.AddDefaulted();
	FSTCarWork& Work = State->Job.Legs[0].Work.AddDefaulted_GetRef();
	Work.Item = Iron;
	TestEqual(TEXT("its one train is busy: 2 short"), Subsystem->GetGroupShortfall(FSTTestAccess::Groups(*Subsystem)[0]), 2);
	FSTTestAccess::Groups(*Subsystem)[0].Needed = 2.1;
	TestEqual(TEXT("2.1 needed wants 2"), Subsystem->GetGroupTarget(FSTTestAccess::Groups(*Subsystem)[0]), 2);
	TestEqual(TEXT("and is 1 short"), Subsystem->GetGroupShortfall(FSTTestAccess::Groups(*Subsystem)[0]), 1);
	return true;
}

// A trip built around one consumer fills each car that lines up with its unloading platforms from that item's supplier
// (a gathering trip for a factory with two inputs) and ends there, even when a nearer consumer of one of the items
// would score more on its own.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTGatherMixedFactoryTest, "SmartTrains.Gather.MixedFactory", TestFlags)
bool FSTGatherMixedFactoryTest::RunTest(const FString& Parameters)
{
	UClass* Iron = USTTestSolid::StaticClass();
	UClass* Coal = USTTestSolidB::StaticClass();
	ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
	AFGTrainStationIdentifier* Start = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* IronMine = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* CoalMine = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Factory = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* IronUser = FSTTestAccess::MakeStation();
	const TArray<AFGTrainStationIdentifier*> All = {Start, IronMine, CoalMine, Factory, IronUser};
	for (AFGTrainStationIdentifier* From : All)
	{
		for (AFGTrainStationIdentifier* To : All)
		{
			const bool bNear = (From == Start && To == IronMine) || (From == IronMine && To == IronUser);
			FSTTestAccess::SetTrip(*Subsystem, From, To, bNear ? 30.f : 300.f);
		}
	}
	TArray<FSTNode> Nodes = {
		FSTTestAccess::Node(IronMine, Iron, ESTRole::Supply, 4800, 4800, 1), FSTTestAccess::Node(CoalMine, Coal, ESTRole::Supply, 4800, 4800, 1),
		FSTTestAccess::Node(Factory, Iron, ESTRole::Demand, 0, 4800, 1), FSTTestAccess::Node(Factory, Coal, ESTRole::Demand, 0, 4800, 1),
		FSTTestAccess::Node(IronUser, Iron, ESTRole::Demand, 0, 4800, 1)};
	for (FSTNode& Node : Nodes)
	{
		Node.Rate = 1.0;
	}
	TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;
	FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);
	FSTTestAccess::ServeAll(Planner);
	TArray<FSTTestAccess::FPlatformInfo> Platforms = {FSTTestAccess::Platform(Iron, true, 4800.0, 48), FSTTestAccess::Platform(Coal, true, 4800.0, 48),
		FSTTestAccess::Platform(Iron, false, 0.0, 0), FSTTestAccess::Platform(Coal, false, 0.0, 0), FSTTestAccess::Platform(Iron, false, 0.0, 0)};
	for (FSTTestAccess::FPlatformInfo& Platform : Platforms)
	{
		Platform.Slots = 48;
	}
	FSTTestAccess::FCarInfo Car = FSTTestAccess::Car();
	Car.Slots = 32;
	// Car 1 stops beside the iron platforms, car 2 beside the coal ones; at the nearer iron consumer only car 1 has a platform.
	FSTTestAccess::FCarPlatforms CarPlatforms;
	CarPlatforms.Add(IronMine, {{0, INDEX_NONE}});
	CarPlatforms.Add(CoalMine, {{INDEX_NONE, 1}});
	CarPlatforms.Add(Factory, {{2, 3}});
	CarPlatforms.Add(IronUser, {{4, INDEX_NONE}});
	FSTTestAccess::SetTrain(Planner, Start, true, false, {Car, Car}, Platforms, CarPlatforms);
	FSTTestAccess::BuildMoves(Planner);
	auto Delivered = [](const FSTPlanResult& Result, UClass* Item)
	{
		int32 Moved = 0;
		for (const TArray<FSTPlanCarStep>& Stop : Result.Steps)
		{
			for (const FSTPlanCarStep& Step : Stop)
			{
				for (const TPair<UClass*, int32>& Each : Step.Unloaded)
				{
					Moved += Each.Key == Item ? Each.Value : 0;
				}
			}
		}
		return Moved;
	};
	TestTrue(TEXT("the factory is a consumer the train can deliver to"), Planner.GetConsumerStations().Contains(Factory));
	TArray<FSTPlanStop> Stops;
	Planner.SetOnlyStation(Factory);
	FSTPlanResult Result = Planner.Improve(Stops, 0);
	Planner.SetOnlyStation(nullptr);
	FSTTripPlanner::Trim(Stops, Result, 0);
	Result = Planner.Simulate(Stops);
	if (TestTrue(FString::Printf(TEXT("a trip for the factory is found (%s)"), *Result.Problem), Result.bValid && Stops.Num() > 0))
	{
		TestTrue(TEXT("it delivers iron"), Delivered(Result, Iron) > 0);
		TestTrue(TEXT("and coal, in the other car"), Delivered(Result, Coal) > 0);
		TestEqual(TEXT("it ends at the factory"), Stops.Last().Station, Factory);
		TestFalse(TEXT("it doesn't stop at the other iron consumer"), Stops.ContainsByPredicate([&](const FSTPlanStop& Stop) { return Stop.Station == IronUser; }));
	}
	return true;
}

// Car patterns: a consumer whose platforms don't line up with the train's pattern is another group; a car beside a
// platform that would give it an item outside its pattern skips the stop; cargo that doesn't line up is reported.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTCarPatternTest, "SmartTrains.Gather.CarPattern", TestFlags)
bool FSTCarPatternTest::RunTest(const FString& Parameters)
{
	UClass* Iron = USTTestSolid::StaticClass();
	UClass* Coal = USTTestSolidB::StaticClass();
	ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
	AFGTrainStationIdentifier* Start = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* IronMine = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Factory = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Swapped = FSTTestAccess::MakeStation();
	const TArray<AFGTrainStationIdentifier*> All = {Start, IronMine, Factory, Swapped};
	for (AFGTrainStationIdentifier* From : All)
	{
		for (AFGTrainStationIdentifier* To : All)
		{
			FSTTestAccess::SetTrip(*Subsystem, From, To, 100.f);
		}
	}
	TArray<FSTNode> Nodes = {
		FSTTestAccess::Node(IronMine, Iron, ESTRole::Supply, 4800, 4800, 1),
		FSTTestAccess::Node(Factory, Iron, ESTRole::Demand, 0, 4800, 1), FSTTestAccess::Node(Factory, Coal, ESTRole::Demand, 0, 4800, 1),
		FSTTestAccess::Node(Swapped, Iron, ESTRole::Demand, 0, 4800, 1), FSTTestAccess::Node(Swapped, Coal, ESTRole::Demand, 0, 4800, 1)};
	for (FSTNode& Node : Nodes)
	{
		Node.Rate = 1.0;
	}
	TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;
	FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);
	FSTTestAccess::ServeAll(Planner);
	TArray<FSTTestAccess::FPlatformInfo> Platforms = {FSTTestAccess::Platform(Iron, true, 4800.0, 48), FSTTestAccess::Platform(Iron, true, 4800.0, 48),
		FSTTestAccess::Platform(Iron, false, 0.0, 0), FSTTestAccess::Platform(Coal, false, 0.0, 0),
		FSTTestAccess::Platform(Coal, false, 0.0, 0), FSTTestAccess::Platform(Iron, false, 0.0, 0)};
	for (FSTTestAccess::FPlatformInfo& Platform : Platforms)
	{
		Platform.Slots = 48;
	}
	FSTTestAccess::FCarInfo Empty = FSTTestAccess::Car();
	Empty.Slots = 32;
	FSTTestAccess::FCarInfo HoldsCoal = FSTTestAccess::Car(Coal, 100.0);
	HoldsCoal.Slots = 32;
	// Both cars stop beside an iron platform at the mine; the factory takes iron then coal, the other station coal then iron.
	FSTTestAccess::FCarPlatforms CarPlatforms;
	CarPlatforms.Add(IronMine, {{0, 1}});
	CarPlatforms.Add(Factory, {{2, 3}});
	CarPlatforms.Add(Swapped, {{4, 5}});
	FSTTestAccess::SetTrain(Planner, Start, true, false, {Empty, Empty}, Platforms, CarPlatforms);
	FSTTestAccess::BuildMoves(Planner);
	const TArray<UClass*> Pattern = {Iron, Coal};
	TArray<TArray<UClass*>> Allowed;
	int32 Mismatched = 0;
	TestTrue(TEXT("the factory lines up with iron | coal"), Planner.MakeCarItems(Factory, Pattern, false, Allowed, Mismatched) && Mismatched == 0);
	TestTrue(TEXT("car 1 may only take iron"), Allowed.Num() == 2 && Allowed[0] == TArray<UClass*>{Iron});
	TestFalse(TEXT("the station taking coal then iron is another group"), Planner.MakeCarItems(Swapped, Pattern, false, Allowed, Mismatched));
	TestTrue(TEXT("a train with an open pattern may serve it"), Planner.MakeCarItems(Swapped, {nullptr, nullptr}, false, Allowed, Mismatched)
		&& Allowed.Num() == 2 && Allowed[0] == TArray<UClass*>{Coal});
	TestTrue(TEXT("so may one changing group"), Planner.MakeCarItems(Swapped, Pattern, true, Allowed, Mismatched) && Mismatched == 0);

	Planner.MakeCarItems(Factory, Pattern, false, Allowed, Mismatched);
	Planner.SetCarItems(Allowed);
	FSTPlanStop Pickup;
	Pickup.Station = IronMine;
	Pickup.LoadItems = {Iron};
	FSTPlanStop Drop;
	Drop.Station = Factory;
	Drop.UnloadItems = {Iron};
	const FSTPlanResult Result = Planner.Simulate({Pickup, Drop});
	if (TestTrue(FString::Printf(TEXT("the iron trip works (%s)"), *Result.Problem), Result.bValid && Result.Steps.Num() == 2 && Result.Steps[0].Num() == 2))
	{
		TestTrue(TEXT("car 1 loads iron"), Result.Steps[0][0].Loaded.Num() > 0);
		TestTrue(TEXT("car 2 (coal) skips the iron mine"), Result.Steps[0][1].bSkip && Result.Steps[0][1].Loaded.Num() == 0);
	}
	Planner.SetCarItems({});

	FSTTestAccess::SetTrain(Planner, Start, true, false, {HoldsCoal, Empty}, Platforms, CarPlatforms);
	FSTTestAccess::BuildMoves(Planner);
	TestTrue(TEXT("coal in car 1 at the factory is checked"), Planner.MakeCarItems(Factory, {nullptr, nullptr}, false, Allowed, Mismatched));
	TestEqual(TEXT("coal in car 1 doesn't line up with the factory's iron platform"), Mismatched, 1);
	TestTrue(TEXT("but lines up at the other station"), Planner.MakeCarItems(Swapped, {nullptr, nullptr}, false, Allowed, Mismatched) && Mismatched == 0);
	return true;
}

// Platforms of two stations feeding the same machines share their items out in proportion to car loads per minute:
// a fast item gets most of them, a slow one still gets one; a platform set up before counts and is left alone.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTShareOutByRateTest, "SmartTrains.Setup.ShareOutByRate", TestFlags)
bool FSTShareOutByRateTest::RunTest(const FString& Parameters)
{
	UClass* Fast = USTTestSolid::StaticClass();
	UClass* Slow = USTTestSolidB::StaticClass();
	AActor* Machine = NewObject<AActor>(GetTransientPackage());
	auto MakePreview = [&](bool bKeptSlow)
	{
		STMigration::FPreview Preview;
		for (int32 StationIndex = 0; StationIndex < 2; ++StationIndex)
		{
			STMigration::FStationProposal& Station = Preview.Stations.AddDefaulted_GetRef();
			for (int32 Index = 0; Index < 2; ++Index)
			{
				STMigration::FPlatformProposal& Platform = Station.Platforms.AddDefaulted_GetRef();
				Platform.Index = Index;
				Platform.Role = ESTRole::Demand;
				Platform.Machines.Add(Machine);
				if (bKeptSlow && StationIndex == 1 && Index == 1)
				{
					Platform.bKept = true;
					Platform.Items = {Slow};
					continue;
				}
				Platform.Items = {Fast};
				Platform.SpreadChoices = {Fast, Slow};
				Platform.SpreadRates = {1440.f, 144.f};
			}
		}
		STMigration::ShareOutItems(Preview);
		return Preview;
	};
	auto Count = [](const STMigration::FPreview& Preview, UClass* Item)
	{
		int32 Found = 0;
		for (const STMigration::FStationProposal& Station : Preview.Stations)
		{
			for (const STMigration::FPlatformProposal& Platform : Station.Platforms)
			{
				Found += Platform.Items.Num() == 1 && Platform.Items[0].Get() == Item ? 1 : 0;
			}
		}
		return Found;
	};
	const STMigration::FPreview Shared = MakePreview(false);
	TestEqual(TEXT("the fast item gets three of the four platforms"), Count(Shared, Fast), 3);
	TestEqual(TEXT("the slow item still gets one"), Count(Shared, Slow), 1);
	TestTrue(TEXT("the first station's platforms all take the fast item"), Count(Shared, Fast) >= 2
		&& Shared.Stations[0].Platforms[0].Items[0].Get() == Fast && Shared.Stations[0].Platforms[1].Items[0].Get() == Fast);
	const STMigration::FPreview Kept = MakePreview(true);
	TestEqual(TEXT("with a platform set up for the slow item, the other three take the fast one"), Count(Kept, Fast), 3);
	TestTrue(TEXT("the set-up platform keeps its item"), Kept.Stations[1].Platforms[1].Items[0].Get() == Slow);
	return true;
}

#endif
