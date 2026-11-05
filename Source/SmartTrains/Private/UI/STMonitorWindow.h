#pragma once

#include "CoreMinimal.h"
#include "UI/STPanel.h"
#include "STMonitorWindow.generated.h"

class APlayerController;
class UTextBlock;
class UVerticalBox;
struct FSTMonitorView;

// Throughput monitor opened by "/st monitor": per item what limits deliveries (supply, trains, fit, congestion),
// congested stations, platforms that need a look and why idle trains wait. A popup on the viewport, refreshed every
// second; Close or Escape closes it.
UCLASS()
class USTMonitorWindow : public USTPanel
{
	GENERATED_BODY()

public:
	// Opens the window for Controller's local player shortly after the chat closes.
	static void Open(APlayerController* Controller);

	void Close();

protected:
	virtual UWidget* WrapRoot(UVerticalBox* Root) override;
	virtual void BuildContent(UVerticalBox* Root) override;
	virtual void OnHidden() override;
	virtual void RequestView(USTRemoteCallObject* RCO) override;
	virtual void Refresh(USTRemoteCallObject* RCO) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

private:
	static void Create(APlayerController* Controller);

	void ShowView(const FSTMonitorView& View);
	void AddSection(const FText& Heading, const TArray<FString>& Lines);
	void RestoreGameInput();

	UPROPERTY()
	TObjectPtr<UVerticalBox> mContent;

	TWeakObjectPtr<APlayerController> mController;
	bool bInputTaken = false;
	// The text of the view shown, to rebuild only when it changes.
	FString mShown;

	static TWeakObjectPtr<USTMonitorWindow> sOpen;
};
