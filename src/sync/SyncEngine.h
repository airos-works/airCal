// Runs the providers on a background thread: on request (debounced after
// local edits), periodically, and on demand.
#pragma once

#include "Provider.h"

#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace aircal {

struct SyncStatus {
	std::string accountId;
	bool running = false;
	std::string message;	// progress text while running
	std::string error;		// last error, "" when fine
	bool authentication = false;	// the error needs a new sign-in
};

class SyncEngine {
public:
	SyncEngine(Store& store, Http& http, Secrets& secrets);
	~SyncEngine();

	void Start();
	void Stop();

	// Syncs an account ("" = all enabled accounts) after delayMs.
	void Request(const std::string& accountId = "", bool full = false,
		int delayMs = 0);
	void CancelCurrent();
	bool IsBusy() const;

	// Called on the sync thread.
	void SetListener(std::function<void(const SyncStatus&)> listener);

private:
	void Run();
	void SyncAccount(const std::string& accountId, bool full);
	void Notify(const SyncStatus& status);

	Store& fStore;
	Http& fHttp;
	Secrets& fSecrets;
	std::thread fThread;
	mutable std::mutex fLock;
	std::condition_variable fCondition;
	bool fQuit = false;
	bool fBusy = false;
	std::atomic<bool> fCancel{false};
	// account id ("" = all) -> due time (ms since epoch) and full flag
	std::map<std::string, std::pair<int64_t, bool>> fRequests;
	int64_t fNextPeriodic = 0;
	std::function<void(const SyncStatus&)> fListener;
};

}	// namespace aircal
