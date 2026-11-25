#pragma once

#include "CoreMinimal.h"
#include "STViews.h"
#include "UI/STPanel.h"
#include "STTrainPanel.generated.h"

class AFGTrain;
class UBorder;
class UImage;
class USTBar;
class USTButton;
class UTextBlock;
class UWidget;
class UWrapBox;

// What the locomotive panel and the ride HUD both show of a managed train: its state, its pattern as car tiles with
// fill bars, its loop's stops and its consumer's numbers (Docs/Architecture.md, "UI").
UCLASS(Abstract)
class USTTrainDisplay : public USTPanel
{
	GENERATED_BODY()

protected:
	// Adds the state line, the pattern, the stops and the numbers to Root.
	void BuildTrainContent(UVerticalBox* Root);
	// Shows View in them.
	void ShowTrain(const FSTTrainView& View);

	virtual AFGTrain* GetTrain() const { return nullptr; }
	virtual void RequestView(USTRemoteCallObject* RCO) override;

	// The state's badge text and colour.
	static FText StateText(uint8 State);
	static FLinearColor StateColor(uint8 State);

	FBadge mState;

	UPROPERTY()
	TObjectPtr<UTextBlock> mServes;

	UPROPERTY()
	TObjectPtr<UVerticalBox> mTrainSection;

	UPROPERTY()
	TObjectPtr<UWrapBox> mCars;

	UPROPERTY()
	TObjectPtr<UVerticalBox> mStops;

	FStat mStatTrains;
	FStat mStatRate;
	FStat mStatTrip;

	UPROPERTY()
	TObjectPtr<UTextBlock> mDepotLine;

private:
	struct FCarWidgets
	{
		UImage* Icon = nullptr;
		UTextBlock* Name = nullptr;
		UTextBlock* Amount = nullptr;
		USTBar* Bar = nullptr;
	};
	struct FStopWidgets
	{
		UBorder* Row = nullptr;
		FBadge Number;
		UTextBlock* Name = nullptr;
		UTextBlock* Right = nullptr;
	};
	TArray<FCarWidgets> mCarWidgets;
	TArray<FStopWidgets> mStopWidgets;
	// What the car tiles and stop rows were built for, to rebuild them only when it changes.
	FString mCarsKey;
	FString mStopsKey;
};

// SmartTrains section in the Timetable column of the vanilla locomotive menu (Widget_Locomotive_Menu), under the
// stop list: the managed switch, the train's state, pattern, loop and numbers, and Hold / Resume. While the train is
// managed, the vanilla "Edit timetable" button is switched off, so the timetable shows read-only.
UCLASS()
class USTTrainPanel : public USTTrainDisplay
{
	GENERATED_BODY()

protected:
	virtual UWidget* WrapRoot(UVerticalBox* Root) override;
	virtual void BuildContent(UVerticalBox* Root) override;
	virtual bool OnShown() override;
	virtual void OnHidden() override;
	virtual void Refresh(USTRemoteCallObject* RCO) override;
	virtual FName GetStyleSource() const override { return TEXT("TextBlock_185"); }
	virtual AFGTrain* GetTrain() const override;

private:
	// Switches the vanilla timetable edit button off (locked) or back on.
	void SetEditLocked(bool bLocked);

	void Send(uint8 Action);

	UPROPERTY()
	TObjectPtr<USTButton> mManage;

	UPROPERTY()
	TObjectPtr<USTButton> mHold;

	UPROPERTY()
	TObjectPtr<UTextBlock> mFooter;

	TWeakObjectPtr<UWidget> mEditButton;
	bool bEditLocked = false;
	bool bManaged = false;
	bool bHeld = false;
};
