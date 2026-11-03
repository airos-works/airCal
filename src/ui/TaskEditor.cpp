// The task editor: title, list, due date/time, priority, repeat, labels,
// alert and notes. What it offers follows the list's service: Google Tasks
// knows no due times, priorities, labels or repeats; Todoist repeats are
// natural-language "due strings"; CalDAV and local lists use RRULEs.
#include "EventEditor.h"

#include "App.h"
#include "Dialogs.h"
#include "Format.h"
#include "Messages.h"
#include "Theme.h"

#include <Alert.h>
#include <Beep.h>
#include <Button.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <PopUpMenu.h>
#include <ScrollView.h>
#include <SeparatorView.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>

#include <algorithm>
#include <cmath>

using namespace aircal;
using namespace DialogKit;

namespace {

enum {
	kMsgList = 'tlst',
	kMsgHasDue = 'tdue',
	kMsgHasTime = 'ttim',
	kMsgDueCommitted = 'tdcm',
	kMsgDueModified = 'tdmd',
	kMsgPriority = 'tpri',
	kMsgRepeat = 'trep',
	kMsgAlarm = 'talm',
	kMsgSave = 'tsav',
	kMsgDelete = 'tdel',
	kMsgCancel = 'tcan'
};


struct PriorityChoice {
	int value;
	const char* label;
};

const PriorityChoice kPriorities[] = {
	{0, "None"}, {1, "Low"}, {2, "Medium"}, {3, "High"}, {4, "Urgent"}
};

// Todoist shows p1 (most urgent) .. p4 (none).
const PriorityChoice kTodoistPriorities[] = {
	{0, "None"}, {2, "Priority 3"}, {3, "Priority 2"}, {4, "Priority 1"}
};


BStringView*
FieldLabel(const char* text, bool top = false)
{
	BStringView* label = new BStringView(NULL, text);
	label->SetAlignment(B_ALIGN_RIGHT);
	label->SetExplicitAlignment(BAlignment(B_ALIGN_RIGHT,
		top ? B_ALIGN_TOP : B_ALIGN_VERTICAL_CENTER));
	return label;
}


void
SetVisible(BView* view, bool visible)
{
	if (view == NULL || visible == !view->IsHidden(view))
		return;
	if (visible)
		view->Show();
	else
		view->Hide();
}


std::string
JoinLabels(const std::vector<std::string>& labels)
{
	std::string text;
	for (auto& label : labels) {
		if (!text.empty())
			text += ", ";
		text += label;
	}
	return text;
}


std::vector<std::string>
SplitLabels(const std::string& text)
{
	std::vector<std::string> labels;
	size_t begin = 0;
	while (begin <= text.size()) {
		size_t comma = text.find(',', begin);
		if (comma == std::string::npos)
			comma = text.size();
		std::string label = Trim(text.substr(begin, comma - begin));
		if (!label.empty()
			&& std::find(labels.begin(), labels.end(), label) == labels.end())
			labels.push_back(label);
		begin = comma + 1;
	}
	return labels;
}


class TaskEditorWindow : public BWindow {
public:
	TaskEditorWindow(const Task& task, bool isNew, const std::string& key);
	~TaskEditorWindow() override;

	void MessageReceived(BMessage* message) override;

private:
	void BuildLayout();
	void UpdateForList();
	void UpdateDueControls();
	void RebuildPriorityMenu();
	bool Validate(bool mark);
	void Save();
	void Delete();
	void Finish(const std::string& id, bool deleted);

	Task fTask;
	bool fNew;
	bool fReadOnly = false;
	std::string fKey;
	std::string fListId;
	AccountType fType = AccountType::Local;
	int fPriority;
	RepeatPreset fInitialPreset;
	RepeatPreset fPreset;
	int fInitialAlarm;
	int fAlarm;

	BTextControl* fTitle = NULL;
	BPopUpMenu* fListMenu = NULL;
	BMenuField* fListField = NULL;
	BCheckBox* fHasDue = NULL;
	BTextControl* fDueDate = NULL;
	BCheckBox* fHasTime = NULL;
	BTextControl* fDueTime = NULL;
	BStringView* fRepeatLabel = NULL;
	BPopUpMenu* fRepeatMenu = NULL;
	BMenuField* fRepeatField = NULL;
	BStringView* fRepeatTextLabel = NULL;
	BTextControl* fRepeatText = NULL;
	BStringView* fPriorityLabel = NULL;
	BPopUpMenu* fPriorityMenu = NULL;
	BMenuField* fPriorityField = NULL;
	BStringView* fLabelsLabel = NULL;
	BTextControl* fLabels = NULL;
	BPopUpMenu* fAlarmMenu = NULL;
	BMenuField* fAlarmField = NULL;
	BTextView* fNotes = NULL;
	BCheckBox* fCompleted = NULL;
	BStringView* fReadOnlyNote = NULL;
	BButton* fDelete = NULL;
	BButton* fCancel = NULL;
	BButton* fSave = NULL;
};


TaskEditorWindow::TaskEditorWindow(const Task& task, bool isNew,
	const std::string& key)
	:
	BWindow(BRect(0, 0, 340, 360), isNew ? "New Task" : "Task",
		B_FLOATING_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL,
		B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE
			| B_ASYNCHRONOUS_CONTROLS),
	fTask(task),
	fNew(isNew),
	fKey(key),
	fListId(task.calendarId),
	fPriority(task.priority)
{
	{
		Store& store = TheApp()->Store();
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		Calendar* list = store.FindCalendar(fListId);
		fReadOnly = list != NULL && list->readOnly;
	}
	fType = AccountTypeOfCalendar(fListId);
	fInitialPreset = fPreset = PresetFromRule(fTask.rrule);
	fInitialAlarm = fAlarm = fTask.alarms.empty() ? -1 : fTask.alarms[0];

	BuildLayout();
	UpdateForList();
	UpdateDueControls();

	if (fReadOnly) {
		SetTitle("Task (read-only)");
		for (BControl* control : std::initializer_list<BControl*>{fTitle,
				fHasDue, fDueDate, fHasTime, fDueTime, fRepeatText, fLabels,
				fCompleted, fSave})
			control->SetEnabled(false);
		fListField->SetEnabled(false);
		fRepeatField->SetEnabled(false);
		fPriorityField->SetEnabled(false);
		fAlarmField->SetEnabled(false);
		fNotes->MakeEditable(false);
		SetDefaultButton(fCancel);
		fCancel->MakeFocus(true);
	} else {
		fTitle->MakeFocus(true);
		fTitle->TextView()->SelectAll();
	}
}


TaskEditorWindow::~TaskEditorWindow()
{
	Unregister(fKey, this);
}


void
TaskEditorWindow::BuildLayout()
{
	BFont titleFont(be_plain_font);
	titleFont.SetSize(ceilf(titleFont.Size() * 1.45f));
	titleFont.SetFace(B_BOLD_FACE);
	fTitle = new BTextControl("title", NULL, fTask.title.c_str(), NULL);
	fTitle->SetFont(&titleFont);
	fTitle->TextView()->SetFontAndColor(&titleFont);
	fTitle->SetExplicitMinSize(BSize(300, B_SIZE_UNSET));

	fListMenu = new BPopUpMenu("list");
	AddCalendarItems(fListMenu, true, fListId, kMsgList);
	fListField = new BMenuField("list", NULL, fListMenu);

	fHasDue = new BCheckBox("hasDue", "Due", new BMessage(kMsgHasDue));
	fHasDue->SetValue(fTask.hasDue ? B_CONTROL_ON : B_CONTROL_OFF);
	Date due = fTask.hasDue ? fTask.dueDate : Today();
	fDueDate = new BTextControl("dueDate", NULL, DateFieldText(due).c_str(),
		new BMessage(kMsgDueCommitted));
	fDueDate->SetModificationMessage(new BMessage(kMsgDueModified));
	float dateWidth = ceilf(be_plain_font->StringWidth("Wed, Sep 30, 2026")) + 24;
	fDueDate->SetExplicitMinSize(BSize(dateWidth, B_SIZE_UNSET));
	fDueDate->SetExplicitMaxSize(BSize(dateWidth * 1.3f, B_SIZE_UNSET));

	fHasTime = new BCheckBox("hasTime", "at", new BMessage(kMsgHasTime));
	fHasTime->SetValue(fTask.hasDue && fTask.dueHasTime ? B_CONTROL_ON
		: B_CONTROL_OFF);
	int hour = 9, minute = 0;
	if (fTask.hasDue && fTask.dueHasTime) {
		DateTime wall = ToWall(fTask.dueTime);
		hour = wall.hour;
		minute = wall.minute;
	}
	fDueTime = new BTextControl("dueTime", NULL,
		TimeFieldText(hour, minute).c_str(), new BMessage(kMsgDueCommitted));
	fDueTime->SetModificationMessage(new BMessage(kMsgDueModified));
	float timeWidth = ceilf(be_plain_font->StringWidth("12:59 PM")) + 24;
	fDueTime->SetExplicitMinSize(BSize(timeWidth, B_SIZE_UNSET));
	fDueTime->SetExplicitMaxSize(BSize(timeWidth * 1.3f, B_SIZE_UNSET));

	fRepeatLabel = FieldLabel("Repeat:");
	fRepeatMenu = new BPopUpMenu("repeat");
	AddRepeatItems(fRepeatMenu, fTask.rrule, kMsgRepeat);
	fRepeatField = new BMenuField("repeat", NULL, fRepeatMenu);

	fRepeatTextLabel = FieldLabel("Repeat (Todoist):");
	fRepeatText = new BTextControl("repeatText", NULL,
		fTask.recurring ? fTask.dueString.c_str() : "", NULL);
	fRepeatText->SetToolTip("How Todoist repeats the task, for example "
		"\"every monday\", \"every 2 weeks\" or \"every workday at 9am\". "
		"Leave it empty for no repeat.");

	fPriorityLabel = FieldLabel("Priority:");
	fPriorityMenu = new BPopUpMenu("priority");
	fPriorityField = new BMenuField("priority", NULL, fPriorityMenu);

	fLabelsLabel = FieldLabel("Labels:");
	fLabels = new BTextControl("labels", NULL,
		JoinLabels(fTask.labels).c_str(), NULL);
	fLabels->SetToolTip("Separate labels with commas");

	fAlarmMenu = new BPopUpMenu("alert");
	AddAlarmItems(fAlarmMenu, fAlarm, kMsgAlarm);
	fAlarmField = new BMenuField("alert", NULL, fAlarmMenu);

	fNotes = new BTextView("notes");
	fNotes->SetWordWrap(true);
	fNotes->SetStylable(false);
	fNotes->SetText(fTask.notes.c_str());
	BScrollView* notesScroll = new BScrollView("notesScroll", fNotes, 0, false,
		true);
	font_height height;
	be_plain_font->GetHeight(&height);
	float lines = ceilf((height.ascent + height.descent + height.leading) * 4)
		+ 8;
	notesScroll->SetExplicitMinSize(BSize(B_SIZE_UNSET, lines));
	notesScroll->SetExplicitPreferredSize(BSize(B_SIZE_UNSET, lines));

	fCompleted = new BCheckBox("completed", "Completed", NULL);
	fCompleted->SetValue(fTask.completed ? B_CONTROL_ON : B_CONTROL_OFF);

	fReadOnlyNote = new BStringView("readOnly", "This list is read-only");
	fReadOnlyNote->SetAlignment(B_ALIGN_CENTER);
	fReadOnlyNote->SetExplicitAlignment(BAlignment(B_ALIGN_CENTER,
		B_ALIGN_MIDDLE));

	fDelete = new BButton("delete", "Delete", new BMessage(kMsgDelete));
	fCancel = new BButton("cancel", "Cancel", new BMessage(kMsgCancel));
	fSave = new BButton("save", "Save", new BMessage(kMsgSave));

	BGridView* grid = new BGridView(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING);
	BLayoutBuilder::Grid<>(grid)
		.Add(FieldLabel("List:"), 0, 0)
		.Add(fListField, 1, 0)
		.Add(FieldLabel("Due:"), 0, 1)
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING, 1, 1)
			.Add(fHasDue)
			.Add(fDueDate)
			.Add(fHasTime)
			.Add(fDueTime)
			.AddGlue()
		.End()
		.Add(fRepeatLabel, 0, 2)
		.Add(fRepeatField, 1, 2)
		.Add(fRepeatTextLabel, 0, 3)
		.Add(fRepeatText, 1, 3)
		.Add(FieldLabel("Alert:"), 0, 4)
		.Add(fAlarmField, 1, 4)
		.Add(fPriorityLabel, 0, 5)
		.Add(fPriorityField, 1, 5)
		.Add(fLabelsLabel, 0, 6)
		.Add(fLabels, 1, 6)
		.Add(FieldLabel("Notes:", true), 0, 7)
		.Add(notesScroll, 1, 7)
		.Add(fCompleted, 1, 8);
	grid->GridLayout()->SetColumnWeight(0, 0);
	grid->GridLayout()->SetColumnWeight(1, 1);

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(fTitle)
		.Add(new BSeparatorView(B_HORIZONTAL))
		.Add(grid)
		.Add(fReadOnlyNote)
		.AddGroup(B_HORIZONTAL)
			.Add(fDelete)
			.AddGlue()
			.Add(fCancel)
			.Add(fSave)
		.End();

	SetVisible(fReadOnlyNote, fReadOnly);
	SetVisible(fDelete, !fNew && !fReadOnly);
	SetDefaultButton(fSave);
}


void
TaskEditorWindow::UpdateForList()
{
	bool google = fType == AccountType::Google;
	bool todoist = fType == AccountType::Todoist;
	SetVisible(fHasTime, !google);
	SetVisible(fDueTime, !google);
	SetVisible(fRepeatLabel, !google && !todoist);
	SetVisible(fRepeatField, !google && !todoist);
	SetVisible(fRepeatTextLabel, todoist);
	SetVisible(fRepeatText, todoist);
	SetVisible(fPriorityLabel, !google);
	SetVisible(fPriorityField, !google);
	SetVisible(fLabelsLabel, !google);
	SetVisible(fLabels, !google);
	RebuildPriorityMenu();
	UpdateDueControls();
}


void
TaskEditorWindow::RebuildPriorityMenu()
{
	fPriorityMenu->RemoveItems(0, fPriorityMenu->CountItems(), true);
	bool todoist = fType == AccountType::Todoist;
	const PriorityChoice* choices = todoist ? kTodoistPriorities : kPriorities;
	size_t count = todoist ? B_COUNT_OF(kTodoistPriorities)
		: B_COUNT_OF(kPriorities);
	// Todoist has no "low": it shows as none.
	int shown = todoist && fPriority == 1 ? 0 : fPriority;
	for (size_t i = 0; i < count; i++) {
		BMessage* message = new BMessage(kMsgPriority);
		message->AddInt32("priority", choices[i].value);
		BMenuItem* item = new BMenuItem(choices[i].label, message);
		fPriorityMenu->AddItem(item);
		if (choices[i].value == shown)
			item->SetMarked(true);
	}
}


void
TaskEditorWindow::UpdateDueControls()
{
	bool due = fHasDue->Value() == B_CONTROL_ON;
	bool editable = !fReadOnly;
	fDueDate->SetEnabled(editable && due);
	fHasTime->SetEnabled(editable && due);
	fDueTime->SetEnabled(editable && due && fHasTime->Value() == B_CONTROL_ON);
	fAlarmField->SetEnabled(editable && due);
	Validate(true);
}


bool
TaskEditorWindow::Validate(bool mark)
{
	bool due = fHasDue->Value() == B_CONTROL_ON;
	bool timed = due && fType != AccountType::Google
		&& fHasTime->Value() == B_CONTROL_ON;
	Date date;
	int hour, minute;
	bool dateOk = !due || Format::ParseDate(fDueDate->Text(), date);
	bool timeOk = !timed || Format::ParseTime(fDueTime->Text(), hour, minute);
	if (mark) {
		SetFieldValid(fDueDate, dateOk);
		SetFieldValid(fDueTime, timeOk);
	}
	return dateOk && timeOk;
}


void
TaskEditorWindow::Save()
{
	if (fReadOnly)
		return;
	if (!Validate(true)) {
		beep();
		return;
	}

	Task edited = fTask;
	edited.title = Trim(fTitle->Text());
	if (edited.title.empty())
		edited.title = "New Task";
	edited.notes = fNotes->Text();
	edited.calendarId = fListId;

	bool google = fType == AccountType::Google;
	bool todoist = fType == AccountType::Todoist;
	edited.hasDue = fHasDue->Value() == B_CONTROL_ON;
	if (edited.hasDue) {
		Format::ParseDate(fDueDate->Text(), edited.dueDate);
		edited.dueHasTime = !google && fHasTime->Value() == B_CONTROL_ON;
		if (edited.dueHasTime) {
			int hour, minute;
			Format::ParseTime(fDueTime->Text(), hour, minute);
			edited.dueTime = ToInstant(DateTime(edited.dueDate, hour, minute));
		} else
			edited.dueTime = 0;
	} else {
		edited.dueHasTime = false;
		edited.dueTime = 0;
	}

	if (!google) {
		edited.priority = fPriority;
		edited.labels = SplitLabels(fLabels->Text());
	}

	if (todoist) {
		std::string repeat = Trim(fRepeatText->Text());
		std::string before = fTask.recurring ? fTask.dueString : std::string();
		if (repeat != before) {
			edited.recurring = !repeat.empty();
			edited.dueString = repeat;
		}
	} else if (!google && fPreset != fInitialPreset
		&& fPreset != RepeatPreset::Custom) {
		edited.rrule = RuleFromPreset(fPreset,
			edited.hasDue ? edited.dueDate : Today());
		edited.recurring = !edited.rrule.empty();
	}

	if (fAlarm != fInitialAlarm) {
		if (fAlarm < 0)
			edited.alarms.clear();
		else if (edited.alarms.empty())
			edited.alarms.push_back(fAlarm);
		else
			edited.alarms[0] = fAlarm;
	}

	bool completed = fCompleted->Value() == B_CONTROL_ON;
	bool completionChanged = completed != fTask.completed;

	if (fNew) {
		edited.id = NewId();
		edited.created = Now();
	}
	{
		aircal::UndoScope undo(TheApp()->Undo(), TheApp()->Store(), fNew ? "New Task" : "Edit Task");
		if (fNew && completed) {
			edited.completed = true;
			edited.completedAt = Now();
		}
		TheApp()->Store().UpdateTask(edited);
		// Completing goes through the store so a repeating local task moves
		// on to its next date.
		if (completionChanged && !fNew)
			TheApp()->Store().SetTaskCompleted(edited.id, completed);
	}

	TheApp()->RequestPush(fListId);
	if (!fTask.calendarId.empty() && fTask.calendarId != fListId)
		TheApp()->RequestPush(fTask.calendarId);
	Finish(edited.id, false);
}


void
TaskEditorWindow::Delete()
{
	if (fNew || fReadOnly)
		return;
	std::string title = fTask.title.empty() ? "this task"
		: "\"" + fTask.title + "\"";
	std::string text = "Delete " + title + "?";
	BAlert* alert = new BAlert("Delete Task", text.c_str(), "Cancel", "Delete",
		NULL, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
	alert->SetShortcut(0, B_ESCAPE);
	if (alert->Go() != 1)
		return;
	{
		aircal::UndoScope undo(TheApp()->Undo(), TheApp()->Store(), "Delete Task");
		TheApp()->Store().DeleteTask(fTask.id);
	}
	TheApp()->RequestPush(fTask.calendarId);
	Finish(fTask.id, true);
}


void
TaskEditorWindow::Finish(const std::string& id, bool deleted)
{
	TheApp()->ScheduleSave();
	BMessage saved(kMsgItemSaved);
	saved.AddString("kind", "task");
	saved.AddString("id", id.c_str());
	if (deleted)
		saved.AddBool("deleted", true);
	PostToMain(&saved);
	Quit();
}


void
TaskEditorWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgList:
		{
			const char* id;
			if (message->FindString("calendar", &id) == B_OK) {
				fListId = id;
				fType = AccountTypeOfCalendar(fListId);
				UpdateForList();
			}
			break;
		}
		case kMsgHasDue:
		case kMsgHasTime:
			UpdateDueControls();
			break;
		case kMsgDueCommitted:
		{
			Date date;
			int hour, minute;
			if (Format::ParseDate(fDueDate->Text(), date)) {
				std::string text = DateFieldText(date);
				if (text != fDueDate->Text())
					fDueDate->SetText(text.c_str());
			}
			if (Format::ParseTime(fDueTime->Text(), hour, minute)) {
				std::string text = TimeFieldText(hour, minute);
				if (text != fDueTime->Text())
					fDueTime->SetText(text.c_str());
			}
			Validate(true);
			break;
		}
		case kMsgDueModified:
			Validate(true);
			break;
		case kMsgPriority:
		{
			int32 priority;
			if (message->FindInt32("priority", &priority) == B_OK)
				fPriority = priority;
			break;
		}
		case kMsgRepeat:
		{
			int32 preset;
			if (message->FindInt32("preset", &preset) == B_OK)
				fPreset = (RepeatPreset)preset;
			break;
		}
		case kMsgAlarm:
		{
			int32 minutes;
			if (message->FindInt32("minutes", &minutes) == B_OK)
				fAlarm = minutes;
			break;
		}
		case kMsgSave:
			Save();
			break;
		case kMsgDelete:
			Delete();
			break;
		case kMsgCancel:
			Quit();
			break;
		case B_COLORS_UPDATED:
			Theme::Refresh();
			Validate(true);
			BWindow::MessageReceived(message);
			break;
		default:
			BWindow::MessageReceived(message);
	}
}

}	// namespace


void
OpenTaskEditor(const std::string& taskId, const Task& draft, float anchorX,
	float anchorY)
{
	std::string key = taskId.empty() ? std::string() : "task:" + taskId;
	if (ActivateRegistered(key))
		return;

	Store& store = TheApp()->Store();
	Task task;
	bool isNew = taskId.empty();
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		if (!isNew) {
			Task* existing = store.FindTask(taskId);
			if (existing == NULL)
				return;
			task = *existing;
		} else {
			task = draft;
			task.id.clear();
			Calendar* list = store.FindCalendar(task.calendarId);
			if (list == NULL || list->readOnly || !list->tasks)
				list = store.DefaultCalendar(true);
			if (list == NULL) {
				BAlert* alert = new BAlert("airCal", "There is no task list to "
					"add the task to. Add an account or a list first.", "OK");
				alert->Go(NULL);
				return;
			}
			task.calendarId = list->id;
		}
	}

	TaskEditorWindow* window = new TaskEditorWindow(task, isNew, key);
	Register(key, window);
	PlaceWindow(window, anchorX, anchorY);
	window->Show();
}
