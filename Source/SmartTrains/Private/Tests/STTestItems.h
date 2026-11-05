#pragma once

#include "CoreMinimal.h"
#include "Resources/FGItemDescriptor.h"
#include "STTestItems.generated.h"

// Items for SmartTrains' automation tests (Private/Tests): abstract, so nothing in the game lists or makes them.

// A solid with stacks of 100.
UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTTestSolid : public UFGItemDescriptor
{
	GENERATED_BODY()

public:
	USTTestSolid()
	{
		mForm = EResourceForm::RF_SOLID;
		mCachedStackSize = 100;
	}
};

// A fluid whose stack stands in for a whole fluid platform or car (2,400 m3), since the editor's inventories don't
// report slot sizes.
UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTTestFluid : public UFGItemDescriptor
{
	GENERATED_BODY()

public:
	USTTestFluid()
	{
		mForm = EResourceForm::RF_LIQUID;
		mCachedStackSize = 2400000;
	}
};
