#pragma once

#include "CoreMinimal.h"
#include "UI/STPanel.h"
#include "UI/STPlatformPanel.h"
#include "STPlatformWindow.generated.h"

class APlayerController;
class USTButton;
class UTextBlock;

// The platform settings (USTPlatformPanel) in a window of their own, opened from the SmartTrains section of a
// freight or fluid platform menu. It sits over the game without changing the input mode, so the platform menu
// behind it keeps working; Close or Escape closes it, and so does closing the platform menu.
UCLASS()
class USTPlatformWindow : public USTPlatformPanel
{
	GENERATED_BODY()

public:
	// Opens the window for the platform of Menu, or brings an open one to the front.
	static void Open(UUserWidget* Menu);
	// Closes the window if it shows Menu's platform.
	static void CloseFor(const UUserWidget* Menu);

	void Close();

protected:
	virtual UWidget* WrapRoot(UVerticalBox* Root) override;
	virtual void OnHidden() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual bool ShowsHeading() const override { return false; }

private:
	static TWeakObjectPtr<USTPlatformWindow> sOpen;
};

// SmartTrains section beside the vanilla freight and fluid platform menus: the platform's role and items in one
// line, the readout's first reason, and a button that opens the settings window.
UCLASS()
class USTPlatformLauncher : public USTPanel
{
	GENERATED_BODY()

protected:
	virtual void BuildContent(UVerticalBox* Root) override;
	virtual UWidget* WrapRoot(UVerticalBox* Root) override;
	virtual bool OnShown() override;
	virtual void OnHidden() override;
	virtual void RequestView(USTRemoteCallObject* RCO) override;
	virtual void Refresh(USTRemoteCallObject* RCO) override;
	virtual FName GetStyleSource() const override { return TEXT("StationSettingText"); }

private:
	UPROPERTY()
	TObjectPtr<UTextBlock> mSummary;

	UPROPERTY()
	TObjectPtr<UTextBlock> mStatus;
};
