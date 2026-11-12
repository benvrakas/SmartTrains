#pragma once

#include "CoreMinimal.h"
#include "UI/STPanel.h"
#include "STStationPanel.generated.h"

class AFGTrainStationIdentifier;
class USTButton;
class UTextBlock;

// "SmartTrains" section added under the train and station lists of the vanilla station menu (Widget_TrainStationNew):
// the station's platform roles and the fleet status.
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

	AFGTrainStationIdentifier* GetStation() const;

	// Station name from its factory: shows it (bApply false) or renames the station.
	void SendRename(bool bApply);
	// Shows the lines and turns the rename button into its confirm step while its preview can be applied (Ready 1).
	void ShowSetUp(const TArray<FString>& Lines, uint8 Ready);
	UPROPERTY()
	TObjectPtr<UTextBlock> mLines;

	UPROPERTY()
	TObjectPtr<UTextBlock> mNetwork;

	UPROPERTY()
	TObjectPtr<USTButton> mRename;

	UPROPERTY()
	TObjectPtr<UTextBlock> mSetUpLines;

	// 1 while the rename button may confirm its preview.
	uint8 mSetupReady = 0;
};

// Autodetect all platforms and Rename from factory, side by side under the station name on the station menu's first
// tab (Widget_TrainStationNew, VerticalBox_0). One row: that box doesn't push the rest of the menu down.
UCLASS()
class USTStationSetupBar : public USTStationPanel
{
	GENERATED_BODY()

protected:
	virtual void BuildContent(UVerticalBox* Root) override;
	virtual bool OnShown() override;
	virtual FMargin GetOuterPadding() const override { return FMargin(0.f, 6.f, 0.f, 0.f); }

	// First click asks to confirm (it overwrites platforms set up already), the second runs it.
	void ClickAutodetect();

	UPROPERTY()
	TObjectPtr<USTButton> mAutodetect;

	bool bAutodetectArmed = false;
};
