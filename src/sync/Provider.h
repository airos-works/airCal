// Sync providers: each account type knows how to push local changes to its
// service and pull the service's state into the Store.
#pragma once

#include "core/Http.h"
#include "core/Store.h"

#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace aircal {

// Keeps account credentials (BKeyStore in the application).
class Secrets {
public:
	virtual ~Secrets() {}
	virtual bool Get(const std::string& key, std::string& value) = 0;
	virtual bool Set(const std::string& key, const std::string& value) = 0;
	virtual void Remove(const std::string& key) = 0;
};

// What an account signs in with; stored as JSON under "account:<id>".
struct Credentials {
	std::string accessToken;
	std::string refreshToken;
	Instant expires = 0;		// 0 = does not expire
	std::string clientId;		// OAuth client (Google desktop / Todoist dynamic)
	std::string clientSecret;
	std::string password;		// CalDAV
	std::string apiToken;		// Todoist personal token

	static bool Load(Secrets& secrets, const std::string& accountId,
		Credentials& out);
	bool Save(Secrets& secrets, const std::string& accountId) const;
};

struct SyncError : std::runtime_error {
	bool authentication;
	explicit SyncError(const std::string& message, bool auth = false)
		: std::runtime_error(message), authentication(auth) {}
};

struct SyncContext {
	Store& store;
	Http& http;
	Secrets& secrets;
	const std::atomic<bool>& cancelled;
	std::function<void(const std::string&)> progress;
	bool full = false;		// ignore sync tokens / ctags

	SyncContext(Store& s, Http& h, Secrets& c, const std::atomic<bool>& x)
		: store(s), http(h), secrets(c), cancelled(x) {}
	void CheckCancelled() const
	{
		if (cancelled)
			throw SyncError("Cancelled");
	}
	void Progress(const std::string& text) const
	{
		if (progress)
			progress(text);
	}
};

class Provider {
public:
	virtual ~Provider() {}
	// Pushes pending changes of the account, then pulls. Throws SyncError.
	virtual void Sync(const std::string& accountId, SyncContext& context) = 0;
};

std::unique_ptr<Provider> CreateProvider(AccountType type);

// Service endpoints. Normally the real services; AIRCAL_CLOUD_BASE (or an
// account's `server` for Google/Todoist) points them at a test double such
// as tools/fake-cloud.py, which serves Google under /google and Todoist
// under /todoist.
struct Endpoints {
	std::string googleAuth;		// https://accounts.google.com/o/oauth2/v2/auth
	std::string googleToken;	// https://oauth2.googleapis.com/token
	std::string googleUserInfo;	// https://openidconnect.googleapis.com/v1/userinfo
	std::string googleCalendar;	// https://www.googleapis.com/calendar/v3
	std::string googleTasks;	// https://tasks.googleapis.com/tasks/v1
	std::string todoistAuth;	// https://app.todoist.com/oauth/authorize
	std::string todoistToken;	// https://api.todoist.com/oauth/access_token
	std::string todoistRegister;	// https://api.todoist.com/oauth/register
	std::string todoistApi;		// https://api.todoist.com/api/v1

	// base "" = the real services.
	static Endpoints For(const std::string& base);
	static std::string TestBase();	// AIRCAL_CLOUD_BASE or ""
};

// --- Helpers shared by providers ---------------------------------------------

// Parses "#RRGGBB" (or "RRGGBB") to 0xRRGGBB; fallback when invalid.
uint32_t ParseHexColor(const std::string& text, uint32_t fallback);
std::string FormatHexColor(uint32_t color);

// HTTP error -> SyncError with a readable message.
[[noreturn]] void ThrowHttpError(const std::string& service,
	const HttpResponse& response);

}	// namespace aircal
