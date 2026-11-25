// Automation tests for the loop plan's arithmetic (Docs/Testing.md, "Automated tests").
// Run in the editor: sfmod test SmartTrains, or "Automation RunTests SmartTrains" in its console.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "STLoopMath.h"

namespace
{
	constexpr EAutomationTestFlags LoopTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	int32 Sum(const TArray<int32>& Values)
	{
		int32 Total = 0;
		for (const int32 Value : Values)
		{
			Total += Value;
		}
		return Total;
	}
}

// Trains split in proportion to need by largest remainder, and every train is given out.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTLoopSplitTest, "SmartTrains.Loops.Split", LoopTestFlags)
bool FSTLoopSplitTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("1x and 2x with 3 trains"), STLoopMath::Split({1.0, 2.0}, 3), TArray<int32>({1, 2}));
	TestEqual(TEXT("1x and 2x with 6 trains"), STLoopMath::Split({1.0, 2.0}, 6), TArray<int32>({2, 4}));
	TestEqual(TEXT("1x and 2x with 4 trains: the remainder to the larger"), STLoopMath::Split({1.0, 2.0}, 4), TArray<int32>({1, 3}));
	TestEqual(TEXT("three equal with 7 trains: the extra one to the first"), STLoopMath::Split({1.0, 1.0, 1.0}, 7), TArray<int32>({3, 2, 2}));
	const TArray<int32> Uneven = STLoopMath::Split({0.7, 2.9, 1.3, 5.1}, 23);
	TestEqual(TEXT("every train given out"), Sum(Uneven), 23);
	TestTrue(TEXT("larger need, at least as many trains"), Uneven[3] >= Uneven[1] && Uneven[1] >= Uneven[2] && Uneven[2] >= Uneven[0]);
	TestEqual(TEXT("no need, no train"), STLoopMath::Split({0.0, 3.0}, 5), TArray<int32>({0, 5}));
	TestEqual(TEXT("no trains"), STLoopMath::Split({1.0, 2.0}, 0), TArray<int32>({0, 0}));
	TestEqual(TEXT("no consumers"), STLoopMath::Split({}, 4), TArray<int32>());
	// With enough trains every consumer with a need has one, however small its share.
	TestEqual(TEXT("a tiny need still gets one"), STLoopMath::Split({0.01, 10.0}, 3), TArray<int32>({1, 2}));
	return true;
}

// With fewer trains than consumers, the consumers with the smallest needs go unserved.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTLoopUnservedTest, "SmartTrains.Loops.Unserved", LoopTestFlags)
bool FSTLoopUnservedTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("two trains, three consumers"), STLoopMath::Split({1.0, 3.0, 2.0}, 2), TArray<int32>({0, 1, 1}));
	TestEqual(TEXT("one train: the largest need"), STLoopMath::Split({1.0, 3.0, 2.0}, 1), TArray<int32>({0, 1, 0}));
	TestEqual(TEXT("ties: the lower index"), STLoopMath::Split({2.0, 2.0, 2.0}, 2), TArray<int32>({1, 1, 0}));
	return true;
}

// A consumer takes its item from the nearest supplier with spare rate, then the next nearest.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSTLoopSupplierTest, "SmartTrains.Loops.Suppliers", LoopTestFlags)
bool FSTLoopSupplierTest::RunTest(const FString& Parameters)
{
	{
		TArray<STLoopMath::FOffer> Offers = {{300.0, 5.0}, {100.0, 2.0}, {200.0, 10.0}};
		const TArray<double> Taken = STLoopMath::Allocate(6.0, Offers);
		TestEqual(TEXT("nearest first, all it has"), Taken[1], 2.0);
		TestEqual(TEXT("next nearest the rest"), Taken[2], 4.0);
		TestEqual(TEXT("the farthest none"), Taken[0], 0.0);
		TestEqual(TEXT("spare lowered"), Offers[2].Spare, 6.0);
	}
	{
		TArray<STLoopMath::FOffer> Offers = {{100.0, 1.0}, {200.0, -1.0}};
		const TArray<double> Taken = STLoopMath::Allocate(5.0, Offers);
		TestEqual(TEXT("a supplier of unknown rate takes all that is left"), Taken[1], 4.0);
	}
	{
		TArray<STLoopMath::FOffer> Offers = {{100.0, 1.0}, {200.0, 1.0}};
		const TArray<double> Taken = STLoopMath::Allocate(5.0, Offers);
		TestEqual(TEXT("not enough anywhere: the nearest takes the rest"), Taken[0], 4.0);
		TestEqual(TEXT("the other gives what it has"), Taken[1], 1.0);
	}
	{
		TArray<STLoopMath::FOffer> Offers = {{100.0, 0.0}, {200.0, 3.0}};
		const TArray<double> Taken = STLoopMath::Allocate(2.0, Offers);
		TestEqual(TEXT("a supplier with no spare is passed over"), Taken[0], 0.0);
		TestEqual(TEXT("the next one gives it"), Taken[1], 2.0);
	}
	return true;
}

#endif
