// Automation tests for the station set-up: names, the belt network's maximum flow, and sharing items out (Docs/Testing.md).

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "STMigration.h"
#include "STMinCostFlow.h"
#include "STStationSetup.h"
#include "Tests/STTestItems.h"

namespace
{
	constexpr EAutomationTestFlags SetupTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	const TCHAR* DefaultFormat = TEXT("{area} {product} {role} {n} ({rate})");
}

// Names fill the format, drop empty values with their brackets, and format rates like the game's numbers.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTSetupFormatTest, "SmartTrains.Setup.Format", SetupTestFlags)
bool FSTSetupFormatTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("all values"), STSetup::FormatName(DefaultFormat, TEXT("Grass Fields"), TEXT("Caterium Ingot"), TEXT("SRC"), 1, TEXT("240")),
		FString(TEXT("Grass Fields Caterium Ingot SRC 1 (240)")));
	TestEqual(TEXT("no rate: the brackets go"), STSetup::FormatName(DefaultFormat, TEXT("Grass Fields"), TEXT("Coal"), TEXT("In"), 2, FString()),
		FString(TEXT("Grass Fields Coal In 2")));
	TestEqual(TEXT("no area, no role, no number"), STSetup::FormatName(DefaultFormat, FString(), TEXT("Modular Frame"), FString(), 0, TEXT("288")),
		FString(TEXT("Modular Frame (288)")));
	TestEqual(TEXT("a leading separator goes"), STSetup::FormatName(TEXT("{area} - {product}"), FString(), TEXT("Rotor"), FString(), 0, FString()),
		FString(TEXT("Rotor")));
	TestEqual(TEXT("small rate keeps a decimal"), STSetup::FormatRate(7.5), FString(TEXT("7.5")));
	TestEqual(TEXT("whole small rate has none"), STSetup::FormatRate(2.0), FString(TEXT("2")));
	TestEqual(TEXT("no rate"), STSetup::FormatRate(0.0), FString());
	return true;
}

// A station keeps its number when it already has it; a new one takes the smallest free number, whatever rate the
// others carry.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTSetupNumberTest, "SmartTrains.Setup.Number", SetupTestFlags)
bool FSTSetupNumberTest::RunTest(const FString& Parameters)
{
	const FString Area = TEXT("Grass Fields");
	const FString Product = TEXT("Caterium Ingot");
	const FString Role = TEXT("Out");
	TestTrue(TEXT("matches with another rate"), STSetup::MatchesName(TEXT("Grass Fields Caterium Ingot Out 1 (1,200)"), DefaultFormat, Area, Product, Role, 1));
	TestTrue(TEXT("matches without a rate"), STSetup::MatchesName(TEXT("Grass Fields Caterium Ingot Out 1"), DefaultFormat, Area, Product, Role, 1));
	TestFalse(TEXT("1 is not 10"), STSetup::MatchesName(TEXT("Grass Fields Caterium Ingot Out 10 (240)"), DefaultFormat, Area, Product, Role, 1));
	TestFalse(TEXT("other product"), STSetup::MatchesName(TEXT("Grass Fields Copper Ingot Out 1 (240)"), DefaultFormat, Area, Product, Role, 1));
	TestFalse(TEXT("words in the rate"), STSetup::MatchesName(TEXT("Grass Fields Caterium Ingot Out 1 (old)"), DefaultFormat, Area, Product, Role, 1));

	TSet<FString> Taken = {TEXT("Grass Fields Caterium Ingot Out 1 (240)"), TEXT("Grass Fields Caterium Ingot Out 3"), TEXT("Train Station 12")};
	TestEqual(TEXT("first free number"), STSetup::PickNumber(DefaultFormat, Area, Product, Role, TEXT("Train Station 7"), Taken), 2);
	TestEqual(TEXT("keeps its own number"), STSetup::PickNumber(DefaultFormat, Area, Product, Role, TEXT("Grass Fields Caterium Ingot Out 5 (100)"), Taken), 5);
	TestEqual(TEXT("its number taken by another: next free"), STSetup::PickNumber(DefaultFormat, Area, Product, Role, TEXT("Grass Fields Caterium Ingot Out 3 (90)"), Taken), 2);
	TestEqual(TEXT("no {n}: no number"), STSetup::PickNumber(TEXT("{area} {product}"), Area, Product, Role, FString(), Taken), 0);
	return true;
}

// Belt capacity's max flow: two platforms' belts (780 each) and a third merged onto two belts before a manifold of
// machine belts; the two belts are the narrowest cut (the Dune Desert Circuit Board case, 2026-11-11).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTBeltMaxFlowTest, "SmartTrains.Setup.BeltMaxFlow", SetupTestFlags)
bool FSTBeltMaxFlowTest::RunTest(const FString& Parameters)
{
	// 0 source, 1 sink, 2..4 platform outputs, 5 merger, 6 manifold, 7..9 machines.
	FSTMinCostFlow Flow(10);
	for (int32 Platform = 2; Platform <= 4; ++Platform)
	{
		Flow.AddEdge(0, Platform, 1e12, 0.0);
		Flow.AddEdge(Platform, 5, 780.0, 0.0);
	}
	Flow.AddEdge(5, 6, 780.0, 0.0);
	Flow.AddEdge(5, 6, 780.0, 0.0);
	for (int32 Machine = 7; Machine <= 9; ++Machine)
	{
		Flow.AddEdge(6, Machine, 780.0, 0.0);
		Flow.AddEdge(Machine, 1, 1e12, 0.0);
	}
	TestEqual(TEXT("two belts carry 1,560/min"), Flow.SolveMaxFlow(0, 1), 1560.0, 0.01);
	const TArray<bool> Side = Flow.GetSourceSide(0);
	TestTrue(TEXT("the merger is on the source side"), Side[5]);
	TestFalse(TEXT("the manifold is past the cut"), Side[6]);
	return true;
}

// Platforms of two stations feeding the same machines share their items out in proportion to car loads per minute:
// a fast item gets most of them, a slow one still gets one; a platform set up before counts and is left alone.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTShareOutByRateTest, "SmartTrains.Setup.ShareOutByRate", SetupTestFlags)
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

// Lanes: four platforms reach the same machines, one through their second input and three through their first (a
// balancer per ingredient, IB Dune Desert Computer In 1). The three-platform lane takes the item used most, the
// single one the other, whatever their order; a lane with a platform set up already gives its item to the rest.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTShareOutLanesTest, "SmartTrains.Setup.ShareOutLanes", SetupTestFlags)
bool FSTShareOutLanesTest::RunTest(const FString& Parameters)
{
	UClass* Boards = USTTestSolid::StaticClass();
	UClass* Oscillators = USTTestSolidB::StaticClass();
	AActor* Machine = NewObject<AActor>(GetTransientPackage());
	auto MakePreview = [&](bool bKeptBoard)
	{
		STMigration::FPreview Preview;
		STMigration::FStationProposal& Station = Preview.Stations.AddDefaulted_GetRef();
		for (int32 Index = 0; Index < 4; ++Index)
		{
			STMigration::FPlatformProposal& Platform = Station.Platforms.AddDefaulted_GetRef();
			Platform.Index = Index;
			Platform.Role = ESTRole::Demand;
			Platform.Machines.Add(Machine);
			Platform.MachineInputs = {Index == 0 ? TEXT("Assembler|Input1") : TEXT("Assembler|Input0")};
			Platform.BeltRate = 1560.0;
			if (bKeptBoard && Index == 2)
			{
				Platform.bKept = true;
				Platform.Items = {Boards};
				continue;
			}
			// What the share-out by car loads alone would give: alternating.
			Platform.Items = {Index % 2 == 0 ? Boards : Oscillators};
			Platform.SpreadChoices = {Boards, Oscillators};
			Platform.SpreadRates = {810.f, 270.f};
		}
		STMigration::ShareOutItems(Preview);
		return Preview;
	};
	for (const bool bKept : {false, true})
	{
		const STMigration::FPreview Preview = MakePreview(bKept);
		const TArray<STMigration::FPlatformProposal>& Platforms = Preview.Stations[0].Platforms;
		TestTrue(FString::Printf(TEXT("the single-platform lane takes the less used item (kept: %d)"), bKept), Platforms[0].Items[0].Get() == Oscillators);
		for (int32 Index = 1; Index < 4; ++Index)
		{
			TestTrue(FString::Printf(TEXT("platform %d in the three-platform lane takes the most used item (kept: %d)"), Index + 1, bKept), Platforms[Index].Items[0].Get() == Boards);
		}
	}
	return true;
}

#endif
