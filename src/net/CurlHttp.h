// libcurl transport (one easy handle per thread, connections reused).
#pragma once

#include "core/Http.h"

namespace aircal {

class CurlHttp : public Http {
public:
	CurlHttp();
	HttpResponse Perform(const HttpRequest& request,
		const std::atomic<bool>* cancelled) override;
};

}	// namespace aircal
