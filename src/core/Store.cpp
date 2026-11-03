#include "Store.h"
#include "Recurrence.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using nlohmann::json;

namespace aircal {

namespace {

// Type-checked getters: files written by older versions or by hand must not
// throw.
std::string
Str(const json& j, const char* key)
{
	auto it = j.find(key);
	return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}


int64_t
Int(const json& j, const char* key, int64_t fallback = 0)
{
	auto it = j.find(key);
	return it != j.end() && it->is_number() ? it->get<int64_t>() : fallback;
}


bool
Bool(const json& j, const char* key, bool fallback = false)
{
	auto it = j.find(key);
	return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}


Date
DateOf(const json& j, const char* key)
{
	Date date;
	Date::FromIso(Str(j, key), date);
	return date;
}


std::vector<int>
Ints(const json& j, const char* key)
{
	std::vector<int> result;
	auto it = j.find(key);
	if (it != j.end() && it->is_array()) {
		for (auto& value : *it) {
			if (value.is_number())
				result.push_back(value.get<int>());
		}
	}
	return result;
}


std::vector<std::string>
Strings(const json& j, const char* key)
{
	std::vector<std::string> result;
	auto it = j.find(key);
	if (it != j.end() && it->is_array()) {
		for (auto& value : *it) {
			if (value.is_string())
				result.push_back(value.get<std::string>());
		}
	}
	return result;
}


json
AccountToJson(const Account& a)
{
	return json{{"id", a.id}, {"type", AccountTypeName(a.type)}, {"name", a.name},
		{"user", a.user}, {"server", a.server}, {"principal", a.principal},
		{"enabled", a.enabled}, {"lastSync", a.lastSync},
		{"lastError", a.lastError}, {"syncState", a.syncState}};
}


Account
AccountFromJson(const json& j)
{
	Account a;
	a.id = Str(j, "id");
	a.type = AccountTypeFromName(Str(j, "type"));
	a.name = Str(j, "name");
	a.user = Str(j, "user");
	a.server = Str(j, "server");
	a.principal = Str(j, "principal");
	a.enabled = Bool(j, "enabled", true);
	a.lastSync = Int(j, "lastSync");
	a.lastError = Str(j, "lastError");
	a.syncState = Str(j, "syncState");
	return a;
}


json
CalendarToJson(const Calendar& c)
{
	return json{{"id", c.id}, {"account", c.accountId}, {"remoteId", c.remoteId},
		{"name", c.name}, {"color", c.color}, {"events", c.events},
		{"tasks", c.tasks}, {"visible", c.visible}, {"readOnly", c.readOnly},
		{"primary", c.primary}, {"parent", c.parentId}, {"order", c.order},
		{"ctag", c.ctag}, {"syncToken", c.syncToken},
		{"defaultAlarms", c.defaultAlarms}};
}


Calendar
CalendarFromJson(const json& j)
{
	Calendar c;
	c.id = Str(j, "id");
	c.accountId = Str(j, "account");
	c.remoteId = Str(j, "remoteId");
	c.name = Str(j, "name");
	c.color = (uint32_t)Int(j, "color", 0x4a90d9);
	c.events = Bool(j, "events", true);
	c.tasks = Bool(j, "tasks", false);
	c.visible = Bool(j, "visible", true);
	c.readOnly = Bool(j, "readOnly");
	c.primary = Bool(j, "primary");
	c.parentId = Str(j, "parent");
	c.order = (int)Int(j, "order");
	c.ctag = Str(j, "ctag");
	c.syncToken = Str(j, "syncToken");
	c.defaultAlarms = Ints(j, "defaultAlarms");
	return c;
}


json
EventToJson(const Event& e)
{
	json j{{"id", e.id}, {"calendar", e.calendarId}, {"title", e.title}};
	auto put = [&](const char* key, const std::string& value) {
		if (!value.empty())
			j[key] = value;
	};
	put("remoteId", e.remoteId);
	put("uid", e.uid);
	put("etag", e.etag);
	put("location", e.location);
	put("notes", e.notes);
	put("url", e.url);
	put("organizer", e.organizer);
	if (!e.attendees.empty())
		j["attendees"] = e.attendees;
	if (e.tentative)
		j["tentative"] = true;
	if (e.cancelled)
		j["cancelled"] = true;
	if (e.allDay) {
		j["allDay"] = true;
		j["startDate"] = e.startDate.ToIso();
		j["endDate"] = e.endDate.ToIso();
	} else {
		j["start"] = e.start;
		j["end"] = e.end;
		put("tzid", e.tzid);
	}
	put("rrule", e.rrule);
	if (!e.exdates.empty())
		j["exdates"] = e.exdates;
	if (!e.exdateDays.empty()) {
		json days = json::array();
		for (auto& day : e.exdateDays)
			days.push_back(day.ToIso());
		j["exdateDays"] = days;
	}
	if (!e.rdates.empty())
		j["rdates"] = e.rdates;
	if (!e.masterId.empty()) {
		j["master"] = e.masterId;
		j["recurrenceId"] = e.recurrenceId;
		j["recurrenceDate"] = e.recurrenceDate.ToIso();
	}
	if (!e.alarms.empty())
		j["alarms"] = e.alarms;
	put("pending", PendingName(e.pending));
	if (e.revision != 0)
		j["revision"] = e.revision;
	if (e.updated != 0)
		j["updated"] = e.updated;
	put("raw", e.raw);
	return j;
}


Event
EventFromJson(const json& j)
{
	Event e;
	e.id = Str(j, "id");
	e.calendarId = Str(j, "calendar");
	e.remoteId = Str(j, "remoteId");
	e.uid = Str(j, "uid");
	e.etag = Str(j, "etag");
	e.title = Str(j, "title");
	e.location = Str(j, "location");
	e.notes = Str(j, "notes");
	e.url = Str(j, "url");
	e.organizer = Str(j, "organizer");
	e.attendees = Strings(j, "attendees");
	e.tentative = Bool(j, "tentative");
	e.cancelled = Bool(j, "cancelled");
	e.allDay = Bool(j, "allDay");
	if (e.allDay) {
		e.startDate = DateOf(j, "startDate");
		e.endDate = DateOf(j, "endDate");
		if (e.endDate <= e.startDate)
			e.endDate = e.startDate.AddDays(1);
	} else {
		e.start = Int(j, "start");
		e.end = Int(j, "end", e.start);
		e.tzid = Str(j, "tzid");
	}
	e.rrule = Str(j, "rrule");
	auto exdates = j.find("exdates");
	if (exdates != j.end() && exdates->is_array()) {
		for (auto& value : *exdates) {
			if (value.is_number())
				e.exdates.push_back(value.get<int64_t>());
		}
	}
	for (auto& day : Strings(j, "exdateDays")) {
		Date date;
		if (Date::FromIso(day, date))
			e.exdateDays.push_back(date);
	}
	auto rdates = j.find("rdates");
	if (rdates != j.end() && rdates->is_array()) {
		for (auto& value : *rdates) {
			if (value.is_number())
				e.rdates.push_back(value.get<int64_t>());
		}
	}
	e.masterId = Str(j, "master");
	e.recurrenceId = Int(j, "recurrenceId");
	e.recurrenceDate = DateOf(j, "recurrenceDate");
	e.alarms = Ints(j, "alarms");
	e.pending = PendingFromName(Str(j, "pending"));
	e.revision = (uint32_t)Int(j, "revision");
	e.updated = Int(j, "updated");
	e.raw = Str(j, "raw");
	return e;
}


json
TaskToJson(const Task& t)
{
	json j{{"id", t.id}, {"calendar", t.calendarId}, {"title", t.title}};
	auto put = [&](const char* key, const std::string& value) {
		if (!value.empty())
			j[key] = value;
	};
	put("remoteId", t.remoteId);
	put("uid", t.uid);
	put("etag", t.etag);
	put("notes", t.notes);
	if (t.hasDue) {
		j["due"] = t.dueDate.ToIso();
		if (t.dueHasTime)
			j["dueTime"] = t.dueTime;
	}
	put("dueString", t.dueString);
	if (t.recurring)
		j["recurring"] = true;
	put("rrule", t.rrule);
	if (t.durationMinutes != 0)
		j["duration"] = t.durationMinutes;
	if (t.hasDeadline)
		j["deadline"] = t.deadline.ToIso();
	if (t.priority != 0)
		j["priority"] = t.priority;
	if (t.completed) {
		j["completed"] = true;
		j["completedAt"] = t.completedAt;
	}
	put("parent", t.parentId);
	if (!t.labels.empty())
		j["labels"] = t.labels;
	put("order", t.order);
	put("section", t.section);
	if (!t.alarms.empty())
		j["alarms"] = t.alarms;
	put("pending", PendingName(t.pending));
	if (t.revision != 0)
		j["revision"] = t.revision;
	if (t.updated != 0)
		j["updated"] = t.updated;
	if (t.created != 0)
		j["created"] = t.created;
	put("raw", t.raw);
	return j;
}


Task
TaskFromJson(const json& j)
{
	Task t;
	t.id = Str(j, "id");
	t.calendarId = Str(j, "calendar");
	t.remoteId = Str(j, "remoteId");
	t.uid = Str(j, "uid");
	t.etag = Str(j, "etag");
	t.title = Str(j, "title");
	t.notes = Str(j, "notes");
	std::string due = Str(j, "due");
	t.hasDue = Date::FromIso(due, t.dueDate);
	if (t.hasDue && j.contains("dueTime")) {
		t.dueHasTime = true;
		t.dueTime = Int(j, "dueTime");
	}
	t.dueString = Str(j, "dueString");
	t.recurring = Bool(j, "recurring");
	t.rrule = Str(j, "rrule");
	t.durationMinutes = (int)Int(j, "duration");
	t.hasDeadline = Date::FromIso(Str(j, "deadline"), t.deadline);
	t.priority = (int)Int(j, "priority");
	t.completed = Bool(j, "completed");
	t.completedAt = Int(j, "completedAt");
	t.parentId = Str(j, "parent");
	t.labels = Strings(j, "labels");
	t.order = Str(j, "order");
	t.section = Str(j, "section");
	t.alarms = Ints(j, "alarms");
	t.pending = PendingFromName(Str(j, "pending"));
	t.revision = (uint32_t)Int(j, "revision");
	t.updated = Int(j, "updated");
	t.created = Int(j, "created");
	t.raw = Str(j, "raw");
	return t;
}


json
PreferencesToJson(const Preferences& p)
{
	return json{{"weekStart", p.weekStart}, {"dayStartHour", p.dayStartHour},
		{"defaultDuration", p.defaultDuration}, {"defaultAlarm", p.defaultAlarm},
		{"syncMinutes", p.syncMinutes}, {"showCompleted", p.showCompleted},
		{"showTasksInCalendar", p.showTasksInCalendar},
		{"use24Hour", p.use24Hour}, {"defaultCalendar", p.defaultCalendar},
		{"defaultTaskList", p.defaultTaskList}, {"view", p.view},
		{"taskFilter", p.taskFilter}, {"windowFrame", p.windowFrame},
		{"sidebarWidth", p.sidebarWidth}, {"notifications", p.notifications}};
}


Preferences
PreferencesFromJson(const json& j)
{
	Preferences p;
	p.weekStart = (int)Int(j, "weekStart", p.weekStart) % 7;
	p.dayStartHour = std::max(0, std::min(20, (int)Int(j, "dayStartHour",
		p.dayStartHour)));
	p.defaultDuration = std::max(5, (int)Int(j, "defaultDuration",
		p.defaultDuration));
	p.defaultAlarm = (int)Int(j, "defaultAlarm", p.defaultAlarm);
	p.syncMinutes = std::max(1, (int)Int(j, "syncMinutes", p.syncMinutes));
	p.showCompleted = Bool(j, "showCompleted", p.showCompleted);
	p.showTasksInCalendar = Bool(j, "showTasksInCalendar", p.showTasksInCalendar);
	p.use24Hour = Bool(j, "use24Hour", p.use24Hour);
	p.defaultCalendar = Str(j, "defaultCalendar");
	p.defaultTaskList = Str(j, "defaultTaskList");
	std::string view = Str(j, "view");
	if (!view.empty())
		p.view = view;
	std::string filter = Str(j, "taskFilter");
	if (!filter.empty())
		p.taskFilter = filter;
	p.windowFrame = Str(j, "windowFrame");
	auto width = j.find("sidebarWidth");
	if (width != j.end() && width->is_number())
		p.sidebarWidth = std::max(200.0f, std::min(500.0f, width->get<float>()));
	p.notifications = Bool(j, "notifications", p.notifications);
	return p;
}


std::string
Lower(const std::string& text)
{
	std::string result = text;
	for (char& c : result) {
		if (c >= 'A' && c <= 'Z')
			c = c - 'A' + 'a';
	}
	return result;
}

}	// namespace


std::string
EventToJsonText(const Event& event)
{
	return EventToJson(event).dump();
}


bool
EventFromJsonText(const std::string& text, Event& event)
{
	json j = json::parse(text, nullptr, false);
	if (j.is_discarded() || !j.is_object())
		return false;
	event = EventFromJson(j);
	return true;
}


// --- Store -------------------------------------------------------------------------

Store::Store(const std::string& directory)
	:
	fDirectory(directory)
{
}


Store::~Store()
{
}


bool
Store::LoadFile(const std::string& name, std::string& out) const
{
	FILE* file = fopen((fDirectory + "/" + name).c_str(), "rb");
	if (file == nullptr)
		return false;
	char buffer[65536];
	size_t n;
	out.clear();
	while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0)
		out.append(buffer, n);
	fclose(file);
	return true;
}


bool
Store::WriteFile(const std::string& name, const std::string& data) const
{
	std::string path = fDirectory + "/" + name;
	std::string temporary = path + ".new";
	int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return false;
	size_t written = 0;
	while (written < data.size()) {
		ssize_t n = write(fd, data.data() + written, data.size() - written);
		if (n <= 0) {
			close(fd);
			unlink(temporary.c_str());
			return false;
		}
		written += n;
	}
	fsync(fd);
	close(fd);
	return rename(temporary.c_str(), path.c_str()) == 0;
}


bool
Store::Load()
{
	std::lock_guard<std::recursive_mutex> lock(fMutex);
	mkdir(fDirectory.c_str(), 0700);

	std::string text;
	if (LoadFile("preferences.json", text)) {
		json j = json::parse(text, nullptr, false);
		if (j.is_object())
			fPreferences = PreferencesFromJson(j);
	}
	if (LoadFile("accounts.json", text)) {
		json j = json::parse(text, nullptr, false);
		if (j.is_array()) {
			for (auto& item : j) {
				if (item.is_object())
					fAccounts.push_back(std::make_unique<Account>(AccountFromJson(item)));
			}
		}
	}
	if (LoadFile("calendars.json", text)) {
		json j = json::parse(text, nullptr, false);
		if (j.is_array()) {
			for (auto& item : j) {
				if (item.is_object())
					fCalendars.push_back(std::make_unique<Calendar>(CalendarFromJson(item)));
			}
		}
	}
	if (LoadFile("events.json", text)) {
		json j = json::parse(text, nullptr, false);
		if (j.is_array()) {
			for (auto& item : j) {
				if (item.is_object())
					fEvents.push_back(std::make_unique<Event>(EventFromJson(item)));
			}
		}
	}
	if (LoadFile("tasks.json", text)) {
		json j = json::parse(text, nullptr, false);
		if (j.is_array()) {
			for (auto& item : j) {
				if (item.is_object())
					fTasks.push_back(std::make_unique<Task>(TaskFromJson(item)));
			}
		}
	}

	bool hasLocal = false;
	for (auto& account : fAccounts) {
		if (account->type == AccountType::Local)
			hasLocal = true;
	}
	if (!hasLocal) {
		Account local;
		local.id = "local";
		local.type = AccountType::Local;
		local.name = "On My Computer";
		AddAccount(local);
		Calendar calendar;
		calendar.id = NewId();
		calendar.accountId = local.id;
		calendar.name = "Calendar";
		calendar.color = 0xe8504a;
		calendar.events = true;
		AddCalendar(calendar);
		Calendar list;
		list.id = NewId();
		list.accountId = local.id;
		list.name = "Reminders";
		list.color = 0x3b8ced;
		list.events = false;
		list.tasks = true;
		AddCalendar(list);
		fDirty = true;
	}
	return true;
}


bool
Store::Save()
{
	std::string preferences, accounts, calendars, events, tasks;
	{
		std::lock_guard<std::recursive_mutex> lock(fMutex);
		preferences = PreferencesToJson(fPreferences).dump(1, '\t');
		json a = json::array();
		for (auto& account : fAccounts)
			a.push_back(AccountToJson(*account));
		accounts = a.dump(1, '\t');
		json c = json::array();
		for (auto& calendar : fCalendars)
			c.push_back(CalendarToJson(*calendar));
		calendars = c.dump(1, '\t');
		json e = json::array();
		for (auto& event : fEvents)
			e.push_back(EventToJson(*event));
		events = e.dump(0, ' ', false, json::error_handler_t::replace);
		json t = json::array();
		for (auto& task : fTasks)
			t.push_back(TaskToJson(*task));
		tasks = t.dump(0, ' ', false, json::error_handler_t::replace);
		fDirty = false;
	}
	mkdir(fDirectory.c_str(), 0700);
	bool ok = WriteFile("preferences.json", preferences);
	ok = WriteFile("accounts.json", accounts) && ok;
	ok = WriteFile("calendars.json", calendars) && ok;
	ok = WriteFile("events.json", events) && ok;
	ok = WriteFile("tasks.json", tasks) && ok;
	return ok;
}


void
Store::MarkDirty()
{
	std::lock_guard<std::recursive_mutex> lock(fMutex);
	fDirty = true;
}


bool
Store::IsDirty() const
{
	std::lock_guard<std::recursive_mutex> lock(fMutex);
	return fDirty;
}


void
Store::SetChangeListener(std::function<void()> listener)
{
	std::lock_guard<std::recursive_mutex> lock(fMutex);
	fListener = listener;
}


void
Store::Changed()
{
	std::function<void()> listener;
	{
		std::lock_guard<std::recursive_mutex> lock(fMutex);
		fRevision++;
		fDirty = true;
		listener = fListener;
	}
	if (listener)
		listener();
}


Account*
Store::FindAccount(const std::string& id) const
{
	for (auto& item : fAccounts) {
		if (item->id == id)
			return item.get();
	}
	return nullptr;
}


Calendar*
Store::FindCalendar(const std::string& id) const
{
	for (auto& item : fCalendars) {
		if (item->id == id)
			return item.get();
	}
	return nullptr;
}


Calendar*
Store::FindCalendarByRemote(const std::string& accountId,
	const std::string& remoteId) const
{
	for (auto& item : fCalendars) {
		if (item->accountId == accountId && item->remoteId == remoteId)
			return item.get();
	}
	return nullptr;
}


Event*
Store::FindEvent(const std::string& id) const
{
	for (auto& item : fEvents) {
		if (item->id == id)
			return item.get();
	}
	return nullptr;
}


Event*
Store::FindEventByRemote(const std::string& calendarId,
	const std::string& remoteId) const
{
	for (auto& item : fEvents) {
		if (item->calendarId == calendarId && item->remoteId == remoteId)
			return item.get();
	}
	return nullptr;
}


Task*
Store::FindTask(const std::string& id) const
{
	for (auto& item : fTasks) {
		if (item->id == id)
			return item.get();
	}
	return nullptr;
}


Task*
Store::FindTaskByRemote(const std::string& calendarId,
	const std::string& remoteId) const
{
	for (auto& item : fTasks) {
		if (item->calendarId == calendarId && item->remoteId == remoteId)
			return item.get();
	}
	return nullptr;
}


std::vector<Calendar*>
Store::CalendarsOf(const std::string& accountId) const
{
	std::vector<Calendar*> result;
	for (auto& item : fCalendars) {
		if (item->accountId == accountId)
			result.push_back(item.get());
	}
	std::stable_sort(result.begin(), result.end(),
		[](const Calendar* a, const Calendar* b) {
			if (a->primary != b->primary)
				return a->primary;
			return a->order < b->order;
		});
	return result;
}


std::vector<Event*>
Store::EventsIn(const std::string& calendarId) const
{
	std::vector<Event*> result;
	for (auto& item : fEvents) {
		if (item->calendarId == calendarId)
			result.push_back(item.get());
	}
	return result;
}


std::vector<Event*>
Store::OverridesOf(const std::string& masterId) const
{
	std::vector<Event*> result;
	for (auto& item : fEvents) {
		if (item->masterId == masterId)
			result.push_back(item.get());
	}
	return result;
}


std::vector<Task*>
Store::TasksIn(const std::string& calendarId) const
{
	std::vector<Task*> result;
	for (auto& item : fTasks) {
		if (item->calendarId == calendarId)
			result.push_back(item.get());
	}
	return result;
}


Account&
Store::AddAccount(const Account& account)
{
	std::lock_guard<std::recursive_mutex> lock(fMutex);
	fAccounts.push_back(std::make_unique<Account>(account));
	if (fAccounts.back()->id.empty())
		fAccounts.back()->id = NewId();
	fDirty = true;
	return *fAccounts.back();
}


Calendar&
Store::AddCalendar(const Calendar& calendar)
{
	std::lock_guard<std::recursive_mutex> lock(fMutex);
	fCalendars.push_back(std::make_unique<Calendar>(calendar));
	if (fCalendars.back()->id.empty())
		fCalendars.back()->id = NewId();
	fDirty = true;
	return *fCalendars.back();
}


Event&
Store::AddEvent(const Event& event)
{
	std::lock_guard<std::recursive_mutex> lock(fMutex);
	fEvents.push_back(std::make_unique<Event>(event));
	if (fEvents.back()->id.empty())
		fEvents.back()->id = NewId();
	fDirty = true;
	return *fEvents.back();
}


Task&
Store::AddTask(const Task& task)
{
	std::lock_guard<std::recursive_mutex> lock(fMutex);
	fTasks.push_back(std::make_unique<Task>(task));
	if (fTasks.back()->id.empty())
		fTasks.back()->id = NewId();
	fDirty = true;
	return *fTasks.back();
}


void
Store::RemoveAccount(const std::string& idReference)
{
	// A copy: the reference may point into the item being removed.
	const std::string id = idReference;
	std::lock_guard<std::recursive_mutex> lock(fMutex);
	for (Calendar* calendar : CalendarsOf(id))
		RemoveCalendar(calendar->id);
	fAccounts.erase(std::remove_if(fAccounts.begin(), fAccounts.end(),
		[&](const std::unique_ptr<Account>& a) { return a->id == id; }),
		fAccounts.end());
	fDirty = true;
}


void
Store::RemoveCalendar(const std::string& idReference)
{
	// A copy: the reference may point into the item being removed.
	const std::string id = idReference;
	std::lock_guard<std::recursive_mutex> lock(fMutex);
	fEvents.erase(std::remove_if(fEvents.begin(), fEvents.end(),
		[&](const std::unique_ptr<Event>& e) { return e->calendarId == id; }),
		fEvents.end());
	fTasks.erase(std::remove_if(fTasks.begin(), fTasks.end(),
		[&](const std::unique_ptr<Task>& t) { return t->calendarId == id; }),
		fTasks.end());
	fCalendars.erase(std::remove_if(fCalendars.begin(), fCalendars.end(),
		[&](const std::unique_ptr<Calendar>& c) { return c->id == id; }),
		fCalendars.end());
	if (fPreferences.defaultCalendar == id)
		fPreferences.defaultCalendar.clear();
	if (fPreferences.defaultTaskList == id)
		fPreferences.defaultTaskList.clear();
	fDirty = true;
}


void
Store::RemoveEvent(const std::string& idReference)
{
	// A copy: the reference may point into the item being removed.
	const std::string id = idReference;
	std::lock_guard<std::recursive_mutex> lock(fMutex);
	fEvents.erase(std::remove_if(fEvents.begin(), fEvents.end(),
		[&](const std::unique_ptr<Event>& e) {
			return e->id == id || e->masterId == id;
		}), fEvents.end());
	fDirty = true;
}


void
Store::RemoveTask(const std::string& idReference)
{
	// A copy: the reference may point into the item being removed.
	const std::string id = idReference;
	std::lock_guard<std::recursive_mutex> lock(fMutex);
	fTasks.erase(std::remove_if(fTasks.begin(), fTasks.end(),
		[&](const std::unique_ptr<Task>& t) { return t->id == id; }),
		fTasks.end());
	fDirty = true;
}


Calendar*
Store::DefaultCalendar(bool forTasks) const
{
	std::lock_guard<std::recursive_mutex> lock(fMutex);
	Calendar* preferred = FindCalendar(forTasks
		? fPreferences.defaultTaskList : fPreferences.defaultCalendar);
	if (preferred != nullptr && !preferred->readOnly
		&& (forTasks ? preferred->tasks : preferred->events))
		return preferred;
	// A primary calendar of a remote account, then any writable one.
	Calendar* fallback = nullptr;
	for (auto& calendar : fCalendars) {
		if (calendar->readOnly || !(forTasks ? calendar->tasks : calendar->events))
			continue;
		Account* account = FindAccount(calendar->accountId);
		if (account == nullptr || !account->enabled)
			continue;
		if (calendar->primary)
			return calendar.get();
		if (fallback == nullptr || (account->type != AccountType::Local
				&& FindAccount(fallback->accountId)->type == AccountType::Local))
			fallback = calendar.get();
	}
	return fallback;
}


Account*
Store::AccountOf(const Calendar& calendar) const
{
	return FindAccount(calendar.accountId);
}


std::vector<DisplayEvent>
Store::EventsBetween(Instant from, Instant to, bool visibleOnly) const
{
	std::lock_guard<std::recursive_mutex> lock(fMutex);
	std::vector<DisplayEvent> result;
	std::map<std::string, std::vector<const Event*>> overrides;
	for (auto& event : fEvents) {
		if (!event->masterId.empty())
			overrides[event->masterId].push_back(event.get());
	}
	std::map<std::string, const Calendar*> calendars;
	for (auto& calendar : fCalendars) {
		const Account* account = FindAccount(calendar->accountId);
		if (account == nullptr || !account->enabled)
			continue;
		if (calendar->events && (!visibleOnly || calendar->visible))
			calendars[calendar->id] = calendar.get();
	}

	for (auto& event : fEvents) {
		if (!event->masterId.empty() && FindEvent(event->masterId) != nullptr)
			continue;	// expanded with its master
		if (event->pending == Pending::Delete || event->cancelled)
			continue;
		auto calendar = calendars.find(event->calendarId);
		if (calendar == calendars.end())
			continue;
		static const std::vector<const Event*> kNone;
		auto found = overrides.find(event->id);
		const std::vector<const Event*>& list
			= found != overrides.end() ? found->second : kNone;
		for (const Occurrence& occurrence : ExpandEvent(*event, list, from, to)) {
			const Event& source = *occurrence.event;
			if (source.pending == Pending::Delete)
				continue;
			DisplayEvent display;
			display.eventId = event->id;
			if (occurrence.event != event.get())
				display.overrideId = source.id;
			display.calendarId = event->calendarId;
			display.title = source.title.empty() ? "New Event" : source.title;
			display.location = source.location;
			display.color = calendar->second->color;
			display.allDay = occurrence.allDay;
			display.tentative = source.tentative;
			display.recurring = event->IsRecurring();
			display.readOnly = calendar->second->readOnly;
			display.hasAlarm = !source.alarms.empty();
			display.pending = source.pending != Pending::None;
			display.start = occurrence.allDay ? StartOfDay(occurrence.startDate)
				: occurrence.start;
			display.end = occurrence.allDay ? StartOfDay(occurrence.endDate)
				: occurrence.end;
			display.startDate = occurrence.FirstDay();
			display.endDate = occurrence.LastDay();
			display.originalStart = occurrence.originalStart;
			display.originalDate = occurrence.originalDate;
			result.push_back(display);
		}
	}
	std::sort(result.begin(), result.end(),
		[](const DisplayEvent& a, const DisplayEvent& b) {
			if (a.allDay != b.allDay)
				return a.allDay;
			if (a.start != b.start)
				return a.start < b.start;
			if (a.end != b.end)
				return a.end > b.end;
			return a.title < b.title;
		});
	return result;
}


DisplayTask
Store::MakeDisplay(const Task& task) const
{
	DisplayTask display;
	display.taskId = task.id;
	display.calendarId = task.calendarId;
	display.title = task.title.empty() ? "New Task" : task.title;
	display.notes = task.notes;
	const Calendar* calendar = FindCalendar(task.calendarId);
	if (calendar != nullptr) {
		display.color = calendar->color;
		display.listName = calendar->name;
	}
	display.hasDue = task.hasDue;
	display.dueDate = task.dueDate;
	display.dueHasTime = task.dueHasTime;
	display.dueTime = task.dueTime;
	display.completed = task.completed;
	display.recurring = task.recurring;
	display.priority = task.priority;
	display.labels = task.labels;
	display.pending = task.pending != Pending::None;
	int depth = 0;
	const Task* parent = task.parentId.empty() ? nullptr : FindTask(task.parentId);
	while (parent != nullptr && depth < 8) {
		depth++;
		parent = parent->parentId.empty() ? nullptr : FindTask(parent->parentId);
	}
	display.depth = depth;
	return display;
}


static bool
TaskVisible(const Store& store, const Task& task)
{
	if (task.pending == Pending::Delete)
		return false;
	const Calendar* calendar = store.FindCalendar(task.calendarId);
	if (calendar == nullptr || !calendar->visible)
		return false;
	const Account* account = store.FindAccount(calendar->accountId);
	return account != nullptr && account->enabled;
}


static bool
TaskDueBefore(const DisplayTask& a, const DisplayTask& b)
{
	if (a.completed != b.completed)
		return !a.completed;
	if (a.hasDue != b.hasDue)
		return a.hasDue;
	if (a.hasDue && a.dueDate != b.dueDate)
		return a.dueDate < b.dueDate;
	if (a.dueHasTime != b.dueHasTime)
		return a.dueHasTime;
	if (a.dueHasTime && a.dueTime != b.dueTime)
		return a.dueTime < b.dueTime;
	if (a.priority != b.priority)
		return a.priority > b.priority;
	return a.title < b.title;
}


std::vector<DisplayTask>
Store::TasksBetween(const Date& from, const Date& to, bool includeCompleted,
	bool includeOverdue) const
{
	std::lock_guard<std::recursive_mutex> lock(fMutex);
	std::vector<DisplayTask> result;
	Date today = Today();
	for (auto& task : fTasks) {
		if (!task->hasDue || !TaskVisible(*this, *task))
			continue;
		if (task->completed && !includeCompleted)
			continue;
		bool inRange = task->dueDate >= from && task->dueDate <= to;
		bool overdue = includeOverdue && !task->completed && task->dueDate < today
			&& task->dueDate < from;
		if (inRange || overdue)
			result.push_back(MakeDisplay(*task));
	}
	std::sort(result.begin(), result.end(), TaskDueBefore);
	return result;
}


std::vector<DisplayTask>
Store::TaskList(const std::string& calendarId, bool includeCompleted) const
{
	std::lock_guard<std::recursive_mutex> lock(fMutex);
	// Children follow their parent; siblings by order key, then due date.
	std::map<std::string, std::vector<const Task*>> children;
	std::vector<const Task*> roots;
	for (auto& task : fTasks) {
		if (!calendarId.empty() && task->calendarId != calendarId)
			continue;
		if (!TaskVisible(*this, *task) && calendarId.empty())
			continue;
		if (task->pending == Pending::Delete)
			continue;
		if (task->completed && !includeCompleted)
			continue;
		const Task* parent = task->parentId.empty() ? nullptr : FindTask(task->parentId);
		if (parent != nullptr && (includeCompleted || !parent->completed)
			&& parent->pending != Pending::Delete)
			children[task->parentId].push_back(task.get());
		else
			roots.push_back(task.get());
	}
	// Lists in account order, then in each account's own order.
	std::map<std::string, size_t> accountIndex;
	for (size_t i = 0; i < fAccounts.size(); i++)
		accountIndex[fAccounts[i]->id] = i;
	auto byOrder = [this, &accountIndex](const Task* a, const Task* b) {
		if (a->calendarId != b->calendarId) {
			const Calendar* ca = FindCalendar(a->calendarId);
			const Calendar* cb = FindCalendar(b->calendarId);
			if (ca != nullptr && cb != nullptr) {
				if (ca->accountId != cb->accountId)
					return accountIndex[ca->accountId] < accountIndex[cb->accountId];
				if (ca->primary != cb->primary)
					return ca->primary;
				if (ca->order != cb->order)
					return ca->order < cb->order;
				if (ca->name != cb->name)
					return ca->name < cb->name;
			}
			return a->calendarId < b->calendarId;
		}
		if (a->completed != b->completed)
			return !a->completed;
		if (a->order != b->order) {
			// Numeric keys (Todoist child_order) sort numerically.
			if (!a->order.empty() && !b->order.empty()
				&& a->order.find_first_not_of("0123456789") == std::string::npos
				&& b->order.find_first_not_of("0123456789") == std::string::npos)
				return std::stoll(a->order) < std::stoll(b->order);
			return a->order < b->order;
		}
		if (a->created != b->created)
			return a->created < b->created;
		return a->title < b->title;
	};
	std::sort(roots.begin(), roots.end(), byOrder);
	std::vector<DisplayTask> result;
	std::function<void(const Task*, int)> add = [&](const Task* task, int depth) {
		DisplayTask display = MakeDisplay(*task);
		display.depth = depth;
		result.push_back(display);
		auto found = children.find(task->id);
		if (found == children.end() || depth > 6)
			return;
		std::vector<const Task*> list = found->second;
		std::sort(list.begin(), list.end(), byOrder);
		for (const Task* child : list)
			add(child, depth + 1);
	};
	for (const Task* root : roots)
		add(root, 0);
	return result;
}


std::vector<DisplayEvent>
Store::Search(const std::string& text, size_t limit) const
{
	std::lock_guard<std::recursive_mutex> lock(fMutex);
	std::string needle = Lower(text);
	std::vector<DisplayEvent> result;
	if (needle.empty())
		return result;
	// Search occurrences two years around today, nearest first.
	Instant now = Now();
	std::vector<DisplayEvent> all = EventsBetween(now - 365 * 86400LL,
		now + 2 * 365 * 86400LL);
	for (auto& event : all) {
		if (Lower(event.title).find(needle) != std::string::npos
			|| Lower(event.location).find(needle) != std::string::npos) {
			result.push_back(event);
		} else {
			const Event* source = FindEvent(event.overrideId.empty()
				? event.eventId : event.overrideId);
			if (source != nullptr
				&& Lower(source->notes).find(needle) != std::string::npos)
				result.push_back(event);
		}
	}
	std::stable_sort(result.begin(), result.end(),
		[now](const DisplayEvent& a, const DisplayEvent& b) {
			bool aFuture = a.end >= now, bFuture = b.end >= now;
			if (aFuture != bFuture)
				return aFuture;
			return aFuture ? a.start < b.start : a.start > b.start;
		});
	if (result.size() > limit)
		result.resize(limit);
	return result;
}


std::vector<DisplayTask>
Store::SearchTasks(const std::string& text, size_t limit) const
{
	std::lock_guard<std::recursive_mutex> lock(fMutex);
	std::string needle = Lower(text);
	std::vector<DisplayTask> result;
	if (needle.empty())
		return result;
	for (auto& task : fTasks) {
		if (!TaskVisible(*this, *task))
			continue;
		bool labelMatch = false;
		for (auto& label : task->labels) {
			if (Lower(label).find(needle) != std::string::npos)
				labelMatch = true;
		}
		if (labelMatch || Lower(task->title).find(needle) != std::string::npos
			|| Lower(task->notes).find(needle) != std::string::npos)
			result.push_back(MakeDisplay(*task));
	}
	std::sort(result.begin(), result.end(), TaskDueBefore);
	if (result.size() > limit)
		result.resize(limit);
	return result;
}


void
Store::UpdateEvent(const Event& edited)
{
	{
		std::lock_guard<std::recursive_mutex> lock(fMutex);
		Event* existing = FindEvent(edited.id);
		if (existing == nullptr) {
			Event& added = AddEvent(edited);
			added.pending = Pending::Create;
			added.revision++;
			added.updated = Now();
		} else {
			// Moving between calendars of different accounts: delete there,
			// create here.
			bool moved = existing->calendarId != edited.calendarId;
			Calendar* from = FindCalendar(existing->calendarId);
			Calendar* to = FindCalendar(edited.calendarId);
			bool crossAccount = moved && from != nullptr && to != nullptr
				&& from->accountId != to->accountId;
			if (crossAccount && !existing->remoteId.empty()) {
				Event tombstone = *existing;
				tombstone.id = NewId();
				tombstone.pending = Pending::Delete;
				tombstone.masterId.clear();
				AddEvent(tombstone);
				// The tombstone takes the server copy (and its overrides go
				// with the server copy's deletion).
				for (Event* item : OverridesOf(existing->id))
					RemoveEvent(item->id);
				existing = FindEvent(edited.id);
			}
			Pending previous = existing->pending;
			uint32_t revision = existing->revision;
			*existing = edited;
			existing->revision = revision + 1;
			existing->updated = Now();
			if (crossAccount) {
				existing->remoteId.clear();
				existing->etag.clear();
				existing->raw.clear();
				existing->pending = Pending::Create;
			} else if (previous == Pending::Create || existing->remoteId.empty())
				existing->pending = Pending::Create;
			else
				existing->pending = Pending::Update;
			if (moved && !crossAccount && previous != Pending::Create) {
				// Same account, other calendar: providers move it.
				existing->pending = Pending::Update;
			}
		}
		fDirty = true;
	}
	Changed();
}


void
Store::DeleteEvent(const std::string& idReference)
{
	// A copy: the reference may point into the item being removed.
	const std::string id = idReference;
	{
		std::lock_guard<std::recursive_mutex> lock(fMutex);
		Event* event = FindEvent(id);
		if (event == nullptr)
			return;
		Calendar* calendar = FindCalendar(event->calendarId);
		Account* account = calendar != nullptr ? FindAccount(calendar->accountId)
			: nullptr;
		bool local = account == nullptr || account->type == AccountType::Local;
		if (local || event->remoteId.empty()) {
			if (event->IsOverride() && !local) {
				// An unsynced override of a synced master: becomes an
				// exclusion on the master instead.
				Event* master = FindEvent(event->masterId);
				if (master != nullptr) {
					if (master->allDay)
						master->exdateDays.push_back(event->recurrenceDate);
					else
						master->exdates.push_back(event->recurrenceId);
					master->pending = master->pending == Pending::Create
						? Pending::Create : Pending::Update;
					master->revision++;
				}
			}
			RemoveEvent(id);
		} else {
			event->pending = Pending::Delete;
			event->revision++;
		}
		fDirty = true;
	}
	Changed();
}


void
Store::UpdateTask(const Task& edited)
{
	{
		std::lock_guard<std::recursive_mutex> lock(fMutex);
		Task* existing = FindTask(edited.id);
		if (existing == nullptr) {
			Task& added = AddTask(edited);
			added.pending = Pending::Create;
			added.revision++;
			added.updated = Now();
			if (added.created == 0)
				added.created = Now();
		} else {
			Calendar* from = FindCalendar(existing->calendarId);
			Calendar* to = FindCalendar(edited.calendarId);
			bool crossAccount = existing->calendarId != edited.calendarId
				&& from != nullptr && to != nullptr
				&& from->accountId != to->accountId;
			if (crossAccount && !existing->remoteId.empty()) {
				Task tombstone = *existing;
				tombstone.id = NewId();
				tombstone.pending = Pending::Delete;
				AddTask(tombstone);
				existing = FindTask(edited.id);
			}
			Pending previous = existing->pending;
			uint32_t revision = existing->revision;
			*existing = edited;
			existing->revision = revision + 1;
			existing->updated = Now();
			if (crossAccount) {
				existing->remoteId.clear();
				existing->etag.clear();
				existing->raw.clear();
				existing->parentId.clear();
				existing->pending = Pending::Create;
			} else if (previous == Pending::Create || existing->remoteId.empty())
				existing->pending = Pending::Create;
			else
				existing->pending = Pending::Update;
		}
		fDirty = true;
	}
	Changed();
}


void
Store::DeleteTask(const std::string& idReference)
{
	// A copy: the reference may point into the item being removed.
	const std::string id = idReference;
	{
		std::lock_guard<std::recursive_mutex> lock(fMutex);
		Task* task = FindTask(id);
		if (task == nullptr)
			return;
		// Subtasks go with their parent.
		std::vector<std::string> children;
		for (auto& item : fTasks) {
			if (item->parentId == id)
				children.push_back(item->id);
		}
		for (auto& child : children)
			DeleteTask(child);
		task = FindTask(id);
		if (task == nullptr)
			return;
		Calendar* calendar = FindCalendar(task->calendarId);
		Account* account = calendar != nullptr ? FindAccount(calendar->accountId)
			: nullptr;
		if (account == nullptr || account->type == AccountType::Local
			|| task->remoteId.empty())
			RemoveTask(id);
		else {
			task->pending = Pending::Delete;
			task->revision++;
		}
		fDirty = true;
	}
	Changed();
}


void
Store::SetTaskCompleted(const std::string& idReference, bool completed)
{
	// A copy: the reference may point into the item being removed.
	const std::string id = idReference;
	std::lock_guard<std::recursive_mutex> lock(fMutex);
	Task* task = FindTask(id);
	if (task == nullptr || task->completed == completed)
		return;
	Task edited = *task;
	edited.completed = completed;
	edited.completedAt = completed ? Now() : 0;
	// A local recurring task moves to its next date instead.
	Calendar* calendar = FindCalendar(task->calendarId);
	Account* account = calendar != nullptr ? FindAccount(calendar->accountId)
		: nullptr;
	if (completed && account != nullptr && account->type == AccountType::Local
		&& !task->rrule.empty() && task->hasDue) {
		Event probe;
		probe.allDay = true;
		probe.startDate = task->dueDate;
		probe.endDate = task->dueDate.AddDays(1);
		probe.rrule = task->rrule;
		Instant from = StartOfDay(task->dueDate.AddDays(1));
		std::vector<Instant> next = RuleInstances(probe, from,
			from + 400 * 86400LL * 5, 1);
		if (!next.empty()) {
			edited.completed = false;
			edited.completedAt = 0;
			Date nextDate = LocalDate(next[0]);
			if (edited.dueHasTime) {
				DateTime wall = ToWall(edited.dueTime);
				wall.date = nextDate;
				edited.dueTime = ToInstant(wall);
			}
			edited.dueDate = nextDate;
		}
	}
	UpdateTask(edited);
}

}	// namespace aircal
