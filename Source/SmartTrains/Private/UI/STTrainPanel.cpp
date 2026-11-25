#include "UI/STTrainPanel.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/WrapBox.h"
#include "FGLocomotive.h"
#include "FGTrain.h"
#include "SmartTrains.h"
#include "STRemoteCallObject.h"
#include "UI/STControls.h"
#include "UI/STStyle.h"
#include "UI/STWidgetUtils.h"

#define LOCTEXT_NAMESPACE "SmartTrains"

namespace
{
	// Tallest the section gets before it scrolls; the stop list above keeps the rest of the column.
	constexpr float TrainPanelMaxHeight = 360.f;
	// Width of one car tile. [px]
	constexpr float CarTileWidth = 112.f;

	// "1,200 / 6,400", or "1,680 / 2,400 m³" for a fluid (inventories count fluids in litres).
	FString CarAmount(const FSTCarView& Car)
	{
		const double Scale = Car.bFluid ? 1000.0 : 1.0;
		const FText Amount = FText::AsNumber(FMath::RoundToInt(Car.Amount / Scale));
		const FText Capacity = FText::AsNumber(FMath::RoundToInt(Car.Capacity / Scale));
		return FString::Printf(TEXT("%s / %s%s"), *Amount.ToString(), *Capacity.ToString(), Car.bFluid ? TEXT(" m³") : TEXT(""));
	}
}

//~ USTTrainDisplay

FText USTTrainDisplay::StateText(uint8 State)
{
	switch (State)
	{
	case 1: return LOCTEXT("StateLoop", "On its loop");
	case 2: return LOCTEXT("StateSwitching", "Switching patterns");
	case 3: return LOCTEXT("StateHeld", "Held");
	case 4: return LOCTEXT("StateNoLoop", "No loop");
	case 5: return LOCTEXT("StateDepot", "At a depot");
	default: return LOCTEXT("StateOff", "Not managed");
	}
}

FLinearColor USTTrainDisplay::StateColor(uint8 State)
{
	switch (State)
	{
	case 1: return STStyle::Good;
	case 2: return STStyle::Warn;
	case 3: return STStyle::Warn;
	case 4: return STStyle::Orange;
	case 5: return STStyle::Muted;
	default: return STStyle::Muted;
	}
}

void USTTrainDisplay::BuildTrainContent(UVerticalBox* Root)
{
	UHorizontalBox* StateRow = MakeHBox(Root, FMargin(0.f, 2.f, 0.f, 2.f));
	mState = MakeBadge(StateRow);
	mServes = MakeTinted(StateRow, FText::GetEmpty(), 11, STStyle::Text, false, true, FMargin(8.f, 0.f, 0.f, 0.f));
	SetFill(mServes);
	SetVAlignCenter(mServes);

	mTrainSection = MakeVBox(Root);
	MakeLabel(mTrainSection, LOCTEXT("PatternLabel", "Pattern"));
	mCars = WidgetTree->ConstructWidget<UWrapBox>(UWrapBox::StaticClass());
	mCars->SetInnerSlotPadding(FVector2D(4.f, 4.f));
	mTrainSection->AddChildToVerticalBox(mCars);

	MakeLabel(mTrainSection, LOCTEXT("LoopLabel", "Loop"));
	mStops = MakeVBox(mTrainSection);

	UHorizontalBox* Stats = MakeHBox(mTrainSection, FMargin(0.f, 8.f, 0.f, 0.f));
	UVerticalBox* A = MakeVBox(Stats);
	SetFill(A);
	mStatTrains = MakeStat(A, LOCTEXT("StatTrains", "trains on this pattern"));
	UVerticalBox* B = MakeVBox(Stats, FMargin(4.f, 0.f, 0.f, 0.f));
	SetFill(B);
	mStatRate = MakeStat(B, LOCTEXT("StatRate", "car loads / min at most"));
	UVerticalBox* C = MakeVBox(Stats, FMargin(4.f, 0.f, 0.f, 0.f));
	SetFill(C);
	mStatTrip = MakeStat(C, LOCTEXT("StatTrip", "round trip"));
	mDepotLine = MakeTinted(mTrainSection, FText::GetEmpty(), 10, STStyle::Muted, false, true, FMargin(0.f, 4.f, 0.f, 0.f));
}

void USTTrainDisplay::RequestView(USTRemoteCallObject* RCO)
{
	if (AFGTrain* Train = GetTrain())
	{
		RCO->Server_RequestTrain(Train);
	}
}

void USTTrainDisplay::ShowTrain(const FSTTrainView& View)
{
	SetBadge(mState, StateText(View.State), StateColor(View.State));
	FString Serves;
	if (View.State == 5)
	{
		Serves = FString::Printf(TEXT("Waits for %s (its machines want %.0f %% of their input now)"), *View.Consumer, 100.f * View.Use);
	}
	else if (!View.Consumer.IsEmpty())
	{
		Serves = FString::Printf(TEXT("Serves %s"), *View.Consumer);
	}
	else if (View.bManaged)
	{
		Serves = View.Status;
	}
	mServes->SetText(FText::FromString(Serves));
	const bool bShowTrain = View.bManaged && (View.Cars.Num() > 0 || View.Stops.Num() > 0);
	mTrainSection->SetVisibility(bShowTrain ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	if (!bShowTrain)
	{
		return;
	}

	// Car tiles: rebuilt when the cars or their items change, else only their amounts and bars.
	FString CarsKey;
	for (const FSTCarView& Car : View.Cars)
	{
		CarsKey += GetNameSafe(Car.Item.Get()) + TEXT(";");
	}
	if (CarsKey != mCarsKey)
	{
		mCarsKey = CarsKey;
		mCars->ClearChildren();
		mCarWidgets.Reset();
		for (const FSTCarView& Car : View.Cars)
		{
			USizeBox* Width = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
			Width->SetWidthOverride(CarTileWidth);
			mCars->AddChild(Width);
			UVerticalBox* Tile = MakeCard(nullptr, STStyle::Card, FMargin(7.f, 6.f), FMargin(0.f));
			Width->AddChild(Tile->GetParent());
			FCarWidgets& Widgets = mCarWidgets.AddDefaulted_GetRef();
			UHorizontalBox* Top = MakeHBox(Tile);
			Widgets.Icon = MakeIcon(Top, 26.f);
			SetIcon(Widgets.Icon, Car.Item.Get());
			Widgets.Name = MakeTinted(Top, FText::FromString(STNames::Item(Car.Item)), 10, STStyle::Text, true, true, FMargin(5.f, 0.f, 0.f, 0.f));
			SetFill(Widgets.Name);
			SetVAlignCenter(Widgets.Name);
			Widgets.Bar = MakeBar(Tile, 5.f, FMargin(0.f, 5.f, 0.f, 2.f));
			Widgets.Amount = MakeTinted(Tile, FText::GetEmpty(), 9, STStyle::Muted, false, true);
		}
	}
	for (int32 Index = 0; Index < View.Cars.Num() && Index < mCarWidgets.Num(); ++Index)
	{
		const FSTCarView& Car = View.Cars[Index];
		const FCarWidgets& Widgets = mCarWidgets[Index];
		const float Fill = Car.Capacity > 0 ? static_cast<float>(Car.Amount) / Car.Capacity : 0.f;
		Widgets.Bar->SetValue(Fill);
		Widgets.Bar->SetPulse(Car.bActive);
		Widgets.Amount->SetText(FText::FromString(Car.Amount <= 0 ? FString(TEXT("Empty")) : CarAmount(Car)));
	}

	// Stop rows: rebuilt when the stations change.
	FString StopsKey;
	for (const FSTStopView& Stop : View.Stops)
	{
		StopsKey += Stop.Station + TEXT(";");
	}
	if (StopsKey != mStopsKey)
	{
		mStopsKey = StopsKey;
		mStops->ClearChildren();
		mStopWidgets.Reset();
		for (int32 Index = 0; Index < View.Stops.Num(); ++Index)
		{
			FStopWidgets& Widgets = mStopWidgets.AddDefaulted_GetRef();
			UVerticalBox* Row = MakeCard(mStops, STStyle::Panel, FMargin(6.f, 4.f), FMargin(0.f, 1.f), &Widgets.Row);
			UHorizontalBox* Line = MakeHBox(Row);
			Widgets.Number = MakeBadge(Line);
			Widgets.Number.Text->SetText(FText::AsNumber(Index + 1));
			Widgets.Name = MakeTinted(Line, FText::FromString(View.Stops[Index].Station), 11, STStyle::Text, false, true, FMargin(8.f, 0.f, 0.f, 0.f));
			SetFill(Widgets.Name);
			SetVAlignCenter(Widgets.Name);
			Widgets.Right = MakeTinted(Line, FText::GetEmpty(), 10, STStyle::Muted, true, false, FMargin(6.f, 0.f, 0.f, 0.f));
			SetVAlignCenter(Widgets.Right);
		}
	}
	for (int32 Index = 0; Index < View.Stops.Num() && Index < mStopWidgets.Num(); ++Index)
	{
		const FSTStopView& Stop = View.Stops[Index];
		const FStopWidgets& Widgets = mStopWidgets[Index];
		FString Right = Stop.Kind == 0 ? TEXT("Pick up") : Stop.Kind == 1 ? TEXT("Deliver") : TEXT("Drive");
		if (Stop.bDocked)
		{
			Right = Stop.Kind == 0 ? TEXT("Loading") : Stop.Kind == 1 ? TEXT("Unloading") : TEXT("There");
		}
		else if (Stop.bCurrent)
		{
			Right = View.NextSeconds >= 0.f ? FString::Printf(TEXT("Next · %s"), *STStyle::Duration(View.NextSeconds)) : FString(TEXT("Next"));
		}
		Widgets.Right->SetText(FText::FromString(Right));
		Tint(Widgets.Right, Stop.bCurrent ? STStyle::Orange : STStyle::Muted);
		Widgets.Row->SetBrush(STStyle::Rounded(Stop.bCurrent ? STStyle::Raised : STStyle::Panel));
		SetBadge(Widgets.Number, FText::AsNumber(Index + 1), Stop.bCurrent ? STStyle::Orange : STStyle::Line);
	}

	const int32 Others = FMath::Max(0, View.PatternTrains - 1);
	mStatTrains.Value->SetText(FText::FromString(View.PatternTarget > 0 ? FString::Printf(TEXT("%d of %d"), View.PatternTrains, View.PatternTarget)
		: FString::FromInt(View.PatternTrains)));
	mStatTrains.Label->SetText(Others > 0 ? FText::Format(LOCTEXT("StatTrainsOthers", "trains on this pattern: this one + {0}"), Others)
		: LOCTEXT("StatTrainsAlone", "trains on this pattern: only this one"));
	mStatRate.Value->SetText(FText::FromString(FString::Printf(TEXT("%.2f"), View.Rate)));
	mStatTrip.Value->SetText(FText::FromString(STStyle::Duration(View.RoundTrip)));
	mDepotLine->SetText(View.PatternAtDepot > 0 ? FText::Format(LOCTEXT("DepotLine", "{0} of this pattern's trains wait at depot signals: its machines want {1} % of their input now."),
		View.PatternAtDepot, FText::AsNumber(FMath::RoundToInt(100.f * View.Use))) : FText::GetEmpty());
	mDepotLine->SetVisibility(View.PatternAtDepot > 0 ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
}

//~ USTTrainPanel

UWidget* USTTrainPanel::WrapRoot(UVerticalBox* Root)
{
	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	Scroll->AddChild(Root);
	USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Size->SetMaxDesiredHeight(TrainPanelMaxHeight);
	Size->AddChild(Scroll);
	UBorder* Border = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Border->SetBrush(STStyle::Rounded(STStyle::Panel));
	Border->SetPadding(FMargin(8.f, 6.f, 8.f, 8.f));
	Border->SetContent(Size);
	return Border;
}

void USTTrainPanel::BuildContent(UVerticalBox* Root)
{
	UHorizontalBox* Bar = MakeTitleBar(Root, FText::GetEmpty(), 20.f);
	// A compact switch: the vanilla button is too wide for the column and covered the title.
	USizeBox* ManageSize = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	ManageSize->SetWidthOverride(118.f);
	mManage = WidgetTree->ConstructWidget<USTSegment>(USTSegment::StaticClass());
	mManage->SetText(LOCTEXT("ManageOff", "Managed: off"));
	mManage->SetFont(GetFont(10, true));
	mManage->OnClick = [this]() { Send(static_cast<uint8>(bManaged ? ESTTrainAction::Unmanage : ESTTrainAction::Manage)); };
	ManageSize->AddChild(mManage);
	Bar->AddChildToHorizontalBox(ManageSize);
	SetSlotPadding(ManageSize, FMargin(6.f, 0.f, 0.f, 0.f));
	SetVAlignCenter(ManageSize);
	BuildTrainContent(Root);
	mHold = MakeButton(Root, LOCTEXT("Hold", "Hold here"), [this]() { Send(static_cast<uint8>(bHeld ? ESTTrainAction::Resume : ESTTrainAction::Hold)); },
		FMargin(0.f, 8.f, 0.f, 2.f));
	mFooter = MakeTinted(Root, LOCTEXT("Loading", "Asking the server..."), 9, STStyle::Muted, false, true, FMargin(0.f, 4.f, 0.f, 0.f));
}

AFGTrain* USTTrainPanel::GetTrain() const
{
	UUserWidget* Menu = GetMenu();
	if (AFGTrain* Train = Cast<AFGTrain>(STWidgetUtils::GetObjectProperty(Menu, TEXT("mTrain"))))
	{
		return Train;
	}
	const AFGLocomotive* Locomotive = Cast<AFGLocomotive>(STWidgetUtils::GetObjectProperty(Menu, TEXT("FGLocomotive")));
	if (!Locomotive)
	{
		Locomotive = Cast<AFGLocomotive>(STWidgetUtils::GetObjectProperty(Menu, TEXT("mInteractObject")));
	}
	return Locomotive ? Locomotive->GetTrain() : nullptr;
}

bool USTTrainPanel::OnShown()
{
	mEditButton = Cast<UWidget>(STWidgetUtils::GetObjectProperty(GetMenu(), TEXT("mEditTimeTableButton")));
	if (!mEditButton.IsValid())
	{
		UE_LOG(LogSmartTrains, Warning, TEXT("Locomotive menu: mEditTimeTableButton not found; managed timetables stay editable in the menu"));
	}
	bManaged = false;
	UE_LOG(LogSmartTrains, Log, TEXT("Locomotive menu for %s"), GetTrain() ? *GetTrain()->GetTrainName().ToString() : TEXT("no train yet"));
	return true;
}

void USTTrainPanel::OnHidden()
{
	// The game reuses menu widgets; leave the button as it was found.
	SetEditLocked(false);
}

void USTTrainPanel::SetEditLocked(bool bLocked)
{
	if (!mEditButton.IsValid())
	{
		return;
	}
	// Applied on every refresh while locked, in case the menu's own logic enables the button again.
	if (bLocked)
	{
		mEditButton->SetIsEnabled(false);
	}
	if (bLocked == bEditLocked)
	{
		return;
	}
	bEditLocked = bLocked;
	mEditButton->SetIsEnabled(!bLocked);
	UE_LOG(LogSmartTrains, Log, TEXT("Locomotive menu: timetable editing %s"), bLocked ? TEXT("locked (managed train)") : TEXT("unlocked"));
}

void USTTrainPanel::Send(uint8 Action)
{
	USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this);
	AFGTrain* Train = GetTrain();
	if (!RCO || !Train)
	{
		return;
	}
	UE_LOG(LogSmartTrains, Log, TEXT("Locomotive menu: %s action %d"), *Train->GetTrainName().ToString(), Action);
	RCO->Server_SetTrain(Train, static_cast<ESTTrainAction>(Action), RCO->NextEdit());
}

void USTTrainPanel::Refresh(USTRemoteCallObject* RCO)
{
	const FSTTrainView* View = RCO->GetTrainView(GetTrain());
	if (!View)
	{
		return;
	}
	bManaged = View->bManaged;
	bHeld = View->bHeld;
	ShowTrain(*View);
	FString Footer;
	if (!View->bManaged && !View->CannotManage.IsEmpty())
	{
		Footer = TEXT("Can't be managed: ") + View->CannotManage + TEXT(".");
	}
	else if (View->bManaged)
	{
		Footer = TEXT("SmartTrains writes this timetable; editing it is locked while managed.");
	}
	else
	{
		Footer = View->Status;
	}
	mFooter->SetText(FText::FromString(Footer));
	mManage->SetText(View->bManaged ? LOCTEXT("ManageOn", "Managed: on") : LOCTEXT("ManageOff", "Managed: off"));
	mManage->SetSelected(View->bManaged);
	mManage->SetIsEnabled(View->bManaged || View->CannotManage.IsEmpty());
	mHold->SetVisibility(View->bManaged ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	mHold->SetText(bHeld ? LOCTEXT("Resume", "Resume") : LOCTEXT("Hold", "Hold here"));
	SetEditLocked(View->bManaged);
}

#undef LOCTEXT_NAMESPACE
