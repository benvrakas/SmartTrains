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

void USTStationPanel::BuildContent(UVerticalBox* Root)
{
	MakeHeading(Root, LOCTEXT("StationHeading", "SmartTrains"));
	mDepot = MakeButton(Root, GetDepotText(false), [this]() { SendStation(!bDepot); });
	MakeText(Root, LOCTEXT("DepotHint", "Managed trains with nothing to do wait at the nearest depot. With no depot they keep running their last trip."), 10);
	mLines = MakeText(Root, LOCTEXT("Loading", "Asking the server..."), 10, true, FMargin(0.f, 6.f, 0.f, 0.f));
	mNetwork = MakeText(Root, FText::GetEmpty(), 10, true, FMargin(0.f, 6.f, 0.f, 0.f));
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
	return true;
}

void USTStationPanel::RequestView(USTRemoteCallObject* RCO)
{
	if (AFGTrainStationIdentifier* Station = GetStation())
	{
		RCO->Server_RequestStation(Station);
	}
}

void USTStationPanel::SendStation(bool bNewDepot)
{
	USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this);
	AFGTrainStationIdentifier* Station = GetStation();
	if (!RCO || !Station)
	{
		return;
	}
	bDepot = bNewDepot;
	UE_LOG(LogSmartTrains, Log, TEXT("Station menu: %s depot %s"), *STLayout::Name(Station), bDepot ? TEXT("on") : TEXT("off"));
	RCO->Server_SetStation(Station, bDepot, RCO->NextEdit());
	mDepot->SetText(GetDepotText(bDepot));
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

FText USTStationPanel::GetDepotText(bool bOn) const
{
	return bOn ? LOCTEXT("DepotOn", "Depot: on") : LOCTEXT("DepotOff", "Depot: off");
}

void USTStationPanel::Refresh(USTRemoteCallObject* RCO)
{
	if (const FSTStationView* View = RCO->GetStationView(GetStation()))
	{
		bDepot = View->bDepot;
		mDepot->SetText(GetDepotText(bDepot));
		if (mLines)
		{
			SetLines(mLines, View->Lines);
		}
		if (mNetwork)
		{
			SetLines(mNetwork, View->NetworkLines);
		}
		ShowSetUp(View->SetupLines, View->SetupReady);
	}
}

void USTStationDepotBar::BuildContent(UVerticalBox* Root)
{
	// One row, as tall as a single button: the space under the name plate doesn't push the rest of the menu down.
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	Root->AddChildToVerticalBox(Row);
	mDepot = MakeButton(nullptr, GetDepotText(false), [this]() { SendStation(!bDepot); }, FMargin(0.f));
	// Shows the name it would give first (in its own text); a second click renames.
	mRename = MakeButton(nullptr, FText::GetEmpty(), [this]() { SendRename(mSetupReady == 1); }, FMargin(0.f));
	for (USTButton* Button : {mDepot.Get(), mRename.Get()})
	{
		if (UHorizontalBoxSlot* ButtonSlot = Row->AddChildToHorizontalBox(Button))
		{
			ButtonSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			ButtonSlot->SetPadding(FMargin(0.f, 0.f, Button == mDepot ? 4.f : 0.f, 0.f));
		}
	}
	ShowSetUp(TArray<FString>(), 0);
}

bool USTStationDepotBar::OnShown()
{
	UE_LOG(LogSmartTrains, Log, TEXT("Station depot bar for %s"), *STLayout::Name(GetStation()));
	return GetStation() != nullptr;
}

FText USTStationDepotBar::GetDepotText(bool bOn) const
{
	return bOn ? LOCTEXT("DepotBarOn", "SmartTrains depot: on") : LOCTEXT("DepotBarOff", "SmartTrains depot: off");
}

#undef LOCTEXT_NAMESPACE
