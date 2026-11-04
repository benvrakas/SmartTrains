#pragma once

#include "CoreMinimal.h"
#include "UI/STPanel.h"
#include "STStationPanel.generated.h"

class AFGTrainStationIdentifier;
class USTButton;
class USTTextInput;
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

private:
	AFGTrainStationIdentifier* GetStation() const;

	void SendStation(bool bNewDepot, const FString& NewPool);

	UPROPERTY()
	TObjectPtr<USTButton> mDepot;

	UPROPERTY()
	TObjectPtr<USTTextInput> mPool;

	UPROPERTY()
	TObjectPtr<UTextBlock> mLines;

	UPROPERTY()
	TObjectPtr<UTextBlock> mNetwork;

	bool bDepot = false;
	FString mPoolValue;
};
