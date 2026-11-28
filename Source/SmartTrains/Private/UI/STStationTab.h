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

	// Sets each page's position in the sliding tabs from the widths the pages before it have when shown (after the new
	// page has its size), and logs them.
	void UpdateOffsets(const FParts& Parts);

	// Logs every widget from Root down to the pages' own children: visibility, opacity, render transform, clipping,
	// desired size and laid-out size and position (which part of the menu doesn't draw). When says what happened.
	void LogRender(const UWidget* Root, const FParts& Parts, const TCHAR* When);

	// Logs what the sliding tabs slide by: the active page, each page's size and visibility, the offsets, the strip's
	// position and width. When says what happened.
	void LogState(const FParts& Parts, const TCHAR* When);
}
