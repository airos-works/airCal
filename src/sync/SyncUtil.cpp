#include "SyncUtil.h"

namespace aircal {

Event*
UpsertRemoteEvent(Store& store, const std::string& calendarId,
	const Event& remote)
{
	Event* local = remote.remoteId.empty() ? nullptr
		: store.FindEventByRemote(calendarId, remote.remoteId);
	if (local == nullptr && remote.IsOverride()) {
		// A locally made override that the server now returns under its
		// own id.
		for (Event* candidate : store.OverridesOf(remote.masterId)) {
			bool same = remote.allDay
				? candidate->recurrenceDate == remote.recurrenceDate
				: candidate->recurrenceId == remote.recurrenceId;
			if (same) {
				local = candidate;
				break;
			}
		}
	}
	if (local == nullptr) {
		Event copy = remote;
		copy.id = NewId();
		copy.calendarId = calendarId;
		copy.pending = Pending::None;
		return &store.AddEvent(copy);
	}
	if (local->pending != Pending::None)
		return nullptr;
	std::string id = local->id;
	uint32_t revision = local->revision;
	*local = remote;
	local->id = id;
	local->calendarId = calendarId;
	local->revision = revision;
	local->pending = Pending::None;
	return local;
}


Task*
UpsertRemoteTask(Store& store, const std::string& calendarId, const Task& remote)
{
	Task* local = store.FindTaskByRemote(calendarId, remote.remoteId);
	if (local == nullptr) {
		Task copy = remote;
		copy.id = NewId();
		copy.calendarId = calendarId;
		copy.pending = Pending::None;
		if (copy.created == 0)
			copy.created = copy.updated;
		return &store.AddTask(copy);
	}
	if (local->pending != Pending::None)
		return nullptr;
	std::string id = local->id;
	std::string parent = local->parentId;
	uint32_t revision = local->revision;
	Instant created = local->created;
	std::vector<int> alarms = local->alarms;
	*local = remote;
	local->id = id;
	local->calendarId = calendarId;
	local->revision = revision;
	local->created = created != 0 ? created : remote.created;
	if (local->parentId.empty())
		local->parentId = parent;
	if (local->alarms.empty())
		local->alarms = alarms;	// local-only reminders survive
	local->pending = Pending::None;
	return local;
}


void
RemoveStaleTasks(Store& store, const std::string& calendarId,
	const std::set<std::string>& seen)
{
	std::vector<std::string> stale;
	for (Task* task : store.TasksIn(calendarId)) {
		if (!task->remoteId.empty() && task->pending == Pending::None
			&& seen.count(task->remoteId) == 0)
			stale.push_back(task->id);
	}
	for (auto& id : stale)
		store.RemoveTask(id);
}

}	// namespace aircal
