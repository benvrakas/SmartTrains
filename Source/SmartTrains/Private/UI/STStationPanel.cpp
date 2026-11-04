#include "UI/STStationPanel.h"
#include "Blueprint/WidgetTree.h"
#include "Buildables/FGBuildableRailroadStation.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "FGTrainStationIdentifier.h"
#include "SmartTrains.h"
#include "STLayout.h"
#include "STRemoteCallObject.h"
#include "UI/STControls.h"
#include "UI/STWidgetUtils.h"

#define LOCTEXT_NAMESPACE "SmartTrains"

namespace
{
	FString RowKey(const FSTMigrationRow& Row)
	{
		return FString::Printf(TEXT("%s%d"), Row.bStation ? TEXT("S") : TEXT("T"), Row.Number);
	}

	void AddFilled(UHorizontalBox* Box, UWidget* Child, const FMargin& Padding = FMargin(0.f, 0.f, 4.f, 0.f))
	{
		if (UHorizontalBoxSlot* Slot = Box->AddChildToHorizontalBox(Child))
		{
			Slot->SetPadding(Padding);
			Slot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}
	}
}

void USTStationPanel::BuildContent(UVerticalBox* Root)
{
	MakeHeading(Root, LOCTEXT("StationHeading", "SmartTrains"));
	mDepot = MakeButton(Root, LOCTEXT("DepotOff", "Depot: off"), [this]() { SendStation(!bDepot, mPoolValue); });
	MakeText(Root, LOCTEXT("DepotHint", "Idle managed trains park at the nearest depot."), 10);
	MakeText(Root, LOCTEXT("StationPoolLabel", "Pool tag (only trains with the same tag serve this station):"), 10, true, FMargin(0.f, 6.f, 0.f, 2.f));
	mPool = MakeInput(Root, LOCTEXT("StationPoolHint", "no pool"));
	mPool->OnCommitted = [this](const FString& Text) { SendStation(bDepot, Text.TrimStartAndEnd()); };
	mLines = MakeText(Root, LOCTEXT("Loading", "Asking the server..."), 10, true, FMargin(0.f, 6.f, 0.f, 0.f));
	mNetwork = MakeText(Root, FText::GetEmpty(), 10, true, FMargin(0.f, 6.f, 0.f, 0.f));

	mMigrationToggle = MakeButton(Root, LOCTEXT("ShowMigration", "Migration tool..."), [this]() { ToggleMigration(); }, FMargin(0.f, 10.f, 0.f, 2.f));
	mMigration = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Root->AddChildToVerticalBox(mMigration);
	mMigration->SetVisibility(ESlateVisibility::Collapsed);
	MakeText(mMigration, LOCTEXT("MigrationHint", "Proposes platform roles and managed trains from DynamicTimeTable groups, platform load/unload "
		"settings, belts, stock and timetables. Trains that only shuttle between two stations outside the groups are left alone. "
		"Untick what you don't want, then apply. Undo puts back the last apply."), 10);
	UHorizontalBox* Steps = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	mMigration->AddChildToVerticalBox(Steps)->SetPadding(FMargin(0.f, 4.f));
	AddFilled(Steps, MakeButton(nullptr, LOCTEXT("Preview", "Preview"), [this]() { SendMigration(static_cast<uint8>(ESTMigrationStep::Preview)); }));
	mApply = MakeButton(nullptr, LOCTEXT("Apply", "Apply ticked"), [this]() { ApplyTicked(); });
	AddFilled(Steps, mApply);
	mUndo = MakeButton(nullptr, LOCTEXT("Undo", "Undo"), [this]() { SendMigration(static_cast<uint8>(ESTMigrationStep::Undo)); });
	AddFilled(Steps, mUndo, FMargin(0.f));
	UHorizontalBox* Beta = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	mMigration->AddChildToVerticalBox(Beta)->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
	AddFilled(Beta, MakeButton(nullptr, LOCTEXT("TestCopy", "Save a test copy"), [this]() { SendMigration(static_cast<uint8>(ESTMigrationStep::TestCopy)); }));
	AddFilled(Beta, MakeButton(nullptr, LOCTEXT("Backup", "Save a backup"), [this]() { SendMigration(static_cast<uint8>(ESTMigrationStep::Backup)); }), FMargin(0.f));
	mBetaRow = Beta;
	mMigrationLines = MakeText(mMigration, FText::GetEmpty(), 10);
	mMigrationRows = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	mMigration->AddChildToVerticalBox(mMigrationRows)->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
}

AFGTrainStationIdentifier* USTStationPanel::GetStation() const
{
	const AFGBuildableRailroadStation* Station = Cast<AFGBuildableRailroadStation>(STWidgetUtils::GetObjectProperty(GetMenu(), TEXT("RailroadStation")));
	if (!Station)
	{
		Station = Cast<AFGBuildableRailroadStation>(STWidgetUtils::GetObjectProperty(GetMenu(), TEXT("mInteractObject")));
	}
	return STLayout::GetIdentifier(Station);
}

bool USTStationPanel::OnShown()
{
	bApplyArmed = false;
	mShownSerial = -1;
	UE_LOG(LogSmartTrains, Log, TEXT("Station menu for %s"), *STLayout::Name(GetStation()));
	if (bMigrationOpen)
	{
		SendMigration(static_cast<uint8>(ESTMigrationStep::Refresh));
	}
	return true;
}

void USTStationPanel::RequestView(USTRemoteCallObject* RCO)
{
	if (AFGTrainStationIdentifier* Station = GetStation())
	{
		RCO->Server_RequestStation(Station);
	}
}

void USTStationPanel::SendStation(bool bNewDepot, const FString& NewPool)
{
	USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this);
	AFGTrainStationIdentifier* Station = GetStation();
	if (!RCO || !Station)
	{
		return;
	}
	bDepot = bNewDepot;
	mPoolValue = NewPool;
	UE_LOG(LogSmartTrains, Log, TEXT("Station menu: %s depot %s pool '%s'"), *STLayout::Name(Station), bDepot ? TEXT("on") : TEXT("off"), *NewPool);
	RCO->Server_SetStation(Station, bDepot, NewPool, RCO->NextEdit());
	mDepot->SetText(bDepot ? LOCTEXT("DepotOn", "Depot: on") : LOCTEXT("DepotOff", "Depot: off"));
}

void USTStationPanel::Refresh(USTRemoteCallObject* RCO)
{
	if (const FSTStationView* View = RCO->GetStationView(GetStation()))
	{
		bDepot = View->bDepot;
		mPoolValue = View->Pool;
		mDepot->SetText(bDepot ? LOCTEXT("DepotOn", "Depot: on") : LOCTEXT("DepotOff", "Depot: off"));
		mPool->SetTextIfIdle(View->Pool);
		SetLines(mLines, View->Lines);
		SetLines(mNetwork, View->NetworkLines);
	}
	RefreshMigration(RCO->GetMigrationView());
}

void USTStationPanel::ToggleMigration()
{
	bMigrationOpen = !bMigrationOpen;
	mMigration->SetVisibility(bMigrationOpen ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	mMigrationToggle->SetText(bMigrationOpen ? LOCTEXT("HideMigration", "Hide migration tool") : LOCTEXT("ShowMigration", "Migration tool..."));
	if (bMigrationOpen)
	{
		SendMigration(static_cast<uint8>(ESTMigrationStep::Refresh));
	}
}

void USTStationPanel::SendMigration(uint8 Step, const TArray<FString>& Selection)
{
	if (USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this))
	{
		UE_LOG(LogSmartTrains, Log, TEXT("Migration window: step %d [%s]"), Step, *FString::Join(Selection, TEXT(" ")));
		RCO->Server_Migrate(static_cast<ESTMigrationStep>(Step), Selection);
	}
}

void USTStationPanel::ApplyTicked()
{
	const USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this);
	if (!RCO)
	{
		return;
	}
	const FSTMigrationView& View = RCO->GetMigrationView();
	if (View.bNeedsConfirm && !bApplyArmed)
	{
		bApplyArmed = true;
		mApply->SetText(LOCTEXT("ApplyArmed", "No test copy or backup yet: click again to apply here"));
		return;
	}
	FString Stations;
	FString Trains;
	for (const FSTMigrationRow& Row : View.Rows)
	{
		if (const bool* Ticked = mTicked.Find(RowKey(Row)); Ticked && *Ticked)
		{
			FString& List = Row.bStation ? Stations : Trains;
			List += (List.IsEmpty() ? TEXT("") : TEXT(",")) + FString::FromInt(Row.Number);
		}
	}
	TArray<FString> Selection;
	if (!Stations.IsEmpty())
	{
		Selection.Append({TEXT("stations"), Stations});
	}
	if (!Trains.IsEmpty())
	{
		Selection.Append({TEXT("trains"), Trains});
	}
	if (Selection.Num() == 0)
	{
		mMigrationLines->SetText(LOCTEXT("NothingTicked", "Nothing is ticked."));
		return;
	}
	if (bApplyArmed)
	{
		Selection.Add(TEXT("confirm"));
	}
	bApplyArmed = false;
	SendMigration(static_cast<uint8>(ESTMigrationStep::Apply), Selection);
}

void USTStationPanel::RefreshMigration(const FSTMigrationView& View)
{
	mUndo->SetIsEnabled(View.bCanUndo);
	mApply->SetIsEnabled(View.bHasPreview);
	if (!bApplyArmed)
	{
		mApply->SetText(LOCTEXT("Apply", "Apply ticked"));
	}
	mBetaRow->SetVisibility(View.bBetaTools ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	if (View.Serial == mShownSerial)
	{
		return;
	}
	mShownSerial = View.Serial;
	SetLines(mMigrationLines, View.Lines.Num() > 0 ? View.Lines
		: TArray<FString>{View.bHasPreview ? TEXT("Preview below; press Preview again to refresh it.") : TEXT("Press Preview to see what would change.")});

	// A fresh preview starts from its proposals; after other steps the player's ticks on the same rows are kept.
	const bool bFreshPreview = View.Lines.Num() > 0 && View.Lines[0].StartsWith(TEXT("Migration preview"));
	TMap<FString, bool> Ticks;
	for (const FSTMigrationRow& Row : View.Rows)
	{
		const FString Key = RowKey(Row);
		const bool* Previous = bFreshPreview ? nullptr : mTicked.Find(Key);
		Ticks.Add(Key, Previous ? *Previous : Row.bProposed);
	}
	mTicked = MoveTemp(Ticks);
	mMigrationRows->ClearChildren();
	for (const FSTMigrationRow& Row : View.Rows)
	{
		const FString Key = RowKey(Row);
		const FString Text = Row.Text;
		USTRow* Widget = MakeRow(mMigrationRows, FText::GetEmpty(), nullptr);
		auto Show = [Widget, Key, Text](bool bTicked)
		{
			Widget->SetText(FText::FromString(FString::Printf(TEXT("%s %s %s"), bTicked ? TEXT("[x]") : TEXT("[  ]"), *Key, *Text)));
		};
		Show(mTicked.FindRef(Key));
		Widget->OnClick = [this, Key, Show]()
		{
			bool& Ticked = mTicked.FindOrAdd(Key);
			Ticked = !Ticked;
			Show(Ticked);
		};
	}
	UE_LOG(LogSmartTrains, Verbose, TEXT("Migration window: %d rows shown"), View.Rows.Num());
}

#undef LOCTEXT_NAMESPACE
