#include "Editing.h"
#include "Recurrence.h"

namespace aircal {

namespace {

// Copies the user-editable fields.
void
CopyContent(Event& to, const Event& from)
{
	to.title = from.title;
	to.location = from.location;
	to.notes = from.notes;
	to.url = from.url;
	to.alarms = from.alarms;
	to.attendees = from.attendees;
	to.tentative = from.tentative;
}


void
CopyTimes(Event& to, const Event& from)
{
	to.allDay = from.allDay;
	to.start = from.start;
	to.end = from.end;
	to.startDate = from.startDate;
	to.endDate = from.endDate;
	to.tzid = from.tzid;
}


Event*
FindOverride(Store& store, const OccurrenceRef& ref, const Event& master)
{
	if (!ref.overrideId.empty()) {
		Event* found = store.FindEvent(ref.overrideId);
		if (found != nullptr)
			return found;
	}
	for (Event* candidate : store.OverridesOf(master.id)) {
		bool same = master.allDay ? candidate->recurrenceDate == ref.originalDate
			: candidate->recurrenceId == ref.originalStart;
		if (same)
			return candidate;
	}
	return nullptr;
}


bool
IsFirstOccurrence(const Event& master, const OccurrenceRef& ref)
{
	return master.allDay ? ref.originalDate <= master.startDate
		: ref.originalStart <= master.start;
}


// Ends the series before the occurrence; drops overrides after it.
void
TruncateSeries(Store& store, Event& master, const OccurrenceRef& ref)
{
	Instant before = master.allDay ? StartOfDay(ref.originalDate) : ref.originalStart;
	Event edited = master;
	edited.rrule = TruncateRule(master.rrule, master, before);
	std::vector<std::string> drop;
	for (Event* child : store.OverridesOf(master.id)) {
		Instant at = master.allDay ? StartOfDay(child->recurrenceDate)
			: child->recurrenceId;
		if (at >= before)
			drop.push_back(child->id);
	}
	for (auto& id : drop)
		store.DeleteEvent(id);
	store.UpdateEvent(edited);
}

}	// namespace


bool
IsRecurringOccurrence(Store& store, const OccurrenceRef& ref)
{
	std::lock_guard<std::recursive_mutex> lock(store.Mutex());
	Event* event = store.FindEvent(ref.eventId);
	return event != nullptr && event->IsRecurring();
}


bool
ResolveOccurrence(Store& store, const OccurrenceRef& ref, Event& out)
{
	std::lock_guard<std::recursive_mutex> lock(store.Mutex());
	Event* master = store.FindEvent(ref.eventId);
	if (master == nullptr)
		return false;
	if (!master->IsRecurring()) {
		out = *master;
		return true;
	}
	Event* override = FindOverride(store, ref, *master);
	if (override != nullptr) {
		out = *override;
		return true;
	}
	out = *master;
	if (master->allDay) {
		int64_t days = master->Duration();
		out.startDate = ref.originalDate;
		out.endDate = ref.originalDate.AddDays(days > 0 ? days : 1);
	} else {
		int64_t length = master->end - master->start;
		out.start = ref.originalStart;
		out.end = ref.originalStart + length;
	}
	return true;
}


std::string
SaveEvent(Store& store, const OccurrenceRef& ref, const Event& edited,
	EditScope scope)
{
	std::lock_guard<std::recursive_mutex> lock(store.Mutex());
	Event* master = ref.eventId.empty() ? nullptr : store.FindEvent(ref.eventId);
	if (master == nullptr) {
		// New event.
		Event fresh = edited;
		if (fresh.id.empty())
			fresh.id = NewId();
		fresh.masterId.clear();
		fresh.remoteId.clear();
		fresh.pending = Pending::Create;
		store.UpdateEvent(fresh);
		return fresh.id;
	}

	if (!master->IsRecurring()) {
		Event updated = *master;
		CopyContent(updated, edited);
		CopyTimes(updated, edited);
		updated.calendarId = edited.calendarId;
		updated.rrule = edited.rrule;
		if (!updated.rrule.empty() && master->rrule.empty()) {
			updated.exdates.clear();
			updated.exdateDays.clear();
		}
		store.UpdateEvent(updated);
		return updated.id;
	}

	if (scope == EditScope::Future && IsFirstOccurrence(*master, ref))
		scope = EditScope::All;

	if (scope == EditScope::This) {
		Event* existing = FindOverride(store, ref, *master);
		Event override = existing != nullptr ? *existing : *master;
		if (existing == nullptr) {
			override.id = NewId();
			override.remoteId.clear();
			override.etag.clear();
			override.raw.clear();
			override.exdates.clear();
			override.exdateDays.clear();
			override.rdates.clear();
			override.masterId = master->id;
			override.recurrenceId = master->allDay ? 0 : ref.originalStart;
			override.recurrenceDate = master->allDay ? ref.originalDate
				: LocalDate(ref.originalStart);
		}
		override.rrule.clear();
		override.cancelled = false;
		CopyContent(override, edited);
		CopyTimes(override, edited);
		override.calendarId = master->calendarId;
		override.uid = master->uid;
		store.UpdateEvent(override);
		return override.id;
	}

	if (scope == EditScope::Future) {
		std::string oldRule = master->rrule;
		TruncateSeries(store, *master, ref);
		Event series = edited;
		series.id = NewId();
		series.uid.clear();
		series.remoteId.clear();
		series.etag.clear();
		series.raw.clear();
		series.masterId.clear();
		series.exdates.clear();
		series.exdateDays.clear();
		series.rdates.clear();
		RecurrenceRule rule;
		std::string text = edited.rrule.empty() ? oldRule : edited.rrule;
		if (RecurrenceRule::Parse(text, rule)) {
			rule.count = 0;	// the remaining count is unknown after a split
			series.rrule = rule.ToString();
		} else
			series.rrule = text;
		series.pending = Pending::Create;
		store.UpdateEvent(series);
		return series.id;
	}

	// All: shift the series by the occurrence's change.
	Event updated = *master;
	CopyContent(updated, edited);
	updated.calendarId = edited.calendarId;
	updated.rrule = edited.rrule;
	if (edited.allDay == master->allDay) {
		if (master->allDay) {
			int64_t shift = edited.startDate.DayNumber() - ref.originalDate.DayNumber();
			int64_t days = edited.endDate.DayNumber() - edited.startDate.DayNumber();
			updated.startDate = master->startDate.AddDays(shift);
			updated.endDate = updated.startDate.AddDays(days > 0 ? days : 1);
		} else {
			// Same wall-clock change in the event's zone.
			Instant shift = edited.start - ref.originalStart;
			updated.start = master->start + shift;
			updated.end = updated.start + (edited.end - edited.start);
			updated.tzid = edited.tzid.empty() ? master->tzid : edited.tzid;
		}
	} else {
		// Timed <-> all-day: anchor at the series' first date.
		Date first = master->allDay ? master->startDate : LocalDate(master->start);
		updated.allDay = edited.allDay;
		if (edited.allDay) {
			int64_t days = edited.endDate.DayNumber() - edited.startDate.DayNumber();
			updated.startDate = first;
			updated.endDate = first.AddDays(days > 0 ? days : 1);
		} else {
			DateTime wall = ToWall(edited.start);
			wall.date = first;
			updated.start = ToInstant(wall);
			updated.end = updated.start + (edited.end - edited.start);
			updated.tzid = edited.tzid;
		}
		// Old recurrence ids no longer match.
		updated.exdates.clear();
		updated.exdateDays.clear();
		for (Event* child : store.OverridesOf(master->id))
			store.DeleteEvent(child->id);
	}
	if (updated.rrule.empty()) {
		// The series stops repeating: its overrides go.
		std::vector<std::string> children;
		for (Event* child : store.OverridesOf(master->id))
			children.push_back(child->id);
		for (auto& id : children)
			store.DeleteEvent(id);
	}
	store.UpdateEvent(updated);
	return updated.id;
}


void
DeleteOccurrence(Store& store, const OccurrenceRef& ref, EditScope scope)
{
	std::lock_guard<std::recursive_mutex> lock(store.Mutex());
	Event* master = store.FindEvent(ref.eventId);
	if (master == nullptr)
		return;
	if (!master->IsRecurring() || scope == EditScope::All
		|| (scope == EditScope::Future && IsFirstOccurrence(*master, ref))) {
		store.DeleteEvent(master->id);
		return;
	}
	if (scope == EditScope::Future) {
		TruncateSeries(store, *master, ref);
		return;
	}
	Event* existing = FindOverride(store, ref, *master);
	Event cancelled;
	if (existing != nullptr)
		cancelled = *existing;
	else {
		cancelled = *master;
		cancelled.id = NewId();
		cancelled.remoteId.clear();
		cancelled.etag.clear();
		cancelled.raw.clear();
		cancelled.rrule.clear();
		cancelled.exdates.clear();
		cancelled.exdateDays.clear();
		cancelled.rdates.clear();
		cancelled.masterId = master->id;
		cancelled.recurrenceId = master->allDay ? 0 : ref.originalStart;
		cancelled.recurrenceDate = master->allDay ? ref.originalDate
			: LocalDate(ref.originalStart);
		if (master->allDay) {
			cancelled.startDate = ref.originalDate;
			cancelled.endDate = ref.originalDate.AddDays(master->Duration());
		} else {
			cancelled.start = ref.originalStart;
			cancelled.end = ref.originalStart + (master->end - master->start);
		}
	}
	cancelled.cancelled = true;
	store.UpdateEvent(cancelled);
}


std::string
MoveOccurrence(Store& store, const OccurrenceRef& ref, Instant newStart,
	Instant newEnd, bool allDay, const Date& newDate, int newDays,
	EditScope scope)
{
	std::lock_guard<std::recursive_mutex> lock(store.Mutex());
	Event current;
	if (!ResolveOccurrence(store, ref, current))
		return "";
	Event edited = current;
	edited.allDay = allDay;
	if (allDay) {
		edited.startDate = newDate;
		edited.endDate = newDate.AddDays(newDays > 0 ? newDays : 1);
	} else {
		edited.start = newStart;
		edited.end = newEnd > newStart ? newEnd : newStart + 3600;
		if (current.allDay)
			edited.tzid = "";
	}
	Event* master = store.FindEvent(ref.eventId);
	if (master != nullptr && master->IsRecurring())
		edited.rrule = master->rrule;
	return SaveEvent(store, ref, edited, scope);
}


std::string
DuplicateEvent(Store& store, const OccurrenceRef& ref)
{
	std::lock_guard<std::recursive_mutex> lock(store.Mutex());
	Event current;
	if (!ResolveOccurrence(store, ref, current))
		return "";
	Event copy = current;
	copy.id = NewId();
	copy.uid.clear();
	copy.remoteId.clear();
	copy.etag.clear();
	copy.raw.clear();
	copy.masterId.clear();
	copy.recurrenceId = 0;
	copy.rrule.clear();
	copy.exdates.clear();
	copy.exdateDays.clear();
	copy.rdates.clear();
	copy.cancelled = false;
	copy.pending = Pending::Create;
	Calendar* calendar = store.FindCalendar(copy.calendarId);
	if (calendar == nullptr || calendar->readOnly) {
		Calendar* fallback = store.DefaultCalendar(false);
		if (fallback == nullptr)
			return "";
		copy.calendarId = fallback->id;
	}
	store.UpdateEvent(copy);
	return copy.id;
}

}	// namespace aircal
