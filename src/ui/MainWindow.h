// The main window: sidebar (quick entry, mini month, upcoming list,
// calendars) and the calendar area (header + day/week/month/year/tasks).
#pragma once

#include <Window.h>

#include "core/Editing.h"
#include "core/Store.h"

#include <map>
#include <string>

class BCardLayout;
class BMenuItem;
class BSplitView;
class CalendarView;
class HeaderView;
class SidebarView;

struct Selection {
	enum Kind { None, EventItem, TaskItem };
	Kind kind = None;
	aircal::OccurrenceRef ref;		// events
	std::string taskId;				// tasks

	bool IsEvent(const aircal::DisplayEvent& event) const
	{
		return kind == EventItem && ref.eventId == event.eventId
			&& (event.allDay ? ref.originalDate == event.originalDate
				: ref.originalStart == event.originalStart);
	}
	bool IsTask(const std::string& id) const
		{ return kind == TaskItem && taskId == id; }
};

// The base of the day/week, month, year and tasks views.
class CalendarView : public BView {
public:
	CalendarView(const char* name, uint32 flags);

	virtual void SetAnchor(const aircal::Date& date);
	const aircal::Date& Anchor() const { return fAnchor; }
	// Fetch from the store again and redraw.
	virtual void Reload() = 0;
	// The header title ("November 2026"), split into a bold and a light part.
	virtual void Title(std::string& bold, std::string& light) const = 0;
	// The anchor one page forward/back.
	virtual aircal::Date Step(int direction) const = 0;
	virtual void ScrollToNow() {}
	virtual void ThemeChanged() { Invalidate(); }

protected:
	aircal::Date fAnchor;
};

class MainWindow : public BWindow {
public:
	MainWindow();
	~MainWindow() override;

	void MessageReceived(BMessage* message) override;
	bool QuitRequested() override;
	void MenusBeginning() override;

	void SaveState();

	// Navigation
	void SetViewMode(const std::string& mode);
	const std::string& ViewMode() const { return fMode; }
	void GoTo(const aircal::Date& date);
	const aircal::Date& CurrentDate() const { return fDate; }
	void SelectDate(const aircal::Date& date);	// sidebar selection only

	// Selection
	const Selection& CurrentSelection() const { return fSelection; }
	void SetSelection(const Selection& selection);
	void SelectEvent(const aircal::DisplayEvent& event);
	void SelectTask(const std::string& taskId);

	// Actions (also used by the views)
	void EditEvent(const aircal::DisplayEvent& event, BPoint screenPoint);
	void EditTask(const std::string& taskId, BPoint screenPoint);
	void NewEventAt(aircal::Instant start, aircal::Instant end, bool allDay,
		const aircal::Date& date, BPoint screenPoint, const std::string& title = "");
	void NewTask(const aircal::Date* due, BPoint screenPoint);
	void ToggleTask(const std::string& taskId, bool completed);
	void MoveEvent(const aircal::DisplayEvent& event, aircal::Instant start,
		aircal::Instant end, bool allDay, const aircal::Date& date, int days);
	void DeleteSelection();
	void DeleteEvent(const aircal::DisplayEvent& event);
	void ShowEventMenu(const aircal::DisplayEvent& event, BPoint screenPoint);
	void ShowTaskMenu(const std::string& taskId, BPoint screenPoint);
	void QuickEntry(const std::string& text);

	const std::string& SearchText() const { return fSearch; }

private:
	void BuildMenus(class BMenuBar* bar);
	void UpdateHeader();
	void ReloadAll();
	CalendarView* CurrentView() const;
	void UpdateSyncStatus(BMessage* message);

	HeaderView* fHeader = nullptr;
	SidebarView* fSidebar = nullptr;
	BSplitView* fSplit = nullptr;
	BCardLayout* fCards = nullptr;
	CalendarView* fDayView = nullptr;
	CalendarView* fWeekView = nullptr;
	CalendarView* fMonthView = nullptr;
	CalendarView* fYearView = nullptr;
	CalendarView* fTasksView = nullptr;
	BMenuItem* fShowCompletedItem = nullptr;
	BMenuItem* fUndoItem = nullptr;
	BMenuItem* fRedoItem = nullptr;
	BMenuItem* fShowSidebarItem = nullptr;
	BMenuItem* fViewItems[5] = {};

	std::string fMode = "week";
	aircal::Date fDate;
	Selection fSelection;
	std::string fSearch;
	bool fReloadPending = false;
	bool fSidebarHidden = false;
	float fSidebarWidth = 270;
	std::map<std::string, std::string> fSyncErrors;	// account -> error
	int fSyncRunning = 0;
};
