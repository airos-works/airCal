// A minimal HTTP/1.1 client over plain sockets for the integration tests
// (http:// only; the app uses libcurl).
#pragma once

#include "core/Http.h"

namespace aircal {

class PlainHttp : public Http {
public:
	HttpResponse Perform(const HttpRequest& request,
		const std::atomic<bool>* cancelled) override;
	int requests = 0;
};

}	// namespace aircal
