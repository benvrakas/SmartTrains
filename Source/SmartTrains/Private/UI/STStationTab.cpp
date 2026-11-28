#include "UI/STStationTab.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/SizeBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelWidget.h"
#include "Components/VerticalBoxSlot.h"
#include "SmartTrains.h"
#include "UI/STWidgetUtils.h"

namespace
{
	const FName WindowProperty(TEXT("mTrainStationWindow"));
	const FName TabsProperty(TEXT("Widget_SlidingTabs"));
	const FName ContentSlot(TEXT("mContent"));
	const FName TabListProperty(TEXT("mTabs"));
	const FName TabsContainerProperty(TEXT("Widget_TabsContainer"));
	const FName ButtonSizeProperty(TEXT("mButtonSize"));
	const FName ButtonCanvasProperty(TEXT("mContainer"));
	const FName ActiveIndexProperty(TEXT("mActiveIndex"));
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

	// Logs the tab buttons as laid out (position and size of each).
	void LogButtons(const UUserWidget* Window)
	{
		const UUserWidget* Container = Cast<UUserWidget>(STWidgetUtils::GetObjectProperty(Window, TabsContainerProperty));
		const UPanelWidget* Buttons = Cast<UPanelWidget>(STWidgetUtils::GetObjectProperty(Container, ButtonCanvasProperty));
		for (int32 Index = 0; Buttons && Index < Buttons->GetChildrenCount(); ++Index)
		{
			const UWidget* Button = Buttons->GetChildAt(Index);
			const UCanvasPanelSlot* Slot = Button ? Cast<UCanvasPanelSlot>(Button->Slot) : nullptr;
			UE_LOG(LogSmartTrains, Log, TEXT("Station tab: button %d at %s size %s"), Index,
				Slot ? *Slot->GetPosition().ToString() : TEXT("-"), Slot ? *Slot->GetSize().ToString() : TEXT("-"));
		}
	}

	// Page's width when shown. The Timetable page keeps its content collapsed until it is opened, so it measures 0 wide
	// then; a canvas page's width is worked out from its children's slots as the canvas does, collapsed or not.
	double ShownWidth(const UWidget* Page)
	{
		if (!Page)
		{
			return 0.0;
		}
		double Width = Page->GetDesiredSize().X;
		if (const UCanvasPanel* Canvas = Cast<UCanvasPanel>(Page); Canvas && Width <= 0.0)
		{
			for (int32 Index = 0; Index < Canvas->GetChildrenCount(); ++Index)
			{
				const UWidget* Child = Canvas->GetChildAt(Index);
				const UCanvasPanelSlot* Slot = Child ? Cast<UCanvasPanelSlot>(Child->Slot) : nullptr;
				if (!Slot)
				{
					continue;
				}
				const FAnchors Anchors = Slot->GetAnchors();
				const FMargin Offsets = Slot->GetOffsets();
				const double Size = Slot->GetAutoSize() ? Child->GetDesiredSize().X : Offsets.Right;
				const bool bDocked = Anchors.Minimum.X == Anchors.Maximum.X && (Anchors.Minimum.X == 0.f || Anchors.Minimum.X == 1.f);
				Width = FMath::Max(Width, Size + (bDocked ? FMath::Abs(Offsets.Left) : 0.0));
			}
		}
		return Width;
	}

	FString ExportProperty(const FProperty* Property, const void* Value)
	{
		FString Text;
		Property->ExportTextItem_Direct(Text, Value, nullptr, nullptr, PPF_None);
		return Text;
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
	LogButtons(Parts.Window);
	return true;
}

void STStationTab::UpdateOffsets(const FParts& Parts)
{
	// As the tabs do themselves: every page shown and measured, the offsets worked out, the others hidden again. Measured
	// while hidden, a page counts as 0 wide and the window slides to the wrong place.
	if (!Parts.Tabs)
	{
		return;
	}
	CallNoParams(Parts.Tabs, TEXT("ShowAllIndexes"));
	Parts.Tabs->ForceLayoutPrepass();
	const bool bSet = CallNoParams(Parts.Tabs, TEXT("SetupIndexOffset"));
	CallNoParams(Parts.Tabs, TEXT("HideNonActiveIndexes"));
	// The tabs take each page's offset from the measured widths before it, and the Timetable page measures 0 wide until
	// it is opened: each page's offset is set again from the widths the pages before it have when shown.
	const FMapProperty* Map = FindFProperty<FMapProperty>(Parts.Tabs->GetClass(), IndexOffsetProperty);
	const FIntProperty* Key = Map ? CastField<FIntProperty>(Map->KeyProp) : nullptr;
	const FNumericProperty* Value = Map ? CastField<FNumericProperty>(Map->ValueProp) : nullptr;
	if (Key && Value && Value->IsFloatingPoint() && Parts.Pages)
	{
		FScriptMapHelper Helper(Map, Map->ContainerPtrToValuePtr<void>(Parts.Tabs));
		double Running = 0.0;
		for (int32 Index = 0; Index < Parts.Pages->GetChildrenCount(); ++Index)
		{
			const int32 At = Helper.FindMapIndexWithKey(&Index);
			if (At != INDEX_NONE)
			{
				Value->SetFloatingPointPropertyValue(Helper.GetValuePtr(At), -Running);
			}
			Running += ShownWidth(Parts.Pages->GetChildAt(Index));
		}
	}
	else
	{
		UE_LOG(LogSmartTrains, Warning, TEXT("Station tab: the tabs' page offsets (%s) aren't a map of index to position; left as the tabs set them"),
			*IndexOffsetProperty.ToString());
	}
	LogState(Parts, bSet ? TEXT("page offsets set") : TEXT("page offsets set (the tabs' own pass not found)"));
}

void STStationTab::LogState(const FParts& Parts, const TCHAR* When)
{
	if (!Parts.Tabs)
	{
		return;
	}
	FString Sizes;
	for (int32 Index = 0; Parts.Pages && Index < Parts.Pages->GetChildrenCount(); ++Index)
	{
		const UWidget* Page = Parts.Pages->GetChildAt(Index);
		Sizes += FString::Printf(TEXT(" [%d %s, shown %.0f wide, %s]"), Index, Page ? *Page->GetDesiredSize().ToString() : TEXT("-"), ShownWidth(Page),
			Page ? *StaticEnum<ESlateVisibility>()->GetNameStringByValue(static_cast<int64>(Page->GetVisibility())) : TEXT("-"));
	}
	FString Strip;
	if (const UWidget* Scale = Cast<UWidget>(STWidgetUtils::GetObjectProperty(Parts.Tabs, TEXT("mContentScaleBox"))))
	{
		if (const UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Scale->Slot))
		{
			Strip += FString::Printf(TEXT(" at %s"), *Slot->GetPosition().ToString());
		}
	}
	if (const USizeBox* Box = Cast<USizeBox>(STWidgetUtils::GetObjectProperty(Parts.Tabs, TEXT("mSizeBox"))))
	{
		Strip += FString::Printf(TEXT(", window %.0f wide"), Box->GetWidthOverride());
	}
	FString Offsets;
	if (const FMapProperty* Map = FindFProperty<FMapProperty>(Parts.Tabs->GetClass(), IndexOffsetProperty))
	{
		FScriptMapHelper Helper(Map, Map->ContainerPtrToValuePtr<void>(Parts.Tabs));
		for (FScriptMapHelper::FIterator It(Helper); It; ++It)
		{
			Offsets += FString::Printf(TEXT(" [%s: %s]"), *ExportProperty(Map->KeyProp, Helper.GetKeyPtr(It)), *ExportProperty(Map->ValueProp, Helper.GetValuePtr(It)));
		}
	}
	const FIntProperty* Active = FindFProperty<FIntProperty>(Parts.Tabs->GetClass(), ActiveIndexProperty);
	UE_LOG(LogSmartTrains, Log, TEXT("Station tab: %s; active %d, strip %s%s, pages%s, offsets%s"), When,
		Active ? Active->GetPropertyValue_InContainer(Parts.Tabs) : -1, *Parts.Tabs->GetDesiredSize().ToString(), *Strip, *Sizes, *Offsets);
}
