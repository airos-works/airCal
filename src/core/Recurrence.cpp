#include "Recurrence.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>

namespace aircal {

namespace {

const char* kWeekdayCodes[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};

// Wall-clock times are within a day of the instants they stand for (zone
// offsets are at most +-14 hours), so comparisons of wall seconds against
// instants with this much slack are safe without a zone lookup.
const int64_t kSlack = 2 * 86400;

// Safety net against rules that never match (FREQ=DAILY;BYMONTHDAY=30;BYMONTH=2).
const int64_t kMaxPeriods = 4000000;


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


std::vector<std::string>
Split(const std::string& text, char separator)
{
	std::vector<std::string> parts;
	size_t start = 0;
	while (true) {
		size_t found = text.find(separator, start);
		parts.push_back(text.substr(start, found == std::string::npos
			? std::string::npos : found - start));
		if (found == std::string::npos)
			break;
		start = found + 1;
	}
	return parts;
}


bool
ParseInt(const std::string& text, int& out)
{
	if (text.empty())
		return false;
	char* end = nullptr;
	long value = strtol(text.c_str(), &end, 10);
	if (end == text.c_str() || *end != '\0')
		return false;
	if (value < -1000000 || value > 1000000)
		return false;
	out = (int)value;
	return true;
}


int
WeekdayFromCode(const std::string& code)
{
	for (int i = 0; i < 7; i++) {
		if (code == kWeekdayCodes[i])
			return i;
	}
	return -1;
}


// Parses "1,-1,15" keeping values in [minimum, maximum] (and != 0 when
// zeroAllowed is false).
void
ParseIntList(const std::string& value, int minimum, int maximum,
	bool zeroAllowed, std::vector<int>& out)
{
	out.clear();
	for (const std::string& item : Split(value, ',')) {
		int number;
		if (!ParseInt(Trim(item), number))
			continue;
		if (number < minimum || number > maximum || (number == 0 && !zeroAllowed))
			continue;
		out.push_back(number);
	}
}


std::string
JoinInts(const std::vector<int>& values)
{
	std::string result;
	for (size_t i = 0; i < values.size(); i++) {
		if (i > 0)
			result += ",";
		result += std::to_string(values[i]);
	}
	return result;
}


bool
Contains(const std::vector<int>& values, int value)
{
	return std::find(values.begin(), values.end(), value) != values.end();
}


std::vector<int>
Sorted(std::vector<int> values)
{
	std::sort(values.begin(), values.end());
	values.erase(std::unique(values.begin(), values.end()), values.end());
	return values;
}


int
DaysInYear(int year)
{
	return IsLeapYear(year) ? 366 : 365;
}


// The day number of the first day of the week (starting on weekStart) that
// contains the date.
int64_t
WeekStartDay(const Date& date, int weekStart)
{
	return date.DayNumber() - ((date.Weekday() - weekStart + 7) % 7);
}


// ISO 8601 style week numbering with a configurable first weekday: week 1 is
// the first week with at least four days in the year.
void
WeekNumber(const Date& date, int weekStart, int& week, int& weeksInYear)
{
	int64_t start = WeekStartDay(date, weekStart);
	int weekYear = Date::FromDayNumber(start + 3).year;
	int64_t firstWeek = WeekStartDay(Date(weekYear, 1, 4), weekStart);
	week = (int)((start - firstWeek) / 7) + 1;
	int64_t lastWeek = WeekStartDay(Date(weekYear, 12, 28), weekStart);
	weeksInYear = (int)((lastWeek - firstWeek) / 7) + 1;
}


bool
Overlaps(Instant start, Instant end, Instant rangeStart, Instant rangeEnd)
{
	if (end <= start)
		return start >= rangeStart && start < rangeEnd;
	return start < rangeEnd && end > rangeStart;
}


typedef std::function<bool(const DateTime&)> WallVisitor;


// Generates a rule's instances as wall-clock times (in the event's zone) in
// order. The series start itself is not generated: callers add it first.
class Expansion {
public:
	Expansion(const RecurrenceRule& rule, const DateTime& start, bool allDay,
		const std::string& tzid);

	// Visits instances after the start until visit returns false or an
	// instance passes UNTIL. Periods that end before skipTo (wall seconds)
	// are skipped (only allowed without COUNT); periods starting after
	// stopAfter (wall seconds) end the expansion.
	void Run(int64_t skipTo, int64_t stopAfter, const WallVisitor& visit) const;

private:
	enum Scope { kNoScope, kMonthScope, kYearScope };

	bool DayMatches(const Date& date) const;
	bool PeriodDays(int64_t period, std::vector<Date>& days,
		int64_t& periodStart) const;
	int64_t FirstPeriod(int64_t skipTo) const;
	bool PastUntil(const DateTime& wall, int64_t wallSeconds) const;
	void ApplySetPos(std::vector<DateTime>& candidates) const;

	const RecurrenceRule& fRule;
	RecurrenceRule::Frequency fFrequency;
	int fInterval;
	DateTime fStart;
	int64_t fStartSeconds;
	bool fAllDay;
	std::string fZone;
	std::vector<int> fByMonth;
	std::vector<int> fByMonthDay;
	std::vector<RecurrenceRule::WeekdayNum> fByDay;
	std::vector<int> fHours;
	std::vector<int> fMinutes;
	std::vector<int> fSeconds;
	Scope fScope;
	int64_t fWeekStart0;
};


Expansion::Expansion(const RecurrenceRule& rule, const DateTime& start,
	bool allDay, const std::string& tzid)
	:
	fRule(rule),
	fFrequency(rule.frequency),
	fInterval(std::max(1, rule.interval)),
	fStart(start),
	fStartSeconds(start.AsUtcSeconds()),
	fAllDay(allDay),
	fZone(tzid),
	fByMonth(Sorted(rule.byMonth)),
	fByMonthDay(rule.byMonthDay),
	fByDay(rule.byDay),
	fScope(kNoScope)
{
	// All-day series have no time of day to repeat on.
	if (fAllDay && fFrequency < RecurrenceRule::Daily)
		fFrequency = RecurrenceRule::Daily;

	// RFC 5545: missing BYxxx parts default to DTSTART's.
	bool noDays = rule.byWeekNo.empty() && rule.byYearDay.empty()
		&& fByMonthDay.empty() && fByDay.empty();
	if (noDays) {
		switch (fFrequency) {
			case RecurrenceRule::Yearly:
				if (fByMonth.empty())
					fByMonth.push_back(start.date.month);
				fByMonthDay.push_back(start.date.day);
				break;
			case RecurrenceRule::Monthly:
				fByMonthDay.push_back(start.date.day);
				break;
			case RecurrenceRule::Weekly:
			{
				RecurrenceRule::WeekdayNum day;
				day.weekday = start.date.Weekday();
				fByDay.push_back(day);
				break;
			}
			default:
				break;
		}
	}

	if (fFrequency == RecurrenceRule::Monthly)
		fScope = kMonthScope;
	else if (fFrequency == RecurrenceRule::Yearly) {
		if (!fByMonth.empty())
			fScope = kMonthScope;
		else if (rule.byWeekNo.empty())
			fScope = kYearScope;
	}

	if (fAllDay) {
		fHours.push_back(0);
		fMinutes.push_back(0);
		fSeconds.push_back(0);
	} else {
		fHours = rule.byHour.empty() ? std::vector<int>(1, start.hour)
			: Sorted(rule.byHour);
		fMinutes = rule.byMinute.empty() ? std::vector<int>(1, start.minute)
			: Sorted(rule.byMinute);
		fSeconds = rule.bySecond.empty() ? std::vector<int>(1, start.second)
			: Sorted(rule.bySecond);
	}
	fWeekStart0 = WeekStartDay(start.date, fRule.weekStart);
}


bool
Expansion::DayMatches(const Date& date) const
{
	if (!fByMonth.empty() && !Contains(fByMonth, date.month))
		return false;
	if (!fRule.byWeekNo.empty()) {
		int week, weeks;
		WeekNumber(date, fRule.weekStart, week, weeks);
		if (!Contains(fRule.byWeekNo, week)
			&& !Contains(fRule.byWeekNo, week - weeks - 1))
			return false;
	}
	int yearLength = DaysInYear(date.year);
	int dayOfYear = 0;
	if (!fRule.byYearDay.empty() || (!fByDay.empty() && fScope == kYearScope))
		dayOfYear = date.DayOfYear();
	if (!fRule.byYearDay.empty()) {
		if (!Contains(fRule.byYearDay, dayOfYear)
			&& !Contains(fRule.byYearDay, dayOfYear - yearLength - 1))
			return false;
	}
	int monthLength = DaysInMonth(date.year, date.month);
	if (!fByMonthDay.empty()) {
		if (!Contains(fByMonthDay, date.day)
			&& !Contains(fByMonthDay, date.day - monthLength - 1))
			return false;
	}
	if (!fByDay.empty()) {
		int weekday = date.Weekday();
		bool matched = false;
		for (const RecurrenceRule::WeekdayNum& day : fByDay) {
			if (day.weekday != weekday)
				continue;
			if (day.ordinal == 0 || fScope == kNoScope) {
				matched = true;
				break;
			}
			int nth, nthLast;
			if (fScope == kMonthScope) {
				nth = (date.day - 1) / 7 + 1;
				nthLast = -((monthLength - date.day) / 7 + 1);
			} else {
				nth = (dayOfYear - 1) / 7 + 1;
				nthLast = -((yearLength - dayOfYear) / 7 + 1);
			}
			if (day.ordinal == nth || day.ordinal == nthLast) {
				matched = true;
				break;
			}
		}
		if (!matched)
			return false;
	}
	return true;
}


bool
Expansion::PeriodDays(int64_t period, std::vector<Date>& days,
	int64_t& periodStart) const
{
	int64_t step = period * fInterval;
	switch (fFrequency) {
		case RecurrenceRule::Yearly:
		{
			int64_t year = fStart.date.year + step;
			if (year > 9999)
				return false;
			periodStart = DateTime(Date((int)year, 1, 1)).AsUtcSeconds();
			for (int month = 1; month <= 12; month++) {
				if (!fByMonth.empty() && !Contains(fByMonth, month))
					continue;
				int length = DaysInMonth((int)year, month);
				for (int day = 1; day <= length; day++) {
					Date date((int)year, month, day);
					if (DayMatches(date))
						days.push_back(date);
				}
			}
			return true;
		}
		case RecurrenceRule::Monthly:
		{
			int64_t total = (int64_t)fStart.date.year * 12 + fStart.date.month - 1
				+ step;
			int year = (int)(total / 12);
			int month = (int)(total % 12) + 1;
			if (year > 9999)
				return false;
			periodStart = DateTime(Date(year, month, 1)).AsUtcSeconds();
			int length = DaysInMonth(year, month);
			for (int day = 1; day <= length; day++) {
				Date date(year, month, day);
				if (DayMatches(date))
					days.push_back(date);
			}
			return true;
		}
		case RecurrenceRule::Weekly:
		{
			int64_t first = fWeekStart0 + step * 7;
			Date firstDate = Date::FromDayNumber(first);
			if (firstDate.year > 9999)
				return false;
			periodStart = DateTime(firstDate).AsUtcSeconds();
			for (int i = 0; i < 7; i++) {
				Date date = Date::FromDayNumber(first + i);
				if (DayMatches(date))
					days.push_back(date);
			}
			return true;
		}
		default:
		{
			Date date = fStart.date.AddDays(step);
			if (date.year > 9999)
				return false;
			periodStart = DateTime(date).AsUtcSeconds();
			if (DayMatches(date))
				days.push_back(date);
			return true;
		}
	}
}


int64_t
Expansion::FirstPeriod(int64_t skipTo) const
{
	DateTime target = DateTime::FromUtcSeconds(skipTo);
	int64_t difference;
	switch (fFrequency) {
		case RecurrenceRule::Yearly:
			difference = target.date.year - fStart.date.year;
			break;
		case RecurrenceRule::Monthly:
			difference = ((int64_t)target.date.year * 12 + target.date.month)
				- ((int64_t)fStart.date.year * 12 + fStart.date.month);
			break;
		case RecurrenceRule::Weekly:
			difference = (WeekStartDay(target.date, fRule.weekStart)
				- fWeekStart0) / 7;
			break;
		case RecurrenceRule::Daily:
			difference = target.date.DayNumber() - fStart.date.DayNumber();
			break;
		case RecurrenceRule::Hourly:
			difference = (skipTo - fStartSeconds) / 3600;
			break;
		case RecurrenceRule::Minutely:
			difference = (skipTo - fStartSeconds) / 60;
			break;
		default:
			difference = skipTo - fStartSeconds;
			break;
	}
	if (difference <= 0)
		return 0;
	return difference / fInterval;
}


bool
Expansion::PastUntil(const DateTime& wall, int64_t wallSeconds) const
{
	if (!fRule.hasUntil)
		return false;
	if (fRule.untilIsDate)
		return wall.date > fRule.untilDate;
	if (fRule.untilFloating)
		return wallSeconds > fRule.until;
	if (wallSeconds - kSlack > fRule.until)
		return true;
	if (wallSeconds + kSlack <= fRule.until)
		return false;
	Instant instant = fAllDay ? StartOfDay(wall.date) : ToInstant(wall, fZone);
	return instant > fRule.until;
}


void
Expansion::ApplySetPos(std::vector<DateTime>& candidates) const
{
	std::vector<DateTime> chosen;
	int count = (int)candidates.size();
	for (int position : fRule.bySetPos) {
		int index = position > 0 ? position - 1 : count + position;
		if (index >= 0 && index < count)
			chosen.push_back(candidates[index]);
	}
	std::sort(chosen.begin(), chosen.end());
	chosen.erase(std::unique(chosen.begin(), chosen.end()), chosen.end());
	candidates.swap(chosen);
}


void
Expansion::Run(int64_t skipTo, int64_t stopAfter,
	const WallVisitor& visit) const
{
	if (fFrequency == RecurrenceRule::None)
		return;
	int64_t period = 0;
	if (fRule.count == 0 && skipTo > fStartSeconds)
		period = FirstPeriod(skipTo);

	int64_t unit = fFrequency == RecurrenceRule::Hourly ? 3600
		: fFrequency == RecurrenceRule::Minutely ? 60 : 1;
	std::vector<Date> days;
	std::vector<DateTime> candidates;
	for (int64_t iteration = 0; iteration < kMaxPeriods; iteration++, period++) {
		candidates.clear();
		if (fFrequency >= RecurrenceRule::Daily) {
			days.clear();
			int64_t periodStart = 0;
			if (!PeriodDays(period, days, periodStart))
				return;
			if (periodStart > stopAfter)
				return;
			for (const Date& date : days) {
				for (int hour : fHours) {
					for (int minute : fMinutes) {
						for (int second : fSeconds)
							candidates.push_back(DateTime(date, hour, minute, second));
					}
				}
			}
		} else {
			int64_t base = fStartSeconds + period * fInterval * unit;
			if (base > stopAfter)
				return;
			DateTime wall = DateTime::FromUtcSeconds(base);
			if (wall.date.year > 9999)
				return;
			if (!DayMatches(wall.date))
				continue;
			if (!fRule.byHour.empty() && !Contains(fRule.byHour, wall.hour))
				continue;
			if (fFrequency <= RecurrenceRule::Minutely && !fRule.byMinute.empty()
				&& !Contains(fRule.byMinute, wall.minute))
				continue;
			if (fFrequency == RecurrenceRule::Secondly && !fRule.bySecond.empty()
				&& !Contains(fRule.bySecond, wall.second))
				continue;
			if (fFrequency == RecurrenceRule::Hourly) {
				for (int minute : fMinutes) {
					for (int second : fSeconds) {
						candidates.push_back(DateTime(wall.date, wall.hour, minute,
							second));
					}
				}
			} else if (fFrequency == RecurrenceRule::Minutely) {
				for (int second : fSeconds) {
					candidates.push_back(DateTime(wall.date, wall.hour,
						wall.minute, second));
				}
			} else
				candidates.push_back(wall);
		}
		if (candidates.empty())
			continue;
		if (!fRule.bySetPos.empty())
			ApplySetPos(candidates);
		for (const DateTime& candidate : candidates) {
			int64_t seconds = candidate.AsUtcSeconds();
			if (seconds <= fStartSeconds)
				continue;
			if (PastUntil(candidate, seconds))
				return;
			if (!visit(candidate))
				return;
		}
	}
}


// One instance of a series before overrides are applied.
struct Instance {
	Instant start = 0;
	Instant end = 0;
	Date startDate;
	Date endDate;
};


bool
InstanceLess(const Instance& a, const Instance& b)
{
	return a.start < b.start;
}


// The series' instances (DTSTART, rule instances and RDATEs, minus EXDATEs)
// that overlap the range, in order.
std::vector<Instance>
CollectInstances(const Event& master, Instant rangeStart, Instant rangeEnd,
	size_t maxCount)
{
	std::vector<Instance> result;
	if (maxCount == 0 || rangeEnd <= rangeStart)
		return result;

	const bool allDay = master.allDay;
	const std::string zone = allDay ? std::string() : master.tzid;
	int64_t durationSeconds = std::max<int64_t>(0, master.end - master.start);
	int64_t durationDays = master.endDate.DayNumber() - master.startDate.DayNumber();
	if (durationDays < 1)
		durationDays = 1;
	int64_t span = allDay ? durationDays * 86400 : durationSeconds;

	std::set<Instant> excludedInstants(master.exdates.begin(),
		master.exdates.end());
	std::set<int64_t> excludedDays;
	for (const Date& date : master.exdateDays)
		excludedDays.insert(date.DayNumber());
	if (allDay) {
		for (Instant instant : master.exdates)
			excludedDays.insert(LocalDate(instant).DayNumber());
	}

	DateTime startWall = allDay ? DateTime(master.startDate)
		: ToWall(master.start, zone);

	enum Verdict { kSkip, kAdded, kStop };
	auto consider = [&](const DateTime& wall, bool isStart) -> Verdict {
		int64_t wallSeconds = wall.AsUtcSeconds();
		if (wallSeconds + span + kSlack < rangeStart)
			return kSkip;
		if (wallSeconds - kSlack >= rangeEnd)
			return kStop;
		Instance instance;
		if (allDay) {
			instance.startDate = wall.date;
			instance.endDate = wall.date.AddDays(durationDays);
			instance.start = StartOfDay(instance.startDate);
			instance.end = StartOfDay(instance.endDate);
		} else {
			instance.start = isStart ? master.start : ToInstant(wall, zone);
			instance.end = instance.start + durationSeconds;
			instance.startDate = wall.date;
			instance.endDate = wall.date;
		}
		if (!Overlaps(instance.start, instance.end, rangeStart, rangeEnd))
			return instance.start >= rangeEnd ? kStop : kSkip;
		if (allDay) {
			if (excludedDays.count(wall.date.DayNumber()) != 0)
				return kSkip;
		} else if (excludedInstants.count(instance.start) != 0
			|| excludedDays.count(wall.date.DayNumber()) != 0)
			return kSkip;
		result.push_back(instance);
		return kAdded;
	};

	RecurrenceRule rule;
	bool hasRule = !master.rrule.empty()
		&& RecurrenceRule::Parse(master.rrule, rule);

	// DTSTART is always the first instance.
	if (consider(startWall, true) != kStop && hasRule && result.size() < maxCount
		&& rule.count != 1) {
		int generated = 1;
		Expansion expansion(rule, startWall, allDay, zone);
		int64_t skipTo = rangeStart - span - kSlack;
		int64_t stopAfter = rangeEnd + kSlack;
		expansion.Run(skipTo, stopAfter, [&](const DateTime& wall) {
			if (rule.count > 0 && generated >= rule.count)
				return false;
			generated++;
			if (consider(wall, false) == kStop)
				return false;
			if (result.size() >= maxCount)
				return false;
			if (rule.count > 0 && generated >= rule.count)
				return false;
			return true;
		});
	}

	// RDATEs: extra instances, unless the rule already produced them.
	if (!master.rdates.empty()) {
		std::set<int64_t> present;
		for (const Instance& instance : result)
			present.insert(allDay ? instance.startDate.DayNumber() : instance.start);
		for (Instant rdate : master.rdates) {
			DateTime wall = allDay ? DateTime(LocalDate(rdate)) : ToWall(rdate, zone);
			int64_t key = allDay ? wall.date.DayNumber() : rdate;
			if (present.count(key) != 0)
				continue;
			size_t before = result.size();
			consider(wall, false);
			if (result.size() == before)
				continue;
			if (!allDay) {
				result.back().start = rdate;
				result.back().end = rdate + durationSeconds;
			}
			present.insert(key);
		}
		std::stable_sort(result.begin(), result.end(), InstanceLess);
		if (result.size() > maxCount)
			result.resize(maxCount);
	}
	return result;
}


Occurrence
MakeOccurrence(const Event& event, const Event& master)
{
	Occurrence occurrence;
	occurrence.event = &event;
	occurrence.master = &master;
	occurrence.allDay = event.allDay;
	if (event.allDay) {
		occurrence.startDate = event.startDate;
		occurrence.endDate = event.endDate > event.startDate ? event.endDate
			: event.startDate.AddDays(1);
		occurrence.start = StartOfDay(occurrence.startDate);
		occurrence.end = StartOfDay(occurrence.endDate);
	} else {
		occurrence.start = event.start;
		occurrence.end = std::max(event.start, event.end);
		occurrence.startDate = LocalDate(occurrence.start);
		occurrence.endDate = occurrence.LastDay().AddDays(1);
	}
	return occurrence;
}


bool
OccurrenceLess(const Occurrence& a, const Occurrence& b)
{
	return a.start < b.start;
}

}	// namespace


// --- RecurrenceRule ------------------------------------------------------------

bool
RecurrenceRule::Parse(const std::string& input, RecurrenceRule& out)
{
	std::string text = Trim(input);
	if (text.size() >= 6 && Upper(text.substr(0, 6)) == "RRULE:")
		text = text.substr(6);

	RecurrenceRule rule;
	for (const std::string& part : Split(text, ';')) {
		size_t equals = part.find('=');
		if (equals == std::string::npos)
			continue;
		std::string key = Upper(Trim(part.substr(0, equals)));
		std::string value = Upper(Trim(part.substr(equals + 1)));
		if (key == "FREQ") {
			static const char* kNames[] = {"", "SECONDLY", "MINUTELY", "HOURLY",
				"DAILY", "WEEKLY", "MONTHLY", "YEARLY"};
			rule.frequency = None;
			for (int i = 1; i < 8; i++) {
				if (value == kNames[i])
					rule.frequency = (Frequency)i;
			}
			if (rule.frequency == None)
				return false;
		} else if (key == "INTERVAL") {
			int interval;
			if (ParseInt(value, interval) && interval >= 1)
				rule.interval = interval;
		} else if (key == "COUNT") {
			int count;
			if (ParseInt(value, count) && count >= 1)
				rule.count = count;
		} else if (key == "UNTIL") {
			DateTime wall;
			bool utc, dateOnly;
			if (ParseBasic(value, wall, utc, dateOnly)) {
				rule.hasUntil = true;
				rule.untilIsDate = dateOnly;
				rule.untilFloating = !dateOnly && !utc;
				rule.untilDate = wall.date;
				rule.until = dateOnly ? 0 : wall.AsUtcSeconds();
			}
		} else if (key == "BYDAY") {
			rule.byDay.clear();
			for (const std::string& rawItem : Split(value, ',')) {
				std::string item = Trim(rawItem);
				if (item.size() < 2)
					continue;
				WeekdayNum day;
				day.weekday = WeekdayFromCode(item.substr(item.size() - 2));
				if (day.weekday < 0)
					continue;
				std::string ordinal = Trim(item.substr(0, item.size() - 2));
				if (!ordinal.empty()) {
					if (ordinal[0] == '+')
						ordinal = ordinal.substr(1);
					if (!ParseInt(ordinal, day.ordinal) || day.ordinal == 0
						|| day.ordinal < -53 || day.ordinal > 53)
						continue;
				}
				rule.byDay.push_back(day);
			}
		} else if (key == "BYMONTHDAY")
			ParseIntList(value, -31, 31, false, rule.byMonthDay);
		else if (key == "BYMONTH")
			ParseIntList(value, 1, 12, false, rule.byMonth);
		else if (key == "BYYEARDAY")
			ParseIntList(value, -366, 366, false, rule.byYearDay);
		else if (key == "BYWEEKNO")
			ParseIntList(value, -53, 53, false, rule.byWeekNo);
		else if (key == "BYSETPOS")
			ParseIntList(value, -366, 366, false, rule.bySetPos);
		else if (key == "BYHOUR")
			ParseIntList(value, 0, 23, true, rule.byHour);
		else if (key == "BYMINUTE")
			ParseIntList(value, 0, 59, true, rule.byMinute);
		else if (key == "BYSECOND")
			ParseIntList(value, 0, 60, true, rule.bySecond);
		else if (key == "WKST") {
			int weekday = WeekdayFromCode(value);
			if (weekday >= 0)
				rule.weekStart = weekday;
		}
		// Anything else (X-NAME, RSCALE, SKIP ...) is ignored.
	}
	if (rule.frequency == None)
		return false;
	out = rule;
	return true;
}


std::string
RecurrenceRule::ToString() const
{
	static const char* kNames[] = {"", "SECONDLY", "MINUTELY", "HOURLY", "DAILY",
		"WEEKLY", "MONTHLY", "YEARLY"};
	if (frequency == None)
		return "";
	std::string result = std::string("FREQ=") + kNames[frequency];
	if (interval > 1)
		result += ";INTERVAL=" + std::to_string(interval);
	if (count > 0)
		result += ";COUNT=" + std::to_string(count);
	if (hasUntil) {
		result += ";UNTIL=";
		if (untilIsDate)
			result += untilDate.ToBasic();
		else if (untilFloating)
			result += FormatBasicWall(DateTime::FromUtcSeconds(until));
		else
			result += FormatBasicUtc(until);
	}
	if (!bySecond.empty())
		result += ";BYSECOND=" + JoinInts(bySecond);
	if (!byMinute.empty())
		result += ";BYMINUTE=" + JoinInts(byMinute);
	if (!byHour.empty())
		result += ";BYHOUR=" + JoinInts(byHour);
	if (!byDay.empty()) {
		result += ";BYDAY=";
		for (size_t i = 0; i < byDay.size(); i++) {
			if (i > 0)
				result += ",";
			if (byDay[i].ordinal != 0)
				result += std::to_string(byDay[i].ordinal);
			result += kWeekdayCodes[((byDay[i].weekday % 7) + 7) % 7];
		}
	}
	if (!byMonthDay.empty())
		result += ";BYMONTHDAY=" + JoinInts(byMonthDay);
	if (!byYearDay.empty())
		result += ";BYYEARDAY=" + JoinInts(byYearDay);
	if (!byWeekNo.empty())
		result += ";BYWEEKNO=" + JoinInts(byWeekNo);
	if (!byMonth.empty())
		result += ";BYMONTH=" + JoinInts(byMonth);
	if (!bySetPos.empty())
		result += ";BYSETPOS=" + JoinInts(bySetPos);
	if (weekStart != 1)
		result += std::string(";WKST=") + kWeekdayCodes[((weekStart % 7) + 7) % 7];
	return result;
}


// --- Expansion -------------------------------------------------------------------

std::vector<Occurrence>
ExpandEvent(const Event& master, const std::vector<const Event*>& overrides,
	Instant rangeStart, Instant rangeEnd, size_t maxCount)
{
	std::vector<Occurrence> result;
	if (!master.IsRecurring()) {
		Occurrence occurrence = MakeOccurrence(master, master);
		occurrence.originalStart = occurrence.start;
		occurrence.originalDate = occurrence.startDate;
		if (maxCount > 0
			&& Overlaps(occurrence.start, occurrence.end, rangeStart, rangeEnd))
			result.push_back(occurrence);
		return result;
	}

	// Overrides by the instance they replace.
	std::map<int64_t, const Event*> replaced;
	for (const Event* override : overrides) {
		if (override == nullptr || override == &master
			|| override->masterId != master.id)
			continue;
		int64_t key = master.allDay ? override->recurrenceDate.DayNumber()
			: override->recurrenceId;
		replaced[key] = override;
	}

	std::vector<Instance> instances = CollectInstances(master, rangeStart,
		rangeEnd, maxCount);
	for (const Instance& instance : instances) {
		int64_t key = master.allDay ? instance.startDate.DayNumber()
			: instance.start;
		if (replaced.count(key) != 0)
			continue;
		Occurrence occurrence;
		occurrence.event = &master;
		occurrence.master = &master;
		occurrence.allDay = master.allDay;
		occurrence.start = instance.start;
		occurrence.end = instance.end;
		if (master.allDay) {
			occurrence.startDate = instance.startDate;
			occurrence.endDate = instance.endDate;
			occurrence.originalDate = instance.startDate;
			occurrence.originalStart = instance.start;
		} else {
			occurrence.startDate = LocalDate(instance.start);
			occurrence.endDate = occurrence.LastDay().AddDays(1);
			occurrence.originalStart = instance.start;
			occurrence.originalDate = occurrence.startDate;
		}
		result.push_back(occurrence);
	}

	// Overrides show at their own (possibly moved) time.
	std::set<Instant> excluded(master.exdates.begin(), master.exdates.end());
	std::set<int64_t> excludedDays;
	for (const Date& date : master.exdateDays)
		excludedDays.insert(date.DayNumber());
	for (const auto& entry : replaced) {
		const Event* override = entry.second;
		if (override->cancelled)
			continue;
		if (master.allDay ? excludedDays.count(entry.first) != 0
				: excluded.count(entry.first) != 0)
			continue;
		Occurrence occurrence = MakeOccurrence(*override, master);
		if (master.allDay) {
			occurrence.originalDate = override->recurrenceDate;
			occurrence.originalStart = StartOfDay(override->recurrenceDate);
		} else {
			occurrence.originalStart = override->recurrenceId;
			occurrence.originalDate = ToWall(override->recurrenceId,
				master.tzid).date;
		}
		if (Overlaps(occurrence.start, occurrence.end, rangeStart, rangeEnd))
			result.push_back(occurrence);
	}

	std::stable_sort(result.begin(), result.end(), OccurrenceLess);
	if (result.size() > maxCount)
		result.resize(maxCount);
	return result;
}


std::vector<Instant>
RuleInstances(const Event& master, Instant rangeStart, Instant rangeEnd,
	size_t maxCount)
{
	std::vector<Instant> result;
	if (!master.IsRecurring()) {
		Occurrence occurrence = MakeOccurrence(master, master);
		if (maxCount > 0
			&& Overlaps(occurrence.start, occurrence.end, rangeStart, rangeEnd))
			result.push_back(occurrence.start);
		return result;
	}
	for (const Instance& instance : CollectInstances(master, rangeStart,
			rangeEnd, maxCount))
		result.push_back(instance.start);
	return result;
}


std::string
TruncateRule(const std::string& rrule, const Event& master, Instant before)
{
	RecurrenceRule rule;
	if (!RecurrenceRule::Parse(rrule, rule))
		return rrule;
	rule.count = 0;
	rule.hasUntil = true;
	rule.untilFloating = false;
	if (master.allDay) {
		rule.untilIsDate = true;
		rule.untilDate = LocalDate(before - 1);
		rule.until = 0;
	} else {
		rule.untilIsDate = false;
		rule.until = before - 1;
	}
	return rule.ToString();
}

}	// namespace aircal
