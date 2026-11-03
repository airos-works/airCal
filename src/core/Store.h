// The local database: accounts, calendars, events and tasks, persisted as
// JSON files in the settings directory. One recursive mutex guards it; the
// UI and the sync thread take it for every access. Pointers into the store
// stay valid until the item is removed (items are heap allocated).
#pragma once

#include "Model.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aircal {

struct Preferences {
	int weekStart = 0;			// 0 Sunday, 1 Monday
	int dayStartHour = 8;		// first hour shown in day/week views
	int defaultDuration = 60;	// minutes
	int defaultAlarm = -1;		// minutes before, -1 none
	int syncMinutes = 5;
	bool showCompleted = false;
	bool showTasksInCalendar = true;
	bool use24Hour = false;
	std::string defaultCalendar;	// local ids
	std::string defaultTaskList;
	std::string view = "week";		// day, week, month, year, tasks
	std::string taskFilter = "today";	// today, upcoming, all, completed, list:<id>
	std::string windowFrame;		// "l t r b"
	float sidebarWidth = 270;
	bool notifications = true;
};

// A copy of what one occurrence needs for drawing, safe to keep while the
// store changes.
struct DisplayEvent {
	std::string eventId;	// the master's (or standalone) id
	std::string overrideId;	// the override's id, if the instance is one
	std::string calendarId;
	std::string title;
	std::string location;
	uint32_t color = 0;
	bool allDay = false;
	bool tentative = false;
	bool recurring = false;
	bool readOnly = false;
	bool hasAlarm = false;
	bool pending = false;		// not yet on the server
	Instant start = 0;
	Instant end = 0;
	Date startDate;			// first day
	Date endDate;			// last day, inclusive
	Instant originalStart = 0;
	Date originalDate;
};

struct DisplayTask {
	std::string taskId;
	std::string calendarId;
	std::string listName;
	std::string title;
	std::string notes;
	uint32_t color = 0;
	bool hasDue = false;
	Date dueDate;
	bool dueHasTime = false;
	Instant dueTime = 0;
	bool completed = false;
	bool recurring = false;
	int priority = 0;
	int depth = 0;			// subtask nesting
	std::vector<std::string> labels;
	bool pending = false;
};

class Store {
public:
	explicit Store(const std::string& directory);
	~Store();

	// Loads the JSON files; creates a local "On My Computer" account with a
	// calendar and a task list when there is nothing yet.
	bool Load();
	// Writes the files that changed (atomically: write + rename).
	bool Save();
	void MarkDirty();
	bool IsDirty() const;

	std::recursive_mutex& Mutex() { return fMutex; }
	uint64_t Revision() const { return fRevision; }
	// Called (on the calling thread) after every change.
	void SetChangeListener(std::function<void()> listener);
	// Bumps the revision and notifies; call after modifying items.
	void Changed();

	Preferences& Prefs() { return fPreferences; }

	// --- Items -----------------------------------------------------------------
	std::vector<std::unique_ptr<Account>>& Accounts() { return fAccounts; }
	std::vector<std::unique_ptr<Calendar>>& Calendars() { return fCalendars; }
	std::vector<std::unique_ptr<Event>>& Events() { return fEvents; }
	std::vector<std::unique_ptr<Task>>& Tasks() { return fTasks; }

	Account* FindAccount(const std::string& id) const;
	Calendar* FindCalendar(const std::string& id) const;
	Calendar* FindCalendarByRemote(const std::string& accountId,
		const std::string& remoteId) const;
	Event* FindEvent(const std::string& id) const;
	Event* FindEventByRemote(const std::string& calendarId,
		const std::string& remoteId) const;
	Task* FindTask(const std::string& id) const;
	Task* FindTaskByRemote(const std::string& calendarId,
		const std::string& remoteId) const;
	std::vector<Calendar*> CalendarsOf(const std::string& accountId) const;
	std::vector<Event*> EventsIn(const std::string& calendarId) const;
	std::vector<Event*> OverridesOf(const std::string& masterId) const;
	std::vector<Task*> TasksIn(const std::string& calendarId) const;

	Account& AddAccount(const Account& account);
	Calendar& AddCalendar(const Calendar& calendar);
	Event& AddEvent(const Event& event);
	Task& AddTask(const Task& task);
	// Removes outright (no sync). Removing an account removes its calendars,
	// removing a calendar its events/tasks, removing a master its overrides.
	void RemoveAccount(const std::string& id);
	void RemoveCalendar(const std::string& id);
	void RemoveEvent(const std::string& id);
	void RemoveTask(const std::string& id);

	// The calendar new events / tasks go to.
	Calendar* DefaultCalendar(bool forTasks) const;
	Account* AccountOf(const Calendar& calendar) const;

	// --- Queries for the UI ----------------------------------------------------
	std::vector<DisplayEvent> EventsBetween(Instant from, Instant to,
		bool visibleOnly = true) const;
	// Tasks due within [from, to] (inclusive), plus overdue incomplete tasks
	// when includeOverdue.
	std::vector<DisplayTask> TasksBetween(const Date& from, const Date& to,
		bool includeCompleted, bool includeOverdue = false) const;
	// All tasks of the visible task lists (or of one list), ordered as lists
	// show them: by list, then hierarchy and order.
	std::vector<DisplayTask> TaskList(const std::string& calendarId,
		bool includeCompleted) const;
	std::vector<DisplayEvent> Search(const std::string& text, size_t limit) const;
	std::vector<DisplayTask> SearchTasks(const std::string& text, size_t limit) const;
	DisplayTask MakeDisplay(const Task& task) const;

	// --- Local edits (mark pending for sync) -----------------------------------
	// Applies an edited copy of an event; sets pending, revision, updated.
	void UpdateEvent(const Event& edited);
	// Deletes: local-only items go at once, synced ones become Pending::Delete.
	void DeleteEvent(const std::string& id);
	void UpdateTask(const Task& edited);
	void DeleteTask(const std::string& id);
	void SetTaskCompleted(const std::string& id, bool completed);

	const std::string& Directory() const { return fDirectory; }

private:
	bool LoadFile(const std::string& name, std::string& out) const;
	bool WriteFile(const std::string& name, const std::string& data) const;

	std::string fDirectory;
	mutable std::recursive_mutex fMutex;
	uint64_t fRevision = 1;
	bool fDirty = false;
	std::function<void()> fListener;
	Preferences fPreferences;
	std::vector<std::unique_ptr<Account>> fAccounts;
	std::vector<std::unique_ptr<Calendar>> fCalendars;
	std::vector<std::unique_ptr<Event>> fEvents;
	std::vector<std::unique_ptr<Task>> fTasks;
};

// JSON (de)serialization, shared with tests.
std::string EventToJsonText(const Event& event);
bool EventFromJsonText(const std::string& text, Event& event);

}	// namespace aircal
