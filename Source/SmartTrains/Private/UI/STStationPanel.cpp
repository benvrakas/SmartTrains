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
	mDepot = MakeButton(Root, LOCTEXT("DepotOff", "Depot: off"), [this]() { SendStation(!bDepot, mPoolValue); });
	MakeText(Root, LOCTEXT("DepotHint", "Idle managed trains park at the nearest depot."), 10);
	MakeText(Root, LOCTEXT("StationPoolLabel", "Pool tag (only trains with the same tag serve this station):"), 10, true, FMargin(0.f, 6.f, 0.f, 2.f));
	mPool = MakeInput(Root, LOCTEXT("StationPoolHint", "no pool"));
	mPool->OnCommitted = [this](const FString& Text) { SendStation(bDepot, Text.TrimStartAndEnd()); };
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
}

#undef LOCTEXT_NAMESPACE
