// CalDAV (RFC 4791): discovery through current-user-principal and
// calendar-home-set, calendars by PROPFIND, objects by calendar-query
// REPORTs, writes by PUT/DELETE with ETag preconditions. Events and VTODO
// tasks; one iCalendar resource holds a series' master and its overrides.
#include "Provider.h"
#include "SyncUtil.h"
#include "core/ICal.h"
#include "core/Xml.h"

#include <nlohmann/json.hpp>

#include <set>

using nlohmann::json;

namespace aircal {

namespace {

using xml::kApple;
using xml::kCalDav;
using xml::kCalServer;
using xml::kDav;

const char* kXmlHead = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";


struct DavResponse {
	std::string href;
	const xml::Element* prop = nullptr;	// the 200 propstat's prop
	std::string status;
};


// The responses of a multistatus; their props stay owned by `root`.
std::vector<DavResponse>
Responses(const xml::Element* root)
{
	std::vector<DavResponse> result;
	if (root == nullptr)
		return result;
	for (const xml::Element* response : root->Children(kDav, "response")) {
		DavResponse item;
		item.href = response->ChildText(kDav, "href");
		item.status = response->ChildText(kDav, "status");
		for (const xml::Element* propstat : response->Children(kDav, "propstat")) {
			std::string status = propstat->ChildText(kDav, "status");
			if (status.find(" 200") != std::string::npos || status.empty()) {
				item.prop = propstat->Child(kDav, "prop");
				break;
			}
		}
		result.push_back(item);
	}
	return result;
}


std::string
HrefText(const xml::Element* prop, const char* ns, const char* name)
{
	if (prop == nullptr)
		return "";
	const xml::Element* element = prop->Child(ns, name);
	if (element == nullptr)
		return "";
	return element->ChildText(kDav, "href");
}


bool
SameCollection(const std::string& a, const std::string& b)
{
	auto normal = [](std::string path) {
		path = UrlDecode(UrlPath(path.find("://") == std::string::npos
			? "http://x" + path : path));
		if (path.empty() || path.back() != '/')
			path += '/';
		return path;
	};
	return normal(a) == normal(b);
}


class CalDavProvider : public Provider {
public:
	void Sync(const std::string& accountId, SyncContext& context) override;

private:
	HttpResponse Dav(const std::string& method, const std::string& url,
		const std::string& body, const std::string& depth,
		const std::vector<std::pair<std::string, std::string>>& extra = {});
	std::unique_ptr<xml::Element> Multistatus(const std::string& method,
		const std::string& url, const std::string& body, const std::string& depth,
		bool allowFailure = false);
	std::string Discover();
	void SyncCalendars();
	void PushEvents(const Calendar& calendar);
	void PullEvents(Calendar& calendar);
	void PushTasks(const Calendar& calendar);
	void PullTasks(Calendar& calendar);
	std::string Url(const std::string& href) const
		{ return ResolveUrl(fHome, href); }

	SyncContext* fContext = nullptr;
	std::string fAccountId;
	std::string fUser;
	std::string fServer;
	std::string fHome;
	Credentials fCredentials;
	bool fSingle = false;		// the URL is a calendar itself
};


HttpResponse
CalDavProvider::Dav(const std::string& method, const std::string& url,
	const std::string& body, const std::string& depth,
	const std::vector<std::pair<std::string, std::string>>& extra)
{
	fContext->CheckCancelled();
	HttpRequest request;
	request.method = method;
	request.url = url;
	request.user = fUser;
	request.password = fCredentials.password;
	request.followRedirects = true;
	request.body = body;
	if (!body.empty()) {
		request.contentType = method == "PUT" ? "text/calendar; charset=utf-8"
			: "application/xml; charset=utf-8";
	}
	if (!depth.empty())
		request.headers.push_back({"Depth", depth});
	for (auto& header : extra)
		request.headers.push_back(header);
	HttpResponse response = fContext->http.Perform(request, &fContext->cancelled);
	if (response.status == 401)
		throw SyncError("The CalDAV server did not accept the user name or "
			"password.", true);
	return response;
}


std::unique_ptr<xml::Element>
CalDavProvider::Multistatus(const std::string& method, const std::string& url,
	const std::string& body, const std::string& depth, bool allowFailure)
{
	HttpResponse response = Dav(method, url, body, depth);
	if (response.status != 207) {
		if (allowFailure)
			return nullptr;
		ThrowHttpError("The CalDAV server", response);
	}
	std::unique_ptr<xml::Element> root = xml::Parse(response.body);
	if (!root && !allowFailure)
		throw SyncError("The CalDAV server sent an unreadable reply.");
	return root;
}


std::string
CalDavProvider::Discover()
{
	std::string base = fServer;
	if (base.find("://") == std::string::npos)
		base = "https://" + base;
	std::string probe = std::string(kXmlHead)
		+ "<d:propfind xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
		"<d:prop><d:current-user-principal/><c:calendar-home-set/>"
		"<d:resourcetype/></d:prop></d:propfind>";

	std::vector<std::string> candidates = {base};
	if (UrlPath(base) == "/" || UrlPath(base).empty())
		candidates.push_back(UrlOrigin(base) + "/.well-known/caldav");
	else
		candidates.push_back(UrlOrigin(base) + "/.well-known/caldav");

	for (const std::string& candidate : candidates) {
		HttpResponse response = Dav("PROPFIND", candidate, probe, "0");
		if (response.status != 207)
			continue;
		std::string effective = response.url.empty() ? candidate : response.url;
		std::unique_ptr<xml::Element> root = xml::Parse(response.body);
		std::vector<DavResponse> items = Responses(root.get());
		if (items.empty())
			continue;
		const xml::Element* prop = items[0].prop;
		std::string home = HrefText(prop, kCalDav, "calendar-home-set");
		if (!home.empty())
			return ResolveUrl(effective, home);
		// A calendar collection itself.
		if (prop != nullptr && prop->Child(kDav, "resourcetype") != nullptr
			&& prop->Child(kDav, "resourcetype")->Child(kCalDav, "calendar")
				!= nullptr) {
			fSingle = true;
			return effective;
		}
		std::string principal = HrefText(prop, kDav, "current-user-principal");
		if (principal.empty())
			continue;
		principal = ResolveUrl(effective, principal);
		std::string ask = std::string(kXmlHead)
			+ "<d:propfind xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
			"<d:prop><c:calendar-home-set/></d:prop></d:propfind>";
		HttpResponse homeResponse = Dav("PROPFIND", principal, ask, "0");
		if (homeResponse.status == 207) {
			std::unique_ptr<xml::Element> homeRoot = xml::Parse(homeResponse.body);
			std::vector<DavResponse> homeItems = Responses(homeRoot.get());
			if (!homeItems.empty()) {
				home = HrefText(homeItems[0].prop, kCalDav, "calendar-home-set");
				if (!home.empty())
					return ResolveUrl(homeResponse.url.empty() ? principal
						: homeResponse.url, home);
			}
		}
		// Some servers keep calendars right under the principal.
		return principal;
	}
	throw SyncError("No CalDAV calendars were found at " + fServer
		+ ". Check the server address.");
}


void
CalDavProvider::Sync(const std::string& accountId, SyncContext& context)
{
	fContext = &context;
	fAccountId = accountId;
	{
		std::lock_guard<std::recursive_mutex> lock(context.store.Mutex());
		Account* account = context.store.FindAccount(accountId);
		if (account == nullptr)
			return;
		fUser = account->user;
		fServer = account->server;
		fHome = account->principal;
		json state = json::parse(account->syncState, nullptr, false);
		fSingle = state.is_object() && state.contains("single")
			&& state["single"] == true;
	}
	if (!Credentials::Load(context.secrets, accountId, fCredentials))
		fCredentials = Credentials();	// servers without passwords exist

	if (fHome.empty()) {
		context.Progress("Finding calendars");
		fHome = Discover();
		std::lock_guard<std::recursive_mutex> lock(context.store.Mutex());
		Account* account = context.store.FindAccount(accountId);
		if (account != nullptr) {
			account->principal = fHome;
			account->syncState = json{{"single", fSingle}}.dump();
		}
	}

	context.Progress("Calendars");
	SyncCalendars();

	std::vector<std::string> ids;
	{
		std::lock_guard<std::recursive_mutex> lock(context.store.Mutex());
		for (Calendar* calendar : context.store.CalendarsOf(accountId))
			ids.push_back(calendar->id);
	}
	for (const std::string& id : ids) {
		Calendar copy;
		std::string previousTag;
		{
			std::lock_guard<std::recursive_mutex> lock(context.store.Mutex());
			Calendar* calendar = context.store.FindCalendar(id);
			if (calendar == nullptr)
				continue;
			copy = *calendar;
		}
		context.Progress(copy.name);
		if (copy.events)
			PushEvents(copy);
		if (copy.tasks)
			PushTasks(copy);
		// syncToken holds the ctag of the last complete pull.
		bool changed = context.full || copy.syncToken.empty()
			|| copy.syncToken != copy.ctag;
		if (changed) {
			if (copy.events)
				PullEvents(copy);
			if (copy.tasks)
				PullTasks(copy);
		}
		std::lock_guard<std::recursive_mutex> lock(context.store.Mutex());
		Calendar* calendar = context.store.FindCalendar(id);
		if (calendar != nullptr && !copy.ctag.empty()) {
			// A push changes the ctag; pull again next time unless nothing
			// was written.
			calendar->syncToken = copy.syncToken;
		}
	}
}


void
CalDavProvider::SyncCalendars()
{
	std::string body = std::string(kXmlHead)
		+ "<d:propfind xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\" "
		"xmlns:cs=\"http://calendarserver.org/ns/\" "
		"xmlns:ic=\"http://apple.com/ns/ical/\"><d:prop><d:resourcetype/>"
		"<d:displayname/><cs:getctag/><d:sync-token/>"
		"<c:supported-calendar-component-set/><ic:calendar-color/>"
		"<ic:calendar-order/><d:current-user-privilege-set/></d:prop>"
		"</d:propfind>";
	std::unique_ptr<xml::Element> root = Multistatus("PROPFIND", fHome, body,
		fSingle ? "0" : "1");
	std::vector<DavResponse> items = Responses(root.get());

	static const uint32_t kColors[] = {0x34c759, 0x007aff, 0xff9500, 0xaf52de,
		0xff2d55, 0x5ac8fa};
	std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
	Store& store = fContext->store;
	std::set<std::string> seen;
	int index = 0;
	for (auto& item : items) {
		const xml::Element* prop = item.prop;
		if (prop == nullptr)
			continue;
		const xml::Element* type = prop->Child(kDav, "resourcetype");
		if (type == nullptr || type->Child(kCalDav, "calendar") == nullptr)
			continue;
		std::string href = item.href;
		bool events = true, tasks = false;
		const xml::Element* components
			= prop->Child(kCalDav, "supported-calendar-component-set");
		if (components != nullptr && !components->children.empty()) {
			events = tasks = false;
			for (const xml::Element* comp : components->Children(kCalDav, "comp")) {
				std::string name = comp->Attribute("name");
				if (name == "VEVENT")
					events = true;
				else if (name == "VTODO")
					tasks = true;
			}
		}
		if (!events && !tasks)
			continue;
		bool writable = true;
		const xml::Element* privileges
			= prop->Child(kDav, "current-user-privilege-set");
		if (privileges != nullptr && !privileges->children.empty()) {
			writable = false;
			for (const xml::Element* privilege : privileges->Children(kDav, "privilege")) {
				if (privilege->Child(kDav, "write") != nullptr
					|| privilege->Child(kDav, "write-content") != nullptr
					|| privilege->Child(kDav, "all") != nullptr)
					writable = true;
			}
		}

		Calendar* calendar = nullptr;
		for (Calendar* candidate : store.CalendarsOf(fAccountId)) {
			if (SameCollection(candidate->remoteId, href))
				calendar = candidate;
		}
		if (calendar == nullptr) {
			Calendar fresh;
			fresh.accountId = fAccountId;
			fresh.remoteId = href;
			fresh.color = kColors[index % 6];
			calendar = &store.AddCalendar(fresh);
		}
		seen.insert(calendar->id);
		std::string name = prop->ChildText(kDav, "displayname");
		if (name.empty()) {
			std::string path = UrlDecode(UrlPath(Url(href)));
			while (!path.empty() && path.back() == '/')
				path.pop_back();
			name = path.substr(path.rfind('/') + 1);
		}
		calendar->name = name;
		std::string color = prop->ChildText(kApple, "calendar-color");
		if (!color.empty())
			calendar->color = ParseHexColor(color, calendar->color);
		std::string order = prop->ChildText(kApple, "calendar-order");
		calendar->order = order.empty() ? index : atoi(order.c_str());
		calendar->events = events;
		calendar->tasks = tasks;
		calendar->readOnly = !writable;
		std::string tag = prop->ChildText(kCalServer, "getctag");
		if (tag.empty())
			tag = prop->ChildText(kDav, "sync-token");
		calendar->ctag = tag;
		index++;
	}
	for (Calendar* calendar : store.CalendarsOf(fAccountId)) {
		if (seen.count(calendar->id) == 0)
			store.RemoveCalendar(calendar->id);
	}
	store.Changed();
}


void
CalDavProvider::PushEvents(const Calendar& calendar)
{
	std::vector<Event> masters;
	std::map<std::string, std::vector<Event>> overrides;
	{
		std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
		for (Event* event : fContext->store.EventsIn(calendar.id)) {
			if (event->IsOverride())
				continue;
			bool dirty = event->pending != Pending::None;
			std::vector<Event> list;
			for (Event* child : fContext->store.OverridesOf(event->id)) {
				list.push_back(*child);
				if (child->pending != Pending::None)
					dirty = true;
			}
			if (!dirty)
				continue;
			masters.push_back(*event);
			overrides[event->id] = list;
		}
	}
	if (masters.empty())
		return;

	for (Event& master : masters) {
		fContext->CheckCancelled();
		if (calendar.readOnly) {
			std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
			Event* local = fContext->store.FindEvent(master.id);
			if (local != nullptr)
				local->pending = Pending::None;
			continue;
		}
		if (master.pending == Pending::Delete) {
			if (!master.remoteId.empty()) {
				std::vector<std::pair<std::string, std::string>> headers;
				if (!master.etag.empty())
					headers.push_back({"If-Match", master.etag});
				HttpResponse response = Dav("DELETE", Url(master.remoteId), "",
					"", headers);
				if (response.status == 412)
					response = Dav("DELETE", Url(master.remoteId), "", "");
				if (!response.Ok() && response.status != 404 && response.status != 410)
					ThrowHttpError("The CalDAV server", response);
			}
			std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
			fContext->store.RemoveEvent(master.id);
			fContext->store.Changed();
			continue;
		}

		std::vector<const Event*> list;
		for (const Event& child : overrides[master.id]) {
			if (child.pending != Pending::Delete)
				list.push_back(&child);
		}
		if (master.uid.empty())
			master.uid = NewId() + "@aircal";
		std::string href = master.remoteId;
		bool create = href.empty();
		if (create) {
			std::string collection = Url(calendar.remoteId);
			if (collection.back() != '/')
				collection += '/';
			std::string name;
			for (char c : master.uid) {
				name += (isalnum((unsigned char)c) || c == '-' || c == '_'
					|| c == '.') ? c : '-';
			}
			href = UrlPath(collection) + name + ".ics";
		}
		std::string raw = master.raw;
		std::string etag = master.etag;
		HttpResponse response;
		std::string body;
		for (int attempt = 0; attempt < 2; attempt++) {
			body = ical::EventsToCalendar(raw, master, list);
			std::vector<std::pair<std::string, std::string>> headers;
			if (create)
				headers.push_back({"If-None-Match", "*"});
			else if (!etag.empty())
				headers.push_back({"If-Match", etag});
			response = Dav("PUT", Url(href), body, "", headers);
			if (response.status != 412)
				break;
			// Changed on the server: take its version as the base and write
			// our fields over it.
			HttpResponse current = Dav("GET", Url(href), "", "");
			if (!current.Ok()) {
				create = true;
				raw.clear();
				etag.clear();
				continue;
			}
			raw = current.body;
			etag = current.Header("etag");
			create = false;
		}
		if (!response.Ok())
			ThrowHttpError("The CalDAV server", response);

		std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
		Store& store = fContext->store;
		Event* local = store.FindEvent(master.id);
		if (local == nullptr)
			continue;
		local->remoteId = href;
		local->uid = master.uid;
		local->raw = body;
		// A weak or missing ETag means the server changed the data; the
		// next pull fetches it.
		std::string newTag = response.Header("etag");
		local->etag = newTag.compare(0, 2, "W/") == 0 ? std::string() : newTag;
		if (local->revision == master.revision)
			local->pending = Pending::None;
		for (const Event& child : overrides[master.id]) {
			Event* item = store.FindEvent(child.id);
			if (item == nullptr)
				continue;
			if (child.pending == Pending::Delete) {
				store.RemoveEvent(child.id);
				continue;
			}
			item->remoteId = href + "#" + ical::EventKey(*item);
			item->uid = master.uid;
			if (item->revision == child.revision)
				item->pending = Pending::None;
		}
		store.Changed();
	}
}


static std::string
TimeRangeQuery(const char* component, bool range)
{
	std::string filter = std::string("<c:comp-filter name=\"") + component + "\"";
	if (range) {
		Instant now = Now();
		filter += "><c:time-range start=\""
			+ FormatBasicUtc(now - SyncWindowPast()) + "\" end=\""
			+ FormatBasicUtc(now + SyncWindowFuture()) + "\"/></c:comp-filter>";
	} else
		filter += "/>";
	return std::string(kXmlHead)
		+ "<c:calendar-query xmlns:d=\"DAV:\" "
		"xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:prop><d:getetag/>"
		"<c:calendar-data/></d:prop><c:filter><c:comp-filter name=\"VCALENDAR\">"
		+ filter + "</c:comp-filter></c:filter></c:calendar-query>";
}


void
CalDavProvider::PullEvents(Calendar& calendar)
{
	std::unique_ptr<xml::Element> root = Multistatus("REPORT",
		Url(calendar.remoteId), TimeRangeQuery("VEVENT", true), "1");
	std::vector<DavResponse> items = Responses(root.get());

	std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
	Store& store = fContext->store;
	// Local events by resource (ids: removing a master frees its overrides).
	std::map<std::string, std::vector<std::string>> byResource;
	for (Event* event : store.EventsIn(calendar.id)) {
		std::string href = event->remoteId.substr(0, event->remoteId.find('#'));
		if (!href.empty())
			byResource[href].push_back(event->id);
	}
	std::set<std::string> seen;
	for (auto& item : items) {
		if (item.prop == nullptr || item.href.empty())
			continue;
		std::string data = item.prop->ChildText(kCalDav, "calendar-data");
		std::string etag = item.prop->ChildText(kDav, "getetag");
		seen.insert(item.href);
		std::vector<std::string> existing = byResource[item.href];
		bool dirty = false, unchanged = !existing.empty();
		std::map<std::string, std::string> keep;
		for (const std::string& id : existing) {
			Event* event = store.FindEvent(id);
			if (event == nullptr)
				continue;
			if (event->pending != Pending::None)
				dirty = true;
			if (event->etag != etag || etag.empty())
				unchanged = false;
			keep[ical::EventKey(*event)] = event->id;
		}
		if (dirty || unchanged || data.empty())
			continue;
		ical::Component component;
		if (!ical::Parse(data, component))
			continue;
		std::vector<Event> parsed = ical::EventsFromCalendar(component, &keep);
		if (parsed.empty())
			continue;
		std::set<std::string> kept;
		for (Event& event : parsed) {
			event.calendarId = calendar.id;
			event.etag = etag;
			event.remoteId = event.IsOverride()
				? item.href + "#" + ical::EventKey(event) : item.href;
			Event* local = store.FindEvent(event.id);
			if (local != nullptr) {
				uint32_t revision = local->revision;
				*local = event;
				local->revision = revision;
				local->pending = Pending::None;
			} else
				store.AddEvent(event);
			kept.insert(event.id);
		}
		for (const std::string& id : existing) {
			if (kept.count(id) == 0 && store.FindEvent(id) != nullptr)
				store.RemoveEvent(id);
		}
	}
	for (auto& resource : byResource) {
		if (seen.count(resource.first) != 0)
			continue;
		for (const std::string& id : resource.second) {
			Event* event = store.FindEvent(id);
			if (event != nullptr && event->pending == Pending::None)
				store.RemoveEvent(id);
		}
	}
	calendar.syncToken = calendar.ctag;
	store.Changed();
}


void
CalDavProvider::PushTasks(const Calendar& calendar)
{
	std::vector<Task> pending;
	{
		std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
		for (Task* task : fContext->store.TasksIn(calendar.id)) {
			if (task->pending != Pending::None)
				pending.push_back(*task);
		}
	}
	for (Task& task : pending) {
		fContext->CheckCancelled();
		if (calendar.readOnly) {
			std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
			Task* local = fContext->store.FindTask(task.id);
			if (local != nullptr)
				local->pending = Pending::None;
			continue;
		}
		if (task.pending == Pending::Delete) {
			if (!task.remoteId.empty()) {
				HttpResponse response = Dav("DELETE", Url(task.remoteId), "", "");
				if (!response.Ok() && response.status != 404 && response.status != 410)
					ThrowHttpError("The CalDAV server", response);
			}
			std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
			fContext->store.RemoveTask(task.id);
			fContext->store.Changed();
			continue;
		}
		std::string parentUid;
		{
			std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
			Task* parent = task.parentId.empty() ? nullptr
				: fContext->store.FindTask(task.parentId);
			if (parent != nullptr) {
				if (parent->uid.empty()) {
					parent->uid = NewId() + "@aircal";
				}
				parentUid = parent->uid;
			}
		}
		if (task.uid.empty())
			task.uid = NewId() + "@aircal";
		std::string href = task.remoteId;
		bool create = href.empty();
		if (create) {
			std::string collection = Url(calendar.remoteId);
			if (collection.back() != '/')
				collection += '/';
			std::string name;
			for (char c : task.uid) {
				name += (isalnum((unsigned char)c) || c == '-' || c == '_'
					|| c == '.') ? c : '-';
			}
			href = UrlPath(collection) + name + ".ics";
		}
		std::string raw = task.raw;
		std::string etag = task.etag;
		HttpResponse response;
		std::string body;
		for (int attempt = 0; attempt < 2; attempt++) {
			body = ical::TaskToCalendar(raw, task, parentUid);
			std::vector<std::pair<std::string, std::string>> headers;
			if (create)
				headers.push_back({"If-None-Match", "*"});
			else if (!etag.empty())
				headers.push_back({"If-Match", etag});
			response = Dav("PUT", Url(href), body, "", headers);
			if (response.status != 412)
				break;
			HttpResponse current = Dav("GET", Url(href), "", "");
			if (!current.Ok()) {
				create = true;
				raw.clear();
				etag.clear();
				continue;
			}
			raw = current.body;
			etag = current.Header("etag");
			create = false;
		}
		if (!response.Ok())
			ThrowHttpError("The CalDAV server", response);
		std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
		Task* local = fContext->store.FindTask(task.id);
		if (local == nullptr)
			continue;
		local->remoteId = href;
		local->uid = task.uid;
		local->raw = body;
		std::string newTag = response.Header("etag");
		local->etag = newTag.compare(0, 2, "W/") == 0 ? std::string() : newTag;
		if (local->revision == task.revision)
			local->pending = Pending::None;
		fContext->store.Changed();
	}
}


void
CalDavProvider::PullTasks(Calendar& calendar)
{
	std::unique_ptr<xml::Element> root = Multistatus("REPORT",
		Url(calendar.remoteId), TimeRangeQuery("VTODO", false), "1");
	std::vector<DavResponse> items = Responses(root.get());

	std::lock_guard<std::recursive_mutex> lock(fContext->store.Mutex());
	Store& store = fContext->store;
	std::set<std::string> seen;
	std::map<std::string, std::string> parentUids;	// local id -> parent uid
	for (auto& item : items) {
		if (item.prop == nullptr || item.href.empty())
			continue;
		std::string data = item.prop->ChildText(kCalDav, "calendar-data");
		std::string etag = item.prop->ChildText(kDav, "getetag");
		seen.insert(item.href);
		Task* existing = store.FindTaskByRemote(calendar.id, item.href);
		if (existing != nullptr && (existing->pending != Pending::None
				|| (!etag.empty() && existing->etag == etag)))
			continue;
		ical::Component component;
		if (data.empty() || !ical::Parse(data, component))
			continue;
		std::vector<Task> parsed = ical::TasksFromCalendar(component);
		if (parsed.empty())
			continue;
		Task remote = parsed[0];
		std::string parentUid = remote.parentId;
		remote.parentId.clear();
		remote.remoteId = item.href;
		remote.etag = etag;
		remote.raw = data;
		Task* local = UpsertRemoteTask(store, calendar.id, remote);
		if (local != nullptr) {
			local->parentId.clear();
			if (!parentUid.empty())
				parentUids[local->id] = parentUid;
		}
	}
	for (auto& link : parentUids) {
		Task* child = store.FindTask(link.first);
		if (child == nullptr)
			continue;
		for (Task* candidate : store.TasksIn(calendar.id)) {
			if (candidate->uid == link.second) {
				child->parentId = candidate->id;
				break;
			}
		}
	}
	RemoveStaleTasks(store, calendar.id, seen);
	calendar.syncToken = calendar.ctag;
	store.Changed();
}

}	// namespace


std::unique_ptr<Provider>
CreateCalDavProvider()
{
	return std::unique_ptr<Provider>(new CalDavProvider);
}

}	// namespace aircal
