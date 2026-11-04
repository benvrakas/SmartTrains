#include "UI/STPanel.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WrapBoxSlot.h"
#include "SmartTrains.h"
#include "STRemoteCallObject.h"
#include "TimerManager.h"
#include "UI/STControls.h"
#include "UI/STWidgetUtils.h"

#define LOCTEXT_NAMESPACE "SmartTrains"

namespace
{
	constexpr float RequestInterval = 1.f;
	constexpr float RefreshInterval = 0.2f;
	// How long a server message stays under the panel. [s]
	constexpr double MessageSeconds = 8.0;

	// Sets Padding on Child's slot in the box kinds the panels use.
	void SetSlotPadding(UWidget* Child, const FMargin& Padding)
	{
		UPanelSlot* AnySlot = Child ? Child->Slot : nullptr;
		if (UVerticalBoxSlot* Vertical = Cast<UVerticalBoxSlot>(AnySlot))
		{
			Vertical->SetPadding(Padding);
		}
		else if (UHorizontalBoxSlot* Horizontal = Cast<UHorizontalBoxSlot>(AnySlot))
		{
			Horizontal->SetPadding(Padding);
		}
		else if (UScrollBoxSlot* Scroll = Cast<UScrollBoxSlot>(AnySlot))
		{
			Scroll->SetPadding(Padding);
		}
		else if (UWrapBoxSlot* Wrap = Cast<UWrapBoxSlot>(AnySlot))
		{
			Wrap->SetPadding(Padding);
		}
	}
}

TSharedRef<SWidget> USTPanel::RebuildWidget()
{
	BuildTree();
	return Super::RebuildWidget();
}

void USTPanel::BuildTree()
{
	if (!WidgetTree)
	{
		WidgetTree = NewObject<UWidgetTree>(this, TEXT("WidgetTree"), RF_Transient);
	}
	if (WidgetTree->RootWidget)
	{
		return;
	}
	mRoot = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	BuildContent(mRoot);
	mMessage = MakeText(mRoot, FText::GetEmpty(), 11, true, FMargin(0.f, 6.f, 0.f, 0.f));
	mMessage->SetVisibility(ESlateVisibility::Collapsed);
	WidgetTree->RootWidget = WrapRoot(mRoot);
}

UWidget* USTPanel::WrapRoot(UVerticalBox* Root)
{
	const FMargin Spacing = GetOuterPadding();
	if (Spacing == FMargin(0.f))
	{
		return Root;
	}
	UBorder* Border = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Border->SetBrushColor(FLinearColor::Transparent);
	Border->SetPadding(Spacing);
	Border->SetContent(Root);
	return Border;
}

void USTPanel::NativeConstruct()
{
	Super::NativeConstruct();
	ApplyStyle();
	bActive = OnShown();
	SetVisibility(bActive ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	if (!bActive)
	{
		return;
	}
	mShownRevision = MAX_uint32;
	if (UWorld* World = GetWorld())
	{
		FTimerManager& Timers = World->GetTimerManager();
		Timers.SetTimer(mRequestTimer, FTimerDelegate::CreateWeakLambda(this, [this]()
		{
			if (USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this))
			{
				RequestView(RCO);
			}
		}), RequestInterval, true, 0.f);
		Timers.SetTimer(mRefreshTimer, this, &USTPanel::Tick, RefreshInterval, true, 0.f);
	}
	UE_LOG(LogSmartTrains, Log, TEXT("%s shown in %s"), *GetClass()->GetName(), *GetNameSafe(GetMenu()));
}

void USTPanel::NativeDestruct()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(mRequestTimer);
		World->GetTimerManager().ClearTimer(mRefreshTimer);
	}
	if (bActive)
	{
		bActive = false;
		OnHidden();
	}
	Super::NativeDestruct();
}

UUserWidget* USTPanel::GetMenu() const
{
	return STWidgetUtils::GetOwningUserWidget(this);
}

void USTPanel::ApplyStyle()
{
	if (!bHasStyle)
	{
		FSlateColor Color;
		bHasStyle = STWidgetUtils::FindTextStyle(GetMenu(), GetStyleSource(), 11, mFont, Color);
		if (bHasStyle)
		{
			mTextColor = Color;
		}
		else
		{
			UE_LOG(LogSmartTrains, Log, TEXT("%s: no vanilla text to copy the font from; default font used"), *GetClass()->GetName());
		}
	}
	for (const FStyledText& Each : mStyled)
	{
		if (UTextBlock* Text = Each.Text.Get())
		{
			Text->SetFont(GetFont(Each.Size));
			Text->SetColorAndOpacity(mTextColor);
		}
	}
}

FSlateFontInfo USTPanel::GetFont(int32 Size) const
{
	FSlateFontInfo Font = mFont;
	if (!bHasStyle)
	{
		Font = FCoreStyle::GetDefaultFontStyle("Regular", Size);
	}
	Font.Size = Size;
	return Font;
}

void USTPanel::Tick()
{
	USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this);
	if (!RCO)
	{
		if (!bWarnedNoRCO)
		{
			bWarnedNoRCO = true;
			mMessage->SetText(LOCTEXT("NoServer", "SmartTrains isn't running on this server."));
			mMessage->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
		}
		return;
	}
	if (RCO->GetRevision() != mShownRevision)
	{
		mShownRevision = RCO->GetRevision();
		Refresh(RCO);
	}
	ShowServerMessage(RCO);
}

void USTPanel::RefreshSoon()
{
	if (USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this))
	{
		RequestView(RCO);
		Refresh(RCO);
	}
}

void USTPanel::ShowServerMessage(USTRemoteCallObject* RCO)
{
	const UWorld* World = GetWorld();
	const bool bShow = RCO && World && !RCO->GetLastMessage().IsEmpty() && World->GetTimeSeconds() - RCO->GetLastMessageTime() < MessageSeconds;
	if (bShow)
	{
		mMessage->SetText(FText::FromString(RCO->GetLastMessage()));
		mMessage->SetColorAndOpacity(RCO->IsLastMessageError() ? FSlateColor(FLinearColor(1.f, 0.35f, 0.3f)) : mTextColor);
	}
	mMessage->SetVisibility(bShow ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
}

UTextBlock* USTPanel::MakeText(UPanelWidget* Parent, const FText& Text, int32 Size, bool bWrap, const FMargin& Spacing)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Block->SetText(Text);
	Block->SetAutoWrapText(bWrap);
	Block->SetFont(GetFont(Size));
	Block->SetColorAndOpacity(mTextColor);
	if (mStyled.Num() > 256)
	{
		mStyled.RemoveAll([](const FStyledText& Each) { return !Each.Text.IsValid(); });
	}
	mStyled.Add({Block, Size});
	if (Parent)
	{
		Parent->AddChild(Block);
		SetSlotPadding(Block, Spacing);
	}
	return Block;
}

USTButton* USTPanel::MakeButton(UPanelWidget* Parent, const FText& Text, TFunction<void()> OnClick, const FMargin& Spacing)
{
	USTButton* Button = WidgetTree->ConstructWidget<USTButton>(USTButton::StaticClass());
	Button->SetText(Text);
	Button->OnClick = MoveTemp(OnClick);
	if (Parent)
	{
		Parent->AddChild(Button);
		SetSlotPadding(Button, Spacing);
	}
	return Button;
}

USTTextInput* USTPanel::MakeInput(UPanelWidget* Parent, const FText& Hint, const FMargin& Spacing)
{
	USTTextInput* Input = WidgetTree->ConstructWidget<USTTextInput>(USTTextInput::StaticClass());
	Input->SetHint(Hint);
	if (Parent)
	{
		Parent->AddChild(Input);
		SetSlotPadding(Input, Spacing);
	}
	return Input;
}

USTRow* USTPanel::MakeRow(UPanelWidget* Parent, const FText& Text, TFunction<void()> OnClick)
{
	USTRow* Row = WidgetTree->ConstructWidget<USTRow>(USTRow::StaticClass());
	Row->SetText(Text);
	Row->SetStyle(GetFont(11), mTextColor);
	Row->OnClick = MoveTemp(OnClick);
	if (Parent)
	{
		Parent->AddChild(Row);
	}
	return Row;
}

UWidget* USTPanel::MakeHeading(UPanelWidget* Parent, const FText& Text, const FMargin& Spacing)
{
	UWidget* Heading = STWidgetUtils::CreateVanillaWidget(WidgetTree, STWidgetUtils::MenuLabelClass);
	if (Heading)
	{
		STWidgetUtils::SetWidgetText(Cast<UUserWidget>(Heading), Text);
		if (Parent)
		{
			Parent->AddChild(Heading);
			SetSlotPadding(Heading, Spacing);
		}
		return Heading;
	}
	return MakeText(Parent, Text, 16, false, Spacing);
}

void USTPanel::SetLines(UTextBlock* Text, const TArray<FString>& Lines)
{
	if (!Text)
	{
		return;
	}
	Text->SetText(FText::FromString(FString::Join(Lines, TEXT("\n"))));
	Text->SetVisibility(Lines.Num() > 0 ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
}

#undef LOCTEXT_NAMESPACE
