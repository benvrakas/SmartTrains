#include "UI/STMonitorWindow.h"
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

TWeakObjectPtr<USTMonitorWindow> USTMonitorWindow::sOpen;

namespace
{
	constexpr float MonitorWidth = 980.f;
	constexpr float MonitorMaxHeight = 860.f;
	const FLinearColor MonitorDetailColor(0.72f, 0.72f, 0.72f);
	const FLinearColor MonitorProblemColor(1.f, 0.55f, 0.25f);
	const FLinearColor MonitorNoteColor(1.f, 0.85f, 0.35f);
	const FLinearColor MonitorFineColor(0.55f, 0.85f, 0.55f);
	// Wait after the command before taking the input, so the closing chat doesn't take it back. [s]
	constexpr float MonitorOpenDelay = 0.2f;

	FString Signature(const FSTMonitorView& View)
	{
		FString Text = FString::Join(View.Summary, TEXT("|")) + FString::Join(View.Platforms, TEXT("|")) + FString::Join(View.Idle, TEXT("|"));
		for (const TArray<FSTMonitorRow>* Rows : {&View.Items, &View.Stations})
		{
			for (const FSTMonitorRow& Row : *Rows)
			{
				Text += Row.Title + Row.Verdict + FString::Join(Row.Details, TEXT("|"));
			}
		}
		return Text;
	}
}

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

void USTMonitorWindow::BuildContent(UVerticalBox* Root)
{
	UHorizontalBox* Title = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Root->AddChildToVerticalBox(Title);
	if (UHorizontalBoxSlot* TitleSlot = Title->AddChildToHorizontalBox(MakeText(nullptr, LOCTEXT("MonitorHeading", "SmartTrains monitor"), 18, false)))
	{
		TitleSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		TitleSlot->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
	}
	USTButton* CloseButton = MakeButton(nullptr, LOCTEXT("Close", "Close"), [this]() { Close(); });
	CloseButton->SetFixedWidth(140.f);
	if (UHorizontalBoxSlot* CloseSlot = Title->AddChildToHorizontalBox(CloseButton))
	{
		CloseSlot->SetVerticalAlignment(VAlign_Center);
	}
	MakeText(Root, LOCTEXT("MonitorHint", "What limits deliveries per item over the last half hour: not enough supply, not enough trains, no train that fits, "
		"or a congested station. Updates every few seconds."), 10, true, FMargin(0.f, 4.f, 0.f, 6.f));
	mContent = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Root->AddChildToVerticalBox(mContent);
	MakeText(mContent, LOCTEXT("MonitorLoading", "Asking the server..."), 11);
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

void USTMonitorWindow::RequestView(USTRemoteCallObject* RCO)
{
	RCO->Server_RequestMonitor();
}

void USTMonitorWindow::Refresh(USTRemoteCallObject* RCO)
{
	const FSTMonitorView& View = RCO->GetMonitorView();
	if (View.Summary.Num() == 0)
	{
		return;
	}
	FString Shown = Signature(View);
	if (Shown == mShown)
	{
		return;
	}
	mShown = MoveTemp(Shown);
	ShowView(View);
}

void USTMonitorWindow::AddSection(const FText& Heading, const TArray<FString>& Lines)
{
	if (Lines.Num() == 0)
	{
		return;
	}
	MakeText(mContent, Heading, 15, false, FMargin(0.f, 12.f, 0.f, 4.f));
	UTextBlock* Text = MakeText(mContent, FText::GetEmpty(), 10, true, FMargin(12.f, 0.f, 4.f, 2.f));
	SetLines(Text, Lines);
}

void USTMonitorWindow::ShowView(const FSTMonitorView& View)
{
	mContent->ClearChildren();
	SetLines(MakeText(mContent, FText::GetEmpty(), 11), View.Summary);

	auto AddRows = [this](const FText& Heading, const TArray<FSTMonitorRow>& Rows)
	{
		if (Rows.Num() == 0)
		{
			return;
		}
		MakeText(mContent, Heading, 15, false, FMargin(0.f, 12.f, 0.f, 4.f));
		for (const FSTMonitorRow& Row : Rows)
		{
			UTextBlock* Title = MakeText(mContent, FText::FromString(FString::Printf(TEXT("%s   %s"), *Row.Title, *Row.Verdict)), 12, true, FMargin(0.f, 4.f, 0.f, 0.f));
			Title->SetColorAndOpacity(FSlateColor(Row.Severity >= 2 ? MonitorProblemColor : (Row.Severity == 1 ? MonitorNoteColor : MonitorFineColor)));
			if (Row.Details.Num() > 0 && Row.Severity > 0)
			{
				UTextBlock* Details = MakeText(mContent, FText::GetEmpty(), 10, true, FMargin(24.f, 0.f, 4.f, 2.f));
				SetLines(Details, Row.Details);
				Details->SetColorAndOpacity(FSlateColor(MonitorDetailColor));
			}
		}
	};
	AddRows(LOCTEXT("ItemsHeading", "Items"), View.Items);
	AddRows(LOCTEXT("StationsHeading", "Congested stations"), View.Stations);
	AddSection(LOCTEXT("PlatformsHeading", "Platforms to look at"), View.Platforms);
	AddSection(LOCTEXT("IdleHeading", "Idle trains and why"), View.Idle);
	UE_LOG(LogSmartTrains, Verbose, TEXT("Monitor window: %d items, %d stations shown"), View.Items.Num(), View.Stations.Num());
}

#undef LOCTEXT_NAMESPACE
