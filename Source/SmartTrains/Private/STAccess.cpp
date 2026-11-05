#include "STAccess.h"
#include "Engine/World.h"
#include "FGPlayerController.h"
#include "FGPlayerState.h"
#include "GameFramework/PlayerState.h"
#include "Server/FGDSAuthenticationTypes.h"
#include "SmartTrains.h"
#include "STSubsystem.h"

namespace
{
	FString PlayerName(const AFGPlayerController* Controller)
	{
		const APlayerState* State = Controller ? Controller->GetPlayerState<APlayerState>() : nullptr;
		return State ? State->GetPlayerName() : FString(TEXT("server"));
	}

	const TCHAR* LevelName(EPrivilegeLevel Level)
	{
		switch (Level)
		{
		case EPrivilegeLevel::None: return TEXT("None");
		case EPrivilegeLevel::NotAuthenticated: return TEXT("NotAuthenticated");
		case EPrivilegeLevel::Client: return TEXT("Client");
		case EPrivilegeLevel::Administrator: return TEXT("Administrator");
		case EPrivilegeLevel::InitialAdmin: return TEXT("InitialAdmin");
		case EPrivilegeLevel::APIToken: return TEXT("APIToken");
		}
		return TEXT("?");
	}

	bool IsDedicated(const AFGPlayerController* Controller)
	{
		const UWorld* World = Controller ? Controller->GetWorld() : nullptr;
		return World && World->GetNetMode() == NM_DedicatedServer;
	}
}

bool STAccess::IsAdmin(const AFGPlayerController* Controller)
{
	if (!Controller)
	{
		return true;
	}
	if (Controller->IsLocalController())
	{
		return true;
	}
	const AFGPlayerState* State = Controller->GetPlayerState<AFGPlayerState>();
	if (State && State->IsServerAdmin())
	{
		return true;
	}
	const EPrivilegeLevel Level = Controller->GetDSPrivilegeLevel();
	return IsDedicated(Controller) && (Level == EPrivilegeLevel::Administrator || Level == EPrivilegeLevel::InitialAdmin);
}

bool STAccess::MayChange(const ASTSubsystem& Subsystem, const AFGPlayerController* Controller, EChange Change, const TCHAR* Action, FString& OutWhy)
{
	if (IsAdmin(Controller))
	{
		return true;
	}
	if (Change == EChange::Edit && !Subsystem.GetSettings().bAdminsOnly)
	{
		return true;
	}
	const TCHAR* Admin = IsDedicated(Controller) ? TEXT("a server admin") : TEXT("the host");
	OutWhy = Change == EChange::Network
		? FString::Printf(TEXT("Only %s can %s."), Admin, Action)
		: FString::Printf(TEXT("Only %s can %s here: 'Only admins change SmartTrains' is on."), Admin, Action);
	UE_LOG(LogSmartTrains, Log, TEXT("ACCESS refused: %s (%s) tried to %s"), *PlayerName(Controller), *Describe(Controller), Action);
	return false;
}

FString STAccess::Describe(const AFGPlayerController* Controller)
{
	if (!Controller)
	{
		return TEXT("server console, admin");
	}
	if (IsDedicated(Controller))
	{
		const AFGPlayerState* State = Controller->GetPlayerState<AFGPlayerState>();
		return FString::Printf(TEXT("%s, privilege level %s, server admin flag %s"), IsAdmin(Controller) ? TEXT("admin") : TEXT("not an admin"),
			LevelName(Controller->GetDSPrivilegeLevel()), State && State->IsServerAdmin() ? TEXT("on") : TEXT("off"));
	}
	return IsAdmin(Controller) ? TEXT("host, admin") : TEXT("client of the host, not an admin");
}
