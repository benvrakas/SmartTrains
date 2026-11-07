#include "UI/STStationPanel.h"
#include "Blueprint/WidgetTree.h"
#include "Buildables/FGBuildableRailroadStation.h"
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
	}
}

void USTStationDepotBar::BuildContent(UVerticalBox* Root)
{
	mDepot = MakeButton(Root, GetDepotText(false), [this]() { SendStation(!bDepot); });
}

bool USTStationDepotBar::OnShown()
{
	UE_LOG(LogSmartTrains, Log, TEXT("Station depot bar for %s"), *STLayout::Name(GetStation()));
	return GetStation() != nullptr;
}

FText USTStationDepotBar::GetDepotText(bool bOn) const
{
	return bOn ? LOCTEXT("DepotBarOn", "SmartTrains depot (rail yard for waiting trains): On")
		: LOCTEXT("DepotBarOff", "SmartTrains depot (rail yard for waiting trains): Off");
}

#undef LOCTEXT_NAMESPACE
