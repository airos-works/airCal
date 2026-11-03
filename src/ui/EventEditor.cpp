// The event editor: a compact popover-style window next to the clicked
// event, plus the dialog helpers the other editors share (DialogKit).
#include "EventEditor.h"

#include "App.h"
#include "Dialogs.h"
#include "Format.h"
#include "MainWindow.h"
#include "Messages.h"
#include "Theme.h"
#include "core/Editing.h"

#include <Alert.h>
#include <Beep.h>
#include <Button.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <Screen.h>
#include <ScrollView.h>
#include <SeparatorView.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>

using namespace aircal;


// #pragma mark - DialogKit


namespace DialogKit {

namespace {

std::mutex sRegistryLock;
std::map<std::string, std::pair<BMessenger, BWindow*>> sRegistry;

const int kAlarmChoices[] = {-1, 0, 5, 10, 15, 30, 60, 120, 1440, 2880, 10080};

struct RepeatChoice {
	RepeatPreset preset;
	const char* label;
};

const RepeatChoice kRepeatChoices[] = {
	{RepeatPreset::None, "Never"},
	{RepeatPreset::Daily, "Every Day"},
	{RepeatPreset::Weekdays, "Every Weekday"},
	{RepeatPreset::Weekly, "Every Week"},
	{RepeatPreset::Biweekly, "Every 2 Weeks"},
	{RepeatPreset::Monthly, "Every Month"},
	{RepeatPreset::Yearly, "Every Year"}
};


float
DotRadius(const BFont& font)
{
	font_height height;
	font.GetHeight(&height);
	return floorf((height.ascent + height.descent) * 0.3f);
}

}	// namespace


void
DrawColorDot(BView* view, BPoint center, float radius, uint32 color,
	bool enabled)
{
	rgb_color fill = Theme::FromHex(color);
	if (!enabled)
		fill = Theme::Mix(fill, view->LowColor(), 0.5f);
	rgb_color previous = view->HighColor();
	drawing_mode mode = view->DrawingMode();
	view->SetDrawingMode(B_OP_ALPHA);
	view->SetHighColor(fill);
	view->FillEllipse(center, radius, radius);
	view->SetHighColor(Theme::Separator());
	view->StrokeEllipse(center, radius, radius);
	view->SetDrawingMode(mode);
	view->SetHighColor(previous);
}


ColorDotItem::ColorDotItem(const char* label, uint32 color, BMessage* message)
	:
	BMenuItem(label, message),
	fColor(color)
{
}


float
ColorDotItem::DotSpace() const
{
	BFont font(be_plain_font);
	if (Menu() != NULL)
		Menu()->GetFont(&font);
	return DotRadius(font) * 2 + 7;
}


void
ColorDotItem::GetContentSize(float* _width, float* _height)
{
	BMenuItem::GetContentSize(_width, _height);
	if (_width != NULL)
		*_width += DotSpace();
}


void
ColorDotItem::DrawContent()
{
	BMenu* menu = Menu();
	BPoint where = ContentLocation();
	BFont font;
	menu->GetFont(&font);
	font_height height;
	font.GetHeight(&height);
	float radius = DotRadius(font);
	BPoint center(where.x + radius + 1,
		where.y + ceilf(height.ascent + height.descent) / 2);
	DrawColorDot(menu, center, radius, fColor, IsEnabled());
	menu->MovePenTo(where.x + DotSpace(), where.y);
	BMenuItem::DrawContent();
}


ColorDotView::ColorDotView(uint32 color)
	:
	BView("dot", B_WILL_DRAW),
	fColor(color)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
}


void
ColorDotView::Draw(BRect updateRect)
{
	BRect bounds = Bounds();
	BFont font;
	GetFont(&font);
	float radius = std::min(DotRadius(font),
		floorf(std::min(bounds.Width(), bounds.Height()) / 2) - 1);
	BPoint center((bounds.left + bounds.right) / 2,
		(bounds.top + bounds.bottom) / 2);
	DrawColorDot(this, center, radius, fColor);
}


void
ColorDotView::MessageReceived(BMessage* message)
{
	if (message->what == B_COLORS_UPDATED) {
		Theme::Refresh();
		Invalidate();
	}
	BView::MessageReceived(message);
}


BSize
ColorDotView::MinSize()
{
	BFont font;
	GetFont(&font);
	float size = DotRadius(font) * 2 + 4;
	return BSize(size, size);
}


BSize
ColorDotView::MaxSize()
{
	return MinSize();
}


BSize
ColorDotView::PreferredSize()
{
	return MinSize();
}


BWindow*
MainBWindow()
{
	App* app = TheApp();
	return app != NULL ? static_cast<BWindow*>(app->Window()) : NULL;
}


void
PostToMain(BMessage* message)
{
	BWindow* window = MainBWindow();
	if (window != NULL)
		BMessenger(window).SendMessage(message);
}


void
PlaceWindow(BWindow* window, float anchorX, float anchorY)
{
	// Lay out first: the preferred size is only right once the views know
	// their fonts and contents.
	if (window->Lock()) {
		window->Layout(true);
		window->ResizeToPreferred();
		window->Unlock();
	}
	if (anchorX < 0) {
		BWindow* main = MainBWindow();
		if (main != NULL)
			window->CenterIn(main->Frame());
		else
			window->CenterOnScreen();
		window->MoveOnScreen(B_DO_NOT_RESIZE_TO_FIT);
		return;
	}

	BRect screen = BScreen(B_MAIN_SCREEN_ID).Frame();
	BRect frame = window->Frame();
	const float gap = 14;
	const float margin = 8;
	float left = anchorX + gap;
	if (left + frame.Width() > screen.right - margin)
		left = anchorX - gap - frame.Width();
	float top = anchorY - 48;
	left = std::max(screen.left + margin,
		std::min(left, screen.right - margin - frame.Width()));
	// The window tab and border sit outside the frame.
	BRect decorated = frame;
	if (window->Lock()) {
		BRect decor = window->DecoratorFrame();
		if (decor.IsValid())
			decorated = decor;
		window->Unlock();
	}
	float above = frame.top - decorated.top;
	float below = decorated.bottom - frame.bottom;
	top = std::max(screen.top + margin + above,
		std::min(top, screen.bottom - margin - below - frame.Height()));
	window->MoveTo(left, top);
	window->MoveOnScreen(B_DO_NOT_RESIZE_TO_FIT);
}


bool
ActivateRegistered(const std::string& key)
{
	if (key.empty())
		return false;
	BMessenger messenger;
	{
		std::lock_guard<std::mutex> lock(sRegistryLock);
		auto found = sRegistry.find(key);
		if (found == sRegistry.end())
			return false;
		messenger = found->second.first;
	}
	if (messenger.LockTarget() != B_OK)
		return false;
	BLooper* looper = NULL;
	messenger.Target(&looper);
	BWindow* window = dynamic_cast<BWindow*>(looper);
	if (window != NULL)
		window->Activate();
	if (looper != NULL)
		looper->Unlock();
	return window != NULL;
}


void
Register(const std::string& key, BWindow* window)
{
	if (key.empty())
		return;
	std::lock_guard<std::mutex> lock(sRegistryLock);
	sRegistry[key] = std::make_pair(BMessenger(window), window);
}


void
Unregister(const std::string& key, BWindow* window)
{
	if (key.empty())
		return;
	std::lock_guard<std::mutex> lock(sRegistryLock);
	auto found = sRegistry.find(key);
	if (found != sRegistry.end() && found->second.second == window)
		sRegistry.erase(found);
}


bool
PostToRegistered(const std::string& key, BMessage* message)
{
	BMessenger messenger;
	{
		std::lock_guard<std::mutex> lock(sRegistryLock);
		auto found = sRegistry.find(key);
		if (found == sRegistry.end())
			return false;
		messenger = found->second.first;
	}
	return messenger.SendMessage(message) == B_OK;
}


int
AddCalendarItems(BMenu* menu, bool tasks, const std::string& selected,
	uint32 what)
{
	Store& store = TheApp()->Store();
	std::lock_guard<std::recursive_mutex> lock(store.Mutex());
	int selectable = 0;
	bool grouped = store.Accounts().size() > 1;
	for (auto& account : store.Accounts()) {
		bool header = false;
		for (Calendar* calendar : store.CalendarsOf(account->id)) {
			if (!(tasks ? calendar->tasks : calendar->events))
				continue;
			bool isSelected = calendar->id == selected;
			bool writable = !calendar->readOnly && account->enabled;
			if (!writable && !isSelected)
				continue;
			if (grouped && !header) {
				if (menu->CountItems() > 0)
					menu->AddSeparatorItem();
				BMenuItem* title = new BMenuItem(account->name.c_str(), NULL);
				title->SetEnabled(false);
				menu->AddItem(title);
				header = true;
			}
			BMessage* message = new BMessage(what);
			message->AddString("calendar", calendar->id.c_str());
			ColorDotItem* item = new ColorDotItem(calendar->name.c_str(),
				calendar->color, message);
			item->SetEnabled(writable);
			menu->AddItem(item);
			if (isSelected)
				item->SetMarked(true);
			if (writable)
				selectable++;
		}
	}
	return selectable;
}


void
AddAlarmItems(BMenu* menu, int current, uint32 what)
{
	std::vector<int> choices(std::begin(kAlarmChoices), std::end(kAlarmChoices));
	if (std::find(choices.begin(), choices.end(), current) == choices.end()) {
		choices.push_back(current);
		std::sort(choices.begin(), choices.end());
	}
	for (int minutes : choices) {
		BMessage* message = new BMessage(what);
		message->AddInt32("minutes", minutes);
		BMenuItem* item = new BMenuItem(Format::AlarmText(minutes).c_str(),
			message);
		menu->AddItem(item);
		if (minutes == current)
			item->SetMarked(true);
		if (minutes == -1)
			menu->AddSeparatorItem();
	}
}


void
AddRepeatItems(BMenu* menu, const std::string& rrule, uint32 what)
{
	RepeatPreset current = PresetFromRule(rrule);
	for (const RepeatChoice& choice : kRepeatChoices) {
		BMessage* message = new BMessage(what);
		message->AddInt32("preset", (int32)choice.preset);
		BMenuItem* item = new BMenuItem(choice.label, message);
		menu->AddItem(item);
		if (choice.preset == current)
			item->SetMarked(true);
		if (choice.preset == RepeatPreset::None)
			menu->AddSeparatorItem();
	}
	if (current == RepeatPreset::Custom) {
		menu->AddSeparatorItem();
		BMessage* message = new BMessage(what);
		message->AddInt32("preset", (int32)RepeatPreset::Custom);
		std::string label = "Custom: " + DescribeRule(rrule);
		BMenuItem* item = new BMenuItem(label.c_str(), message);
		menu->AddItem(item);
		item->SetMarked(true);
	}
}


std::string
DateFieldText(const Date& date)
{
	// Always with the year: ParseDate reads a year-less date that lies more
	// than two months back as next year's.
	return Format::WeekdayShort(date.Weekday()) + ", " + Format::DateShort(date)
		+ ", " + std::to_string(date.year);
}


std::string
TimeFieldText(int hour, int minute)
{
	char buffer[32];
	if (Format::Use24Hour())
		snprintf(buffer, sizeof(buffer), "%d:%02d", hour, minute);
	else {
		int twelve = hour % 12 == 0 ? 12 : hour % 12;
		snprintf(buffer, sizeof(buffer), "%d:%02d %s", twelve, minute,
			hour < 12 ? "AM" : "PM");
	}
	return buffer;
}


void
SetFieldValid(BTextControl* control, bool valid)
{
	control->MarkAsInvalid(!valid);
	BTextView* textView = control->TextView();
	rgb_color color = ui_color(B_DOCUMENT_TEXT_COLOR);
	if (!valid) {
		rgb_color red = {230, 45, 40, 255};
		color = Theme::Mix(color, red, 0.8f);
	} else if (!control->IsEnabled())
		color = Theme::Mix(color, ui_color(B_DOCUMENT_BACKGROUND_COLOR), 0.5f);
	BFont font;
	textView->GetFontAndColor(0, &font);
	textView->SetFontAndColor(0, textView->TextLength(), &font, B_FONT_ALL,
		&color);
	textView->SetFontAndColor(&font, B_FONT_ALL, &color);
}


bool
IsVideoCallUrl(const std::string& text)
{
	std::string url;
	for (char c : text)
		url += (char)tolower((unsigned char)c);
	if (url.compare(0, 8, "https://") != 0 && url.compare(0, 7, "http://") != 0)
		return false;
	static const char* kHosts[] = {"meet.google.com", "zoom.us/", "zoom.com/",
		"zoomgov.com/", "teams.microsoft.com", "teams.live.com", "webex.com",
		"whereby.com", "meet.jit.si", "gotomeeting.com", "goto.com/meeting",
		"chime.aws", "facetime.apple.com", "bluejeans.com", "skype.com",
		"discord.gg", "around.co", "meet.lync.com", "join.skype.com"};
	for (const char* host : kHosts) {
		if (url.find(host) != std::string::npos)
			return true;
	}
	return false;
}


AccountType
AccountTypeOfCalendar(const std::string& calendarId)
{
	Store& store = TheApp()->Store();
	std::lock_guard<std::recursive_mutex> lock(store.Mutex());
	Calendar* calendar = store.FindCalendar(calendarId);
	Account* account = calendar != NULL ? store.AccountOf(*calendar) : NULL;
	return account != NULL ? account->type : AccountType::Local;
}


std::string
Trim(const std::string& text)
{
	size_t begin = 0;
	size_t end = text.size();
	while (begin < end && isspace((unsigned char)text[begin]))
		begin++;
	while (end > begin && isspace((unsigned char)text[end - 1]))
		end--;
	return text.substr(begin, end - begin);
}

}	// namespace DialogKit


// #pragma mark - EventEditorWindow


namespace {

using namespace DialogKit;

enum {
	kMsgAllDay = 'eald',
	kMsgStartCommitted = 'estc',
	kMsgFieldCommitted = 'efcm',
	kMsgFieldModified = 'efmd',
	kMsgRepeat = 'erep',
	kMsgAlarm = 'ealm',
	kMsgCalendar = 'ecal',
	kMsgUrlModified = 'eurl',
	kMsgJoin = 'ejoi',
	kMsgZone = 'ezon',
	kMsgSave = 'esav',
	kMsgDelete = 'edel',
	kMsgCancel = 'ecan'
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


float
TextWidth(const char* sample)
{
	return ceilf(be_plain_font->StringWidth(sample)) + 24;
}


class EventEditorWindow : public BWindow {
public:
	EventEditorWindow(const OccurrenceRef& ref, const Event& event, bool isNew,
		const std::string& key);
	~EventEditorWindow() override;

	void MessageReceived(BMessage* message) override;
	void DispatchMessage(BMessage* message, BHandler* target) override;
	void FrameResized(float width, float height) override;

private:
	void BuildLayout();
	void FillTimes();
	void RememberStart();
	bool Validate(bool mark);
	void StartCommitted();
	void NormalizeField(BTextControl* control, bool isDate);
	void AllDayToggled();
	void UpdateTimeVisibility();
	void BuildZoneMenu();
	void ZoneChosen(const std::string& zone);
	void UpdateJoin();
	std::string JoinUrl() const;
	void CalendarChosen(const std::string& calendarId);
	void RebuildAlarmMenu();
	void ApplyReadOnly();
	void Save();
	void Delete();
	void Finish(const std::string& id, bool deleted);

	OccurrenceRef fRef;
	Event fEvent;
	bool fNew;
	bool fReadOnly = false;
	bool fRecurring = false;
	std::string fKey;
	std::string fRule;				// the series' rule as shown
	RepeatPreset fInitialPreset = RepeatPreset::None;
	RepeatPreset fPreset = RepeatPreset::None;
	int fInitialAlarm = -1;
	int fAlarm = -1;
	std::string fCalendarId;
	std::string fZone;			// the zone times are shown and typed in ("" local)
	int fDefaultDuration = 60;
	Instant fLastStart = 0;
	Date fLastStartDate;

	BTextControl* fTitle = NULL;
	BTextControl* fLocation = NULL;
	BCheckBox* fAllDay = NULL;
	BTextControl* fStartDate = NULL;
	BTextControl* fStartTime = NULL;
	BTextControl* fEndDate = NULL;
	BTextControl* fEndTime = NULL;
	BPopUpMenu* fRepeatMenu = NULL;
	BMenuField* fRepeatField = NULL;
	BPopUpMenu* fAlarmMenu = NULL;
	BMenuField* fAlarmField = NULL;
	BPopUpMenu* fCalendarMenu = NULL;
	BMenuField* fCalendarField = NULL;
	BStringView* fZoneLabel = NULL;
	BPopUpMenu* fZoneMenu = NULL;
	BMenuField* fZoneField = NULL;
	BTextControl* fInvitees = NULL;
	BTextControl* fUrl = NULL;
	BButton* fJoin = NULL;
	BTextView* fNotes = NULL;
	BScrollView* fNotesScroll = NULL;
	BStringView* fReadOnlyNote = NULL;
	BButton* fDelete = NULL;
	BButton* fCancel = NULL;
	BButton* fSave = NULL;
};


EventEditorWindow::EventEditorWindow(const OccurrenceRef& ref,
	const Event& event, bool isNew, const std::string& key)
	:
	BWindow(BRect(0, 0, 360, 400), isNew ? "New Event" : "Event",
		B_FLOATING_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL,
		B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE
			| B_ASYNCHRONOUS_CONTROLS),
	fRef(ref),
	fEvent(event),
	fNew(isNew),
	fKey(key),
	fCalendarId(event.calendarId)
{
	Store& store = TheApp()->Store();
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		fDefaultDuration = std::max(5, store.Prefs().defaultDuration);
		Calendar* calendar = store.FindCalendar(fEvent.calendarId);
		fReadOnly = calendar != NULL && calendar->readOnly;
		fRule = fEvent.rrule;
		if (!fNew) {
			Event* master = store.FindEvent(fRef.eventId);
			if (master != NULL && master->IsRecurring()) {
				fRecurring = true;
				fRule = master->rrule;
			}
		}
	}
	// Times show in the event's own zone when it has one other than ours.
	if (!fEvent.allDay && !fEvent.tzid.empty() && fEvent.tzid != "UTC"
		&& CanonicalZone(fEvent.tzid) != CanonicalZone(LocalZone()))
		fZone = CanonicalZone(fEvent.tzid);
	fInitialPreset = fPreset = PresetFromRule(fRule);
	fInitialAlarm = fAlarm = fEvent.alarms.empty() ? -1 : fEvent.alarms[0];

	BuildLayout();
	FillTimes();
	RememberStart();
	UpdateTimeVisibility();
	UpdateJoin();
	if (fReadOnly)
		ApplyReadOnly();
	else {
		fTitle->MakeFocus(true);
		fTitle->TextView()->SelectAll();
	}
}


EventEditorWindow::~EventEditorWindow()
{
	Unregister(fKey, this);
}


void
EventEditorWindow::BuildLayout()
{
	BFont titleFont(be_plain_font);
	titleFont.SetSize(ceilf(titleFont.Size() * 1.45f));
	titleFont.SetFace(B_BOLD_FACE);
	fTitle = new BTextControl("title", NULL, fEvent.title.c_str(),
		NULL);
	fTitle->SetFont(&titleFont);
	fTitle->TextView()->SetFontAndColor(&titleFont);
	fTitle->SetExplicitMinSize(BSize(320, B_SIZE_UNSET));

	fLocation = new BTextControl("location", NULL, fEvent.location.c_str(),
		NULL);
	fLocation->SetToolTip("Location");

	fAllDay = new BCheckBox("allDay", "All-day", new BMessage(kMsgAllDay));
	fAllDay->SetValue(fEvent.allDay ? B_CONTROL_ON : B_CONTROL_OFF);

	float dateWidth = TextWidth("Wed, Sep 30, 2026");
	float timeWidth = TextWidth("12:59 PM");
	auto makeField = [&](const char* name, bool isDate, bool isStart) {
		BTextControl* field = new BTextControl(name, NULL, "",
			new BMessage(isStart ? kMsgStartCommitted : kMsgFieldCommitted));
		field->SetModificationMessage(new BMessage(kMsgFieldModified));
		float width = isDate ? dateWidth : timeWidth;
		field->SetExplicitMinSize(BSize(width, B_SIZE_UNSET));
		field->SetExplicitMaxSize(BSize(width * 1.3f, B_SIZE_UNSET));
		field->SetToolTip(isDate ? "A date such as \"Nov 3\", \"11/3/2026\", "
			"\"tomorrow\" or \"fri\"" : "A time such as \"9:30\", \"3pm\" or "
			"\"15:30\"");
		return field;
	};
	fStartDate = makeField("startDate", true, true);
	fStartTime = makeField("startTime", false, true);
	fEndDate = makeField("endDate", true, false);
	fEndTime = makeField("endTime", false, false);

	fRepeatMenu = new BPopUpMenu("repeat");
	AddRepeatItems(fRepeatMenu, fRule, kMsgRepeat);
	fRepeatField = new BMenuField("repeat", NULL, fRepeatMenu);

	fAlarmMenu = new BPopUpMenu("alert");
	AddAlarmItems(fAlarmMenu, fAlarm, kMsgAlarm);
	fAlarmField = new BMenuField("alert", NULL, fAlarmMenu);

	fCalendarMenu = new BPopUpMenu("calendar");
	AddCalendarItems(fCalendarMenu, false, fCalendarId, kMsgCalendar);
	fCalendarField = new BMenuField("calendar", NULL, fCalendarMenu);

	fZoneLabel = FieldLabel("Time zone:");
	fZoneMenu = new BPopUpMenu("zone");
	fZoneField = new BMenuField("zone", NULL, fZoneMenu);
	BuildZoneMenu();

	std::string invitees;
	for (auto& attendee : fEvent.attendees)
		invitees += (invitees.empty() ? "" : ", ") + attendee;
	fInvitees = new BTextControl("invitees", NULL, invitees.c_str(),
		NULL);
	fInvitees->SetToolTip("E-mail addresses, separated by commas. Google and "
		"CalDAV servers that schedule (iCloud, Nextcloud, Fastmail) send the "
		"invitations.");

	fUrl = new BTextControl("url", NULL, fEvent.url.c_str(), NULL);
	fUrl->SetModificationMessage(new BMessage(kMsgUrlModified));
	fJoin = new BButton("join", "Join", new BMessage(kMsgJoin));
	fJoin->SetToolTip("Open the video call");

	fNotes = new BTextView("notes");
	fNotes->SetWordWrap(true);
	fNotes->SetStylable(false);
	fNotes->SetText(fEvent.notes.c_str());
	fNotesScroll = new BScrollView("notesScroll", fNotes, 0, false, true);
	font_height height;
	be_plain_font->GetHeight(&height);
	float lines = ceilf((height.ascent + height.descent + height.leading) * 4)
		+ 8;
	fNotesScroll->SetExplicitMinSize(BSize(B_SIZE_UNSET, lines));
	fNotesScroll->SetExplicitPreferredSize(BSize(B_SIZE_UNSET, lines));

	fDelete = new BButton("delete", "Delete", new BMessage(kMsgDelete));
	fCancel = new BButton("cancel", "Cancel", new BMessage(kMsgCancel));
	fSave = new BButton("save", "Save", new BMessage(kMsgSave));

	BGridView* grid = new BGridView(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING);
	BLayoutBuilder::Grid<> builder(grid);
	builder
		.Add(fAllDay, 1, 0)
		.Add(FieldLabel("Starts:"), 0, 1)
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING, 1, 1)
			.Add(fStartDate)
			.Add(fStartTime)
			.AddGlue()
		.End()
		.Add(FieldLabel("Ends:"), 0, 2)
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING, 1, 2)
			.Add(fEndDate)
			.Add(fEndTime)
			.AddGlue()
		.End()
		.Add(fZoneLabel, 0, 3)
		.Add(fZoneField, 1, 3)
		.Add(FieldLabel("Repeat:"), 0, 4)
		.Add(fRepeatField, 1, 4)
		.Add(FieldLabel("Alert:"), 0, 5)
		.Add(fAlarmField, 1, 5)
		.Add(FieldLabel("Calendar:"), 0, 6)
		.Add(fCalendarField, 1, 6)
		.Add(FieldLabel("Invitees:"), 0, 7)
		.Add(fInvitees, 1, 7)
		.Add(FieldLabel("URL:"), 0, 8)
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING, 1, 8)
			.Add(fUrl)
			.Add(fJoin)
		.End()
		.Add(FieldLabel("Notes:", true), 0, 9)
		.Add(fNotesScroll, 1, 9);
	int row = 10;

	// Who else is there (read-only; invitations are answered on the web).
	if (!fEvent.organizer.empty()) {
		BStringView* organizer = new BStringView("organizer",
			fEvent.organizer.c_str());
		organizer->SetTruncation(B_TRUNCATE_END);
		builder.Add(FieldLabel("Organizer:"), 0, row).Add(organizer, 1, row);
		row++;
	}
	grid->GridLayout()->SetColumnWeight(0, 0);
	grid->GridLayout()->SetColumnWeight(1, 1);

	fReadOnlyNote = new BStringView("readOnly", "This calendar is read-only");
	fReadOnlyNote->SetAlignment(B_ALIGN_CENTER);
	fReadOnlyNote->SetExplicitAlignment(BAlignment(B_ALIGN_CENTER,
		B_ALIGN_MIDDLE));

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(fTitle)
		.Add(fLocation)
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
EventEditorWindow::FillTimes()
{
	int startHour, startMinute, endHour, endMinute;
	Date startDate, endDate;
	if (fEvent.allDay) {
		startDate = fEvent.startDate;
		endDate = fEvent.endDate.AddDays(-1);
		if (endDate < startDate)
			endDate = startDate;
		// Times to come back to when "All-day" is switched off.
		startHour = 9;
		startMinute = 0;
		int end = startHour * 60 + std::min(fDefaultDuration, 14 * 60);
		endHour = end / 60;
		endMinute = end % 60;
	} else {
		DateTime start = ToWall(fEvent.start, fZone);
		DateTime end = ToWall(fEvent.end, fZone);
		startDate = start.date;
		startHour = start.hour;
		startMinute = start.minute;
		endDate = end.date;
		endHour = end.hour;
		endMinute = end.minute;
	}
	fStartDate->SetText(DateFieldText(startDate).c_str());
	fStartTime->SetText(TimeFieldText(startHour, startMinute).c_str());
	fEndDate->SetText(DateFieldText(endDate).c_str());
	fEndTime->SetText(TimeFieldText(endHour, endMinute).c_str());
}


void
EventEditorWindow::RememberStart()
{
	Date date;
	int hour, minute;
	if (Format::ParseDate(fStartDate->Text(), date)) {
		fLastStartDate = date;
		if (Format::ParseTime(fStartTime->Text(), hour, minute))
			fLastStart = ToInstant(DateTime(date, hour, minute), fZone);
	}
}


bool
EventEditorWindow::Validate(bool mark)
{
	bool allDay = fAllDay->Value() == B_CONTROL_ON;
	Date startDate, endDate;
	int startHour = 0, startMinute = 0, endHour = 0, endMinute = 0;
	bool startDateOk = Format::ParseDate(fStartDate->Text(), startDate);
	bool endDateOk = Format::ParseDate(fEndDate->Text(), endDate);
	bool startTimeOk = allDay
		|| Format::ParseTime(fStartTime->Text(), startHour, startMinute);
	bool endTimeOk = allDay
		|| Format::ParseTime(fEndTime->Text(), endHour, endMinute);
	bool ordered = true;
	if (startDateOk && endDateOk && startTimeOk && endTimeOk) {
		if (allDay)
			ordered = endDate >= startDate;
		else {
			ordered = ToInstant(DateTime(endDate, endHour, endMinute), fZone)
				>= ToInstant(DateTime(startDate, startHour, startMinute), fZone);
		}
	}
	if (mark) {
		SetFieldValid(fStartDate, startDateOk);
		SetFieldValid(fStartTime, startTimeOk);
		SetFieldValid(fEndDate, endDateOk && ordered);
		SetFieldValid(fEndTime, endTimeOk && ordered);
	}
	return startDateOk && endDateOk && startTimeOk && endTimeOk && ordered;
}


void
EventEditorWindow::NormalizeField(BTextControl* control, bool isDate)
{
	std::string text;
	if (isDate) {
		Date date;
		if (!Format::ParseDate(control->Text(), date))
			return;
		text = DateFieldText(date);
	} else {
		int hour, minute;
		if (!Format::ParseTime(control->Text(), hour, minute))
			return;
		text = TimeFieldText(hour, minute);
	}
	if (text != control->Text())
		control->SetText(text.c_str());
}


void
EventEditorWindow::StartCommitted()
{
	NormalizeField(fStartDate, true);
	NormalizeField(fStartTime, false);
	// Keep the length: the end moves with the start.
	Date startDate, endDate;
	if (!Format::ParseDate(fStartDate->Text(), startDate))
		return;
	if (fAllDay->Value() == B_CONTROL_ON) {
		int64_t delta = startDate.DayNumber() - fLastStartDate.DayNumber();
		if (delta != 0 && Format::ParseDate(fEndDate->Text(), endDate))
			fEndDate->SetText(DateFieldText(endDate.AddDays(delta)).c_str());
	} else {
		int hour, minute, endHour, endMinute;
		if (!Format::ParseTime(fStartTime->Text(), hour, minute))
			return;
		Instant start = ToInstant(DateTime(startDate, hour, minute), fZone);
		Instant delta = start - fLastStart;
		if (delta != 0 && Format::ParseDate(fEndDate->Text(), endDate)
			&& Format::ParseTime(fEndTime->Text(), endHour, endMinute)) {
			Instant end = ToInstant(DateTime(endDate, endHour, endMinute), fZone) + delta;
			DateTime wall = ToWall(end, fZone);
			fEndDate->SetText(DateFieldText(wall.date).c_str());
			fEndTime->SetText(TimeFieldText(wall.hour, wall.minute).c_str());
		}
	}
	RememberStart();
	Validate(true);
}


void
EventEditorWindow::AllDayToggled()
{
	bool allDay = fAllDay->Value() == B_CONTROL_ON;
	Date startDate, endDate;
	int hour, minute;
	if (allDay) {
		// A timed event ending at midnight ends on the day before.
		if (Format::ParseDate(fStartDate->Text(), startDate)
			&& Format::ParseDate(fEndDate->Text(), endDate)
			&& Format::ParseTime(fEndTime->Text(), hour, minute)
			&& hour == 0 && minute == 0 && endDate > startDate)
			fEndDate->SetText(DateFieldText(endDate.AddDays(-1)).c_str());
	} else if (Format::ParseDate(fStartDate->Text(), startDate)
		&& Format::ParseTime(fStartTime->Text(), hour, minute)
		&& !Validate(false)) {
		// Make the times consistent again.
		Instant end = ToInstant(DateTime(startDate, hour, minute), fZone)
			+ fDefaultDuration * 60;
		DateTime wall = ToWall(end, fZone);
		fEndDate->SetText(DateFieldText(wall.date).c_str());
		fEndTime->SetText(TimeFieldText(wall.hour, wall.minute).c_str());
	}
	UpdateTimeVisibility();
	RememberStart();
	Validate(true);
}


void
EventEditorWindow::DispatchMessage(BMessage* message, BHandler* target)
{
	// Return in a one-line field saves, as in Fantastical (the notes keep
	// their new lines; the date and time fields first commit themselves).
	if (message->what == B_KEY_DOWN && !fReadOnly) {
		const char* bytes = NULL;
		int32 modifiers = message->GetInt32("modifiers", 0);
		if (message->FindString("bytes", &bytes) == B_OK && bytes[0] == B_ENTER
			&& (modifiers & (B_SHIFT_KEY | B_OPTION_KEY)) == 0) {
			BView* focus = CurrentFocus();
			BTextControl* fields[] = {fTitle, fLocation, fInvitees, fUrl,
				fStartDate, fStartTime, fEndDate, fEndTime};
			for (BTextControl* field : fields) {
				if (field != NULL && focus == field->TextView()) {
					if (field == fStartDate || field == fStartTime)
						StartCommitted();
					PostMessage(kMsgSave);
					return;
				}
			}
		}
	}
	BWindow::DispatchMessage(message, target);
}


void
EventEditorWindow::FrameResized(float width, float height)
{
	BWindow::FrameResized(width, height);
	// Rows that come and go (time zone, all-day) must not push it off screen.
	MoveOnScreen(B_DO_NOT_RESIZE_TO_FIT);
}


void
EventEditorWindow::BuildZoneMenu()
{
	auto add = [&](BMenu* menu, const std::string& zone, const std::string& label) {
		BMessage* message = new BMessage(kMsgZone);
		message->AddString("zone", zone.c_str());
		BMenuItem* item = new BMenuItem(label.c_str(), message);
		item->SetMarked(zone == fZone);
		menu->AddItem(item);
	};
	add(fZoneMenu, "", "Local (" + LocalZone() + ")");
	if (!fZone.empty())
		add(fZoneMenu, fZone, fZone);
	fZoneMenu->AddSeparatorItem();
	// Every zone, by region.
	std::map<std::string, BMenu*> regions;
	for (const std::string& zone : AllZones()) {
		std::string region = zone.substr(0, zone.find('/'));
		BMenu*& menu = regions[region];
		if (menu == NULL)
			menu = new BMenu(region.c_str());
		std::string city = zone.substr(zone.find('/') + 1);
		for (char& c : city) {
			if (c == '_')
				c = ' ';
		}
		BMessage* message = new BMessage(kMsgZone);
		message->AddString("zone", zone.c_str());
		menu->AddItem(new BMenuItem(city.c_str(), message));
	}
	for (auto& region : regions)
		fZoneMenu->AddItem(region.second);
	fZoneMenu->SetLabelFromMarked(false);
	fZoneField->MenuItem()->SetLabel(fZone.empty()
		? ("Local (" + LocalZone() + ")").c_str() : fZone.c_str());
}


void
EventEditorWindow::ZoneChosen(const std::string& requested)
{
	// The typed wall-clock times stay; they now mean that zone's time.
	std::string zone = requested.empty() ? std::string() : CanonicalZone(requested);
	if (zone == CanonicalZone(LocalZone()))
		zone.clear();
	fZone = zone;
	fZoneField->MenuItem()->SetLabel(fZone.empty()
		? ("Local (" + LocalZone() + ")").c_str() : fZone.c_str());
	for (int32 i = 0; i < fZoneMenu->CountItems(); i++) {
		BMenuItem* item = fZoneMenu->ItemAt(i);
		const char* value = NULL;
		if (item->Submenu() == NULL && item->Message() != NULL
			&& item->Message()->FindString("zone", &value) == B_OK)
			item->SetMarked(fZone == value);
	}
	RememberStart();
	Validate(true);
}


void
EventEditorWindow::UpdateTimeVisibility()
{
	bool timed = fAllDay->Value() != B_CONTROL_ON;
	SetVisible(fStartTime, timed);
	SetVisible(fEndTime, timed);
	SetVisible(fZoneField, timed);
	SetVisible(fZoneLabel, timed);
}


std::string
EventEditorWindow::JoinUrl() const
{
	std::string url = Trim(fUrl->Text());
	if (IsVideoCallUrl(url))
		return url;
	// Invitations often carry the link in the location instead.
	std::string location = fLocation->Text();
	size_t begin = location.find("https://");
	if (begin != std::string::npos) {
		size_t end = location.find_first_of(" \t\n,;<>\"", begin);
		std::string candidate = location.substr(begin,
			end == std::string::npos ? std::string::npos : end - begin);
		if (IsVideoCallUrl(candidate))
			return candidate;
	}
	return std::string();
}


void
EventEditorWindow::UpdateJoin()
{
	SetVisible(fJoin, !JoinUrl().empty());
}


void
EventEditorWindow::CalendarChosen(const std::string& calendarId)
{
	fCalendarId = calendarId;
	if (!fNew || fAlarm != fInitialAlarm)
		return;
	// A new event takes the calendar's default alerts until the user picks one.
	Store& store = TheApp()->Store();
	std::vector<int> alarms;
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		Calendar* calendar = store.FindCalendar(calendarId);
		if (calendar != NULL && !calendar->defaultAlarms.empty())
			alarms = calendar->defaultAlarms;
		else if (store.Prefs().defaultAlarm >= 0)
			alarms.push_back(store.Prefs().defaultAlarm);
	}
	fEvent.alarms = alarms;
	fInitialAlarm = fAlarm = alarms.empty() ? -1 : alarms[0];
	RebuildAlarmMenu();
}


void
EventEditorWindow::RebuildAlarmMenu()
{
	fAlarmMenu->RemoveItems(0, fAlarmMenu->CountItems(), true);
	AddAlarmItems(fAlarmMenu, fAlarm, kMsgAlarm);
}


void
EventEditorWindow::ApplyReadOnly()
{
	SetTitle("Event (read-only)");
	fTitle->SetEnabled(false);
	fLocation->SetEnabled(false);
	fAllDay->SetEnabled(false);
	fZoneField->SetEnabled(false);
	fInvitees->SetEnabled(false);
	fStartDate->SetEnabled(false);
	fStartTime->SetEnabled(false);
	fEndDate->SetEnabled(false);
	fEndTime->SetEnabled(false);
	fRepeatField->SetEnabled(false);
	fAlarmField->SetEnabled(false);
	fCalendarField->SetEnabled(false);
	fUrl->SetEnabled(false);
	fNotes->MakeEditable(false);
	fSave->SetEnabled(false);
	SetDefaultButton(fCancel);
	fCancel->MakeFocus(true);
}


void
EventEditorWindow::Save()
{
	if (fReadOnly)
		return;
	if (!Validate(true)) {
		beep();
		return;
	}

	Event edited = fEvent;
	edited.title = Trim(fTitle->Text());
	if (edited.title.empty())
		edited.title = "New Event";
	edited.location = Trim(fLocation->Text());
	edited.url = Trim(fUrl->Text());
	edited.notes = fNotes->Text();
	edited.calendarId = fCalendarId;
	edited.attendees.clear();
	{
		std::string list = fInvitees->Text();
		size_t start = 0;
		while (start <= list.size()) {
			size_t comma = list.find_first_of(",;\n", start);
			if (comma == std::string::npos)
				comma = list.size();
			std::string one = Trim(list.substr(start, comma - start));
			if (!one.empty() && one.find('@') != std::string::npos)
				edited.attendees.push_back(one);
			start = comma + 1;
		}
	}

	bool allDay = fAllDay->Value() == B_CONTROL_ON;
	Date startDate, endDate;
	Format::ParseDate(fStartDate->Text(), startDate);
	Format::ParseDate(fEndDate->Text(), endDate);
	if (allDay) {
		edited.allDay = true;
		edited.startDate = startDate;
		edited.endDate = endDate.AddDays(1);
		edited.tzid.clear();
	} else {
		int startHour, startMinute, endHour, endMinute;
		Format::ParseTime(fStartTime->Text(), startHour, startMinute);
		Format::ParseTime(fEndTime->Text(), endHour, endMinute);
		Instant start = ToInstant(DateTime(startDate, startHour, startMinute), fZone);
		Instant end = ToInstant(DateTime(endDate, endHour, endMinute), fZone);
		// Untouched times keep their exact instants and zone.
		bool unchanged = !fNew && !fEvent.allDay
			&& start / 60 == fEvent.start / 60 && end / 60 == fEvent.end / 60;
		if (!unchanged) {
			edited.start = start;
			edited.end = end;
			// The event keeps the zone it was written in (it drives
			// recurrence across DST); one that was all-day becomes local.
			if (fNew || fEvent.allDay)
				edited.tzid.clear();
		}
		// The zone picked in the editor.
		if (!fZone.empty())
			edited.tzid = fZone;
		else if (!fEvent.allDay && !fEvent.tzid.empty() && fEvent.tzid != "UTC"
			&& CanonicalZone(fEvent.tzid) != CanonicalZone(LocalZone()))
			edited.tzid.clear();
		edited.allDay = false;
	}

	bool ruleChanged = fPreset != fInitialPreset;
	std::string rule = fRule;
	if (ruleChanged && fPreset != RepeatPreset::Custom)
		rule = RuleFromPreset(fPreset, startDate);
	edited.rrule = rule;

	if (fAlarm != fInitialAlarm) {
		if (fAlarm < 0)
			edited.alarms.clear();
		else if (edited.alarms.empty())
			edited.alarms.push_back(fAlarm);
		else
			edited.alarms[0] = fAlarm;
	}

	EditScope scope = EditScope::All;
	if (!fNew && fRecurring) {
		if (!AskEditScope(this, false, scope))
			return;
		// A changed rule cannot apply to one occurrence alone, and a series
		// lives in one calendar.
		if (ruleChanged && scope == EditScope::This)
			scope = EditScope::All;
		if (fCalendarId != fEvent.calendarId)
			scope = EditScope::All;
	}

	Store& store = TheApp()->Store();
	std::string id;
	{
		// Ends before Finish(): Quit() leaves the thread without running
		// destructors, which would keep the store locked.
		aircal::UndoScope undo(TheApp()->Undo(), TheApp()->Store(),
			fNew ? "New Event" : "Edit Event");
		if (!fNew && fRecurring && ruleChanged && rule.empty()
			&& scope == EditScope::Future) {
			// "Never" from here on: end the series and keep this one on its own.
			DeleteOccurrence(store, fRef, EditScope::Future);
			Event single = edited;
			single.id.clear();
			single.uid.clear();
			single.remoteId.clear();
			single.etag.clear();
			single.raw.clear();
			single.masterId.clear();
			single.recurrenceId = 0;
			single.recurrenceDate = Date();
			single.exdates.clear();
			single.exdateDays.clear();
			single.rdates.clear();
			single.cancelled = false;
			id = SaveEvent(store, OccurrenceRef(), single, EditScope::All);
		} else
			id = SaveEvent(store, fRef, edited, scope);
	}

	TheApp()->RequestPush(fCalendarId);
	if (fEvent.calendarId != fCalendarId && !fEvent.calendarId.empty())
		TheApp()->RequestPush(fEvent.calendarId);
	Finish(id, false);
}


void
EventEditorWindow::Delete()
{
	if (fNew || fReadOnly)
		return;
	EditScope scope = EditScope::All;
	if (fRecurring) {
		if (!AskEditScope(this, true, scope))
			return;
	} else {
		std::string title = fEvent.title.empty() ? "this event"
			: "\"" + fEvent.title + "\"";
		std::string text = "Delete " + title + "?";
		BAlert* alert = new BAlert("Delete Event", text.c_str(), "Cancel",
			"Delete", NULL, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
		alert->SetShortcut(0, B_ESCAPE);
		if (alert->Go() != 1)
			return;
	}
	{
		aircal::UndoScope undo(TheApp()->Undo(), TheApp()->Store(), "Delete Event");
		DeleteOccurrence(TheApp()->Store(), fRef, scope);
	}
	TheApp()->RequestPush(fEvent.calendarId);
	Finish(fRef.eventId, true);
}


void
EventEditorWindow::Finish(const std::string& id, bool deleted)
{
	TheApp()->ScheduleSave();
	BMessage saved(kMsgItemSaved);
	saved.AddString("kind", "event");
	saved.AddString("id", id.c_str());
	if (deleted)
		saved.AddBool("deleted", true);
	PostToMain(&saved);
	Quit();
}


void
EventEditorWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgAllDay:
			AllDayToggled();
			break;
		case kMsgStartCommitted:
			StartCommitted();
			break;
		case kMsgFieldCommitted:
			NormalizeField(fEndDate, true);
			NormalizeField(fEndTime, false);
			Validate(true);
			break;
		case kMsgFieldModified:
			Validate(true);
			break;
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
		case kMsgCalendar:
		{
			const char* id;
			if (message->FindString("calendar", &id) == B_OK)
				CalendarChosen(id);
			break;
		}
		case kMsgUrlModified:
			UpdateJoin();
			break;
		case kMsgZone:
		{
			const char* zone;
			if (message->FindString("zone", &zone) == B_OK)
				ZoneChosen(zone);
			break;
		}
		case kMsgJoin:
		{
			std::string url = JoinUrl();
			if (!url.empty())
				OpenUrl(url);
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


std::string
EditorKey(const OccurrenceRef& ref)
{
	if (ref.eventId.empty())
		return std::string();
	std::string key = "event:" + ref.eventId + "@";
	if (ref.originalStart != 0)
		key += std::to_string(ref.originalStart);
	else
		key += ref.originalDate.ToIso();
	return key;
}

}	// namespace


void
OpenEventEditor(const OccurrenceRef& ref, const Event& draft, float anchorX,
	float anchorY)
{
	std::string key = EditorKey(ref);
	if (ActivateRegistered(key))
		return;

	Store& store = TheApp()->Store();
	Event event;
	bool isNew = ref.eventId.empty();
	if (!isNew) {
		if (!ResolveOccurrence(store, ref, event))
			return;		// gone meanwhile
	} else {
		event = draft;
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		Calendar* calendar = store.FindCalendar(event.calendarId);
		if (calendar == NULL || calendar->readOnly || !calendar->events)
			calendar = store.DefaultCalendar(false);
		if (calendar == NULL) {
			BAlert* alert = new BAlert("airCal", "There is no calendar to add "
				"the event to. Add an account or a calendar first.", "OK");
			alert->Go(NULL);
			return;
		}
		event.calendarId = calendar->id;
		if (event.alarms.empty()) {
			if (!calendar->defaultAlarms.empty())
				event.alarms = calendar->defaultAlarms;
			else if (store.Prefs().defaultAlarm >= 0)
				event.alarms.push_back(store.Prefs().defaultAlarm);
		}
		int duration = std::max(5, store.Prefs().defaultDuration) * 60;
		if (event.allDay) {
			if (event.startDate.DayNumber() == 0)
				event.startDate = Today();
			if (event.endDate <= event.startDate)
				event.endDate = event.startDate.AddDays(1);
		} else {
			if (event.start == 0) {
				// The next full hour.
				DateTime wall = ToWall(Now());
				wall.minute = 0;
				wall.second = 0;
				event.start = ToInstant(wall) + 3600;
			}
			if (event.end <= event.start)
				event.end = event.start + duration;
			event.tzid.clear();
		}
	}

	EventEditorWindow* window = new EventEditorWindow(ref, event, isNew, key);
	Register(key, window);
	PlaceWindow(window, anchorX, anchorY);
	window->Show();
}
