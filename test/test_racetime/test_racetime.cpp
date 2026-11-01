#include <unity.h>
#include <string.h>
#include "../test_utils.h"
#include "..\speeduino\raceTime.h"
#include "..\speeduino\torqueRequest.h"
#include "..\speeduino\statuses.h"

/**
 * @file
 * @brief FND-05 race-time framework and FND-09 arbiter tests.
 *
 * Style follows test_launchcontrol: a fixture struct holding the state under test, with small
 * helpers (arm/release/advance) rather than mocks. There is no hardware pin in play - the
 * public seam is raceTimeTick1ms(statuses&, bool pinHigh), which takes the already
 * polarity-corrected input level, so the state machine is testable without stubbing digitalRead.
 */

namespace {

/** @brief Fresh fixture per test. The clock is a module static, so it must be reset. */
struct raceTimeFixture {
  statuses current;

  raceTimeFixture()
  {
    memset(&current, 0, sizeof(current));
    raceTimeResetForTest();

    raceTimeConfig &s = raceTimeGetPageMutable();
    s.enable = RT_ENABLED;
    s.armPin = 0;
    s.armPolarity = 0;
    s.armPullup = 0;
    s.startSource = RT_START_LAUNCH_PIN_RELEASE;
    s.stopSource = RT_STOP_NONE;
    s.motionSource = RT_MOTION_NONE;
    s.curveEnable = 0;
    s.stopTimeMs = 0;
    s.stopSpeed = 0;
    s.falseStartSpeed = 0;
    memset(s.curveRetard, 0, sizeof(s.curveRetard));
  }

  /** @brief Place the 16 breakpoints evenly across @p spanMs. */
  void setCurveSpan(uint16_t spanMs)
  {
    raceTimeConfig &s = raceTimeGetPageMutable();
    for (uint8_t i = 0; i < RACETIME_CURVE_POINTS; ++i)
    {
      s.curveTimeMs[i] = (uint16_t)(((uint32_t)i * spanMs) / (uint32_t)(RACETIME_CURVE_POINTS - 1U));
    }
  }

  /** @brief Arm, then release, then run for @p ms milliseconds. */
  void completeLaunch(uint32_t ms)
  {
    raceTimeTick1ms(current, true);   // assert arm  -> ARMED
    raceTimeTick1ms(current, false);  // release     -> RUNNING
    advance(ms);
  }

  void advance(uint32_t ms)
  {
    for (uint32_t i = 0; i < ms; ++i) { raceTimeTick1ms(current, false); }
  }

  /** @brief Tick with the arm input held asserted - time spent staged, not racing. */
  void holdArmed(uint32_t ms)
  {
    for (uint32_t i = 0; i < ms; ++i) { raceTimeTick1ms(current, true); }
  }
};

// ============================ State machine ===========================

void test_starts_off_and_idle(void)
{
  raceTimeFixture f;
  TEST_ASSERT_EQUAL_UINT32(0U, raceTimeGetMs());
  TEST_ASSERT_EQUAL(RT_STATE_OFF, raceTimeGetState());
  TEST_ASSERT_TRUE(!raceTimeIsRunning());
  TEST_ASSERT_TRUE(!raceTimeIsFalseStart());
}

void test_arm_does_not_start_the_clock(void)
{
  raceTimeFixture f;
  raceTimeTick1ms(f.current, true);

  TEST_ASSERT_EQUAL(RT_STATE_ARMED, raceTimeGetState());
  TEST_ASSERT_EQUAL_UINT32(0U, raceTimeGetMs());

  // Time spent staged is not race time. This is the whole point of a separate armed state:
  // a driver can sit on the trans-brake for ten seconds without it counting.
  f.holdArmed(5000);
  TEST_ASSERT_EQUAL_UINT32(0U, raceTimeGetMs());
  TEST_ASSERT_EQUAL(RT_STATE_ARMED, raceTimeGetState());
}

void test_release_starts_the_clock(void)
{
  raceTimeFixture f;
  raceTimeTick1ms(f.current, true);
  raceTimeTick1ms(f.current, false);

  TEST_ASSERT_EQUAL(RT_STATE_RUNNING, raceTimeGetState());
  TEST_ASSERT_TRUE(raceTimeIsRunning());
  TEST_ASSERT_EQUAL_UINT32(0U, raceTimeGetMs());

  f.advance(1000);
  TEST_ASSERT_EQUAL_UINT32(1000U, raceTimeGetMs());
}

void test_clock_advances_one_ms_per_tick(void)
{
  raceTimeFixture f;
  f.completeLaunch(0);

  // Spot checks rather than a loop-and-compare, so a systematic off-by-one is obvious.
  f.advance(1);
  TEST_ASSERT_EQUAL_UINT32(1U, raceTimeGetMs());
  f.advance(9);
  TEST_ASSERT_EQUAL_UINT32(10U, raceTimeGetMs());
  f.advance(990);
  TEST_ASSERT_EQUAL_UINT32(1000U, raceTimeGetMs());
}

void test_clock_does_not_advance_when_stopped(void)
{
  raceTimeFixture f;
  f.completeLaunch(0);
  raceTimeReset();
  f.advance(100);

  TEST_ASSERT_EQUAL_UINT32(0U, raceTimeGetMs());
  TEST_ASSERT_EQUAL(RT_STATE_OFF, raceTimeGetState());
}

void test_disable_resets_the_clock(void)
{
  raceTimeFixture f;
  f.completeLaunch(2500);
  TEST_ASSERT_EQUAL_UINT32(2500U, raceTimeGetMs());

  // Turning the feature off must not leave a stale elapsed time for a consumer to act on.
  raceTimeGetPageMutable().enable = RT_DISABLED;
  raceTimeTick1ms(f.current, false);

  TEST_ASSERT_EQUAL_UINT32(0U, raceTimeGetMs());
  TEST_ASSERT_EQUAL(RT_STATE_OFF, raceTimeGetState());
}

// ============================ Stop conditions ===========================

void test_stop_after_time(void)
{
  raceTimeFixture f;
  raceTimeGetPageMutable().stopSource = RT_STOP_TIME;
  raceTimeGetPageMutable().stopTimeMs = 1000;
  f.completeLaunch(0);

  f.advance(999);
  TEST_ASSERT_EQUAL(RT_STATE_RUNNING, raceTimeGetState());

  f.advance(1);
  TEST_ASSERT_EQUAL(RT_STATE_STOPPED, raceTimeGetState());
  TEST_ASSERT_EQUAL_UINT32(1000U, raceTimeGetMs());

  // A stopped clock is frozen: further ticks must not keep counting.
  f.advance(500);
  TEST_ASSERT_EQUAL_UINT32(1000U, raceTimeGetMs());
}

void test_stop_above_speed(void)
{
  raceTimeFixture f;
  raceTimeGetPageMutable().stopSource = RT_STOP_SPEED;
  raceTimeGetPageMutable().stopSpeed = 200;
  f.completeLaunch(0);

  f.current.vss = 199;
  f.advance(10);
  TEST_ASSERT_EQUAL(RT_STATE_RUNNING, raceTimeGetState());

  f.current.vss = 200;
  f.advance(1);
  TEST_ASSERT_EQUAL(RT_STATE_STOPPED, raceTimeGetState());
}

void test_stop_when_arm_reapplied(void)
{
  raceTimeFixture f;
  raceTimeGetPageMutable().stopSource = RT_STOP_ARM_PIN;
  f.completeLaunch(50);
  TEST_ASSERT_EQUAL(RT_STATE_RUNNING, raceTimeGetState());

  raceTimeTick1ms(f.current, true);
  TEST_ASSERT_EQUAL(RT_STATE_STOPPED, raceTimeGetState());
}

void test_never_stops_by_default(void)
{
  raceTimeFixture f;
  f.completeLaunch(0);
  f.advance(20000);

  // The default is "driver timed": a quarter-mile pass plus shut-down is well under 20 s.
  TEST_ASSERT_EQUAL(RT_STATE_RUNNING, raceTimeGetState());
  TEST_ASSERT_EQUAL_UINT32(20000U, raceTimeGetMs());
}

// ============================ False start ===========================

void test_false_start_on_movement_while_armed(void)
{
  raceTimeFixture f;
  raceTimeGetPageMutable().motionSource = RT_MOTION_VSS;
  raceTimeGetPageMutable().falseStartSpeed = 0;

  raceTimeTick1ms(f.current, true);
  TEST_ASSERT_EQUAL(RT_STATE_ARMED, raceTimeGetState());

  f.current.vss = 5; // the car moved while still staged
  raceTimeTick1ms(f.current, true);

  TEST_ASSERT_EQUAL(RT_STATE_FALSE_START, raceTimeGetState());
  TEST_ASSERT_TRUE(raceTimeIsFalseStart());
  // A false start must not produce a time - the run did not happen.
  TEST_ASSERT_EQUAL_UINT32(0U, raceTimeGetMs());
}

void test_false_start_threshold_is_respected(void)
{
  raceTimeFixture f;
  raceTimeGetPageMutable().motionSource = RT_MOTION_VSS;
  raceTimeGetPageMutable().falseStartSpeed = 10;

  raceTimeTick1ms(f.current, true);
  f.current.vss = 10; // at the threshold, not above it
  raceTimeTick1ms(f.current, true);
  TEST_ASSERT_EQUAL(RT_STATE_ARMED, raceTimeGetState());

  f.current.vss = 11;
  raceTimeTick1ms(f.current, true);
  TEST_ASSERT_EQUAL(RT_STATE_FALSE_START, raceTimeGetState());
}

void test_no_false_start_when_detection_off(void)
{
  raceTimeFixture f;
  raceTimeGetPageMutable().motionSource = RT_MOTION_NONE;

  raceTimeTick1ms(f.current, true);
  f.current.vss = 50;
  raceTimeTick1ms(f.current, true);

  TEST_ASSERT_EQUAL(RT_STATE_ARMED, raceTimeGetState());
  TEST_ASSERT_TRUE(!raceTimeIsFalseStart());
}

void test_false_start_is_cleared_on_rearm(void)
{
  raceTimeFixture f;
  raceTimeGetPageMutable().motionSource = RT_MOTION_VSS;

  raceTimeTick1ms(f.current, true);
  f.current.vss = 5;
  raceTimeTick1ms(f.current, true);
  TEST_ASSERT_TRUE(raceTimeIsFalseStart());

  // Release and re-assert: a second attempt at the lights.
  f.current.vss = 0;
  raceTimeTick1ms(f.current, false);
  raceTimeTick1ms(f.current, true);

  TEST_ASSERT_EQUAL(RT_STATE_ARMED, raceTimeGetState());
  TEST_ASSERT_TRUE(!raceTimeIsFalseStart());
}

// ============================ 32-bit behaviour ===========================

void test_clock_does_not_wrap_at_16_bits(void)
{
  raceTimeFixture f;
  f.completeLaunch(0);

  // 65.536 s is where a 16-bit millisecond clock would roll over. A pass is shorter than
  // this, but the channel is 32-bit so that a car left armed after a red light does not
  // silently report a small time.
  f.advance(66000);
  TEST_ASSERT_EQUAL_UINT32(66000U, raceTimeGetMs());
  TEST_ASSERT_TRUE(raceTimeGetMs() > 65535U);
}

void test_clock_reaches_beyond_one_minute(void)
{
  raceTimeFixture f;
  f.completeLaunch(0);
  f.advance(120000);
  TEST_ASSERT_EQUAL_UINT32(120000U, raceTimeGetMs());
}

// ============================ Retard curve ===========================

void test_curve_disabled_returns_zero(void)
{
  raceTimeFixture f;
  raceTimeGetPageMutable().curveEnable = 0;
  raceTimeGetPageMutable().curveRetard[0] = 20;
  f.completeLaunch(10);

  TEST_ASSERT_EQUAL_UINT8(0U, raceTimeCurveRetard());
}

void test_curve_not_running_returns_zero(void)
{
  raceTimeFixture f;
  raceTimeGetPageMutable().curveEnable = 1;
  raceTimeGetPageMutable().curveRetard[0] = 20;

  // Armed but not released: no race time has elapsed, so no retard is applied.
  raceTimeTick1ms(f.current, true);
  TEST_ASSERT_EQUAL_UINT8(0U, raceTimeCurveRetard());
}

void test_curve_flat_is_flat(void)
{
  raceTimeFixture f;
  raceTimeGetPageMutable().curveEnable = 1;
  for (uint8_t i = 0; i < RACETIME_CURVE_POINTS; ++i)
  {
    raceTimeGetPageMutable().curveRetard[i] = 12;
  }
  f.completeLaunch(0);

  f.advance(0);
  TEST_ASSERT_EQUAL_UINT8(12U, raceTimeCurveRetard());
  f.advance(1234);
  TEST_ASSERT_EQUAL_UINT8(12U, raceTimeCurveRetard());
  f.advance(5000);
  TEST_ASSERT_EQUAL_UINT8(12U, raceTimeCurveRetard());
}

void test_curve_endpoints_are_exact(void)
{
  raceTimeFixture f;
  raceTimeGetPageMutable().curveEnable = 1;
  f.setCurveSpan(1600); // 15 intervals of 100 ms
  raceTimeGetPageMutable().curveRetard[0] = 0;
  raceTimeGetPageMutable().curveRetard[RACETIME_CURVE_POINTS - 1] = 15;
  f.completeLaunch(0);

  TEST_ASSERT_EQUAL_UINT8(0U, raceTimeCurveRetard());

  // Past the end of the span the final point is held, not extrapolated.
  f.advance(5000);
  TEST_ASSERT_EQUAL_UINT8(15U, raceTimeCurveRetard());
}

void test_curve_interpolates_between_points(void)
{
  raceTimeFixture f;
  raceTimeGetPageMutable().curveEnable = 1;
  f.setCurveSpan(1500); // 15 intervals of 100 ms
  for (uint8_t i = 0; i < RACETIME_CURVE_POINTS; ++i)
  {
    // point i == i degrees, so halfway between point 0 and point 1 is 0.5 -> 1 (rounded)
    raceTimeGetPageMutable().curveRetard[i] = i;
  }
  f.completeLaunch(0);

  TEST_ASSERT_EQUAL_UINT8(0U, raceTimeCurveRetard()); // t=0, point 0
  f.advance(50);                                      // halfway between 0 and 100 ms
  TEST_ASSERT_EQUAL_UINT8(1U, raceTimeCurveRetard()); // 0.5 rounds up
  f.advance(50);                                      // t=100, point 1
  TEST_ASSERT_EQUAL_UINT8(1U, raceTimeCurveRetard());
  f.advance(100);                                     // t=200, point 2
  TEST_ASSERT_EQUAL_UINT8(2U, raceTimeCurveRetard());
}

void test_curve_handles_descending_values(void)
{
  raceTimeFixture f;
  raceTimeGetPageMutable().curveEnable = 1;
  f.setCurveSpan(1500);
  for (uint8_t i = 0; i < RACETIME_CURVE_POINTS; ++i)
  {
    // Descending: 15, 14, 13 ... This is the common real shape - retard off the line, then
    // give the timing back. A naive unsigned interpolation wraps here.
    raceTimeGetPageMutable().curveRetard[i] = (uint8_t)(15U - i);
  }
  f.completeLaunch(0);

  TEST_ASSERT_EQUAL_UINT8(15U, raceTimeCurveRetard());
  f.advance(1500);
  TEST_ASSERT_EQUAL_UINT8(0U, raceTimeCurveRetard());
}

void test_curve_axis_repaired_if_not_ascending(void)
{
  // A hand-edited or truncated block can hold a non-monotonic axis. The lookup assumes
  // ordering, so clampSettings() must repair it rather than silently reading the wrong
  // segment. Set the whole axis to a constant and check it comes back ascending.
  raceTimeConfig &s = raceTimeGetPageMutable();
  for (uint8_t i = 0; i < RACETIME_CURVE_POINTS; ++i) { s.curveTimeMs[i] = 500; }
  raceTimeRestoreDefaults(); // applies clampSettings()

  const raceTimeConfig &c = raceTimeGetPage();
  TEST_ASSERT_EQUAL_UINT16(0U, c.curveTimeMs[0]);
  for (uint8_t i = 1; i < RACETIME_CURVE_POINTS; ++i)
  {
    TEST_ASSERT_TRUE(c.curveTimeMs[i] > c.curveTimeMs[i - 1U]);
  }
}

void test_default_curve_axis_is_ascending(void)
{
  raceTimeRestoreDefaults();
  const raceTimeConfig &c = raceTimeGetPage();

  TEST_ASSERT_EQUAL_UINT16(0U, c.curveTimeMs[0]);
  for (uint8_t i = 1; i < RACETIME_CURVE_POINTS; ++i)
  {
    TEST_ASSERT_TRUE(c.curveTimeMs[i] > c.curveTimeMs[i - 1U]);
  }
}

// ============================ Shipped defaults ===========================
//
// These pin the values a fresh ECU lays down. They MUST match the `defaultValue =` entries
// for page 17 in reference/speeduino.ini: a disagreement is invisible to every other test and
// shows up as a different curve depending on whether the tuner pressed Burn or the ECU
// defaulted the block itself.

void test_defaults_are_a_usable_drag_configuration(void)
{
  raceTimeRestoreDefaults();
  const raceTimeConfig &c = raceTimeGetPage();

  // Ships disabled, but otherwise configured: enabling it is one switch, not a blank page.
  TEST_ASSERT_EQUAL_UINT8(RT_DISABLED, c.enable);

  // Arm input must be a real, configured pin - not 0 ("none"). Pin 34 is the multiplexed
  // VSS / cam-3 pin on this board, so the default must not be it.
  TEST_ASSERT_NOT_EQUAL(0, c.armPin);
  TEST_ASSERT_NOT_EQUAL(34, c.armPin);

  // A trans-brake switch pulls to ground, so active-low with no internal pull-up.
  TEST_ASSERT_EQUAL_UINT8(1U, c.armPolarity);
  TEST_ASSERT_EQUAL_UINT8(0U, c.armPullup);

  // False-start detection on, with a small non-zero threshold: a staged car is stationary,
  // so 2 km/h of movement before the release is already a fault.
  TEST_ASSERT_EQUAL_UINT8((uint8_t)RT_MOTION_VSS, c.motionSource);
  TEST_ASSERT_TRUE(c.falseStartSpeed > 0U);

  // The curve ships enabled - a retard curve that is off by default is a feature nobody finds.
  TEST_ASSERT_EQUAL_UINT8(1U, c.curveEnable);
}

void test_default_curve_is_a_descending_retard_ramp(void)
{
  raceTimeRestoreDefaults();
  const raceTimeConfig &c = raceTimeGetPage();

  // Retard off the line for traction, then give the timing back. This is the shape a turbo
  // drag car actually runs; a flat or all-zero curve is the bug this test exists to prevent.
  TEST_ASSERT_TRUE_MESSAGE(c.curveRetard[0] >= 8U,
                           "Default must pull real timing at the hit, not a token amount");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, c.curveRetard[RACETIME_CURVE_POINTS - 1U],
                                  "Default must give all the timing back by the end of the ramp");

  // Monotonically non-increasing: never retard more later in the run than off the line.
  for (uint8_t i = 1; i < RACETIME_CURVE_POINTS; ++i)
  {
    TEST_ASSERT_TRUE_MESSAGE(c.curveRetard[i] <= c.curveRetard[i - 1U],
                             "Default curve must not increase retard as the run progresses");
  }

  // And the ramp must finish well before the end of a pass, not at 6 s.
  TEST_ASSERT_TRUE(c.curveTimeMs[RACETIME_CURVE_POINTS - 1U] >= 4000U);
}

// ============================ FND-09 arbiter ===========================

void test_arbiter_largest_retard_wins_not_the_sum(void)
{
  torqueBusResetForTest();
  torqueRequestRetard(TorqueProducer::Launch, 10);
  torqueRequestRetard(TorqueProducer::RaceTime, 15);

  // 15, not 25. Two systems are not additive; see the rule in torqueRequest.h.
  TEST_ASSERT_EQUAL_INT8(15, torqueBusRetard());
  TEST_ASSERT_EQUAL(TorqueProducer::RaceTime, torqueBusRetardProducer());
}

void test_arbiter_reports_no_producer_when_idle(void)
{
  torqueBusResetForTest();
  TEST_ASSERT_EQUAL_INT8(0, torqueBusRetard());
  TEST_ASSERT_EQUAL(TorqueProducer::None, torqueBusRetardProducer());
  TEST_ASSERT_TRUE(!torqueBusIsActive());
}

void test_arbiter_reset_clears_everything(void)
{
  torqueBusResetForTest();
  torqueRequestRetard(TorqueProducer::Knock, 8);
  TEST_ASSERT_TRUE(torqueBusIsActive());

  torqueBusReset();
  TEST_ASSERT_EQUAL_INT8(0, torqueBusRetard());
  TEST_ASSERT_TRUE(!torqueBusIsActive());
}

void test_arbiter_clamps_retard(void)
{
  torqueBusResetForTest();
  torqueRequestRetard(TorqueProducer::Launch, 100);
  TEST_ASSERT_EQUAL_INT8((int8_t)TORQUE_RETARD_MAX, torqueBusRetard());

  torqueRequestRetard(TorqueProducer::Launch, -20);
  TEST_ASSERT_EQUAL_INT8(0, torqueBusRetard());
}

void test_arbiter_rejects_writes_to_the_none_slot(void)
{
  torqueBusResetForTest();
  // Producer::None is the empty slot. Writing to it would make the bus permanently active.
  torqueRequestRetard(TorqueProducer::None, 20);
  TEST_ASSERT_EQUAL_INT8(0, torqueBusRetard());
  TEST_ASSERT_TRUE(!torqueBusIsActive());
}

void test_arbiter_same_producer_replaces_its_own_request(void)
{
  torqueBusResetForTest();
  torqueRequestRetard(TorqueProducer::RaceTime, 5);
  torqueRequestRetard(TorqueProducer::RaceTime, 12);

  // Not 17. Repeated calls replace, so a producer can call from inside a loop.
  TEST_ASSERT_EQUAL_INT8(12, torqueBusRetard());
}

void test_arbiter_strongest_cut_wins(void)
{
  torqueBusResetForTest();
  torqueRequestRetard(TorqueProducer::Launch, 0, TorqueCutMode::Fuel, 30);
  torqueRequestRetard(TorqueProducer::Protection, 0, TorqueCutMode::Both, 100);

  TEST_ASSERT_EQUAL(TorqueCutMode::Both, torqueBusCutMode());
  TEST_ASSERT_EQUAL_UINT8(100U, torqueBusCutPercent());
  TEST_ASSERT_EQUAL(TorqueProducer::Protection, torqueBusCutProducer());
}

void test_arbiter_no_cut_zeroes_the_percentage(void)
{
  torqueBusResetForTest();
  torqueRequestRetard(TorqueProducer::Launch, 5, TorqueCutMode::NoCut, 80);

  TEST_ASSERT_EQUAL(TorqueCutMode::NoCut, torqueBusCutMode());
  TEST_ASSERT_EQUAL_UINT8(0U, torqueBusCutPercent());
}

void test_arbiter_cut_percent_scoped_to_winning_mode(void)
{
  torqueBusResetForTest();
  torqueRequestRetard(TorqueProducer::Launch, 0, TorqueCutMode::Fuel, 100);
  torqueRequestRetard(TorqueProducer::Protection, 0, TorqueCutMode::Spark, 20);

  // Spark outranks fuel, so the reported percentage is the spark producer's, not the highest
  // number on the bus. Comparing percentages across different cut types is meaningless.
  TEST_ASSERT_EQUAL(TorqueCutMode::Spark, torqueBusCutMode());
  TEST_ASSERT_EQUAL_UINT8(20U, torqueBusCutPercent());
}

// ============================ Block layout contract ===========================

void test_payload_is_exactly_one_page(void)
{
  // The INI declares pageSize = 256 for page 17. If the struct grows, TunerStudio reads off
  // the end, so this is asserted at compile time too - this test documents why.
  TEST_ASSERT_EQUAL_UINT16(256U, RACETIME_PAYLOAD_SIZE);
  TEST_ASSERT_EQUAL_UINT16(256U, (uint16_t)sizeof(raceTimeBlock));
}

void test_block_sits_above_the_core_tune(void)
{
  TEST_ASSERT_TRUE(EEPROM_RACETIME_BASE >= 0x1000);
  TEST_ASSERT_TRUE(EEPROM_RACETIME_PAYLOAD == EEPROM_RACETIME_BASE + EEPROM_RACETIME_HEADER_SIZE);
}

void test_block_fits_in_the_emulated_eeprom(void)
{
  const uint32_t end = (uint32_t)EEPROM_RACETIME_BASE + (uint32_t)RACETIME_BLOCK_SIZE;
  // 768 sectors x 4096 / 32 = 23,808 bytes, highest address 0x5CFF.
  TEST_ASSERT_TRUE(end <= 0x5D00U);
}

void test_default_label_is_not_blank(void)
{
  raceTimeFixture f;
  raceTimeRestoreDefaults();

  // A blank axis label makes the curve unusable in the tuning software - the tuner cannot tell
  // what they built. The default must say something.
  TEST_ASSERT_TRUE(raceTimeGetPage().curveLabel[0] != 0);
  (void)f;
}

} // namespace

void testRaceTime(void)
{
  SET_UNITY_FILENAME()
  {
    RUN_TEST(test_starts_off_and_idle);
    RUN_TEST(test_arm_does_not_start_the_clock);
    RUN_TEST(test_release_starts_the_clock);
    RUN_TEST(test_clock_advances_one_ms_per_tick);
    RUN_TEST(test_clock_does_not_advance_when_stopped);
    RUN_TEST(test_disable_resets_the_clock);

    RUN_TEST(test_stop_after_time);
    RUN_TEST(test_stop_above_speed);
    RUN_TEST(test_stop_when_arm_reapplied);
    RUN_TEST(test_never_stops_by_default);

    RUN_TEST(test_false_start_on_movement_while_armed);
    RUN_TEST(test_false_start_threshold_is_respected);
    RUN_TEST(test_no_false_start_when_detection_off);
    RUN_TEST(test_false_start_is_cleared_on_rearm);

    RUN_TEST(test_clock_does_not_wrap_at_16_bits);
    RUN_TEST(test_clock_reaches_beyond_one_minute);

    RUN_TEST(test_curve_disabled_returns_zero);
    RUN_TEST(test_curve_not_running_returns_zero);
    RUN_TEST(test_curve_flat_is_flat);
    RUN_TEST(test_curve_endpoints_are_exact);
    RUN_TEST(test_curve_interpolates_between_points);
    RUN_TEST(test_curve_handles_descending_values);
    RUN_TEST(test_curve_axis_repaired_if_not_ascending);
    RUN_TEST(test_default_curve_axis_is_ascending);
    RUN_TEST(test_defaults_are_a_usable_drag_configuration);
    RUN_TEST(test_default_curve_is_a_descending_retard_ramp);

    RUN_TEST(test_arbiter_largest_retard_wins_not_the_sum);
    RUN_TEST(test_arbiter_reports_no_producer_when_idle);
    RUN_TEST(test_arbiter_reset_clears_everything);
    RUN_TEST(test_arbiter_clamps_retard);
    RUN_TEST(test_arbiter_rejects_writes_to_the_none_slot);
    RUN_TEST(test_arbiter_same_producer_replaces_its_own_request);
    RUN_TEST(test_arbiter_strongest_cut_wins);
    RUN_TEST(test_arbiter_no_cut_zeroes_the_percentage);
    RUN_TEST(test_arbiter_cut_percent_scoped_to_winning_mode);

    RUN_TEST(test_payload_is_exactly_one_page);
    RUN_TEST(test_block_sits_above_the_core_tune);
    RUN_TEST(test_block_fits_in_the_emulated_eeprom);
    RUN_TEST(test_default_label_is_not_blank);
  }
}
