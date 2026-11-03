#include "Model.h"
#include "Recurrence.h"

#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <random>
#include <unistd.h>

namespace aircal {

const char*
AccountTypeName(AccountType type)
{
	switch (type) {
		case AccountType::Local: return "local";
		case AccountType::Google: return "google";
		case AccountType::CalDAV: return "caldav";
		case AccountType::Todoist: return "todoist";
	}
	return "local";
}


AccountType
AccountTypeFromName(const std::string& name)
{
	if (name == "google")
		return AccountType::Google;
	if (name == "caldav")
		return AccountType::CalDAV;
	if (name == "todoist")
		return AccountType::Todoist;
	return AccountType::Local;
}


const char*
PendingName(Pending pending)
{
	switch (pending) {
		case Pending::None: return "";
		case Pending::Create: return "create";
		case Pending::Update: return "update";
		case Pending::Delete: return "delete";
	}
	return "";
}


Pending
PendingFromName(const std::string& name)
{
	if (name == "create")
		return Pending::Create;
	if (name == "update")
		return Pending::Update;
	if (name == "delete")
		return Pending::Delete;
	return Pending::None;
}


Date
Occurrence::FirstDay() const
{
	return allDay ? startDate : LocalDate(start);
}


Date
Occurrence::LastDay() const
{
	if (allDay)
		return endDate.DayNumber() > startDate.DayNumber()
			? endDate.AddDays(-1) : startDate;
	if (end <= start)
		return LocalDate(start);
	// An event ending at midnight does not cover the next day.
	return LocalDate(end - 1);
}


static const char* kDayCodes[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};


RepeatPreset
PresetFromRule(const std::string& text)
{
	if (text.empty())
		return RepeatPreset::None;
	RecurrenceRule rule;
	if (!RecurrenceRule::Parse(text, rule))
		return RepeatPreset::Custom;
	bool simple = rule.count == 0 && !rule.hasUntil && rule.byMonthDay.empty()
		&& rule.byMonth.empty() && rule.bySetPos.empty() && rule.byYearDay.empty()
		&& rule.byWeekNo.empty() && rule.byHour.empty();
	if (!simple)
		return RepeatPreset::Custom;
	switch (rule.frequency) {
		case RecurrenceRule::Daily:
			return rule.interval == 1 && rule.byDay.empty()
				? RepeatPreset::Daily : RepeatPreset::Custom;
		case RecurrenceRule::Weekly:
			if (rule.byDay.size() == 5 && rule.interval == 1) {
				bool weekdays = true;
				for (auto& day : rule.byDay) {
					if (day.weekday == 0 || day.weekday == 6 || day.ordinal != 0)
						weekdays = false;
				}
				return weekdays ? RepeatPreset::Weekdays : RepeatPreset::Custom;
			}
			if (rule.byDay.size() > 1)
				return RepeatPreset::Custom;
			if (rule.interval == 1)
				return RepeatPreset::Weekly;
			if (rule.interval == 2)
				return RepeatPreset::Biweekly;
			return RepeatPreset::Custom;
		case RecurrenceRule::Monthly:
			return rule.interval == 1 && rule.byDay.empty()
				? RepeatPreset::Monthly : RepeatPreset::Custom;
		case RecurrenceRule::Yearly:
			return rule.interval == 1 && rule.byDay.empty()
				? RepeatPreset::Yearly : RepeatPreset::Custom;
		default:
			return RepeatPreset::Custom;
	}
}


std::string
RuleFromPreset(RepeatPreset preset, const Date& start)
{
	switch (preset) {
		case RepeatPreset::Daily:
			return "FREQ=DAILY";
		case RepeatPreset::Weekdays:
			return "FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR";
		case RepeatPreset::Weekly:
			return std::string("FREQ=WEEKLY;BYDAY=") + kDayCodes[start.Weekday()];
		case RepeatPreset::Biweekly:
			return std::string("FREQ=WEEKLY;INTERVAL=2;BYDAY=")
				+ kDayCodes[start.Weekday()];
		case RepeatPreset::Monthly:
			return "FREQ=MONTHLY";
		case RepeatPreset::Yearly:
			return "FREQ=YEARLY";
		default:
			return "";
	}
}


std::string
DescribeRule(const std::string& text)
{
	if (text.empty())
		return "Never";
	RecurrenceRule rule;
	if (!RecurrenceRule::Parse(text, rule))
		return "Custom";
	static const char* kDayNames[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
		"Thursday", "Friday", "Saturday"};
	static const char* kOrdinals[] = {"last", "", "first", "second", "third",
		"fourth", "fifth"};
	static const char* kMonths[] = {"January", "February", "March", "April",
		"May", "June", "July", "August", "September", "October", "November",
		"December"};

	std::string unit;
	switch (rule.frequency) {
		case RecurrenceRule::Daily: unit = "day"; break;
		case RecurrenceRule::Weekly: unit = "week"; break;
		case RecurrenceRule::Monthly: unit = "month"; break;
		case RecurrenceRule::Yearly: unit = "year"; break;
		case RecurrenceRule::Hourly: unit = "hour"; break;
		default: unit = "time"; break;
	}
	std::string result;
	if (rule.interval > 1)
		result = "Every " + std::to_string(rule.interval) + " " + unit + "s";
	else
		result = "Every " + unit;

	if (rule.frequency == RecurrenceRule::Weekly && rule.byDay.size() == 5
		&& PresetFromRule(text) == RepeatPreset::Weekdays)
		result = "Every weekday";
	else if (!rule.byDay.empty()) {
		result += " on ";
		for (size_t i = 0; i < rule.byDay.size(); i++) {
			if (i > 0)
				result += i + 1 == rule.byDay.size() ? " and " : ", ";
			int ordinal = rule.byDay[i].ordinal;
			if (ordinal != 0 && ordinal >= -1 && ordinal <= 5) {
				result += kOrdinals[ordinal + 1];
				result += " ";
			}
			result += kDayNames[rule.byDay[i].weekday % 7];
		}
	}
	if (!rule.byMonthDay.empty()) {
		result += " on day";
		for (size_t i = 0; i < rule.byMonthDay.size(); i++)
			result += (i ? ", " : " ") + std::to_string(rule.byMonthDay[i]);
	}
	if (!rule.byMonth.empty() && rule.frequency == RecurrenceRule::Yearly) {
		result += " in ";
		for (size_t i = 0; i < rule.byMonth.size(); i++) {
			int month = rule.byMonth[i];
			if (month >= 1 && month <= 12)
				result += (i ? ", " : "") + std::string(kMonths[month - 1]);
		}
	}
	if (rule.count > 0)
		result += ", " + std::to_string(rule.count) + " times";
	else if (rule.hasUntil) {
		Date until = rule.untilIsDate ? rule.untilDate : LocalDate(rule.until);
		result += ", until " + until.ToIso();
	}
	return result;
}


std::string
NewId()
{
	static thread_local std::mt19937_64 generator([] {
		uint64_t seed = 0;
		int fd = open("/dev/urandom", O_RDONLY);
		if (fd >= 0) {
			if (read(fd, &seed, sizeof(seed)) != sizeof(seed))
				seed = 0;
			close(fd);
		}
		std::random_device device;
		return seed ^ ((uint64_t)device() << 32) ^ (uint64_t)time(nullptr);
	}());
	char buffer[40];
	snprintf(buffer, sizeof(buffer), "%016llx%016llx",
		(unsigned long long)generator(), (unsigned long long)generator());
	return buffer;
}

}	// namespace aircal
