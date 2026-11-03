// Date and time text for the UI ("9:30 AM", "Tue, Nov 3", "November 2026").
#pragma once

#include "core/DateTime.h"

#include <string>

namespace Format {

void SetUse24Hour(bool use24Hour);
bool Use24Hour();

std::string Time(aircal::Instant t, bool compact = false);	// "9:30 AM" / "9:30a" / "21:30"
std::string Hour(int hour);				// "9 AM" / "09:00"
std::string TimeRange(aircal::Instant start, aircal::Instant end);
std::string WeekdayShort(int weekday);	// "Mon"
std::string WeekdayLong(int weekday);	// "Monday"
std::string WeekdayLetter(int weekday);	// "M"
std::string MonthShort(int month);		// "Nov"
std::string MonthLong(int month);		// "November"
std::string DateShort(const aircal::Date& d);		// "Nov 3"
std::string DateMedium(const aircal::Date& d);	// "Tue, Nov 3"
std::string DateLong(const aircal::Date& d);		// "Tuesday, November 3, 2026"
std::string DateNumeric(const aircal::Date& d);	// "11/3/2026"
// "Today", "Tomorrow", "Yesterday" or DateMedium.
std::string RelativeDay(const aircal::Date& d);
std::string Duration(int minutes);		// "1 hr 30 min"
std::string AlarmText(int minutes);		// "15 minutes before", "At time of event"

// Parses what people type into date/time fields: "Nov 3", "11/3/2026",
// "2026-11-03", "tomorrow", "fri", "3pm", "15:30", "9:30 am".
bool ParseDate(const std::string& text, aircal::Date& out);
bool ParseTime(const std::string& text, int& hour, int& minute);

}	// namespace Format
