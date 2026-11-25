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
#include "UI/STStyle.h"
#include "Components/Border.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "Components/WrapBox.h"
#include "UI/STWidgetUtils.h"
#include "UI/STStationTab.h"
#include "Components/Button.h"
#include "Components/PanelWidget.h"
#include "Components/ScrollBox.h"
#include "Engine/World.h"
#include "TimerManager.h"

#define LOCTEXT_NAMESPACE "SmartTrains"

namespace
{
	// Width of the SmartTrains tab's page in the station menu, as wide as the Train Station page. [slate units]
	constexpr float StationTabPageWidth = 560.f;
	// Wait before the tabs work out their page positions again, so the new page has its size. [s]
	constexpr float StationTabOffsetDelay = 0.3f;
}

namespace
{
	// Width each set-up bar button asks for at most; the row shares out whatever the station menu gives it. [px]
	constexpr float BarButtonMaxWidth = 120.f;
	// How long the Autodetect all button shows its result. [s]
	constexpr double BarResultSeconds = 8.0;
}

void USTStationPanel::BuildContent(UVerticalBox* Root)
{
	UHorizontalBox* Bar = MakeTitleBar(Root, FText::GetEmpty(), 20.f);
	mRole = MakeBadge(Bar, FMargin(6.f, 0.f, 0.f, 0.f));

	MakeLabel(Root, LOCTEXT("PlatformsLabel", "Platforms from the station"));
	mStrip = WidgetTree->ConstructWidget<UWrapBox>(UWrapBox::StaticClass());
	mStrip->SetInnerSlotPadding(FVector2D(4.f, 4.f));
	Root->AddChildToVerticalBox(mStrip);

	mConsumer = MakeCard(Root, STStyle::Header, FMargin(10.f, 8.f), FMargin(0.f, 8.f, 0.f, 0.f));
	UHorizontalBox* Top = MakeHBox(mConsumer);
	USizeBox* RingSize = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	RingSize->SetWidthOverride(72.f);
	RingSize->SetHeightOverride(72.f);
	UOverlay* RingStack = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
	RingSize->AddChild(RingStack);
	mRing = WidgetTree->ConstructWidget<USTRing>(USTRing::StaticClass());
	RingStack->AddChildToOverlay(mRing);
	mRingText = MakeTinted(nullptr, FText::FromString(TEXT("-")), 16, STStyle::Text, true, false);
	if (UOverlaySlot* TextSlot = RingStack->AddChildToOverlay(mRingText))
	{
		TextSlot->SetHorizontalAlignment(HAlign_Center);
		TextSlot->SetVerticalAlignment(VAlign_Center);
	}
	Top->AddChildToHorizontalBox(RingSize);
	SetVAlignCenter(RingSize);
	UVerticalBox* Grid = MakeVBox(Top, FMargin(10.f, 0.f, 0.f, 0.f));
	SetFill(Grid);
	UHorizontalBox* RowA = MakeHBox(Grid);
	UVerticalBox* A1 = MakeVBox(RowA);
	SetFill(A1);
	mRate = MakeStat(A1, LOCTEXT("StationRate", "car loads / min at most"));
	UVerticalBox* A2 = MakeVBox(RowA, FMargin(4.f, 0.f, 0.f, 0.f));
	SetFill(A2);
	mTrip = MakeStat(A2, LOCTEXT("StationTrip", "round trip"));
	UHorizontalBox* RowB = MakeHBox(Grid, FMargin(0.f, 4.f, 0.f, 0.f));
	UVerticalBox* B1 = MakeVBox(RowB);
	SetFill(B1);
	mShare = MakeStat(B1, LOCTEXT("StationShare", "of the network's need"));
	UVerticalBox* B2 = MakeVBox(RowB, FMargin(4.f, 0.f, 0.f, 0.f));
	SetFill(B2);
	mServed = MakeStat(B2, LOCTEXT("StationServed", "trains of its share"));
	mNote = MakeTinted(mConsumer, FText::GetEmpty(), 10, STStyle::Muted, false, true, FMargin(0.f, 6.f, 0.f, 0.f));

	MakeLabel(mConsumer, LOCTEXT("PriorityLabel", "Priority"));
	UHorizontalBox* Priorities = MakeHBox(mConsumer);
	for (int32 Index = 0; Index <= static_cast<int32>(ESTPriority::High); ++Index)
	{
		const ESTPriority Level = static_cast<ESTPriority>(Index);
		USTSegment* Segment = WidgetTree->ConstructWidget<USTSegment>(USTSegment::StaticClass());
		Segment->SetText(FText::FromString(STNames::Priority(Level)));
		Segment->SetFont(GetFont(11, true));
		Segment->OnClick = [this, Level]() { SetPriority(Level); };
		Priorities->AddChildToHorizontalBox(Segment);
		SetFill(Segment);
		SetSlotPadding(Segment, FMargin(Index > 0 ? 3.f : 0.f, 0.f, 0.f, 0.f));
		mPrioritySegments.Add(Segment);
	}
	mPriorityHint = MakeTinted(mConsumer, FText::GetEmpty(), 10, STStyle::Muted, false, true, FMargin(0.f, 4.f, 0.f, 0.f));

	mSourcesLabel = MakeLabel(Root, LOCTEXT("SuppliedFrom", "Supplied from"));
	mSources = MakeVBox(Root);
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
	UE_LOG(LogSmartTrains, Log, TEXT("Station menu for %s"), *STLayout::Name(GetStation()));
	// The game reuses menu widgets: the priority shown is the next readout's.
	mPriority = ESTPriority::Medium;
	return true;
}

void USTStationPanel::SetPriority(ESTPriority NewPriority)
{
	USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this);
	AFGTrainStationIdentifier* Station = GetStation();
	if (!RCO || !Station || NewPriority == mPriority)
	{
		return;
	}
	mPriority = NewPriority;
	mPriorityEdit = RCO->NextEdit();
	UE_LOG(LogSmartTrains, Log, TEXT("Station menu: %s priority -> %s (edit %d)"), *STLayout::Name(Station), STNames::Priority(NewPriority), mPriorityEdit);
	RCO->Server_SetStationPriority(Station, NewPriority, mPriorityEdit);
	RefreshSoon();
}

void USTStationPanel::RequestView(USTRemoteCallObject* RCO)
{
	if (AFGTrainStationIdentifier* Station = GetStation())
	{
		RCO->Server_RequestStation(Station);
	}
}

void USTStationPanel::SendRename(bool bApply)
{
	USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this);
	AFGTrainStationIdentifier* Station = GetStation();
	if (!RCO || !Station)
	{
		return;
	}
	UE_LOG(LogSmartTrains, Log, TEXT("Station menu: %s for %s"), bApply ? TEXT("rename") : TEXT("rename preview"), *STLayout::Name(Station));
	RCO->Server_SetUpStation(Station, bApply ? 1 : 0, RCO->NextEdit());
	if (mRename && !bApply)
	{
		mRename->SetText(LOCTEXT("RenameAsking", "Working out the name..."));
	}
	if (mSetUpLines)
	{
		mSetUpLines->SetText(LOCTEXT("SetUpAsking", "Tracing belts and pipes..."));
		mSetUpLines->SetVisibility(ESlateVisibility::Visible);
	}
}

void USTStationPanel::ShowSetUp(const TArray<FString>& Lines, uint8 Ready)
{
	if (!mRename)
	{
		return;
	}
	// The preview's first line is "Name: Old -> New", "Name: Old (unchanged)" or "Name: Old (kept: why)"; only a new name
	// can be confirmed, and the button says it.
	const FString First = Lines.Num() > 0 ? Lines[0] : FString();
	FString NewName;
	const bool bNew = Ready == 1 && First.StartsWith(TEXT("Name: ")) && First.Split(TEXT(" -> "), nullptr, &NewName);
	mSetupReady = bNew ? 1 : 0;
	if (bNew)
	{
		mRename->SetText(FText::Format(LOCTEXT("RenameTo", "Rename to: {0}"), FText::FromString(NewName)));
	}
	else if (Ready == 1 && First.EndsWith(TEXT("(unchanged)")))
	{
		mRename->SetText(LOCTEXT("RenameSame", "Name is up to date"));
	}
	else if (Ready == 1 && First.Contains(TEXT("(kept")))
	{
		mRename->SetText(LOCTEXT("RenameNone", "No name found from its factory"));
	}
	else
	{
		mRename->SetText(LOCTEXT("Rename", "Rename from factory..."));
	}
	if (mSetUpLines)
	{
		SetLines(mSetUpLines, Lines);
		mSetUpLines->SetVisibility(Lines.Num() > 0 ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	}
}

void USTStationPanel::Refresh(USTRemoteCallObject* RCO)
{
	const FSTStationView* View = RCO->GetStationView(GetStation());
	if (!View)
	{
		return;
	}
	ShowSetUp(View->SetupLines, View->SetupReady);
	if (!mStrip)
	{
		return;
	}
	// The role badge: what the station is to the loops.
	const bool bSupplier = View->Supplies.Num() > 0;
	const FText Role = View->bConsumer && bSupplier ? LOCTEXT("RoleBoth", "Consumer + supplier") : View->bConsumer ? LOCTEXT("RoleConsumer", "Consumer")
		: bSupplier ? LOCTEXT("RoleSupplier", "Supplier") : LOCTEXT("RoleNone", "Not used");
	SetBadge(mRole, Role, View->bConsumer ? STStyle::Orange : bSupplier ? STStyle::Good : STStyle::Muted);

	// Platforms in order from the station: a tile each with its role's colour, icon and item.
	FString StripKey;
	for (const FSTPlatformChip& Chip : View->Platforms)
	{
		StripKey += FString::Printf(TEXT("%d:%d:"), Chip.Index, static_cast<int32>(Chip.Role));
		for (const TSubclassOf<UFGItemDescriptor>& Item : Chip.Items)
		{
			StripKey += GetNameSafe(Item.Get()) + TEXT(",");
		}
		StripKey += TEXT(";");
	}
	if (StripKey != mStripKey)
	{
		mStripKey = StripKey;
		mStrip->ClearChildren();
		for (const FSTPlatformChip& Chip : View->Platforms)
		{
			const FLinearColor RoleColor = Chip.Role == ESTRole::Demand ? STStyle::Orange : Chip.Role == ESTRole::Supply ? STStyle::Good
				: Chip.Role == ESTRole::Sink ? STStyle::Warn : STStyle::Line;
			USizeBox* Width = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
			Width->SetWidthOverride(118.f);
			mStrip->AddChild(Width);
			UBorder* Edge = nullptr;
			UVerticalBox* Outer = MakeCard(nullptr, RoleColor, FMargin(0.f, 3.f, 0.f, 0.f), FMargin(0.f), &Edge);
			Width->AddChild(Edge);
			UVerticalBox* Tile = MakeCard(Outer, Chip.Role == ESTRole::Off ? STStyle::Inset : STStyle::Card, FMargin(7.f, 5.f), FMargin(0.f));
			MakeTinted(Tile, FText::FromString(FString::Printf(TEXT("P%d · %s"), Chip.Index + 1, STNames::Role(Chip.Role))), 9, STStyle::Muted, true, false);
			if (Chip.Items.Num() == 0)
			{
				MakeTinted(Tile, Chip.Role == ESTRole::Sink ? LOCTEXT("SinkAny", "Any cargo") : LOCTEXT("NotUsed", "Not used"), 10, STStyle::Muted, false, true,
					FMargin(0.f, 3.f, 0.f, 0.f));
			}
			for (const TSubclassOf<UFGItemDescriptor>& Item : Chip.Items)
			{
				UHorizontalBox* Line = MakeHBox(Tile, FMargin(0.f, 3.f, 0.f, 0.f));
				SetIcon(MakeIcon(Line, 22.f), Item.Get());
				UTextBlock* Name = MakeTinted(Line, FText::FromString(STNames::Item(Item)), 10, STStyle::Text, true, true, FMargin(5.f, 0.f, 0.f, 0.f));
				SetFill(Name);
				SetVAlignCenter(Name);
			}
		}
	}

	// The consumer's loop.
	mConsumer->SetVisibility(View->bConsumer ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	if (View->bConsumer)
	{
		mRing->SetValue(View->Target > 0 ? FMath::Clamp(static_cast<float>(View->Trains) / View->Target, 0.f, 1.f) : 0.f);
		mRing->SetColor(View->Trains >= View->Target && View->Trains > 0 ? STStyle::Orange : STStyle::Warn);
		mRingText->SetText(FText::AsNumber(View->Trains));
		mRate.Value->SetText(FText::FromString(FString::Printf(TEXT("%.2f"), View->Rate)));
		mTrip.Value->SetText(FText::FromString(STStyle::Duration(View->RoundTrip)));
		mShare.Value->SetText(FText::FromString(View->Share >= 0.f ? FString::Printf(TEXT("%.0f %%"), 100.f * View->Share) : FString(TEXT("-"))));
		mServed.Value->SetText(FText::FromString(View->Trains == 0 ? FString(TEXT("Unserved")) : FString::Printf(TEXT("%d of %d"), View->Trains, View->Target)));
		FString Note;
		if (!View->Problem.IsEmpty())
		{
			Note = View->Problem;
		}
		else if (View->AtDepot > 0)
		{
			Note = FString::Printf(TEXT("%d of its trains wait at depot signals: its machines want %.0f %% of their input now."), View->AtDepot, 100.f * View->Use);
		}
		mNote->SetText(FText::FromString(Note));
		Tint(mNote, View->Problem.IsEmpty() ? STStyle::Muted : STStyle::Orange);
		mNote->SetVisibility(Note.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);
		if (View->AppliedEdit >= mPriorityEdit)
		{
			mPriority = View->Priority;
		}
		for (int32 Index = 0; Index < mPrioritySegments.Num(); ++Index)
		{
			mPrioritySegments[Index]->SetSelected(static_cast<int32>(mPriority) == Index);
		}
		mPriorityHint->SetText(mPriority == ESTPriority::High
			? LOCTEXT("PriorityHigh", "Takes trains from other loops until it has enough for its factory's full input.")
			: mPriority == ESTPriority::Low
			? LOCTEXT("PriorityLow", "Gets trains only once every Medium and High loop has enough for its full input.")
			: LOCTEXT("PriorityMedium", "Shares the trains High loops leave with the other Medium loops, by need."));
	}

	// Its suppliers (a consumer) or what it offers (a supplier).
	const TArray<FSTSourceView>& Sources = View->bConsumer ? View->Suppliers : View->Supplies;
	FString SourcesKey = View->bConsumer ? TEXT("c") : TEXT("s");
	for (const FSTSourceView& Source : Sources)
	{
		SourcesKey += Source.Station + GetNameSafe(Source.Item.Get()) + FString::Printf(TEXT("%.0f:%.2f:%.2f;"), Source.Seconds, Source.Rate, Source.Taken);
	}
	if (SourcesKey != mSourcesKey)
	{
		mSourcesKey = SourcesKey;
		mSources->ClearChildren();
		for (const FSTSourceView& Source : Sources)
		{
			UVerticalBox* Row = MakeCard(mSources, STStyle::Inset, FMargin(7.f, 4.f), FMargin(0.f, 1.f));
			UHorizontalBox* Line = MakeHBox(Row);
			SetIcon(MakeIcon(Line, 20.f), Source.Item.Get());
			UTextBlock* Name = MakeTinted(Line, FText::FromString(View->bConsumer ? Source.Station : STNames::Item(Source.Item)), 10, STStyle::Text, false, true,
				FMargin(6.f, 0.f, 0.f, 0.f));
			SetFill(Name);
			SetVAlignCenter(Name);
			const FString Right = View->bConsumer ? STStyle::Duration(Source.Seconds)
				: Source.Rate >= 0.f ? FString::Printf(TEXT("loops take %.2f of %.2f"), Source.Taken, Source.Rate) : FString::Printf(TEXT("loops take %.2f"), Source.Taken);
			UTextBlock* Side = MakeTinted(Line, FText::FromString(Right), 9, STStyle::Muted, false, false, FMargin(6.f, 0.f, 0.f, 0.f));
			SetVAlignCenter(Side);
		}
	}
	mSourcesLabel->SetText(FText::FromString((View->bConsumer ? FString(TEXT("Supplied from")) : FString(TEXT("Offers"))).ToUpper()));
	const bool bSources = Sources.Num() > 0;
	mSourcesLabel->SetVisibility(bSources ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	mSources->SetVisibility(bSources ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
}

void USTStationSetupBar::BuildContent(UVerticalBox* Root)
{
	// One row, as tall as a single button: the space under the name plate doesn't push the rest of the menu down.
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Root->AddChildToVerticalBox(Row);
	mAutodetect = MakeButton(nullptr, LOCTEXT("AutodetectAll", "Autodetect all platforms"), [this]() { ClickAutodetect(); }, FMargin(0.f));
	// Shows the name it would give first (in its own text); a second click renames.
	mRename = MakeButton(nullptr, FText::GetEmpty(), [this]() { SendRename(mSetupReady == 1); }, FMargin(0.f));
	for (USTButton* Button : {mAutodetect.Get(), mRename.Get()})
	{
		// Both share the width under the name plate whatever their text; asking for more widened the menu past its frame.
		Button->SetFitToSlot(BarButtonMaxWidth);
		if (UHorizontalBoxSlot* ButtonSlot = Row->AddChildToHorizontalBox(Button))
		{
			ButtonSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			ButtonSlot->SetPadding(FMargin(0.f, 0.f, Button == mAutodetect ? 4.f : 0.f, 0.f));
		}
	}
	ShowSetUp(TArray<FString>(), 0);
}

void USTStationSetupBar::ClickAutodetect()
{
	USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this);
	AFGTrainStationIdentifier* Station = GetStation();
	if (!RCO || !Station)
	{
		return;
	}
	if (!bAutodetectArmed)
	{
		bAutodetectArmed = true;
		mAutodetect->SetText(LOCTEXT("AutodetectAllConfirm", "Click again: redo every platform"));
		return;
	}
	bAutodetectArmed = false;
	mAutodetect->SetText(LOCTEXT("AutodetectAllWorking", "Autodetecting..."));
	const UWorld* World = GetWorld();
	mAutodetectSentAt = World ? World->GetTimeSeconds() : 0.0;
	UE_LOG(LogSmartTrains, Log, TEXT("Station menu: autodetect all platforms of %s"), *STLayout::Name(Station));
	RCO->Server_AutodetectStation(Station, RCO->NextEdit());
}

void USTStationSetupBar::InstallTab()
{
	UUserWidget* Menu = GetMenu();
	if (!Menu || mFallbackPanel)
	{
		return;
	}
	const FText Title = LOCTEXT("SmartTrainsTab", "SmartTrains");
	STStationTab::FParts Parts;
	const bool bFound = STStationTab::FindParts(Menu, this, Parts);
	if (bFound && STStationTab::HasTab(Parts, Title))
	{
		return;
	}
	USTStationPanel* Panel = CreateWidget<USTStationPanel>(GetOwningPlayer(), USTStationPanel::StaticClass());
	if (!Panel)
	{
		return;
	}
	Panel->SetMenu(Menu);
	if (bFound)
	{
		mTabPage = Menu->WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		mTabPage->SetWidthOverride(StationTabPageWidth);
		UScrollBox* Scroll = Menu->WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
		Scroll->AddChild(Panel);
		mTabPage->AddChild(Scroll);
		if (STStationTab::AddTab(Parts, mTabPage, Title))
		{
			// Once the new page has its size, the tabs work out where each page sits again.
			if (UWorld* World = GetWorld())
			{
				const STStationTab::FParts Kept = Parts;
				TWeakObjectPtr<UUserWidget> WeakTabs = Parts.Tabs;
				World->GetTimerManager().SetTimer(mOffsetTimer, FTimerDelegate::CreateWeakLambda(this, [WeakTabs, Kept]()
				{
					if (WeakTabs.IsValid())
					{
						STStationTab::UpdateOffsets(Kept);
					}
				}), StationTabOffsetDelay, false);
			}
			return;
		}
		Scroll->RemoveChild(Panel);
		mTabPage = nullptr;
	}
	if (UVerticalBox* Box = Cast<UVerticalBox>(GetParent()))
	{
		Box->AddChildToVerticalBox(Panel);
		mFallbackPanel = Panel;
	}
}

bool USTStationSetupBar::OnShown()
{
	UE_LOG(LogSmartTrains, Log, TEXT("Station set-up bar for %s"), *STLayout::Name(GetStation()));
	InstallTab();
	bAutodetectArmed = false;
	mAutodetectSentAt = -1.0;
	mResultUntil = -1.0;
	if (mAutodetect)
	{
		mAutodetect->SetText(LOCTEXT("AutodetectAll", "Autodetect all platforms"));
	}
	return GetStation() != nullptr;
}

void USTStationSetupBar::ShowServerMessage(USTRemoteCallObject* RCO)
{
	if (mMessage)
	{
		mMessage->SetVisibility(ESlateVisibility::Collapsed);
	}
	const UWorld* World = GetWorld();
	if (!World || !mAutodetect)
	{
		return;
	}
	const double Time = World->GetTimeSeconds();
	if (mAutodetectSentAt >= 0.0 && RCO && RCO->GetLastMessageTime() >= mAutodetectSentAt)
	{
		// "Autodetect all: P1 Demand Copper Ingot; ... (3 changed)." or an error such as "Only admins may ...".
		mAutodetectSentAt = -1.0;
		mResultUntil = Time + BarResultSeconds;
		const FString& Message = RCO->GetLastMessage();
		FString Changed;
		if (RCO->IsLastMessageError() || !Message.StartsWith(TEXT("Autodetect all")))
		{
			mAutodetect->SetText(FText::FromString(Message));
		}
		else if (Message.Split(TEXT("("), nullptr, &Changed, ESearchCase::CaseSensitive, ESearchDir::FromEnd) && Changed.StartsWith(TEXT("0 ")))
		{
			mAutodetect->SetText(LOCTEXT("AutodetectAllSame", "Done: nothing changed"));
		}
		else
		{
			mAutodetect->SetText(FText::Format(LOCTEXT("AutodetectAllDone", "Done: {0} platforms changed"), FText::FromString(Changed.Left(Changed.Find(TEXT(" "))))));
		}
	}
	else if (mResultUntil >= 0.0 && Time > mResultUntil && !bAutodetectArmed)
	{
		mResultUntil = -1.0;
		mAutodetect->SetText(LOCTEXT("AutodetectAll", "Autodetect all platforms"));
	}
}

#undef LOCTEXT_NAMESPACE
