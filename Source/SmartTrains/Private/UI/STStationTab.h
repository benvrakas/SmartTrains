#pragma once

#include "CoreMinimal.h"

class UPanelWidget;
class UUserWidget;
class UWidget;

// The SmartTrains tab of the vanilla station menu (Widget_TrainStationNew), next to Train Station and Timetable
// (Docs/Architecture.md, "UI"). The menu's tabs are a Widget_SlidingTabs (its mTabs) whose mContent slot holds one page
// per tab; the tab buttons are Widget_SlidingTabs_Button. C++ reaches them by reflection only.
namespace STStationTab
{
	struct FParts
	{
		// The menu's Widget_SlidingTabs.
		UUserWidget* Tabs = nullptr;
		// The panel whose children are the pages, one per tab.
		UPanelWidget* Pages = nullptr;
		// The last vanilla tab button, copied for the new tab.
		UUserWidget* TemplateButton = nullptr;
	};

	// Finds the tabs, their pages and a tab button of Menu, starting from Anchor, a widget on the first tab's page.
	// Returns false and logs what is missing (with the menu's widget tree, once per game) if the menu isn't as
	// expected.
	bool FindParts(UUserWidget* Menu, UWidget* Anchor, FParts& OutParts);

	// Adds Page as the last page, with its slot set up like the first page's. Returns its index.
	int32 AddPage(const FParts& Parts, UWidget* Page);

	// Adds a copy of the template tab button after it, titled Title with the first tab's icon. Returns it or nullptr.
	UUserWidget* AddButton(const FParts& Parts, const FText& Title);

	// Shows page Index: the tabs' own SetActiveButton (Button) and SetActiveIndex, or the switcher's index.
	void ShowPage(const FParts& Parts, int32 Index, UUserWidget* Button);
}
