#pragma once

#include "CoreMinimal.h"
#include "UI/STPanel.h"
#include "STStationPanel.generated.h"

class AFGTrainStationIdentifier;
class USTButton;
class USTTextInput;
class UTextBlock;
struct FSTMigrationView;

// "SmartTrains" section added under the train and station lists of the vanilla station menu (Widget_TrainStationNew):
// depot and pool tag, the station's platform roles, the fleet status, and the migration window (preview with ticked
// stations and trains, apply, undo, test copy and backup).
UCLASS()
class USTStationPanel : public USTPanel
{
	GENERATED_BODY()

protected:
	virtual void BuildContent(UVerticalBox* Root) override;
	virtual bool OnShown() override;
	virtual void RequestView(USTRemoteCallObject* RCO) override;
	virtual void Refresh(USTRemoteCallObject* RCO) override;
	virtual FMargin GetOuterPadding() const override { return FMargin(10.f, 8.f, 10.f, 10.f); }
	virtual FName GetStyleSource() const override { return TEXT("TextBlock_0"); }

private:
	AFGTrainStationIdentifier* GetStation() const;

	void SendStation(bool bNewDepot, const FString& NewPool);

	void ToggleMigration();
	void SendMigration(uint8 Step, const TArray<FString>& Selection = TArray<FString>());
	void ApplyTicked();
	void RefreshMigration(const FSTMigrationView& View);

	UPROPERTY()
	TObjectPtr<USTButton> mDepot;

	UPROPERTY()
	TObjectPtr<USTTextInput> mPool;

	UPROPERTY()
	TObjectPtr<UTextBlock> mLines;

	UPROPERTY()
	TObjectPtr<UTextBlock> mNetwork;

	UPROPERTY()
	TObjectPtr<USTButton> mMigrationToggle;

	UPROPERTY()
	TObjectPtr<UVerticalBox> mMigration;

	UPROPERTY()
	TObjectPtr<USTButton> mApply;

	UPROPERTY()
	TObjectPtr<USTButton> mUndo;

	UPROPERTY()
	TObjectPtr<UWidget> mBetaRow;

	UPROPERTY()
	TObjectPtr<UTextBlock> mMigrationLines;

	UPROPERTY()
	TObjectPtr<UVerticalBox> mMigrationRows;

	bool bDepot = false;
	FString mPoolValue;
	bool bMigrationOpen = false;
	// Applying without a test copy or backup takes a second click.
	bool bApplyArmed = false;
	int32 mShownSerial = -1;
	// Ticks per row, keyed "S3" / "T5".
	TMap<FString, bool> mTicked;
};
