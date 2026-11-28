#pragma once

#include "CoreMinimal.h"
#include "UI/STPanel.h"
#include "UI/STStationTab.h"
#include "STStationPanel.generated.h"

class AFGTrainStationIdentifier;
class USTButton;
class UTextBlock;

// "SmartTrains" section added under the train and station lists of the vanilla station menu (Widget_TrainStationNew):
// its platforms in order with their roles and items, and for a consumer its loop (trains against its share, rate,
// round trip, use) and suppliers; for a supplier what it offers and what loops take.
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

	// Sends the station's priority as a consumer.
	void SetPriority(ESTPriority NewPriority);

	// Station name from its factory: shows it (bApply false) or renames the station.
	void SendRename(bool bApply);
	// Shows the lines and turns the rename button into its confirm step while its preview can be applied (Ready 1).
	void ShowSetUp(const TArray<FString>& Lines, uint8 Ready);
	// The station panel's widgets (none of them in the set-up bar, which shares the class).
	FBadge mRole;

	UPROPERTY()
	TObjectPtr<class UWrapBox> mStrip;

	UPROPERTY()
	TObjectPtr<UVerticalBox> mConsumer;

	UPROPERTY()
	TObjectPtr<class USTRing> mRing;

	UPROPERTY()
	TObjectPtr<UTextBlock> mRingText;

	FStat mRate;
	FStat mTrip;
	FStat mShare;
	FStat mServed;

	UPROPERTY()
	TObjectPtr<UTextBlock> mNote;

	// Its priority: Low, Medium and High, and what the choice does.
	UPROPERTY()
	TArray<TObjectPtr<class USTSegment>> mPrioritySegments;

	UPROPERTY()
	TObjectPtr<UTextBlock> mPriorityHint;

	ESTPriority mPriority = ESTPriority::Medium;
	// Edit number of the last priority click; older readouts don't undo it.
	int32 mPriorityEdit = 0;

	UPROPERTY()
	TObjectPtr<UTextBlock> mSourcesLabel;

	UPROPERTY()
	TObjectPtr<UVerticalBox> mSources;

	// What the strip and the source rows were built for.
	FString mStripKey;
	FString mSourcesKey;

	UPROPERTY()
	TObjectPtr<USTButton> mRename;

	UPROPERTY()
	TObjectPtr<UTextBlock> mSetUpLines;

	// 1 while the rename button may confirm its preview.
	uint8 mSetupReady = 0;
};

// Autodetect all platforms and Rename from factory, side by side under the station name on the station menu's first
// tab (Widget_TrainStationNew, VerticalBox_0). One row: that box doesn't push the rest of the menu down. It also adds
// the menu's SmartTrains tab (STStationTab) with the station panel as its page.
UCLASS()
class USTStationSetupBar : public USTStationPanel
{
	GENERATED_BODY()

protected:
	// Adds the SmartTrains tab once per menu widget; without the menu's tabs, the panel goes under this bar instead.
	void InstallTab();

	// The tab's page: a canvas with the content background and a scroll box with the station panel.
	UPROPERTY()
	TObjectPtr<class UCanvasPanel> mTabPage;

	// The station panel under this bar when the menu has no room for a tab.
	UPROPERTY()
	TObjectPtr<USTStationPanel> mFallbackPanel;

	FTimerHandle mOffsetTimer;

	// The menu's tab parts once the tab is added (they live as long as the menu, which holds this bar).
	STStationTab::FParts mTabParts;
	FTimerHandle mTabLogTimer;

	// Bound to the sliding tabs' OnActiveTabChanged: logs the tabs' state now and once the slide is done.
	UFUNCTION()
	void OnStationTabChanged(UWidget* PreviouslyActiveWidget, UWidget* ActiveWidget);

	virtual void BuildContent(UVerticalBox* Root) override;
	virtual bool OnShown() override;
	virtual FMargin GetOuterPadding() const override { return FMargin(0.f, 6.f, 0.f, 0.f); }

	// The bar has no room for the message line (the SmartTrains section further down shows it in full): the Autodetect
	// button shows its own result for a few seconds instead.
	virtual void ShowServerMessage(USTRemoteCallObject* RCO) override;

	// First click asks to confirm (it overwrites platforms set up already), the second runs it.
	void ClickAutodetect();

	UPROPERTY()
	TObjectPtr<USTButton> mAutodetect;

	bool bAutodetectArmed = false;
	// When Autodetect all was sent (world seconds); its answer is the first server message after it. -1: none awaited.
	double mAutodetectSentAt = -1.0;
	// Until when the button shows the result (world seconds).
	double mResultUntil = -1.0;
};
