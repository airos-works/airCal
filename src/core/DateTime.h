// Civil dates, wall-clock times and time zones.
//
// airCal keeps every timed instant as UTC seconds since the epoch and every
// all-day date as a civil (proleptic Gregorian) date. Wall-clock conversions
// go through ICU with the zone's Olson ID; "" means the local zone (see
// SetLocalZone) and "UTC" is UTC.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aircal {

typedef int64_t Instant;	// seconds since 1970-01-01T00:00:00Z

struct Date {
	int year = 1970;
	int month = 1;	// 1..12
	int day = 1;	// 1..31

	Date() = default;
	Date(int y, int m, int d) : year(y), month(m), day(d) {}

	// Days since 1970-01-01 (negative before).
	int64_t DayNumber() const;
	static Date FromDayNumber(int64_t days);

	Date AddDays(int64_t days) const { return FromDayNumber(DayNumber() + days); }
	// Adds months, clamping the day to the target month's length.
	Date AddMonths(int months) const;
	Date AddYears(int years) const { return AddMonths(years * 12); }

	// 0 = Sunday ... 6 = Saturday
	int Weekday() const;
	int DayOfYear() const;	// 1..366
	bool IsValid() const;

	std::string ToIso() const;	// YYYY-MM-DD
	std::string ToBasic() const;	// YYYYMMDD
	static bool FromIso(const std::string& text, Date& out);	// YYYY-MM-DD or YYYYMMDD

	bool operator==(const Date& o) const
		{ return year == o.year && month == o.month && day == o.day; }
	bool operator!=(const Date& o) const { return !(*this == o); }
	bool operator<(const Date& o) const { return DayNumber() < o.DayNumber(); }
	bool operator<=(const Date& o) const { return DayNumber() <= o.DayNumber(); }
	bool operator>(const Date& o) const { return o < *this; }
	bool operator>=(const Date& o) const { return o <= *this; }
};

int DaysInMonth(int year, int month);
bool IsLeapYear(int year);

struct DateTime {
	Date date;
	int hour = 0;
	int minute = 0;
	int second = 0;

	DateTime() = default;
	DateTime(Date d, int h = 0, int m = 0, int s = 0)
		: date(d), hour(h), minute(m), second(s) {}

	// Seconds since the epoch if this wall time were UTC.
	int64_t AsUtcSeconds() const;
	static DateTime FromUtcSeconds(int64_t seconds);

	bool operator==(const DateTime& o) const
		{ return AsUtcSeconds() == o.AsUtcSeconds(); }
	bool operator<(const DateTime& o) const
		{ return AsUtcSeconds() < o.AsUtcSeconds(); }
};

// --- Time zones ------------------------------------------------------------

// Sets the zone "" stands for (the system's zone, e.g. "Australia/Hobart").
void SetLocalZone(const std::string& olsonId);
const std::string& LocalZone();

// Whether ICU knows the zone (after mapping Windows names such as
// "Eastern Standard Time").
bool IsKnownZone(const std::string& tzid);
// The Olson ID for an iCalendar/Google TZID, "" if unknown.
std::string CanonicalZone(const std::string& tzid);

// Wall time in a zone -> instant. Nonexistent times (DST gaps) move forward.
Instant ToInstant(const DateTime& wall, const std::string& tzid = "");
// Instant -> wall time in a zone.
DateTime ToWall(Instant instant, const std::string& tzid = "");
// Offset from UTC in seconds at that instant.
int ZoneOffset(Instant instant, const std::string& tzid = "");

// The local date an instant falls on.
inline Date LocalDate(Instant t) { return ToWall(t).date; }
// Local midnight at the start of a date.
inline Instant StartOfDay(const Date& d) { return ToInstant(DateTime(d)); }

// Every "Area/City" zone ICU knows, sorted.
std::vector<std::string> AllZones();

Instant Now();
Date Today();

// --- Formats -----------------------------------------------------------------

// RFC 3339: 2026-11-03T14:00:00Z, 2026-11-03T14:00:00.000+11:00. False if
// the text has no offset (a floating time) unless floatingZone is given.
bool ParseRfc3339(const std::string& text, Instant& out,
	const std::string* floatingZone = nullptr);
std::string FormatRfc3339Utc(Instant t);	// 2026-11-03T03:00:00Z
std::string FormatRfc3339(Instant t, const std::string& tzid);	// with offset

// iCalendar basic format: 20261103T140000Z (UTC) or 20261103T140000.
std::string FormatBasicUtc(Instant t);
std::string FormatBasicWall(const DateTime& wall);
bool ParseBasic(const std::string& text, DateTime& wall, bool& utc,
	bool& dateOnly);

}	// namespace aircal
