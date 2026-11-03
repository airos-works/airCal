#include "Test.h"
#include "core/Recurrence.h"

#include <chrono>

using namespace aircal;

namespace {

const char* kNewYork = "America/New_York";


Event
Timed(const Date& date, int hour, int minute, int lengthMinutes,
	const std::string& rrule, const std::string& tzid = kNewYork)
{
	Event event;
	event.id = "master";
	event.uid = "master@test";
	event.tzid = tzid;
	event.start = ToInstant(DateTime(date, hour, minute), tzid);
	event.end = event.start + lengthMinutes * 60;
	event.rrule = rrule;
	return event;
}


Event
AllDay(const Date& date, int days, const std::string& rrule)
{
	Event event;
	event.id = "master";
	event.allDay = true;
	event.startDate = date;
	event.endDate = date.AddDays(days);
	event.rrule = rrule;
	return event;
}


// The instance dates between two dates (the event's wall dates).
std::string
Dates(const Event& event, const Date& from, const Date& to,
	size_t maxCount = 5000)
{
	std::string result;
	for (Instant start : RuleInstances(event, StartOfDay(from), StartOfDay(to),
			maxCount)) {
		if (!result.empty())
			result += " ";
		result += event.allDay ? LocalDate(start).ToIso()
			: ToWall(start, event.tzid).date.ToIso();
	}
	return result;
}


std::string
Times(const std::vector<Occurrence>& occurrences, const std::string& tzid)
{
	std::string result;
	for (const Occurrence& occurrence : occurrences) {
		if (!result.empty())
			result += " ";
		DateTime wall = ToWall(occurrence.start, tzid);
		char buffer[32];
		snprintf(buffer, sizeof(buffer), "%02d-%02d@%02d:%02d", wall.date.month,
			wall.date.day, wall.hour, wall.minute);
		result += buffer;
	}
	return result;
}

}	// namespace


TEST(RecurrenceParseAndFormat)
{
	RecurrenceRule rule;
	CHECK(RecurrenceRule::Parse("RRULE:freq=Monthly;interval=2;byday=2TU,-1fr,+1MO,SA;"
		"bymonthday=-1,15;bysetpos=-1;wkst=su;X-FOO=bar;COUNT=4", rule));
	CHECK_EQ(rule.frequency, RecurrenceRule::Monthly);
	CHECK_EQ(rule.interval, 2);
	CHECK_EQ(rule.count, 4);
	CHECK_EQ(rule.byDay.size(), (size_t)4);
	CHECK_EQ(rule.byDay[0].weekday, 2);
	CHECK_EQ(rule.byDay[0].ordinal, 2);
	CHECK_EQ(rule.byDay[1].weekday, 5);
	CHECK_EQ(rule.byDay[1].ordinal, -1);
	CHECK_EQ(rule.byDay[2].ordinal, 1);
	CHECK_EQ(rule.byDay[3].ordinal, 0);
	CHECK_EQ(rule.byMonthDay.size(), (size_t)2);
	CHECK_EQ(rule.byMonthDay[0], -1);
	CHECK_EQ(rule.bySetPos[0], -1);
	CHECK_EQ(rule.weekStart, 0);
	CHECK_EQ(rule.ToString(), std::string("FREQ=MONTHLY;INTERVAL=2;COUNT=4;"
		"BYDAY=2TU,-1FR,1MO,SA;BYMONTHDAY=-1,15;BYSETPOS=-1;WKST=SU"));

	CHECK(RecurrenceRule::Parse("FREQ=WEEKLY;UNTIL=20261231T235959Z", rule));
	CHECK(rule.hasUntil && !rule.untilIsDate && !rule.untilFloating);
	CHECK_EQ(rule.until, DateTime(Date(2026, 12, 31), 23, 59, 59).AsUtcSeconds());
	CHECK_EQ(rule.ToString(), std::string("FREQ=WEEKLY;UNTIL=20261231T235959Z"));

	CHECK(RecurrenceRule::Parse("FREQ=DAILY;UNTIL=20261231", rule));
	CHECK(rule.untilIsDate);
	CHECK(rule.untilDate == Date(2026, 12, 31));
	CHECK_EQ(rule.ToString(), std::string("FREQ=DAILY;UNTIL=20261231"));

	CHECK(RecurrenceRule::Parse("FREQ=DAILY;UNTIL=20261231T090000", rule));
	CHECK(rule.untilFloating);
	CHECK_EQ(rule.ToString(), std::string("FREQ=DAILY;UNTIL=20261231T090000"));

	CHECK(!RecurrenceRule::Parse("INTERVAL=2", rule));
	CHECK(!RecurrenceRule::Parse("FREQ=SOMETIMES", rule));
	CHECK(!RecurrenceRule::Parse("", rule));
}


TEST(RecurrenceWeeklyAcrossDst)
{
	// Mondays and Wednesdays at 9:00 New York time across the November DST end.
	Event event = Timed(Date(2026, 10, 19), 9, 0, 60, "FREQ=WEEKLY;BYDAY=MO,WE");
	std::vector<Occurrence> occurrences = ExpandEvent(event, {},
		StartOfDay(Date(2026, 10, 19)), StartOfDay(Date(2026, 11, 12)));
	CHECK_EQ(Times(occurrences, kNewYork), std::string("10-19@09:00 10-21@09:00 "
		"10-26@09:00 10-28@09:00 11-02@09:00 11-04@09:00 11-09@09:00 11-11@09:00"));
	CHECK_EQ(occurrences.size(), (size_t)8);
	if (occurrences.size() == 8) {
		CHECK_EQ(FormatBasicUtc(occurrences[0].start),
			std::string("20261019T130000Z"));
		CHECK_EQ(FormatBasicUtc(occurrences[4].start),
			std::string("20261102T140000Z"));
		CHECK_EQ(occurrences[4].end - occurrences[4].start, (Instant)3600);
		CHECK(occurrences[4].event == &event);
		CHECK(occurrences[4].master == &event);
		CHECK_EQ(occurrences[4].originalStart, occurrences[4].start);
	}

	// The same rule in another zone stays at 9:00 there.
	Event hobart = Timed(Date(2026, 9, 28), 9, 0, 30, "FREQ=WEEKLY",
		"Australia/Hobart");
	std::vector<Occurrence> south = ExpandEvent(hobart, {},
		ToInstant(DateTime(Date(2026, 9, 27)), "Australia/Hobart"),
		ToInstant(DateTime(Date(2026, 10, 13)), "Australia/Hobart"));
	CHECK_EQ(Times(south, "Australia/Hobart"),
		std::string("09-28@09:00 10-05@09:00 10-12@09:00"));
}


TEST(RecurrenceMonthlyByDay)
{
	Event second = Timed(Date(2026, 1, 13), 18, 30, 60, "FREQ=MONTHLY;BYDAY=2TU");
	CHECK_EQ(Dates(second, Date(2026, 1, 1), Date(2026, 6, 1)),
		std::string("2026-01-13 2026-02-10 2026-03-10 2026-04-14 2026-05-12"));

	Event last = Timed(Date(2026, 1, 30), 16, 0, 30, "FREQ=MONTHLY;BYDAY=-1FR");
	CHECK_EQ(Dates(last, Date(2026, 1, 1), Date(2026, 5, 1)),
		std::string("2026-01-30 2026-02-27 2026-03-27 2026-04-24"));

	Event lastDay = Timed(Date(2028, 1, 31), 12, 0, 30, "FREQ=MONTHLY;BYMONTHDAY=-1");
	CHECK_EQ(Dates(lastDay, Date(2028, 1, 1), Date(2028, 5, 1)),
		std::string("2028-01-31 2028-02-29 2028-03-31 2028-04-30"));

	// The 31st only exists in some months (RFC 5545 skips the others).
	Event thirtyFirst = Timed(Date(2026, 1, 31), 12, 0, 30, "FREQ=MONTHLY");
	CHECK_EQ(Dates(thirtyFirst, Date(2026, 1, 1), Date(2026, 8, 1)),
		std::string("2026-01-31 2026-03-31 2026-05-31 2026-07-31"));

	// The last weekday of the month.
	Event lastWeekday = Timed(Date(2026, 1, 30), 17, 0, 15,
		"FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=-1");
	CHECK_EQ(Dates(lastWeekday, Date(2026, 1, 1), Date(2026, 7, 1)),
		std::string("2026-01-30 2026-02-27 2026-03-31 2026-04-30 2026-05-29 "
			"2026-06-30"));
}


TEST(RecurrenceYearly)
{
	Event yearly = Timed(Date(2026, 3, 15), 10, 0, 60, "FREQ=YEARLY");
	CHECK_EQ(Dates(yearly, Date(2026, 1, 1), Date(2029, 1, 1)),
		std::string("2026-03-15 2027-03-15 2028-03-15"));

	// February 29th recurs only in leap years.
	Event leap = AllDay(Date(2024, 2, 29), 1, "FREQ=YEARLY");
	CHECK_EQ(Dates(leap, Date(2024, 1, 1), Date(2034, 1, 1)),
		std::string("2024-02-29 2028-02-29 2032-02-29"));

	// Thanksgiving.
	Event thanks = AllDay(Date(2026, 11, 26), 1,
		"FREQ=YEARLY;BYMONTH=11;BYDAY=4TH");
	CHECK_EQ(Dates(thanks, Date(2026, 1, 1), Date(2029, 1, 1)),
		std::string("2026-11-26 2027-11-25 2028-11-23"));

	// Monday of ISO week 20.
	Event week = Timed(Date(2026, 5, 11), 9, 0, 60,
		"FREQ=YEARLY;BYWEEKNO=20;BYDAY=MO");
	CHECK_EQ(Dates(week, Date(2026, 1, 1), Date(2029, 1, 1)),
		std::string("2026-05-11 2027-05-17 2028-05-15"));

	// The 100th day of the year.
	Event yearDay = AllDay(Date(2026, 4, 10), 1, "FREQ=YEARLY;BYYEARDAY=100");
	CHECK_EQ(Dates(yearDay, Date(2026, 1, 1), Date(2029, 1, 1)),
		std::string("2026-04-10 2027-04-10 2028-04-09"));
}


TEST(RecurrenceCountAndUntil)
{
	Event daily = Timed(Date(2026, 11, 1), 9, 0, 30, "FREQ=DAILY;COUNT=5");
	CHECK_EQ(Dates(daily, Date(2026, 10, 1), Date(2026, 12, 1)),
		std::string("2026-11-01 2026-11-02 2026-11-03 2026-11-04 2026-11-05"));
	// COUNT still counts the instances before the range.
	CHECK_EQ(Dates(daily, Date(2026, 11, 4), Date(2026, 12, 1)),
		std::string("2026-11-04 2026-11-05"));

	// DTSTART (a Sunday) counts as the first instance although it does not
	// match BYDAY.
	Event weekly = Timed(Date(2026, 11, 1), 9, 0, 30,
		"FREQ=WEEKLY;BYDAY=MO;COUNT=3");
	CHECK_EQ(Dates(weekly, Date(2026, 10, 1), Date(2026, 12, 31)),
		std::string("2026-11-01 2026-11-02 2026-11-09"));

	// 9:00 EST is 14:00Z; UNTIL is inclusive.
	daily.rrule = "FREQ=DAILY;UNTIL=20261105T140000Z";
	CHECK_EQ(Dates(daily, Date(2026, 10, 1), Date(2026, 12, 1)),
		std::string("2026-11-01 2026-11-02 2026-11-03 2026-11-04 2026-11-05"));
	daily.rrule = "FREQ=DAILY;UNTIL=20261105T135959Z";
	CHECK_EQ(Dates(daily, Date(2026, 10, 1), Date(2026, 12, 1)),
		std::string("2026-11-01 2026-11-02 2026-11-03 2026-11-04"));
	// A floating UNTIL is wall time in the event's zone.
	daily.rrule = "FREQ=DAILY;UNTIL=20261105T090000";
	CHECK_EQ(Dates(daily, Date(2026, 10, 1), Date(2026, 12, 1)),
		std::string("2026-11-01 2026-11-02 2026-11-03 2026-11-04 2026-11-05"));
	daily.rrule = "FREQ=DAILY;UNTIL=20261104";
	CHECK_EQ(Dates(daily, Date(2026, 10, 1), Date(2026, 12, 1)),
		std::string("2026-11-01 2026-11-02 2026-11-03 2026-11-04"));

	// maxCount caps the result.
	daily.rrule = "FREQ=DAILY";
	CHECK_EQ(RuleInstances(daily, 0, StartOfDay(Date(2030, 1, 1)), 10).size(),
		(size_t)10);

	Event hourly = Timed(Date(2026, 11, 3), 9, 15, 10,
		"FREQ=HOURLY;INTERVAL=3;COUNT=4");
	std::vector<Occurrence> hours = ExpandEvent(hourly, {},
		StartOfDay(Date(2026, 11, 3)), StartOfDay(Date(2026, 11, 5)));
	CHECK_EQ(Times(hours, kNewYork),
		std::string("11-03@09:15 11-03@12:15 11-03@15:15 11-03@18:15"));
}


TEST(RecurrenceExdatesAndRdates)
{
	Event daily = Timed(Date(2026, 11, 2), 9, 0, 30, "FREQ=DAILY;COUNT=5");
	daily.exdates.push_back(ToInstant(DateTime(Date(2026, 11, 3), 9, 0), kNewYork));
	daily.exdateDays.push_back(Date(2026, 11, 5));
	daily.rdates.push_back(ToInstant(DateTime(Date(2026, 11, 20), 14, 0), kNewYork));
	// An RDATE equal to a rule instance is not doubled.
	daily.rdates.push_back(ToInstant(DateTime(Date(2026, 11, 4), 9, 0), kNewYork));
	std::vector<Occurrence> occurrences = ExpandEvent(daily, {},
		StartOfDay(Date(2026, 11, 1)), StartOfDay(Date(2026, 12, 1)));
	CHECK_EQ(Times(occurrences, kNewYork),
		std::string("11-02@09:00 11-04@09:00 11-06@09:00 11-20@14:00"));

	Event allDay = AllDay(Date(2026, 11, 2), 1, "FREQ=DAILY;COUNT=4");
	allDay.exdateDays.push_back(Date(2026, 11, 3));
	CHECK_EQ(Dates(allDay, Date(2026, 11, 1), Date(2026, 12, 1)),
		std::string("2026-11-02 2026-11-04 2026-11-05"));
}


TEST(RecurrenceOverrides)
{
	Event master = Timed(Date(2026, 11, 2), 9, 0, 60, "FREQ=DAILY;COUNT=10");
	auto at = [](int day, int hour) {
		return ToInstant(DateTime(Date(2026, 11, day), hour, 0), kNewYork);
	};

	Event moved;	// Nov 4 moved to 15:00
	moved.id = "moved";
	moved.masterId = "master";
	moved.tzid = kNewYork;
	moved.recurrenceId = at(4, 9);
	moved.start = at(4, 15);
	moved.end = at(4, 16);
	moved.title = "Moved";

	Event cancelled;	// Nov 5 deleted
	cancelled.id = "cancelled";
	cancelled.masterId = "master";
	cancelled.recurrenceId = at(5, 9);
	cancelled.start = at(5, 9);
	cancelled.end = at(5, 10);
	cancelled.cancelled = true;

	Event movedIn;	// Nov 10 moved into the range
	movedIn.id = "in";
	movedIn.masterId = "master";
	movedIn.recurrenceId = at(10, 9);
	movedIn.start = at(6, 12);
	movedIn.end = at(6, 13);

	Event movedOut;	// Nov 3 moved out of the range
	movedOut.id = "out";
	movedOut.masterId = "master";
	movedOut.recurrenceId = at(3, 9);
	movedOut.start = at(20, 9);
	movedOut.end = at(20, 10);

	Event stranger;	// another series' override
	stranger.id = "stranger";
	stranger.masterId = "other";
	stranger.recurrenceId = at(2, 9);
	stranger.start = at(2, 11);
	stranger.end = at(2, 12);

	std::vector<const Event*> overrides = {&moved, &cancelled, &movedIn,
		&movedOut, &stranger};
	std::vector<Occurrence> occurrences = ExpandEvent(master, overrides,
		StartOfDay(Date(2026, 11, 2)), StartOfDay(Date(2026, 11, 7)));
	CHECK_EQ(Times(occurrences, kNewYork),
		std::string("11-02@09:00 11-04@15:00 11-06@09:00 11-06@12:00"));
	if (occurrences.size() == 4) {
		CHECK(occurrences[0].event == &master);
		CHECK(occurrences[1].event == &moved);
		CHECK(occurrences[1].master == &master);
		CHECK_EQ(occurrences[1].originalStart, at(4, 9));
		CHECK(occurrences[1].originalDate == Date(2026, 11, 4));
		CHECK(occurrences[3].event == &movedIn);
		CHECK_EQ(occurrences[3].originalStart, at(10, 9));
	}

	// The instance moved out shows where it went.
	std::vector<Occurrence> later = ExpandEvent(master, overrides,
		StartOfDay(Date(2026, 11, 19)), StartOfDay(Date(2026, 11, 21)));
	CHECK_EQ(later.size(), (size_t)1);
	if (later.size() == 1)
		CHECK(later[0].event == &movedOut);

	// All-day series match overrides by date.
	Event allDay = AllDay(Date(2026, 11, 2), 1, "FREQ=WEEKLY;COUNT=3");
	Event skip;
	skip.masterId = "master";
	skip.allDay = true;
	skip.recurrenceDate = Date(2026, 11, 9);
	skip.startDate = Date(2026, 11, 9);
	skip.endDate = Date(2026, 11, 10);
	skip.cancelled = true;
	std::vector<Occurrence> days = ExpandEvent(allDay, {&skip},
		StartOfDay(Date(2026, 11, 1)), StartOfDay(Date(2026, 12, 1)));
	CHECK_EQ(days.size(), (size_t)2);
	if (days.size() == 2) {
		CHECK(days[0].startDate == Date(2026, 11, 2));
		CHECK(days[1].startDate == Date(2026, 11, 16));
	}
}


TEST(RecurrenceAllDayMultiDay)
{
	// A two-day event every week: Mon-Tue.
	Event event = AllDay(Date(2026, 11, 2), 2, "FREQ=WEEKLY;COUNT=3");
	// Only the Tuesday of the second instance is in range.
	std::vector<Occurrence> occurrences = ExpandEvent(event, {},
		StartOfDay(Date(2026, 11, 10)), StartOfDay(Date(2026, 11, 11)));
	CHECK_EQ(occurrences.size(), (size_t)1);
	if (occurrences.size() == 1) {
		CHECK(occurrences[0].allDay);
		CHECK(occurrences[0].startDate == Date(2026, 11, 9));
		CHECK(occurrences[0].endDate == Date(2026, 11, 11));
		CHECK(occurrences[0].originalDate == Date(2026, 11, 9));
		CHECK(occurrences[0].FirstDay() == Date(2026, 11, 9));
		CHECK(occurrences[0].LastDay() == Date(2026, 11, 10));
	}
	// The end date is exclusive.
	CHECK(ExpandEvent(event, {}, StartOfDay(Date(2026, 11, 11)),
		StartOfDay(Date(2026, 11, 16))).empty());
}


TEST(RecurrenceNonRecurring)
{
	Event single = Timed(Date(2026, 11, 3), 9, 0, 60, "");
	CHECK_EQ(ExpandEvent(single, {}, StartOfDay(Date(2026, 11, 3)),
		StartOfDay(Date(2026, 11, 4))).size(), (size_t)1);
	// Touching the range end/start is not overlapping.
	CHECK(ExpandEvent(single, {}, single.end, single.end + 3600).empty());
	CHECK(ExpandEvent(single, {}, single.start - 3600, single.start).empty());

	Event instant = Timed(Date(2026, 11, 3), 9, 0, 0, "");
	std::vector<Occurrence> zero = ExpandEvent(instant, {}, instant.start,
		instant.start + 60);
	CHECK_EQ(zero.size(), (size_t)1);
	if (zero.size() == 1) {
		CHECK(zero[0].event == &instant && zero[0].master == &instant);
	}
	CHECK(ExpandEvent(instant, {}, instant.start - 60, instant.start).empty());
}


TEST(RecurrenceLongSeriesIsFast)
{
	Event daily = Timed(Date(2005, 1, 3), 9, 0, 30, "FREQ=DAILY");
	Event weekly = Timed(Date(2005, 1, 3), 9, 0, 30, "FREQ=WEEKLY;BYDAY=MO,WE,FR");
	Event counted = Timed(Date(2005, 1, 3), 9, 0, 30, "FREQ=DAILY;COUNT=10000");
	Instant from = StartOfDay(Date(2026, 11, 2));
	Instant to = StartOfDay(Date(2026, 11, 9));

	auto begin = std::chrono::steady_clock::now();
	std::vector<Occurrence> days, weekdays, countedDays;
	for (int i = 0; i < 20; i++) {
		days = ExpandEvent(daily, {}, from, to);
		weekdays = ExpandEvent(weekly, {}, from, to);
	}
	countedDays = ExpandEvent(counted, {}, from, to);
	double seconds = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - begin).count();

	CHECK_EQ(Times(days, kNewYork), std::string("11-02@09:00 11-03@09:00 "
		"11-04@09:00 11-05@09:00 11-06@09:00 11-07@09:00 11-08@09:00"));
	CHECK_EQ(Times(weekdays, kNewYork),
		std::string("11-02@09:00 11-04@09:00 11-06@09:00"));
	CHECK_EQ(countedDays.size(), (size_t)7);
	CHECK(seconds < 1.0);
}


TEST(RecurrenceTruncate)
{
	Event weekly = Timed(Date(2026, 11, 2), 9, 0, 60, "FREQ=WEEKLY;COUNT=10");
	Instant fourth = ToInstant(DateTime(Date(2026, 11, 23), 9, 0), kNewYork);
	std::string rule = TruncateRule(weekly.rrule, weekly, fourth);
	CHECK_EQ(rule, "FREQ=WEEKLY;UNTIL=" + FormatBasicUtc(fourth - 1));
	weekly.rrule = rule;
	CHECK_EQ(Dates(weekly, Date(2026, 11, 1), Date(2027, 6, 1)),
		std::string("2026-11-02 2026-11-09 2026-11-16"));

	Event allDay = AllDay(Date(2026, 11, 2), 1, "FREQ=DAILY");
	rule = TruncateRule(allDay.rrule, allDay, StartOfDay(Date(2026, 11, 5)));
	CHECK_EQ(rule, std::string("FREQ=DAILY;UNTIL=20261104"));
	allDay.rrule = rule;
	CHECK_EQ(Dates(allDay, Date(2026, 11, 1), Date(2026, 12, 1)),
		std::string("2026-11-02 2026-11-03 2026-11-04"));
}
