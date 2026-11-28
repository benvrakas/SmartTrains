#pragma once

#include "CoreMinimal.h"

class UPanelWidget;
class UUserWidget;
class UWidget;

// The SmartTrains tab of the vanilla station menu (Widget_TrainStationNew), after Train Station and Timetable
// (Docs/Architecture.md, "UI"). The menu's window (mTrainStationWindow, a Widget_Window_DarkMode) lists its tabs in
// mTabs (icon and title each) and its Widget_TabsContainer makes one button per entry (InitTabs); a button shows page
// N of the menu's Widget_SlidingTabs, whose mContent slot holds a HorizontalBox with one page per tab. C++ reaches
// them by reflection only.
namespace STStationTab
{
	struct FParts
	{
		UUserWidget* Window = nullptr;
		UUserWidget* Tabs = nullptr;
		UPanelWidget* Pages = nullptr;
	};

	// Finds the window, the sliding tabs and their pages of Menu; Anchor is a widget on the first tab's page. Returns
	// false and logs what is missing (with the menu's widget tree, once per game) if the menu isn't as expected.
	bool FindParts(UUserWidget* Menu, UWidget* Anchor, FParts& OutParts);

	// True when the window lists a tab titled Title.
	bool HasTab(const FParts& Parts, const FText& Title);

	// Adds Page as the last page (its slot set up like the first page's) and a tab titled Title with the first tab's
	// icon, then has the window remake its tab buttons. Returns false (logged) if the window's tab list can't be changed.
	bool AddTab(const FParts& Parts, UWidget* Page, const FText& Title);

	// Rebuilds the row of pages as Slate draws it in the pages' order when the two differ, and logs it (When says what
	// happened). Moving a child in UMG (InsertChildAt, ShiftChild) reorders UMG's list only, so the drawn row can hold
	// the pages in another order or miss some, and the window then shows the wrong page. Returns true if rebuilt.
	bool SyncRow(const FParts& Parts, const TCHAR* When);

	// Has the sliding tabs work out each page's position again (after the new page has its size), with the drawn row
	// rebuilt first if needed, and logs the pages' widths and positions.
	void UpdateOffsets(const FParts& Parts);
}
