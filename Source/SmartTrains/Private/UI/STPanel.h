#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/SlateColor.h"
#include "STPanel.generated.h"

class UBorder;
class UHorizontalBox;
class UImage;
class UPanelWidget;
class USTBar;
class USTButton;
class USTLogo;
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

	//~ The SmartTrains look (UI/STStyle.h).
	// A box with rounded corners in Color holding a vertical box (returned), Padding inside it.
	UVerticalBox* MakeCard(UPanelWidget* Parent, const FLinearColor& Color, const FMargin& Inner = FMargin(8.f), const FMargin& Spacing = FMargin(0.f, 3.f),
		UBorder** OutBorder = nullptr);
	UVerticalBox* MakeVBox(UPanelWidget* Parent, const FMargin& Spacing = FMargin(0.f));
	UHorizontalBox* MakeHBox(UPanelWidget* Parent, const FMargin& Spacing = FMargin(0.f));
	// A small muted caps label over a section ("PATTERN").
	UTextBlock* MakeLabel(UPanelWidget* Parent, const FText& Text, const FMargin& Spacing = FMargin(0.f, 6.f, 0.f, 3.f));
	// A text in Color (kept when the panel copies the menu's font), bold or not.
	UTextBlock* MakeTinted(UPanelWidget* Parent, const FText& Text, int32 Size, const FLinearColor& Color, bool bBold = false, bool bWrap = true,
		const FMargin& Spacing = FMargin(0.f));
	// Recolours a text made by the panel, keeping that colour from then on.
	void Tint(UTextBlock* Text, const FLinearColor& Color);

	// A pill with a dark text on a colour (a state).
	struct FBadge
	{
		UBorder* Box = nullptr;
		UTextBlock* Text = nullptr;
	};
	FBadge MakeBadge(UPanelWidget* Parent, const FMargin& Spacing = FMargin(0.f));
	static void SetBadge(const FBadge& Badge, const FText& Text, const FLinearColor& Color);

	// A tile with a big value over a muted label.
	struct FStat
	{
		UTextBlock* Value = nullptr;
		UTextBlock* Label = nullptr;
	};
	FStat MakeStat(UPanelWidget* Parent, const FText& Label, const FMargin& Spacing = FMargin(0.f));

	// A fill bar Height tall that fills its slot's width.
	USTBar* MakeBar(UPanelWidget* Parent, float Height = 6.f, const FMargin& Spacing = FMargin(0.f, 3.f));
	USTLogo* MakeLogo(UPanelWidget* Parent, float Size);
	// An item icon Size square; SetIcon fills it (nothing shown for no item).
	UImage* MakeIcon(UPanelWidget* Parent, float Size, const FMargin& Spacing = FMargin(0.f));
	static void SetIcon(UImage* Image, UClass* Item);
	// The panel's title bar: the logo, "SMARTTRAINS" and Title; returns the bar so a caller can add to its right.
	UHorizontalBox* MakeTitleBar(UPanelWidget* Parent, const FText& Title, float LogoSize = 22.f);

	// Slot settings for a child of a vertical, horizontal, scroll or wrap box.
	static void SetSlotPadding(UWidget* Child, const FMargin& Padding);
	// Lets Child take Fill of the box's spare room (horizontal or vertical box), else size to its content.
	static void SetFill(UWidget* Child, float Fill = 1.f);
	static void SetVAlignCenter(UWidget* Child);

	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	// Shows the server's last message for a few seconds, red if it is an error.
	virtual void ShowServerMessage(USTRemoteCallObject* RCO);

	// Requests and refreshes right away (after a click).
	void RefreshSoon();

	// Font and colour for texts made outside MakeText (rows), at Size.
	FSlateFontInfo GetFont(int32 Size, bool bBold = false) const;
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
		bool bBold = false;
		bool bOwnColor = false;
		FLinearColor Color = FLinearColor::White;
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
	// Time since shown, for the fade-in. [s]
	float mShownFor = 0.f;
};
