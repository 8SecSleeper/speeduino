#include "../test_harness_device.h"
#include "../test_harness_native.h"

void runAllTests(void)
{
    extern void testPpLaunch(void);

    testPpLaunch();
}

TEST_HARNESS(runAllTests)
