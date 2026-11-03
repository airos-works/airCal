// Quick-entry parser tests. "Now" is Tuesday Nov 3 2026 10:00 in the test
// zone (America/New_York, see TestMain.cpp).
#include "Test.h"
#include "core/QuickEntry.h"

#include <string>

using namespace aircal;

namespace {

Instant
TestNow()
{
	return ToInstant(DateTime(Date(2026, 11, 3), 10, 0), "");
}


QuickEntryResult
Parse(const std::string& text, int weekStart = 0)
{
	return ParseQuickEntry(text, TestNow(), 60, weekStart);
}


Instant
At(int year, int month, int day, int hour, int minute = 0)
{
	return ToInstant(DateTime(Date(year, month, day), hour, minute), "");
}


std::string
Iso(const Date& date)
{
	return date.ToIso();
}


bool
Contains(const std::string& text, const std::string& part)
{
	return text.find(part) != std::string::npos;
}

}	// namespace


// --- Titles, locations, times ---------------------------------------------------

TEST(QuickEntryPlainTitle)
{
	QuickEntryResult r = Parse("Write the quarterly report");
	CHECK_EQ(r.title, std::string("Write the quarterly report"));
	CHECK(!r.isTask);
	CHECK(!r.hasDate);
	CHECK(!r.hasTime);
	CHECK(r.allDay);
	CHECK_EQ(Iso(r.startDate), std::string("2026-11-03"));
	CHECK_EQ(Iso(r.endDate), std::string("2026-11-03"));
	CHECK(r.rrule.empty());
	CHECK_EQ(r.alarmMinutes, -1);
}


TEST(QuickEntryMeetAtFiveIsATime)
{
	QuickEntryResult r = Parse("Meet at 5");
	CHECK_EQ(r.title, std::string("Meet"));
	CHECK(r.location.empty());
	CHECK(r.hasTime);
	CHECK(!r.hasDate);
	CHECK(!r.allDay);
	CHECK_EQ(r.start, At(2026, 11, 3, 17));
	CHECK_EQ(r.end, At(2026, 11, 3, 18));
}


TEST(QuickEntryBareHourGuess)
{
	// 9 means 9am, which has passed at 10:00: tomorrow.
	QuickEntryResult r = Parse("Call at 9");
	CHECK_EQ(r.start, At(2026, 11, 4, 9));
	CHECK(!r.hasDate);
	CHECK_EQ(Parse("Call at 12").start, At(2026, 11, 3, 12));
	CHECK_EQ(Parse("Call at 15:30").start, At(2026, 11, 3, 15, 30));
	CHECK_EQ(Parse("Call at 3 o'clock").start, At(2026, 11, 3, 15));
}


TEST(QuickEntryDinnerAtNobu)
{
	QuickEntryResult r = Parse("Dinner at Nobu at 7pm");
	CHECK_EQ(r.title, std::string("Dinner"));
	CHECK_EQ(r.location, std::string("Nobu"));
	CHECK_EQ(r.start, At(2026, 11, 3, 19));
	CHECK_EQ(r.end, At(2026, 11, 3, 20));
}


TEST(QuickEntryLunchWithSam)
{
	QuickEntryResult r = Parse("Lunch with Sam at Cafe Rio tomorrow 12:30pm");
	CHECK_EQ(r.title, std::string("Lunch with Sam"));
	CHECK_EQ(r.location, std::string("Cafe Rio"));
	CHECK(r.hasDate);
	CHECK(r.hasTime);
	CHECK(!r.allDay);
	CHECK_EQ(r.start, At(2026, 11, 4, 12, 30));
	CHECK_EQ(r.end, At(2026, 11, 4, 13, 30));
	std::string summary = r.Describe();
	CHECK(Contains(summary, "Event"));
	CHECK(Contains(summary, "Wed, Nov 4"));
	CHECK(Contains(summary, "12:30 PM"));
	CHECK(Contains(summary, "1:30 PM"));
	CHECK(Contains(summary, "at Cafe Rio"));
}


TEST(QuickEntryFlightWithDuration)
{
	QuickEntryResult r = Parse("Flight to NYC Nov 12 at 6:45am for 3 hours");
	CHECK_EQ(r.title, std::string("Flight to NYC"));
	CHECK(r.location.empty());
	CHECK_EQ(r.start, At(2026, 11, 12, 6, 45));
	CHECK_EQ(r.end, At(2026, 11, 12, 9, 45));
}


TEST(QuickEntryNoonMidnight)
{
	QuickEntryResult r = Parse("Lunch tomorrow at noon");
	CHECK_EQ(r.title, std::string("Lunch"));
	CHECK_EQ(r.start, At(2026, 11, 4, 12));
	// Midnight today has passed: the coming midnight.
	r = Parse("Fireworks at midnight");
	CHECK_EQ(r.title, std::string("Fireworks"));
	CHECK_EQ(r.start, At(2026, 11, 4, 0));
}


TEST(QuickEntryTimeForms)
{
	CHECK_EQ(Parse("Call 3pm").start, At(2026, 11, 3, 15));
	CHECK_EQ(Parse("Call 3:30 pm").start, At(2026, 11, 3, 15, 30));
	CHECK_EQ(Parse("Call 3:30 p.m.").start, At(2026, 11, 3, 15, 30));
	CHECK_EQ(Parse("Call 11am").start, At(2026, 11, 3, 11));
	CHECK_EQ(Parse("Call 12am friday").start, At(2026, 11, 6, 0));
	CHECK_EQ(Parse("Call 3:30 pm").title, std::string("Call"));
}


TEST(QuickEntryPeriods)
{
	QuickEntryResult r = Parse("Drinks tonight");
	CHECK_EQ(r.title, std::string("Drinks"));
	CHECK(r.hasDate);
	CHECK_EQ(r.start, At(2026, 11, 3, 20));
	CHECK_EQ(Parse("Run tomorrow morning").start, At(2026, 11, 4, 9));
	CHECK_EQ(Parse("Review friday afternoon").start, At(2026, 11, 6, 14));
	CHECK_EQ(Parse("Party saturday evening").start, At(2026, 11, 7, 18));
	CHECK_EQ(Parse("Movie tomorrow night").start, At(2026, 11, 4, 20));
	// A period word moves a guessed hour.
	CHECK_EQ(Parse("Breakfast tomorrow morning at 7").start, At(2026, 11, 4, 7));
	CHECK_EQ(Parse("Dinner tonight at 9").start, At(2026, 11, 3, 21));
	CHECK_EQ(Parse("Call mum in the evening").start, At(2026, 11, 3, 18));
}


// --- Dates -----------------------------------------------------------------------

TEST(QuickEntryRelativeDays)
{
	QuickEntryResult r = Parse("Dentist tomorrow");
	CHECK_EQ(r.title, std::string("Dentist"));
	CHECK(r.hasDate);
	CHECK(r.allDay);
	CHECK_EQ(Iso(r.startDate), std::string("2026-11-04"));
	CHECK_EQ(Iso(Parse("Dentist tmrw").startDate), std::string("2026-11-04"));
	CHECK_EQ(Iso(Parse("Dentist today").startDate), std::string("2026-11-03"));
	CHECK_EQ(Iso(Parse("Dentist yesterday").startDate), std::string("2026-11-02"));
	r = Parse("Dentist the day after tomorrow");
	CHECK_EQ(Iso(r.startDate), std::string("2026-11-05"));
	CHECK_EQ(r.title, std::string("Dentist"));
	CHECK_EQ(Iso(Parse("Dentist day after tomorrow").startDate),
		std::string("2026-11-05"));
}


TEST(QuickEntryWeekdays)
{
	// Today is Tuesday.
	CHECK_EQ(Iso(Parse("Gym friday").startDate), std::string("2026-11-06"));
	CHECK_EQ(Iso(Parse("Gym fri").startDate), std::string("2026-11-06"));
	CHECK_EQ(Iso(Parse("Gym tuesday").startDate), std::string("2026-11-03"));
	CHECK_EQ(Iso(Parse("Gym monday").startDate), std::string("2026-11-09"));
	CHECK_EQ(Iso(Parse("Gym on Thurs").startDate), std::string("2026-11-05"));
	QuickEntryResult r = Parse("Gym this friday");
	CHECK_EQ(Iso(r.startDate), std::string("2026-11-06"));
	CHECK_EQ(r.title, std::string("Gym"));
	r = Parse("Gym on friday");
	CHECK_EQ(Iso(r.startDate), std::string("2026-11-06"));
	CHECK_EQ(r.title, std::string("Gym"));
	// "sun"/"sat" are words unless qualified.
	CHECK_EQ(Parse("Watch the sun rise").title, std::string("Watch the sun rise"));
	CHECK_EQ(Iso(Parse("Brunch on sun").startDate), std::string("2026-11-08"));
	QuickEntryResult sale = Parse("Yard sale 9am-noon sat");
	CHECK_EQ(sale.title, std::string("Yard sale"));
	CHECK_EQ(sale.start, At(2026, 11, 7, 9));
	CHECK_EQ(sale.end, At(2026, 11, 7, 12));
}


TEST(QuickEntryNextWeekday)
{
	// Week starting Sunday: this week is Nov 1-7, next week Nov 8-14.
	CHECK_EQ(Iso(Parse("Gym next monday").startDate), std::string("2026-11-09"));
	CHECK_EQ(Iso(Parse("Gym next friday").startDate), std::string("2026-11-13"));
	CHECK_EQ(Iso(Parse("Gym next tuesday").startDate), std::string("2026-11-10"));
	CHECK_EQ(Iso(Parse("Gym next sunday").startDate), std::string("2026-11-08"));
	// Week starting Monday: next week is Nov 9-15.
	CHECK_EQ(Iso(Parse("Gym next sunday", 1).startDate), std::string("2026-11-15"));
	CHECK_EQ(Parse("Gym next monday").title, std::string("Gym"));
}


TEST(QuickEntryNextWeekMonth)
{
	QuickEntryResult r = Parse("Plan sprint next week");
	CHECK_EQ(r.title, std::string("Plan sprint"));
	CHECK(r.allDay);
	CHECK_EQ(Iso(r.startDate), std::string("2026-11-08"));
	CHECK_EQ(Iso(Parse("Plan sprint next week", 1).startDate),
		std::string("2026-11-09"));
	CHECK_EQ(Iso(Parse("Budget next month").startDate), std::string("2026-12-01"));
	CHECK_EQ(Iso(Parse("Taxes next year").startDate), std::string("2027-01-01"));
	CHECK_EQ(Iso(Parse("Hike next weekend").startDate), std::string("2026-11-14"));
	CHECK_EQ(Iso(Parse("Hike this weekend").startDate), std::string("2026-11-07"));
}


TEST(QuickEntryInDaysWeeks)
{
	QuickEntryResult r = Parse("Review in 2 weeks");
	CHECK_EQ(r.title, std::string("Review"));
	CHECK(r.hasDate);
	CHECK(r.allDay);
	CHECK_EQ(Iso(r.startDate), std::string("2026-11-17"));
	CHECK_EQ(Iso(Parse("Review in 3 days").startDate), std::string("2026-11-06"));
	CHECK_EQ(Iso(Parse("Review in a week").startDate), std::string("2026-11-10"));
	CHECK_EQ(Iso(Parse("Review in 2 months").startDate), std::string("2027-01-03"));
	r = Parse("Review in 3 days at 4pm");
	CHECK_EQ(r.start, At(2026, 11, 6, 16));
}


TEST(QuickEntryInMinutesHours)
{
	QuickEntryResult r = Parse("Check oven in 30 minutes");
	CHECK_EQ(r.title, std::string("Check oven"));
	CHECK(r.hasDate);
	CHECK(r.hasTime);
	CHECK_EQ(r.start, TestNow() + 30 * 60);
	CHECK_EQ(r.end, TestNow() + 90 * 60);
	CHECK_EQ(Parse("Call back in an hour").start, TestNow() + 3600);
	CHECK_EQ(Parse("Call back in half an hour").start, TestNow() + 1800);
	CHECK_EQ(Parse("Call back in 1.5 hours").start, TestNow() + 5400);
}


TEST(QuickEntryMonthNameDates)
{
	CHECK_EQ(Iso(Parse("Party Nov 12").startDate), std::string("2026-11-12"));
	CHECK_EQ(Iso(Parse("Party November 12th").startDate), std::string("2026-11-12"));
	CHECK_EQ(Iso(Parse("Party Nov. 12").startDate), std::string("2026-11-12"));
	CHECK_EQ(Iso(Parse("Party 12 Nov").startDate), std::string("2026-11-12"));
	CHECK_EQ(Iso(Parse("Party 12th of November").startDate),
		std::string("2026-11-12"));
	CHECK_EQ(Iso(Parse("Party Nov 12 2027").startDate), std::string("2027-11-12"));
	CHECK_EQ(Iso(Parse("Party Nov 12, 2027").startDate), std::string("2027-11-12"));
	// Nov 3 is today: it stays this year.
	CHECK_EQ(Iso(Parse("Party Nov 3").startDate), std::string("2026-11-03"));
	// Already passed this year: next year.
	CHECK_EQ(Iso(Parse("Party March 5").startDate), std::string("2027-03-05"));
	CHECK_EQ(Parse("Party November 12th").title, std::string("Party"));
	// "May" without a day is just a word.
	CHECK_EQ(Parse("May I help").title, std::string("May I help"));
}


TEST(QuickEntryOrdinalDay)
{
	QuickEntryResult r = Parse("Rent on the 12th");
	CHECK_EQ(r.title, std::string("Rent"));
	CHECK_EQ(Iso(r.startDate), std::string("2026-11-12"));
	CHECK_EQ(Iso(Parse("Rent the 2nd").startDate), std::string("2026-12-02"));
	CHECK_EQ(Iso(Parse("Party 12th").startDate), std::string("2026-11-12"));
	// An adjective, not a date.
	r = Parse("Review 1st draft");
	CHECK_EQ(r.title, std::string("Review 1st draft"));
	CHECK(!r.hasDate);
}


TEST(QuickEntryNumericDates)
{
	CHECK_EQ(Iso(Parse("Exam 11/12").startDate), std::string("2026-11-12"));
	CHECK_EQ(Iso(Parse("Exam 11/12/2027").startDate), std::string("2027-11-12"));
	CHECK_EQ(Iso(Parse("Exam 11/12/27").startDate), std::string("2027-11-12"));
	CHECK_EQ(Iso(Parse("Exam 1/15").startDate), std::string("2027-01-15"));
	QuickEntryResult r = Parse("Exam 2026-11-12 at 9am");
	CHECK_EQ(r.title, std::string("Exam"));
	CHECK_EQ(r.start, At(2026, 11, 12, 9));
	// Not dates.
	CHECK_EQ(Parse("Exam 13/45").title, std::string("Exam 13/45"));
}


TEST(QuickEntryWeekdayThenDate)
{
	// An explicit date beats the weekday next to it.
	QuickEntryResult r = Parse("Drinks Fri, Nov 13 at 6pm");
	CHECK_EQ(r.title, std::string("Drinks"));
	CHECK_EQ(r.start, At(2026, 11, 13, 18));
}


// --- Ranges and durations ---------------------------------------------------------

TEST(QuickEntryTimeRanges)
{
	QuickEntryResult r = Parse("Dentist on Friday from 3-4pm alert 30 minutes before");
	CHECK_EQ(r.title, std::string("Dentist"));
	CHECK_EQ(r.start, At(2026, 11, 6, 15));
	CHECK_EQ(r.end, At(2026, 11, 6, 16));
	CHECK_EQ(r.alarmMinutes, 30);

	r = Parse("Workshop 3 to 5pm");
	CHECK_EQ(r.title, std::string("Workshop"));
	CHECK_EQ(r.start, At(2026, 11, 3, 15));
	CHECK_EQ(r.end, At(2026, 11, 3, 17));

	r = Parse("Workshop tomorrow 11-1pm");
	CHECK_EQ(r.start, At(2026, 11, 4, 11));
	CHECK_EQ(r.end, At(2026, 11, 4, 13));

	r = Parse("Shift friday 10pm-2am");
	CHECK_EQ(r.start, At(2026, 11, 6, 22));
	CHECK_EQ(r.end, At(2026, 11, 7, 2));

	r = Parse("Class from 9 to 5 tomorrow");
	CHECK_EQ(r.start, At(2026, 11, 4, 9));
	CHECK_EQ(r.end, At(2026, 11, 4, 17));

	r = Parse("Talk tomorrow 3:30\xE2\x80\x93" "4:15pm");
	CHECK_EQ(r.start, At(2026, 11, 4, 15, 30));
	CHECK_EQ(r.end, At(2026, 11, 4, 16, 15));

	r = Parse("Sync between 2 and 3pm tomorrow");
	CHECK_EQ(r.title, std::string("Sync"));
	CHECK_EQ(r.start, At(2026, 11, 4, 14));
	CHECK_EQ(r.end, At(2026, 11, 4, 15));
}


TEST(QuickEntryPartyFriday)
{
	QuickEntryResult r = Parse("Party friday 8pm-11pm /Personal");
	CHECK_EQ(r.title, std::string("Party"));
	CHECK_EQ(r.calendarHint, std::string("Personal"));
	CHECK_EQ(r.start, At(2026, 11, 6, 20));
	CHECK_EQ(r.end, At(2026, 11, 6, 23));
	CHECK(Contains(r.Describe(), "/Personal"));
	CHECK(Contains(r.Describe(), "8:00 PM"));
	CHECK(Contains(r.Describe(), "11:00 PM"));
	CHECK_EQ(Parse("Lunch /Family_Stuff").calendarHint, std::string("Family Stuff"));
}


TEST(QuickEntryDurations)
{
	QuickEntryResult r = Parse("Call tomorrow 2pm for 30 min");
	CHECK_EQ(r.end - r.start, (Instant)30 * 60);
	r = Parse("Call tomorrow 2pm for 1.5 hours");
	CHECK_EQ(r.end - r.start, (Instant)90 * 60);
	r = Parse("Call tomorrow 2pm for an hour and a half");
	CHECK_EQ(r.end - r.start, (Instant)90 * 60);
	r = Parse("Call tomorrow 2pm for 1 hour 15 minutes");
	CHECK_EQ(r.end - r.start, (Instant)75 * 60);
	CHECK_EQ(r.title, std::string("Call"));
	r = Parse("Call tomorrow 2pm for 2h");
	CHECK_EQ(r.end - r.start, (Instant)120 * 60);
	// Default length.
	r = ParseQuickEntry("Call tomorrow 2pm", TestNow(), 45, 0);
	CHECK_EQ(r.end - r.start, (Instant)45 * 60);
	// "for" not followed by a duration stays in the title.
	CHECK_EQ(Parse("Dinner for two").title, std::string("Dinner for two"));
}


TEST(QuickEntryMultiDayAllDay)
{
	QuickEntryResult r = Parse("Conference Nov 12-14");
	CHECK_EQ(r.title, std::string("Conference"));
	CHECK(r.allDay);
	CHECK_EQ(Iso(r.startDate), std::string("2026-11-12"));
	CHECK_EQ(Iso(r.endDate), std::string("2026-11-14"));
	CHECK(Contains(r.Describe(), "Thu, Nov 12"));
	CHECK(Contains(r.Describe(), "Sat, Nov 14"));

	r = Parse("Conference from Nov 12 to Nov 14");
	CHECK_EQ(r.title, std::string("Conference"));
	CHECK_EQ(Iso(r.startDate), std::string("2026-11-12"));
	CHECK_EQ(Iso(r.endDate), std::string("2026-11-14"));

	r = Parse("Retreat friday for 3 days");
	CHECK_EQ(Iso(r.startDate), std::string("2026-11-06"));
	CHECK_EQ(Iso(r.endDate), std::string("2026-11-08"));
}


TEST(QuickEntryVacationAcrossYear)
{
	QuickEntryResult r = Parse("Vacation from Dec 20 to Jan 3");
	CHECK_EQ(r.title, std::string("Vacation"));
	CHECK(r.allDay);
	CHECK(r.hasDate);
	CHECK_EQ(Iso(r.startDate), std::string("2026-12-20"));
	CHECK_EQ(Iso(r.endDate), std::string("2027-01-03"));
}


TEST(QuickEntryWeekdayRange)
{
	QuickEntryResult r = Parse("Trip monday to wednesday");
	CHECK_EQ(r.title, std::string("Trip"));
	CHECK_EQ(Iso(r.startDate), std::string("2026-11-09"));
	CHECK_EQ(Iso(r.endDate), std::string("2026-11-11"));
	r = Parse("Trip fri to mon");
	CHECK_EQ(Iso(r.startDate), std::string("2026-11-06"));
	CHECK_EQ(Iso(r.endDate), std::string("2026-11-09"));
}


TEST(QuickEntryOutUntil)
{
	QuickEntryResult r = Parse("Out of office until Friday");
	CHECK_EQ(r.title, std::string("Out of office"));
	CHECK(r.allDay);
	CHECK_EQ(Iso(r.startDate), std::string("2026-11-03"));
	CHECK_EQ(Iso(r.endDate), std::string("2026-11-06"));
	CHECK(r.rrule.empty());
}


// --- Recurrence -------------------------------------------------------------------

TEST(QuickEntryStandup)
{
	QuickEntryResult r = Parse("Standup every weekday at 9:15am for 15 minutes");
	CHECK_EQ(r.title, std::string("Standup"));
	CHECK_EQ(r.rrule, std::string("FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR"));
	// 9:15 has passed today: the next weekday.
	CHECK_EQ(r.start, At(2026, 11, 4, 9, 15));
	CHECK_EQ(r.end, At(2026, 11, 4, 9, 30));
	CHECK(Contains(r.Describe(), "Every weekday"));
}


TEST(QuickEntryGymTwiceAWeek)
{
	QuickEntryResult r = Parse("Gym every monday and wednesday at 7am");
	CHECK_EQ(r.title, std::string("Gym"));
	CHECK_EQ(r.rrule, std::string("FREQ=WEEKLY;BYDAY=MO,WE"));
	CHECK(r.hasDate);
	CHECK_EQ(r.start, At(2026, 11, 4, 7));
	r = Parse("Gym every mon, wed and fri");
	CHECK_EQ(r.rrule, std::string("FREQ=WEEKLY;BYDAY=MO,WE,FR"));
	CHECK_EQ(Iso(r.startDate), std::string("2026-11-04"));
	CHECK_EQ(r.title, std::string("Gym"));
}


TEST(QuickEntryEveryMondaySetsDate)
{
	QuickEntryResult r = Parse("Team sync every Monday at 9am for 30 minutes /Work");
	CHECK_EQ(r.title, std::string("Team sync"));
	CHECK_EQ(r.rrule, std::string("FREQ=WEEKLY;BYDAY=MO"));
	CHECK_EQ(r.start, At(2026, 11, 9, 9));
	CHECK_EQ(r.end, At(2026, 11, 9, 9, 30));
	CHECK_EQ(r.calendarHint, std::string("Work"));
	// Tuesday today, nothing passed yet: today.
	CHECK_EQ(Iso(Parse("Swim every tuesday").startDate), std::string("2026-11-03"));
	// "on mondays" is a recurrence too.
	r = Parse("Piano on mondays");
	CHECK_EQ(r.rrule, std::string("FREQ=WEEKLY;BYDAY=MO"));
	CHECK_EQ(r.title, std::string("Piano"));
}


TEST(QuickEntrySimpleRecurrences)
{
	CHECK_EQ(Parse("Vitamins every day").rrule, std::string("FREQ=DAILY"));
	CHECK_EQ(Parse("Vitamins daily").rrule, std::string("FREQ=DAILY"));
	CHECK_EQ(Parse("Report every week").rrule, std::string("FREQ=WEEKLY"));
	CHECK_EQ(Parse("Report weekly").rrule, std::string("FREQ=WEEKLY"));
	CHECK_EQ(Parse("Payroll every 2 weeks").rrule,
		std::string("FREQ=WEEKLY;INTERVAL=2"));
	CHECK_EQ(Parse("Payroll every other week").rrule,
		std::string("FREQ=WEEKLY;INTERVAL=2"));
	CHECK_EQ(Parse("Payroll biweekly").rrule, std::string("FREQ=WEEKLY;INTERVAL=2"));
	CHECK_EQ(Parse("Rent every month").rrule, std::string("FREQ=MONTHLY"));
	CHECK_EQ(Parse("Rent monthly").rrule, std::string("FREQ=MONTHLY"));
	CHECK_EQ(Parse("Review every 3 months").rrule,
		std::string("FREQ=MONTHLY;INTERVAL=3"));
	CHECK_EQ(Parse("Checkup every year").rrule, std::string("FREQ=YEARLY"));
	CHECK_EQ(Parse("Checkup yearly").rrule, std::string("FREQ=YEARLY"));
	CHECK_EQ(Parse("Checkup annually").rrule, std::string("FREQ=YEARLY"));
	CHECK_EQ(Parse("1:1 every other monday").rrule,
		std::string("FREQ=WEEKLY;INTERVAL=2;BYDAY=MO"));
	QuickEntryResult r = Parse("Weekly 1:1 with Ana fridays 2pm");
	CHECK_EQ(r.title, std::string("1:1 with Ana"));
	CHECK_EQ(r.rrule, std::string("FREQ=WEEKLY;BYDAY=FR"));
	CHECK_EQ(r.start, At(2026, 11, 6, 14));
	r = Parse("Vitamins every day");
	CHECK_EQ(r.title, std::string("Vitamins"));
	CHECK(r.hasDate);
	CHECK(r.allDay);
}


TEST(QuickEntryYearlyAndMonthlyDates)
{
	QuickEntryResult r = Parse("Anniversary every Nov 12");
	CHECK_EQ(r.rrule, std::string("FREQ=YEARLY"));
	CHECK_EQ(Iso(r.startDate), std::string("2026-11-12"));
	CHECK_EQ(r.title, std::string("Anniversary"));
	r = Parse("Pay card every 15th");
	CHECK_EQ(r.rrule, std::string("FREQ=MONTHLY"));
	CHECK_EQ(Iso(r.startDate), std::string("2026-11-15"));
}


TEST(QuickEntryUntil)
{
	QuickEntryResult r = Parse("Yoga every day until Dec 20");
	CHECK_EQ(r.title, std::string("Yoga"));
	CHECK_EQ(r.rrule, std::string("FREQ=DAILY;UNTIL=20261220"));
	// Timed: UTC, the end of Dec 20 local (20261221T045959Z in New York).
	r = Parse("Yoga every monday at 6pm until Dec 20");
	CHECK_EQ(r.rrule, "FREQ=WEEKLY;BYDAY=MO;UNTIL="
		+ FormatBasicUtc(At(2026, 12, 20, 23, 59) + 59));
	CHECK_EQ(r.title, std::string("Yoga"));
	CHECK(Contains(r.Describe(), "until Dec 20"));
	r = Parse("Class every week 10 times");
	CHECK_EQ(r.rrule, std::string("FREQ=WEEKLY;COUNT=10"));
}


// --- Alerts -----------------------------------------------------------------------

TEST(QuickEntryAlerts)
{
	QuickEntryResult r = Parse("Meeting tomorrow 3pm alert 10 minutes before");
	CHECK_EQ(r.title, std::string("Meeting"));
	CHECK_EQ(r.alarmMinutes, 10);
	CHECK_EQ(Parse("Meeting tomorrow 3pm remind me 1 hour before").alarmMinutes, 60);
	CHECK_EQ(Parse("Meeting tomorrow 3pm alarm 15 min").alarmMinutes, 15);
	CHECK_EQ(Parse("Meeting tomorrow 3pm with alert").alarmMinutes, 0);
	CHECK_EQ(Parse("Meeting tomorrow 3pm 1 day before").alarmMinutes, 1440);
	r = Parse("Meeting tomorrow 3pm with alert");
	CHECK_EQ(r.title, std::string("Meeting"));
	CHECK(Contains(Parse("Meeting tomorrow 3pm alert 10 minutes before").Describe(),
		"Alert 10 min before"));
	// Words that only look like alerts stay.
	CHECK_EQ(Parse("Fire alarm test").title, std::string("Fire alarm test"));
}


// --- Locations --------------------------------------------------------------------

TEST(QuickEntryLocations)
{
	QuickEntryResult r = Parse("Coffee @ Blue Bottle tomorrow 8am");
	CHECK_EQ(r.title, std::string("Coffee"));
	CHECK_EQ(r.location, std::string("Blue Bottle"));
	CHECK_EQ(r.start, At(2026, 11, 4, 8));

	r = Parse("Standup in Room 5 tomorrow at 9");
	CHECK_EQ(r.title, std::string("Standup"));
	CHECK_EQ(r.location, std::string("Room 5"));
	CHECK_EQ(r.start, At(2026, 11, 4, 9));

	r = Parse("Dinner at Nobu with Sam friday 7pm");
	CHECK_EQ(r.title, std::string("Dinner with Sam"));
	CHECK_EQ(r.location, std::string("Nobu"));

	r = Parse("Drinks at The Local, Hobart tonight");
	CHECK_EQ(r.location, std::string("The Local, Hobart"));
	CHECK_EQ(r.title, std::string("Drinks"));

	// "in" with a lower-case word is not a place.
	r = Parse("Check in tomorrow");
	CHECK_EQ(r.title, std::string("Check in"));
	CHECK(r.location.empty());
	// "at" at the end is not a location either.
	CHECK_EQ(Parse("Look at").title, std::string("Look at"));
}


// --- Tasks ------------------------------------------------------------------------

TEST(QuickEntryTodoPayRent)
{
	QuickEntryResult r = Parse("todo Pay rent due Nov 1 !!");
	CHECK(r.isTask);
	CHECK_EQ(r.title, std::string("Pay rent"));
	CHECK(r.hasDate);
	CHECK(!r.hasTime);
	CHECK(r.allDay);
	// Nov 1 has passed this year.
	CHECK_EQ(Iso(r.startDate), std::string("2027-11-01"));
	CHECK_EQ(r.priority, 3);
	CHECK_EQ(r.dueString, std::string("Nov 1"));
	std::string summary = r.Describe();
	CHECK(Contains(summary, "Task"));
	CHECK(Contains(summary, "Due Mon, Nov 1"));
	CHECK(Contains(summary, "!!"));
}


TEST(QuickEntryRemindMeTo)
{
	QuickEntryResult r = Parse("remind me to call Mum tomorrow at 6pm");
	CHECK(r.isTask);
	CHECK_EQ(r.title, std::string("call Mum"));
	CHECK(r.hasDate);
	CHECK(r.hasTime);
	CHECK(!r.allDay);
	CHECK_EQ(r.start, At(2026, 11, 4, 18));
	CHECK_EQ(r.dueString, std::string("tomorrow at 6pm"));
	CHECK_EQ(r.alarmMinutes, 0);
	CHECK(Contains(r.Describe(), "Due Wed, Nov 4, 6:00 PM"));
}


TEST(QuickEntryTaskKeywords)
{
	CHECK(Parse("task Buy milk").isTask);
	CHECK_EQ(Parse("task Buy milk").title, std::string("Buy milk"));
	CHECK(Parse("Todo: Buy milk").isTask);
	CHECK_EQ(Parse("Todo: Buy milk").title, std::string("Buy milk"));
	CHECK(Parse("to do Buy milk").isTask);
	CHECK(Parse("reminder Buy milk").isTask);
	CHECK(Parse("- Buy milk").isTask);
	CHECK_EQ(Parse("- Buy milk").title, std::string("Buy milk"));
	CHECK(!Parse("Buy milk").isTask);
	CHECK_EQ(Parse("todo").title, std::string("New Task"));
	CHECK_EQ(Parse("tomorrow 3pm").title, std::string("New Event"));
}


TEST(QuickEntryTaskPriorityLabels)
{
	QuickEntryResult r = Parse("todo File taxes p1 #finance #home friday");
	CHECK_EQ(r.title, std::string("File taxes"));
	CHECK_EQ(r.priority, 4);
	CHECK_EQ(r.labels.size(), (size_t)2);
	if (r.labels.size() == 2) {
		CHECK_EQ(r.labels[0], std::string("finance"));
		CHECK_EQ(r.labels[1], std::string("home"));
	}
	CHECK_EQ(Iso(r.startDate), std::string("2026-11-06"));
	CHECK_EQ(r.dueString, std::string("friday"));
	CHECK_EQ(Parse("todo a p4").priority, 1);
	CHECK_EQ(Parse("todo a p2").priority, 3);
	CHECK_EQ(Parse("todo a !").priority, 2);
	CHECK_EQ(Parse("todo a !!!").priority, 4);
	// Events leave them alone.
	CHECK_EQ(Parse("Review p1 bugs").title, std::string("Review p1 bugs"));
}


TEST(QuickEntryTaskRecurring)
{
	QuickEntryResult r = Parse("todo Water plants every monday at 9am");
	CHECK(r.isTask);
	CHECK_EQ(r.title, std::string("Water plants"));
	CHECK_EQ(r.rrule, std::string("FREQ=WEEKLY;BYDAY=MO"));
	CHECK_EQ(r.dueString, std::string("every monday at 9am"));
	CHECK(r.hasDate);
	CHECK_EQ(r.start, At(2026, 11, 9, 9));
}


TEST(QuickEntryTaskWithoutDate)
{
	QuickEntryResult r = Parse("todo Buy milk #errands");
	CHECK(r.isTask);
	CHECK(!r.hasDate);
	CHECK(!r.hasTime);
	CHECK(r.dueString.empty());
	CHECK_EQ(r.Describe(), std::string("Task \xC2\xB7 #errands"));
}


TEST(QuickEntryTaskDueByTime)
{
	QuickEntryResult r = Parse("task Submit report by 5pm friday");
	CHECK_EQ(r.title, std::string("Submit report"));
	CHECK_EQ(r.start, At(2026, 11, 6, 17));
	CHECK_EQ(r.dueString, std::string("5pm friday"));
}


// --- Punctuation, case, robustness -------------------------------------------------

TEST(QuickEntryPunctuationAndCase)
{
	QuickEntryResult r = Parse("LUNCH WITH SAM, TOMORROW AT 1PM.");
	CHECK_EQ(r.title, std::string("LUNCH WITH SAM"));
	CHECK_EQ(r.start, At(2026, 11, 4, 13));
	r = Parse("  Follow-up   call,  Friday,  3pm  ");
	CHECK_EQ(r.title, std::string("Follow-up call"));
	CHECK_EQ(r.start, At(2026, 11, 6, 15));
	CHECK_EQ(Parse("COVID-19 booster").title, std::string("COVID-19 booster"));
	r = Parse("Call Bob on 555-1234 tomorrow");
	CHECK_EQ(r.title, std::string("Call Bob on 555-1234"));
	CHECK_EQ(Iso(r.startDate), std::string("2026-11-04"));
}


TEST(QuickEntryDescribeAllDay)
{
	QuickEntryResult r = Parse("Holiday tomorrow every year");
	std::string summary = r.Describe();
	CHECK(Contains(summary, "Wed, Nov 4"));
	CHECK(Contains(summary, "All day"));
	CHECK(Contains(summary, "Every year"));
}


TEST(QuickEntryGarbage)
{
	const char* inputs[] = {
		"", " ", "   \t\n", "!!!", "at", "in", "every", "from", "- ", "-", "/",
		"#", "@", "at at at", "from - to", "every other", "for", "for an",
		"Nov", "Nov 31", "Feb 30 2027", "99/99/99", "at 99", "at 25:61",
		"in 99999999 days", "every 99999 weeks", "for 9999999999 hours",
		"11-1", "3pm-", "-3pm", "\xE2\x80\x93", "\xFF\xFE\xE2\x80",
		"2026-13-45", "the", "the day after", "next", "this", "until",
		"remind me", "remind me to", "with alert alert alert", "1/2/3/4",
		"12:", ":30", "3:7pm", "p5", "every 0 days", "in 0 minutes",
	};
	for (const char* input : inputs) {
		QuickEntryResult r = Parse(input);
		CHECK(!r.title.empty());
		CHECK(r.startDate.IsValid());
		CHECK(r.allDay || r.end >= r.start);
		r.Describe();
	}
	CHECK_EQ(Parse("").title, std::string("New Event"));
	CHECK_EQ(Parse("Nov 31").title, std::string("Nov 31"));
}


TEST(QuickEntryVeryLongInput)
{
	std::string text;
	for (int i = 0; i < 20000; i++)
		text += "at 1m ";
	text += "tomorrow 3pm";
	QuickEntryResult r = Parse(text);
	CHECK(!r.title.empty());
	CHECK_EQ(r.start, At(2026, 11, 4, 15));

	std::string words;
	for (int i = 0; i < 50000; i++)
		words += "mondays and ";
	r = Parse(words);
	CHECK(!r.title.empty());

	std::string noise(100000, 'x');
	r = Parse(noise);
	CHECK_EQ(r.title.size(), noise.size());
}
