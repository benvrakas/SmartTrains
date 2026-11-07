#include "STModules.h"
#include "STChatCommand.h"
#include "STClipboardHooks.h"
#include "STConfig.h"
#include "STDockHooks.h"
#include "STRemoteCallObject.h"
#include "STSubsystem.h"
#include "Patching/WidgetBlueprintHookManager.h"
#include "UI/STUIHooks.h"

USTGameInstanceModule::USTGameInstanceModule()
{
	bRootModule = true;
	ModConfigurations.Add(USTConfig::StaticClass());
	RemoteCallObjects.Add(USTRemoteCallObject::StaticClass());
}

void USTGameInstanceModule::DispatchLifecycleEvent(ELifecyclePhase Phase)
{
	// SML registers the configuration during initialization; its editor classes must be in place first.
	if (Phase == ELifecyclePhase::INITIALIZATION)
	{
		USTConfig::UseSMLEditorClasses();
	}
	else if (Phase == ELifecyclePhase::POST_INITIALIZATION && mWidgetHooks.Num() == 0)
	{
		STClipboardHooks::Install();
		STDockHooks::Install();
		TArray<UWidgetBlueprintHookData*> Hooks;
		STUIHooks::Register(this, Hooks);
		mWidgetHooks.Append(Hooks);
	}
	Super::DispatchLifecycleEvent(Phase);
}

USTGameWorldModule::USTGameWorldModule()
{
	bRootModule = true;
	ModSubsystems.Add(ASTSubsystem::StaticClass());
	mChatCommands.Add(ASTChatCommand::StaticClass());
}
