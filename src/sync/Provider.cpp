#include "Provider.h"

#include <nlohmann/json.hpp>

#include <cstdlib>

using nlohmann::json;

namespace aircal {

std::unique_ptr<Provider> CreateGoogleProvider();
std::unique_ptr<Provider> CreateCalDavProvider();
std::unique_ptr<Provider> CreateTodoistProvider();


namespace {

class LocalProvider : public Provider {
public:
	void Sync(const std::string& accountId, SyncContext& context) override
	{
		// Nothing to talk to: local edits are final.
		std::lock_guard<std::recursive_mutex> lock(context.store.Mutex());
		for (Calendar* calendar : context.store.CalendarsOf(accountId)) {
			for (Event* event : context.store.EventsIn(calendar->id))
				event->pending = Pending::None;
			for (Task* task : context.store.TasksIn(calendar->id))
				task->pending = Pending::None;
		}
	}
};

}	// namespace


std::unique_ptr<Provider>
CreateProvider(AccountType type)
{
	switch (type) {
		case AccountType::Google:
			return CreateGoogleProvider();
		case AccountType::CalDAV:
			return CreateCalDavProvider();
		case AccountType::Todoist:
			return CreateTodoistProvider();
		case AccountType::Local:
			break;
	}
	return std::unique_ptr<Provider>(new LocalProvider);
}


bool
Credentials::Load(Secrets& secrets, const std::string& accountId,
	Credentials& out)
{
	std::string text;
	if (!secrets.Get("account:" + accountId, text))
		return false;
	json j = json::parse(text, nullptr, false);
	if (!j.is_object())
		return false;
	auto str = [&](const char* key) {
		auto it = j.find(key);
		return it != j.end() && it->is_string() ? it->get<std::string>()
			: std::string();
	};
	out.accessToken = str("access");
	out.refreshToken = str("refresh");
	auto expires = j.find("expires");
	out.expires = expires != j.end() && expires->is_number()
		? expires->get<int64_t>() : 0;
	out.clientId = str("clientId");
	out.clientSecret = str("clientSecret");
	out.password = str("password");
	out.apiToken = str("apiToken");
	return true;
}


bool
Credentials::Save(Secrets& secrets, const std::string& accountId) const
{
	json j{{"access", accessToken}, {"refresh", refreshToken},
		{"expires", expires}, {"clientId", clientId},
		{"clientSecret", clientSecret}, {"password", password},
		{"apiToken", apiToken}};
	return secrets.Set("account:" + accountId, j.dump());
}


std::string
Endpoints::TestBase()
{
	const char* base = getenv("AIRCAL_CLOUD_BASE");
	return base != nullptr ? base : "";
}


Endpoints
Endpoints::For(const std::string& requested)
{
	Endpoints e;
	std::string base = requested;
	while (!base.empty() && base.back() == '/')
		base.pop_back();
	if (base.empty()) {
		e.googleAuth = "https://accounts.google.com/o/oauth2/v2/auth";
		e.googleToken = "https://oauth2.googleapis.com/token";
		e.googleUserInfo = "https://openidconnect.googleapis.com/v1/userinfo";
		e.googleCalendar = "https://www.googleapis.com/calendar/v3";
		e.googleTasks = "https://tasks.googleapis.com/tasks/v1";
		e.todoistAuth = "https://app.todoist.com/oauth/authorize";
		e.todoistToken = "https://api.todoist.com/oauth/access_token";
		e.todoistRegister = "https://api.todoist.com/oauth/register";
		e.todoistApi = "https://api.todoist.com/api/v1";
	} else {
		e.googleAuth = base + "/google/o/oauth2/v2/auth";
		e.googleToken = base + "/google/token";
		e.googleUserInfo = base + "/google/openidconnect/v1/userinfo";
		e.googleCalendar = base + "/google/calendar/v3";
		e.googleTasks = base + "/google/tasks/v1";
		e.todoistAuth = base + "/todoist/oauth/authorize";
		e.todoistToken = base + "/todoist/oauth/access_token";
		e.todoistRegister = base + "/todoist/oauth/register";
		e.todoistApi = base + "/todoist/api/v1";
	}
	return e;
}


uint32_t
ParseHexColor(const std::string& input, uint32_t fallback)
{
	std::string text = input;
	if (!text.empty() && text[0] == '#')
		text = text.substr(1);
	if (text.size() < 6)
		return fallback;
	std::string digits = text.substr(0, 6);
	char* end = nullptr;
	unsigned long value = strtoul(digits.c_str(), &end, 16);
	if (end == nullptr || *end != '\0')
		return fallback;
	return (uint32_t)value;
}


std::string
FormatHexColor(uint32_t color)
{
	char buffer[16];
	snprintf(buffer, sizeof(buffer), "#%06X", color & 0xffffff);
	return buffer;
}


void
ThrowHttpError(const std::string& service, const HttpResponse& response)
{
	if (response.status == 0)
		throw SyncError(service + " could not be reached: " + response.error);
	if (response.status == 401)
		throw SyncError(service + " needs you to sign in again.", true);
	if (response.status == 403)
		throw SyncError(service + " denied access (HTTP 403).");
	if (response.status == 404)
		throw SyncError(service + " could not find the item (HTTP 404).");
	if (response.status == 429)
		throw SyncError(service + " asked to slow down. airCal will try again later.");
	// A short, token-free snippet of the error body helps.
	std::string detail;
	json j = json::parse(response.body, nullptr, false);
	if (j.is_object()) {
		auto error = j.find("error");
		if (error != j.end()) {
			if (error->is_string())
				detail = error->get<std::string>();
			else if (error->is_object() && error->contains("message")
				&& (*error)["message"].is_string())
				detail = (*error)["message"].get<std::string>();
		}
	}
	std::string message = service + " returned HTTP "
		+ std::to_string(response.status);
	if (!detail.empty())
		message += ": " + detail.substr(0, 160);
	throw SyncError(message);
}

}	// namespace aircal
