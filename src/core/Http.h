// The HTTP transport the providers use. The application uses libcurl
// (src/net/CurlHttp.cpp); tests script responses.
#pragma once

#include <atomic>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace aircal {

struct HttpRequest {
	std::string method = "GET";
	std::string url;
	std::vector<std::pair<std::string, std::string>> headers;
	std::string body;
	std::string contentType;
	std::string bearer;		// Authorization: Bearer
	std::string user;		// Basic/Digest authentication
	std::string password;
	bool followRedirects = false;
	long timeout = 60;		// seconds
};

struct HttpResponse {
	long status = 0;		// 0 when the request failed below HTTP
	std::string body;
	std::map<std::string, std::string> headers;	// names in lower case
	std::string error;		// transport error text
	std::string url;		// effective URL after redirects

	bool Ok() const { return status >= 200 && status < 300; }
	std::string Header(const std::string& lowerName) const
	{
		auto found = headers.find(lowerName);
		return found != headers.end() ? found->second : std::string();
	}
};

class Http {
public:
	virtual ~Http() {}
	virtual HttpResponse Perform(const HttpRequest& request,
		const std::atomic<bool>* cancelled = nullptr) = 0;
};

std::string UrlEncode(const std::string& text);
std::string UrlDecode(const std::string& text);
std::string FormEncode(const std::vector<std::pair<std::string, std::string>>& fields);
// Splits "a=1&b=2" into decoded pairs.
std::map<std::string, std::string> ParseQuery(const std::string& query);
// Resolves a possibly relative reference ("/dav/cal/", "x.ics") against a base.
std::string ResolveUrl(const std::string& base, const std::string& reference);
// "https://host:port" of a URL.
std::string UrlOrigin(const std::string& url);
std::string UrlPath(const std::string& url);

std::string Base64Encode(const std::string& data);
std::string Base64UrlEncode(const std::string& data);
std::string Sha256(const std::string& data);	// raw 32 bytes
std::string RandomBytes(size_t count);

}	// namespace aircal
