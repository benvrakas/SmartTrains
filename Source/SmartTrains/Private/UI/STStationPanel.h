#pragma once

#include "CoreMinimal.h"
#include "UI/STPanel.h"
#include "STStationPanel.generated.h"

class AFGTrainStationIdentifier;
class USTButton;
class UTextBlock;

// "SmartTrains" section added under the train and station lists of the vanilla station menu (Widget_TrainStationNew):
// depot and pool tag, the station's platform roles and the fleet status.
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

	void SendStation(bool bNewDepot);
	// Station set-up: 0 preview, 1 apply, 2 apply keeping the name.
	void SendSetUp(uint8 Mode);
	// Shows the apply buttons only while a preview can be applied.
	void ShowSetUp(const TArray<FString>& Lines, bool bReady);
	// The depot button's text.
	virtual FText GetDepotText(bool bOn) const;

	UPROPERTY()
	TObjectPtr<USTButton> mDepot;

	UPROPERTY()
	TObjectPtr<UTextBlock> mLines;

	UPROPERTY()
	TObjectPtr<UTextBlock> mNetwork;

	UPROPERTY()
	TObjectPtr<USTButton> mSetUp;

	UPROPERTY()
	TObjectPtr<USTButton> mSetUpApply;

	UPROPERTY()
	TObjectPtr<USTButton> mSetUpKeepName;

	UPROPERTY()
	TObjectPtr<UTextBlock> mSetUpLines;

	bool bDepot = false;
};

// Depot switch added under the station name on the station menu's first tab (Widget_TrainStationNew, VerticalBox_0).
UCLASS()
class USTStationDepotBar : public USTStationPanel
{
	GENERATED_BODY()

protected:
	virtual void BuildContent(UVerticalBox* Root) override;
	virtual bool OnShown() override;
	virtual FMargin GetOuterPadding() const override { return FMargin(0.f, 6.f, 0.f, 0.f); }
	virtual FText GetDepotText(bool bOn) const override;
};
