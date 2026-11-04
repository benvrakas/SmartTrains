#pragma once

#include "CoreMinimal.h"

class UObject;
class UWidgetBlueprintHookData;

// Adds SmartTrains' panels to the vanilla platform, locomotive and station menus.
namespace STUIHooks
{
	// Registers the widget blueprint hooks; Outer owns the hook data, which is also added to OutHooks to keep it alive.
	void Register(UObject* Outer, TArray<UWidgetBlueprintHookData*>& OutHooks);
}
