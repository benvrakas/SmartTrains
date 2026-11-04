#pragma once

#include "CoreMinimal.h"
#include "FGRemoteCallObject.h"
#include "STViews.h"
#include "STRemoteCallObject.generated.h"

class AFGBuildableTrainPlatformCargo;
class AFGTrain;
class AFGTrainStationIdentifier;

// Carries a player's SmartTrains menu edits to the server and the server's readouts back (Docs/Architecture.md, "UI").
// The subsystem lives on the server only, so every menu, the host's included, reads its state through this object.
UCLASS(NotBlueprintable)
class SMARTTRAINS_API USTRemoteCallObject : public UFGRemoteCallObject
{
	GENERATED_BODY()

public:
	//~ Edits. Edit is the sender's edit number, echoed in later views.

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_SetPlatform(AFGBuildableTrainPlatformCargo* Platform, ESTRole Role, const TArray<FSTItemRule>& Items, int32 Edit);

	// Gives every platform of the same cargo type in Platform's station Platform's settings.
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_CopyPlatformToStation(AFGBuildableTrainPlatformCargo* Platform, int32 Edit);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_SetStation(AFGTrainStationIdentifier* Station, bool bDepot, const FString& Pool, int32 Edit);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_SetTrain(AFGTrain* Train, ESTTrainAction Action, const FString& Pool, int32 Edit);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_Migrate(ESTMigrationStep Step, const TArray<FString>& Selection);

	//~ Readouts: the server answers each request with the matching Client_ call.

	UFUNCTION(Server, Unreliable)
	void Server_RequestPlatform(AFGBuildableTrainPlatformCargo* Platform);

	UFUNCTION(Server, Unreliable)
	void Server_RequestTrain(AFGTrain* Train);

	UFUNCTION(Server, Unreliable)
	void Server_RequestStation(AFGTrainStationIdentifier* Station);

	UFUNCTION(Client, Reliable)
	void Client_Platform(const FSTPlatformView& View);

	UFUNCTION(Client, Reliable)
	void Client_Train(const FSTTrainView& View);

	UFUNCTION(Client, Reliable)
	void Client_Station(const FSTStationView& View);

	UFUNCTION(Client, Reliable)
	void Client_Migration(const FSTMigrationView& View);

	// Opens the migration window ("/st migrate").
	UFUNCTION(Client, Reliable)
	void Client_OpenMigrationWindow();

	// Tells the player why an edit was refused or what it did.
	UFUNCTION(Client, Reliable)
	void Client_Message(const FString& Message, bool bError);

	//~ Local side.

	// Returns the local player's instance, or nullptr (logged) if the server doesn't run the mod.
	static USTRemoteCallObject* GetLocal(const UObject* WorldContext);

	// Takes the next edit number to send with an edit.
	int32 NextEdit() { return ++mSentEdit; }

	// Latest views received, or nullptr. Views made before this player's last edit are dropped, so a click isn't undone
	// by a readout that was already on its way.
	const FSTPlatformView* GetPlatformView(const AFGBuildableTrainPlatformCargo* Platform) const;
	const FSTTrainView* GetTrainView(const AFGTrain* Train) const;
	const FSTStationView* GetStationView(const AFGTrainStationIdentifier* Station) const;
	const FSTMigrationView& GetMigrationView() const { return mMigration; }

	// Changes whenever a view or message arrives.
	uint32 GetRevision() const { return mRevision; }

	// Last message from the server and when it came (game seconds).
	const FString& GetLastMessage() const { return mLastMessage; }
	bool IsLastMessageError() const { return bLastMessageError; }
	double GetLastMessageTime() const { return mLastMessageTime; }

	// Returns the name of the player who owns this object, for the log.
	FString GetPlayerName() const;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	UPROPERTY(Replicated, Meta = (NoAutoJson))
	bool mForceNetField_USTRemoteCallObject = false;

	// Client side: edit numbers sent so far.
	int32 mSentEdit = 0;
	// Server side: the last edit number applied from this player.
	int32 mAppliedEdit = 0;
	// Server side: counts migration answers.
	int32 mMigrationSerial = 0;

	TMap<TWeakObjectPtr<AFGBuildableTrainPlatformCargo>, FSTPlatformView> mPlatforms;
	TMap<TWeakObjectPtr<AFGTrain>, FSTTrainView> mTrains;
	TMap<TWeakObjectPtr<AFGTrainStationIdentifier>, FSTStationView> mStations;
	FSTMigrationView mMigration;
	uint32 mRevision = 0;
	FString mLastMessage;
	bool bLastMessageError = false;
	double mLastMessageTime = -1000.0;
};
