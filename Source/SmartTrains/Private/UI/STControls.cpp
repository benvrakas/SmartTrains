#include "UI/STControls.h"
#include "Components/ButtonSlot.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateNoResource.h"
#include "Components/Button.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "SmartTrains.h"
#include "UI/STWidgetUtils.h"

//~ USTButton

void USTButton::SetText(const FText& Text)
{
	mText = Text;
	if (bFitToSlot)
	{
		SetToolTipText(Text);
	}
	if (mVanillaButton)
	{
		STWidgetUtils::SetWidgetText(mVanillaButton, Text);
	}
	else if (mFallbackText)
	{
		mFallbackText->SetText(Text);
	}
}

void USTButton::SetFixedWidth(float Width)
{
	BuildTree();
	if (Width > 0.f)
	{
		mFrame->SetWidthOverride(Width);
	}
	else
	{
		mFrame->ClearWidthOverride();
	}
}

void USTButton::SetFitToSlot(float MaxWidth)
{
	BuildTree();
	bFitToSlot = true;
	mFrame->SetMaxDesiredWidth(MaxWidth);
	UTextBlock* Text = mVanillaButton ? Cast<UTextBlock>(STWidgetUtils::GetObjectProperty(mVanillaButton, TEXT("mTextObject"))) : mFallbackText.Get();
	if (!Text)
	{
		UE_LOG(LogSmartTrains, Warning, TEXT("Button has no text block to shorten; long text may be cut off"));
		return;
	}
	Text->SetAutoWrapText(false);
	Text->SetClipping(EWidgetClipping::ClipToBounds);
	Text->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	SetText(mText);
}

TSharedRef<SWidget> USTButton::RebuildWidget()
{
	BuildTree();
	return Super::RebuildWidget();
}

void USTButton::BuildTree()
{
	if (!WidgetTree)
	{
		WidgetTree = NewObject<UWidgetTree>(this, TEXT("WidgetTree"), RF_Transient);
	}
	if (WidgetTree->RootWidget)
	{
		return;
	}
	mFrame = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	WidgetTree->RootWidget = mFrame;
	mVanillaButton = STWidgetUtils::CreateVanillaWidget(WidgetTree, STWidgetUtils::StandardButtonClass);
	UButton* Inner = mVanillaButton ? STWidgetUtils::GetInnerButton(mVanillaButton) : nullptr;
	if (mVanillaButton && Inner)
	{
		mFrame->AddChild(mVanillaButton);
		Inner->OnClicked.AddUniqueDynamic(this, &USTButton::HandleClicked);
	}
	else
	{
		if (mVanillaButton)
		{
			UE_LOG(LogSmartTrains, Warning, TEXT("Vanilla button has no mButton; using a plain button"));
			mVanillaButton = nullptr;
		}
		UButton* Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
		mFallbackText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		FSlateFontInfo Font = mFallbackText->GetFont();
		Font.Size = 12;
		mFallbackText->SetFont(Font);
		Button->AddChild(mFallbackText);
		Button->OnClicked.AddUniqueDynamic(this, &USTButton::HandleClicked);
		mFrame->AddChild(Button);
	}
	SetText(mText);
}

FReply USTButton::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (InMouseEvent.GetEffectingButton() == EKeys::RightMouseButton && OnRightClick)
	{
		UE_LOG(LogSmartTrains, Verbose, TEXT("Button \"%s\" right-clicked"), *mText.ToString());
		OnRightClick();
		return FReply::Handled();
	}
	return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
}

void USTButton::HandleClicked()
{
	UE_LOG(LogSmartTrains, Verbose, TEXT("Button \"%s\" clicked"), *mText.ToString());
	if (OnClick)
	{
		OnClick();
	}
}

//~ USTTextInput

TSharedRef<SWidget> USTTextInput::RebuildWidget()
{
	BuildTree();
	return Super::RebuildWidget();
}

void USTTextInput::BuildTree()
{
	if (!WidgetTree)
	{
		WidgetTree = NewObject<UWidgetTree>(this, TEXT("WidgetTree"), RF_Transient);
	}
	if (WidgetTree->RootWidget)
	{
		return;
	}
	mVanillaBox = STWidgetUtils::CreateVanillaWidget(WidgetTree, STWidgetUtils::InputBoxClass);
	mVanillaText = mVanillaBox ? Cast<UEditableText>(STWidgetUtils::GetObjectProperty(mVanillaBox, TEXT("mInputBox"))) : nullptr;
	if (mVanillaBox && mVanillaText)
	{
		WidgetTree->RootWidget = mVanillaBox;
		mVanillaText->OnTextCommitted.AddUniqueDynamic(this, &USTTextInput::HandleCommitted);
		mVanillaText->OnTextChanged.AddUniqueDynamic(this, &USTTextInput::HandleChanged);
	}
	else
	{
		UE_LOG(LogSmartTrains, Warning, TEXT("Vanilla input box %s; using a plain text box"), mVanillaBox ? TEXT("has no mInputBox") : TEXT("missing"));
		mVanillaBox = nullptr;
		mFallbackBox = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass());
		mFallbackBox->OnTextCommitted.AddUniqueDynamic(this, &USTTextInput::HandleCommitted);
		mFallbackBox->OnTextChanged.AddUniqueDynamic(this, &USTTextInput::HandleChanged);
		WidgetTree->RootWidget = mFallbackBox;
	}
	SetHint(mHint);
}

FString USTTextInput::GetText() const
{
	if (mVanillaText)
	{
		return mVanillaText->GetText().ToString();
	}
	return mFallbackBox ? mFallbackBox->GetText().ToString() : FString();
}

void USTTextInput::SetTextIfIdle(const FString& Text)
{
	BuildTree();
	if (IsEditing() || GetText() == Text)
	{
		return;
	}
	if (mVanillaText)
	{
		mVanillaText->SetText(FText::FromString(Text));
		// The game's box shows its own copy of the text over the field while it isn't focused.
		STWidgetUtils::CallTextFunction(mVanillaBox, TEXT("UpdateText"), FText::FromString(Text));
	}
	else if (mFallbackBox)
	{
		mFallbackBox->SetText(FText::FromString(Text));
	}
}

void USTTextInput::SetHint(const FText& Hint)
{
	mHint = Hint;
	if (mVanillaBox)
	{
		STWidgetUtils::CallTextFunction(mVanillaBox, TEXT("SetPreviewText"), Hint);
	}
	else if (mFallbackBox)
	{
		mFallbackBox->SetHintText(Hint);
	}
}

bool USTTextInput::IsEditing() const
{
	return (mVanillaText && mVanillaText->HasKeyboardFocus()) || (mFallbackBox && mFallbackBox->HasKeyboardFocus());
}

void USTTextInput::HandleCommitted(const FText& Text, ETextCommit::Type Method)
{
	if (Method == ETextCommit::OnCleared)
	{
		return;
	}
	UE_LOG(LogSmartTrains, Verbose, TEXT("Text input committed: \"%s\" (%d)"), *Text.ToString(), static_cast<int32>(Method));
	if (OnCommitted)
	{
		OnCommitted(Text.ToString());
	}
}

void USTTextInput::HandleChanged(const FText& Text)
{
	if (OnChanged)
	{
		OnChanged(Text.ToString());
	}
}

//~ USTRow

TSharedRef<SWidget> USTRow::RebuildWidget()
{
	BuildTree();
	return Super::RebuildWidget();
}

void USTRow::BuildTree()
{
	if (!WidgetTree)
	{
		WidgetTree = NewObject<UWidgetTree>(this, TEXT("WidgetTree"), RF_Transient);
	}
	if (WidgetTree->RootWidget)
	{
		return;
	}
	mButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
	FButtonStyle Style;
	Style.SetNormal(FSlateNoResource());
	Style.SetHovered(FSlateColorBrush(FLinearColor(1.f, 1.f, 1.f, 0.08f)));
	Style.SetPressed(FSlateColorBrush(FLinearColor(1.f, 1.f, 1.f, 0.16f)));
	Style.SetDisabled(FSlateNoResource());
	Style.SetNormalPadding(FMargin(4.f, 2.f));
	Style.SetPressedPadding(FMargin(4.f, 2.f));
	mButton->SetStyle(Style);
	mButton->OnClicked.AddUniqueDynamic(this, &USTRow::HandleClicked);
	WidgetTree->RootWidget = mButton;

	UHorizontalBox* Line = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	// A button centres its content by default; rows read from the left.
	if (UButtonSlot* LineSlot = Cast<UButtonSlot>(mButton->AddChild(Line)))
	{
		LineSlot->SetHorizontalAlignment(HAlign_Fill);
		LineSlot->SetVerticalAlignment(VAlign_Center);
	}
	mIconBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	mIconBox->SetWidthOverride(24.f);
	mIconBox->SetHeightOverride(24.f);
	mIconBox->SetVisibility(ESlateVisibility::Collapsed);
	mIcon = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
	mIconBox->AddChild(mIcon);
	if (UHorizontalBoxSlot* IconSlot = Line->AddChildToHorizontalBox(mIconBox))
	{
		IconSlot->SetPadding(FMargin(0.f, 0.f, 6.f, 0.f));
		IconSlot->SetVerticalAlignment(VAlign_Center);
	}
	mText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	mText->SetAutoWrapText(true);
	mText->SetJustification(ETextJustify::Left);
	FSlateFontInfo Font = mText->GetFont();
	Font.Size = 11;
	mText->SetFont(Font);
	if (UHorizontalBoxSlot* TextSlot = Line->AddChildToHorizontalBox(mText))
	{
		TextSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		TextSlot->SetVerticalAlignment(VAlign_Center);
	}
}

void USTRow::SetText(const FText& Text)
{
	BuildTree();
	mText->SetText(Text);
}

void USTRow::SetIcon(UTexture2D* Icon)
{
	BuildTree();
	if (Icon)
	{
		mIcon->SetBrushFromTexture(Icon);
	}
	mIconBox->SetVisibility(Icon ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
}

void USTRow::SetStyle(const FSlateFontInfo& Font, const FSlateColor& Color)
{
	BuildTree();
	mText->SetFont(Font);
	mText->SetColorAndOpacity(Color);
}

void USTRow::HandleClicked()
{
	UE_LOG(LogSmartTrains, Verbose, TEXT("Row \"%s\" clicked"), *mText->GetText().ToString());
	if (OnClick)
	{
		OnClick();
	}
}
