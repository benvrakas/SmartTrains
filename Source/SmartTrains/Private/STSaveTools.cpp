#include "Engine/World.h"
#include "FGGameMode.h"
#include "FGSaveSession.h"
#include "FGSaveSystem.h"
#include "Misc/DateTime.h"
#include "SmartTrains.h"
#include "STMigration.h"
#include "TimerManager.h"

namespace
{
	FString Timestamp()
	{
		return FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
	}

	// Puts the running world's session name back once, from the save callback or the fallback timer.
	struct FRestore
	{
		TWeakObjectPtr<AFGGameMode> GameMode;
		FString Session;
		bool bDone = false;

		void Run(const TCHAR* Why)
		{
			if (bDone)
			{
				return;
			}
			bDone = true;
			if (AFGGameMode* Mode = GameMode.Get())
			{
				Mode->SetSaveSessionName(Session);
			}
			UE_LOG(LogSmartTrains, Log, TEXT("Test copy: session name '%s' put back (%s)"), *Session, Why);
		}
	};
}

bool STSaveTools::TestCopy(UWorld* World, const FString& SessionName, FString& OutMessage)
{
	AFGGameMode* GameMode = World ? World->GetAuthGameMode<AFGGameMode>() : nullptr;
	UFGSaveSession* SaveSession = World ? UFGSaveSession::Get(World) : nullptr;
	if (!GameMode || !SaveSession)
	{
		OutMessage = TEXT("only the host can make a test copy");
		return false;
	}
	const FString Original = GameMode->GetSaveSessionName();
	const FString Copy = SessionName.IsEmpty() ? Original + TEXT(" (SmartTrains test)") : SessionName;
	if (Copy == Original)
	{
		OutMessage = TEXT("the copy needs a session name different from this one");
		return false;
	}
	const FString File = UFGSaveSystem::SanitizeSaveName(Copy + TEXT("_") + Timestamp());
	if (!UFGSaveSystem::IsValidSaveName(File))
	{
		OutMessage = FString::Printf(TEXT("'%s' isn't a valid save name"), *File);
		return false;
	}
	TSharedRef<FRestore> Restore = MakeShared<FRestore>();
	Restore->GameMode = GameMode;
	Restore->Session = Original;
	GameMode->SetSaveSessionName(Copy);
	UE_LOG(LogSmartTrains, Log, TEXT("Test copy: saving '%s' as session '%s' (this world stays '%s')"), *File, *Copy, *Original);
	SaveSession->SaveGame(File, FOnSaveGameComplete::CreateLambda([Restore, File](bool bSuccess, const FText& Error, void*)
	{
		UE_LOG(LogSmartTrains, Log, TEXT("Test copy '%s' %s%s"), *File, bSuccess ? TEXT("saved") : TEXT("FAILED: "), bSuccess ? TEXT("") : *Error.ToString());
		Restore->Run(TEXT("save finished"));
	}), nullptr);
	FTimerHandle Handle;
	World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([Restore]() { Restore->Run(TEXT("fallback after 30 s")); }), 30.f, false);
	OutMessage = FString::Printf(TEXT("saving a test copy as session '%s' (file %s). Load it from the main menu to test; this world keeps saving as '%s'."),
		*Copy, *File, *Original);
	return true;
}

bool STSaveTools::Backup(UWorld* World, FString& OutMessage)
{
	AFGGameMode* GameMode = World ? World->GetAuthGameMode<AFGGameMode>() : nullptr;
	UFGSaveSession* SaveSession = World ? UFGSaveSession::Get(World) : nullptr;
	if (!GameMode || !SaveSession)
	{
		OutMessage = TEXT("only the host can make a backup");
		return false;
	}
	const FString File = UFGSaveSystem::SanitizeSaveName(GameMode->GetSaveSessionName() + TEXT("_SmartTrains_backup_") + Timestamp());
	SaveSession->SaveGame(File, FOnSaveGameComplete::CreateLambda([File](bool bSuccess, const FText& Error, void*)
	{
		UE_LOG(LogSmartTrains, Log, TEXT("Backup '%s' %s%s"), *File, bSuccess ? TEXT("saved") : TEXT("FAILED: "), bSuccess ? TEXT("") : *Error.ToString());
	}), nullptr);
	OutMessage = FString::Printf(TEXT("saving backup %s in this session"), *File);
	return true;
}
