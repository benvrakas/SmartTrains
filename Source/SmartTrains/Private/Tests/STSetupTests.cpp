// Automation tests for the station set-up names (Docs/Testing.md).

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "STStationSetup.h"

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

#endif
