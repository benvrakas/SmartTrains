#pragma once

#include "CoreMinimal.h"
#include "UI/STPanel.h"
#include "STTrainPanel.generated.h"

class AFGTrain;
class USTButton;
class USTTextInput;
class UTextBlock;
class UWidget;

// "SmartTrains" section added under the train info in the vanilla locomotive menu (Widget_Locomotive_Menu): the
// managed toggle, keep vanilla, the pool tag and the current job. While the train is managed, the vanilla "Edit
// timetable" button is switched off, so the timetable shows read-only.
UCLASS()
class USTTrainPanel : public USTPanel
{
	GENERATED_BODY()

protected:
	virtual void BuildContent(UVerticalBox* Root) override;
	virtual bool OnShown() override;
	virtual void OnHidden() override;
	virtual void RequestView(USTRemoteCallObject* RCO) override;
	virtual void Refresh(USTRemoteCallObject* RCO) override;
	virtual FMargin GetOuterPadding() const override { return FMargin(10.f, 4.f, 10.f, 8.f); }
	virtual FName GetStyleSource() const override { return TEXT("TextBlock_185"); }

private:
	AFGTrain* GetTrain() const;

	// Switches the vanilla timetable edit button off (locked) or back on.
	void SetEditLocked(bool bLocked);

	void Send(uint8 Action, const FString& Pool = FString());

	UPROPERTY()
	TObjectPtr<UTextBlock> mStatus;

	UPROPERTY()
	TObjectPtr<USTButton> mManage;

	UPROPERTY()
	TObjectPtr<USTButton> mKeepVanilla;

	UPROPERTY()
	TObjectPtr<USTTextInput> mPool;

	UPROPERTY()
	TObjectPtr<UTextBlock> mLines;

	TWeakObjectPtr<UWidget> mEditButton;
	bool bEditLocked = false;
	bool bManaged = false;
	bool bKeepVanilla = false;
};
