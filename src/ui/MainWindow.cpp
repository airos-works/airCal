#include "MainWindow.h"

#include "App.h"
#include "Dialogs.h"
#include "Format.h"
#include "HeaderView.h"
#include "Messages.h"
#include "SidebarView.h"
#include "Theme.h"
#include "Views.h"
#include "core/ICal.h"
#include "core/QuickEntry.h"

#include <Alert.h>
#include <CardLayout.h>
#include <File.h>
#include <FilePanel.h>
#include <GroupLayout.h>
#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <Screen.h>
#include <SplitView.h>

#include <cstdio>
#include <cstdlib>

using namespace aircal;

enum {
	kMsgEditSelection = 'edsl',
	kMsgContextEdit = 'cxed',
	kMsgContextDelete = 'cxdl',
	kMsgContextDuplicate = 'cxdp',
	kMsgContextComplete = 'cxcp',
	kMsgContextCalendar = 'cxcl',
	kMsgContextDue = 'cxdu',
	kMsgExportRef = 'exrf',
	kMsgFocusSearch = 'fsrh',
	kMsgFocusEntry = 'fent'
};


// --- CalendarView ---------------------------------------------------------------------

CalendarView::CalendarView(const char* name, uint32 flags)
	:
	BView(name, flags | B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_FRAME_EVENTS),
	fAnchor(Today())
{
}


void
CalendarView::SetAnchor(const Date& date)
{
	fAnchor = date;
	Reload();
}


// --- MainWindow ------------------------------------------------------------------------

MainWindow::MainWindow()
	:
	BWindow(BRect(80, 60, 1260, 840), "airCal", B_TITLED_WINDOW,
		B_ASYNCHRONOUS_CONTROLS | B_QUIT_ON_WINDOW_CLOSE
			| B_AUTO_UPDATE_SIZE_LIMITS)
{
	Store& store = TheApp()->Store();
	std::string frame;
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		fMode = store.Prefs().view;
		frame = store.Prefs().windowFrame;
		fSidebarWidth = store.Prefs().sidebarWidth;
	}
	if (fMode != "day" && fMode != "week" && fMode != "month" && fMode != "year"
		&& fMode != "tasks")
		fMode = "week";
	fDate = Today();
	const char* start = getenv("AIRCAL_START_DATE");
	Date requested;
	if (start != nullptr && Date::FromIso(start, requested))
		fDate = requested;

	BMenuBar* bar = new BMenuBar("menu");
	BuildMenus(bar);

	fHeader = new HeaderView();
	fSidebar = new SidebarView();
	fDayView = CreateDayWeekView(1);
	fWeekView = CreateDayWeekView(7);
	fMonthView = CreateMonthView();
	fYearView = CreateYearView();
	fTasksView = CreateTasksView();

	BView* cards = new BView("cards", 0);
	fCards = new BCardLayout();
	cards->SetLayout(fCards);
	fCards->AddView(fDayView);
	fCards->AddView(fWeekView);
	fCards->AddView(fMonthView);
	fCards->AddView(fYearView);
	fCards->AddView(fTasksView);

	BGroupView* main = new BGroupView(B_VERTICAL, 0);
	BLayoutBuilder::Group<>(main)
		.Add(fHeader)
		.Add(cards, 1.0f);

	fSplit = new BSplitView(B_HORIZONTAL, 0);
	BLayoutBuilder::Split<>(fSplit)
		.Add(fSidebar, 0.0f)
		.Add(main, 1.0f);
	fSplit->SetCollapsible(0, true);
	fSplit->SetCollapsible(1, false);

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(bar)
		.Add(fSplit, 1.0f);

	SetSizeLimits(720, 32768, 480, 32768);
	float l, t, r, b;
	if (sscanf(frame.c_str(), "%f %f %f %f", &l, &t, &r, &b) == 4
		&& r - l >= 600 && b - t >= 400) {
		BRect saved(l, t, r, b);
		BRect screen = BScreen(this).Frame();
		if (screen.Intersects(saved)) {
			MoveTo(saved.LeftTop());
			ResizeTo(saved.Width(), saved.Height());
		}
	} else {
		BRect screen = BScreen(this).Frame();
		float width = std::min(1280.0f, screen.Width() - 80);
		float height = std::min(840.0f, screen.Height() - 80);
		ResizeTo(width, height);
		CenterOnScreen();
	}
	fSplit->SetItemWeight(0, 0.0f, true);
	fSidebar->SetExplicitPreferredSize(BSize(fSidebarWidth, B_SIZE_UNSET));

	SetViewMode(fMode);
	GoTo(fDate);
	SetPulseRate(50000);
}


MainWindow::~MainWindow()
{
}


void
MainWindow::BuildMenus(BMenuBar* bar)
{
	BMenu* app = new BMenu("airCal");
	app->AddItem(new BMenuItem("About airCal", new BMessage(B_ABOUT_REQUESTED)));
	app->AddSeparatorItem();
	app->AddItem(new BMenuItem("Settings…", new BMessage(kMsgShowSettings), ','));
	app->AddItem(new BMenuItem("Accounts…", new BMessage(kMsgShowAccounts)));
	app->AddSeparatorItem();
	app->AddItem(new BMenuItem("Quit", new BMessage(B_QUIT_REQUESTED), 'Q'));
	bar->AddItem(app);

	BMenu* file = new BMenu("File");
	file->AddItem(new BMenuItem("New Event", new BMessage(kMsgNewEvent), 'N'));
	file->AddItem(new BMenuItem("New Task", new BMessage(kMsgNewTask), 'N',
		B_SHIFT_KEY));
	file->AddItem(new BMenuItem("Quick Entry", new BMessage(kMsgFocusEntry), 'K'));
	file->AddSeparatorItem();
	file->AddItem(new BMenuItem("Import Calendar File…", new BMessage(kMsgImport),
		'O'));
	file->AddItem(new BMenuItem("Export Visible Events…", new BMessage(kMsgExport),
		'E', B_SHIFT_KEY));
	bar->AddItem(file);

	BMenu* edit = new BMenu("Edit");
	edit->AddItem(new BMenuItem("Cut", new BMessage(B_CUT), 'X'));
	edit->AddItem(new BMenuItem("Copy", new BMessage(B_COPY), 'C'));
	edit->AddItem(new BMenuItem("Paste", new BMessage(B_PASTE), 'V'));
	edit->AddItem(new BMenuItem("Select All", new BMessage(B_SELECT_ALL), 'A'));
	edit->AddSeparatorItem();
	edit->AddItem(new BMenuItem("Edit Selection", new BMessage(kMsgEditSelection),
		B_ENTER));
	edit->AddItem(new BMenuItem("Duplicate", new BMessage(kMsgDuplicateEvent), 'D'));
	edit->AddItem(new BMenuItem("Delete", new BMessage(kMsgDeleteSelection),
		B_DELETE));
	edit->AddSeparatorItem();
	edit->AddItem(new BMenuItem("Find…", new BMessage(kMsgFocusSearch), 'F'));
	bar->AddItem(edit);

	BMenu* view = new BMenu("View");
	const char* names[] = {"Day", "Week", "Month", "Year", "Tasks"};
	const char* modes[] = {"day", "week", "month", "year", "tasks"};
	for (int i = 0; i < 5; i++) {
		BMessage* message = new BMessage(kMsgSetView);
		message->AddString("view", modes[i]);
		fViewItems[i] = new BMenuItem(names[i], message, '1' + i);
		view->AddItem(fViewItems[i]);
	}
	view->AddSeparatorItem();
	view->AddItem(new BMenuItem("Go to Today", new BMessage(kMsgGoToday), 'T'));
	view->AddItem(new BMenuItem("Next", new BMessage(kMsgGoNext), B_RIGHT_ARROW));
	view->AddItem(new BMenuItem("Previous", new BMessage(kMsgGoPrevious),
		B_LEFT_ARROW));
	view->AddSeparatorItem();
	fShowSidebarItem = new BMenuItem("Show Sidebar", new BMessage(kMsgToggleSidebar),
		'S', B_OPTION_KEY);
	view->AddItem(fShowSidebarItem);
	fShowCompletedItem = new BMenuItem("Show Completed Tasks",
		new BMessage(kMsgShowCompleted));
	view->AddItem(fShowCompletedItem);
	bar->AddItem(view);

	BMenu* calendar = new BMenu("Calendar");
	calendar->AddItem(new BMenuItem("Sync Now", new BMessage(kMsgSyncNow), 'R'));
	BMessage* full = new BMessage(kMsgSyncNow);
	full->AddBool("full", true);
	calendar->AddItem(new BMenuItem("Refresh All Calendars", full, 'R',
		B_SHIFT_KEY));
	calendar->AddSeparatorItem();
	calendar->AddItem(new BMenuItem("Add Google Account…",
		new BMessage('adgo')));
	calendar->AddItem(new BMenuItem("Add CalDAV Account…",
		new BMessage('adcd')));
	calendar->AddItem(new BMenuItem("Add Todoist Account…",
		new BMessage('adtd')));
	calendar->AddItem(new BMenuItem("Accounts…", new BMessage(kMsgShowAccounts)));
	bar->AddItem(calendar);
}


void
MainWindow::MenusBeginning()
{
	bool showCompleted;
	{
		std::lock_guard<std::recursive_mutex> lock(TheApp()->Store().Mutex());
		showCompleted = TheApp()->Store().Prefs().showCompleted;
	}
	fShowCompletedItem->SetMarked(showCompleted);
	fShowSidebarItem->SetMarked(!fSidebarHidden);
	const char* modes[] = {"day", "week", "month", "year", "tasks"};
	for (int i = 0; i < 5; i++)
		fViewItems[i]->SetMarked(fMode == modes[i]);
}


CalendarView*
MainWindow::CurrentView() const
{
	if (fMode == "day")
		return fDayView;
	if (fMode == "month")
		return fMonthView;
	if (fMode == "year")
		return fYearView;
	if (fMode == "tasks")
		return fTasksView;
	return fWeekView;
}


void
MainWindow::SetViewMode(const std::string& mode)
{
	if (mode != "day" && mode != "week" && mode != "month" && mode != "year"
		&& mode != "tasks")
		return;
	fMode = mode;
	CalendarView* view = CurrentView();
	fCards->SetVisibleItem(fCards->IndexOfView(view));
	view->SetAnchor(fDate);
	fHeader->SetMode(mode);
	UpdateHeader();
	view->MakeFocus(true);
	{
		std::lock_guard<std::recursive_mutex> lock(TheApp()->Store().Mutex());
		TheApp()->Store().Prefs().view = mode;
	}
}


void
MainWindow::GoTo(const Date& date)
{
	fDate = date;
	CurrentView()->SetAnchor(date);
	fSidebar->SetSelectedDate(date);
	UpdateHeader();
}


void
MainWindow::SelectDate(const Date& date)
{
	fSidebar->SetSelectedDate(date);
}


void
MainWindow::UpdateHeader()
{
	std::string bold, light;
	CurrentView()->Title(bold, light);
	fHeader->SetTitle(bold, light);
	SetTitle(("airCal — " + bold + (light.empty() ? "" : " " + light)).c_str());
}


void
MainWindow::ReloadAll()
{
	fReloadPending = false;
	CurrentView()->Reload();
	fSidebar->Reload();
	// Other views reload when shown.
}


void
MainWindow::SetSelection(const Selection& selection)
{
	fSelection = selection;
	CurrentView()->Invalidate();
	fSidebar->Reload();
}


void
MainWindow::SelectEvent(const DisplayEvent& event)
{
	Selection selection;
	selection.kind = Selection::EventItem;
	selection.ref.eventId = event.eventId;
	selection.ref.overrideId = event.overrideId;
	selection.ref.originalStart = event.originalStart;
	selection.ref.originalDate = event.originalDate;
	SetSelection(selection);
}


void
MainWindow::SelectTask(const std::string& taskId)
{
	Selection selection;
	selection.kind = Selection::TaskItem;
	selection.taskId = taskId;
	SetSelection(selection);
}


void
MainWindow::EditEvent(const DisplayEvent& event, BPoint screenPoint)
{
	SelectEvent(event);
	OccurrenceRef ref = fSelection.ref;
	Event draft;
	OpenEventEditor(ref, draft, screenPoint.x, screenPoint.y);
}


void
MainWindow::EditTask(const std::string& taskId, BPoint screenPoint)
{
	SelectTask(taskId);
	Task draft;
	OpenTaskEditor(taskId, draft, screenPoint.x, screenPoint.y);
}


void
MainWindow::NewEventAt(Instant start, Instant end, bool allDay, const Date& date,
	BPoint screenPoint, const std::string& title)
{
	Store& store = TheApp()->Store();
	Event draft;
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		Calendar* calendar = store.DefaultCalendar(false);
		if (calendar == nullptr) {
			BAlert* alert = new BAlert("airCal", "There is no calendar you can "
				"add events to. Add an account or turn one on in Accounts.",
				"OK");
			alert->Go(nullptr);
			return;
		}
		draft.calendarId = calendar->id;
		draft.alarms = calendar->defaultAlarms;
		if (draft.alarms.empty() && store.Prefs().defaultAlarm >= 0)
			draft.alarms.push_back(store.Prefs().defaultAlarm);
	}
	draft.title = title;
	draft.allDay = allDay;
	if (allDay) {
		draft.startDate = date;
		draft.endDate = date.AddDays(1);
	} else {
		draft.start = start;
		draft.end = end > start ? end : start + 3600;
	}
	OccurrenceRef none;
	OpenEventEditor(none, draft, screenPoint.x, screenPoint.y);
}


void
MainWindow::NewTask(const Date* due, BPoint screenPoint)
{
	Store& store = TheApp()->Store();
	Task draft;
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		Calendar* list = store.DefaultCalendar(true);
		if (list == nullptr) {
			BAlert* alert = new BAlert("airCal", "There is no task list you can "
				"add tasks to. Add a Todoist or Google account, or turn one on in "
				"Accounts.", "OK");
			alert->Go(nullptr);
			return;
		}
		draft.calendarId = list->id;
	}
	if (due != nullptr) {
		draft.hasDue = true;
		draft.dueDate = *due;
	}
	OpenTaskEditor("", draft, screenPoint.x, screenPoint.y);
}


void
MainWindow::ToggleTask(const std::string& taskId, bool completed)
{
	Store& store = TheApp()->Store();
	std::string calendar;
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		Task* task = store.FindTask(taskId);
		if (task == nullptr)
			return;
		Calendar* list = store.FindCalendar(task->calendarId);
		if (list != nullptr && list->readOnly)
			return;
		calendar = task->calendarId;
		store.SetTaskCompleted(taskId, completed);
	}
	TheApp()->RequestPush(calendar);
	TheApp()->ScheduleSave();
}


void
MainWindow::MoveEvent(const DisplayEvent& event, Instant start, Instant end,
	bool allDay, const Date& date, int days)
{
	if (event.readOnly)
		return;
	OccurrenceRef ref;
	ref.eventId = event.eventId;
	ref.overrideId = event.overrideId;
	ref.originalStart = event.originalStart;
	ref.originalDate = event.originalDate;
	Store& store = TheApp()->Store();
	EditScope scope = EditScope::All;
	if (IsRecurringOccurrence(store, ref)) {
		if (!AskEditScope(this, false, scope))
			return;
	}
	MoveOccurrence(store, ref, start, end, allDay, date, days, scope);
	TheApp()->RequestPush(event.calendarId);
	TheApp()->ScheduleSave();
}


void
MainWindow::DeleteEvent(const DisplayEvent& event)
{
	if (event.readOnly)
		return;
	OccurrenceRef ref;
	ref.eventId = event.eventId;
	ref.overrideId = event.overrideId;
	ref.originalStart = event.originalStart;
	ref.originalDate = event.originalDate;
	Store& store = TheApp()->Store();
	EditScope scope = EditScope::All;
	if (IsRecurringOccurrence(store, ref)) {
		if (!AskEditScope(this, true, scope))
			return;
	}
	DeleteOccurrence(store, ref, scope);
	fSelection = Selection();
	TheApp()->RequestPush(event.calendarId);
	TheApp()->ScheduleSave();
}


void
MainWindow::DeleteSelection()
{
	Store& store = TheApp()->Store();
	if (fSelection.kind == Selection::TaskItem) {
		std::string calendar;
		{
			std::lock_guard<std::recursive_mutex> lock(store.Mutex());
			Task* task = store.FindTask(fSelection.taskId);
			if (task == nullptr)
				return;
			Calendar* list = store.FindCalendar(task->calendarId);
			if (list != nullptr && list->readOnly)
				return;
			calendar = task->calendarId;
		}
		store.DeleteTask(fSelection.taskId);
		fSelection = Selection();
		TheApp()->RequestPush(calendar);
		TheApp()->ScheduleSave();
		return;
	}
	if (fSelection.kind != Selection::EventItem)
		return;
	DisplayEvent event;
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		Event* master = store.FindEvent(fSelection.ref.eventId);
		if (master == nullptr)
			return;
		Calendar* calendar = store.FindCalendar(master->calendarId);
		event.eventId = master->id;
		event.overrideId = fSelection.ref.overrideId;
		event.originalStart = fSelection.ref.originalStart;
		event.originalDate = fSelection.ref.originalDate;
		event.calendarId = master->calendarId;
		event.readOnly = calendar == nullptr || calendar->readOnly;
	}
	DeleteEvent(event);
}


void
MainWindow::ShowEventMenu(const DisplayEvent& event, BPoint screenPoint)
{
	SelectEvent(event);
	BPopUpMenu* menu = new BPopUpMenu("event", false, false);
	menu->AddItem(new BMenuItem("Edit…", new BMessage(kMsgContextEdit)));
	menu->AddItem(new BMenuItem("Duplicate", new BMessage(kMsgContextDuplicate)));
	if (!event.readOnly) {
		BMenu* move = new BMenu("Move to Calendar");
		Store& store = TheApp()->Store();
		{
			std::lock_guard<std::recursive_mutex> lock(store.Mutex());
			for (auto& account : store.Accounts()) {
				if (!account->enabled)
					continue;
				for (Calendar* calendar : store.CalendarsOf(account->id)) {
					if (!calendar->events || calendar->readOnly)
						continue;
					BMessage* message = new BMessage(kMsgContextCalendar);
					message->AddString("calendar", calendar->id.c_str());
					BMenuItem* item = new BMenuItem((calendar->name + " — "
						+ account->name).c_str(), message);
					item->SetMarked(calendar->id == event.calendarId);
					move->AddItem(item);
				}
			}
		}
		menu->AddItem(move);
		menu->AddSeparatorItem();
		menu->AddItem(new BMenuItem("Delete", new BMessage(kMsgContextDelete)));
	}
	menu->SetTargetForItems(this);
	for (int32 i = 0; i < menu->CountItems(); i++) {
		BMenu* sub = menu->SubmenuAt(i);
		if (sub != nullptr)
			sub->SetTargetForItems(this);
	}
	menu->SetAsyncAutoDestruct(true);
	menu->Go(screenPoint, true, true, true);
}


void
MainWindow::ShowTaskMenu(const std::string& taskId, BPoint screenPoint)
{
	SelectTask(taskId);
	bool completed = false;
	{
		std::lock_guard<std::recursive_mutex> lock(TheApp()->Store().Mutex());
		Task* task = TheApp()->Store().FindTask(taskId);
		if (task == nullptr)
			return;
		completed = task->completed;
	}
	BPopUpMenu* menu = new BPopUpMenu("task", false, false);
	menu->AddItem(new BMenuItem("Edit…", new BMessage(kMsgContextEdit)));
	menu->AddItem(new BMenuItem(completed ? "Mark as Not Completed"
		: "Mark as Completed", new BMessage(kMsgContextComplete)));
	BMenu* due = new BMenu("Due");
	const char* labels[] = {"Today", "Tomorrow", "Next Week", "No Date"};
	for (int i = 0; i < 4; i++) {
		BMessage* message = new BMessage(kMsgContextDue);
		message->AddInt32("which", i);
		due->AddItem(new BMenuItem(labels[i], message));
	}
	due->SetTargetForItems(this);
	menu->AddItem(due);
	menu->AddSeparatorItem();
	menu->AddItem(new BMenuItem("Delete", new BMessage(kMsgContextDelete)));
	menu->SetTargetForItems(this);
	menu->SetAsyncAutoDestruct(true);
	menu->Go(screenPoint, true, true, true);
}


void
MainWindow::QuickEntry(const std::string& text)
{
	Store& store = TheApp()->Store();
	int duration, weekStart;
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		duration = store.Prefs().defaultDuration;
		weekStart = store.Prefs().weekStart;
	}
	QuickEntryResult result = ParseQuickEntry(text, Now(), duration, weekStart);
	std::string calendarId;
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		Calendar* target = nullptr;
		if (!result.calendarHint.empty()) {
			std::string hint = result.calendarHint;
			for (char& c : hint)
				c = (char)tolower((unsigned char)c);
			for (auto& calendar : store.Calendars()) {
				if (calendar->readOnly
					|| (result.isTask ? !calendar->tasks : !calendar->events))
					continue;
				std::string name = calendar->name;
				for (char& c : name)
					c = (char)tolower((unsigned char)c);
				if (name.compare(0, hint.size(), hint) == 0) {
					target = calendar.get();
					break;
				}
			}
		}
		if (target == nullptr)
			target = store.DefaultCalendar(result.isTask);
		if (target == nullptr) {
			BAlert* alert = new BAlert("airCal", result.isTask
				? "There is no task list to add this to."
				: "There is no calendar to add this to.", "OK");
			alert->Go(nullptr);
			return;
		}
		calendarId = target->id;
		Account* account = store.AccountOf(*target);
		bool todoist = account != nullptr && account->type == AccountType::Todoist;

		if (result.isTask) {
			Task task;
			task.calendarId = calendarId;
			task.title = result.title;
			task.priority = result.priority;
			task.labels = result.labels;
			task.created = Now();
			if (result.hasDate || result.hasTime) {
				task.hasDue = true;
				task.dueDate = result.startDate;
				if (result.hasTime) {
					task.dueHasTime = true;
					task.dueTime = result.start;
					task.dueDate = LocalDate(result.start);
				}
			}
			if (!result.rrule.empty()) {
				task.recurring = true;
				if (todoist)
					task.dueString = result.dueString;
				else
					task.rrule = result.rrule;
				if (!task.hasDue) {
					task.hasDue = true;
					task.dueDate = result.startDate;
				}
			}
			if (result.alarmMinutes >= 0)
				task.alarms.push_back(result.alarmMinutes);
			store.UpdateTask(task);
			for (auto& item : store.Tasks()) {
				if (item->title == task.title && item->calendarId == calendarId
					&& item->pending == Pending::Create)
					SelectTask(item->id);
			}
		} else {
			Event event;
			event.calendarId = calendarId;
			event.title = result.title;
			event.location = result.location;
			event.allDay = result.allDay;
			if (result.allDay) {
				event.startDate = result.startDate;
				event.endDate = result.endDate.AddDays(1);
				if (event.endDate <= event.startDate)
					event.endDate = event.startDate.AddDays(1);
			} else {
				event.start = result.start;
				event.end = result.end;
			}
			event.rrule = result.rrule;
			if (result.alarmMinutes >= 0)
				event.alarms.push_back(result.alarmMinutes);
			else {
				event.alarms = target->defaultAlarms;
				if (event.alarms.empty() && store.Prefs().defaultAlarm >= 0)
					event.alarms.push_back(store.Prefs().defaultAlarm);
			}
			OccurrenceRef none;
			std::string id = SaveEvent(store, none, event, EditScope::All);
			Selection selection;
			selection.kind = Selection::EventItem;
			selection.ref.eventId = id;
			selection.ref.originalStart = event.start;
			selection.ref.originalDate = event.startDate;
			fSelection = selection;
		}
	}
	TheApp()->RequestPush(calendarId);
	TheApp()->ScheduleSave();
	if (!result.isTask && fMode != "tasks") {
		Date date = result.allDay ? result.startDate : LocalDate(result.start);
		GoTo(date);
	}
}


void
MainWindow::UpdateSyncStatus(BMessage* message)
{
	std::string account = message->GetString("account", "");
	bool running = message->GetBool("running", false);
	std::string error = message->GetString("error", "");
	std::string progress = message->GetString("message", "");
	if (running) {
		fSyncRunning = 1;
	} else {
		fSyncRunning = 0;
		if (error.empty())
			fSyncErrors.erase(account);
		else
			fSyncErrors[account] = error;
	}
	std::string text;
	bool failed = !fSyncErrors.empty();
	if (fSyncRunning) {
		text = progress.empty() ? "Syncing…" : "Syncing " + progress + "…";
	} else if (failed) {
		std::string name;
		{
			std::lock_guard<std::recursive_mutex> lock(TheApp()->Store().Mutex());
			Account* item = TheApp()->Store().FindAccount(fSyncErrors.begin()->first);
			if (item != nullptr)
				name = item->name;
		}
		text = (name.empty() ? "" : name + ": ") + fSyncErrors.begin()->second;
	} else
		text = "Synced " + Format::Time(Now());
	fSidebar->SetStatus(text, failed && !fSyncRunning);
	fHeader->SetSync(fSyncRunning != 0, failed, text);
}


void
MainWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgStoreChanged:
			if (!fReloadPending) {
				fReloadPending = true;
				// Coalesce bursts (sync applies many changes).
				BMessage reload('rlda');
				PostMessage(&reload);
			}
			break;
		case 'rlda':
			ReloadAll();
			break;
		case kMsgSyncStatus:
			UpdateSyncStatus(message);
			break;
		case kMsgSetView:
		{
			const char* view;
			if (message->FindString("view", &view) == B_OK)
				SetViewMode(view);
			break;
		}
		case kMsgGoToday:
			GoTo(Today());
			CurrentView()->ScrollToNow();
			break;
		case kMsgGoNext:
			GoTo(CurrentView()->Step(1));
			break;
		case kMsgGoPrevious:
			GoTo(CurrentView()->Step(-1));
			break;
		case kMsgGoToDate:
		{
			const char* view;
			if (message->FindString("view", &view) == B_OK)
				SetViewMode(view);
			const char* text;
			Date date;
			if (message->FindString("date", &text) == B_OK && Date::FromIso(text, date))
				GoTo(date);
			break;
		}
		case kMsgSelectDate:
		{
			const char* text;
			Date date;
			if (message->FindString("date", &text) == B_OK && Date::FromIso(text, date)) {
				if (fMode == "tasks")
					SetViewMode("day");
				GoTo(date);
			}
			break;
		}
		case kMsgToggleSidebar:
			fSidebarHidden = !fSidebarHidden;
			if (fSidebarHidden)
				fSidebar->Hide();
			else
				fSidebar->Show();
			break;
		case kMsgSearchChanged:
			fSearch = message->GetString("text", "");
			fSidebar->SetSearch(fSearch);
			if (!fSearch.empty() && fSidebarHidden) {
				fSidebarHidden = false;
				fSidebar->Show();
			}
			break;
		case kMsgFocusSearch:
			fHeader->FocusSearch();
			break;
		case kMsgFocusEntry:
			if (fSidebarHidden) {
				fSidebarHidden = false;
				fSidebar->Show();
			}
			fSidebar->FocusQuickEntry();
			break;
		case kMsgShowCompleted:
		{
			std::lock_guard<std::recursive_mutex> lock(TheApp()->Store().Mutex());
			TheApp()->Store().Prefs().showCompleted
				= !TheApp()->Store().Prefs().showCompleted;
			TheApp()->ScheduleSave();
			ReloadAll();
			break;
		}
		case kMsgNewEvent:
		{
			Instant start = 0, end = 0;
			bool allDay = message->GetBool("allDay", false);
			const char* title = message->GetString("title", "");
			if (message->FindInt64("start", &start) != B_OK) {
				// The next round hour on the shown date.
				int duration;
				{
					std::lock_guard<std::recursive_mutex> lock(
						TheApp()->Store().Mutex());
					duration = TheApp()->Store().Prefs().defaultDuration;
				}
				DateTime now = ToWall(Now());
				DateTime wall(fDate, now.hour + 1, 0);
				if (wall.hour > 23)
					wall = DateTime(fDate, 9, 0);
				start = ToInstant(wall);
				end = start + duration * 60;
			} else if (message->FindInt64("end", &end) != B_OK)
				end = start + 3600;
			NewEventAt(start, end, allDay, fDate, BPoint(-1, -1), title);
			break;
		}
		case kMsgNewTask:
		{
			const char* text;
			Date due;
			if (message->FindString("date", &text) == B_OK && Date::FromIso(text, due))
				NewTask(&due, BPoint(-1, -1));
			else
				NewTask(nullptr, BPoint(-1, -1));
			break;
		}
		case kMsgQuickEntry:
		{
			const char* text;
			if (message->FindString("text", &text) == B_OK && text[0] != '\0')
				QuickEntry(text);
			break;
		}
		case kMsgToggleTask:
		{
			const char* id;
			if (message->FindString("task", &id) == B_OK)
				ToggleTask(id, message->GetBool("completed", true));
			break;
		}
		case kMsgEditSelection:
		case kMsgContextEdit:
			if (fSelection.kind == Selection::TaskItem)
				EditTask(fSelection.taskId, BPoint(-1, -1));
			else if (fSelection.kind == Selection::EventItem) {
				Event draft;
				OpenEventEditor(fSelection.ref, draft);
			}
			break;
		case kMsgDeleteSelection:
		case kMsgContextDelete:
			DeleteSelection();
			break;
		case kMsgDuplicateEvent:
		case kMsgContextDuplicate:
			if (fSelection.kind == Selection::EventItem) {
				std::string calendar;
				std::string id = DuplicateEvent(TheApp()->Store(), fSelection.ref);
				{
					std::lock_guard<std::recursive_mutex> lock(
						TheApp()->Store().Mutex());
					Event* copy = TheApp()->Store().FindEvent(id);
					if (copy != nullptr)
						calendar = copy->calendarId;
				}
				if (!calendar.empty()) {
					TheApp()->RequestPush(calendar);
					TheApp()->ScheduleSave();
				}
			}
			break;
		case kMsgContextComplete:
			if (fSelection.kind == Selection::TaskItem) {
				bool completed = false;
				{
					std::lock_guard<std::recursive_mutex> lock(
						TheApp()->Store().Mutex());
					Task* task = TheApp()->Store().FindTask(fSelection.taskId);
					if (task != nullptr)
						completed = task->completed;
				}
				ToggleTask(fSelection.taskId, !completed);
			}
			break;
		case kMsgContextDue:
			if (fSelection.kind == Selection::TaskItem) {
				Store& store = TheApp()->Store();
				std::string calendar;
				{
					std::lock_guard<std::recursive_mutex> lock(store.Mutex());
					Task* task = store.FindTask(fSelection.taskId);
					if (task == nullptr)
						break;
					Task edited = *task;
					int which = message->GetInt32("which", 0);
					Date today = Today();
					edited.hasDue = which != 3;
					edited.dueHasTime = false;
					if (which == 0)
						edited.dueDate = today;
					else if (which == 1)
						edited.dueDate = today.AddDays(1);
					else if (which == 2) {
						int weekStart = store.Prefs().weekStart;
						int delta = (weekStart - today.Weekday() + 7) % 7;
						edited.dueDate = today.AddDays(delta == 0 ? 7 : delta);
					}
					calendar = edited.calendarId;
					store.UpdateTask(edited);
				}
				TheApp()->RequestPush(calendar);
				TheApp()->ScheduleSave();
			}
			break;
		case kMsgContextCalendar:
			if (fSelection.kind == Selection::EventItem) {
				const char* target;
				if (message->FindString("calendar", &target) != B_OK)
					break;
				Store& store = TheApp()->Store();
				std::string from;
				{
					std::lock_guard<std::recursive_mutex> lock(store.Mutex());
					Event* master = store.FindEvent(fSelection.ref.eventId);
					if (master == nullptr || master->calendarId == target)
						break;
					from = master->calendarId;
					Event edited = *master;
					edited.calendarId = target;
					store.UpdateEvent(edited);
					// Overrides follow their master.
					for (Event* child : store.OverridesOf(master->id)) {
						Event moved = *child;
						moved.calendarId = target;
						store.UpdateEvent(moved);
					}
				}
				TheApp()->RequestPush(from);
				TheApp()->RequestPush(target);
				TheApp()->ScheduleSave();
			}
			break;
		case kMsgItemSaved:
			ReloadAll();
			break;
		case kMsgTaskFilter:
			PostMessage(message, fTasksView);
			break;
		case kMsgAccountAdded:
		case kMsgPrefsChanged:
		{
			bool use24;
			{
				std::lock_guard<std::recursive_mutex> lock(TheApp()->Store().Mutex());
				use24 = TheApp()->Store().Prefs().use24Hour;
			}
			Format::SetUse24Hour(use24);
			ReloadAll();
			fDayView->Reload();
			fWeekView->Reload();
			fMonthView->Reload();
			fYearView->Reload();
			fTasksView->Reload();
			UpdateHeader();
			break;
		}
		case kMsgShowAccounts:
			OpenAccountsWindow();
			break;
		case kMsgShowSettings:
			OpenSettingsWindow();
			break;
		case 'adgo':
			OpenAddAccount("google");
			break;
		case 'adcd':
			OpenAddAccount("caldav");
			break;
		case 'adtd':
			OpenAddAccount("todoist");
			break;
		case kMsgSyncNow:
			be_app->PostMessage(message);
			break;
		case kMsgImport:
		{
			BFilePanel* panel = new BFilePanel(B_OPEN_PANEL, &be_app_messenger,
				nullptr, B_FILE_NODE, true, nullptr, nullptr, false, true);
			panel->Window()->SetTitle("airCal: Import Calendar File");
			panel->Show();
			break;
		}
		case kMsgExport:
		{
			BFilePanel* panel = new BFilePanel(B_SAVE_PANEL, new BMessenger(this),
				nullptr, 0, false, new BMessage(kMsgExportRef), nullptr, false, true);
			panel->SetSaveText("airCal.ics");
			panel->Window()->SetTitle("airCal: Export Events");
			panel->Show();
			break;
		}
		case kMsgExportRef:
		{
			entry_ref directory;
			const char* name;
			if (message->FindRef("directory", &directory) != B_OK
				|| message->FindString("name", &name) != B_OK)
				break;
			BPath path(&directory);
			path.Append(name);
			std::string text;
			{
				Store& store = TheApp()->Store();
				std::lock_guard<std::recursive_mutex> lock(store.Mutex());
				std::vector<const Event*> events;
				for (auto& event : store.Events()) {
					Calendar* calendar = store.FindCalendar(event->calendarId);
					if (calendar != nullptr && calendar->visible
						&& event->pending != Pending::Delete)
						events.push_back(event.get());
				}
				text = ical::ExportEvents(events);
			}
			BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
			if (file.InitCheck() == B_OK) {
				file.Write(text.data(), text.size());
				file.WriteAttr("BEOS:TYPE", B_MIME_STRING_TYPE, 0, "text/calendar",
					14);
			}
			break;
		}
		case B_COLORS_UPDATED:
		{
			Theme::Refresh();
			fHeader->Invalidate();
			fSidebar->Invalidate();
			CalendarView* views[] = {fDayView, fWeekView, fMonthView, fYearView,
				fTasksView};
			for (CalendarView* view : views)
				view->ThemeChanged();
			fSidebar->Reload();
			BWindow::MessageReceived(message);
			break;
		}
		case B_ABOUT_REQUESTED:
			be_app->PostMessage(B_ABOUT_REQUESTED);
			break;
		default:
			BWindow::MessageReceived(message);
	}
}


void
MainWindow::SaveState()
{
	BRect frame = Frame();
	char text[64];
	snprintf(text, sizeof(text), "%.0f %.0f %.0f %.0f", frame.left, frame.top,
		frame.right, frame.bottom);
	Store& store = TheApp()->Store();
	std::lock_guard<std::recursive_mutex> lock(store.Mutex());
	store.Prefs().windowFrame = text;
	store.Prefs().view = fMode;
	if (!fSidebarHidden && fSidebar->Bounds().Width() > 100)
		store.Prefs().sidebarWidth = fSidebar->Bounds().Width();
	store.MarkDirty();
}


bool
MainWindow::QuitRequested()
{
	SaveState();
	return true;
}
