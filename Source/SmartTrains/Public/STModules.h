#pragma once

#include "CoreMinimal.h"
#include "Module/GameInstanceModule.h"
#include "Module/GameWorldModule.h"
#include "STModules.generated.h"

class UWidgetBlueprintHookData;

// SmartTrains' root game instance module; registers the configuration, the remote call object, the UI and the
// clipboard hooks.
UCLASS()
class SMARTTRAINS_API USTGameInstanceModule : public UGameInstanceModule
{
	GENERATED_BODY()

public:
	USTGameInstanceModule();

	virtual void DispatchLifecycleEvent(ELifecyclePhase Phase) override;

private:
	UPROPERTY()
	TArray<TObjectPtr<UWidgetBlueprintHookData>> mWidgetHooks;
};

// SmartTrains' root game world module; registers the subsystem and the chat command.
UCLASS()
class SMARTTRAINS_API USTGameWorldModule : public UGameWorldModule
{
	GENERATED_BODY()

public:
	USTGameWorldModule();
};
