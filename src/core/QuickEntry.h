// Natural-language entry, Fantastical style:
//   "Lunch with Sam at Cafe Rio tomorrow 12:30pm"
//   "Dentist on Friday from 3-4pm alert 30 minutes before"
//   "Team sync every Monday at 9am for 30 minutes /Work"
//   "todo Pay rent due Nov 1 !!"     "remind me to call Mum tomorrow at 6pm"
#pragma once

#include "Model.h"

#include <string>
#include <vector>

namespace aircal {

struct QuickEntryResult {
	bool isTask = false;
	std::string title;
	std::string location;
	std::string calendarHint;	// text after '/', matched against calendar names
	std::vector<std::string> labels;	// #label (tasks)

	bool hasDate = false;		// a date was given
	bool hasTime = false;		// a start time was given
	bool allDay = true;
	Date startDate;
	Date endDate;				// inclusive last day for all-day ranges
	Instant start = 0;			// timed: start instant (local zone)
	Instant end = 0;			// timed: end instant
	std::string rrule;			// "FREQ=WEEKLY;BYDAY=MO"
	int alarmMinutes = -1;		// -1 none
	int priority = 0;			// tasks: "!" 2, "!!" 3, "!!!" 4, "p1".."p4"
	std::string dueString;		// tasks: the date phrase as typed (Todoist)

	// Human summary for the preview line under the entry field.
	std::string Describe() const;
};

// `now` is the reference instant; dates are interpreted in the local zone.
// defaultMinutes: event length when only a start time is given (60).
// weekStart: 0 = Sunday, 1 = Monday (for "next week").
QuickEntryResult ParseQuickEntry(const std::string& text, Instant now,
	int defaultMinutes = 60, int weekStart = 0);

}	// namespace aircal
