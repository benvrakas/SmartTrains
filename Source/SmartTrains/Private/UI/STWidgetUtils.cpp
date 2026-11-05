#include "UI/STWidgetUtils.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "FGRecipe.h"
#include "FGRecipeManager.h"
#include "Resources/FGAnyUndefinedDescriptor.h"
#include "Resources/FGBuildDescriptor.h"
#include "Resources/FGItemDescriptor.h"
#include "Resources/FGNoneDescriptor.h"
#include "Resources/FGOverflowDescriptor.h"
#include "Resources/FGResourceDescriptor.h"
#include "Resources/FGVisualizationModeDescriptor.h"
#include "Resources/FGWildCardDescriptor.h"
#include "SmartTrains.h"
#include "STTypes.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace STWidgetUtils
{
	const TCHAR* StandardButtonClass = TEXT("/Game/FactoryGame/Interface/UI/InGame/-Shared/Widget_StandardButton.Widget_StandardButton_C");
	const TCHAR* MenuLabelClass = TEXT("/Game/FactoryGame/Buildable/Vehicle/Train/Locomotive/UI/BPW_TrainMenuLabel.BPW_TrainMenuLabel_C");
	const TCHAR* ContentBackgroundClass = TEXT("/Game/FactoryGame/Interface/UI/InGame/Widget_Window_ContentBackground_DarkMode.Widget_Window_ContentBackground_DarkMode_C");
	const TCHAR* InputBoxClass = TEXT("/Game/FactoryGame/Interface/UI/Menu/Widget_InputBox.Widget_InputBox_C");
	const TCHAR* FreightPlatformMenuClass = TEXT("/Game/FactoryGame/Buildable/-Shared/Widgets/Widget_Train_DockingStation.Widget_Train_DockingStation_C");
	const TCHAR* FluidPlatformMenuClass = TEXT("/Game/FactoryGame/Buildable/-Shared/Widgets/BPW_Train_DockingStation_Fluid.BPW_Train_DockingStation_Fluid_C");
	const TCHAR* LocomotiveMenuClass = TEXT("/Game/FactoryGame/Buildable/Vehicle/Train/Locomotive/UI/Widget_Locomotive_Menu.Widget_Locomotive_Menu_C");
	const TCHAR* StationMenuClass = TEXT("/Game/FactoryGame/Buildable/Factory/Train/Station/Widget_TrainStationNew.Widget_TrainStationNew_C");
}

UClass* STWidgetUtils::LoadWidgetClass(const TCHAR* Path)
{
	static TMap<FString, TWeakObjectPtr<UClass>> Cache;
	if (const TWeakObjectPtr<UClass>* Found = Cache.Find(Path))
	{
		if (Found->IsValid())
		{
			return Found->Get();
		}
	}
	UClass* Class = LoadClass<UUserWidget>(nullptr, Path);
	if (!Class)
	{
		UE_LOG(LogSmartTrains, Warning, TEXT("Widget class %s not found"), Path);
	}
	Cache.Add(Path, Class);
	return Class;
}

UUserWidget* STWidgetUtils::CreateVanillaWidget(UWidgetTree* Tree, const TCHAR* Path)
{
	UClass* Class = Tree ? LoadWidgetClass(Path) : nullptr;
	return Class ? Tree->ConstructWidget<UUserWidget>(Class) : nullptr;
}

bool STWidgetUtils::CallTextFunction(UUserWidget* Widget, FName FunctionName, const FText& Text)
{
	UFunction* Function = Widget ? Widget->FindFunction(FunctionName) : nullptr;
	if (!Function)
	{
		return false;
	}
	FTextProperty* TextParam = nullptr;
	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		if (FTextProperty* AsText = CastField<FTextProperty>(*It))
		{
			TextParam = AsText;
			break;
		}
	}
	if (!TextParam)
	{
		return false;
	}
	uint8* Params = static_cast<uint8*>(FMemory_Alloca(FMath::Max<int32>(Function->ParmsSize, 1)));
	FMemory::Memzero(Params, Function->ParmsSize);
	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		It->InitializeValue_InContainer(Params);
	}
	TextParam->SetPropertyValue_InContainer(Params, Text);
	Widget->ProcessEvent(Function, Params);
	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		It->DestroyValue_InContainer(Params);
	}
	return true;
}

bool STWidgetUtils::SetWidgetText(UUserWidget* Widget, const FText& Text)
{
	if (CallTextFunction(Widget, TEXT("SetText"), Text))
	{
		return true;
	}
	if (UTextBlock* TextObject = Cast<UTextBlock>(GetObjectProperty(Widget, TEXT("mTextObject"))))
	{
		TextObject->SetText(Text);
		return true;
	}
	UE_LOG(LogSmartTrains, Warning, TEXT("%s has no SetText; text \"%s\" not shown"), *GetNameSafe(Widget), *Text.ToString());
	return false;
}

UButton* STWidgetUtils::GetInnerButton(UUserWidget* StandardButton)
{
	return Cast<UButton>(GetObjectProperty(StandardButton, TEXT("mButton")));
}

UObject* STWidgetUtils::GetObjectProperty(const UObject* Owner, FName PropertyName)
{
	FObjectPropertyBase* Property = Owner ? FindFProperty<FObjectPropertyBase>(Owner->GetClass(), PropertyName) : nullptr;
	return Property ? Property->GetObjectPropertyValue_InContainer(Owner) : nullptr;
}

UUserWidget* STWidgetUtils::GetOwningUserWidget(const UWidget* Widget)
{
	for (UObject* Outer = Widget ? Widget->GetOuter() : nullptr; Outer; Outer = Outer->GetOuter())
	{
		if (UUserWidget* AsUser = Cast<UUserWidget>(Outer))
		{
			return AsUser;
		}
	}
	return nullptr;
}

bool STWidgetUtils::FindTextStyle(UUserWidget* Menu, FName SourceName, int32 Size, FSlateFontInfo& OutFont, FSlateColor& OutColor)
{
	if (!Menu || !Menu->WidgetTree)
	{
		return false;
	}
	UTextBlock* Source = SourceName.IsNone() ? nullptr : Cast<UTextBlock>(Menu->WidgetTree->FindWidget(SourceName));
	if (!Source)
	{
		Menu->WidgetTree->ForEachWidget([&](UWidget* Child)
		{
			Source = Source ? Source : Cast<UTextBlock>(Child);
		});
	}
	if (!Source)
	{
		return false;
	}
	OutFont = Source->GetFont();
	OutFont.Size = Size;
	OutColor = Source->GetColorAndOpacity();
	return true;
}

TArray<TSubclassOf<UFGItemDescriptor>> STWidgetUtils::GetPickableItems(const UObject* WorldContext, bool bFluid)
{
	auto IsCargo = [bFluid](UClass* Class)
	{
		if (!Class || !Class->IsChildOf(UFGItemDescriptor::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists)
			|| Class->IsChildOf(UFGBuildDescriptor::StaticClass()) || Class->IsChildOf(UFGNoneDescriptor::StaticClass())
			|| Class->IsChildOf(UFGAnyUndefinedDescriptor::StaticClass()) || Class->IsChildOf(UFGOverflowDescriptor::StaticClass())
			|| Class->IsChildOf(UFGWildCardDescriptor::StaticClass()) || Class->IsChildOf(UFGVisualizationModeDescriptor::StaticClass())
			|| Class->GetName().StartsWith(TEXT("SKEL_")) || Class->GetName().StartsWith(TEXT("REINST_")))
		{
			return false;
		}
		return STNames::IsFluid(Class) == bFluid && !UFGItemDescriptor::GetItemName(Class).IsEmpty();
	};

	TSet<UClass*> Found;
	AFGRecipeManager* Recipes = WorldContext ? AFGRecipeManager::Get(const_cast<UObject*>(WorldContext)) : nullptr;
	if (Recipes)
	{
		TArray<TSubclassOf<UFGRecipe>> Available;
		Recipes->GetAllAvailableRecipes(Available);
		for (const TSubclassOf<UFGRecipe>& Recipe : Available)
		{
			for (const FItemAmount& Product : UFGRecipe::GetProducts(Recipe))
			{
				if (IsCargo(Product.ItemClass))
				{
					Found.Add(Product.ItemClass);
				}
			}
		}
	}
	const bool bFromRecipes = Found.Num() > 0;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		// Raw resources aren't made by recipes; without recipes every item is offered.
		if ((!bFromRecipes || It->IsChildOf(UFGResourceDescriptor::StaticClass())) && IsCargo(*It))
		{
			Found.Add(*It);
		}
	}
	TArray<TSubclassOf<UFGItemDescriptor>> Items;
	for (UClass* Class : Found)
	{
		Items.Add(Class);
	}
	Items.Sort([](const TSubclassOf<UFGItemDescriptor>& A, const TSubclassOf<UFGItemDescriptor>& B)
	{
		return UFGItemDescriptor::GetItemName(A).ToString() < UFGItemDescriptor::GetItemName(B).ToString();
	});
	UE_LOG(LogSmartTrains, Log, TEXT("Item picker: %d %s items (%s)"), Items.Num(), bFluid ? TEXT("fluid") : TEXT("solid"),
		bFromRecipes ? TEXT("made by available recipes, plus resources") : TEXT("no recipes known: all items"));
	return Items;
}

AFGBuildableTrainPlatformCargo* STWidgetUtils::GetMenuPlatform(const UUserWidget* Menu)
{
	if (AFGBuildableTrainPlatformCargo* Platform = Cast<AFGBuildableTrainPlatformCargo>(GetObjectProperty(Menu, TEXT("mTrainPlatformCargo"))))
	{
		return Platform;
	}
	return Cast<AFGBuildableTrainPlatformCargo>(GetObjectProperty(Menu, TEXT("mInteractObject")));
}
