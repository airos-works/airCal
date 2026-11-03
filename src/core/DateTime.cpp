#include "DateTime.h"

#include <unicode/ucal.h>
#include <unicode/ustring.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace aircal {

// --- Civil dates (Howard Hinnant's algorithms) -------------------------------

int64_t
Date::DayNumber() const
{
	int64_t y = year - (month <= 2 ? 1 : 0);
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const int64_t yoe = y - era * 400;
	const int64_t mp = (month + 9) % 12;
	const int64_t doy = (153 * mp + 2) / 5 + day - 1;
	const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}


Date
Date::FromDayNumber(int64_t z)
{
	z += 719468;
	const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	const int64_t doe = z - era * 146097;
	const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const int64_t y = yoe + era * 400;
	const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const int64_t mp = (5 * doy + 2) / 153;
	const int64_t d = doy - (153 * mp + 2) / 5 + 1;
	const int64_t m = mp < 10 ? mp + 3 : mp - 9;
	return Date((int)(y + (m <= 2 ? 1 : 0)), (int)m, (int)d);
}


bool
IsLeapYear(int y)
{
	return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}


int
DaysInMonth(int year, int month)
{
	static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (month < 1 || month > 12)
		return 30;
	if (month == 2 && IsLeapYear(year))
		return 29;
	return kDays[month - 1];
}


Date
Date::AddMonths(int months) const
{
	int64_t total = (int64_t)year * 12 + (month - 1) + months;
	int y = (int)(total >= 0 ? total / 12 : (total - 11) / 12);
	int m = (int)(total - (int64_t)y * 12) + 1;
	int d = day;
	int dim = DaysInMonth(y, m);
	if (d > dim)
		d = dim;
	return Date(y, m, d);
}


int
Date::Weekday() const
{
	int64_t z = DayNumber();
	// 1970-01-01 was a Thursday (4).
	int64_t w = (z + 4) % 7;
	if (w < 0)
		w += 7;
	return (int)w;
}


int
Date::DayOfYear() const
{
	return (int)(DayNumber() - Date(year, 1, 1).DayNumber()) + 1;
}


bool
Date::IsValid() const
{
	return month >= 1 && month <= 12 && day >= 1
		&& day <= DaysInMonth(year, month) && year > 0 && year < 10000;
}


std::string
Date::ToIso() const
{
	char buffer[16];
	snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", year, month, day);
	return buffer;
}


std::string
Date::ToBasic() const
{
	char buffer[16];
	snprintf(buffer, sizeof(buffer), "%04d%02d%02d", year, month, day);
	return buffer;
}


bool
Date::FromIso(const std::string& text, Date& out)
{
	int y, m, d;
	char tail;
	if (text.size() >= 10 && text[4] == '-'
		&& sscanf(text.c_str(), "%4d-%2d-%2d", &y, &m, &d) == 3) {
		Date date(y, m, d);
		if (!date.IsValid())
			return false;
		out = date;
		return true;
	}
	if (text.size() >= 8 && sscanf(text.c_str(), "%4d%2d%2d%c", &y, &m, &d, &tail) >= 3) {
		Date date(y, m, d);
		if (!date.IsValid())
			return false;
		out = date;
		return true;
	}
	return false;
}


int64_t
DateTime::AsUtcSeconds() const
{
	return date.DayNumber() * 86400 + hour * 3600 + minute * 60 + second;
}


DateTime
DateTime::FromUtcSeconds(int64_t seconds)
{
	int64_t days = seconds >= 0 ? seconds / 86400 : (seconds - 86399) / 86400;
	int64_t rest = seconds - days * 86400;
	DateTime result(Date::FromDayNumber(days));
	result.hour = (int)(rest / 3600);
	result.minute = (int)(rest / 60 % 60);
	result.second = (int)(rest % 60);
	return result;
}


// --- Time zones ----------------------------------------------------------------

namespace {

std::mutex sZoneLock;
std::string sLocalZone;
std::string sUtc = "UTC";


std::vector<UChar>
ToUChars(const std::string& text)
{
	std::vector<UChar> result(text.size() * 2 + 2);
	int32_t length = 0;
	UErrorCode status = U_ZERO_ERROR;
	u_strFromUTF8(result.data(), (int32_t)result.size(), &length, text.c_str(),
		(int32_t)text.size(), &status);
	if (U_FAILURE(status))
		length = 0;
	result.resize(length);
	return result;
}


std::string
FromUChars(const UChar* text, int32_t length)
{
	std::string result(length * 3 + 1, '\0');
	int32_t written = 0;
	UErrorCode status = U_ZERO_ERROR;
	u_strToUTF8(&result[0], (int32_t)result.size(), &written, text, length,
		&status);
	if (U_FAILURE(status))
		return "";
	result.resize(written);
	return result;
}


std::string
Canonicalize(const std::string& tzid)
{
	if (tzid.empty())
		return "";
	std::string id = tzid;
	// Strip quotes and the Mozilla style "/mozilla.org/20050126_1/Area/City".
	if (id.size() > 1 && id.front() == '"' && id.back() == '"')
		id = id.substr(1, id.size() - 2);
	if (!id.empty() && id[0] == '/') {
		size_t last = id.rfind('/');
		size_t previous = last == std::string::npos || last == 0
			? std::string::npos : id.rfind('/', last - 1);
		if (previous != std::string::npos)
			id = id.substr(previous + 1);
	}
	if (id == "Z" || id == "UTC" || id == "GMT" || id == "Etc/UTC")
		return "UTC";

	std::vector<UChar> source = ToUChars(id);
	UChar canonical[128];
	UBool isSystem = false;
	UErrorCode status = U_ZERO_ERROR;
	int32_t length = ucal_getCanonicalTimeZoneID(source.data(),
		(int32_t)source.size(), canonical, 128, &isSystem, &status);
	if (U_SUCCESS(status) && isSystem && length > 0) {
		std::string result = FromUChars(canonical, length);
		if (result != "Etc/Unknown")
			return result;
	}

	// Windows names, as Outlook and Exchange write them.
	status = U_ZERO_ERROR;
	length = ucal_getTimeZoneIDForWindowsID(source.data(), (int32_t)source.size(),
		nullptr, canonical, 128, &status);
	if (U_SUCCESS(status) && length > 0)
		return FromUChars(canonical, length);
	return "";
}


struct CalendarCache {
	std::map<std::string, UCalendar*> calendars;
	std::map<std::string, std::string> canonical;

	~CalendarCache()
	{
		for (auto& entry : calendars)
			ucal_close(entry.second);
	}

	const std::string& Canonical(const std::string& tzid)
	{
		auto found = canonical.find(tzid);
		if (found != canonical.end())
			return found->second;
		return canonical[tzid] = Canonicalize(tzid);
	}

	UCalendar* Get(const std::string& requested)
	{
		std::string zone;
		if (requested.empty()) {
			std::lock_guard<std::mutex> lock(sZoneLock);
			zone = sLocalZone;
		} else
			zone = Canonical(requested);
		if (zone.empty())
			zone = requested.empty() ? std::string() : std::string("UTC");

		auto found = calendars.find(zone);
		if (found != calendars.end())
			return found->second;

		UErrorCode status = U_ZERO_ERROR;
		UCalendar* calendar;
		if (zone.empty()) {
			// No zone set: ICU's default.
			calendar = ucal_open(nullptr, 0, "en_US_POSIX", UCAL_GREGORIAN,
				&status);
		} else {
			std::vector<UChar> id = ToUChars(zone);
			calendar = ucal_open(id.data(), (int32_t)id.size(), "en_US_POSIX",
				UCAL_GREGORIAN, &status);
		}
		if (U_FAILURE(status) || calendar == nullptr)
			return nullptr;
		ucal_setGregorianChange(calendar, -1e16, &status);
		ucal_setAttribute(calendar, UCAL_LENIENT, 1);
		calendars[zone] = calendar;
		return calendar;
	}
};


CalendarCache&
Cache()
{
	static thread_local CalendarCache cache;
	return cache;
}


uint32_t sLocalGeneration = 0;
thread_local uint32_t tCacheGeneration = 0;


void
CheckGeneration()
{
	uint32_t generation;
	{
		std::lock_guard<std::mutex> lock(sZoneLock);
		generation = sLocalGeneration;
	}
	if (generation != tCacheGeneration) {
		CalendarCache& cache = Cache();
		auto found = cache.calendars.find(std::string());
		for (auto it = cache.calendars.begin(); it != cache.calendars.end();) {
			// Drop everything: the local entry may be keyed by its name.
			ucal_close(it->second);
			it = cache.calendars.erase(it);
		}
		(void)found;
		tCacheGeneration = generation;
	}
}

}	// namespace


void
SetLocalZone(const std::string& olsonId)
{
	std::string zone = Canonicalize(olsonId);
	std::lock_guard<std::mutex> lock(sZoneLock);
	sLocalZone = zone.empty() ? olsonId : zone;
	sLocalGeneration++;
}


const std::string&
LocalZone()
{
	std::lock_guard<std::mutex> lock(sZoneLock);
	return sLocalZone;
}


bool
IsKnownZone(const std::string& tzid)
{
	return !CanonicalZone(tzid).empty();
}


std::string
CanonicalZone(const std::string& tzid)
{
	return Cache().Canonical(tzid);
}


Instant
ToInstant(const DateTime& wall, const std::string& tzid)
{
	if (tzid == "UTC" || tzid == "Z")
		return wall.AsUtcSeconds();
	CheckGeneration();
	UCalendar* calendar = Cache().Get(tzid);
	if (calendar == nullptr)
		return wall.AsUtcSeconds();
	UErrorCode status = U_ZERO_ERROR;
	ucal_clear(calendar);
	ucal_setAttribute(calendar, UCAL_REPEATED_WALL_TIME, UCAL_WALLTIME_FIRST);
	ucal_setAttribute(calendar, UCAL_SKIPPED_WALL_TIME, UCAL_WALLTIME_NEXT_VALID);
	ucal_setDateTime(calendar, wall.date.year, wall.date.month - 1,
		wall.date.day, wall.hour, wall.minute, wall.second, &status);
	UDate millis = ucal_getMillis(calendar, &status);
	if (U_FAILURE(status))
		return wall.AsUtcSeconds();
	return (Instant)(millis / 1000.0 + (millis < 0 ? -0.5 : 0.5));
}


int
ZoneOffset(Instant instant, const std::string& tzid)
{
	if (tzid == "UTC" || tzid == "Z")
		return 0;
	CheckGeneration();
	UCalendar* calendar = Cache().Get(tzid);
	if (calendar == nullptr)
		return 0;
	UErrorCode status = U_ZERO_ERROR;
	ucal_setMillis(calendar, (UDate)instant * 1000.0, &status);
	int32_t zone = ucal_get(calendar, UCAL_ZONE_OFFSET, &status);
	int32_t dst = ucal_get(calendar, UCAL_DST_OFFSET, &status);
	if (U_FAILURE(status))
		return 0;
	return (zone + dst) / 1000;
}


DateTime
ToWall(Instant instant, const std::string& tzid)
{
	return DateTime::FromUtcSeconds(instant + ZoneOffset(instant, tzid));
}


Instant
Now()
{
	return (Instant)time(nullptr);
}


Date
Today()
{
	return LocalDate(Now());
}


// --- Formats -------------------------------------------------------------------

bool
ParseRfc3339(const std::string& text, Instant& out,
	const std::string* floatingZone)
{
	int y, mo, d, h = 0, mi = 0, s = 0;
	if (text.size() < 10 || sscanf(text.c_str(), "%4d-%2d-%2d", &y, &mo, &d) != 3)
		return false;
	Date date(y, mo, d);
	if (!date.IsValid())
		return false;
	size_t pos = 10;
	if (pos >= text.size()) {
		if (floatingZone == nullptr)
			return false;
		out = ToInstant(DateTime(date), *floatingZone);
		return true;
	}
	if (text[pos] != 'T' && text[pos] != 't' && text[pos] != ' ')
		return false;
	if (sscanf(text.c_str() + pos + 1, "%2d:%2d", &h, &mi) != 2)
		return false;
	pos += 6;
	if (pos < text.size() && text[pos] == ':') {
		if (sscanf(text.c_str() + pos + 1, "%2d", &s) != 1)
			return false;
		pos += 3;
	}
	if (pos < text.size() && (text[pos] == '.' || text[pos] == ',')) {
		pos++;
		while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
			pos++;
	}
	DateTime wall(date, h, mi, s);
	if (pos >= text.size()) {
		if (floatingZone == nullptr)
			return false;
		out = ToInstant(wall, *floatingZone);
		return true;
	}
	char sign = text[pos];
	if (sign == 'Z' || sign == 'z') {
		out = wall.AsUtcSeconds();
		return true;
	}
	if (sign != '+' && sign != '-')
		return false;
	int oh = 0, om = 0;
	if (sscanf(text.c_str() + pos + 1, "%2d:%2d", &oh, &om) < 1)
		return false;
	int offset = (oh * 3600 + om * 60) * (sign == '-' ? -1 : 1);
	out = wall.AsUtcSeconds() - offset;
	return true;
}


std::string
FormatRfc3339Utc(Instant t)
{
	DateTime wall = DateTime::FromUtcSeconds(t);
	char buffer[32];
	snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
		wall.date.year, wall.date.month, wall.date.day, wall.hour, wall.minute,
		wall.second);
	return buffer;
}


std::string
FormatRfc3339(Instant t, const std::string& tzid)
{
	int offset = ZoneOffset(t, tzid);
	DateTime wall = DateTime::FromUtcSeconds(t + offset);
	char buffer[40];
	int magnitude = offset < 0 ? -offset : offset;
	snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
		wall.date.year, wall.date.month, wall.date.day, wall.hour, wall.minute,
		wall.second, offset < 0 ? '-' : '+', magnitude / 3600,
		magnitude / 60 % 60);
	return buffer;
}


std::string
FormatBasicUtc(Instant t)
{
	DateTime wall = DateTime::FromUtcSeconds(t);
	return FormatBasicWall(wall) + "Z";
}


std::string
FormatBasicWall(const DateTime& wall)
{
	char buffer[32];
	snprintf(buffer, sizeof(buffer), "%04d%02d%02dT%02d%02d%02d",
		wall.date.year, wall.date.month, wall.date.day, wall.hour, wall.minute,
		wall.second);
	return buffer;
}


bool
ParseBasic(const std::string& input, DateTime& wall, bool& utc, bool& dateOnly)
{
	std::string text;
	for (char c : input) {
		if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
			text += c;
	}
	utc = false;
	dateOnly = false;
	int y, mo, d, h = 0, mi = 0, s = 0;
	if (text.size() < 8 || sscanf(text.c_str(), "%4d%2d%2d", &y, &mo, &d) != 3)
		return false;
	for (int i = 0; i < 8; i++) {
		if (text[i] < '0' || text[i] > '9')
			return false;
	}
	Date date(y, mo, d);
	if (!date.IsValid())
		return false;
	if (text.size() == 8) {
		dateOnly = true;
		wall = DateTime(date);
		return true;
	}
	if (text[8] != 'T' || text.size() < 15
		|| sscanf(text.c_str() + 9, "%2d%2d%2d", &h, &mi, &s) != 3)
		return false;
	if (h > 24 || mi > 59 || s > 60)
		return false;
	wall = DateTime(date, h, mi, s);
	if (h == 24) {
		wall = DateTime(date.AddDays(1), 0, mi, s);
	}
	utc = text.size() > 15 && (text[15] == 'Z' || text[15] == 'z');
	return true;
}

}	// namespace aircal
