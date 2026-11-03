// Merging remote state into the Store, shared by the providers. Callers hold
// the store's lock.
#pragma once

#include "core/Store.h"

#include <set>
#include <string>

namespace aircal {

// How far back events are fetched (seconds).
inline int64_t SyncWindowPast() { return 400 * 86400LL; }
// How far ahead CalDAV time-range queries reach.
inline int64_t SyncWindowFuture() { return 3 * 366 * 86400LL; }

// Inserts or replaces the local copy of a remote event (matched by remoteId,
// or for overrides also by master + recurrence id). Keeps the local id and
// leaves items with local changes alone. Returns the local event, or nullptr
// when skipped.
Event* UpsertRemoteEvent(Store& store, const std::string& calendarId,
	const Event& remote);
Task* UpsertRemoteTask(Store& store, const std::string& calendarId,
	const Task& remote);
// Removes synced (not pending) tasks of the list whose remote ids were not seen.
void RemoveStaleTasks(Store& store, const std::string& calendarId,
	const std::set<std::string>& seen);

}	// namespace aircal
