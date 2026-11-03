#include "Test.h"
#include "core/Editing.h"
#include "core/Undo.h"

#include <unistd.h>

using namespace aircal;

namespace {

std::string
TempDir()
{
	char path[] = "/tmp/aircal-undo-XXXXXX";
	return mkdtemp(path);
}


struct Fixture {
	Store store;
	UndoStack undo;
	std::string remoteCalendar;
	std::string remoteAccount;

	Fixture() : store(TempDir())
	{
		store.Load();
		Account account;
		account.type = AccountType::Google;
		account.name = "remote";
		remoteAccount = store.AddAccount(account).id;
		Calendar calendar;
		calendar.accountId = remoteAccount;
		calendar.name = "Work";
		calendar.events = true;
		calendar.tasks = true;
		remoteCalendar = store.AddCalendar(calendar).id;
	}

	Event* Synced(const std::string& title)
	{
		Event event;
		event.calendarId = remoteCalendar;
		event.title = title;
		event.start = ToInstant(DateTime(Date(2026, 11, 10), 9, 0));
		event.end = event.start + 3600;
		event.remoteId = "r-" + title;
		event.pending = Pending::None;
		return &store.AddEvent(event);
	}
};

}	// namespace


TEST(UndoMoveRestoresTimesAsPendingUpdate)
{
	Fixture f;
	Event* event = f.Synced("Review");
	std::string id = event->id;
	Instant start = event->start;
	{
		UndoScope scope(f.undo, f.store, "Move Event");
		OccurrenceRef ref;
		ref.eventId = id;
		MoveOccurrence(f.store, ref, start + 7200, start + 10800, false, Date(), 0,
			EditScope::All);
	}
	CHECK(f.undo.CanUndo());
	CHECK_EQ(f.undo.UndoName(), std::string("Move Event"));
	CHECK_EQ(f.store.FindEvent(id)->start, start + 7200);
	f.undo.Undo(f.store);
	CHECK_EQ(f.store.FindEvent(id)->start, start);
	CHECK(f.store.FindEvent(id)->pending == Pending::Update);
	CHECK_EQ(f.store.FindEvent(id)->remoteId, std::string("r-Review"));
	CHECK(f.undo.CanRedo());
	f.undo.Redo(f.store);
	CHECK_EQ(f.store.FindEvent(id)->start, start + 7200);
}


TEST(UndoDeleteBeforeSyncRevives)
{
	Fixture f;
	std::string id = f.Synced("Lunch")->id;
	{
		UndoScope scope(f.undo, f.store, "Delete Event");
		f.store.DeleteEvent(id);
	}
	CHECK(f.store.FindEvent(id)->pending == Pending::Delete);
	f.undo.Undo(f.store);
	CHECK(f.store.FindEvent(id) != nullptr);
	CHECK(f.store.FindEvent(id)->pending == Pending::Update);
	CHECK_EQ(f.store.FindEvent(id)->title, std::string("Lunch"));
}


TEST(UndoDeleteAfterSyncRecreates)
{
	Fixture f;
	std::string id = f.Synced("Dentist")->id;
	{
		UndoScope scope(f.undo, f.store, "Delete Event");
		f.store.DeleteEvent(id);
	}
	f.store.RemoveEvent(id);	// the sync sent the deletion
	f.undo.Undo(f.store);
	Event* back = f.store.FindEvent(id);
	CHECK(back != nullptr);
	CHECK(back != nullptr && back->pending == Pending::Create);
	CHECK(back != nullptr && back->remoteId.empty());
}


TEST(UndoNewEventDeletesIt)
{
	Fixture f;
	std::string id;
	{
		UndoScope scope(f.undo, f.store, "New Event");
		Event event;
		event.calendarId = f.remoteCalendar;
		event.title = "Made";
		event.start = Now();
		event.end = event.start + 600;
		OccurrenceRef none;
		id = SaveEvent(f.store, none, event, EditScope::All);
	}
	CHECK(f.store.FindEvent(id) != nullptr);
	f.undo.Undo(f.store);
	CHECK(f.store.FindEvent(id) == nullptr);	// never synced: simply gone
	f.undo.Redo(f.store);
	CHECK(f.store.FindEvent(id) != nullptr);
	CHECK(f.store.FindEvent(id)->pending == Pending::Create);
}


TEST(UndoTaskCompletionAndNestedScopes)
{
	Fixture f;
	Task task;
	task.calendarId = f.remoteCalendar;
	task.title = "Pay rent";
	task.remoteId = "t1";
	std::string id = f.store.AddTask(task).id;
	{
		UndoScope outer(f.undo, f.store, "Complete Task");
		UndoScope inner(f.undo, f.store, "inner");
		f.store.SetTaskCompleted(id, true);
	}
	CHECK_EQ(f.undo.UndoName(), std::string("Complete Task"));
	CHECK(f.store.FindTask(id)->completed);
	f.undo.Undo(f.store);
	CHECK(!f.store.FindTask(id)->completed);
	CHECK(f.store.FindTask(id)->pending == Pending::Update);
	CHECK(!f.undo.CanUndo());
}
