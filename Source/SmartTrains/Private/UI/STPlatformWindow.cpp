#include "UI/STPlatformWindow.h"
#include "Blueprint/WidgetTree.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
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
#include "UI/STControls.h"
#include "UI/STWidgetUtils.h"
#include "UI/STStyle.h"

#define LOCTEXT_NAMESPACE "SmartTrains"

TWeakObjectPtr<USTPlatformWindow> USTPlatformWindow::sOpen;

namespace
{
	constexpr float PlatformWindowWidth = 620.f;
	// The window keeps this height whatever it shows, so it doesn't jump when the role changes; content scrolls. [px]
	constexpr float PlatformWindowHeight = 860.f;
	constexpr float LauncherWidth = 300.f;
	// Size of the item icons beside the platform menu. [px]
	constexpr float LauncherIconSize = 32.f;
}

//~ USTPlatformWindow

void USTPlatformWindow::Open(UUserWidget* Menu)
{
	APlayerController* Controller = Menu ? Menu->GetOwningPlayer() : nullptr;
	if (!Controller || !Controller->IsLocalController())
	{
		UE_LOG(LogSmartTrains, Warning, TEXT("Platform window: no local player for %s"), *GetNameSafe(Menu));
		return;
	}
	if (USTPlatformWindow* Existing = sOpen.Get())
	{
		if (Existing->GetMenu() == Menu && Existing->IsInViewport())
		{
			Existing->SetKeyboardFocus();
			return;
		}
		Existing->Close();
	}
	USTPlatformWindow* Window = CreateWidget<USTPlatformWindow>(Controller, USTPlatformWindow::StaticClass());
	if (!Window)
	{
		UE_LOG(LogSmartTrains, Error, TEXT("Platform window could not be created"));
		return;
	}
	Window->SetMenu(Menu);
	Window->SetIsFocusable(true);
	Window->AddToViewport(60);
	Window->SetKeyboardFocus();
	sOpen = Window;
	UE_LOG(LogSmartTrains, Log, TEXT("Platform window opened from %s"), *GetNameSafe(Menu));
}

void USTPlatformWindow::CloseFor(const UUserWidget* Menu)
{
	if (USTPlatformWindow* Existing = sOpen.Get(); Existing && Existing->GetMenu() == Menu)
	{
		Existing->Close();
	}
}

void USTPlatformWindow::Close()
{
	UE_LOG(LogSmartTrains, Log, TEXT("Platform window closed"));
	RemoveFromParent();
	if (UUserWidget* Menu = GetMenu())
	{
		Menu->SetKeyboardFocus();
	}
}

void USTPlatformWindow::OnHidden()
{
	Super::OnHidden();
	if (sOpen.Get() == this)
	{
		sOpen.Reset();
	}
}

FReply USTPlatformWindow::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		Close();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

UWidget* USTPlatformWindow::WrapRoot(UVerticalBox* Root)
{
	UOverlay* Screen = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
	// A light shade over the game; it also catches clicks beside the window, so the window keeps the focus.
	UBorder* Shade = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Shade->SetBrushColor(FLinearColor(0.f, 0.f, 0.f, 0.35f));
	Shade->SetVisibility(ESlateVisibility::Visible);
	if (UOverlaySlot* ShadeSlot = Screen->AddChildToOverlay(Shade))
	{
		ShadeSlot->SetHorizontalAlignment(HAlign_Fill);
		ShadeSlot->SetVerticalAlignment(VAlign_Fill);
	}

	UBorder* Frame = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Frame->SetBrush(STStyle::Outlined(STStyle::Panel, STStyle::Line, 1.f, 6.f));
	Frame->SetPadding(FMargin(18.f, 14.f));
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Frame->SetContent(Column);
	UHorizontalBox* Title = MakeTitleBar(Column, LOCTEXT("PlatformWindowTitle", "Platform settings"), 26.f);
	USTButton* CloseButton = MakeButton(nullptr, LOCTEXT("Close", "Close"), [this]() { Close(); });
	CloseButton->SetFixedWidth(120.f);
	Title->AddChildToHorizontalBox(CloseButton);
	SetVAlignCenter(CloseButton);

	USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Size->SetWidthOverride(PlatformWindowWidth);
	Size->SetHeightOverride(PlatformWindowHeight);
	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	Scroll->AddChild(Root);
	Size->AddChild(Scroll);
	Column->AddChildToVerticalBox(Size);
	if (UOverlaySlot* FrameSlot = Screen->AddChildToOverlay(Frame))
	{
		FrameSlot->SetHorizontalAlignment(HAlign_Center);
		FrameSlot->SetVerticalAlignment(VAlign_Center);
	}
	return Screen;
}

//~ USTPlatformLauncher

UWidget* USTPlatformLauncher::WrapRoot(UVerticalBox* Root)
{
	UBorder* Frame = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Frame->SetBrush(STStyle::Rounded(STStyle::Panel));
	Frame->SetPadding(FMargin(14.f, 10.f));
	USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Size->SetWidthOverride(LauncherWidth);
	Size->AddChild(Root);
	Frame->SetContent(Size);
	return Frame;
}

void USTPlatformLauncher::BuildContent(UVerticalBox* Root)
{
	MakeTitleBar(Root, FText::GetEmpty(), 20.f);
	UHorizontalBox* RoleRow = MakeHBox(Root, FMargin(0.f, 2.f, 0.f, 2.f));
	mRole = MakeBadge(RoleRow);
	mSummary = MakeTinted(RoleRow, LOCTEXT("Loading", "Asking the server..."), 11, STStyle::Text, false, true, FMargin(8.f, 0.f, 0.f, 0.f));
	SetFill(mSummary);
	SetVAlignCenter(mSummary);
	mItemRows = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Root->AddChildToVerticalBox(mItemRows)->SetPadding(FMargin(0.f, 2.f, 0.f, 0.f));
	mStats = MakeHBox(Root, FMargin(0.f, 6.f, 0.f, 0.f));
	UVerticalBox* A = MakeVBox(mStats);
	SetFill(A);
	mRate = MakeStat(A, LOCTEXT("LauncherRate", "per minute at most"));
	UVerticalBox* B = MakeVBox(mStats, FMargin(4.f, 0.f, 0.f, 0.f));
	SetFill(B);
	mLoops = MakeStat(B, LOCTEXT("LauncherLoops", "loops"));
	mStatus = MakeTinted(Root, FText::GetEmpty(), 10, STStyle::Muted, false, true, FMargin(0.f, 4.f, 0.f, 6.f));
	MakeButton(Root, LOCTEXT("OpenPlatformWindow", "Settings"), [this]() { USTPlatformWindow::Open(GetMenu()); });
	// Copy and Paste, half the width each, as in the settings window.
	UHorizontalBox* Clipboard = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Root->AddChildToVerticalBox(Clipboard)->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
	USTButton* Copy = MakeButton(nullptr, LOCTEXT("Copy", "Copy"), [this]()
	{
		if (USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this))
		{
			RCO->CopyPlatformFromMenu(STWidgetUtils::GetMenuPlatform(GetMenu()));
		}
	}, FMargin(0.f));
	mPaste = MakeButton(nullptr, LOCTEXT("Paste", "Paste"), [this]()
	{
		if (USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this))
		{
			RCO->PastePlatformFromMenu(STWidgetUtils::GetMenuPlatform(GetMenu()));
			RefreshSoon();
		}
	}, FMargin(0.f));
	mPaste->SetIsEnabled(false);
	for (USTButton* Button : {Copy, mPaste.Get()})
	{
		if (UHorizontalBoxSlot* ButtonSlot = Clipboard->AddChildToHorizontalBox(Button))
		{
			ButtonSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			ButtonSlot->SetPadding(FMargin(0.f, 0.f, Button == Copy ? 4.f : 0.f, 0.f));
		}
	}
}

bool USTPlatformLauncher::OnShown()
{
	mSummary->SetText(LOCTEXT("Loading", "Asking the server..."));
	mStatus->SetText(FText::GetEmpty());
	mItemRows->ClearChildren();
	mShownItems.Reset();
	bItemsShown = false;
	return STWidgetUtils::GetMenuPlatform(GetMenu()) != nullptr;
}

void USTPlatformLauncher::OnHidden()
{
	USTPlatformWindow::CloseFor(GetMenu());
}

void USTPlatformLauncher::RequestView(USTRemoteCallObject* RCO)
{
	if (AFGBuildableTrainPlatformCargo* Platform = STWidgetUtils::GetMenuPlatform(GetMenu()))
	{
		RCO->Server_RequestPlatform(Platform);
	}
}

void USTPlatformLauncher::Refresh(USTRemoteCallObject* RCO)
{
	const FSTPlatformView* View = RCO->GetPlatformView(STWidgetUtils::GetMenuPlatform(GetMenu()));
	if (!View)
	{
		return;
	}
	const FLinearColor RoleColor = View->Role == ESTRole::Demand ? STStyle::Orange : View->Role == ESTRole::Supply ? STStyle::Good
		: View->Role == ESTRole::Sink ? STStyle::Warn : STStyle::Muted;
	SetBadge(mRole, FText::FromString(STNames::Role(View->Role)), RoleColor);
	mSummary->SetText(View->Role == ESTRole::Off ? LOCTEXT("LauncherOff", "SmartTrains leaves it alone") : View->Role == ESTRole::Sink ? LOCTEXT("LauncherSink", "Any cargo")
		: View->Items.Num() == 0 ? LOCTEXT("LauncherNoItems", "No items yet") : FText::GetEmpty());
	const bool bWorks = View->Role == ESTRole::Supply || View->Role == ESTRole::Demand;
	mStats->SetVisibility(bWorks ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	mRate.Value->SetText(FText::FromString(View->RatePerMinute >= 0.f ? FString::Printf(TEXT("%.0f%s"), View->RatePerMinute, View->bFluid ? TEXT(" m³") : TEXT(""))
		: FString(TEXT("-"))));
	mRate.Label->SetText(View->Role == ESTRole::Supply ? LOCTEXT("LauncherRateSupply", "made per minute at most") : LOCTEXT("LauncherRateDemand", "used per minute at most"));
	mLoops.Value->SetText(FText::AsNumber(View->Loops));
	mLoops.Label->SetText(View->Role == ESTRole::Supply ? LOCTEXT("LauncherLoopsSupply", "loops take from here") : LOCTEXT("LauncherLoopsDemand", "trains on its loop"));
	ShowItems(View->Role == ESTRole::Off || View->Role == ESTRole::Sink ? TArray<FSTItemRule>() : View->Items);
	mPaste->SetIsEnabled(RCO->HasClipboard(View->bFluid));
	// One line: the next train here.
	const FString Next = View->NextTrain.IsEmpty() ? FString() : View->NextSeconds == 0.f ? FString::Printf(TEXT("Docked now: %s"), *View->NextTrain)
		: FString::Printf(TEXT("Next: %s%s"), *View->NextTrain, View->NextSeconds > 0.f ? *FString::Printf(TEXT(" in %s"), *STStyle::Duration(View->NextSeconds)) : TEXT(""));
	mStatus->SetText(FText::FromString(Next));
	mStatus->SetVisibility(Next.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);
}

void USTPlatformLauncher::ShowItems(const TArray<FSTItemRule>& Items)
{
	TArray<TSubclassOf<UFGItemDescriptor>> Wanted;
	for (const FSTItemRule& Rule : Items)
	{
		Wanted.Add(Rule.Item);
	}
	if (bItemsShown && Wanted == mShownItems)
	{
		return;
	}
	bItemsShown = true;
	mShownItems = Wanted;
	mItemRows->ClearChildren();
	for (const TSubclassOf<UFGItemDescriptor>& Item : Wanted)
	{
		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		mItemRows->AddChildToVerticalBox(Row)->SetPadding(FMargin(0.f, 2.f));
		if (UTexture2D* Texture = Item ? UFGItemDescriptor::GetSmallIcon(Item) : nullptr)
		{
			USizeBox* IconBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
			IconBox->SetWidthOverride(LauncherIconSize);
			IconBox->SetHeightOverride(LauncherIconSize);
			UImage* Icon = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
			Icon->SetBrushFromTexture(Texture);
			IconBox->AddChild(Icon);
			if (UHorizontalBoxSlot* IconSlot = Row->AddChildToHorizontalBox(IconBox))
			{
				IconSlot->SetVerticalAlignment(VAlign_Center);
				IconSlot->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
			}
		}
		UTextBlock* Name = MakeTinted(nullptr, Item ? UFGItemDescriptor::GetItemName(Item) : LOCTEXT("LauncherAnyItem", "Any item"), 13, STStyle::Text, true, true);
		if (UHorizontalBoxSlot* NameSlot = Row->AddChildToHorizontalBox(Name))
		{
			NameSlot->SetVerticalAlignment(VAlign_Center);
			NameSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}
	}
}

#undef LOCTEXT_NAMESPACE
