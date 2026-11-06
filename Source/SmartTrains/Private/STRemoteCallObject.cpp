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
		int32 Total = 16 + Bytes(Row.Text) + Bytes(Row.Title) + Bytes(Row.Details) + Bytes(Row.Flag) + Bytes(Row.Users) + Bytes(Row.Note);
		for (const FSTMigrationPlatformRow& Platform : Row.Platforms)
		{
			Total += 16 + 8 * Platform.Items.Num() + Bytes(Platform.Confidence) + Bytes(Platform.Note) + Bytes(Platform.Conflict);
		}
		return Total;
	}

	int32 Bytes(const FSTMonitorRow& Row)
	{
		return 16 + Bytes(Row.Title) + Bytes(Row.Verdict) + Bytes(Row.Columns) + Bytes(Row.Advice) + Bytes(Row.Details);
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

	// Trims View to about MonitorBytes: first the details of the longest lists' last rows, then those rows. Returns the
	// number of rows dropped.
	int32 FitMonitor(FSTMonitorView& View)
	{
		TArray<TArray<FSTMonitorRow>*> Lists = {&View.Trains, &View.Platforms, &View.Stations, &View.Items};
		auto Total = [&]()
		{
			int32 Sum = Bytes(View.Summary) + Bytes(View.Idle) + Bytes(View.Tiles);
			for (const TArray<FSTMonitorRow>* List : Lists)
			{
				Sum += Bytes(*List);
			}
			return Sum;
		};
		int32 Size = Total();
		for (TArray<FSTMonitorRow>* List : Lists)
		{
			for (int32 Index = List->Num() - 1; Index >= 0 && Size > MonitorBytes; --Index)
			{
				Size -= Bytes((*List)[Index].Details) - 5;
				(*List)[Index].Details.Reset();
			}
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
	if (!Allowed(*this, *Subsystem, STAccess::EChange::Edit, TEXT("change platform settings")))
	{
		Client_Platform(Subsystem->MakePlatformView(Platform, mAppliedEdit));
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
	if (Slot.bValid && !Allowed(*this, *Subsystem, STAccess::EChange::Edit, TEXT("paste platform settings")))
	{
		Client_Platform(Subsystem->MakePlatformView(Platform, mAppliedEdit));
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
	if (!Allowed(*this, *Subsystem, STAccess::EChange::Edit, TEXT("change station settings")))
	{
		Client_Station(Subsystem->MakeStationView(Station, mAppliedEdit));
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
		Lines = STMigration::Preview(Subsystem, World, STMigration::ParseTraceLimit(Selection), STMigration::ParseOneItem(Selection));
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

bool USTRemoteCallObject::Server_MigratePlatform_Validate(int32 Station, int32 Platform, ESTRole Role, const TArray<TSubclassOf<UFGItemDescriptor>>& Items)
{
	return Items.Num() <= MaxItems && Role <= ESTRole::Demand;
}

void USTRemoteCallObject::Server_MigratePlatform_Implementation(int32 Station, int32 Platform, ESTRole Role, const TArray<TSubclassOf<UFGItemDescriptor>>& Items)
{
	ASTSubsystem* Subsystem = ASTSubsystem::Get(GetOwnerPlayerController());
	UWorld* World = GetWorld();
	if (!Subsystem || !World)
	{
		return;
	}
	FString Message;
	if (STAccess::MayChange(*Subsystem, GetOwnerPlayerController(), STAccess::EChange::Edit, TEXT("change the migration preview"), Message))
	{
		STMigration::SetPlatform(World, Station, Platform, Role, Items, GetPlayerName(), Message);
	}
	FSTMigrationView View = STMigration::MakeView(Subsystem, World);
	View.Lines.Add(Message);
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

void USTRemoteCallObject::Client_Monitor_Implementation(const FSTMonitorView& View)
{
	mMonitor = View;
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
