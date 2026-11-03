// The month view: weeks as rows, multi-day events as bars across the days,
// timed events and tasks as lines. Drag items between days; double-click a
// day to add an event; the wheel moves by month.
#include "App.h"
#include "Drawing.h"
#include "Format.h"
#include "MainWindow.h"
#include "Messages.h"
#include "Theme.h"
#include "Views.h"

#include <Window.h>

#include <algorithm>
#include <cmath>

using namespace aircal;

namespace {

const float kWeekdayHeight = 26;


struct MonthItem {
	bool isTask = false;
	DisplayEvent event;
	DisplayTask task;
	int week = 0;
	int firstDay = 0;	// column within the week
	int lastDay = 0;
	int lane = 0;
	bool bar = false;	// all-day / multi-day bar
	BRect frame;
};


class MonthView : public CalendarView {
public:
	MonthView();

	void Reload() override;
	void Title(std::string& bold, std::string& light) const override;
	Date Step(int direction) const override;
	void Draw(BRect update) override;
	void FrameResized(float width, float height) override;
	void MouseDown(BPoint where) override;
	void MouseMoved(BPoint where, uint32 transit, const BMessage* drag) override;
	void MouseUp(BPoint where) override;
	void KeyDown(const char* bytes, int32 count) override;
	void MessageReceived(BMessage* message) override;

private:
	void Layout();
	BRect CellFrame(int week, int day) const;
	bool CellAt(BPoint where, int& week, int& day) const;
	float LaneHeight() const;
	MainWindow* Main() const { return static_cast<MainWindow*>(Window()); }

	Date fFirst;		// first shown day
	int fWeeks = 6;
	int fWeekStart = 0;
	std::vector<DisplayEvent> fEvents;
	std::vector<DisplayTask> fTasks;
	std::vector<MonthItem> fItems;
	std::vector<int> fMore;		// per cell: hidden items
	int fDragIndex = -1;
	bool fDragging = false;
	BPoint fPress;
	int fDropWeek = -1;
	int fDropDay = -1;
	bigtime_t fLastWheel = 0;
};


MonthView::MonthView()
	:
	CalendarView("month", B_NAVIGABLE)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}


void
MonthView::Reload()
{
	Store& store = TheApp()->Store();
	bool showTasks, showCompleted;
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		fWeekStart = store.Prefs().weekStart;
		showTasks = store.Prefs().showTasksInCalendar;
		showCompleted = store.Prefs().showCompleted;
	}
	Date firstOfMonth(fAnchor.year, fAnchor.month, 1);
	int delta = (firstOfMonth.Weekday() - fWeekStart + 7) % 7;
	fFirst = firstOfMonth.AddDays(-delta);
	int days = delta + DaysInMonth(fAnchor.year, fAnchor.month);
	fWeeks = (days + 6) / 7;
	if (fWeeks < 5)
		fWeeks = 5;
	Date last = fFirst.AddDays(fWeeks * 7 - 1);
	fEvents = store.EventsBetween(StartOfDay(fFirst), StartOfDay(last.AddDays(1)));
	fTasks.clear();
	if (showTasks)
		fTasks = store.TasksBetween(fFirst, last, showCompleted, false);
	Layout();
	Invalidate();
}


void
MonthView::Title(std::string& bold, std::string& light) const
{
	bold = Format::MonthLong(fAnchor.month);
	light = std::to_string(fAnchor.year);
}


Date
MonthView::Step(int direction) const
{
	Date first(fAnchor.year, fAnchor.month, 1);
	return first.AddMonths(direction);
}


BRect
MonthView::CellFrame(int week, int day) const
{
	BRect bounds = Bounds();
	float cellWidth = bounds.Width() / 7;
	float cellHeight = (bounds.Height() - kWeekdayHeight) / fWeeks;
	return BRect(floorf(day * cellWidth), floorf(kWeekdayHeight + week * cellHeight),
		floorf((day + 1) * cellWidth) - 1,
		floorf(kWeekdayHeight + (week + 1) * cellHeight) - 1);
}


bool
MonthView::CellAt(BPoint where, int& week, int& day) const
{
	BRect bounds = Bounds();
	if (where.y < kWeekdayHeight)
		return false;
	float cellWidth = bounds.Width() / 7;
	float cellHeight = (bounds.Height() - kWeekdayHeight) / fWeeks;
	day = std::max(0, std::min(6, (int)(where.x / cellWidth)));
	week = std::max(0, std::min(fWeeks - 1, (int)((where.y - kWeekdayHeight) / cellHeight)));
	return true;
}


float
MonthView::LaneHeight() const
{
	BFont font(be_plain_font);
	return ceilf(Drawing::TextHeight(font) * 0.92f + 5);
}


void
MonthView::FrameResized(float width, float height)
{
	Layout();
	Invalidate();
}


void
MonthView::Layout()
{
	fItems.clear();
	fMore.assign(fWeeks * 7, 0);
	if (Bounds().Width() < 10)
		return;
	float lane = LaneHeight();
	BRect cell = CellFrame(0, 0);
	float numberHeight = lane + 4;
	int maxLanes = std::max(1, (int)((cell.Height() - numberHeight - 2) / lane));

	for (int week = 0; week < fWeeks; week++) {
		Date weekStart = fFirst.AddDays(week * 7);
		std::vector<MonthItem> items;
		for (const DisplayEvent& event : fEvents) {
			int64_t first = event.startDate.DayNumber() - weekStart.DayNumber();
			int64_t last = event.endDate.DayNumber() - weekStart.DayNumber();
			if (last < 0 || first > 6)
				continue;
			MonthItem item;
			item.event = event;
			item.week = week;
			item.firstDay = (int)std::max<int64_t>(0, first);
			item.lastDay = (int)std::min<int64_t>(6, last);
			item.bar = event.allDay || last > first;
			items.push_back(item);
		}
		for (const DisplayTask& task : fTasks) {
			int64_t day = task.dueDate.DayNumber() - weekStart.DayNumber();
			if (day < 0 || day > 6)
				continue;
			MonthItem item;
			item.isTask = true;
			item.task = task;
			item.week = week;
			item.firstDay = item.lastDay = (int)day;
			items.push_back(item);
		}
		// Bars first (longest first), then timed by time, then tasks.
		std::stable_sort(items.begin(), items.end(), [](const MonthItem& a,
				const MonthItem& b) {
			auto rank = [](const MonthItem& item) {
				return item.bar ? 0 : (item.isTask ? 2 : 1);
			};
			if (rank(a) != rank(b))
				return rank(a) < rank(b);
			if (a.bar) {
				if (a.firstDay != b.firstDay)
					return a.firstDay < b.firstDay;
				return a.lastDay - a.firstDay > b.lastDay - b.firstDay;
			}
			if (!a.isTask && !b.isTask)
				return a.event.start < b.event.start;
			if (a.isTask && b.isTask && a.task.dueHasTime != b.task.dueHasTime)
				return a.task.dueHasTime;
			return false;
		});
		std::vector<std::vector<bool>> used;
		for (MonthItem& item : items) {
			int laneIndex = 0;
			while (true) {
				if (laneIndex >= (int)used.size())
					used.push_back(std::vector<bool>(7, false));
				bool free = true;
				for (int d = item.firstDay; d <= item.lastDay; d++) {
					if (used[laneIndex][d])
						free = false;
				}
				if (free)
					break;
				laneIndex++;
			}
			for (int d = item.firstDay; d <= item.lastDay; d++)
				used[laneIndex][d] = true;
			item.lane = laneIndex;
		}
		// Hide what doesn't fit (keeping a lane for "+N more").
		std::vector<int> lanesNeeded(7, 0);
		for (MonthItem& item : items) {
			for (int d = item.firstDay; d <= item.lastDay; d++)
				lanesNeeded[d] = std::max(lanesNeeded[d], item.lane + 1);
		}
		for (MonthItem& item : items) {
			bool hidden = false;
			for (int d = item.firstDay; d <= item.lastDay; d++) {
				int limit = lanesNeeded[d] > maxLanes ? maxLanes - 1 : maxLanes;
				if (item.lane >= limit)
					hidden = true;
			}
			if (hidden) {
				for (int d = item.firstDay; d <= item.lastDay; d++)
					fMore[week * 7 + d]++;
				continue;
			}
			BRect left = CellFrame(week, item.firstDay);
			BRect right = CellFrame(week, item.lastDay);
			float top = left.top + numberHeight + item.lane * lane;
			item.frame = BRect(left.left + 3, top, right.right - 3, top + lane - 2);
			fItems.push_back(item);
		}
	}
}


void
MonthView::Draw(BRect update)
{
	BRect bounds = Bounds();
	SetHighColor(Theme::Background());
	FillRect(update);
	MainWindow* main = Main();
	Date today = Today();
	BFont font(be_plain_font);
	BFont small(font);
	small.SetSize(font.Size() * 0.9f);
	BFont bold(be_bold_font);
	bold.SetSize(font.Size() * 0.9f);
	BFont number(be_bold_font);
	number.SetSize(font.Size() * 1.0f);

	// Weekday names.
	SetHighColor(Theme::Header());
	FillRect(BRect(0, 0, bounds.right, kWeekdayHeight - 1));
	SetFont(&small);
	SetDrawingMode(B_OP_ALPHA);
	for (int d = 0; d < 7; d++) {
		BRect cell = CellFrame(0, d);
		std::string name = Format::WeekdayShort((fWeekStart + d) % 7);
		SetHighColor(Theme::SecondaryText());
		DrawString(name.c_str(), BPoint(cell.right - 8 - StringWidth(name.c_str()),
			kWeekdayHeight - 8));
	}
	SetDrawingMode(B_OP_COPY);

	// Cells.
	for (int week = 0; week < fWeeks; week++) {
		for (int d = 0; d < 7; d++) {
			BRect cell = CellFrame(week, d);
			if (!cell.Intersects(update))
				continue;
			Date date = fFirst.AddDays(week * 7 + d);
			bool inMonth = date.month == fAnchor.month;
			int weekday = date.Weekday();
			if (!inMonth)
				SetHighColor(Theme::OtherMonth());
			else if (weekday == 0 || weekday == 6)
				SetHighColor(Theme::Weekend());
			else
				SetHighColor(Theme::Background());
			FillRect(cell);
			if (fDragging && week == fDropWeek && d == fDropDay) {
				SetHighColor(Theme::Selection());
				FillRect(cell);
			}
			// Day number, top right; the 1st gets the month name.
			std::string text = std::to_string(date.day);
			if (date.day == 1)
				text = Format::MonthShort(date.month) + " 1";
			SetFont(&number);
			float width = StringWidth(text.c_str());
			font_height height;
			number.GetHeight(&height);
			BPoint at(cell.right - 8 - width, cell.top + height.ascent + 5);
			SetDrawingMode(B_OP_ALPHA);
			if (date == today) {
				float r = (height.ascent + height.descent) / 2 + 3;
				BRect pill(at.x - 6, at.y - height.ascent - 3, at.x + width + 6,
					at.y + height.descent + 2);
				if (pill.Width() < pill.Height())
					pill.left = pill.right - pill.Height();
				Drawing::FillRound(this, pill, r, Theme::Accent());
				SetHighColor(Theme::AccentText());
			} else
				SetHighColor(inMonth ? Theme::Text() : Theme::TertiaryText());
			DrawString(text.c_str(), at);
			if (fMore[week * 7 + d] > 0) {
				SetFont(&small);
				SetHighColor(Theme::SecondaryText());
				std::string more = "+" + std::to_string(fMore[week * 7 + d]) + " more";
				DrawString(more.c_str(), BPoint(cell.left + 8, cell.bottom - 5));
			}
			SetDrawingMode(B_OP_COPY);
		}
	}
	// Grid lines.
	SetHighColor(Theme::GridLine());
	for (int week = 0; week <= fWeeks; week++) {
		float y = week == fWeeks ? bounds.bottom : CellFrame(week, 0).top;
		StrokeLine(BPoint(0, y), BPoint(bounds.right, y));
	}
	for (int d = 1; d < 7; d++) {
		float x = CellFrame(0, d).left;
		StrokeLine(BPoint(x, kWeekdayHeight), BPoint(x, bounds.bottom));
	}

	// Items.
	SetDrawingMode(B_OP_ALPHA);
	for (size_t i = 0; i < fItems.size(); i++) {
		const MonthItem& item = fItems[i];
		if (!item.frame.Intersects(update))
			continue;
		BRect frame = item.frame;
		if (fDragging && (int)i == fDragIndex) {
			Drawing::FillRound(this, frame, 4, Theme::WithAlpha(Theme::Selection(), 200));
			continue;
		}
		if (item.isTask) {
			bool selected = main->CurrentSelection().IsTask(item.task.taskId);
			if (selected)
				Drawing::FillRound(this, frame, 4, Theme::EventFill(item.task.color));
			rgb_color color = item.task.priority >= 2 ? Theme::Priority(item.task.priority)
				: Theme::FromHex(item.task.color);
			Drawing::DrawCheck(this, BPoint(frame.left + 7, (frame.top + frame.bottom) / 2),
				5, color, item.task.completed);
			SetFont(&small);
			SetHighColor(item.task.completed ? Theme::TertiaryText() : Theme::Text());
			std::string text = item.task.title;
			if (item.task.dueHasTime)
				text = Format::Time(item.task.dueTime, true) + " " + text;
			text = Drawing::Fit(this, text, frame.Width() - 18);
			DrawString(text.c_str(), BPoint(frame.left + 16, Drawing::Baseline(small, frame)));
			continue;
		}
		bool selected = main->CurrentSelection().IsEvent(item.event);
		if (item.bar) {
			Drawing::FillRound(this, frame, 4, Theme::AllDayFill(item.event.color, selected));
			SetFont(&bold);
			SetHighColor(Theme::AllDayText(item.event.color, selected));
			std::string title = item.event.title;
			if (!item.event.allDay && item.firstDay == (int)(item.event.startDate.DayNumber()
					- fFirst.AddDays(item.week * 7).DayNumber()))
				title = Format::Time(item.event.start, true) + " " + title;
			DrawString(Drawing::Fit(this, title, frame.Width() - 10).c_str(),
				BPoint(frame.left + 6, Drawing::Baseline(bold, frame)));
			continue;
		}
		if (selected)
			Drawing::FillRound(this, frame, 4, Theme::EventFill(item.event.color, true));
		Drawing::FillCircle(this, BPoint(frame.left + 6, (frame.top + frame.bottom) / 2),
			3.5f, selected ? Theme::EventText(item.event.color, true)
				: Theme::EventBar(item.event.color));
		SetFont(&small);
		std::string time = Format::Time(item.event.start, true);
		float timeWidth = StringWidth(time.c_str());
		SetHighColor(selected ? Theme::EventText(item.event.color, true) : Theme::Text());
		SetFont(&small);
		std::string title = Drawing::Fit(this, item.event.title,
			frame.Width() - 18 - timeWidth - 6);
		DrawString(title.c_str(), BPoint(frame.left + 14, Drawing::Baseline(small, frame)));
		SetHighColor(selected ? Theme::EventText(item.event.color, true)
			: Theme::SecondaryText());
		DrawString(time.c_str(), BPoint(frame.right - timeWidth - 2,
			Drawing::Baseline(small, frame)));
	}
	SetDrawingMode(B_OP_COPY);
}


void
MonthView::MouseDown(BPoint where)
{
	MakeFocus(true);
	BMessage* current = Window()->CurrentMessage();
	int32 clicks = current != nullptr ? current->GetInt32("clicks", 1) : 1;
	int32 buttons = current != nullptr
		? current->GetInt32("buttons", B_PRIMARY_MOUSE_BUTTON) : B_PRIMARY_MOUSE_BUTTON;
	MainWindow* main = Main();
	BPoint screen = ConvertToScreen(where);
	fPress = where;
	fDragIndex = -1;
	fDragging = false;
	for (int i = (int)fItems.size() - 1; i >= 0; i--) {
		MonthItem& item = fItems[i];
		if (!item.frame.Contains(where))
			continue;
		if (item.isTask) {
			if (where.x < item.frame.left + 14)
				main->ToggleTask(item.task.taskId, !item.task.completed);
			else if (buttons & B_SECONDARY_MOUSE_BUTTON)
				main->ShowTaskMenu(item.task.taskId, screen);
			else if (clicks >= 2)
				main->EditTask(item.task.taskId, screen);
			else {
				main->SelectTask(item.task.taskId);
				fDragIndex = i;
				SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
			}
			return;
		}
		if (buttons & B_SECONDARY_MOUSE_BUTTON) {
			main->ShowEventMenu(item.event, screen);
			return;
		}
		if (clicks >= 2) {
			main->EditEvent(item.event, screen);
			return;
		}
		main->SelectEvent(item.event);
		if (!item.event.readOnly) {
			fDragIndex = i;
			SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
		}
		return;
	}
	int week, day;
	if (!CellAt(where, week, day))
		return;
	Date date = fFirst.AddDays(week * 7 + day);
	BRect cell = CellFrame(week, day);
	// The "+N more" line and the day number open the day.
	if (where.y > cell.bottom - 18 && fMore[week * 7 + day] > 0) {
		BMessage message(kMsgGoToDate);
		message.AddString("date", date.ToIso().c_str());
		message.AddString("view", "day");
		Window()->PostMessage(&message);
		return;
	}
	if (where.y < cell.top + LaneHeight() + 2 && where.x > cell.right - 40) {
		BMessage message(kMsgGoToDate);
		message.AddString("date", date.ToIso().c_str());
		message.AddString("view", "day");
		Window()->PostMessage(&message);
		return;
	}
	if (clicks >= 2) {
		int duration;
		{
			std::lock_guard<std::recursive_mutex> lock(TheApp()->Store().Mutex());
			duration = TheApp()->Store().Prefs().defaultDuration;
		}
		Instant start = ToInstant(DateTime(date, 9, 0));
		main->NewEventAt(start, start + duration * 60, false, date, screen);
		return;
	}
	main->SetSelection(Selection());
	main->SelectDate(date);
}


void
MonthView::MouseMoved(BPoint where, uint32 transit, const BMessage* drag)
{
	if (fDragIndex < 0)
		return;
	if (!fDragging && (fabs(where.x - fPress.x) > 5 || fabs(where.y - fPress.y) > 5))
		fDragging = true;
	if (!fDragging)
		return;
	int week, day;
	if (CellAt(where, week, day) && (week != fDropWeek || day != fDropDay)) {
		fDropWeek = week;
		fDropDay = day;
		Invalidate();
	}
}


void
MonthView::MouseUp(BPoint where)
{
	if (fDragIndex >= 0 && fDragging && fDropWeek >= 0) {
		MonthItem item = fItems[fDragIndex];
		Date target = fFirst.AddDays(fDropWeek * 7 + fDropDay);
		fDragIndex = -1;
		fDragging = false;
		fDropWeek = fDropDay = -1;
		Invalidate();
		if (item.isTask) {
			Store& store = TheApp()->Store();
			std::string calendar;
			{
				std::lock_guard<std::recursive_mutex> lock(store.Mutex());
				aircal::UndoScope undo(TheApp()->Undo(), TheApp()->Store(), "Move Task");
				Task* task = store.FindTask(item.task.taskId);
				if (task != nullptr && task->dueDate != target) {
					Task edited = *task;
					if (edited.dueHasTime) {
						DateTime wall = ToWall(edited.dueTime);
						wall.date = target;
						edited.dueTime = ToInstant(wall);
					}
					edited.dueDate = target;
					calendar = edited.calendarId;
					store.UpdateTask(edited);
				}
			}
			if (!calendar.empty()) {
				TheApp()->RequestPush(calendar);
				TheApp()->ScheduleSave();
			}
			return;
		}
		// The day the item was grabbed on moves to the target day.
		int week, day;
		int grabbedOffset = 0;
		if (CellAt(fPress, week, day)) {
			Date grabbed = fFirst.AddDays(week * 7 + day);
			grabbedOffset = (int)(grabbed.DayNumber() - item.event.startDate.DayNumber());
		}
		Date newStart = target.AddDays(-grabbedOffset);
		if (newStart == item.event.startDate)
			return;
		if (item.event.allDay) {
			int days = (int)(item.event.endDate.DayNumber()
				- item.event.startDate.DayNumber()) + 1;
			Main()->MoveEvent(item.event, 0, 0, true, newStart, days);
		} else {
			DateTime wall = ToWall(item.event.start);
			wall.date = newStart;
			Instant start = ToInstant(wall);
			Main()->MoveEvent(item.event, start, start + (item.event.end
				- item.event.start), false, newStart, 0);
		}
		return;
	}
	fDragIndex = -1;
	fDragging = false;
	fDropWeek = fDropDay = -1;
}


void
MonthView::KeyDown(const char* bytes, int32 count)
{
	switch (bytes[0]) {
		case B_DELETE:
		case B_BACKSPACE:
			Window()->PostMessage(kMsgDeleteSelection);
			break;
		case B_ENTER:
			Window()->PostMessage('edsl');
			break;
		case B_LEFT_ARROW:
		case B_UP_ARROW:
		case B_PAGE_UP:
			Window()->PostMessage(kMsgGoPrevious);
			break;
		case B_RIGHT_ARROW:
		case B_DOWN_ARROW:
		case B_PAGE_DOWN:
			Window()->PostMessage(kMsgGoNext);
			break;
		case 't':
		case 'T':
			Window()->PostMessage(kMsgGoToday);
			break;
		default:
			CalendarView::KeyDown(bytes, count);
	}
}


void
MonthView::MessageReceived(BMessage* message)
{
	if (message->what == B_MOUSE_WHEEL_CHANGED) {
		float delta = 0;
		if (message->FindFloat("be:wheel_delta_y", &delta) == B_OK && delta != 0) {
			bigtime_t now = system_time();
			if (now - fLastWheel > 250000) {
				fLastWheel = now;
				Window()->PostMessage(delta > 0 ? kMsgGoNext : kMsgGoPrevious);
			}
		}
		return;
	}
	CalendarView::MessageReceived(message);
}


// --- YearView ------------------------------------------------------------------------

class YearView : public CalendarView {
public:
	YearView();

	void Reload() override;
	void Title(std::string& bold, std::string& light) const override;
	Date Step(int direction) const override;
	void Draw(BRect update) override;
	void MouseDown(BPoint where) override;
	void KeyDown(const char* bytes, int32 count) override;
	void FrameResized(float, float) override { Invalidate(); }

private:
	BRect MonthFrame(int month) const;
	void Grid(int& columns, int& rows) const;
	bool DayAt(BPoint where, Date& date) const;

	std::map<int64_t, int> fCounts;		// day number -> events
	std::map<int64_t, std::vector<uint32_t>> fColors;
	int fWeekStart = 0;
	int fMax = 1;
};


YearView::YearView()
	:
	CalendarView("year", B_NAVIGABLE)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}


void
YearView::Reload()
{
	Store& store = TheApp()->Store();
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		fWeekStart = store.Prefs().weekStart;
	}
	Date first(fAnchor.year, 1, 1);
	Date last(fAnchor.year + 1, 1, 1);
	std::vector<DisplayEvent> events = store.EventsBetween(StartOfDay(first),
		StartOfDay(last));
	fCounts.clear();
	fColors.clear();
	fMax = 1;
	for (const DisplayEvent& event : events) {
		for (int64_t day = event.startDate.DayNumber();
				day <= event.endDate.DayNumber() && day - event.startDate.DayNumber() < 366;
				day++) {
			int& count = fCounts[day];
			count++;
			fMax = std::max(fMax, count);
			std::vector<uint32_t>& colors = fColors[day];
			if (colors.size() < 3 && std::find(colors.begin(), colors.end(),
					event.color) == colors.end())
				colors.push_back(event.color);
		}
	}
	Invalidate();
}


void
YearView::Title(std::string& bold, std::string& light) const
{
	bold = std::to_string(fAnchor.year);
	light = "";
}


Date
YearView::Step(int direction) const
{
	return Date(fAnchor.year + direction, fAnchor.month, 1);
}


void
YearView::Grid(int& columns, int& rows) const
{
	BRect bounds = Bounds();
	float aspect = bounds.Width() / std::max(1.0f, bounds.Height());
	if (aspect > 1.9f) {
		columns = 6;
		rows = 2;
	} else if (aspect > 1.05f) {
		columns = 4;
		rows = 3;
	} else {
		columns = 3;
		rows = 4;
	}
}


BRect
YearView::MonthFrame(int month) const
{
	int columns, rows;
	Grid(columns, rows);
	BRect bounds = Bounds().InsetByCopy(16, 12);
	float width = bounds.Width() / columns;
	float height = bounds.Height() / rows;
	int column = month % columns;
	int row = month / columns;
	return BRect(bounds.left + column * width + 8, bounds.top + row * height + 6,
		bounds.left + (column + 1) * width - 8, bounds.top + (row + 1) * height - 6);
}


void
YearView::Draw(BRect update)
{
	SetHighColor(Theme::Background());
	FillRect(update);
	Date today = Today();
	BFont font(be_plain_font);
	BFont title(be_bold_font);
	title.SetSize(font.Size() * 1.15f);
	BFont small(font);
	SetDrawingMode(B_OP_ALPHA);
	for (int month = 0; month < 12; month++) {
		BRect frame = MonthFrame(month);
		if (!frame.Intersects(update))
			continue;
		SetFont(&title);
		font_height titleHeight;
		title.GetHeight(&titleHeight);
		bool currentMonth = today.year == fAnchor.year && today.month == month + 1;
		SetHighColor(currentMonth ? Theme::Accent() : Theme::Text());
		DrawString(Format::MonthLong(month + 1).c_str(),
			BPoint(frame.left + 4, frame.top + titleHeight.ascent));
		float top = frame.top + titleHeight.ascent + titleHeight.descent + 8;
		float cellWidth = frame.Width() / 7;
		float cellHeight = std::min(cellWidth, (frame.bottom - top) / 7);
		small.SetSize(std::max(8.0f, std::min(font.Size(), cellHeight * 0.48f)));
		SetFont(&small);
		font_height smallHeight;
		small.GetHeight(&smallHeight);
		for (int d = 0; d < 7; d++) {
			std::string letter = Format::WeekdayLetter((fWeekStart + d) % 7);
			SetHighColor(Theme::TertiaryText());
			DrawString(letter.c_str(), BPoint(frame.left + d * cellWidth
				+ (cellWidth - StringWidth(letter.c_str())) / 2,
				top + smallHeight.ascent));
		}
		top += cellHeight;
		Date first(fAnchor.year, month + 1, 1);
		int offset = (first.Weekday() - fWeekStart + 7) % 7;
		int days = DaysInMonth(fAnchor.year, month + 1);
		for (int day = 1; day <= days; day++) {
			int index = offset + day - 1;
			int row = index / 7, column = index % 7;
			BRect cell(frame.left + column * cellWidth, top + row * cellHeight,
				frame.left + (column + 1) * cellWidth - 1, top + (row + 1) * cellHeight - 1);
			Date date(fAnchor.year, month + 1, day);
			auto found = fCounts.find(date.DayNumber());
			BPoint center((cell.left + cell.right) / 2, (cell.top + cell.bottom) / 2);
			float r = std::min(cell.Width(), cell.Height()) / 2 - 1;
			if (found != fCounts.end()) {
				// Heat: busier days are stronger.
				float amount = 0.15f + 0.5f * std::min(1.0f, found->second / (float)std::max(4, fMax));
				uint32_t color = fColors[date.DayNumber()].empty() ? 0x4a90d9
					: fColors[date.DayNumber()][0];
				Drawing::FillCircle(this, center, r, Theme::Mix(Theme::Background(),
					Theme::FromHex(color), amount));
			}
			std::string text = std::to_string(day);
			if (date == today) {
				Drawing::FillCircle(this, center, r, Theme::Accent());
				SetHighColor(Theme::AccentText());
			} else if (date.Weekday() == 0 || date.Weekday() == 6)
				SetHighColor(Theme::SecondaryText());
			else
				SetHighColor(Theme::Text());
			DrawString(text.c_str(), BPoint(center.x - StringWidth(text.c_str()) / 2,
				center.y + smallHeight.ascent / 2 - 1));
		}
	}
	SetDrawingMode(B_OP_COPY);
}


bool
YearView::DayAt(BPoint where, Date& date) const
{
	BFont font(be_plain_font);
	BFont title(be_bold_font);
	title.SetSize(font.Size() * 1.15f);
	font_height titleHeight;
	title.GetHeight(&titleHeight);
	for (int month = 0; month < 12; month++) {
		BRect frame = MonthFrame(month);
		if (!frame.Contains(where))
			continue;
		float top = frame.top + titleHeight.ascent + titleHeight.descent + 8;
		float cellWidth = frame.Width() / 7;
		float cellHeight = std::min(cellWidth, (frame.bottom - top) / 7);
		if (where.y < top + cellHeight) {
			date = Date(fAnchor.year, month + 1, 1);
			return true;	// the month name: open the month
		}
		int column = (int)((where.x - frame.left) / cellWidth);
		int row = (int)((where.y - top - cellHeight) / cellHeight);
		Date first(fAnchor.year, month + 1, 1);
		int offset = (first.Weekday() - fWeekStart + 7) % 7;
		int day = row * 7 + column - offset + 1;
		if (day < 1 || day > DaysInMonth(fAnchor.year, month + 1))
			return false;
		date = Date(fAnchor.year, month + 1, day);
		return true;
	}
	return false;
}


void
YearView::MouseDown(BPoint where)
{
	MakeFocus(true);
	Date date;
	if (!DayAt(where, date))
		return;
	BMessage* current = Window()->CurrentMessage();
	int32 clicks = current != nullptr ? current->GetInt32("clicks", 1) : 1;
	BMessage message(kMsgGoToDate);
	message.AddString("date", date.ToIso().c_str());
	if (clicks >= 2 || date.day == 1)
		message.AddString("view", clicks >= 2 ? "day" : "month");
	if (clicks >= 2 || date.day == 1)
		Window()->PostMessage(&message);
	else
		static_cast<MainWindow*>(Window())->SelectDate(date);
}


void
YearView::KeyDown(const char* bytes, int32 count)
{
	switch (bytes[0]) {
		case B_LEFT_ARROW:
		case B_PAGE_UP:
			Window()->PostMessage(kMsgGoPrevious);
			break;
		case B_RIGHT_ARROW:
		case B_PAGE_DOWN:
			Window()->PostMessage(kMsgGoNext);
			break;
		default:
			CalendarView::KeyDown(bytes, count);
	}
}

}	// namespace


CalendarView*
CreateMonthView()
{
	return new MonthView();
}


CalendarView*
CreateYearView()
{
	return new YearView();
}
