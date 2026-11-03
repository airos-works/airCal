// RFC 5545 recurrence rules: parsing, building and expansion.
#pragma once

#include "Model.h"

#include <string>
#include <vector>

namespace aircal {

struct RecurrenceRule {
	enum Frequency { None, Secondly, Minutely, Hourly, Daily, Weekly, Monthly,
		Yearly };

	struct WeekdayNum {
		int weekday = 0;	// 0 = SU ... 6 = SA
		int ordinal = 0;	// 0 = every, 1 = first, -1 = last ...
	};

	Frequency frequency = None;
	int interval = 1;
	int count = 0;				// 0 = unlimited
	bool hasUntil = false;
	bool untilIsDate = false;
	Instant until = 0;			// when !untilIsDate (UTC)
	Date untilDate;				// when untilIsDate
	// UNTIL without 'Z': `until` then holds the wall time (as if UTC) in the
	// event's zone.
	bool untilFloating = false;
	std::vector<WeekdayNum> byDay;
	std::vector<int> byMonthDay;	// 1..31, -31..-1
	std::vector<int> byMonth;		// 1..12
	std::vector<int> byYearDay;
	std::vector<int> byWeekNo;
	std::vector<int> bySetPos;
	std::vector<int> byHour, byMinute, bySecond;
	int weekStart = 1;			// 0 = SU, 1 = MO (RFC default MO)

	// Parses "FREQ=WEEKLY;BYDAY=MO,WE" (an "RRULE:" prefix is allowed).
	static bool Parse(const std::string& text, RecurrenceRule& out);
	std::string ToString() const;	// canonical text without the prefix
};

// Expands a master event into occurrences that overlap [rangeStart, rangeEnd)
// (instants; for all-day events the local days overlapping the range).
// Excludes exdates, applies RDATEs, and replaces instances that have an
// override (overrides: events whose masterId is master.id). Cancelled
// overrides remove their instance. Overrides moved into the range are
// included even when the original instance lies outside it. Stops after
// maxCount occurrences as a safety net.
// A non-recurring event yields itself if it overlaps.
std::vector<Occurrence> ExpandEvent(const Event& master,
	const std::vector<const Event*>& overrides, Instant rangeStart,
	Instant rangeEnd, size_t maxCount = 5000);

// Start instants (timed) or dates (all-day, as local midnight instants) of a
// rule's instances from the series start, for tests and the editor's
// "ends after" preview. Same semantics as ExpandEvent minus overrides.
std::vector<Instant> RuleInstances(const Event& master, Instant rangeStart,
	Instant rangeEnd, size_t maxCount = 5000);

// The rule with UNTIL set so that the series ends before `before` (used for
// "delete this and future events"). Removes COUNT.
std::string TruncateRule(const std::string& rrule, const Event& master,
	Instant before);

}	// namespace aircal
