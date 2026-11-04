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
#include "TimerManager.h"
#include "UI/STControls.h"
#include "UI/STWidgetUtils.h"

#define LOCTEXT_NAMESPACE "SmartTrains"

TWeakObjectPtr<USTMigrationWindow> USTMigrationWindow::sOpen;

namespace
{
	constexpr float WindowWidth = 640.f;
	constexpr float WindowMaxHeight = 760.f;
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

void USTMigrationWindow::BuildContent(UVerticalBox* Root)
{
	UHorizontalBox* Title = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Root->AddChildToVerticalBox(Title);
	AddFilled(Title, MakeHeading(nullptr, LOCTEXT("MigrationHeading", "SmartTrains migration")), FMargin(0.f, 0.f, 8.f, 0.f));
	USTButton* CloseButton = MakeButton(nullptr, LOCTEXT("Close", "Close"), [this]() { Close(); });
	if (UHorizontalBoxSlot* CloseSlot = Title->AddChildToHorizontalBox(CloseButton))
	{
		CloseSlot->SetVerticalAlignment(VAlign_Center);
	}

	MakeText(Root, LOCTEXT("MigrationHint", "Proposes platform roles and managed trains from DynamicTimeTable groups, platform load/unload "
		"settings, belts, stock and timetables. Trains that only shuttle between two stations outside the groups are left alone. "
		"Untick what you don't want, then apply. Undo puts back the last apply."), 10);
	UHorizontalBox* Steps = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Root->AddChildToVerticalBox(Steps)->SetPadding(FMargin(0.f, 6.f, 0.f, 4.f));
	AddFilled(Steps, MakeButton(nullptr, LOCTEXT("Preview", "Preview"), [this]() { SendMigration(static_cast<uint8>(ESTMigrationStep::Preview)); }));
	mApply = MakeButton(nullptr, LOCTEXT("Apply", "Apply ticked"), [this]() { ApplyTicked(); });
	AddFilled(Steps, mApply);
	mUndo = MakeButton(nullptr, LOCTEXT("Undo", "Undo"), [this]() { SendMigration(static_cast<uint8>(ESTMigrationStep::Undo)); });
	AddFilled(Steps, mUndo, FMargin(0.f));
	UHorizontalBox* Beta = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Root->AddChildToVerticalBox(Beta)->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
	AddFilled(Beta, MakeButton(nullptr, LOCTEXT("TestCopy", "Save a test copy"), [this]() { SendMigration(static_cast<uint8>(ESTMigrationStep::TestCopy)); }));
	AddFilled(Beta, MakeButton(nullptr, LOCTEXT("Backup", "Save a backup"), [this]() { SendMigration(static_cast<uint8>(ESTMigrationStep::Backup)); }), FMargin(0.f));
	mBetaRow = Beta;
	mMigrationLines = MakeText(Root, FText::GetEmpty(), 10);
	mMigrationRows = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Root->AddChildToVerticalBox(mMigrationRows)->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
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
	for (const FSTMigrationRow& Row : View.Rows)
	{
		const FString Key = RowKey(Row);
		const FString Text = Row.Text;
		USTRow* Widget = MakeRow(mMigrationRows, FText::GetEmpty(), nullptr);
		auto Show = [Widget, Key, Text](bool bTicked)
		{
			Widget->SetText(FText::FromString(FString::Printf(TEXT("%s %s %s"), bTicked ? TEXT("[x]") : TEXT("[  ]"), *Key, *Text)));
		};
		Show(mTicked.FindRef(Key));
		Widget->OnClick = [this, Key, Show]()
		{
			bool& Ticked = mTicked.FindOrAdd(Key);
			Ticked = !Ticked;
			Show(Ticked);
		};
	}
	UE_LOG(LogSmartTrains, Verbose, TEXT("Migration window: %d rows shown"), View.Rows.Num());
}

#undef LOCTEXT_NAMESPACE
