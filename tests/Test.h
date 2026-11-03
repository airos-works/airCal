// A tiny test harness: TEST(name) { CHECK(...); CHECK_EQ(a, b); }
#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace test {

struct Case {
	const char* name;
	std::function<void()> body;
};

inline std::vector<Case>& Cases()
{
	static std::vector<Case> cases;
	return cases;
}

inline int& Failures()
{
	static int failures = 0;
	return failures;
}

struct Register {
	Register(const char* name, std::function<void()> body)
	{
		Cases().push_back({name, body});
	}
};

template<typename T>
std::string Show(const T& value)
{
	std::ostringstream stream;
	stream << value;
	return stream.str();
}

inline int RunAll(const char* filter = nullptr)
{
	int run = 0;
	for (auto& c : Cases()) {
		if (filter != nullptr && std::string(c.name).find(filter) == std::string::npos)
			continue;
		int before = Failures();
		try {
			c.body();
		} catch (const std::exception& e) {
			std::cerr << c.name << ": exception: " << e.what() << "\n";
			Failures()++;
		}
		run++;
		if (Failures() != before)
			std::cerr << "FAILED " << c.name << "\n";
	}
	std::cerr << run << " tests, " << Failures() << " failures\n";
	return Failures() == 0 ? 0 : 1;
}

}	// namespace test

#define TEST_CONCAT2(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT2(a, b)
#define TEST(name) \
	static void name(); \
	static test::Register TEST_CONCAT(register_, name)(#name, name); \
	static void name()

#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK(" #condition ") failed\n"; \
			test::Failures()++; \
		} \
	} while (0)

#define CHECK_EQ(a, b) \
	do { \
		auto _a = (a); \
		auto _b = (b); \
		if (!(_a == _b)) { \
			std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK_EQ(" #a ", " #b \
				") failed: " << test::Show(_a) << " != " << test::Show(_b) << "\n"; \
			test::Failures()++; \
		} \
	} while (0)
