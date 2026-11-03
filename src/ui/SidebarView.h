// The sidebar: natural-language entry, mini month, and either the upcoming
// list (events and tasks by day, or search results) or the calendar list.
#pragma once

#include <View.h>

#include "core/DateTime.h"

#include <string>

class HintField;
class BStringView;
class BScrollView;
class MiniMonthView;
class UpcomingView;
class CalendarListView;
class SidebarTabs;
class StatusLine;

class SidebarView : public BView {
public:
	SidebarView();

	void Reload();
	void SetSelectedDate(const aircal::Date& date);
	void SetVisibleRange(const aircal::Date& first, const aircal::Date& last);
	void SetSearch(const std::string& text);
	void SetStatus(const std::string& text, bool error);
	void ShowCalendars(bool calendars);
	void FocusQuickEntry();

	void AttachedToWindow() override;
	void Draw(BRect update) override;
	void MessageReceived(BMessage* message) override;

private:
	void UpdatePreview();

	HintField* fEntry = nullptr;
	BStringView* fPreview = nullptr;
	MiniMonthView* fMiniMonth = nullptr;
	SidebarTabs* fTabs = nullptr;
	UpcomingView* fUpcoming = nullptr;
	CalendarListView* fCalendars = nullptr;
	BScrollView* fUpcomingScroll = nullptr;
	BScrollView* fCalendarsScroll = nullptr;
	StatusLine* fStatus = nullptr;
};
