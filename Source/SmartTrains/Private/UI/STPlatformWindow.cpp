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

#define LOCTEXT_NAMESPACE "SmartTrains"

TWeakObjectPtr<USTPlatformWindow> USTPlatformWindow::sOpen;

namespace
{
	constexpr float PlatformWindowWidth = 620.f;
	constexpr float PlatformWindowMaxHeight = 860.f;
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

	UOverlay* Frame = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
	UWidget* Background = STWidgetUtils::CreateVanillaWidget(WidgetTree, STWidgetUtils::ContentBackgroundClass);
	if (!Background)
	{
		UBorder* Plain = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Plain->SetBrushColor(FLinearColor(0.02f, 0.02f, 0.025f, 0.97f));
		Background = Plain;
	}
	if (UOverlaySlot* BackgroundSlot = Frame->AddChildToOverlay(Background))
	{
		BackgroundSlot->SetHorizontalAlignment(HAlign_Fill);
		BackgroundSlot->SetVerticalAlignment(VAlign_Fill);
	}
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	UHorizontalBox* Title = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Column->AddChildToVerticalBox(Title)->SetPadding(FMargin(0.f, 0.f, 0.f, 6.f));
	if (UHorizontalBoxSlot* TitleSlot = Title->AddChildToHorizontalBox(MakeText(nullptr, LOCTEXT("PlatformWindowTitle", "SmartTrains: platform settings"), 16, false)))
	{
		TitleSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		TitleSlot->SetVerticalAlignment(VAlign_Center);
	}
	USTButton* CloseButton = MakeButton(nullptr, LOCTEXT("Close", "Close"), [this]() { Close(); });
	CloseButton->SetFixedWidth(120.f);
	Title->AddChildToHorizontalBox(CloseButton);

	USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Size->SetWidthOverride(PlatformWindowWidth);
	Size->SetMaxDesiredHeight(PlatformWindowMaxHeight);
	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	Scroll->AddChild(Root);
	Size->AddChild(Scroll);
	Column->AddChildToVerticalBox(Size);
	if (UOverlaySlot* ContentSlot = Frame->AddChildToOverlay(Column))
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

//~ USTPlatformLauncher

UWidget* USTPlatformLauncher::WrapRoot(UVerticalBox* Root)
{
	UOverlay* Frame = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
	if (UWidget* Background = STWidgetUtils::CreateVanillaWidget(WidgetTree, STWidgetUtils::ContentBackgroundClass))
	{
		if (UOverlaySlot* BackgroundSlot = Frame->AddChildToOverlay(Background))
		{
			BackgroundSlot->SetHorizontalAlignment(HAlign_Fill);
			BackgroundSlot->SetVerticalAlignment(VAlign_Fill);
		}
	}
	USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Size->SetWidthOverride(LauncherWidth);
	Size->AddChild(Root);
	if (UOverlaySlot* ContentSlot = Frame->AddChildToOverlay(Size))
	{
		ContentSlot->SetPadding(FMargin(14.f, 10.f));
	}
	return Frame;
}

void USTPlatformLauncher::BuildContent(UVerticalBox* Root)
{
	MakeText(Root, LOCTEXT("LauncherHeading", "SmartTrains"), 15, false, FMargin(0.f, 0.f, 0.f, 4.f));
	mSummary = MakeText(Root, LOCTEXT("Loading", "Asking the server..."), 11);
	mItemRows = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Root->AddChildToVerticalBox(mItemRows)->SetPadding(FMargin(0.f, 2.f, 0.f, 0.f));
	mStatus = MakeText(Root, FText::GetEmpty(), 10, true, FMargin(0.f, 2.f, 0.f, 6.f));
	MakeButton(Root, LOCTEXT("OpenPlatformWindow", "Open SmartTrains settings"), [this]() { USTPlatformWindow::Open(GetMenu()); });
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
	if (View->Role == ESTRole::Off)
	{
		mSummary->SetText(LOCTEXT("LauncherOff", "Off"));
	}
	else if (View->Role == ESTRole::Sink)
	{
		mSummary->SetText(LOCTEXT("LauncherSink", "Sink: any item"));
	}
	else
	{
		mSummary->SetText(FText::FromString(View->Items.Num() > 0 ? FString::Printf(TEXT("%s:"), STNames::Role(View->Role))
			: FString::Printf(TEXT("%s: no items"), STNames::Role(View->Role))));
	}
	ShowItems(View->Role == ESTRole::Off || View->Role == ESTRole::Sink ? TArray<FSTItemRule>() : View->Items);
	// The readout's last line is the first reason or the inbound trains; one line is enough here.
	mStatus->SetText(View->Lines.Num() > 0 ? FText::FromString(View->Lines.Last()) : FText::GetEmpty());
	mStatus->SetVisibility(View->Lines.Num() > 0 ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
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
		UTextBlock* Name = MakeText(nullptr, Item ? UFGItemDescriptor::GetItemName(Item) : LOCTEXT("LauncherAnyItem", "Any item"), 12, true);
		if (UHorizontalBoxSlot* NameSlot = Row->AddChildToHorizontalBox(Name))
		{
			NameSlot->SetVerticalAlignment(VAlign_Center);
			NameSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}
	}
}

#undef LOCTEXT_NAMESPACE
