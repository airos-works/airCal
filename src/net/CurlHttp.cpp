#include "CurlHttp.h"

#include <curl/curl.h>

#include <algorithm>
#include <cstring>

namespace aircal {

namespace {

struct Buffer {
	std::string body;
	std::map<std::string, std::string> headers;
	size_t limit = 64 * 1024 * 1024;
	bool exceeded = false;
};


size_t
ReceiveBody(char* data, size_t size, size_t count, void* user)
{
	Buffer& buffer = *static_cast<Buffer*>(user);
	size_t bytes = size * count;
	if (buffer.body.size() + bytes > buffer.limit) {
		buffer.exceeded = true;
		return 0;
	}
	buffer.body.append(data, bytes);
	return bytes;
}


size_t
ReceiveHeader(char* data, size_t size, size_t count, void* user)
{
	Buffer& buffer = *static_cast<Buffer*>(user);
	size_t bytes = size * count;
	std::string line(data, bytes);
	if (line.compare(0, 5, "HTTP/") == 0) {
		// A new response (after a redirect or 100-continue): start over.
		buffer.headers.clear();
		return bytes;
	}
	size_t colon = line.find(':');
	if (colon == std::string::npos)
		return bytes;
	std::string name = line.substr(0, colon);
	std::transform(name.begin(), name.end(), name.begin(),
		[](unsigned char c) { return (char)tolower(c); });
	std::string value = line.substr(colon + 1);
	size_t begin = value.find_first_not_of(" \t");
	size_t end = value.find_last_not_of(" \t\r\n");
	value = begin == std::string::npos ? "" : value.substr(begin, end - begin + 1);
	buffer.headers[name] = value;
	return bytes;
}


int
Progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	const std::atomic<bool>* cancelled = static_cast<const std::atomic<bool>*>(user);
	return cancelled != nullptr && *cancelled ? 1 : 0;
}


CURL*
Handle()
{
	struct Cached {
		CURL* handle = nullptr;
		~Cached() { if (handle != nullptr) curl_easy_cleanup(handle); }
	};
	static thread_local Cached cached;
	if (cached.handle == nullptr)
		cached.handle = curl_easy_init();
	else
		curl_easy_reset(cached.handle);
	return cached.handle;
}

}	// namespace


CurlHttp::CurlHttp()
{
	static CURLcode initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
	(void)initialized;
}


HttpResponse
CurlHttp::Perform(const HttpRequest& request, const std::atomic<bool>* cancelled)
{
	HttpResponse response;
	if (cancelled != nullptr && *cancelled) {
		response.error = "cancelled";
		return response;
	}
	bool digest = false;
	for (int attempt = 0; attempt < 2; attempt++) {
		CURL* curl = Handle();
		if (curl == nullptr) {
			response.error = "could not start HTTP";
			return response;
		}
		curl_slist* headers = nullptr;
		headers = curl_slist_append(headers, "Expect:");
		if (!request.bearer.empty())
			headers = curl_slist_append(headers,
				("Authorization: Bearer " + request.bearer).c_str());
		if (!request.contentType.empty())
			headers = curl_slist_append(headers,
				("Content-Type: " + request.contentType).c_str());
		bool hasAccept = false;
		for (auto& header : request.headers) {
			if (strcasecmp(header.first.c_str(), "Accept") == 0)
				hasAccept = true;
			headers = curl_slist_append(headers,
				(header.first + ": " + header.second).c_str());
		}
		if (!hasAccept)
			headers = curl_slist_append(headers, "Accept: */*");

		Buffer buffer;
		curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "airCal/1.0 (air/OS)");
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, request.timeout);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
		curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
		curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
		curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,
			request.followRedirects ? 1L : 0L);
		curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
		// Keep PROPFIND/REPORT bodies across 301/302/303 redirects.
		curl_easy_setopt(curl, CURLOPT_POSTREDIR, (long)CURL_REDIR_POST_ALL);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &ReceiveBody);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &ReceiveHeader);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, &buffer);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &Progress);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancelled);
		if (!request.user.empty()) {
			curl_easy_setopt(curl, CURLOPT_USERNAME, request.user.c_str());
			curl_easy_setopt(curl, CURLOPT_PASSWORD, request.password.c_str());
			curl_easy_setopt(curl, CURLOPT_HTTPAUTH,
				digest ? CURLAUTH_DIGEST : CURLAUTH_BASIC);
		}
		if (request.method == "GET") {
			curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
		} else if (request.method == "HEAD") {
			curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
		} else {
			curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
			if (!request.body.empty() || request.method == "POST"
				|| request.method == "PUT" || request.method == "PATCH") {
				curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
				curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
					(curl_off_t)request.body.size());
			}
		}
		// Loopback test servers must not go through a proxy.
		if (request.url.find("://127.0.0.1") != std::string::npos
			|| request.url.find("://localhost") != std::string::npos
			|| request.url.find("://10.0.2.2") != std::string::npos)
			curl_easy_setopt(curl, CURLOPT_PROXY, "");

		CURLcode result = curl_easy_perform(curl);
		curl_slist_free_all(headers);
		long status = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
		char* effective = nullptr;
		curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
		response = HttpResponse();
		response.url = effective != nullptr ? effective : request.url;
		if (result != CURLE_OK) {
			response.status = 0;
			if (cancelled != nullptr && *cancelled)
				response.error = "cancelled";
			else if (buffer.exceeded)
				response.error = "the reply was too large";
			else
				response.error = curl_easy_strerror(result);
			return response;
		}
		response.status = status;
		response.body = std::move(buffer.body);
		response.headers = std::move(buffer.headers);
		// A server that only speaks Digest.
		if (status == 401 && !digest && !request.user.empty()
			&& response.Header("www-authenticate").find("Digest") != std::string::npos
			&& response.Header("www-authenticate").find("Basic") == std::string::npos) {
			digest = true;
			continue;
		}
		break;
	}
	return response;
}

}	// namespace aircal
