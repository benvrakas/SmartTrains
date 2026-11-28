#include "UI/STStationTab.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelWidget.h"
#include "Components/VerticalBoxSlot.h"
#include "SmartTrains.h"
#include "UI/STWidgetUtils.h"
#include "Widgets/SBoxPanel.h"

namespace
{
	const FName WindowProperty(TEXT("mTrainStationWindow"));
	const FName TabsProperty(TEXT("Widget_SlidingTabs"));
	const FName ContentSlot(TEXT("mContent"));
	const FName TabListProperty(TEXT("mTabs"));
	const FName TabsContainerProperty(TEXT("Widget_TabsContainer"));
	const FName ButtonSizeProperty(TEXT("mButtonSize"));
	const FName IndexOffsetProperty(TEXT("mIndexOffset"));

	bool bTreeLogged = false;

	FString Describe(const UWidget* Widget)
	{
		return Widget ? FString::Printf(TEXT("%s (%s)"), *Widget->GetName(), *Widget->GetClass()->GetName()) : FString(TEXT("none"));
	}

	// Logs Root and everything under it, one line per widget, with user widgets' trees.
	void LogTree(const UWidget* Root, int32 Depth)
	{
		if (!Root || Depth > 40)
		{
			return;
		}
		UE_LOG(LogSmartTrains, Log, TEXT("UI tree %s%s"), *FString::ChrN(Depth * 2, TEXT(' ')), *Describe(Root));
		if (const UPanelWidget* Panel = Cast<UPanelWidget>(Root))
		{
			for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
			{
				LogTree(Panel->GetChildAt(Index), Depth + 1);
			}
		}
		else if (const UUserWidget* User = Cast<UUserWidget>(Root); User && User->WidgetTree)
		{
			LogTree(User->WidgetTree->RootWidget, Depth + 1);
		}
	}

	// Calls a Blueprint function without parameters; false if Widget has none of that name.
	bool CallNoParams(UUserWidget* Widget, FName FunctionName)
	{
		UFunction* Function = Widget ? Widget->FindFunction(FunctionName) : nullptr;
		if (!Function || Function->ParmsSize > 0)
		{
			return false;
		}
		Widget->ProcessEvent(Function, nullptr);
		return true;
	}

	// The window's tab list (an array of FImageAndText: a texture and a text) and its entries' two fields.
	struct FTabList
	{
		FArrayProperty* Array = nullptr;
		FStructProperty* Entry = nullptr;
		FObjectPropertyBase* Texture = nullptr;
		FTextProperty* Text = nullptr;
	};

	FTabList GetTabList(const UUserWidget* Window)
	{
		FTabList List;
		List.Array = Window ? FindFProperty<FArrayProperty>(Window->GetClass(), TabListProperty) : nullptr;
		List.Entry = List.Array ? CastField<FStructProperty>(List.Array->Inner) : nullptr;
		for (TFieldIterator<FProperty> It(List.Entry ? List.Entry->Struct : nullptr); It; ++It)
		{
			if (FObjectPropertyBase* AsObject = CastField<FObjectPropertyBase>(*It); AsObject && It->GetName().StartsWith(TEXT("Texture")))
			{
				List.Texture = AsObject;
			}
			else if (FTextProperty* AsText = CastField<FTextProperty>(*It); AsText && It->GetName().StartsWith(TEXT("Text")))
			{
				List.Text = AsText;
			}
		}
		return List;
	}

	// The window's tab buttons are a fixed width each (the tabs container's mButtonSize): narrowed so After tabs take the
	// width Before tabs had.
	void FitButtons(const UUserWidget* Window, int32 Before, int32 After)
	{
		UUserWidget* Container = Cast<UUserWidget>(STWidgetUtils::GetObjectProperty(Window, TabsContainerProperty));
		const FStructProperty* Size = Container ? FindFProperty<FStructProperty>(Container->GetClass(), ButtonSizeProperty) : nullptr;
		if (!Size || Size->Struct != TBaseStructure<FVector2D>::Get() || Before <= 0 || After <= Before)
		{
			UE_LOG(LogSmartTrains, Warning, TEXT("Station tab: the tab buttons' size wasn't found (%s); the last tab may be cut off"), *Describe(Container));
			return;
		}
		FVector2D* Value = Size->ContainerPtrToValuePtr<FVector2D>(Container);
		const double Old = Value->X;
		Value->X = Old * Before / After;
		UE_LOG(LogSmartTrains, Log, TEXT("Station tab: tab buttons %.0f wide instead of %.0f"), Value->X, Old);
	}

	void CopySlot(const UPanelSlot* From, UPanelSlot* To)
	{
		const UHorizontalBoxSlot* A = Cast<UHorizontalBoxSlot>(From);
		UHorizontalBoxSlot* B = Cast<UHorizontalBoxSlot>(To);
		if (A && B)
		{
			B->SetSize(A->GetSize());
			B->SetPadding(A->GetPadding());
			B->SetHorizontalAlignment(A->GetHorizontalAlignment());
			B->SetVerticalAlignment(A->GetVerticalAlignment());
		}
	}
}

bool STStationTab::FindParts(UUserWidget* Menu, UWidget* Anchor, FParts& OutParts)
{
	OutParts = FParts();
	OutParts.Window = Cast<UUserWidget>(STWidgetUtils::GetObjectProperty(Menu, WindowProperty));
	OutParts.Tabs = Cast<UUserWidget>(STWidgetUtils::GetObjectProperty(Menu, TabsProperty));
	OutParts.Pages = OutParts.Tabs ? Cast<UPanelWidget>(OutParts.Tabs->GetContentForSlot(ContentSlot)) : nullptr;
	// The set-up bar must be on one of those pages.
	bool bOnPage = false;
	for (const UWidget* At = Anchor; At && !bOnPage; At = At->GetParent())
	{
		bOnPage = At->GetParent() == OutParts.Pages;
	}
	const FTabList List = GetTabList(OutParts.Window);
	const bool bFound = OutParts.Window && OutParts.Pages && bOnPage && List.Texture && List.Text;
	UE_LOG(LogSmartTrains, Log, TEXT("Station tab: window %s, tabs %s, pages %s with %d, set-up bar on a page %d, tab list %s"),
		*Describe(OutParts.Window), *Describe(OutParts.Tabs), *Describe(OutParts.Pages), OutParts.Pages ? OutParts.Pages->GetChildrenCount() : 0, bOnPage,
		List.Texture && List.Text ? TEXT("found") : TEXT("missing"));
	if (!bFound && !bTreeLogged)
	{
		bTreeLogged = true;
		UE_LOG(LogSmartTrains, Warning, TEXT("Station tab: the station menu isn't laid out as expected; the SmartTrains section goes on the Train Station tab instead. Its widget tree:"));
		LogTree(Menu && Menu->WidgetTree ? Menu->WidgetTree->RootWidget : nullptr, 0);
	}
	return bFound;
}

bool STStationTab::HasTab(const FParts& Parts, const FText& Title)
{
	const FTabList List = GetTabList(Parts.Window);
	if (!List.Array || !List.Text)
	{
		return false;
	}
	FScriptArrayHelper Helper(List.Array, List.Array->ContainerPtrToValuePtr<void>(Parts.Window));
	for (int32 Index = 0; Index < Helper.Num(); ++Index)
	{
		if (List.Text->GetPropertyValue_InContainer(Helper.GetRawPtr(Index)).EqualTo(Title))
		{
			return true;
		}
	}
	return false;
}

bool STStationTab::AddTab(const FParts& Parts, UWidget* Page, const FText& Title)
{
	const FTabList List = GetTabList(Parts.Window);
	if (!List.Array || !List.Texture || !List.Text)
	{
		return false;
	}
	FScriptArrayHelper Helper(List.Array, List.Array->ContainerPtrToValuePtr<void>(Parts.Window));
	const int32 Pages = Parts.Pages->GetChildrenCount();
	if (Helper.Num() != Pages)
	{
		UE_LOG(LogSmartTrains, Warning, TEXT("Station tab: the window lists %d tabs for %d pages; not added"), Helper.Num(), Pages);
		return false;
	}
	UPanelSlot* PageSlot = Parts.Pages->AddChild(Page);
	CopySlot(Parts.Pages->GetChildAt(0) ? Parts.Pages->GetChildAt(0)->Slot : nullptr, PageSlot);
	// The first tab's icon (the station), as there is no SmartTrains texture.
	UObject* Icon = Helper.Num() > 0 ? List.Texture->GetObjectPropertyValue_InContainer(Helper.GetRawPtr(0)) : nullptr;
	const int32 Added = Helper.AddValue();
	List.Texture->SetObjectPropertyValue_InContainer(Helper.GetRawPtr(Added), Icon);
	List.Text->SetPropertyValue_InContainer(Helper.GetRawPtr(Added), Title);
	FitButtons(Parts.Window, Pages, Helper.Num());
	const bool bInit = CallNoParams(Parts.Window, TEXT("InitTabs"));
	UE_LOG(LogSmartTrains, Log, TEXT("Station tab: page %d added, %d tabs listed, buttons remade %d"), Pages, Helper.Num(), bInit);
	return true;
}

bool STStationTab::SyncRow(const FParts& Parts, const TCHAR* When)
{
	const UHorizontalBox* Box = Cast<UHorizontalBox>(Parts.Pages);
	const TSharedPtr<SWidget> Live = Box ? Box->GetCachedWidget() : nullptr;
	if (!Live.IsValid() || Live->GetType() != FName(TEXT("SHorizontalBox")))
	{
		return false;
	}
	const TSharedRef<SHorizontalBox> Row = StaticCastSharedRef<SHorizontalBox>(Live.ToSharedRef());
	FChildren* Children = Row->GetChildren();
	bool bSame = Children->Num() == Box->GetChildrenCount();
	FString Drawn;
	for (int32 Index = 0; Index < Children->Num(); ++Index)
	{
		const SWidget* Child = &Children->GetChildAt(Index).Get();
		int32 Page = INDEX_NONE;
		for (int32 Each = 0; Each < Box->GetChildrenCount(); ++Each)
		{
			if (Box->GetChildAt(Each) && Box->GetChildAt(Each)->GetCachedWidget().Get() == Child)
			{
				Page = Each;
			}
		}
		bSame = bSame && Page == Index;
		Drawn += FString::Printf(TEXT(" %d"), Page);
	}
	if (bSame)
	{
		return false;
	}
	Row->ClearChildren();
	for (UPanelSlot* Each : Box->GetSlots())
	{
		if (UHorizontalBoxSlot* Slot = Cast<UHorizontalBoxSlot>(Each))
		{
			Slot->BuildSlot(Row);
		}
	}
	UE_LOG(LogSmartTrains, Log, TEXT("Station tab: %s: the drawn row held pages [%s ] of %d; rebuilt in order"), When, *Drawn, Box->GetChildrenCount());
	return true;
}

void STStationTab::UpdateOffsets(const FParts& Parts)
{
	// As the tabs do themselves: every page shown and measured, the offsets worked out, the others hidden again. Measured
	// while hidden, a page counts as 0 wide and the window slides to the wrong place.
	if (!Parts.Tabs || !Parts.Pages)
	{
		return;
	}
	SyncRow(Parts, TEXT("before the offsets"));
	CallNoParams(Parts.Tabs, TEXT("ShowAllIndexes"));
	Parts.Tabs->ForceLayoutPrepass();
	const bool bSet = CallNoParams(Parts.Tabs, TEXT("SetupIndexOffset"));
	CallNoParams(Parts.Tabs, TEXT("HideNonActiveIndexes"));
	// One line per menu: each page's width and where the tabs place it.
	FString Widths;
	for (int32 Index = 0; Index < Parts.Pages->GetChildrenCount(); ++Index)
	{
		const UWidget* Page = Parts.Pages->GetChildAt(Index);
		Widths += FString::Printf(TEXT(" %.0f"), Page ? Page->GetDesiredSize().X : 0.0);
	}
	FString Offsets;
	const FMapProperty* Map = FindFProperty<FMapProperty>(Parts.Tabs->GetClass(), IndexOffsetProperty);
	const FNumericProperty* Value = Map ? CastField<FNumericProperty>(Map->ValueProp) : nullptr;
	if (Value && Value->IsFloatingPoint())
	{
		FScriptMapHelper Helper(Map, Map->ContainerPtrToValuePtr<void>(Parts.Tabs));
		for (FScriptMapHelper::FIterator It(Helper); It; ++It)
		{
			Offsets += FString::Printf(TEXT(" %.0f"), Value->GetFloatingPointPropertyValue(Helper.GetValuePtr(It)));
		}
	}
	UE_LOG(LogSmartTrains, Log, TEXT("Station tab: page offsets worked out again (%d): pages%s wide, at%s"), bSet, *Widths, *Offsets);
}
