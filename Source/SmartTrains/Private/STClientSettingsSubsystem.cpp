#include "STClientSettingsSubsystem.h"
#include "Engine/World.h"
#include "SmartTrains.h"
#include "STConfig.h"
#include "STRemoteCallObject.h"

namespace
{
	// How often the Mods menu values are read. [s]
	constexpr float ReadInterval = 0.5f;
	// Edits are sent once none has followed for this long, so dragging a slider sends one change. [s]
	constexpr double EditSettleSeconds = 2.0;
}

ASTClientSettingsSubsystem::ASTClientSettingsSubsystem()
{
	ReplicationPolicy = ESubsystemReplicationPolicy::SpawnOnClient;
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}

void ASTClientSettingsSubsystem::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	mReadTimer -= DeltaSeconds;
	if (mReadTimer > 0.f || GetNetMode() != NM_Client)
	{
		return;
	}
	mReadTimer = ReadInterval;

	TMap<FString, FString> Values;
	if (!USTConfig::ReadValues(this, Values))
	{
		return;
	}
	const double Now = GetWorld()->GetTimeSeconds();
	// The first read is what the menu held when the player joined: only later changes go to the server.
	if (!mHasLocalValues)
	{
		mLocalValues = MoveTemp(Values);
		mHasLocalValues = true;
		return;
	}
	for (const TPair<FString, FString>& Pair : Values)
	{
		const FString* Old = mLocalValues.Find(Pair.Key);
		if (!Old || *Old != Pair.Value)
		{
			mPendingEdits.Add(Pair.Key, Pair.Value);
			mLastEditTime = Now;
		}
	}
	mLocalValues = MoveTemp(Values);
	if (mPendingEdits.Num() == 0 || Now - mLastEditTime < EditSettleSeconds)
	{
		return;
	}

	TArray<FString> Keys;
	TArray<FString> Changed;
	for (const TPair<FString, FString>& Pair : mPendingEdits)
	{
		Keys.Add(Pair.Key);
		Changed.Add(Pair.Value);
	}
	mPendingEdits.Reset();
	if (USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this))
	{
		UE_LOG(LogSmartTrains, Log, TEXT("Settings: sending %d Mods menu changes to the server"), Keys.Num());
		RCO->Server_SetSettings(Keys, Changed);
	}
}
