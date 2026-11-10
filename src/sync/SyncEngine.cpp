#include "SyncEngine.h"

#include <chrono>

namespace aircal {

static int64_t
NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}


SyncEngine::SyncEngine(Store& store, Http& http, Secrets& secrets)
	:
	fStore(store),
	fHttp(http),
	fSecrets(secrets)
{
}


SyncEngine::~SyncEngine()
{
	Stop();
}


void
SyncEngine::Start()
{
	std::lock_guard<std::mutex> lock(fLock);
	if (fThread.joinable())
		return;
	fQuit = false;
	fRunning = true;
	fNextPeriodic = 0;
	fThread = std::thread([this] { Run(); });
}


bool
SyncEngine::Stop(int timeoutMs)
{
	{
		std::unique_lock<std::mutex> lock(fLock);
		fQuit = true;
		fCancel = true;
		fCondition.notify_all();
		if (timeoutMs >= 0 && fThread.joinable()
			&& !fCondition.wait_for(lock, std::chrono::milliseconds(timeoutMs),
				[this] { return !fRunning; })) {
			fThread.detach();
			return false;
		}
	}
	if (fThread.joinable())
		fThread.join();
	fCancel = false;
	return true;
}


void
SyncEngine::Request(const std::string& accountId, bool full, int delayMs)
{
	{
		std::lock_guard<std::mutex> lock(fLock);
		int64_t due = NowMs() + delayMs;
		auto found = fRequests.find(accountId);
		if (found == fRequests.end())
			fRequests[accountId] = {due, full};
		else {
			// Debounce: a later request for the same account moves it.
			found->second.first = delayMs > 0 ? std::max(found->second.first, due)
				: std::min(found->second.first, due);
			found->second.second = found->second.second || full;
		}
	}
	fCondition.notify_all();
}


void
SyncEngine::CancelCurrent()
{
	fCancel = true;
}


bool
SyncEngine::IsBusy() const
{
	std::lock_guard<std::mutex> lock(fLock);
	return fBusy;
}


void
SyncEngine::SetListener(std::function<void(const SyncStatus&)> listener)
{
	std::lock_guard<std::mutex> lock(fLock);
	fListener = listener;
}


void
SyncEngine::Notify(const SyncStatus& status)
{
	std::function<void(const SyncStatus&)> listener;
	{
		std::lock_guard<std::mutex> lock(fLock);
		listener = fListener;
	}
	if (listener)
		listener(status);
}


void
SyncEngine::Run()
{
	std::unique_lock<std::mutex> lock(fLock);
	while (!fQuit) {
		int64_t now = NowMs();
		int minutes;
		{
			std::lock_guard<std::recursive_mutex> storeLock(fStore.Mutex());
			minutes = fStore.Prefs().syncMinutes;
		}
		if (fNextPeriodic == 0)
			fNextPeriodic = now + minutes * 60000LL;
		if (now >= fNextPeriodic) {
			fRequests[""] = {now, fRequests.count("") != 0 && fRequests[""].second};
			fNextPeriodic = now + minutes * 60000LL;
		}
		// The earliest due request.
		std::string account;
		bool full = false;
		int64_t earliest = fNextPeriodic;
		bool found = false;
		for (auto& request : fRequests) {
			if (request.second.first <= now && !found) {
				account = request.first;
				full = request.second.second;
				found = true;
			}
			earliest = std::min(earliest, request.second.first);
		}
		if (!found) {
			fCondition.wait_for(lock, std::chrono::milliseconds(
				std::max<int64_t>(50, std::min<int64_t>(earliest - now, 60000))));
			continue;
		}
		fRequests.erase(account);
		fBusy = true;
		fCancel = false;
		lock.unlock();

		std::vector<std::string> accounts;
		{
			std::lock_guard<std::recursive_mutex> storeLock(fStore.Mutex());
			for (auto& item : fStore.Accounts()) {
				if (item->enabled && (account.empty() || item->id == account))
					accounts.push_back(item->id);
			}
		}
		for (auto& id : accounts) {
			if (fCancel)
				break;
			SyncAccount(id, full);
		}
		fStore.Save();

		lock.lock();
		fBusy = false;
	}
	fRunning = false;
	lock.unlock();
	fCondition.notify_all();
}


void
SyncEngine::SyncAccount(const std::string& accountId, bool full)
{
	AccountType type;
	{
		std::lock_guard<std::recursive_mutex> storeLock(fStore.Mutex());
		Account* account = fStore.FindAccount(accountId);
		if (account == nullptr)
			return;
		type = account->type;
	}
	SyncStatus status;
	status.accountId = accountId;
	status.running = true;
	Notify(status);

	SyncContext context(fStore, fHttp, fSecrets, fCancel);
	context.full = full;
	context.progress = [&](const std::string& text) {
		SyncStatus progress = status;
		progress.message = text;
		Notify(progress);
	};
	std::unique_ptr<Provider> provider = CreateProvider(type);
	try {
		provider->Sync(accountId, context);
	} catch (const SyncError& error) {
		status.error = error.what();
		status.authentication = error.authentication;
	} catch (const std::exception& error) {
		status.error = std::string("Sync failed: ") + error.what();
	}
	if (status.error == "Cancelled")
		status.error.clear();
	{
		std::lock_guard<std::recursive_mutex> storeLock(fStore.Mutex());
		Account* account = fStore.FindAccount(accountId);
		if (account != nullptr) {
			account->lastError = status.error;
			if (status.error.empty())
				account->lastSync = Now();
		}
	}
	fStore.Changed();
	status.running = false;
	Notify(status);
}

}	// namespace aircal
