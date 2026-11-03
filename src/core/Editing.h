// Editing events, including single occurrences of recurring series. All
// functions take the store's lock and mark changes pending for sync.
#pragma once

#include "Store.h"

namespace aircal {

enum class EditScope {
	This,		// only this occurrence (an override)
	Future,		// this and following occurrences (splits the series)
	All			// the whole series
};

// Identifies what is being edited: an event, or one occurrence of a series.
struct OccurrenceRef {
	std::string eventId;	// master or standalone event
	std::string overrideId;	// existing override of that occurrence, if any
	Instant originalStart = 0;	// recurrence id (timed)
	Date originalDate;			// recurrence id (all-day)
};

// Whether the reference points into a recurring series (so callers ask for
// a scope).
bool IsRecurringOccurrence(Store& store, const OccurrenceRef& ref);

// The event as it looks at that occurrence (master values with the
// occurrence's times, or the override).
bool ResolveOccurrence(Store& store, const OccurrenceRef& ref, Event& out);

// Saves `edited` (new values, times as shown for the occurrence). For a new
// event, ref.eventId is empty and edited.id may be empty. Returns the id of
// the event that now holds the change.
std::string SaveEvent(Store& store, const OccurrenceRef& ref,
	const Event& edited, EditScope scope);

void DeleteOccurrence(Store& store, const OccurrenceRef& ref, EditScope scope);

// Moves an occurrence to a new start keeping its length (drag and drop).
// allDay switches between timed and all-day.
std::string MoveOccurrence(Store& store, const OccurrenceRef& ref,
	Instant newStart, Instant newEnd, bool allDay, const Date& newDate,
	int newDays, EditScope scope);

// A copy as a new event (same calendar, no recurrence id).
std::string DuplicateEvent(Store& store, const OccurrenceRef& ref);

}	// namespace aircal
