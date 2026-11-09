#pragma once

#include "CoreMinimal.h"
#include "Resources/FGItemDescriptor.h"
#include "STReplayItems.generated.h"

// Stand-ins for the items of a replayed snapshot (SmartTrains.Replay.Snapshots): the editor has no game items, so each
// snapshot item gets one of these, with its form and stack size set on the class default. Abstract and hidden.
UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItemBase : public UFGItemDescriptor
{
	GENERATED_BODY()

public:
	void Configure(bool bFluid, int32 Stack)
	{
		mForm = bFluid ? EResourceForm::RF_LIQUID : EResourceForm::RF_SOLID;
		mCachedStackSize = FMath::Max(1, Stack);
	}
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem00 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem01 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem02 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem03 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem04 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem05 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem06 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem07 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem08 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem09 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem10 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem11 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem12 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem13 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem14 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem15 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem16 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem17 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem18 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem19 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem20 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem21 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem22 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem23 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem24 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem25 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem26 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem27 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem28 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem29 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem30 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem31 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem32 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem33 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem34 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem35 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem36 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem37 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem38 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem39 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem40 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem41 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem42 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem43 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem44 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem45 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem46 : public USTReplayItemBase
{
	GENERATED_BODY()
};

UCLASS(Abstract, NotBlueprintable, HideDropdown)
class USTReplayItem47 : public USTReplayItemBase
{
	GENERATED_BODY()
};

// The stand-in classes, in order.
TArray<UClass*> STReplayItemClasses();
