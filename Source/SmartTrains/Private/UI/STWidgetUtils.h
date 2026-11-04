#pragma once

#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/SlateColor.h"
#include "Templates/SubclassOf.h"

class UButton;
class UFGItemDescriptor;
class UUserWidget;
class UWidget;
class UWidgetTree;

// Helpers for building UI from the game's own Blueprint widgets, which C++ can only reach through reflection.
namespace STWidgetUtils
{
	// Class paths of the vanilla widgets the mod uses or hooks.
	extern const TCHAR* StandardButtonClass;
	extern const TCHAR* MenuLabelClass;
	extern const TCHAR* ContentBackgroundClass;
	extern const TCHAR* InputBoxClass;
	extern const TCHAR* FreightPlatformMenuClass;
	extern const TCHAR* FluidPlatformMenuClass;
	extern const TCHAR* LocomotiveMenuClass;
	extern const TCHAR* StationMenuClass;

	// Loads a widget class by path; logs and returns nullptr if it's missing.
	UClass* LoadWidgetClass(const TCHAR* Path);

	// Creates a vanilla widget of the class at Path in Tree, or nullptr if the class is missing.
	UUserWidget* CreateVanillaWidget(UWidgetTree* Tree, const TCHAR* Path);

	// Calls the Blueprint function FunctionName(FText) on Widget; returns false if it has none.
	bool CallTextFunction(UUserWidget* Widget, FName FunctionName, const FText& Text);

	// Sets a vanilla button's or label's text (SetText, else its mTextObject); returns false if neither exists.
	bool SetWidgetText(UUserWidget* Widget, const FText& Text);

	// Returns the UButton inside a vanilla standard button (its mButton), or nullptr.
	UButton* GetInnerButton(UUserWidget* StandardButton);

	// Returns the object stored in Owner's object property PropertyName, or nullptr.
	UObject* GetObjectProperty(const UObject* Owner, FName PropertyName);

	// Returns the nearest user widget that owns Widget, skipping Widget itself.
	UUserWidget* GetOwningUserWidget(const UWidget* Widget);

	// Font and colour of the TextBlock named SourceName in Menu's tree (or its first TextBlock), at Size.
	// Returns false and leaves the outputs alone if Menu has no TextBlock.
	bool FindTextStyle(UUserWidget* Menu, FName SourceName, int32 Size, FSlateFontInfo& OutFont, FSlateColor& OutColor);

	// Items a platform can be set to, for the item picker: those the players can make or mine, of the platform's form
	// (solids, or liquids and gases), sorted by name. Falls back to every item if no recipes are known.
	TArray<TSubclassOf<UFGItemDescriptor>> GetPickableItems(const UObject* WorldContext, bool bFluid);
}
