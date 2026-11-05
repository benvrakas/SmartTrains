#pragma once

#include "CoreMinimal.h"

class AFGPlayerController;
class ASTSubsystem;

// Who may change what in SmartTrains (Docs/Architecture.md, "Admin-gated controls").
namespace STAccess
{
	enum class EChange : uint8
	{
		// Platform, station and train settings, pastes and test jobs: open to everyone unless "Only admins change
		// SmartTrains" is on.
		Edit,
		// Server settings, migration apply and undo, test copies and backups: admins only.
		Network,
	};

	// True if Controller is an admin: the host, a player the game flags as server admin, or on a dedicated server a
	// player whose privilege level is Administrator. No player (the server console) counts as an admin.
	bool IsAdmin(const AFGPlayerController* Controller);

	// True if Controller may make a change of kind Change; otherwise false, with OutWhy telling the player why. Refusals
	// are logged with Action and the player's name.
	bool MayChange(const ASTSubsystem& Subsystem, const AFGPlayerController* Controller, EChange Change, const TCHAR* Action, FString& OutWhy);

	// "admin" or "not an admin", with the dedicated server privilege level, for chat and the log.
	FString Describe(const AFGPlayerController* Controller);
}
