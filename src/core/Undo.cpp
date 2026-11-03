#include "Undo.h"

#include <algorithm>

namespace aircal {

static const size_t kMaxRecords = 50;


void
UndoStack::Begin(Store& store, const std::string& name)
{
	if (fDepth++ > 0)
		return;	// nested scopes belong to the outer action
	fName = name;
	fEvents.clear();
	fTasks.clear();
	for (auto& event : store.Events())
		fEvents[event->id] = *event;
	for (auto& task : store.Tasks())
		fTasks[task->id] = *task;
	fOpen = true;
}


void
UndoStack::Commit(Store& store)
{
	if (--fDepth > 0 || !fOpen)
		return;
	fOpen = false;
	Record record;
	record.name = fName;
	std::set<std::string> seen;
	for (auto& event : store.Events()) {
		seen.insert(event->id);
		auto before = fEvents.find(event->id);
		bool changed = before == fEvents.end()
			|| before->second.revision != event->revision
			|| before->second.pending != event->pending;
		if (!changed)
			continue;
		Change change;
		change.id = event->id;
		change.existsAfter = true;
		change.eventAfter = *event;
		if (before != fEvents.end()) {
			change.existedBefore = true;
			change.eventBefore = before->second;
		}
		record.changes.push_back(change);
	}
	for (auto& before : fEvents) {
		if (seen.count(before.first) != 0)
			continue;
		Change change;
		change.id = before.first;
		change.existedBefore = true;
		change.eventBefore = before.second;
		record.changes.push_back(change);
	}
	seen.clear();
	for (auto& task : store.Tasks()) {
		seen.insert(task->id);
		auto before = fTasks.find(task->id);
		bool changed = before == fTasks.end()
			|| before->second.revision != task->revision
			|| before->second.pending != task->pending;
		if (!changed)
			continue;
		Change change;
		change.isTask = true;
		change.id = task->id;
		change.existsAfter = true;
		change.taskAfter = *task;
		if (before != fTasks.end()) {
			change.existedBefore = true;
			change.taskBefore = before->second;
		}
		record.changes.push_back(change);
	}
	for (auto& before : fTasks) {
		if (seen.count(before.first) != 0)
			continue;
		Change change;
		change.isTask = true;
		change.id = before.first;
		change.existedBefore = true;
		change.taskBefore = before.second;
		record.changes.push_back(change);
	}
	fEvents.clear();
	fTasks.clear();
	if (record.changes.empty() || fApplying)
		return;
	fUndo.push_back(record);
	if (fUndo.size() > kMaxRecords)
		fUndo.pop_front();
	fRedo.clear();
}


// RestoreEvent/RestoreTask put an item back into a recorded state (or
// remove it), as a local edit that syncs.
static void
RestoreEvent(Store& store, const std::string& id, const Event* target,
	std::set<std::string>& calendars)
{
	Event* current = store.FindEvent(id);
	if (target == nullptr || target->pending == Pending::Delete) {
		if (current != nullptr) {
			calendars.insert(current->calendarId);
			store.DeleteEvent(id);
		}
		return;
	}
	calendars.insert(target->calendarId);
	Event copy = *target;
	if (current != nullptr) {
		calendars.insert(current->calendarId);
		copy.remoteId = current->remoteId;
		copy.etag = current->etag;
		copy.raw = current->raw;
		copy.revision = current->revision + 1;
		copy.pending = copy.remoteId.empty() ? Pending::Create : Pending::Update;
		if (target->pending == Pending::None && current->pending == Pending::Delete
			&& current->remoteId == target->remoteId)
			copy.pending = Pending::Update;	// deletion not sent: just revive
		*current = copy;
	} else {
		// Gone from the server too: comes back as a new item.
		copy.remoteId.clear();
		copy.etag.clear();
		copy.raw.clear();
		copy.revision++;
		copy.pending = Pending::Create;
		store.AddEvent(copy);
	}
}


static void
RestoreTask(Store& store, const std::string& id, const Task* target,
	std::set<std::string>& calendars)
{
	Task* current = store.FindTask(id);
	if (target == nullptr || target->pending == Pending::Delete) {
		if (current != nullptr) {
			calendars.insert(current->calendarId);
			store.DeleteTask(id);
		}
		return;
	}
	calendars.insert(target->calendarId);
	Task copy = *target;
	if (current != nullptr) {
		calendars.insert(current->calendarId);
		copy.remoteId = current->remoteId;
		copy.etag = current->etag;
		copy.raw = current->raw;
		copy.revision = current->revision + 1;
		copy.pending = copy.remoteId.empty() ? Pending::Create : Pending::Update;
		*current = copy;
	} else {
		copy.remoteId.clear();
		copy.etag.clear();
		copy.raw.clear();
		copy.revision++;
		copy.pending = Pending::Create;
		store.AddTask(copy);
	}
}


std::set<std::string>
UndoStack::Apply(Store& store, const Record& record, bool backwards,
	Record& inverse)
{
	std::set<std::string> calendars;
	std::lock_guard<std::recursive_mutex> lock(store.Mutex());
	fApplying = true;
	// Masters before overrides and parents before subtasks when bringing
	// things back.
	std::vector<const Change*> order;
	for (const Change& change : record.changes)
		order.push_back(&change);
	std::stable_sort(order.begin(), order.end(), [backwards](const Change* a,
			const Change* b) {
		const bool aChild = a->isTask ? !(backwards ? a->taskBefore : a->taskAfter)
			.parentId.empty() : (backwards ? a->eventBefore : a->eventAfter).IsOverride();
		const bool bChild = b->isTask ? !(backwards ? b->taskBefore : b->taskAfter)
			.parentId.empty() : (backwards ? b->eventBefore : b->eventAfter).IsOverride();
		return !aChild && bChild;
	});
	for (const Change* change : order) {
		bool exists = backwards ? change->existedBefore : change->existsAfter;
		if (change->isTask)
			RestoreTask(store, change->id, exists
				? (backwards ? &change->taskBefore : &change->taskAfter) : nullptr,
				calendars);
		else
			RestoreEvent(store, change->id, exists
				? (backwards ? &change->eventBefore : &change->eventAfter) : nullptr,
				calendars);
	}
	fApplying = false;
	inverse.name = record.name;
	store.Changed();
	return calendars;
}


std::set<std::string>
UndoStack::Undo(Store& store)
{
	if (fUndo.empty())
		return {};
	Record record = fUndo.back();
	fUndo.pop_back();
	Record inverse;
	std::set<std::string> calendars = Apply(store, record, true, inverse);
	fRedo.push_back(record);
	return calendars;
}


std::set<std::string>
UndoStack::Redo(Store& store)
{
	if (fRedo.empty())
		return {};
	Record record = fRedo.back();
	fRedo.pop_back();
	Record inverse;
	std::set<std::string> calendars = Apply(store, record, false, inverse);
	fUndo.push_back(record);
	return calendars;
}

}	// namespace aircal
