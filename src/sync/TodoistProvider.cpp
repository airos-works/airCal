// Todoist API v1: projects become task lists, tasks become tasks.
#include "OAuth.h"
#include "Provider.h"
#include "SyncUtil.h"

#include <nlohmann/json.hpp>

#include <set>

using nlohmann::json;

namespace aircal {

namespace {

std::string
JStr(const json& j, const char* key)
{
	auto it = j.find(key);
	return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}


// Ids are strings in v1; be lenient with numbers.
std::string
JId(const json& j, const char* key)
{
	auto it = j.find(key);
	if (it == j.end() || it->is_null())
		return "";
	if (it->is_string())
		return it->get<std::string>();
	if (it->is_number_integer())
		return std::to_string(it->get<int64_t>());
	return "";
}


uint32_t
TodoistColor(const std::string& name)
{
	static const std::map<std::string, uint32_t> kColors = {
		{"berry_red", 0xb8255f}, {"red", 0xdc4c3e}, {"orange", 0xc77100},
		{"yellow", 0xb29104}, {"olive_green", 0x949c31}, {"lime_green", 0x65a33a},
		{"green", 0x369307}, {"mint_green", 0x42a393}, {"teal", 0x148fad},
		{"sky_blue", 0x319dc0}, {"light_blue", 0x6988a4}, {"blue", 0x4180ff},
		{"grape", 0x692ec2}, {"violet", 0xca3fee}, {"lavender", 0xa4698c},
		{"magenta", 0xe05095}, {"salmon", 0xc9766f}, {"charcoal", 0x808080},
		{"grey", 0x999999}, {"taupe", 0x8f7a69}};
	auto found = kColors.find(name);
	return found != kColors.end() ? found->second : 0x808080;
}


// Todoist 1 (normal) .. 4 (urgent, "p1") <-> airCal 0 .. 4.
int
FromTodoistPriority(int priority)
{
	switch (priority) {
		case 4: return 4;
		case 3: return 3;
		case 2: return 2;
		default: return 0;
	}
}


int
ToTodoistPriority(int priority)
{
	switch (priority) {
		case 4: return 4;
		case 3: return 3;
		case 2:
		case 1: return 2;
		default: return 1;
	}
}


class TodoistProvider : public Provider {
public:
	void Sync(const std::string& accountId, SyncContext& context) override;

private:
	json Api(const std::string& method, const std::string& path,
		const json* body, bool allowMissing = false);
	std::vector<json> Paged(const std::string& path, const char* listKey);
	void SyncProjects();
	void PushTasks();
	void PullTasks();
	Task TaskFromJson(const json& j);
	json DueFields(const Task& task, const json& previous);

	SyncContext* fContext = nullptr;
	std::string fAccountId;
	Credentials fCredentials;
	Endpoints fEndpoints;
	std::map<std::string, std::string> fSections;	// id -> name
};


json
TodoistProvider::Api(const std::string& method, const std::string& path,
	const json* body, bool allowMissing)
{
	for (int attempt = 0; attempt < 2; attempt++) {
		fContext->CheckCancelled();
		if (fCredentials.apiToken.empty()
			&& RefreshIfNeeded(fContext->http, fEndpoints.todoistToken,
				fCredentials, fContext->cancelled, attempt > 0))
			fCredentials.Save(fContext->secrets, fAccountId);
		HttpRequest request;
		request.method = method;
		request.url = fEndpoints.todoistApi + path;
		request.bearer = fCredentials.apiToken.empty() ? fCredentials.accessToken
			: fCredentials.apiToken;
		if (body != nullptr) {
			request.contentType = "application/json";
			request.body = body->dump();
		}
		if (method == "POST" && body == nullptr) {
			// close/reopen take no body.
			request.contentType = "application/json";
			request.body = "{}";
		}
		HttpResponse response = fContext->http.Perform(request,
			&fContext->cancelled);
		if ((response.status == 401 || response.status == 403) && attempt == 0
			&& fCredentials.apiToken.empty() && !fCredentials.refreshToken.empty())
			continue;
		if (allowMissing && response.status == 404)
			return json();
		if (response.status == 401 || response.status == 403)
			throw SyncError("Todoist needs you to sign in again.", true);
		if (!response.Ok())
			ThrowHttpError("Todoist", response);
		if (response.body.empty())
			return json::object();
		json j = json::parse(response.body, nullptr, false);
		if (j.is_discarded())
			return json::object();
		return j;
	}
	throw SyncError("Todoist needs you to sign in again.", true);
}


std::vector<json>
TodoistProvider::Paged(const std::string& path, const char* listKey)
{
	std::vector<json> items;
	std::string cursor;
	for (int page = 0; page < 500; page++) {
		std::string url = path + (path.find('?') == std::string::npos ? "?" : "&")
			+ "limit=200";
		if (!cursor.empty())
			url += "&cursor=" + UrlEncode(cursor);
		json result = Api("GET", url, nullptr);
		if (result.is_array()) {
			for (auto& item : result)
				items.push_back(item);
			break;
		}
		if (result.contains(listKey) && result[listKey].is_array()) {
			for (auto& item : result[listKey])
				items.push_back(item);
		}
		cursor = JStr(result, "next_cursor");
		if (cursor.empty())
			break;
	}
	return items;
}


void
TodoistProvider::Sync(const std::string& accountId, SyncContext& context)
{
	fContext = &context;
	fAccountId = accountId;
	std::string server;
	{
		std::lock_guard<std::recursive_mutex> lock(context.store.Mutex());
		Account* account = context.store.FindAccount(accountId);
		if (account == nullptr)
			return;
		server = account->server;
	}
	fEndpoints = Endpoints::For(server);
	if (!Credentials::Load(context.secrets, accountId, fCredentials))
		throw SyncError("Todoist needs you to sign in again.", true);

	context.Progress("Projects");
	SyncProjects();
	context.Progress("Tasks");
	PushTasks();
	PullTasks();
}


void
TodoistProvider::SyncProjects()
{
	std::vector<json> projects = Paged("/projects", "results");
	std::vector<json> sections = Paged("/sections", "results");
	fSections.clear();
	for (auto& section : sections)
		fSections[JId(section, "id")] = JStr(section, "name");

	std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
	Store& store = fContext->store;
	std::set<std::string> seen;
	std::map<std::string, std::string> parents;
	for (auto& project : projects) {
		std::string id = JId(project, "id");
		if (id.empty() || (project.contains("is_deleted")
				&& project["is_deleted"] == true)
			|| (project.contains("is_archived") && project["is_archived"] == true))
			continue;
		seen.insert(id);
		Calendar* list = store.FindCalendarByRemote(fAccountId, id);
		if (list == nullptr) {
			Calendar fresh;
			fresh.accountId = fAccountId;
			fresh.remoteId = id;
			list = &store.AddCalendar(fresh);
		}
		list->name = JStr(project, "name");
		list->color = TodoistColor(JStr(project, "color"));
		list->events = false;
		list->tasks = true;
		list->primary = project.contains("inbox_project")
			&& project["inbox_project"] == true;
		auto order = project.find("child_order");
		list->order = order != project.end() && order->is_number()
			? order->get<int>() : 0;
		if (list->primary)
			list->order = -1;
		std::string parent = JId(project, "parent_id");
		if (!parent.empty())
			parents[id] = parent;
		else
			list->parentId.clear();
	}
	for (auto& link : parents) {
		Calendar* child = store.FindCalendarByRemote(fAccountId, link.first);
		Calendar* parent = store.FindCalendarByRemote(fAccountId, link.second);
		if (child != nullptr && parent != nullptr) {
			child->parentId = parent->id;
			// Sub-projects sort after their parent.
			child->order = parent->order * 1000 + child->order + 1;
		}
	}
	for (Calendar* calendar : store.CalendarsOf(fAccountId)) {
		if (seen.count(calendar->remoteId) == 0)
			store.RemoveCalendar(calendar->id);
	}
	store.Changed();
}


Task
TodoistProvider::TaskFromJson(const json& j)
{
	Task t;
	t.remoteId = JId(j, "id");
	t.title = JStr(j, "content");
	t.notes = JStr(j, "description");
	auto priority = j.find("priority");
	t.priority = FromTodoistPriority(priority != j.end() && priority->is_number()
		? priority->get<int>() : 1);
	t.completed = j.contains("checked") && j["checked"] == true;
	Instant completedAt = 0;
	if (ParseRfc3339(JStr(j, "completed_at"), completedAt))
		t.completedAt = completedAt;
	if (t.completedAt != 0)
		t.completed = true;
	if (j.contains("labels") && j["labels"].is_array()) {
		for (auto& label : j["labels"]) {
			if (label.is_string())
				t.labels.push_back(label.get<std::string>());
		}
	}
	auto order = j.find("child_order");
	if (order != j.end() && order->is_number())
		t.order = std::to_string(order->get<int64_t>());
	std::string section = JId(j, "section_id");
	if (!section.empty())
		t.section = fSections[section];

	if (j.contains("due") && j["due"].is_object()) {
		const json& due = j["due"];
		std::string date = JStr(due, "date");
		t.dueString = JStr(due, "string");
		t.recurring = due.contains("is_recurring") && due["is_recurring"] == true;
		if (date.size() >= 10 && Date::FromIso(date.substr(0, 10), t.dueDate)) {
			t.hasDue = true;
			if (date.size() > 10) {
				// Floating ("2026-11-05T09:00:00", the user's local time) or
				// fixed ("...Z").
				std::string local = "";
				Instant when = 0;
				if (ParseRfc3339(date, when, &local)) {
					t.dueHasTime = true;
					t.dueTime = when;
					t.dueDate = LocalDate(when);
				}
			}
		}
		// Older replies keep the time in due.datetime.
		std::string datetime = JStr(due, "datetime");
		if (!t.dueHasTime && !datetime.empty()) {
			Instant when = 0;
			std::string local = "";
			if (ParseRfc3339(datetime, when, &local)) {
				t.dueHasTime = true;
				t.dueTime = when;
				t.dueDate = LocalDate(when);
				t.hasDue = true;
			}
		}
	}
	if (j.contains("duration") && j["duration"].is_object()) {
		auto amount = j["duration"].find("amount");
		if (amount != j["duration"].end() && amount->is_number()) {
			int value = amount->get<int>();
			t.durationMinutes = JStr(j["duration"], "unit") == "day"
				? value * 1440 : value;
		}
	}
	if (j.contains("deadline") && j["deadline"].is_object())
		t.hasDeadline = Date::FromIso(JStr(j["deadline"], "date"), t.deadline);
	Instant added = 0;
	if (ParseRfc3339(JStr(j, "added_at"), added))
		t.created = added;
	Instant updated = 0;
	if (ParseRfc3339(JStr(j, "updated_at"), updated))
		t.updated = updated;
	t.raw = j.dump();
	return t;
}


// The due fields to send for a task, compared with the server's copy.
json
TodoistProvider::DueFields(const Task& task, const json& previous)
{
	json fields = json::object();
	Task before;
	bool hadPrevious = previous.is_object();
	if (hadPrevious)
		before = TaskFromJson(previous);
	bool sameDue = hadPrevious && before.hasDue == task.hasDue
		&& (!task.hasDue || (before.dueDate == task.dueDate
			&& before.dueHasTime == task.dueHasTime
			&& (!task.dueHasTime || before.dueTime == task.dueTime)))
		&& before.dueString == task.dueString && before.recurring == task.recurring;
	if (sameDue)
		return fields;
	if (!task.hasDue) {
		if (!hadPrevious || before.hasDue)
			fields["due_string"] = "no date";
		return fields;
	}
	if (task.recurring && !task.dueString.empty()) {
		// Keep the recurrence; when the date moved, restart it there.
		// The rule starts at the date picked in airCal ("every tuesday
		// starting 2026-11-10"); Todoist would otherwise pick the next match.
		std::string text = task.dueString;
		size_t starting = text.find(" starting ");
		if (starting != std::string::npos)
			text = text.substr(0, starting);
		bool dateChanged = !hadPrevious || before.dueDate != task.dueDate;
		if (dateChanged || before.dueString != task.dueString)
			text += " starting " + task.dueDate.ToIso();
		fields["due_string"] = text;
		fields["due_lang"] = "en";
		return fields;
	}
	if (task.dueHasTime)
		fields["due_datetime"] = FormatRfc3339Utc(task.dueTime);
	else
		fields["due_date"] = task.dueDate.ToIso();
	return fields;
}


void
TodoistProvider::PushTasks()
{
	std::vector<Task> pending;
	std::map<std::string, std::string> projectOf;	// list id -> project id
	{
		std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
		for (Calendar* list : fContext->store.CalendarsOf(fAccountId)) {
			projectOf[list->id] = list->remoteId;
			for (Task* task : fContext->store.TasksIn(list->id)) {
				if (task->pending != Pending::None)
					pending.push_back(*task);
			}
		}
	}
	// Parents first.
	std::stable_sort(pending.begin(), pending.end(), [](const Task& a, const Task& b) {
		return a.parentId.empty() && !b.parentId.empty();
	});

	for (Task& task : pending) {
		fContext->CheckCancelled();
		if (task.pending == Pending::Delete) {
			if (!task.remoteId.empty())
				Api("DELETE", "/tasks/" + UrlEncode(task.remoteId), nullptr, true);
			std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
			fContext->store.RemoveTask(task.id);
			continue;
		}
		json previous = json::parse(task.raw, nullptr, false);
		std::string project = projectOf[task.calendarId];
		std::string parentRemote;
		{
			std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
			Task* parent = task.parentId.empty() ? nullptr
				: fContext->store.FindTask(task.parentId);
			if (parent != nullptr)
				parentRemote = parent->remoteId;
		}

		json result;
		json labels = task.labels;
		if (task.pending == Pending::Create || task.remoteId.empty()) {
			json body{{"content", task.title}, {"description", task.notes},
				{"priority", ToTodoistPriority(task.priority)}, {"labels", labels}};
			if (!project.empty())
				body["project_id"] = project;
			if (!parentRemote.empty())
				body["parent_id"] = parentRemote;
			json due = DueFields(task, json());
			if (due.contains("due_string") && due["due_string"] == "no date")
				due.erase("due_string");
			body.update(due);
			result = Api("POST", "/tasks", &body);
			if (task.completed && result.is_object() && result.contains("id"))
				Api("POST", "/tasks/" + UrlEncode(JId(result, "id")) + "/close",
					nullptr, true);
		} else {
			std::string remote = UrlEncode(task.remoteId);
			Task before = previous.is_object() ? TaskFromJson(previous) : Task();
			json body{{"content", task.title}, {"description", task.notes},
				{"priority", ToTodoistPriority(task.priority)}, {"labels", labels}};
			body.update(DueFields(task, previous));
			result = Api("POST", "/tasks/" + remote, &body, true);
			if (result.is_null()) {
				// Gone on the server (deleted or completed elsewhere).
				std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
				fContext->store.RemoveTask(task.id);
				continue;
			}
			std::string oldProject = previous.is_object()
				? JId(previous, "project_id") : std::string();
			std::string oldParent = previous.is_object()
				? JId(previous, "parent_id") : std::string();
			if (!project.empty() && project != oldProject) {
				json move{{"project_id", project}};
				Api("POST", "/tasks/" + remote + "/move", &move, true);
			} else if (parentRemote != oldParent && !parentRemote.empty()) {
				json move{{"parent_id", parentRemote}};
				Api("POST", "/tasks/" + remote + "/move", &move, true);
			}
			if (task.completed != before.completed) {
				Api("POST", "/tasks/" + remote + (task.completed ? "/close"
					: "/reopen"), nullptr, true);
				// Closing a recurring task moves it to its next date.
				json fresh = Api("GET", "/tasks/" + remote, nullptr, true);
				if (fresh.is_object() && fresh.contains("id"))
					result = fresh;
				else if (task.completed)
					result["checked"] = true;
			}
		}

		std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
		Task* local = fContext->store.FindTask(task.id);
		if (local == nullptr)
			continue;
		if (result.is_object() && result.contains("id")) {
			local->remoteId = JId(result, "id");
			local->raw = result.dump();
			if (local->revision == task.revision) {
				Task fresh = TaskFromJson(result);
				// Recurring tasks come back reopened at their next date.
				local->hasDue = fresh.hasDue;
				local->dueDate = fresh.dueDate;
				local->dueHasTime = fresh.dueHasTime;
				local->dueTime = fresh.dueTime;
				local->dueString = fresh.dueString;
				local->recurring = fresh.recurring;
				if (fresh.recurring)
					local->completed = fresh.completed;
			}
		}
		if (local->revision == task.revision)
			local->pending = Pending::None;
		fContext->store.Changed();
	}
}


void
TodoistProvider::PullTasks()
{
	std::vector<json> active = Paged("/tasks", "results");
	// Recently completed tasks, so ticking one off keeps it visible.
	std::vector<json> completed;
	try {
		Instant now = Now();
		completed = Paged("/tasks/completed/by_completion_date?since="
			+ UrlEncode(FormatRfc3339Utc(now - 14 * 86400LL)) + "&until="
			+ UrlEncode(FormatRfc3339Utc(now + 86400)), "items");
	} catch (const SyncError& error) {
		if (error.authentication)
			throw;
		// Optional; plans without completed-task access fail here.
	}

	std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
	Store& store = fContext->store;
	std::map<std::string, std::string> listOf;	// project id -> list id
	for (Calendar* list : store.CalendarsOf(fAccountId))
		listOf[list->remoteId] = list->id;

	std::map<std::string, std::set<std::string>> seen;	// list id -> remote ids
	std::map<std::string, std::pair<std::string, std::string>> parents;
	auto take = [&](const json& item, bool done) {
		std::string project = JId(item, "project_id");
		auto list = listOf.find(project);
		if (list == listOf.end())
			return;
		Task remote = TaskFromJson(item);
		if (remote.remoteId.empty())
			return;
		if (done)
			remote.completed = true;
		if (seen[list->second].count(remote.remoteId) != 0)
			return;	// active copy wins over a completed one
		seen[list->second].insert(remote.remoteId);
		// Moved between projects on the server: drop the old local copy.
		for (auto& other : listOf) {
			if (other.second == list->second)
				continue;
			Task* old = store.FindTaskByRemote(other.second, remote.remoteId);
			if (old != nullptr && old->pending == Pending::None)
				store.RemoveTask(old->id);
		}
		UpsertRemoteTask(store, list->second, remote);
		std::string parent = JId(item, "parent_id");
		parents[remote.remoteId] = {list->second, parent};
	};
	for (auto& item : active)
		take(item, false);
	for (auto& item : completed)
		take(item, true);
	for (auto& link : parents) {
		Task* child = store.FindTaskByRemote(link.second.first, link.first);
		if (child == nullptr || child->pending != Pending::None)
			continue;
		child->parentId.clear();
		if (link.second.second.empty())
			continue;
		for (auto& list : listOf) {
			Task* parent = store.FindTaskByRemote(list.second, link.second.second);
			if (parent != nullptr) {
				child->parentId = parent->id;
				break;
			}
		}
	}
	for (auto& list : listOf)
		RemoveStaleTasks(store, list.second, seen[list.second]);
	store.Changed();
}

}	// namespace


std::unique_ptr<Provider>
CreateTodoistProvider()
{
	return std::unique_ptr<Provider>(new TodoistProvider);
}

}	// namespace aircal
