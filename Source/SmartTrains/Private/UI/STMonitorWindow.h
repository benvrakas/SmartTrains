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

// Throughput monitor opened by "/st monitor", in tabs: Overview (tiles and the biggest limits), Items (what limits each
// item, with rates and trains), Stations (docked share and late arrivals), Platforms (starved and backed up) and Trains
// (each managed train's state and trip or idle reason). A popup on the viewport, refreshed every second; Close or
// Escape closes it.
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
		Services,
		Items,
		Stations,
		Platforms,
		Trains,
	};

	static void Create(APlayerController* Controller);

	void SelectTab(ETab Tab);
	void UpdateTabs(const FSTMonitorView& View);
	void ShowTab(const FSTMonitorView& View);
	void ShowOverview(const FSTMonitorView& View);
	void ShowServices(const FSTMonitorView& View);
	void ShowItems(const FSTMonitorView& View);
	void ShowStations(const FSTMonitorView& View);
	void ShowPlatforms(const FSTMonitorView& View);
	void ShowTrains(const FSTMonitorView& View);

	// Table pieces. Width 0 fills the rest of the line.
	UHorizontalBox* AddLine(const FMargin& Spacing = FMargin(0.f, 3.f));
	void AddCell(UHorizontalBox* Line, UWidget* Cell, float Width);
	UTextBlock* MakeCell(const FString& Text, int32 Size, const FLinearColor& Color, bool bWrap = false);
	UWidget* MakeIcon(TSubclassOf<UFGItemDescriptor> Item, float Size);
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

	struct FTabWidgets
	{
		TWeakObjectPtr<USTButton> Button;
		TWeakObjectPtr<UBorder> Underline;
	};
	TArray<FTabWidgets> mTabs;

	TWeakObjectPtr<APlayerController> mController;
	bool bInputTaken = false;
	// The text of the view shown and the tab it was shown in, to rebuild only when either changes.
	FString mShown;
	ETab mShownTab = ETab::Overview;

	// The tab and the item filter, kept while the game runs.
	static ETab sTab;
	static bool sProblemsOnly;
	static TWeakObjectPtr<USTMonitorWindow> sOpen;
};
