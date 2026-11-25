#include "MobilePlanner.h"

#include "App.h"
#include "DateNavigator.h"
#include "Drawing.h"
#include "Format.h"
#include "MainWindow.h"
#include "Messages.h"
#include "Theme.h"

#include <MobileKit.h>

using namespace aircal;
namespace mk = BMobileKit;

namespace {
enum { kPage = 'mpag', kFilter = 'mflt', kAdd = 'madd', kMore = 'mmor', kList = 'mlst' };

void
AddSection(mk::TableView* list, const std::string& title)
{
	auto* row = new mk::TableRow(title.c_str());
	row->SetHeader(true);
	list->AddItem(row);
}

// The selected day reads like the leaf of a desk calendar. Keep this in the
// list so it scrolls away with the agenda on a short screen.
class AgendaHeading : public mk::TableRow {
public:
	AgendaHeading(const Date& date, const std::string& summary)
		: TableRow(Format::DateLong(date).c_str(), summary.c_str()), fDate(date)
	{
		SetHeader(true);
	}

	void Update(BView* owner, const BFont* font) override
	{
		TableRow::Update(owner, font);
		SetHeight(ceilf(mk::TargetSize() * 1.55f));
	}

	void DrawItem(BView* owner, BRect frame, bool) override
	{
		const auto& skin = mk::CurrentSkin();
		float unit = be_plain_font->Size();
		owner->PushState();
		mk::DrawPage(owner, frame, frame);
		BRect leaf = frame.InsetByCopy(unit, unit * 0.55f);
		leaf.right = leaf.left + 4 * unit;
		mk::DrawPlate(owner, leaf, 4, skin.fieldBackground, skin.pageBottom);
		BRect cap = leaf.InsetByCopy(1, 1);
		cap.bottom = cap.top + ceilf(unit * 1.35f);
		mk::FillGradient(owner, cap, Theme::Accent(),
			Theme::Mix(Theme::Accent(), make_color(0, 0, 0), 0.3f), 2);
		BFont font(be_bold_font);
		font.SetSize(unit * 0.86f);
		owner->SetFont(&font);
		BString month(Format::MonthShort(fDate.month).c_str());
		month.ToUpper();
		mk::DrawCentered(owner, month.String(), cap, Theme::AccentText(), skin.selectedEmboss);
		BRect number = leaf;
		number.top = cap.bottom + 1;
		font.SetSize(unit * 1.9f);
		owner->SetFont(&font);
		mk::DrawCentered(owner, std::to_string(fDate.day).c_str(), number, skin.pageText,
			skin.controlEmboss);
		float left = leaf.right + unit * 1.2f;
		font.SetSize(unit * 1.6f);
		owner->SetFont(&font);
		mk::DrawEngraved(owner, Format::WeekdayLong(fDate.Weekday()).c_str(),
			BPoint(left, frame.top + Height() / 2), skin.pageText, skin.controlEmboss);
		font = *be_plain_font;
		owner->SetFont(&font);
		mk::DrawEngraved(owner, Detail(), BPoint(left,
			frame.top + Height() / 2 + unit * 1.5f), skin.pageSecondary, skin.controlEmboss);
		owner->SetHighColor(skin.barEdge);
		owner->StrokeLine(frame.LeftBottom(), frame.RightBottom());
		owner->PopState();
	}

private:
	Date fDate;
};

BMessage*
EditTaskMessage(const DisplayTask& task)
{
	auto* message = new BMessage(kMsgEditTask);
	message->AddString("task", task.taskId.c_str());
	return message;
}

class TaskRow : public mk::TableRow {
public:
	TaskRow(const DisplayTask& task, const std::string& detail)
		: mk::TableRow(task.title.c_str(), detail.c_str(), EditTaskMessage(task)),
		fColor(Theme::FromHex(task.color)), fCompleted(task.completed)
	{
		SetAccessory(mk::B_ROW_CHEVRON);
	}

	void DrawItem(BView* owner, BRect frame, bool complete) override
	{
		BRect check = frame;
		check.right = check.left + mk::TargetSize() - 1;
		frame.left = check.right + 1;
		TableRow::DrawItem(owner, frame, complete);
		owner->PushState();
		rgb_color background = ui_color(B_LIST_BACKGROUND_COLOR);
		if (IsSelected()) {
			const auto& skin = mk::CurrentSkin();
			mk::FillGradient(owner, check, skin.fillTop, skin.fillBottom);
		} else {
			owner->SetHighColor(background);
			owner->FillRect(check);
		}
		owner->SetHighColor(Theme::Mix(background, ui_color(B_LIST_ITEM_TEXT_COLOR), 0.16f));
		owner->StrokeLine(check.LeftBottom(), check.RightBottom());
		Drawing::DrawCheck(owner, BPoint((check.left + check.right) / 2,
			(check.top + check.bottom) / 2), be_plain_font->Size() * 0.8f,
			fColor, fCompleted);
		owner->PopState();
	}

	bool Completed() const { return fCompleted; }
private:
	rgb_color fColor;
	bool fCompleted;
};

class TouchTable : public mk::TableView {
public:
	explicit TouchTable(const char* name) : TableView(name) {}

	status_t Invoke(BMessage* message = nullptr) override
	{
		// BListView calls InvokeNotify when selection moves under a finger
		// or an arrow key. Only a released tap or Enter activates a row.
		if (InvokeKind() == B_CONTROL_MODIFIED)
			return BListView::Invoke(message);
		return TableView::Invoke(message);
	}
};

class PlannerList : public TouchTable {
public:
	PlannerList() : TouchTable("plannerRows") {}

	void Draw(BRect update) override
	{
		TableView::Draw(update);
		const int32 count = CountItems();
		if (count == 0)
			return;
		BRect sheet = update;
		float first = ItemFrame(count - 1).bottom + 1;
		sheet.top = std::max(first, sheet.top);
		if (!sheet.IsValid())
			return;
		const auto& skin = mk::CurrentSkin();
		rgb_color paper = Theme::Mix(ui_color(B_LIST_BACKGROUND_COLOR), skin.pageTop, 0.3f);
		SetHighColor(paper);
		FillRect(sheet);
		float spacing = ceilf(mk::TargetSize() * 0.65f);
		SetHighColor(Theme::Mix(paper, skin.pageSecondary, 0.15f));
		float y = first + ceilf((sheet.top - first + 1) / spacing) * spacing - 1;
		for (; y <= sheet.bottom; y += spacing)
			StrokeLine(BPoint(sheet.left, y), BPoint(sheet.right, y));
		float margin = floorf(mk::TargetSize());
		SetHighColor(Theme::Mix(paper, Theme::Accent(), 0.2f));
		StrokeLine(BPoint(margin, sheet.top), BPoint(margin, sheet.bottom));
	}

	void MouseDown(BPoint point) override
	{
		fPendingTask.clear();
		auto* task = CheckAt(point);
		if (task != nullptr) {
			fPendingTask = task->Message()->GetString("task", "");
			SetMouseEventMask(B_POINTER_EVENTS);
		} else
			TableView::MouseDown(point);
	}

	void MouseUp(BPoint point) override
	{
		if (fPendingTask.empty()) {
			TableView::MouseUp(point);
			return;
		}
		auto* task = CheckAt(point);
		if (task != nullptr && task->Message()->GetString("task", "") == fPendingTask) {
			BMessage toggle(kMsgToggleTask);
			toggle.AddString("task", fPendingTask.c_str());
			toggle.AddBool("completed", !task->Completed());
			Window()->PostMessage(&toggle);
		}
		fPendingTask.clear();
	}

private:
	TaskRow* CheckAt(BPoint point)
	{
		return point.x < mk::TargetSize()
			? dynamic_cast<TaskRow*>(RowAt(IndexOf(point))) : nullptr;
	}
	std::string fPendingTask;
};

std::string
TaskDetail(const DisplayTask& task, bool showList)
{
	std::string detail;
	if (task.hasDue) {
		detail = Format::RelativeDay(task.dueDate);
		if (task.dueHasTime)
			detail += " · " + Format::Time(task.dueTime);
	}
	if (showList) {
		if (!detail.empty())
			detail += " · ";
		detail += task.listName;
	}
	return detail.empty() ? "No due date" : detail;
}
}

MobilePlanner::MobilePlanner(MainWindow* owner)
	: BGroupView("mobilePlanner", B_VERTICAL, 0), fOwner(owner)
{
	SetFlags(Flags() | B_FRAME_EVENTS);
	fDates = new DateNavigator(owner->CurrentDate(), false, 0, owner, kMsgGoToDate, true);
	fList = new PlannerList();
	fBody = new BGroupView("plannerBody", B_VERTICAL, 0);
	fLists = new TouchTable("taskLists");
	fListSidebar = mk::Scroll("taskListScroll", fLists);
	fListSidebar->SetExplicitMaxSize(BSize(mk::TargetSize() * 6, B_SIZE_UNLIMITED));
	fListSidebar->SetExplicitMinSize(BSize(mk::TargetSize() * 4, 0));
	BLayoutBuilder::Group<>(fBody).Add(fDates, 0).Add(fListSidebar, 0.5f)
		.Add(mk::Scroll("plannerScroll", fList), 1);
	fListSidebar->Hide();

	fTaskHeader = new mk::MetalBar("tasksBar");
	fFilter = new mk::SegmentedControl("taskStatus", new BMessage(kFilter));
	fFilter->AddSegment("Open");
	fFilter->AddSegment("Completed");
	BLayoutBuilder::Group<>(fTaskHeader).Add(new mk::TitleView("taskTitle", "Tasks"), 1)
		.Add(fFilter);

	auto* bar = new mk::MetalBar("plannerBar", mk::B_MOBILE_BOTTOM_BAR);
	fMore = new mk::MetalButton("more", NULL, new BMessage(kMore),
		mk::B_ROUNDED_BUTTON, mk::B_GLYPH_MORE);
	fMore->SetToolTip("Calendar actions");
	fTabs = new mk::SegmentedControl("plannerTabs", new BMessage(kPage));
	fTabs->AddSegment("Calendar");
	fTabs->AddSegment("Tasks");
	fAdd = new mk::MetalButton("newItem", NULL, new BMessage(kAdd),
		mk::B_ROUNDED_BUTTON, mk::B_GLYPH_ADD);
	BLayoutBuilder::Group<>(bar).Add(fMore).AddGlue().Add(fTabs).AddGlue().Add(fAdd);
	BLayoutBuilder::Group<>(this).Add(fTaskHeader).Add(fBody, 1).Add(bar);
	fTaskHeader->Hide();
}

void
MobilePlanner::AttachedToWindow()
{
	BGroupView::AttachedToWindow();
	for (BControl* control : std::initializer_list<BControl*>{fFilter, fTabs, fAdd, fMore})
		control->SetTarget(this);
	fList->SetTarget(fOwner);
	fLists->SetTarget(this);
}

void
MobilePlanner::FrameResized(float width, float height)
{
	BGroupView::FrameResized(width, height);
	bool tablet = width >= 15 * mk::TargetSize() && height >= 9 * mk::TargetSize();
	if (tablet == fTablet)
		return;
	fTablet = tablet;
	fDates->SetMonthLayout(tablet);
	auto* layout = fBody->GroupLayout();
	layout->SetOrientation(tablet ? B_HORIZONTAL : B_VERTICAL);
	layout->SetItemWeight(0, tablet ? 0.9f : 0);
	if (fTasks) {
		if (tablet)
			fListSidebar->Show();
		else
			fListSidebar->Hide();
	}
	if (!IsHidden())
		Reload();
}

void
MobilePlanner::ShowTasks(bool tasks)
{
	if (fTasks != tasks) {
		fTasks = tasks;
		if (fTablet) {
			if (tasks)
				fListSidebar->Show();
			else
				fListSidebar->Hide();
		}
		if (tasks) {
			fDates->Hide();
			fTaskHeader->Show();
		} else {
			fTaskHeader->Hide();
			fDates->Show();
		}
	}
	fTabs->Select(tasks ? 1 : 0);
	fAdd->SetToolTip(tasks ? "New task" : "New event");
	fAdd->SetEnabled(true);
	Reload(true);
}

void
MobilePlanner::Reload(bool resetScroll)
{
	float scroll = resetScroll ? 0 : fList->Bounds().top;
	fList->RemoveRows();
	fOwner->SetTitle(fTasks ? "airCal — Tasks"
		: ("airCal — " + Format::DateMedium(fOwner->CurrentDate())).c_str());
	if (fTasks)
		LoadTasks();
	else
		LoadAgenda();
	if (auto* bar = fList->ScrollBar(B_VERTICAL))
		bar->SetValue(scroll);
}

Date
MobilePlanner::Step(int direction) const
{
	Date date = fOwner->CurrentDate();
	if (fTasks)
		return date;
	return fTablet ? date.AddMonths(direction) : date.AddDays(direction * 7);
}

void
MobilePlanner::LoadListSidebar()
{
	Store& store = TheApp()->Store();
	fLists->RemoveRows();
	AddSection(fLists, "Task lists");
	auto addList = [this](const std::string& id, const char* name, rgb_color color, bool writable) {
		auto* choose = new BMessage(kList);
		choose->AddString("list", id.c_str());
		auto* row = new mk::TableRow(name, NULL, choose);
		row->SetGlyph(mk::B_GLYPH_FOLDER, color);
		if (id == fSelectedList) {
			row->SetAccessory(mk::B_ROW_CHECK);
			fAdd->SetEnabled(writable);
		}
		fLists->AddItem(row);
	};
	std::vector<Calendar> lists;
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		for (const auto& calendar : store.Calendars()) {
			auto* account = store.FindAccount(calendar->accountId);
			if (calendar->tasks && calendar->visible && account != nullptr && account->enabled)
				lists.push_back(*calendar);
		}
	}
	if (std::none_of(lists.begin(), lists.end(), [this](const Calendar& list) {
		return list.id == fSelectedList;
	}))
		fSelectedList.clear();
	addList("", "All lists", mk::CurrentSkin().pageSecondary, true);
	for (const auto& list : lists)
		addList(list.id, list.name.c_str(), Theme::FromHex(list.color), !list.readOnly);
}

void
MobilePlanner::LoadTasks()
{
	bool completed = fFilter->Value() == 1;
	fList->SetEmptyText(completed ? "No completed tasks" : "No open tasks");
	Store& store = TheApp()->Store();
	if (fTablet)
		LoadListSidebar();
	else
		fAdd->SetEnabled(true);
	auto tasks = store.TaskList(fTablet ? fSelectedList : "", completed);
	std::string lastList;
	for (const auto& task : tasks) {
		if (task.completed != completed)
			continue;
		if (lastList != task.calendarId) {
			AddSection(fList, task.listName);
			lastList = task.calendarId;
		}
		fList->AddItem(new TaskRow(task, TaskDetail(task, false)));
	}
}

void
MobilePlanner::LoadAgenda()
{
	Date date = fOwner->CurrentDate();
	Store& store = TheApp()->Store();
	Preferences prefs;
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		prefs = store.Prefs();
	}
	fDates->SetDate(date);
	fDates->SetWeekStart(prefs.weekStart);
	Date first = fDates->FirstDate(), last = first.AddDays(fDates->DayCount() - 1);
	auto events = store.EventsBetween(StartOfDay(first), StartOfDay(last.AddDays(1)));
	std::map<int64_t, uint32> markers;
	int eventCount = 0;
	int taskCount = 0;
	for (const auto& event : events) {
		int64_t end = std::min(last, event.endDate).DayNumber();
		for (int64_t day = std::max(first, event.startDate).DayNumber(); day <= end; ++day)
			markers.emplace(day, event.color);
		if (date < event.startDate || date > event.endDate)
			continue;
		++eventCount;
		auto* edit = new BMessage(kMsgEditEvent);
		edit->AddString("event", event.eventId.c_str());
		edit->AddString("override", event.overrideId.c_str());
		edit->AddInt64("original", event.originalStart);
		edit->AddString("originalDate", event.originalDate.ToIso().c_str());
		std::string detail = event.allDay ? "All day" : Format::TimeRange(event.start, event.end);
		if (!event.location.empty())
			detail += " · " + event.location;
		auto* row = new mk::TableRow(event.title.c_str(), detail.c_str(), edit);
		row->SetGlyph(mk::B_GLYPH_CLOCK, Theme::FromHex(event.color));
		row->SetAccessory(mk::B_ROW_CHEVRON);
		fList->AddItem(row);
	}
	if (prefs.showTasksInCalendar) {
		for (const auto& task : store.TasksBetween(first, last, prefs.showCompleted)) {
			markers.emplace(task.dueDate.DayNumber(), task.color);
			if (task.dueDate == date) {
				fList->AddItem(new TaskRow(task, TaskDetail(task, true)));
				++taskCount;
			}
		}
	}
	fDates->SetMarkers(std::move(markers));
	std::string summary;
	if (eventCount != 0)
		summary = std::to_string(eventCount) + (eventCount == 1 ? " event" : " events");
	if (taskCount != 0) {
		if (!summary.empty())
			summary += " · ";
		summary += std::to_string(taskCount) + (taskCount == 1 ? " task due" : " tasks due");
	}
	fList->AddItem(new AgendaHeading(date, summary.empty() ? "Nothing scheduled" : summary), 0);
}

void
MobilePlanner::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kPage:
			ShowTasks(fTabs->Value() == 1);
			break;
		case kList:
			fSelectedList = message->GetString("list", "");
			Reload(true);
			break;
		case kFilter:
			Reload(true);
			break;
		case kAdd:
		{
			BMessage create(fTasks ? kMsgNewTask : kMsgNewEvent);
			if (fTasks && fTablet)
				create.AddString("list", fSelectedList.c_str());
			fOwner->PostMessage(&create);
			break;
		}
		case kMore:
		{
			auto* sheet = new mk::ActionSheet(fOwner, "airCal");
			if (!fTasks)
				sheet->AddAction("Go to Today", BMessage(kMsgGoToday));
			sheet->AddAction("Sync now", BMessage(kMsgSyncNow));
			sheet->AddAction("Accounts…", BMessage(kMsgShowAccounts));
			sheet->Open();
			break;
		}
		default:
			BGroupView::MessageReceived(message);
	}
}
