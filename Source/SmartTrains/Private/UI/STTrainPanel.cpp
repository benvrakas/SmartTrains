#include "UI/STTrainPanel.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "FGLocomotive.h"
#include "FGTrain.h"
#include "SmartTrains.h"
#include "STRemoteCallObject.h"
#include "UI/STControls.h"
#include "UI/STWidgetUtils.h"

#define LOCTEXT_NAMESPACE "SmartTrains"

void USTTrainPanel::BuildContent(UVerticalBox* Root)
{
	MakeHeading(Root, LOCTEXT("TrainHeading", "SmartTrains"));
	mStatus = MakeText(Root, LOCTEXT("Loading", "Asking the server..."));
	mManage = MakeButton(Root, LOCTEXT("ManageOff", "Managed by SmartTrains: off"), [this]()
	{
		Send(static_cast<uint8>(bManaged ? ESTTrainAction::Unmanage : ESTTrainAction::Manage));
	});
	mKeepVanilla = MakeButton(Root, LOCTEXT("KeepVanillaOff", "Keep vanilla: off"), [this]()
	{
		Send(static_cast<uint8>(bKeepVanilla ? ESTTrainAction::KeepVanillaOff : ESTTrainAction::KeepVanillaOn));
	});
	MakeText(Root, LOCTEXT("PoolLabel", "Pool tag (serves only stations with the same tag; empty serves all):"), 10, true, FMargin(0.f, 6.f, 0.f, 2.f));
	mPool = MakeInput(Root, LOCTEXT("PoolHint", "no pool"));
	mPool->OnCommitted = [this](const FString& Text)
	{
		Send(static_cast<uint8>(ESTTrainAction::SetPool), Text);
	};
	mLines = MakeText(Root, FText::GetEmpty(), 10, true, FMargin(0.f, 6.f, 0.f, 0.f));
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

void USTTrainPanel::Send(uint8 Action, const FString& Pool)
{
	USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this);
	AFGTrain* Train = GetTrain();
	if (!RCO || !Train)
	{
		return;
	}
	UE_LOG(LogSmartTrains, Log, TEXT("Locomotive menu: %s action %d pool '%s'"), *Train->GetTrainName().ToString(), Action, *Pool);
	RCO->Server_SetTrain(Train, static_cast<ESTTrainAction>(Action), Pool, RCO->NextEdit());
}

void USTTrainPanel::Refresh(USTRemoteCallObject* RCO)
{
	const FSTTrainView* View = RCO->GetTrainView(GetTrain());
	if (!View)
	{
		return;
	}
	bManaged = View->bManaged;
	bKeepVanilla = View->bKeepVanilla;
	FString Status = View->Status;
	if (!View->bManaged && !View->CannotManage.IsEmpty())
	{
		Status += TEXT(" Can't be managed: ") + View->CannotManage + TEXT(".");
	}
	mStatus->SetText(FText::FromString(Status));
	mManage->SetText(View->bManaged ? LOCTEXT("ManageOn", "Managed by SmartTrains: on") : LOCTEXT("ManageOff", "Managed by SmartTrains: off"));
	mManage->SetIsEnabled(View->bManaged || View->CannotManage.IsEmpty());
	mKeepVanilla->SetText(View->bKeepVanilla ? LOCTEXT("KeepVanillaOn", "Keep vanilla: on") : LOCTEXT("KeepVanillaOff", "Keep vanilla: off"));
	mKeepVanilla->SetVisibility(View->bManaged ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
	mPool->SetTextIfIdle(View->Pool);
	TArray<FString> Lines = View->Lines;
	if (View->bManaged)
	{
		Lines.Add(TEXT("SmartTrains writes this train's timetable; turn management off to edit it."));
	}
	SetLines(mLines, Lines);
	SetEditLocked(View->bManaged);
}

#undef LOCTEXT_NAMESPACE
