#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/SlateColor.h"
#include "Types/SlateEnums.h"
#include "STControls.generated.h"

class UButton;
class UEditableText;
class UEditableTextBox;
class UImage;
class USizeBox;
class UTextBlock;
class UTexture2D;

// A rounded text button in the SmartTrains style (STStyle) that reports left and right clicks.
UCLASS()
class USTButton : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetText(const FText& Text);

	void SetFont(const FSlateFontInfo& Font);

	// Makes the button exactly Width wide; 0 sizes it to its text.
	void SetFixedWidth(float Width);

	// The button asks for no more width than MaxWidth and takes what its slot gives it; text that doesn't fit stays on
	// one line and ends in "...", in full in the tooltip.
	void SetFitToSlot(float MaxWidth);

	TFunction<void()> OnClick;

	// Called on a right click; unset means a right click does nothing.
	TFunction<void()> OnRightClick;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

private:
	void BuildTree();

	UFUNCTION()
	void HandleClicked();

	UPROPERTY()
	TObjectPtr<USizeBox> mFrame;

	UPROPERTY()
	TObjectPtr<UTextBlock> mLabel;

	FText mText;
	bool bFitToSlot = false;
};

// A one-line text field: the game's input box, or a plain one if it is missing. Reports Enter and focus loss.
UCLASS()
class USTTextInput : public UUserWidget
{
	GENERATED_BODY()

public:
	FString GetText() const;

	// Shows Text unless the player is typing in the field.
	void SetTextIfIdle(const FString& Text);

	// Grey text shown while the field is empty.
	void SetHint(const FText& Hint);

	// True while the field has keyboard focus.
	bool IsEditing() const;

	// Called with the text when the player presses Enter or leaves the field.
	TFunction<void(const FString&)> OnCommitted;

	// Called on every change while typing.
	TFunction<void(const FString&)> OnChanged;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	void BuildTree();

	UFUNCTION()
	void HandleCommitted(const FText& Text, ETextCommit::Type Method);

	UFUNCTION()
	void HandleChanged(const FText& Text);

	UPROPERTY()
	TObjectPtr<UUserWidget> mVanillaBox;

	UPROPERTY()
	TObjectPtr<UEditableText> mVanillaText;

	UPROPERTY()
	TObjectPtr<UEditableTextBox> mFallbackBox;

	FText mHint;
};

// A flat, full-width clickable row: an optional icon and text that wraps. Used for lists (items, migration rows).
UCLASS()
class USTRow : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetText(const FText& Text);
	void SetIcon(UTexture2D* Icon);
	void SetStyle(const FSlateFontInfo& Font, const FSlateColor& Color);

	TFunction<void()> OnClick;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	void BuildTree();

	UFUNCTION()
	void HandleClicked();

	UPROPERTY()
	TObjectPtr<UButton> mButton;

	UPROPERTY()
	TObjectPtr<UImage> mIcon;

	UPROPERTY()
	TObjectPtr<USizeBox> mIconBox;

	UPROPERTY()
	TObjectPtr<UTextBlock> mText;
};

// One choice of a row of choices (a platform's role): flat, rounded, orange when selected.
UCLASS()
class USTSegment : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetText(const FText& Text);
	void SetSelected(bool bInSelected);
	void SetFont(const FSlateFontInfo& Font);

	TFunction<void()> OnClick;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	void BuildTree();
	void ApplyLook();

	UFUNCTION()
	void HandleClicked();

	UPROPERTY()
	TObjectPtr<UButton> mButton;

	UPROPERTY()
	TObjectPtr<UTextBlock> mText;

	bool bSelected = false;
};

// An item as a square tile showing only its icon, its name in the tooltip (the item picker's grid).
UCLASS()
class USTIconTile : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetItem(UTexture2D* Icon, const FText& Name);

	TFunction<void()> OnClick;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	void BuildTree();

	UFUNCTION()
	void HandleClicked();

	UPROPERTY()
	TObjectPtr<UButton> mButton;

	UPROPERTY()
	TObjectPtr<UImage> mIcon;
};
