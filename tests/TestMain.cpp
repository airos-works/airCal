#include "Test.h"
#include "core/DateTime.h"

#include <cstdlib>

int
main(int argc, char** argv)
{
	// Tests run in a fixed zone with DST so results do not depend on the host.
	aircal::SetLocalZone(getenv("AIRCAL_TEST_ZONE") != nullptr
		? getenv("AIRCAL_TEST_ZONE") : "America/New_York");
	return test::RunAll(argc > 1 ? argv[1] : nullptr);
}
