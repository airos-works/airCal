// The application: owns the store, the sync engine and the windows.
#pragma once

#include <Application.h>
#include <MessageRunner.h>

#include "core/Store.h"
#include "core/Undo.h"
#include "sync/SyncEngine.h"

#include <memory>
#include <string>

class MainWindow;

namespace aircal {
class CurlHttp;
}

class App : public BApplication {
public:
	App();
	~App() override;

	void ReadyToRun() override;
	void MessageReceived(BMessage* message) override;
	void ArgvReceived(int32 argc, char** argv) override;
	void RefsReceived(BMessage* message) override;
	bool QuitRequested() override;
	void AboutRequested() override;

	aircal::Store& Store() { return *fStore; }
	aircal::SyncEngine& Sync() { return *fSync; }
	aircal::Secrets& Secrets() { return *fSecrets; }
	aircal::Http& Http();
	aircal::UndoStack& Undo() { return fUndo; }
	// ~/config/settings (B_USER_SETTINGS_DIRECTORY) and airCal's folder in it.
	const std::string& SettingsRoot() const { return fSettingsRoot; }
	const std::string& DataDirectory() const { return fDataDirectory; }
	// The base URL of a test double (AIRCAL_CLOUD_BASE), "" for the real services.
	std::string CloudBase() const;

	// Local changes: save soon and push to the server soon.
	void ScheduleSave();
	void RequestPush(const std::string& calendarId);

	MainWindow* Window() const { return fWindow; }

private:
	void CheckAlarms();

	std::unique_ptr<aircal::Store> fStore;
	std::unique_ptr<aircal::Secrets> fSecrets;
	std::unique_ptr<aircal::CurlHttp> fHttp;
	std::unique_ptr<aircal::SyncEngine> fSync;
	aircal::UndoStack fUndo;
	MainWindow* fWindow = nullptr;
	BMessageRunner* fSaveRunner = nullptr;
	BMessageRunner* fAlarmRunner = nullptr;
	std::string fSettingsRoot;
	std::string fDataDirectory;
	aircal::Instant fAlarmsCheckedUntil = 0;
	std::vector<std::string> fPendingImports;
};

App* TheApp();

// Opens a URL in the preferred web browser.
void OpenUrl(const std::string& url);
