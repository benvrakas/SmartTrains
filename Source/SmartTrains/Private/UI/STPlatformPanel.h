#pragma once

#include "CoreMinimal.h"
#include "STTypes.h"
#include "UI/STPanel.h"
#include "STPlatformPanel.generated.h"

class AFGBuildableTrainPlatformCargo;
class UHorizontalBox;
class UImage;
class USTButton;
class USTRow;
class USTTextInput;
class UTextBlock;
class UFGItemDescriptor;

// SmartTrains panel beside the vanilla freight and fluid platform menus (Widget_Train_DockingStation,
// BPW_Train_DockingStation_Fluid): role, items with their batch or target, priority and manual rate, an item picker,
// copy/paste and copy to the whole station, and the live readout. Edits are sent at once; the panel keeps its own copy
// until the server's readout includes them.
UCLASS()
class USTPlatformPanel : public USTPanel
{
	GENERATED_BODY()

protected:
	virtual void BuildContent(UVerticalBox* Root) override;
	virtual UWidget* WrapRoot(UVerticalBox* Root) override;
	virtual bool OnShown() override;
	virtual void RequestView(USTRemoteCallObject* RCO) override;
	virtual void Refresh(USTRemoteCallObject* RCO) override;
	virtual FName GetStyleSource() const override { return TEXT("StationSettingText"); }

private:
	// Widgets of one item's rows.
	struct FItemWidgets
	{
		TWeakObjectPtr<UTextBlock> Name;
		TWeakObjectPtr<UImage> Icon;
		TWeakObjectPtr<USTButton> Priority;
		TWeakObjectPtr<UTextBlock> Amount;
		TWeakObjectPtr<UTextBlock> Readout;
		TWeakObjectPtr<USTTextInput> Rate;
		TWeakObjectPtr<UWidget> AmountRow;
	};

	AFGBuildableTrainPlatformCargo* GetPlatform() const;

	// Sends the panel's copy of the settings to the server.
	void SendEdit(const TCHAR* What);

	void SetRole(ESTRole NewRole);
	void AddItem(TSubclassOf<UFGItemDescriptor> Item);
	void RemoveItem(int32 Index);
	void CyclePriority(int32 Index, int32 Step);
	void StepAmount(int32 Index, int32 Step);
	void SetRate(int32 Index, const FString& Text);

	// Rebuilds the item rows if the item list changed, then updates all texts.
	void RefreshEditor();
	void RebuildItemRows();

	void OpenPicker();
	void ClosePicker();
	void FilterPicker(const FString& Search);

	// Settings as the player last set them (or as the server last reported them).
	ESTRole mRole = ESTRole::Off;
	TArray<FSTItemRule> mItems;
	bool bHasSettings = false;
	bool bFluid = false;
	// Edit number of this panel's last change; older readouts don't overwrite the panel's copy.
	int32 mLastEdit = 0;
	TArray<FString> mItemLines;

	UPROPERTY()
	TObjectPtr<UVerticalBox> mMain;

	UPROPERTY()
	TObjectPtr<UTextBlock> mTitle;

	UPROPERTY()
	TArray<TObjectPtr<USTButton>> mRoleButtons;

	UPROPERTY()
	TObjectPtr<UTextBlock> mRoleHint;

	UPROPERTY()
	TObjectPtr<UTextBlock> mNoItems;

	UPROPERTY()
	TObjectPtr<UVerticalBox> mItemBox;

	UPROPERTY()
	TObjectPtr<USTButton> mAddItem;

	UPROPERTY()
	TObjectPtr<USTButton> mPaste;

	UPROPERTY()
	TObjectPtr<UTextBlock> mLines;

	UPROPERTY()
	TObjectPtr<UTextBlock> mEditedBy;

	UPROPERTY()
	TObjectPtr<UVerticalBox> mPicker;

	UPROPERTY()
	TObjectPtr<USTTextInput> mSearch;

	UPROPERTY()
	TObjectPtr<UVerticalBox> mPickerList;

	TArray<FItemWidgets> mRows;
	// Items the rows were built for.
	TArray<TSubclassOf<UFGItemDescriptor>> mRowItems;
	// Picker rows and the item each stands for (null: any item).
	TArray<TPair<TWeakObjectPtr<USTRow>, TSubclassOf<UFGItemDescriptor>>> mPickerRows;
	bool bPickerBuiltFluid = false;
	bool bPickerBuilt = false;
};
