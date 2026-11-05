#include "UI/STMonitorWindow.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Resources/FGItemDescriptor.h"
#include "SmartTrains.h"
#include "STRemoteCallObject.h"
#include "TimerManager.h"
#include "UI/STControls.h"
#include "UI/STWidgetUtils.h"

#define LOCTEXT_NAMESPACE "SmartTrains"

TWeakObjectPtr<USTMonitorWindow> USTMonitorWindow::sOpen;
USTMonitorWindow::ETab USTMonitorWindow::sTab = USTMonitorWindow::ETab::Overview;
bool USTMonitorWindow::sProblemsOnly = false;

namespace
{
	constexpr float MonitorWidth = 1100.f;
	constexpr float MonitorMaxHeight = 820.f;
	constexpr float MonitorIconSize = 28.f;
	const FLinearColor MonitorText(0.92f, 0.92f, 0.92f);
	const FLinearColor MonitorDim(0.62f, 0.62f, 0.62f);
	const FLinearColor MonitorProblem(1.f, 0.5f, 0.22f);
	const FLinearColor MonitorNote(1.f, 0.84f, 0.32f);
	const FLinearColor MonitorFine(0.5f, 0.85f, 0.5f);
	// The game's orange, for the selected tab.
	const FLinearColor MonitorAccent(0.98f, 0.58f, 0.15f);
	const FLinearColor MonitorTile(0.f, 0.f, 0.f, 0.35f);
	const FLinearColor MonitorDivider(1.f, 1.f, 1.f, 0.08f);
	// Wait after the command before taking the input, so the closing chat doesn't take it back. [s]
	constexpr float MonitorOpenDelay = 0.2f;

	FLinearColor SeverityColor(uint8 Severity)
	{
		return Severity >= 2 ? MonitorProblem : (Severity == 1 ? MonitorNote : MonitorFine);
	}

	FString Signature(const FSTMonitorView& View)
	{
		FString Text = FString::Join(View.Summary, TEXT("|")) + FString::Join(View.Idle, TEXT("|"));
		for (const TArray<FSTMonitorRow>* Rows : {&View.Tiles, &View.Items, &View.Stations, &View.Platforms, &View.Trains})
		{
			for (const FSTMonitorRow& Row : *Rows)
			{
				Text += Row.Title + Row.Verdict + Row.Advice + FString::Join(Row.Columns, TEXT("|")) + FString::Join(Row.Details, TEXT("|"));
			}
		}
		return Text;
	}

	int32 CountProblems(const TArray<FSTMonitorRow>& Rows, uint8 AtLeast)
	{
		int32 Count = 0;
		for (const FSTMonitorRow& Row : Rows)
		{
			Count += Row.Severity >= AtLeast ? 1 : 0;
		}
		return Count;
	}
}

//~ Opening and closing

void USTMonitorWindow::Open(APlayerController* Controller)
{
	if (!Controller || !Controller->IsLocalController() || IsRunningDedicatedServer())
	{
		return;
	}
	if (USTMonitorWindow* Existing = sOpen.Get(); Existing && Existing->IsInViewport())
	{
		return;
	}
	FTimerHandle Handle;
	TWeakObjectPtr<APlayerController> Weak(Controller);
	Controller->GetWorldTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([Weak]()
	{
		if (APlayerController* Still = Weak.Get())
		{
			Create(Still);
		}
	}), MonitorOpenDelay, false);
}

void USTMonitorWindow::Create(APlayerController* Controller)
{
	if (sOpen.IsValid() && sOpen->IsInViewport())
	{
		return;
	}
	USTMonitorWindow* Window = CreateWidget<USTMonitorWindow>(Controller, USTMonitorWindow::StaticClass());
	if (!Window)
	{
		UE_LOG(LogSmartTrains, Error, TEXT("Monitor window could not be created"));
		return;
	}
	Window->mController = Controller;
	Window->SetIsFocusable(true);
	Window->AddToViewport(100);
	sOpen = Window;
	UWidgetBlueprintLibrary::SetInputMode_UIOnlyEx(Controller, Window, EMouseLockMode::DoNotLock);
	Controller->SetShowMouseCursor(true);
	Window->bInputTaken = true;
	UE_LOG(LogSmartTrains, Log, TEXT("Monitor window opened"));
}

void USTMonitorWindow::Close()
{
	UE_LOG(LogSmartTrains, Log, TEXT("Monitor window closed"));
	RemoveFromParent();
}

void USTMonitorWindow::RestoreGameInput()
{
	if (!bInputTaken)
	{
		return;
	}
	bInputTaken = false;
	if (APlayerController* Controller = mController.Get())
	{
		UWidgetBlueprintLibrary::SetInputMode_GameOnly(Controller);
		Controller->SetShowMouseCursor(false);
	}
}

void USTMonitorWindow::OnHidden()
{
	RestoreGameInput();
	if (sOpen.Get() == this)
	{
		sOpen.Reset();
	}
}

FReply USTMonitorWindow::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		Close();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

//~ Frame and tabs

UWidget* USTMonitorWindow::WrapRoot(UVerticalBox* Root)
{
	UOverlay* Screen = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
	// Dims the game and catches clicks beside the window, so the window keeps the keyboard focus.
	UBorder* Dim = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Dim->SetBrushColor(FLinearColor(0.f, 0.f, 0.f, 0.45f));
	Dim->SetVisibility(ESlateVisibility::Visible);
	if (UOverlaySlot* DimSlot = Screen->AddChildToOverlay(Dim))
	{
		DimSlot->SetHorizontalAlignment(HAlign_Fill);
		DimSlot->SetVerticalAlignment(VAlign_Fill);
	}
	UOverlay* Frame = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
	UWidget* Background = STWidgetUtils::CreateVanillaWidget(WidgetTree, STWidgetUtils::ContentBackgroundClass);
	if (!Background)
	{
		UBorder* Plain = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Plain->SetBrushColor(FLinearColor(0.02f, 0.02f, 0.025f, 0.96f));
		Background = Plain;
	}
	if (UOverlaySlot* BackgroundSlot = Frame->AddChildToOverlay(Background))
	{
		BackgroundSlot->SetHorizontalAlignment(HAlign_Fill);
		BackgroundSlot->SetVerticalAlignment(VAlign_Fill);
	}
	USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Size->SetWidthOverride(MonitorWidth);
	Size->SetMaxDesiredHeight(MonitorMaxHeight);
	Size->AddChild(Root);
	if (UOverlaySlot* ContentSlot = Frame->AddChildToOverlay(Size))
	{
		ContentSlot->SetPadding(FMargin(20.f, 14.f));
	}
	if (UOverlaySlot* FrameSlot = Screen->AddChildToOverlay(Frame))
	{
		FrameSlot->SetHorizontalAlignment(HAlign_Center);
		FrameSlot->SetVerticalAlignment(VAlign_Center);
	}
	return Screen;
}

void USTMonitorWindow::BuildContent(UVerticalBox* Root)
{
	UHorizontalBox* Title = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Root->AddChildToVerticalBox(Title);
	if (UHorizontalBoxSlot* TitleSlot = Title->AddChildToHorizontalBox(MakeText(nullptr, LOCTEXT("MonitorHeading", "SmartTrains monitor"), 18, false)))
	{
		TitleSlot->SetVerticalAlignment(VAlign_Center);
		TitleSlot->SetPadding(FMargin(0.f, 0.f, 16.f, 0.f));
	}
	mWindowText = MakeText(nullptr, LOCTEXT("MonitorWaiting", "Asking the server..."), 10, false);
	mWindowText->SetColorAndOpacity(FSlateColor(MonitorDim));
	if (UHorizontalBoxSlot* WindowSlot = Title->AddChildToHorizontalBox(mWindowText))
	{
		WindowSlot->SetVerticalAlignment(VAlign_Center);
		WindowSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	USTButton* CloseButton = MakeButton(nullptr, LOCTEXT("Close", "Close"), [this]() { Close(); });
	CloseButton->SetFixedWidth(140.f);
	if (UHorizontalBoxSlot* CloseSlot = Title->AddChildToHorizontalBox(CloseButton))
	{
		CloseSlot->SetVerticalAlignment(VAlign_Center);
	}

	// Tab bar: a button per tab with a line under the selected one.
	UHorizontalBox* Bar = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Root->AddChildToVerticalBox(Bar)->SetPadding(FMargin(0.f, 10.f, 0.f, 0.f));
	const TArray<TPair<ETab, FText>> Tabs = {
		{ETab::Overview, LOCTEXT("TabOverview", "Overview")},
		{ETab::Items, LOCTEXT("TabItems", "Items")},
		{ETab::Stations, LOCTEXT("TabStations", "Stations")},
		{ETab::Platforms, LOCTEXT("TabPlatforms", "Platforms")},
		{ETab::Trains, LOCTEXT("TabTrains", "Trains")},
	};
	mTabs.Reset();
	for (const TPair<ETab, FText>& Tab : Tabs)
	{
		UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		const ETab Which = Tab.Key;
		USTButton* Button = MakeButton(nullptr, Tab.Value, [this, Which]() { SelectTab(Which); }, FMargin(0.f));
		Column->AddChildToVerticalBox(Button);
		UBorder* Underline = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		USizeBox* LineBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		LineBox->SetHeightOverride(3.f);
		Underline->SetContent(LineBox);
		Column->AddChildToVerticalBox(Underline)->SetPadding(FMargin(0.f, 2.f, 0.f, 0.f));
		if (UHorizontalBoxSlot* TabSlot = Bar->AddChildToHorizontalBox(Column))
		{
			TabSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			TabSlot->SetPadding(FMargin(0.f, 0.f, 6.f, 0.f));
		}
		mTabs.Add({Button, Underline});
	}

	UBorder* Rule = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Rule->SetBrushColor(MonitorDivider);
	USizeBox* RuleBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	RuleBox->SetHeightOverride(1.f);
	Rule->SetContent(RuleBox);
	Root->AddChildToVerticalBox(Rule)->SetPadding(FMargin(0.f, 0.f, 0.f, 8.f));

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	mContent = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Scroll->AddChild(mContent);
	if (UVerticalBoxSlot* ScrollSlot = Root->AddChildToVerticalBox(Scroll))
	{
		ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	MakeText(mContent, LOCTEXT("MonitorLoading", "Asking the server..."), 11);
}

void USTMonitorWindow::SelectTab(ETab Tab)
{
	sTab = Tab;
	UE_LOG(LogSmartTrains, Log, TEXT("Monitor window: tab %d"), static_cast<int32>(Tab));
	if (const USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this))
	{
		const FSTMonitorView& View = RCO->GetMonitorView();
		UpdateTabs(View);
		if (View.Tiles.Num() > 0)
		{
			mShownTab = sTab;
			ShowTab(View);
		}
	}
}

void USTMonitorWindow::UpdateTabs(const FSTMonitorView& View)
{
	const TArray<int32> Counts = {
		CountProblems(View.Items, 2) + CountProblems(View.Stations, 2),
		CountProblems(View.Items, 2),
		CountProblems(View.Stations, 2),
		View.Platforms.Num(),
		CountProblems(View.Trains, 1),
	};
	const TArray<FText> Names = {
		LOCTEXT("TabOverview", "Overview"), LOCTEXT("TabItems", "Items"), LOCTEXT("TabStations", "Stations"), LOCTEXT("TabPlatforms", "Platforms"),
		LOCTEXT("TabTrains", "Trains")};
	for (int32 Index = 0; Index < mTabs.Num(); ++Index)
	{
		const bool bSelected = static_cast<int32>(sTab) == Index;
		if (USTButton* Button = mTabs[Index].Button.Get())
		{
			// The overview counts what limits deliveries; the other tabs count their rows that need a look.
			Button->SetText(Counts[Index] > 0 && Index > 0 ? FText::Format(LOCTEXT("TabCount", "{0}  ({1})"), Names[Index], FText::AsNumber(Counts[Index])) : Names[Index]);
		}
		if (UBorder* Underline = mTabs[Index].Underline.Get())
		{
			Underline->SetBrushColor(bSelected ? MonitorAccent : FLinearColor::Transparent);
		}
	}
}

//~ Data

void USTMonitorWindow::RequestView(USTRemoteCallObject* RCO)
{
	RCO->Server_RequestMonitor();
}

void USTMonitorWindow::Refresh(USTRemoteCallObject* RCO)
{
	const FSTMonitorView& View = RCO->GetMonitorView();
	if (View.Tiles.Num() == 0)
	{
		return;
	}
	FString Shown = Signature(View);
	if (Shown == mShown && mShownTab == sTab)
	{
		return;
	}
	mShown = MoveTemp(Shown);
	mShownTab = sTab;
	mWindowText->SetText(View.WindowMinutes > 0
		? FText::Format(LOCTEXT("MonitorWindow", "Judged over the last {0} min, updated every 10 s"), FText::AsNumber(View.WindowMinutes))
		: LOCTEXT("MonitorFirstMinute", "Collecting the first minute of samples"));
	UpdateTabs(View);
	ShowTab(View);
}

void USTMonitorWindow::ShowTab(const FSTMonitorView& View)
{
	mContent->ClearChildren();
	switch (sTab)
	{
	case ETab::Items: ShowItems(View); break;
	case ETab::Stations: ShowStations(View); break;
	case ETab::Platforms: ShowPlatforms(View); break;
	case ETab::Trains: ShowTrains(View); break;
	default: ShowOverview(View); break;
	}
}

//~ Table pieces

UHorizontalBox* USTMonitorWindow::AddLine(const FMargin& Spacing)
{
	UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	mContent->AddChildToVerticalBox(Line)->SetPadding(Spacing);
	return Line;
}

void USTMonitorWindow::AddCell(UHorizontalBox* Line, UWidget* Cell, float Width)
{
	UWidget* Child = Cell;
	if (Width > 0.f)
	{
		USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		Box->SetWidthOverride(Width);
		Box->AddChild(Cell);
		Child = Box;
	}
	if (UHorizontalBoxSlot* CellSlot = Line->AddChildToHorizontalBox(Child))
	{
		CellSlot->SetVerticalAlignment(VAlign_Center);
		CellSlot->SetPadding(FMargin(0.f, 0.f, 10.f, 0.f));
		if (Width <= 0.f)
		{
			CellSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}
	}
}

UTextBlock* USTMonitorWindow::MakeCell(const FString& Text, int32 Size, const FLinearColor& Color, bool bWrap)
{
	UTextBlock* Cell = MakeText(nullptr, FText::FromString(Text), Size, bWrap);
	Cell->SetColorAndOpacity(FSlateColor(Color));
	return Cell;
}

UWidget* USTMonitorWindow::MakeIcon(TSubclassOf<UFGItemDescriptor> Item, float Size)
{
	USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Box->SetWidthOverride(Size);
	Box->SetHeightOverride(Size);
	if (UTexture2D* Texture = Item ? UFGItemDescriptor::GetSmallIcon(Item) : nullptr)
	{
		UImage* Icon = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
		Icon->SetBrushFromTexture(Texture);
		Box->AddChild(Icon);
	}
	return Box;
}

void USTMonitorWindow::AddHeader(const TArray<TPair<FText, float>>& Columns)
{
	UHorizontalBox* Line = AddLine(FMargin(0.f, 0.f, 0.f, 2.f));
	for (const TPair<FText, float>& Column : Columns)
	{
		UTextBlock* Cell = MakeText(nullptr, Column.Key, 10, false);
		Cell->SetColorAndOpacity(FSlateColor(MonitorDim));
		AddCell(Line, Cell, Column.Value);
	}
	AddDivider();
}

void USTMonitorWindow::AddDivider()
{
	UBorder* Line = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Line->SetBrushColor(MonitorDivider);
	USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Box->SetHeightOverride(1.f);
	Line->SetContent(Box);
	mContent->AddChildToVerticalBox(Line)->SetPadding(FMargin(0.f, 3.f));
}

void USTMonitorWindow::AddHeading(const FText& Text)
{
	MakeText(mContent, Text, 15, false, FMargin(0.f, 14.f, 0.f, 6.f));
}

void USTMonitorWindow::AddDetails(const TArray<FString>& Lines, float Indent)
{
	if (Lines.Num() == 0)
	{
		return;
	}
	UTextBlock* Text = MakeText(mContent, FText::GetEmpty(), 10, true, FMargin(Indent, 0.f, 8.f, 2.f));
	SetLines(Text, Lines);
	Text->SetColorAndOpacity(FSlateColor(MonitorDim));
}

void USTMonitorWindow::AddNote(const FText& Text, const FLinearColor& Color)
{
	UTextBlock* Note = MakeText(mContent, Text, 11, true, FMargin(0.f, 4.f));
	Note->SetColorAndOpacity(FSlateColor(Color));
}

void USTMonitorWindow::AddRow(const FSTMonitorRow& Row, bool bIcon, const TArray<float>& Widths, float TitleWidth, float VerdictWidth)
{
	UHorizontalBox* Line = AddLine();
	if (bIcon)
	{
		AddCell(Line, MakeIcon(Row.Item, MonitorIconSize), MonitorIconSize);
	}
	AddCell(Line, MakeCell(Row.Title, 12, MonitorText, true), TitleWidth);
	AddCell(Line, MakeCell(Row.Verdict, 12, SeverityColor(Row.Severity)), VerdictWidth);
	for (int32 Index = 0; Index < Widths.Num(); ++Index)
	{
		AddCell(Line, MakeCell(Row.Columns.IsValidIndex(Index) ? Row.Columns[Index] : FString(), 11, MonitorText), Widths[Index]);
	}
	if (Row.Severity > 0)
	{
		TArray<FString> Lines = Row.Details;
		if (!Row.Advice.IsEmpty())
		{
			Lines.Add(TEXT("What to do: ") + Row.Advice);
		}
		AddDetails(Lines, bIcon ? MonitorIconSize + 10.f : 0.f);
	}
	AddDivider();
}

//~ Tabs

void USTMonitorWindow::ShowOverview(const FSTMonitorView& View)
{
	// Tiles.
	UHorizontalBox* Tiles = AddLine(FMargin(0.f, 2.f, 0.f, 6.f));
	for (const FSTMonitorRow& Tile : View.Tiles)
	{
		UBorder* Box = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Box->SetBrushColor(MonitorTile);
		Box->SetPadding(FMargin(14.f, 10.f));
		UVerticalBox* Inside = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		Inside->AddChildToVerticalBox(MakeCell(Tile.Verdict, 24, Tile.Severity > 0 ? SeverityColor(Tile.Severity) : MonitorText));
		Inside->AddChildToVerticalBox(MakeCell(Tile.Title, 10, MonitorDim));
		Box->SetContent(Inside);
		if (UHorizontalBoxSlot* TileSlot = Tiles->AddChildToHorizontalBox(Box))
		{
			TileSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			TileSlot->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
		}
	}

	// The biggest limits, each with its last advice line.
	AddHeading(LOCTEXT("BiggestLimits", "What limits deliveries"));
	int32 Shown = 0;
	for (const FSTMonitorRow& Row : View.Items)
	{
		if (Row.Severity < 2 || Shown >= 8)
		{
			continue;
		}
		++Shown;
		UHorizontalBox* Line = AddLine();
		AddCell(Line, MakeIcon(Row.Item, MonitorIconSize), MonitorIconSize);
		AddCell(Line, MakeCell(Row.Title, 12, MonitorText, true), 240.f);
		AddCell(Line, MakeCell(Row.Verdict, 12, SeverityColor(Row.Severity)), 150.f);
		AddCell(Line, MakeCell(!Row.Advice.IsEmpty() ? Row.Advice : (Row.Details.Num() > 0 ? Row.Details[0] : FString()), 10, MonitorDim, true), 0.f);
		AddDivider();
	}
	for (const FSTMonitorRow& Row : View.Stations)
	{
		if (Row.Severity < 2 || Shown >= 10)
		{
			continue;
		}
		++Shown;
		UHorizontalBox* Line = AddLine();
		AddCell(Line, MakeIcon(nullptr, MonitorIconSize), MonitorIconSize);
		AddCell(Line, MakeCell(Row.Title, 12, MonitorText, true), 240.f);
		AddCell(Line, MakeCell(Row.Verdict, 12, SeverityColor(Row.Severity)), 150.f);
		AddCell(Line, MakeCell(!Row.Advice.IsEmpty() ? Row.Advice : (Row.Details.Num() > 0 ? Row.Details[0] : FString()), 10, MonitorDim, true), 0.f);
		AddDivider();
	}
	if (Shown == 0)
	{
		AddNote(View.Items.Num() > 0 ? LOCTEXT("NothingLimits", "Nothing limits deliveries right now.")
			: LOCTEXT("NoItemsYet", "No supply or demand platforms yet. Give platforms a role in their menus, or use /st migrate."), MonitorFine);
	}

	AddHeading(LOCTEXT("FleetHeading", "Fleet"));
	AddDetails(View.Summary, 0.f);
	if (View.Idle.Num() > 0)
	{
		AddHeading(LOCTEXT("IdleHeading", "Idle trains and why"));
		AddDetails(View.Idle, 0.f);
	}
}

void USTMonitorWindow::ShowItems(const FSTMonitorView& View)
{
	UHorizontalBox* Tools = AddLine(FMargin(0.f, 0.f, 0.f, 8.f));
	USTButton* Filter = MakeButton(nullptr, sProblemsOnly ? LOCTEXT("ShowAllItems", "Showing items that need a look; click for all")
		: LOCTEXT("ShowProblems", "Showing all items; click for the ones that need a look"), [this]()
	{
		sProblemsOnly = !sProblemsOnly;
		SelectTab(ETab::Items);
	}, FMargin(0.f));
	AddCell(Tools, Filter, 520.f);
	AddCell(Tools, MakeCell(TEXT("Rates per minute. Trains: needed by the flow plan / managed trains that fit."), 10, MonitorDim, true), 0.f);

	const TArray<float> Widths = {120.f, 120.f, 110.f, 100.f};
	AddHeader({{FText::GetEmpty(), MonitorIconSize}, {LOCTEXT("ColItem", "Item"), 0.f}, {LOCTEXT("ColLimit", "Limit"), 150.f},
		{LOCTEXT("ColSupply", "Supply"), Widths[0]}, {LOCTEXT("ColDemand", "Demand"), Widths[1]}, {LOCTEXT("ColTrains", "Trains"), Widths[2]},
		{LOCTEXT("ColBelow", "Below target"), Widths[3]}});
	int32 Shown = 0;
	for (const FSTMonitorRow& Row : View.Items)
	{
		if (sProblemsOnly && Row.Severity == 0)
		{
			continue;
		}
		++Shown;
		AddRow(Row, true, Widths, 0.f, 150.f);
	}
	if (Shown == 0)
	{
		AddNote(sProblemsOnly ? LOCTEXT("NoItemProblems", "No item needs a look.") : LOCTEXT("NoItems", "No supply or demand platforms yet."), MonitorFine);
	}
}

void USTMonitorWindow::ShowStations(const FSTMonitorView& View)
{
	AddCell(AddLine(FMargin(0.f, 0.f, 0.f, 8.f)), MakeCell(TEXT("Stations with platform roles. Docked: share of the time a train is docked there. Late: "
		"average arrival against the plan."), 10, MonitorDim, true), 0.f);
	const TArray<float> Widths = {110.f, 90.f, 100.f};
	AddHeader({{LOCTEXT("ColStation", "Station"), 0.f}, {LOCTEXT("ColStatus", "Status"), 150.f}, {LOCTEXT("ColDocked", "Docked"), Widths[0]},
		{LOCTEXT("ColArrivals", "Arrivals"), Widths[1]}, {LOCTEXT("ColLate", "Late"), Widths[2]}});
	for (const FSTMonitorRow& Row : View.Stations)
	{
		AddRow(Row, false, Widths, 0.f, 150.f);
	}
	if (View.Stations.Num() == 0)
	{
		AddNote(LOCTEXT("NoStations", "No samples yet."), MonitorDim);
	}
}

void USTMonitorWindow::ShowPlatforms(const FSTMonitorView& View)
{
	AddCell(AddLine(FMargin(0.f, 0.f, 0.f, 8.f)), MakeCell(TEXT("Demand platforms empty half the time or more, and supply platforms full half the time or more."),
		10, MonitorDim, true), 0.f);
	const TArray<float> Widths = {220.f, 100.f};
	AddHeader({{FText::GetEmpty(), MonitorIconSize}, {LOCTEXT("ColStation", "Station"), 0.f}, {LOCTEXT("ColStatus", "Status"), 150.f},
		{LOCTEXT("ColItem", "Item"), Widths[0]}, {LOCTEXT("ColShare", "Of the time"), Widths[1]}});
	for (const FSTMonitorRow& Row : View.Platforms)
	{
		AddRow(Row, true, Widths, 0.f, 150.f);
	}
	if (View.Platforms.Num() == 0)
	{
		AddNote(LOCTEXT("NoPlatformProblems", "No platform is starved or backing up."), MonitorFine);
	}
}

void USTMonitorWindow::ShowTrains(const FSTMonitorView& View)
{
	if (View.Idle.Num() > 0)
	{
		AddHeading(LOCTEXT("IdleReasons", "Why idle trains wait"));
		AddDetails(View.Idle, 0.f);
		AddHeading(LOCTEXT("AllTrains", "Managed trains"));
	}
	const TArray<float> Widths = {260.f};
	AddHeader({{LOCTEXT("ColTrain", "Train"), 0.f}, {LOCTEXT("ColState", "State"), 150.f}, {LOCTEXT("ColWhere", "Last station"), Widths[0]}});
	for (const FSTMonitorRow& Row : View.Trains)
	{
		UHorizontalBox* Line = AddLine();
		AddCell(Line, MakeCell(Row.Title, 12, MonitorText, true), 0.f);
		AddCell(Line, MakeCell(Row.Verdict, 12, Row.Severity > 0 ? SeverityColor(Row.Severity) : MonitorText), 150.f);
		AddCell(Line, MakeCell(Row.Columns.Num() > 0 ? Row.Columns[0] : FString(), 11, MonitorText, true), Widths[0]);
		AddDetails(Row.Details, 0.f);
		AddDivider();
	}
	if (View.Trains.Num() == 0)
	{
		AddNote(LOCTEXT("NoTrains", "No managed trains. Switch Managed by SmartTrains on in a locomotive menu."), MonitorDim);
	}
}

#undef LOCTEXT_NAMESPACE
