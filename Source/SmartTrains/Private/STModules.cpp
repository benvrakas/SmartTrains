#include "STModules.h"
#include "STChatCommand.h"
#include "STConfig.h"
#include "STSubsystem.h"

USTGameInstanceModule::USTGameInstanceModule()
{
	bRootModule = true;
	ModConfigurations.Add(USTConfig::StaticClass());
}

void USTGameInstanceModule::DispatchLifecycleEvent(ELifecyclePhase Phase)
{
	// SML registers the configuration during initialization; its editor classes must be in place first.
	if (Phase == ELifecyclePhase::INITIALIZATION)
	{
		USTConfig::UseSMLEditorClasses();
	}
	Super::DispatchLifecycleEvent(Phase);
}

USTGameWorldModule::USTGameWorldModule()
{
	bRootModule = true;
	ModSubsystems.Add(ASTSubsystem::StaticClass());
	mChatCommands.Add(ASTChatCommand::StaticClass());
}
