// Google Calendar API v3 and Google Tasks API v1.
//
// Calendars: masters and exceptions (singleEvents=false), incremental with
// sync tokens. Recurring instances that have not been changed are not on the
// server as separate events; their ids are "<master>_<start in UTC basic
// format>" ("<master>_<YYYYMMDD>" for all-day series), which Google accepts
// for PATCH and DELETE on single instances.
#include "OAuth.h"
#include "Provider.h"
#include "SyncUtil.h"
#include "core/ICal.h"

#include <nlohmann/json.hpp>

#include <cstring>
#include <set>

using nlohmann::json;

namespace aircal {

namespace {

const char* kTasksPrefix = "tasks/";


std::string
JStr(const json& j, const char* key)
{
	auto it = j.find(key);
	return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}


class GoogleProvider : public Provider {
public:
	void Sync(const std::string& accountId, SyncContext& context) override;

private:
	json Api(const std::string& method, const std::string& url,
		const json* body, bool allowMissing = false);
	void SyncCalendarList();
	void SyncTaskLists();
	void PushEvents(Calendar& calendar);
	void PullEvents(Calendar& calendar);
	void PushTasks(Calendar& list);
	void PullTasks(Calendar& list);

	Event EventFromJson(const json& j, const Calendar& calendar,
		const std::string& calendarZone);
	json EventToJson(const Event& event, bool isOverride);
	Task TaskFromJson(const json& j);
	json TaskToJson(const Task& task);
	std::string InstanceId(const Event& master, const Event& override) const;

	SyncContext* fContext = nullptr;
	std::string fAccountId;
	Credentials fCredentials;
	Endpoints fEndpoints;
	std::map<std::string, std::string> fCalendarZones;	// remote id -> zone
};


json
GoogleProvider::Api(const std::string& method, const std::string& url,
	const json* body, bool allowMissing)
{
	for (int attempt = 0; attempt < 2; attempt++) {
		fContext->CheckCancelled();
		if (RefreshIfNeeded(fContext->http, fEndpoints.googleToken, fCredentials,
				fContext->cancelled, attempt > 0))
			fCredentials.Save(fContext->secrets, fAccountId);
		HttpRequest request;
		request.method = method;
		request.url = url;
		request.bearer = fCredentials.accessToken;
		if (body != nullptr) {
			request.contentType = "application/json";
			request.body = body->dump();
		}
		HttpResponse response = fContext->http.Perform(request,
			&fContext->cancelled);
		if (response.status == 401 && attempt == 0)
			continue;
		if (allowMissing && (response.status == 404 || response.status == 410))
			return json();
		if (response.status == 410)
			throw SyncError("gone");
		if (!response.Ok())
			ThrowHttpError("Google", response);
		if (response.body.empty())
			return json::object();
		json j = json::parse(response.body, nullptr, false);
		if (j.is_discarded())
			throw SyncError("Google sent an unreadable reply.");
		return j;
	}
	throw SyncError("Google needs you to sign in again.", true);
}


void
GoogleProvider::Sync(const std::string& accountId, SyncContext& context)
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
		throw SyncError("Google needs you to sign in again.", true);

	context.Progress("Calendars");
	SyncCalendarList();
	SyncTaskLists();

	std::vector<std::string> calendars;
	{
		std::lock_guard<std::recursive_mutex> lock(context.store.Mutex());
		for (Calendar* calendar : context.store.CalendarsOf(accountId))
			calendars.push_back(calendar->id);
	}
	for (const std::string& id : calendars) {
		Calendar copy;
		{
			std::lock_guard<std::recursive_mutex> lock(context.store.Mutex());
			Calendar* calendar = context.store.FindCalendar(id);
			if (calendar == nullptr)
				continue;
			copy = *calendar;
		}
		context.Progress(copy.name);
		if (copy.tasks) {
			PushTasks(copy);
			PullTasks(copy);
		} else {
			PushEvents(copy);
			PullEvents(copy);
		}
		std::lock_guard<std::recursive_mutex> lock(context.store.Mutex());
		Calendar* calendar = context.store.FindCalendar(id);
		if (calendar != nullptr)
			calendar->syncToken = copy.syncToken;
	}
}


void
GoogleProvider::SyncCalendarList()
{
	std::vector<json> items;
	std::string pageToken;
	do {
		std::string url = fEndpoints.googleCalendar
			+ "/users/me/calendarList?maxResults=250&showHidden=true";
		if (!pageToken.empty())
			url += "&pageToken=" + UrlEncode(pageToken);
		json page = Api("GET", url, nullptr);
		if (page.contains("items") && page["items"].is_array()) {
			for (auto& item : page["items"])
				items.push_back(item);
		}
		pageToken = JStr(page, "nextPageToken");
	} while (!pageToken.empty());

	std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
	Store& store = fContext->store;
	std::set<std::string> seen;
	int order = 0;
	for (auto& item : items) {
		std::string remoteId = JStr(item, "id");
		if (remoteId.empty() || (item.contains("deleted") && item["deleted"] == true))
			continue;
		seen.insert(remoteId);
		fCalendarZones[remoteId] = JStr(item, "timeZone");
		Calendar* calendar = store.FindCalendarByRemote(fAccountId, remoteId);
		if (calendar == nullptr) {
			Calendar fresh;
			fresh.accountId = fAccountId;
			fresh.remoteId = remoteId;
			fresh.visible = !(item.contains("selected") && item["selected"] == false);
			calendar = &store.AddCalendar(fresh);
		}
		std::string name = JStr(item, "summaryOverride");
		if (name.empty())
			name = JStr(item, "summary");
		calendar->name = name;
		calendar->color = ParseHexColor(JStr(item, "backgroundColor"), 0x4285f4);
		std::string role = JStr(item, "accessRole");
		calendar->readOnly = role != "owner" && role != "writer";
		calendar->primary = item.contains("primary") && item["primary"] == true;
		calendar->events = true;
		calendar->tasks = false;
		calendar->order = order++;
		calendar->defaultAlarms.clear();
		if (item.contains("defaultReminders") && item["defaultReminders"].is_array()) {
			for (auto& reminder : item["defaultReminders"]) {
				if (reminder.contains("minutes") && reminder["minutes"].is_number()
					&& JStr(reminder, "method") != "email")
					calendar->defaultAlarms.push_back(reminder["minutes"].get<int>());
			}
		}
	}
	for (Calendar* calendar : store.CalendarsOf(fAccountId)) {
		if (!calendar->pendingCreate && !calendar->tasks && seen.count(calendar->remoteId) == 0)
			store.RemoveCalendar(calendar->id);
	}
	store.Changed();
}


void
GoogleProvider::SyncTaskLists()
{
	// Lists made in airCal first.
	std::vector<std::pair<std::string, std::string>> fresh;	// id, name
	{
		std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
		for (Calendar* list : fContext->store.CalendarsOf(fAccountId)) {
			if (list->pendingCreate && list->tasks)
				fresh.push_back({list->id, list->name});
		}
	}
	for (auto& item : fresh) {
		json body{{"title", item.second}};
		json created = Api("POST", fEndpoints.googleTasks + "/users/@me/lists", &body);
		std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
		Calendar* list = fContext->store.FindCalendar(item.first);
		if (list != nullptr && created.is_object() && !JStr(created, "id").empty()) {
			list->remoteId = kTasksPrefix + JStr(created, "id");
			list->pendingCreate = false;
		}
	}

	std::vector<json> items;
	std::string pageToken;
	do {
		std::string url = fEndpoints.googleTasks + "/users/@me/lists?maxResults=100";
		if (!pageToken.empty())
			url += "&pageToken=" + UrlEncode(pageToken);
		json page = Api("GET", url, nullptr);
		if (page.contains("items") && page["items"].is_array()) {
			for (auto& item : page["items"])
				items.push_back(item);
		}
		pageToken = JStr(page, "nextPageToken");
	} while (!pageToken.empty());

	std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
	Store& store = fContext->store;
	static const uint32_t kColors[] = {0x1a73e8, 0x0b8043, 0xf4511e, 0x8e24aa,
		0xf6bf26, 0x039be5};
	std::set<std::string> seen;
	int order = 100;
	for (auto& item : items) {
		std::string id = JStr(item, "id");
		if (id.empty())
			continue;
		std::string remoteId = kTasksPrefix + id;
		seen.insert(remoteId);
		Calendar* list = store.FindCalendarByRemote(fAccountId, remoteId);
		if (list == nullptr) {
			Calendar fresh;
			fresh.accountId = fAccountId;
			fresh.remoteId = remoteId;
			fresh.color = kColors[(order - 100) % 6];
			list = &store.AddCalendar(fresh);
		}
		list->name = JStr(item, "title");
		list->events = false;
		list->tasks = true;
		list->primary = order == 100;	// the first list is the default one
		list->order = order++;
	}
	for (Calendar* calendar : store.CalendarsOf(fAccountId)) {
		if (!calendar->pendingCreate && calendar->tasks && seen.count(calendar->remoteId) == 0)
			store.RemoveCalendar(calendar->id);
	}
	store.Changed();
}


// --- Events -------------------------------------------------------------------------

static void
ParseGoogleTime(const json& j, bool& allDay, Instant& instant, Date& date,
	std::string& zone, const std::string& fallbackZone)
{
	allDay = false;
	if (!j.is_object())
		return;
	std::string day = JStr(j, "date");
	if (!day.empty()) {
		allDay = true;
		Date::FromIso(day, date);
		return;
	}
	std::string dateTime = JStr(j, "dateTime");
	zone = JStr(j, "timeZone");
	if (zone.empty())
		zone = fallbackZone;
	std::string canonical = CanonicalZone(zone);
	ParseRfc3339(dateTime, instant, &canonical);
	date = LocalDate(instant);
}


Event
GoogleProvider::EventFromJson(const json& j, const Calendar& calendar,
	const std::string& calendarZone)
{
	Event e;
	e.calendarId = calendar.id;
	e.remoteId = JStr(j, "id");
	e.uid = JStr(j, "iCalUID");
	e.etag = JStr(j, "etag");
	e.title = JStr(j, "summary");
	e.location = JStr(j, "location");
	e.notes = JStr(j, "description");
	e.url = JStr(j, "hangoutLink");
	if (e.url.empty() && j.contains("conferenceData")
		&& j["conferenceData"].contains("entryPoints")
		&& j["conferenceData"]["entryPoints"].is_array()) {
		for (auto& entry : j["conferenceData"]["entryPoints"]) {
			if (JStr(entry, "entryPointType") == "video") {
				e.url = JStr(entry, "uri");
				break;
			}
		}
	}
	std::string status = JStr(j, "status");
	e.cancelled = status == "cancelled";
	e.tentative = status == "tentative";
	if (j.contains("organizer") && j["organizer"].is_object()) {
		std::string name = JStr(j["organizer"], "displayName");
		std::string mail = JStr(j["organizer"], "email");
		e.organizer = name.empty() ? mail : name + " <" + mail + ">";
	}
	if (j.contains("attendees") && j["attendees"].is_array()) {
		for (auto& attendee : j["attendees"]) {
			std::string name = JStr(attendee, "displayName");
			std::string mail = JStr(attendee, "email");
			e.attendees.push_back(name.empty() ? mail : name + " <" + mail + ">");
		}
	}

	std::string zone;
	Date endDate;
	Instant endInstant = 0;
	std::string endZone;
	bool endAllDay = false;
	if (j.contains("start"))
		ParseGoogleTime(j["start"], e.allDay, e.start, e.startDate, zone, calendarZone);
	if (j.contains("end"))
		ParseGoogleTime(j["end"], endAllDay, endInstant, endDate, endZone, calendarZone);
	if (e.allDay) {
		e.endDate = endAllDay && endDate > e.startDate ? endDate
			: e.startDate.AddDays(1);
	} else {
		e.end = endInstant >= e.start ? endInstant : e.start;
		e.tzid = CanonicalZone(zone);
	}

	if (j.contains("recurrence") && j["recurrence"].is_array()) {
		for (auto& line : j["recurrence"]) {
			if (!line.is_string())
				continue;
			std::string text = line.get<std::string>();
			ical::Component holder;
			if (!ical::Parse("BEGIN:VEVENT\r\n" + text + "\r\nEND:VEVENT\r\n", holder))
				continue;
			const ical::Component* component = holder.name == "VEVENT" ? &holder
				: (holder.children.empty() ? nullptr : &holder.children[0]);
			if (component == nullptr || component->properties.empty())
				continue;
			const ical::Property& property = component->properties[0];
			if (property.name == "RRULE")
				e.rrule = property.value;
			else if (property.name == "EXDATE" || property.name == "RDATE") {
				// Comma separated lists.
				size_t start = 0;
				while (start <= property.value.size()) {
					size_t comma = property.value.find(',', start);
					if (comma == std::string::npos)
						comma = property.value.size();
					ical::Property one = property;
					one.value = property.value.substr(start, comma - start);
					ical::TimeValue value = ical::ParseTime(one, nullptr);
					if (value.valid) {
						if (property.name == "RDATE")
							e.rdates.push_back(value.dateOnly
								? StartOfDay(value.date) : value.instant);
						else if (value.dateOnly)
							e.exdateDays.push_back(value.date);
						else
							e.exdates.push_back(value.instant);
					}
					start = comma + 1;
				}
			}
		}
	}

	// Exceptions: originalStartTime is the recurrence id.
	if (j.contains("recurringEventId")) {
		bool originalAllDay = false;
		Instant original = 0;
		Date originalDate;
		std::string originalZone;
		if (j.contains("originalStartTime"))
			ParseGoogleTime(j["originalStartTime"], originalAllDay, original,
				originalDate, originalZone, calendarZone);
		e.recurrenceId = original;
		e.recurrenceDate = originalDate;
	}

	if (j.contains("reminders") && j["reminders"].is_object()) {
		const json& reminders = j["reminders"];
		if (reminders.contains("useDefault") && reminders["useDefault"] == true)
			e.alarms = calendar.defaultAlarms;
		if (reminders.contains("overrides") && reminders["overrides"].is_array()) {
			for (auto& reminder : reminders["overrides"]) {
				if (reminder.contains("minutes") && reminder["minutes"].is_number()
					&& JStr(reminder, "method") != "email")
					e.alarms.push_back(reminder["minutes"].get<int>());
			}
		}
	}
	Instant updated = 0;
	if (ParseRfc3339(JStr(j, "updated"), updated))
		e.updated = updated;
	json raw = j;
	raw["_calendar"] = calendar.remoteId;
	e.raw = raw.dump();
	return e;
}


static json
GoogleTime(bool allDay, Instant instant, const Date& date, const std::string& zone)
{
	if (allDay)
		return json{{"date", date.ToIso()}};
	std::string tz = zone.empty() || zone == "UTC" ? LocalZone() : zone;
	if (tz.empty())
		tz = "UTC";
	return json{{"dateTime", FormatRfc3339(instant, tz)}, {"timeZone", tz}};
}


json
GoogleProvider::EventToJson(const Event& e, bool isOverride)
{
	json j;
	j["summary"] = e.title;
	j["location"] = e.location;
	j["description"] = e.notes;
	j["start"] = GoogleTime(e.allDay, e.start, e.startDate, e.tzid);
	j["end"] = GoogleTime(e.allDay, e.end, e.endDate, e.tzid);
	if (e.allDay && j["start"].contains("date"))
		j["end"] = json{{"date", (e.endDate > e.startDate ? e.endDate
			: e.startDate.AddDays(1)).ToIso()}};
	j["status"] = e.tentative ? "tentative" : "confirmed";
	if (!isOverride) {
		json recurrence = json::array();
		if (!e.rrule.empty())
			recurrence.push_back("RRULE:" + e.rrule);
		std::string zone = e.tzid.empty() || e.tzid == "UTC" ? LocalZone() : e.tzid;
		for (Instant exdate : e.exdates) {
			recurrence.push_back("EXDATE;TZID=" + zone + ":"
				+ FormatBasicWall(ToWall(exdate, zone)));
		}
		for (const Date& day : e.exdateDays)
			recurrence.push_back("EXDATE;VALUE=DATE:" + day.ToBasic());
		if (!e.rrule.empty() || !recurrence.empty())
			j["recurrence"] = recurrence;
		else
			j["recurrence"] = json::array();
	}
	// Invitees: known ones keep their replies, new ones are added.
	json previous = json::parse(e.raw, nullptr, false);
	std::map<std::string, json> known;
	if (previous.is_object() && previous.contains("attendees")
		&& previous["attendees"].is_array()) {
		for (auto& attendee : previous["attendees"]) {
			std::string mail = JStr(attendee, "email");
			for (char& c : mail)
				c = (char)tolower((unsigned char)c);
			known[mail] = attendee;
		}
	}
	if (!e.attendees.empty() || !known.empty()) {
		json attendees = json::array();
		for (const std::string& person : e.attendees) {
			std::string mail = person, name;
			size_t open = person.find('<'), close = person.rfind('>');
			if (open != std::string::npos && close != std::string::npos && close > open) {
				name = person.substr(0, open);
				while (!name.empty() && name.back() == ' ')
					name.pop_back();
				mail = person.substr(open + 1, close - open - 1);
			}
			for (char& c : mail)
				c = (char)tolower((unsigned char)c);
			auto found = known.find(mail);
			if (found != known.end())
				attendees.push_back(found->second);
			else {
				json attendee{{"email", mail}};
				if (!name.empty())
					attendee["displayName"] = name;
				attendees.push_back(attendee);
			}
		}
		j["attendees"] = attendees;
	}
	json overrides = json::array();
	for (int minutes : e.alarms)
		overrides.push_back(json{{"method", "popup"}, {"minutes", minutes}});
	j["reminders"] = json{{"useDefault", false}, {"overrides", overrides}};
	return j;
}


std::string
GoogleProvider::InstanceId(const Event& master, const Event& override) const
{
	if (master.allDay)
		return master.remoteId + "_" + override.recurrenceDate.ToBasic();
	return master.remoteId + "_"
		+ FormatBasicUtc(override.recurrenceId).substr(0, 15) + "Z";
}


void
GoogleProvider::PushEvents(Calendar& calendar)
{
	if (calendar.readOnly) {
		// Nothing can be written; drop local edits so they don't linger.
		std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
		for (Event* event : fContext->store.EventsIn(calendar.id)) {
			if (event->pending != Pending::None) {
				event->pending = Pending::None;
				calendar.syncToken.clear();	// pull restores the server copy
			}
		}
		return;
	}
	std::string base = fEndpoints.googleCalendar + "/calendars/"
		+ UrlEncode(calendar.remoteId) + "/events";

	// Masters and standalone events first, then overrides (they need the
	// master's id).
	for (int pass = 0; pass < 2; pass++) {
		std::vector<Event> pending;
		{
			std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
			for (Event* event : fContext->store.EventsIn(calendar.id)) {
				if (event->pending != Pending::None
					&& event->IsOverride() == (pass == 1))
					pending.push_back(*event);
			}
		}
		for (Event& event : pending) {
			fContext->CheckCancelled();
			Event master;
			if (event.IsOverride()) {
				std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
				Event* found = fContext->store.FindEvent(event.masterId);
				if (found == nullptr || found->remoteId.empty())
					continue;	// master not on the server yet
				master = *found;
			}

			if (event.pending == Pending::Delete) {
				std::string id = event.remoteId;
				if (!id.empty())
					Api("DELETE", base + "/" + UrlEncode(id), nullptr, true);
				std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
				fContext->store.RemoveEvent(event.id);
				continue;
			}

			json result;
			if (event.IsOverride()) {
				std::string id = event.remoteId.empty()
					? InstanceId(master, event) : event.remoteId;
				if (event.cancelled) {
					Api("DELETE", base + "/" + UrlEncode(id), nullptr, true);
					result = json::object();
				} else {
					json body = EventToJson(event, true);
					result = Api("PATCH", base + "/" + UrlEncode(id)
						+ (body.contains("attendees") ? "?sendUpdates=all" : ""), &body);
				}
				if (result.is_object() && result.empty())
					result["id"] = id;
			} else if (event.pending == Pending::Create || event.remoteId.empty()) {
				json body = EventToJson(event, false);
				result = Api("POST", base + (body.contains("attendees")
					? "?sendUpdates=all" : ""), &body);
			} else {
				// Moved from another calendar of this account?
				json raw = json::parse(event.raw, nullptr, false);
				std::string from = raw.is_object() ? JStr(raw, "_calendar")
					: std::string();
				if (!from.empty() && from != calendar.remoteId) {
					Api("POST", fEndpoints.googleCalendar + "/calendars/"
						+ UrlEncode(from) + "/events/" + UrlEncode(event.remoteId)
						+ "/move?destination=" + UrlEncode(calendar.remoteId),
						nullptr);
				}
				json body = EventToJson(event, false);
				result = Api("PATCH", base + "/" + UrlEncode(event.remoteId)
					+ (body.contains("attendees") ? "?sendUpdates=all" : ""), &body,
					true);
				if (result.is_null()) {
					// Deleted on the server meanwhile: create it again.
					result = Api("POST", base, &body);
				}
			}

			std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
			Event* local = fContext->store.FindEvent(event.id);
			if (local == nullptr)
				continue;
			if (result.is_object() && result.contains("id")) {
				local->remoteId = JStr(result, "id");
				std::string etag = JStr(result, "etag");
				if (!etag.empty())
					local->etag = etag;
				std::string uid = JStr(result, "iCalUID");
				if (!uid.empty())
					local->uid = uid;
				if (result.contains("start")) {
					json raw = result;
					raw["_calendar"] = calendar.remoteId;
					local->raw = raw.dump();
				}
			}
			if (local->revision == event.revision)
				local->pending = Pending::None;
			fContext->store.Changed();
		}
	}
}


void
GoogleProvider::PullEvents(Calendar& calendar)
{
	std::string zone = fCalendarZones[calendar.remoteId];
	bool full = calendar.syncToken.empty() || fContext->full;
	std::vector<json> items;
	std::string nextSyncToken;
	for (int attempt = 0; attempt < 2; attempt++) {
		items.clear();
		std::string pageToken;
		try {
			do {
				std::string url = fEndpoints.googleCalendar + "/calendars/"
					+ UrlEncode(calendar.remoteId)
					+ "/events?maxResults=2500&singleEvents=false&showDeleted=true";
				if (full) {
					url += "&timeMin=" + UrlEncode(FormatRfc3339Utc(Now()
						- SyncWindowPast()));
				} else
					url += "&syncToken=" + UrlEncode(calendar.syncToken);
				if (!pageToken.empty())
					url += "&pageToken=" + UrlEncode(pageToken);
				json page = Api("GET", url, nullptr);
				if (page.contains("items") && page["items"].is_array()) {
					for (auto& item : page["items"])
						items.push_back(item);
				}
				pageToken = JStr(page, "nextPageToken");
				if (pageToken.empty())
					nextSyncToken = JStr(page, "nextSyncToken");
			} while (!pageToken.empty());
			break;
		} catch (const SyncError& error) {
			// 410: the sync token expired; start over.
			if (std::string(error.what()) != "gone" || full)
				throw;
			full = true;
		}
	}

	std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
	Store& store = fContext->store;
	std::set<std::string> seen;
	// Masters first so exceptions find them.
	for (int pass = 0; pass < 2; pass++) {
		for (auto& item : items) {
			bool isException = item.contains("recurringEventId");
			if (isException != (pass == 1))
				continue;
			Event remote = EventFromJson(item, calendar, zone);
			if (remote.remoteId.empty())
				continue;
			seen.insert(remote.remoteId);
			if (isException) {
				Event* master = store.FindEventByRemote(calendar.id,
					JStr(item, "recurringEventId"));
				if (master == nullptr) {
					if (remote.cancelled)
						continue;
				} else
					remote.masterId = master->id;
			} else if (remote.cancelled) {
				Event* local = store.FindEventByRemote(calendar.id, remote.remoteId);
				if (local != nullptr && local->pending == Pending::None)
					store.RemoveEvent(local->id);
				continue;
			}
			UpsertRemoteEvent(store, calendar.id, remote);
		}
	}
	if (full) {
		std::vector<std::string> stale;
		for (Event* event : store.EventsIn(calendar.id)) {
			if (!event->remoteId.empty() && event->pending == Pending::None
				&& seen.count(event->remoteId) == 0)
				stale.push_back(event->id);
		}
		for (auto& id : stale) {
			Event* event = store.FindEvent(id);
			// Overrides go with their master; an override known only locally
			// as an instance id may not be in a timeMin-bounded list.
			if (event != nullptr && (event->masterId.empty()
					|| store.FindEvent(event->masterId) == nullptr
					|| seen.count(store.FindEvent(event->masterId)->remoteId) == 0))
				store.RemoveEvent(id);
		}
	}
	if (!nextSyncToken.empty())
		calendar.syncToken = nextSyncToken;
	store.Changed();
}


// --- Tasks ---------------------------------------------------------------------------

Task
GoogleProvider::TaskFromJson(const json& j)
{
	Task t;
	t.remoteId = JStr(j, "id");
	t.etag = JStr(j, "etag");
	t.title = JStr(j, "title");
	t.notes = JStr(j, "notes");
	std::string due = JStr(j, "due");
	if (!due.empty() && Date::FromIso(due.substr(0, 10), t.dueDate))
		t.hasDue = true;
	t.completed = JStr(j, "status") == "completed";
	Instant completed = 0;
	if (ParseRfc3339(JStr(j, "completed"), completed))
		t.completedAt = completed;
	t.order = JStr(j, "position");
	Instant updated = 0;
	if (ParseRfc3339(JStr(j, "updated"), updated))
		t.updated = updated;
	t.raw = j.dump();
	return t;
}


json
GoogleProvider::TaskToJson(const Task& t)
{
	json j;
	j["title"] = t.title;
	j["notes"] = t.notes;
	j["due"] = t.hasDue ? json(t.dueDate.ToIso() + "T00:00:00.000Z") : json();
	j["status"] = t.completed ? "completed" : "needsAction";
	if (t.completed)
		j["completed"] = FormatRfc3339Utc(t.completedAt != 0 ? t.completedAt : Now());
	else
		j["completed"] = nullptr;
	return j;
}


void
GoogleProvider::PushTasks(Calendar& list)
{
	std::string listId = list.remoteId.substr(strlen(kTasksPrefix));
	std::string base = fEndpoints.googleTasks + "/lists/" + UrlEncode(listId)
		+ "/tasks";
	std::vector<Task> pending;
	{
		std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
		// Parents before children.
		for (Task* task : fContext->store.TasksIn(list.id)) {
			if (task->pending != Pending::None && task->parentId.empty())
				pending.push_back(*task);
		}
		for (Task* task : fContext->store.TasksIn(list.id)) {
			if (task->pending != Pending::None && !task->parentId.empty())
				pending.push_back(*task);
		}
	}
	for (Task& task : pending) {
		fContext->CheckCancelled();
		if (task.pending == Pending::Delete) {
			if (!task.remoteId.empty()) {
				// The tombstone may belong to another list (moved tasks).
				std::string from = listId;
				json raw = json::parse(task.raw, nullptr, false);
				if (raw.is_object() && !JStr(raw, "_list").empty())
					from = JStr(raw, "_list");
				Api("DELETE", fEndpoints.googleTasks + "/lists/" + UrlEncode(from)
					+ "/tasks/" + UrlEncode(task.remoteId), nullptr, true);
			}
			std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
			fContext->store.RemoveTask(task.id);
			continue;
		}
		json body = TaskToJson(task);
		json result;
		if (task.pending == Pending::Create || task.remoteId.empty()) {
			std::string url = base;
			std::string parentRemote;
			{
				std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
				Task* parent = task.parentId.empty() ? nullptr
					: fContext->store.FindTask(task.parentId);
				if (parent != nullptr)
					parentRemote = parent->remoteId;
			}
			if (!parentRemote.empty())
				url += "?parent=" + UrlEncode(parentRemote);
			result = Api("POST", url, &body);
		} else {
			json raw = json::parse(task.raw, nullptr, false);
			std::string from = raw.is_object() ? JStr(raw, "_list") : std::string();
			if (!from.empty() && from != listId) {
				// Moved to another list of this account: Tasks has no
				// portable move between lists, so delete there, insert here.
				Api("DELETE", fEndpoints.googleTasks + "/lists/" + UrlEncode(from)
					+ "/tasks/" + UrlEncode(task.remoteId), nullptr, true);
				result = Api("POST", base, &body);
			} else {
				result = Api("PATCH", base + "/" + UrlEncode(task.remoteId), &body,
					true);
				if (result.is_null())
					result = Api("POST", base, &body);
			}
		}
		std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
		Task* local = fContext->store.FindTask(task.id);
		if (local == nullptr)
			continue;
		if (result.is_object() && result.contains("id")) {
			local->remoteId = JStr(result, "id");
			local->etag = JStr(result, "etag");
			json raw = result;
			raw["_list"] = listId;
			local->raw = raw.dump();
		}
		if (local->revision == task.revision)
			local->pending = Pending::None;
		fContext->store.Changed();
	}
}


void
GoogleProvider::PullTasks(Calendar& list)
{
	std::string listId = list.remoteId.substr(strlen(kTasksPrefix));
	std::vector<json> items;
	std::string pageToken;
	do {
		std::string url = fEndpoints.googleTasks + "/lists/" + UrlEncode(listId)
			+ "/tasks?maxResults=100&showCompleted=true&showHidden=true";
		if (!pageToken.empty())
			url += "&pageToken=" + UrlEncode(pageToken);
		json page = Api("GET", url, nullptr);
		if (page.contains("items") && page["items"].is_array()) {
			for (auto& item : page["items"])
				items.push_back(item);
		}
		pageToken = JStr(page, "nextPageToken");
	} while (!pageToken.empty());

	std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
	Store& store = fContext->store;
	std::set<std::string> seen;
	std::map<std::string, std::string> parents;	// remote child -> remote parent
	for (auto& item : items) {
		if (item.contains("deleted") && item["deleted"] == true)
			continue;
		Task remote = TaskFromJson(item);
		if (remote.remoteId.empty())
			continue;
		json raw = item;
		raw["_list"] = listId;
		remote.raw = raw.dump();
		seen.insert(remote.remoteId);
		std::string parent = JStr(item, "parent");
		if (!parent.empty())
			parents[remote.remoteId] = parent;
		UpsertRemoteTask(store, list.id, remote);
	}
	for (auto& link : parents) {
		Task* child = store.FindTaskByRemote(list.id, link.first);
		Task* parent = store.FindTaskByRemote(list.id, link.second);
		if (child != nullptr && parent != nullptr && child->pending == Pending::None)
			child->parentId = parent->id;
	}
	RemoveStaleTasks(store, list.id, seen);
	store.Changed();
}

}	// namespace


std::unique_ptr<Provider>
CreateGoogleProvider()
{
	return std::unique_ptr<Provider>(new GoogleProvider);
}

}	// namespace aircal
