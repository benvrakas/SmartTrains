// Automation tests for the trip planner, the flow plan and the assignment (Docs/Testing.md, "Automated tests").
// Run in the editor: Tools/BuildScripts/test-smarttrains.bat, or "Automation RunTests SmartTrains" in its console.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGTrain.h"
#include "FGTrainStationIdentifier.h"
#include "Misc/AutomationTest.h"
#include "STForecast.h"
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

	static void BuildRoster(ASTSubsystem& Subsystem, TArray<FSTRosterRoute> Routes)
	{
		Subsystem.BuildRoster(MoveTemp(Routes));
	}

	static const TMap<TWeakObjectPtr<AFGTrain>, FSTRosterDuty>& GetRoster(const ASTSubsystem& Subsystem)
	{
		return Subsystem.mRoster;
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

	static bool MergeLoops(const ASTSubsystem& Subsystem, FSTService& Into, const FSTService& Other)
	{
		return Subsystem.MergeLoops(Into, Other);
	}

	static void LinkSupplyChain(TArray<FSTNode>& Nodes)
	{
		ASTSubsystem::LinkSupplyChain(Nodes);
	}

	static void AddServiceConfig(ASTSubsystem& Subsystem, const FSTServiceConfig& Config)
	{
		Subsystem.mServiceConfigs.Add(Config);
	}

	// Gives Train a repeat of its last trip (looping it, moving nothing, when bLoop).
	static void SetRepeating(ASTSubsystem& Subsystem, const AFGTrain* Train, bool bLoop)
	{
		for (FSTTrainState& State : Subsystem.mTrains)
		{
			if (State.Train == Train)
			{
				State.bHasJob = true;
				State.Job = FSTJob();
				State.Job.bRepeat = true;
				State.Job.bLoop = bLoop;
			}
		}
	}

	static bool IsShuttleRoute(const ASTSubsystem& Subsystem, const AFGTrain* Train, const FSTRosterRoute& Route)
	{
		return Subsystem.IsShuttleRoute(Train, Route.From.Get(), Route.To.Get(), Route.Item.Get());
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
		// Near the sink, the car doesn't line up with the consumers of what it holds, so the sink may take it.
		for (int32 Index = bSinkNear ? 2 : 0; Index < 5; ++Index)
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

// The roster shares the flow plan's routes out over trains that fit them, no train above one train's worth, once a route
// has been in three flow plans; duties stay while the routes hardly change, and a new route doesn't move the others.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTRosterTest, "SmartTrains.Planner.Roster", TestFlags)
bool FSTRosterTest::RunTest(const FString& Parameters)
{
	ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
	TArray<AFGTrain*> Trains;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Trains.Add(NewObject<AFGTrain>(GetTransientPackage()));
	}
	auto Route = [&](double Needed, const TArray<int32>& Fitting)
	{
		FSTRosterRoute Each;
		Each.From = FSTTestAccess::MakeStation();
		Each.To = FSTTestAccess::MakeStation();
		Each.Item = USTTestSolid::StaticClass();
		Each.Trains = Needed;
		for (const int32 Index : Fitting)
		{
			Each.Fitting.Add(Trains[Index]);
		}
		return Each;
	};
	TArray<FSTRosterRoute> Routes = {Route(1.4, {0, 1, 2}), Route(0.6, {1, 2}), Route(0.3, {2})};
	const TMap<TWeakObjectPtr<AFGTrain>, FSTRosterDuty>& Roster = FSTTestAccess::GetRoster(*Subsystem);
	FSTTestAccess::BuildRoster(*Subsystem, Routes);
	TestEqual(TEXT("a route seen once isn't on the roster yet"), Roster.Num(), 0);
	FSTTestAccess::BuildRoster(*Subsystem, Routes);
	FSTTestAccess::BuildRoster(*Subsystem, Routes);
	double Assigned = 0.0;
	for (const TPair<TWeakObjectPtr<AFGTrain>, FSTRosterDuty>& Pair : Roster)
	{
		TestTrue(TEXT("no train is given more than one train's worth"), Pair.Value.Load <= 1.0 + 1e-6);
		Assigned += Pair.Value.Load;
	}
	TestEqual(TEXT("all 2.3 trains' worth of routes fit on 3 trains"), Assigned, 2.3, 1e-6);
	const FSTRosterDuty* Third = Roster.Find(Trains[2]);
	TestTrue(TEXT("the route only the third train fits is on it"), Third && Third->Routes.Contains(2));

	TMap<TWeakObjectPtr<AFGTrain>, TArray<int32>> Before;
	for (const TPair<TWeakObjectPtr<AFGTrain>, FSTRosterDuty>& Pair : Roster)
	{
		Before.Add(Pair.Key, Pair.Value.Routes);
	}
	Routes[0].Trains = 1.5;
	FSTTestAccess::BuildRoster(*Subsystem, Routes);
	bool bSame = Roster.Num() == Before.Num();
	for (const TPair<TWeakObjectPtr<AFGTrain>, FSTRosterDuty>& Pair : Roster)
	{
		const TArray<int32>* Old = Before.Find(Pair.Key);
		bSame &= Old && *Old == Pair.Value.Routes;
	}
	TestTrue(TEXT("a small change keeps every duty"), bSame);

	// A small new route that only the first train fits: the others keep their duties.
	TArray<FSTRosterRoute> More = Routes;
	More.Add(Route(0.05, {0}));
	TMap<TWeakObjectPtr<AFGTrain>, double> Loads;
	for (const TPair<TWeakObjectPtr<AFGTrain>, FSTRosterDuty>& Pair : Roster)
	{
		Loads.Add(Pair.Key, Pair.Value.Load);
	}
	for (int32 Plan = 0; Plan < 3; ++Plan)
	{
		FSTTestAccess::BuildRoster(*Subsystem, More);
	}
	const FSTRosterDuty* Second = Roster.Find(Trains[1]);
	const FSTRosterDuty* Last = Roster.Find(Trains[2]);
	TestTrue(TEXT("a new route leaves the other trains' duties as they were"), Second && Last && FMath::IsNearlyEqual(Second->Load, Loads.FindRef(Trains[1]))
		&& FMath::IsNearlyEqual(Last->Load, Loads.FindRef(Trains[2])));
	return true;
}

// A route that keeps a whole train busy gets a shuttle (a whole train running only it) that other trains leave its route
// to; a small route shares a train. The shuttle stays while the route needs half a train, and goes below that.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTRosterShuttleTest, "SmartTrains.Planner.RosterShuttle", TestFlags)
bool FSTRosterShuttleTest::RunTest(const FString& Parameters)
{
	ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
	TArray<AFGTrain*> Trains;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Trains.Add(NewObject<AFGTrain>(GetTransientPackage()));
		FSTTestAccess::AddManaged(*Subsystem, Trains.Last());
	}
	// Each train carries 9600 per trip; the second carries only 3200 because one car fits.
	TArray<AFGTrainStationIdentifier*> Stations;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Stations.Add(FSTTestAccess::MakeStation());
	}
	auto Route = [&](int32 Which, double PerTrip, UClass* Item = USTTestSolid::StaticClass())
	{
		FSTRosterRoute Each;
		Each.From = Stations[2 * Which];
		Each.To = Stations[2 * Which + 1];
		Each.Item = Item;
		Each.RoundTrip = 600.0;
		Each.Flow = PerTrip / Each.RoundTrip;
		for (int32 Index = 0; Index < Trains.Num(); ++Index)
		{
			Each.Fitting.Add(Trains[Index]);
			Each.Capacity.Add(Index == 1 ? 3200 : 9600);
			Each.Whole.Add(Index != 1);
		}
		Each.Trains = PerTrip / ((9600.0 + 3200.0 + 9600.0) / 3.0);
		return Each;
	};
	TArray<FSTRosterRoute> Routes = {Route(0, 9600.0), Route(1, 2000.0, USTTestSolidB::StaticClass())};
	const TMap<TWeakObjectPtr<AFGTrain>, FSTRosterDuty>& Roster = FSTTestAccess::GetRoster(*Subsystem);
	for (int32 Plan = 0; Plan < 3; ++Plan)
	{
		FSTTestAccess::BuildRoster(*Subsystem, Routes);
	}
	TWeakObjectPtr<AFGTrain> Shuttle;
	int32 Shuttles = 0;
	for (const TPair<TWeakObjectPtr<AFGTrain>, FSTRosterDuty>& Pair : Roster)
	{
		if (Pair.Value.bShuttle)
		{
			Shuttle = Pair.Key;
			++Shuttles;
			TestEqual(TEXT("a shuttle runs one route"), Pair.Value.Routes.Num(), 1);
		}
	}
	TestEqual(TEXT("the big route gets one shuttle"), Shuttles, 1);
	TestTrue(TEXT("the shuttle is a train whose cars all fit"), Shuttle.IsValid() && Shuttle.Get() != Trains[1]);
	TestFalse(TEXT("the shuttle plans its route"), FSTTestAccess::IsShuttleRoute(*Subsystem, Shuttle.Get(), Routes[0]));
	TestTrue(TEXT("other trains leave the shuttle's route to it"), FSTTestAccess::IsShuttleRoute(*Subsystem, Trains[1], Routes[0]));
	TestFalse(TEXT("the small route has no shuttle"), FSTTestAccess::IsShuttleRoute(*Subsystem, Trains[1], Routes[1]));
	const bool bSmallShared = Roster.FilterByPredicate([](const TPair<TWeakObjectPtr<AFGTrain>, FSTRosterDuty>& Pair) { return !Pair.Value.bShuttle && Pair.Value.Load > 0.0; }).Num() > 0;
	TestTrue(TEXT("the small route shares a train"), bSmallShared);

	auto ShuttleOf = [&]()
	{
		const FSTRosterDuty* Duty = Roster.Find(Shuttle);
		return Duty && Duty->bShuttle;
	};
	Routes[0] = Route(0, 5760.0);
	FSTTestAccess::BuildRoster(*Subsystem, Routes);
	TestTrue(TEXT("the shuttle stays while its route needs more than half of it"), ShuttleOf());
	Routes[0] = Route(0, 3840.0);
	FSTTestAccess::BuildRoster(*Subsystem, Routes);
	TestTrue(TEXT("one plan needing less doesn't end the service yet"), ShuttleOf());
	for (int32 Plan = 0; Plan < 6; ++Plan)
	{
		FSTTestAccess::BuildRoster(*Subsystem, Routes);
	}
	TestFalse(TEXT("the shuttle goes when its route keeps needing less than half of it"), ShuttleOf());
	TestFalse(TEXT("then any train may plan the route"), FSTTestAccess::IsShuttleRoute(*Subsystem, Trains[1], Routes[0]));
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

// Shuttles are per item: two routes of one item that each need 0.6 of a train get one shuttle running both, and other
// trains leave both to it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTShuttlePerItemTest, "SmartTrains.Planner.ShuttlePerItem", TestFlags)
bool FSTShuttlePerItemTest::RunTest(const FString& Parameters)
{
	ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
	TArray<AFGTrain*> Trains;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Trains.Add(NewObject<AFGTrain>(GetTransientPackage()));
		FSTTestAccess::AddManaged(*Subsystem, Trains.Last());
	}
	TArray<AFGTrainStationIdentifier*> Stations;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Stations.Add(FSTTestAccess::MakeStation());
	}
	auto Route = [&](int32 Which, double PerTrip)
	{
		FSTRosterRoute Each;
		Each.From = Stations[2 * Which];
		Each.To = Stations[2 * Which + 1];
		Each.Item = USTTestSolid::StaticClass();
		Each.RoundTrip = 600.0;
		Each.Flow = PerTrip / Each.RoundTrip;
		for (AFGTrain* Train : Trains)
		{
			Each.Fitting.Add(Train);
			Each.Capacity.Add(9600);
			Each.Whole.Add(true);
		}
		Each.Trains = PerTrip / 9600.0;
		return Each;
	};
	TArray<FSTRosterRoute> Routes = {Route(0, 5760.0), Route(1, 5760.0)};
	const TMap<TWeakObjectPtr<AFGTrain>, FSTRosterDuty>& Roster = FSTTestAccess::GetRoster(*Subsystem);
	for (int32 Plan = 0; Plan < 3; ++Plan)
	{
		FSTTestAccess::BuildRoster(*Subsystem, Routes);
	}
	TWeakObjectPtr<AFGTrain> Shuttle;
	int32 Shuttles = 0;
	for (const TPair<TWeakObjectPtr<AFGTrain>, FSTRosterDuty>& Pair : Roster)
	{
		if (Pair.Value.bShuttle)
		{
			Shuttle = Pair.Key;
			++Shuttles;
			TestEqual(TEXT("the shuttle runs both routes of the item"), Pair.Value.Routes.Num(), 2);
		}
	}
	TestEqual(TEXT("the item gets one shuttle"), Shuttles, 1);
	AFGTrain* Other = Trains[0] == Shuttle.Get() ? Trains[1] : Trains[0];
	TestTrue(TEXT("other trains leave the first route to it"), FSTTestAccess::IsShuttleRoute(*Subsystem, Other, Routes[0]));
	TestTrue(TEXT("and the second"), FSTTestAccess::IsShuttleRoute(*Subsystem, Other, Routes[1]));
	FSTTestAccess::SetRepeating(*Subsystem, Shuttle.Get(), true);
	TestFalse(TEXT("a shuttle looping its last trip leaves its routes to others"), FSTTestAccess::IsShuttleRoute(*Subsystem, Other, Routes[0]));
	Routes = {Route(0, 1920.0), Route(1, 1920.0)};
	for (int32 Plan = 0; Plan < 7; ++Plan)
	{
		FSTTestAccess::BuildRoster(*Subsystem, Routes);
	}
	const FSTRosterDuty* Duty = Roster.Find(Shuttle);
	TestFalse(TEXT("the shuttle goes when the item needs less than half of it"), Duty && Duty->bShuttle);
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
		// Past the batch rule, so only the least load decides.
		Load.NoBatchItems.Add(Ingot);
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

// Service settings: a pinned item keeps a service train though it needs less than one; a pooled one gets none.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTServiceSettingsTest, "SmartTrains.Planner.ServiceSettings", TestFlags)
bool FSTServiceSettingsTest::RunTest(const FString& Parameters)
{
	auto Shuttles = [](bool bPinned, bool bPooled, double PerTrip)
	{
		ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
		TArray<AFGTrain*> Trains;
		for (int32 Index = 0; Index < 3; ++Index)
		{
			Trains.Add(NewObject<AFGTrain>(GetTransientPackage()));
			FSTTestAccess::AddManaged(*Subsystem, Trains.Last());
		}
		FSTServiceConfig Config;
		Config.Item = USTTestSolid::StaticClass();
		Config.bPinned = bPinned;
		Config.bPooled = bPooled;
		FSTTestAccess::AddServiceConfig(*Subsystem, Config);
		FSTRosterRoute Route;
		Route.From = FSTTestAccess::MakeStation();
		Route.To = FSTTestAccess::MakeStation();
		Route.Item = USTTestSolid::StaticClass();
		Route.RoundTrip = 600.0;
		Route.Flow = PerTrip / Route.RoundTrip;
		for (AFGTrain* Train : Trains)
		{
			Route.Fitting.Add(Train);
			Route.Capacity.Add(9600);
			Route.Whole.Add(true);
		}
		Route.Trains = PerTrip / 9600.0;
		for (int32 Plan = 0; Plan < 3; ++Plan)
		{
			FSTTestAccess::BuildRoster(*Subsystem, {Route});
		}
		int32 Count = 0;
		for (const TPair<TWeakObjectPtr<AFGTrain>, FSTRosterDuty>& Pair : FSTTestAccess::GetRoster(*Subsystem))
		{
			Count += Pair.Value.bShuttle ? 1 : 0;
		}
		return Count;
	};
	TestEqual(TEXT("a small route gets no service by itself"), Shuttles(false, false, 2000.0), 0);
	TestEqual(TEXT("pinned, it gets one"), Shuttles(true, false, 2000.0), 1);
	TestEqual(TEXT("a big route gets one"), Shuttles(false, false, 9600.0), 1);
	TestEqual(TEXT("pooled, even a big route gets none"), Shuttles(false, true, 9600.0), 0);
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

// Service cars: each flow gets the cars that line up with its supply and demand platforms; a car carries flows one
// after another when their stretches of the loop don't overlap.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTServiceCarsTest, "SmartTrains.Services.Cars", TestFlags)
bool FSTServiceCarsTest::RunTest(const FString& Parameters)
{
	UClass* Ingot = USTTestSolid::StaticClass();
	UClass* Concrete = USTTestSolidB::StaticClass();
	ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
	AFGTrainStationIdentifier* Start = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Mine = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Quarry = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Factory = FSTTestAccess::MakeStation();
	AFGTrainStationIdentifier* Other = FSTTestAccess::MakeStation();
	{
		// Two cars: ingots from the mine in either, concrete from the quarry only in car 2; at the factory car 1 meets
		// the ingot platform and car 2 the concrete one.
		TArray<FSTNode> Nodes;
		TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;
		FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);
		TArray<FSTTestAccess::FPlatformInfo> Platforms = {FSTTestAccess::Platform(Ingot, true, 0.0, 0), FSTTestAccess::Platform(Ingot, true, 0.0, 0),
			FSTTestAccess::Platform(Concrete, true, 0.0, 0), FSTTestAccess::Platform(Ingot, false, 0.0, 0), FSTTestAccess::Platform(Concrete, false, 0.0, 0)};
		FSTTestAccess::FCarInfo Car = FSTTestAccess::Car();
		Car.Slots = 32;
		FSTTestAccess::FCarPlatforms CarPlatforms;
		CarPlatforms.Add(Mine, {{0, 1}});
		CarPlatforms.Add(Quarry, {{INDEX_NONE, 2}});
		CarPlatforms.Add(Factory, {{3, 4}});
		FSTTestAccess::SetTrain(Planner, Start, true, false, {Car, Car}, Platforms, CarPlatforms);
		FSTPlanStop A;
		A.Station = Mine;
		A.LoadItems.Add(Ingot);
		FSTPlanStop B;
		B.Station = Quarry;
		B.LoadItems.Add(Concrete);
		FSTPlanStop C;
		C.Station = Factory;
		C.UnloadItems = {Ingot, Concrete};
		const TArray<double> PerTrain = Planner.AllocateServiceCars({A, B, C}, {{Mine, Factory, Ingot, 1.0}, {Quarry, Factory, Concrete, 1.0}});
		TestTrue(FString::Printf(TEXT("both inputs get a car (%.0f, %.0f)"), PerTrain[0], PerTrain[1]), PerTrain[0] > 0.0 && PerTrain[1] > 0.0);
	}
	{
		// One car, two flows one after the other along the loop: it carries both.
		TArray<FSTNode> Nodes;
		TMap<const AFGTrainStationIdentifier*, TArray<FSTBooking>> Bookings;
		FSTTripPlanner Planner(*Subsystem, Nodes, Bookings);
		TArray<FSTTestAccess::FPlatformInfo> Platforms = {FSTTestAccess::Platform(Ingot, true, 0.0, 0), FSTTestAccess::Platform(Ingot, false, 0.0, 0),
			FSTTestAccess::Platform(Concrete, true, 0.0, 0), FSTTestAccess::Platform(Concrete, false, 0.0, 0)};
		FSTTestAccess::FCarInfo Car = FSTTestAccess::Car();
		Car.Slots = 32;
		FSTTestAccess::FCarPlatforms CarPlatforms;
		CarPlatforms.Add(Mine, {{0}});
		CarPlatforms.Add(Factory, {{1}});
		CarPlatforms.Add(Quarry, {{2}});
		CarPlatforms.Add(Other, {{3}});
		FSTTestAccess::SetTrain(Planner, Start, true, false, {Car}, Platforms, CarPlatforms);
		FSTPlanStop A;
		A.Station = Mine;
		A.LoadItems.Add(Ingot);
		FSTPlanStop B;
		B.Station = Factory;
		B.UnloadItems.Add(Ingot);
		FSTPlanStop C;
		C.Station = Quarry;
		C.LoadItems.Add(Concrete);
		FSTPlanStop D;
		D.Station = Other;
		D.UnloadItems.Add(Concrete);
		const TArray<double> PerTrain = Planner.AllocateServiceCars({A, B, C, D}, {{Mine, Factory, Ingot, 1.0}, {Quarry, Other, Concrete, 1.0}});
		TestTrue(FString::Printf(TEXT("one car carries both, one after the other (%.0f, %.0f)"), PerTrain[0], PerTrain[1]), PerTrain[0] > 0.0 && PerTrain[1] > 0.0);
	}
	return true;
}

// Merging loops: the second loop's stops go into the first so every flow still picks up before it drops.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTServiceMergeTest, "SmartTrains.Services.Merge", TestFlags)
bool FSTServiceMergeTest::RunTest(const FString& Parameters)
{
	ASTSubsystem* Subsystem = FSTTestAccess::MakeSubsystem();
	TArray<AFGTrainStationIdentifier*> Stations;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Stations.Add(FSTTestAccess::MakeStation());
	}
	for (AFGTrainStationIdentifier* From : Stations)
	{
		for (AFGTrainStationIdentifier* To : Stations)
		{
			if (From != To)
			{
				FSTTestAccess::SetTrip(*Subsystem, From, To, 100.f);
			}
		}
	}
	auto Loop = [&](int32 Pick, int32 Drop, UClass* Item)
	{
		FSTService Service;
		FSTServiceStop& From = Service.Stops.AddDefaulted_GetRef();
		From.Station = Stations[Pick];
		From.LoadItems.Add(Item);
		FSTServiceStop& To = Service.Stops.AddDefaulted_GetRef();
		To.Station = Stations[Drop];
		To.UnloadItems.Add(Item);
		FSTServiceFlow& Flow = Service.Flows.AddDefaulted_GetRef();
		Flow.From = Stations[Pick];
		Flow.To = Stations[Drop];
		Flow.Item = Item;
		return Service;
	};
	FSTService Merged = Loop(0, 1, USTTestSolid::StaticClass());
	TestTrue(TEXT("two loops merge"), FSTTestAccess::MergeLoops(*Subsystem, Merged, Loop(2, 3, USTTestSolidB::StaticClass())));
	TestEqual(TEXT("into four stops"), Merged.Stops.Num(), 4);
	for (const FSTServiceFlow& Flow : Merged.Flows)
	{
		const int32 Pick = Merged.Stops.IndexOfByPredicate([&](const FSTServiceStop& Stop) { return Stop.Station == Flow.From; });
		const int32 Drop = Merged.Stops.IndexOfByPredicate([&](const FSTServiceStop& Stop) { return Stop.Station == Flow.To; });
		TestTrue(TEXT("each flow picks up before it drops"), Pick != INDEX_NONE && Drop != INDEX_NONE && Pick < Drop);
	}
	FSTService Shared = Loop(0, 1, USTTestSolid::StaticClass());
	TestTrue(TEXT("a loop sharing the supplier merges into a milk run"), FSTTestAccess::MergeLoops(*Subsystem, Shared, Loop(0, 2, USTTestSolid::StaticClass())));
	TestEqual(TEXT("with three stops"), Shared.Stops.Num(), 3);
	return true;
}

#endif
