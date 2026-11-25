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
#include "UI/STStyle.h"
#include "Engine/Texture2D.h"

#define LOCTEXT_NAMESPACE "SmartTrains"

TWeakObjectPtr<USTMonitorWindow> USTMonitorWindow::sOpen;
USTMonitorWindow::ETab USTMonitorWindow::sTab = USTMonitorWindow::ETab::Overview;

namespace
{
	constexpr float MonitorWidth = 1100.f;
	constexpr float MonitorMaxHeight = 820.f;
	constexpr float MonitorIconSize = 28.f;
	// Wait after the command before taking the input, so the closing chat doesn't take it back. [s]
	constexpr float MonitorOpenDelay = 0.2f;

	FString Signature(const FSTMonitorView& View)
	{
		FString Text = FString::Join(View.Summary, TEXT("|"));
		for (const TArray<FSTMonitorRow>* Rows : {&View.Tiles, &View.Consumers, &View.Suppliers, &View.Trains})
		{
			for (const FSTMonitorRow& Row : *Rows)
			{
				Text += Row.Title + Row.Verdict + Row.Advice + FString::Join(Row.Columns, TEXT("|")) + FString::Join(Row.Details, TEXT("|"))
					+ FString::Printf(TEXT("%d/%d/%d"), Row.Trains, Row.Target, Row.AtDepot);
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
	Dim->SetBrushColor(FLinearColor(0.f, 0.f, 0.f, 0.5f));
	Dim->SetVisibility(ESlateVisibility::Visible);
	if (UOverlaySlot* DimSlot = Screen->AddChildToOverlay(Dim))
	{
		DimSlot->SetHorizontalAlignment(HAlign_Fill);
		DimSlot->SetVerticalAlignment(VAlign_Fill);
	}
	UBorder* Frame = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Frame->SetBrush(STStyle::Outlined(STStyle::Panel, STStyle::Line, 1.f, 6.f));
	Frame->SetPadding(FMargin(18.f, 14.f));
	USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Size->SetWidthOverride(MonitorWidth);
	Size->SetMaxDesiredHeight(MonitorMaxHeight);
	Size->AddChild(Root);
	Frame->SetContent(Size);
	if (UOverlaySlot* FrameSlot = Screen->AddChildToOverlay(Frame))
	{
		FrameSlot->SetHorizontalAlignment(HAlign_Center);
		FrameSlot->SetVerticalAlignment(VAlign_Center);
	}
	return Screen;
}

void USTMonitorWindow::BuildContent(UVerticalBox* Root)
{
	UHorizontalBox* Title = MakeTitleBar(Root, LOCTEXT("MonitorHeading", "Monitor"), 30.f);
	mWindowText = MakeTinted(Title, LOCTEXT("MonitorWaiting", "Asking the server..."), 10, STStyle::Muted, false, false, FMargin(12.f, 0.f, 12.f, 0.f));
	SetVAlignCenter(mWindowText);
	USTButton* CloseButton = MakeButton(Title, LOCTEXT("Close", "Close"), [this]() { Close(); }, FMargin(0.f));
	CloseButton->SetFixedWidth(120.f);
	SetVAlignCenter(CloseButton);

	// Tiles, the same on every tab.
	mTiles = MakeHBox(Root, FMargin(0.f, 2.f, 0.f, 8.f));

	// Tab bar: a segment per tab.
	UHorizontalBox* Bar = MakeHBox(Root, FMargin(0.f, 0.f, 0.f, 8.f));
	const TArray<TPair<ETab, FText>> Tabs = {
		{ETab::Overview, LOCTEXT("TabOverview", "Overview")},
		{ETab::Consumers, LOCTEXT("TabConsumers", "Consumers")},
		{ETab::Suppliers, LOCTEXT("TabSuppliers", "Suppliers")},
		{ETab::Trains, LOCTEXT("TabTrains", "Trains")},
	};
	mTabs.Reset();
	for (const TPair<ETab, FText>& Tab : Tabs)
	{
		const ETab Which = Tab.Key;
		USTSegment* Segment = WidgetTree->ConstructWidget<USTSegment>(USTSegment::StaticClass());
		Segment->SetText(Tab.Value);
		Segment->SetFont(GetFont(12, true));
		Segment->OnClick = [this, Which]() { SelectTab(Which); };
		Bar->AddChildToHorizontalBox(Segment);
		SetFill(Segment);
		SetSlotPadding(Segment, FMargin(mTabs.Num() > 0 ? 4.f : 0.f, 0.f, 0.f, 0.f));
		mTabs.Add(Segment);
	}

	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	mContent = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Scroll->AddChild(mContent);
	if (UVerticalBoxSlot* ScrollSlot = Root->AddChildToVerticalBox(Scroll))
	{
		ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	MakeTinted(mContent, LOCTEXT("MonitorLoading", "Asking the server..."), 11, STStyle::Muted);
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
		CountProblems(View.Consumers, 2) + CountProblems(View.Trains, 1),
		CountProblems(View.Consumers, 1),
		CountProblems(View.Suppliers, 1),
		CountProblems(View.Trains, 1),
	};
	const TArray<FText> Names = {
		LOCTEXT("TabOverview", "Overview"), LOCTEXT("TabConsumers", "Consumers"), LOCTEXT("TabSuppliers", "Suppliers"), LOCTEXT("TabTrains", "Trains")};
	for (int32 Index = 0; Index < mTabs.Num(); ++Index)
	{
		// Each tab counts its rows that need a look.
		mTabs[Index]->SetText(Counts[Index] > 0 && Index > 0 ? FText::Format(LOCTEXT("TabCount", "{0}  ·  {1}"), Names[Index], FText::AsNumber(Counts[Index])) : Names[Index]);
		mTabs[Index]->SetSelected(static_cast<int32>(sTab) == Index);
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
	mWindowText->SetText(LOCTEXT("MonitorWindow", "The loop plan as last worked out, updated every few seconds"));
	UpdateTabs(View);
	ShowTiles(View);
	ShowTab(View);
}

void USTMonitorWindow::ShowTiles(const FSTMonitorView& View)
{
	mTiles->ClearChildren();
	for (const FSTMonitorRow& Tile : View.Tiles)
	{
		UBorder* Box = nullptr;
		UVerticalBox* Inside = MakeCard(mTiles, Tile.Severity > 0 ? STStyle::Alert : STStyle::Header, FMargin(12.f, 8.f), FMargin(0.f), &Box);
		if (Tile.Severity > 0)
		{
			Box->SetBrush(STStyle::Outlined(STStyle::Alert, STStyle::OrangeDeep, 1.f, 4.f));
		}
		SetFill(Box);
		SetSlotPadding(Box, FMargin(mTiles->GetChildrenCount() > 1 ? 6.f : 0.f, 0.f, 0.f, 0.f));
		MakeTinted(Inside, FText::FromString(Tile.Verdict), 22, Tile.Severity > 0 ? STStyle::Orange : STStyle::Text, true, false);
		MakeTinted(Inside, FText::FromString(Tile.Title), 9, STStyle::Muted, false, true);
	}
}

void USTMonitorWindow::ShowTab(const FSTMonitorView& View)
{
	mContent->ClearChildren();
	switch (sTab)
	{
	case ETab::Consumers: ShowConsumers(View); break;
	case ETab::Suppliers: ShowSuppliers(View); break;
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
	return MakeTinted(nullptr, FText::FromString(Text), Size, Color, false, bWrap);
}

UWidget* USTMonitorWindow::MakeItemIcon(TSubclassOf<UFGItemDescriptor> Item, float Size)
{
	USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Box->SetWidthOverride(Size);
	Box->SetHeightOverride(Size);
	if (UTexture2D* Texture = Item ? UFGItemDescriptor::GetSmallIcon(Item) : nullptr)
	{
		UImage* Icon = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
		Icon->SetBrushFromTexture(Texture);
		Icon->SetToolTipText(UFGItemDescriptor::GetItemName(Item));
		Box->AddChild(Icon);
	}
	return Box;
}

void USTMonitorWindow::AddHeader(const TArray<TPair<FText, float>>& Columns)
{
	UHorizontalBox* Line = AddLine(FMargin(10.f, 0.f, 0.f, 4.f));
	for (const TPair<FText, float>& Column : Columns)
	{
		AddCell(Line, MakeTinted(nullptr, FText::FromString(Column.Key.ToString().ToUpper()), 9, STStyle::Muted, true, false), Column.Value);
	}
}

void USTMonitorWindow::AddDivider()
{
}

void USTMonitorWindow::AddHeading(const FText& Text)
{
	MakeLabel(mContent, Text, FMargin(0.f, 12.f, 0.f, 6.f));
}

void USTMonitorWindow::AddDetails(const TArray<FString>& Lines, float Indent)
{
	if (Lines.Num() == 0)
	{
		return;
	}
	UTextBlock* Text = MakeTinted(mContent, FText::GetEmpty(), 10, STStyle::Muted, false, true, FMargin(Indent, 0.f, 8.f, 4.f));
	SetLines(Text, Lines);
}

void USTMonitorWindow::AddNote(const FText& Text, const FLinearColor& Color)
{
	MakeTinted(mContent, Text, 11, Color, false, true, FMargin(0.f, 4.f));
}

UHorizontalBox* USTMonitorWindow::AddRowCard(uint8 Severity, const FString& Key, const TArray<FString>& Details)
{
	UBorder* Card = nullptr;
	UVerticalBox* Inside = MakeCard(mContent, Severity >= 2 ? STStyle::Alert : STStyle::Header, FMargin(10.f, 6.f), FMargin(0.f, 2.f), &Card);
	if (Severity > 0)
	{
		Card->SetBrush(STStyle::Outlined(Severity >= 2 ? STStyle::Alert : STStyle::Header, Severity >= 2 ? STStyle::OrangeDeep : STStyle::Line, 1.f, 4.f));
	}
	UHorizontalBox* Line = MakeHBox(Inside);
	const bool bOpen = mExpanded.Contains(Key);
	if (Details.Num() > 0)
	{
		// The details stay closed until asked for: a toggle at the row's end.
		UHorizontalBox* Tail = MakeHBox(nullptr);
		USTSegment* Toggle = WidgetTree->ConstructWidget<USTSegment>(USTSegment::StaticClass());
		Toggle->SetText(bOpen ? LOCTEXT("Less", "Less") : LOCTEXT("More", "More"));
		Toggle->SetFont(GetFont(10, true));
		Toggle->SetSelected(bOpen);
		Toggle->OnClick = [this, Key]()
		{
			if (mExpanded.Remove(Key) == 0)
			{
				mExpanded.Add(Key);
			}
			mShown.Reset();
			RefreshSoon();
		};
		Tail->AddChildToHorizontalBox(Toggle);
		if (bOpen)
		{
			UTextBlock* Text = MakeTinted(Inside, FText::GetEmpty(), 10, STStyle::Muted, false, true, FMargin(0.f, 6.f, 0.f, 0.f));
			SetLines(Text, Details);
		}
		// Added last by the caller's cells; kept here to place after them.
		mPendingToggle = Tail;
	}
	else
	{
		mPendingToggle = nullptr;
	}
	return Line;
}

void USTMonitorWindow::AddRow(const FSTMonitorRow& Row, bool bIcon, const TArray<float>& Widths, float TitleWidth, float VerdictWidth)
{
	TArray<FString> Lines = Row.Details;
	if (!Row.Advice.IsEmpty() && !Lines.Contains(Row.Advice))
	{
		Lines.Add(TEXT("What to do: ") + Row.Advice);
	}
	UHorizontalBox* Line = AddRowCard(Row.Severity, Row.Title, Lines);
	if (bIcon)
	{
		AddCell(Line, MakeItemIcon(Row.Item, MonitorIconSize), MonitorIconSize);
	}
	AddCell(Line, MakeTinted(nullptr, FText::FromString(Row.Title), 12, STStyle::Text, true, true), TitleWidth);
	AddCell(Line, MakeCell(Row.Verdict, 11, Row.Severity > 0 ? STStyle::SeverityColor(Row.Severity) : STStyle::Muted), VerdictWidth);
	for (int32 Index = 0; Index < Widths.Num(); ++Index)
	{
		AddCell(Line, MakeCell(Row.Columns.IsValidIndex(Index) ? Row.Columns[Index] : FString(), 11, STStyle::Text), Widths[Index]);
	}
	FinishRow(Line);
}

void USTMonitorWindow::FinishRow(UHorizontalBox* Line)
{
	if (mPendingToggle)
	{
		AddCell(Line, mPendingToggle, 64.f);
		mPendingToggle = nullptr;
	}
}

//~ Tabs

void USTMonitorWindow::ShowOverview(const FSTMonitorView& View)
{
	// What needs a look: unserved consumers and trains without a loop.
	AddHeading(LOCTEXT("NeedsALook", "What needs a look"));
	int32 Shown = 0;
	for (const TArray<FSTMonitorRow>* Rows : {&View.Consumers, &View.Trains})
	{
		for (const FSTMonitorRow& Row : *Rows)
		{
			if (Row.Severity < 1 || Shown >= 12)
			{
				continue;
			}
			++Shown;
			UHorizontalBox* Line = AddRowCard(Row.Severity, TEXT("look:") + Row.Title, TArray<FString>());
			AddCell(Line, MakeTinted(nullptr, FText::FromString(Row.Title), 12, STStyle::Text, true, true), 300.f);
			AddCell(Line, MakeCell(Row.Verdict, 11, STStyle::SeverityColor(Row.Severity)), 120.f);
			AddCell(Line, MakeCell(!Row.Advice.IsEmpty() ? Row.Advice : (Row.Details.Num() > 0 ? Row.Details[0] : FString()), 10, STStyle::Muted, true), 0.f);
		}
	}
	if (Shown == 0)
	{
		AddNote(View.Consumers.Num() > 0 ? LOCTEXT("AllServed", "Every consumer has trains on its loop.")
			: LOCTEXT("NoConsumersYet", "No consumers yet. Give a station's unloading platforms the Demand role in their menus, or use /st migrate."), STStyle::Good);
	}
}

void USTMonitorWindow::ShowConsumers(const FSTMonitorView& View)
{
	const TArray<float> Widths = {80.f, 80.f, 60.f};
	AddHeader({{LOCTEXT("ColConsumer", "Consumer"), 0.f}, {LOCTEXT("ColPattern", "Pattern"), 150.f}, {LOCTEXT("ColTrains", "Trains"), 170.f},
		{LOCTEXT("ColRate", "Car loads/min"), Widths[0]}, {LOCTEXT("ColRoundTrip", "Round trip"), Widths[1]}, {LOCTEXT("ColShare", "Share"), Widths[2]},
		{FText::GetEmpty(), 64.f}});
	for (const FSTMonitorRow& Row : View.Consumers)
	{
		TArray<FString> Lines = Row.Details;
		UHorizontalBox* Line = AddRowCard(Row.Severity, Row.Title, Lines);
		UVerticalBox* Name = MakeVBox(nullptr);
		MakeTinted(Name, FText::FromString(Row.Title), 12, STStyle::Text, true, true);
		MakeTinted(Name, FText::FromString(Row.Verdict), 10, Row.Severity > 0 ? STStyle::SeverityColor(Row.Severity) : STStyle::Muted, false, false);
		AddCell(Line, Name, 0.f);
		// The pattern: one icon per car.
		UHorizontalBox* Pattern = MakeHBox(nullptr);
		for (const TSubclassOf<UFGItemDescriptor>& Item : Row.Pattern)
		{
			Pattern->AddChildToHorizontalBox(MakeItemIcon(Item, 24.f))->SetPadding(FMargin(0.f, 0.f, 3.f, 0.f));
		}
		AddCell(Line, Pattern, 150.f);
		// Trains against its share: a pip per train, filled for those on the loop, hollow for the share it lacks.
		UVerticalBox* Trains = MakeVBox(nullptr);
		UHorizontalBox* Pips = MakeHBox(Trains);
		const int32 Count = FMath::Min(FMath::Max(Row.Trains, Row.Target), 10);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const bool bHas = Index < Row.Trains;
			const bool bDepot = bHas && Index >= Row.Trains - Row.AtDepot;
			UBorder* Pip = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
			Pip->SetBrush(bHas ? STStyle::Rounded(bDepot ? STStyle::Muted : STStyle::Orange, 3.f) : STStyle::Outlined(FLinearColor::Transparent, STStyle::Muted, 1.f, 3.f));
			USizeBox* PipSize = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
			PipSize->SetWidthOverride(13.f);
			PipSize->SetHeightOverride(8.f);
			Pip->SetContent(PipSize);
			Pips->AddChildToHorizontalBox(Pip)->SetPadding(FMargin(0.f, 0.f, 3.f, 0.f));
		}
		MakeTinted(Trains, FText::FromString(Row.AtDepot > 0 ? FString::Printf(TEXT("%d of %d · %d at depot"), Row.Trains, Row.Target, Row.AtDepot)
			: FString::Printf(TEXT("%d of %d"), Row.Trains, Row.Target)), 9, STStyle::Muted, false, false, FMargin(0.f, 3.f, 0.f, 0.f));
		AddCell(Line, Trains, 170.f);
		for (int32 Index = 0; Index < Widths.Num(); ++Index)
		{
			AddCell(Line, MakeCell(Row.Columns.IsValidIndex(Index) ? Row.Columns[Index] : FString(), 11, STStyle::Text), Widths[Index]);
		}
		FinishRow(Line);
	}
	if (View.Consumers.Num() == 0)
	{
		AddNote(LOCTEXT("NoConsumers", "No consumers yet: give a station's unloading platforms the Demand role."), STStyle::Muted);
	}
}

void USTMonitorWindow::ShowSuppliers(const FSTMonitorView& View)
{
	const TArray<float> Widths = {200.f, 90.f, 90.f};
	AddHeader({{FText::GetEmpty(), MonitorIconSize}, {LOCTEXT("ColSupplier", "Supplier"), 0.f}, {LOCTEXT("ColStatus", "Status"), 120.f},
		{LOCTEXT("ColItem", "Item"), Widths[0]}, {LOCTEXT("ColMakes", "Makes"), Widths[1]}, {LOCTEXT("ColTaken", "Taken"), Widths[2]}, {FText::GetEmpty(), 64.f}});
	for (const FSTMonitorRow& Row : View.Suppliers)
	{
		AddRow(Row, true, Widths, 0.f, 120.f);
	}
	if (View.Suppliers.Num() == 0)
	{
		AddNote(LOCTEXT("NoSuppliers", "No suppliers yet: give a station's loading platforms the Supply role."), STStyle::Muted);
	}
}

void USTMonitorWindow::ShowTrains(const FSTMonitorView& View)
{
	AddHeader({{LOCTEXT("ColTrain", "Train"), 0.f}, {LOCTEXT("ColState", "State"), 150.f}, {LOCTEXT("ColLoop", "Its consumer"), 260.f}, {FText::GetEmpty(), 64.f}});
	for (const FSTMonitorRow& Row : View.Trains)
	{
		UHorizontalBox* Line = AddRowCard(Row.Severity, TEXT("train:") + Row.Title, Row.Details);
		AddCell(Line, MakeTinted(nullptr, FText::FromString(Row.Title), 12, STStyle::Text, true, true), 0.f);
		AddCell(Line, MakeCell(Row.Verdict, 11, Row.Severity > 0 ? STStyle::SeverityColor(Row.Severity) : STStyle::Good), 150.f);
		AddCell(Line, MakeCell(Row.Columns.Num() > 0 ? Row.Columns[0] : FString(), 11, STStyle::Text, true), 260.f);
		FinishRow(Line);
	}
	if (View.Trains.Num() == 0)
	{
		AddNote(LOCTEXT("NoTrains", "No managed trains. Switch Managed on in a locomotive menu."), STStyle::Muted);
	}
}

#undef LOCTEXT_NAMESPACE
