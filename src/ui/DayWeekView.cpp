// The day and week views: a header with the dates and an all-day lane, and a
// scrolling 24-hour grid. Drag on empty space to create an event, drag an
// event to move it (across days too), drag its bottom edge to resize.
#include "App.h"
#include "Drawing.h"
#include "Format.h"
#include "MainWindow.h"
#include "Messages.h"
#include "Theme.h"
#include "Views.h"

#include <Cursor.h>
#include <LayoutBuilder.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <Window.h>

#include <algorithm>
#include <cmath>

using namespace aircal;

namespace {

const float kGutter = 58;
const float kHeaderHeight = 46;
const float kLaneHeight = 21;
const int kMaxLanes = 4;
const int kSnapMinutes = 15;


struct GridItem {
	bool isTask = false;
	DisplayEvent event;
	DisplayTask task;
	int day = 0;			// column
	Instant segmentStart = 0;
	Instant segmentEnd = 0;
	int column = 0;
	int columns = 1;
	BRect frame;
};


struct LaneItem {
	bool isTask = false;
	DisplayEvent event;
	DisplayTask task;
	int firstDay = 0;
	int lastDay = 0;
	int lane = 0;
	BRect frame;
};


class DayWeekView;


// --- The all-day header ---------------------------------------------------------------

class DayHeader : public BView {
public:
	DayHeader(DayWeekView* owner);
	void Draw(BRect update) override;
	void MouseDown(BPoint where) override;
	void MouseMoved(BPoint where, uint32 transit, const BMessage* drag) override;
	void MouseUp(BPoint where) override;
	BSize MinSize() override { return BSize(100, Height()); }
	BSize MaxSize() override { return BSize(B_SIZE_UNLIMITED, Height()); }
	BSize PreferredSize() override { return BSize(400, Height()); }
	float Height() const;

	void Rebuild();

private:
	DayWeekView* fOwner;
	std::vector<LaneItem> fItems;
	int fLanes = 0;
	std::vector<int> fHidden;	// per day
	int fDragIndex = -1;
	BPoint fPress;
	int fPressDay = -1;
	int fDragDay = -1;
	bool fDragging = false;
	int32 fClicks = 0;
};


// --- The time grid ---------------------------------------------------------------------

class TimeGrid : public BView {
public:
	TimeGrid(DayWeekView* owner);
	void Draw(BRect update) override;
	void MouseDown(BPoint where) override;
	void MouseMoved(BPoint where, uint32 transit, const BMessage* drag) override;
	void MouseUp(BPoint where) override;
	void KeyDown(const char* bytes, int32 count) override;
	void FrameResized(float width, float height) override;
	void AttachedToWindow() override;
	void TargetedByScrollView(BScrollView* scroller) override;
	// The scroll view sizes this to the visible area; the 24 hours are
	// scrolled through the bounds.
	BSize MinSize() override { return BSize(100, 120); }
	BSize MaxSize() override { return BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED); }
	BSize PreferredSize() override { return BSize(400, 600); }

	void Rebuild();
	void UpdateScrollBar();
	float HourHeight() const { return fHourHeight; }
	float YFor(int minutes) const { return minutes * fHourHeight / 60.0f; }
	int MinutesAt(float y) const;

private:
	enum Mode { kIdle, kCreating, kMoving, kResizing };
	int HitItem(BPoint where, bool& bottomEdge, bool& checkbox) const;
	void DrawItem(const GridItem& item, bool ghost);

	DayWeekView* fOwner;
	BScrollView* fScroller = nullptr;
	float fHourHeight = 48;
	std::vector<GridItem> fItems;
	Mode fMode = kIdle;
	int fActive = -1;
	BPoint fPress;
	int fPressDay = 0;
	int fPressMinutes = 0;
	int fCurrentDay = 0;
	int fCurrentMinutes = 0;
	bool fMoved = false;
	int32 fClicks = 0;
};


class DayWeekView : public CalendarView {
public:
	DayWeekView(int days);

	void SetAnchor(const Date& date) override;
	void Reload() override;
	void Title(std::string& bold, std::string& light) const override;
	Date Step(int direction) const override;
	void ScrollToNow() override;
	void ThemeChanged() override;
	void AttachedToWindow() override;
	void Pulse() override;
	void Draw(BRect update) override;

	int Days() const { return fDays; }
	const Date& First() const { return fFirst; }
	Date DayAt(int index) const { return fFirst.AddDays(index); }
	// Column geometry (in grid/header coordinates).
	float ColumnLeft(int day, float width) const;
	float ColumnWidth(float width) const;
	int DayAtX(float x, float width) const;

	const std::vector<DisplayEvent>& Events() const { return fEvents; }
	const std::vector<DisplayTask>& Tasks() const { return fTasks; }
	MainWindow* Main() const { return static_cast<MainWindow*>(Window()); }
	int WeekStart() const { return fWeekStart; }

private:
	int fDays;
	Date fFirst;
	int fWeekStart = 0;
	int fDayStartHour = 8;
	bool fScrolled = false;
	std::vector<DisplayEvent> fEvents;
	std::vector<DisplayTask> fTasks;
	DayHeader* fHeader = nullptr;
	TimeGrid* fGrid = nullptr;
	BScrollView* fScroll = nullptr;
	int fLastMinute = -1;
	Date fLastToday;
};


// --- DayWeekView ------------------------------------------------------------------------

DayWeekView::DayWeekView(int days)
	:
	CalendarView(days == 1 ? "day" : "week", B_PULSE_NEEDED),
	fDays(days)
{
	SetViewColor(B_TRANSPARENT_COLOR);
	fHeader = new DayHeader(this);
	fGrid = new TimeGrid(this);
	fScroll = new BScrollView("gridScroll", fGrid, 0, false, true, B_NO_BORDER);
	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(fHeader)
		.Add(fScroll, 1.0f);
}


void
DayWeekView::AttachedToWindow()
{
	CalendarView::AttachedToWindow();
}


void
DayWeekView::Draw(BRect update)
{
}


float
DayWeekView::ColumnWidth(float width) const
{
	return std::max(10.0f, (width - kGutter) / fDays);
}


float
DayWeekView::ColumnLeft(int day, float width) const
{
	return kGutter + day * ColumnWidth(width);
}


int
DayWeekView::DayAtX(float x, float width) const
{
	int day = (int)floorf((x - kGutter) / ColumnWidth(width));
	return std::max(0, std::min(fDays - 1, day));
}


void
DayWeekView::SetAnchor(const Date& date)
{
	fAnchor = date;
	Reload();
}


void
DayWeekView::Reload()
{
	Store& store = TheApp()->Store();
	bool showTasks, showCompleted;
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		fWeekStart = store.Prefs().weekStart;
		fDayStartHour = store.Prefs().dayStartHour;
		showTasks = store.Prefs().showTasksInCalendar;
		showCompleted = store.Prefs().showCompleted;
	}
	if (fDays == 7) {
		int delta = (fAnchor.Weekday() - fWeekStart + 7) % 7;
		fFirst = fAnchor.AddDays(-delta);
	} else
		fFirst = fAnchor;
	Instant from = StartOfDay(fFirst);
	Instant to = StartOfDay(fFirst.AddDays(fDays));
	fEvents = store.EventsBetween(from, to);
	fTasks.clear();
	if (showTasks) {
		Date today = Today();
		bool includeOverdue = fFirst <= today && today <= fFirst.AddDays(fDays - 1);
		fTasks = store.TasksBetween(fFirst, fFirst.AddDays(fDays - 1),
			showCompleted, includeOverdue);
		// Overdue tasks show on today.
		for (auto& task : fTasks) {
			if (!task.completed && task.dueDate < today && includeOverdue) {
				task.dueDate = today;
				task.dueHasTime = false;
			}
		}
	}
	fHeader->Rebuild();
	fGrid->Rebuild();
	fHeader->InvalidateLayout();
	if (Window() != nullptr && !fScrolled && fGrid->Bounds().Height() > 0) {
		ScrollToNow();
		fScrolled = true;
	}
}


void
DayWeekView::Title(std::string& bold, std::string& light) const
{
	if (fDays == 1) {
		bold = Format::MonthLong(fAnchor.month) + " " + std::to_string(fAnchor.day);
		light = std::to_string(fAnchor.year);
		return;
	}
	Date last = fFirst.AddDays(fDays - 1);
	if (fFirst.month == last.month) {
		bold = Format::MonthLong(fFirst.month);
		light = std::to_string(fFirst.year);
	} else {
		bold = Format::MonthShort(fFirst.month) + " – " + Format::MonthShort(last.month);
		light = std::to_string(last.year);
	}
}


Date
DayWeekView::Step(int direction) const
{
	return fAnchor.AddDays(direction * fDays);
}


void
DayWeekView::ScrollToNow()
{
	if (fGrid->Window() == nullptr)
		return;
	DateTime now = ToWall(Now());
	Date today = Today();
	float target;
	bool todayVisible = today >= fFirst && today <= fFirst.AddDays(fDays - 1);
	if (todayVisible && (now.hour < fDayStartHour || now.hour > fDayStartHour + 9))
		target = fGrid->YFor(std::max(0, now.hour * 60 + now.minute - 120));
	else
		target = fGrid->YFor(fDayStartHour * 60);
	float maxScroll = std::max(0.0f, 24 * fGrid->HourHeight()
		- fGrid->Bounds().Height());
	fGrid->ScrollTo(0, std::min(target, maxScroll));
}


void
DayWeekView::ThemeChanged()
{
	fHeader->Invalidate();
	fGrid->Invalidate();
	if (fScroll->ScrollBar(B_VERTICAL) != nullptr)
		fScroll->ScrollBar(B_VERTICAL)->Invalidate();
}


void
DayWeekView::Pulse()
{
	// Move the "now" line once a minute; roll over at midnight.
	DateTime now = ToWall(Now());
	int minute = now.hour * 60 + now.minute;
	if (minute != fLastMinute) {
		fLastMinute = minute;
		fGrid->Invalidate();
	}
	Date today = Today();
	if (today != fLastToday) {
		fLastToday = today;
		Reload();
	}
	if (!fScrolled && fGrid->Bounds().Height() > 0 && fScroll->Bounds().Height() > 50) {
		ScrollToNow();
		fScrolled = true;
	}
}


// --- DayHeader ------------------------------------------------------------------------------

DayHeader::DayHeader(DayWeekView* owner)
	:
	BView("dayHeader", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_FRAME_EVENTS),
	fOwner(owner)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}


float
DayHeader::Height() const
{
	int lanes = std::min(fLanes, kMaxLanes);
	return kHeaderHeight + std::max(1, lanes) * kLaneHeight + 8;
}


void
DayHeader::Rebuild()
{
	fItems.clear();
	int days = fOwner->Days();
	Date first = fOwner->First();
	for (const DisplayEvent& event : fOwner->Events()) {
		bool multiDayTimed = !event.allDay
			&& event.endDate.DayNumber() > event.startDate.DayNumber()
			&& event.end - event.start >= 86400;
		if (!event.allDay && !multiDayTimed)
			continue;
		LaneItem item;
		item.event = event;
		item.firstDay = (int)std::max<int64_t>(0, event.startDate.DayNumber()
			- first.DayNumber());
		item.lastDay = (int)std::min<int64_t>(days - 1, event.endDate.DayNumber()
			- first.DayNumber());
		if (item.lastDay < 0 || item.firstDay >= days)
			continue;
		fItems.push_back(item);
	}
	for (const DisplayTask& task : fOwner->Tasks()) {
		if (task.dueHasTime)
			continue;
		LaneItem item;
		item.isTask = true;
		item.task = task;
		item.firstDay = item.lastDay = (int)(task.dueDate.DayNumber()
			- first.DayNumber());
		if (item.firstDay < 0 || item.firstDay >= days)
			continue;
		fItems.push_back(item);
	}
	// Longer items first, then by start.
	std::stable_sort(fItems.begin(), fItems.end(), [](const LaneItem& a,
			const LaneItem& b) {
		if (a.isTask != b.isTask)
			return !a.isTask;
		if (a.firstDay != b.firstDay)
			return a.firstDay < b.firstDay;
		return a.lastDay - a.firstDay > b.lastDay - b.firstDay;
	});
	std::vector<std::vector<bool>> used;
	fLanes = 0;
	fHidden.assign(days, 0);
	for (LaneItem& item : fItems) {
		int lane = 0;
		while (true) {
			if (lane >= (int)used.size())
				used.push_back(std::vector<bool>(days, false));
			bool free = true;
			for (int d = item.firstDay; d <= item.lastDay; d++) {
				if (used[lane][d])
					free = false;
			}
			if (free)
				break;
			lane++;
		}
		for (int d = item.firstDay; d <= item.lastDay; d++)
			used[lane][d] = true;
		item.lane = lane;
		fLanes = std::max(fLanes, lane + 1);
	}
	if (fLanes > kMaxLanes) {
		for (LaneItem& item : fItems) {
			if (item.lane >= kMaxLanes - 1) {
				for (int d = item.firstDay; d <= item.lastDay; d++)
					fHidden[d]++;
			}
		}
	}
	InvalidateLayout();
	Invalidate();
}


void
DayHeader::Draw(BRect update)
{
	BRect bounds = Bounds();
	SetHighColor(Theme::Header());
	FillRect(bounds);
	float width = bounds.Width() - B_V_SCROLL_BAR_WIDTH;
	int days = fOwner->Days();
	Date today = Today();
	Date first = fOwner->First();
	MainWindow* main = fOwner->Main();

	BFont font(be_plain_font);
	BFont small(font);
	small.SetSize(font.Size() * 0.92f);
	BFont number(be_bold_font);
	number.SetSize(font.Size() * (days == 1 ? 1.9f : 1.55f));

	// Weekday names and day numbers.
	for (int d = 0; d < days; d++) {
		Date date = first.AddDays(d);
		float left = fOwner->ColumnLeft(d, width);
		float columnWidth = fOwner->ColumnWidth(width);
		BRect column(left, 0, left + columnWidth - 1, kHeaderHeight);
		if (date.Weekday() == 0 || date.Weekday() == 6) {
			SetHighColor(Theme::Mix(Theme::Header(), Theme::Weekend(), 0.6f));
			FillRect(BRect(left, 0, left + columnWidth - 1, bounds.bottom));
		}
		bool isToday = date == today;
		std::string weekday = days == 1 ? Format::WeekdayLong(date.Weekday())
			: Format::WeekdayShort(date.Weekday());
		std::string day = std::to_string(date.day);
		SetFont(&number);
		float numberWidth = StringWidth(day.c_str());
		font_height numberHeight;
		number.GetHeight(&numberHeight);
		float radius = std::max(numberWidth / 2 + 6, (numberHeight.ascent + 4) / 2 + 2);
		SetFont(&small);
		float weekdayWidth = StringWidth(weekday.c_str());
		// Room for today's circle around the number.
		float gap = isToday ? std::max(6.0f, radius - numberWidth / 2 + 3) : 6;
		float total = weekdayWidth + gap + numberWidth;
		float x = days == 1 ? left + 12 : left + (columnWidth - total) / 2;
		float baseline = kHeaderHeight - 13;
		SetDrawingMode(B_OP_ALPHA);
		SetHighColor(isToday ? Theme::Accent() : Theme::SecondaryText());
		SetLowColor(Theme::Header());
		DrawString(weekday.c_str(), BPoint(x, baseline));
		SetFont(&number);
		float numberX = x + weekdayWidth + gap;
		if (isToday) {
			BPoint center(numberX + numberWidth / 2,
				baseline - numberHeight.ascent / 2 + 1);
			Drawing::FillCircle(this, center, radius, Theme::Accent());
			SetHighColor(Theme::AccentText());
		} else
			SetHighColor(Theme::Text());
		DrawString(day.c_str(), BPoint(numberX, baseline));
		SetDrawingMode(B_OP_COPY);
		(void)column;
	}

	// All-day lane.
	float laneTop = kHeaderHeight + 2;
	SetFont(&small);
	SetHighColor(Theme::TertiaryText());
	SetDrawingMode(B_OP_ALPHA);
	DrawString("all-day", BPoint(kGutter - 8 - StringWidth("all-day"),
		laneTop + kLaneHeight - 6));
	float columnWidth = fOwner->ColumnWidth(width);
	int lanesShown = std::min(fLanes, kMaxLanes);
	for (LaneItem& item : fItems) {
		item.frame = BRect();
		if (fLanes > kMaxLanes && item.lane >= kMaxLanes - 1)
			continue;
		int firstDay = item.firstDay, lastDay = item.lastDay;
		if (fDragging && fDragIndex >= 0 && &item == &fItems[fDragIndex]) {
			int shift = fDragDay - fPressDay;
			firstDay = std::max(0, firstDay + shift);
			lastDay = std::min(fOwner->Days() - 1, lastDay + shift);
		}
		BRect frame(fOwner->ColumnLeft(firstDay, width) + 2,
			laneTop + item.lane * kLaneHeight + 1,
			fOwner->ColumnLeft(lastDay, width) + columnWidth - 3,
			laneTop + (item.lane + 1) * kLaneHeight - 2);
		item.frame = frame;
		if (item.isTask) {
			bool selected = main != nullptr && main->CurrentSelection().IsTask(item.task.taskId);
			rgb_color color = Theme::FromHex(item.task.color);
			if (selected)
				Drawing::FillRound(this, frame, 4, Theme::EventFill(item.task.color));
			Drawing::DrawCheck(this, BPoint(frame.left + 9, (frame.top + frame.bottom) / 2),
				5.5f, item.task.priority >= 2 ? Theme::Priority(item.task.priority)
					: color, item.task.completed);
			SetHighColor(item.task.completed ? Theme::TertiaryText() : Theme::Text());
			std::string text = Drawing::Fit(this, item.task.title, frame.Width() - 22);
			DrawString(text.c_str(), BPoint(frame.left + 19,
				Drawing::Baseline(small, frame)));
			if (item.task.completed) {
				float y = (frame.top + frame.bottom) / 2;
				StrokeLine(BPoint(frame.left + 19, y),
					BPoint(frame.left + 19 + StringWidth(text.c_str()), y));
			}
			continue;
		}
		bool selected = main != nullptr && main->CurrentSelection().IsEvent(item.event);
		Drawing::FillRound(this, frame, 4, Theme::AllDayFill(item.event.color, selected));
		SetHighColor(Theme::AllDayText(item.event.color, selected));
		std::string text = Drawing::Fit(this, item.event.title, frame.Width() - 10);
		DrawString(text.c_str(), BPoint(frame.left + 6, Drawing::Baseline(small, frame)));
	}
	if (fLanes > kMaxLanes) {
		for (int d = 0; d < days; d++) {
			if (fHidden[d] == 0)
				continue;
			std::string more = "+" + std::to_string(fHidden[d]) + " more";
			BRect frame(fOwner->ColumnLeft(d, width) + 4,
				laneTop + (kMaxLanes - 1) * kLaneHeight + 1,
				fOwner->ColumnLeft(d, width) + columnWidth - 4,
				laneTop + kMaxLanes * kLaneHeight - 2);
			SetHighColor(Theme::SecondaryText());
			DrawString(more.c_str(), BPoint(frame.left + 4, Drawing::Baseline(small, frame)));
		}
	}
	(void)lanesShown;
	SetDrawingMode(B_OP_COPY);

	// Column separators and the bottom line.
	SetHighColor(Theme::GridLine());
	for (int d = 1; d < days; d++) {
		float x = fOwner->ColumnLeft(d, width);
		StrokeLine(BPoint(x, kHeaderHeight - 4), BPoint(x, bounds.bottom));
	}
	StrokeLine(BPoint(kGutter, kHeaderHeight - 4), BPoint(kGutter, bounds.bottom));
	SetHighColor(Theme::GridLineStrong());
	StrokeLine(BPoint(0, bounds.bottom), BPoint(bounds.right, bounds.bottom));
}


void
DayHeader::MouseDown(BPoint where)
{
	MakeFocus(true);
	float width = Bounds().Width() - B_V_SCROLL_BAR_WIDTH;
	fPress = where;
	fPressDay = fDragDay = fOwner->DayAtX(where.x, width);
	fDragging = false;
	fDragIndex = -1;
	BMessage* current = Window()->CurrentMessage();
	fClicks = current != nullptr ? current->GetInt32("clicks", 1) : 1;
	int32 buttons = current != nullptr ? current->GetInt32("buttons", B_PRIMARY_MOUSE_BUTTON)
		: B_PRIMARY_MOUSE_BUTTON;
	MainWindow* main = fOwner->Main();
	Date date = fOwner->DayAt(fPressDay);

	// Day number: open the day.
	if (where.y < kHeaderHeight && where.x > kGutter) {
		if (fOwner->Days() > 1) {
			BMessage message(kMsgGoToDate);
			message.AddString("date", date.ToIso().c_str());
			message.AddString("view", "day");
			Window()->PostMessage(&message);
		}
		return;
	}
	for (size_t i = 0; i < fItems.size(); i++) {
		LaneItem& item = fItems[i];
		if (!item.frame.IsValid() || !item.frame.Contains(where))
			continue;
		BPoint screen = ConvertToScreen(where);
		if (item.isTask) {
			if (where.x < item.frame.left + 18) {
				main->ToggleTask(item.task.taskId, !item.task.completed);
				return;
			}
			if (buttons & B_SECONDARY_MOUSE_BUTTON) {
				main->ShowTaskMenu(item.task.taskId, screen);
				return;
			}
			if (fClicks >= 2)
				main->EditTask(item.task.taskId, screen);
			else
				main->SelectTask(item.task.taskId);
			return;
		}
		if (buttons & B_SECONDARY_MOUSE_BUTTON) {
			main->ShowEventMenu(item.event, screen);
			return;
		}
		if (fClicks >= 2) {
			main->EditEvent(item.event, screen);
			return;
		}
		main->SelectEvent(item.event);
		if (!item.event.readOnly && item.event.allDay) {
			fDragIndex = (int)i;
			SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
		}
		return;
	}
	if (fClicks >= 2 && where.x > kGutter) {
		main->NewEventAt(0, 0, true, date, ConvertToScreen(where));
		return;
	}
	main->SelectDate(date);
}


void
DayHeader::MouseMoved(BPoint where, uint32 transit, const BMessage* drag)
{
	if (fDragIndex < 0)
		return;
	float width = Bounds().Width() - B_V_SCROLL_BAR_WIDTH;
	int day = fOwner->DayAtX(where.x, width);
	if (!fDragging && fabs(where.x - fPress.x) > 4)
		fDragging = true;
	if (fDragging && day != fDragDay) {
		fDragDay = day;
		Invalidate();
	}
}


void
DayHeader::MouseUp(BPoint where)
{
	if (fDragIndex >= 0 && fDragging && fDragDay != fPressDay) {
		LaneItem item = fItems[fDragIndex];
		int shift = fDragDay - fPressDay;
		Date date = item.event.startDate.AddDays(shift);
		int days = (int)(item.event.endDate.DayNumber() - item.event.startDate.DayNumber()) + 1;
		fDragIndex = -1;
		fDragging = false;
		fOwner->Main()->MoveEvent(item.event, 0, 0, true, date, days);
	}
	fDragIndex = -1;
	fDragging = false;
	Invalidate();
}


// --- TimeGrid ----------------------------------------------------------------------------------

TimeGrid::TimeGrid(DayWeekView* owner)
	:
	BView("timeGrid", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_FRAME_EVENTS | B_NAVIGABLE),
	fOwner(owner)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}


void
TimeGrid::AttachedToWindow()
{
	BView::AttachedToWindow();
	UpdateScrollBar();
}


void
TimeGrid::TargetedByScrollView(BScrollView* scroller)
{
	fScroller = scroller;
}


void
TimeGrid::FrameResized(float width, float height)
{
	Rebuild();
	UpdateScrollBar();
}


void
TimeGrid::UpdateScrollBar()
{
	if (fScroller == nullptr)
		return;
	BScrollBar* bar = fScroller->ScrollBar(B_VERTICAL);
	if (bar == nullptr)
		return;
	float total = 24 * fHourHeight;
	float visible = fScroller->Bounds().Height();
	bar->SetRange(0, std::max(0.0f, total - visible));
	bar->SetProportion(std::min(1.0f, visible / total));
	bar->SetSteps(fHourHeight / 4, std::max(visible - fHourHeight, fHourHeight));
}


int
TimeGrid::MinutesAt(float y) const
{
	int minutes = (int)(y * 60 / fHourHeight);
	return std::max(0, std::min(24 * 60, minutes));
}


void
TimeGrid::Rebuild()
{
	fItems.clear();
	float width = Bounds().Width();
	int days = fOwner->Days();
	Date first = fOwner->First();
	std::vector<std::vector<GridItem>> perDay(days);
	for (const DisplayEvent& event : fOwner->Events()) {
		if (event.allDay || event.end - event.start >= 86400)
			continue;
		for (int d = 0; d < days; d++) {
			Instant dayStart = StartOfDay(first.AddDays(d));
			Instant dayEnd = StartOfDay(first.AddDays(d + 1));
			if (event.start >= dayEnd || (event.end <= dayStart
					&& !(event.end == event.start && event.start == dayStart)))
				continue;
			GridItem item;
			item.event = event;
			item.day = d;
			item.segmentStart = std::max(event.start, dayStart);
			item.segmentEnd = std::min(std::max(event.end, event.start + 15 * 60), dayEnd);
			perDay[d].push_back(item);
		}
	}
	for (const DisplayTask& task : fOwner->Tasks()) {
		if (!task.dueHasTime)
			continue;
		int d = (int)(LocalDate(task.dueTime).DayNumber() - first.DayNumber());
		if (d < 0 || d >= days)
			continue;
		GridItem item;
		item.isTask = true;
		item.task = task;
		item.day = d;
		item.segmentStart = task.dueTime;
		item.segmentEnd = task.dueTime + 30 * 60;
		perDay[d].push_back(item);
	}
	// Overlap columns per day.
	for (auto& items : perDay) {
		std::sort(items.begin(), items.end(), [](const GridItem& a, const GridItem& b) {
			if (a.segmentStart != b.segmentStart)
				return a.segmentStart < b.segmentStart;
			return a.segmentEnd > b.segmentEnd;
		});
		size_t clusterStart = 0;
		Instant clusterEnd = 0;
		std::vector<Instant> columnEnds;
		auto finish = [&](size_t end) {
			int columns = (int)columnEnds.size();
			for (size_t i = clusterStart; i < end; i++)
				items[i].columns = std::max(1, columns);
		};
		for (size_t i = 0; i < items.size(); i++) {
			GridItem& item = items[i];
			Instant visibleEnd = std::max(item.segmentEnd, item.segmentStart + 20 * 60);
			if (i > 0 && item.segmentStart >= clusterEnd) {
				finish(i);
				clusterStart = i;
				columnEnds.clear();
			}
			int column = -1;
			for (size_t c = 0; c < columnEnds.size(); c++) {
				if (columnEnds[c] <= item.segmentStart) {
					column = (int)c;
					break;
				}
			}
			if (column < 0) {
				column = (int)columnEnds.size();
				columnEnds.push_back(0);
			}
			columnEnds[column] = visibleEnd;
			item.column = column;
			clusterEnd = std::max(clusterEnd, visibleEnd);
		}
		finish(items.size());
		for (auto& item : items) {
			Instant dayStart = StartOfDay(first.AddDays(item.day));
			float columnWidth = fOwner->ColumnWidth(width);
			float left = fOwner->ColumnLeft(item.day, width) + 1;
			float usable = columnWidth - 6;
			float each = usable / item.columns;
			float top = YFor((int)((item.segmentStart - dayStart) / 60));
			float bottom = YFor((int)((item.segmentEnd - dayStart) / 60));
			if (bottom - top < 18)
				bottom = top + 18;
			// Later columns overlap the previous a little, as Fantastical does.
			float x = left + item.column * each;
			float w = item.column + 1 < item.columns ? each * 1.0f - 2 : each - 1;
			item.frame = BRect(x, top + 1, x + w, bottom - 1);
			fItems.push_back(item);
		}
	}
	Invalidate();
}


void
TimeGrid::DrawItem(const GridItem& item, bool ghost)
{
	MainWindow* main = fOwner->Main();
	BRect frame = item.frame;
	BFont font(be_plain_font);
	BFont bold(be_bold_font);
	bold.SetSize(font.Size() * 0.92f);
	BFont small(font);
	small.SetSize(font.Size() * 0.86f);
	SetDrawingMode(B_OP_ALPHA);

	if (item.isTask) {
		bool selected = main->CurrentSelection().IsTask(item.task.taskId);
		BRect pill(frame.left, frame.top, frame.right, frame.top + 20);
		Drawing::FillRound(this, pill, 5, selected ? Theme::EventFill(item.task.color, false)
			: Theme::Mix(Theme::Background(), Theme::FromHex(item.task.color), 0.08f));
		Drawing::StrokeRound(this, pill, 5, Theme::WithAlpha(Theme::FromHex(item.task.color), 120));
		Drawing::DrawCheck(this, BPoint(pill.left + 10, (pill.top + pill.bottom) / 2), 5.5f,
			item.task.priority >= 2 ? Theme::Priority(item.task.priority)
				: Theme::FromHex(item.task.color), item.task.completed);
		SetFont(&small);
		SetHighColor(item.task.completed ? Theme::TertiaryText() : Theme::Text());
		std::string text = Format::Time(item.task.dueTime, true) + " " + item.task.title;
		text = Drawing::Fit(this, text, pill.Width() - 24);
		DrawString(text.c_str(), BPoint(pill.left + 20, Drawing::Baseline(small, pill)));
		SetDrawingMode(B_OP_COPY);
		return;
	}

	const DisplayEvent& event = item.event;
	bool selected = main->CurrentSelection().IsEvent(event) && !ghost;
	rgb_color fill = Theme::EventFill(event.color, selected);
	if (ghost)
		fill = Theme::WithAlpha(Theme::EventFill(event.color, true), 170);
	Drawing::FillRound(this, frame, 4, fill);
	if (!selected) {
		SetHighColor(Theme::EventBar(event.color));
		FillRoundRect(BRect(frame.left, frame.top, frame.left + 3, frame.bottom), 2, 2);
	}
	if (event.tentative) {
		// Hatched look for tentative events: a dashed border.
		SetHighColor(Theme::EventBar(event.color));
		StrokeRoundRect(frame, 4, 4, B_MIXED_COLORS);
	}
	rgb_color text = ghost ? Theme::AccentText() : Theme::EventText(event.color, selected);
	float x = frame.left + 7;
	float available = frame.right - x - 3;
	float height = frame.Height();
	font_height boldHeight;
	bold.GetHeight(&boldHeight);
	float line = ceilf(boldHeight.ascent + boldHeight.descent + 1);
	SetHighColor(text);
	if (height < line * 2 + 2) {
		// One line: "Title 9:30"
		SetFont(&bold);
		std::string title = Drawing::Fit(this, event.title, available);
		DrawString(title.c_str(), BPoint(x, frame.top + boldHeight.ascent + 2));
		float used = StringWidth(title.c_str());
		SetFont(&small);
		std::string time = Format::Time(event.start, true);
		if (used + StringWidth(time.c_str()) + 8 < available) {
			SetHighColor(Theme::WithAlpha(text, 190));
			DrawString(time.c_str(), BPoint(x + used + 6, frame.top + boldHeight.ascent + 2));
		}
	} else {
		SetFont(&small);
		SetHighColor(Theme::WithAlpha(text, 200));
		font_height smallHeight;
		small.GetHeight(&smallHeight);
		float y = frame.top + smallHeight.ascent + 3;
		std::string time = Format::Time(event.start);
		DrawString(Drawing::Fit(this, time, available).c_str(), BPoint(x, y));
		SetFont(&bold);
		SetHighColor(text);
		y += line;
		// Wrap the title over the available lines.
		std::string rest = event.title;
		while (!rest.empty() && y < frame.bottom - 2) {
			std::string piece = Drawing::Fit(this, rest, available);
			if (piece.size() < rest.size() && y + line < frame.bottom - 2) {
				// Break at the last space that fits.
				size_t cut = rest.size();
				while (cut > 0) {
					size_t space = rest.rfind(' ', cut - 1);
					if (space == std::string::npos || space == 0)
						break;
					if (StringWidth(rest.substr(0, space).c_str()) <= available) {
						piece = rest.substr(0, space);
						break;
					}
					cut = space;
				}
			}
			DrawString(piece.c_str(), BPoint(x, y));
			if (piece.size() >= rest.size() || piece.find("…") != std::string::npos)
				break;
			rest = rest.substr(piece.size());
			while (!rest.empty() && rest[0] == ' ')
				rest.erase(0, 1);
			y += line;
		}
		if (!event.location.empty() && y + line - 4 < frame.bottom) {
			SetFont(&small);
			SetHighColor(Theme::WithAlpha(text, 190));
			DrawString(Drawing::Fit(this, event.location, available).c_str(),
				BPoint(x, y + line - 2));
		}
	}
	if (event.recurring && frame.Width() > 40 && height >= 18) {
		Drawing::DrawRepeat(this, BRect(frame.right - 13, frame.top + 3,
			frame.right - 3, frame.top + 13), Theme::WithAlpha(text, 150));
	}
	SetDrawingMode(B_OP_COPY);
}


void
TimeGrid::Draw(BRect update)
{
	BRect bounds = Bounds();
	float width = bounds.Width();
	int days = fOwner->Days();
	Date first = fOwner->First();
	Date today = Today();

	SetHighColor(Theme::Background());
	FillRect(update);
	// Weekend tint and today.
	for (int d = 0; d < days; d++) {
		Date date = first.AddDays(d);
		float left = fOwner->ColumnLeft(d, width);
		BRect column(left, update.top, left + fOwner->ColumnWidth(width) - 1,
			update.bottom);
		if (date.Weekday() == 0 || date.Weekday() == 6) {
			SetHighColor(Theme::Weekend());
			FillRect(column);
		}
	}
	// Gutter.
	SetHighColor(Theme::Mix(Theme::Background(), Theme::Header(), 0.5f));
	FillRect(BRect(0, update.top, kGutter - 1, update.bottom));

	// Hour lines and labels.
	BFont font(be_plain_font);
	BFont small(font);
	small.SetSize(font.Size() * 0.82f);
	SetFont(&small);
	font_height smallHeight;
	small.GetHeight(&smallHeight);
	for (int hour = 0; hour <= 24; hour++) {
		float y = floorf(YFor(hour * 60));
		if (y < update.top - fHourHeight || y > update.bottom + fHourHeight)
			continue;
		SetHighColor(Theme::GridLine());
		StrokeLine(BPoint(kGutter, y), BPoint(width, y));
		float half = floorf(YFor(hour * 60 + 30));
		SetHighColor(Theme::HalfHourLine());
		if (hour < 24)
			StrokeLine(BPoint(kGutter, half), BPoint(width, half));
		if (hour > 0 && hour < 24) {
			std::string label = Format::Hour(hour);
			SetHighColor(Theme::TertiaryText());
			SetDrawingMode(B_OP_ALPHA);
			DrawString(label.c_str(), BPoint(kGutter - 8 - StringWidth(label.c_str()),
				y + smallHeight.ascent / 2 - 1));
			SetDrawingMode(B_OP_COPY);
		}
	}
	SetHighColor(Theme::GridLine());
	for (int d = 0; d <= days; d++) {
		float x = fOwner->ColumnLeft(d, width);
		StrokeLine(BPoint(x, update.top), BPoint(x, update.bottom));
	}

	// The creation range while dragging.
	if (fMode == kCreating && fMoved) {
		int a = std::min(fPressMinutes, fCurrentMinutes);
		int b = std::max(fPressMinutes, fCurrentMinutes);
		if (b - a < kSnapMinutes)
			b = a + kSnapMinutes;
		float left = fOwner->ColumnLeft(fPressDay, width) + 1;
		BRect frame(left, YFor(a) + 1, left + fOwner->ColumnWidth(width) - 6, YFor(b) - 1);
		rgb_color accent = Theme::Accent();
		SetDrawingMode(B_OP_ALPHA);
		Drawing::FillRound(this, frame, 4, Theme::WithAlpha(accent, 70));
		Drawing::StrokeRound(this, frame, 4, accent);
		SetHighColor(Theme::Text());
		std::string text = Format::Time(ToInstant(DateTime(fOwner->DayAt(fPressDay),
			a / 60, a % 60))) + " – " + Format::Time(ToInstant(DateTime(
			fOwner->DayAt(fPressDay), b / 60 % 24, b % 60)));
		SetFont(&small);
		DrawString(Drawing::Fit(this, text, frame.Width() - 8).c_str(),
			BPoint(frame.left + 5, frame.top + smallHeight.ascent + 3));
		SetDrawingMode(B_OP_COPY);
	}

	for (size_t i = 0; i < fItems.size(); i++) {
		if (!fItems[i].frame.Intersects(update))
			continue;
		if ((fMode == kMoving || fMode == kResizing) && fMoved && (int)i == fActive) {
			// Draw the original dimmed.
			GridItem dimmed = fItems[i];
			SetDrawingMode(B_OP_ALPHA);
			Drawing::FillRound(this, dimmed.frame, 4, Theme::WithAlpha(
				Theme::EventFill(dimmed.event.color), 90));
			SetDrawingMode(B_OP_COPY);
			continue;
		}
		DrawItem(fItems[i], false);
	}

	// The item being moved or resized.
	if ((fMode == kMoving || fMode == kResizing) && fMoved && fActive >= 0) {
		GridItem ghost = fItems[fActive];
		int length = (int)((ghost.event.end - ghost.event.start) / 60);
		Date day = fOwner->DayAt(fCurrentDay);
		int startMinutes;
		if (fMode == kMoving) {
			Instant dayStart = StartOfDay(fOwner->DayAt(ghost.day));
			int originalMinutes = (int)((ghost.event.start - dayStart) / 60);
			startMinutes = originalMinutes + (fCurrentMinutes - fPressMinutes);
		} else {
			Instant dayStart = StartOfDay(fOwner->DayAt(ghost.day));
			startMinutes = (int)((ghost.event.start - dayStart) / 60);
			day = fOwner->DayAt(ghost.day);
			length = std::max(kSnapMinutes, fCurrentMinutes - startMinutes);
		}
		float left = fOwner->ColumnLeft((int)(day.DayNumber() - first.DayNumber()), width) + 1;
		ghost.frame = BRect(left, YFor(startMinutes) + 1,
			left + fOwner->ColumnWidth(width) - 6, YFor(startMinutes + length) - 1);
		ghost.event.start = ToInstant(DateTime(day, 0, 0)) + startMinutes * 60;
		ghost.event.end = ghost.event.start + length * 60;
		DrawItem(ghost, true);
	}

	// Now line.
	for (int d = 0; d < days; d++) {
		if (first.AddDays(d) != today)
			continue;
		DateTime now = ToWall(Now());
		float y = floorf(YFor(now.hour * 60 + now.minute));
		float left = fOwner->ColumnLeft(d, width);
		SetHighColor(Theme::Accent());
		SetPenSize(1.5f);
		StrokeLine(BPoint(left, y), BPoint(left + fOwner->ColumnWidth(width) - 1, y));
		SetPenSize(1);
		SetDrawingMode(B_OP_ALPHA);
		Drawing::FillCircle(this, BPoint(left + 1, y), 4, Theme::Accent());
		// The time in the gutter.
		std::string label = Format::Time(Now(), true);
		SetFont(&small);
		float labelWidth = StringWidth(label.c_str()) + 8;
		BRect pill(kGutter - labelWidth - 4, y - 8, kGutter - 4, y + 8);
		Drawing::FillRound(this, pill, 8, Theme::Accent());
		SetHighColor(Theme::AccentText());
		DrawString(label.c_str(), BPoint(pill.left + 4, Drawing::Baseline(small, pill)));
		SetDrawingMode(B_OP_COPY);
	}
}


int
TimeGrid::HitItem(BPoint where, bool& bottomEdge, bool& checkbox) const
{
	bottomEdge = false;
	checkbox = false;
	for (int i = (int)fItems.size() - 1; i >= 0; i--) {
		const GridItem& item = fItems[i];
		BRect frame = item.frame;
		if (item.isTask)
			frame.bottom = frame.top + 20;
		if (!frame.Contains(where))
			continue;
		if (item.isTask)
			checkbox = where.x < frame.left + 18;
		else
			bottomEdge = where.y > frame.bottom - 5 && frame.Height() > 14;
		return i;
	}
	return -1;
}


void
TimeGrid::MouseDown(BPoint where)
{
	MakeFocus(true);
	BMessage* current = Window()->CurrentMessage();
	fClicks = current != nullptr ? current->GetInt32("clicks", 1) : 1;
	int32 buttons = current != nullptr
		? current->GetInt32("buttons", B_PRIMARY_MOUSE_BUTTON) : B_PRIMARY_MOUSE_BUTTON;
	MainWindow* main = fOwner->Main();
	float width = Bounds().Width();
	fPress = where;
	fPressDay = fCurrentDay = fOwner->DayAtX(where.x, width);
	int minutes = MinutesAt(where.y);
	fPressMinutes = fCurrentMinutes = minutes / kSnapMinutes * kSnapMinutes;
	fMoved = false;
	fActive = -1;
	fMode = kIdle;
	if (where.x < kGutter)
		return;

	bool bottomEdge, checkbox;
	int hit = HitItem(where, bottomEdge, checkbox);
	BPoint screen = ConvertToScreen(where);
	if (hit >= 0) {
		GridItem& item = fItems[hit];
		if (item.isTask) {
			if (checkbox)
				main->ToggleTask(item.task.taskId, !item.task.completed);
			else if (buttons & B_SECONDARY_MOUSE_BUTTON)
				main->ShowTaskMenu(item.task.taskId, screen);
			else if (fClicks >= 2)
				main->EditTask(item.task.taskId, screen);
			else
				main->SelectTask(item.task.taskId);
			return;
		}
		if (buttons & B_SECONDARY_MOUSE_BUTTON) {
			main->ShowEventMenu(item.event, screen);
			return;
		}
		if (fClicks >= 2) {
			main->EditEvent(item.event, screen);
			return;
		}
		main->SelectEvent(item.event);
		if (!item.event.readOnly) {
			fActive = hit;
			fMode = bottomEdge ? kResizing : kMoving;
			// Moving keeps the grab offset: compare against the press.
			fPressMinutes = fCurrentMinutes = MinutesAt(where.y);
			SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
		}
		return;
	}
	if (buttons & B_SECONDARY_MOUSE_BUTTON)
		return;
	if (fClicks >= 2) {
		int duration;
		{
			std::lock_guard<std::recursive_mutex> lock(TheApp()->Store().Mutex());
			duration = TheApp()->Store().Prefs().defaultDuration;
		}
		int start = minutes / 30 * 30;
		Date day = fOwner->DayAt(fPressDay);
		Instant at = ToInstant(DateTime(day, start / 60, start % 60));
		main->NewEventAt(at, at + duration * 60, false, day, screen);
		return;
	}
	fMode = kCreating;
	main->SetSelection(Selection());
	main->SelectDate(fOwner->DayAt(fPressDay));
	SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
}


void
TimeGrid::MouseMoved(BPoint where, uint32 transit, const BMessage* drag)
{
	if (fMode == kIdle) {
		bool bottomEdge, checkbox;
		int hit = HitItem(where, bottomEdge, checkbox);
		BCursor cursor(hit >= 0 && bottomEdge ? B_CURSOR_ID_RESIZE_NORTH_SOUTH
			: B_CURSOR_ID_SYSTEM_DEFAULT);
		SetViewCursor(&cursor);
		return;
	}
	float width = Bounds().Width();
	if (!fMoved && fabs(where.y - fPress.y) < 4 && fabs(where.x - fPress.x) < 4)
		return;
	fMoved = true;
	int minutes = MinutesAt(where.y);
	int snapped = (int)roundf((float)minutes / kSnapMinutes) * kSnapMinutes;
	int day = fMode == kMoving ? fOwner->DayAtX(where.x, width) : fCurrentDay;
	if (fMode == kMoving) {
		// Snap the difference, not the position.
		int delta = (int)roundf((float)(minutes - fPressMinutes) / kSnapMinutes)
			* kSnapMinutes;
		snapped = fPressMinutes + delta;
	}
	if (snapped != fCurrentMinutes || day != fCurrentDay) {
		fCurrentMinutes = snapped;
		fCurrentDay = day;
		Invalidate();
	}
	// Auto-scroll near the edges.
	BRect visible = Bounds();
	if (where.y < visible.top + 10 && visible.top > 0)
		ScrollBy(0, -std::min(visible.top, fHourHeight / 4));
	else if (where.y > visible.bottom - 10 && visible.bottom < 24 * fHourHeight)
		ScrollBy(0, std::min(24 * fHourHeight - visible.bottom, fHourHeight / 4));
}


void
TimeGrid::MouseUp(BPoint where)
{
	Mode mode = fMode;
	fMode = kIdle;
	MainWindow* main = fOwner->Main();
	if (mode == kCreating && fMoved) {
		int a = std::min(fPressMinutes, fCurrentMinutes);
		int b = std::max(fPressMinutes, fCurrentMinutes);
		if (b - a < kSnapMinutes)
			b = a + kSnapMinutes;
		Date day = fOwner->DayAt(fPressDay);
		Instant start = ToInstant(DateTime(day, a / 60, a % 60));
		Instant end = b >= 24 * 60 ? StartOfDay(day.AddDays(1))
			: ToInstant(DateTime(day, b / 60, b % 60));
		Invalidate();
		main->NewEventAt(start, end, false, day, ConvertToScreen(where));
		return;
	}
	if ((mode == kMoving || mode == kResizing) && fMoved && fActive >= 0) {
		GridItem item = fItems[fActive];
		fActive = -1;
		Date first = fOwner->First();
		Instant dayStart = StartOfDay(fOwner->DayAt(item.day));
		if (mode == kMoving) {
			int originalMinutes = (int)((item.event.start - dayStart) / 60);
			int startMinutes = originalMinutes + (fCurrentMinutes - fPressMinutes);
			Date day = fOwner->DayAt(fCurrentDay);
			// Keep the wall-clock length across DST.
			Instant start = ToInstant(DateTime(day, 0, 0)) + startMinutes * 60;
			if (startMinutes >= 0 && startMinutes < 24 * 60)
				start = ToInstant(DateTime(day, startMinutes / 60, startMinutes % 60));
			Instant end = start + (item.event.end - item.event.start);
			if (start != item.event.start)
				main->MoveEvent(item.event, start, end, false, day, 0);
		} else {
			int startMinutes = (int)((item.event.start - dayStart) / 60);
			int length = std::max(kSnapMinutes, fCurrentMinutes - startMinutes);
			Instant end = item.event.start + length * 60;
			if (end != item.event.end)
				main->MoveEvent(item.event, item.event.start, end, false,
					LocalDate(item.event.start), 0);
		}
		(void)first;
	}
	fActive = -1;
	Invalidate();
}


void
TimeGrid::KeyDown(const char* bytes, int32 count)
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
			Window()->PostMessage(kMsgGoPrevious);
			break;
		case B_RIGHT_ARROW:
			Window()->PostMessage(kMsgGoNext);
			break;
		case B_UP_ARROW:
			ScrollBy(0, -fHourHeight / 2);
			break;
		case B_DOWN_ARROW:
			ScrollBy(0, fHourHeight / 2);
			break;
		case 't':
		case 'T':
			Window()->PostMessage(kMsgGoToday);
			break;
		default:
			BView::KeyDown(bytes, count);
	}
}

}	// namespace


CalendarView*
CreateDayWeekView(int days)
{
	return new DayWeekView(days);
}
