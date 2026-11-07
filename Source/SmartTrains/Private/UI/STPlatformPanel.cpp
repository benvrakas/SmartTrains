#include "UI/STPlatformPanel.h"
#include "Blueprint/WidgetTree.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "FGInventoryComponent.h"
#include "Resources/FGItemDescriptor.h"
#include "SmartTrains.h"
#include "STRemoteCallObject.h"
#include "TimerManager.h"
#include "UI/STControls.h"
#include "UI/STWidgetUtils.h"

#define LOCTEXT_NAMESPACE "SmartTrains"

namespace
{
	constexpr float PanelWidth = 340.f;
	constexpr float PanelMaxHeight = 640.f;
	constexpr float PickerMaxHeight = 440.f;
	// Step of the batch and target buttons. [%]
	constexpr float PercentStep = 10.f;


	FText RoleHint(ESTRole Role)
	{
		switch (Role)
		{
		case ESTRole::Supply: return LOCTEXT("HintSupply", "Managed trains pick up these items here. The platform is set to load.");
		case ESTRole::Demand: return LOCTEXT("HintDemand", "Managed trains deliver these items here. The platform is set to unload.");
		case ESTRole::Sink: return LOCTEXT("HintSink", "Managed trains unload cargo here that no consumer takes, any item. The platform is set to unload.");
		default: return LOCTEXT("HintOff", "Off: the platform keeps its own load/unload setting and SmartTrains leaves it alone.");
		}
	}

	FText RequestHint(ESTRequest Request)
	{
		switch (Request)
		{
		case ESTRequest::On: return LOCTEXT("HintRequestOn", "On: until it and the deliveries on the way are 90% full, and no train has an empty car that lines up with it, a train that holds other cargo is emptied first and sent for it.");
		case ESTRequest::Off: return LOCTEXT("HintRequestOff", "Off: it only gets trains with room for its items.");
		default: return LOCTEXT("HintRequestAuto", "Automatic: when it will fall below its target soon (Mods menu: Request a train this early), too little is on the way and no train has an empty car that lines up with it, a train that holds other cargo is emptied first and sent for it.");
		}
	}

	template <typename EnumType>
	EnumType StepEnum(EnumType Value, int32 Step, int32 Count)
	{
		return static_cast<EnumType>(((static_cast<int32>(Value) + Step) % Count + Count) % Count);
	}

	ESTPriority StepPriority(ESTPriority Priority, int32 Step)
	{
		const int32 Count = static_cast<int32>(ESTPriority::Critical) + 1;
		return static_cast<ESTPriority>(((static_cast<int32>(Priority) + Step) % Count + Count) % Count);
	}

	// The amount the +/- buttons change: send-at share of a load for supply, target share for demand. [%]
	float GetPercent(const FSTItemRule& Rule, ESTRole Role)
	{
		return Role == ESTRole::Supply ? STNames::SendAtPercent(Rule.SendAtPercent) : Rule.TargetPercent;
	}

	// Adds Child to Box, filling the rest of the line if bFill.
	void AddToLine(UHorizontalBox* Box, UWidget* Child, bool bFill = false, const FMargin& Padding = FMargin(0.f, 0.f, 4.f, 0.f))
	{
		if (UHorizontalBoxSlot* Slot = Box->AddChildToHorizontalBox(Child))
		{
			Slot->SetPadding(Padding);
			Slot->SetVerticalAlignment(VAlign_Center);
			if (bFill)
			{
				Slot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			}
		}
	}
}

UWidget* USTPlatformPanel::WrapRoot(UVerticalBox* Root)
{
	UOverlay* Frame = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass());
	if (UWidget* Background = STWidgetUtils::CreateVanillaWidget(WidgetTree, STWidgetUtils::ContentBackgroundClass))
	{
		if (UOverlaySlot* BackgroundSlot = Frame->AddChildToOverlay(Background))
		{
			BackgroundSlot->SetHorizontalAlignment(HAlign_Fill);
			BackgroundSlot->SetVerticalAlignment(VAlign_Fill);
		}
	}
	USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Size->SetWidthOverride(PanelWidth);
	Size->SetMaxDesiredHeight(PanelMaxHeight);
	UScrollBox* Scroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	Scroll->AddChild(Root);
	Size->AddChild(Scroll);
	if (UOverlaySlot* ContentSlot = Frame->AddChildToOverlay(Size))
	{
		ContentSlot->SetPadding(FMargin(14.f, 10.f));
	}
	return Frame;
}

void USTPlatformPanel::BuildContent(UVerticalBox* Root)
{
	mMain = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Root->AddChildToVerticalBox(mMain);
	if (ShowsHeading())
	{
		MakeHeading(mMain, LOCTEXT("PlatformHeading", "SmartTrains"), FMargin(0.f, 0.f, 0.f, 4.f));
	}
	mTitle = MakeText(mMain, LOCTEXT("Loading", "Asking the server..."));

	// Click steps forward, right click back.
	mRoleButton = MakeButton(mMain, FText::GetEmpty(), [this]() { StepRole(1); }, FMargin(0.f, 6.f, 0.f, 2.f));
	mRoleButton->OnRightClick = [this]() { StepRole(-1); };
	mRoleHint = MakeText(mMain, RoleHint(ESTRole::Off), 10);
	mRequestButton = MakeButton(mMain, FText::GetEmpty(), [this]() { StepRequest(1); }, FMargin(0.f, 6.f, 0.f, 2.f));
	mRequestButton->OnRightClick = [this]() { StepRequest(-1); };
	mRequestHint = MakeText(mMain, RequestHint(ESTRequest::Auto), 10);

	mItemsLabel = MakeText(mMain, LOCTEXT("ItemsLabel", "Items"), 13, false, FMargin(0.f, 8.f, 0.f, 2.f));
	mNoItems = MakeText(mMain, LOCTEXT("NoItems", "No items yet. Add the item this platform supplies or demands."), 10);
	mNoItems->SetVisibility(ESlateVisibility::Collapsed);
	mItemBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	mMain->AddChildToVerticalBox(mItemBox);
	mAddItem = MakeButton(mMain, LOCTEXT("AddItem", "Add item..."), [this]() { OpenPicker(); }, FMargin(0.f, 4.f));

	UHorizontalBox* Tools = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	mMain->AddChildToVerticalBox(Tools)->SetPadding(FMargin(0.f, 8.f, 0.f, 2.f));
	AddToLine(Tools, MakeButton(nullptr, LOCTEXT("Copy", "Copy"), [this]()
	{
		USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this);
		if (RCO && GetPlatform())
		{
			UE_LOG(LogSmartTrains, Log, TEXT("Platform menu: copy settings of %s"), *GetPlatform()->GetName());
			RCO->Server_CopyPlatformSettings(GetPlatform(), false);
		}
	}), true);
	mPaste = MakeButton(nullptr, LOCTEXT("Paste", "Paste"), [this]()
	{
		USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this);
		if (RCO && GetPlatform())
		{
			mLastEdit = RCO->NextEdit();
			UE_LOG(LogSmartTrains, Log, TEXT("Platform menu: paste settings onto %s (edit %d)"), *GetPlatform()->GetName(), mLastEdit);
			RCO->Server_PastePlatformSettings(GetPlatform(), false, mLastEdit);
		}
	});
	AddToLine(Tools, mPaste, true, FMargin(0.f));
	mCopyToStation = MakeButton(mMain, LOCTEXT("CopyToStation", "Copy to the station's other platforms"), [this]()
	{
		USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this);
		if (RCO && GetPlatform())
		{
			mLastEdit = RCO->NextEdit();
			UE_LOG(LogSmartTrains, Log, TEXT("Platform menu: copy %s to its station (edit %d)"), *GetPlatform()->GetName(), mLastEdit);
			RCO->Server_CopyPlatformToStation(GetPlatform(), mLastEdit);
		}
	}, FMargin(0.f, 4.f, 0.f, 2.f));
	mCopyHint = MakeText(mMain, LOCTEXT("CopyHint", "Copy and Paste carry every setting of the platform. This gives every platform of this kind in the station all of them, once; each keeps its own settings after that."), 9);

	mLines = MakeText(mMain, FText::GetEmpty(), 10, true, FMargin(0.f, 8.f, 0.f, 0.f));
	mEditedBy = MakeText(mMain, FText::GetEmpty(), 9, true, FMargin(0.f, 4.f, 0.f, 0.f));

	mPicker = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Root->AddChildToVerticalBox(mPicker);
	mPicker->SetVisibility(ESlateVisibility::Collapsed);
	MakeHeading(mPicker, LOCTEXT("PickHeading", "Pick an item"), FMargin(0.f, 0.f, 0.f, 4.f));
	mSearch = MakeInput(mPicker, LOCTEXT("SearchHint", "Search"));
	mSearch->OnChanged = [this](const FString& Text) { FilterPicker(Text); };
	MakeButton(mPicker, LOCTEXT("Cancel", "Cancel"), [this]() { ClosePicker(); });
	USizeBox* ListBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	ListBox->SetMaxDesiredHeight(PickerMaxHeight);
	UScrollBox* ListScroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	mPickerList = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	ListScroll->AddChild(mPickerList);
	ListBox->AddChild(ListScroll);
	mPicker->AddChildToVerticalBox(ListBox)->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));
}

AFGBuildableTrainPlatformCargo* USTPlatformPanel::GetPlatform() const
{
	return STWidgetUtils::GetMenuPlatform(GetMenu());
}

bool USTPlatformPanel::OnShown()
{
	// The game reuses menu widgets, so the panel may still show another platform.
	bHasSettings = false;
	mLastEdit = 0;
	mRole = ESTRole::Off;
	mRequest = ESTRequest::Auto;
	mItems.Reset();
	mItemLines.Reset();
	mRowItems.Reset();
	mRows.Reset();
	mItemBox->ClearChildren();
	mLines->SetText(FText::GetEmpty());
	mTitle->SetText(LOCTEXT("Loading", "Asking the server..."));
	ClosePicker();
	const AFGBuildableTrainPlatformCargo* Platform = GetPlatform();
	UE_LOG(LogSmartTrains, Log, TEXT("Platform menu for %s"), *GetNameSafe(Platform));
	// The window's layout is still being built during construction; log where the panel landed once it is drawn.
	if (UWorld* World = GetWorld())
	{
		FTimerHandle Handle;
		World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateWeakLambda(this, [this]()
		{
			const UUserWidget* Menu = GetMenu();
			UE_LOG(LogSmartTrains, Log, TEXT("Platform panel geometry: panel at %s size %s; menu %s at %s size %s"),
				*GetCachedGeometry().GetAbsolutePosition().ToString(), *GetCachedGeometry().GetLocalSize().ToString(), *GetNameSafe(Menu),
				Menu ? *Menu->GetCachedGeometry().GetAbsolutePosition().ToString() : TEXT("-"), Menu ? *Menu->GetCachedGeometry().GetLocalSize().ToString() : TEXT("-"));
		}), 0.5f, false);
	}
	return Platform != nullptr;
}

void USTPlatformPanel::RequestView(USTRemoteCallObject* RCO)
{
	if (AFGBuildableTrainPlatformCargo* Platform = GetPlatform())
	{
		RCO->Server_RequestPlatform(Platform);
	}
}

void USTPlatformPanel::Refresh(USTRemoteCallObject* RCO)
{
	const FSTPlatformView* View = RCO->GetPlatformView(GetPlatform());
	if (!View)
	{
		return;
	}
	mTitle->SetText(FText::FromString(View->Title));
	bFluid = View->bFluid;
	if (View->AppliedEdit >= mLastEdit)
	{
		mRole = View->Role;
		mRequest = View->Request;
		mItems = View->Items;
		mItemLines = View->ItemLines;
		bHasSettings = true;
	}
	SetLines(mLines, View->Lines);
	mCopyToStation->SetVisibility(View->bOnStation ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	mCopyHint->SetVisibility(View->bOnStation ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	mEditedBy->SetText(View->EditedBy.IsEmpty() ? FText::GetEmpty() : FText::Format(LOCTEXT("EditedBy", "Last changed by {0}"), FText::FromString(View->EditedBy)));
	RefreshEditor();
}

void USTPlatformPanel::SendEdit(const TCHAR* What)
{
	USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this);
	AFGBuildableTrainPlatformCargo* Platform = GetPlatform();
	if (!RCO || !Platform)
	{
		return;
	}
	// "Any item" is the Sink role's own rule.
	if (mRole != ESTRole::Sink)
	{
		mItems.RemoveAll([](const FSTItemRule& Rule) { return !Rule.Item; });
	}
	mLastEdit = RCO->NextEdit();
	UE_LOG(LogSmartTrains, Log, TEXT("Platform menu: %s on %s -> %s, %d items (edit %d)"), What, *Platform->GetName(), STNames::Role(mRole), mItems.Num(), mLastEdit);
	RCO->Server_SetPlatform(Platform, mRole, mItems, mLastEdit);
	RefreshEditor();
}

void USTPlatformPanel::StepRole(int32 Step)
{
	SetRole(StepEnum(mRole, Step, static_cast<int32>(ESTRole::Sink) + 1));
}

void USTPlatformPanel::StepRequest(int32 Step)
{
	USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this);
	AFGBuildableTrainPlatformCargo* Platform = GetPlatform();
	if (!bHasSettings || mRole != ESTRole::Demand || !RCO || !Platform)
	{
		return;
	}
	mRequest = StepEnum(mRequest, Step, static_cast<int32>(ESTRequest::Off) + 1);
	mLastEdit = RCO->NextEdit();
	UE_LOG(LogSmartTrains, Log, TEXT("Platform menu: request on %s -> %s (edit %d)"), *Platform->GetName(), STNames::Request(mRequest), mLastEdit);
	RCO->Server_SetPlatformRequest(Platform, mRequest, mLastEdit);
	RefreshEditor();
}

void USTPlatformPanel::SetRole(ESTRole NewRole)
{
	if (!bHasSettings || NewRole == mRole)
	{
		return;
	}
	mRole = NewRole;
	SendEdit(TEXT("role"));
}

void USTPlatformPanel::AddItem(TSubclassOf<UFGItemDescriptor> Item)
{
	ClosePicker();
	if (mItems.ContainsByPredicate([Item](const FSTItemRule& Rule) { return Rule.Item == Item; }))
	{
		return;
	}
	FSTItemRule& Rule = mItems.AddDefaulted_GetRef();
	Rule.Item = Item;
	Rule.Priority = Item ? ESTPriority::Normal : ESTPriority::Low;
	SendEdit(TEXT("add item"));
}

void USTPlatformPanel::RemoveItem(int32 Index)
{
	if (mItems.IsValidIndex(Index))
	{
		mItems.RemoveAt(Index);
		SendEdit(TEXT("remove item"));
	}
}

void USTPlatformPanel::CyclePriority(int32 Index, int32 Step)
{
	if (mItems.IsValidIndex(Index))
	{
		mItems[Index].Priority = StepPriority(mItems[Index].Priority, Step);
		SendEdit(TEXT("priority"));
	}
}

void USTPlatformPanel::StepAmount(int32 Index, int32 Step)
{
	if (!mItems.IsValidIndex(Index))
	{
		return;
	}
	FSTItemRule& Rule = mItems[Index];
	const float Value = FMath::Clamp(GetPercent(Rule, mRole) + Step * PercentStep, PercentStep, 100.f);
	if (mRole == ESTRole::Supply)
	{
		Rule.SendAtPercent = Value;
	}
	else
	{
		Rule.TargetPercent = Value;
	}
	SendEdit(TEXT("amount"));
}

void USTPlatformPanel::SetRate(int32 Index, const FString& Text)
{
	if (!mItems.IsValidIndex(Index))
	{
		return;
	}
	const float Shown = Text.TrimStartAndEnd().IsEmpty() ? 0.f : FMath::Max(0.f, FCString::Atof(*Text));
	// Inventories count fluids in litres; the field is in m³ like the rest of the game's UI.
	const float Rate = STNames::IsFluid(mItems[Index].Item) ? Shown * 1000.f : Shown;
	if (FMath::IsNearlyEqual(Rate, mItems[Index].ManualRatePerMinute))
	{
		return;
	}
	mItems[Index].ManualRatePerMinute = Rate;
	SendEdit(TEXT("manual rate"));
}

void USTPlatformPanel::RefreshEditor()
{
	mRoleButton->SetText(FText::Format(LOCTEXT("RoleButton", "Role: {0}"), FText::FromString(STNames::Role(mRole))));
	mRoleButton->SetIsEnabled(bHasSettings);
	mRoleHint->SetText(RoleHint(mRole));
	const bool bDemand = mRole == ESTRole::Demand;
	mRequestButton->SetText(FText::Format(LOCTEXT("RequestButton", "Request a train: {0}"), FText::FromString(STNames::Request(mRequest))));
	mRequestButton->SetIsEnabled(bHasSettings);
	mRequestButton->SetVisibility(bDemand ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	mRequestHint->SetText(RequestHint(mRequest));
	mRequestHint->SetVisibility(bDemand ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	const bool bHasItems = mRole == ESTRole::Supply || mRole == ESTRole::Demand;
	mItemsLabel->SetVisibility(bHasItems ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	mItemBox->SetVisibility(bHasItems ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	mAddItem->SetVisibility(bHasItems ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	mAddItem->SetIsEnabled(bHasSettings);
	const USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this);
	mPaste->SetIsEnabled(bHasSettings && RCO && RCO->HasClipboard(bFluid));

	TArray<TSubclassOf<UFGItemDescriptor>> Items;
	for (const FSTItemRule& Rule : mItems)
	{
		Items.Add(Rule.Item);
	}
	if (Items != mRowItems)
	{
		mRowItems = Items;
		RebuildItemRows();
	}
	for (int32 Index = 0; Index < mRows.Num() && Index < mItems.Num(); ++Index)
	{
		const FSTItemRule& Rule = mItems[Index];
		FItemWidgets& Row = mRows[Index];
		if (Row.Priority.IsValid())
		{
			Row.Priority->SetText(FText::Format(LOCTEXT("Priority", "Priority: {0}"), FText::FromString(STNames::Priority(Rule.Priority))));
			// Priority counts only where cargo is delivered.
			Row.Priority->SetVisibility(mRole == ESTRole::Supply ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
		}
		if (Row.Amount.IsValid())
		{
			Row.Amount->SetText(mRole == ESTRole::Supply
				? FText::Format(LOCTEXT("SendAt", "Send at {0}% of a load"), FText::AsNumber(FMath::RoundToInt(GetPercent(Rule, mRole))))
				: FText::Format(LOCTEXT("Target", "Keep stocked at {0}%"), FText::AsNumber(FMath::RoundToInt(GetPercent(Rule, mRole)))));
		}
		if (Row.AmountRow.IsValid())
		{
			Row.AmountRow->SetVisibility(Rule.Item ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
		}
		if (Row.Readout.IsValid())
		{
			Row.Readout->SetText(mItemLines.IsValidIndex(Index) ? FText::FromString(mItemLines[Index]) : FText::GetEmpty());
		}
		if (Row.Rate.IsValid())
		{
			const float Shown = STNames::IsFluid(Rule.Item) ? Rule.ManualRatePerMinute / 1000.f : Rule.ManualRatePerMinute;
			Row.Rate->SetTextIfIdle(Rule.ManualRatePerMinute > 0.f ? FString::SanitizeFloat(Shown, 0) : FString());
		}
	}
	mNoItems->SetVisibility(bHasSettings && mItems.Num() == 0 && bHasItems ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
}

void USTPlatformPanel::RebuildItemRows()
{
	mItemBox->ClearChildren();
	mRows.Reset();
	for (int32 Index = 0; Index < mItems.Num(); ++Index)
	{
		const TSubclassOf<UFGItemDescriptor> Item = mItems[Index].Item;
		FItemWidgets& Row = mRows.AddDefaulted_GetRef();
		UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
		mItemBox->AddChildToVerticalBox(Box)->SetPadding(FMargin(0.f, 4.f, 0.f, 6.f));

		UHorizontalBox* Head = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		Box->AddChildToVerticalBox(Head);
		USizeBox* IconBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		IconBox->SetWidthOverride(28.f);
		IconBox->SetHeightOverride(28.f);
		UImage* Icon = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
		if (UTexture2D* Texture = Item ? UFGItemDescriptor::GetSmallIcon(Item) : nullptr)
		{
			Icon->SetBrushFromTexture(Texture);
		}
		else
		{
			IconBox->SetVisibility(ESlateVisibility::Collapsed);
		}
		IconBox->AddChild(Icon);
		AddToLine(Head, IconBox, false, FMargin(0.f, 0.f, 6.f, 0.f));
		UTextBlock* Name = MakeText(nullptr, Item ? UFGItemDescriptor::GetItemName(Item) : LOCTEXT("AnyItem", "Any item (sink for surplus)"), 12, true);
		AddToLine(Head, Name, true);
		AddToLine(Head, MakeButton(nullptr, LOCTEXT("Remove", "Remove"), [this, Index]() { RemoveItem(Index); }), false, FMargin(0.f));
		Row.Name = Name;
		Row.Icon = Icon;

		UHorizontalBox* Settings = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		Box->AddChildToVerticalBox(Settings)->SetPadding(FMargin(0.f, 2.f, 0.f, 0.f));
		USTButton* PriorityButton = MakeButton(nullptr, FText::GetEmpty(), [this, Index]() { CyclePriority(Index, 1); });
		PriorityButton->OnRightClick = [this, Index]() { CyclePriority(Index, -1); };
		AddToLine(Settings, PriorityButton, true);
		Row.Priority = PriorityButton;

		UHorizontalBox* AmountRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		Box->AddChildToVerticalBox(AmountRow)->SetPadding(FMargin(0.f, 2.f, 0.f, 0.f));
		AddToLine(AmountRow, MakeButton(nullptr, FText::FromString(TEXT("-")), [this, Index]() { StepAmount(Index, -1); }));
		UTextBlock* Amount = MakeText(nullptr, FText::GetEmpty(), 11, false);
		AddToLine(AmountRow, Amount, true, FMargin(4.f, 0.f));
		AddToLine(AmountRow, MakeButton(nullptr, FText::FromString(TEXT("+")), [this, Index]() { StepAmount(Index, 1); }), false, FMargin(0.f));
		Row.Amount = Amount;
		Row.AmountRow = AmountRow;

		Row.Readout = MakeText(Box, FText::GetEmpty(), 10);

		if (Item)
		{
			UHorizontalBox* RateRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
			Box->AddChildToVerticalBox(RateRow)->SetPadding(FMargin(0.f, 2.f, 0.f, 0.f));
			AddToLine(RateRow, MakeText(nullptr, STNames::IsFluid(Item) ? LOCTEXT("RateFluid", "Manual rate (m³/min):") : LOCTEXT("RateSolid", "Manual rate (/min):"), 10, false));
			USizeBox* RateBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
			RateBox->SetWidthOverride(110.f);
			USTTextInput* Rate = MakeInput(nullptr, LOCTEXT("RateHint", "auto"));
			Rate->OnCommitted = [this, Index](const FString& Text) { SetRate(Index, Text); };
			RateBox->AddChild(Rate);
			AddToLine(RateRow, RateBox, false, FMargin(0.f));
			Row.Rate = Rate;
		}
	}
	UE_LOG(LogSmartTrains, Verbose, TEXT("Platform menu: %d item rows built"), mRows.Num());
}

void USTPlatformPanel::OpenPicker()
{
	const AFGBuildableTrainPlatformCargo* Platform = GetPlatform();
	mPickerList->ClearChildren();
	mPickerRows.Reset();
	TSet<UClass*> Shown;
	for (const FSTItemRule& Rule : mItems)
	{
		Shown.Add(Rule.Item.Get());
	}
	auto AddRow = [this, &Shown](TSubclassOf<UFGItemDescriptor> Item, const FText& Text)
	{
		Shown.Add(Item.Get());
		USTRow* Row = MakeRow(mPickerList, Text, [this, Item]() { AddItem(Item); });
		Row->SetIcon(Item ? UFGItemDescriptor::GetSmallIcon(Item) : nullptr);
		mPickerRows.Add({Row, Item});
	};
	// Items on the platform now come first.
	if (const UFGInventoryComponent* Inventory = Platform ? Platform->GetInventory() : nullptr)
	{
		TArray<FInventoryStack> Stacks;
		Inventory->GetInventoryStacks(Stacks);
		for (const FInventoryStack& Stack : Stacks)
		{
			const TSubclassOf<UFGItemDescriptor> Item = Stack.Item.GetItemClass();
			if (Item && !Shown.Contains(Item.Get()))
			{
				AddRow(Item, FText::Format(LOCTEXT("PickHere", "{0} (on this platform)"), UFGItemDescriptor::GetItemName(Item)));
			}
		}
	}
	for (const TSubclassOf<UFGItemDescriptor>& Item : STWidgetUtils::GetPickableItems(this, bFluid))
	{
		if (!Shown.Contains(Item.Get()))
		{
			AddRow(Item, UFGItemDescriptor::GetItemName(Item));
		}
	}
	mSearch->SetTextIfIdle(FString());
	FilterPicker(FString());
	mMain->SetVisibility(ESlateVisibility::Collapsed);
	mPicker->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	UE_LOG(LogSmartTrains, Log, TEXT("Platform menu: item picker opened with %d items"), mPickerRows.Num());
}

void USTPlatformPanel::ClosePicker()
{
	if (mPicker)
	{
		mPicker->SetVisibility(ESlateVisibility::Collapsed);
	}
	if (mMain)
	{
		mMain->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	}
}

void USTPlatformPanel::FilterPicker(const FString& Search)
{
	const FString Wanted = Search.TrimStartAndEnd();
	for (const TPair<TWeakObjectPtr<USTRow>, TSubclassOf<UFGItemDescriptor>>& Each : mPickerRows)
	{
		if (USTRow* Row = Each.Key.Get())
		{
			const FString Name = Each.Value ? UFGItemDescriptor::GetItemName(Each.Value).ToString() : FString(TEXT("Any item"));
			Row->SetVisibility(Wanted.IsEmpty() || Name.Contains(Wanted, ESearchCase::IgnoreCase) ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		}
	}
}

#undef LOCTEXT_NAMESPACE
