#pragma once

#include "CoreMinimal.h"
#include "Module/GameInstanceModule.h"
#include "Module/GameWorldModule.h"
#include "STModules.generated.h"

// SmartTrains' root game instance module; registers the configuration.
UCLASS()
class SMARTTRAINS_API USTGameInstanceModule : public UGameInstanceModule
{
	GENERATED_BODY()

public:
	USTGameInstanceModule();

	virtual void DispatchLifecycleEvent(ELifecyclePhase Phase) override;
};

// SmartTrains' root game world module; registers the subsystem and the chat command.
UCLASS()
class SMARTTRAINS_API USTGameWorldModule : public UGameWorldModule
{
	GENERATED_BODY()

public:
	USTGameWorldModule();
};
