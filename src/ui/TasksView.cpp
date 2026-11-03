// The tasks view: Today (with overdue), Upcoming, All (by list), Completed,
// or one list; an "add a task" field that understands natural language.
#include "App.h"
#include "Drawing.h"
#include "Format.h"
#include "MainWindow.h"
#include "Messages.h"
#include "Theme.h"
#include "Views.h"
#include "core/QuickEntry.h"

#include <LayoutBuilder.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include "HintField.h"

#include <TextView.h>
#include <Window.h>

#include <algorithm>
#include <cmath>

using namespace aircal;

namespace {

const float kRowHeight = 44;
const float kSectionHeight = 30;

enum {
	kMsgAddTask = 'tadd'
};


struct Row {
	bool section = false;
	std::string title;		// section title
	rgb_color sectionColor;
	DisplayTask task;
	BRect frame;
};


class TasksView;


class FilterBar : public BView {
public:
	FilterBar(TasksView* owner);
	void Draw(BRect update) override;
	void MouseDown(BPoint where) override;
	BSize MinSize() override { return BSize(200, 40); }
	BSize MaxSize() override { return BSize(B_SIZE_UNLIMITED, 40); }
	BSize PreferredSize() override { return BSize(400, 40); }

private:
	TasksView* fOwner;
	BRect fParts[4];
};


class TaskBody : public BView {
public:
	TaskBody(TasksView* owner);
	void Draw(BRect update) override;
	void MouseDown(BPoint where) override;
	void KeyDown(const char* bytes, int32 count) override;
	void FrameResized(float width, float height) override;
	void TargetedByScrollView(BScrollView* scroller) override { fScroller = scroller; }
	void SetRows(const std::vector<Row>& rows);
	void UpdateScrollBar();

private:
	TasksView* fOwner;
	BScrollView* fScroller = nullptr;
	std::vector<Row> fRows;
	float fHeight = 0;
};


class TasksView : public CalendarView {
public:
	TasksView();

	void Reload() override;
	void Title(std::string& bold, std::string& light) const override;
	Date Step(int direction) const override { return fAnchor; }
	void AttachedToWindow() override;
	void MessageReceived(BMessage* message) override;
	void ThemeChanged() override;

	const std::string& Filter() const { return fFilter; }
	void SetFilter(const std::string& filter);
	MainWindow* Main() const { return static_cast<MainWindow*>(Window()); }

private:
	std::string fFilter = "today";
	FilterBar* fBar = nullptr;
	HintField* fAdd = nullptr;
	TaskBody* fBody = nullptr;
	BScrollView* fScroll = nullptr;
};


// --- FilterBar ------------------------------------------------------------------------

static const char* kFilters[] = {"today", "upcoming", "all", "completed"};
static const char* kFilterNames[] = {"Today", "Upcoming", "All", "Completed"};


FilterBar::FilterBar(TasksView* owner)
	:
	BView("filters", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_FRAME_EVENTS),
	fOwner(owner)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}


void
FilterBar::Draw(BRect update)
{
	BRect bounds = Bounds();
	SetHighColor(Theme::Background());
	FillRect(bounds);
	BFont font(be_plain_font);
	float x = 16;
	SetDrawingMode(B_OP_ALPHA);
	for (int i = 0; i < 4; i++) {
		float width = StringWidth(kFilterNames[i]) + 24;
		BRect part(x, 8, x + width, bounds.bottom - 6);
		fParts[i] = part;
		bool selected = fOwner->Filter() == kFilters[i];
		if (selected)
			Drawing::FillRound(this, part, part.Height() / 2, Theme::Accent());
		else
			Drawing::FillRound(this, part, part.Height() / 2, Theme::Control());
		SetHighColor(selected ? Theme::AccentText() : Theme::Text());
		DrawString(kFilterNames[i], BPoint(part.left + 12, Drawing::Baseline(font, part)));
		x += width + 8;
	}
	if (fOwner->Filter().compare(0, 5, "list:") == 0) {
		std::string name;
		uint32_t color = 0x808080;
		{
			std::lock_guard<std::recursive_mutex> lock(TheApp()->Store().Mutex());
			Calendar* list = TheApp()->Store().FindCalendar(fOwner->Filter().substr(5));
			if (list != nullptr) {
				name = list->name;
				color = list->color;
			}
		}
		float width = StringWidth(name.c_str()) + 36;
		BRect part(x, 8, x + width, bounds.bottom - 6);
		Drawing::FillRound(this, part, part.Height() / 2, Theme::Mix(Theme::Background(),
			Theme::FromHex(color), 0.3f));
		Drawing::FillCircle(this, BPoint(part.left + 13, (part.top + part.bottom) / 2), 4,
			Theme::FromHex(color));
		SetHighColor(Theme::Text());
		DrawString(name.c_str(), BPoint(part.left + 24, Drawing::Baseline(font, part)));
	}
	SetDrawingMode(B_OP_COPY);
}


void
FilterBar::MouseDown(BPoint where)
{
	for (int i = 0; i < 4; i++) {
		if (fParts[i].Contains(where)) {
			fOwner->SetFilter(kFilters[i]);
			return;
		}
	}
}


// --- TaskBody ------------------------------------------------------------------------------

TaskBody::TaskBody(TasksView* owner)
	:
	BView("taskBody", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_FRAME_EVENTS | B_NAVIGABLE),
	fOwner(owner)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}


void
TaskBody::SetRows(const std::vector<Row>& rows)
{
	fRows = rows;
	float y = 4;
	for (Row& row : fRows) {
		float height = row.section ? kSectionHeight : kRowHeight;
		row.frame = BRect(0, y, 10000, y + height - 1);
		y += height;
	}
	fHeight = y + 20;
	UpdateScrollBar();
	Invalidate();
}


void
TaskBody::FrameResized(float width, float height)
{
	UpdateScrollBar();
}


void
TaskBody::UpdateScrollBar()
{
	if (fScroller == nullptr)
		return;
	BScrollBar* bar = fScroller->ScrollBar(B_VERTICAL);
	if (bar == nullptr)
		return;
	float visible = Bounds().Height();
	bar->SetRange(0, std::max(0.0f, fHeight - visible));
	bar->SetProportion(fHeight > 0 ? std::min(1.0f, visible / fHeight) : 1);
	bar->SetSteps(kRowHeight / 2, std::max(kRowHeight, visible - kRowHeight));
}


void
TaskBody::Draw(BRect update)
{
	BRect bounds = Bounds();
	SetHighColor(Theme::Background());
	FillRect(update);
	MainWindow* main = fOwner->Main();
	BFont font(be_plain_font);
	BFont bold(be_bold_font);
	bold.SetSize(font.Size());
	BFont small(font);
	small.SetSize(font.Size() * 0.86f);
	BFont sectionFont(be_bold_font);
	sectionFont.SetSize(font.Size() * 1.08f);
	Date today = Today();

	if (fRows.empty()) {
		SetFont(&font);
		SetHighColor(Theme::TertiaryText());
		const char* text = fOwner->Filter() == "completed" ? "No completed tasks"
			: "Nothing to do. Add a task above, or type \"todo ...\" in the sidebar.";
		SetDrawingMode(B_OP_ALPHA);
		DrawString(text, BPoint((bounds.Width() - StringWidth(text)) / 2, 80));
		SetDrawingMode(B_OP_COPY);
		return;
	}
	SetDrawingMode(B_OP_ALPHA);
	for (const Row& row : fRows) {
		BRect frame = row.frame;
		frame.right = bounds.right;
		if (!frame.Intersects(update))
			continue;
		if (row.section) {
			SetFont(&sectionFont);
			SetHighColor(row.sectionColor);
			DrawString(row.title.c_str(), BPoint(20, frame.bottom - 8));
			SetHighColor(Theme::GridLine());
			StrokeLine(BPoint(20, frame.bottom - 1), BPoint(bounds.right - 16,
				frame.bottom - 1));
			continue;
		}
		const DisplayTask& task = row.task;
		float indent = 20 + task.depth * 24;
		bool selected = main->CurrentSelection().IsTask(task.taskId);
		if (selected) {
			Drawing::FillRound(this, BRect(12, frame.top + 2, bounds.right - 12,
				frame.bottom - 2), 6, Theme::Selection());
		}
		rgb_color ring = task.priority >= 2 ? Theme::Priority(task.priority)
			: Theme::TertiaryText();
		BPoint center(indent + 10, frame.top + 15);
		if (task.priority >= 2 && !task.completed) {
			Drawing::FillCircle(this, center, 8, Theme::WithAlpha(ring, 40));
		}
		Drawing::DrawCheck(this, center, 8, task.completed
			? Theme::FromHex(task.color) : ring, task.completed);
		float x = indent + 28;
		SetFont(&font);
		SetHighColor(task.completed ? Theme::TertiaryText() : Theme::Text());
		std::string title = Drawing::Fit(this, task.title, bounds.right - x - 24);
		DrawString(title.c_str(), BPoint(x, frame.top + 19));
		if (task.completed) {
			SetHighColor(Theme::TertiaryText());
			StrokeLine(BPoint(x, frame.top + 15), BPoint(x + StringWidth(title.c_str()),
				frame.top + 15));
		}
		// Second line: due, list, labels, notes.
		SetFont(&small);
		float y = frame.top + 36;
		float lineX = x;
		if (task.hasDue) {
			std::string due = Format::RelativeDay(task.dueDate);
			if (task.dueHasTime)
				due += " " + Format::Time(task.dueTime);
			bool overdue = !task.completed && (task.dueDate < today
				|| (task.dueHasTime && task.dueTime < Now()));
			SetHighColor(overdue ? Theme::Accent() : (task.dueDate == today
				? Theme::Priority(2) : Theme::SecondaryText()));
			DrawString(due.c_str(), BPoint(lineX, y));
			lineX += StringWidth(due.c_str()) + 6;
			if (task.recurring) {
				Drawing::DrawRepeat(this, BRect(lineX, y - 10, lineX + 11, y + 1),
					Theme::SecondaryText());
				lineX += 16;
			}
			lineX += 8;
		}
		Drawing::FillCircle(this, BPoint(lineX + 4, y - 4), 3.5f, Theme::FromHex(task.color));
		SetHighColor(Theme::SecondaryText());
		DrawString(task.listName.c_str(), BPoint(lineX + 12, y));
		lineX += 12 + StringWidth(task.listName.c_str()) + 12;
		for (const std::string& label : task.labels) {
			std::string text = "#" + label;
			SetHighColor(Theme::Priority(2));
			DrawString(text.c_str(), BPoint(lineX, y));
			lineX += StringWidth(text.c_str()) + 8;
		}
		if (!task.notes.empty() && lineX < bounds.right - 60) {
			std::string notes = task.notes;
			std::replace(notes.begin(), notes.end(), '\n', ' ');
			SetHighColor(Theme::TertiaryText());
			DrawString(Drawing::Fit(this, notes, bounds.right - lineX - 24).c_str(),
				BPoint(lineX, y));
		}
		if (task.pending) {
			Drawing::FillCircle(this, BPoint(bounds.right - 22, frame.top + 15), 2.5f,
				Theme::TertiaryText());
		}
	}
	SetDrawingMode(B_OP_COPY);
}


void
TaskBody::MouseDown(BPoint where)
{
	MakeFocus(true);
	BMessage* current = Window()->CurrentMessage();
	int32 clicks = current != nullptr ? current->GetInt32("clicks", 1) : 1;
	int32 buttons = current != nullptr
		? current->GetInt32("buttons", B_PRIMARY_MOUSE_BUTTON) : B_PRIMARY_MOUSE_BUTTON;
	MainWindow* main = fOwner->Main();
	for (const Row& row : fRows) {
		if (row.section || where.y < row.frame.top || where.y > row.frame.bottom)
			continue;
		const DisplayTask& task = row.task;
		float indent = 20 + task.depth * 24;
		BPoint screen = ConvertToScreen(where);
		if (where.x >= indent - 2 && where.x <= indent + 22 && where.y < row.frame.top + 28) {
			main->ToggleTask(task.taskId, !task.completed);
			return;
		}
		if (buttons & B_SECONDARY_MOUSE_BUTTON)
			main->ShowTaskMenu(task.taskId, screen);
		else if (clicks >= 2)
			main->EditTask(task.taskId, screen);
		else
			main->SelectTask(task.taskId);
		return;
	}
	main->SetSelection(Selection());
}


void
TaskBody::KeyDown(const char* bytes, int32 count)
{
	MainWindow* main = fOwner->Main();
	switch (bytes[0]) {
		case B_DELETE:
		case B_BACKSPACE:
			Window()->PostMessage(kMsgDeleteSelection);
			break;
		case B_ENTER:
			Window()->PostMessage('edsl');
			break;
		case ' ':
			if (main->CurrentSelection().kind == Selection::TaskItem) {
				for (const Row& row : fRows) {
					if (!row.section && row.task.taskId == main->CurrentSelection().taskId) {
						main->ToggleTask(row.task.taskId, !row.task.completed);
						break;
					}
				}
			}
			break;
		case B_UP_ARROW:
		case B_DOWN_ARROW:
		{
			int index = -1;
			std::vector<int> tasks;
			for (size_t i = 0; i < fRows.size(); i++) {
				if (fRows[i].section)
					continue;
				if (main->CurrentSelection().IsTask(fRows[i].task.taskId))
					index = (int)tasks.size();
				tasks.push_back((int)i);
			}
			if (tasks.empty())
				break;
			int next = index < 0 ? 0 : index + (bytes[0] == B_UP_ARROW ? -1 : 1);
			next = std::max(0, std::min((int)tasks.size() - 1, next));
			const Row& row = fRows[tasks[next]];
			main->SelectTask(row.task.taskId);
			BRect visible = Bounds();
			if (row.frame.top < visible.top)
				ScrollTo(0, row.frame.top);
			else if (row.frame.bottom > visible.bottom)
				ScrollTo(0, row.frame.bottom - visible.Height());
			break;
		}
		default:
			BView::KeyDown(bytes, count);
	}
}


// --- TasksView ------------------------------------------------------------------------------

TasksView::TasksView()
	:
	CalendarView("tasks", 0)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	{
		std::lock_guard<std::recursive_mutex> lock(TheApp()->Store().Mutex());
		fFilter = TheApp()->Store().Prefs().taskFilter;
	}
	fBar = new FilterBar(this);
	fAdd = new HintField("addTask", "Add a task", new BMessage(kMsgAddTask), nullptr,
		HintField::kPlus);
	fBody = new TaskBody(this);
	fScroll = new BScrollView("taskScroll", fBody, 0, false, true, B_NO_BORDER);
	BView* addRow = new BGroupView(B_HORIZONTAL, 0);
	BLayoutBuilder::Group<>(static_cast<BGroupView*>(addRow))
		.SetInsets(16, 2, 16, 8)
		.Add(fAdd);
	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(fBar)
		.Add(addRow)
		.Add(fScroll, 1.0f);
}


void
TasksView::AttachedToWindow()
{
	CalendarView::AttachedToWindow();
	fAdd->SetTarget(BMessenger(this));
	ThemeChanged();
}


void
TasksView::ThemeChanged()
{
	fBar->Invalidate();
	fBody->Invalidate();
	rgb_color background = Theme::Background();
	BView* addRow = fAdd->Parent();
	if (addRow != nullptr) {
		addRow->SetViewColor(background);
		addRow->Invalidate();
	}
	SetViewColor(background);
	Invalidate();
}


void
TasksView::SetFilter(const std::string& filter)
{
	fFilter = filter;
	{
		std::lock_guard<std::recursive_mutex> lock(TheApp()->Store().Mutex());
		TheApp()->Store().Prefs().taskFilter = filter;
	}
	fBody->ScrollTo(0, 0);
	Reload();
	if (Window() != nullptr) {
		BMessage message(kMsgSetView);
		message.AddString("view", "tasks");
		Window()->PostMessage(&message);
	}
}


void
TasksView::Title(std::string& bold, std::string& light) const
{
	bold = "Tasks";
	if (fFilter.compare(0, 5, "list:") == 0) {
		std::lock_guard<std::recursive_mutex> lock(TheApp()->Store().Mutex());
		Calendar* list = TheApp()->Store().FindCalendar(fFilter.substr(5));
		light = list != nullptr ? list->name : "";
		return;
	}
	for (int i = 0; i < 4; i++) {
		if (fFilter == kFilters[i])
			light = kFilterNames[i];
	}
}


void
TasksView::Reload()
{
	Store& store = TheApp()->Store();
	bool showCompleted;
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		showCompleted = store.Prefs().showCompleted;
	}
	std::string placeholder = "Add a task to ";
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		Calendar* list = fFilter.compare(0, 5, "list:") == 0
			? store.FindCalendar(fFilter.substr(5)) : store.DefaultCalendar(true);
		placeholder += list != nullptr ? "\"" + list->name + "\"" : "a list";
		placeholder += " — e.g. \"Call Sam tomorrow 3pm !!\"";
	}
	fAdd->SetHint(placeholder.c_str());

	std::vector<Row> rows;
	Date today = Today();
	auto section = [&](const std::string& title, rgb_color color) {
		Row row;
		row.section = true;
		row.title = title;
		row.sectionColor = color;
		rows.push_back(row);
	};
	auto add = [&](const DisplayTask& task) {
		Row row;
		row.task = task;
		rows.push_back(row);
	};
	std::string search = Main() != nullptr ? Main()->SearchText() : "";

	if (fFilter == "today") {
		std::vector<DisplayTask> tasks = store.TasksBetween(today, today, showCompleted,
			true);
		std::vector<DisplayTask> overdue, due;
		for (auto& task : tasks)
			(task.dueDate < today ? overdue : due).push_back(task);
		if (!overdue.empty()) {
			section("Overdue", Theme::Accent());
			for (auto& task : overdue)
				add(task);
		}
		if (!due.empty() || overdue.empty()) {
			section("Today · " + Format::DateMedium(today), Theme::Text());
			for (auto& task : due)
				add(task);
		}
	} else if (fFilter == "upcoming") {
		std::vector<DisplayTask> tasks = store.TasksBetween(today, today.AddDays(365),
			showCompleted, true);
		Date current;
		bool first = true;
		for (auto& task : tasks) {
			Date date = task.dueDate < today ? today.AddDays(-1) : task.dueDate;
			if (first || date != current) {
				current = date;
				first = false;
				if (task.dueDate < today)
					section("Overdue", Theme::Accent());
				else
					section(Format::RelativeDay(date) + (date == today || date
						== today.AddDays(1) ? " · " + Format::DateMedium(date) : ""),
						date == today ? Theme::Accent() : Theme::Text());
			}
			add(task);
		}
	} else if (fFilter == "completed") {
		std::vector<DisplayTask> all = store.TaskList("", true);
		std::vector<DisplayTask> done;
		for (auto& task : all) {
			if (task.completed) {
				task.depth = 0;
				done.push_back(task);
			}
		}
		if (!done.empty())
			section("Completed", Theme::SecondaryText());
		for (auto& task : done)
			add(task);
	} else {
		std::string list = fFilter.compare(0, 5, "list:") == 0 ? fFilter.substr(5) : "";
		std::vector<DisplayTask> tasks = store.TaskList(list, showCompleted);
		std::string currentList;
		for (auto& task : tasks) {
			if (task.calendarId != currentList) {
				currentList = task.calendarId;
				std::string name = task.listName;
				{
					std::lock_guard<std::recursive_mutex> lock(store.Mutex());
					Calendar* calendar = store.FindCalendar(task.calendarId);
					Account* account = calendar != nullptr ? store.AccountOf(*calendar)
						: nullptr;
					if (account != nullptr && list.empty())
						name += "  ·  " + account->name;
				}
				section(name, Theme::FromHex(task.color));
			}
			add(task);
		}
	}
	// Search narrows any filter.
	if (!search.empty()) {
		std::string needle = search;
		for (char& c : needle)
			c = (char)tolower((unsigned char)c);
		std::vector<Row> filtered;
		for (auto& row : rows) {
			if (row.section) {
				filtered.push_back(row);
				continue;
			}
			std::string haystack = row.task.title + " " + row.task.notes;
			for (auto& label : row.task.labels)
				haystack += " " + label;
			for (char& c : haystack)
				c = (char)tolower((unsigned char)c);
			if (haystack.find(needle) != std::string::npos)
				filtered.push_back(row);
		}
		// Drop empty sections.
		rows.clear();
		for (size_t i = 0; i < filtered.size(); i++) {
			if (filtered[i].section && (i + 1 >= filtered.size() || filtered[i + 1].section))
				continue;
			rows.push_back(filtered[i]);
		}
	}
	fBody->SetRows(rows);
	fBar->Invalidate();
}


void
TasksView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgAddTask:
		{
			std::string text = fAdd->Text();
			if (text.empty())
				break;
			Store& store = TheApp()->Store();
			int weekStart;
			{
				std::lock_guard<std::recursive_mutex> lock(store.Mutex());
				weekStart = store.Prefs().weekStart;
			}
			QuickEntryResult result = ParseQuickEntry("todo " + text, Now(), 60,
				weekStart);
			std::string calendarId;
			{
				std::lock_guard<std::recursive_mutex> lock(store.Mutex());
				aircal::UndoScope undo(TheApp()->Undo(), TheApp()->Store(), "New Task");
				Calendar* list = fFilter.compare(0, 5, "list:") == 0
					? store.FindCalendar(fFilter.substr(5)) : nullptr;
				if (list == nullptr || list->readOnly)
					list = store.DefaultCalendar(true);
				if (list == nullptr)
					break;
				Account* account = store.AccountOf(*list);
				Task task;
				task.calendarId = list->id;
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
				} else if (fFilter == "today") {
					task.hasDue = true;
					task.dueDate = Today();
				}
				if (!result.rrule.empty()) {
					task.recurring = true;
					if (account != nullptr && account->type == AccountType::Todoist)
						task.dueString = result.dueString;
					else
						task.rrule = result.rrule;
				}
				if (account != nullptr && account->type == AccountType::Google)
					task.dueHasTime = false;
				calendarId = list->id;
				store.UpdateTask(task);
			}
			fAdd->SetText("");
			TheApp()->RequestPush(calendarId);
			TheApp()->ScheduleSave();
			break;
		}
		case kMsgTaskFilter:
		{
			const char* filter;
			if (message->FindString("filter", &filter) == B_OK)
				SetFilter(filter);
			break;
		}
		default:
			CalendarView::MessageReceived(message);
	}
}

}	// namespace


CalendarView*
CreateTasksView()
{
	return new TasksView();
}
