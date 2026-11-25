#include "DuePicker.h"

#include "DateNavigator.h"
#include "EventEditor.h"
#include "Format.h"

#include <MenuField.h>
#include <MobileKit.h>
#include <PopUpMenu.h>
#include <Screen.h>
#include <Spinner.h>
#include <StringView.h>
#include <TextView.h>

namespace mk = BMobileKit;
using namespace aircal;

namespace {
enum {
	kPick = 'dpok', kToday = 'dptd', kPreviousMonth = 'dppm',
	kNextMonth = 'dpnm', kBrowse = 'dpbr'
};

class DuePicker : public BWindow {
public:
	DuePicker(BWindow* owner, const char* title, uint32 reply)
		: BWindow(owner->Frame(), title, B_MODAL_WINDOW_LOOK, B_MODAL_SUBSET_WINDOW_FEEL,
			B_ASYNCHRONOUS_CONTROLS | B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_CLOSE_ON_ESCAPE),
		fEditor(owner), fReply(reply)
	{
		AddToSubset(owner);
	}

	void Open(BWindow* owner, float width = B_SIZE_UNSET, float extraHeight = 0)
	{
		Layout(true);
		ResizeToPreferred();
		if (width != B_SIZE_UNSET) {
			BRect screen = BScreen(owner).Frame();
			float available = std::min(owner->Frame().Height(), screen.Height() - 12);
			float ideal = std::max(Bounds().Height(), GetLayout()->MinSize().height + extraHeight);
			ResizeTo(std::min(width, screen.Width() - 12), std::min(available, ideal));
		}
		CenterIn(owner->Frame());
		MoveOnScreen();
		Layout(true);
		ReadyToPick();
		Show();
	}

protected:
	virtual void ReadyToPick() {}

	void Accept(BMessage& choice)
	{
		choice.what = fReply;
		fEditor.SendMessage(&choice);
		PostMessage(B_QUIT_REQUESTED);
	}
private:
	BMessenger fEditor;
	uint32 fReply;
};

class DayButton : public BButton {
public:
	DayButton() : BButton("day", "", new BMessage(kPick))
	{
		SetFlat(true);
		SetBehavior(B_TOGGLE_BEHAVIOR);
		BSize cell(ceilf(be_plain_font->StringWidth("88") + 1.5f * be_plain_font->Size()),
			ceilf(1.9f * be_plain_font->Size()));
		SetExplicitMinSize(cell);
		SetExplicitPreferredSize(cell);
		SetExplicitMaxSize(cell);
	}

	void SetDate(const Date& date, const Date& selected)
	{
		fDate = date;
		SetLabel(std::to_string(date.day).c_str());
		Message()->SetString("date", date.ToIso().c_str());
		SetEnabled(date.month == selected.month);
		SetValue(date == selected ? B_CONTROL_ON : B_CONTROL_OFF);
	}

	void KeyDown(const char* bytes, int32 count) override
	{
		Date date;
		switch (bytes[0]) {
			case B_LEFT_ARROW: date = fDate.AddDays(-1); break;
			case B_RIGHT_ARROW: date = fDate.AddDays(1); break;
			case B_UP_ARROW: date = fDate.AddDays(-7); break;
			case B_DOWN_ARROW: date = fDate.AddDays(7); break;
			case B_PAGE_UP: date = fDate.AddMonths(-1); break;
			case B_PAGE_DOWN: date = fDate.AddMonths(1); break;
			default: BButton::KeyDown(bytes, count); return;
		}
		BMessage browse(kBrowse);
		browse.AddString("date", date.ToIso().c_str());
		Window()->PostMessage(&browse);
	}

private:
	Date fDate;
};

class DesktopDatePicker : public DuePicker {
public:
	DesktopDatePicker(BWindow* owner, const Date& date, int weekStart, uint32 reply)
		: DuePicker(owner, "Due date", reply), fWeekStart(weekStart)
	{
		fMonth = new BStringView("month", "");
		fMonth->SetAlignment(B_ALIGN_CENTER);
		auto* previous = new BButton("previous", "←", new BMessage(kPreviousMonth));
		auto* next = new BButton("next", "→", new BMessage(kNextMonth));
		previous->SetToolTip("Previous month");
		next->SetToolTip("Next month");
		// Leave room for the longest month name when browsing in a fixed-size dialog.
		BSize arrowSize(ceilf(be_plain_font->Size() * 3), B_SIZE_UNSET);
		for (auto* arrow : {previous, next}) {
			arrow->SetExplicitMinSize(arrowSize);
			arrow->SetExplicitPreferredSize(arrowSize);
			arrow->SetExplicitMaxSize(arrowSize);
		}

		BLayoutBuilder::Group<> layout(this, B_VERTICAL, B_USE_SMALL_SPACING);
		layout.SetInsets(B_USE_WINDOW_SPACING).AddGroup(B_HORIZONTAL)
			.Add(previous).Add(fMonth, 1).Add(next);
		auto grid = layout.AddGrid(0.0f, 0.0f);
		for (int i = 0; i < 7; ++i) {
			std::string weekday = Format::WeekdayShort((weekStart + i) % 7);
			auto* heading = new BStringView(NULL, weekday.c_str());
			heading->SetAlignment(B_ALIGN_CENTER);
			grid.Add(heading, i, 0);
		}
		for (int i = 0; i < 42; ++i) {
			fDays[i] = new DayButton();
			grid.Add(fDays[i], i % 7, i / 7 + 1);
		}
		layout.AddGroup(B_HORIZONTAL)
			.Add(new BButton("today", "Today", new BMessage(kToday)))
			.AddGlue()
			.Add(new BButton("cancel", "Cancel", new BMessage(B_QUIT_REQUESTED)));
		ShowMonth(date);
	}

	void MessageReceived(BMessage* message) override
	{
		switch (message->what) {
			case kPreviousMonth:
			case kNextMonth:
				ShowMonth(fSelected.AddMonths(message->what == kNextMonth ? 1 : -1));
				break;
			case kBrowse:
			{
				Date date;
				if (Date::FromIso(message->GetString("date", ""), date))
					ShowMonth(date);
				break;
			}
			case kToday:
				message->SetString("date", Today().ToIso().c_str());
				[[fallthrough]];
			case kPick:
				Accept(*message);
				break;
			default:
				DuePicker::MessageReceived(message);
		}
	}

private:
	void ShowMonth(const Date& date)
	{
		fSelected = date;
		std::string month = Format::MonthLong(date.month) + " " + std::to_string(date.year);
		fMonth->SetText(month.c_str());
		Date first(date.year, date.month, 1);
		int offset = (first.Weekday() - fWeekStart + 7) % 7;
		for (int i = 0; i < 42; ++i)
			fDays[i]->SetDate(first.AddDays(i - offset), date);
		fDays[offset + date.day - 1]->MakeFocus();
	}

	int fWeekStart;
	Date fSelected;
	BStringView* fMonth;
	DayButton* fDays[42];
};

class DesktopTimePicker : public DuePicker {
public:
	DesktopTimePicker(BWindow* owner, int hour, int minute, uint32 reply)
		: DuePicker(owner, "Due time", reply)
	{
		bool use24Hour = Format::Use24Hour();
		fHours = new BSpinner("hours", "Hour", NULL);
		fHours->SetRange(use24Hour ? 0 : 1, use24Hour ? 23 : 12);
		fHours->SetValue(use24Hour ? hour : (hour + 11) % 12 + 1);
		fMinutes = new BSpinner("minutes", "Minute", NULL);
		fMinutes->SetRange(0, 59);
		fMinutes->SetValue(minute);

		BLayoutBuilder::Group<> layout(this, B_VERTICAL);
		auto fields = layout.SetInsets(B_USE_WINDOW_SPACING).AddGroup(B_HORIZONTAL);
		fields.Add(fHours).Add(fMinutes);
		if (!use24Hour) {
			auto* menu = new BPopUpMenu("period");
			menu->AddItem(new BMenuItem("AM", NULL));
			fPM = new BMenuItem("PM", NULL);
			menu->AddItem(fPM);
			menu->ItemAt(hour >= 12 ? 1 : 0)->SetMarked(true);
			fields.Add(new BMenuField("period", NULL, menu));
		}
		auto* done = new BButton("done", "Set time", new BMessage(kPick));
		layout.AddGroup(B_HORIZONTAL).AddGlue()
			.Add(new BButton("cancel", "Cancel", new BMessage(B_QUIT_REQUESTED)))
			.Add(done);
		SetDefaultButton(done);
		fHours->MakeFocus();
		fHours->TextView()->SelectAll();
	}

	void MessageReceived(BMessage* message) override
	{
		if (message->what != kPick) {
			DuePicker::MessageReceived(message);
			return;
		}
		fHours->SetValueFromText();
		fMinutes->SetValueFromText();
		int hour = fHours->Value();
		if (fPM != nullptr)
			hour = hour % 12 + (fPM->IsMarked() ? 12 : 0);
		BMessage choice;
		choice.AddInt32("hour", hour);
		choice.AddInt32("minute", fMinutes->Value());
		Accept(choice);
	}

private:
	BSpinner* fHours;
	BSpinner* fMinutes;
	BMenuItem* fPM = nullptr;
};

class MobileDatePicker : public DuePicker {
public:
	MobileDatePicker(BWindow* owner, const Date& date, int weekStart, uint32 reply)
		: DuePicker(owner, "Due date", reply)
	{
		auto* page = new DateNavigator(date, true, weekStart, this, kPick);
		BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
			.Add(page)
			.AddGroup(B_HORIZONTAL)
				.SetInsets(B_USE_SMALL_SPACING)
				.Add(new BButton("today", "Today", new BMessage(kToday)))
				.AddGlue()
				.Add(new BButton("cancel", "Cancel", new BMessage(B_QUIT_REQUESTED)));
		page->FindView("dates")->MakeFocus();
	}

	void MessageReceived(BMessage* message) override
	{
		if (message->what == kToday) {
			BMessage choice;
			choice.AddString("date", Today().ToIso().c_str());
			Accept(choice);
		} else if (message->what == kPick)
			Accept(*message);
		else
			DuePicker::MessageReceived(message);
	}
};

class TimeValue : public mk::ListItem {
public:
	explicit TimeValue(const char* value) : ListItem(value) {}

	void DrawItem(BView* owner, BRect frame, bool) override
	{
		const auto& skin = mk::CurrentSkin();
		owner->PushState();
		rgb_color ink = skin.pageText;
		rgb_color emboss = skin.controlEmboss;
		if (IsSelected()) {
			mk::FillGradient(owner, frame, skin.selectedTop, skin.selectedBottom);
			ink = skin.selectedText;
			emboss = skin.selectedEmboss;
		} else
			mk::DrawPage(owner, frame, frame);
		owner->SetFont(be_bold_font);
		mk::DrawCentered(owner, Text(), frame, ink, emboss);
		owner->PopState();
	}
};

// Each column selects an exact wall-clock value. The selected minute is
// retained when reopening a task, including values between five-minute marks.
class MobileTimePicker : public DuePicker {
public:
	MobileTimePicker(BWindow* owner, int hour, int minute, uint32 reply)
		: DuePicker(owner, "Due time", reply), f24Hour(Format::Use24Hour())
	{
		fHours = new BListView("hours");
		fMinutes = new BListView("minutes");
		for (int i = 0; i < (f24Hour ? 24 : 12); ++i)
			fHours->AddItem(new TimeValue(std::to_string(f24Hour ? i : i + 1).c_str()));
		for (int i = 0; i < 60; ++i) {
			char text[4];
			snprintf(text, sizeof(text), "%02d", i);
			fMinutes->AddItem(new TimeValue(text));
		}
		auto* hours = mk::Scroll("hourScroll", fHours);
		auto* minutes = mk::Scroll("minuteScroll", fMinutes);
		float column = mk::TargetSize() * 2;
		for (auto* scroll : {hours, minutes}) {
			scroll->SetExplicitMinSize(BSize(column, column));
			scroll->SetExplicitPreferredSize(BSize(column, column * 2));
		}
		BLayoutBuilder::Group<> sheet(this, B_VERTICAL, B_USE_SMALL_SPACING);
		sheet.SetInsets(B_USE_WINDOW_SPACING)
			.AddGrid(B_USE_SMALL_SPACING, B_USE_SMALL_SPACING)
				.Add(new BStringView(NULL, "Hour"), 0, 0)
				.Add(new BStringView(NULL, "Minute"), 1, 0)
				.Add(hours, 0, 1).Add(minutes, 1, 1)
			.End();
		if (!f24Hour) {
			fPeriod = new mk::SegmentedControl("period", NULL);
			fPeriod->AddSegment("AM");
			fPeriod->AddSegment("PM");
			fPeriod->Select(hour >= 12 ? 1 : 0);
			sheet.Add(fPeriod);
		}
		auto* done = new BButton("done", "Set time", new BMessage(kPick));
		sheet.AddGroup(B_HORIZONTAL).AddGlue()
			.Add(new BButton("cancel", "Cancel", new BMessage(B_QUIT_REQUESTED)))
			.Add(done);
		SetDefaultButton(done);
		fHours->Select(f24Hour ? hour : (hour + 11) % 12);
		fMinutes->Select(minute);
	}

	~MobileTimePicker() override
	{
		for (auto* list : {fHours, fMinutes}) {
			while (auto* item = list->RemoveItem((int32)0))
				delete item;
		}
	}

	void ReadyToPick() override
	{
		fHours->ScrollToSelection();
		fMinutes->ScrollToSelection();
	}

	void MessageReceived(BMessage* message) override
	{
		if (message->what != kPick) {
			DuePicker::MessageReceived(message);
			return;
		}
		int hour = fHours->CurrentSelection();
		int minute = fMinutes->CurrentSelection();
		if (hour < 0 || minute < 0)
			return;
		if (!f24Hour)
			hour = (hour + 1) % 12 + 12 * fPeriod->Value();
		BMessage choice;
		choice.AddInt32("hour", hour);
		choice.AddInt32("minute", minute);
		Accept(choice);
	}

private:
	bool f24Hour;
	BListView* fHours;
	BListView* fMinutes;
	mk::SegmentedControl* fPeriod = nullptr;
};
}

void
PickDueDate(BWindow* editor, const Date& date, int weekStart, uint32 reply)
{
	if (!DialogKit::MobileEditor()) {
		(new DesktopDatePicker(editor, date, weekStart, reply))->Open(editor);
		return;
	}
	auto* picker = new MobileDatePicker(editor, date, weekStart, reply);
	picker->Open(editor, mk::TargetSize() * 7,
		6 * (mk::TargetSize() - 2 * be_plain_font->Size()));
}

void
PickDueTime(BWindow* editor, int hour, int minute, uint32 reply)
{
	if (!DialogKit::MobileEditor()) {
		(new DesktopTimePicker(editor, hour, minute, reply))->Open(editor);
		return;
	}
	auto* picker = new MobileTimePicker(editor, hour, minute, reply);
	picker->Open(editor, mk::TargetSize() * 5.5f, mk::TargetSize() * 2);
}
