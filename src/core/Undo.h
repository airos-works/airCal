// Undo and redo of local edits. A scope around each user action records
// which events and tasks it changed; undoing applies the old versions as new
// local edits, so they sync like any other change (an event deleted on the
// server comes back as a new one).
#pragma once

#include "Store.h"

#include <deque>
#include <memory>
#include <set>
#include <string>

namespace aircal {

class UndoStack {
public:
	struct Change {
		bool isTask = false;
		std::string id;
		bool existedBefore = false;
		bool existsAfter = false;
		Event eventBefore, eventAfter;
		Task taskBefore, taskAfter;
	};
	struct Record {
		std::string name;
		std::vector<Change> changes;
	};

	// Store lock must be held from Begin to Commit.
	void Begin(Store& store, const std::string& name);
	void Commit(Store& store);

	bool CanUndo() const { return !fUndo.empty(); }
	bool CanRedo() const { return !fRedo.empty(); }
	std::string UndoName() const { return fUndo.empty() ? "" : fUndo.back().name; }
	std::string RedoName() const { return fRedo.empty() ? "" : fRedo.back().name; }

	// Return the calendars that changed (to push them).
	std::set<std::string> Undo(Store& store);
	std::set<std::string> Redo(Store& store);

private:
	struct Mark {
		uint32_t revision;
		Pending pending;
	};
	std::set<std::string> Apply(Store& store, const Record& record, bool backwards,
		Record& inverse);

	bool fOpen = false;
	int fDepth = 0;
	std::string fName;
	std::map<std::string, Event> fEvents;
	std::map<std::string, Task> fTasks;
	std::deque<Record> fUndo;
	std::deque<Record> fRedo;
	bool fApplying = false;
};

// RAII: locks the store, snapshots, commits the record when it ends.
class UndoScope {
public:
	UndoScope(UndoStack& stack, Store& store, const std::string& name)
		: fStack(stack), fStore(store), fLock(store.Mutex())
	{
		fStack.Begin(fStore, name);
	}
	~UndoScope() { fStack.Commit(fStore); }

private:
	UndoStack& fStack;
	Store& fStore;
	std::lock_guard<std::recursive_mutex> fLock;
};

}	// namespace aircal
