#include "STModules.h"
#include "Engine/World.h"
#include "FGChatManager.h"
#include "Kismet/GameplayStatics.h"
#include "ModLoading/ModLoadingLibrary.h"
#include "SmartTrains.h"
#include "TimerManager.h"
#include "STChatCommand.h"
#include "STClipboardHooks.h"
#include "STConfig.h"
#include "STDepotSignal.h"
#include "STDockHooks.h"
#include "STLayout.h"
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
		FSTPlatformAccess::InstallSignalHook();
		STDepots::ResolveVanillaAssets();
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

void USTGameWorldModule::DispatchLifecycleEvent(ELifecyclePhase Phase)
{
	Super::DispatchLifecycleEvent(Phase);
	UWorld* World = GetWorld();
	if (Phase == ELifecyclePhase::POST_INITIALIZATION && World)
	{
		// After the load, so the chat is there to show it.
		World->GetTimerManager().SetTimer(mNoticeTimer, FTimerDelegate::CreateUObject(this, &USTGameWorldModule::NoteConflictingMods), 20.f, false);
	}
}

void USTGameWorldModule::NoteConflictingMods()
{
	UWorld* World = GetWorld();
	UGameInstance* Game = World ? World->GetGameInstance() : nullptr;
	UModLoadingLibrary* Mods = Game ? Game->GetSubsystem<UModLoadingLibrary>() : nullptr;
	if (!Mods)
	{
		return;
	}
	struct FConflict
	{
		const TCHAR* Match;
		const TCHAR* What;
	};
	static const FConflict Conflicts[] = {
		{TEXT("DynamicTimeTable"), TEXT("writes the timetables of trains in its groups; SmartTrains writes managed trains' timetables back, so the two fight over them. Keep managed trains out of its groups, or uninstall it.")},
		{TEXT("Dynamic Train Routes"), TEXT("rewrites train routes every few seconds; with Auto Trains installed both steer the same trains.")},
		{TEXT("RailDispatch"), TEXT("dispatches its own pool of trains; a train both mods manage gets two sets of orders. Switch each train on in only one of them.")},
	};
	TArray<FString> Found;
	for (const FModInfo& Mod : Mods->GetLoadedMods())
	{
		for (const FConflict& Each : Conflicts)
		{
			if (Mod.Name.Contains(Each.Match) || Mod.FriendlyName.Contains(Each.Match) || Mod.Name.Replace(TEXT(" "), TEXT("")).Contains(FString(Each.Match).Replace(TEXT(" "), TEXT(""))))
			{
				Found.Add(FString::Printf(TEXT("%s (%s) %s"), *Mod.FriendlyName, *Mod.Name, Each.What));
			}
		}
	}
	for (const FString& Line : Found)
	{
		UE_LOG(LogSmartTrains, Warning, TEXT("Installed with SmartTrains: %s"), *Line);
	}
	if (Found.Num() == 0 || IsRunningDedicatedServer())
	{
		return;
	}
	AFGChatManager* Chat = AFGChatManager::Get(World);
	if (!Chat)
	{
		return;
	}
	for (const FString& Line : Found)
	{
		FChatMessageStruct Message;
		Message.MessageType = EFGChatMessageType::CMT_SystemMessage;
		Message.MessageSender = FText::FromString(TEXT("SmartTrains"));
		Message.MessageSenderColor = FLinearColor(0.98f, 0.58f, 0.15f);
		Message.MessageText = FText::FromString(TEXT("Also installed: ") + Line);
		Chat->AddChatMessageToReceived(Message);
	}
}
