#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/SlateColor.h"
#include "STPanel.generated.h"

class UPanelWidget;
class USTButton;
class USTRemoteCallObject;
class USTRow;
class USTTextInput;
class UTextBlock;
class UVerticalBox;

// Base of the panels SmartTrains adds to vanilla menus. Builds its own widget tree, asks the server for a readout
// every second while shown, and refreshes when one arrives (Docs/Architecture.md, "UI").
UCLASS(Abstract)
class USTPanel : public UUserWidget
{
	GENERATED_BODY()

public:
	// For a panel in a window of its own: the vanilla menu it belongs to (for its object and its font). Set before the
	// panel is shown.
	void SetMenu(UUserWidget* Menu) { mMenuOverride = Menu; }

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

	// Adds the panel's widgets to Root; called once.
	virtual void BuildContent(UVerticalBox* Root) {}

	// Returns the tree's root: Root inside a transparent border with GetOuterPadding, or a frame of the panel's own.
	virtual UWidget* WrapRoot(UVerticalBox* Root);

	// Space around the panel inside its vanilla parent. Hooks into vertical boxes can't set slot padding (see
	// Docs/Debugging.md, "SML slot setup"), so the panels keep their distance themselves.
	virtual FMargin GetOuterPadding() const { return FMargin(0.f); }

	// Called after construction once the menu is known; returns false to stay hidden (e.g. nothing to show for).
	virtual bool OnShown() { return true; }
	virtual void OnHidden() {}

	// Asks the server for this panel's readout.
	virtual void RequestView(USTRemoteCallObject* RCO) {}

	// Updates the widgets from the latest readout.
	virtual void Refresh(USTRemoteCallObject* RCO) {}

	// Name of the vanilla TextBlock in the menu whose font the panel's texts copy.
	virtual FName GetStyleSource() const { return NAME_None; }

	// The vanilla menu the panel sits in.
	UUserWidget* GetMenu() const;

	// Widget factories; texts get the menu's font once it is known.
	UTextBlock* MakeText(UPanelWidget* Parent, const FText& Text, int32 Size = 11, bool bWrap = true, const FMargin& Spacing = FMargin(0.f, 2.f));
	USTButton* MakeButton(UPanelWidget* Parent, const FText& Text, TFunction<void()> OnClick, const FMargin& Spacing = FMargin(0.f, 2.f));
	USTTextInput* MakeInput(UPanelWidget* Parent, const FText& Hint, const FMargin& Spacing = FMargin(0.f, 2.f));
	USTRow* MakeRow(UPanelWidget* Parent, const FText& Text, TFunction<void()> OnClick);
	// Adds a vanilla menu heading, or a large text if the game's heading is missing.
	UWidget* MakeHeading(UPanelWidget* Parent, const FText& Text, const FMargin& Spacing = FMargin(0.f, 8.f, 0.f, 4.f));

	// Shows Lines in Text, one per line.
	static void SetLines(UTextBlock* Text, const TArray<FString>& Lines);

	// Shows the server's last message for a few seconds, red if it is an error.
	virtual void ShowServerMessage(USTRemoteCallObject* RCO);

	// Requests and refreshes right away (after a click).
	void RefreshSoon();

	// Font and colour for texts made outside MakeText (rows), at Size.
	FSlateFontInfo GetFont(int32 Size) const;
	FSlateColor GetTextColor() const { return mTextColor; }

	UPROPERTY()
	TObjectPtr<UVerticalBox> mRoot;

	// Line under the panel for server messages.
	UPROPERTY()
	TObjectPtr<UTextBlock> mMessage;

private:
	void BuildTree();
	void ApplyStyle();
	void Tick();

	struct FStyledText
	{
		TWeakObjectPtr<UTextBlock> Text;
		int32 Size = 11;
	};
	TArray<FStyledText> mStyled;
	FSlateFontInfo mFont;
	FSlateColor mTextColor = FSlateColor(FLinearColor::White);
	bool bHasStyle = false;

	TWeakObjectPtr<UUserWidget> mMenuOverride;
	FTimerHandle mRequestTimer;
	FTimerHandle mRefreshTimer;
	uint32 mShownRevision = MAX_uint32;
	bool bActive = false;
	bool bWarnedNoRCO = false;
};
