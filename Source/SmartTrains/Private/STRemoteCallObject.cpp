#include "STRemoteCallObject.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGPlayerController.h"
#include "FGTrain.h"
#include "FGTrainStationIdentifier.h"
#include "GameFramework/PlayerState.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "SmartTrains.h"
#include "STLayout.h"
#include "STMigration.h"
#include "STSubsystem.h"
#include "UI/STMigrationWindow.h"

namespace
{
	// Most item rules one platform may have.
	constexpr int32 MaxItems = 32;

	// Most words a migration selection may have.
	constexpr int32 MaxSelectionWords = 256;
}

void USTRemoteCallObject::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(USTRemoteCallObject, mForceNetField_USTRemoteCallObject);
}

USTRemoteCallObject* USTRemoteCallObject::GetLocal(const UObject* WorldContext)
{
	AFGPlayerController* Controller = WorldContext ? Cast<AFGPlayerController>(UGameplayStatics::GetPlayerController(WorldContext, 0)) : nullptr;
	if (!Controller)
	{
		return nullptr;
	}
	USTRemoteCallObject* RCO = Controller->GetRemoteCallObjectOfClass<USTRemoteCallObject>();
	if (!RCO)
	{
		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogSmartTrains, Warning, TEXT("Remote call object not registered on the local player (is SmartTrains installed on the server?); menus stay empty"));
		}
	}
	return RCO;
}

FString USTRemoteCallObject::GetPlayerName() const
{
	const AFGPlayerController* Controller = GetOwnerPlayerController();
	const APlayerState* State = Controller ? Controller->GetPlayerState<APlayerState>() : nullptr;
	return State ? State->GetPlayerName() : GetNameSafe(Controller);
}

const FSTPlatformView* USTRemoteCallObject::GetPlatformView(const AFGBuildableTrainPlatformCargo* Platform) const
{
	return mPlatforms.Find(const_cast<AFGBuildableTrainPlatformCargo*>(Platform));
}

const FSTTrainView* USTRemoteCallObject::GetTrainView(const AFGTrain* Train) const
{
	return mTrains.Find(const_cast<AFGTrain*>(Train));
}

const FSTStationView* USTRemoteCallObject::GetStationView(const AFGTrainStationIdentifier* Station) const
{
	return mStations.Find(const_cast<AFGTrainStationIdentifier*>(Station));
}

//~ Edits

bool USTRemoteCallObject::Server_SetPlatform_Validate(AFGBuildableTrainPlatformCargo* Platform, ESTRole Role, const TArray<FSTItemRule>& Items, int32 Edit)
{
	return Items.Num() <= MaxItems && Role <= ESTRole::Demand;
}

void USTRemoteCallObject::Server_SetPlatform_Implementation(AFGBuildableTrainPlatformCargo* Platform, ESTRole Role, const TArray<FSTItemRule>& Items, int32 Edit)
{
	mAppliedEdit = FMath::Max(mAppliedEdit, Edit);
	ASTSubsystem* Subsystem = ASTSubsystem::Get(GetOwnerPlayerController());
	if (!Subsystem || !IsValid(Platform))
	{
		UE_LOG(LogSmartTrains, Warning, TEXT("Server_SetPlatform from %s: %s"), *GetPlayerName(), Subsystem ? TEXT("platform gone") : TEXT("no subsystem"));
		return;
	}
	const bool bFluid = STLayout::GetPlatformCargoType(Platform) == EFreightCargoType::FCT_Liquid;
	TArray<FSTItemRule> Clean;
	for (const FSTItemRule& Rule : Items)
	{
		if (!Rule.Item && Role == ESTRole::Supply)
		{
			continue;
		}
		if (Rule.Item && !STLayout::ItemFitsCargoType(Rule.Item, bFluid ? EFreightCargoType::FCT_Liquid : EFreightCargoType::FCT_Standard))
		{
			Client_Message(FString::Printf(TEXT("%s doesn't fit this kind of platform."), *STNames::Item(Rule.Item)), true);
			continue;
		}
		if (Clean.ContainsByPredicate([&](const FSTItemRule& Each) { return Each.Item == Rule.Item; }))
		{
			continue;
		}
		FSTItemRule& Kept = Clean.Add_GetRef(Rule);
		Kept.TargetPercent = FMath::Clamp(Kept.TargetPercent, 1.f, 100.f);
		Kept.SendAtPercent = FMath::Clamp(Kept.SendAtPercent, 0.f, 100.f);
		Kept.ManualRatePerMinute = FMath::Max(0.f, Kept.ManualRatePerMinute);
		Kept.Priority = static_cast<ESTPriority>(FMath::Min(static_cast<uint8>(Kept.Priority), static_cast<uint8>(ESTPriority::Critical)));
	}
	Subsystem->SetPlatformConfig(Platform, Role, Clean, FString::Printf(TEXT("platform menu (%s)"), *GetPlayerName()));
	Client_Platform(Subsystem->MakePlatformView(Platform, mAppliedEdit));
}

bool USTRemoteCallObject::Server_CopyPlatformSettings_Validate(AFGBuildableTrainPlatformCargo* Platform, bool bFromGameKey)
{
	return true;
}

void USTRemoteCallObject::Server_CopyPlatformSettings_Implementation(AFGBuildableTrainPlatformCargo* Platform, bool bFromGameKey)
{
	const ASTSubsystem* Subsystem = ASTSubsystem::Get(GetOwnerPlayerController());
	if (!Subsystem || !IsValid(Platform))
	{
		return;
	}
	const bool bFluid = STLayout::GetPlatformCargoType(Platform) == EFreightCargoType::FCT_Liquid;
	FSTPlatformSettings& Slot = bFluid ? mFluidClipboard : mFreightClipboard;
	Slot.bValid = true;
	Slot.Role = ESTRole::Off;
	Slot.Items.Reset();
	if (const FSTPlatformConfig* Config = Subsystem->FindPlatformConfig(Platform))
	{
		Slot.Role = Config->Role;
		Slot.Items = Config->Items;
	}
	const FString Summary = STNames::Settings(Slot.Role, Slot.Items);
	UE_LOG(LogSmartTrains, Log, TEXT("CLIPBOARD %s copied %s from %s (%s)"), *GetPlayerName(), *Summary, *Platform->GetName(), bFromGameKey ? TEXT("game key") : TEXT("panel"));
	Client_Clipboard(bFluid, true);
	Client_Message(FString::Printf(TEXT("Copied SmartTrains settings: %s."), *Summary), false);
}

bool USTRemoteCallObject::Server_PastePlatformSettings_Validate(AFGBuildableTrainPlatformCargo* Platform, bool bFromGameKey, int32 Edit)
{
	return true;
}

void USTRemoteCallObject::Server_PastePlatformSettings_Implementation(AFGBuildableTrainPlatformCargo* Platform, bool bFromGameKey, int32 Edit)
{
	mAppliedEdit = FMath::Max(mAppliedEdit, Edit);
	ASTSubsystem* Subsystem = ASTSubsystem::Get(GetOwnerPlayerController());
	if (!Subsystem || !IsValid(Platform))
	{
		return;
	}
	const bool bFluid = STLayout::GetPlatformCargoType(Platform) == EFreightCargoType::FCT_Liquid;
	const FSTPlatformSettings& Slot = bFluid ? mFluidClipboard : mFreightClipboard;
	if (!Slot.bValid)
	{
		UE_LOG(LogSmartTrains, Log, TEXT("CLIPBOARD %s pasted onto %s, but nothing was copied from a %s platform"), *GetPlayerName(), *Platform->GetName(), bFluid ? TEXT("fluid") : TEXT("freight"));
		if (!bFromGameKey)
		{
			Client_Message(FString::Printf(TEXT("Copy a %s platform's settings first."), bFluid ? TEXT("fluid") : TEXT("freight")), true);
		}
		return;
	}
	const FString Cause = FString::Printf(TEXT("paste (%s)"), *GetPlayerName());
	// Through the game's key the game pastes the load/unload setting itself, so removing a role keeps that setting.
	if (bFromGameKey && Slot.Role == ESTRole::Off)
	{
		if (FSTPlatformConfig* Config = Subsystem->FindPlatformConfig(Platform))
		{
			Config->bHasOriginalLoadMode = false;
		}
	}
	Subsystem->SetPlatformConfig(Platform, Slot.Role, Slot.Items, Cause);
	Client_Message(FString::Printf(TEXT("Pasted SmartTrains settings: %s."), *STNames::Settings(Slot.Role, Slot.Items)), false);
	Client_Platform(Subsystem->MakePlatformView(Platform, mAppliedEdit));
}

bool USTRemoteCallObject::Server_SetStationLink_Validate(AFGBuildableTrainPlatformCargo* Platform, ESTPlatformLink Link, int32 Edit)
{
	return Link <= ESTPlatformLink::Separate;
}

void USTRemoteCallObject::Server_SetStationLink_Implementation(AFGBuildableTrainPlatformCargo* Platform, ESTPlatformLink Link, int32 Edit)
{
	mAppliedEdit = FMath::Max(mAppliedEdit, Edit);
	ASTSubsystem* Subsystem = ASTSubsystem::Get(GetOwnerPlayerController());
	AFGTrainStationIdentifier* Station = IsValid(Platform) ? STLayout::GetIdentifier(STLayout::FindStation(Platform)) : nullptr;
	if (!Subsystem || !Station)
	{
		Client_Message(TEXT("This platform isn't part of a station."), true);
		return;
	}
	Subsystem->SetStationLink(Station, Link, Platform, FString::Printf(TEXT("platform menu (%s)"), *GetPlayerName()));
	Client_Platform(Subsystem->MakePlatformView(Platform, mAppliedEdit));
}

bool USTRemoteCallObject::Server_CopyPlatformToStation_Validate(AFGBuildableTrainPlatformCargo* Platform, int32 Edit)
{
	return true;
}

void USTRemoteCallObject::Server_CopyPlatformToStation_Implementation(AFGBuildableTrainPlatformCargo* Platform, int32 Edit)
{
	mAppliedEdit = FMath::Max(mAppliedEdit, Edit);
	ASTSubsystem* Subsystem = ASTSubsystem::Get(GetOwnerPlayerController());
	if (!Subsystem || !IsValid(Platform))
	{
		return;
	}
	FString Result;
	const int32 Copied = Subsystem->CopyPlatformToStation(Platform, FString::Printf(TEXT("platform menu copy (%s)"), *GetPlayerName()), Result);
	Client_Message(Result, Copied < 0);
	Client_Platform(Subsystem->MakePlatformView(Platform, mAppliedEdit));
}

bool USTRemoteCallObject::Server_SetStation_Validate(AFGTrainStationIdentifier* Station, bool bDepot, int32 Edit)
{
	return true;
}

void USTRemoteCallObject::Server_SetStation_Implementation(AFGTrainStationIdentifier* Station, bool bDepot, int32 Edit)
{
	mAppliedEdit = FMath::Max(mAppliedEdit, Edit);
	ASTSubsystem* Subsystem = ASTSubsystem::Get(GetOwnerPlayerController());
	if (!Subsystem || !IsValid(Station))
	{
		return;
	}
	const FString Cause = FString::Printf(TEXT("station menu (%s)"), *GetPlayerName());
	const FSTStationConfig* Config = Subsystem->FindStationConfig(Station);
	if (!Config || Config->bDepot != bDepot)
	{
		Subsystem->SetStationDepot(Station, bDepot, Cause);
	}
	Client_Station(Subsystem->MakeStationView(Station, mAppliedEdit));
}

bool USTRemoteCallObject::Server_SetTrain_Validate(AFGTrain* Train, ESTTrainAction Action, int32 Edit)
{
	return Action <= ESTTrainAction::KeepVanillaOff;
}

void USTRemoteCallObject::Server_SetTrain_Implementation(AFGTrain* Train, ESTTrainAction Action, int32 Edit)
{
	mAppliedEdit = FMath::Max(mAppliedEdit, Edit);
	ASTSubsystem* Subsystem = ASTSubsystem::Get(GetOwnerPlayerController());
	if (!Subsystem || !IsValid(Train))
	{
		return;
	}
	const FString Cause = FString::Printf(TEXT("locomotive menu (%s)"), *GetPlayerName());
	FString Why;
	switch (Action)
	{
	case ESTTrainAction::Manage:
	case ESTTrainAction::Unmanage:
		if (!Subsystem->SetTrainManaged(Train, Action == ESTTrainAction::Manage, Cause, Why))
		{
			Client_Message(FString::Printf(TEXT("Can't manage %s: %s"), *STLayout::Name(Train), *Why), true);
		}
		break;
	case ESTTrainAction::KeepVanillaOn:
	case ESTTrainAction::KeepVanillaOff:
		Subsystem->SetTrainKeepVanilla(Train, Action == ESTTrainAction::KeepVanillaOn, Cause);
		break;
	}
	Client_Train(Subsystem->MakeTrainView(Train, mAppliedEdit));
}

bool USTRemoteCallObject::Server_Migrate_Validate(ESTMigrationStep Step, const TArray<FString>& Selection)
{
	return Selection.Num() <= MaxSelectionWords && Step <= ESTMigrationStep::Backup;
}

void USTRemoteCallObject::Server_Migrate_Implementation(ESTMigrationStep Step, const TArray<FString>& Selection)
{
	ASTSubsystem* Subsystem = ASTSubsystem::Get(GetOwnerPlayerController());
	UWorld* World = GetWorld();
	if (!Subsystem || !World)
	{
		return;
	}
	const FString Cause = FString::Printf(TEXT("migration window (%s)"), *GetPlayerName());
	UE_LOG(LogSmartTrains, Log, TEXT("Migration window: step %d [%s] by %s"), static_cast<int32>(Step), *FString::Join(Selection, TEXT(" ")), *GetPlayerName());
	TArray<FString> Lines;
	switch (Step)
	{
	case ESTMigrationStep::Refresh:
		break;
	case ESTMigrationStep::Preview:
		Lines = STMigration::Preview(Subsystem, World, STMigration::ParseTraceLimit(Selection));
		// The rows show the stations and trains; keep only the summary and notes.
		Lines.RemoveAll([](const FString& Line)
		{
			const bool bRow = Line.Len() > 1 && (Line[0] == TEXT('S') || Line[0] == TEXT('T')) && FChar::IsDigit(Line[1]);
			return bRow || Line.StartsWith(TEXT("Apply with /st"));
		});
		break;
	case ESTMigrationStep::Apply:
		Lines = STMigration::Apply(Subsystem, World, Selection, Cause);
		break;
	case ESTMigrationStep::Undo:
		Lines = STMigration::Undo(Subsystem, World, Cause);
		break;
	case ESTMigrationStep::TestCopy:
	case ESTMigrationStep::Backup:
	{
		FString Message;
		if (!Subsystem->GetSettings().bBetaTools)
		{
			Message = TEXT("Turn on 'Beta testing tools' in the Mods menu first.");
		}
		else
		{
			const bool bOk = Step == ESTMigrationStep::Backup ? STSaveTools::Backup(World, Message) : STSaveTools::TestCopy(World, FString(), Message);
			Subsystem->bMadeBackupThisSession |= bOk;
		}
		Lines.Add(Message);
		break;
	}
	}
	FSTMigrationView View = STMigration::MakeView(Subsystem, World);
	View.Lines = MoveTemp(Lines);
	View.Serial = ++mMigrationSerial;
	Client_Migration(View);
}

//~ Readouts

void USTRemoteCallObject::Server_RequestPlatform_Implementation(AFGBuildableTrainPlatformCargo* Platform)
{
	ASTSubsystem* Subsystem = ASTSubsystem::Get(GetOwnerPlayerController());
	if (Subsystem && IsValid(Platform))
	{
		Client_Platform(Subsystem->MakePlatformView(Platform, mAppliedEdit));
	}
}

void USTRemoteCallObject::Server_RequestTrain_Implementation(AFGTrain* Train)
{
	ASTSubsystem* Subsystem = ASTSubsystem::Get(GetOwnerPlayerController());
	if (Subsystem && IsValid(Train))
	{
		Client_Train(Subsystem->MakeTrainView(Train, mAppliedEdit));
	}
}

void USTRemoteCallObject::Server_RequestStation_Implementation(AFGTrainStationIdentifier* Station)
{
	ASTSubsystem* Subsystem = ASTSubsystem::Get(GetOwnerPlayerController());
	if (Subsystem && IsValid(Station))
	{
		Client_Station(Subsystem->MakeStationView(Station, mAppliedEdit));
	}
}

void USTRemoteCallObject::Client_Platform_Implementation(const FSTPlatformView& View)
{
	if (View.AppliedEdit >= mSentEdit && View.Platform)
	{
		mPlatforms.Add(View.Platform.Get(), View);
		++mRevision;
	}
}

void USTRemoteCallObject::Client_Train_Implementation(const FSTTrainView& View)
{
	if (View.AppliedEdit >= mSentEdit && View.Train)
	{
		mTrains.Add(View.Train.Get(), View);
		++mRevision;
	}
}

void USTRemoteCallObject::Client_Station_Implementation(const FSTStationView& View)
{
	if (View.AppliedEdit >= mSentEdit && View.Station)
	{
		mStations.Add(View.Station.Get(), View);
		++mRevision;
	}
}

void USTRemoteCallObject::Client_Migration_Implementation(const FSTMigrationView& View)
{
	mMigration = View;
	++mRevision;
}

void USTRemoteCallObject::Client_Clipboard_Implementation(bool bFluid, bool bValid)
{
	(bFluid ? bHasFluidClipboard : bHasFreightClipboard) = bValid;
	++mRevision;
}

void USTRemoteCallObject::Client_OpenMigrationWindow_Implementation()
{
	UE_LOG(LogSmartTrains, Log, TEXT("Opening the migration window"));
	USTMigrationWindow::Open(GetOwnerPlayerController());
}

void USTRemoteCallObject::Client_Message_Implementation(const FString& Message, bool bError)
{
	mLastMessage = Message;
	bLastMessageError = bError;
	const UWorld* World = GetWorld();
	mLastMessageTime = World ? World->GetTimeSeconds() : 0.0;
	++mRevision;
	UE_LOG(LogSmartTrains, Log, TEXT("Server message%s: %s"), bError ? TEXT(" (error)") : TEXT(""), *Message);
}
