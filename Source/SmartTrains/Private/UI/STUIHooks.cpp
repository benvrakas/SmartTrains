#include "UI/STUIHooks.h"
#include "Engine/Engine.h"
#include "Patching/WidgetBlueprintHookManager.h"
#include "SmartTrains.h"
#include "UI/STPlatformPanel.h"
#include "UI/STStationPanel.h"
#include "UI/STTrainPanel.h"
#include "UI/STWidgetUtils.h"

namespace
{
	// Right edge of the vanilla freight and fluid platform layouts (their background images), plus a gap.
	constexpr float FreightMenuWidth = 438.f + 10.f;
	constexpr float FluidMenuWidth = 755.f + 10.f;

	// Creates and registers one hook adding NewWidgetClass to the panel ParentName of the vanilla widget WidgetPath.
	UWidgetBlueprintHookData* AddHook(UObject* Outer, UWidgetBlueprintHookManager* Manager, const TCHAR* WidgetPath, FName ParentName,
		TSubclassOf<UUserWidget> NewWidgetClass, FName NewWidgetName, UWidgetBlueprintHookSlot* Slot = nullptr)
	{
		UWidgetBlueprintHookData* Hook = NewObject<UWidgetBlueprintHookData>(Outer);
		Hook->WidgetClass = TSoftClassPtr<UUserWidget>(FSoftObjectPath(WidgetPath));
		Hook->NewWidgetClass = NewWidgetClass;
		Hook->NewWidgetName = NewWidgetName;
		Hook->ParentWidgetName = ParentName;
		Hook->ParentWidgetType = EWidgetBlueprintHookParentType::Direct;
		Hook->SlotConfiguration = Slot;
		if (!Hook->WidgetClass.LoadSynchronous())
		{
			UE_LOG(LogSmartTrains, Error, TEXT("UI hook %s: vanilla widget %s not found"), *NewWidgetName.ToString(), WidgetPath);
			return nullptr;
		}
		if (!Hook->ResolveParentWidgetOnArchetype())
		{
			UE_LOG(LogSmartTrains, Error, TEXT("UI hook %s: panel %s not found in %s"), *NewWidgetName.ToString(), *ParentName.ToString(), WidgetPath);
			return nullptr;
		}
		Manager->RegisterWidgetBlueprintHook(Hook);
		UE_LOG(LogSmartTrains, Log, TEXT("UI hook %s added to %s in %s"), *NewWidgetName.ToString(), *ParentName.ToString(), WidgetPath);
		return Hook;
	}

	// Returns a canvas slot setup that places a widget, sized to its content, with its top left corner at (Left, 0).
	// The canvas grows to fit it, so the window becomes wider.
	UWidgetBlueprintHookSlot_Canvas* MakeBesideCanvasSlot(UObject* Outer, float Left)
	{
		UWidgetBlueprintHookSlot_Canvas* Slot = NewObject<UWidgetBlueprintHookSlot_Canvas>(Outer);
		Slot->LayoutData.Anchors = FAnchors(0.f, 0.f, 0.f, 0.f);
		Slot->LayoutData.Alignment = FVector2D(0.f, 0.f);
		Slot->LayoutData.Offsets = FMargin(Left, 0.f, 0.f, 0.f);
		Slot->bAutoSize = true;
		Slot->ZOrder = 20;
		return Slot;
	}
}

void STUIHooks::Register(UObject* Outer, TArray<UWidgetBlueprintHookData*>& OutHooks)
{
#if WITH_EDITOR
	UE_LOG(LogSmartTrains, Log, TEXT("Editor build: UI hooks not registered"));
#else
	if (IsRunningDedicatedServer())
	{
		UE_LOG(LogSmartTrains, Log, TEXT("Dedicated server: UI hooks not registered"));
		return;
	}
	UWidgetBlueprintHookManager* Manager = GEngine ? GEngine->GetEngineSubsystem<UWidgetBlueprintHookManager>() : nullptr;
	if (!Manager)
	{
		UE_LOG(LogSmartTrains, Error, TEXT("SML widget hook manager missing; no UI"));
		return;
	}
	auto Keep = [&OutHooks](UWidgetBlueprintHookData* Hook)
	{
		if (Hook)
		{
			OutHooks.Add(Hook);
		}
	};
	Keep(AddHook(Outer, Manager, STWidgetUtils::FreightPlatformMenuClass, TEXT("CanvasPanel_149"),
		USTPlatformPanel::StaticClass(), TEXT("ST_PlatformPanel"), MakeBesideCanvasSlot(Outer, FreightMenuWidth)));
	Keep(AddHook(Outer, Manager, STWidgetUtils::FluidPlatformMenuClass, TEXT("CanvasPanel_0"),
		USTPlatformPanel::StaticClass(), TEXT("ST_PlatformPanelFluid"), MakeBesideCanvasSlot(Outer, FluidMenuWidth)));
	// Vertical box parents get no slot setup: SML's generic slot setup dereferences a null slot for them.
	Keep(AddHook(Outer, Manager, STWidgetUtils::LocomotiveMenuClass, TEXT("VerticalBox_0"),
		USTTrainPanel::StaticClass(), TEXT("ST_TrainPanel")));
	Keep(AddHook(Outer, Manager, STWidgetUtils::StationMenuClass, TEXT("VerticalBox_2"),
		USTStationPanel::StaticClass(), TEXT("ST_StationPanel")));
	UE_LOG(LogSmartTrains, Log, TEXT("%d of 4 UI hooks registered"), OutHooks.Num());
#endif
}
