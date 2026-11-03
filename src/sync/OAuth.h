// OAuth 2.0 for installed applications (RFC 8252): the system browser, a
// loopback redirect and PKCE.
#pragma once

#include "Provider.h"

#include <atomic>
#include <functional>
#include <map>
#include <string>

namespace aircal {

struct Pkce {
	std::string verifier;
	std::string challenge;	// S256
	static Pkce Make();
};

std::string RandomToken(size_t bytes = 24);	// base64url

// A one-shot HTTP listener on 127.0.0.1 (and ::1 on the same port when
// possible) that waits for the browser's redirect.
class LoopbackListener {
public:
	LoopbackListener();
	~LoopbackListener();

	// port 0 picks a free port.
	bool Start(int port = 0);
	int Port() const { return fPort; }
	// "http://127.0.0.1:<port><path>" or with host "localhost".
	std::string RedirectUri(const std::string& path,
		const std::string& host = "127.0.0.1") const;
	// Waits for "GET <path>?..." and answers it with a small page. Returns
	// the decoded query; empty on timeout/cancel.
	std::map<std::string, std::string> Wait(const std::string& path,
		int timeoutSeconds, const std::atomic<bool>& cancelled,
		const std::string& successTitle);
	void Stop();

private:
	int fSocket4 = -1;
	int fSocket6 = -1;
	int fPort = 0;
};

struct TokenResponse {
	std::string accessToken;
	std::string refreshToken;
	Instant expires = 0;
	std::string scope;
	std::string idToken;
};

// POSTs a form to a token endpoint; throws SyncError on failure.
TokenResponse RequestToken(Http& http, const std::string& url,
	const std::vector<std::pair<std::string, std::string>>& form,
	const std::atomic<bool>& cancelled);

// Refreshes credentials when they expire within a minute; returns true when
// they changed (the caller saves them). Throws SyncError(auth) when refresh
// is impossible.
bool RefreshIfNeeded(Http& http, const std::string& tokenUrl,
	Credentials& credentials, const std::atomic<bool>& cancelled,
	bool force = false);

// --- Interactive sign-in ---------------------------------------------------------

typedef std::function<void(const std::string& url)> OpenBrowser;

struct SignInResult {
	Credentials credentials;
	std::string email;
	std::string name;
};

// Google: needs a Desktop app client (id, and secret if issued).
SignInResult GoogleSignIn(Http& http, const Endpoints& endpoints,
	const std::string& clientId, const std::string& clientSecret,
	OpenBrowser open, const std::atomic<bool>& cancelled);

// Todoist: registers a dynamic public client (RFC 7591) for the loopback
// redirect, then authorization code + PKCE. No setup needed.
SignInResult TodoistSignIn(Http& http, const Endpoints& endpoints,
	OpenBrowser open, const std::atomic<bool>& cancelled);

// Todoist personal API token: checks it and returns the user.
SignInResult TodoistTokenSignIn(Http& http, const Endpoints& endpoints,
	const std::string& token, const std::atomic<bool>& cancelled);

// The Google Desktop client: the first of ~/config/settings/airCal/
// google-client.json, ~/config/settings/Mail/google-client.json (shared with
// Mail), or the client built in (AIRCAL_GOOGLE_CLIENT_ID). Accepts Google's
// downloaded JSON ({"installed": {...}}) or {"client_id", "client_secret"}.
bool FindGoogleClient(const std::string& settingsRoot, std::string& id,
	std::string& secret, std::string* source = nullptr);
bool ParseGoogleClientJson(const std::string& text, std::string& id,
	std::string& secret);

}	// namespace aircal
