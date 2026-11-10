#include "App.h"

#include "Dialogs.h"
#include "Format.h"
#include "MainWindow.h"
#include "Messages.h"
#include "Theme.h"
#include "core/ICal.h"
#include "net/CurlHttp.h"
#include "platform/KeyStoreSecrets.h"

#include <nlohmann/json.hpp>

#include <AboutWindow.h>
#include <Alert.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <FormattingConventions.h>
#include <Locale.h>
#include <LocaleRoster.h>
#include <Notification.h>
#include <Path.h>
#include <Roster.h>
#include <TimeZone.h>
#include <Url.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unistd.h>

using namespace aircal;

static const char* kSignature = "application/x-vnd.airos-airCal";

enum {
	kMsgSaveNow = 'savn',
	kMsgCheckAlarms = 'calm',
	kMsgSyncStatusInternal = 'sysi',
	kMsgScheduleSave = 'scsv'
};


namespace {

// Test runs (AIRCAL_FILE_SECRETS=1): credentials in a 0600 JSON file in the
// data folder instead of the keyring, so they never touch the user's keys.
class FileSecrets : public Secrets {
public:
	explicit FileSecrets(const std::string& path) : fPath(path) {}

	bool Get(const std::string& key, std::string& value) override
	{
		nlohmann::json all = Load();
		if (!all.contains(key) || !all[key].is_string())
			return false;
		value = all[key].get<std::string>();
		return true;
	}
	bool Set(const std::string& key, const std::string& value) override
	{
		nlohmann::json all = Load();
		all[key] = value;
		return Write(all);
	}
	void Remove(const std::string& key) override
	{
		nlohmann::json all = Load();
		all.erase(key);
		Write(all);
	}

private:
	nlohmann::json Load()
	{
		BFile file(fPath.c_str(), B_READ_ONLY);
		off_t size = 0;
		if (file.InitCheck() != B_OK || file.GetSize(&size) != B_OK || size <= 0)
			return nlohmann::json::object();
		std::string text(size, '\0');
		file.Read(&text[0], size);
		nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
		return j.is_object() ? j : nlohmann::json::object();
	}
	bool Write(const nlohmann::json& all)
	{
		BFile file(fPath.c_str(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
		if (file.InitCheck() != B_OK)
			return false;
		file.SetPermissions(0600);
		std::string text = all.dump();
		return file.Write(text.data(), text.size()) == (ssize_t)text.size();
	}

	std::string fPath;
};

}	// namespace


App*
TheApp()
{
	return static_cast<App*>(be_app);
}


void
OpenUrl(const std::string& url)
{
	BUrl target(url.c_str(), true);
	if (target.IsValid() && target.OpenWithPreferredApplication(false) == B_OK)
		return;
	// Fallback: the open command.
	const char* args[] = {url.c_str(), nullptr};
	be_roster->Launch("text/html", 1, args);
}


App::App()
	:
	BApplication(kSignature)
{
	Theme::Refresh();

	BPath settings;
	find_directory(B_USER_SETTINGS_DIRECTORY, &settings, true);
	fSettingsRoot = settings.Path();
	const char* override = getenv("AIRCAL_SETTINGS_DIR");
	fDataDirectory = override != nullptr ? override
		: fSettingsRoot + "/airCal";

	// The system's time zone drives every local-time conversion.
	BTimeZone zone;
	if (BLocaleRoster::Default()->GetDefaultTimeZone(&zone) == B_OK
		&& zone.ID().Length() > 0)
		SetLocalZone(zone.ID().String());
	else
		SetLocalZone("UTC");

	fStore.reset(new aircal::Store(fDataDirectory));
	fStore->Load();
	{
		// First run: take the clock style from the locale.
		BFormattingConventions conventions;
		std::string text;
		const BLocale* locale = BLocaleRoster::Default()->GetDefaultLocale();
		if (locale != nullptr
			&& locale->GetFormattingConventions(&conventions) == B_OK) {
			std::lock_guard<std::recursive_mutex> lock(fStore->Mutex());
			struct stat st;
			if (stat((fDataDirectory + "/preferences.json").c_str(), &st) != 0)
				fStore->Prefs().use24Hour = conventions.Use24HourClock();
		}
		Format::SetUse24Hour(fStore->Prefs().use24Hour);
	}
	if (getenv("AIRCAL_FILE_SECRETS") != nullptr)
		fSecrets.reset(new FileSecrets(fDataDirectory + "/test-credentials.json"));
	else
		fSecrets.reset(new KeyStoreSecrets(fDataDirectory + "/credentials.json"));
	fHttp.reset(new CurlHttp());
	fSync.reset(new SyncEngine(*fStore, *fHttp, *fSecrets));
	fAlarmsCheckedUntil = Now();
}


App::~App()
{
	// A sync stuck in a call that does not see the cancel flag (the keystore
	// waiting on an access prompt nobody answers) must not keep airCal from
	// quitting. QuitRequested() has saved, and the store writes atomically:
	// leave without tearing down what the stuck thread still uses.
	if (!fSync->Stop(10000)) {
		fprintf(stderr, "airCal: the sync did not stop; quitting anyway.\n");
		_exit(0);
	}
	delete fSaveRunner;
	delete fAlarmRunner;
	fStore->Save();
}


aircal::Http&
App::Http()
{
	return *fHttp;
}


std::string
App::CloudBase() const
{
	return Endpoints::TestBase();
}


void
App::ReadyToRun()
{
	fWindow = new MainWindow();
	BMessenger window(fWindow);
	fStore->SetChangeListener([window]() mutable {
		window.SendMessage(kMsgStoreChanged);
	});
	fSync->SetListener([window](const SyncStatus& status) mutable {
		BMessage message(kMsgSyncStatus);
		message.AddString("account", status.accountId.c_str());
		message.AddBool("running", status.running);
		message.AddString("message", status.message.c_str());
		message.AddString("error", status.error.c_str());
		message.AddBool("auth", status.authentication);
		window.SendMessage(&message);
	});
	fWindow->Show();
	fSync->Start();
	fSync->Request("", false, 500);

	BMessage check(kMsgCheckAlarms);
	fAlarmRunner = new BMessageRunner(be_app_messenger, &check, 20000000LL);

	for (auto& path : fPendingImports) {
		BMessage refs(B_REFS_RECEIVED);
		entry_ref ref;
		if (get_ref_for_path(path.c_str(), &ref) == B_OK) {
			refs.AddRef("refs", &ref);
			RefsReceived(&refs);
		}
	}
	fPendingImports.clear();
}


void
App::ArgvReceived(int32 argc, char** argv)
{
	BMessage navigate(kMsgGoToDate);
	bool hasNavigation = false;
	for (int32 i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--view") == 0 && i + 1 < argc) {
			navigate.AddString("view", argv[++i]);
			hasNavigation = true;
		} else if (strcmp(argv[i], "--date") == 0 && i + 1 < argc) {
			navigate.AddString("date", argv[++i]);
			hasNavigation = true;
		} else if (strcmp(argv[i], "--sync") == 0) {
			fSync->Request("", true, 0);
		} else if (argv[i][0] != '-') {
			BPath path(argv[i]);
			if (path.InitCheck() == B_OK)
				fPendingImports.push_back(path.Path());
		}
	}
	if (hasNavigation) {
		if (fWindow != nullptr)
			BMessenger(fWindow).SendMessage(&navigate);
		else {
			// Before ReadyToRun: remember it for the window.
			const char* view = nullptr;
			if (navigate.FindString("view", &view) == B_OK) {
				std::lock_guard<std::recursive_mutex> lock(fStore->Mutex());
				fStore->Prefs().view = view;
			}
			const char* date = nullptr;
			if (navigate.FindString("date", &date) == B_OK)
				setenv("AIRCAL_START_DATE", date, 1);
		}
	}
	if (fWindow != nullptr && !fPendingImports.empty()) {
		for (auto& path : fPendingImports) {
			BMessage refs(B_REFS_RECEIVED);
			entry_ref ref;
			if (get_ref_for_path(path.c_str(), &ref) == B_OK) {
				refs.AddRef("refs", &ref);
				RefsReceived(&refs);
			}
		}
		fPendingImports.clear();
	}
}


void
App::RefsReceived(BMessage* message)
{
	entry_ref ref;
	int imported = 0;
	std::string calendarName;
	for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; i++) {
		BFile file(&ref, B_READ_ONLY);
		off_t size = 0;
		if (file.InitCheck() != B_OK || file.GetSize(&size) != B_OK
			|| size > 32 * 1024 * 1024)
			continue;
		std::string text(size, '\0');
		if (file.Read(&text[0], size) != size)
			continue;
		std::vector<Event> events = ical::ImportEvents(text);
		if (events.empty())
			continue;
		std::lock_guard<std::recursive_mutex> lock(fStore->Mutex());
		Calendar* calendar = fStore->DefaultCalendar(false);
		if (calendar == nullptr)
			continue;
		calendarName = calendar->name;
		// Masters first, then overrides with their new master ids.
		std::map<std::string, std::string> ids;
		for (Event& event : events) {
			if (event.IsOverride())
				continue;
			std::string old = event.id;
			event.id = NewId();
			ids[old] = event.id;
			event.calendarId = calendar->id;
			event.remoteId.clear();
			event.etag.clear();
			event.pending = Pending::Create;
			fStore->UpdateEvent(event);
			imported++;
		}
		for (Event& event : events) {
			if (!event.IsOverride() || ids.count(event.masterId) == 0)
				continue;
			event.id = NewId();
			event.masterId = ids[event.masterId];
			event.calendarId = calendar->id;
			event.remoteId.clear();
			event.pending = Pending::Create;
			fStore->UpdateEvent(event);
		}
		RequestPush(calendar->id);
	}
	if (imported > 0) {
		ScheduleSave();
		BString text;
		text.SetToFormat("Imported %d event%s into \"%s\".", imported,
			imported == 1 ? "" : "s", calendarName.c_str());
		BNotification notification(B_INFORMATION_NOTIFICATION);
		notification.SetGroup("airCal");
		notification.SetTitle("Events imported");
		notification.SetContent(text);
		notification.Send();
	}
}


void
App::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgSaveNow:
			delete fSaveRunner;
			fSaveRunner = nullptr;
			fStore->Save();
			break;
		case kMsgCheckAlarms:
			CheckAlarms();
			break;
		case kMsgScheduleSave:
		{
			delete fSaveRunner;
			BMessage save(kMsgSaveNow);
			fSaveRunner = new BMessageRunner(be_app_messenger, &save, 1500000LL, 1);
			break;
		}
		case kMsgSyncNow:
			fSync->Request("", message->GetBool("full", false), 0);
			break;
		case kMsgShowAccounts:
			OpenAccountsWindow();
			break;
		case kMsgShowSettings:
			OpenSettingsWindow();
			break;
		case B_COLORS_UPDATED:
			Theme::Refresh();
			if (fWindow != nullptr)
				BMessenger(fWindow).SendMessage(message);
			break;
		case kMsgSetView:
		case kMsgGoToday:
		case kMsgGoNext:
		case kMsgGoPrevious:
		case kMsgGoToDate:
		case kMsgNewEvent:
		case kMsgNewTask:
		case kMsgScriptTask:
		{
			// hey waits for the answer: run it in the window and reply here.
			BMessage reply(B_REPLY);
			if (fWindow != nullptr && fWindow->Lock()) {
				fWindow->ScriptTask(message, reply);
				fWindow->Unlock();
			}
			message->SendReply(&reply);
			break;
		}
		case kMsgQuickEntry:
			if (fWindow != nullptr && message->IsSourceWaiting()) {
				BMessage reply(B_REPLY);
				const char* text = message->GetString("text", "");
				if (fWindow->Lock()) {
					reply.AddString("id", fWindow->QuickEntry(text).c_str());
					fWindow->Unlock();
				}
				message->SendReply(&reply);
				break;
			}
			// fall through
		case kMsgTaskFilter:
		case kMsgToggleSidebar:
		case kMsgToggleTask:
		case 'adgo':
		case 'adcd':
		case 'adtd':
			// Scripting entry points (hey airCal 'svew' with view=month).
			if (fWindow != nullptr)
				BMessenger(fWindow).SendMessage(message);
			break;
		default:
			BApplication::MessageReceived(message);
	}
}


bool
App::QuitRequested()
{
	fSync->CancelCurrent();
	if (fWindow != nullptr && fWindow->Lock()) {
		fWindow->SaveState();
		fWindow->Unlock();
	}
	fStore->Save();
	return BApplication::QuitRequested();
}


void
App::AboutRequested()
{
	BAboutWindow* about = new BAboutWindow("airCal", kSignature);
	about->AddDescription("Calendars and to-dos for air/OS, with Google "
		"Calendar, Google Tasks, CalDAV and Todoist.");
	const char* authors[] = {"airos-works", nullptr};
	about->AddAuthors(authors);
	about->AddCopyright(2026, "airos-works");
	about->Show();
}


void
App::ScheduleSave()
{
	// Called from any window's thread: the runner is only touched on the
	// application's thread.
	PostMessage(kMsgScheduleSave);
}


void
App::RequestPush(const std::string& calendarId)
{
	std::string account;
	{
		std::lock_guard<std::recursive_mutex> lock(fStore->Mutex());
		Calendar* calendar = fStore->FindCalendar(calendarId);
		if (calendar != nullptr)
			account = calendar->accountId;
	}
	if (!account.empty())
		fSync->Request(account, false, 1500);
	else
		fSync->Request("", false, 1500);
}


void
App::CheckAlarms()
{
	Instant now = Now();
	Instant from = fAlarmsCheckedUntil;
	if (now - from > 3600)
		from = now - 3600;	// after a suspend, don't flood
	fAlarmsCheckedUntil = now;
	bool enabled;
	std::vector<std::pair<std::string, std::string>> notes;	// title, content
	{
		std::lock_guard<std::recursive_mutex> lock(fStore->Mutex());
		enabled = fStore->Prefs().notifications;
		if (!enabled)
			return;
		// Events whose alarm time falls in (from, now].
		std::vector<DisplayEvent> events = fStore->EventsBetween(from,
			now + 8 * 86400LL);
		for (const DisplayEvent& display : events) {
			const Event* source = fStore->FindEvent(display.overrideId.empty()
				? display.eventId : display.overrideId);
			if (source == nullptr)
				continue;
			for (int minutes : source->alarms) {
				Instant at = display.start - minutes * 60LL;
				if (at > from && at <= now) {
					std::string content = display.allDay ? "All day"
						: Format::TimeRange(display.start, display.end);
					if (display.start - now > 3600 || display.allDay)
						content = Format::RelativeDay(display.startDate) + ", "
							+ content;
					if (!display.location.empty())
						content += " · " + display.location;
					notes.push_back({display.title, content});
					break;
				}
			}
		}
		for (auto& task : fStore->Tasks()) {
			if (task->completed || !task->hasDue || task->pending == Pending::Delete)
				continue;
			Instant due = task->dueHasTime ? task->dueTime
				: StartOfDay(task->dueDate) + 9 * 3600;
			std::vector<int> alarms = task->alarms;
			if (alarms.empty() && task->dueHasTime)
				alarms.push_back(0);
			for (int minutes : alarms) {
				Instant at = due - minutes * 60LL;
				if (at > from && at <= now) {
					notes.push_back({task->title, std::string("Due ")
						+ (task->dueHasTime ? Format::Time(task->dueTime)
							: Format::RelativeDay(task->dueDate))});
					break;
				}
			}
		}
	}
	for (auto& note : notes) {
		BNotification notification(B_INFORMATION_NOTIFICATION);
		notification.SetGroup("airCal");
		notification.SetTitle(note.first.c_str());
		notification.SetContent(note.second.c_str());
		notification.SetMessageID((note.first + note.second).c_str());
		notification.Send(30 * 1000000LL);
	}
}


int
main(int argc, char** argv)
{
	App app;
	app.Run();
	return 0;
}
