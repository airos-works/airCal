#include "ICal.h"
#include "Recurrence.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

namespace aircal {
namespace ical {

namespace {

const char* kProductId = "-//airos-works//airCal//EN";


std::string
Upper(std::string text)
{
	for (char& c : text)
		c = (char)toupper((unsigned char)c);
	return text;
}


std::string
Trim(const std::string& text)
{
	size_t start = 0;
	size_t end = text.size();
	while (start < end && isspace((unsigned char)text[start]))
		start++;
	while (end > start && isspace((unsigned char)text[end - 1]))
		end--;
	return text.substr(start, end - start);
}


bool
StartsWithIgnoreCase(const std::string& text, const char* prefix)
{
	size_t length = strlen(prefix);
	if (text.size() < length)
		return false;
	for (size_t i = 0; i < length; i++) {
		if (toupper((unsigned char)text[i]) != toupper((unsigned char)prefix[i]))
			return false;
	}
	return true;
}


std::vector<std::string>
SplitList(const std::string& text)
{
	std::vector<std::string> parts;
	size_t start = 0;
	while (true) {
		size_t found = text.find(',', start);
		std::string part = Trim(text.substr(start, found == std::string::npos
			? std::string::npos : found - start));
		if (!part.empty())
			parts.push_back(part);
		if (found == std::string::npos)
			break;
		start = found + 1;
	}
	return parts;
}


// Splits a TEXT list at commas that are not escaped, then unescapes.
std::vector<std::string>
SplitTextList(const std::string& text)
{
	std::vector<std::string> parts;
	std::string current;
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] == '\\' && i + 1 < text.size()) {
			current += text[i];
			current += text[++i];
		} else if (text[i] == ',') {
			std::string part = Trim(UnescapeText(current));
			if (!part.empty())
				parts.push_back(part);
			current.clear();
		} else
			current += text[i];
	}
	std::string part = Trim(UnescapeText(current));
	if (!part.empty())
		parts.push_back(part);
	return parts;
}


// One content line -> property. False if it is not "NAME[;params]:value".
bool
ParseLine(const std::string& line, Property& property)
{
	size_t length = line.size();
	size_t i = 0;
	while (i < length && line[i] != ';' && line[i] != ':')
		i++;
	if (i >= length)
		return false;
	property.name = Upper(Trim(line.substr(0, i)));
	if (property.name.empty())
		return false;
	while (i < length && line[i] == ';') {
		i++;
		size_t equals = i;
		while (equals < length && line[equals] != '=' && line[equals] != ';'
			&& line[equals] != ':')
			equals++;
		std::string name = Upper(Trim(line.substr(i, equals - i)));
		if (equals >= length)
			return false;
		if (line[equals] != '=') {
			// A parameter without a value (vCalendar 1.0 style "ENCODING").
			if (!name.empty())
				property.params.push_back(std::make_pair(name, std::string()));
			i = equals;
			continue;
		}
		i = equals + 1;
		while (true) {
			std::string value;
			if (i < length && line[i] == '"') {
				size_t close = line.find('"', i + 1);
				if (close == std::string::npos)
					return false;
				value = line.substr(i + 1, close - i - 1);
				i = close + 1;
			}
			while (i < length && line[i] != ',' && line[i] != ';' && line[i] != ':')
				value += line[i++];
			if (!name.empty())
				property.params.push_back(std::make_pair(name, value));
			if (i < length && line[i] == ',') {
				i++;
				continue;
			}
			break;
		}
	}
	if (i >= length || line[i] != ':')
		return false;
	property.value = line.substr(i + 1);
	return true;
}


std::string
QuoteParam(const std::string& input)
{
	std::string value;
	for (char c : input) {
		if (c != '"' && c != '\r' && c != '\n')
			value += c;
	}
	if (value.find_first_of(":;,") != std::string::npos)
		return "\"" + value + "\"";
	return value;
}


std::string
FormatProperty(const Property& property)
{
	std::string line = property.name;
	size_t i = 0;
	while (i < property.params.size()) {
		const std::string& name = property.params[i].first;
		line += ";" + name + "=";
		size_t j = i;
		for (; j < property.params.size() && property.params[j].first == name; j++) {
			if (j > i)
				line += ",";
			line += QuoteParam(property.params[j].second);
		}
		i = j;
	}
	line += ":";
	line += property.value;
	return line;
}


void
AppendFolded(std::string& out, const std::string& line)
{
	size_t position = 0;
	bool first = true;
	while (true) {
		size_t room = first ? 75 : 74;
		if (line.size() - position <= room)
			break;
		size_t cut = position + room;
		// Never split a UTF-8 sequence: back up to its lead byte.
		while (cut > position && ((unsigned char)line[cut] & 0xC0) == 0x80)
			cut--;
		if (cut == position)
			cut = position + room;
		if (!first)
			out += ' ';
		out.append(line, position, cut - position);
		out += "\r\n";
		position = cut;
		first = false;
	}
	if (!first)
		out += ' ';
	out.append(line, position, std::string::npos);
	out += "\r\n";
}


void
SerializeComponent(const Component& component, std::string& out)
{
	AppendFolded(out, "BEGIN:" + component.name);
	for (const Property& property : component.properties)
		AppendFolded(out, FormatProperty(property));
	for (const Component& child : component.children)
		SerializeComponent(child, out);
	AppendFolded(out, "END:" + component.name);
}


// "+1000", "-0500", "+053000" -> seconds east of UTC.
bool
ParseOffset(const std::string& input, int& seconds)
{
	std::string text = Trim(input);
	if (text.size() < 5 || (text[0] != '+' && text[0] != '-'))
		return false;
	for (size_t i = 1; i < text.size(); i++) {
		if (!isdigit((unsigned char)text[i]))
			return false;
	}
	int hours = atoi(text.substr(1, 2).c_str());
	int minutes = atoi(text.substr(3, 2).c_str());
	int secs = text.size() >= 7 ? atoi(text.substr(5, 2).c_str()) : 0;
	seconds = (hours * 3600 + minutes * 60 + secs) * (text[0] == '-' ? -1 : 1);
	return true;
}


// [+-]P[nW][nD][T[nH][nM][nS]] -> seconds.
bool
ParseDuration(const std::string& input, int64_t& seconds)
{
	std::string text = Upper(Trim(input));
	size_t i = 0;
	int sign = 1;
	if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
		sign = text[i] == '-' ? -1 : 1;
		i++;
	}
	if (i >= text.size() || text[i] != 'P')
		return false;
	i++;
	bool time = false;
	bool any = false;
	int64_t total = 0;
	while (i < text.size()) {
		if (text[i] == 'T') {
			time = true;
			i++;
			continue;
		}
		if (!isdigit((unsigned char)text[i]))
			return false;
		int64_t number = 0;
		while (i < text.size() && isdigit((unsigned char)text[i])) {
			number = number * 10 + (text[i] - '0');
			if (number > 100000000)
				return false;
			i++;
		}
		if (i >= text.size())
			return false;
		char unit = text[i++];
		if (unit == 'W' && !time)
			total += number * 7 * 86400;
		else if (unit == 'D' && !time)
			total += number * 86400;
		else if (unit == 'H' && time)
			total += number * 3600;
		else if (unit == 'M' && time)
			total += number * 60;
		else if (unit == 'S' && time)
			total += number;
		else
			return false;
		any = true;
	}
	if (!any)
		return false;
	seconds = sign * total;
	return true;
}


// Minutes before -> TRIGGER value.
std::string
FormatTrigger(int minutes)
{
	if (minutes <= 0)
		return minutes == 0 ? "PT0S" : "PT" + std::to_string(-minutes) + "M";
	if (minutes % 1440 == 0)
		return "-P" + std::to_string(minutes / 1440) + "D";
	if (minutes % 60 == 0)
		return "-PT" + std::to_string(minutes / 60) + "H";
	return "-PT" + std::to_string(minutes) + "M";
}


// The minutes-before of a VALARM's TRIGGER; `reference` is the start (or
// due) instant absolute triggers are measured from. False if unusable.
bool
AlarmMinutes(const Component& alarm, bool hasReference, Instant reference,
	int64_t duration, const Component* calendar, int& minutes)
{
	const Property* trigger = alarm.Find("TRIGGER");
	if (trigger == nullptr)
		return false;
	if (Upper(trigger->Param("VALUE")) == "DATE-TIME") {
		if (!hasReference)
			return false;
		TimeValue when = ParseTime(*trigger, calendar);
		if (!when.valid)
			return false;
		int64_t before = reference - when.instant;
		if (before < 0)
			return false;
		minutes = (int)(before / 60);
		return true;
	}
	int64_t offset;
	if (!ParseDuration(trigger->value, offset))
		return false;
	if (Upper(trigger->Param("RELATED")) == "END")
		offset += duration;
	if (offset > 0)
		return false;
	minutes = (int)(-offset / 60);
	return true;
}


std::string
FormatPerson(const Property& property)
{
	std::string mail = Trim(property.value);
	if (StartsWithIgnoreCase(mail, "mailto:"))
		mail = mail.substr(7);
	std::string name = Trim(property.Param("CN"));
	if (name.empty())
		return mail;
	if (mail.empty() || mail == name)
		return name;
	return name + " <" + mail + ">";
}


bool
HasRecurrence(const Event& event)
{
	return event.IsOverride() || (!event.allDay && event.recurrenceId != 0)
		|| (event.allDay && event.recurrenceDate != Date());
}


bool
WritesZone(const std::string& tzid)
{
	return !tzid.empty() && tzid != "UTC" && tzid.compare(0, 4, "Etc/") != 0
		&& IsKnownZone(tzid);
}


// Writes a DATE (all-day) or DATE-TIME value: with TZID for real zones, UTC
// otherwise.
void
WriteTime(Property& property, bool allDay, Instant instant, const Date& date,
	const std::string& tzid)
{
	property.RemoveParam("VALUE");
	property.RemoveParam("TZID");
	if (allDay) {
		property.SetParam("VALUE", "DATE");
		property.value = date.ToBasic();
	} else if (WritesZone(tzid)) {
		property.SetParam("TZID", tzid);
		property.value = FormatBasicWall(ToWall(instant, tzid));
	} else
		property.value = FormatBasicUtc(instant);
}


void
SetOrRemoveText(Component& component, const char* name, const std::string& text)
{
	if (text.empty())
		component.Remove(name);
	else
		component.Set(name, EscapeText(text));
}


Component
NewCalendar()
{
	Component root;
	root.name = "VCALENDAR";
	root.Add("VERSION", "2.0");
	root.Add("PRODID", kProductId);
	root.Add("CALSCALE", "GREGORIAN");
	return root;
}


bool
ParseExisting(const std::string& existing, Component& root)
{
	if (!existing.empty() && Parse(existing, root) && root.name == "VCALENDAR")
		return true;
	root = NewCalendar();
	return false;
}


void
BumpSequence(Component& component, bool isNew)
{
	int sequence = 0;
	const Property* existing = component.Find("SEQUENCE");
	if (existing != nullptr)
		sequence = atoi(existing->value.c_str()) + 1;
	else if (!isNew)
		sequence = 1;
	component.Set("SEQUENCE", std::to_string(sequence));
}


void
StampComponent(Component& component, bool isNew)
{
	std::string now = FormatBasicUtc(Now());
	component.Set("DTSTAMP", now);
	if (isNew && component.Find("CREATED") == nullptr)
		component.Set("CREATED", now);
	component.Set("LAST-MODIFIED", now);
	BumpSequence(component, isNew);
}


// Replaces the DISPLAY alarms with ours, keeps the others (AUDIO, EMAIL ...).
void
WriteAlarms(Component& component, const std::vector<int>& alarms,
	const std::string& title, bool hasReference, Instant reference,
	int64_t duration, const Component* calendar)
{
	std::set<int> kept;
	std::vector<Component> children;
	for (Component& child : component.children) {
		if (child.name == "VALARM") {
			const Property* action = child.Find("ACTION");
			std::string name = action != nullptr ? Upper(Trim(action->value)) : "";
			if (name.empty() || name == "DISPLAY")
				continue;
			int minutes;
			if (AlarmMinutes(child, hasReference, reference, duration, calendar,
					minutes))
				kept.insert(minutes);
		}
		children.push_back(child);
	}
	std::set<int> written;
	for (int minutes : alarms) {
		if (kept.count(minutes) != 0 || !written.insert(minutes).second)
			continue;
		Component alarm;
		alarm.name = "VALARM";
		alarm.Add("ACTION", "DISPLAY");
		alarm.Add("DESCRIPTION", EscapeText(title.empty() ? "Reminder" : title));
		alarm.Add("TRIGGER", FormatTrigger(minutes));
		children.push_back(alarm);
	}
	component.children.swap(children);
}


std::vector<const Component*>
ComponentsNamed(const Component& calendar, const char* name)
{
	std::vector<const Component*> result;
	if (calendar.name == name)
		result.push_back(&calendar);
	for (const Component& child : calendar.children) {
		if (child.name == name)
			result.push_back(&child);
	}
	return result;
}


std::string
ValueOf(const Component& component, const char* name)
{
	const Property* property = component.Find(name);
	return property != nullptr ? property->value : std::string();
}


std::string
TextOf(const Component& component, const char* name)
{
	return UnescapeText(ValueOf(component, name));
}


// One VEVENT -> Event (without ids). False without a usable DTSTART.
bool
EventFromComponent(const Component& component, const Component* calendar,
	Event& event, bool& isOverride)
{
	const Property* startProperty = component.Find("DTSTART");
	if (startProperty == nullptr)
		return false;
	TimeValue start = ParseTime(*startProperty, calendar);
	if (!start.valid)
		return false;

	event.uid = Trim(ValueOf(component, "UID"));
	event.title = TextOf(component, "SUMMARY");
	event.location = TextOf(component, "LOCATION");
	event.notes = TextOf(component, "DESCRIPTION");
	event.url = Trim(ValueOf(component, "URL"));

	event.allDay = start.dateOnly;
	if (event.allDay) {
		event.startDate = start.date;
		event.endDate = start.date.AddDays(1);
		event.start = StartOfDay(event.startDate);
		event.end = StartOfDay(event.endDate);
	} else {
		event.start = start.instant;
		event.end = start.instant;
		event.tzid = start.tzid;
	}

	const Property* endProperty = component.Find("DTEND");
	const Property* durationProperty = component.Find("DURATION");
	int64_t duration;
	if (endProperty != nullptr) {
		TimeValue end = ParseTime(*endProperty, calendar);
		if (end.valid) {
			if (event.allDay) {
				Date endDate = end.dateOnly ? end.date
					: ToWall(end.instant, end.tzid).date;
				if (endDate > event.startDate)
					event.endDate = endDate;
				event.end = StartOfDay(event.endDate);
			} else if (end.instant >= event.start)
				event.end = end.instant;
		}
	} else if (durationProperty != nullptr
		&& ParseDuration(durationProperty->value, duration) && duration >= 0) {
		if (event.allDay) {
			int64_t days = std::max<int64_t>(1, duration / 86400);
			event.endDate = event.startDate.AddDays(days);
			event.end = StartOfDay(event.endDate);
		} else
			event.end = event.start + duration;
	}

	std::string status = Upper(Trim(ValueOf(component, "STATUS")));
	event.cancelled = status == "CANCELLED";
	event.tentative = status == "TENTATIVE";

	const Property* rule = component.Find("RRULE");
	if (rule != nullptr) {
		event.rrule = Trim(rule->value);
		if (StartsWithIgnoreCase(event.rrule, "RRULE:"))
			event.rrule = event.rrule.substr(6);
	}

	for (const Property* exdate : component.FindAll("EXDATE")) {
		for (const std::string& item : SplitList(exdate->value)) {
			Property single = *exdate;
			single.value = item;
			TimeValue value = ParseTime(single, calendar);
			if (!value.valid)
				continue;
			if (value.dateOnly)
				event.exdateDays.push_back(value.date);
			else if (event.allDay)
				event.exdateDays.push_back(ToWall(value.instant, value.tzid).date);
			else
				event.exdates.push_back(value.instant);
		}
	}
	for (const Property* rdate : component.FindAll("RDATE")) {
		bool period = Upper(rdate->Param("VALUE")) == "PERIOD";
		for (std::string item : SplitList(rdate->value)) {
			if (period || item.find('/') != std::string::npos)
				item = item.substr(0, item.find('/'));
			Property single = *rdate;
			single.RemoveParam("VALUE");
			if (Upper(rdate->Param("VALUE")) == "DATE")
				single.SetParam("VALUE", "DATE");
			single.value = item;
			TimeValue value = ParseTime(single, calendar);
			if (value.valid)
				event.rdates.push_back(value.dateOnly ? StartOfDay(value.date)
					: value.instant);
		}
	}

	const Property* recurrence = component.Find("RECURRENCE-ID");
	isOverride = false;
	if (recurrence != nullptr) {
		TimeValue value = ParseTime(*recurrence, calendar);
		if (value.valid) {
			isOverride = true;
			if (value.dateOnly) {
				event.recurrenceDate = value.date;
				event.recurrenceId = StartOfDay(value.date);
			} else {
				event.recurrenceId = value.instant;
				event.recurrenceDate = ToWall(value.instant, value.tzid).date;
			}
		}
	}

	const Property* organizer = component.Find("ORGANIZER");
	if (organizer != nullptr)
		event.organizer = FormatPerson(*organizer);
	for (const Property* attendee : component.FindAll("ATTENDEE"))
		event.attendees.push_back(FormatPerson(*attendee));

	int64_t length = event.end - event.start;
	for (const Component& child : component.children) {
		if (child.name != "VALARM")
			continue;
		int minutes;
		if (AlarmMinutes(child, true, event.start, length, calendar, minutes)
			&& std::find(event.alarms.begin(), event.alarms.end(), minutes)
				== event.alarms.end())
			event.alarms.push_back(minutes);
	}

	const Property* modified = component.Find("LAST-MODIFIED");
	if (modified == nullptr)
		modified = component.Find("DTSTAMP");
	if (modified != nullptr) {
		TimeValue value = ParseTime(*modified, calendar);
		if (value.valid)
			event.updated = value.instant;
	}
	return true;
}


std::string
KeyOf(const std::map<std::string, std::string>* keepIds, const std::string& key)
{
	if (keepIds != nullptr) {
		auto found = keepIds->find(key);
		if (found != keepIds->end() && !found->second.empty())
			return found->second;
	}
	return NewId();
}


// Whether a component's RECURRENCE-ID names the override's instance.
bool
SameRecurrence(const Component& component, const Event& override,
	const Event& master, const Component* calendar)
{
	const Property* property = component.Find("RECURRENCE-ID");
	if (property == nullptr)
		return false;
	TimeValue value = ParseTime(*property, calendar);
	if (!value.valid)
		return false;
	if (master.allDay) {
		Date date = value.dateOnly ? value.date : ToWall(value.instant,
			value.tzid).date;
		return date == override.recurrenceDate;
	}
	if (value.dateOnly)
		return value.date == ToWall(override.recurrenceId, master.tzid).date;
	return value.instant == override.recurrenceId;
}


// "Name <mail>", "mail" or "mailto:mail" -> lower-case mail and the name.
void
SplitPerson(const std::string& text, std::string& mail, std::string& name)
{
	std::string value = Trim(text);
	name.clear();
	size_t open = value.find('<');
	size_t close = value.rfind('>');
	if (open != std::string::npos && close != std::string::npos && close > open) {
		name = Trim(value.substr(0, open));
		value = value.substr(open + 1, close - open - 1);
	}
	if (StartsWithIgnoreCase(value, "mailto:"))
		value = value.substr(7);
	mail = value;
	for (char& c : mail)
		c = (char)tolower((unsigned char)c);
}


// ATTENDEEs from event.attendees: existing ones keep their parameters
// (PARTSTAT and so on), new ones are invited; ORGANIZER from event.organizer
// when there is none yet.
void
WriteAttendees(Component& component, const Event& event)
{
	std::vector<Property> kept;
	std::map<std::string, Property> existing;
	bool hadAttendees = component.Find("ATTENDEE") != nullptr;
	for (const Property* attendee : component.FindAll("ATTENDEE")) {
		std::string mail, name;
		SplitPerson(attendee->value, mail, name);
		existing[mail] = *attendee;
	}
	std::string organizerMail, organizerName;
	SplitPerson(event.organizer, organizerMail, organizerName);
	for (const std::string& person : event.attendees) {
		std::string mail, name;
		SplitPerson(person, mail, name);
		if (mail.empty())
			continue;
		auto found = existing.find(mail);
		if (found != existing.end()) {
			kept.push_back(found->second);
			continue;
		}
		Property attendee;
		attendee.name = "ATTENDEE";
		if (!name.empty())
			attendee.SetParam("CN", name);
		attendee.SetParam("ROLE", "REQ-PARTICIPANT");
		attendee.SetParam("PARTSTAT", mail == organizerMail ? "ACCEPTED" : "NEEDS-ACTION");
		if (mail != organizerMail)
			attendee.SetParam("RSVP", "TRUE");
		attendee.value = "mailto:" + mail;
		kept.push_back(attendee);
	}
	component.Remove("ATTENDEE");
	for (const Property& attendee : kept)
		component.properties.push_back(attendee);
	if (!kept.empty() && component.Find("ORGANIZER") == nullptr
		&& !organizerMail.empty()) {
		Property& organizer = component.Add("ORGANIZER", "mailto:" + organizerMail);
		if (!organizerName.empty())
			organizer.SetParam("CN", organizerName);
	}
	if (kept.empty() && hadAttendees)
		component.Remove("ORGANIZER");
}


void
WriteEventFields(Component& component, const Event& event, const Event& master,
	const std::string& uid, bool isNew, bool isOverride,
	const Component* calendar)
{
	component.Set("UID", uid);
	StampComponent(component, isNew);
	SetOrRemoveText(component, "SUMMARY", event.title);
	SetOrRemoveText(component, "LOCATION", event.location);
	SetOrRemoveText(component, "DESCRIPTION", event.notes);
	if (event.url.empty())
		component.Remove("URL");
	else
		component.Set("URL", event.url);

	if (component.Find("DTSTART") == nullptr)
		component.Add("DTSTART", "");
	Property& start = *component.Find("DTSTART");
	WriteTime(start, event.allDay, event.start, event.startDate, event.tzid);

	component.Remove("DURATION");
	if (component.Find("DTEND") == nullptr)
		component.Add("DTEND", "");
	Property& end = *component.Find("DTEND");
	if (event.allDay) {
		Date endDate = event.endDate > event.startDate ? event.endDate
			: event.startDate.AddDays(1);
		WriteTime(end, true, 0, endDate, "");
	} else
		WriteTime(end, false, std::max(event.start, event.end), Date(), event.tzid);

	if (event.cancelled)
		component.Set("STATUS", "CANCELLED");
	else if (event.tentative)
		component.Set("STATUS", "TENTATIVE");
	else {
		const Property* status = component.Find("STATUS");
		if (status != nullptr) {
			std::string value = Upper(Trim(status->value));
			if (value == "CANCELLED" || value == "TENTATIVE")
				component.Remove("STATUS");
		}
	}

	if (isOverride) {
		component.Remove("RRULE");
		if (component.Find("RECURRENCE-ID") == nullptr)
			component.Add("RECURRENCE-ID", "");
		Property& recurrence = *component.Find("RECURRENCE-ID");
		recurrence.params.clear();
		WriteTime(recurrence, master.allDay, event.recurrenceId,
			event.recurrenceDate, master.tzid);
	} else {
		std::string rule = Trim(event.rrule);
		if (StartsWithIgnoreCase(rule, "RRULE:"))
			rule = rule.substr(6);
		if (rule.empty())
			component.Remove("RRULE");
		else
			component.Set("RRULE", rule);
	}

	WriteAlarms(component, event.alarms, event.title, true, event.start,
		event.end - event.start, calendar);
	WriteAttendees(component, event);
}


// Builds a TaskToCalendar-style DATE or UTC DATE-TIME due value.
void
WriteDue(Component& component, const Task& task)
{
	if (!task.hasDue) {
		component.Remove("DUE");
		return;
	}
	if (component.Find("DUE") == nullptr)
		component.Add("DUE", "");
	Property& due = *component.Find("DUE");
	due.params.clear();
	if (task.dueHasTime)
		due.value = FormatBasicUtc(task.dueTime);
	else {
		due.SetParam("VALUE", "DATE");
		due.value = task.dueDate.ToBasic();
	}
}

}	// namespace


// --- Property / Component ----------------------------------------------------------

std::string
Property::Param(const std::string& rawName) const
{
	std::string name = Upper(rawName);
	std::string result;
	bool found = false;
	for (const auto& param : params) {
		if (param.first != name)
			continue;
		if (found)
			result += ",";
		result += param.second;
		found = true;
	}
	return result;
}


void
Property::SetParam(const std::string& rawName, const std::string& value)
{
	std::string name = Upper(rawName);
	bool set = false;
	for (size_t i = 0; i < params.size();) {
		if (params[i].first != name) {
			i++;
			continue;
		}
		if (set) {
			params.erase(params.begin() + i);
			continue;
		}
		params[i].second = value;
		set = true;
		i++;
	}
	if (!set)
		params.push_back(std::make_pair(name, value));
}


void
Property::RemoveParam(const std::string& rawName)
{
	std::string name = Upper(rawName);
	params.erase(std::remove_if(params.begin(), params.end(),
		[&](const std::pair<std::string, std::string>& param) {
			return param.first == name;
		}), params.end());
}


const Property*
Component::Find(const std::string& rawName) const
{
	std::string name = Upper(rawName);
	for (const Property& property : properties) {
		if (property.name == name)
			return &property;
	}
	return nullptr;
}


Property*
Component::Find(const std::string& rawName)
{
	std::string name = Upper(rawName);
	for (Property& property : properties) {
		if (property.name == name)
			return &property;
	}
	return nullptr;
}


std::vector<const Property*>
Component::FindAll(const std::string& rawName) const
{
	std::string name = Upper(rawName);
	std::vector<const Property*> result;
	for (const Property& property : properties) {
		if (property.name == name)
			result.push_back(&property);
	}
	return result;
}


void
Component::Remove(const std::string& rawName)
{
	std::string name = Upper(rawName);
	properties.erase(std::remove_if(properties.begin(), properties.end(),
		[&](const Property& property) { return property.name == name; }),
		properties.end());
}


Property&
Component::Set(const std::string& rawName, const std::string& value)
{
	std::string name = Upper(rawName);
	size_t first = properties.size();
	for (size_t i = 0; i < properties.size();) {
		if (properties[i].name != name) {
			i++;
			continue;
		}
		if (first != properties.size()) {
			properties.erase(properties.begin() + i);
			continue;
		}
		first = i;
		i++;
	}
	if (first == properties.size())
		return Add(name, value);
	properties[first].params.clear();
	properties[first].value = value;
	return properties[first];
}


Property&
Component::Add(const std::string& rawName, const std::string& value)
{
	Property property;
	property.name = Upper(rawName);
	property.value = value;
	properties.push_back(property);
	return properties.back();
}


// --- Parsing and serializing ---------------------------------------------------------

bool
Parse(const std::string& text, Component& root)
{
	root = Component();

	// Unfold.
	std::vector<std::string> lines;
	size_t position = 0;
	if (text.compare(0, 3, "\xEF\xBB\xBF") == 0)
		position = 3;
	while (position < text.size()) {
		size_t newline = text.find('\n', position);
		std::string line = text.substr(position, newline == std::string::npos
			? std::string::npos : newline - position);
		position = newline == std::string::npos ? text.size() : newline + 1;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
			if (!lines.empty())
				lines.back().append(line, 1, std::string::npos);
			continue;
		}
		if (!line.empty())
			lines.push_back(line);
	}

	// Build the tree. Only the innermost open component's child list grows,
	// so the pointers to the open ones stay valid.
	std::vector<Component*> stack;
	bool haveRoot = false;
	for (const std::string& line : lines) {
		Property property;
		if (!ParseLine(line, property))
			continue;
		if (property.name == "BEGIN") {
			std::string name = Upper(Trim(property.value));
			if (name.empty())
				return false;
			if (stack.empty()) {
				if (!haveRoot) {
					root.name = name;
					haveRoot = true;
					stack.push_back(&root);
				} else if (name == root.name) {
					// Concatenated calendars: merge into the first.
					stack.push_back(&root);
				} else {
					root.children.push_back(Component());
					root.children.back().name = name;
					stack.push_back(&root.children.back());
				}
			} else {
				Component* parent = stack.back();
				parent->children.push_back(Component());
				parent->children.back().name = name;
				stack.push_back(&parent->children.back());
			}
		} else if (property.name == "END") {
			std::string name = Upper(Trim(property.value));
			size_t open = stack.size();
			while (open > 0 && stack[open - 1]->name != name)
				open--;
			if (open == 0)
				return false;
			stack.resize(open - 1);
		} else if (!stack.empty())
			stack.back()->properties.push_back(property);
	}
	// A missing END at the end of the text (truncated downloads) is accepted.
	return haveRoot;
}


std::string
Serialize(const Component& root)
{
	std::string out;
	SerializeComponent(root, out);
	return out;
}


std::string
EscapeText(const std::string& text)
{
	std::string result;
	result.reserve(text.size());
	for (size_t i = 0; i < text.size(); i++) {
		char c = text[i];
		switch (c) {
			case '\\': result += "\\\\"; break;
			case ';': result += "\\;"; break;
			case ',': result += "\\,"; break;
			case '\n': result += "\\n"; break;
			case '\r':
				if (i + 1 >= text.size() || text[i + 1] != '\n')
					result += "\\n";
				break;
			default: result += c; break;
		}
	}
	return result;
}


std::string
UnescapeText(const std::string& text)
{
	std::string result;
	result.reserve(text.size());
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] != '\\' || i + 1 >= text.size()) {
			result += text[i];
			continue;
		}
		char next = text[++i];
		if (next == 'n' || next == 'N')
			result += '\n';
		else
			result += next;
	}
	return result;
}


TimeValue
ParseTime(const Property& property, const Component* calendar)
{
	TimeValue result;
	std::string text = Trim(property.value);
	size_t comma = text.find(',');
	if (comma != std::string::npos)
		text = Trim(text.substr(0, comma));
	DateTime wall;
	bool utc, dateOnly;
	if (!ParseBasic(text, wall, utc, dateOnly))
		return result;
	if (Upper(property.Param("VALUE")) == "DATE" && !dateOnly) {
		dateOnly = true;
		wall = DateTime(wall.date);
	}
	result.valid = true;
	if (dateOnly) {
		result.dateOnly = true;
		result.date = wall.date;
		result.instant = StartOfDay(wall.date);
		return result;
	}
	if (utc) {
		result.instant = wall.AsUtcSeconds();
		result.tzid = "UTC";
		result.date = LocalDate(result.instant);
		return result;
	}

	std::string tzid = Trim(property.Param("TZID"));
	if (tzid.empty()) {
		result.floating = true;
		result.instant = ToInstant(wall, "");
		result.date = wall.date;
		return result;
	}

	std::string zone = CanonicalZone(tzid);
	if (zone.empty() && calendar != nullptr) {
		// Not an Olson or Windows name: use the resource's VTIMEZONE.
		for (const Component& child : calendar->children) {
			if (child.name != "VTIMEZONE")
				continue;
			const Property* id = child.Find("TZID");
			if (id == nullptr || Trim(id->value) != tzid)
				continue;
			const Property* location = child.Find("X-LIC-LOCATION");
			if (location != nullptr)
				zone = CanonicalZone(Trim(location->value));
			if (!zone.empty())
				break;
			const Component* rules = nullptr;
			for (const Component& sub : child.children) {
				if (sub.name == "STANDARD") {
					rules = &sub;
					break;
				}
			}
			if (rules == nullptr && !child.children.empty())
				rules = &child.children.front();
			int offset;
			const Property* to = rules != nullptr ? rules->Find("TZOFFSETTO")
				: nullptr;
			if (to == nullptr || !ParseOffset(to->value, offset))
				break;
			result.instant = wall.AsUtcSeconds() - offset;
			result.date = LocalDate(result.instant);
			// Whole hours map onto ICU's fixed "Etc/GMT-10" style zones.
			if (offset == 0)
				result.tzid = "UTC";
			else if (offset % 3600 == 0 && offset >= -12 * 3600
				&& offset <= 14 * 3600) {
				int hours = offset / 3600;
				result.tzid = std::string("Etc/GMT") + (hours > 0 ? "-" : "+")
					+ std::to_string(hours > 0 ? hours : -hours);
			}
			return result;
		}
	}
	result.tzid = zone;
	result.instant = ToInstant(wall, zone);
	result.date = LocalDate(result.instant);
	return result;
}


// --- Events ----------------------------------------------------------------------------

std::string
EventKey(const Event& event)
{
	std::string key = event.uid + "|";
	if (!HasRecurrence(event))
		return key;
	if (event.allDay)
		return key + event.recurrenceDate.ToBasic();
	return key + FormatBasicUtc(event.recurrenceId);
}


std::vector<Event>
EventsFromCalendar(const Component& calendar,
	const std::map<std::string, std::string>* keepIds)
{
	std::vector<Event> masters;
	std::vector<Event> overrides;
	for (const Component* component : ComponentsNamed(calendar, "VEVENT")) {
		Event event;
		bool isOverride;
		if (!EventFromComponent(*component, &calendar, event, isOverride))
			continue;
		if (isOverride)
			overrides.push_back(event);
		else
			masters.push_back(event);
	}

	std::vector<Event> result;
	std::vector<bool> used(overrides.size(), false);
	for (Event& master : masters) {
		master.id = KeyOf(keepIds, EventKey(master));
		result.push_back(master);
		for (size_t i = 0; i < overrides.size(); i++) {
			if (used[i] || overrides[i].uid != master.uid)
				continue;
			used[i] = true;
			Event& override = overrides[i];
			override.masterId = master.id;
			override.id = KeyOf(keepIds, EventKey(override));
			result.push_back(override);
		}
	}
	// Overrides whose master is not in the resource (an invitation to one
	// instance): standalone events.
	for (size_t i = 0; i < overrides.size(); i++) {
		if (used[i])
			continue;
		Event& override = overrides[i];
		override.id = KeyOf(keepIds, EventKey(override));
		result.push_back(override);
	}
	if (!result.empty())
		result.front().raw = Serialize(calendar);
	return result;
}


std::string
EventsToCalendar(const std::string& existing, const Event& master,
	const std::vector<const Event*>& overrides)
{
	Component root;
	ParseExisting(existing, root);

	std::string uid = Trim(master.uid);
	if (uid.empty()) {
		for (const Component& child : root.children) {
			if (child.name == "VEVENT" && child.Find("RECURRENCE-ID") == nullptr
				&& !ValueOf(child, "UID").empty()) {
				uid = Trim(ValueOf(child, "UID"));
				break;
			}
		}
	}
	if (uid.empty())
		uid = NewId() + "@aircal";

	std::vector<const Event*> live;
	std::vector<const Event*> cancelled;
	for (const Event* override : overrides) {
		if (override == nullptr || override == &master)
			continue;
		(override->cancelled ? cancelled : live).push_back(override);
	}

	// Drop override components that are no longer overrides (or cancelled).
	std::vector<Component> children;
	for (Component& child : root.children) {
		if (child.name == "VEVENT" && Trim(ValueOf(child, "UID")) == uid
			&& child.Find("RECURRENCE-ID") != nullptr) {
			bool keep = false;
			for (const Event* override : live) {
				if (SameRecurrence(child, *override, master, &root))
					keep = true;
			}
			if (!keep)
				continue;
		}
		children.push_back(child);
	}
	root.children.swap(children);

	// The master.
	size_t masterIndex = root.children.size();
	for (size_t i = 0; i < root.children.size(); i++) {
		const Component& child = root.children[i];
		if (child.name == "VEVENT" && child.Find("RECURRENCE-ID") == nullptr
			&& Trim(ValueOf(child, "UID")) == uid) {
			masterIndex = i;
			break;
		}
	}
	bool masterIsNew = masterIndex == root.children.size();
	if (masterIsNew) {
		size_t position = root.children.size();
		for (size_t i = 0; i < root.children.size(); i++) {
			if (root.children[i].name == "VEVENT"
				&& Trim(ValueOf(root.children[i], "UID")) == uid) {
				position = i;
				break;
			}
		}
		Component event;
		event.name = "VEVENT";
		root.children.insert(root.children.begin() + position, event);
		masterIndex = position;
	}
	{
		Component& component = root.children[masterIndex];
		WriteEventFields(component, master, master, uid, masterIsNew, false,
			&root);

		component.Remove("EXDATE");
		std::set<int64_t> written;
		auto addExdate = [&](bool allDay, Instant instant, const Date& date) {
			int64_t key = allDay ? date.DayNumber() : instant;
			if (!written.insert(allDay ? -key - 1 : key).second)
				return;
			Property& exdate = component.Add("EXDATE", "");
			WriteTime(exdate, allDay, instant, date, master.tzid);
		};
		for (Instant instant : master.exdates)
			addExdate(master.allDay, instant, LocalDate(instant));
		for (const Date& date : master.exdateDays)
			addExdate(true, StartOfDay(date), date);
		for (const Event* override : cancelled) {
			addExdate(master.allDay, override->recurrenceId,
				override->recurrenceDate);
		}
	}

	// The overrides.
	for (const Event* override : live) {
		size_t index = root.children.size();
		for (size_t i = 0; i < root.children.size(); i++) {
			const Component& child = root.children[i];
			if (child.name == "VEVENT" && Trim(ValueOf(child, "UID")) == uid
				&& SameRecurrence(child, *override, master, &root)) {
				index = i;
				break;
			}
		}
		bool isNew = index == root.children.size();
		if (isNew) {
			Component event;
			event.name = "VEVENT";
			root.children.push_back(event);
		}
		WriteEventFields(root.children[index], *override, master, uid, isNew,
			true, &root);
	}
	return Serialize(root);
}


// --- Tasks ---------------------------------------------------------------------------

std::vector<Task>
TasksFromCalendar(const Component& calendar,
	const std::map<std::string, std::string>* keepIds)
{
	std::vector<Task> result;
	for (const Component* component : ComponentsNamed(calendar, "VTODO")) {
		Task task;
		task.uid = Trim(ValueOf(*component, "UID"));
		task.title = TextOf(*component, "SUMMARY");
		task.notes = TextOf(*component, "DESCRIPTION");

		const Property* due = component->Find("DUE");
		if (due != nullptr) {
			TimeValue value = ParseTime(*due, &calendar);
			if (value.valid) {
				task.hasDue = true;
				if (value.dateOnly)
					task.dueDate = value.date;
				else {
					task.dueHasTime = true;
					task.dueTime = value.instant;
					task.dueDate = LocalDate(value.instant);
				}
			}
		}

		std::string status = Upper(Trim(ValueOf(*component, "STATUS")));
		const Property* completed = component->Find("COMPLETED");
		task.completed = status == "COMPLETED" || completed != nullptr;
		if (completed != nullptr) {
			TimeValue value = ParseTime(*completed, &calendar);
			if (value.valid)
				task.completedAt = value.instant;
		}

		const Property* priority = component->Find("PRIORITY");
		if (priority != nullptr) {
			int value = atoi(Trim(priority->value).c_str());
			if (value == 1)
				task.priority = 4;
			else if (value >= 2 && value <= 4)
				task.priority = 3;
			else if (value == 5)
				task.priority = 2;
			else if (value >= 6 && value <= 9)
				task.priority = 1;
		}

		for (const Property* related : component->FindAll("RELATED-TO")) {
			std::string type = Upper(related->Param("RELTYPE"));
			if (type.empty() || type == "PARENT") {
				task.parentId = Trim(related->value);
				break;
			}
		}

		for (const Property* categories : component->FindAll("CATEGORIES")) {
			for (const std::string& label : SplitTextList(categories->value)) {
				if (std::find(task.labels.begin(), task.labels.end(), label)
					== task.labels.end())
					task.labels.push_back(label);
			}
		}

		const Property* rule = component->Find("RRULE");
		if (rule != nullptr) {
			task.rrule = Trim(rule->value);
			if (StartsWithIgnoreCase(task.rrule, "RRULE:"))
				task.rrule = task.rrule.substr(6);
			task.recurring = !task.rrule.empty();
		}

		Instant reference = task.dueHasTime ? task.dueTime
			: task.hasDue ? StartOfDay(task.dueDate) : 0;
		for (const Component& child : component->children) {
			if (child.name != "VALARM")
				continue;
			int minutes;
			if (AlarmMinutes(child, task.hasDue, reference, 0, &calendar, minutes)
				&& std::find(task.alarms.begin(), task.alarms.end(), minutes)
					== task.alarms.end())
				task.alarms.push_back(minutes);
		}

		const Property* modified = component->Find("LAST-MODIFIED");
		if (modified == nullptr)
			modified = component->Find("DTSTAMP");
		if (modified != nullptr) {
			TimeValue value = ParseTime(*modified, &calendar);
			if (value.valid)
				task.updated = value.instant;
		}
		const Property* created = component->Find("CREATED");
		if (created != nullptr) {
			TimeValue value = ParseTime(*created, &calendar);
			if (value.valid)
				task.created = value.instant;
		}

		task.id = KeyOf(keepIds, task.uid + "|");
		task.raw = Serialize(calendar);
		result.push_back(task);
	}
	return result;
}


std::string
TaskToCalendar(const std::string& existing, const Task& task,
	const std::string& parentUid)
{
	Component root;
	ParseExisting(existing, root);

	std::string uid = Trim(task.uid);
	if (uid.empty()) {
		for (const Component& child : root.children) {
			if (child.name == "VTODO" && !ValueOf(child, "UID").empty()) {
				uid = Trim(ValueOf(child, "UID"));
				break;
			}
		}
	}
	if (uid.empty())
		uid = NewId() + "@aircal";

	size_t index = root.children.size();
	for (size_t i = 0; i < root.children.size(); i++) {
		const Component& child = root.children[i];
		if (child.name == "VTODO" && child.Find("RECURRENCE-ID") == nullptr
			&& Trim(ValueOf(child, "UID")) == uid) {
			index = i;
			break;
		}
	}
	bool isNew = index == root.children.size();
	if (isNew) {
		Component todo;
		todo.name = "VTODO";
		root.children.push_back(todo);
	}
	Component& component = root.children[index];
	component.Set("UID", uid);
	StampComponent(component, isNew);
	SetOrRemoveText(component, "SUMMARY", task.title);
	SetOrRemoveText(component, "DESCRIPTION", task.notes);
	WriteDue(component, task);

	// DTSTART after the new DUE makes the resource invalid.
	const Property* start = component.Find("DTSTART");
	if (start != nullptr && task.hasDue) {
		TimeValue value = ParseTime(*start, &root);
		Instant due = task.dueHasTime ? task.dueTime : StartOfDay(task.dueDate);
		if (value.valid && value.instant > due)
			component.Remove("DTSTART");
	}

	if (task.completed) {
		component.Set("STATUS", "COMPLETED");
		component.Set("COMPLETED",
			FormatBasicUtc(task.completedAt != 0 ? task.completedAt : Now()));
		component.Set("PERCENT-COMPLETE", "100");
	} else {
		std::string status = Upper(Trim(ValueOf(component, "STATUS")));
		if (status != "IN-PROCESS" && status != "CANCELLED")
			component.Set("STATUS", "NEEDS-ACTION");
		component.Remove("COMPLETED");
		if (Trim(ValueOf(component, "PERCENT-COMPLETE")) == "100")
			component.Remove("PERCENT-COMPLETE");
	}

	static const int kPriorities[] = {0, 9, 5, 3, 1};
	if (task.priority >= 1 && task.priority <= 4)
		component.Set("PRIORITY", std::to_string(kPriorities[task.priority]));
	else
		component.Remove("PRIORITY");

	std::vector<Property> properties;
	for (Property& property : component.properties) {
		if (property.name == "RELATED-TO") {
			std::string type = Upper(property.Param("RELTYPE"));
			if (type.empty() || type == "PARENT")
				continue;
		}
		properties.push_back(property);
	}
	component.properties.swap(properties);
	if (!parentUid.empty())
		component.Add("RELATED-TO", parentUid);

	if (task.labels.empty())
		component.Remove("CATEGORIES");
	else {
		std::string categories;
		for (size_t i = 0; i < task.labels.size(); i++) {
			if (i > 0)
				categories += ",";
			categories += EscapeText(task.labels[i]);
		}
		component.Set("CATEGORIES", categories);
	}

	std::string rule = Trim(task.rrule);
	if (StartsWithIgnoreCase(rule, "RRULE:"))
		rule = rule.substr(6);
	if (rule.empty())
		component.Remove("RRULE");
	else
		component.Set("RRULE", rule);

	Instant reference = task.dueHasTime ? task.dueTime
		: task.hasDue ? StartOfDay(task.dueDate) : 0;
	WriteAlarms(component, task.alarms, task.title, task.hasDue, reference, 0,
		&root);
	return Serialize(root);
}


// --- Files -----------------------------------------------------------------------------

std::vector<Event>
ImportEvents(const std::string& text)
{
	std::vector<Event> result;
	Component root;
	if (!Parse(text, root))
		return result;
	if (root.name != "VCALENDAR") {
		Component wrapper = NewCalendar();
		wrapper.children.push_back(root);
		root = wrapper;
	}

	// Group the events by UID, keeping the file's order.
	std::vector<std::string> order;
	std::map<std::string, std::vector<const Component*>> groups;
	std::vector<Component> unnamed;
	for (const Component& child : root.children) {
		if (child.name != "VEVENT")
			continue;
		std::string uid = Trim(ValueOf(child, "UID"));
		if (uid.empty()) {
			Component copy = child;
			copy.Set("UID", NewId() + "@aircal");
			unnamed.push_back(copy);
			continue;
		}
		if (groups.find(uid) == groups.end())
			order.push_back(uid);
		groups[uid].push_back(&child);
	}

	auto convert = [&](const std::vector<const Component*>& events) {
		Component calendar;
		calendar.name = "VCALENDAR";
		calendar.properties = root.properties;
		for (const Component& child : root.children) {
			if (child.name == "VTIMEZONE")
				calendar.children.push_back(child);
		}
		for (const Component* event : events)
			calendar.children.push_back(*event);
		std::vector<Event> converted = EventsFromCalendar(calendar);
		result.insert(result.end(), converted.begin(), converted.end());
	};
	for (const std::string& uid : order)
		convert(groups[uid]);
	for (const Component& event : unnamed)
		convert(std::vector<const Component*>(1, &event));
	return result;
}


std::string
ExportEvents(const std::vector<const Event*>& events)
{
	Component root = NewCalendar();
	std::vector<Component> zones;
	std::set<std::string> zoneIds;
	std::vector<Component> components;

	std::set<std::string> masterIds;
	for (const Event* event : events) {
		if (event != nullptr && !event->IsOverride())
			masterIds.insert(event->id);
	}

	for (const Event* event : events) {
		if (event == nullptr)
			continue;
		if (event->IsOverride() && masterIds.count(event->masterId) != 0)
			continue;	// written with its master
		std::string text;
		if (event->IsOverride()) {
			// The master is not exported: the instance becomes a plain event.
			Event single = *event;
			single.masterId.clear();
			single.rrule.clear();
			text = EventsToCalendar("", single, std::vector<const Event*>());
		} else {
			std::vector<const Event*> overrides;
			for (const Event* other : events) {
				if (other != nullptr && other != event && other->IsOverride()
					&& other->masterId == event->id)
					overrides.push_back(other);
			}
			// raw may be provider JSON: EventsToCalendar starts afresh then.
			text = EventsToCalendar(event->raw, *event, overrides);
		}
		Component part;
		if (!Parse(text, part))
			continue;
		for (const Component& child : part.children) {
			if (child.name == "VTIMEZONE") {
				std::string id = Trim(ValueOf(child, "TZID"));
				if (zoneIds.insert(id).second)
					zones.push_back(child);
			} else if (child.name == "VEVENT")
				components.push_back(child);
		}
	}
	root.children = zones;
	root.children.insert(root.children.end(), components.begin(),
		components.end());
	return Serialize(root);
}

}	// namespace ical
}	// namespace aircal
