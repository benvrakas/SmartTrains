#include "UI/STMigrationWindow.h"
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
#include "STTrace.h"
#include "TimerManager.h"
#include "UI/STControls.h"
#include "UI/STWidgetUtils.h"

#define LOCTEXT_NAMESPACE "SmartTrains"

TWeakObjectPtr<USTMigrationWindow> USTMigrationWindow::sOpen;
bool USTMigrationWindow::sRename = false;
bool USTMigrationWindow::sPlatforms = true;
USTMigrationWindow::ETab USTMigrationWindow::sTab = USTMigrationWindow::ETab::Stations;
USTMigrationWindow::EFilter USTMigrationWindow::sStationFilter = USTMigrationWindow::EFilter::All;
USTMigrationWindow::EFilter USTMigrationWindow::sTrainFilter = USTMigrationWindow::EFilter::All;

namespace
{
	constexpr float MigrationWidth = 1120.f;
	constexpr float MigrationMaxHeight = 860.f;
	constexpr float MigrationIconSize = 24.f;
	const FLinearColor MigrationText(0.92f, 0.92f, 0.92f);
	const FLinearColor MigrationDim(0.62f, 0.62f, 0.62f);
	const FLinearColor MigrationFlag(1.f, 0.55f, 0.22f);
	const FLinearColor MigrationCheck(1.f, 0.84f, 0.32f);
	const FLinearColor MigrationReady(0.5f, 0.85f, 0.5f);
	const FLinearColor MigrationSupply(0.5f, 0.85f, 0.5f);
	const FLinearColor MigrationDemand(0.45f, 0.72f, 1.f);
	// The game's orange, for the selected tab.
	const FLinearColor MigrationAccent(0.98f, 0.58f, 0.15f);
	const FLinearColor MigrationDivider(1.f, 1.f, 1.f, 0.08f);
	// Stations table columns: tick, new name, platforms, status (the station name fills the rest). [px]
	constexpr float TickWidth = 44.f;
	constexpr float NewNameWidth = 330.f;
	constexpr float PlatformsWidth = 170.f;
	constexpr float StatusWidth = 120.f;
	// Wait after the command before taking the input, so the closing chat doesn't take it back. [s]
	constexpr float MigrationOpenDelay = 0.2f;

	FString RowKey(const FSTMigrationRow& Row)
	{
		return FString::Printf(TEXT("%s%d"), Row.bStation ? TEXT("S") : TEXT("T"), Row.Number);
	}

	// A platform whose items the preview isn't sure of.
	bool PlatformNeedsLook(const FSTMigrationPlatformRow& Platform)
	{
		return !Platform.Conflict.IsEmpty()
			|| (Platform.Role != ESTRole::Off && (Platform.Items.Num() == 0 || Platform.Confidence == TEXT("low") || Platform.Confidence == TEXT("unclear")));
	}

	// Why a station needs a look before applying, or empty.
	FString StationNeedsLook(const FSTMigrationRow& Row)
	{
		if (!Row.Flag.IsEmpty())
		{
			return Row.Flag;
		}
		for (const FSTMigrationPlatformRow& Platform : Row.Platforms)
		{
			if (PlatformNeedsLook(Platform))
			{
				return FString::Printf(TEXT("platform %d: %s"), Platform.Number,
					!Platform.Conflict.IsEmpty() ? *Platform.Conflict : (Platform.Items.Num() == 0 ? TEXT("no item") : *FString::Printf(TEXT("%s confidence"), *Platform.Confidence)));
			}
		}
		return FString();
	}

	FLinearColor RoleColor(ESTRole Role)
	{
		return Role == ESTRole::Supply ? MigrationSupply : (Role == ESTRole::Demand ? MigrationDemand : MigrationDim);
	}

	FLinearColor ConfidenceColor(const FString& Confidence)
	{
		return Confidence == TEXT("high") || Confidence == TEXT("set up") ? MigrationReady : (Confidence == TEXT("medium") ? MigrationCheck : MigrationFlag);
	}

	FString RoleCounts(const FSTMigrationRow& Row)
	{
		int32 Supply = 0;
		int32 Demand = 0;
		int32 Off = 0;
		int32 Empty = 0;
		for (const FSTMigrationPlatformRow& Platform : Row.Platforms)
		{
			Empty += Platform.bCargo ? 0 : 1;
			Supply += Platform.bCargo && Platform.Role == ESTRole::Supply ? 1 : 0;
			Demand += Platform.bCargo && Platform.Role == ESTRole::Demand ? 1 : 0;
			Off += Platform.bCargo && Platform.Role == ESTRole::Off ? 1 : 0;
		}
		TArray<FString> Parts;
		if (Supply > 0)
		{
			Parts.Add(FString::Printf(TEXT("%d supply"), Supply));
		}
		if (Demand > 0)
		{
			Parts.Add(FString::Printf(TEXT("%d demand"), Demand));
		}
		if (Off > 0)
		{
			Parts.Add(FString::Printf(TEXT("%d off"), Off));
		}
		if (Empty > 0)
		{
			Parts.Add(FString::Printf(TEXT("%d empty"), Empty));
		}
		return FString::Join(Parts, TEXT(", "));
	}
}

//~ Opening and closing

void USTMigrationWindow::Open(APlayerController* Controller)
{
	if (!Controller || !Controller->IsLocalController() || IsRunningDedicatedServer())
	{
		return;
	}
	if (USTMigrationWindow* Existing = sOpen.Get(); Existing && Existing->IsInViewport())
	{
		UE_LOG(LogSmartTrains, Log, TEXT("Migration window already open; refreshing"));
		Existing->SendMigration(static_cast<uint8>(ESTMigrationStep::Refresh));
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
	}), MigrationOpenDelay, false);
}

void USTMigrationWindow::Create(APlayerController* Controller)
{
	if (sOpen.IsValid() && sOpen->IsInViewport())
	{
		return;
	}
	USTMigrationWindow* Window = CreateWidget<USTMigrationWindow>(Controller, USTMigrationWindow::StaticClass());
	if (!Window)
	{
		UE_LOG(LogSmartTrains, Error, TEXT("Migration window could not be created"));
		return;
	}
	Window->mController = Controller;
	Window->SetIsFocusable(true);
	Window->AddToViewport(100);
	sOpen = Window;
	UWidgetBlueprintLibrary::SetInputMode_UIOnlyEx(Controller, Window, EMouseLockMode::DoNotLock);
	Controller->SetShowMouseCursor(true);
	Window->bInputTaken = true;
	UE_LOG(LogSmartTrains, Log, TEXT("Migration window opened"));
}

void USTMigrationWindow::Close()
{
	UE_LOG(LogSmartTrains, Log, TEXT("Migration window closed"));
	RemoveFromParent();
}

void USTMigrationWindow::RestoreGameInput()
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

bool USTMigrationWindow::OnShown()
{
	bApplyArmed = false;
	mShownSerial = -1;
	SendMigration(static_cast<uint8>(ESTMigrationStep::Refresh));
	return true;
}

void USTMigrationWindow::OnHidden()
{
	RestoreGameInput();
	if (sOpen.Get() == this)
	{
		sOpen.Reset();
	}
}

FReply USTMigrationWindow::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		Close();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

//~ Frame, toolbar and tabs

UWidget* USTMigrationWindow::WrapRoot(UVerticalBox* Root)
{
	UOverlay* Screen = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
	// Dims the game and catches clicks beside the window, so the window keeps the keyboard focus.
	UBorder* Dim = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Dim->SetBrushColor(FLinearColor(0.f, 0.f, 0.f, 0.55f));
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
	Size->SetWidthOverride(MigrationWidth);
	Size->SetMaxDesiredHeight(MigrationMaxHeight);
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

void USTMigrationWindow::BuildContent(UVerticalBox* Root)
{
	// Title and close.
	UHorizontalBox* Title = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Root->AddChildToVerticalBox(Title);
	if (UHorizontalBoxSlot* TitleSlot = Title->AddChildToHorizontalBox(MakeText(nullptr, LOCTEXT("MigrationHeading", "SmartTrains migration"), 18, false)))
	{
		TitleSlot->SetVerticalAlignment(VAlign_Center);
		TitleSlot->SetPadding(FMargin(0.f, 0.f, 16.f, 0.f));
	}
	mStatus = MakeText(nullptr, LOCTEXT("MigrationNoPreview", "Press Preview to see what would change."), 10, true);
	mStatus->SetColorAndOpacity(FSlateColor(MigrationDim));
	if (UHorizontalBoxSlot* StatusSlot = Title->AddChildToHorizontalBox(mStatus))
	{
		StatusSlot->SetVerticalAlignment(VAlign_Center);
		StatusSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
	USTButton* CloseButton = MakeButton(nullptr, LOCTEXT("Close", "Close"), [this]() { Close(); });
	CloseButton->SetFixedWidth(140.f);
	if (UHorizontalBoxSlot* CloseSlot = Title->AddChildToHorizontalBox(CloseButton))
	{
		CloseSlot->SetVerticalAlignment(VAlign_Center);
	}

	// Toolbar: the steps fill the row; Rename stations and the beta tools keep a fixed width.
	UHorizontalBox* Toolbar = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Root->AddChildToVerticalBox(Toolbar)->SetPadding(FMargin(0.f, 10.f, 0.f, 4.f));
	auto AddTool = [&](UHorizontalBox* Box, UWidget* Widget, float Width)
	{
		if (UHorizontalBoxSlot* ToolSlot = Box->AddChildToHorizontalBox(Widget))
		{
			ToolSlot->SetSize(FSlateChildSize(Width > 0.f ? ESlateSizeRule::Automatic : ESlateSizeRule::Fill));
			ToolSlot->SetPadding(FMargin(0.f, 0.f, 6.f, 0.f));
			ToolSlot->SetVerticalAlignment(VAlign_Center);
		}
	};
	AddTool(Toolbar, MakeButton(nullptr, LOCTEXT("Preview", "1  Preview"), [this]() { SendPreview(); }, FMargin(0.f)), 0.f);
	mApply = MakeButton(nullptr, LOCTEXT("Apply", "2  Apply ticked"), [this]() { ApplyTicked(); }, FMargin(0.f));
	AddTool(Toolbar, mApply, 0.f);
	mUndo = MakeButton(nullptr, LOCTEXT("Undo", "Undo last apply"), [this]() { SendMigration(static_cast<uint8>(ESTMigrationStep::Undo)); }, FMargin(0.f));
	AddTool(Toolbar, mUndo, 0.f);
	mRenameMode = MakeButton(nullptr, FText::GetEmpty(), [this]()
	{
		// Platforms only -> names and platforms -> names only -> platforms only.
		if (!sRename)
		{
			sRename = true;
		}
		else if (sPlatforms)
		{
			sPlatforms = false;
		}
		else
		{
			sRename = false;
			sPlatforms = true;
		}
		ShowRenameMode();
		ShowTab();
	}, FMargin(0.f));
	mRenameMode->SetFixedWidth(290.f);
	AddTool(Toolbar, mRenameMode, 290.f);
	ShowRenameMode();
	UHorizontalBox* Beta = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	USTButton* TestCopy = MakeButton(nullptr, LOCTEXT("TestCopy", "Test copy"), [this]() { SendMigration(static_cast<uint8>(ESTMigrationStep::TestCopy)); }, FMargin(0.f));
	TestCopy->SetFixedWidth(130.f);
	AddTool(Beta, TestCopy, 130.f);
	USTButton* Backup = MakeButton(nullptr, LOCTEXT("Backup", "Backup"), [this]() { SendMigration(static_cast<uint8>(ESTMigrationStep::Backup)); }, FMargin(0.f));
	Backup->SetFixedWidth(130.f);
	AddTool(Beta, Backup, 130.f);
	AddTool(Toolbar, Beta, 1.f);
	mBetaRow = Beta;

	// Tab bar: a button per tab with a line under the selected one.
	UHorizontalBox* Bar = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Root->AddChildToVerticalBox(Bar)->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
	mTabs.Reset();
	for (const ETab Tab : {ETab::Stations, ETab::Trains, ETab::Result})
	{
		UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		USTButton* Button = MakeButton(nullptr, FText::GetEmpty(), [this, Tab]() { SelectTab(Tab); }, FMargin(0.f));
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
	Rule->SetBrushColor(MigrationDivider);
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
}

void USTMigrationWindow::ShowRenameMode()
{
	mRenameMode->SetText(!sRename ? LOCTEXT("ApplyPlatforms", "Apply: platforms only")
		: (sPlatforms ? LOCTEXT("ApplyBoth", "Apply: platforms and names") : LOCTEXT("ApplyNames", "Apply: names only")));
}

void USTMigrationWindow::SendPreview()
{
	// The server is still working on the last one: another click would queue a second preview behind it.
	if (mPreviewSentAt > 0.0 && FPlatformTime::Seconds() - mPreviewSentAt < 120.0)
	{
		mStatus->SetText(LOCTEXT("PreviewPending", "Preview requested; the server is still working it out."));
		return;
	}
	mPreviewSentAt = FPlatformTime::Seconds();
	mStatus->SetText(LOCTEXT("PreviewAsked", "Working out the preview..."));
	SendMigration(static_cast<uint8>(ESTMigrationStep::Preview), TArray<FString>());
}

void USTMigrationWindow::SendMigration(uint8 Step, const TArray<FString>& Selection)
{
	if (USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this))
	{
		UE_LOG(LogSmartTrains, Log, TEXT("Migration window: step %d [%s]"), Step, *FString::Join(Selection, TEXT(" ")));
		RCO->Server_Migrate(static_cast<ESTMigrationStep>(Step), Selection);
	}
}

void USTMigrationWindow::ApplyTicked()
{
	const USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this);
	if (!RCO)
	{
		return;
	}
	const FSTMigrationView& View = RCO->GetMigrationView();
	if (View.bNeedsConfirm && !bApplyArmed)
	{
		bApplyArmed = true;
		mApply->SetText(LOCTEXT("ApplyArmed", "No test copy or backup yet: click again to apply here"));
		return;
	}
	FString Stations;
	FString Trains;
	for (const FSTMigrationRow& Row : View.Rows)
	{
		if (const bool* Ticked = mTicked.Find(RowKey(Row)); Ticked && *Ticked)
		{
			FString& List = Row.bStation ? Stations : Trains;
			List += (List.IsEmpty() ? TEXT("") : TEXT(",")) + FString::FromInt(Row.Number);
		}
	}
	TArray<FString> Selection;
	if (!Stations.IsEmpty())
	{
		Selection.Append({TEXT("stations"), Stations});
	}
	if (!Trains.IsEmpty())
	{
		Selection.Append({TEXT("trains"), Trains});
	}
	if (Selection.Num() == 0)
	{
		mStatus->SetText(LOCTEXT("NothingTicked", "Nothing is ticked."));
		return;
	}
	if (bApplyArmed)
	{
		Selection.Add(TEXT("confirm"));
	}
	if (sRename)
	{
		Selection.Add(sPlatforms ? TEXT("rename") : TEXT("namesonly"));
	}
	bApplyArmed = false;
	sTab = ETab::Result;
	SendMigration(static_cast<uint8>(ESTMigrationStep::Apply), Selection);
}

void USTMigrationWindow::SelectTab(ETab Tab)
{
	sTab = Tab;
	UE_LOG(LogSmartTrains, Verbose, TEXT("Migration window: tab %d"), static_cast<int32>(Tab));
	ShowTab();
}

void USTMigrationWindow::UpdateTabs(const FSTMigrationView& View)
{
	int32 Stations = 0;
	int32 NeedsLook = 0;
	int32 Trains = 0;
	int32 TrainsTicked = 0;
	for (const FSTMigrationRow& Row : View.Rows)
	{
		if (Row.bStation)
		{
			++Stations;
			NeedsLook += StationNeedsLook(Row).IsEmpty() ? 0 : 1;
		}
		else
		{
			++Trains;
			TrainsTicked += mTicked.FindRef(RowKey(Row)) ? 1 : 0;
		}
	}
	const TArray<FText> Names = {
		Stations > 0 ? FText::Format(LOCTEXT("TabStationsCount", "Stations ({0}, {1} to check)"), FText::AsNumber(Stations), FText::AsNumber(NeedsLook)) : LOCTEXT("TabStations", "Stations"),
		Trains > 0 ? FText::Format(LOCTEXT("TabTrainsCount", "Trains ({0} of {1} ticked)"), FText::AsNumber(TrainsTicked), FText::AsNumber(Trains)) : LOCTEXT("TabTrains", "Trains"),
		LOCTEXT("TabResult", "Result")};
	for (int32 Index = 0; Index < mTabs.Num(); ++Index)
	{
		if (USTButton* Button = mTabs[Index].Button.Get())
		{
			Button->SetText(Names[Index]);
		}
		if (UBorder* Underline = mTabs[Index].Underline.Get())
		{
			Underline->SetBrushColor(static_cast<int32>(sTab) == Index ? MigrationAccent : FLinearColor::Transparent);
		}
	}
}

//~ Data

void USTMigrationWindow::Refresh(USTRemoteCallObject* RCO)
{
	const FSTMigrationView& View = RCO->GetMigrationView();
	mUndo->SetIsEnabled(View.bCanUndo);
	mApply->SetIsEnabled(View.bHasPreview);
	if (!bApplyArmed)
	{
		mApply->SetText(LOCTEXT("Apply", "2  Apply ticked"));
	}
	mBetaRow->SetVisibility(View.bBetaTools ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	if (View.Serial == mShownSerial)
	{
		return;
	}
	mShownSerial = View.Serial;
	mPreviewSentAt = 0.0;

	// A fresh preview starts from its proposals and opens the stations that need a look; after other steps the
	// player's ticks on the same rows are kept.
	const bool bFreshPreview = View.Lines.Num() > 0 && View.Lines[0].StartsWith(TEXT("Migration preview"));
	TMap<FString, bool> Ticks;
	for (const FSTMigrationRow& Row : View.Rows)
	{
		const FString Key = RowKey(Row);
		const bool* Previous = bFreshPreview ? nullptr : mTicked.Find(Key);
		Ticks.Add(Key, Previous ? *Previous : Row.bProposed);
	}
	mTicked = MoveTemp(Ticks);
	if (bFreshPreview)
	{
		mExpanded.Reset();
		sTab = ETab::Stations;
	}
	mStatus->SetText(View.bHasPreview ? FText::FromString(View.Lines.Num() > 0 ? View.Lines[0] : FString(TEXT("Preview ready.")))
		: LOCTEXT("MigrationNoPreview", "Press Preview to see what would change."));
	ShowTab();
	UE_LOG(LogSmartTrains, Verbose, TEXT("Migration window: %d rows"), View.Rows.Num());
}

void USTMigrationWindow::ShowTab()
{
	const USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this);
	if (!RCO || !mContent)
	{
		return;
	}
	const FSTMigrationView& View = RCO->GetMigrationView();
	UpdateTabs(View);
	mContent->ClearChildren();
	switch (sTab)
	{
	case ETab::Trains: ShowTrains(View); break;
	case ETab::Result: ShowResult(View); break;
	default: ShowStations(View); break;
	}
}

bool USTMigrationWindow::Passes(const FSTMigrationRow& Row, EFilter Filter) const
{
	switch (Filter)
	{
	case EFilter::NeedsLook: return !StationNeedsLook(Row).IsEmpty();
	case EFilter::Ticked: return mTicked.FindRef(RowKey(Row));
	case EFilter::NotTicked: return !mTicked.FindRef(RowKey(Row));
	default: return true;
	}
}

void USTMigrationWindow::SetAllTicks(bool bStations, bool bTicked)
{
	const USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this);
	if (!RCO)
	{
		return;
	}
	const EFilter Filter = bStations ? sStationFilter : sTrainFilter;
	int32 Changed = 0;
	for (const FSTMigrationRow& Row : RCO->GetMigrationView().Rows)
	{
		if (Row.bStation == bStations && Passes(Row, Filter))
		{
			mTicked.Add(RowKey(Row), bTicked);
			++Changed;
		}
	}
	UE_LOG(LogSmartTrains, Log, TEXT("Migration window: %s %d %s"), bTicked ? TEXT("ticked") : TEXT("unticked"), Changed, bStations ? TEXT("stations") : TEXT("trains"));
	ShowTab();
}

//~ Table pieces

UHorizontalBox* USTMigrationWindow::AddLine(const FMargin& Spacing)
{
	UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	mContent->AddChildToVerticalBox(Line)->SetPadding(Spacing);
	return Line;
}

void USTMigrationWindow::AddCell(UHorizontalBox* Line, UWidget* Cell, float Width)
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
		CellSlot->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
		if (Width <= 0.f)
		{
			CellSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}
	}
}

UTextBlock* USTMigrationWindow::MakeCell(const FString& Text, int32 Size, const FLinearColor& Color, bool bWrap)
{
	UTextBlock* Cell = MakeText(nullptr, FText::FromString(Text), Size, bWrap);
	Cell->SetColorAndOpacity(FSlateColor(Color));
	return Cell;
}

UWidget* USTMigrationWindow::MakeIcon(TSubclassOf<UFGItemDescriptor> Item, float Size)
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

void USTMigrationWindow::AddDivider()
{
	UBorder* Line = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Line->SetBrushColor(MigrationDivider);
	USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Box->SetHeightOverride(1.f);
	Line->SetContent(Box);
	mContent->AddChildToVerticalBox(Line)->SetPadding(FMargin(0.f, 3.f));
}

void USTMigrationWindow::AddHeading(const FText& Text)
{
	MakeText(mContent, Text, 15, false, FMargin(0.f, 14.f, 0.f, 6.f));
}

void USTMigrationWindow::AddNote(const FString& Text, const FLinearColor& Color, float Indent)
{
	UTextBlock* Note = MakeText(mContent, FText::FromString(Text), 10, true, FMargin(Indent, 0.f, 8.f, 2.f));
	Note->SetColorAndOpacity(FSlateColor(Color));
}

//~ Tabs

void USTMigrationWindow::AddFilterBar(bool bStations, int32 Shown, int32 Total)
{
	UHorizontalBox* Bar = AddLine(FMargin(0.f, 0.f, 0.f, 8.f));
	EFilter& Filter = bStations ? sStationFilter : sTrainFilter;
	const TArray<TPair<EFilter, FText>> Filters = bStations
		? TArray<TPair<EFilter, FText>>{{EFilter::All, LOCTEXT("FilterAll", "All")}, {EFilter::NeedsLook, LOCTEXT("FilterCheck", "To check")},
			{EFilter::Ticked, LOCTEXT("FilterTicked", "Ticked")}, {EFilter::NotTicked, LOCTEXT("FilterNotTicked", "Not ticked")}}
		: TArray<TPair<EFilter, FText>>{{EFilter::All, LOCTEXT("FilterAll", "All")}, {EFilter::Ticked, LOCTEXT("FilterTicked", "Ticked")},
			{EFilter::NotTicked, LOCTEXT("FilterNotTicked", "Not ticked")}};
	for (const TPair<EFilter, FText>& Each : Filters)
	{
		const EFilter Which = Each.Key;
		const bool bOn = Filter == Which;
		USTButton* Button = MakeButton(nullptr, bOn ? FText::Format(LOCTEXT("FilterOn", "[ {0} ]"), Each.Value) : Each.Value, [this, bStations, Which]()
		{
			(bStations ? sStationFilter : sTrainFilter) = Which;
			ShowTab();
		}, FMargin(0.f));
		Button->SetFixedWidth(130.f);
		AddCell(Bar, Button, 130.f);
	}
	AddCell(Bar, MakeCell(FString::Printf(TEXT("%d of %d shown"), Shown, Total), 10, MigrationDim), 0.f);
	for (const bool bTick : {true, false})
	{
		USTButton* Button = MakeButton(nullptr, bTick ? LOCTEXT("TickShown", "Tick shown") : LOCTEXT("UntickShown", "Untick shown"), [this, bStations, bTick]()
		{
			SetAllTicks(bStations, bTick);
		}, FMargin(0.f));
		Button->SetFixedWidth(130.f);
		AddCell(Bar, Button, 130.f);
	}
}

void USTMigrationWindow::ShowStations(const FSTMigrationView& View)
{
	if (!View.bHasPreview)
	{
		AddHeading(LOCTEXT("HowHeading", "How it works"));
		AddNote(TEXT("1. Preview follows every platform's belts and pipes to the machines behind them and proposes a role and item for each "
			"platform, which trains to manage, and (with Rename stations on) a name for each station from its factory, area and rate."), MigrationText);
		AddNote(TEXT("2. Check the stations marked Check (filter \"To check\"), untick what you don't want, then Apply ticked."), MigrationText);
		AddNote(TEXT("3. Undo puts back everything the last apply changed: platforms, station names and managed trains."), MigrationText);
		return;
	}
	int32 Total = 0;
	int32 Shown = 0;
	for (const FSTMigrationRow& Row : View.Rows)
	{
		Total += Row.bStation ? 1 : 0;
		Shown += Row.bStation && Passes(Row, sStationFilter) ? 1 : 0;
	}
	AddFilterBar(true, Shown, Total);
	AddStationHeadings();
	for (const FSTMigrationRow& Row : View.Rows)
	{
		if (Row.bStation && Passes(Row, sStationFilter))
		{
			AddStation(Row);
		}
	}
}

void USTMigrationWindow::AddStationHeadings()
{
	UHorizontalBox* Line = AddLine(FMargin(0.f, 0.f, 0.f, 2.f));
	AddCell(Line, MakeCell(TEXT(""), 10, MigrationDim), TickWidth);
	AddCell(Line, MakeCell(TEXT("Station (click to see its platforms)"), 10, MigrationDim), 0.f);
	if (sRename)
	{
		AddCell(Line, MakeCell(TEXT("New name"), 10, MigrationDim), NewNameWidth);
	}
	AddCell(Line, MakeCell(TEXT("Platforms"), 10, MigrationDim), PlatformsWidth);
	AddCell(Line, MakeCell(TEXT("Status"), 10, MigrationDim), StatusWidth);
	AddDivider();
}

void USTMigrationWindow::AddStation(const FSTMigrationRow& Row)
{
	const FString Key = RowKey(Row);
	const bool bTicked = mTicked.FindRef(Key);
	const bool bOpen = mExpanded.Contains(Key);
	const FString Why = StationNeedsLook(Row);
	auto Toggle = [this, Key]()
	{
		if (!mExpanded.Remove(Key))
		{
			mExpanded.Add(Key);
		}
		ShowTab();
	};

	UHorizontalBox* Line = AddLine(FMargin(0.f, 4.f, 0.f, 2.f));
	USTRow* Tick = MakeRow(nullptr, FText::FromString(bTicked ? TEXT("[x]") : TEXT("[  ]")), [this, Key]()
	{
		bool& Ticked = mTicked.FindOrAdd(Key);
		Ticked = !Ticked;
		ShowTab();
	});
	Tick->SetStyle(GetFont(13), FSlateColor(bTicked ? MigrationReady : MigrationDim));
	AddCell(Line, Tick, TickWidth);
	USTRow* Title = MakeRow(nullptr, FText::FromString(FString::Printf(TEXT("%s  %s   %s"), bOpen ? TEXT("-") : TEXT("+"), *Key, *Row.Title)), Toggle);
	Title->SetStyle(GetFont(12), FSlateColor(MigrationText));
	AddCell(Line, Title, 0.f);
	if (sRename)
	{
		// The new name; "stays" when it keeps its name; "..." while the names are still being worked out.
		const bool bStays = Row.Details.ContainsByPredicate([](const FString& Detail) { return Detail.StartsWith(TEXT("Name stays")); });
		AddCell(Line, MakeCell(!Row.NewName.IsEmpty() ? Row.NewName : (bStays ? TEXT("stays") : TEXT("...")), 11,
			!Row.NewName.IsEmpty() ? MigrationAccent : MigrationDim, true), NewNameWidth);
	}
	AddCell(Line, MakeCell(RoleCounts(Row), 10, MigrationDim), PlatformsWidth);
	AddCell(Line, MakeCell(!Row.Flag.IsEmpty() ? TEXT("Not ticked") : (Why.IsEmpty() ? TEXT("Ready") : TEXT("Check")), 11,
		!Row.Flag.IsEmpty() ? MigrationFlag : (Why.IsEmpty() ? MigrationReady : MigrationCheck)), StatusWidth);
	if (!Why.IsEmpty())
	{
		AddNote(Why, Row.Flag.IsEmpty() ? MigrationCheck : MigrationFlag, TickWidth + 8.f);
	}
	if (bOpen)
	{
		const float Indent = TickWidth + 24.f;
		for (const FSTMigrationPlatformRow& Platform : Row.Platforms)
		{
			UHorizontalBox* PlatformLine = AddLine(FMargin(Indent, 2.f, 0.f, 0.f));
			AddCell(PlatformLine, MakeCell(FString::Printf(TEXT("P%d"), Platform.Number), 11, MigrationDim), 40.f);
			if (!Platform.bCargo)
			{
				AddCell(PlatformLine, MakeCell(TEXT("Empty platform: no freight or fluid"), 11, MigrationDim), 0.f);
				continue;
			}
			AddCell(PlatformLine, MakeCell(STNames::Role(Platform.Role), 11, RoleColor(Platform.Role)), 80.f);
			UHorizontalBox* Items = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
			for (const TSubclassOf<UFGItemDescriptor>& Item : Platform.Items)
			{
				if (UHorizontalBoxSlot* IconSlot = Items->AddChildToHorizontalBox(MakeIcon(Item, MigrationIconSize)))
				{
					IconSlot->SetVerticalAlignment(VAlign_Center);
					IconSlot->SetPadding(FMargin(0.f, 0.f, 4.f, 0.f));
				}
				if (UHorizontalBoxSlot* NameSlot = Items->AddChildToHorizontalBox(MakeCell(STNames::Item(Item), 11, MigrationText)))
				{
					NameSlot->SetVerticalAlignment(VAlign_Center);
					NameSlot->SetPadding(FMargin(0.f, 0.f, 12.f, 0.f));
				}
			}
			if (Platform.Items.Num() == 0)
			{
				Items->AddChildToHorizontalBox(MakeCell(Platform.Role == ESTRole::Off ? TEXT("-") : TEXT("no item"), 11, Platform.Role == ESTRole::Off ? MigrationDim : MigrationFlag));
			}
			AddCell(PlatformLine, Items, 0.f);
			AddCell(PlatformLine, MakeCell(Platform.Confidence, 11, ConfidenceColor(Platform.Confidence)), 90.f);
			if (!Platform.Conflict.IsEmpty())
			{
				AddNote(TEXT("! ") + Platform.Conflict, MigrationFlag, Indent + 40.f);
			}
			if (!Platform.Note.IsEmpty())
			{
				AddNote(Platform.Note, MigrationDim, Indent + 40.f);
			}
		}
		if (!Row.Users.IsEmpty())
		{
			AddNote(TEXT("Used by: ") + Row.Users, MigrationDim, Indent);
		}
		AddNote(TEXT("Change a platform afterwards in its own menu."), MigrationDim, Indent);
	}
	AddDivider();
}

void USTMigrationWindow::ShowTrains(const FSTMigrationView& View)
{
	if (!View.bHasPreview)
	{
		AddNote(TEXT("Press Preview first."), MigrationDim);
		return;
	}
	int32 Total = 0;
	int32 Shown = 0;
	for (const FSTMigrationRow& Row : View.Rows)
	{
		Total += Row.bStation ? 0 : 1;
		Shown += !Row.bStation && Passes(Row, sTrainFilter) ? 1 : 0;
	}
	AddFilterBar(false, Shown, Total);
	AddNote(TEXT("Ticked trains become managed: SmartTrains writes their timetables. Unticked trains keep their own."), MigrationDim);
	for (const FSTMigrationRow& Row : View.Rows)
	{
		if (!Row.bStation && Passes(Row, sTrainFilter))
		{
			AddTrain(Row);
		}
	}
}

void USTMigrationWindow::AddTrain(const FSTMigrationRow& Row)
{
	const FString Key = RowKey(Row);
	const bool bTicked = mTicked.FindRef(Key);
	UHorizontalBox* Line = AddLine(FMargin(0.f, 4.f, 0.f, 2.f));
	USTRow* Tick = MakeRow(nullptr, FText::FromString(bTicked ? TEXT("[x]") : TEXT("[  ]")), [this, Key]()
	{
		bool& Ticked = mTicked.FindOrAdd(Key);
		Ticked = !Ticked;
		ShowTab();
	});
	Tick->SetStyle(GetFont(13), FSlateColor(bTicked ? MigrationReady : MigrationDim));
	AddCell(Line, Tick, 44.f);
	AddCell(Line, MakeCell(FString::Printf(TEXT("%s   %s"), *Key, *Row.Title), 12, MigrationText, true), 0.f);
	AddCell(Line, MakeCell(bTicked ? TEXT("Will be managed") : TEXT("Keeps its timetable"), 11, bTicked ? MigrationReady : MigrationDim), 170.f);
	if (!Row.Users.IsEmpty())
	{
		AddNote(Row.Users, MigrationDim, 52.f);
	}
	if (!Row.Note.IsEmpty())
	{
		AddNote(Row.Note, MigrationCheck, 52.f);
	}
	AddDivider();
}

void USTMigrationWindow::ShowResult(const FSTMigrationView& View)
{
	if (View.Lines.Num() == 0)
	{
		AddNote(TEXT("No step run yet in this session."), MigrationDim);
		return;
	}
	for (const FString& Line : View.Lines)
	{
		AddNote(Line, MigrationText);
	}
}

#undef LOCTEXT_NAMESPACE
