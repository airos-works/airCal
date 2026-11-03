// End-to-end sync tests against tools/fake-cloud.py (Google, Todoist) and
// Radicale (tools/caldav-server.sh). Run with `make integration-host`; both
// servers must be running:
//   tools/fake-cloud.py --port 18480 --seed &
//   tools/caldav-server.sh --daemon --seed
#include "PlainHttp.h"
#include "core/Editing.h"
#include "core/Recurrence.h"
#include "core/Store.h"
#include "sync/OAuth.h"
#include "sync/Provider.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

using namespace aircal;
using nlohmann::json;

static int sFailures = 0;
static int sChecks = 0;

#define EXPECT(condition, what) \
	do { \
		sChecks++; \
		if (!(condition)) { \
			sFailures++; \
			std::cerr << "FAIL " << __LINE__ << ": " << what << "\n"; \
		} else \
			std::cerr << "ok   " << what << "\n"; \
	} while (0)


class MemorySecrets : public Secrets {
public:
	bool Get(const std::string& key, std::string& value) override
	{
		auto found = fValues.find(key);
		if (found == fValues.end())
			return false;
		value = found->second;
		return true;
	}
	bool Set(const std::string& key, const std::string& value) override
	{
		fValues[key] = value;
		return true;
	}
	void Remove(const std::string& key) override { fValues.erase(key); }

private:
	std::map<std::string, std::string> fValues;
};


static std::string sBase = "http://127.0.0.1:18480";
static std::string sCalDav = "http://127.0.0.1:5232/";


// Plays the browser: follows the authorization redirect to the loopback
// listener.
static void
Browser(const std::string& url)
{
	std::thread([url]() {
		PlainHttp http;
		HttpRequest request;
		request.url = url;
		HttpResponse response = http.Perform(request, nullptr);
		std::string location = response.Header("location");
		if (location.empty()) {
			std::cerr << "browser: no redirect (" << response.status << ")\n";
			return;
		}
		// localhost may be ::1 for real browsers; the test client uses v4.
		size_t host = location.find("://localhost");
		if (host != std::string::npos)
			location.replace(host, 12, "://127.0.0.1");
		usleep(100000);
		HttpRequest callback;
		callback.url = location;
		HttpResponse done = http.Perform(callback, nullptr);
		if (done.status != 200)
			std::cerr << "browser: callback answered " << done.status << "\n";
	}).detach();
}


static void
Sync(Store& store, PlainHttp& http, Secrets& secrets, const std::string& account,
	bool full = false)
{
	std::atomic<bool> cancelled(false);
	SyncContext context(store, http, secrets, cancelled);
	context.full = full;
	AccountType type;
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		type = store.FindAccount(account)->type;
	}
	try {
		CreateProvider(type)->Sync(account, context);
	} catch (const std::exception& error) {
		std::cerr << "sync error: " << error.what() << "\n";
		sFailures++;
	}
}


static json
AdminState(PlainHttp& http)
{
	HttpRequest request;
	request.url = sBase + "/admin/state";
	return json::parse(http.Perform(request, nullptr).body, nullptr, false);
}


static Calendar*
CalendarNamed(Store& store, const std::string& account, const std::string& name)
{
	for (Calendar* calendar : store.CalendarsOf(account)) {
		if (calendar->name == name)
			return calendar;
	}
	return nullptr;
}


static int
PendingCount(Store& store)
{
	int count = 0;
	for (auto& event : store.Events())
		count += event->pending != Pending::None;
	for (auto& task : store.Tasks())
		count += task->pending != Pending::None;
	return count;
}


static void
TestTodoist(PlainHttp& http, MemorySecrets& secrets, const std::string& dir)
{
	std::cerr << "\n== Todoist\n";
	Store store(dir + "/todoist");
	store.Load();
	Endpoints endpoints = Endpoints::For(sBase);
	std::atomic<bool> cancelled(false);

	// The browser flow with dynamic registration and PKCE.
	SignInResult signIn;
	try {
		signIn = TodoistSignIn(http, endpoints, Browser, cancelled);
	} catch (const std::exception& error) {
		std::cerr << "sign-in error: " << error.what() << "\n";
	}
	EXPECT(!signIn.credentials.accessToken.empty(), "OAuth sign-in returns a token");
	EXPECT(signIn.credentials.clientId.compare(0, 4, "tdd_") == 0,
		"dynamic client id " + signIn.credentials.clientId);

	// And with the personal token.
	SignInResult tokenSignIn = TodoistTokenSignIn(http, endpoints,
		" test-todoist-token\n", cancelled);
	EXPECT(tokenSignIn.credentials.apiToken == "test-todoist-token", "API token sign-in");

	Account account;
	account.type = AccountType::Todoist;
	account.name = signIn.email;
	account.server = sBase;
	std::string id = store.AddAccount(account).id;
	signIn.credentials.Save(secrets, id);
	Sync(store, http, secrets, id);

	Calendar* inbox = CalendarNamed(store, id, "Inbox");
	Calendar* work = CalendarNamed(store, id, "Work");
	Calendar* garden = CalendarNamed(store, id, "Garden");
	EXPECT(inbox != nullptr && inbox->primary, "Inbox is the primary list");
	EXPECT(work != nullptr && work->color == 0x4180ff, "Work is blue");
	EXPECT(garden != nullptr && !garden->parentId.empty(), "Garden is a sub-project");
	int active = 0, completed = 0, recurring = 0, timed = 0, subtasks = 0;
	Task* weekly = nullptr;
	for (auto& task : store.Tasks()) {
		if (task->completed)
			completed++;
		else
			active++;
		if (task->recurring) {
			recurring++;
			if (task->dueString.find("monday") != std::string::npos)
				weekly = task.get();
		}
		if (task->dueHasTime)
			timed++;
		if (!task->parentId.empty())
			subtasks++;
	}
	EXPECT(active == 13, "13 active tasks (" + std::to_string(active) + ")");
	EXPECT(completed == 2, "2 recently completed (" + std::to_string(completed) + ")");
	EXPECT(recurring >= 2, "recurring tasks");
	EXPECT(timed >= 2, "tasks with due times");
	EXPECT(subtasks >= 1, "subtasks keep their parent");
	EXPECT(weekly != nullptr && weekly->hasDue, "every monday task has a date");

	// Create, then edit, complete and delete through sync.
	Task fresh;
	fresh.calendarId = work->id;
	fresh.title = "Write the airCal tests";
	fresh.priority = 4;
	fresh.labels = {"focus"};
	fresh.hasDue = true;
	fresh.dueDate = Today().AddDays(2);
	store.UpdateTask(fresh);
	std::string freshId;
	for (auto& task : store.Tasks()) {
		if (task->title == fresh.title)
			freshId = task->id;
	}
	Sync(store, http, secrets, id);
	Task* created = store.FindTask(freshId);
	EXPECT(created != nullptr && !created->remoteId.empty()
		&& created->pending == Pending::None, "new task pushed");
	json state = AdminState(http);
	bool onServer = false;
	if (state.contains("todoist") && state["todoist"].contains("tasks")) {
		for (auto& item : state["todoist"]["tasks"].items()) {
			json task = item.value();
			if (task.value("content", "") == fresh.title) {
				onServer = task.value("priority", 0) == 4
					&& task["due"].is_object()
					&& task["due"].value("date", "") == fresh.dueDate.ToIso();
			}
		}
	}
	EXPECT(onServer, "task on the server with priority 4 and due date");

	Task edited = *created;
	edited.title = "Write the airCal integration tests";
	edited.dueHasTime = true;
	edited.dueTime = ToInstant(DateTime(edited.dueDate, 15, 30));
	store.UpdateTask(edited);
	Sync(store, http, secrets, id);
	created = store.FindTask(freshId);
	EXPECT(created != nullptr && created->title == edited.title && created->dueHasTime
		&& created->dueTime == edited.dueTime, "edit with a due time round-trips");

	// Moving to another project.
	Task moved = *store.FindTask(freshId);
	moved.calendarId = inbox->id;
	store.UpdateTask(moved);
	Sync(store, http, secrets, id);
	created = store.FindTask(freshId);
	EXPECT(created != nullptr && created->calendarId == inbox->id
		&& created->pending == Pending::None, "moved to Inbox");

	store.SetTaskCompleted(freshId, true);
	Sync(store, http, secrets, id);
	created = store.FindTask(freshId);
	EXPECT(created != nullptr && created->completed, "completion synced (kept as completed)");

	if (weekly != nullptr) {
		std::string weeklyId = weekly->id;
		Date before = weekly->dueDate;
		store.SetTaskCompleted(weeklyId, true);
		Sync(store, http, secrets, id);
		Task* after = store.FindTask(weeklyId);
		EXPECT(after != nullptr && !after->completed && after->dueDate > before,
			"recurring task moves to its next date (" + (after ? after->dueDate.ToIso()
				: std::string("gone")) + ")");
	}

	store.DeleteTask(freshId);
	Sync(store, http, secrets, id);
	EXPECT(store.FindTask(freshId) == nullptr, "deleted locally");
	state = AdminState(http);
	bool stillThere = false;
	for (auto& item : state["todoist"]["tasks"].items()) {
		if (item.value().value("content", "") == edited.title
			&& !item.value().value("is_deleted", false))
			stillThere = true;
	}
	EXPECT(!stillThere, "deleted on the server");
	EXPECT(PendingCount(store) == 0, "nothing left pending");

	// A project made in airCal, with a subtask under a task in it.
	Calendar project;
	project.accountId = id;
	project.name = "airCal project";
	project.tasks = true;
	project.events = false;
	project.pendingCreate = true;
	std::string projectId = store.AddCalendar(project).id;
	Task parentTask;
	parentTask.calendarId = projectId;
	parentTask.title = "Plan the launch";
	store.UpdateTask(parentTask);
	std::string parentId;
	for (auto& item : store.Tasks()) {
		if (item->title == parentTask.title)
			parentId = item->id;
	}
	Task child;
	child.calendarId = projectId;
	child.parentId = parentId;
	child.title = "Write the announcement";
	store.UpdateTask(child);
	Sync(store, http, secrets, id);
	Calendar* made = store.FindCalendar(projectId);
	EXPECT(made != nullptr && !made->pendingCreate && !made->remoteId.empty(),
		"new project created on Todoist");
	bool childLinked = false;
	for (auto& item : store.Tasks()) {
		if (item->title == child.title)
			childLinked = item->parentId == parentId && item->pending == Pending::None;
	}
	EXPECT(childLinked, "subtask created under its parent");

	// Refresh: expire tokens, sync again.
	HttpRequest expire;
	expire.method = "POST";
	expire.url = sBase + "/admin/expire-tokens";
	http.Perform(expire, nullptr);
	Credentials credentials;
	Credentials::Load(secrets, id, credentials);
	std::string oldToken = credentials.accessToken;
	Sync(store, http, secrets, id);
	Credentials::Load(secrets, id, credentials);
	EXPECT(credentials.accessToken != oldToken, "expired token refreshed");
	store.Save();
}


static void
TestGoogle(PlainHttp& http, MemorySecrets& secrets, const std::string& dir)
{
	std::cerr << "\n== Google\n";
	Store store(dir + "/google");
	store.Load();
	Endpoints endpoints = Endpoints::For(sBase);
	std::atomic<bool> cancelled(false);
	SignInResult signIn;
	try {
		signIn = GoogleSignIn(http, endpoints, "test-client.apps.googleusercontent.com",
			"test-secret", Browser, cancelled);
	} catch (const std::exception& error) {
		std::cerr << "sign-in error: " << error.what() << "\n";
	}
	EXPECT(!signIn.credentials.refreshToken.empty(), "Google sign-in with PKCE");
	EXPECT(signIn.email == "tester@example.com", "email " + signIn.email);

	Account account;
	account.type = AccountType::Google;
	account.name = signIn.email;
	account.server = sBase;
	std::string id = store.AddAccount(account).id;
	signIn.credentials.Save(secrets, id);
	Sync(store, http, secrets, id);

	Calendar* primary = CalendarNamed(store, id, "tester@example.com");
	Calendar* work = CalendarNamed(store, id, "Work");
	int lists = 0, holidaysReadOnly = 0;
	for (Calendar* calendar : store.CalendarsOf(id)) {
		if (calendar->tasks)
			lists++;
		if (calendar->readOnly)
			holidaysReadOnly++;
	}
	EXPECT(primary != nullptr && primary->primary, "primary calendar");
	EXPECT(work != nullptr, "Work calendar");
	EXPECT(lists == 2, "two task lists");
	EXPECT(holidaysReadOnly == 1, "holidays read-only");
	EXPECT(primary != nullptr && !primary->syncToken.empty(), "sync token stored");

	// The weekly "Team sync": this week's moved to 14:00, next week's cancelled.
	Date today = Today();
	Date weekStart = today.AddDays(-today.Weekday());
	std::vector<DisplayEvent> week = store.EventsBetween(StartOfDay(weekStart),
		StartOfDay(weekStart.AddDays(14)));
	int teamSync = 0, moved = 0;
	for (auto& event : week) {
		if (event.title.find("Team sync") != std::string::npos) {
			teamSync++;
			if (!event.overrideId.empty() && ToWall(event.start).hour == 14)
				moved++;
		}
	}
	EXPECT(teamSync == 1, "team sync: one shown in two weeks (one moved, one cancelled), got "
		+ std::to_string(teamSync));
	EXPECT(moved == 1, "the moved instance is at 14:00");
	int total = (int)store.EventsBetween(StartOfDay(weekStart),
		StartOfDay(weekStart.AddDays(7))).size();
	EXPECT(total >= 10, "busy week: " + std::to_string(total) + " occurrences");

	// Create a weekly event, change one instance, delete another.
	Event event;
	event.calendarId = work->id;
	event.title = "airCal review";
	event.location = "Room 4";
	event.start = ToInstant(DateTime(today.AddDays(1), 10, 0));
	event.end = event.start + 3600;
	event.rrule = "FREQ=WEEKLY;COUNT=4";
	event.alarms = {10};
	OccurrenceRef none;
	std::string eventId = SaveEvent(store, none, event, EditScope::All);
	Sync(store, http, secrets, id);
	Event* master = store.FindEvent(eventId);
	EXPECT(master != nullptr && !master->remoteId.empty() && master->pending == Pending::None,
		"recurring event created on Google");

	OccurrenceRef second;
	second.eventId = eventId;
	second.originalStart = event.start + 7 * 86400;
	Event changed;
	ResolveOccurrence(store, second, changed);
	changed.title = "airCal review (moved)";
	changed.start += 3600;
	changed.end += 3600;
	SaveEvent(store, second, changed, EditScope::This);
	OccurrenceRef third = second;
	third.originalStart = event.start + 14 * 86400;
	DeleteOccurrence(store, third, EditScope::This);
	Sync(store, http, secrets, id);
	EXPECT(PendingCount(store) == 0, "instance edit and delete pushed");
	// A fresh store sees the same thing.
	Store check(dir + "/google-check");
	check.Load();
	Account copy = *store.FindAccount(id);
	check.AddAccount(copy);
	Sync(check, http, secrets, id);
	int seen = 0, seenMoved = 0;
	for (auto& item : check.EventsBetween(event.start - 3600, event.start + 30 * 86400)) {
		if (item.title.find("airCal review") == std::string::npos)
			continue;
		seen++;
		if (item.title == "airCal review (moved)" && ToWall(item.start).hour == 11)
			seenMoved++;
	}
	EXPECT(seen == 3, "4 occurrences minus the deleted one: " + std::to_string(seen));
	EXPECT(seenMoved == 1, "the moved one at 11:00");

	// Incremental: change on the "server" side through a second client.
	Event* other = nullptr;
	for (auto& item : check.Events()) {
		if (item->title == "airCal review" && item->IsRecurring())
			other = item.get();
	}
	if (other != nullptr) {
		Event renamed = *other;
		renamed.title = "airCal design review";
		check.UpdateEvent(renamed);
		Sync(check, http, secrets, id);
	}
	Sync(store, http, secrets, id);
	EXPECT(store.FindEvent(eventId) != nullptr
		&& store.FindEvent(eventId)->title == "airCal design review",
		"incremental sync picks up a remote rename");

	// "All events" delete.
	OccurrenceRef all;
	all.eventId = eventId;
	all.originalStart = event.start;
	DeleteOccurrence(store, all, EditScope::All);
	Sync(store, http, secrets, id);
	Sync(check, http, secrets, id);
	int left = 0;
	for (auto& item : check.Events()) {
		if (item->title.find("airCal") != std::string::npos)
			left++;
	}
	EXPECT(left == 0, "series deleted everywhere (" + std::to_string(left) + " left)");

	// Google Tasks.
	Calendar* myTasks = CalendarNamed(store, id, "My Tasks");
	EXPECT(myTasks != nullptr, "My Tasks list");
	int googleTasks = 0, googleSub = 0;
	for (Task* task : store.TasksIn(myTasks ? myTasks->id : "")) {
		googleTasks++;
		if (!task->parentId.empty())
			googleSub++;
	}
	EXPECT(googleTasks == 5, "5 tasks in My Tasks: " + std::to_string(googleTasks));
	EXPECT(googleSub == 1, "one subtask");
	Task task;
	task.calendarId = myTasks->id;
	task.title = "Buy calendar refills";
	task.hasDue = true;
	task.dueDate = today.AddDays(3);
	store.UpdateTask(task);
	Sync(store, http, secrets, id);
	std::string taskId;
	for (auto& item : store.Tasks()) {
		if (item->title == task.title)
			taskId = item->id;
	}
	EXPECT(!taskId.empty() && !store.FindTask(taskId)->remoteId.empty(), "task inserted");
	store.SetTaskCompleted(taskId, true);
	Sync(store, http, secrets, id);
	Sync(check, http, secrets, id);
	bool doneThere = false;
	for (auto& item : check.Tasks()) {
		if (item->title == task.title)
			doneThere = item->completed && item->dueDate == task.dueDate;
	}
	EXPECT(doneThere, "completed task with its due date on Google");

	Calendar list;
	list.accountId = id;
	list.name = "Packing";
	list.tasks = true;
	list.events = false;
	list.pendingCreate = true;
	std::string listId = store.AddCalendar(list).id;
	Task item;
	item.calendarId = listId;
	item.title = "Passport";
	store.UpdateTask(item);
	Sync(store, http, secrets, id);
	Calendar* packing = store.FindCalendar(listId);
	EXPECT(packing != nullptr && !packing->pendingCreate
		&& packing->remoteId.compare(0, 6, "tasks/") == 0, "new Google task list");
	Sync(check, http, secrets, id);
	bool listThere = false;
	for (auto& calendar : check.Calendars()) {
		if (calendar->name == "Packing")
			listThere = !check.TasksIn(calendar->id).empty();
	}
	EXPECT(listThere, "list and its task seen by another client");
	store.Save();
}


static void
TestCalDav(PlainHttp& http, MemorySecrets& secrets, const std::string& dir)
{
	std::cerr << "\n== CalDAV\n";
	Store store(dir + "/caldav");
	store.Load();
	Account account;
	account.type = AccountType::CalDAV;
	account.name = "tester@radicale";
	account.user = "tester";
	account.server = sCalDav;
	std::string id = store.AddAccount(account).id;
	Credentials credentials;
	credentials.password = "secret";
	credentials.Save(secrets, id);
	Sync(store, http, secrets, id);
	EXPECT(!store.FindAccount(id)->principal.empty(),
		"discovered home " + store.FindAccount(id)->principal);
	Calendar* personal = CalendarNamed(store, id, "Personal");
	Calendar* shared = CalendarNamed(store, id, "Shared");
	EXPECT(personal != nullptr && personal->events && personal->tasks
		&& personal->color == 0x34c759, "Personal: events + tasks, green");
	EXPECT(shared != nullptr && shared->events && !shared->tasks, "Shared: events only");
	int events = 0, tasks = 0, completedTasks = 0;
	for (auto& event : store.Events())
		events += event->calendarId == (shared ? shared->id : "") ? 1 : 0;
	for (auto& task : store.Tasks()) {
		tasks++;
		completedTasks += task->completed;
	}
	EXPECT(events >= 3, "Shared events (incl. override): " + std::to_string(events));
	EXPECT(tasks == 2 && completedTasks == 1, "two to-dos, one completed");

	// The weekly New York event: EXDATE last week, moved to 8pm this week.
	Date today = Today();
	std::vector<DisplayEvent> range = store.EventsBetween(StartOfDay(today.AddDays(-14)),
		StartOfDay(today.AddDays(14)));
	int weekly = 0, evening = 0;
	for (auto& item : range) {
		const Event* source = store.FindEvent(item.eventId);
		if (source == nullptr || !source->IsRecurring() || source->calendarId != shared->id)
			continue;
		weekly++;
		if (!item.overrideId.empty() && ToWall(item.start, "America/New_York").hour == 20)
			evening++;
	}
	EXPECT(weekly >= 2 && weekly <= 4, "weekly series around today: " + std::to_string(weekly));
	EXPECT(evening == 1, "override at 8pm New York time");

	// Create a recurring event, edit one occurrence, push, pull with a new store.
	Event event;
	event.calendarId = personal->id;
	event.title = "Piano lesson";
	event.tzid = "Australia/Hobart";
	event.start = ToInstant(DateTime(today.AddDays(2), 17, 0), "Australia/Hobart");
	event.end = event.start + 2700;
	event.rrule = "FREQ=WEEKLY";
	event.notes = "Bring the Chopin";
	event.alarms = {30};
	OccurrenceRef none;
	std::string eventId = SaveEvent(store, none, event, EditScope::All);
	Sync(store, http, secrets, id);
	Event* master = store.FindEvent(eventId);
	EXPECT(master != nullptr && master->remoteId.find(".ics") != std::string::npos
		&& master->pending == Pending::None, "PUT new event: " + (master ? master->remoteId : ""));
	OccurrenceRef next;
	next.eventId = eventId;
	next.originalStart = event.start + 7 * 86400;
	Event changed;
	ResolveOccurrence(store, next, changed);
	changed.location = "Town hall";
	changed.start += 1800;
	changed.end += 1800;
	SaveEvent(store, next, changed, EditScope::This);
	OccurrenceRef after = next;
	after.originalStart = event.start + 14 * 86400;
	DeleteOccurrence(store, after, EditScope::This);
	Sync(store, http, secrets, id);
	EXPECT(PendingCount(store) == 0, "override and exclusion pushed");

	Store check(dir + "/caldav-check");
	check.Load();
	Account copy = *store.FindAccount(id);
	check.AddAccount(copy);
	Sync(check, http, secrets, id);
	int lessons = 0, townHall = 0;
	for (auto& item : check.EventsBetween(event.start - 3600, event.start + 22 * 86400)) {
		if (item.title != "Piano lesson")
			continue;
		lessons++;
		if (item.location == "Town hall" && ToWall(item.start, "Australia/Hobart").minute == 30)
			townHall++;
	}
	EXPECT(lessons == 3, "4 weeks minus one excluded: " + std::to_string(lessons));
	EXPECT(townHall == 1, "moved lesson in the town hall");
	Event* copyMaster = nullptr;
	for (auto& item : check.Events()) {
		if (item->title == "Piano lesson" && item->IsRecurring())
			copyMaster = item.get();
	}
	EXPECT(copyMaster != nullptr && copyMaster->notes == "Bring the Chopin"
		&& copyMaster->alarms.size() == 1 && copyMaster->alarms[0] == 30
		&& copyMaster->tzid == "Australia/Hobart", "notes, alarm and zone round-trip");

	// "Future": end the series after the second lesson.
	OccurrenceRef third = next;
	third.originalStart = event.start + 21 * 86400;
	DeleteOccurrence(store, third, EditScope::Future);
	Sync(store, http, secrets, id);
	Sync(check, http, secrets, id);
	int remaining = 0;
	for (auto& item : check.EventsBetween(event.start - 3600, event.start + 60 * 86400)) {
		if (item.title == "Piano lesson")
			remaining++;
	}
	EXPECT(remaining == 2, "series truncated: " + std::to_string(remaining));

	// Tasks.
	Task task;
	task.calendarId = personal->id;
	task.title = "Tune the piano";
	task.priority = 3;
	task.hasDue = true;
	task.dueDate = today.AddDays(5);
	task.labels = {"music"};
	store.UpdateTask(task);
	Sync(store, http, secrets, id);
	std::string taskId;
	for (auto& item : store.Tasks()) {
		if (item->title == task.title)
			taskId = item->id;
	}
	store.SetTaskCompleted(taskId, true);
	Sync(store, http, secrets, id);
	Sync(check, http, secrets, id);
	bool found = false;
	for (auto& item : check.Tasks()) {
		if (item->title == task.title)
			found = item->completed && item->priority == 3 && item->dueDate == task.dueDate
				&& item->labels.size() == 1;
	}
	EXPECT(found, "VTODO round-trip (completed, priority, due, category)");

	// Clean up the server copy.
	store.DeleteEvent(eventId);
	store.DeleteTask(taskId);
	Sync(store, http, secrets, id);
	Sync(check, http, secrets, id, true);
	int leftovers = 0;
	for (auto& item : check.Events())
		leftovers += item->title == "Piano lesson";
	for (auto& item : check.Tasks())
		leftovers += item->title == task.title;
	EXPECT(leftovers == 0, "deleted on the server");
	store.Save();
}


int
main(int argc, char** argv)
{
	SetLocalZone(getenv("AIRCAL_TEST_ZONE") != nullptr ? getenv("AIRCAL_TEST_ZONE")
		: "Australia/Hobart");
	if (getenv("AIRCAL_CLOUD_BASE") != nullptr)
		sBase = getenv("AIRCAL_CLOUD_BASE");
	std::string dir = "/tmp/claude-1000/aircal-integration-" + std::to_string(getpid());
	if (getenv("TMPDIR") != nullptr)
		dir = std::string(getenv("TMPDIR")) + "/aircal-integration-" + std::to_string(getpid());
	mkdir(dir.c_str(), 0700);
	PlainHttp http;
	HttpRequest reset;
	reset.method = "POST";
	reset.url = sBase + "/admin/reset?auth=1";
	if (http.Perform(reset, nullptr).status != 200) {
		std::cerr << "fake-cloud is not running at " << sBase << "\n";
		return 2;
	}
	MemorySecrets secrets;
	std::string only = argc > 1 ? argv[1] : "";
	if (only.empty() || only == "todoist")
		TestTodoist(http, secrets, dir);
	if (only.empty() || only == "google")
		TestGoogle(http, secrets, dir);
	if (only.empty() || only == "caldav")
		TestCalDav(http, secrets, dir);
	std::cerr << "\n" << sChecks << " checks, " << sFailures << " failures, "
		<< http.requests << " requests\n";
	return sFailures == 0 ? 0 : 1;
}
