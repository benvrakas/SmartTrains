#include "UI/STStationTab.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelWidget.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WidgetSwitcher.h"
#include "SmartTrains.h"
#include "UI/STWidgetUtils.h"

namespace
{
	const FName TabsProperty(TEXT("mTabs"));
	const FName ContentSlot(TEXT("mContent"));

	bool bTreeLogged = false;

	FString Describe(const UWidget* Widget)
	{
		return Widget ? FString::Printf(TEXT("%s (%s)"), *Widget->GetName(), *Widget->GetClass()->GetName()) : FString(TEXT("none"));
	}

	// Logs Root and everything under it, one line per widget, with user widgets' trees and named slots.
	void LogTree(const UWidget* Root, int32 Depth)
	{
		if (!Root || Depth > 40)
		{
			return;
		}
		UE_LOG(LogSmartTrains, Log, TEXT("UI tree %s%s%s"), *FString::ChrN(Depth * 2, TEXT(' ')), *Describe(Root),
			Root->Slot ? *FString::Printf(TEXT(" slot %s"), *Root->Slot->GetClass()->GetName()) : TEXT(""));
		if (const UPanelWidget* Panel = Cast<UPanelWidget>(Root))
		{
			for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
			{
				LogTree(Panel->GetChildAt(Index), Depth + 1);
			}
		}
		else if (const UUserWidget* User = Cast<UUserWidget>(Root))
		{
			if (User->WidgetTree)
			{
				LogTree(User->WidgetTree->RootWidget, Depth + 1);
			}
		}
	}

	// Calls FunctionName on Widget with its first parameter of the matching kind set (an int or an object). Returns
	// false if Widget has no such function.
	bool CallFunction(UUserWidget* Widget, FName FunctionName, TOptional<int32> Int, UObject* Object)
	{
		UFunction* Function = Widget ? Widget->FindFunction(FunctionName) : nullptr;
		if (!Function)
		{
			return false;
		}
		uint8* Params = static_cast<uint8*>(FMemory_Alloca(FMath::Max<int32>(Function->ParmsSize, 1)));
		FMemory::Memzero(Params, FMath::Max<int32>(Function->ParmsSize, 1));
		bool bSet = false;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->InitializeValue_InContainer(Params);
			if (bSet || It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm))
			{
				continue;
			}
			if (FIntProperty* AsInt = CastField<FIntProperty>(*It); AsInt && Int.IsSet())
			{
				AsInt->SetPropertyValue_InContainer(Params, Int.GetValue());
				bSet = true;
			}
			else if (FObjectPropertyBase* AsObject = CastField<FObjectPropertyBase>(*It); AsObject && Object && Object->IsA(AsObject->PropertyClass))
			{
				AsObject->SetObjectPropertyValue_InContainer(Params, Object);
				bSet = true;
			}
		}
		if (bSet)
		{
			Widget->ProcessEvent(Function, Params);
		}
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->DestroyValue_InContainer(Params);
		}
		return bSet;
	}

	bool SetObjectProperty(UObject* Owner, FName Name, UObject* Value)
	{
		FObjectPropertyBase* Property = Owner ? FindFProperty<FObjectPropertyBase>(Owner->GetClass(), Name) : nullptr;
		if (!Property || (Value && !Value->IsA(Property->PropertyClass)))
		{
			return false;
		}
		Property->SetObjectPropertyValue_InContainer(Owner, Value);
		return true;
	}

	void CopySlot(const UPanelSlot* From, UPanelSlot* To)
	{
		if (const UHorizontalBoxSlot* A = Cast<UHorizontalBoxSlot>(From))
		{
			if (UHorizontalBoxSlot* B = Cast<UHorizontalBoxSlot>(To))
			{
				B->SetSize(A->GetSize());
				B->SetPadding(A->GetPadding());
				B->SetHorizontalAlignment(A->GetHorizontalAlignment());
				B->SetVerticalAlignment(A->GetVerticalAlignment());
			}
		}
		else if (const UVerticalBoxSlot* C = Cast<UVerticalBoxSlot>(From))
		{
			if (UVerticalBoxSlot* D = Cast<UVerticalBoxSlot>(To))
			{
				D->SetSize(C->GetSize());
				D->SetPadding(C->GetPadding());
				D->SetHorizontalAlignment(C->GetHorizontalAlignment());
				D->SetVerticalAlignment(C->GetVerticalAlignment());
			}
		}
		else if (const UOverlaySlot* E = Cast<UOverlaySlot>(From))
		{
			if (UOverlaySlot* F = Cast<UOverlaySlot>(To))
			{
				F->SetPadding(E->GetPadding());
				F->SetHorizontalAlignment(E->GetHorizontalAlignment());
				F->SetVerticalAlignment(E->GetVerticalAlignment());
			}
		}
		else if (const UCanvasPanelSlot* G = Cast<UCanvasPanelSlot>(From))
		{
			if (UCanvasPanelSlot* H = Cast<UCanvasPanelSlot>(To))
			{
				H->SetLayout(G->GetLayout());
				H->SetAutoSize(G->GetAutoSize());
				H->SetZOrder(G->GetZOrder());
			}
		}
	}

	void CollectButtons(const UUserWidget* Owner, TArray<UUserWidget*>& OutButtons)
	{
		if (!Owner || !Owner->WidgetTree)
		{
			return;
		}
		Owner->WidgetTree->ForEachWidget([&OutButtons](UWidget* Widget)
		{
			UUserWidget* User = Cast<UUserWidget>(Widget);
			if (User && User->GetClass()->GetName().Contains(TEXT("SlidingTabs_Button")))
			{
				OutButtons.AddUnique(User);
			}
		});
	}
}

bool STStationTab::FindParts(UUserWidget* Menu, UWidget* Anchor, FParts& OutParts)
{
	OutParts = FParts();
	OutParts.Tabs = Cast<UUserWidget>(STWidgetUtils::GetObjectProperty(Menu, TabsProperty));
	UWidget* Content = OutParts.Tabs ? OutParts.Tabs->GetContentForSlot(ContentSlot) : nullptr;
	// The pages: the outermost panel with several children on the way from Anchor up to the tabs' content.
	TArray<UWidget*> Chain;
	for (UWidget* At = Anchor; At && Chain.Num() < 64; At = At->GetParent())
	{
		Chain.Add(At);
		if (At == Content)
		{
			break;
		}
	}
	const bool bReached = Content && Chain.Num() > 0 && Chain.Last() == Content;
	if (bReached)
	{
		for (UWidget* Each : Chain)
		{
			UPanelWidget* Panel = Cast<UPanelWidget>(Each);
			if (Panel && Panel->GetChildrenCount() >= 2 && Each != Anchor)
			{
				OutParts.Pages = Panel;
			}
		}
	}
	TArray<UUserWidget*> Buttons;
	CollectButtons(Menu, Buttons);
	CollectButtons(OutParts.Tabs, Buttons);
	OutParts.TemplateButton = Buttons.Num() > 0 ? Buttons.Last() : nullptr;
	const bool bFound = OutParts.Tabs && OutParts.Pages && OutParts.TemplateButton;
	UE_LOG(LogSmartTrains, Log, TEXT("Station tab: tabs %s, content %s (%s from the set-up bar), pages %s with %d, %d tab buttons (template %s, in %s)"),
		*Describe(OutParts.Tabs), *Describe(Content), bReached ? TEXT("reached") : TEXT("not reached"), *Describe(OutParts.Pages),
		OutParts.Pages ? OutParts.Pages->GetChildrenCount() : 0, Buttons.Num(), *Describe(OutParts.TemplateButton),
		*Describe(OutParts.TemplateButton ? OutParts.TemplateButton->GetParent() : nullptr));
	if (!bFound || !bTreeLogged)
	{
		bTreeLogged = true;
		LogTree(Menu ? Menu->WidgetTree->RootWidget : nullptr, 0);
		for (const UUserWidget* Button : Buttons)
		{
			FString Values;
			for (TFieldIterator<FProperty> It(Button->GetClass()); It; ++It)
			{
				if (It->GetOwnerClass() == Button->GetClass() && (CastField<FIntProperty>(*It) || CastField<FTextProperty>(*It) || CastField<FBoolProperty>(*It)))
				{
					FString Value;
					It->ExportTextItem_InContainer(Value, Button, nullptr, nullptr, PPF_None);
					Values += FString::Printf(TEXT(" %s=%s"), *It->GetName(), *Value);
				}
			}
			UE_LOG(LogSmartTrains, Log, TEXT("Station tab: button %s in %s:%s"), *Describe(Button), *Describe(Button->GetParent()), *Values);
		}
	}
	if (!bFound)
	{
		UE_LOG(LogSmartTrains, Warning, TEXT("Station tab: the station menu isn't laid out as expected; the SmartTrains section goes on the Train Station tab instead"));
	}
	return bFound;
}

int32 STStationTab::AddPage(const FParts& Parts, UWidget* Page)
{
	const int32 Index = Parts.Pages->GetChildrenCount();
	UPanelSlot* PageSlot = Parts.Pages->AddChild(Page);
	CopySlot(Parts.Pages->GetChildAt(0) ? Parts.Pages->GetChildAt(0)->Slot : nullptr, PageSlot);
	UE_LOG(LogSmartTrains, Log, TEXT("Station tab: page %d added to %s"), Index, *Describe(Parts.Pages));
	return Index;
}

UUserWidget* STStationTab::AddButton(const FParts& Parts, const FText& Title)
{
	UPanelWidget* Parent = Parts.TemplateButton->GetParent();
	UUserWidget* Button = Parent ? CreateWidget<UUserWidget>(Parts.TemplateButton->GetOwningPlayer(), Parts.TemplateButton->GetClass()) : nullptr;
	if (!Button)
	{
		UE_LOG(LogSmartTrains, Warning, TEXT("Station tab: no tab button made (template %s)"), *Describe(Parts.TemplateButton));
		return nullptr;
	}
	const bool bLinked = SetObjectProperty(Button, TEXT("mSlidingTabsWidget"), Parts.Tabs);
	UPanelSlot* ButtonSlot = Parent->AddChild(Button);
	CopySlot(Parts.TemplateButton->Slot, ButtonSlot);
	const bool bTitled = STWidgetUtils::CallTextFunction(Button, TEXT("SetTitle"), Title);
	// The first tab's icon (the station), as the button has no SmartTrains one.
	TArray<UUserWidget*> Buttons;
	CollectButtons(STWidgetUtils::GetOwningUserWidget(Parts.TemplateButton), Buttons);
	UObject* Icon = Buttons.Num() > 0 ? STWidgetUtils::GetObjectProperty(Buttons[0], TEXT("mIcon")) : nullptr;
	const bool bIcon = Icon && CallFunction(Button, TEXT("SetIcon"), TOptional<int32>(), Icon);
	UE_LOG(LogSmartTrains, Log, TEXT("Station tab: button added to %s (linked %d, titled %d, icon %d)"), *Describe(Parent), bLinked, bTitled, bIcon);
	return Button;
}

void STStationTab::ShowPage(const FParts& Parts, int32 Index, UUserWidget* Button)
{
	if (UWidgetSwitcher* Switcher = Cast<UWidgetSwitcher>(Parts.Pages))
	{
		Switcher->SetActiveWidgetIndex(Index);
		UE_LOG(LogSmartTrains, Log, TEXT("Station tab: switcher shows page %d"), Index);
		return;
	}
	const bool bButton = CallFunction(Parts.Tabs, TEXT("SetActiveButton"), TOptional<int32>(), Button);
	const bool bIndex = CallFunction(Parts.Tabs, TEXT("SetActiveIndex"), Index, nullptr);
	UE_LOG(LogSmartTrains, Log, TEXT("Station tab: page %d shown (SetActiveButton %d, SetActiveIndex %d)"), Index, bButton, bIndex);
}
