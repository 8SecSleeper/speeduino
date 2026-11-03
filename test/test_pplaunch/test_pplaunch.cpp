#include <unity.h>
#include <string.h>
#include "../test_utils.h"
#include "..\speeduino\ppLaunch.h"
#include "..\speeduino\boostLaunch.h"
#include "..\speeduino\torqueRequest.h"
#include "..\speeduino\statuses.h"

/**
 * @file
 * @brief Launch control (pages 18/19) and launch boost target (page 20) tests.
 *
 * Style follows test_racetime: a fixture struct holding the state under test, with small helpers
 * (arm/release/advance) rather than mocks. The module reads the arm input and the axis through
 * the UNIT_TEST injection points, so the state machine and the curve maths are testable without
 * stubbing digitalRead or the VSS ISR.
 *
 * The layout tests are the most important ones here. The page structs ARE the tuning-file ABI: if
 * an offset moves, TunerStudio reads the wrong field and nothing anywhere reports an error - the
 * tuner just gets a curve that is subtly wrong.
 */

namespace {

// The offsets declared in reference/speeduino.ini. If one of the static_asserts in ppLaunch.h or
// boostLaunch.h ever has to change, the ini MUST change with it - this table is the checklist.
constexpr uint16_t INI_SPEED_ENABLE = 0;
constexpr uint16_t INI_SPEED_AXIS_SOURCE = 4;
constexpr uint16_t INI_SPEED_TIMEOUT = 8;
constexpr uint16_t INI_SPEED_LABEL = 32;
constexpr uint16_t INI_SPEED_CURVE_TIME = 56;
constexpr uint16_t INI_SPEED_CURVE_AXIS = 120;
constexpr uint16_t INI_SPEED_CURVE_RETARD = 184;
constexpr uint16_t INI_SPEED_CURVE_LIMIT = 248;
constexpr uint16_t INI_SPEED_CURVE_TRIM = 312;
constexpr uint16_t INI_SPEED_SLIP_BIN = 376;
constexpr uint16_t INI_SPEED_SLIP_RETARD = 392;
constexpr uint16_t INI_SPEED_SLIP_TRIM = 408;

constexpr uint16_t INI_RPM_ENABLE = 0;
constexpr uint16_t INI_RPM_CUT_METHOD = 4;
constexpr uint16_t INI_RPM_TIMEOUT = 6;
constexpr uint16_t INI_RPM_SOFT_ZONE = 8;
constexpr uint16_t INI_RPM_SOFT_HYST = 10;
constexpr uint16_t INI_RPM_SOFT_RETARD = 14;
constexpr uint16_t INI_RPM_LABEL = 36;
constexpr uint16_t INI_RPM_CURVE_TIME = 56;
constexpr uint16_t INI_RPM_CURVE_LIMIT = 120;

constexpr uint16_t INI_BL_ENABLE = 0;
constexpr uint16_t INI_BL_LABEL = 8;
constexpr uint16_t INI_BL_CURVE_TIME = 32;
constexpr uint16_t INI_BL_CURVE_TARGET = 96;
constexpr uint16_t INI_BL_CURVE_TRIM = 160;

/** @brief Fresh fixture per test. */
struct Fixture
{
  statuses current;
  config2 page2;

  Fixture()
  {
    memset(&current, 0, sizeof(current));
    memset(&page2, 0, sizeof(page2));
    ppLaunchResetForTest();
    boostLaunchResetForTest();
    torqueBusResetForTest();

    // The speed module refuses to run a speed axis on a tune with no VSS input at all, because a
    // zero axis would hold the limiter and the retard on for the whole pass with no indication
    // why. Most tests want a configured car.
    page2.vssMode = VSS_MODE_EXTERNAL_KM;
    ppLaunchSetAxisForTest(0);

    // ---- speed page, enabled and simplified for deterministic maths --------------------
    ppLaunchSpeedBlock &s = ppLaunchGetSpeedPageMutable();
    s.enable = PP_ENABLED;
    s.armPin = 0;          // unbound: the arm level comes from the test injection point
    s.armPolarity = 0;     // asserted HIGH
    s.axisSource = PP_AXIS_VSS_MPH10;
    s.curveMode = PP_CURVE_TIME;
    s.limitMethod = PP_LIMIT_SPARK;
    s.featureFlags = (uint8_t)(PP_FLAG_PERFECT_PASS | PP_FLAG_SLIP_CORRECT | PP_FLAG_DUTY_TRIM);
    s.timeoutMs = 0;
    s.startBelowAxis = 0;
    s.rearmBelowAxis = 0;
    s.slipDeadband = 0;
    s.axisFilterShift = 0;   // raw axis: the tests want the number they injected
    s.axisAccelLimit = 0xFFFFU;
    s.retardSlewTenths = 0;  // no slew unless a test asks for it

    // ---- rpm page, disabled by default so the speed page is the active mode ------------
    ppLaunchGetRpmPageMutable().enable = PP_DISABLED;
  }

  /** @brief Drive the 1 kHz tick for a number of milliseconds. */
  void tick(uint32_t ms = 1U)
  {
    for (uint32_t i = 0U; i < ms; ++i) { ppLaunchTick1ms(current); }
  }

  /** @brief Arm the module and release, i.e. start a pass. */
  void launch(uint16_t axisStart = 0U)
  {
    ppLaunchSetAxisForTest(axisStart);
    ppLaunchSetArmForTest(true);
    tick(2U);
    ppLaunchSetArmForTest(false);
    tick(1U);            // release edge: the clock starts here
  }

  /** @brief Advance the clock and evaluate, with a fixed axis. */
  void run(uint32_t ms, uint16_t axis)
  {
    ppLaunchSetAxisForTest(axis);
    for (uint32_t i = 0U; i < ms; ++i)
    {
      ppLaunchTick1ms(current);
      ppLaunchUpdate(current, page2);
    }
  }

  /** @brief Advance the clock and evaluate with a fixed engine RPM (for the RPM mode). */
  void runRpm(uint32_t ms, uint16_t rpm)
  {
    current.RPM = rpm;
    for (uint32_t i = 0U; i < ms; ++i)
    {
      ppLaunchTick1ms(current);
      ppLaunchUpdate(current, page2);
      boostLaunchUpdate(current);
    }
  }
};

// =====================================================================================
// Layout contract - the tuning-file ABI
// =====================================================================================

void test_both_launch_pages_are_whole_tunerstudio_pages(void)
{
  TEST_ASSERT_EQUAL_UINT16(512, sizeof(ppLaunchSpeedConfig));
  TEST_ASSERT_EQUAL_UINT16(512, sizeof(ppLaunchRpmConfig));
  TEST_ASSERT_EQUAL_UINT16(512, PPLAUNCH_PAYLOAD_SIZE);
}

void test_boost_page_is_a_kilobyte_with_room_to_grow(void)
{
  // Deliberately twice a normal page: this is where the full boost control feature set goes.
  TEST_ASSERT_EQUAL_UINT16(1024, sizeof(boostLaunchConfig));
  TEST_ASSERT_EQUAL_UINT16(1024, BOOSTLAUNCH_PAYLOAD_SIZE);
  // At least half the payload must still be reserved, or the "room to grow" claim is a lie.
  const uint16_t used = offsetof(boostLaunchConfig, reserved03);
  TEST_ASSERT_TRUE_MESSAGE(used <= (BOOSTLAUNCH_PAYLOAD_SIZE / 2U),
                           "the boost page must keep at least half its payload reserved");
}

void test_speed_page_offsets_match_the_ini(void)
{
  TEST_ASSERT_EQUAL_UINT16(INI_SPEED_ENABLE, offsetof(ppLaunchSpeedConfig, enable));
  TEST_ASSERT_EQUAL_UINT16(INI_SPEED_AXIS_SOURCE, offsetof(ppLaunchSpeedConfig, axisSource));
  TEST_ASSERT_EQUAL_UINT16(INI_SPEED_TIMEOUT, offsetof(ppLaunchSpeedConfig, timeoutMs));
  TEST_ASSERT_EQUAL_UINT16(INI_SPEED_LABEL, offsetof(ppLaunchSpeedConfig, label));
  TEST_ASSERT_EQUAL_UINT16(INI_SPEED_CURVE_TIME, offsetof(ppLaunchSpeedConfig, curveTime));
  TEST_ASSERT_EQUAL_UINT16(INI_SPEED_CURVE_AXIS, offsetof(ppLaunchSpeedConfig, curveAxis));
  TEST_ASSERT_EQUAL_UINT16(INI_SPEED_CURVE_RETARD, offsetof(ppLaunchSpeedConfig, curveRetard));
  TEST_ASSERT_EQUAL_UINT16(INI_SPEED_CURVE_LIMIT, offsetof(ppLaunchSpeedConfig, curveLimit));
  TEST_ASSERT_EQUAL_UINT16(INI_SPEED_CURVE_TRIM, offsetof(ppLaunchSpeedConfig, curveBoostTrim));
  TEST_ASSERT_EQUAL_UINT16(INI_SPEED_SLIP_BIN, offsetof(ppLaunchSpeedConfig, slipBin));
  TEST_ASSERT_EQUAL_UINT16(INI_SPEED_SLIP_RETARD, offsetof(ppLaunchSpeedConfig, slipRetard));
  TEST_ASSERT_EQUAL_UINT16(INI_SPEED_SLIP_TRIM, offsetof(ppLaunchSpeedConfig, slipBoostTrim));
}

void test_rpm_page_offsets_match_the_ini(void)
{
  TEST_ASSERT_EQUAL_UINT16(INI_RPM_ENABLE, offsetof(ppLaunchRpmConfig, enable));
  TEST_ASSERT_EQUAL_UINT16(INI_RPM_CUT_METHOD, offsetof(ppLaunchRpmConfig, cutMethod));
  TEST_ASSERT_EQUAL_UINT16(INI_RPM_TIMEOUT, offsetof(ppLaunchRpmConfig, timeoutMs));
  TEST_ASSERT_EQUAL_UINT16(INI_RPM_SOFT_ZONE, offsetof(ppLaunchRpmConfig, softZoneRpm));
  TEST_ASSERT_EQUAL_UINT16(INI_RPM_SOFT_HYST, offsetof(ppLaunchRpmConfig, softHystRpm));
  TEST_ASSERT_EQUAL_UINT16(INI_RPM_SOFT_RETARD, offsetof(ppLaunchRpmConfig, softRetardTenths));
  TEST_ASSERT_EQUAL_UINT16(INI_RPM_LABEL, offsetof(ppLaunchRpmConfig, label));
  TEST_ASSERT_EQUAL_UINT16(INI_RPM_CURVE_TIME, offsetof(ppLaunchRpmConfig, curveTime));
  TEST_ASSERT_EQUAL_UINT16(INI_RPM_CURVE_LIMIT, offsetof(ppLaunchRpmConfig, curveLimit));
}

void test_boost_page_offsets_match_the_ini(void)
{
  TEST_ASSERT_EQUAL_UINT16(INI_BL_ENABLE, offsetof(boostLaunchConfig, enable));
  TEST_ASSERT_EQUAL_UINT16(INI_BL_LABEL, offsetof(boostLaunchConfig, label));
  TEST_ASSERT_EQUAL_UINT16(INI_BL_CURVE_TIME, offsetof(boostLaunchConfig, curveTime));
  TEST_ASSERT_EQUAL_UINT16(INI_BL_CURVE_TARGET, offsetof(boostLaunchConfig, curveTarget));
  TEST_ASSERT_EQUAL_UINT16(INI_BL_CURVE_TRIM, offsetof(boostLaunchConfig, curveDutyTrim));
}

void test_the_rpm_page_is_simple_on_purpose(void)
{
  // It must NOT have grown a reference wire, a slip curve or a boost table - that would be the
  // "full secondary setup" this page exists to avoid needing.
  TEST_ASSERT_EQUAL_UINT16(512, sizeof(ppLaunchRpmConfig));
  // The only curves are time and limit, plus a big reserved tail.
  TEST_ASSERT_EQUAL_UINT16(offsetof(ppLaunchRpmConfig, curveLimit),
                           offsetof(ppLaunchRpmConfig, curveTime) + (2U * PP_RPM_CURVE_POINTS));
  TEST_ASSERT_TRUE(offsetof(ppLaunchRpmConfig, reserved3) <= 200U);
}

void test_blocks_do_not_overlap(void)
{
  // 0x1000..0x181F LTT, 0x1820..0x192F race time, 0x1930..0x1B3F speed, 0x1B40..0x1C4F rpm,
  // 0x1D50..0x215F launch boost. A block that overlaps another silently corrupts a tune.
  TEST_ASSERT_EQUAL_UINT16(EEPROM_PPLAUNCH_SPEED_BASE, 0x1930);
  TEST_ASSERT_EQUAL_UINT16(EEPROM_PPLAUNCH_RPM_BASE,
                           EEPROM_PPLAUNCH_SPEED_BASE + PPLAUNCH_BLOCK_SIZE);
  TEST_ASSERT_TRUE(EEPROM_BOOSTLAUNCH_BASE >=
                   (EEPROM_PPLAUNCH_RPM_BASE + PPLAUNCH_BLOCK_SIZE));
  TEST_ASSERT_TRUE((uint32_t)EEPROM_BOOSTLAUNCH_BASE + BOOSTLAUNCH_BLOCK_SIZE <= 0x5D00UL);
}

void test_each_block_has_a_distinct_magic(void)
{
  TEST_ASSERT_NOT_EQUAL(EEPROM_PPLAUNCH_SPEED_MAGIC, EEPROM_PPLAUNCH_RPM_MAGIC);
  TEST_ASSERT_NOT_EQUAL(EEPROM_PPLAUNCH_SPEED_MAGIC, EEPROM_BOOSTLAUNCH_MAGIC);
  TEST_ASSERT_NOT_EQUAL(EEPROM_PPLAUNCH_RPM_MAGIC, EEPROM_BOOSTLAUNCH_MAGIC);
}

// =====================================================================================
// Defaults
// =====================================================================================

void test_every_mode_ships_disabled_but_configured(void)
{
  ppLaunchResetForTest();
  boostLaunchResetForTest();

  TEST_ASSERT_EQUAL_UINT8(PP_DISABLED, ppLaunchGetSpeedPage().enable);
  TEST_ASSERT_EQUAL_UINT8(PP_DISABLED, ppLaunchGetRpmPage().enable);
  // Boost ships disabled: substituting the boost target on a car that never asked for it is the
  // worst default available, and boost is what puts rods through blocks.
  TEST_ASSERT_EQUAL_UINT8(BL_DISABLED, boostLaunchGetPage().enable);

  TEST_ASSERT_EQUAL_STRING("speed", (const char *)ppLaunchGetSpeedPage().label);
  TEST_ASSERT_EQUAL_STRING("rpm", (const char *)ppLaunchGetRpmPage().label);
}

void test_time_axes_ship_strictly_ascending(void)
{
  ppLaunchResetForTest();
  boostLaunchResetForTest();

  const ppLaunchSpeedBlock &s = ppLaunchGetSpeedPage();
  const ppLaunchRpmBlock &r = ppLaunchGetRpmPage();
  const boostLaunchBlock &b = boostLaunchGetPage();

  TEST_ASSERT_EQUAL_UINT16(0, s.curveTime[0]);
  TEST_ASSERT_EQUAL_UINT16(0, r.curveTime[0]);
  TEST_ASSERT_EQUAL_UINT16(0, b.curveTime[0]);

  for (uint8_t i = 1U; i < PP_CURVE_POINTS; ++i)
  {
    TEST_ASSERT_GREATER_THAN_UINT16(s.curveTime[i - 1U], s.curveTime[i]);
    TEST_ASSERT_GREATER_THAN_UINT16(r.curveTime[i - 1U], r.curveTime[i]);
  }
  for (uint8_t i = 1U; i < BL_CURVE_POINTS; ++i)
  {
    TEST_ASSERT_GREATER_THAN_UINT16(b.curveTime[i - 1U], b.curveTime[i]);
  }
}

void test_rpm_defaults_ramp_rather_than_sit_flat(void)
{
  // A flat default is what you get when nobody thought about it. The simple limiter's default
  // should still show a tuner what a launch ramp looks like.
  ppLaunchResetForTest();
  const ppLaunchRpmBlock &r = ppLaunchGetRpmPage();
  TEST_ASSERT_TRUE(r.curveLimit[0] < r.curveLimit[PP_RPM_CURVE_POINTS - 1U]);
  // ...and the soft retard should ship enabled, or the feature's most useful part is invisible.
  TEST_ASSERT_GREATER_THAN_UINT16(0U, r.softZoneRpm);
  TEST_ASSERT_GREATER_THAN_UINT16(0U, r.softRetardTenths);
}

// =====================================================================================
// Speed mode: state machine
// =====================================================================================

void test_arming_does_not_start_the_clock_or_apply_anything(void)
{
  Fixture f;
  f.tick(5U);
  TEST_ASSERT_EQUAL_UINT8(PP_STATE_IDLE, ppLaunchState());

  ppLaunchSetArmForTest(true);
  f.tick(20U);
  TEST_ASSERT_EQUAL_UINT8(PP_STATE_ARMED, ppLaunchState());
  TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, ppLaunchRpmLimit());
  TEST_ASSERT_EQUAL_UINT8(0, ppLaunchRetard());
}

void test_release_edge_starts_the_clock(void)
{
  Fixture f;
  ppLaunchSetArmForTest(true);
  f.tick(50U);
  TEST_ASSERT_EQUAL_UINT8(PP_STATE_ARMED, ppLaunchState());

  ppLaunchSetArmForTest(false);
  f.tick(1U);
  TEST_ASSERT_EQUAL_UINT8(PP_STATE_LAUNCHING, ppLaunchState());
}

void test_armed_timeout_prevents_an_indefinitely_armed_car(void)
{
  Fixture f;
  ppLaunchGetSpeedPageMutable().timeoutMs = 100U;

  ppLaunchSetArmForTest(true);
  f.tick(99U);
  TEST_ASSERT_EQUAL_UINT8(PP_STATE_ARMED, ppLaunchState());
  f.tick(2U);
  TEST_ASSERT_EQUAL_UINT8(PP_STATE_TIMEOUT, ppLaunchState());
  TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, ppLaunchRpmLimit());
}

void test_disabling_mid_pass_drops_every_output(void)
{
  Fixture f;
  f.launch();
  f.run(100U, 0U);
  TEST_ASSERT_EQUAL_UINT8(PP_STATE_LAUNCHING, ppLaunchState());

  ppLaunchGetSpeedPageMutable().enable = PP_DISABLED;
  f.run(1U, 0U);
  TEST_ASSERT_EQUAL_UINT8(PP_STATE_OFF, ppLaunchState());
  TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, ppLaunchRpmLimit());
  TEST_ASSERT_EQUAL_UINT8(0, ppLaunchRetard());
}

void test_the_pass_completes_at_the_end_of_the_curve_and_latches(void)
{
  Fixture f;
  const uint16_t lastTime = ppLaunchGetSpeedPage().curveTime[PP_CURVE_POINTS - 1U];

  f.launch();
  f.run((uint32_t)lastTime - 2U, 0U);
  TEST_ASSERT_EQUAL_UINT8(PP_STATE_LAUNCHING, ppLaunchState());

  f.run(5U, 0U);
  TEST_ASSERT_EQUAL_UINT8(PP_STATE_COMPLETE, ppLaunchState());
  TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, ppLaunchRpmLimit());

  // COMPLETE must LATCH: the driver released the arm input to launch, so falling back to IDLE on
  // the next millisecond would destroy the only record that the run finished.
  f.run(100U, 0U);
  TEST_ASSERT_EQUAL_UINT8(PP_STATE_COMPLETE, ppLaunchState());
}

void test_rearm_below_axis_returns_to_idle(void)
{
  Fixture f;
  ppLaunchGetSpeedPageMutable().rearmBelowAxis = 100U;

  f.launch(0U);
  TEST_ASSERT_EQUAL_UINT8(PP_STATE_LAUNCHING, ppLaunchState());

  f.run(50U, 500U);
  TEST_ASSERT_EQUAL_UINT8(PP_STATE_LAUNCHING, ppLaunchState());

  f.run(5U, 50U);   // coasted back to stage
  TEST_ASSERT_EQUAL_UINT8(PP_STATE_IDLE, ppLaunchState());
}

// =====================================================================================
// Speed mode: retard, limit, trim
// =====================================================================================

void test_retard_follows_the_curve_and_goes_through_the_torque_bus(void)
{
  Fixture f;
  for (uint8_t i = 0U; i < PP_CURVE_POINTS; ++i) { ppLaunchGetSpeedPageMutable().curveRetard[i] = 100U; }

  f.launch();
  f.run(100U, 0U);

  TEST_ASSERT_EQUAL_UINT8(10, ppLaunchRetard());
  // Routing through the bus is what stops launch retard summing with the race-time curve.
  TEST_ASSERT_EQUAL_INT8(10, torqueBusRetard());
  TEST_ASSERT_EQUAL_UINT8((uint8_t)TorqueProducer::Launch, (uint8_t)torqueBusRetardProducer());
}

void test_retard_rounds_to_nearest_and_honours_large_values(void)
{
  Fixture f;
  // 45.6 degrees: past any byte-era limit, and it must round up rather than truncate.
  for (uint8_t i = 0U; i < PP_CURVE_POINTS; ++i) { ppLaunchGetSpeedPageMutable().curveRetard[i] = 456U; }

  f.launch();
  f.run(100U, 0U);
  TEST_ASSERT_EQUAL_UINT8(46, ppLaunchRetard());
}

void test_a_descending_retard_curve_does_not_wrap(void)
{
  Fixture f;
  ppLaunchSpeedBlock &s = ppLaunchGetSpeedPageMutable();
  for (uint8_t i = 0U; i < PP_CURVE_POINTS; ++i) { s.curveRetard[i] = 0U; }
  s.curveRetard[0] = 200U;   // 20.0 at t=0
  s.curveRetard[1] = 0U;     // 0.0 at t=100

  f.launch();
  f.run(50U, 0U);
  TEST_ASSERT_EQUAL_UINT8(10, ppLaunchRetard());   // halfway down, not clamped to 20
}

void test_retard_slew_limits_the_rate_in_both_directions(void)
{
  Fixture f;
  ppLaunchSpeedBlock &s = ppLaunchGetSpeedPageMutable();
  s.featureFlags |= PP_FLAG_RETARD_SLEW;
  s.retardSlewTenths = 1000U;   // 1000 tenths/s = 100 deg/s

  for (uint8_t i = 0U; i < PP_CURVE_POINTS; ++i)
  {
    s.curveRetard[i] = (i <= 10U) ? 200U : 0U;   // 20 deg held to t=1000, then released
  }

  f.launch();
  f.run(50U, 0U);
  const uint8_t climbing = ppLaunchRetard();
  TEST_ASSERT_TRUE_MESSAGE(climbing > 0U, "slew must move toward the target");
  TEST_ASSERT_TRUE_MESSAGE(climbing < 20U, "slew must not arrive at 20 deg in 50 ms");

  f.run(500U, 0U);
  TEST_ASSERT_EQUAL_UINT8(20, ppLaunchRetard());

  f.run(600U, 0U);   // past t=1000, the curve asks for zero
  const uint8_t falling = ppLaunchRetard();
  TEST_ASSERT_TRUE_MESSAGE(falling < 20U, "retard must have started falling");
  TEST_ASSERT_TRUE_MESSAGE(falling > 0U, "retard must not snap to zero in one step");

  f.run(1000U, 0U);
  TEST_ASSERT_EQUAL_UINT8(0, ppLaunchRetard());
}

void test_limit_follows_the_curve_and_uses_the_configured_method(void)
{
  Fixture f;
  for (uint8_t i = 0U; i < PP_CURVE_POINTS; ++i) { ppLaunchGetSpeedPageMutable().curveLimit[i] = 5500U; }

  f.launch();
  f.run(100U, 0U);

  TEST_ASSERT_EQUAL_UINT16(5500, ppLaunchRpmLimit());
  TEST_ASSERT_EQUAL_UINT8(PP_LIMIT_SPARK, ppLaunchLimitMethod());
  TEST_ASSERT_TRUE(ppLaunchIsLimiting());
}

void test_limit_method_none_disables_the_limiter_but_keeps_retard(void)
{
  Fixture f;
  ppLaunchSpeedBlock &s = ppLaunchGetSpeedPageMutable();
  s.limitMethod = PP_LIMIT_NONE;
  for (uint8_t i = 0U; i < PP_CURVE_POINTS; ++i)
  {
    s.curveLimit[i] = 5000U;
    s.curveRetard[i] = 100U;
  }

  f.launch();
  f.run(100U, 0U);

  TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, ppLaunchRpmLimit());
  TEST_ASSERT_EQUAL_UINT8(10, ppLaunchRetard());
}

void test_start_gate_blocks_the_limiter_above_its_speed(void)
{
  Fixture f;
  ppLaunchSpeedBlock &s = ppLaunchGetSpeedPageMutable();
  s.startBelowAxis = 100U;   // 10.0 mph
  for (uint8_t i = 0U; i < PP_CURVE_POINTS; ++i) { s.curveLimit[i] = 5000U; }

  f.launch(0U);
  f.run(50U, 50U);
  TEST_ASSERT_TRUE(ppLaunchIsLimiting());

  f.run(50U, 500U);   // the car has left
  TEST_ASSERT_FALSE(ppLaunchIsLimiting());
}

void test_speed_mode_duty_trim_is_gated_on_its_flag(void)
{
  Fixture f;
  ppLaunchSpeedBlock &s = ppLaunchGetSpeedPageMutable();
  for (uint8_t i = 0U; i < PP_CURVE_POINTS; ++i) { s.curveBoostTrim[i] = 30U; }

  // Flag on: the trim is published.
  s.featureFlags |= PP_FLAG_DUTY_TRIM;
  f.launch();
  f.run(100U, 0U);
  TEST_ASSERT_EQUAL_UINT8(30, ppLaunchBoostTrim());

  // Flag off: the curve is inert.
  s.featureFlags = (uint8_t)(s.featureFlags & (uint8_t)(~PP_FLAG_DUTY_TRIM));
  f.run(100U, 0U);
  TEST_ASSERT_EQUAL_UINT8(0, ppLaunchBoostTrim());
}

// =====================================================================================
// Speed mode: perfect-pass scoring
// =====================================================================================

void test_slip_is_signed_and_only_positive_slip_is_corrected(void)
{
  Fixture f;
  ppLaunchSpeedBlock &s = ppLaunchGetSpeedPageMutable();
  for (uint8_t i = 0U; i < PP_CURVE_POINTS; ++i)
  {
    s.curveAxis[i] = 200U;   // wire says 20.0 mph
    s.curveRetard[i] = 0U;
    s.curveBoostTrim[i] = 0U;
  }
  // 1 degree of retard and 10% trim per 10 units of slip.
  s.slipBin[0] = 0U;  s.slipRetard[0] = 0U;  s.slipBoostTrim[0] = 0U;
  s.slipBin[1] = 10U; s.slipRetard[1] = 10U; s.slipBoostTrim[1] = 10U;
  for (uint8_t i = 2U; i < PP_SLIP_POINTS; ++i)
  {
    s.slipBin[i] = (uint16_t)(s.slipBin[i - 1U] + 100U);
    s.slipRetard[i] = s.slipRetard[i - 1U];
    s.slipBoostTrim[i] = s.slipBoostTrim[i - 1U];
  }

  // Ahead of the wire = wheelspin: correction applies.
  f.launch(0U);
  f.run(100U, 210U);
  TEST_ASSERT_EQUAL_INT16(10, ppLaunchSlip());
  TEST_ASSERT_EQUAL_UINT8(1, ppLaunchRetard());
  TEST_ASSERT_EQUAL_UINT8(10, ppLaunchBoostTrim());

  // Behind the wire = a bog. Pulling timing from a car that is already slow makes it slower, so
  // the correction must NOT apply. That asymmetry is the difference between a correction and a
  // mistake.
  Fixture f2;
  ppLaunchSpeedBlock &s2 = ppLaunchGetSpeedPageMutable();
  for (uint8_t i = 0U; i < PP_CURVE_POINTS; ++i) { s2.curveAxis[i] = 200U; s2.curveRetard[i] = 0U; }
  s2.slipBin[1] = 10U; s2.slipRetard[1] = 100U;
  f2.launch(0U);
  f2.run(100U, 190U);
  TEST_ASSERT_EQUAL_INT16(-10, ppLaunchSlip());
  TEST_ASSERT_EQUAL_UINT8(0, ppLaunchRetard());
}

// =====================================================================================
// RPM mode: the simple limiter
// =====================================================================================

/** @brief Configure and enable the RPM page instead of the speed page. */
void enableRpmMode(Fixture &f)
{
  ppLaunchGetSpeedPageMutable().enable = PP_DISABLED;
  ppLaunchRpmBlock &r = ppLaunchGetRpmPageMutable();
  r.enable = PP_ENABLED;
  r.armPin = 0;
  r.armPolarity = 0;
  r.timeoutMs = 0;
  r.cutMethod = PP_LIMIT_SPARK;
  (void)f;
}

void test_rpm_mode_follows_the_limit_curve_versus_time(void)
{
  Fixture f;
  enableRpmMode(f);
  ppLaunchRpmBlock &r = ppLaunchGetRpmPageMutable();
  for (uint8_t i = 0U; i < PP_RPM_CURVE_POINTS; ++i) { r.curveLimit[i] = 5000U; }

  f.launch(0U);
  f.runRpm(100U, 3000U);

  TEST_ASSERT_EQUAL_UINT8(PP_MODE_RPM, (uint8_t)ppLaunchActiveMode());
  TEST_ASSERT_EQUAL_UINT16(5000, ppLaunchRpmLimit());
  TEST_ASSERT_EQUAL_UINT8(PP_LIMIT_SPARK, (uint8_t)ppLaunchLimitMethod());
}

void test_rpm_mode_limit_ramps_with_time(void)
{
  Fixture f;
  enableRpmMode(f);
  ppLaunchRpmBlock &r = ppLaunchGetRpmPageMutable();
  r.curveLimit[0] = 4000U;
  r.curveLimit[1] = 5000U;   // t=100
  for (uint8_t i = 2U; i < PP_RPM_CURVE_POINTS; ++i) { r.curveLimit[i] = 5000U; }

  f.launch(0U);
  f.runRpm(50U, 3000U);
  TEST_ASSERT_EQUAL_UINT16(4500, ppLaunchRpmLimit());   // halfway up the ramp
  f.runRpm(100U, 3000U);
  TEST_ASSERT_EQUAL_UINT16(5000, ppLaunchRpmLimit());
}

void test_rpm_mode_soft_retard_engages_within_the_zone(void)
{
  Fixture f;
  enableRpmMode(f);
  ppLaunchRpmBlock &r = ppLaunchGetRpmPageMutable();
  for (uint8_t i = 0U; i < PP_RPM_CURVE_POINTS; ++i) { r.curveLimit[i] = 6000U; }
  r.softZoneRpm = 300U;         // engages at 5700
  r.softHystRpm = 100U;
  r.softRetardTenths = 80U;     // 8.0 degrees
  r.retardSlewTenths = 0U;      // immediate, so the test reads the band not the ramp

  f.launch(0U);
  f.runRpm(100U, 5000U);        // well below the zone
  TEST_ASSERT_EQUAL_UINT8(0, ppLaunchRetard());

  f.runRpm(10U, 5750U);         // inside the zone
  TEST_ASSERT_EQUAL_UINT8(8, ppLaunchRetard());
}

void test_rpm_mode_soft_retard_hysteresis_prevents_flicker(void)
{
  Fixture f;
  enableRpmMode(f);
  ppLaunchRpmBlock &r = ppLaunchGetRpmPageMutable();
  for (uint8_t i = 0U; i < PP_RPM_CURVE_POINTS; ++i) { r.curveLimit[i] = 6000U; }
  r.softZoneRpm = 300U;     // engage at 5700
  r.softHystRpm = 100U;     // release only below 5600
  r.softRetardTenths = 80U;
  r.retardSlewTenths = 0U;

  f.launch(0U);
  f.runRpm(100U, 5750U);
  TEST_ASSERT_EQUAL_UINT8(8, ppLaunchRetard());   // engaged

  // Dropping just below the engage point must NOT release it - that is the hysteresis band, and
  // without it the timing would flicker on and off right at the boundary.
  f.runRpm(10U, 5650U);
  TEST_ASSERT_EQUAL_UINT8(8, ppLaunchRetard());

  f.runRpm(10U, 5500U);          // below the release point
  TEST_ASSERT_EQUAL_UINT8(0, ppLaunchRetard());
}

void test_rpm_mode_soft_zone_of_zero_disables_the_retard(void)
{
  Fixture f;
  enableRpmMode(f);
  ppLaunchRpmBlock &r = ppLaunchGetRpmPageMutable();
  for (uint8_t i = 0U; i < PP_RPM_CURVE_POINTS; ++i) { r.curveLimit[i] = 6000U; }
  r.softZoneRpm = 0U;
  r.softRetardTenths = 80U;

  f.launch(0U);
  f.runRpm(100U, 5999U);   // right at the limit
  TEST_ASSERT_EQUAL_UINT8(0, ppLaunchRetard());
}

void test_rpm_mode_cut_method_is_selectable(void)
{
  const PpLimitMethod methods[] = { PP_LIMIT_SPARK, PP_LIMIT_FUEL, PP_LIMIT_BOTH, PP_LIMIT_ROLLING };
  for (uint8_t m = 0U; m < 4U; ++m)
  {
    Fixture f;
    enableRpmMode(f);
    ppLaunchRpmBlock &r = ppLaunchGetRpmPageMutable();
    r.cutMethod = (uint8_t)methods[m];
    for (uint8_t i = 0U; i < PP_RPM_CURVE_POINTS; ++i) { r.curveLimit[i] = 5000U; }

    f.launch(0U);
    f.runRpm(100U, 3000U);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)methods[m], (uint8_t)ppLaunchLimitMethod());
  }
}

void test_rpm_mode_no_cut_still_allows_retard(void)
{
  Fixture f;
  enableRpmMode(f);
  ppLaunchRpmBlock &r = ppLaunchGetRpmPageMutable();
  r.cutMethod = PP_LIMIT_NONE;
  for (uint8_t i = 0U; i < PP_RPM_CURVE_POINTS; ++i) { r.curveLimit[i] = 6000U; }
  r.softZoneRpm = 300U;
  r.softRetardTenths = 80U;
  r.retardSlewTenths = 0U;

  f.launch(0U);
  f.runRpm(100U, 5900U);

  TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, ppLaunchRpmLimit());  // no limiter
  TEST_ASSERT_EQUAL_UINT8(8, ppLaunchRetard());              // but the soft retard works
}

void test_rpm_mode_retard_also_goes_through_the_torque_bus(void)
{
  Fixture f;
  enableRpmMode(f);
  ppLaunchRpmBlock &r = ppLaunchGetRpmPageMutable();
  for (uint8_t i = 0U; i < PP_RPM_CURVE_POINTS; ++i) { r.curveLimit[i] = 6000U; }
  r.softZoneRpm = 300U;
  r.softRetardTenths = 120U;   // 12 deg
  r.retardSlewTenths = 0U;

  f.launch(0U);
  f.runRpm(100U, 5900U);
  TEST_ASSERT_EQUAL_INT8(12, torqueBusRetard());
}

// =====================================================================================
// Launch boost target (page 20) - the base-target layering
// =====================================================================================

void test_launch_boost_contributes_nothing_when_disabled(void)
{
  Fixture f;
  boostLaunchUpdate(f.current);
  TEST_ASSERT_FALSE(boostLaunchIsActive());
  TEST_ASSERT_EQUAL_UINT16(0, boostLaunchBaseTarget());
  TEST_ASSERT_EQUAL_UINT8(0, boostLaunchDutyTrim());
}

void test_launch_boost_is_inactive_before_the_launch(void)
{
  Fixture f;
  boostLaunchGetPageMutable().enable = BL_ENABLED;

  // Enabled, but the car has not launched: the module must contribute nothing, so the boost
  // controller behaves exactly as if the feature did not exist.
  f.tick(5U);
  boostLaunchUpdate(f.current);
  TEST_ASSERT_FALSE(boostLaunchIsActive());
}

void test_launch_boost_supplies_the_base_target_while_launching(void)
{
  Fixture f;
  boostLaunchBlock &b = boostLaunchGetPageMutable();
  b.enable = BL_ENABLED;
  for (uint8_t i = 0U; i < BL_CURVE_POINTS; ++i) { b.curveTarget[i] = 220U; }

  f.launch(0U);
  f.run(100U, 0U);
  boostLaunchUpdate(f.current);

  TEST_ASSERT_TRUE(boostLaunchIsActive());
  TEST_ASSERT_EQUAL_UINT16(220, boostLaunchBaseTarget());
}

void test_launch_boost_follows_the_time_curve(void)
{
  Fixture f;
  boostLaunchBlock &b = boostLaunchGetPageMutable();
  b.enable = BL_ENABLED;
  // Ramp 200 -> 300 over the first 100 ms, then HOLD 300. Setting only the first two points
  // would leave the rest at zero and turn the curve into a 300 -> 0 descent.
  for (uint8_t i = 0U; i < BL_CURVE_POINTS; ++i) { b.curveTarget[i] = 300U; }
  b.curveTarget[0] = 200U;    // t=0
  b.curveTarget[1] = 300U;    // t=100

  f.launch(0U);
  f.run(50U, 0U);
  boostLaunchUpdate(f.current);
  TEST_ASSERT_EQUAL_UINT16(250, boostLaunchBaseTarget());   // halfway up

  f.run(100U, 0U);
  boostLaunchUpdate(f.current);
  TEST_ASSERT_EQUAL_UINT16(300, boostLaunchBaseTarget());
}

void test_launch_boost_reverts_fully_when_the_launch_completes(void)
{
  // The whole point: no "hold the last value" state. A car that has finished launching belongs
  // on its normal boost strategy.
  Fixture f;
  boostLaunchBlock &b = boostLaunchGetPageMutable();
  b.enable = BL_ENABLED;
  for (uint8_t i = 0U; i < BL_CURVE_POINTS; ++i) { b.curveTarget[i] = 250U; }

  const uint16_t lastTime = ppLaunchGetSpeedPage().curveTime[PP_CURVE_POINTS - 1U];
  f.launch(0U);
  f.run((uint32_t)lastTime + 5U, 0U);
  boostLaunchUpdate(f.current);

  TEST_ASSERT_EQUAL_UINT8(PP_STATE_COMPLETE, ppLaunchState());
  TEST_ASSERT_FALSE_MESSAGE(boostLaunchIsActive(),
                            "the launch boost module must switch itself off when the pass ends");
  TEST_ASSERT_EQUAL_UINT16(0, boostLaunchBaseTarget());
}

void test_launch_boost_zero_target_is_no_override_not_zero_boost(void)
{
  // Commanding zero boost because a tuner left a row blank would stall the car.
  Fixture f;
  boostLaunchBlock &b = boostLaunchGetPageMutable();
  b.enable = BL_ENABLED;
  for (uint8_t i = 0U; i < BL_CURVE_POINTS; ++i)
  {
    b.curveTarget[i] = 0U;
    b.curveDutyTrim[i] = 0U;
  }

  f.launch(0U);
  f.run(100U, 0U);
  boostLaunchUpdate(f.current);

  TEST_ASSERT_FALSE(boostLaunchIsActive());
  TEST_ASSERT_EQUAL_UINT16(0, boostLaunchBaseTarget());
}

void test_launch_boost_trim_alone_still_counts_as_active(void)
{
  // Trimming duty without moving the target is a normal way to shape the hit, so it must count
  // as the module being active even though the target is 0.
  Fixture f;
  boostLaunchBlock &b = boostLaunchGetPageMutable();
  b.enable = BL_ENABLED;
  for (uint8_t i = 0U; i < BL_CURVE_POINTS; ++i)
  {
    b.curveTarget[i] = 0U;
    b.curveDutyTrim[i] = 25U;
  }

  f.launch(0U);
  f.run(100U, 0U);
  boostLaunchUpdate(f.current);

  TEST_ASSERT_TRUE(boostLaunchIsActive());
  TEST_ASSERT_EQUAL_UINT8(25, boostLaunchDutyTrim());
}

void test_launch_boost_curves_are_interpolated_not_stepped(void)
{
  Fixture f;
  boostLaunchBlock &b = boostLaunchGetPageMutable();
  b.enable = BL_ENABLED;
  for (uint8_t i = 0U; i < BL_CURVE_POINTS; ++i)
  {
    b.curveTarget[i] = (uint16_t)(200U + i * 10U);
    b.curveDutyTrim[i] = (uint8_t)(i * 2U);
  }

  f.launch(0U);
  f.run(50U, 0U);    // halfway between 200 (t=0) and 210 (t=100)
  boostLaunchUpdate(f.current);

  TEST_ASSERT_EQUAL_UINT16(205, boostLaunchBaseTarget());
  TEST_ASSERT_EQUAL_UINT8(1, boostLaunchDutyTrim());
}

// =====================================================================================
// Mode interaction
// =====================================================================================

void test_each_mode_keeps_its_own_table_when_switching(void)
{
  // The requirement behind having two pages at all: flip modes, flip back, and the table you
  // spent an evening on is still there.
  ppLaunchSpeedBlock &s = ppLaunchGetSpeedPageMutable();
  ppLaunchRpmBlock &r = ppLaunchGetRpmPageMutable();

  s.enable = PP_ENABLED;
  r.enable = PP_DISABLED;
  for (uint8_t i = 0U; i < PP_CURVE_POINTS; ++i) { s.curveRetard[i] = 111U; }

  s.enable = PP_DISABLED;
  r.enable = PP_ENABLED;
  for (uint8_t i = 0U; i < PP_RPM_CURVE_POINTS; ++i) { r.curveLimit[i] = 7777U; }

  s.enable = PP_ENABLED;
  r.enable = PP_DISABLED;

  for (uint8_t i = 0U; i < PP_CURVE_POINTS; ++i) { TEST_ASSERT_EQUAL_UINT16(111U, s.curveRetard[i]); }
  for (uint8_t i = 0U; i < PP_RPM_CURVE_POINTS; ++i) { TEST_ASSERT_EQUAL_UINT16(7777U, r.curveLimit[i]); }
}

void test_speed_mode_takes_precedence_when_both_are_enabled(void)
{
  Fixture f;
  ppLaunchGetRpmPageMutable().enable = PP_ENABLED;
  // Both enabled: the speed page is the more specific controller and wins.
  TEST_ASSERT_EQUAL_UINT8(PP_MODE_SPEED, (uint8_t)ppLaunchActiveMode());
}

// =====================================================================================
// Safety interlocks
// =====================================================================================

void test_a_speed_axis_without_a_vss_input_refuses_to_run(void)
{
  Fixture f;
  f.page2.vssMode = VSS_MODE_OFF;

  f.launch(0U);
  f.run(100U, 500U);

  TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, ppLaunchRpmLimit());
  TEST_ASSERT_EQUAL_UINT8(0, ppLaunchRetard());
}

void test_retard_never_exceeds_the_scheduler_maximum(void)
{
  Fixture f;
  ppLaunchSpeedBlock &s = ppLaunchGetSpeedPageMutable();
  for (uint8_t i = 0U; i < PP_CURVE_POINTS; ++i) { s.curveRetard[i] = 600U; }
  for (uint8_t i = 0U; i < PP_SLIP_POINTS; ++i)
  {
    s.slipBin[i] = (uint16_t)(i * 10U);
    s.slipRetard[i] = 600U;
  }

  f.launch(0U);
  f.run(100U, 5000U);
  TEST_ASSERT_TRUE(ppLaunchRetard() <= 60U);
}

// =====================================================================================
// Storage
// =====================================================================================

void test_restore_defaults_covers_all_three_pages(void)
{
  ppLaunchGetSpeedPageMutable().curveRetard[0] = 1U;
  ppLaunchGetRpmPageMutable().curveLimit[0] = 1U;
  boostLaunchGetPageMutable().curveTarget[0] = 1U;

  ppLaunchRestoreDefaults();
  boostLaunchRestoreDefaults();

  // Not zero: a zeroed launch curve reads as "no limiter configured" everywhere it is consulted.
  TEST_ASSERT_EQUAL_UINT16(120U, ppLaunchGetSpeedPage().curveRetard[0]);
  TEST_ASSERT_EQUAL_UINT16(4200U, ppLaunchGetRpmPage().curveLimit[0]);
  // The boost page defaults to a REAL curve too - a zeroed one would be an invisible feature.
  TEST_ASSERT_TRUE(boostLaunchGetPage().curveTarget[0] > 0U);
}

void test_the_three_pages_do_not_alias_each_other(void)
{
  ppLaunchGetSpeedPageMutable().curveTime[1] = 101U;
  ppLaunchGetRpmPageMutable().curveTime[1] = 202U;
  boostLaunchGetPageMutable().curveTime[1] = 303U;

  TEST_ASSERT_EQUAL_UINT16(101U, ppLaunchGetSpeedPage().curveTime[1]);
  TEST_ASSERT_EQUAL_UINT16(202U, ppLaunchGetRpmPage().curveTime[1]);
  TEST_ASSERT_EQUAL_UINT16(303U, boostLaunchGetPage().curveTime[1]);
}

// =====================================================================================
// Runner
// =====================================================================================

} // anonymous namespace

void testPpLaunch(void)
{
  UNITY_BEGIN();

  // Layout contract
  RUN_TEST(test_both_launch_pages_are_whole_tunerstudio_pages);
  RUN_TEST(test_boost_page_is_a_kilobyte_with_room_to_grow);
  RUN_TEST(test_speed_page_offsets_match_the_ini);
  RUN_TEST(test_rpm_page_offsets_match_the_ini);
  RUN_TEST(test_boost_page_offsets_match_the_ini);
  RUN_TEST(test_the_rpm_page_is_simple_on_purpose);
  RUN_TEST(test_blocks_do_not_overlap);
  RUN_TEST(test_each_block_has_a_distinct_magic);

  // Defaults
  RUN_TEST(test_every_mode_ships_disabled_but_configured);
  RUN_TEST(test_time_axes_ship_strictly_ascending);
  RUN_TEST(test_rpm_defaults_ramp_rather_than_sit_flat);

  // Speed mode: state machine
  RUN_TEST(test_arming_does_not_start_the_clock_or_apply_anything);
  RUN_TEST(test_release_edge_starts_the_clock);
  RUN_TEST(test_armed_timeout_prevents_an_indefinitely_armed_car);
  RUN_TEST(test_disabling_mid_pass_drops_every_output);
  RUN_TEST(test_the_pass_completes_at_the_end_of_the_curve_and_latches);
  RUN_TEST(test_rearm_below_axis_returns_to_idle);

  // Speed mode: retard / limit / trim
  RUN_TEST(test_retard_follows_the_curve_and_goes_through_the_torque_bus);
  RUN_TEST(test_retard_rounds_to_nearest_and_honours_large_values);
  RUN_TEST(test_a_descending_retard_curve_does_not_wrap);
  RUN_TEST(test_retard_slew_limits_the_rate_in_both_directions);
  RUN_TEST(test_limit_follows_the_curve_and_uses_the_configured_method);
  RUN_TEST(test_limit_method_none_disables_the_limiter_but_keeps_retard);
  RUN_TEST(test_start_gate_blocks_the_limiter_above_its_speed);
  RUN_TEST(test_speed_mode_duty_trim_is_gated_on_its_flag);

  // Speed mode: perfect-pass
  RUN_TEST(test_slip_is_signed_and_only_positive_slip_is_corrected);

  // RPM mode
  RUN_TEST(test_rpm_mode_follows_the_limit_curve_versus_time);
  RUN_TEST(test_rpm_mode_limit_ramps_with_time);
  RUN_TEST(test_rpm_mode_soft_retard_engages_within_the_zone);
  RUN_TEST(test_rpm_mode_soft_retard_hysteresis_prevents_flicker);
  RUN_TEST(test_rpm_mode_soft_zone_of_zero_disables_the_retard);
  RUN_TEST(test_rpm_mode_cut_method_is_selectable);
  RUN_TEST(test_rpm_mode_no_cut_still_allows_retard);
  RUN_TEST(test_rpm_mode_retard_also_goes_through_the_torque_bus);

  // Launch boost (page 20)
  RUN_TEST(test_launch_boost_contributes_nothing_when_disabled);
  RUN_TEST(test_launch_boost_is_inactive_before_the_launch);
  RUN_TEST(test_launch_boost_supplies_the_base_target_while_launching);
  RUN_TEST(test_launch_boost_follows_the_time_curve);
  RUN_TEST(test_launch_boost_reverts_fully_when_the_launch_completes);
  RUN_TEST(test_launch_boost_zero_target_is_no_override_not_zero_boost);
  RUN_TEST(test_launch_boost_trim_alone_still_counts_as_active);
  RUN_TEST(test_launch_boost_curves_are_interpolated_not_stepped);

  // Mode interaction
  RUN_TEST(test_each_mode_keeps_its_own_table_when_switching);
  RUN_TEST(test_speed_mode_takes_precedence_when_both_are_enabled);

  // Safety
  RUN_TEST(test_a_speed_axis_without_a_vss_input_refuses_to_run);
  RUN_TEST(test_retard_never_exceeds_the_scheduler_maximum);

  // Storage
  RUN_TEST(test_restore_defaults_covers_all_three_pages);
  RUN_TEST(test_the_three_pages_do_not_alias_each_other);

  UNITY_END();
}
