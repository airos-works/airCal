// The settings window. Changes apply at once, as in Haiku's preferences.
#include "EventEditor.h"

#include "App.h"
#include "Dialogs.h"
#include "Format.h"
#include "Messages.h"

#include <Button.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <PopUpMenu.h>
#include <SeparatorView.h>
#include <StringView.h>

using namespace aircal;
using namespace DialogKit;

namespace {

enum {
	kMsgWeekStart = 'swks',
	kMsgDayStart = 'sdys',
	kMsgDuration = 'sdur',
	kMsgAlarm = 'salm',
	kMsgCalendar = 'scal',
	kMsgTaskList = 'stls',
	kMsgSyncEvery = 'ssyn',
	kMsgUse24Hour = 's24h',
	kMsgShowTasks = 'stsk',
	kMsgShowCompleted = 'scmp',
	kMsgNotifications = 'snot',
	kMsgClose = 'sclo'
};

const char* kSettingsKey = "settings";


BStringView*
FieldLabel(const char* text)
{
	BStringView* label = new BStringView(NULL, text);
	label->SetAlignment(B_ALIGN_RIGHT);
	label->SetExplicitAlignment(BAlignment(B_ALIGN_RIGHT,
		B_ALIGN_VERTICAL_CENTER));
	return label;
}


BMenuField*
ChoiceField(const char* name, uint32 what, const std::vector<int>& values,
	const std::vector<std::string>& labels, int current)
{
	BPopUpMenu* menu = new BPopUpMenu(name);
	for (size_t i = 0; i < values.size(); i++) {
		BMessage* message = new BMessage(what);
		message->AddInt32("value", values[i]);
		BMenuItem* item = new BMenuItem(labels[i].c_str(), message);
		menu->AddItem(item);
		if (values[i] == current)
			item->SetMarked(true);
	}
	return new BMenuField(name, NULL, menu);
}


std::string
MinutesText(int minutes)
{
	if (minutes == 1)
		return "1 minute";
	if (minutes == 60)
		return "1 hour";
	return std::to_string(minutes) + " minutes";
}


class SettingsWindow : public BWindow {
public:
	SettingsWindow();
	~SettingsWindow() override;

	void MessageReceived(BMessage* message) override;

private:
	void Apply(BMessage* message);

	BCheckBox* fUse24Hour;
	BCheckBox* fShowTasks;
	BCheckBox* fShowCompleted;
	BCheckBox* fNotifications;
};


SettingsWindow::SettingsWindow()
	:
	BWindow(BRect(0, 0, 400, 300), "airCal Settings", B_TITLED_WINDOW_LOOK,
		B_NORMAL_WINDOW_FEEL, B_NOT_ZOOMABLE | B_NOT_RESIZABLE
			| B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE
			| B_ASYNCHRONOUS_CONTROLS)
{
	Store& store = TheApp()->Store();
	Preferences prefs;
	std::string defaultCalendar;
	std::string defaultTaskList;
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		prefs = store.Prefs();
		Calendar* calendar = store.DefaultCalendar(false);
		if (calendar != NULL)
			defaultCalendar = calendar->id;
		Calendar* list = store.DefaultCalendar(true);
		if (list != NULL)
			defaultTaskList = list->id;
	}

	BMenuField* weekStart = ChoiceField("weekStart", kMsgWeekStart, {0, 1},
		{"Sunday", "Monday"}, prefs.weekStart);

	std::vector<int> hours;
	std::vector<std::string> hourLabels;
	for (int hour = 0; hour <= 12; hour++) {
		hours.push_back(hour);
		hourLabels.push_back(Format::Hour(hour));
	}
	BMenuField* dayStart = ChoiceField("dayStart", kMsgDayStart, hours,
		hourLabels, prefs.dayStartHour);

	std::vector<int> durations = {15, 30, 45, 60, 90, 120};
	std::vector<std::string> durationLabels;
	for (int minutes : durations)
		durationLabels.push_back(Format::Duration(minutes));
	BMenuField* duration = ChoiceField("duration", kMsgDuration, durations,
		durationLabels, prefs.defaultDuration);

	BPopUpMenu* alarmMenu = new BPopUpMenu("alert");
	AddAlarmItems(alarmMenu, prefs.defaultAlarm, kMsgAlarm);
	BMenuField* alarm = new BMenuField("alert", NULL, alarmMenu);

	BPopUpMenu* calendarMenu = new BPopUpMenu("calendar");
	AddCalendarItems(calendarMenu, false, defaultCalendar, kMsgCalendar);
	BMenuField* calendar = new BMenuField("calendar", NULL, calendarMenu);

	BPopUpMenu* listMenu = new BPopUpMenu("list");
	AddCalendarItems(listMenu, true, defaultTaskList, kMsgTaskList);
	BMenuField* list = new BMenuField("list", NULL, listMenu);

	std::vector<int> intervals = {1, 5, 15, 30, 60};
	std::vector<std::string> intervalLabels;
	for (int minutes : intervals)
		intervalLabels.push_back(MinutesText(minutes));
	BMenuField* syncEvery = ChoiceField("syncEvery", kMsgSyncEvery, intervals,
		intervalLabels, prefs.syncMinutes);

	fUse24Hour = new BCheckBox("use24", "24-hour time",
		new BMessage(kMsgUse24Hour));
	fUse24Hour->SetValue(prefs.use24Hour);
	fShowTasks = new BCheckBox("showTasks", "Show tasks in calendar",
		new BMessage(kMsgShowTasks));
	fShowTasks->SetValue(prefs.showTasksInCalendar);
	fShowCompleted = new BCheckBox("showCompleted", "Show completed tasks",
		new BMessage(kMsgShowCompleted));
	fShowCompleted->SetValue(prefs.showCompleted);
	fNotifications = new BCheckBox("notifications", "Notifications for alerts",
		new BMessage(kMsgNotifications));
	fNotifications->SetValue(prefs.notifications);

	BButton* close = new BButton("close", "Close", new BMessage(kMsgClose));

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING)
			.Add(FieldLabel("Week starts on:"), 0, 0)
			.Add(weekStart, 1, 0)
			.Add(FieldLabel("Day starts at:"), 0, 1)
			.Add(dayStart, 1, 1)
			.Add(FieldLabel("Default event length:"), 0, 2)
			.Add(duration, 1, 2)
			.Add(FieldLabel("Default alert:"), 0, 3)
			.Add(alarm, 1, 3)
			.Add(FieldLabel("Default calendar:"), 0, 4)
			.Add(calendar, 1, 4)
			.Add(FieldLabel("Default task list:"), 0, 5)
			.Add(list, 1, 5)
			.Add(FieldLabel("Sync every:"), 0, 6)
			.Add(syncEvery, 1, 6)
		.End()
		.Add(new BSeparatorView(B_HORIZONTAL))
		.AddGroup(B_VERTICAL, B_USE_SMALL_SPACING)
			.Add(fUse24Hour)
			.Add(fShowTasks)
			.Add(fShowCompleted)
			.Add(fNotifications)
		.End()
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(close)
		.End();

	SetDefaultButton(close);
}


SettingsWindow::~SettingsWindow()
{
	Unregister(kSettingsKey, this);
}


void
SettingsWindow::Apply(BMessage* message)
{
	Store& store = TheApp()->Store();
	int32 value = message->GetInt32("value", 0);
	const char* calendar = message->GetString("calendar", "");
	bool use24Hour;
	{
		std::lock_guard<std::recursive_mutex> lock(store.Mutex());
		Preferences& prefs = store.Prefs();
		switch (message->what) {
			case kMsgWeekStart:
				prefs.weekStart = value;
				break;
			case kMsgDayStart:
				prefs.dayStartHour = value;
				break;
			case kMsgDuration:
				prefs.defaultDuration = value;
				break;
			case kMsgAlarm:
				prefs.defaultAlarm = message->GetInt32("minutes", -1);
				break;
			case kMsgCalendar:
				prefs.defaultCalendar = calendar;
				break;
			case kMsgTaskList:
				prefs.defaultTaskList = calendar;
				break;
			case kMsgSyncEvery:
				prefs.syncMinutes = value;
				break;
			case kMsgUse24Hour:
				prefs.use24Hour = fUse24Hour->Value() == B_CONTROL_ON;
				break;
			case kMsgShowTasks:
				prefs.showTasksInCalendar = fShowTasks->Value() == B_CONTROL_ON;
				break;
			case kMsgShowCompleted:
				prefs.showCompleted = fShowCompleted->Value() == B_CONTROL_ON;
				break;
			case kMsgNotifications:
				prefs.notifications = fNotifications->Value() == B_CONTROL_ON;
				break;
		}
		use24Hour = prefs.use24Hour;
	}
	Format::SetUse24Hour(use24Hour);
	store.Changed();
	TheApp()->ScheduleSave();
	BMessage changed(kMsgPrefsChanged);
	PostToMain(&changed);
}


void
SettingsWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgWeekStart:
		case kMsgDayStart:
		case kMsgDuration:
		case kMsgAlarm:
		case kMsgCalendar:
		case kMsgTaskList:
		case kMsgSyncEvery:
		case kMsgUse24Hour:
		case kMsgShowTasks:
		case kMsgShowCompleted:
		case kMsgNotifications:
			Apply(message);
			break;
		case kMsgClose:
			Quit();
			break;
		default:
			BWindow::MessageReceived(message);
	}
}

}	// namespace


void
OpenSettingsWindow()
{
	if (ActivateRegistered(kSettingsKey))
		return;
	SettingsWindow* window = new SettingsWindow();
	Register(kSettingsKey, window);
	PlaceWindow(window, -1, -1);
	window->Show();
}
