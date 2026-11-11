#pragma once

#include "CoreMinimal.h"
#include "UI/STPanel.h"
#include "STTrainPanel.generated.h"

class AFGTrain;
class USTButton;
class UTextBlock;
class UWidget;

// SmartTrains section in the Timetable column of the vanilla locomotive menu (Widget_Locomotive_Menu), under the
// stop list: the managed switch, one status line and the job's per-stop lines. While the train is managed, the
// vanilla "Edit timetable" button is switched off, so the timetable shows read-only.
UCLASS()
class USTTrainPanel : public USTPanel
{
	GENERATED_BODY()

protected:
	virtual UWidget* WrapRoot(UVerticalBox* Root) override;
	virtual void BuildContent(UVerticalBox* Root) override;
	virtual bool OnShown() override;
	virtual void OnHidden() override;
	virtual void RequestView(USTRemoteCallObject* RCO) override;
	virtual void Refresh(USTRemoteCallObject* RCO) override;
	virtual FName GetStyleSource() const override { return TEXT("TextBlock_185"); }

private:
	AFGTrain* GetTrain() const;

	// Switches the vanilla timetable edit button off (locked) or back on.
	void SetEditLocked(bool bLocked);

	void Send(uint8 Action);

	UPROPERTY()
	TObjectPtr<USTButton> mManage;

	// Hold / Resume and Cancel job, shown while managed.
	UPROPERTY()
	TObjectPtr<class UHorizontalBox> mControls;

	UPROPERTY()
	TObjectPtr<USTButton> mHold;

	UPROPERTY()
	TObjectPtr<USTButton> mCancelJob;

	UPROPERTY()
	TObjectPtr<UTextBlock> mStatus;

	UPROPERTY()
	TObjectPtr<UTextBlock> mLines;

	TWeakObjectPtr<UWidget> mEditButton;
	bool bEditLocked = false;
	bool bManaged = false;
	bool bHeld = false;
};
