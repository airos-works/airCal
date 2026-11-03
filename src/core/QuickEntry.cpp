// Natural-language quick entry, Fantastical style (English only).
//
// The text is split into tokens that remember where they came from in the
// original string. The tokens are then scanned left to right; at each one the
// phrase matchers (task markers, calendar hint, alert, recurrence, duration,
// relative offsets, dates, times, connector + date/time, location) are tried
// in order and the first that matches consumes its tokens. Whatever is left,
// in its original order and casing, is the title.
//
// Matchers never change the parser state; Apply() merges a matched phrase.
// That lets the location matcher probe "does a phrase start here?" to find
// where "at Cafe Rio ..." ends.

#include "QuickEntry.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace aircal {

namespace {

const char* const kSeparator = " \xC2\xB7 ";	// " · "
const char* const kDash = " \xE2\x80\x93 ";		// " – "

const char* const kDayCodes[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
const char* const kDayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const kMonthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// Limits that keep garbage from overflowing anything.
const int kMaxMinutes = 1000000;	// about two years
const int kMaxDays = 36600;
const int kMaxInterval = 999;


bool
IsSpaceChar(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'
		|| c == '\v';
}


bool
IsDigitChar(char c)
{
	return c >= '0' && c <= '9';
}


bool
IsAlphaChar(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}


bool
IsUpperChar(char c)
{
	return c >= 'A' && c <= 'Z';
}


char
LowerChar(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}


bool
IsTrailingPunctuation(char c)
{
	return c == ',' || c == ';' || c == ':' || c == '?' || c == '!' || c == ')'
		|| c == '"' || c == '\'' || c == '.';
}


bool
AllDigits(const std::string& s)
{
	if (s.empty())
		return false;
	for (char c : s) {
		if (!IsDigitChar(c))
			return false;
	}
	return true;
}


// "12" -> 12; at most six digits so nothing overflows.
bool
ParseSmallInt(const std::string& s, int& out)
{
	if (!AllDigits(s) || s.size() > 6)
		return false;
	out = atoi(s.c_str());
	return true;
}


std::string
TrimPunctuation(const std::string& text, const char* extra = "")
{
	size_t begin = 0;
	size_t end = text.size();
	auto strip = [&](char c) {
		if (IsSpaceChar(c) || c == ',' || c == ';' || c == ':' || c == '-')
			return true;
		for (const char* e = extra; *e != '\0'; e++) {
			if (*e == c)
				return true;
		}
		return false;
	};
	while (true) {
		if (begin < end && strip(text[begin]))
			begin++;
		else if (end - begin >= 3 && (unsigned char)text[begin] == 0xE2
			&& (unsigned char)text[begin + 1] == 0x80
			&& ((unsigned char)text[begin + 2] == 0x93
				|| (unsigned char)text[begin + 2] == 0x94))
			begin += 3;
		else
			break;
	}
	while (true) {
		if (end > begin && strip(text[end - 1]))
			end--;
		else if (end - begin >= 3 && (unsigned char)text[end - 3] == 0xE2
			&& (unsigned char)text[end - 2] == 0x80
			&& ((unsigned char)text[end - 1] == 0x93
				|| (unsigned char)text[end - 1] == 0x94))
			end -= 3;
		else
			break;
	}
	return text.substr(begin, end - begin);
}


// --- Tokens ------------------------------------------------------------------

struct Token {
	size_t begin = 0;	// byte offsets into the original text
	size_t end = 0;
	std::string orig;	// as typed
	std::string norm;	// lower case, periods and edge punctuation removed
};


std::string
Normalize(const std::string& word)
{
	std::string s;
	s.reserve(word.size());
	for (size_t i = 0; i < word.size(); i++) {
		char c = word[i];
		if (c == '.') {
			// Keep decimal points ("1.5") and dotted times ("3.30pm").
			if (i > 0 && i + 1 < word.size() && IsDigitChar(word[i - 1])
				&& IsDigitChar(word[i + 1]))
				s += '.';
			continue;
		}
		s += LowerChar(c);
	}
	size_t end = s.size();
	while (end > 0 && IsTrailingPunctuation(s[end - 1]))
		end--;
	size_t begin = 0;
	while (begin < end && (s[begin] == '(' || s[begin] == '"' || s[begin] == '\''))
		begin++;
	if (end > begin)
		return s.substr(begin, end - begin);
	// Only punctuation ("!!!", ","): keep it as it is.
	return s;
}


void
PushToken(std::vector<Token>& tokens, const std::string& text, size_t begin,
	size_t end, const char* norm = nullptr)
{
	if (end <= begin)
		return;
	Token token;
	token.begin = begin;
	token.end = end;
	token.orig = text.substr(begin, end - begin);
	token.norm = norm != nullptr ? std::string(norm) : Normalize(token.orig);
	bool punctuation = true;
	for (char c : token.norm) {
		if (c != ',' && c != ';' && c != ':' && c != '.')
			punctuation = false;
	}
	if (punctuation)
		return;
	tokens.push_back(token);
}


// dddd-d[d]-d[d] with optional trailing punctuation.
bool
IsIsoWord(const std::string& text, size_t begin, size_t end)
{
	size_t i = begin;
	int digits = 0;
	while (i < end && IsDigitChar(text[i]) && digits < 5) {
		i++;
		digits++;
	}
	if (digits != 4 || i >= end || text[i] != '-')
		return false;
	for (int part = 0; part < 2; part++) {
		i++;
		digits = 0;
		while (i < end && IsDigitChar(text[i]) && digits < 3) {
			i++;
			digits++;
		}
		if (digits < 1 || digits > 2)
			return false;
		if (part == 0 && (i >= end || text[i] != '-'))
			return false;
	}
	for (; i < end; i++) {
		if (!IsTrailingPunctuation(text[i]))
			return false;
	}
	return true;
}


bool
EndsWithMeridiem(const std::string& text, size_t start, size_t at)
{
	if (at < start + 2)
		return false;
	char a = LowerChar(text[at - 2]);
	char m = LowerChar(text[at - 1]);
	return (a == 'a' || a == 'p') && m == 'm';
}


// Splits a whitespace-delimited word: at commas ("mon,wed"), at dashes next
// to digits or am/pm ("3-4pm", "8pm-11pm", "Nov 12-14") and at en/em dashes.
void
SplitWord(const std::string& text, size_t begin, size_t end,
	std::vector<Token>& tokens)
{
	if (IsIsoWord(text, begin, end)) {
		PushToken(tokens, text, begin, end);
		return;
	}
	size_t start = begin;
	size_t i = begin;
	while (i < end) {
		unsigned char c = (unsigned char)text[i];
		if (c == 0xE2 && i + 2 < end && (unsigned char)text[i + 1] == 0x80
			&& ((unsigned char)text[i + 2] == 0x93
				|| (unsigned char)text[i + 2] == 0x94)) {
			PushToken(tokens, text, start, i);
			PushToken(tokens, text, i, i + 3, "-");
			i += 3;
			start = i;
			continue;
		}
		if (c == '-' && end - begin > 1) {
			// Not inside "555-1234": ranges have at most two digits a side.
			size_t before = 0;
			while (i - before > begin && IsDigitChar(text[i - before - 1]))
				before++;
			size_t after = 0;
			while (i + after + 1 < end && IsDigitChar(text[i + after + 1]))
				after++;
			bool split = (before > 0 || after > 0
					|| EndsWithMeridiem(text, start, i))
				&& before <= 2 && after <= 2;
			if (split) {
				PushToken(tokens, text, start, i);
				PushToken(tokens, text, i, i + 1);
				start = i + 1;
			}
		} else if (c == ',') {
			bool number = i > begin && IsDigitChar(text[i - 1]) && i + 1 < end
				&& IsDigitChar(text[i + 1]);
			if (!number) {
				PushToken(tokens, text, start, i + 1);
				start = i + 1;
			}
		}
		i++;
	}
	PushToken(tokens, text, start, end);
}


std::vector<Token>
Tokenize(const std::string& text)
{
	std::vector<Token> tokens;
	size_t i = 0;
	size_t n = text.size();
	while (i < n) {
		while (i < n && IsSpaceChar(text[i]))
			i++;
		if (i >= n)
			break;
		size_t begin = i;
		while (i < n && !IsSpaceChar(text[i]))
			i++;
		SplitWord(text, begin, i, tokens);
	}
	return tokens;
}


// --- Words ---------------------------------------------------------------------

int
MonthFromWord(const std::string& s)
{
	static const struct { const char* word; int month; } kWords[] = {
		{"jan", 1}, {"january", 1}, {"feb", 2}, {"february", 2}, {"mar", 3},
		{"march", 3}, {"apr", 4}, {"april", 4}, {"may", 5}, {"jun", 6},
		{"june", 6}, {"jul", 7}, {"july", 7}, {"aug", 8}, {"august", 8},
		{"sep", 9}, {"sept", 9}, {"september", 9}, {"oct", 10}, {"october", 10},
		{"nov", 11}, {"november", 11}, {"dec", 12}, {"december", 12}
	};
	for (auto& entry : kWords) {
		if (s == entry.word)
			return entry.month;
	}
	return 0;
}


// 0 = Sunday. plural: "mondays" (a recurrence). risky: "sun", "sat" are
// ordinary words, only dates after "on", "this", "next", "every"...
int
WeekdayFromWord(const std::string& s, bool& plural, bool& risky)
{
	static const struct {
		const char* word;
		int day;
		bool plural;
		bool risky;
	} kWords[] = {
		{"sun", 0, false, true}, {"sunday", 0, false, false},
		{"sundays", 0, true, false},
		{"mon", 1, false, false}, {"monday", 1, false, false},
		{"mondays", 1, true, false},
		{"tue", 2, false, false}, {"tues", 2, false, false},
		{"tuesday", 2, false, false}, {"tuesdays", 2, true, false},
		{"wed", 3, false, false}, {"weds", 3, false, false},
		{"wednesday", 3, false, false}, {"wednesdays", 3, true, false},
		{"thu", 4, false, false}, {"thur", 4, false, false},
		{"thurs", 4, false, false}, {"thursday", 4, false, false},
		{"thursdays", 4, true, false},
		{"fri", 5, false, false}, {"friday", 5, false, false},
		{"fridays", 5, true, false},
		{"sat", 6, false, true}, {"saturday", 6, false, false},
		{"saturdays", 6, true, false}
	};
	plural = false;
	risky = false;
	for (auto& entry : kWords) {
		if (s == entry.word) {
			plural = entry.plural;
			risky = entry.risky;
			return entry.day;
		}
	}
	return -1;
}


int
NumberWord(const std::string& s)
{
	static const struct { const char* word; int value; } kWords[] = {
		{"a", 1}, {"an", 1}, {"one", 1}, {"two", 2}, {"three", 3}, {"four", 4},
		{"five", 5}, {"six", 6}, {"seven", 7}, {"eight", 8}, {"nine", 9},
		{"ten", 10}, {"eleven", 11}, {"twelve", 12}, {"fifteen", 15},
		{"twenty", 20}, {"thirty", 30}, {"forty", 40}, {"forty-five", 45},
		{"fortyfive", 45}, {"sixty", 60}, {"ninety", 90}
	};
	for (auto& entry : kWords) {
		if (s == entry.word)
			return entry.value;
	}
	return 0;
}


// "12th", "1st", "22nd", "3rd" -> day.
bool
ParseOrdinal(const std::string& s, int& day)
{
	if (s.size() < 3 || s.size() > 4)
		return false;
	std::string suffix = s.substr(s.size() - 2);
	if (suffix != "st" && suffix != "nd" && suffix != "rd" && suffix != "th")
		return false;
	int value;
	if (!ParseSmallInt(s.substr(0, s.size() - 2), value))
		return false;
	if (value < 1 || value > 31)
		return false;
	day = value;
	return true;
}


// 12 or 12th.
bool
ParseDayNumber(const std::string& s, int& day)
{
	int value;
	if (ParseSmallInt(s, value) && s.size() <= 2 && value >= 1 && value <= 31) {
		day = value;
		return true;
	}
	return ParseOrdinal(s, day);
}


bool
ParseYear(const std::string& s, int& year)
{
	int value;
	if (s.size() != 4 || !ParseSmallInt(s, value) || value < 1900 || value > 2200)
		return false;
	year = value;
	return true;
}


enum Unit { kNoUnit, kMinuteUnit, kHourUnit, kDayUnit, kWeekUnit, kMonthUnit,
	kYearUnit };


Unit
UnitFromWord(const std::string& s)
{
	if (s == "m" || s == "min" || s == "mins" || s == "minute" || s == "minutes")
		return kMinuteUnit;
	if (s == "h" || s == "hr" || s == "hrs" || s == "hour" || s == "hours")
		return kHourUnit;
	if (s == "d" || s == "day" || s == "days")
		return kDayUnit;
	if (s == "w" || s == "wk" || s == "wks" || s == "week" || s == "weeks")
		return kWeekUnit;
	if (s == "mo" || s == "mos" || s == "month" || s == "months")
		return kMonthUnit;
	if (s == "y" || s == "yr" || s == "yrs" || s == "year" || s == "years")
		return kYearUnit;
	return kNoUnit;
}


int
UnitMinutes(Unit unit)
{
	switch (unit) {
		case kMinuteUnit:
			return 1;
		case kHourUnit:
			return 60;
		case kDayUnit:
			return 1440;
		case kWeekUnit:
			return 10080;
		default:
			return 0;
	}
}


// "2h", "30m", "1h30", "1h30m", "1.5h", "90min" -> minutes.
bool
ParseAttachedDuration(const std::string& s, double& minutes, bool& inDays)
{
	size_t i = 0;
	double total = 0;
	bool any = false;
	bool allDays = true;
	Unit last = kNoUnit;
	while (i < s.size()) {
		size_t numberStart = i;
		while (i < s.size() && (IsDigitChar(s[i]) || s[i] == '.'))
			i++;
		if (i == numberStart || i - numberStart > 6)
			return false;
		double value = atof(s.substr(numberStart, i - numberStart).c_str());
		size_t unitStart = i;
		while (i < s.size() && IsAlphaChar(s[i]))
			i++;
		Unit unit;
		if (i == unitStart) {
			// "1h30": bare minutes after hours.
			if (last != kHourUnit || i != s.size())
				return false;
			unit = kMinuteUnit;
		} else
			unit = UnitFromWord(s.substr(unitStart, i - unitStart));
		if (UnitMinutes(unit) == 0)
			return false;
		if (unit != kDayUnit && unit != kWeekUnit)
			allDays = false;
		total += value * UnitMinutes(unit);
		last = unit;
		any = true;
	}
	if (!any || total <= 0 || total > kMaxMinutes)
		return false;
	minutes = total;
	inDays = allDays;
	return true;
}


bool
IsAlertWord(const std::string& s)
{
	return s == "alert" || s == "alarm" || s == "reminder" || s == "notification"
		|| s == "notify" || s == "alerts" || s == "alarms";
}


bool
IsBeforeWord(const std::string& s)
{
	return s == "before" || s == "prior" || s == "ahead" || s == "early"
		|| s == "earlier" || s == "beforehand";
}


bool
IsRangeSeparator(const std::string& s)
{
	return s == "-" || s == "to" || s == "till" || s == "til" || s == "until"
		|| s == "thru" || s == "through";
}


bool
IsTomorrowWord(const std::string& s)
{
	return s == "tomorrow" || s == "tmrw" || s == "tmr" || s == "tmrrw"
		|| s == "tomorow" || s == "tommorow" || s == "tommorrow";
}


int
PeriodHour(const std::string& s)
{
	if (s == "morning")
		return 9;
	if (s == "afternoon")
		return 14;
	if (s == "evening")
		return 18;
	if (s == "night")
		return 20;
	return -1;
}


// --- Phrases -------------------------------------------------------------------

struct TimeVal {
	int raw = 0;		// hour as typed
	int minute = 0;
	char meridiem = 0;	// 'a', 'p' or 0
	bool strong = false;	// meridiem, colon, o'clock or a word (noon)
	bool fixed = false;		// 24-hour or noon/midnight: never guessed
};


// The hour a time means, with an explicit or carried meridiem. Without one,
// Fantastical's business-hours guess: 1-7 are afternoon, 8-12 as typed.
int
ResolveHour(const TimeVal& t, char meridiem)
{
	if (t.fixed)
		return t.raw;
	if (meridiem == 'a')
		return t.raw % 12;
	if (meridiem == 'p')
		return t.raw % 12 + 12;
	if (t.raw >= 1 && t.raw <= 7)
		return t.raw + 12;
	return t.raw;
}


struct DateVal {
	enum Kind { kNamed, kWeekday, kMonthDay, kDayOnly, kNumeric };
	Kind kind = kNamed;
	Date date;
	bool explicitYear = false;
	int period = -1;
};


struct Phrase {
	enum Kind { kNone, kDate, kTime, kPeriod, kRelative, kRecurrence, kCount,
		kUntil, kDuration, kAlert, kPriority, kLabel, kHint, kLocation };
	Kind kind = kNone;
	size_t begin = 0;	// first token, including a connector ("at", "due")
	size_t core = 0;	// first token of the phrase proper
	size_t end = 0;		// one past the last token

	// kDate
	Date date;
	bool weak = false;			// a bare weekday: an explicit date beats it
	bool hasEndDate = false;
	Date endDate;
	bool viaUntil = false;		// "Nov 12 until Dec 20"
	int period = -1;			// "tonight", "this morning"; kPeriod

	// kTime
	int hour = 0;
	int minute = 0;
	bool guessed = false;		// no am/pm typed: a period word may move it
	bool hasEndTime = false;
	int endHour = 0;
	int endMinute = 0;

	// kRelative: seconds from now
	int64_t seconds = 0;

	// kRecurrence
	std::string frequency;
	int interval = 1;
	std::vector<int> byDay;
	bool hasDate = false;		// "every Nov 12", "every 15th"

	int count = 0;				// kCount
	int minutes = 0;			// kDuration, kAlert
	bool inDays = false;		// kDuration in whole days/weeks
	int priority = 0;
	std::string text;			// label, hint, location
};


class Parser {
public:
							Parser(const std::string& text, Instant now,
								int defaultMinutes, int weekStart);

			QuickEntryResult Run();

private:
			const std::string& N(size_t i) const;
			bool			Is(size_t i, const char* word) const
								{ return N(i) == word; }

			// Pieces
			bool			ParseNumber(size_t i, double& value,
								size_t& next) const;
			bool			ParseDuration(size_t i, int& minutes, bool& inDays,
								size_t& next) const;
			bool			ParseTime(size_t i, bool weakAllowed, TimeVal& t,
								size_t& next) const;
			bool			ParseDate(size_t i, bool qualified, DateVal& d,
								size_t& next) const;
			bool			ParseWeekdayList(size_t i, bool requirePlural,
								std::vector<int>& days, size_t& next) const;
			bool			ResolveMonthDay(int month, int day, int year,
								Date& out) const;
			bool			ResolveDayOfMonth(int day, Date& out) const;
			bool			BareOrdinalAllowed(size_t i) const;
			Date			UpcomingWeekday(int weekday) const;
			Date			NextWeekStart() const;

			// Matchers: fill `p`, never touch the state
			bool			Match(size_t i, Phrase& p, bool probe) const;
			bool			MatchTaskToken(size_t i, Phrase& p) const;
			bool			MatchHint(size_t i, Phrase& p) const;
			bool			MatchAlert(size_t i, Phrase& p) const;
			bool			MatchRecurrence(size_t i, Phrase& p) const;
			bool			MatchCount(size_t i, Phrase& p) const;
			bool			MatchUntil(size_t i, Phrase& p) const;
			bool			MatchDuration(size_t i, Phrase& p) const;
			bool			MatchRelative(size_t i, Phrase& p) const;
			bool			MatchDateRange(size_t i, bool qualified,
								Phrase& p) const;
			bool			MatchTimeRange(size_t i, bool weakAllowed,
								bool allowAnd, Phrase& p) const;
			bool			MatchPeriod(size_t i, bool qualified,
								Phrase& p) const;
			bool			MatchConnector(size_t i, Phrase& p) const;
			bool			MatchLocation(size_t i, Phrase& p) const;
			bool			IsLocationStop(size_t i) const;

			void			DetectTask();
			void			Apply(const Phrase& p);
			void			AddDuePhrase(const Phrase& p);
			std::string		Span(size_t first, size_t last) const;
			std::string		BuildTitle() const;
			std::string		BuildRule(bool timed, int hour, int minute) const;

private:
			std::string		fText;
			std::vector<Token> fTokens;
			std::vector<bool> fConsumed;
			Instant			fNow;
			Date			fToday;
			int				fDefaultMinutes;
			int				fWeekStart;

			bool			fIsTask = false;
			bool			fRemindKeyword = false;

			bool			fHasDate = false;
			bool			fDateWeak = false;
			Date			fDate;
			bool			fHasEndDate = false;
			Date			fEndDate;
			bool			fViaUntil = false;
			int				fPeriod = -1;

			bool			fHasTime = false;
			int				fHour = 0;
			int				fMinute = 0;
			bool			fGuessed = false;
			bool			fHasEndTime = false;
			int				fEndHour = 0;
			int				fEndMinute = 0;

			bool			fRelative = false;
			int64_t			fRelativeSeconds = 0;

			std::string		fFrequency;
			int				fInterval = 1;
			std::vector<int> fByDay;
			int				fCount = 0;
			bool			fHasUntil = false;
			Date			fUntil;

			int				fDuration = -1;
			bool			fDurationInDays = false;
			int				fAlarm = -1;
			int				fPriority = 0;
			std::vector<std::string> fLabels;
			std::string		fHint;
			std::string		fLocation;

			// Token ranges of the date/time/recurrence phrases (dueString).
			struct DueSpan {
				size_t begin;
				size_t core;
				size_t end;
			};
			std::vector<DueSpan> fDue;
};


Parser::Parser(const std::string& text, Instant now, int defaultMinutes,
	int weekStart)
	:
	fText(text),
	fNow(now),
	fToday(LocalDate(now)),
	fDefaultMinutes(defaultMinutes > 0 && defaultMinutes <= kMaxMinutes
		? defaultMinutes : 60),
	fWeekStart(weekStart >= 0 && weekStart <= 6 ? weekStart : 0)
{
	fTokens = Tokenize(fText);
	fConsumed.assign(fTokens.size(), false);
}


const std::string&
Parser::N(size_t i) const
{
	static const std::string kEmpty;
	return i < fTokens.size() ? fTokens[i].norm : kEmpty;
}


// --- Pieces ----------------------------------------------------------------------

// 3, 1.5, "a", "an", "two", ...
bool
Parser::ParseNumber(size_t i, double& value, size_t& next) const
{
	const std::string& s = N(i);
	if (s.empty())
		return false;
	int word = NumberWord(s);
	if (word > 0) {
		value = word;
		next = i + 1;
		return true;
	}
	size_t dots = 0;
	for (char c : s) {
		if (c == '.')
			dots++;
		else if (!IsDigitChar(c))
			return false;
	}
	if (dots > 1 || s.size() > 7 || s[0] == '.' || s.back() == '.')
		return false;
	value = atof(s.c_str());
	next = i + 1;
	return true;
}


// "2 hours", "30 min", "1.5 hours", "an hour", "half an hour",
// "1 hour 30 minutes", "1h30", "3 days".
bool
Parser::ParseDuration(size_t i, int& minutes, bool& inDays, size_t& next) const
{
	double total = 0;
	bool allDays = true;
	bool any = false;
	size_t j = i;
	for (int components = 0; components < 4 && j < fTokens.size(); components++) {
		size_t start = j;
		if (Is(j, "half") || (Is(j, "a") && Is(j + 1, "half"))) {
			size_t k = Is(j, "a") ? j + 2 : j + 1;
			if (Is(k, "an") || Is(k, "a"))
				k++;
			if (UnitFromWord(N(k)) == kHourUnit) {
				total += 30;
				allDays = false;
				j = k + 1;
				any = true;
			}
		}
		if (j == start) {
			double value;
			bool days;
			size_t k;
			if (ParseAttachedDuration(N(j), value, days)) {
				total += value;
				if (!days)
					allDays = false;
				j++;
				any = true;
			} else if (ParseNumber(j, value, k)
				&& UnitMinutes(UnitFromWord(N(k))) > 0) {
				Unit unit = UnitFromWord(N(k));
				total += value * UnitMinutes(unit);
				if (unit != kDayUnit && unit != kWeekUnit)
					allDays = false;
				j = k + 1;
				any = true;
				// "an hour and a half"
				if (unit == kHourUnit && Is(j, "and") && Is(j + 1, "a")
					&& Is(j + 2, "half")) {
					total += 30;
					j += 3;
				}
			}
		}
		if (j == start || total > kMaxMinutes)
			break;
		// "1 hour 30 minutes", "1 hour and 30 minutes"
		size_t k = Is(j, "and") ? j + 1 : j;
		double value;
		size_t after;
		bool days;
		if ((ParseNumber(k, value, after) && UnitMinutes(UnitFromWord(N(after))) > 0
				&& NumberWord(N(k)) == 0)
			|| ParseAttachedDuration(N(k), value, days))
			j = k;
		else
			break;
	}
	if (!any || total < 1 || total > kMaxMinutes)
		return false;
	minutes = (int)(total + 0.5);
	inDays = allDays;
	next = j;
	return true;
}


bool
Parser::ParseTime(size_t i, bool weakAllowed, TimeVal& t, size_t& next) const
{
	if (i >= fTokens.size())
		return false;
	const std::string& s = N(i);
	t = TimeVal();
	if (s == "noon" || s == "midday") {
		t.raw = 12;
		t.fixed = t.strong = true;
		next = i + 1;
		return true;
	}
	if (s == "midnight") {
		t.raw = 0;
		t.fixed = t.strong = true;
		next = i + 1;
		return true;
	}

	size_t k = 0;
	int hour = 0;
	int digits = 0;
	while (k < s.size() && IsDigitChar(s[k]) && digits < 3) {
		hour = hour * 10 + (s[k] - '0');
		k++;
		digits++;
	}
	if (digits == 0 || digits > 2)
		return false;
	int minute = 0;
	bool colon = false;
	bool dotted = false;
	if (k < s.size() && (s[k] == ':' || s[k] == '.')) {
		colon = s[k] == ':';
		dotted = !colon;
		k++;
		if (k + 2 > s.size() || !IsDigitChar(s[k]) || !IsDigitChar(s[k + 1]))
			return false;
		minute = (s[k] - '0') * 10 + (s[k + 1] - '0');
		k += 2;
	}
	std::string suffix = s.substr(k);
	char meridiem = 0;
	bool oclock = false;
	size_t after = i + 1;
	if (suffix == "am" || suffix == "a")
		meridiem = 'a';
	else if (suffix == "pm" || suffix == "p")
		meridiem = 'p';
	else if (!suffix.empty())
		return false;
	if (meridiem == 0) {
		const std::string& following = N(i + 1);
		if (following == "am") {
			meridiem = 'a';
			after = i + 2;
		} else if (following == "pm") {
			meridiem = 'p';
			after = i + 2;
		} else if (following == "o'clock" || following == "oclock") {
			oclock = true;
			after = i + 2;
		}
	}
	if (dotted && meridiem == 0)
		return false;
	if (minute > 59)
		return false;
	if (meridiem != 0 && (hour < 1 || hour > 12))
		return false;
	if (meridiem == 0 && hour > 23)
		return false;
	if (oclock && (hour < 1 || hour > 12))
		return false;

	t.raw = hour;
	t.minute = minute;
	t.meridiem = meridiem;
	t.strong = meridiem != 0 || colon || oclock;
	t.fixed = meridiem == 0 && (hour > 12 || hour == 0);
	if (!t.strong && !weakAllowed)
		return false;
	next = after;
	return true;
}


Date
Parser::UpcomingWeekday(int weekday) const
{
	int diff = (weekday - fToday.Weekday() + 7) % 7;
	return fToday.AddDays(diff);
}


Date
Parser::NextWeekStart() const
{
	int offset = (fToday.Weekday() - fWeekStart + 7) % 7;
	return fToday.AddDays(7 - offset);
}


// A date without a year: this year, or the next one when it has passed.
bool
Parser::ResolveMonthDay(int month, int day, int year, Date& out) const
{
	if (month < 1 || month > 12 || day < 1 || day > 31)
		return false;
	if (year != 0) {
		Date date(year, month, day);
		if (!date.IsValid())
			return false;
		out = date;
		return true;
	}
	for (int k = 0; k <= 8; k++) {
		Date date(fToday.year + k, month, day);
		if (date.IsValid() && date >= fToday) {
			out = date;
			return true;
		}
	}
	return false;
}


// "the 12th": this month, or the next month that has that day.
bool
Parser::ResolveDayOfMonth(int day, Date& out) const
{
	Date first(fToday.year, fToday.month, 1);
	for (int k = 0; k <= 12; k++) {
		Date month = first.AddMonths(k);
		Date date(month.year, month.month, day);
		if (date.IsValid() && date >= fToday) {
			out = date;
			return true;
		}
	}
	return false;
}


// A bare "12th" is only a date where it cannot be an adjective ("1st draft").
bool
Parser::BareOrdinalAllowed(size_t i) const
{
	if (i + 1 >= fTokens.size())
		return true;
	const Token& token = fTokens[i];
	if (!token.orig.empty() && token.orig.back() == ',')
		return true;
	const std::string& s = N(i + 1);
	if (s == "at" || s == "from" || s == "for" || s == "to" || s == "until"
		|| s == "every" || s == "@" || s == "-" || s == "by" || s == "in"
		|| s == "alert" || s == "with")
		return true;
	TimeVal t;
	size_t next;
	return ParseTime(i + 1, false, t, next);
}


bool
Parser::ParseDate(size_t i, bool qualified, DateVal& d, size_t& next) const
{
	if (i >= fTokens.size())
		return false;
	const std::string& s = N(i);
	d = DateVal();

	if (s == "today") {
		d.date = fToday;
		next = i + 1;
		return true;
	}
	if (s == "tonight") {
		d.date = fToday;
		d.period = 20;
		next = i + 1;
		return true;
	}
	if (IsTomorrowWord(s)) {
		d.date = fToday.AddDays(1);
		next = i + 1;
		return true;
	}
	if (s == "yesterday") {
		d.date = fToday.AddDays(-1);
		next = i + 1;
		return true;
	}
	if (s == "day" && Is(i + 1, "after") && IsTomorrowWord(N(i + 2))) {
		d.date = fToday.AddDays(2);
		next = i + 3;
		return true;
	}
	if (s == "the") {
		if (Is(i + 1, "day") && Is(i + 2, "after") && IsTomorrowWord(N(i + 3))) {
			d.date = fToday.AddDays(2);
			next = i + 4;
			return true;
		}
		int day;
		if (ParseOrdinal(N(i + 1), day))
			return ParseDate(i + 1, true, d, next);
		return false;
	}

	bool plural;
	bool risky;
	if (s == "this") {
		const std::string& what = N(i + 1);
		int weekday = WeekdayFromWord(what, plural, risky);
		if (weekday >= 0 && !plural) {
			d.kind = DateVal::kWeekday;
			d.date = UpcomingWeekday(weekday);
			next = i + 2;
			return true;
		}
		if (what == "weekend") {
			int weekday = fToday.Weekday();
			d.date = weekday == 0 ? fToday : UpcomingWeekday(6);
			next = i + 2;
			return true;
		}
		int period = PeriodHour(what);
		if (period >= 0 || what == "tonight") {
			d.date = fToday;
			d.period = period >= 0 ? period : 20;
			next = i + 2;
			return true;
		}
		return false;
	}
	if (s == "next") {
		const std::string& what = N(i + 1);
		int weekday = WeekdayFromWord(what, plural, risky);
		if (weekday >= 0 && !plural) {
			// The weekday in the week after this one.
			d.date = NextWeekStart().AddDays((weekday - fWeekStart + 7) % 7);
			next = i + 2;
			return true;
		}
		if (what == "week") {
			d.date = NextWeekStart();
			next = i + 2;
			return true;
		}
		if (what == "weekend") {
			d.date = NextWeekStart().AddDays((6 - fWeekStart + 7) % 7);
			next = i + 2;
			return true;
		}
		if (what == "month") {
			d.date = Date(fToday.year, fToday.month, 1).AddMonths(1);
			next = i + 2;
			return true;
		}
		if (what == "year") {
			d.date = Date(fToday.year + 1, 1, 1);
			next = i + 2;
			return true;
		}
		return false;
	}

	int weekday = WeekdayFromWord(s, plural, risky);
	if (weekday >= 0) {
		if (plural)
			return false;
		if (risky && !qualified) {
			// "sat" is a date next to a time: "9am-noon sat", "sat 9am".
			TimeVal t;
			size_t after;
			if (!(i > 0 && fConsumed[i - 1]) && !ParseTime(i + 1, false, t, after))
				return false;
		}
		d.kind = DateVal::kWeekday;
		d.date = UpcomingWeekday(weekday);
		next = i + 1;
		return true;
	}

	// Nov 12, November 12th, Nov 12 2027, Nov the 12th
	int month = MonthFromWord(s);
	if (month != 0) {
		size_t j = i + 1;
		if (Is(j, "the"))
			j++;
		int day;
		if (!ParseDayNumber(N(j), day))
			return false;
		j++;
		int year = 0;
		if (ParseYear(N(j), year))
			j++;
		if (!ResolveMonthDay(month, day, year, d.date))
			return false;
		d.kind = DateVal::kMonthDay;
		d.explicitYear = year != 0;
		next = j;
		return true;
	}

	// 12 Nov, 12th of November, 12th
	int day;
	if (ParseDayNumber(s, day)) {
		size_t j = i + 1;
		if (Is(j, "of"))
			j++;
		month = MonthFromWord(N(j));
		if (month != 0) {
			j++;
			int year = 0;
			if (ParseYear(N(j), year))
				j++;
			if (!ResolveMonthDay(month, day, year, d.date))
				return false;
			d.kind = DateVal::kMonthDay;
			d.explicitYear = year != 0;
			next = j;
			return true;
		}
		if (ParseOrdinal(s, day) && (qualified || BareOrdinalAllowed(i))) {
			if (!ResolveDayOfMonth(day, d.date))
				return false;
			d.kind = DateVal::kDayOnly;
			next = i + 1;
			return true;
		}
		return false;
	}

	// 11/12, 11/12/2027, 11/12/27 (US order)
	if (s.find('/') != std::string::npos && IsDigitChar(s[0])) {
		int parts[3] = {0, 0, 0};
		int count = 0;
		size_t start = 0;
		while (count < 3) {
			size_t slash = s.find('/', start);
			std::string part = s.substr(start,
				slash == std::string::npos ? std::string::npos : slash - start);
			if (!ParseSmallInt(part, parts[count]) || part.size() > 4)
				return false;
			count++;
			if (slash == std::string::npos)
				break;
			start = slash + 1;
			if (count == 3)
				return false;
		}
		if (count < 2)
			return false;
		int year = 0;
		if (count == 3) {
			year = parts[2];
			if (year < 100)
				year += 2000;
			if (year < 1900 || year > 2200)
				return false;
		}
		if (!ResolveMonthDay(parts[0], parts[1], year, d.date))
			return false;
		d.kind = DateVal::kNumeric;
		d.explicitYear = year != 0;
		next = i + 1;
		return true;
	}

	// 2026-11-12
	if (s.size() >= 8 && s.size() <= 10 && s[4] == '-' && IsDigitChar(s[0])) {
		int y;
		int m;
		int dd;
		char tail;
		if (sscanf(s.c_str(), "%4d-%2d-%2d%c", &y, &m, &dd, &tail) != 3)
			return false;
		Date date(y, m, dd);
		if (!date.IsValid())
			return false;
		d.date = date;
		d.kind = DateVal::kNumeric;
		d.explicitYear = true;
		next = i + 1;
		return true;
	}
	return false;
}


// "monday", "mon, wed and fri", "mondays and wednesdays".
bool
Parser::ParseWeekdayList(size_t i, bool requirePlural, std::vector<int>& days,
	size_t& next) const
{
	days.clear();
	size_t j = i;
	for (int count = 0; count < 14 && j < fTokens.size(); count++) {
		bool plural;
		bool risky;
		int weekday = WeekdayFromWord(N(j), plural, risky);
		if (weekday < 0 || (requirePlural && days.empty() && !plural))
			break;
		if (std::find(days.begin(), days.end(), weekday) == days.end())
			days.push_back(weekday);
		j++;
		next = j;
		size_t k = j;
		if (Is(k, "and") || Is(k, "&") || Is(k, "+") || Is(k, "or"))
			k++;
		if (WeekdayFromWord(N(k), plural, risky) < 0)
			break;
		j = k;
	}
	return !days.empty();
}


// --- Matchers ----------------------------------------------------------------------

bool
Parser::Match(size_t i, Phrase& p, bool probe) const
{
	p = Phrase();
	if (i >= fTokens.size())
		return false;
	bool found = (fIsTask && MatchTaskToken(i, p))
		|| MatchHint(i, p)
		|| MatchAlert(i, p)
		|| MatchRecurrence(i, p)
		|| MatchCount(i, p)
		|| MatchUntil(i, p)
		|| MatchDuration(i, p)
		|| MatchRelative(i, p)
		|| MatchDateRange(i, false, p)
		|| MatchTimeRange(i, false, false, p)
		|| MatchPeriod(i, false, p);
	if (found) {
		p.begin = p.core = i;
		return p.end > i;
	}
	if (MatchConnector(i, p))
		return p.end > i;
	if (!probe && MatchLocation(i, p))
		return p.end > i;
	return false;
}


// Tasks: "!", "!!", "!!!", "p1".."p4", "#label", "@label".
bool
Parser::MatchTaskToken(size_t i, Phrase& p) const
{
	const Token& token = fTokens[i];
	const std::string& s = token.norm;
	if (!s.empty() && s.find_first_not_of('!') == std::string::npos) {
		p.kind = Phrase::kPriority;
		p.priority = s.size() == 1 ? 2 : s.size() == 2 ? 3 : 4;
		p.end = i + 1;
		return true;
	}
	if (s.size() == 2 && s[0] == 'p' && s[1] >= '1' && s[1] <= '4') {
		p.kind = Phrase::kPriority;
		p.priority = 5 - (s[1] - '0');
		p.end = i + 1;
		return true;
	}
	if (token.orig.size() > 1 && (token.orig[0] == '#' || token.orig[0] == '@')) {
		std::string label = TrimPunctuation(token.orig.substr(1), ".!?)\"'");
		if (label.empty() || label[0] == '#' || label[0] == '@')
			return false;
		p.kind = Phrase::kLabel;
		p.text = label;
		p.end = i + 1;
		return true;
	}
	return false;
}


// "/Work", "/Family_Calendar"
bool
Parser::MatchHint(size_t i, Phrase& p) const
{
	const Token& token = fTokens[i];
	if (token.orig.size() < 2 || token.orig[0] != '/')
		return false;
	char first = token.orig[1];
	if (!IsAlphaChar(first) && !IsDigitChar(first) && first != '_'
		&& (unsigned char)first < 0x80)
		return false;
	std::string hint = TrimPunctuation(token.orig.substr(1), ".!?)\"'");
	for (char& c : hint) {
		if (c == '_')
			c = ' ';
	}
	hint = TrimPunctuation(hint);
	if (hint.empty())
		return false;
	p.kind = Phrase::kHint;
	p.text = hint;
	p.end = i + 1;
	return true;
}


// "alert 10 minutes before", "remind me 1 hour before", "alarm 15 min",
// "with alert", "30 min before".
bool
Parser::MatchAlert(size_t i, Phrase& p) const
{
	const std::string& s = N(i);
	size_t j = i;
	bool keyword = false;
	if (s == "with") {
		size_t k = i + 1;
		if (Is(k, "a") || Is(k, "an"))
			k++;
		if (!IsAlertWord(N(k)))
			return false;
		p.kind = Phrase::kAlert;
		p.minutes = 0;
		p.end = k + 1;
		int minutes;
		bool inDays;
		size_t after;
		if (ParseDuration(k + 1, minutes, inDays, after)) {
			p.minutes = minutes;
			p.end = after;
			if (IsBeforeWord(N(after)))
				p.end++;
		}
		return true;
	}
	if (IsAlertWord(s)) {
		j = i + 1;
		keyword = true;
	} else if ((s == "remind" || s == "notify" || s == "ping" || s == "alert")
		&& Is(i + 1, "me")) {
		j = i + 2;
		keyword = true;
	}
	if (keyword && Is(j, "me"))
		j++;
	int minutes;
	bool inDays;
	size_t k;
	if (!ParseDuration(j, minutes, inDays, k))
		return false;
	bool before = false;
	if (IsBeforeWord(N(k))) {
		k++;
		before = true;
	} else if (Is(k, "in") && Is(k + 1, "advance")) {
		k += 2;
		before = true;
	}
	if (!keyword && !before)
		return false;
	p.kind = Phrase::kAlert;
	p.minutes = minutes;
	p.end = k;
	return true;
}


bool
Parser::MatchRecurrence(size_t i, Phrase& p) const
{
	const std::string& s = N(i);
	auto simple = [&](const char* frequency, int interval) {
		p.kind = Phrase::kRecurrence;
		p.frequency = frequency;
		p.interval = interval;
		p.end = i + 1;
		return true;
	};
	if (s == "daily" || s == "everyday")
		return simple("DAILY", 1);
	if (s == "weekly")
		return simple("WEEKLY", 1);
	if (s == "biweekly" || s == "fortnightly")
		return simple("WEEKLY", 2);
	if (s == "monthly")
		return simple("MONTHLY", 1);
	if (s == "yearly" || s == "annually")
		return simple("YEARLY", 1);
	if (s == "hourly")
		return simple("HOURLY", 1);
	if (s == "weekdays") {
		simple("WEEKLY", 1);
		p.byDay = {1, 2, 3, 4, 5};
		return true;
	}
	if (s == "weekends") {
		simple("WEEKLY", 1);
		p.byDay = {6, 0};
		return true;
	}
	size_t next;
	if (ParseWeekdayList(i, true, p.byDay, next)) {
		p.kind = Phrase::kRecurrence;
		p.frequency = "WEEKLY";
		p.end = next;
		return true;
	}
	if (s != "every" && s != "each")
		return false;

	size_t j = i + 1;
	int interval = 1;
	bool explicitInterval = false;
	if (Is(j, "other")) {
		interval = 2;
		explicitInterval = true;
		j++;
	} else {
		int value = 0;
		const std::string& n = N(j);
		bool number = ParseSmallInt(n, value) || (value = NumberWord(n)) > 0;
		if (!number) {
			int ordinal;
			if (ParseOrdinal(n, ordinal)) {
				value = ordinal;
				number = true;
			}
		}
		Unit unit = UnitFromWord(N(j + 1));
		if (number && value >= 1 && value <= kMaxInterval
			&& (unit == kDayUnit || unit == kWeekUnit || unit == kMonthUnit
				|| unit == kYearUnit || unit == kHourUnit)) {
			interval = value;
			explicitInterval = true;
			j++;
		}
	}

	p.kind = Phrase::kRecurrence;
	p.interval = interval;
	const std::string& what = N(j);
	Unit unit = UnitFromWord(what);
	if (what == "weekday" || what == "weekdays") {
		p.frequency = "WEEKLY";
		p.interval = 1;
		p.byDay = {1, 2, 3, 4, 5};
		p.end = j + 1;
		return true;
	}
	if (what == "weekend" || what == "weekends") {
		p.frequency = "WEEKLY";
		p.byDay = {6, 0};
		p.end = j + 1;
		return true;
	}
	if (what.size() > 1 && unit != kNoUnit && unit != kMinuteUnit) {
		// Not "m", "h", "d" alone: those are too loose after "every".
		switch (unit) {
			case kHourUnit:
				p.frequency = "HOURLY";
				break;
			case kDayUnit:
				p.frequency = "DAILY";
				break;
			case kWeekUnit:
				p.frequency = "WEEKLY";
				break;
			case kMonthUnit:
				p.frequency = "MONTHLY";
				break;
			default:
				p.frequency = "YEARLY";
				break;
		}
		p.end = j + 1;
		// "every week on monday and thursday"
		size_t after;
		std::vector<int> days;
		if (unit == kWeekUnit && Is(j + 1, "on")
			&& ParseWeekdayList(j + 2, false, days, after)) {
			p.byDay = days;
			p.end = after;
		}
		return true;
	}
	if (ParseWeekdayList(j, false, p.byDay, next)) {
		p.frequency = "WEEKLY";
		p.end = next;
		return true;
	}
	if (explicitInterval)
		return false;
	// "every 15th" (monthly), "every Nov 12" (yearly)
	DateVal date;
	if (ParseDate(j, true, date, next)
		&& (date.kind == DateVal::kDayOnly || date.kind == DateVal::kMonthDay
			|| date.kind == DateVal::kNumeric)) {
		p.frequency = date.kind == DateVal::kDayOnly ? "MONTHLY" : "YEARLY";
		p.hasDate = true;
		p.date = date.date;
		p.end = next;
		return true;
	}
	return false;
}


// "10 times" after a recurrence.
bool
Parser::MatchCount(size_t i, Phrase& p) const
{
	if (fFrequency.empty())
		return false;
	int value = 0;
	if (!ParseSmallInt(N(i), value) && (value = NumberWord(N(i))) == 0)
		return false;
	if (value < 1 || value > 9999 || !Is(i + 1, "times"))
		return false;
	p.kind = Phrase::kCount;
	p.count = value;
	p.end = i + 2;
	return true;
}


// "until Dec 20"
bool
Parser::MatchUntil(size_t i, Phrase& p) const
{
	const std::string& s = N(i);
	if (s != "until" && s != "till" && s != "til" && s != "thru"
		&& s != "through")
		return false;
	DateVal date;
	size_t next;
	if (!ParseDate(i + 1, true, date, next))
		return false;
	p.kind = Phrase::kUntil;
	p.date = date.date;
	p.end = next;
	return true;
}


// "for 2 hours", "for 30 min", "for 3 days"
bool
Parser::MatchDuration(size_t i, Phrase& p) const
{
	if (!Is(i, "for"))
		return false;
	int minutes;
	bool inDays;
	size_t next;
	if (!ParseDuration(i + 1, minutes, inDays, next))
		return false;
	p.kind = Phrase::kDuration;
	p.minutes = minutes;
	p.inDays = inDays;
	p.end = next;
	return true;
}


// "in 3 days", "in 2 weeks", "in a month", "in an hour", "in 30 minutes",
// "in the morning".
bool
Parser::MatchRelative(size_t i, Phrase& p) const
{
	if (!Is(i, "in"))
		return false;
	if (Is(i + 1, "the")) {
		int period = PeriodHour(N(i + 2));
		if (period < 0)
			return false;
		p.kind = Phrase::kPeriod;
		p.period = period;
		p.end = i + 3;
		return true;
	}
	double value;
	size_t k;
	if (ParseNumber(i + 1, value, k)) {
		Unit unit = UnitFromWord(N(k));
		if ((unit == kMonthUnit || unit == kYearUnit) && value >= 1
			&& value <= 1200 && value == (int)value) {
			p.kind = Phrase::kDate;
			p.date = fToday.AddMonths((int)value * (unit == kYearUnit ? 12 : 1));
			p.end = k + 1;
			return true;
		}
	}
	int minutes;
	bool inDays;
	if (!ParseDuration(i + 1, minutes, inDays, k))
		return false;
	if (inDays) {
		int days = minutes / 1440;
		if (days > kMaxDays)
			return false;
		p.kind = Phrase::kDate;
		p.date = fToday.AddDays(days);
	} else {
		p.kind = Phrase::kRelative;
		p.seconds = (int64_t)minutes * 60;
	}
	p.end = k;
	return true;
}


// A date, or a date range: "Nov 12-14", "Nov 12 to Nov 14", "Dec 20 to Jan 3",
// "monday to wednesday".
bool
Parser::MatchDateRange(size_t i, bool qualified, Phrase& p) const
{
	DateVal first;
	size_t j;
	if (!ParseDate(i, qualified, first, j))
		return false;
	p.kind = Phrase::kDate;
	p.date = first.date;
	p.weak = first.kind == DateVal::kWeekday;
	p.period = first.period;
	p.end = j;

	if (!IsRangeSeparator(N(j)))
		return true;
	bool viaUntil = Is(j, "until") || Is(j, "till") || Is(j, "til");
	Date last;
	bool lastYear = false;
	size_t k = 0;
	int day;
	bool found = false;
	if (first.kind == DateVal::kMonthDay && ParseDayNumber(N(j + 1), day)
		&& MonthFromWord(N(j + 2)) == 0 && !Is(j + 2, "of")) {
		// "Nov 12-14": the same month.
		k = j + 2;
		int year = first.date.year;
		if (ParseYear(N(k), year)) {
			k++;
			lastYear = true;
		}
		Date date(year, first.date.month, day);
		if (date.IsValid()) {
			if (lastYear && !first.explicitYear)
				p.date = Date(year, first.date.month, first.date.day);
			last = date;
			found = p.date.IsValid();
		}
	} else {
		DateVal second;
		if (ParseDate(j + 1, true, second, k) && second.period < 0) {
			last = second.date;
			lastYear = second.explicitYear;
			if (second.kind == DateVal::kWeekday) {
				// "monday to wednesday": the first such day from the start.
				last = p.date.AddDays(
					(second.date.Weekday() - p.date.Weekday() + 7) % 7);
			}
			found = true;
		}
	}
	if (!found)
		return true;
	for (int guard = 0; last < p.date && !lastYear && guard < 8; guard++)
		last = last.AddYears(1);
	if (last < p.date)
		return true;
	p.hasEndDate = true;
	p.endDate = last;
	p.weak = false;
	p.viaUntil = viaUntil;
	p.end = k;
	return true;
}


// "3pm", "15:30", "noon", "3-4pm", "3pm-4pm", "3 to 5pm", "between 3 and 4".
// weakAllowed: a bare number ("at 5") counts as a time.
bool
Parser::MatchTimeRange(size_t i, bool weakAllowed, bool allowAnd,
	Phrase& p) const
{
	TimeVal a;
	size_t j;
	if (!ParseTime(i, true, a, j))
		return false;
	TimeVal b;
	size_t k = 0;
	bool range = false;
	if (IsRangeSeparator(N(j)) || (allowAnd && Is(j, "and"))) {
		if (ParseTime(j + 1, true, b, k) && (weakAllowed || b.strong)
			&& (weakAllowed || a.strong || b.meridiem != 0))
			range = true;
	}
	if (!range) {
		if (!a.strong && !weakAllowed)
			return false;
		p.kind = Phrase::kTime;
		p.hour = ResolveHour(a, a.meridiem);
		p.minute = a.minute;
		p.guessed = a.meridiem == 0 && !a.fixed;
		p.end = j;
		return true;
	}

	int endHour = ResolveHour(b, b.meridiem);
	int startHour;
	if (a.meridiem == 0 && b.meridiem != 0 && !a.fixed) {
		// "3 to 5pm": carry the meridiem back; "11-1pm" is 11am.
		startHour = ResolveHour(a, b.meridiem);
		if (startHour * 60 + a.minute > endHour * 60 + b.minute)
			startHour = ResolveHour(a, b.meridiem == 'p' ? 'a' : 'p');
	} else
		startHour = ResolveHour(a, a.meridiem);
	if (a.meridiem == 0 && b.meridiem == 0 && !b.fixed
		&& endHour * 60 + b.minute <= startHour * 60 + a.minute && endHour < 12)
		endHour += 12;
	p.kind = Phrase::kTime;
	p.hour = startHour;
	p.minute = a.minute;
	p.guessed = a.meridiem == 0 && b.meridiem == 0 && !a.fixed;
	p.hasEndTime = true;
	p.endHour = endHour;
	p.endMinute = b.minute;
	p.end = k;
	return true;
}


// "morning", "afternoon", "evening"; "night" only after a date or "at".
bool
Parser::MatchPeriod(size_t i, bool qualified, Phrase& p) const
{
	const std::string& s = N(i);
	int period = PeriodHour(s);
	if (period < 0)
		return false;
	if (s == "night" && !qualified && !(i > 0 && fConsumed[i - 1]))
		return false;
	p.kind = Phrase::kPeriod;
	p.period = period;
	p.end = i + 1;
	return true;
}


// "on friday", "at 5", "from 3-4pm", "due Nov 1", "by friday", "for monday",
// "between 3 and 4pm", "on mondays".
bool
Parser::MatchConnector(size_t i, Phrase& p) const
{
	static const char* const kConnectors[] = {"on", "at", "from", "due", "by",
		"for", "starting", "around", "@", "between", "before"};
	auto isConnector = [](const std::string& s) {
		for (const char* word : kConnectors) {
			if (s == word)
				return true;
		}
		return false;
	};
	const std::string& s = N(i);
	if (!isConnector(s))
		return false;
	size_t j = i + 1;
	std::string last = s;
	// "due on", "due by", "starting on", "due at"
	if ((s == "due" || s == "starting") && isConnector(N(j))) {
		last = N(j);
		j++;
	}
	bool weak = last == "at" || last == "@" || last == "from" || last == "by"
		|| last == "between" || last == "around" || last == "before";
	bool allowAnd = last == "between";
	bool found = false;
	if (last != "between" && last != "around")
		found = MatchDateRange(j, true, p);
	if (!found)
		found = MatchTimeRange(j, weak, allowAnd, p);
	if (!found && last == "at")
		found = MatchPeriod(j, true, p);
	if (!found && last == "on")
		found = MatchRecurrence(j, p);
	if (!found)
		return false;
	p.begin = i;
	p.core = j;
	return true;
}


bool
Parser::IsLocationStop(size_t i) const
{
	if (i >= fTokens.size() || fConsumed[i])
		return true;
	const std::string& s = N(i);
	if (s == "with" || s == "for" || s == "about" || s == "to" || s == "re"
		|| s == "regarding" || s == "-")
		return true;
	Phrase probe;
	return Match(i, probe, true);
}


// "at Cafe Rio", "@ Nobu", "@Nobu", "in Room 5" (events: "in" needs a
// capitalized place so "in time" stays in the title).
bool
Parser::MatchLocation(size_t i, Phrase& p) const
{
	const Token& token = fTokens[i];
	size_t first;
	size_t textBegin;
	if (token.norm == "at" || token.norm == "@") {
		first = i + 1;
		if (first >= fTokens.size() || IsLocationStop(first))
			return false;
		textBegin = fTokens[first].begin;
	} else if (token.norm == "in" && !fIsTask) {
		first = i + 1;
		if (first >= fTokens.size())
			return false;
		const Token& place = fTokens[first];
		bool capital = !place.orig.empty() && IsUpperChar(place.orig[0]);
		if (!capital && place.norm == "the" && first + 1 < fTokens.size()) {
			const std::string& orig = fTokens[first + 1].orig;
			capital = !orig.empty() && IsUpperChar(orig[0]);
		}
		if (!capital || IsLocationStop(first))
			return false;
		textBegin = place.begin;
	} else if (!fIsTask && token.orig.size() > 1 && token.orig[0] == '@') {
		first = i;
		textBegin = token.begin + 1;
	} else
		return false;

	size_t j = first + 1;
	while (j < fTokens.size() && !IsLocationStop(j))
		j++;
	std::string text = TrimPunctuation(
		fText.substr(textBegin, fTokens[j - 1].end - textBegin), ".!?)(\"");
	if (text.empty())
		return false;
	p.kind = Phrase::kLocation;
	p.begin = i;
	p.core = first;
	p.end = j;
	p.text = text;
	return true;
}


// --- Applying ----------------------------------------------------------------------

// "todo", "task", "reminder", "remind me (to)", "to do", "- ".
void
Parser::DetectTask()
{
	if (fTokens.empty())
		return;
	const std::string& s = N(0);
	size_t count = 0;
	if (s == "todo" || s == "to-do" || s == "task")
		count = 1;
	else if (s == "reminder") {
		count = 1;
		fRemindKeyword = true;
	} else if (s == "to" && Is(1, "do"))
		count = 2;
	else if (s == "remind" && Is(1, "me")) {
		int minutes;
		bool inDays;
		size_t next;
		if (Is(2, "to"))
			count = 3;
		else if (!ParseDuration(2, minutes, inDays, next))
			count = 2;
		fRemindKeyword = count > 0;
	} else if (fTokens[0].orig == "-" || fTokens[0].orig == "*")
		count = 1;
	if (count == 0)
		return;
	fIsTask = true;
	for (size_t k = 0; k < count && k < fTokens.size(); k++)
		fConsumed[k] = true;
}


void
Parser::AddDuePhrase(const Phrase& p)
{
	fDue.push_back({p.begin, p.core, p.end});
}


void
Parser::Apply(const Phrase& p)
{
	for (size_t k = p.begin; k < p.end && k < fConsumed.size(); k++)
		fConsumed[k] = true;

	switch (p.kind) {
		case Phrase::kDate:
			if (!fHasDate || (fDateWeak && !p.weak)) {
				fHasDate = true;
				fDate = p.date;
				fDateWeak = p.weak;
				fHasEndDate = p.hasEndDate;
				fEndDate = p.endDate;
				fViaUntil = p.viaUntil;
			}
			if (p.period >= 0 && fPeriod < 0)
				fPeriod = p.period;
			AddDuePhrase(p);
			break;
		case Phrase::kTime:
			if (!fHasTime) {
				fHasTime = true;
				fHour = p.hour;
				fMinute = p.minute;
				fGuessed = p.guessed;
				fHasEndTime = p.hasEndTime;
				fEndHour = p.endHour;
				fEndMinute = p.endMinute;
			}
			AddDuePhrase(p);
			break;
		case Phrase::kPeriod:
			if (fPeriod < 0)
				fPeriod = p.period;
			AddDuePhrase(p);
			break;
		case Phrase::kRelative:
			if (!fRelative) {
				fRelative = true;
				fRelativeSeconds = p.seconds;
			}
			AddDuePhrase(p);
			break;
		case Phrase::kRecurrence:
			if (fFrequency == "WEEKLY" && fByDay.empty() && !p.byDay.empty()
				&& p.frequency == "WEEKLY") {
				// "weekly ... on fridays"
				fByDay = p.byDay;
				fInterval = std::max(fInterval, p.interval);
			} else if (fFrequency.empty()) {
				fFrequency = p.frequency;
				fInterval = p.interval;
				fByDay = p.byDay;
				if (p.hasDate && (!fHasDate || fDateWeak)) {
					fHasDate = true;
					fDate = p.date;
					fDateWeak = false;
				}
			}
			AddDuePhrase(p);
			break;
		case Phrase::kCount:
			if (fCount == 0)
				fCount = p.count;
			AddDuePhrase(p);
			break;
		case Phrase::kUntil:
			if (!fHasUntil) {
				fHasUntil = true;
				fUntil = p.date;
			}
			AddDuePhrase(p);
			break;
		case Phrase::kDuration:
			if (fDuration < 0) {
				fDuration = p.minutes;
				fDurationInDays = p.inDays;
			}
			break;
		case Phrase::kAlert:
			if (fAlarm < 0)
				fAlarm = p.minutes;
			break;
		case Phrase::kPriority:
			if (fPriority == 0)
				fPriority = p.priority;
			break;
		case Phrase::kLabel:
			if (std::find(fLabels.begin(), fLabels.end(), p.text) == fLabels.end())
				fLabels.push_back(p.text);
			break;
		case Phrase::kHint:
			if (fHint.empty())
				fHint = p.text;
			break;
		case Phrase::kLocation:
			if (fLocation.empty())
				fLocation = p.text;
			break;
		case Phrase::kNone:
			break;
	}
}


// Original text from the start of token `first` to the end of token `last`.
std::string
Parser::Span(size_t first, size_t last) const
{
	if (first > last || last >= fTokens.size())
		return std::string();
	size_t begin = fTokens[first].begin;
	return fText.substr(begin, fTokens[last].end - begin);
}


std::string
Parser::BuildTitle() const
{
	std::string title;
	size_t previousEnd = std::string::npos;
	for (size_t i = 0; i < fTokens.size(); i++) {
		if (fConsumed[i])
			continue;
		const Token& token = fTokens[i];
		if (!title.empty() && token.begin != previousEnd)
			title += ' ';
		title += token.orig;
		previousEnd = token.end;
	}
	return TrimPunctuation(title);
}


std::string
Parser::BuildRule(bool timed, int hour, int minute) const
{
	if (fFrequency.empty())
		return std::string();
	std::string rule = "FREQ=" + fFrequency;
	if (fInterval > 1)
		rule += ";INTERVAL=" + std::to_string(fInterval);
	if (!fByDay.empty()) {
		std::vector<int> days = fByDay;
		// Monday first, as people read them.
		std::sort(days.begin(), days.end(),
			[](int a, int b) { return (a + 6) % 7 < (b + 6) % 7; });
		rule += ";BYDAY=";
		for (size_t k = 0; k < days.size(); k++) {
			if (k > 0)
				rule += ',';
			rule += kDayCodes[days[k]];
		}
	}
	if (fHasUntil) {
		if (timed)
			rule += ";UNTIL=" + FormatBasicUtc(ToInstant(
				DateTime(fUntil, 23, 59, 59), ""));
		else
			rule += ";UNTIL=" + fUntil.ToBasic();
	} else if (fCount > 0)
		rule += ";COUNT=" + std::to_string(fCount);
	return rule;
}


QuickEntryResult
Parser::Run()
{
	QuickEntryResult result;
	DetectTask();

	size_t i = 0;
	while (i < fTokens.size()) {
		if (fConsumed[i]) {
			i++;
			continue;
		}
		Phrase phrase;
		if (Match(i, phrase, false)) {
			Apply(phrase);
			i = phrase.end;
		} else
			i++;
	}

	// "until" without a recurrence ends an all-day range ("Out until Friday");
	// a range written with "until" next to a recurrence is its UNTIL.
	if (!fFrequency.empty() && fViaUntil && fHasEndDate && !fHasUntil) {
		fHasUntil = true;
		fUntil = fEndDate;
		fHasEndDate = false;
	}
	if (fFrequency.empty() && fHasUntil) {
		Date start = fHasDate ? fDate : fToday;
		if (fUntil >= start && !fHasEndDate) {
			fHasDate = true;
			fDate = start;
			fHasEndDate = true;
			fEndDate = fUntil;
		}
		fHasUntil = false;
	}

	// The time of day: an explicit time, adjusted by a period word ("7
	// tomorrow morning"), or the period itself ("tonight" = 8pm).
	bool timed = fHasTime || fPeriod >= 0;
	int hour = fHour;
	int minute = fMinute;
	int endHour = fEndHour;
	int endMinute = fEndMinute;
	if (fHasTime && fGuessed && fPeriod >= 0) {
		int shift = 0;
		if (fPeriod >= 12 && hour < 12)
			shift = 12;
		else if (fPeriod < 12 && hour >= 12 && hour - 12 >= 1)
			shift = -12;
		hour += shift;
		if (fHasEndTime && endHour + shift >= 0 && endHour + shift <= 23)
			endHour += shift;
	} else if (!fHasTime && fPeriod >= 0) {
		hour = fPeriod;
		minute = 0;
	}

	Date date = fToday;
	Instant start = 0;
	if (fRelative) {
		start = fNow + fRelativeSeconds;
		DateTime wall = ToWall(start);
		date = wall.date;
		hour = wall.hour;
		minute = wall.minute;
		timed = true;
		fHasEndTime = false;
	} else if (fHasDate) {
		date = fDate;
	} else if (!fByDay.empty()) {
		// "every monday": the first such day, today if it still can be.
		for (int k = 0; k < 14; k++) {
			Date candidate = fToday.AddDays(k);
			if (std::find(fByDay.begin(), fByDay.end(), candidate.Weekday())
				== fByDay.end())
				continue;
			if (timed && ToInstant(DateTime(candidate, hour, minute), "") < fNow)
				continue;
			date = candidate;
			break;
		}
	} else if (timed && ToInstant(DateTime(fToday, hour, minute), "") < fNow) {
		// Only a time, and it has passed today: tomorrow.
		date = fToday.AddDays(1);
	}

	result.isTask = fIsTask;
	result.hasDate = fHasDate || fRelative || !fFrequency.empty();
	result.hasTime = timed;
	result.allDay = !timed;
	if (timed) {
		if (!fRelative)
			start = ToInstant(DateTime(date, hour, minute), "");
		Instant end;
		if (fHasEndTime) {
			Date endDay = fHasEndDate ? fEndDate : date;
			end = ToInstant(DateTime(endDay, endHour, endMinute), "");
			if (end <= start)
				end = ToInstant(DateTime(date.AddDays(1), endHour, endMinute), "");
		} else if (fDuration > 0)
			end = start + (Instant)fDuration * 60;
		else
			end = start + (Instant)fDefaultMinutes * 60;
		result.start = start;
		result.end = end;
		result.startDate = date;
		result.endDate = LocalDate(end > start ? end - 1 : end);
	} else {
		result.startDate = date;
		result.endDate = date;
		if (fHasEndDate && fEndDate >= date)
			result.endDate = fEndDate;
		else if (fDuration >= 1440 && fDurationInDays)
			result.endDate = date.AddDays(std::min(fDuration / 1440, kMaxDays) - 1);
		result.start = StartOfDay(result.startDate);
		result.end = StartOfDay(result.endDate.AddDays(1));
	}

	result.rrule = BuildRule(timed, hour, minute);
	result.alarmMinutes = fAlarm;
	if (fIsTask && fRemindKeyword && timed && result.alarmMinutes < 0)
		result.alarmMinutes = 0;
	result.location = fLocation;
	result.calendarHint = fHint;
	if (fIsTask) {
		result.priority = fPriority;
		result.labels = fLabels;
		// The date phrases as typed, adjacent ones joined: "tomorrow at 6pm".
		std::string due;
		size_t k = 0;
		while (k < fDue.size()) {
			size_t first = fDue[k].core;
			size_t last = fDue[k].end;
			while (k + 1 < fDue.size() && fDue[k + 1].begin == last) {
				k++;
				last = fDue[k].end;
			}
			std::string piece = TrimPunctuation(Span(first, last - 1), ".!?");
			if (!piece.empty()) {
				if (!due.empty())
					due += ' ';
				due += piece;
			}
			k++;
		}
		result.dueString = due;
	}

	result.title = BuildTitle();
	if (result.title.empty())
		result.title = fIsTask ? "New Task" : "New Event";
	return result;
}


// --- Describe ------------------------------------------------------------------------

std::string
FormatDay(const Date& date)
{
	char buffer[48];
	if (!date.IsValid())
		return "?";
	if (date.year != Today().year) {
		snprintf(buffer, sizeof(buffer), "%s, %s %d, %d", kDayNames[date.Weekday()],
			kMonthNames[date.month - 1], date.day, date.year);
	} else {
		snprintf(buffer, sizeof(buffer), "%s, %s %d", kDayNames[date.Weekday()],
			kMonthNames[date.month - 1], date.day);
	}
	return buffer;
}


std::string
FormatClock(const DateTime& wall)
{
	int hour = wall.hour % 12;
	if (hour == 0)
		hour = 12;
	char buffer[16];
	snprintf(buffer, sizeof(buffer), "%d:%02d %s", hour, wall.minute,
		wall.hour < 12 ? "AM" : "PM");
	return buffer;
}


std::string
FormatMinutes(int minutes)
{
	char buffer[32];
	if (minutes % 10080 == 0 && minutes > 0)
		snprintf(buffer, sizeof(buffer), "%d wk", minutes / 10080);
	else if (minutes % 1440 == 0 && minutes > 0)
		snprintf(buffer, sizeof(buffer), "%d day%s", minutes / 1440,
			minutes == 1440 ? "" : "s");
	else if (minutes % 60 == 0 && minutes > 0)
		snprintf(buffer, sizeof(buffer), "%d hr", minutes / 60);
	else
		snprintf(buffer, sizeof(buffer), "%d min", minutes);
	return buffer;
}


// "Every week on Mon, Wed until Dec 20"
std::string
DescribeRepeat(const std::string& rule)
{
	std::string frequency;
	int interval = 1;
	std::vector<std::string> days;
	std::string until;
	int count = 0;
	size_t start = 0;
	while (start < rule.size()) {
		size_t end = rule.find(';', start);
		if (end == std::string::npos)
			end = rule.size();
		std::string part = rule.substr(start, end - start);
		size_t equals = part.find('=');
		if (equals != std::string::npos) {
			std::string key = part.substr(0, equals);
			std::string value = part.substr(equals + 1);
			if (key == "FREQ")
				frequency = value;
			else if (key == "INTERVAL")
				interval = std::max(1, atoi(value.c_str()));
			else if (key == "COUNT")
				count = atoi(value.c_str());
			else if (key == "UNTIL")
				until = value;
			else if (key == "BYDAY") {
				size_t s = 0;
				while (s < value.size()) {
					size_t comma = value.find(',', s);
					if (comma == std::string::npos)
						comma = value.size();
					days.push_back(value.substr(s, comma - s));
					s = comma + 1;
				}
			}
		}
		start = end + 1;
	}

	const char* unit = "day";
	if (frequency == "HOURLY")
		unit = "hour";
	else if (frequency == "WEEKLY")
		unit = "week";
	else if (frequency == "MONTHLY")
		unit = "month";
	else if (frequency == "YEARLY")
		unit = "year";

	std::string text;
	bool weekdays = days.size() == 5;
	for (const std::string& day : days) {
		if (day == "SA" || day == "SU")
			weekdays = false;
	}
	if (weekdays && interval == 1 && frequency == "WEEKLY")
		text = "Every weekday";
	else {
		if (interval == 1)
			text = std::string("Every ") + unit;
		else
			text = "Every " + std::to_string(interval) + " " + unit + "s";
		if (!days.empty()) {
			text += " on ";
			for (size_t k = 0; k < days.size(); k++) {
				if (k > 0)
					text += ", ";
				const char* name = days[k].c_str();
				for (int w = 0; w < 7; w++) {
					if (days[k] == kDayCodes[w])
						name = kDayNames[w];
				}
				text += name;
			}
		}
	}
	if (until.size() >= 8) {
		Date date;
		if (Date::FromIso(until.substr(0, 8), date)) {
			if (until.size() > 8) {
				// UTC: the local day it falls on.
				DateTime wall;
				bool utc;
				bool dateOnly;
				if (ParseBasic(until, wall, utc, dateOnly) && utc)
					date = LocalDate(wall.AsUtcSeconds());
			}
			char buffer[24];
			snprintf(buffer, sizeof(buffer), " until %s %d",
				kMonthNames[date.month - 1], date.day);
			text += buffer;
		}
	} else if (count > 0)
		text += ", " + std::to_string(count) + " times";
	return text;
}

}	// namespace


std::string
QuickEntryResult::Describe() const
{
	std::vector<std::string> parts;
	parts.push_back(isTask ? "Task" : "Event");

	bool dated = !isTask || hasDate || hasTime;
	if (dated) {
		std::string when;
		if (allDay) {
			when = FormatDay(startDate);
			if (endDate > startDate)
				when += kDash + FormatDay(endDate);
			else if (!isTask)
				when += std::string(kSeparator) + "All day";
		} else {
			DateTime first = ToWall(start);
			DateTime last = ToWall(end);
			when = FormatDay(first.date);
			if (isTask)
				when += ", " + FormatClock(first);
			else if (last.date == first.date || (end - start < 86400
				&& last.hour == 0 && last.minute == 0))
				when += std::string(kSeparator) + FormatClock(first) + kDash
					+ FormatClock(last);
			else
				when += ", " + FormatClock(first) + kDash + FormatDay(last.date)
					+ ", " + FormatClock(last);
		}
		parts.push_back(isTask ? "Due " + when : when);
	}
	if (!location.empty())
		parts.push_back("at " + location);
	if (!rrule.empty())
		parts.push_back(DescribeRepeat(rrule));
	if (alarmMinutes == 0)
		parts.push_back("Alert at start");
	else if (alarmMinutes > 0)
		parts.push_back("Alert " + FormatMinutes(alarmMinutes) + " before");
	if (isTask) {
		if (priority >= 2)
			parts.push_back(std::string((size_t)(priority - 1), '!'));
		else if (priority == 1)
			parts.push_back("Low priority");
		for (const std::string& label : labels)
			parts.push_back("#" + label);
	}
	if (!calendarHint.empty())
		parts.push_back("/" + calendarHint);

	std::string text;
	for (size_t k = 0; k < parts.size(); k++) {
		if (k > 0)
			text += kSeparator;
		text += parts[k];
	}
	return text;
}


QuickEntryResult
ParseQuickEntry(const std::string& text, Instant now, int defaultMinutes,
	int weekStart)
{
	try {
		Parser parser(text, now, defaultMinutes, weekStart);
		return parser.Run();
	} catch (...) {
		// Out of memory or similar: the text as the title, nothing parsed.
		QuickEntryResult result;
		result.title = "New Event";
		return result;
	}
}

}	// namespace aircal
