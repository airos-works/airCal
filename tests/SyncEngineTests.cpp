#include "Test.h"
#include "core/Store.h"
#include "sync/SyncEngine.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unistd.h>

using namespace aircal;

namespace {

std::string
TempDir()
{
	char path[] = "/tmp/aircal-sync-XXXXXX";
	return mkdtemp(path);
}


class NoHttp : public Http {
public:
	HttpResponse Perform(const HttpRequest& request,
		const std::atomic<bool>* cancelled) override
	{
		HttpResponse response;
		response.error = "offline";
		return response;
	}
};


// A keystore whose reads wait until released, like BKeyStore while its
// access prompt is open.
class BlockingSecrets : public Secrets {
public:
	bool Get(const std::string& key, std::string& value) override
	{
		std::unique_lock<std::mutex> lock(fLock);
		fEntered = true;
		fCondition.notify_all();
		fCondition.wait(lock, [this] { return fReleased; });
		return false;
	}
	bool Set(const std::string& key, const std::string& value) override
	{
		return true;
	}
	void Remove(const std::string& key) override {}

	bool WaitEntered(int ms)
	{
		std::unique_lock<std::mutex> lock(fLock);
		return fCondition.wait_for(lock, std::chrono::milliseconds(ms),
			[this] { return fEntered; });
	}
	void Release()
	{
		std::lock_guard<std::mutex> lock(fLock);
		fReleased = true;
		fCondition.notify_all();
	}

private:
	std::mutex fLock;
	std::condition_variable fCondition;
	bool fEntered = false;
	bool fReleased = false;
};


struct Rig {
	Store store;
	NoHttp http;
	BlockingSecrets secrets;
	SyncEngine engine;

	Rig() : store(TempDir()), engine(store, http, secrets)
	{
		store.Load();
		Account account;
		account.type = AccountType::Google;
		account.name = "stuck";
		store.AddAccount(account);
	}
};


int64_t
ElapsedMs(std::chrono::steady_clock::time_point start)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - start).count();
}

}	// namespace


TEST(SyncStopsPromptlyWhenIdle)
{
	Rig rig;
	rig.engine.Start();
	auto start = std::chrono::steady_clock::now();
	CHECK(rig.engine.Stop(5000));
	CHECK(ElapsedMs(start) < 1000);
}


TEST(SyncStopGivesUpOnAStuckKeystore)
{
	// The rig must outlive the detached sync thread: it is never freed.
	static Rig* rig = new Rig;
	rig->engine.Start();
	rig->engine.Request("", false, 0);
	CHECK(rig->secrets.WaitEntered(5000));

	auto start = std::chrono::steady_clock::now();
	CHECK(!rig->engine.Stop(300));
	int64_t elapsed = ElapsedMs(start);
	CHECK(elapsed >= 250);
	CHECK(elapsed < 2000);

	// Let the thread finish so it does not outlive the test run.
	rig->secrets.Release();
	for (int i = 0; i < 100 && rig->engine.IsBusy(); i++)
		usleep(20000);
	CHECK(!rig->engine.IsBusy());
}
