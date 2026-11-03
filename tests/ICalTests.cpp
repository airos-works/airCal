#include "Test.h"
#include "core/ICal.h"
#include "core/Recurrence.h"

#include <algorithm>

using namespace aircal;
using namespace aircal::ical;

namespace {

const char* kNewYork = "America/New_York";


std::string
Crlf(const std::string& text)
{
	std::string result;
	for (char c : text) {
		if (c == '\n')
			result += "\r\n";
		else
			result += c;
	}
	return result;
}


const Component*
Child(const Component& parent, const std::string& name, int index = 0)
{
	for (const Component& child : parent.children) {
		if (child.name == name && index-- == 0)
			return &child;
	}
	return nullptr;
}


int
CountChildren(const Component& parent, const std::string& name)
{
	int count = 0;
	for (const Component& child : parent.children)
		count += child.name == name ? 1 : 0;
	return count;
}


// The VEVENT with (or, for "", without) a RECURRENCE-ID value.
const Component*
EventWithRecurrence(const Component& calendar, const std::string& recurrence)
{
	for (const Component& child : calendar.children) {
		if (child.name != "VEVENT")
			continue;
		const Property* id = child.Find("RECURRENCE-ID");
		if (recurrence.empty() ? id == nullptr
				: id != nullptr && id->value == recurrence)
			return &child;
	}
	return nullptr;
}


const char* kMeeting =
	"BEGIN:VCALENDAR\n"
	"VERSION:2.0\n"
	"PRODID:-//Example//Server//EN\n"
	"X-WR-CALNAME:Work\n"
	"BEGIN:VTIMEZONE\n"
	"TZID:America/New_York\n"
	"BEGIN:STANDARD\n"
	"DTSTART:19701101T020000\n"
	"TZOFFSETFROM:-0400\n"
	"TZOFFSETTO:-0500\n"
	"END:STANDARD\n"
	"END:VTIMEZONE\n"
	"BEGIN:VEVENT\n"
	"UID:standup-42@example.com\n"
	"DTSTAMP:20261001T120000Z\n"
	"LAST-MODIFIED:20261002T120000Z\n"
	"SUMMARY:Stand-up\\, daily\n"
	"DESCRIPTION:Line one\\nLine two\\; done\n"
	"LOCATION:Room 4\n"
	"DTSTART;TZID=\"America/New_York\":20261026T090000\n"
	"DURATION:PT15M\n"
	"RRULE:FREQ=WEEKLY;BYDAY=MO,WE,FR\n"
	"EXDATE;TZID=America/New_York:20261028T090000,20261030T090000\n"
	"EXDATE;TZID=America/New_York:20261102T090000\n"
	"ORGANIZER;CN=\"Boss, The\":mailto:boss@example.com\n"
	"ATTENDEE;CN=Ann;ROLE=REQ-PARTICIPANT;PARTSTAT=ACCEPTED:mailto:ann@example.com\n"
	"ATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:bob@example.com\n"
	"X-MOZ-GENERATION:7\n"
	"SEQUENCE:3\n"
	"BEGIN:VALARM\n"
	"ACTION:DISPLAY\n"
	"DESCRIPTION:Reminder\n"
	"TRIGGER:-PT15M\n"
	"END:VALARM\n"
	"BEGIN:VALARM\n"
	"ACTION:EMAIL\n"
	"SUMMARY:Mail\n"
	"DESCRIPTION:Mail\n"
	"ATTENDEE:mailto:me@example.com\n"
	"TRIGGER;RELATED=START:-P1D\n"
	"END:VALARM\n"
	"END:VEVENT\n"
	"BEGIN:VEVENT\n"
	"UID:standup-42@example.com\n"
	"RECURRENCE-ID;TZID=America/New_York:20261104T090000\n"
	"DTSTAMP:20261001T120000Z\n"
	"SUMMARY:Stand-up (moved)\n"
	"DTSTART;TZID=America/New_York:20261104T110000\n"
	"DTEND;TZID=America/New_York:20261104T111500\n"
	"X-KEEP-ME:yes\n"
	"END:VEVENT\n"
	"END:VCALENDAR\n";

}	// namespace


TEST(ICalUnfoldAndFold)
{
	Component root;
	CHECK(Parse(Crlf("BEGIN:VCALENDAR\n"
		"BEGIN:VEVENT\n"
		"SUMMARY:A long\n"
		" summary that\n"
		"\t continues\n"
		"description:lower case name\n"
		"END:VEVENT\n"
		"END:VCALENDAR\n"), root));
	CHECK_EQ(root.name, std::string("VCALENDAR"));
	const Component* event = Child(root, "VEVENT");
	CHECK(event != nullptr);
	if (event == nullptr)
		return;
	CHECK_EQ(event->Find("SUMMARY")->value,
		std::string("A longsummary that continues"));
	CHECK(event->Find("DESCRIPTION") != nullptr);

	// Multibyte text folds at 75 octets without splitting UTF-8 sequences.
	std::string text;
	for (int i = 0; i < 20; i++)
		text += "Grüße 日本語 ";
	Component calendar;
	calendar.name = "VCALENDAR";
	Component vevent;
	vevent.name = "VEVENT";
	vevent.Add("SUMMARY", EscapeText(text));
	calendar.children.push_back(vevent);
	std::string serialized = Serialize(calendar);
	size_t start = 0;
	int continuations = 0;
	while (start < serialized.size()) {
		size_t end = serialized.find("\r\n", start);
		CHECK(end != std::string::npos);
		if (end == std::string::npos)
			break;
		std::string line = serialized.substr(start, end - start);
		CHECK(line.size() <= 75);
		if (!line.empty() && line[0] == ' ') {
			continuations++;
			CHECK(line.size() < 2 || ((unsigned char)line[1] & 0xC0) != 0x80);
		}
		CHECK(line.empty() || ((unsigned char)line.back() & 0xC0) != 0xC0);
		start = end + 2;
	}
	CHECK(continuations > 3);
	Component back;
	CHECK(Parse(serialized, back));
	const Component* again = Child(back, "VEVENT");
	CHECK(again != nullptr);
	if (again != nullptr)
		CHECK_EQ(UnescapeText(again->Find("SUMMARY")->value), text);

	// Structural errors.
	CHECK(!Parse("BEGIN:VCALENDAR\nEND:VEVENT\n", root));
	CHECK(!Parse("just text", root));
}


TEST(ICalParameters)
{
	Component root;
	CHECK(Parse(Crlf("BEGIN:VEVENT\n"
		"DTSTART;TZID=\"America/New_York\":20261103T090000\n"
		"ATTENDEE;CN=\"Doe, Jane; PhD\";DELEGATED-FROM=\"mailto:a@x.com\",\"mailto:b@x.com\""
		";x-note=\"a:b\":mailto:jane@x.com\n"
		"END:VEVENT\n"), root));
	const Property* start = root.Find("DTSTART");
	CHECK(start != nullptr);
	if (start != nullptr) {
		CHECK_EQ(start->Param("tzid"), std::string("America/New_York"));
		CHECK_EQ(start->value, std::string("20261103T090000"));
	}
	const Property* attendee = root.Find("ATTENDEE");
	CHECK(attendee != nullptr);
	if (attendee == nullptr)
		return;
	CHECK_EQ(attendee->Param("CN"), std::string("Doe, Jane; PhD"));
	CHECK_EQ(attendee->Param("X-NOTE"), std::string("a:b"));
	CHECK_EQ(attendee->Param("DELEGATED-FROM"),
		std::string("mailto:a@x.com,mailto:b@x.com"));
	CHECK_EQ(attendee->params.size(), (size_t)4);
	CHECK_EQ(attendee->value, std::string("mailto:jane@x.com"));

	std::string text = Serialize(root);
	for (size_t fold; (fold = text.find("\r\n ")) != std::string::npos;)
		text.erase(fold, 3);
	CHECK(text.find("CN=\"Doe, Jane; PhD\"") != std::string::npos);
	CHECK(text.find("DELEGATED-FROM=\"mailto:a@x.com\",\"mailto:b@x.com\"")
		!= std::string::npos);
	CHECK(text.find("TZID=America/New_York:") != std::string::npos);

	Property property;
	property.SetParam("value", "DATE");
	property.SetParam("VALUE", "DATE-TIME");
	CHECK_EQ(property.params.size(), (size_t)1);
	CHECK_EQ(property.Param("VALUE"), std::string("DATE-TIME"));
	property.RemoveParam("Value");
	CHECK(property.params.empty());

	Component component;
	component.Add("X-A", "1");
	component.Add("x-a", "2");
	component.Add("X-B", "3");
	CHECK_EQ(component.FindAll("X-A").size(), (size_t)2);
	component.Set("X-A", "4");
	CHECK_EQ(component.FindAll("X-A").size(), (size_t)1);
	CHECK_EQ(component.Find("X-A")->value, std::string("4"));
	component.Remove("X-B");
	CHECK(component.Find("X-B") == nullptr);
}


TEST(ICalTextEscaping)
{
	std::string text = "a,b;c\\d\ne";
	CHECK_EQ(EscapeText(text), std::string("a\\,b\\;c\\\\d\\ne"));
	CHECK_EQ(UnescapeText(EscapeText(text)), text);
	CHECK_EQ(UnescapeText("x\\Ny\\,z"), std::string("x\ny,z"));
}


TEST(ICalParseTime)
{
	Property date;
	date.name = "DTSTART";
	date.value = "20261103";
	TimeValue value = ParseTime(date, nullptr);
	CHECK(value.valid && value.dateOnly);
	CHECK(value.date == Date(2026, 11, 3));

	date.SetParam("VALUE", "DATE");
	CHECK(ParseTime(date, nullptr).dateOnly);

	Property utc;
	utc.value = "20261103T140000Z";
	value = ParseTime(utc, nullptr);
	CHECK(value.valid && !value.dateOnly && !value.floating);
	CHECK_EQ(value.tzid, std::string("UTC"));
	CHECK_EQ(value.instant, DateTime(Date(2026, 11, 3), 14).AsUtcSeconds());

	Property zoned;
	zoned.value = "20261103T090000";
	zoned.SetParam("TZID", "America/New_York");
	value = ParseTime(zoned, nullptr);
	CHECK_EQ(value.tzid, std::string(kNewYork));
	CHECK_EQ(FormatBasicUtc(value.instant), std::string("20261103T140000Z"));

	// Outlook writes Windows zone names.
	zoned.SetParam("TZID", "Eastern Standard Time");
	value = ParseTime(zoned, nullptr);
	CHECK_EQ(value.tzid, std::string(kNewYork));
	CHECK_EQ(FormatBasicUtc(value.instant), std::string("20261103T140000Z"));

	// An unknown TZID uses the resource's VTIMEZONE.
	Component calendar;
	CHECK(Parse(Crlf("BEGIN:VCALENDAR\n"
		"BEGIN:VTIMEZONE\n"
		"TZID:Custom Zone 1\n"
		"BEGIN:DAYLIGHT\n"
		"TZOFFSETTO:+1100\n"
		"END:DAYLIGHT\n"
		"BEGIN:STANDARD\n"
		"TZOFFSETTO:+1000\n"
		"END:STANDARD\n"
		"END:VTIMEZONE\n"
		"BEGIN:VTIMEZONE\n"
		"TZID:Custom Zone 2\n"
		"BEGIN:STANDARD\n"
		"TZOFFSETTO:+0530\n"
		"END:STANDARD\n"
		"END:VTIMEZONE\n"
		"END:VCALENDAR\n"), calendar));
	zoned.SetParam("TZID", "Custom Zone 1");
	value = ParseTime(zoned, &calendar);
	CHECK_EQ(FormatBasicUtc(value.instant), std::string("20261102T230000Z"));
	CHECK_EQ(value.tzid, std::string("Etc/GMT-10"));
	zoned.SetParam("TZID", "Custom Zone 2");
	value = ParseTime(zoned, &calendar);
	CHECK_EQ(FormatBasicUtc(value.instant), std::string("20261103T033000Z"));
	// ... and the local zone without one.
	zoned.SetParam("TZID", "Nowhere/Special");
	value = ParseTime(zoned, &calendar);
	CHECK(value.valid);
	CHECK_EQ(FormatBasicUtc(value.instant), std::string("20261103T140000Z"));

	Property floating;
	floating.value = "20260703T090000";
	value = ParseTime(floating, nullptr);
	CHECK(value.floating);
	CHECK_EQ(value.tzid, std::string());
	CHECK_EQ(FormatBasicUtc(value.instant), std::string("20260703T130000Z"));

	Property bad;
	bad.value = "tomorrow";
	CHECK(!ParseTime(bad, nullptr).valid);
}


TEST(ICalEventsFromCalendar)
{
	Component calendar;
	CHECK(Parse(kMeeting, calendar));
	std::vector<Event> events = EventsFromCalendar(calendar);
	CHECK_EQ(events.size(), (size_t)2);
	if (events.size() != 2)
		return;
	const Event& master = events[0];
	CHECK_EQ(master.uid, std::string("standup-42@example.com"));
	CHECK_EQ(master.title, std::string("Stand-up, daily"));
	CHECK_EQ(master.notes, std::string("Line one\nLine two; done"));
	CHECK_EQ(master.location, std::string("Room 4"));
	CHECK(!master.allDay);
	CHECK_EQ(master.tzid, std::string(kNewYork));
	CHECK_EQ(FormatBasicUtc(master.start), std::string("20261026T130000Z"));
	CHECK_EQ(master.end - master.start, (Instant)(15 * 60));
	CHECK_EQ(master.rrule, std::string("FREQ=WEEKLY;BYDAY=MO,WE,FR"));
	CHECK_EQ(master.exdates.size(), (size_t)3);
	CHECK_EQ(master.organizer, std::string("Boss, The <boss@example.com>"));
	CHECK_EQ(master.attendees.size(), (size_t)2);
	if (master.attendees.size() == 2) {
		CHECK_EQ(master.attendees[0], std::string("Ann <ann@example.com>"));
		CHECK_EQ(master.attendees[1], std::string("bob@example.com"));
	}
	std::vector<int> alarms = master.alarms;
	std::sort(alarms.begin(), alarms.end());
	CHECK_EQ(alarms.size(), (size_t)2);
	if (alarms.size() == 2) {
		CHECK_EQ(alarms[0], 15);
		CHECK_EQ(alarms[1], 1440);
	}
	CHECK_EQ(FormatBasicUtc(master.updated), std::string("20261002T120000Z"));
	CHECK(!master.raw.empty());
	CHECK(master.masterId.empty());
	CHECK_EQ(EventKey(master), std::string("standup-42@example.com|"));

	const Event& override = events[1];
	CHECK_EQ(override.masterId, master.id);
	CHECK(override.id != master.id);
	CHECK_EQ(override.title, std::string("Stand-up (moved)"));
	CHECK_EQ(FormatBasicUtc(override.recurrenceId), std::string("20261104T140000Z"));
	CHECK_EQ(FormatBasicUtc(override.start), std::string("20261104T160000Z"));
	CHECK_EQ(EventKey(override),
		std::string("standup-42@example.com|20261104T140000Z"));
	CHECK(override.raw.empty());

	// Expanded: the exdates are gone, the override replaces its instance.
	std::vector<Occurrence> occurrences = ExpandEvent(master, {&override},
		StartOfDay(Date(2026, 10, 26)), StartOfDay(Date(2026, 11, 7)));
	// Mon/Wed/Fri: 10-26, (10-28, 10-30, 11-02 excluded), 11-04 moved, 11-06.
	CHECK_EQ(occurrences.size(), (size_t)3);
	if (occurrences.size() == 3) {
		CHECK(occurrences[0].event == &master);
		CHECK(occurrences[1].event == &override);
		CHECK_EQ(ToWall(occurrences[1].start, kNewYork).hour, 11);
		CHECK(occurrences[2].startDate == Date(2026, 11, 6));
		CHECK_EQ(ToWall(occurrences[2].start, kNewYork).hour, 9);
	}

	// Ids are kept by key.
	std::map<std::string, std::string> keep;
	keep[EventKey(master)] = "local-master";
	keep[EventKey(override)] = "local-override";
	std::vector<Event> again = EventsFromCalendar(calendar, &keep);
	CHECK_EQ(again.size(), (size_t)2);
	if (again.size() == 2) {
		CHECK_EQ(again[0].id, std::string("local-master"));
		CHECK_EQ(again[1].id, std::string("local-override"));
		CHECK_EQ(again[1].masterId, std::string("local-master"));
	}
}


TEST(ICalEventDefaults)
{
	Component calendar;
	CHECK(Parse(Crlf("BEGIN:VCALENDAR\n"
		"BEGIN:VEVENT\n"
		"UID:holiday\n"
		"DTSTART;VALUE=DATE:20261225\n"
		"STATUS:TENTATIVE\n"
		"EXDATE;VALUE=DATE:20271225\n"
		"RRULE:FREQ=YEARLY\n"
		"END:VEVENT\n"
		"BEGIN:VEVENT\n"
		"UID:trip\n"
		"DTSTART;VALUE=DATE:20261228\n"
		"DTEND;VALUE=DATE:20270102\n"
		"END:VEVENT\n"
		"BEGIN:VEVENT\n"
		"UID:moment\n"
		"DTSTART:20261103T140000Z\n"
		"BEGIN:VALARM\n"
		"ACTION:DISPLAY\n"
		"TRIGGER;VALUE=DATE-TIME:20261103T133000Z\n"
		"END:VALARM\n"
		"END:VEVENT\n"
		"END:VCALENDAR\n"), calendar));
	std::vector<Event> events = EventsFromCalendar(calendar);
	CHECK_EQ(events.size(), (size_t)3);
	if (events.size() != 3)
		return;
	CHECK(events[0].allDay);
	CHECK(events[0].startDate == Date(2026, 12, 25));
	CHECK(events[0].endDate == Date(2026, 12, 26));
	CHECK(events[0].tentative);
	CHECK_EQ(events[0].exdateDays.size(), (size_t)1);
	CHECK(events[1].endDate == Date(2027, 1, 2));
	CHECK(!events[2].allDay);
	CHECK_EQ(events[2].tzid, std::string("UTC"));
	CHECK_EQ(events[2].end, events[2].start);
	CHECK_EQ(events[2].alarms.size(), (size_t)1);
	if (!events[2].alarms.empty())
		CHECK_EQ(events[2].alarms[0], 30);
}


TEST(ICalRoundTripKeepsUnknownData)
{
	Component calendar;
	CHECK(Parse(kMeeting, calendar));
	std::vector<Event> events = EventsFromCalendar(calendar);
	CHECK_EQ(events.size(), (size_t)2);
	if (events.size() != 2)
		return;
	Event master = events[0];
	Event override = events[1];
	master.title = "Stand-up; renamed";
	master.alarms = {10};
	master.exdates.pop_back();
	override.location = "Lobby";

	std::string text = EventsToCalendar(master.raw, master, {&override});
	Component written;
	CHECK(Parse(text, written));
	CHECK(text.find("\r\n") != std::string::npos);
	CHECK(written.Find("X-WR-CALNAME") != nullptr);
	CHECK_EQ(written.Find("PRODID")->value, std::string("-//Example//Server//EN"));
	CHECK_EQ(CountChildren(written, "VTIMEZONE"), 1);
	const Component* vevent = EventWithRecurrence(written, "");
	CHECK(vevent != nullptr);
	if (vevent == nullptr)
		return;
	CHECK_EQ(vevent->Find("SUMMARY")->value, std::string("Stand-up\\; renamed"));
	CHECK(vevent->Find("X-MOZ-GENERATION") != nullptr);
	CHECK_EQ(vevent->FindAll("ATTENDEE").size(), (size_t)2);
	CHECK_EQ(vevent->Find("ATTENDEE")->Param("PARTSTAT"), std::string("ACCEPTED"));
	CHECK(vevent->Find("ORGANIZER") != nullptr);
	CHECK(vevent->Find("DURATION") == nullptr);
	CHECK_EQ(vevent->Find("DTSTART")->Param("TZID"), std::string(kNewYork));
	CHECK_EQ(vevent->Find("DTSTART")->value, std::string("20261026T090000"));
	CHECK_EQ(vevent->Find("DTEND")->value, std::string("20261026T091500"));
	CHECK_EQ(vevent->Find("SEQUENCE")->value, std::string("4"));
	CHECK_EQ(vevent->FindAll("EXDATE").size(), (size_t)2);
	// Our DISPLAY alarm replaced theirs; the EMAIL alarm survives.
	CHECK_EQ(CountChildren(*vevent, "VALARM"), 2);
	bool email = false, display = false;
	for (const Component& alarm : vevent->children) {
		std::string action = alarm.Find("ACTION")->value;
		if (action == "EMAIL")
			email = alarm.Find("ATTENDEE") != nullptr;
		if (action == "DISPLAY")
			display = alarm.Find("TRIGGER")->value == "-PT10M";
	}
	CHECK(email && display);

	const Component* moved = EventWithRecurrence(written, "20261104T090000");
	CHECK(moved != nullptr);
	if (moved != nullptr) {
		CHECK(moved->Find("X-KEEP-ME") != nullptr);
		CHECK_EQ(moved->Find("LOCATION")->value, std::string("Lobby"));
		CHECK_EQ(moved->Find("RECURRENCE-ID")->Param("TZID"), std::string(kNewYork));
		CHECK(moved->Find("RRULE") == nullptr);
	}

	// Reading it back gives the same event.
	std::vector<Event> back = EventsFromCalendar(written);
	CHECK_EQ(back.size(), (size_t)2);
	if (back.size() == 2) {
		CHECK_EQ(back[0].title, master.title);
		CHECK_EQ(back[0].start, master.start);
		CHECK_EQ(back[0].end, master.end);
		CHECK_EQ(back[0].rrule, master.rrule);
		CHECK_EQ(back[0].exdates.size(), (size_t)2);
		CHECK_EQ(back[0].attendees.size(), (size_t)2);
		CHECK_EQ(back[1].recurrenceId, override.recurrenceId);
	}
}


TEST(ICalCancelledOverrideBecomesExdate)
{
	Component calendar;
	CHECK(Parse(kMeeting, calendar));
	std::vector<Event> events = EventsFromCalendar(calendar);
	if (events.size() != 2)
		return;
	Event override = events[1];
	override.cancelled = true;
	std::string text = EventsToCalendar(events[0].raw, events[0], {&override});
	Component written;
	CHECK(Parse(text, written));
	CHECK_EQ(CountChildren(written, "VEVENT"), 1);
	const Component* master = EventWithRecurrence(written, "");
	CHECK(master != nullptr);
	if (master == nullptr)
		return;
	bool found = false;
	for (const Property* exdate : master->FindAll("EXDATE"))
		found = found || exdate->value == "20261104T090000";
	CHECK(found);

	// Overrides no longer listed are removed.
	text = EventsToCalendar(events[0].raw, events[0], {});
	CHECK(Parse(text, written));
	CHECK_EQ(CountChildren(written, "VEVENT"), 1);
}


TEST(ICalNewEvent)
{
	Event event;
	event.title = "Lunch, with Sam";
	event.tzid = "Australia/Hobart";
	event.start = ToInstant(DateTime(Date(2026, 11, 3), 12, 30), event.tzid);
	event.end = event.start + 3600;
	event.alarms = {0, 60};
	event.rrule = "RRULE:FREQ=WEEKLY";
	std::string text = EventsToCalendar("", event, {});
	Component calendar;
	CHECK(Parse(text, calendar));
	CHECK_EQ(calendar.Find("VERSION")->value, std::string("2.0"));
	CHECK_EQ(calendar.Find("PRODID")->value,
		std::string("-//airos-works//airCal//EN"));
	CHECK_EQ(calendar.Find("CALSCALE")->value, std::string("GREGORIAN"));
	const Component* vevent = Child(calendar, "VEVENT");
	CHECK(vevent != nullptr);
	if (vevent == nullptr)
		return;
	std::string uid = vevent->Find("UID")->value;
	CHECK(uid.size() > 7 && uid.substr(uid.size() - 7) == "@aircal");
	CHECK_EQ(vevent->Find("DTSTART")->Param("TZID"), std::string("Australia/Hobart"));
	CHECK_EQ(vevent->Find("DTSTART")->value, std::string("20261103T123000"));
	CHECK_EQ(vevent->Find("RRULE")->value, std::string("FREQ=WEEKLY"));
	CHECK_EQ(vevent->Find("SEQUENCE")->value, std::string("0"));
	CHECK(vevent->Find("DTSTAMP") != nullptr);
	CHECK_EQ(CountChildren(*vevent, "VALARM"), 2);

	// Floating and UTC times are written in UTC; all-day as DATE.
	Event floating = event;
	floating.tzid = "";
	floating.uid = "f@test";
	text = EventsToCalendar("", floating, {});
	CHECK(text.find("DTSTART:" + FormatBasicUtc(event.start)) != std::string::npos);

	Event allDay;
	allDay.uid = "a@test";
	allDay.allDay = true;
	allDay.startDate = Date(2026, 12, 24);
	allDay.endDate = Date(2026, 12, 27);
	allDay.rrule = "FREQ=YEARLY";
	allDay.exdateDays.push_back(Date(2027, 12, 24));
	Event cancelled;
	cancelled.masterId = "x";
	cancelled.allDay = true;
	cancelled.cancelled = true;
	cancelled.recurrenceDate = Date(2028, 12, 24);
	Event moved;
	moved.masterId = "x";
	moved.allDay = true;
	moved.recurrenceDate = Date(2029, 12, 24);
	moved.startDate = Date(2029, 12, 23);
	moved.endDate = Date(2029, 12, 24);
	text = EventsToCalendar("", allDay, {&cancelled, &moved});
	CHECK(text.find("DTSTART;VALUE=DATE:20261224") != std::string::npos);
	CHECK(text.find("DTEND;VALUE=DATE:20261227") != std::string::npos);
	CHECK(text.find("EXDATE;VALUE=DATE:20271224") != std::string::npos);
	CHECK(text.find("EXDATE;VALUE=DATE:20281224") != std::string::npos);
	CHECK(text.find("RECURRENCE-ID;VALUE=DATE:20291224") != std::string::npos);
	std::vector<Event> back = ImportEvents(text);
	CHECK_EQ(back.size(), (size_t)2);
	if (back.size() == 2) {
		CHECK(back[1].recurrenceDate == Date(2029, 12, 24));
		CHECK(back[1].startDate == Date(2029, 12, 23));
		CHECK_EQ(EventKey(back[1]), std::string("a@test|20291224"));
	}
}


TEST(ICalOnlyOverrides)
{
	Component calendar;
	CHECK(Parse(Crlf("BEGIN:VCALENDAR\n"
		"BEGIN:VEVENT\n"
		"UID:series@example.com\n"
		"RECURRENCE-ID:20261110T150000Z\n"
		"DTSTART:20261110T160000Z\n"
		"DTEND:20261110T170000Z\n"
		"SUMMARY:One instance\n"
		"END:VEVENT\n"
		"END:VCALENDAR\n"), calendar));
	std::vector<Event> events = EventsFromCalendar(calendar);
	CHECK_EQ(events.size(), (size_t)1);
	if (events.size() == 1) {
		CHECK(events[0].masterId.empty());
		CHECK(!events[0].raw.empty());
		CHECK_EQ(EventKey(events[0]),
			std::string("series@example.com|20261110T150000Z"));
	}
}


TEST(ICalTasks)
{
	Component calendar;
	CHECK(Parse(Crlf("BEGIN:VCALENDAR\n"
		"VERSION:2.0\n"
		"BEGIN:VTODO\n"
		"UID:task-1\n"
		"SUMMARY:Buy milk\n"
		"DESCRIPTION:2 litres\n"
		"DUE:20261104T030000Z\n"
		"PRIORITY:1\n"
		"RELATED-TO:parent-uid\n"
		"CATEGORIES:home,shop\\,ping\n"
		"CATEGORIES:errands\n"
		"RRULE:FREQ=WEEKLY\n"
		"X-APPLE-SORT-ORDER:12345\n"
		"BEGIN:VALARM\n"
		"ACTION:DISPLAY\n"
		"TRIGGER;RELATED=END:-PT30M\n"
		"END:VALARM\n"
		"END:VTODO\n"
		"BEGIN:VTODO\n"
		"UID:task-2\n"
		"SUMMARY:Done thing\n"
		"DUE;VALUE=DATE:20261101\n"
		"STATUS:COMPLETED\n"
		"COMPLETED:20261101T100000Z\n"
		"PRIORITY:7\n"
		"END:VTODO\n"
		"END:VCALENDAR\n"), calendar));
	std::vector<Task> tasks = TasksFromCalendar(calendar);
	CHECK_EQ(tasks.size(), (size_t)2);
	if (tasks.size() != 2)
		return;
	const Task& milk = tasks[0];
	CHECK_EQ(milk.title, std::string("Buy milk"));
	CHECK_EQ(milk.notes, std::string("2 litres"));
	CHECK(milk.hasDue && milk.dueHasTime);
	CHECK_EQ(FormatBasicUtc(milk.dueTime), std::string("20261104T030000Z"));
	CHECK(milk.dueDate == Date(2026, 11, 3));	// local (New York) date
	CHECK_EQ(milk.priority, 4);
	CHECK_EQ(milk.parentId, std::string("parent-uid"));
	CHECK_EQ(milk.labels.size(), (size_t)3);
	if (milk.labels.size() == 3)
		CHECK_EQ(milk.labels[1], std::string("shop,ping"));
	CHECK(milk.recurring);
	CHECK_EQ(milk.rrule, std::string("FREQ=WEEKLY"));
	CHECK_EQ(milk.alarms.size(), (size_t)1);
	CHECK(!milk.completed);

	const Task& done = tasks[1];
	CHECK(done.completed);
	CHECK_EQ(FormatBasicUtc(done.completedAt), std::string("20261101T100000Z"));
	CHECK(done.hasDue && !done.dueHasTime);
	CHECK(done.dueDate == Date(2026, 11, 1));
	CHECK_EQ(done.priority, 1);

	// Priority mapping both ways.
	const int kInput[] = {0, 1, 2, 4, 5, 6, 9};
	const int kMapped[] = {0, 4, 3, 3, 2, 1, 1};
	for (int i = 0; i < 7; i++) {
		Component one;
		Parse(Crlf("BEGIN:VCALENDAR\nBEGIN:VTODO\nUID:p\nPRIORITY:"
			+ std::to_string(kInput[i]) + "\nEND:VTODO\nEND:VCALENDAR\n"), one);
		std::vector<Task> parsed = TasksFromCalendar(one);
		CHECK_EQ(parsed.size(), (size_t)1);
		if (parsed.size() == 1)
			CHECK_EQ(parsed[0].priority, kMapped[i]);
	}

	Task edited = milk;
	edited.completed = true;
	edited.completedAt = DateTime(Date(2026, 11, 3), 12).AsUtcSeconds();
	edited.priority = 2;
	edited.labels = {"home"};
	edited.hasDue = true;
	edited.dueHasTime = false;
	edited.dueDate = Date(2026, 11, 5);
	edited.alarms.clear();
	std::string text = TaskToCalendar(milk.raw, edited, "new-parent");
	Component written;
	CHECK(Parse(text, written));
	CHECK_EQ(CountChildren(written, "VTODO"), 2);
	const Component* todo = Child(written, "VTODO");
	CHECK(todo != nullptr);
	if (todo == nullptr)
		return;
	CHECK_EQ(todo->Find("UID")->value, std::string("task-1"));
	CHECK_EQ(todo->Find("STATUS")->value, std::string("COMPLETED"));
	CHECK_EQ(todo->Find("COMPLETED")->value, std::string("20261103T120000Z"));
	CHECK_EQ(todo->Find("PERCENT-COMPLETE")->value, std::string("100"));
	CHECK_EQ(todo->Find("PRIORITY")->value, std::string("5"));
	CHECK_EQ(todo->Find("DUE")->value, std::string("20261105"));
	CHECK_EQ(todo->Find("DUE")->Param("VALUE"), std::string("DATE"));
	CHECK_EQ(todo->Find("RELATED-TO")->value, std::string("new-parent"));
	CHECK_EQ(todo->FindAll("RELATED-TO").size(), (size_t)1);
	CHECK_EQ(todo->FindAll("CATEGORIES").size(), (size_t)1);
	CHECK_EQ(todo->Find("CATEGORIES")->value, std::string("home"));
	CHECK(todo->Find("X-APPLE-SORT-ORDER") != nullptr);
	CHECK_EQ(CountChildren(*todo, "VALARM"), 0);

	// Reopening, no priority, timed due.
	edited.completed = false;
	edited.priority = 0;
	edited.dueHasTime = true;
	edited.dueTime = DateTime(Date(2026, 11, 6), 15).AsUtcSeconds();
	text = TaskToCalendar(text, edited, "");
	CHECK(Parse(text, written));
	todo = Child(written, "VTODO");
	CHECK_EQ(todo->Find("STATUS")->value, std::string("NEEDS-ACTION"));
	CHECK(todo->Find("COMPLETED") == nullptr);
	CHECK(todo->Find("PERCENT-COMPLETE") == nullptr);
	CHECK(todo->Find("PRIORITY") == nullptr);
	CHECK(todo->Find("RELATED-TO") == nullptr);
	CHECK_EQ(todo->Find("DUE")->value, std::string("20261106T150000Z"));

	// A new task.
	Task fresh;
	fresh.title = "New";
	fresh.priority = 4;
	text = TaskToCalendar("", fresh, "");
	std::vector<Task> parsed;
	CHECK(Parse(text, written));
	parsed = TasksFromCalendar(written);
	CHECK_EQ(parsed.size(), (size_t)1);
	if (parsed.size() == 1) {
		CHECK(!parsed[0].uid.empty());
		CHECK_EQ(parsed[0].priority, 4);
		CHECK(!parsed[0].hasDue);
	}
}


TEST(ICalImportExport)
{
	std::string file = kMeeting;
	// A second series and an event without a UID.
	file.insert(file.rfind("END:VCALENDAR"),
		"BEGIN:VEVENT\n"
		"UID:other@example.com\n"
		"DTSTART;VALUE=DATE:20261224\n"
		"SUMMARY:Eve\n"
		"END:VEVENT\n"
		"BEGIN:VEVENT\n"
		"DTSTART:20261201T100000Z\n"
		"SUMMARY:No uid\n"
		"END:VEVENT\n");
	std::vector<Event> events = ImportEvents(file);
	CHECK_EQ(events.size(), (size_t)4);
	if (events.size() != 4)
		return;
	CHECK_EQ(events[0].uid, std::string("standup-42@example.com"));
	CHECK_EQ(events[1].masterId, events[0].id);
	CHECK_EQ(events[2].title, std::string("Eve"));
	CHECK(events[2].masterId.empty());
	CHECK_EQ(events[3].title, std::string("No uid"));
	CHECK(!events[3].uid.empty());
	// Each master's raw is a resource of its own, with the time zones.
	Component resource;
	CHECK(Parse(events[0].raw, resource));
	CHECK_EQ(CountChildren(resource, "VEVENT"), 2);
	CHECK_EQ(CountChildren(resource, "VTIMEZONE"), 1);
	CHECK(Parse(events[2].raw, resource));
	CHECK_EQ(CountChildren(resource, "VEVENT"), 1);

	std::vector<const Event*> pointers;
	for (const Event& event : events)
		pointers.push_back(&event);
	std::string text = ExportEvents(pointers);
	Component exported;
	CHECK(Parse(text, exported));
	CHECK_EQ(CountChildren(exported, "VEVENT"), 4);
	CHECK_EQ(CountChildren(exported, "VTIMEZONE"), 1);
	CHECK_EQ(exported.children.front().name, std::string("VTIMEZONE"));
	std::vector<Event> again = ImportEvents(text);
	CHECK_EQ(again.size(), (size_t)4);
	if (again.size() == 4) {
		CHECK_EQ(again[1].masterId, again[0].id);
		CHECK_EQ(again[0].start, events[0].start);
		CHECK_EQ(again[1].start, events[1].start);
	}
}
