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
#include "UI/STStyle.h"
#include "Components/Image.h"
#include "Components/SizeBox.h"
#include "Components/HorizontalBox.h"
#include "Engine/Texture2D.h"
#include "Resources/FGItemDescriptor.h"
#include "UI/STWidgetUtils.h"

#define LOCTEXT_NAMESPACE "SmartTrains"

namespace
{
	constexpr float RequestInterval = 1.f;
	constexpr float RefreshInterval = 0.2f;
	// How long a server message stays under the panel. [s]
	constexpr double MessageSeconds = 8.0;

	// How long a panel takes to fade and slide in. [s]
	constexpr float FadeInSeconds = 0.22f;
	// How far it slides up while fading in. [px]
	constexpr float FadeInRise = 8.f;
}

void USTPanel::SetSlotPadding(UWidget* Child, const FMargin& Padding)
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

void USTPanel::SetFill(UWidget* Child, float Fill)
{
	UPanelSlot* AnySlot = Child ? Child->Slot : nullptr;
	FSlateChildSize Size = Fill > 0.f ? FSlateChildSize(ESlateSizeRule::Fill) : FSlateChildSize(ESlateSizeRule::Automatic);
	Size.Value = Fill > 0.f ? Fill : 1.f;
	if (UHorizontalBoxSlot* Horizontal = Cast<UHorizontalBoxSlot>(AnySlot))
	{
		Horizontal->SetSize(Size);
	}
	else if (UVerticalBoxSlot* Vertical = Cast<UVerticalBoxSlot>(AnySlot))
	{
		Vertical->SetSize(Size);
	}
}

void USTPanel::SetVAlignCenter(UWidget* Child)
{
	if (UHorizontalBoxSlot* Horizontal = Child ? Cast<UHorizontalBoxSlot>(Child->Slot) : nullptr)
	{
		Horizontal->SetVerticalAlignment(VAlign_Center);
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
	mShownFor = 0.f;
	SetRenderOpacity(0.f);
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
	return mMenuOverride.IsValid() ? mMenuOverride.Get() : STWidgetUtils::GetOwningUserWidget(this);
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
			Text->SetFont(GetFont(Each.Size, Each.bBold));
			Text->SetColorAndOpacity(Each.bOwnColor ? FSlateColor(Each.Color) : mTextColor);
		}
	}
}

FSlateFontInfo USTPanel::GetFont(int32 Size, bool bBold) const
{
	FSlateFontInfo Font = mFont;
	if (!bHasStyle)
	{
		Font = FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Size);
	}
	else if (bBold)
	{
		Font.TypefaceFontName = TEXT("Bold");
	}
	Font.Size = Size;
	return Font;
}

void USTPanel::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (mShownFor < FadeInSeconds)
	{
		mShownFor = FMath::Min(mShownFor + InDeltaTime, FadeInSeconds);
		const float T = mShownFor / FadeInSeconds;
		const float Eased = 1.f - FMath::Square(1.f - T);
		SetRenderOpacity(Eased);
		SetRenderTranslation(FVector2D(0.f, FadeInRise * (1.f - Eased)));
	}
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
	Button->SetFont(GetFont(11, true));
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

UVerticalBox* USTPanel::MakeCard(UPanelWidget* Parent, const FLinearColor& Color, const FMargin& Inner, const FMargin& Spacing, UBorder** OutBorder)
{
	UBorder* Border = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Border->SetBrush(STStyle::Rounded(Color));
	Border->SetPadding(Inner);
	UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Border->SetContent(Box);
	if (Parent)
	{
		Parent->AddChild(Border);
		SetSlotPadding(Border, Spacing);
	}
	if (OutBorder)
	{
		*OutBorder = Border;
	}
	return Box;
}

UVerticalBox* USTPanel::MakeVBox(UPanelWidget* Parent, const FMargin& Spacing)
{
	UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	if (Parent)
	{
		Parent->AddChild(Box);
		SetSlotPadding(Box, Spacing);
	}
	return Box;
}

UHorizontalBox* USTPanel::MakeHBox(UPanelWidget* Parent, const FMargin& Spacing)
{
	UHorizontalBox* Box = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	if (Parent)
	{
		Parent->AddChild(Box);
		SetSlotPadding(Box, Spacing);
	}
	return Box;
}

UTextBlock* USTPanel::MakeLabel(UPanelWidget* Parent, const FText& Text, const FMargin& Spacing)
{
	return MakeTinted(Parent, FText::FromString(Text.ToString().ToUpper()), 9, STStyle::Muted, true, false, Spacing);
}

UTextBlock* USTPanel::MakeTinted(UPanelWidget* Parent, const FText& Text, int32 Size, const FLinearColor& Color, bool bBold, bool bWrap, const FMargin& Spacing)
{
	UTextBlock* Block = MakeText(Parent, Text, Size, bWrap, Spacing);
	if (FStyledText* Styled = mStyled.FindByPredicate([Block](const FStyledText& Each) { return Each.Text.Get() == Block; }))
	{
		Styled->bBold = bBold;
		Styled->bOwnColor = true;
		Styled->Color = Color;
	}
	Block->SetFont(GetFont(Size, bBold));
	Block->SetColorAndOpacity(FSlateColor(Color));
	return Block;
}

void USTPanel::Tint(UTextBlock* Text, const FLinearColor& Color)
{
	if (!Text)
	{
		return;
	}
	if (FStyledText* Styled = mStyled.FindByPredicate([Text](const FStyledText& Each) { return Each.Text.Get() == Text; }))
	{
		Styled->bOwnColor = true;
		Styled->Color = Color;
	}
	Text->SetColorAndOpacity(FSlateColor(Color));
}

USTPanel::FBadge USTPanel::MakeBadge(UPanelWidget* Parent, const FMargin& Spacing)
{
	FBadge Badge;
	Badge.Box = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Badge.Box->SetBrush(STStyle::Rounded(STStyle::Good, 9.f));
	Badge.Box->SetPadding(FMargin(8.f, 2.f));
	Badge.Box->SetVerticalAlignment(VAlign_Center);
	Badge.Text = MakeTinted(nullptr, FText::GetEmpty(), 10, STStyle::Dark, true, false);
	Badge.Box->SetContent(Badge.Text);
	if (Parent)
	{
		Parent->AddChild(Badge.Box);
		SetSlotPadding(Badge.Box, Spacing);
		SetVAlignCenter(Badge.Box);
	}
	return Badge;
}

void USTPanel::SetBadge(const FBadge& Badge, const FText& Text, const FLinearColor& Color)
{
	if (Badge.Box && Badge.Text)
	{
		Badge.Text->SetText(Text);
		Badge.Box->SetBrush(STStyle::Rounded(Color, 9.f));
	}
}

USTPanel::FStat USTPanel::MakeStat(UPanelWidget* Parent, const FText& Label, const FMargin& Spacing)
{
	FStat Stat;
	UVerticalBox* Card = MakeCard(Parent, STStyle::Header, FMargin(8.f, 6.f), Spacing);
	Stat.Value = MakeTinted(Card, FText::FromString(TEXT("-")), 16, STStyle::Text, true, false);
	Stat.Label = MakeTinted(Card, Label, 9, STStyle::Muted, false, true);
	return Stat;
}

USTBar* USTPanel::MakeBar(UPanelWidget* Parent, float Height, const FMargin& Spacing)
{
	USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Size->SetHeightOverride(Height);
	USTBar* Bar = WidgetTree->ConstructWidget<USTBar>(USTBar::StaticClass());
	Size->AddChild(Bar);
	if (Parent)
	{
		Parent->AddChild(Size);
		SetSlotPadding(Size, Spacing);
	}
	return Bar;
}

USTLogo* USTPanel::MakeLogo(UPanelWidget* Parent, float Size)
{
	USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Box->SetWidthOverride(Size);
	Box->SetHeightOverride(Size);
	USTLogo* Logo = WidgetTree->ConstructWidget<USTLogo>(USTLogo::StaticClass());
	Box->AddChild(Logo);
	if (Parent)
	{
		Parent->AddChild(Box);
		SetVAlignCenter(Box);
	}
	return Logo;
}

UImage* USTPanel::MakeIcon(UPanelWidget* Parent, float Size, const FMargin& Spacing)
{
	USizeBox* Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Box->SetWidthOverride(Size);
	Box->SetHeightOverride(Size);
	UImage* Image = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
	Image->SetVisibility(ESlateVisibility::Hidden);
	Box->AddChild(Image);
	if (Parent)
	{
		Parent->AddChild(Box);
		SetSlotPadding(Box, Spacing);
		SetVAlignCenter(Box);
	}
	return Image;
}

void USTPanel::SetIcon(UImage* Image, UClass* Item)
{
	if (!Image)
	{
		return;
	}
	UTexture2D* Texture = Item && Item->IsChildOf(UFGItemDescriptor::StaticClass()) ? UFGItemDescriptor::GetSmallIcon(TSubclassOf<UFGItemDescriptor>(Item)) : nullptr;
	if (Texture)
	{
		Image->SetBrushFromTexture(Texture, false);
		Image->SetVisibility(ESlateVisibility::HitTestInvisible);
	}
	else
	{
		Image->SetVisibility(ESlateVisibility::Hidden);
	}
}

UHorizontalBox* USTPanel::MakeTitleBar(UPanelWidget* Parent, const FText& Title, float LogoSize)
{
	UBorder* Border = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Border->SetBrush(STStyle::Rounded(STStyle::Header, 3.f));
	Border->SetPadding(FMargin(10.f, 7.f));
	UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Border->SetContent(Stack);
	UHorizontalBox* Bar = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Stack->AddChildToVerticalBox(Bar);
	MakeLogo(Bar, LogoSize);
	UVerticalBox* Words = MakeVBox(Bar, FMargin(8.f, 0.f, 0.f, 0.f));
	SetFill(Words);
	SetVAlignCenter(Words);
	MakeTinted(Words, FText::FromString(TEXT("SMARTTRAINS")), 10, STStyle::Orange, true, false);
	if (!Title.IsEmpty())
	{
		MakeTinted(Words, Title, 14, STStyle::Text, true, true);
	}
	// The orange rule under the bar.
	UBorder* Rule = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Rule->SetBrush(STStyle::Rounded(STStyle::Orange, 1.f));
	Rule->SetPadding(FMargin(0.f, 1.f));
	Stack->AddChildToVerticalBox(Rule);
	SetSlotPadding(Rule, FMargin(0.f, 7.f, 0.f, 0.f));
	if (Parent)
	{
		Parent->AddChild(Border);
		SetSlotPadding(Border, FMargin(0.f, 0.f, 0.f, 6.f));
	}
	return Bar;
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
