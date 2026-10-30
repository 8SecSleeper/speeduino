#include "../test_utils.h"
#include "globals.h"
#include "timers.h"
#include "src/controllers/vvt/vvtController.h"
#include "setup_oneMsInterval.h"

extern volatile byte loop5ms;
extern volatile byte loop10ms;
extern volatile byte loop20ms;
extern volatile byte loop33ms;
extern volatile byte loop50ms;
extern volatile byte loop66ms;
extern volatile byte loop100ms;
extern volatile byte loop250ms;
extern volatile int loopSec;
extern volatile uint16_t lastRPM_100ms;
extern volatile loop_timer_t TIMER_mask;

static void test_increments_counters_and_sets_1KHz(void)
{
  setup_oneMsInterval();
  oneMSInterval();
  TEST_ASSERT_TRUE(BIT_CHECK(TIMER_mask, BIT_TIMER_1KHZ));
  TEST_ASSERT_EQUAL_UINT8(1U, loop5ms);
  TEST_ASSERT_EQUAL_UINT8(1U, loop10ms);
  TEST_ASSERT_EQUAL_UINT8(1U, loop20ms);
  TEST_ASSERT_EQUAL_UINT8(1U, loop33ms);
  TEST_ASSERT_EQUAL_UINT8(1U, loop50ms);
  TEST_ASSERT_EQUAL_UINT8(1U, loop66ms);
  TEST_ASSERT_EQUAL_UINT8(1U, loop100ms);
  TEST_ASSERT_EQUAL_UINT8(1U, loop250ms);
  TEST_ASSERT_EQUAL_INT(1, loopSec);
}

static void test_5ms_boundary_sets_200Hz(void)
{
  setup_oneMsInterval();
  run_n_intervals(5);
  TEST_ASSERT_TRUE(BIT_CHECK(TIMER_mask, BIT_TIMER_200HZ));
  TEST_ASSERT_EQUAL_UINT8(0U, loop5ms);
  TEST_ASSERT_TRUE(BIT_CHECK( getAndClearTimerMask(), BIT_TIMER_200HZ));
  TEST_ASSERT_FALSE(BIT_CHECK(TIMER_mask, BIT_TIMER_200HZ));
}

static void test_20ms_boundary_sets_50Hz(void)
{
  setup_oneMsInterval();
  run_n_intervals(20);
  TEST_ASSERT_TRUE(BIT_CHECK(TIMER_mask, BIT_TIMER_50HZ));
  TEST_ASSERT_EQUAL_UINT8(0U, loop20ms);
  TEST_ASSERT_TRUE(BIT_CHECK( getAndClearTimerMask(), BIT_TIMER_50HZ));
  TEST_ASSERT_FALSE(BIT_CHECK(TIMER_mask, BIT_TIMER_50HZ));
}

static void test_10ms_boundary_sets_100Hz(void)
{
  setup_oneMsInterval();
  run_n_intervals(10);
  TEST_ASSERT_TRUE(BIT_CHECK(TIMER_mask, BIT_TIMER_100HZ));
  TEST_ASSERT_EQUAL_UINT8(0U, loop10ms);
  TEST_ASSERT_TRUE(BIT_CHECK( getAndClearTimerMask(), BIT_TIMER_100HZ));
  TEST_ASSERT_FALSE(BIT_CHECK(TIMER_mask, BIT_TIMER_100HZ));
}

/**
 * The 40Hz bit is derived from the 100Hz tick by a /3 divider (40Hz is not an integer divisor of
 * 1000ms). Verify it fires on every third 100Hz tick, and never in between.
 */
static void test_40Hz_is_a_third_of_the_100Hz_tick(void)
{
  setup_oneMsInterval();

  uint8_t firedCount = 0;
  uint8_t hundredHzCount = 0;
  const uint8_t subTicks = 30; // 30 x 10ms = 300ms => 10 expected 40Hz pulses

  for (uint8_t i = 0; i < subTicks; ++i)
  {
    run_n_intervals(10);
    loop_timer_t mask = getAndClearTimerMask();
    if (BIT_CHECK(mask, BIT_TIMER_100HZ)) { ++hundredHzCount; }
    if (BIT_CHECK(mask, BIT_TIMER_40HZ))
    {
      ++firedCount;
      // Every 40Hz pulse must coincide with a 100Hz tick - it is derived from one.
      TEST_ASSERT_TRUE(BIT_CHECK(mask, BIT_TIMER_100HZ));
      // ...and specifically every third one (1,2,3 -> fire on 3,6,9...).
      TEST_ASSERT_EQUAL_UINT8(0U, (uint8_t)(hundredHzCount % 3U));
    }
  }

  TEST_ASSERT_EQUAL_UINT8(subTicks, hundredHzCount);
  TEST_ASSERT_EQUAL_UINT8(subTicks / 3U, firedCount);
}

static void test_50ms_boundary_sets_20Hz(void)
{
  setup_oneMsInterval();
  run_n_intervals(50);
  TEST_ASSERT_TRUE(BIT_CHECK(TIMER_mask, BIT_TIMER_20HZ));
  TEST_ASSERT_EQUAL_UINT8(0U, loop50ms);
  TEST_ASSERT_TRUE(BIT_CHECK( getAndClearTimerMask(), BIT_TIMER_20HZ));
  TEST_ASSERT_FALSE(BIT_CHECK(TIMER_mask, BIT_TIMER_20HZ));
}

/**
 * Regression guard for the bug class described in roadmap §6B P3: a controller that nests one timer
 * bit inside another only fires on the passes where BOTH bits happen to be set in the same mask, and
 * LOOP_TIMER is cleared every iteration. Assert the bits the controllers now rely on are co-resident
 * at the rate each controller actually expects.
 */
static void test_slow_bits_coincide_with_unchanged_bits(void)
{
  setup_oneMsInterval();

  uint16_t hz100And200Together = 0;
  uint16_t hz40Count = 0;

  for (uint16_t i = 0; i < 1000U; ++i) // One full second
  {
    oneMSInterval();
    loop_timer_t mask = getAndClearTimerMask();
    if (BIT_CHECK(mask, BIT_TIMER_100HZ) && BIT_CHECK(mask, BIT_TIMER_200HZ)) { ++hz100And200Together; }
    if (BIT_CHECK(mask, BIT_TIMER_40HZ)) { ++hz40Count; }
  }

  // 10ms ticks and 5ms ticks align on every 10ms boundary: 100Hz is a multiple of 200Hz's period.
  TEST_ASSERT_EQUAL_UINT16(100U, hz100And200Together);
  // 40Hz over one second: the /3 divider yields 33 (not 40) in a 1s window, because 1000ms is not a
  // multiple of 25ms. It converges on 40Hz over a 1.2s window and never drifts.
  TEST_ASSERT_EQUAL_UINT16(33U, hz40Count);
}

static void test_33ms_boundary_sets_30Hz(void)
{
  setup_oneMsInterval();
  run_n_intervals(33);
  TEST_ASSERT_TRUE(BIT_CHECK(TIMER_mask, BIT_TIMER_30HZ));
  TEST_ASSERT_EQUAL_UINT8(0U, loop33ms);
  TEST_ASSERT_TRUE(BIT_CHECK( getAndClearTimerMask(), BIT_TIMER_30HZ));
  TEST_ASSERT_FALSE(BIT_CHECK(TIMER_mask, BIT_TIMER_30HZ));
}

static void test_66ms_boundary_sets_15Hz(void)
{
  setup_oneMsInterval();
  run_n_intervals(66);
  TEST_ASSERT_TRUE(BIT_CHECK(TIMER_mask, BIT_TIMER_15HZ));
  TEST_ASSERT_EQUAL_UINT8(0U, loop66ms);
  TEST_ASSERT_TRUE(BIT_CHECK( getAndClearTimerMask(), BIT_TIMER_15HZ));
  TEST_ASSERT_FALSE(BIT_CHECK(TIMER_mask, BIT_TIMER_15HZ));
}

static void test_100ms_boundary_updates_rpmDOT(void)
{
  setup_oneMsInterval();
  currentStatus.RPM = 1500U;
  lastRPM_100ms = 1000U;

  run_n_intervals(100);
  TEST_ASSERT_TRUE(BIT_CHECK(TIMER_mask, BIT_TIMER_10HZ));
  TEST_ASSERT_EQUAL_UINT8(0U, loop100ms);
  TEST_ASSERT_EQUAL_INT(5000, currentStatus.rpmDOT);  // (1500-1000)*10
  TEST_ASSERT_EQUAL_UINT16(1500U, lastRPM_100ms);
}

static void test_250ms_boundary_sets_4Hz(void)
{
  setup_oneMsInterval();
  run_n_intervals(250);
  TEST_ASSERT_TRUE(BIT_CHECK(TIMER_mask, BIT_TIMER_4HZ));
  TEST_ASSERT_EQUAL_UINT8(0U, loop250ms);
  TEST_ASSERT_TRUE(BIT_CHECK( getAndClearTimerMask(), BIT_TIMER_4HZ));
  TEST_ASSERT_FALSE(BIT_CHECK(TIMER_mask, BIT_TIMER_4HZ));
}

static void test_1Hz_boundary(void)
{
  setup_oneMsInterval();
  currentStatus.secl = 100U;

  run_n_intervals(1000);
  TEST_ASSERT_TRUE(BIT_CHECK(TIMER_mask, BIT_TIMER_1HZ));
  TEST_ASSERT_EQUAL_INT(0, loopSec);
  TEST_ASSERT_TRUE(BIT_CHECK( getAndClearTimerMask(), BIT_TIMER_1HZ));
  TEST_ASSERT_FALSE(BIT_CHECK(TIMER_mask, BIT_TIMER_1HZ));

  TEST_ASSERT_EQUAL_UINT8(101U, currentStatus.secl);
  // crankRPM = configPage4.crankRPM * 10
  TEST_ASSERT_EQUAL_UINT16(400U, currentStatus.crankRPM);
}

static void test_runSecs_increments_when_running(void)
{
  setup_oneMsInterval();
  currentStatus.rotationStatus = EngineRotationStatus::Running;
  currentStatus.runSecs = 10U;

  run_n_intervals(1000);
  TEST_ASSERT_EQUAL_UINT8(11U, currentStatus.runSecs);
}

static void test_runSecs_caps_at_255(void)
{
  setup_oneMsInterval();
  currentStatus.rotationStatus = EngineRotationStatus::Running;
  currentStatus.runSecs = 255U;

  run_n_intervals(1000);
  TEST_ASSERT_EQUAL_UINT8(255U, currentStatus.runSecs);
}

static void test_runSecsX10_running_at_10Hz(void)
{
  setup_oneMsInterval();
  currentStatus.rotationStatus = EngineRotationStatus::Running;

  run_n_intervals(100);
  TEST_ASSERT_EQUAL_UINT32(1UL, runSecsX10);

  run_n_intervals(100);
  TEST_ASSERT_EQUAL_UINT32(2UL, runSecsX10);
}

static void test_runSecsX10_resets_when_not_running(void)
{
  setup_oneMsInterval();
  currentStatus.rotationStatus = EngineRotationStatus::Running;
  run_n_intervals(100);
  TEST_ASSERT_EQUAL_UINT32(1UL, runSecsX10);

  currentStatus.rotationStatus = EngineRotationStatus::Stopped;
  run_n_intervals(100);
  TEST_ASSERT_EQUAL_UINT32(0UL, runSecsX10);
}

static void test_inj_priming_runs_when_rpm_zero(void)
{
  setup_oneMsInterval();
  currentStatus.injPrimed = false;
  currentStatus.initialisationComplete = true;
  currentStatus.RPM = 0U;
  currentStatus.injOutputs.primary = 0U;    // Skip actual fuel-schedule dispatch
  currentStatus.injOutputs.secondary = 0U;    // Skip actual fuel-schedule dispatch
  configPage2.primingDelay = 0U;        // seclx10 >= 0 always

  run_n_intervals(100);                 // Crosses 10Hz boundary, hits priming branch
  TEST_ASSERT_TRUE(currentStatus.injPrimed);
}

void testOneMsInterval(void)
{
  SET_UNITY_FILENAME()
  {
    RUN_TEST(test_increments_counters_and_sets_1KHz);
    RUN_TEST(test_5ms_boundary_sets_200Hz);
    RUN_TEST(test_10ms_boundary_sets_100Hz);
    RUN_TEST(test_20ms_boundary_sets_50Hz);
    RUN_TEST(test_33ms_boundary_sets_30Hz);
    RUN_TEST(test_40Hz_is_a_third_of_the_100Hz_tick);
    RUN_TEST(test_50ms_boundary_sets_20Hz);
    RUN_TEST(test_66ms_boundary_sets_15Hz);
    RUN_TEST(test_100ms_boundary_updates_rpmDOT);
    RUN_TEST(test_250ms_boundary_sets_4Hz);
    RUN_TEST(test_1Hz_boundary);
    RUN_TEST(test_slow_bits_coincide_with_unchanged_bits);
    RUN_TEST(test_runSecs_increments_when_running);
    RUN_TEST(test_runSecs_caps_at_255);
    RUN_TEST(test_runSecsX10_running_at_10Hz);
    RUN_TEST(test_runSecsX10_resets_when_not_running);
    RUN_TEST(test_inj_priming_runs_when_rpm_zero);
  }
}
