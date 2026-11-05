#pragma once

#include "CoreMinimal.h"
#include "UI/STPanel.h"
#include "STMigrationWindow.generated.h"

class APlayerController;
class USTButton;
class USTTextInput;
class UTextBlock;
struct FSTMigrationView;

// Migration window opened by "/st migrate": preview with tickable stations and trains, apply, undo, test copy and
// backup. A full-screen popup on the viewport; Close or Escape closes it.
UCLASS()
class USTMigrationWindow : public USTPanel
{
	GENERATED_BODY()

public:
	// Opens the window for Controller's local player shortly after the chat closes, or brings an open one up to date.
	static void Open(APlayerController* Controller);

	void Close();

protected:
	virtual UWidget* WrapRoot(UVerticalBox* Root) override;
	virtual void BuildContent(UVerticalBox* Root) override;
	virtual bool OnShown() override;
	virtual void OnHidden() override;
	virtual void Refresh(USTRemoteCallObject* RCO) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

private:
	static void Create(APlayerController* Controller);

	void SendMigration(uint8 Step, const TArray<FString>& Selection = TArray<FString>());
	// Preview with the trace limit from the field.
	void SendPreview();
	void ReadTraceLimit(const FString& Text);
	void SetAllTicks(bool bStations, bool bTicked);
	void ShowItemsMode();
	void ApplyTicked();
	void RefreshMigration(const FSTMigrationView& View);
	void RestoreGameInput();

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

	UPROPERTY()
	TObjectPtr<USTTextInput> mTraceLimit;

	UPROPERTY()
	TObjectPtr<USTButton> mItemsMode;

	// Per row key: shows the row ticked or not.
	TMap<FString, TFunction<void(bool)>> mShowTick;
	// Trace limit for this session's previews.
	static int32 sTraceLimit;
	// This session's previews give platforms every traced item instead of one.
	static bool sAllItems;

	TWeakObjectPtr<APlayerController> mController;
	bool bInputTaken = false;
	// Applying without a test copy or backup takes a second click.
	bool bApplyArmed = false;
	int32 mShownSerial = -1;
	// Ticks per row, keyed "S3" / "T5".
	TMap<FString, bool> mTicked;

	static TWeakObjectPtr<USTMigrationWindow> sOpen;
};
