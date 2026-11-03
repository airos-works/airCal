// iCalendar (RFC 5545) objects: a faithful parser/serializer that keeps
// unknown components and properties, plus the mapping to airCal's Event and
// Task. CalDAV resources round-trip through this: airCal edits only the
// properties it understands and leaves the rest alone.
#pragma once

#include "Model.h"

#include <map>
#include <string>
#include <vector>

namespace aircal {
namespace ical {

struct Property {
	std::string name;	// upper case, e.g. "DTSTART"
	// Parameters in order; names upper case, values unquoted.
	std::vector<std::pair<std::string, std::string>> params;
	std::string value;	// raw (still escaped for TEXT values)

	std::string Param(const std::string& name) const;	// "" if absent
	void SetParam(const std::string& name, const std::string& value);
	void RemoveParam(const std::string& name);
};

struct Component {
	std::string name;	// "VCALENDAR", "VEVENT", "VTODO", "VALARM", "VTIMEZONE" ...
	std::vector<Property> properties;
	std::vector<Component> children;

	const Property* Find(const std::string& name) const;
	Property* Find(const std::string& name);
	std::vector<const Property*> FindAll(const std::string& name) const;
	void Remove(const std::string& name);	// all properties with the name
	// Replaces the first property with the name (or appends) and drops others.
	Property& Set(const std::string& name, const std::string& value);
	Property& Add(const std::string& name, const std::string& value);
};

// Parses text (CRLF or LF, folded lines). Returns false on structural errors.
bool Parse(const std::string& text, Component& root);
// Serializes with CRLF line endings, folding at 75 octets (UTF-8 safe).
std::string Serialize(const Component& root);

std::string EscapeText(const std::string& text);
std::string UnescapeText(const std::string& text);

// A DATE or DATE-TIME property value -> instant/date. TZID parameters are
// resolved with CanonicalZone(); unknown TZIDs fall back to the VTIMEZONE in
// `calendar` (its STANDARD offset) and then to the local zone.
struct TimeValue {
	bool valid = false;
	bool dateOnly = false;
	bool floating = false;	// no Z and no TZID
	Instant instant = 0;	// timed
	Date date;				// date-only (and the local date of timed values)
	std::string tzid;		// canonical Olson id, "" for floating, "UTC"
};
TimeValue ParseTime(const Property& property, const Component* calendar);

// The VEVENTs / VTODOs of one calendar resource (CalDAV object) as airCal
// objects. Events: the master first (or a lone override when the master is
// not in the resource), then overrides with masterId set to the master's id.
// Ids are new random ids unless `keepIds` gives existing ones by
// "UID|RECURRENCE-ID" key (see EventKey); remoteId/etag/calendarId are left to
// the caller. raw is the whole resource text for the master.
std::vector<Event> EventsFromCalendar(const Component& calendar,
	const std::map<std::string, std::string>* keepIds = nullptr);
std::vector<Task> TasksFromCalendar(const Component& calendar,
	const std::map<std::string, std::string>* keepIds = nullptr);

std::string EventKey(const Event& event);	// "UID|" or "UID|<recurrence id>"

// Builds or updates a resource: starts from `existing` (the previous raw text
// of the resource, may be empty) and writes the master and overrides back,
// removing components for events no longer present. Unknown properties of
// existing components survive.
std::string EventsToCalendar(const std::string& existing, const Event& master,
	const std::vector<const Event*>& overrides);
std::string TaskToCalendar(const std::string& existing, const Task& task,
	const std::string& parentUid);

// Imports every event in an .ics file (several UIDs allowed).
std::vector<Event> ImportEvents(const std::string& text);
std::string ExportEvents(const std::vector<const Event*>& events);

}	// namespace ical
}	// namespace aircal
