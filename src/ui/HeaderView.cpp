#include "HeaderView.h"

#include "Drawing.h"
#include "Messages.h"
#include "Theme.h"

#include "HintField.h"

#include <TextView.h>
#include <Window.h>

#include <algorithm>
#include <cmath>

namespace {

const float kHeight = 58;
const char* kModeNames[] = {"Day", "Week", "Month", "Year", "Tasks"};
const char* kModes[] = {"day", "week", "month", "year", "tasks"};

}	// namespace


HeaderView::HeaderView()
	:
	BView("header", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_FRAME_EVENTS | B_PULSE_NEEDED)
{
	SetViewColor(B_TRANSPARENT_COLOR);
	fSearch = new HintField("search", "Search", new BMessage(kMsgSearchChanged),
		new BMessage(kMsgSearchChanged), HintField::kSearch);
	AddChild(fSearch);
	for (auto& part : fParts)
		part = BRect();
}


void
HeaderView::AttachedToWindow()
{
	BView::AttachedToWindow();
	fSearch->SetTarget(BMessenger(this));
	Layout();
}


BSize
HeaderView::MinSize()
{
	return BSize(600, kHeight);
}


BSize
HeaderView::PreferredSize()
{
	return BSize(900, kHeight);
}


BSize
HeaderView::MaxSize()
{
	return BSize(B_SIZE_UNLIMITED, kHeight);
}


void
HeaderView::SetTitle(const std::string& bold, const std::string& light)
{
	fBold = bold;
	fLight = light;
	Invalidate();
}


void
HeaderView::SetMode(const std::string& mode)
{
	fMode = mode;
	Invalidate();
}


void
HeaderView::SetSync(bool running, bool error, const std::string& tip)
{
	fSyncRunning = running;
	fSyncError = error;
	SetToolTip((const char*)nullptr);
	Invalidate(fParts[kSync]);
	if (Window() != nullptr)
		Window()->SetPulseRate(running ? 50000 : 500000);
	fTip = tip;
}


void
HeaderView::FocusSearch()
{
	fSearch->MakeFocus(true);
	fSearch->SelectAll();
}


void
HeaderView::ClearSearch()
{
	fSearch->SetText("");
}


void
HeaderView::FrameResized(float width, float height)
{
	Layout();
	Invalidate();
}


void
HeaderView::Layout()
{
	BRect bounds = Bounds();
	BFont font(be_plain_font);
	float pillHeight = 28;
	float top = floorf((bounds.Height() - pillHeight) / 2);
	float right = bounds.right - 14;

	// [+] at the far right, then search, then the view switcher.
	fParts[kAdd] = BRect(right - 30, top, right, top + pillHeight);
	right = fParts[kAdd].left - 10;
	float searchWidth = std::min(220.0f, std::max(120.0f, bounds.Width() * 0.18f));
	fSearch->ResizeTo(searchWidth, fSearch->PreferredSize().height);
	fSearch->MoveTo(right - searchWidth, floorf((bounds.Height()
		- fSearch->Bounds().Height()) / 2));
	right = right - searchWidth - 14;
	float segment = 0;
	for (const char* name : kModeNames)
		segment = std::max(segment, StringWidth(name) + 22);
	float switcherLeft = right - segment * 5;
	for (int i = 0; i < 5; i++) {
		fParts[kDay + i] = BRect(switcherLeft + i * segment, top,
			switcherLeft + (i + 1) * segment - 1, top + pillHeight);
	}
	right = switcherLeft - 14;
	fParts[kSync] = BRect(right - 26, top + 1, right, top + pillHeight - 1);

	// Left: sidebar toggle, title (drawn), then < Today >.
	fParts[kSidebar] = BRect(12, top, 40, top + pillHeight);
	BFont title(be_bold_font);
	title.SetSize(font.Size() * 2.0f);
	float titleWidth = title.StringWidth(fBold.c_str());
	BFont light(be_plain_font);
	light.SetSize(font.Size() * 2.0f);
	titleWidth += light.StringWidth((" " + fLight).c_str());
	float navLeft = std::max(52 + titleWidth + 18, 300.0f);
	if (navLeft + 140 > fParts[kSync].left)
		navLeft = fParts[kSync].left - 140;
	fParts[kPrevious] = BRect(navLeft, top, navLeft + 28, top + pillHeight);
	float todayWidth = StringWidth("Today") + 20;
	fParts[kToday] = BRect(fParts[kPrevious].right + 2, top,
		fParts[kPrevious].right + 2 + todayWidth, top + pillHeight);
	fParts[kNext] = BRect(fParts[kToday].right + 2, top, fParts[kToday].right + 30,
		top + pillHeight);
	if (fMode == "tasks") {
		// Tasks have no dates to page through.
		fParts[kPrevious] = fParts[kToday] = fParts[kNext] = BRect();
	}
}


void
HeaderView::Draw(BRect update)
{
	Layout();
	BRect bounds = Bounds();
	SetHighColor(Theme::Header());
	FillRect(bounds);
	SetHighColor(Theme::Separator());
	StrokeLine(BPoint(0, bounds.bottom), BPoint(bounds.right, bounds.bottom));

	BFont font(be_plain_font);
	SetDrawingMode(B_OP_ALPHA);

	// Sidebar toggle.
	if (fHover == kSidebar)
		Drawing::FillRound(this, fParts[kSidebar], 6, Theme::Control());
	Drawing::DrawSidebarIcon(this, fParts[kSidebar].InsetByCopy(4, 4),
		Theme::SecondaryText());

	// "November 2026"
	BFont title(be_bold_font);
	title.SetSize(font.Size() * 2.0f);
	SetFont(&title);
	float baseline = Drawing::Baseline(title, bounds) + 1;
	SetHighColor(Theme::Text());
	float x = 52;
	float limit = fMode == "tasks" ? fParts[kSync].left : fParts[kPrevious].left;
	float available = limit - 12 - x;
	std::string bold = Drawing::Fit(this, fBold, available);
	DrawString(bold.c_str(), BPoint(x, baseline));
	x += StringWidth(bold.c_str());
	BFont light(be_plain_font);
	light.SetSize(font.Size() * 2.0f);
	SetFont(&light);
	// The light part (the year) is shown whole or not at all.
	if (!fLight.empty() && StringWidth((" " + fLight).c_str()) <= limit - 12 - x) {
		SetHighColor(Theme::Accent());
		std::string text = " " + fLight;
		DrawString(text.c_str(), BPoint(x, baseline));
	}
	SetFont(&font);

	// < Today >
	if (fMode == "tasks")
		goto afterNavigation;
	{
	BRect nav(fParts[kPrevious].left, fParts[kPrevious].top, fParts[kNext].right,
		fParts[kNext].bottom);
	Drawing::FillRound(this, nav, 7, Theme::Control());
	for (int part : {kPrevious, kToday, kNext}) {
		if (fPressed == part || fHover == part) {
			Drawing::FillRound(this, fParts[part], 7, fPressed == part
				? Theme::Mix(Theme::Control(), Theme::Text(), 0.15f)
				: Theme::Mix(Theme::Control(), Theme::Text(), 0.06f));
		}
	}
	Drawing::DrawChevron(this, fParts[kPrevious], true, Theme::Text());
	Drawing::DrawChevron(this, fParts[kNext], false, Theme::Text());
	SetHighColor(Theme::Text());
	DrawString("Today", BPoint(fParts[kToday].left + 10,
		Drawing::Baseline(font, fParts[kToday])));
	}
afterNavigation:

	// Sync state.
	rgb_color syncColor = fSyncError ? Theme::Accent() : Theme::SecondaryText();
	if (fHover == kSync)
		Drawing::FillRound(this, fParts[kSync], 6, Theme::Control());
	Drawing::DrawSync(this, fParts[kSync].InsetByCopy(3, 3), syncColor,
		fSyncRunning ? fSyncAngle : 0);
	if (fSyncError && !fSyncRunning) {
		Drawing::FillCircle(this, BPoint(fParts[kSync].right - 4, fParts[kSync].top + 5),
			3.5f, Theme::Accent());
	}

	// Day | Week | Month | Year | Tasks
	BRect switcher(fParts[kDay].left, fParts[kDay].top, fParts[kTasks].right,
		fParts[kTasks].bottom);
	Drawing::FillRound(this, switcher, 7, Theme::Control());
	for (int i = 0; i < 5; i++) {
		BRect part = fParts[kDay + i];
		bool selected = fMode == kModes[i];
		if (selected) {
			BRect inner = part.InsetByCopy(2, 2);
			Drawing::FillRound(this, inner, 6, Theme::ControlSelected());
			if (!Theme::IsDark())
				Drawing::StrokeRound(this, inner, 6, Theme::WithAlpha(Theme::Separator(), 120));
		} else if (fHover == kDay + i)
			Drawing::FillRound(this, part.InsetByCopy(2, 2), 6,
				Theme::Mix(Theme::Control(), Theme::Text(), 0.05f));
		SetHighColor(selected ? Theme::Text() : Theme::SecondaryText());
		BFont& use = font;
		SetFont(selected ? be_bold_font : &use);
		if (selected) {
			BFont boldFont(be_bold_font);
			boldFont.SetSize(font.Size());
			SetFont(&boldFont);
		}
		float width = StringWidth(kModeNames[i]);
		DrawString(kModeNames[i], BPoint(part.left + (part.Width() - width) / 2,
			Drawing::Baseline(font, part)));
		SetFont(&font);
		if (i > 0 && !selected && fMode != kModes[i - 1]) {
			SetHighColor(Theme::WithAlpha(Theme::Separator(), 140));
			StrokeLine(BPoint(part.left, part.top + 8), BPoint(part.left, part.bottom - 8));
		}
	}

	// +
	Drawing::FillRound(this, fParts[kAdd], 7, fPressed == kAdd
		? Theme::Mix(Theme::Accent(), Theme::Text(), 0.2f) : Theme::Accent());
	Drawing::DrawPlus(this, fParts[kAdd], Theme::AccentText(), 2.0f);
	SetDrawingMode(B_OP_COPY);
}


int
HeaderView::PartAt(BPoint where) const
{
	for (int i = 0; i < kPartCount; i++) {
		if (fParts[i].IsValid() && fParts[i].Contains(where))
			return i;
	}
	return kNone;
}


void
HeaderView::MouseDown(BPoint where)
{
	fPressed = PartAt(where);
	if (fPressed != kNone) {
		SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
		Invalidate();
	}
}


void
HeaderView::MouseUp(BPoint where)
{
	int part = PartAt(where);
	int pressed = fPressed;
	fPressed = kNone;
	Invalidate();
	if (part != pressed || part == kNone)
		return;
	switch (part) {
		case kSidebar:
			Window()->PostMessage(kMsgToggleSidebar);
			break;
		case kPrevious:
			Window()->PostMessage(kMsgGoPrevious);
			break;
		case kToday:
			Window()->PostMessage(kMsgGoToday);
			break;
		case kNext:
			Window()->PostMessage(kMsgGoNext);
			break;
		case kSync:
			Window()->PostMessage(fSyncError ? kMsgShowAccounts : kMsgSyncNow);
			break;
		case kAdd:
			Window()->PostMessage(fMode == "tasks" ? kMsgNewTask : kMsgNewEvent);
			break;
		default:
			if (part >= kDay && part <= kTasks) {
				BMessage message(kMsgSetView);
				message.AddString("view", kModes[part - kDay]);
				Window()->PostMessage(&message);
			}
	}
}


void
HeaderView::MouseMoved(BPoint where, uint32 transit, const BMessage* drag)
{
	int hover = transit == B_EXITED_VIEW ? kNone : PartAt(where);
	if (hover != fHover) {
		fHover = hover;
		Invalidate();
		if (hover == kSync)
			SetToolTip(fTip.c_str());
		else if (hover == kAdd)
			SetToolTip(fMode == "tasks" ? "New Task" : "New Event");
		else
			SetToolTip((const char*)nullptr);
	}
}


void
HeaderView::MessageReceived(BMessage* message)
{
	if (message->what == kMsgSearchChanged) {
		BMessage forward(kMsgSearchChanged);
		forward.AddString("text", fSearch->Text());
		Window()->PostMessage(&forward);
		return;
	}
	BView::MessageReceived(message);
}


void
HeaderView::Pulse()
{
	if (!fSyncRunning)
		return;
	fSyncAngle -= 18;
	if (fSyncAngle < -360)
		fSyncAngle += 360;
	Invalidate(fParts[kSync]);
}
