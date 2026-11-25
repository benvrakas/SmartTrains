#pragma once

#include "CoreMinimal.h"
#include "UI/STTrainPanel.h"
#include "STRideHud.generated.h"

class AFGTrain;
class UTextBlock;

// The card at the top right of the screen while the local player rides a managed train: its pattern with fill bars,
// its loop's stops and how many trains share the pattern. It never takes input (Docs/Architecture.md, "Ride HUD").
UCLASS()
class USTRideHud : public USTTrainDisplay
{
	GENERATED_BODY()

public:
	// Starts the client check that shows the card while the local player rides a train (not on a dedicated server).
	static void Install();

	void SetTrain(AFGTrain* InTrain) { mTrain = InTrain; }

protected:
	virtual UWidget* WrapRoot(UVerticalBox* Root) override;
	virtual void BuildContent(UVerticalBox* Root) override;
	virtual void Refresh(USTRemoteCallObject* RCO) override;
	virtual AFGTrain* GetTrain() const override { return mTrain.Get(); }

private:
	TWeakObjectPtr<AFGTrain> mTrain;

	// A hidden vanilla label the card copies the game's font from.
	UPROPERTY()
	TObjectPtr<UUserWidget> mFontSource;

	UPROPERTY()
	TObjectPtr<UTextBlock> mTitle;
};
