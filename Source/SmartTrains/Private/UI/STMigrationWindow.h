#pragma once

#include "CoreMinimal.h"
#include "UI/STPanel.h"
#include "STMigrationWindow.generated.h"

class APlayerController;
class UBorder;
class UFGItemDescriptor;
class UHorizontalBox;
class USTButton;
class USTRow;
class USTTextInput;
enum class ESTRole : uint8;
class UTextBlock;
class UVerticalBox;
struct FSTMigrationPlatformRow;
struct FSTMigrationRow;
struct FSTMigrationView;

// Migration window opened by "/st migrate". One toolbar (Preview, Apply ticked, Undo, Rename stations, and with beta
// tools Test copy and Backup) above three tabs:
// - Stations: a table, one row per station (tick, station, new name while Rename stations is on, platforms, status);
//   clicking a station opens its platforms with role, item icons and confidence.
// - Trains: one row per train, ticked to be managed.
// - Result: what the last step said.
// A full-screen popup; Close or Escape closes it.
UCLASS()
class USTMigrationWindow : public USTPanel
{
	GENERATED_BODY()

public:
	// Opens the window for Controller's local player shortly after the chat closes, or brings an open one up to date.
	static void Open(APlayerController* Controller);

	void Close();

protected:
	virtual UWidget* WrapRoot(UVerticalBox* Root) override;
	virtual void BuildContent(UVerticalBox* Root) override;
	virtual bool OnShown() override;
	virtual void OnHidden() override;
	virtual void Refresh(USTRemoteCallObject* RCO) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

private:
	enum class ETab : uint8
	{
		Stations,
		Trains,
		Result,
	};
	enum class EFilter : uint8
	{
		All,
		NeedsLook,
		Ticked,
		NotTicked,
	};

	static void Create(APlayerController* Controller);

	void SendMigration(uint8 Step, const TArray<FString>& Selection = TArray<FString>());
	// Preview with the trace limit from the field.
	void SendPreview();
	void ShowRenameMode();
	void ApplyTicked();
	void RestoreGameInput();

	void SelectTab(ETab Tab);
	void UpdateTabs(const FSTMigrationView& View);
	// Rebuilds the open tab from the latest view.
	void ShowTab();
	void ShowStations(const FSTMigrationView& View);
	void ShowTrains(const FSTMigrationView& View);
	void ShowResult(const FSTMigrationView& View);
	void AddFilterBar(bool bStations, int32 Shown, int32 Total);
	// Column headings of the stations table.
	void AddStationHeadings();
	void AddStation(const FSTMigrationRow& Row);
	// Sends a platform's role and items set by hand, and ticks its station.
	// The item picker under a platform being changed: a search field and every item that fits, its items first.
	void AddTrain(const FSTMigrationRow& Row);
	// Ticks or unticks every row of a kind that the current filter shows.
	void SetAllTicks(bool bStations, bool bTicked);
	bool Passes(const FSTMigrationRow& Row, EFilter Filter) const;

	// Table pieces. Width 0 fills the rest of the line.
	UHorizontalBox* AddLine(const FMargin& Spacing = FMargin(0.f, 3.f));
	void AddCell(UHorizontalBox* Line, UWidget* Cell, float Width);
	UTextBlock* MakeCell(const FString& Text, int32 Size, const FLinearColor& Color, bool bWrap = false);
	UWidget* MakeIcon(TSubclassOf<UFGItemDescriptor> Item, float Size);
	void AddDivider();
	void AddHeading(const FText& Text);
	void AddNote(const FString& Text, const FLinearColor& Color, float Indent = 0.f);

	UPROPERTY()
	TObjectPtr<USTButton> mApply;

	UPROPERTY()
	TObjectPtr<USTButton> mUndo;

	UPROPERTY()
	TObjectPtr<UWidget> mBetaRow;

	UPROPERTY()
	TObjectPtr<UTextBlock> mStatus;

	UPROPERTY()
	TObjectPtr<UVerticalBox> mContent;

	UPROPERTY()
	TObjectPtr<USTButton> mRenameMode;

	struct FTabWidgets
	{
		TWeakObjectPtr<USTButton> Button;
		TWeakObjectPtr<UBorder> Underline;
	};
	TArray<FTabWidgets> mTabs;

	// Ticks per row, keyed "S3" / "T5", and the stations opened to their platforms.
	TMap<FString, bool> mTicked;
	TSet<FString> mExpanded;

	// The platform whose items are being picked ("S3/2": station row key and platform number), the picker's search
	// text and its rows with their item names.

	// Trace limit and items mode for this session's previews; the open tab and filters.
	// Apply also renames the ticked stations (station set-up names).
	static bool sRename;
	static ETab sTab;
	static EFilter sStationFilter;
	static EFilter sTrainFilter;

	TWeakObjectPtr<APlayerController> mController;
	bool bInputTaken = false;
	// Applying without a test copy or backup takes a second click.
	bool bApplyArmed = false;
	// A preview was asked for and hasn't come back yet (FPlatformTime seconds when asked; 0: none).
	double mPreviewSentAt = 0.0;
	int32 mShownSerial = -1;

	static TWeakObjectPtr<USTMigrationWindow> sOpen;
};
