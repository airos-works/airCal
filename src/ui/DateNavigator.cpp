#include "DateNavigator.h"

#include "Drawing.h"
#include "Format.h"
#include "Theme.h"

#include <MobileKit.h>

using namespace aircal;
namespace mk = BMobileKit;

namespace {
enum { kPrevious = 'dppr', kNext = 'dpnx' };
}

class DateTiles : public BView {
public:
	explicit DateTiles(DateNavigator* page)
		: BView("dates", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_NAVIGABLE),
		fPage(page)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
	}

	float HeaderHeight() const { return ceilf(be_plain_font->Size() * 2); }

	BRect Cell(int index) const
	{
		float w = (Bounds().Width() + 1) / 7;
		float h = (Bounds().Height() + 1 - HeaderHeight()) / (fPage->DayCount() / 7);
		int column = index % 7, row = index / 7;
		return BRect(floorf(column * w), HeaderHeight() + floorf(row * h),
			floorf((column + 1) * w) - 1, HeaderHeight() + floorf((row + 1) * h) - 1);
	}

	void Draw(BRect update) override
	{
		const mk::Skin& skin = mk::CurrentSkin();
		mk::DrawPage(this, Bounds(), update);
		Date first = fPage->FirstDate(), today = Today();
		BFont small(be_bold_font), number(be_bold_font);
		small.SetSize(ceilf(be_plain_font->Size() * 0.86f));
		number.SetSize(ceilf(be_plain_font->Size() * 1.45f));
		for (int i = 0; i < fPage->DayCount(); ++i) {
			Date date = first.AddDays(i);
			BRect cell = Cell(i);
			if (i < 7) {
				BRect heading(cell.left, 0, cell.right, HeaderHeight() - 1);
				SetFont(&small);
				mk::DrawCentered(this, Format::WeekdayShort(date.Weekday()).c_str(),
					heading, skin.pageSecondary, skin.controlEmboss);
			}
			if (!update.Intersects(cell))
				continue;
			bool selected = date == fPage->fDate;
			SetHighColor(Theme::Mix(skin.pageBottom, skin.pageSecondary, 0.25f));
			StrokeLine(cell.RightTop(), cell.RightBottom());
			StrokeLine(cell.LeftBottom(), cell.RightBottom());
			if (selected)
				mk::DrawPlate(this, cell.InsetByCopy(1, 1), 4,
					Theme::Mix(Theme::Accent(), make_color(255, 255, 255), 0.16f),
					Theme::Mix(Theme::Accent(), make_color(0, 0, 0), 0.22f));
			rgb_color ink = skin.pageText;
			if (selected)
				ink = Theme::AccentText();
			else if (date == today)
				ink = Theme::Accent();
			else if (fPage->fMonth && date.month != fPage->fDate.month)
				ink = skin.pageSecondary;
			SetFont(&number);
			BRect label = cell;
			label.bottom -= 4;
			mk::DrawCentered(this, std::to_string(date.day).c_str(), label, ink,
				selected ? skin.selectedEmboss : skin.controlEmboss);
			auto marker = fPage->fMarkers.find(date.DayNumber());
			if (marker != fPage->fMarkers.end()) {
				Drawing::FillCircle(this, BPoint(floorf((cell.left + cell.right) / 2),
					cell.bottom - 5), 2.5f, selected ? ink : Theme::FromHex(marker->second));
			}
			if (selected && IsFocus()) {
				SetHighColor(ui_color(B_KEYBOARD_NAVIGATION_COLOR));
				StrokeRoundRect(cell.InsetByCopy(3, 3), 3, 3);
			}
		}
	}

	int Hit(BPoint point) const
	{
		for (int i = 0; i < fPage->DayCount(); ++i) {
			if (Cell(i).Contains(point))
				return i;
		}
		return -1;
	}

	void MouseDown(BPoint point) override
	{
		fDown = Hit(point);
		if (fDown >= 0)
			SetMouseEventMask(B_POINTER_EVENTS);
	}

	void MouseUp(BPoint point) override
	{
		int index = Hit(point);
		if (index >= 0 && index == fDown)
			fPage->Choose(fPage->FirstDate().AddDays(index));
		fDown = -1;
	}

	void KeyDown(const char* bytes, int32 count) override
	{
		int days;
		switch (bytes[0]) {
			case B_LEFT_ARROW: days = -1; break;
			case B_RIGHT_ARROW: days = 1; break;
			case B_UP_ARROW: days = -7; break;
			case B_DOWN_ARROW: days = 7; break;
			case B_ENTER: case B_SPACE:
				fPage->Choose(fPage->fDate);
				return;
			default: BView::KeyDown(bytes, count); return;
		}
		Date date = fPage->fDate.AddDays(days);
		if (fPage->fBrowseSelects)
			fPage->Choose(date);
		else
			fPage->SetDate(date);
	}

private:
	DateNavigator* fPage;
	int fDown = -1;
};

DateNavigator::DateNavigator(const Date& date, bool month, int weekStart,
	BHandler* target, uint32 choose, bool browseSelects)
	: BGroupView("dateNavigator", B_VERTICAL, 0), fTarget(target), fChoose(choose),
	fBrowseSelects(browseSelects), fDate(date), fMonth(month), fWeekStart(weekStart)
{
	auto* bar = new mk::MetalBar("dateNavigation");
	auto* previous = new mk::MetalButton("previous", NULL, new BMessage(kPrevious),
		mk::B_ROUNDED_BUTTON, mk::B_GLYPH_ARROW_BACK);
	auto* next = new mk::MetalButton("next", NULL, new BMessage(kNext),
		mk::B_ROUNDED_BUTTON, mk::B_GLYPH_ARROW_FORWARD);
	previous->SetToolTip("Previous");
	next->SetToolTip("Next");
	fTitle = new mk::TitleView("period");
	BLayoutBuilder::Group<>(bar).Add(previous).Add(fTitle, 1).Add(next);
	fTiles = new DateTiles(this);
	BLayoutBuilder::Group<>(this).Add(bar).Add(fTiles);
	SetMonthLayout(month);
	SetDate(date);
}

void
DateNavigator::AttachedToWindow()
{
	BGroupView::AttachedToWindow();
	static_cast<BControl*>(FindView("previous"))->SetTarget(this);
	static_cast<BControl*>(FindView("next"))->SetTarget(this);
}

Date
DateNavigator::FirstDate() const
{
	Date first = fMonth ? Date(fDate.year, fDate.month, 1) : fDate;
	return first.AddDays(-((first.Weekday() - fWeekStart + 7) % 7));
}

void
DateNavigator::SetDate(const Date& date)
{
	fDate = date;
	fTitle->SetTitle((Format::MonthLong(date.month) + " " + std::to_string(date.year)).c_str());
	fTiles->Invalidate();
}

void
DateNavigator::SetMonthLayout(bool month)
{
	fMonth = month;
	float height = fTiles->HeaderHeight() + (month ? 6 : 1) * mk::TargetSize();
	float minimum = fBrowseSelects ? height
		: fTiles->HeaderHeight() + 6 * be_plain_font->Size() * 2;
	fTiles->SetExplicitMinSize(BSize(0, minimum));
	fTiles->SetExplicitPreferredSize(BSize(B_SIZE_UNSET, height));
	fTiles->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,
		month && fBrowseSelects ? B_SIZE_UNLIMITED : height));
	fTiles->Invalidate();
}

void
DateNavigator::SetWeekStart(int first)
{
	fWeekStart = first;
	fTiles->Invalidate();
}

void
DateNavigator::SetMarkers(std::map<int64_t, uint32> markers)
{
	fMarkers = std::move(markers);
	fTiles->Invalidate();
}

void
DateNavigator::Choose(const Date& date)
{
	BMessage message(fChoose);
	message.AddString("date", date.ToIso().c_str());
	fTarget.SendMessage(&message);
}

void
DateNavigator::MessageReceived(BMessage* message)
{
	if (message->what == kPrevious || message->what == kNext) {
		int step = message->what == kNext ? 1 : -1;
		Date date = fMonth ? fDate.AddMonths(step) : fDate.AddDays(step * 7);
		if (fBrowseSelects)
			Choose(date);
		else
			SetDate(date);
	} else
		BGroupView::MessageReceived(message);
}
