#include "Format.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <vector>

using aircal::Date;
using aircal::Instant;

namespace Format {

namespace {

bool sUse24Hour = false;

const char* kWeekdaysShort[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* kWeekdaysLong[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
	"Thursday", "Friday", "Saturday"};
const char* kMonthsShort[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul",
	"Aug", "Sep", "Oct", "Nov", "Dec"};
const char* kMonthsLong[] = {"January", "February", "March", "April", "May",
	"June", "July", "August", "September", "October", "November", "December"};


std::string
Lower(const std::string& text)
{
	std::string result;
	for (char c : text)
		result += (char)tolower((unsigned char)c);
	return result;
}


std::string
Trim(const std::string& text)
{
	size_t begin = text.find_first_not_of(" \t,.");
	if (begin == std::string::npos)
		return "";
	size_t end = text.find_last_not_of(" \t,.");
	return text.substr(begin, end - begin + 1);
}

}	// namespace


void
SetUse24Hour(bool use24Hour)
{
	sUse24Hour = use24Hour;
}


bool
Use24Hour()
{
	return sUse24Hour;
}


std::string
Time(Instant t, bool compact)
{
	aircal::DateTime wall = aircal::ToWall(t);
	char buffer[32];
	if (sUse24Hour) {
		snprintf(buffer, sizeof(buffer), "%d:%02d", wall.hour, wall.minute);
		return buffer;
	}
	int hour = wall.hour % 12;
	if (hour == 0)
		hour = 12;
	const char* suffix = wall.hour < 12 ? "AM" : "PM";
	if (compact) {
		if (wall.minute == 0)
			snprintf(buffer, sizeof(buffer), "%d%s", hour, wall.hour < 12 ? "a" : "p");
		else
			snprintf(buffer, sizeof(buffer), "%d:%02d%s", hour, wall.minute,
				wall.hour < 12 ? "a" : "p");
		return buffer;
	}
	snprintf(buffer, sizeof(buffer), "%d:%02d %s", hour, wall.minute, suffix);
	return buffer;
}


std::string
Hour(int hour)
{
	char buffer[16];
	if (sUse24Hour) {
		snprintf(buffer, sizeof(buffer), "%02d:00", hour % 24);
		return buffer;
	}
	if (hour % 24 == 12)
		return "Noon";
	int h = hour % 12;
	if (h == 0)
		h = 12;
	snprintf(buffer, sizeof(buffer), "%d %s", h, hour % 24 < 12 ? "AM" : "PM");
	return buffer;
}


std::string
TimeRange(Instant start, Instant end)
{
	if (end <= start)
		return Time(start);
	if (aircal::LocalDate(start) != aircal::LocalDate(end - 1))
		return Time(start) + " – " + DateShort(aircal::LocalDate(end)) + " "
			+ Time(end);
	return Time(start) + " – " + Time(end);
}


std::string WeekdayShort(int w) { return kWeekdaysShort[((w % 7) + 7) % 7]; }
std::string WeekdayLong(int w) { return kWeekdaysLong[((w % 7) + 7) % 7]; }
std::string WeekdayLetter(int w) { return std::string(1, kWeekdaysShort[((w % 7) + 7) % 7][0]); }
std::string MonthShort(int m) { return kMonthsShort[(m + 11) % 12]; }
std::string MonthLong(int m) { return kMonthsLong[(m + 11) % 12]; }


std::string
DateShort(const Date& d)
{
	return MonthShort(d.month) + " " + std::to_string(d.day);
}


std::string
DateMedium(const Date& d)
{
	std::string text = WeekdayShort(d.Weekday()) + ", " + DateShort(d);
	if (d.year != aircal::Today().year)
		text += ", " + std::to_string(d.year);
	return text;
}


std::string
DateLong(const Date& d)
{
	return WeekdayLong(d.Weekday()) + ", " + MonthLong(d.month) + " "
		+ std::to_string(d.day) + ", " + std::to_string(d.year);
}


std::string
DateNumeric(const Date& d)
{
	char buffer[32];
	snprintf(buffer, sizeof(buffer), "%d/%d/%d", d.month, d.day, d.year);
	return buffer;
}


std::string
RelativeDay(const Date& d)
{
	Date today = aircal::Today();
	int64_t delta = d.DayNumber() - today.DayNumber();
	if (delta == 0)
		return "Today";
	if (delta == 1)
		return "Tomorrow";
	if (delta == -1)
		return "Yesterday";
	return DateMedium(d);
}


std::string
Duration(int minutes)
{
	if (minutes < 60)
		return std::to_string(minutes) + " min";
	if (minutes % 1440 == 0)
		return std::to_string(minutes / 1440) + (minutes == 1440 ? " day" : " days");
	std::string text = std::to_string(minutes / 60) + " hr";
	if (minutes % 60 != 0)
		text += " " + std::to_string(minutes % 60) + " min";
	return text;
}


std::string
AlarmText(int minutes)
{
	if (minutes < 0)
		return "None";
	if (minutes == 0)
		return "At time of event";
	if (minutes % 10080 == 0)
		return std::to_string(minutes / 10080)
			+ (minutes == 10080 ? " week before" : " weeks before");
	if (minutes % 1440 == 0)
		return std::to_string(minutes / 1440)
			+ (minutes == 1440 ? " day before" : " days before");
	if (minutes % 60 == 0)
		return std::to_string(minutes / 60)
			+ (minutes == 60 ? " hour before" : " hours before");
	return std::to_string(minutes) + " minutes before";
}


bool
ParseTime(const std::string& input, int& hour, int& minute)
{
	std::string text = Lower(Trim(input));
	if (text.empty())
		return false;
	if (text == "noon") {
		hour = 12;
		minute = 0;
		return true;
	}
	if (text == "midnight") {
		hour = 0;
		minute = 0;
		return true;
	}
	size_t pos = 0;
	auto number = [&](int& out) {
		size_t start = pos;
		while (pos < text.size() && isdigit((unsigned char)text[pos]))
			pos++;
		if (pos == start || pos - start > 4)
			return false;
		out = atoi(text.substr(start, pos - start).c_str());
		return true;
	};
	int h = 0, m = 0;
	if (!number(h))
		return false;
	size_t digits = pos;
	if (pos < text.size() && (text[pos] == ':' || text[pos] == '.')) {
		pos++;
		if (!number(m))
			return false;
	} else if (digits >= 3) {
		// "930", "1530"
		m = h % 100;
		h /= 100;
	}
	while (pos < text.size() && text[pos] == ' ')
		pos++;
	std::string suffix = text.substr(pos);
	if (suffix == "a" || suffix == "am" || suffix == "a.m") {
		if (h < 1 || h > 12)
			return false;
		h = h % 12;
	} else if (suffix == "p" || suffix == "pm" || suffix == "p.m") {
		if (h < 1 || h > 12)
			return false;
		h = h % 12 + 12;
	} else if (!suffix.empty())
		return false;
	if (h > 23 || m > 59)
		return false;
	hour = h;
	minute = m;
	return true;
}


bool
ParseDate(const std::string& input, Date& out)
{
	std::string text = Lower(Trim(input));
	if (text.empty())
		return false;
	Date today = aircal::Today();
	if (text == "today") {
		out = today;
		return true;
	}
	if (text == "tomorrow" || text == "tmrw") {
		out = today.AddDays(1);
		return true;
	}
	if (text == "yesterday") {
		out = today.AddDays(-1);
		return true;
	}
	if (Date::FromIso(text, out))
		return true;
	// Weekday names: the next such day (today counts).
	for (int w = 0; w < 7; w++) {
		std::string name = Lower(kWeekdaysLong[w]);
		if (text == name || text == name.substr(0, 3)
			|| (text.size() >= 3 && name.compare(0, text.size(), text) == 0)) {
			int delta = (w - today.Weekday() + 7) % 7;
			out = today.AddDays(delta);
			return true;
		}
	}
	// Tokens: numbers and month names.
	std::vector<std::string> tokens;
	std::string current;
	for (char c : text) {
		if (isalnum((unsigned char)c))
			current += c;
		else {
			if (!current.empty())
				tokens.push_back(current);
			current.clear();
		}
	}
	if (!current.empty())
		tokens.push_back(current);
	int month = 0, day = 0, year = 0;
	std::vector<int> numbers;
	bool slashes = text.find('/') != std::string::npos;
	for (auto& token : tokens) {
		bool isNumber = isdigit((unsigned char)token[0]);
		if (isNumber) {
			numbers.push_back(atoi(token.c_str()));	// "3rd" -> 3
			continue;
		}
		if (token.size() >= 3) {
			for (int m = 0; m < 12; m++) {
				std::string name = Lower(kMonthsLong[m]);
				if (name.compare(0, token.size(), token) == 0
					|| token == Lower(kMonthsShort[m])) {
					month = m + 1;
					break;
				}
			}
			// Weekday names in "Tue, Nov 3" are ignored.
		}
	}
	if (month != 0) {
		if (numbers.empty())
			return false;
		day = numbers[0];
		if (numbers.size() > 1)
			year = numbers[1];
	} else if (slashes && numbers.size() >= 2) {
		month = numbers[0];
		day = numbers[1];
		if (numbers.size() > 2)
			year = numbers[2];
	} else
		return false;
	if (year != 0 && year < 100)
		year += 2000;
	bool explicitYear = year != 0;
	if (year == 0)
		year = today.year;
	Date date(year, month, day);
	if (!date.IsValid())
		return false;
	// "Jan 5" typed in December means next January.
	if (!explicitYear && date.DayNumber() < today.DayNumber() - 60)
		date = Date(year + 1, month, day);
	if (!date.IsValid())
		return false;
	out = date;
	return true;
}

}	// namespace Format
