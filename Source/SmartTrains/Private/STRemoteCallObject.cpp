#include "STRemoteCallObject.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "FGPlayerController.h"
#include "FGTrain.h"
#include "FGTrainStationIdentifier.h"
#include "GameFramework/PlayerState.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "SmartTrains.h"
#include "STAccess.h"
#include "STConfig.h"
#include "STLayout.h"
#include "STMigration.h"
#include "STSubsystem.h"
#include "UI/STMigrationWindow.h"
#include "UI/STMonitorWindow.h"

namespace
{
	// Returns true if Owner may make the change; otherwise tells the player why.
	bool Allowed(USTRemoteCallObject& Owner, const ASTSubsystem& Subsystem, STAccess::EChange Change, const TCHAR* Action)
	{
		FString Why;
		if (STAccess::MayChange(Subsystem, Owner.GetOwnerPlayerController(), Change, Action, Why))
		{
			return true;
		}
		Owner.Client_Message(Why, true);
		return false;
	}

	// Most item rules one platform may have.
	constexpr int32 MaxItems = 32;

	// Most words a migration selection may have.
	constexpr int32 MaxSelectionWords = 256;

	// Most estimated bytes of rows in one network message. The engine refuses messages over 64 KiB.
	constexpr int32 RowBytesPerMessage = 24 * 1024;

	// Most estimated bytes of a monitor view, sent unreliably in one message.
	constexpr int32 MonitorBytes = 48 * 1024;

	// Estimated bytes of S on the wire: 2 per character (non-ANSI text goes as UTF-16) plus its length.
	int32 Bytes(const FString& S)
	{
		return 2 * S.Len() + 5;
	}

	int32 Bytes(const TArray<FString>& Lines)
	{
		int32 Total = 5;
		for (const FString& Line : Lines)
		{
			Total += Bytes(Line);
		}
		return Total;
	}

	int32 Bytes(const FSTMigrationRow& Row)
	{
		int32 Total = 16 + Bytes(Row.Text) + Bytes(Row.Title) + Bytes(Row.Details) + Bytes(Row.Flag) + Bytes(Row.Users) + Bytes(Row.Note) + Bytes(Row.NewName);
		for (const FSTMigrationPlatformRow& Platform : Row.Platforms)
		{
			Total += 16 + 8 * Platform.Items.Num() + Bytes(Platform.Confidence) + Bytes(Platform.Note) + Bytes(Platform.Conflict);
		}
		return Total;
	}

	int32 Bytes(const FSTMonitorRow& Row)
	{
		return 24 + Bytes(Row.Title) + Bytes(Row.Verdict) + Bytes(Row.Columns) + Bytes(Row.Advice) + 8 * Row.Pattern.Num();
	}

	// Takes the details out of Rows, keeping whether each row had any.
	void StripDetails(TArray<FSTMonitorRow>& Rows)
	{
		for (FSTMonitorRow& Row : Rows)
		{
			Row.bHasDetails = Row.Details.Num() > 0;
			Row.Details.Reset();
		}
	}

	int32 Bytes(const TArray<FSTMonitorRow>& Rows)
	{
		int32 Total = 5;
		for (const FSTMonitorRow& Row : Rows)
		{
			Total += Bytes(Row);
		}
		return Total;
	}

	// Takes the details out of View (they go one row at a time, on request), then trims it to about MonitorBytes by
	// dropping the last (least urgent) rows of the longest list. Returns the number of rows dropped.
	int32 FitMonitor(FSTMonitorView& View)
	{
		TArray<TArray<FSTMonitorRow>*> Lists = {&View.Trains, &View.Suppliers, &View.Consumers};
		int32 Size = Bytes(View.Summary) + Bytes(View.Tiles);
		for (TArray<FSTMonitorRow>* List : Lists)
		{
			StripDetails(*List);
			Size += Bytes(*List);
		}
		int32 Dropped = 0;
		while (Size > MonitorBytes)
		{
			TArray<FSTMonitorRow>** Longest = Lists.FindByPredicate([](const TArray<FSTMonitorRow>* List) { return List->Num() > 0; });
			for (TArray<FSTMonitorRow>*& List : Lists)
			{
				if (Longest && List->Num() > (*Longest)->Num())
				{
					Longest = &List;
				}
			}
			if (!Longest)
			{
				break;
			}
			Size -= Bytes((*Longest)->Last());
			(*Longest)->Pop();
			++Dropped;
		}
		return Dropped;
	}
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

bool USTRemoteCallObject::Server_SetStationPriority_Validate(AFGTrainStationIdentifier* Station, ESTPriority Priority, int32 Edit)
{
	return Priority <= ESTPriority::High;
}

void USTRemoteCallObject::Server_SetStationPriority_Implementation(AFGTrainStationIdentifier* Station, ESTPriority Priority, int32 Edit)
{
	mAppliedEdit = FMath::Max(mAppliedEdit, Edit);
	ASTSubsystem* Subsystem = ASTSubsystem::Get(GetOwnerPlayerController());
	if (!Subsystem || !IsValid(Station))
	{
		return;
	}
	if (Allowed(*this, *Subsystem, STAccess::EChange::Edit, TEXT("change a station's priority")))
	{
		Subsystem->SetStationPriority(Station, Priority, FString::Printf(TEXT("station menu (%s)"), *GetPlayerName()));
	}
	Client_Station(Subsystem->MakeStationView(Station, mAppliedEdit));
}

bool USTRemoteCallObject::Server_SetPlatform_Validate(AFGBuildableTrainPlatformCargo* Platform, ESTRole Role, const TArray<FSTItemRule>& Items, int32 Edit)
{
	return Items.Num() <= MaxItems && Role <= ESTRole::Sink;
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
	if (!Allowed(*this, *Subsystem, STAccess::EChange::Edit, TEXT("change platform settings")))
	{
		Client_Platform(Subsystem->MakePlatformView(Platform, mAppliedEdit));
		return;
	}
	if (Role != ESTRole::Off && !STLayout::CanMoveCargo(Platform))
	{
		Client_Message(TEXT("This platform can't move freight or fluid, so it can't have a role."), true);
		Client_Platform(Subsystem->MakePlatformView(Platform, mAppliedEdit));
		return;
	}
	const bool bFluid = STLayout::GetPlatformCargoType(Platform) == EFreightCargoType::FCT_Liquid;
	TArray<FSTItemRule> Clean;
	for (const FSTItemRule& Rule : Items)
	{
		if (!Rule.Item && Role != ESTRole::Sink)
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
		Clean.Add(Rule);
	}
	Subsystem->SetPlatformConfig(Platform, Role, Clean, FString::Printf(TEXT("platform menu (%s)"), *GetPlayerName()));
	Client_Platform(Subsystem->MakePlatformView(Platform, mAppliedEdit));
}

void USTRemoteCallObject::CopyPlatformFromMenu(AFGBuildableTrainPlatformCargo* Platform)
{
	if (Platform)
	{
		UE_LOG(LogSmartTrains, Log, TEXT("Platform menu: copy settings of %s"), *Platform->GetName());
		Server_CopyPlatformSettings(Platform, false);
	}
}

int32 USTRemoteCallObject::PastePlatformFromMenu(AFGBuildableTrainPlatformCargo* Platform)
{
	if (!Platform)
	{
		return 0;
	}
	const int32 Edit = NextEdit();
	UE_LOG(LogSmartTrains, Log, TEXT("Platform menu: paste settings onto %s (edit %d)"), *Platform->GetName(), Edit);
	Server_PastePlatformSettings(Platform, false, Edit);
	return Edit;
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
	if (Slot.bValid && !Allowed(*this, *Subsystem, STAccess::EChange::Edit, TEXT("paste platform settings")))
	{
		Client_Platform(Subsystem->MakePlatformView(Platform, mAppliedEdit));
		return;
	}
	if (Slot.bValid && Slot.Role != ESTRole::Off && !STLayout::CanMoveCargo(Platform))
	{
		if (!bFromGameKey)
		{
			Client_Message(TEXT("This platform can't move freight or fluid, so it can't have a role."), true);
		}
		return;
	}
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
	if (!Allowed(*this, *Subsystem, STAccess::EChange::Edit, TEXT("change platform settings")))
	{
		Client_Platform(Subsystem->MakePlatformView(Platform, mAppliedEdit));
		return;
	}
	FString Result;
	const int32 Copied = Subsystem->CopyPlatformToStation(Platform, FString::Printf(TEXT("platform menu copy (%s)"), *GetPlayerName()), Result);
	Client_Message(Result, Copied < 0);
	Client_Platform(Subsystem->MakePlatformView(Platform, mAppliedEdit));
}

bool USTRemoteCallObject::Server_SetUpStation_Validate(AFGTrainStationIdentifier* Station, uint8 Mode, int32 Edit)
{
	return Mode <= 1;
}

void USTRemoteCallObject::Server_SetUpStation_Implementation(AFGTrainStationIdentifier* Station, uint8 Mode, int32 Edit)
{
	mAppliedEdit = FMath::Max(mAppliedEdit, Edit);
	ASTSubsystem* Subsystem = ASTSubsystem::Get(GetOwnerPlayerController());
	if (!Subsystem || !IsValid(Station))
	{
		return;
	}
	const bool bApply = Mode == 1;
	if (bApply && !Allowed(*this, *Subsystem, STAccess::EChange::Edit, TEXT("set up stations")))
	{
		Client_Station(Subsystem->MakeStationView(Station, mAppliedEdit));
		return;
	}
	Subsystem->SetUpStation(Station, bApply, true, false, FString::Printf(TEXT("station menu (%s)"), *GetPlayerName()));
	Client_Station(Subsystem->MakeStationView(Station, mAppliedEdit));
}

bool USTRemoteCallObject::Server_AutodetectPlatform_Validate(AFGBuildableTrainPlatformCargo* Platform, int32 Edit)
{
	return true;
}

void USTRemoteCallObject::Server_AutodetectPlatform_Implementation(AFGBuildableTrainPlatformCargo* Platform, int32 Edit)
{
	mAppliedEdit = FMath::Max(mAppliedEdit, Edit);
	ASTSubsystem* Subsystem = ASTSubsystem::Get(GetOwnerPlayerController());
	if (!Subsystem || !IsValid(Platform))
	{
		return;
	}
	if (Allowed(*this, *Subsystem, STAccess::EChange::Edit, TEXT("change platform settings")))
	{
		Client_Message(Subsystem->AutodetectPlatform(Platform, FString::Printf(TEXT("platform menu (%s)"), *GetPlayerName())), false);
	}
	Client_Platform(Subsystem->MakePlatformView(Platform, mAppliedEdit));
}

bool USTRemoteCallObject::Server_AutodetectStation_Validate(AFGTrainStationIdentifier* Station, int32 Edit)
{
	return true;
}

void USTRemoteCallObject::Server_AutodetectStation_Implementation(AFGTrainStationIdentifier* Station, int32 Edit)
{
	mAppliedEdit = FMath::Max(mAppliedEdit, Edit);
	ASTSubsystem* Subsystem = ASTSubsystem::Get(GetOwnerPlayerController());
	if (!Subsystem || !IsValid(Station))
	{
		return;
	}
	if (Allowed(*this, *Subsystem, STAccess::EChange::Edit, TEXT("change platform settings")))
	{
		Client_Message(Subsystem->AutodetectStation(Station, FString::Printf(TEXT("station menu (%s)"), *GetPlayerName())), false);
	}
	Client_Station(Subsystem->MakeStationView(Station, mAppliedEdit));
}

void USTRemoteCallObject::PushMigrationView(const TArray<FString>& Lines)
{
	ASTSubsystem* Subsystem = ASTSubsystem::Get(GetOwnerPlayerController());
	if (!Subsystem || !GetWorld())
	{
		return;
	}
	FSTMigrationView View = STMigration::MakeView(Subsystem, GetWorld());
	View.Lines = Lines;
	View.Serial = ++mMigrationSerial;
	SendMigration(MoveTemp(View));
}

bool USTRemoteCallObject::Server_SetTrain_Validate(AFGTrain* Train, ESTTrainAction Action, int32 Edit)
{
	return Action <= ESTTrainAction::Resume;
}

void USTRemoteCallObject::Server_SetTrain_Implementation(AFGTrain* Train, ESTTrainAction Action, int32 Edit)
{
	mAppliedEdit = FMath::Max(mAppliedEdit, Edit);
	ASTSubsystem* Subsystem = ASTSubsystem::Get(GetOwnerPlayerController());
	if (!Subsystem || !IsValid(Train))
	{
		return;
	}
	if (!Allowed(*this, *Subsystem, STAccess::EChange::Edit, TEXT("change train settings")))
	{
		Client_Train(Subsystem->MakeTrainView(Train, mAppliedEdit));
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
	case ESTTrainAction::Hold:
	case ESTTrainAction::Resume:
		Subsystem->SetTrainHeld(Train, Action == ESTTrainAction::Hold, Cause);
		break;
	}
	Client_Train(Subsystem->MakeTrainView(Train, mAppliedEdit));
}

bool USTRemoteCallObject::Server_SetSettings_Validate(const TArray<FString>& Keys, const TArray<FString>& Values)
{
	// Far more than the configuration has, and longer keys and values than any of it.
	auto TooLong = [](const FString& Each) { return Each.Len() > 64; };
	return Keys.Num() == Values.Num() && Keys.Num() <= 64 && !Keys.ContainsByPredicate(TooLong) && !Values.ContainsByPredicate(TooLong);
}

void USTRemoteCallObject::Server_SetSettings_Implementation(const TArray<FString>& Keys, const TArray<FString>& Values)
{
	ASTSubsystem* Subsystem = ASTSubsystem::Get(GetWorld());
	if (!Subsystem)
	{
		return;
	}
	FString Why;
	if (!STAccess::MayChange(*Subsystem, GetOwnerPlayerController(), STAccess::EChange::Network, TEXT("change SmartTrains' settings from the Mods menu"), Why))
	{
		Client_Message(TEXT("This server keeps its own SmartTrains settings, and only a server admin can change them. Your Mods menu still applies in games you host."), true);
		return;
	}
	TArray<FString> Applied;
	TArray<FString> Refused;
	for (int32 Index = 0; Index < Keys.Num(); ++Index)
	{
		FString Message;
		const bool bOk = USTConfig::Write(this, Keys[Index], Values[Index], Message);
		(bOk ? Applied : Refused).Add(bOk ? FString::Printf(TEXT("%s = %s"), *Keys[Index], *Values[Index]) : Message);
		UE_LOG(LogSmartTrains, Log, TEXT("Settings: Mods menu (%s): %s %s: %s"), *STAccess::Describe(GetOwnerPlayerController()), *Keys[Index], *Values[Index], *Message);
	}
	if (Applied.Num() > 0)
	{
		Client_Message(FString::Printf(TEXT("Server settings changed: %s."), *FString::Join(Applied, TEXT(", "))), false);
	}
	if (Refused.Num() > 0)
	{
		Client_Message(FString::Printf(TEXT("The server didn't take: %s"), *FString::Join(Refused, TEXT(" "))), true);
	}
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
		Lines = STMigration::Preview(Subsystem, World);
		STMigration::SetRequester(World, this);
		// The rows show the stations and trains; keep only the summary and notes.
		Lines.RemoveAll([](const FString& Line)
		{
			const bool bRow = Line.Len() > 1 && (Line[0] == TEXT('S') || Line[0] == TEXT('T')) && FChar::IsDigit(Line[1]);
			return bRow || Line.StartsWith(TEXT("Apply with /st"));
		});
		break;
	case ESTMigrationStep::Apply:
	case ESTMigrationStep::Undo:
	{
		FString Why;
		if (!STAccess::MayChange(*Subsystem, GetOwnerPlayerController(), STAccess::EChange::Network, Step == ESTMigrationStep::Apply ? TEXT("apply a migration") : TEXT("undo a migration"), Why))
		{
			Lines.Add(Why);
		}
		else
		{
			Lines = Step == ESTMigrationStep::Apply ? STMigration::Apply(Subsystem, World, Selection, Cause) : STMigration::Undo(Subsystem, World, Cause);
		}
		break;
	}
	case ESTMigrationStep::TestCopy:
	case ESTMigrationStep::Backup:
	{
		FString Message;
		const bool bAllowed = STAccess::MayChange(*Subsystem, GetOwnerPlayerController(), STAccess::EChange::Network,
			Step == ESTMigrationStep::Backup ? TEXT("make a backup save") : TEXT("make a test copy"), Message);
		if (bAllowed && !Subsystem->GetSettings().bBetaTools)
		{
			Message = TEXT("Turn on 'Beta testing tools' first (Mods menu, or /st set BetaTools on).");
		}
		else if (bAllowed)
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
	SendMigration(MoveTemp(View));
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

void USTRemoteCallObject::Server_RequestMonitor_Implementation()
{
	if (const ASTSubsystem* Subsystem = ASTSubsystem::Get(GetOwnerPlayerController()))
	{
		FSTMonitorView View = Subsystem->MakeMonitorView();
		if (const int32 Dropped = FitMonitor(View))
		{
			static double LastWarned = -1000.0;
			const double Time = FPlatformTime::Seconds();
			if (Time - LastWarned > 300.0)
			{
				LastWarned = Time;
				UE_LOG(LogSmartTrains, Warning, TEXT("Monitor view for %s too large for one network message: last %d rows left out"), *GetPlayerName(), Dropped);
			}
		}
		Client_Monitor(View);
	}
}

void USTRemoteCallObject::Server_RequestMonitorDetails_Implementation(const FString& Key)
{
	const ASTSubsystem* Subsystem = ASTSubsystem::Get(GetOwnerPlayerController());
	if (!Subsystem || Key.Len() < 3 || Key[1] != TEXT(':'))
	{
		return;
	}
	const FSTMonitorView View = Subsystem->MakeMonitorView();
	const TArray<FSTMonitorRow>* Rows = Key[0] == TEXT('c') ? &View.Consumers : Key[0] == TEXT('s') ? &View.Suppliers : Key[0] == TEXT('t') ? &View.Trains : nullptr;
	const FString Title = Key.Mid(2);
	const FSTMonitorRow* Row = Rows ? Rows->FindByPredicate([&Title](const FSTMonitorRow& Each) { return Each.Title == Title; }) : nullptr;
	TArray<FString> Details = Row ? Row->Details : TArray<FString>({TEXT("Not in the plan any more.")});
	// One row's details stay well inside one network message; a very long one is cut.
	int32 Size = 0;
	for (int32 Index = 0; Index < Details.Num(); ++Index)
	{
		Size += Bytes(Details[Index]);
		if (Size > MonitorBytes / 2)
		{
			Details.SetNum(Index);
			Details.Add(TEXT("(more left out)"));
			break;
		}
	}
	Client_MonitorDetails(Key, Details);
}

void USTRemoteCallObject::Client_Monitor_Implementation(const FSTMonitorView& View)
{
	mMonitor = View;
	++mRevision;
}

void USTRemoteCallObject::Client_MonitorDetails_Implementation(const FString& Key, const TArray<FString>& Details)
{
	if (const TArray<FString>* Known = mMonitorDetails.Find(Key); Known && *Known == Details)
	{
		return;
	}
	mMonitorDetails.Add(Key, Details);
	++mRevision;
}

void USTRemoteCallObject::Client_OpenMonitorWindow_Implementation()
{
	UE_LOG(LogSmartTrains, Log, TEXT("Opening the monitor window"));
	USTMonitorWindow::Open(GetOwnerPlayerController());
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

void USTRemoteCallObject::SendMigration(FSTMigrationView View)
{
	TArray<FSTMigrationRow> Rows = MoveTemp(View.Rows);
	View.Rows.Reset();
	const int32 Serial = View.Serial;
	Client_MigrationStart(View, Rows.Num());
	TArray<FSTMigrationRow> Batch;
	int32 BatchBytes = 0;
	int32 Messages = 1;
	for (FSTMigrationRow& Row : Rows)
	{
		const int32 RowBytes = Bytes(Row);
		if (Batch.Num() > 0 && BatchBytes + RowBytes > RowBytesPerMessage)
		{
			Client_MigrationRows(Serial, Batch);
			++Messages;
			Batch.Reset();
			BatchBytes = 0;
		}
		BatchBytes += RowBytes;
		Batch.Add(MoveTemp(Row));
	}
	if (Batch.Num() > 0)
	{
		Client_MigrationRows(Serial, Batch);
		++Messages;
	}
	UE_LOG(LogSmartTrains, Verbose, TEXT("Migration view %d for %s: %d rows in %d messages"), Serial, *GetPlayerName(), Rows.Num(), Messages);
}

void USTRemoteCallObject::Client_MigrationStart_Implementation(const FSTMigrationView& Header, int32 RowCount)
{
	mPendingMigration = Header;
	mPendingMigrationRows = RowCount;
	if (RowCount <= 0)
	{
		mMigration = MoveTemp(mPendingMigration);
		mPendingMigrationRows = -1;
		++mRevision;
	}
}

void USTRemoteCallObject::Client_MigrationRows_Implementation(int32 Serial, const TArray<FSTMigrationRow>& Rows)
{
	if (Serial != mPendingMigration.Serial || mPendingMigrationRows < 0)
	{
		UE_LOG(LogSmartTrains, Warning, TEXT("Migration rows for answer %d dropped: waiting for answer %d"), Serial, mPendingMigrationRows < 0 ? -1 : mPendingMigration.Serial);
		return;
	}
	mPendingMigration.Rows.Append(Rows);
	if (mPendingMigration.Rows.Num() >= mPendingMigrationRows)
	{
		mMigration = MoveTemp(mPendingMigration);
		mPendingMigration = FSTMigrationView();
		mPendingMigrationRows = -1;
		++mRevision;
	}
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
