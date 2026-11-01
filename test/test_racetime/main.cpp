#include "../test_harness_device.h"
#include "../test_harness_native.h"

void runAllTests(void)
{
    extern void testRaceTime(void);

    testRaceTime();
}

TEST_HARNESS(runAllTests)
