#include "UI/STMigrationWindow.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "SmartTrains.h"
#include "STRemoteCallObject.h"
#include "STTrace.h"
#include "TimerManager.h"
#include "UI/STControls.h"
#include "UI/STWidgetUtils.h"

#define LOCTEXT_NAMESPACE "SmartTrains"

TWeakObjectPtr<USTMigrationWindow> USTMigrationWindow::sOpen;

namespace
{
	constexpr float WindowWidth = 1040.f;
	constexpr float WindowMaxHeight = 860.f;
	// Colours of the secondary texts in the rows.
	const FLinearColor DetailColor(0.72f, 0.72f, 0.72f);
	const FLinearColor FlagColor(1.f, 0.62f, 0.25f);
	// Wait after the command before taking the input, so the closing chat doesn't take it back. [s]
	constexpr float OpenDelay = 0.2f;

	FString RowKey(const FSTMigrationRow& Row)
	{
		return FString::Printf(TEXT("%s%d"), Row.bStation ? TEXT("S") : TEXT("T"), Row.Number);
	}

	void AddFilled(UHorizontalBox* Box, UWidget* Child, const FMargin& Padding = FMargin(0.f, 0.f, 4.f, 0.f))
	{
		if (UHorizontalBoxSlot* Slot = Box->AddChildToHorizontalBox(Child))
		{
			Slot->SetPadding(Padding);
			Slot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}
	}
}

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
	}), OpenDelay, false);
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
	if (UWidget* Background = STWidgetUtils::CreateVanillaWidget(WidgetTree, STWidgetUtils::ContentBackgroundClass))
	{
		if (UOverlaySlot* BackgroundSlot = Frame->AddChildToOverlay(Background))
		{
			BackgroundSlot->SetHorizontalAlignment(HAlign_Fill);
			BackgroundSlot->SetVerticalAlignment(VAlign_Fill);
		}
	}
	else
	{
		UBorder* Plain = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Plain->SetBrushColor(FLinearColor(0.02f, 0.02f, 0.025f, 0.96f));
		if (UOverlaySlot* PlainSlot = Frame->AddChildToOverlay(Plain))
		{
			PlainSlot->SetHorizontalAlignment(HAlign_Fill);
			PlainSlot->SetVerticalAlignment(VAlign_Fill);
		}
	}
	USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Size->SetWidthOverride(WindowWidth);
	Size->SetMaxDesiredHeight(WindowMaxHeight);
	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	Scroll->AddChild(Root);
	Size->AddChild(Scroll);
	if (UOverlaySlot* ContentSlot = Frame->AddChildToOverlay(Size))
	{
		ContentSlot->SetPadding(FMargin(18.f, 14.f));
	}
	if (UOverlaySlot* FrameSlot = Screen->AddChildToOverlay(Frame))
	{
		FrameSlot->SetHorizontalAlignment(HAlign_Center);
		FrameSlot->SetVerticalAlignment(VAlign_Center);
	}
	return Screen;
}

int32 USTMigrationWindow::sTraceLimit = STTrace::DefaultLimit;
bool USTMigrationWindow::sAllItems = false;

void USTMigrationWindow::BuildContent(UVerticalBox* Root)
{
	UHorizontalBox* Title = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Root->AddChildToVerticalBox(Title);
	AddFilled(Title, MakeText(nullptr, LOCTEXT("MigrationHeading", "SmartTrains migration"), 18, false), FMargin(0.f, 0.f, 8.f, 0.f));
	USTButton* CloseButton = MakeButton(nullptr, LOCTEXT("Close", "Close"), [this]() { Close(); });
	CloseButton->SetFixedWidth(140.f);
	if (UHorizontalBoxSlot* CloseSlot = Title->AddChildToHorizontalBox(CloseButton))
	{
		CloseSlot->SetVerticalAlignment(VAlign_Center);
	}

	MakeText(Root, LOCTEXT("MigrationHint", "Preview reads every platform's belts and pipes back to the machines behind them (the connection trace), "
		"and uses the platform's stock and timetables only where the trace finds nothing. Untick what you don't want, then apply. Undo puts back the last apply."),
		10, true, FMargin(0.f, 4.f, 0.f, 6.f));

	UHorizontalBox* Steps = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Root->AddChildToVerticalBox(Steps)->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
	AddFilled(Steps, MakeButton(nullptr, LOCTEXT("Preview", "Preview"), [this]() { SendPreview(); }));
	mApply = MakeButton(nullptr, LOCTEXT("Apply", "Apply ticked"), [this]() { ApplyTicked(); });
	AddFilled(Steps, mApply);
	mUndo = MakeButton(nullptr, LOCTEXT("Undo", "Undo"), [this]() { SendMigration(static_cast<uint8>(ESTMigrationStep::Undo)); });
	AddFilled(Steps, mUndo, FMargin(0.f));

	UHorizontalBox* Limit = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Root->AddChildToVerticalBox(Limit)->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
	if (UHorizontalBoxSlot* LabelSlot = Limit->AddChildToHorizontalBox(MakeText(nullptr, LOCTEXT("TraceLimit", "Trace limit (buildings and belts per platform):"), 11, false)))
	{
		LabelSlot->SetVerticalAlignment(VAlign_Center);
		LabelSlot->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
	}
	mTraceLimit = MakeInput(nullptr, FText::AsNumber(STTrace::DefaultLimit));
	mTraceLimit->SetTextIfIdle(FString::FromInt(sTraceLimit));
	mTraceLimit->OnCommitted = [this](const FString& Text) { ReadTraceLimit(Text); };
	if (USizeBox* LimitBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass()))
	{
		LimitBox->SetWidthOverride(140.f);
		LimitBox->AddChild(mTraceLimit);
		if (UHorizontalBoxSlot* BoxSlot = Limit->AddChildToHorizontalBox(LimitBox))
		{
			BoxSlot->SetVerticalAlignment(VAlign_Center);
		}
	}
	if (UHorizontalBoxSlot* HintSlot = Limit->AddChildToHorizontalBox(MakeText(nullptr, FText::Format(LOCTEXT("TraceLimitHint", "{0} to {1}; raise it for big factories, then press Preview."),
		FText::AsNumber(STTrace::MinLimit), FText::AsNumber(STTrace::MaxLimit)), 10, false)))
	{
		HintSlot->SetVerticalAlignment(VAlign_Center);
		HintSlot->SetPadding(FMargin(8.f, 0.f, 0.f, 0.f));
	}

	mItemsMode = MakeButton(Root, FText::GetEmpty(), [this]()
	{
		sAllItems = !sAllItems;
		ShowItemsMode();
	}, FMargin(0.f, 0.f, 0.f, 4.f));
	ShowItemsMode();

	UHorizontalBox* Beta = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Root->AddChildToVerticalBox(Beta)->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
	AddFilled(Beta, MakeButton(nullptr, LOCTEXT("TestCopy", "Save a test copy"), [this]() { SendMigration(static_cast<uint8>(ESTMigrationStep::TestCopy)); }));
	AddFilled(Beta, MakeButton(nullptr, LOCTEXT("Backup", "Save a backup"), [this]() { SendMigration(static_cast<uint8>(ESTMigrationStep::Backup)); }), FMargin(0.f));
	mBetaRow = Beta;
	mMigrationLines = MakeText(Root, FText::GetEmpty(), 10, true, FMargin(0.f, 2.f, 0.f, 6.f));
	mMigrationRows = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Root->AddChildToVerticalBox(mMigrationRows)->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
}

void USTMigrationWindow::ReadTraceLimit(const FString& Text)
{
	if (Text.TrimStartAndEnd().IsNumeric())
	{
		sTraceLimit = FMath::Clamp(FCString::Atoi(*Text.TrimStartAndEnd()), STTrace::MinLimit, STTrace::MaxLimit);
	}
	mTraceLimit->SetTextIfIdle(FString::FromInt(sTraceLimit));
}

void USTMigrationWindow::ShowItemsMode()
{
	mItemsMode->SetText(sAllItems
		? LOCTEXT("AllItems", "Items per platform: every traced item (for mixed belts); click for one")
		: LOCTEXT("OneItem", "Items per platform: one (recommended); click for every traced item"));
}

void USTMigrationWindow::SendPreview()
{
	ReadTraceLimit(mTraceLimit->GetText());
	TArray<FString> Words = {TEXT("tracelimit"), FString::FromInt(sTraceLimit)};
	if (sAllItems)
	{
		Words.Add(TEXT("allitems"));
	}
	SendMigration(static_cast<uint8>(ESTMigrationStep::Preview), Words);
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

void USTMigrationWindow::Refresh(USTRemoteCallObject* RCO)
{
	RefreshMigration(RCO->GetMigrationView());
}

void USTMigrationWindow::SendMigration(uint8 Step, const TArray<FString>& Selection)
{
	if (USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this))
	{
		UE_LOG(LogSmartTrains, Log, TEXT("Migration window: step %d [%s]"), Step, *FString::Join(Selection, TEXT(" ")));
		RCO->Server_Migrate(static_cast<ESTMigrationStep>(Step), Selection);
	}
}

void USTMigrationWindow::SetAllTicks(bool bStations, bool bTicked)
{
	for (TPair<FString, bool>& Each : mTicked)
	{
		if (Each.Key.StartsWith(bStations ? TEXT("S") : TEXT("T")))
		{
			Each.Value = bTicked;
			if (const TFunction<void(bool)>* Refresh = mShowTick.Find(Each.Key))
			{
				(*Refresh)(bTicked);
			}
		}
	}
	UE_LOG(LogSmartTrains, Log, TEXT("Migration window: %s all %s"), bTicked ? TEXT("ticked") : TEXT("unticked"), bStations ? TEXT("stations") : TEXT("trains"));
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
		mMigrationLines->SetText(LOCTEXT("NothingTicked", "Nothing is ticked."));
		return;
	}
	if (bApplyArmed)
	{
		Selection.Add(TEXT("confirm"));
	}
	bApplyArmed = false;
	SendMigration(static_cast<uint8>(ESTMigrationStep::Apply), Selection);
}

void USTMigrationWindow::RefreshMigration(const FSTMigrationView& View)
{
	mUndo->SetIsEnabled(View.bCanUndo);
	mApply->SetIsEnabled(View.bHasPreview);
	if (!bApplyArmed)
	{
		mApply->SetText(LOCTEXT("Apply", "Apply ticked"));
	}
	mBetaRow->SetVisibility(View.bBetaTools ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	if (View.Serial == mShownSerial)
	{
		return;
	}
	mShownSerial = View.Serial;
	SetLines(mMigrationLines, View.Lines.Num() > 0 ? View.Lines
		: TArray<FString>{View.bHasPreview ? TEXT("Preview below; press Preview again to refresh it.") : TEXT("Press Preview to see what would change.")});

	// A fresh preview starts from its proposals; after other steps the player's ticks on the same rows are kept.
	const bool bFreshPreview = View.Lines.Num() > 0 && View.Lines[0].StartsWith(TEXT("Migration preview"));
	TMap<FString, bool> Ticks;
	for (const FSTMigrationRow& Row : View.Rows)
	{
		const FString Key = RowKey(Row);
		const bool* Previous = bFreshPreview ? nullptr : mTicked.Find(Key);
		Ticks.Add(Key, Previous ? *Previous : Row.bProposed);
	}
	mTicked = MoveTemp(Ticks);
	mMigrationRows->ClearChildren();
	mShowTick.Reset();
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		const bool bStations = Pass == 0;
		TArray<const FSTMigrationRow*> Rows;
		for (const FSTMigrationRow& Row : View.Rows)
		{
			if (Row.bStation == bStations)
			{
				Rows.Add(&Row);
			}
		}
		if (Rows.Num() == 0)
		{
			continue;
		}
		// Section heading with tick all / untick all.
		UHorizontalBox* Heading = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		mMigrationRows->AddChildToVerticalBox(Heading)->SetPadding(FMargin(0.f, Pass == 0 ? 0.f : 12.f, 0.f, 4.f));
		AddFilled(Heading, MakeText(nullptr, FText::Format(bStations ? LOCTEXT("StationsHeading", "Stations ({0})") : LOCTEXT("TrainsHeading", "Trains ({0})"),
			FText::AsNumber(Rows.Num())), 15, false), FMargin(0.f, 0.f, 8.f, 0.f));
		for (const bool bTick : {true, false})
		{
			USTButton* Button = MakeButton(nullptr, bTick ? LOCTEXT("TickAll", "Tick all") : LOCTEXT("UntickAll", "Untick all"), [this, bStations, bTick]()
			{
				SetAllTicks(bStations, bTick);
			});
			Button->SetFixedWidth(130.f);
			if (UHorizontalBoxSlot* ButtonSlot = Heading->AddChildToHorizontalBox(Button))
			{
				ButtonSlot->SetPadding(FMargin(4.f, 0.f, 0.f, 0.f));
				ButtonSlot->SetVerticalAlignment(VAlign_Center);
			}
		}
		for (const FSTMigrationRow* Row : Rows)
		{
			const FString Key = RowKey(*Row);
			const FString Title = Row->Title.IsEmpty() ? Row->Text : Row->Title;
			USTRow* Widget = MakeRow(mMigrationRows, FText::GetEmpty(), nullptr);
			Widget->SetStyle(GetFont(12), GetTextColor());
			auto Show = [Widget, Key, Title](bool bTicked)
			{
				Widget->SetText(FText::FromString(FString::Printf(TEXT("%s   %s   %s"), bTicked ? TEXT("[x]") : TEXT("[  ]"), *Key, *Title)));
			};
			Show(mTicked.FindRef(Key));
			mShowTick.Add(Key, Show);
			Widget->OnClick = [this, Key]()
			{
				bool& Ticked = mTicked.FindOrAdd(Key);
				Ticked = !Ticked;
				if (const TFunction<void(bool)>* Refresh = mShowTick.Find(Key))
				{
					(*Refresh)(Ticked);
				}
			};
			if (Row->Details.Num() > 0)
			{
				UTextBlock* Details = MakeText(mMigrationRows, FText::FromString(FString::Join(Row->Details, TEXT("\n"))), 10, true, FMargin(44.f, 0.f, 4.f, 2.f));
				Details->SetColorAndOpacity(FSlateColor(DetailColor));
			}
			if (!Row->Flag.IsEmpty())
			{
				UTextBlock* Flag = MakeText(mMigrationRows, FText::FromString(TEXT("Not ticked: ") + Row->Flag), 10, true, FMargin(44.f, 0.f, 4.f, 2.f));
				Flag->SetColorAndOpacity(FSlateColor(FlagColor));
			}
			mMigrationRows->AddChildToVerticalBox(WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass()))->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
		}
	}
	UE_LOG(LogSmartTrains, Verbose, TEXT("Migration window: %d rows shown"), View.Rows.Num());
}

#undef LOCTEXT_NAMESPACE
