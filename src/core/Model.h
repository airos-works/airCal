// airCal's data model: accounts, calendars (event calendars and task lists),
// events and tasks. Everything here is plain data; Store keeps it, providers
// fill it, the UI draws it.
#pragma once

#include "DateTime.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aircal {

enum class AccountType {
	Local,		// "On My Computer"
	Google,		// Google Calendar + Google Tasks
	CalDAV,		// any CalDAV server (events, and VTODO tasks where offered)
	Todoist		// Todoist tasks
};

const char* AccountTypeName(AccountType type);	// "local", "google", ...
AccountType AccountTypeFromName(const std::string& name);

struct Account {
	std::string id;
	AccountType type = AccountType::Local;
	std::string name;		// shown in the sidebar, e.g. "you@gmail.com"
	std::string user;		// login (CalDAV user name, Google/Todoist email)
	std::string server;		// CalDAV: the URL the user typed; others: API base override
	std::string principal;	// CalDAV: discovered calendar home
	bool enabled = true;
	Instant lastSync = 0;
	std::string lastError;
	std::string syncState;	// provider-private JSON (sync tokens etc.)
};

enum class Pending {
	None,
	Create,		// made locally, not on the server yet
	Update,		// changed locally
	Delete		// deleted locally, still on the server
};

const char* PendingName(Pending pending);
Pending PendingFromName(const std::string& name);

// A collection: an event calendar, a task list, or (CalDAV) both.
struct Calendar {
	std::string id;			// local id, stable
	std::string accountId;
	std::string remoteId;	// Google calendar id, CalDAV collection href, Todoist project id
	std::string name;
	uint32_t color = 0x4a90d9;	// 0xRRGGBB
	bool events = true;		// holds events
	bool tasks = false;		// holds tasks
	bool visible = true;
	bool readOnly = false;
	bool primary = false;	// Google primary calendar / Todoist Inbox
	std::string parentId;	// Todoist sub-projects
	int order = 0;
	std::string ctag;		// CalDAV getctag / sync-token
	std::string syncToken;	// Google nextSyncToken
	std::vector<int> defaultAlarms;	// minutes before (Google default reminders)
	bool pendingCreate = false;	// made in airCal, not on the server yet
};

// One event. A recurring series is a master (rrule set) plus overrides: events
// with masterId set to the master's id and recurrenceId set to the original
// start of the instance they replace.
struct Event {
	std::string id;			// local id, stable
	std::string calendarId;
	std::string remoteId;	// Google event id or CalDAV resource href
	std::string uid;		// iCalendar UID (Google iCalUID)
	std::string etag;

	std::string title;
	std::string location;
	std::string notes;
	std::string url;		// also video-call links
	std::string organizer;
	std::vector<std::string> attendees;	// "Name <mail>" for display
	bool tentative = false;
	bool cancelled = false;	// a cancelled override (deleted instance)

	bool allDay = false;
	// Timed events: instants. tzid is the zone the event was written in (it
	// drives recurrence across DST); "" means floating/local.
	Instant start = 0;
	Instant end = 0;
	std::string tzid;
	// All-day events: dates, end exclusive.
	Date startDate;
	Date endDate;

	// Recurrence (masters only). rrule without the "RRULE:" prefix.
	std::string rrule;
	std::vector<Instant> exdates;		// timed: original starts excluded
	std::vector<Date> exdateDays;		// all-day: dates excluded
	std::vector<Instant> rdates;

	// Overrides only.
	std::string masterId;
	Instant recurrenceId = 0;		// original start (timed)
	Date recurrenceDate;			// original date (all-day)

	std::vector<int> alarms;		// minutes before start
	Pending pending = Pending::None;
	uint32_t revision = 0;		// bumped by every local edit (sync races)
	Instant updated = 0;
	std::string raw;	// provider data for a faithful round trip (ICS / JSON)

	bool IsRecurring() const { return !rrule.empty() || !rdates.empty(); }
	bool IsOverride() const { return !masterId.empty(); }
	// Length in seconds (timed) or days (all-day).
	int64_t Duration() const
	{
		return allDay ? endDate.DayNumber() - startDate.DayNumber() : end - start;
	}
};

// One displayed instance of an event.
struct Occurrence {
	const Event* event = nullptr;	// the master, or the override replacing it
	const Event* master = nullptr;	// the series master (== event if not recurring)
	bool allDay = false;
	Instant start = 0;
	Instant end = 0;
	Date startDate;
	Date endDate;		// exclusive
	Instant originalStart = 0;	// recurrence id of this instance
	Date originalDate;

	// First and last local dates the occurrence covers.
	Date FirstDay() const;
	Date LastDay() const;
};

struct Task {
	std::string id;			// local id
	std::string calendarId;	// the task list
	std::string remoteId;
	std::string uid;		// CalDAV VTODO UID
	std::string etag;

	std::string title;
	std::string notes;
	bool hasDue = false;
	Date dueDate;
	bool dueHasTime = false;
	Instant dueTime = 0;	// when dueHasTime
	std::string dueString;	// Todoist natural language due ("every monday")
	bool recurring = false;
	std::string rrule;		// CalDAV VTODO recurrence
	int durationMinutes = 0;
	Date deadline;			// Todoist deadline (optional)
	bool hasDeadline = false;

	// 0 none, 1 low, 2 medium, 3 high, 4 urgent (Todoist p1 = 4)
	int priority = 0;
	bool completed = false;
	Instant completedAt = 0;
	std::string parentId;	// local id of the parent task
	std::vector<std::string> labels;
	std::string order;		// sort key
	std::string section;	// Todoist section name
	std::vector<int> alarms;	// minutes before due (local reminders)

	Pending pending = Pending::None;
	uint32_t revision = 0;		// bumped by every local edit (sync races)
	Instant updated = 0;
	Instant created = 0;
	std::string raw;
};

// Recurrence presets offered in the editors.
enum class RepeatPreset {
	None, Daily, Weekdays, Weekly, Biweekly, Monthly, Yearly, Custom
};

RepeatPreset PresetFromRule(const std::string& rrule);
std::string RuleFromPreset(RepeatPreset preset, const Date& start);
std::string DescribeRule(const std::string& rrule);	// "Every week on Monday"

std::string NewId();	// random 128-bit id, hex

}	// namespace aircal
