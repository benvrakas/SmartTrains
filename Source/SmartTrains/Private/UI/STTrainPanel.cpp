#include "UI/STTrainPanel.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "FGLocomotive.h"
#include "FGTrain.h"
#include "SmartTrains.h"
#include "STRemoteCallObject.h"
#include "UI/STControls.h"
#include "UI/STWidgetUtils.h"

#define LOCTEXT_NAMESPACE "SmartTrains"

namespace
{
	// Tallest the section gets before it scrolls; the stop list above keeps the rest of the column.
	constexpr float TrainPanelMaxHeight = 230.f;
}

UWidget* USTTrainPanel::WrapRoot(UVerticalBox* Root)
{
	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	Scroll->AddChild(Root);
	USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Size->SetMaxDesiredHeight(TrainPanelMaxHeight);
	Size->AddChild(Scroll);
	UBorder* Border = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Border->SetBrushColor(FLinearColor(0.f, 0.f, 0.f, 0.25f));
	Border->SetPadding(FMargin(10.f, 6.f, 10.f, 6.f));
	Border->SetContent(Size);
	return Border;
}

void USTTrainPanel::BuildContent(UVerticalBox* Root)
{
	mManage = MakeButton(Root, LOCTEXT("ManageOff", "Managed by SmartTrains: off"), [this]()
	{
		Send(static_cast<uint8>(bManaged ? ESTTrainAction::Unmanage : ESTTrainAction::Manage));
	}, FMargin(0.f, 0.f, 0.f, 2.f));
	mControls = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Root->AddChildToVerticalBox(mControls)->SetPadding(FMargin(0.f, 0.f, 0.f, 2.f));
	mHold = MakeButton(nullptr, LOCTEXT("Hold", "Hold"), [this]() { Send(static_cast<uint8>(bHeld ? ESTTrainAction::Resume : ESTTrainAction::Hold)); }, FMargin(0.f));
	mCancelJob = MakeButton(nullptr, LOCTEXT("CancelJob", "Cancel job"), [this]() { Send(static_cast<uint8>(ESTTrainAction::CancelJob)); }, FMargin(0.f));
	for (USTButton* Button : {mHold.Get(), mCancelJob.Get()})
	{
		if (UHorizontalBoxSlot* ButtonSlot = mControls->AddChildToHorizontalBox(Button))
		{
			ButtonSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			ButtonSlot->SetPadding(FMargin(0.f, 0.f, Button == mHold ? 4.f : 0.f, 0.f));
		}
	}
	mStatus = MakeText(Root, LOCTEXT("Loading", "Asking the server..."), 10);
	mLines = MakeText(Root, FText::GetEmpty(), 10, true, FMargin(0.f, 4.f, 0.f, 0.f));
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

void USTTrainPanel::RequestView(USTRemoteCallObject* RCO)
{
	if (AFGTrain* Train = GetTrain())
	{
		RCO->Server_RequestTrain(Train);
	}
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
	FString Status = View->Status;
	if (!View->bManaged && !View->CannotManage.IsEmpty())
	{
		Status = TEXT("Can't be managed: ") + View->CannotManage + TEXT(".");
	}
	else if (View->bManaged)
	{
		Status += TEXT("\nSmartTrains writes this train's timetable.");
	}
	mStatus->SetText(FText::FromString(Status));
	mManage->SetText(View->bManaged ? LOCTEXT("ManageOn", "Managed by SmartTrains: on") : LOCTEXT("ManageOff", "Managed by SmartTrains: off"));
	mManage->SetIsEnabled(View->bManaged || View->CannotManage.IsEmpty());
	bHeld = View->bHeld;
	mControls->SetVisibility(View->bManaged ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	mHold->SetText(bHeld ? LOCTEXT("Resume", "Resume") : LOCTEXT("Hold", "Hold"));
	mCancelJob->SetIsEnabled(View->bHasJob);
	SetLines(mLines, View->Lines);
	SetEditLocked(View->bManaged);
}

#undef LOCTEXT_NAMESPACE
