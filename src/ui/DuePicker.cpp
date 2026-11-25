#include "DuePicker.h"

#include "DateNavigator.h"
#include "Format.h"

#include <MobileKit.h>
#include <Screen.h>
#include <StringView.h>

namespace mk = BMobileKit;
using namespace aircal;

namespace {
enum { kPick = 'dpok', kToday = 'dptd' };

class DuePicker : public BWindow {
public:
	DuePicker(BWindow* owner, const char* title, uint32 reply)
		: BWindow(owner->Frame(), title, B_MODAL_WINDOW_LOOK, B_MODAL_SUBSET_WINDOW_FEEL,
			B_ASYNCHRONOUS_CONTROLS | B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_CLOSE_ON_ESCAPE),
		fEditor(owner), fReply(reply)
	{
		AddToSubset(owner);
	}

	void Open(BWindow* owner, float width, float extraHeight)
	{
		Layout(true);
		ResizeToPreferred();
		BRect screen = BScreen(owner).Frame();
		float available = std::min(owner->Frame().Height(), screen.Height() - 12);
		float ideal = std::max(Bounds().Height(), GetLayout()->MinSize().height + extraHeight);
		ResizeTo(std::min(width, screen.Width() - 12), std::min(available, ideal));
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

class DatePicker : public DuePicker {
public:
	DatePicker(BWindow* owner, const Date& date, int weekStart, uint32 reply)
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
class TimePicker : public DuePicker {
public:
	TimePicker(BWindow* owner, int hour, int minute, uint32 reply)
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

	~TimePicker() override
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
	auto* picker = new DatePicker(editor, date, weekStart, reply);
	picker->Open(editor, mk::TargetSize() * 7,
		6 * (mk::TargetSize() - 2 * be_plain_font->Size()));
}

void
PickDueTime(BWindow* editor, int hour, int minute, uint32 reply)
{
	auto* picker = new TimePicker(editor, hour, minute, reply);
	picker->Open(editor, mk::TargetSize() * 5.5f, mk::TargetSize() * 2);
}
