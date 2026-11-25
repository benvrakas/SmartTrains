#pragma once

#include "CoreMinimal.h"
#include "UI/STPanel.h"
#include "STMonitorWindow.generated.h"

class APlayerController;
class UBorder;
class UFGItemDescriptor;
class UHorizontalBox;
class UTextBlock;
class UVerticalBox;
struct FSTMonitorRow;
struct FSTMonitorView;

// The loop plan's monitor opened by "/st monitor": tiles on top, then tabs: Overview (what needs a look), Consumers
// (each consumer's pattern, trains against its share, rate, round trip), Suppliers (rate and what loops take) and
// Trains (state and consumer). Rows open their details on a click of their toggle. A popup on the viewport, refreshed
// every second; Close or Escape closes it.
UCLASS()
class USTMonitorWindow : public USTPanel
{
	GENERATED_BODY()

public:
	// Opens the window for Controller's local player shortly after the chat closes.
	static void Open(APlayerController* Controller);

	void Close();

protected:
	virtual UWidget* WrapRoot(UVerticalBox* Root) override;
	virtual void BuildContent(UVerticalBox* Root) override;
	virtual void OnHidden() override;
	virtual void RequestView(USTRemoteCallObject* RCO) override;
	virtual void Refresh(USTRemoteCallObject* RCO) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

private:
	enum class ETab : uint8
	{
		Overview,
		Consumers,
		Suppliers,
		Trains,
	};

	static void Create(APlayerController* Controller);

	void SelectTab(ETab Tab);
	void UpdateTabs(const FSTMonitorView& View);
	void ShowTab(const FSTMonitorView& View);
	void ShowOverview(const FSTMonitorView& View);
	void ShowConsumers(const FSTMonitorView& View);
	void ShowSuppliers(const FSTMonitorView& View);
	void ShowTrains(const FSTMonitorView& View);

	// Table pieces. Width 0 fills the rest of the line.
	UHorizontalBox* AddLine(const FMargin& Spacing = FMargin(0.f, 3.f));
	void AddCell(UHorizontalBox* Line, UWidget* Cell, float Width);
	UTextBlock* MakeCell(const FString& Text, int32 Size, const FLinearColor& Color, bool bWrap = false);
	UWidget* MakeItemIcon(TSubclassOf<UFGItemDescriptor> Item, float Size);
	// A row card: severity on its left edge; Toggle opens its details (Key: whose).
	UHorizontalBox* AddRowCard(uint8 Severity, const FString& Key, const TArray<FString>& Details);
	void ShowTiles(const FSTMonitorView& View);
	// Puts the row's details toggle (from AddRowCard) at the end of Line.
	void FinishRow(UHorizontalBox* Line);

	UPROPERTY()
	TObjectPtr<UHorizontalBox> mPendingToggle;
	void AddHeader(const TArray<TPair<FText, float>>& Columns);
	void AddDivider();
	void AddHeading(const FText& Text);
	void AddDetails(const TArray<FString>& Lines, float Indent);
	void AddNote(const FText& Text, const FLinearColor& Color);
	// A row's Title, Verdict and cells with the tab's widths, and its details for rows that aren't fine.
	void AddRow(const FSTMonitorRow& Row, bool bIcon, const TArray<float>& Widths, float TitleWidth, float VerdictWidth);
	void RestoreGameInput();

	UPROPERTY()
	TObjectPtr<UVerticalBox> mContent;

	UPROPERTY()
	TObjectPtr<UTextBlock> mWindowText;

	UPROPERTY()
	TArray<TObjectPtr<class USTSegment>> mTabs;

	UPROPERTY()
	TObjectPtr<UHorizontalBox> mTiles;

	// Rows whose details are open, by title.
	TSet<FString> mExpanded;

	TWeakObjectPtr<APlayerController> mController;
	bool bInputTaken = false;
	// The text of the view shown and the tab it was shown in, to rebuild only when either changes.
	FString mShown;
	ETab mShownTab = ETab::Overview;

	// The tab, kept while the game runs.
	static ETab sTab;
	static TWeakObjectPtr<USTMonitorWindow> sOpen;
};
