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
#include "UI/STStyle.h"
#include "Components/WrapBox.h"

#define LOCTEXT_NAMESPACE "SmartTrains"

namespace
{
	constexpr float PanelWidth = 340.f;
	constexpr float PanelMaxHeight = 640.f;
	constexpr float PickerMaxHeight = 440.f;


	FText RoleHint(ESTRole Role)
	{
		switch (Role)
		{
		case ESTRole::Supply: return LOCTEXT("HintSupply", "Managed trains pick up these items here. The platform is set to load.");
		case ESTRole::Demand: return LOCTEXT("HintDemand", "Managed trains deliver these items here: the car beside this platform carries one of them on every train of this station's loop. Several items make a group. The platform is set to unload.");
		case ESTRole::Sink: return LOCTEXT("HintSink", "A train switching to another station's loop unloads cargo here that its old consumer had no room for, any item. The platform is set to unload.");
		default: return LOCTEXT("HintOff", "Off: the platform keeps its own load/unload setting and SmartTrains leaves it alone.");
		}
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
		MakeTitleBar(mMain, FText::GetEmpty(), 20.f);
	}
	mTitle = MakeTinted(mMain, LOCTEXT("Loading", "Asking the server..."), 13, STStyle::Text, true, true);

	MakeLabel(mMain, LOCTEXT("RoleLabel", "Role"));
	UHorizontalBox* Roles = MakeHBox(mMain);
	for (int32 Index = 0; Index <= static_cast<int32>(ESTRole::Sink); ++Index)
	{
		const ESTRole Role = static_cast<ESTRole>(Index);
		USTSegment* Segment = WidgetTree->ConstructWidget<USTSegment>(USTSegment::StaticClass());
		Segment->SetText(FText::FromString(STNames::Role(Role)));
		Segment->SetFont(GetFont(11, true));
		Segment->OnClick = [this, Role]() { SetRole(Role); };
		Roles->AddChildToHorizontalBox(Segment);
		SetFill(Segment);
		SetSlotPadding(Segment, FMargin(Index > 0 ? 3.f : 0.f, 0.f, 0.f, 0.f));
		mRoleSegments.Add(Segment);
	}
	mRoleHint = MakeTinted(mMain, RoleHint(ESTRole::Off), 10, STStyle::Muted, false, true, FMargin(0.f, 4.f, 0.f, 0.f));

	mItemsLabel = MakeLabel(mMain, LOCTEXT("ItemsLabel", "Items"));
	mNoItems = MakeTinted(mMain, LOCTEXT("NoItems", "No items yet. Add the item this platform supplies or demands."), 10, STStyle::Muted);
	mNoItems->SetVisibility(ESlateVisibility::Collapsed);
	mItemBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	mMain->AddChildToVerticalBox(mItemBox);
	mAddItem = MakeButton(mMain, LOCTEXT("AddItem", "Add item..."), [this]() { OpenPicker(); }, FMargin(0.f, 4.f));

	mPriorityLabel = MakeLabel(mMain, LOCTEXT("PriorityLabel", "Priority"));
	mPriorityBar = MakeHBox(mMain);
	for (int32 Index = 0; Index <= static_cast<int32>(ESTPriority::High); ++Index)
	{
		const ESTPriority Priority = static_cast<ESTPriority>(Index);
		USTSegment* Segment = WidgetTree->ConstructWidget<USTSegment>(USTSegment::StaticClass());
		Segment->SetText(FText::FromString(STNames::Priority(Priority)));
		Segment->SetFont(GetFont(11, true));
		Segment->OnClick = [this, Priority]() { SetPriority(Priority); };
		mPriorityBar->AddChildToHorizontalBox(Segment);
		SetFill(Segment);
		SetSlotPadding(Segment, FMargin(Index > 0 ? 3.f : 0.f, 0.f, 0.f, 0.f));
		mPrioritySegments.Add(Segment);
	}
	mPriorityHint = MakeTinted(mMain, FText::GetEmpty(), 10, STStyle::Muted, false, true, FMargin(0.f, 4.f, 0.f, 0.f));

	UHorizontalBox* Tools = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	mMain->AddChildToVerticalBox(Tools)->SetPadding(FMargin(0.f, 8.f, 0.f, 2.f));
	AddToLine(Tools, MakeButton(nullptr, LOCTEXT("Copy", "Copy"), [this]()
	{
		if (USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this))
		{
			RCO->CopyPlatformFromMenu(GetPlatform());
		}
	}), true);
	mPaste = MakeButton(nullptr, LOCTEXT("Paste", "Paste"), [this]()
	{
		USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this);
		if (RCO && GetPlatform())
		{
			mLastEdit = RCO->PastePlatformFromMenu(GetPlatform());
		}
	});
	AddToLine(Tools, mPaste, true, FMargin(0.f));
	MakeButton(mMain, LOCTEXT("Autodetect", "Autodetect from its belts and pipes"), [this]()
	{
		if (USTRemoteCallObject* RCO = USTRemoteCallObject::GetLocal(this))
		{
			UE_LOG(LogSmartTrains, Log, TEXT("Platform menu: autodetect %s"), *GetNameSafe(GetPlatform()));
			RCO->Server_AutodetectPlatform(GetPlatform(), RCO->NextEdit());
		}
	}, FMargin(0.f, 4.f, 0.f, 2.f));
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
	mCopyHint = MakeTinted(mMain, LOCTEXT("CopyHint", "Gives every platform of this kind in the station these settings, once."), 9, STStyle::Muted);

	UHorizontalBox* Stats = MakeHBox(mMain, FMargin(0.f, 10.f, 0.f, 0.f));
	UVerticalBox* A = MakeVBox(Stats);
	SetFill(A);
	mStatRate = MakeStat(A, LOCTEXT("PlatformRate", "per minute at most"));
	UVerticalBox* B = MakeVBox(Stats, FMargin(4.f, 0.f, 0.f, 0.f));
	SetFill(B);
	mStatLoops = MakeStat(B, LOCTEXT("PlatformLoops", "loops"));
	mNext = MakeTinted(mMain, FText::GetEmpty(), 10, STStyle::Text, false, true, FMargin(0.f, 6.f, 0.f, 0.f));
	mLines = MakeTinted(mMain, FText::GetEmpty(), 10, STStyle::Muted, false, true, FMargin(0.f, 4.f, 0.f, 0.f));
	mEditedBy = MakeTinted(mMain, FText::GetEmpty(), 9, STStyle::Muted, false, true, FMargin(0.f, 4.f, 0.f, 0.f));

	mPicker = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Root->AddChildToVerticalBox(mPicker);
	mPicker->SetVisibility(ESlateVisibility::Collapsed);
	MakeTitleBar(mPicker, LOCTEXT("PickHeading", "Pick an item"), 20.f);
	mSearch = MakeInput(mPicker, LOCTEXT("SearchHint", "Search items"));
	mSearch->OnChanged = [this](const FString& Text) { FilterPicker(Text); };
	MakeButton(mPicker, LOCTEXT("Cancel", "Cancel"), [this]() { ClosePicker(); });
	USizeBox* ListBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	ListBox->SetMaxDesiredHeight(PickerMaxHeight);
	UScrollBox* ListScroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	mPickerList = WidgetTree->ConstructWidget<UWrapBox>(UWrapBox::StaticClass());
	mPickerList->SetInnerSlotPadding(FVector2D(4.f, 4.f));
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
	mItems.Reset();
	mPriority = ESTPriority::Medium;
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
		mItems = View->Items;
		mPriority = View->Priority;
		mItemLines = View->ItemLines;
		bHasSettings = true;
	}
	// Long readouts stay in /st status; the panel shows the numbers and the next train.
	const bool bWorks = View->Role == ESTRole::Supply || View->Role == ESTRole::Demand;
	mStatRate.Value->SetText(FText::FromString(View->RatePerMinute >= 0.f ? FString::Printf(TEXT("%.0f%s"), View->RatePerMinute, View->bFluid ? TEXT(" m³") : TEXT(""))
		: FString(TEXT("-"))));
	mStatRate.Label->SetText(View->Role == ESTRole::Supply ? LOCTEXT("PlatformRateSupply", "made per minute at most")
		: LOCTEXT("PlatformRateDemand", "used per minute at most"));
	mStatLoops.Value->SetText(FText::AsNumber(View->Loops));
	mStatLoops.Label->SetText(View->Role == ESTRole::Supply ? LOCTEXT("PlatformLoopsSupply", "loops take from here") : LOCTEXT("PlatformLoopsDemand", "trains on its loop"));
	mStatRate.Value->GetParent()->GetParent()->SetVisibility(bWorks ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	mStatLoops.Value->GetParent()->GetParent()->SetVisibility(bWorks ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	mNext->SetText(View->NextTrain.IsEmpty() ? FText::GetEmpty() : FText::FromString(View->NextSeconds == 0.f ? FString::Printf(TEXT("Docked now: %s"), *View->NextTrain)
		: FString::Printf(TEXT("Next train: %s%s"), *View->NextTrain, View->NextSeconds > 0.f ? *FString::Printf(TEXT(" in %s"), *STStyle::Duration(View->NextSeconds)) : TEXT(""))));
	mNext->SetVisibility(View->NextTrain.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);
	// Only the reason a consumer gets no train (its problem), not the history.
	TArray<FString> Notes;
	for (const FString& Line : View->Lines)
	{
		if (!Line.StartsWith(TEXT("Recent: ")) && !Line.StartsWith(TEXT("Next: ")) && !Line.StartsWith(TEXT("Its station's loop")))
		{
			Notes.Add(Line);
		}
	}
	SetLines(mLines, Notes);
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
	UE_LOG(LogSmartTrains, Log, TEXT("Platform menu: %s on %s -> %s, %d items, %s priority (edit %d)"), What, *Platform->GetName(), STNames::Role(mRole), mItems.Num(),
		STNames::Priority(mPriority), mLastEdit);
	RCO->Server_SetPlatform(Platform, mRole, mItems, mPriority, mLastEdit);
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

void USTPlatformPanel::SetPriority(ESTPriority NewPriority)
{
	if (!bHasSettings || NewPriority == mPriority)
	{
		return;
	}
	mPriority = NewPriority;
	SendEdit(TEXT("priority"));
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

void USTPlatformPanel::RefreshEditor()
{
	for (int32 Index = 0; Index < mRoleSegments.Num(); ++Index)
	{
		mRoleSegments[Index]->SetSelected(static_cast<int32>(mRole) == Index);
		mRoleSegments[Index]->SetIsEnabled(bHasSettings);
	}
	mRoleHint->SetText(RoleHint(mRole));
	const bool bHasItems = mRole == ESTRole::Supply || mRole == ESTRole::Demand;
	mItemsLabel->SetVisibility(bHasItems ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	mItemBox->SetVisibility(bHasItems ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	mAddItem->SetVisibility(bHasItems ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
	mAddItem->SetIsEnabled(bHasSettings);
	const bool bDemand = mRole == ESTRole::Demand;
	mPriorityLabel->SetVisibility(bDemand ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	mPriorityBar->SetVisibility(bDemand ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	mPriorityHint->SetVisibility(bDemand ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	for (int32 Index = 0; Index < mPrioritySegments.Num(); ++Index)
	{
		mPrioritySegments[Index]->SetSelected(static_cast<int32>(mPriority) == Index);
		mPrioritySegments[Index]->SetIsEnabled(bHasSettings);
	}
	mPriorityHint->SetText(mPriority == ESTPriority::High
		? LOCTEXT("PriorityHigh", "Its station takes trains from other loops until it has enough for its factory's full input. The station's highest platform priority counts.")
		: mPriority == ESTPriority::Low
		? LOCTEXT("PriorityLow", "Its station gets trains only once every Medium and High loop has enough for its full input.")
		: LOCTEXT("PriorityMedium", "Its station shares the trains High loops leave with the other Medium loops, by need."));
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
		if (Row.Readout.IsValid())
		{
			Row.Readout->SetText(mItemLines.IsValidIndex(Index) ? FText::FromString(mItemLines[Index]) : FText::GetEmpty());
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
		UVerticalBox* Box = MakeCard(mItemBox, STStyle::Card, FMargin(8.f, 6.f), FMargin(0.f, 2.f));

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
		UTextBlock* Name = MakeTinted(nullptr, Item ? UFGItemDescriptor::GetItemName(Item) : LOCTEXT("AnyItem", "Any item"), 12, STStyle::Text, true, true);
		AddToLine(Head, Name, true);
		AddToLine(Head, MakeButton(nullptr, LOCTEXT("Remove", "Remove"), [this, Index]() { RemoveItem(Index); }), false, FMargin(0.f));
		Row.Name = Name;
		Row.Icon = Icon;

		Row.Readout = MakeTinted(Box, FText::GetEmpty(), 10, STStyle::Muted, false, true, FMargin(0.f, 3.f, 0.f, 0.f));

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
		USTIconTile* Tile = WidgetTree->ConstructWidget<USTIconTile>(USTIconTile::StaticClass());
		Tile->SetItem(Item ? UFGItemDescriptor::GetSmallIcon(Item) : nullptr, Text);
		Tile->OnClick = [this, Item]() { AddItem(Item); };
		mPickerList->AddChild(Tile);
		mPickerRows.Add({Tile, Item});
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
	for (const TPair<TWeakObjectPtr<UWidget>, TSubclassOf<UFGItemDescriptor>>& Each : mPickerRows)
	{
		if (UWidget* Row = Each.Key.Get())
		{
			const FString Name = Each.Value ? UFGItemDescriptor::GetItemName(Each.Value).ToString() : FString(TEXT("Any item"));
			Row->SetVisibility(Wanted.IsEmpty() || Name.Contains(Wanted, ESearchCase::IgnoreCase) ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		}
	}
}

#undef LOCTEXT_NAMESPACE
