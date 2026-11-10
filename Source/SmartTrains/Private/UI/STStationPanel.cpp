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
	MakeHeading(Root, LOCTEXT("SetUpHeading", "Set up from its factory"));
	MakeText(Root, LOCTEXT("SetUpHint", "Follows the belts and pipes to the machines. Each button shows what it would do first; click it again to do it."), 10);
	mRename = MakeButton(Root, FText::GetEmpty(), [this]() { SendSetUp(mSetupReady == 1 ? 1 : 0); });
	mAutodetect = MakeButton(Root, FText::GetEmpty(), [this]() { SendSetUp(mSetupReady == 2 ? 3 : 2); });
	mSetUpLines = MakeText(Root, FText::GetEmpty(), 10, true, FMargin(0.f, 4.f, 0.f, 0.f));
	ShowSetUp(TArray<FString>(), 0);

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

void USTStationPanel::SendSetUp(uint8 Mode)
{
	USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this);
	AFGTrainStationIdentifier* Station = GetStation();
	if (!RCO || !Station)
	{
		return;
	}
	static const TCHAR* Names[] = {TEXT("rename preview"), TEXT("rename"), TEXT("autodetect preview"), TEXT("autodetect platforms")};
	UE_LOG(LogSmartTrains, Log, TEXT("Station menu: %s for %s"), Names[FMath::Min<int32>(Mode, 3)], *STLayout::Name(Station));
	RCO->Server_SetUpStation(Station, Mode, RCO->NextEdit());
	if (mSetUpLines)
	{
		mSetUpLines->SetText(LOCTEXT("SetUpAsking", "Tracing belts and pipes..."));
	}
}

void USTStationPanel::ShowSetUp(const TArray<FString>& Lines, uint8 Ready)
{
	if (!mRename)
	{
		return;
	}
	mSetupReady = Ready;
	mRename->SetText(Ready == 1 ? LOCTEXT("RenameConfirm", "Confirm: rename the station") : LOCTEXT("Rename", "Rename station from its factory..."));
	mAutodetect->SetText(Ready == 2 ? LOCTEXT("AutodetectConfirm", "Confirm: set every platform like this")
		: LOCTEXT("Autodetect", "Autodetect all platforms..."));
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
