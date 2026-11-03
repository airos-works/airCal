#include "SidebarView.h"

#include "App.h"
#include "Drawing.h"
#include "Format.h"
#include "MainWindow.h"
#include "Messages.h"
#include "Theme.h"
#include "core/QuickEntry.h"

#include <LayoutBuilder.h>
#include <PopUpMenu.h>
#include <MenuItem.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <StringView.h>
#include "HintField.h"

#include <TextView.h>
#include <Window.h>

#include <algorithm>
#include <cmath>
#include <set>

using namespace aircal;

namespace {

enum {
	kMsgEntryModified = 'enmd',
	kMsgEntryInvoked = 'enin',
	kMsgTab = 'stab',
	kMsgCalendarMenu = 'clmn'
};


MainWindow*
MainOf(BView* view)
{
	return view->Window() != nullptr ? static_cast<MainWindow*>(view->Window()) : nullptr;
}

}	// namespace


// --- MiniMonthView -----------------------------------------------------------------------

class MiniMonthView : public BView {
public:
	MiniMonthView()
		:
		BView("miniMonth", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_FRAME_EVENTS),
		fMonth(Today().year, Today().month, 1),
		fSelected(Today())
	{
		SetViewColor(B_TRANSPARENT_COLOR);
	}

	BSize MinSize() override { return BSize(200, 210); }
	BSize MaxSize() override { return BSize(B_SIZE_UNLIMITED, 210); }
	BSize PreferredSize() override { return BSize(250, 210); }

	void SetSelected(const Date& date)
	{
		fSelected = date;
		Date month(date.year, date.month, 1);
		if (month != fMonth) {
			fMonth = month;
			Reload();
		}
		Invalidate();
	}

	void SetRange(const Date& first, const Date& last)
	{
		fRangeFirst = first;
		fRangeLast = last;
		Invalidate();
	}

	void Reload()
	{
		Store& store = TheApp()->Store();
		{
			std::lock_guard<std::recursive_mutex> lock(store.Mutex());
			fWeekStart = store.Prefs().weekStart;
		}
		Date first = FirstShown();
		std::vector<DisplayEvent> events = store.EventsBetween(StartOfDay(first),
			StartOfDay(first.AddDays(42)));
		fBusy.clear();
		for (auto& event : events) {
			for (int64_t day = event.startDate.DayNumber();
					day <= event.endDate.DayNumber()
					&& day - event.startDate.DayNumber() < 42; day++) {
				auto& colors = fBusy[day];
				if (colors.size() < 3 && std::find(colors.begin(), colors.end(),
						event.color) == colors.end())
					colors.push_back(event.color);
			}
		}
		Invalidate();
	}

	Date FirstShown() const
	{
		int delta = (fMonth.Weekday() - fWeekStart + 7) % 7;
		return fMonth.AddDays(-delta);
	}

	BRect TitleRect() const { return BRect(10, 6, Bounds().right - 64, 28); }
	BRect PrevRect() const { return BRect(Bounds().right - 58, 6, Bounds().right - 34, 28); }
	BRect NextRect() const { return BRect(Bounds().right - 32, 6, Bounds().right - 8, 28); }

	BRect Cell(int row, int column) const
	{
		float width = (Bounds().Width() - 16) / 7;
		float top = 54;
		float height = 25;
		return BRect(8 + column * width, top + row * height,
			8 + (column + 1) * width - 1, top + (row + 1) * height - 1);
	}

	void Draw(BRect update) override
	{
		BRect bounds = Bounds();
		SetHighColor(Theme::Sidebar());
		FillRect(bounds);
		BFont font(be_plain_font);
		BFont bold(be_bold_font);
		bold.SetSize(font.Size() * 1.08f);
		SetDrawingMode(B_OP_ALPHA);
		SetFont(&bold);
		SetHighColor(Theme::Text());
		std::string title = Format::MonthLong(fMonth.month);
		DrawString(title.c_str(), BPoint(12, 23));
		float x = 12 + StringWidth(title.c_str());
		SetFont(&font);
		SetHighColor(Theme::Accent());
		DrawString((" " + std::to_string(fMonth.year)).c_str(), BPoint(x, 23));
		Drawing::DrawChevron(this, PrevRect(), true, Theme::SecondaryText());
		Drawing::DrawChevron(this, NextRect(), false, Theme::SecondaryText());

		BFont small(font);
		small.SetSize(font.Size() * 0.8f);
		SetFont(&small);
		for (int d = 0; d < 7; d++) {
			BRect cell = Cell(0, d);
			std::string letter = Format::WeekdayShort((fWeekStart + d) % 7).substr(0, 2);
			for (char& c : letter)
				c = (char)toupper((unsigned char)c);
			SetHighColor(Theme::TertiaryText());
			DrawString(letter.c_str(), BPoint(cell.left + (cell.Width()
				- StringWidth(letter.c_str())) / 2, 46));
		}
		Date today = Today();
		Date first = FirstShown();
		SetFont(&font);
		font_height height;
		font.GetHeight(&height);
		for (int i = 0; i < 42; i++) {
			Date date = first.AddDays(i);
			BRect cell = Cell(i / 7, i % 7);
			BPoint center((cell.left + cell.right) / 2, cell.top + 10);
			bool inRange = fRangeFirst <= date && date <= fRangeLast
				&& fRangeFirst != fRangeLast;
			if (inRange) {
				SetHighColor(Theme::Mix(Theme::Sidebar(), Theme::Accent(), 0.1f));
				FillRect(BRect(cell.left, cell.top - 1, cell.right, cell.top + 21));
			}
			if (date == fSelected && date != today)
				Drawing::FillCircle(this, center, 10.5f, Theme::Mix(Theme::Sidebar(),
					Theme::Text(), 0.15f));
			if (date == today)
				Drawing::FillCircle(this, center, 10.5f, Theme::Accent());
			std::string text = std::to_string(date.day);
			if (date == today)
				SetHighColor(Theme::AccentText());
			else if (date.month != fMonth.month)
				SetHighColor(Theme::TertiaryText());
			else if (date.Weekday() == 0 || date.Weekday() == 6)
				SetHighColor(Theme::SecondaryText());
			else
				SetHighColor(Theme::Text());
			DrawString(text.c_str(), BPoint(center.x - StringWidth(text.c_str()) / 2,
				center.y + height.ascent / 2 - 1));
			auto busy = fBusy.find(date.DayNumber());
			if (busy != fBusy.end()) {
				int count = (int)busy->second.size();
				float dotX = center.x - (count - 1) * 3.5f;
				for (int k = 0; k < count; k++) {
					Drawing::FillCircle(this, BPoint(dotX + k * 7, cell.top + 22), 1.8f,
						Theme::FromHex(busy->second[k]));
				}
			}
		}
		SetDrawingMode(B_OP_COPY);
	}

	void MouseDown(BPoint where) override
	{
		if (PrevRect().Contains(where)) {
			fMonth = fMonth.AddMonths(-1);
			Reload();
			return;
		}
		if (NextRect().Contains(where)) {
			fMonth = fMonth.AddMonths(1);
			Reload();
			return;
		}
		if (TitleRect().Contains(where)) {
			BMessage message(kMsgGoToDate);
			message.AddString("date", fMonth.ToIso().c_str());
			message.AddString("view", "month");
			Window()->PostMessage(&message);
			return;
		}
		Date first = FirstShown();
		for (int i = 0; i < 42; i++) {
			if (!Cell(i / 7, i % 7).Contains(where))
				continue;
			Date date = first.AddDays(i);
			BMessage* current = Window()->CurrentMessage();
			int32 clicks = current != nullptr ? current->GetInt32("clicks", 1) : 1;
			BMessage message(kMsgSelectDate);
			message.AddString("date", date.ToIso().c_str());
			Window()->PostMessage(&message);
			if (clicks >= 2) {
				BMessage day(kMsgSetView);
				day.AddString("view", "day");
				Window()->PostMessage(&day);
			}
			return;
		}
	}

	void MessageReceived(BMessage* message) override
	{
		if (message->what == B_MOUSE_WHEEL_CHANGED) {
			float delta = 0;
			if (message->FindFloat("be:wheel_delta_y", &delta) == B_OK && delta != 0) {
				fMonth = fMonth.AddMonths(delta > 0 ? 1 : -1);
				Reload();
			}
			return;
		}
		BView::MessageReceived(message);
	}

private:
	Date fMonth;
	Date fSelected;
	Date fRangeFirst;
	Date fRangeLast;
	int fWeekStart = 0;
	std::map<int64_t, std::vector<uint32_t>> fBusy;
};


// --- SidebarTabs -------------------------------------------------------------------------

class SidebarTabs : public BView {
public:
	SidebarTabs()
		:
		BView("tabs", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_FRAME_EVENTS)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
	}

	BSize MinSize() override { return BSize(150, 34); }
	BSize MaxSize() override { return BSize(B_SIZE_UNLIMITED, 34); }
	BSize PreferredSize() override { return BSize(250, 34); }

	bool Calendars() const { return fCalendars; }
	void SetCalendars(bool calendars) { fCalendars = calendars; Invalidate(); }

	void Draw(BRect update) override
	{
		BRect bounds = Bounds();
		SetHighColor(Theme::Sidebar());
		FillRect(bounds);
		BRect control(10, 4, bounds.right - 10, bounds.bottom - 4);
		SetDrawingMode(B_OP_ALPHA);
		Drawing::FillRound(this, control, 6, Theme::Control());
		float half = control.Width() / 2;
		const char* names[] = {"Upcoming", "Calendars"};
		BFont font(be_plain_font);
		for (int i = 0; i < 2; i++) {
			BRect part(control.left + i * half, control.top, control.left + (i + 1) * half,
				control.bottom);
			bool selected = (i == 1) == fCalendars;
			if (selected)
				Drawing::FillRound(this, part.InsetByCopy(2, 2), 5, Theme::ControlSelected());
			SetHighColor(selected ? Theme::Text() : Theme::SecondaryText());
			DrawString(names[i], BPoint(part.left + (part.Width()
				- StringWidth(names[i])) / 2, Drawing::Baseline(font, part)));
		}
		SetDrawingMode(B_OP_COPY);
	}

	void MouseDown(BPoint where) override
	{
		bool calendars = where.x > Bounds().Width() / 2;
		if (calendars != fCalendars) {
			fCalendars = calendars;
			Invalidate();
			BMessage message(kMsgTab);
			message.AddBool("calendars", calendars);
			Parent()->MessageReceived(&message);
		}
	}

private:
	bool fCalendars = false;
};


// --- UpcomingView ----------------------------------------------------------------------------

class UpcomingView : public BView {
public:
	struct Item {
		enum Kind { Header, EventRow, TaskRow, Empty } kind = Header;
		std::string text;
		std::string detail;
		Date date;
		DisplayEvent event;
		DisplayTask task;
		BRect frame;
		bool today = false;
	};

	UpcomingView()
		:
		BView("upcoming", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_FRAME_EVENTS | B_NAVIGABLE)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
	}

	void TargetedByScrollView(BScrollView* scroller) override { fScroller = scroller; }
	void FrameResized(float, float) override { UpdateScrollBar(); }

	void SetSearch(const std::string& text) { fSearch = text; Reload(); }
	void SetStart(const Date& date) { fStart = date; }

	void Reload()
	{
		Store& store = TheApp()->Store();
		fItems.clear();
		bool showTasks, showCompleted;
		{
			std::lock_guard<std::recursive_mutex> lock(store.Mutex());
			showTasks = store.Prefs().showTasksInCalendar;
			showCompleted = store.Prefs().showCompleted;
		}
		if (!fSearch.empty()) {
			std::vector<DisplayEvent> events = store.Search(fSearch, 80);
			std::vector<DisplayTask> tasks = store.SearchTasks(fSearch, 40);
			Item header;
			header.text = events.empty() && tasks.empty() ? "No results"
				: "Results for “" + fSearch + "”";
			fItems.push_back(header);
			for (auto& event : events) {
				Item item;
				item.kind = Item::EventRow;
				item.event = event;
				item.detail = Format::DateMedium(event.startDate);
				fItems.push_back(item);
			}
			if (!tasks.empty()) {
				Item tasksHeader;
				tasksHeader.text = "Tasks";
				fItems.push_back(tasksHeader);
				for (auto& task : tasks) {
					Item item;
					item.kind = Item::TaskRow;
					item.task = task;
					fItems.push_back(item);
				}
			}
		} else {
			Date today = Today();
			Date start = today;
			Date end = start.AddDays(30);
			std::vector<DisplayEvent> events = store.EventsBetween(StartOfDay(start),
				StartOfDay(end));
			std::vector<DisplayTask> tasks;
			if (showTasks)
				tasks = store.TasksBetween(start, end, showCompleted, true);
			std::map<int64_t, std::pair<std::vector<DisplayEvent>,
				std::vector<DisplayTask>>> days;
			Instant now = Now();
			for (auto& event : events) {
				// Ongoing events show on today; finished ones drop off.
				if (!event.allDay && event.end <= now)
					continue;
				Date day = std::max(event.startDate, today);
				days[day.DayNumber()].first.push_back(event);
			}
			for (auto& task : tasks) {
				Date day = task.dueDate < today ? today : task.dueDate;
				days[day.DayNumber()].second.push_back(task);
			}
			days[today.DayNumber()];	// today always shows
			for (auto& day : days) {
				Date date = Date::FromDayNumber(day.first);
				Item header;
				header.date = date;
				header.today = date == today;
				std::string relative = Format::RelativeDay(date);
				header.text = relative == Format::DateMedium(date)
					? Format::WeekdayLong(date.Weekday()) : relative;
				header.detail = Format::DateShort(date);
				for (char& c : header.text)
					c = (char)toupper((unsigned char)c);
				fItems.push_back(header);
				if (day.second.first.empty() && day.second.second.empty()) {
					Item empty;
					empty.kind = Item::Empty;
					empty.text = "No events";
					fItems.push_back(empty);
				}
				for (auto& event : day.second.first) {
					Item item;
					item.kind = Item::EventRow;
					item.event = event;
					fItems.push_back(item);
				}
				for (auto& task : day.second.second) {
					Item item;
					item.kind = Item::TaskRow;
					item.task = task;
					fItems.push_back(item);
				}
			}
		}
		Relayout();
	}

	void Relayout()
	{
		float y = 4;
		for (Item& item : fItems) {
			float height = item.kind == Item::Header ? 30
				: item.kind == Item::Empty ? 24 : 40;
			item.frame = BRect(0, y, 10000, y + height - 1);
			y += height;
		}
		fHeight = y + 8;
		UpdateScrollBar();
		Invalidate();
	}

	void UpdateScrollBar()
	{
		if (fScroller == nullptr)
			return;
		BScrollBar* bar = fScroller->ScrollBar(B_VERTICAL);
		if (bar == nullptr)
			return;
		float visible = Bounds().Height();
		bar->SetRange(0, std::max(0.0f, fHeight - visible));
		bar->SetProportion(fHeight > 0 ? std::min(1.0f, visible / fHeight) : 1);
		bar->SetSteps(20, std::max(40.0f, visible - 40));
	}

	void Draw(BRect update) override
	{
		BRect bounds = Bounds();
		SetHighColor(Theme::Sidebar());
		FillRect(update);
		MainWindow* main = MainOf(this);
		BFont font(be_plain_font);
		BFont small(font);
		small.SetSize(font.Size() * 0.86f);
		BFont header(be_bold_font);
		header.SetSize(font.Size() * 0.8f);
		SetDrawingMode(B_OP_ALPHA);
		for (const Item& item : fItems) {
			BRect frame = item.frame;
			frame.right = bounds.right;
			if (!frame.Intersects(update))
				continue;
			switch (item.kind) {
				case Item::Header:
				{
					SetFont(&header);
					SetHighColor(item.today ? Theme::Accent() : Theme::SecondaryText());
					DrawString(item.text.c_str(), BPoint(14, frame.bottom - 8));
					if (!item.detail.empty()) {
						SetHighColor(Theme::TertiaryText());
						DrawString(item.detail.c_str(), BPoint(bounds.right - 12
							- StringWidth(item.detail.c_str()), frame.bottom - 8));
					}
					SetHighColor(Theme::Separator());
					StrokeLine(BPoint(14, frame.bottom - 1), BPoint(bounds.right - 10,
						frame.bottom - 1));
					break;
				}
				case Item::Empty:
					SetFont(&small);
					SetHighColor(Theme::TertiaryText());
					DrawString(item.text.c_str(), BPoint(24, frame.bottom - 7));
					break;
				case Item::EventRow:
				{
					const DisplayEvent& event = item.event;
					bool selected = main != nullptr && main->CurrentSelection().IsEvent(event);
					BRect row(8, frame.top + 2, bounds.right - 8, frame.bottom - 2);
					if (selected)
						Drawing::FillRound(this, row, 6, Theme::Mix(Theme::Sidebar(),
							Theme::FromHex(event.color), Theme::IsDark() ? 0.35f : 0.2f));
					SetHighColor(Theme::EventBar(event.color));
					FillRoundRect(BRect(row.left + 6, row.top + 5, row.left + 9,
						row.bottom - 5), 1.5f, 1.5f);
					SetFont(&font);
					SetHighColor(Theme::Text());
					std::string time = event.allDay ? "all-day" : Format::Time(event.start);
					if (!item.detail.empty())
						time.clear();	// search results: the date goes below
					SetFont(&small);
					float timeWidth = StringWidth(time.c_str());
					SetFont(&font);
					float textLeft = row.left + 16;
					DrawString(Drawing::Fit(this, event.title, row.Width() - 26
						- timeWidth).c_str(), BPoint(textLeft, row.top + 15));
					SetFont(&small);
					SetHighColor(Theme::SecondaryText());
					DrawString(time.c_str(), BPoint(row.right - 6 - timeWidth, row.top + 15));
					std::string second = event.location;
					if (second.empty() && !event.allDay)
						second = Format::TimeRange(event.start, event.end);
					if (!item.detail.empty()) {
						second = item.detail + " · " + (event.allDay ? std::string("all-day")
							: Format::TimeRange(event.start, event.end));
					}
					SetHighColor(Theme::TertiaryText());
					DrawString(Drawing::Fit(this, second, row.Width() - 26).c_str(),
						BPoint(textLeft, row.top + 31));
					break;
				}
				case Item::TaskRow:
				{
					const DisplayTask& task = item.task;
					bool selected = main != nullptr && main->CurrentSelection().IsTask(task.taskId);
					BRect row(8, frame.top + 2, bounds.right - 8, frame.bottom - 2);
					if (selected)
						Drawing::FillRound(this, row, 6, Theme::Mix(Theme::Sidebar(),
							Theme::FromHex(task.color), Theme::IsDark() ? 0.35f : 0.2f));
					rgb_color ring = task.priority >= 2 ? Theme::Priority(task.priority)
						: Theme::FromHex(task.color);
					Drawing::DrawCheck(this, BPoint(row.left + 12, row.top + 11), 6.5f, ring,
						task.completed);
					SetFont(&font);
					SetHighColor(task.completed ? Theme::TertiaryText() : Theme::Text());
					DrawString(Drawing::Fit(this, task.title, row.Width() - 34).c_str(),
						BPoint(row.left + 26, row.top + 15));
					SetFont(&small);
					std::string due;
					if (task.hasDue) {
						due = task.dueDate < Today() ? "Overdue · "
							+ Format::RelativeDay(task.dueDate)
							: (task.dueHasTime ? Format::Time(task.dueTime) : "");
					}
					std::string second = due.empty() ? task.listName
						: due + " · " + task.listName;
					SetHighColor(task.hasDue && task.dueDate < Today() && !task.completed
						? Theme::Accent() : Theme::TertiaryText());
					DrawString(Drawing::Fit(this, second, row.Width() - 34).c_str(),
						BPoint(row.left + 26, row.top + 31));
					break;
				}
			}
		}
		SetDrawingMode(B_OP_COPY);
	}

	void MouseDown(BPoint where) override
	{
		MakeFocus(true);
		MainWindow* main = MainOf(this);
		BMessage* current = Window()->CurrentMessage();
		int32 clicks = current != nullptr ? current->GetInt32("clicks", 1) : 1;
		int32 buttons = current != nullptr
			? current->GetInt32("buttons", B_PRIMARY_MOUSE_BUTTON) : B_PRIMARY_MOUSE_BUTTON;
		BPoint screen = ConvertToScreen(where);
		for (const Item& item : fItems) {
			if (where.y < item.frame.top || where.y > item.frame.bottom)
				continue;
			if (item.kind == Item::Header && item.date.IsValid() && item.date.year > 1970) {
				BMessage message(kMsgSelectDate);
				message.AddString("date", item.date.ToIso().c_str());
				Window()->PostMessage(&message);
			} else if (item.kind == Item::EventRow) {
				if (buttons & B_SECONDARY_MOUSE_BUTTON)
					main->ShowEventMenu(item.event, screen);
				else if (clicks >= 2)
					main->EditEvent(item.event, screen);
				else {
					main->SelectEvent(item.event);
					main->GoTo(item.event.startDate);
				}
			} else if (item.kind == Item::TaskRow) {
				if (where.x < 34)
					main->ToggleTask(item.task.taskId, !item.task.completed);
				else if (buttons & B_SECONDARY_MOUSE_BUTTON)
					main->ShowTaskMenu(item.task.taskId, screen);
				else if (clicks >= 2)
					main->EditTask(item.task.taskId, screen);
				else
					main->SelectTask(item.task.taskId);
			}
			return;
		}
	}

	void KeyDown(const char* bytes, int32 count) override
	{
		if (bytes[0] == B_DELETE || bytes[0] == B_BACKSPACE)
			Window()->PostMessage(kMsgDeleteSelection);
		else if (bytes[0] == B_ENTER)
			Window()->PostMessage('edsl');
		else
			BView::KeyDown(bytes, count);
	}

private:
	BScrollView* fScroller = nullptr;
	std::vector<Item> fItems;
	std::string fSearch;
	Date fStart;
	float fHeight = 0;
};


// --- CalendarListView -------------------------------------------------------------------------

class CalendarListView : public BView {
public:
	struct Row {
		bool account = false;
		bool section = false;
		std::string id;		// account or calendar id
		std::string name;
		std::string detail;
		uint32_t color = 0;
		bool visible = true;
		bool tasks = false;
		bool error = false;
		int depth = 0;
		BRect frame;
	};

	CalendarListView()
		:
		BView("calendars", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_FRAME_EVENTS)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
	}

	void TargetedByScrollView(BScrollView* scroller) override { fScroller = scroller; }
	void FrameResized(float, float) override { UpdateScrollBar(); }

	void Reload()
	{
		fRows.clear();
		Store& store = TheApp()->Store();
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		for (auto& account : store.Accounts()) {
			Row header;
			header.account = true;
			header.id = account->id;
			header.name = account->name;
			header.error = !account->lastError.empty();
			static const char* kServices[] = {"", "Google", "CalDAV", "Todoist"};
			header.detail = !account->enabled ? "Off" : header.error
				? account->lastError : kServices[(int)account->type];
			fRows.push_back(header);
			std::vector<Calendar*> calendars = store.CalendarsOf(account->id);
			bool hasEvents = false, hasTasks = false;
			for (Calendar* calendar : calendars) {
				hasEvents = hasEvents || calendar->events;
				hasTasks = hasTasks || (calendar->tasks && !calendar->events);
			}
			for (int pass = 0; pass < 2; pass++) {
				bool tasks = pass == 1;
				if (tasks && hasTasks && hasEvents) {
					Row section;
					section.section = true;
					section.name = "Task Lists";
					fRows.push_back(section);
				}
				for (Calendar* calendar : calendars) {
					bool isTaskList = calendar->tasks && !calendar->events;
					if (isTaskList != tasks)
						continue;
					Row row;
					row.id = calendar->id;
					row.name = calendar->name;
					row.color = calendar->color;
					row.visible = calendar->visible;
					row.tasks = isTaskList;
					row.depth = calendar->parentId.empty() ? 0 : 1;
					if (calendar->readOnly)
						row.detail = "read-only";
					fRows.push_back(row);
				}
			}
		}
		float y = 4;
		for (Row& row : fRows) {
			float height = row.account ? 32 : (row.section ? 22 : 26);
			row.frame = BRect(0, y, 10000, y + height - 1);
			y += height;
		}
		fHeight = y + 40;
		UpdateScrollBar();
		Invalidate();
	}

	void UpdateScrollBar()
	{
		if (fScroller == nullptr)
			return;
		BScrollBar* bar = fScroller->ScrollBar(B_VERTICAL);
		if (bar == nullptr)
			return;
		float visible = Bounds().Height();
		bar->SetRange(0, std::max(0.0f, fHeight - visible));
		bar->SetProportion(fHeight > 0 ? std::min(1.0f, visible / fHeight) : 1);
	}

	void Draw(BRect update) override
	{
		BRect bounds = Bounds();
		SetHighColor(Theme::Sidebar());
		FillRect(update);
		BFont font(be_plain_font);
		BFont bold(be_bold_font);
		bold.SetSize(font.Size() * 0.82f);
		BFont small(font);
		small.SetSize(font.Size() * 0.82f);
		SetDrawingMode(B_OP_ALPHA);
		for (const Row& row : fRows) {
			BRect frame = row.frame;
			frame.right = bounds.right;
			if (!frame.Intersects(update))
				continue;
			if (row.account) {
				SetFont(&bold);
				SetHighColor(Theme::SecondaryText());
				std::string name = row.name;
				for (char& c : name)
					c = (char)toupper((unsigned char)c);
				DrawString(Drawing::Fit(this, name, bounds.Width() - 60).c_str(),
					BPoint(14, frame.bottom - 8));
				if (!row.detail.empty()) {
					SetFont(&small);
					SetHighColor(row.error ? Theme::Accent() : Theme::TertiaryText());
					std::string detail = row.error ? "!" : row.detail;
					DrawString(detail.c_str(), BPoint(bounds.right - 14
						- StringWidth(detail.c_str()), frame.bottom - 8));
				}
				continue;
			}
			if (row.section) {
				SetFont(&small);
				SetHighColor(Theme::TertiaryText());
				DrawString(row.name.c_str(), BPoint(22, frame.bottom - 6));
				continue;
			}
			float left = 18 + row.depth * 16;
			BRect box(left, frame.top + 6, left + 13, frame.top + 19);
			rgb_color color = Theme::FromHex(row.color);
			if (row.tasks) {
				Drawing::DrawCheck(this, BPoint((box.left + box.right) / 2,
					(box.top + box.bottom) / 2), 6.5f, color, row.visible);
			} else if (row.visible) {
				Drawing::FillRound(this, box, 3, color);
				SetHighColor(255, 255, 255);
				SetPenSize(1.6f);
				StrokeLine(BPoint(box.left + 3, box.top + 7), BPoint(box.left + 5.5f, box.top + 9.5f));
				StrokeLine(BPoint(box.left + 5.5f, box.top + 9.5f), BPoint(box.right - 3, box.top + 3.5f));
				SetPenSize(1);
			} else
				Drawing::StrokeRound(this, box, 3, color, 1.5f);
			SetFont(&font);
			SetHighColor(row.visible ? Theme::Text() : Theme::SecondaryText());
			DrawString(Drawing::Fit(this, row.name, bounds.Width() - left - 90).c_str(),
				BPoint(left + 22, frame.top + 17));
			if (!row.detail.empty()) {
				SetFont(&small);
				SetHighColor(Theme::TertiaryText());
				DrawString(row.detail.c_str(), BPoint(bounds.right - 12
					- StringWidth(row.detail.c_str()), frame.top + 17));
			}
		}
		// Footer: add account.
		float y = fRows.empty() ? 10 : fRows.back().frame.bottom + 12;
		fAddFrame = BRect(14, y, bounds.right - 14, y + 24);
		SetFont(&font);
		SetHighColor(Theme::Accent());
		Drawing::DrawPlus(this, BRect(fAddFrame.left, fAddFrame.top + 4,
			fAddFrame.left + 16, fAddFrame.top + 20), Theme::Accent(), 1.6f);
		DrawString("Add Account…", BPoint(fAddFrame.left + 22, fAddFrame.top + 16));
		SetDrawingMode(B_OP_COPY);
	}

	void MouseDown(BPoint where) override
	{
		BMessage* current = Window()->CurrentMessage();
		int32 buttons = current != nullptr
			? current->GetInt32("buttons", B_PRIMARY_MOUSE_BUTTON) : B_PRIMARY_MOUSE_BUTTON;
		if (fAddFrame.Contains(where)) {
			Window()->PostMessage(kMsgShowAccounts);
			return;
		}
		for (const Row& row : fRows) {
			if (where.y < row.frame.top || where.y > row.frame.bottom || row.section)
				continue;
			if (row.account) {
				Window()->PostMessage(kMsgShowAccounts);
				return;
			}
			float left = 18 + row.depth * 16;
			bool onBox = where.x < left + 18;
			if ((buttons & B_SECONDARY_MOUSE_BUTTON) != 0) {
				ShowMenu(row, ConvertToScreen(where));
				return;
			}
			if (onBox || !row.tasks) {
				Store& store = TheApp()->Store();
				{
					std::lock_guard<std::recursive_mutex> lock(store.Mutex());
					Calendar* calendar = store.FindCalendar(row.id);
					if (calendar != nullptr)
						calendar->visible = !calendar->visible;
				}
				store.Changed();
				TheApp()->ScheduleSave();
				return;
			}
			// A task list's name shows its tasks.
			BMessage message(kMsgTaskFilter);
			message.AddString("filter", ("list:" + row.id).c_str());
			Window()->PostMessage(&message);
			return;
		}
	}

	void ShowMenu(const Row& row, BPoint screen)
	{
		BPopUpMenu* menu = new BPopUpMenu("calendar", false, false);
		BMessage* only = new BMessage(kMsgCalendarMenu);
		only->AddString("id", row.id.c_str());
		only->AddString("action", "only");
		menu->AddItem(new BMenuItem("Show Only This", only));
		BMessage* all = new BMessage(kMsgCalendarMenu);
		all->AddString("id", row.id.c_str());
		all->AddString("action", "all");
		menu->AddItem(new BMenuItem("Show All", all));
		menu->AddSeparatorItem();
		BMessage* makeDefault = new BMessage(kMsgCalendarMenu);
		makeDefault->AddString("id", row.id.c_str());
		makeDefault->AddString("action", "default");
		menu->AddItem(new BMenuItem(row.tasks ? "Use for New Tasks"
			: "Use for New Events", makeDefault));
		menu->SetTargetForItems(this);
		menu->SetAsyncAutoDestruct(true);
		menu->Go(screen, true, true, true);
	}

	void MessageReceived(BMessage* message) override
	{
		if (message->what != kMsgCalendarMenu) {
			BView::MessageReceived(message);
			return;
		}
		std::string id = message->GetString("id", "");
		std::string action = message->GetString("action", "");
		Store& store = TheApp()->Store();
		{
			std::lock_guard<std::recursive_mutex> lock(store.Mutex());
			Calendar* target = store.FindCalendar(id);
			if (target == nullptr)
				return;
			bool tasks = target->tasks && !target->events;
			if (action == "only" || action == "all") {
				for (auto& calendar : store.Calendars()) {
					bool isTaskList = calendar->tasks && !calendar->events;
					if (isTaskList == tasks)
						calendar->visible = action == "all" || calendar->id == id;
				}
			} else if (action == "default") {
				if (tasks)
					store.Prefs().defaultTaskList = id;
				else
					store.Prefs().defaultCalendar = id;
			}
		}
		store.Changed();
		TheApp()->ScheduleSave();
	}

private:
	BScrollView* fScroller = nullptr;
	std::vector<Row> fRows;
	float fHeight = 0;
	BRect fAddFrame;
};


// --- StatusLine -----------------------------------------------------------------------------

class StatusLine : public BView {
public:
	StatusLine()
		:
		BView("status", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_FRAME_EVENTS)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
	}

	BSize MinSize() override { return BSize(100, 26); }
	BSize MaxSize() override { return BSize(B_SIZE_UNLIMITED, 26); }
	BSize PreferredSize() override { return BSize(250, 26); }

	void Set(const std::string& text, bool error)
	{
		fText = text;
		fError = error;
		SetToolTip(text.c_str());
		Invalidate();
	}

	void Draw(BRect update) override
	{
		BRect bounds = Bounds();
		SetHighColor(Theme::Sidebar());
		FillRect(bounds);
		SetHighColor(Theme::Separator());
		StrokeLine(BPoint(0, 0), BPoint(bounds.right, 0));
		BFont font(be_plain_font);
		BFont small(font);
		small.SetSize(font.Size() * 0.82f);
		SetFont(&small);
		SetDrawingMode(B_OP_ALPHA);
		if (fError)
			Drawing::FillCircle(this, BPoint(14, bounds.Height() / 2 + 1), 3.5f,
				Theme::Accent());
		SetHighColor(fError ? Theme::Accent() : Theme::TertiaryText());
		DrawString(Drawing::Fit(this, fText, bounds.Width() - 30).c_str(),
			BPoint(fError ? 24 : 12, Drawing::Baseline(small, bounds)));
		SetDrawingMode(B_OP_COPY);
	}

	void MouseDown(BPoint where) override
	{
		Window()->PostMessage(fError ? kMsgShowAccounts : kMsgSyncNow);
	}

private:
	std::string fText;
	bool fError = false;
};


// --- SidebarView -----------------------------------------------------------------------------

SidebarView::SidebarView()
	:
	BView("sidebar", B_WILL_DRAW)
{
	SetViewColor(B_TRANSPARENT_COLOR);
	fEntry = new HintField("quickEntry", "New event or task…",
		new BMessage(kMsgEntryInvoked), new BMessage(kMsgEntryModified),
		HintField::kPlus);
	fPreview = new BStringView("preview", "");
	fPreview->SetExplicitMinSize(BSize(50, B_SIZE_UNSET));
	fPreview->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	fPreview->SetTruncation(B_TRUNCATE_END);
	BFont small(be_plain_font);
	small.SetSize(be_plain_font->Size() * 0.85f);
	fPreview->SetFont(&small);
	fMiniMonth = new MiniMonthView();
	fTabs = new SidebarTabs();
	fUpcoming = new UpcomingView();
	fCalendars = new CalendarListView();
	fUpcomingScroll = new BScrollView("upcomingScroll", fUpcoming, 0, false, true,
		B_NO_BORDER);
	fCalendarsScroll = new BScrollView("calendarsScroll", fCalendars, 0, false, true,
		B_NO_BORDER);
	fStatus = new StatusLine();

	BGroupView* entryBox = new BGroupView(B_VERTICAL, 2);
	BLayoutBuilder::Group<>(entryBox)
		.SetInsets(10, 10, 10, 2)
		.Add(fEntry)
		.Add(fPreview);
	entryBox->SetViewColor(B_TRANSPARENT_COLOR);

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(entryBox)
		.Add(fMiniMonth)
		.Add(fTabs)
		.Add(fUpcomingScroll, 1.0f)
		.Add(fCalendarsScroll, 1.0f)
		.Add(fStatus);
	fCalendarsScroll->Hide();
	SetExplicitMinSize(BSize(220, B_SIZE_UNSET));
	SetExplicitMaxSize(BSize(480, B_SIZE_UNLIMITED));
}


void
SidebarView::AttachedToWindow()
{
	BView::AttachedToWindow();
	fEntry->SetTarget(BMessenger(this));
	fEntry->SetToolTip("Type naturally: \"Lunch with Sam tomorrow at noon\", "
		"\"todo Pay rent Friday !!\"");
	fPreview->SetHighColor(Theme::SecondaryText());
	fEntry->Parent()->SetViewColor(Theme::Sidebar());
	Reload();
}


void
SidebarView::Draw(BRect update)
{
	SetHighColor(Theme::Sidebar());
	FillRect(update);
}


void
SidebarView::Reload()
{
	fMiniMonth->Reload();
	fUpcoming->Reload();
	fCalendars->Reload();
	fEntry->Parent()->SetViewColor(Theme::Sidebar());
	fEntry->Parent()->Invalidate();
	fPreview->SetViewColor(Theme::Sidebar());
	fPreview->SetLowColor(Theme::Sidebar());
	fPreview->SetHighColor(Theme::SecondaryText());
	fPreview->Invalidate();
	fTabs->Invalidate();
	fStatus->Invalidate();
	Invalidate();
}


void
SidebarView::SetSelectedDate(const Date& date)
{
	fMiniMonth->SetSelected(date);
}


void
SidebarView::SetVisibleRange(const Date& first, const Date& last)
{
	fMiniMonth->SetRange(first, last);
}


void
SidebarView::SetSearch(const std::string& text)
{
	if (!text.empty() && fTabs->Calendars())
		ShowCalendars(false);
	fUpcoming->SetSearch(text);
}


void
SidebarView::SetStatus(const std::string& text, bool error)
{
	fStatus->Set(text, error);
}


void
SidebarView::ShowCalendars(bool calendars)
{
	fTabs->SetCalendars(calendars);
	if (calendars) {
		if (!fUpcomingScroll->IsHidden())
			fUpcomingScroll->Hide();
		if (fCalendarsScroll->IsHidden())
			fCalendarsScroll->Show();
	} else {
		if (!fCalendarsScroll->IsHidden())
			fCalendarsScroll->Hide();
		if (fUpcomingScroll->IsHidden())
			fUpcomingScroll->Show();
	}
}


void
SidebarView::FocusQuickEntry()
{
	fEntry->MakeFocus(true);
	fEntry->SelectAll();
}


void
SidebarView::UpdatePreview()
{
	std::string text = fEntry->Text();
	if (text.empty()) {
		fPreview->SetText("");
		return;
	}
	int duration, weekStart;
	{
		std::lock_guard<std::recursive_mutex> lock(TheApp()->Store().Mutex());
		duration = TheApp()->Store().Prefs().defaultDuration;
		weekStart = TheApp()->Store().Prefs().weekStart;
	}
	QuickEntryResult result = ParseQuickEntry(text, Now(), duration, weekStart);
	fPreview->SetText(result.Describe().c_str());
	fPreview->SetToolTip(result.Describe().c_str());
}


void
SidebarView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgEntryModified:
			UpdatePreview();
			break;
		case kMsgEntryInvoked:
		{
			std::string text = fEntry->Text();
			if (text.empty())
				break;
			BMessage entry(kMsgQuickEntry);
			entry.AddString("text", text.c_str());
			Window()->PostMessage(&entry);
			fEntry->SetText("");
			fPreview->SetText("");
			break;
		}
		case kMsgTab:
			ShowCalendars(message->GetBool("calendars", false));
			break;
		default:
			BView::MessageReceived(message);
	}
}
