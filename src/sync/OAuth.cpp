#include "OAuth.h"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using nlohmann::json;

namespace aircal {

Pkce
Pkce::Make()
{
	Pkce pkce;
	pkce.verifier = Base64UrlEncode(RandomBytes(48));
	pkce.challenge = Base64UrlEncode(Sha256(pkce.verifier));
	return pkce;
}


std::string
RandomToken(size_t bytes)
{
	return Base64UrlEncode(RandomBytes(bytes));
}


// --- Loopback listener ----------------------------------------------------------

LoopbackListener::LoopbackListener()
{
}


LoopbackListener::~LoopbackListener()
{
	Stop();
}


bool
LoopbackListener::Start(int port)
{
	Stop();
	fSocket4 = socket(AF_INET, SOCK_STREAM, 0);
	if (fSocket4 < 0)
		return false;
	int yes = 1;
	setsockopt(fSocket4, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(port);
	if (bind(fSocket4, (sockaddr*)&address, sizeof(address)) != 0
		|| listen(fSocket4, 4) != 0) {
		Stop();
		return false;
	}
	socklen_t length = sizeof(address);
	getsockname(fSocket4, (sockaddr*)&address, &length);
	fPort = ntohs(address.sin_port);
	fcntl(fSocket4, F_SETFD, FD_CLOEXEC);

	// "localhost" may resolve to ::1 first in the browser.
	fSocket6 = socket(AF_INET6, SOCK_STREAM, 0);
	if (fSocket6 >= 0) {
		setsockopt(fSocket6, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
		sockaddr_in6 address6;
		memset(&address6, 0, sizeof(address6));
		address6.sin6_family = AF_INET6;
		address6.sin6_addr = in6addr_loopback;
		address6.sin6_port = htons(fPort);
		if (bind(fSocket6, (sockaddr*)&address6, sizeof(address6)) != 0
			|| listen(fSocket6, 4) != 0) {
			close(fSocket6);
			fSocket6 = -1;
		} else
			fcntl(fSocket6, F_SETFD, FD_CLOEXEC);
	}
	return true;
}


void
LoopbackListener::Stop()
{
	if (fSocket4 >= 0)
		close(fSocket4);
	if (fSocket6 >= 0)
		close(fSocket6);
	fSocket4 = fSocket6 = -1;
}


std::string
LoopbackListener::RedirectUri(const std::string& path,
	const std::string& host) const
{
	return "http://" + host + ":" + std::to_string(fPort) + path;
}


static std::string
HtmlEscape(const std::string& text)
{
	std::string out;
	for (char c : text) {
		if (c == '<')
			out += "&lt;";
		else if (c == '>')
			out += "&gt;";
		else if (c == '&')
			out += "&amp;";
		else
			out += c;
	}
	return out;
}


static void
Respond(int client, int status, const std::string& title,
	const std::string& message)
{
	std::string body = "<!doctype html><html><head><meta charset=\"utf-8\">"
		"<title>airCal</title><style>body{font-family:sans-serif;"
		"display:flex;align-items:center;justify-content:center;height:90vh;"
		"background:#f4f4f6;color:#222}@media (prefers-color-scheme:dark){"
		"body{background:#1e1e20;color:#ddd}}div{text-align:center}"
		"h1{font-weight:600;font-size:22px}</style></head><body><div><h1>"
		+ HtmlEscape(title) + "</h1><p>" + HtmlEscape(message)
		+ "</p></div></body></html>";
	std::string response = "HTTP/1.1 " + std::to_string(status)
		+ (status == 200 ? " OK" : " Bad Request")
		+ "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: "
		+ std::to_string(body.size())
		+ "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n" + body;
	size_t sent = 0;
	while (sent < response.size()) {
		ssize_t n = send(client, response.data() + sent, response.size() - sent, 0);
		if (n <= 0)
			break;
		sent += n;
	}
}


std::map<std::string, std::string>
LoopbackListener::Wait(const std::string& path, int timeoutSeconds,
	const std::atomic<bool>& cancelled, const std::string& successTitle)
{
	std::map<std::string, std::string> result;
	Instant deadline = Now() + timeoutSeconds;
	while (Now() < deadline && !cancelled) {
		pollfd fds[2];
		int count = 0;
		if (fSocket4 >= 0)
			fds[count++] = {fSocket4, POLLIN, 0};
		if (fSocket6 >= 0)
			fds[count++] = {fSocket6, POLLIN, 0};
		if (count == 0)
			break;
		int ready = poll(fds, count, 200);
		if (ready < 0 && errno != EINTR)
			break;
		if (ready <= 0)
			continue;
		for (int i = 0; i < count; i++) {
			if ((fds[i].revents & POLLIN) == 0)
				continue;
			int client = accept(fds[i].fd, nullptr, nullptr);
			if (client < 0)
				continue;
			// Read the request head (bounded, with a short timeout).
			std::string request;
			Instant readDeadline = Now() + 5;
			while (request.find("\r\n\r\n") == std::string::npos
				&& request.size() < 16384 && Now() < readDeadline) {
				pollfd one = {client, POLLIN, 0};
				if (poll(&one, 1, 200) <= 0)
					continue;
				char buffer[2048];
				ssize_t n = recv(client, buffer, sizeof(buffer), 0);
				if (n <= 0)
					break;
				request.append(buffer, n);
			}
			std::string target;
			if (request.compare(0, 4, "GET ") == 0) {
				size_t space = request.find(' ', 4);
				if (space != std::string::npos)
					target = request.substr(4, space - 4);
			}
			std::string requestPath = target.substr(0, target.find('?'));
			if (requestPath != path) {
				// Browsers also ask for /favicon.ico.
				Respond(client, 404, "Not found", "");
				close(client);
				continue;
			}
			size_t question = target.find('?');
			result = ParseQuery(question == std::string::npos ? std::string()
				: target.substr(question + 1));
			if (result.count("error") != 0) {
				Respond(client, 400, "Sign-in was not completed",
					"You can close this tab and return to airCal.");
			} else {
				Respond(client, 200, successTitle,
					"You can close this tab and return to airCal.");
			}
			close(client);
			return result;
		}
	}
	return result;
}


// --- Tokens ------------------------------------------------------------------------

TokenResponse
RequestToken(Http& http, const std::string& url,
	const std::vector<std::pair<std::string, std::string>>& form,
	const std::atomic<bool>& cancelled)
{
	HttpRequest request;
	request.method = "POST";
	request.url = url;
	request.contentType = "application/x-www-form-urlencoded";
	request.body = FormEncode(form);
	request.headers.push_back({"Accept", "application/json"});
	HttpResponse response = http.Perform(request, &cancelled);
	if (response.status == 0)
		throw SyncError("The sign-in server could not be reached: " + response.error);
	json j = json::parse(response.body, nullptr, false);
	if (!response.Ok() || !j.is_object()) {
		std::string error;
		if (j.is_object() && j.contains("error") && j["error"].is_string())
			error = j["error"].get<std::string>();
		bool auth = error == "invalid_grant" || error == "invalid_client"
			|| error == "unauthorized_client" || response.status == 401
			|| error == "bad_authorization_code"
			|| error == "incorrect_application_credentials";
		throw SyncError(error.empty()
			? "Sign-in failed (HTTP " + std::to_string(response.status) + ")."
			: "Sign-in failed: " + error + ".", auth);
	}
	TokenResponse token;
	auto str = [&](const char* key) {
		auto it = j.find(key);
		return it != j.end() && it->is_string() ? it->get<std::string>()
			: std::string();
	};
	token.accessToken = str("access_token");
	token.refreshToken = str("refresh_token");
	token.scope = str("scope");
	token.idToken = str("id_token");
	auto expires = j.find("expires_in");
	if (expires != j.end() && expires->is_number()) {
		int64_t seconds = expires->get<int64_t>();
		// "10 years" means a token that does not expire.
		token.expires = seconds > 0 && seconds < 315000000 ? Now() + seconds : 0;
	}
	if (token.accessToken.empty())
		throw SyncError("Sign-in failed: no access token in the reply.");
	return token;
}


bool
RefreshIfNeeded(Http& http, const std::string& tokenUrl,
	Credentials& credentials, const std::atomic<bool>& cancelled, bool force)
{
	if (!force && (credentials.expires == 0
			|| credentials.expires > Now() + 60)
		&& !credentials.accessToken.empty())
		return false;
	if (credentials.refreshToken.empty()) {
		if (!credentials.accessToken.empty() && !force)
			return false;
		throw SyncError("The sign-in has expired. Sign in again.", true);
	}
	std::vector<std::pair<std::string, std::string>> form = {
		{"grant_type", "refresh_token"},
		{"refresh_token", credentials.refreshToken},
		{"client_id", credentials.clientId}};
	if (!credentials.clientSecret.empty())
		form.push_back({"client_secret", credentials.clientSecret});
	TokenResponse token;
	try {
		token = RequestToken(http, tokenUrl, form, cancelled);
	} catch (const SyncError& error) {
		if (error.authentication)
			throw SyncError("The sign-in has expired or was revoked. Sign in again.", true);
		throw;
	}
	credentials.accessToken = token.accessToken;
	if (!token.refreshToken.empty())
		credentials.refreshToken = token.refreshToken;
	credentials.expires = token.expires;
	return true;
}


// --- Google ------------------------------------------------------------------------

SignInResult
GoogleSignIn(Http& http, const Endpoints& endpoints, const std::string& clientId,
	const std::string& clientSecret, OpenBrowser open,
	const std::atomic<bool>& cancelled)
{
	if (clientId.empty())
		throw SyncError("No Google client is configured.");
	LoopbackListener listener;
	if (!listener.Start(0))
		throw SyncError("Could not open the local sign-in listener.");
	Pkce pkce = Pkce::Make();
	std::string state = RandomToken();
	std::string redirect = listener.RedirectUri("/oauth/callback");
	std::string scope = "openid email profile "
		"https://www.googleapis.com/auth/calendar "
		"https://www.googleapis.com/auth/tasks";
	std::string url = endpoints.googleAuth + "?" + FormEncode({
		{"client_id", clientId}, {"redirect_uri", redirect},
		{"response_type", "code"}, {"scope", scope}, {"state", state},
		{"code_challenge", pkce.challenge}, {"code_challenge_method", "S256"},
		{"access_type", "offline"}, {"prompt", "consent"}});
	open(url);
	std::map<std::string, std::string> reply = listener.Wait("/oauth/callback",
		600, cancelled, "airCal is signed in to Google");
	listener.Stop();
	if (cancelled)
		throw SyncError("Cancelled");
	if (reply.empty())
		throw SyncError("Google sign-in timed out.");
	if (reply.count("error") != 0)
		throw SyncError("Google sign-in was declined (" + reply["error"] + ").");
	if (reply["state"] != state || reply["code"].empty())
		throw SyncError("Google sign-in returned an unexpected reply.");

	std::vector<std::pair<std::string, std::string>> form = {
		{"grant_type", "authorization_code"}, {"code", reply["code"]},
		{"redirect_uri", redirect}, {"client_id", clientId},
		{"code_verifier", pkce.verifier}};
	if (!clientSecret.empty())
		form.push_back({"client_secret", clientSecret});
	TokenResponse token = RequestToken(http, endpoints.googleToken, form,
		cancelled);
	if (token.refreshToken.empty())
		throw SyncError("Google did not grant offline access. Try again.");
	if (!token.scope.empty()
		&& (token.scope.find("auth/calendar") == std::string::npos
			|| token.scope.find("auth/tasks") == std::string::npos))
		throw SyncError("Allow airCal to see your calendars and tasks to continue.");

	SignInResult result;
	result.credentials.accessToken = token.accessToken;
	result.credentials.refreshToken = token.refreshToken;
	result.credentials.expires = token.expires;
	result.credentials.clientId = clientId;
	result.credentials.clientSecret = clientSecret;

	HttpRequest request;
	request.url = endpoints.googleUserInfo;
	request.bearer = token.accessToken;
	HttpResponse response = http.Perform(request, &cancelled);
	json j = json::parse(response.body, nullptr, false);
	if (response.Ok() && j.is_object()) {
		if (j.contains("email") && j["email"].is_string())
			result.email = j["email"].get<std::string>();
		if (j.contains("name") && j["name"].is_string())
			result.name = j["name"].get<std::string>();
	}
	if (result.email.empty())
		result.email = "Google";
	return result;
}


bool
ParseGoogleClientJson(const std::string& text, std::string& id,
	std::string& secret)
{
	json j = json::parse(text, nullptr, false);
	if (!j.is_object())
		return false;
	const json* client = &j;
	if (j.contains("installed") && j["installed"].is_object())
		client = &j["installed"];
	else if (j.contains("web") && j["web"].is_object())
		return false;	// a Web client cannot use a loopback redirect
	if (!client->contains("client_id") || !(*client)["client_id"].is_string())
		return false;
	id = (*client)["client_id"].get<std::string>();
	secret = client->contains("client_secret")
		&& (*client)["client_secret"].is_string()
		? (*client)["client_secret"].get<std::string>() : std::string();
	return !id.empty();
}


static bool
ReadSmallFile(const std::string& path, std::string& out)
{
	FILE* file = fopen(path.c_str(), "rb");
	if (file == nullptr)
		return false;
	char buffer[16384];
	size_t n = fread(buffer, 1, sizeof(buffer), file);
	fclose(file);
	out.assign(buffer, n);
	return n > 0;
}


#ifndef AIRCAL_GOOGLE_CLIENT_ID
#define AIRCAL_GOOGLE_CLIENT_ID ""
#endif
#ifndef AIRCAL_GOOGLE_CLIENT_SECRET
#define AIRCAL_GOOGLE_CLIENT_SECRET ""
#endif


bool
FindGoogleClient(const std::string& settingsRoot, std::string& id,
	std::string& secret, std::string* source)
{
	const char* candidates[] = {"/airCal/google-client.json",
		"/Mail/google-client.json"};
	for (const char* candidate : candidates) {
		std::string text;
		if (ReadSmallFile(settingsRoot + candidate, text)
			&& ParseGoogleClientJson(text, id, secret)) {
			if (source != nullptr)
				*source = settingsRoot + candidate;
			return true;
		}
	}
	if (AIRCAL_GOOGLE_CLIENT_ID[0] != '\0') {
		id = AIRCAL_GOOGLE_CLIENT_ID;
		secret = AIRCAL_GOOGLE_CLIENT_SECRET;
		if (source != nullptr)
			*source = "built in";
		return true;
	}
	return false;
}


// --- Todoist -----------------------------------------------------------------------

static SignInResult
TodoistUser(Http& http, const Endpoints& endpoints, const std::string& token,
	const std::atomic<bool>& cancelled)
{
	HttpRequest request;
	request.url = endpoints.todoistApi + "/user";
	request.bearer = token;
	HttpResponse response = http.Perform(request, &cancelled);
	if (response.status == 401 || response.status == 403)
		throw SyncError("Todoist did not accept that token.", true);
	if (!response.Ok())
		ThrowHttpError("Todoist", response);
	json j = json::parse(response.body, nullptr, false);
	SignInResult result;
	if (j.is_object()) {
		if (j.contains("email") && j["email"].is_string())
			result.email = j["email"].get<std::string>();
		if (j.contains("full_name") && j["full_name"].is_string())
			result.name = j["full_name"].get<std::string>();
	}
	if (result.email.empty())
		result.email = "Todoist";
	return result;
}


SignInResult
TodoistSignIn(Http& http, const Endpoints& endpoints, OpenBrowser open,
	const std::atomic<bool>& cancelled)
{
	LoopbackListener listener;
	if (!listener.Start(0))
		throw SyncError("Could not open the local sign-in listener.");
	// Todoist accepts http redirect URIs only for localhost.
	std::string redirect = listener.RedirectUri("/todoist/callback", "localhost");

	HttpRequest registration;
	registration.method = "POST";
	registration.url = endpoints.todoistRegister;
	registration.contentType = "application/json";
	registration.body = json{{"client_name", "airCal for air/OS"},
		{"client_uri", "https://github.com/airos-works/airCal"},
		{"redirect_uris", json::array({redirect})},
		// RFC 7591 scopes are space separated (the authorize URL uses commas).
		{"scope", "data:read_write data:delete"},
		{"grant_types", json::array({"authorization_code", "refresh_token"})},
		{"response_types", json::array({"code"})},
		{"token_endpoint_auth_method", "none"}}.dump();
	HttpResponse registered = http.Perform(registration, &cancelled);
	json client = json::parse(registered.body, nullptr, false);
	if (!registered.Ok() || !client.is_object() || !client.contains("client_id")
		|| !client["client_id"].is_string()) {
		// A restricted scope is rejected; retry with the default scope
		// (data:read_write).
		json body = json::parse(registration.body);
		body.erase("scope");
		registration.body = body.dump();
		registered = http.Perform(registration, &cancelled);
		client = json::parse(registered.body, nullptr, false);
		if (!registered.Ok() || !client.is_object() || !client.contains("client_id")
			|| !client["client_id"].is_string())
			ThrowHttpError("Todoist registration", registered);
	}
	std::string clientId = client["client_id"].get<std::string>();
	std::string clientSecret = client.contains("client_secret")
		&& client["client_secret"].is_string()
		? client["client_secret"].get<std::string>() : std::string();

	Pkce pkce = Pkce::Make();
	std::string state = RandomToken();
	std::string url = endpoints.todoistAuth + "?" + FormEncode({
		{"client_id", clientId}, {"scope", "data:read_write,data:delete"},
		{"state", state}, {"redirect_uri", redirect},
		{"response_type", "code"}, {"code_challenge", pkce.challenge},
		{"code_challenge_method", "S256"}});
	open(url);
	std::map<std::string, std::string> reply = listener.Wait("/todoist/callback",
		600, cancelled, "airCal is signed in to Todoist");
	listener.Stop();
	if (cancelled)
		throw SyncError("Cancelled");
	if (reply.empty())
		throw SyncError("Todoist sign-in timed out.");
	if (reply.count("error") != 0)
		throw SyncError("Todoist sign-in was declined (" + reply["error"] + ").");
	if (reply["state"] != state || reply["code"].empty())
		throw SyncError("Todoist sign-in returned an unexpected reply.");

	std::vector<std::pair<std::string, std::string>> form = {
		{"grant_type", "authorization_code"}, {"code", reply["code"]},
		{"client_id", clientId}, {"redirect_uri", redirect},
		{"code_verifier", pkce.verifier}};
	if (!clientSecret.empty())
		form.push_back({"client_secret", clientSecret});
	TokenResponse token = RequestToken(http, endpoints.todoistToken, form,
		cancelled);

	SignInResult result = TodoistUser(http, endpoints, token.accessToken,
		cancelled);
	result.credentials.accessToken = token.accessToken;
	result.credentials.refreshToken = token.refreshToken;
	result.credentials.expires = token.expires;
	result.credentials.clientId = clientId;
	result.credentials.clientSecret = clientSecret;
	return result;
}


SignInResult
TodoistTokenSignIn(Http& http, const Endpoints& endpoints,
	const std::string& input, const std::atomic<bool>& cancelled)
{
	std::string token;
	for (char c : input) {
		if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
			token += c;
	}
	if (token.empty())
		throw SyncError("Paste your Todoist API token first.");
	SignInResult result = TodoistUser(http, endpoints, token, cancelled);
	result.credentials.apiToken = token;
	result.credentials.accessToken = token;
	return result;
}

}	// namespace aircal
