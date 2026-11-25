#include "UI/STRideHud.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "FGCharacterPlayer.h"
#include "FGDriveablePawn.h"
#include "FGLocomotive.h"
#include "FGTrain.h"
#include "GameFramework/PlayerController.h"
#include "SmartTrains.h"
#include "STRemoteCallObject.h"
#include "UI/STStyle.h"
#include "UI/STWidgetUtils.h"

#define LOCTEXT_NAMESPACE "SmartTrains"

namespace
{
	// Width of the card and its distance from the screen's top right corner. [slate units]
	constexpr float HudWidth = 380.f;
	const FVector2D HudOffset(-24.f, 170.f);
	// How often the client checks what the player rides. [s]
	constexpr float HudCheckSeconds = 0.5f;

	TWeakObjectPtr<USTRideHud> GHud;

	// The train the local player sits in, or null.
	AFGTrain* GetRiddenTrain(APlayerController* Controller)
	{
		APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
		const AFGLocomotive* Locomotive = Cast<AFGLocomotive>(Pawn);
		if (!Locomotive)
		{
			if (const AFGCharacterPlayer* Character = Cast<AFGCharacterPlayer>(Pawn))
			{
				Locomotive = Cast<AFGLocomotive>(Character->GetDrivenVehicle());
			}
		}
		return Locomotive ? Locomotive->GetTrain() : nullptr;
	}

	bool TickRideHud(float)
	{
		UWorld* World = GEngine && GEngine->GameViewport ? GEngine->GameViewport->GetWorld() : nullptr;
		APlayerController* Controller = World ? World->GetFirstPlayerController() : nullptr;
		AFGTrain* Train = Controller && Controller->IsLocalController() ? GetRiddenTrain(Controller) : nullptr;
		if (GHud.IsValid() && (!Train || GHud->GetWorld() != World))
		{
			GHud->RemoveFromParent();
			GHud.Reset();
			UE_LOG(LogSmartTrains, Log, TEXT("Ride HUD hidden"));
		}
		if (Train && !GHud.IsValid())
		{
			USTRideHud* Hud = CreateWidget<USTRideHud>(Controller, USTRideHud::StaticClass());
			if (!Hud)
			{
				return true;
			}
			Hud->SetTrain(Train);
			Hud->AddToViewport(5);
			Hud->SetAnchorsInViewport(FAnchors(1.f, 0.f));
			Hud->SetAlignmentInViewport(FVector2D(1.f, 0.f));
			Hud->SetPositionInViewport(HudOffset, false);
			GHud = Hud;
			UE_LOG(LogSmartTrains, Log, TEXT("Ride HUD shown for %s"), *Train->GetTrainName().ToString());
		}
		else if (Train)
		{
			GHud->SetTrain(Train);
		}
		return true;
	}
}

void USTRideHud::Install()
{
	static FTSTicker::FDelegateHandle Handle;
	if (!Handle.IsValid())
	{
		Handle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickRideHud), HudCheckSeconds);
		UE_LOG(LogSmartTrains, Log, TEXT("Ride HUD check started"));
	}
}

UWidget* USTRideHud::WrapRoot(UVerticalBox* Root)
{
	UBorder* Frame = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Frame->SetBrush(STStyle::Outlined(STStyle::Panel, STStyle::Line, 1.f, 5.f));
	Frame->SetPadding(FMargin(10.f, 8.f, 10.f, 10.f));
	Frame->SetContent(Root);
	USizeBox* Width = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Width->SetWidthOverride(HudWidth);
	Width->AddChild(Frame);
	return Width;
}

void USTRideHud::BuildContent(UVerticalBox* Root)
{
	mFontSource = STWidgetUtils::CreateVanillaWidget(WidgetTree, STWidgetUtils::MenuLabelClass);
	if (mFontSource)
	{
		Root->AddChildToVerticalBox(mFontSource);
		mFontSource->SetVisibility(ESlateVisibility::Collapsed);
		SetMenu(mFontSource);
	}
	MakeTitleBar(Root, FText::GetEmpty(), 24.f);
	mTitle = MakeTinted(Root, FText::GetEmpty(), 15, STStyle::Text, true, true, FMargin(0.f, 0.f, 0.f, 2.f));
	BuildTrainContent(Root);
	SetVisibility(ESlateVisibility::Collapsed);
}

void USTRideHud::Refresh(USTRemoteCallObject* RCO)
{
	const FSTTrainView* View = RCO->GetTrainView(GetTrain());
	if (!View || !View->bManaged)
	{
		SetVisibility(ESlateVisibility::Collapsed);
		return;
	}
	SetVisibility(ESlateVisibility::HitTestInvisible);
	mTitle->SetText(View->Consumer.IsEmpty() ? FText::FromString(GetTrain() ? GetTrain()->GetTrainName().ToString() : FString())
		: FText::Format(LOCTEXT("HudTitle", "To {0}"), FText::FromString(View->Consumer)));
	ShowTrain(*View);
}

void USTRideHud::SetTrain(AFGTrain* InTrain)
{
	mTrain = InTrain;
}

#undef LOCTEXT_NAMESPACE
