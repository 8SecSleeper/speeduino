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
    torqueBusResetForTest();

    page2.vssMode = VSS_MODE_EXTERNAL_KM;
    ppLaunchSetAxisForTest(0);

    // Speed page: enabled, deterministic. Axis in tenths of a mph.
    ppLaunchSpeedBlock &s = ppLaunchGetSpeedPageMutable();
    s.enable = PP_ENABLED;
    s.armPin = 0;
    s.armPolarity = 0;
    s.axisSource = PP_AXIS_VSS_MPH10;
    s.curveMode = PP_CURVE_BY_AXIS;
    s.limiterStrategy = (uint8_t)(PP_STRAT_FUEL_CUT | PP_STRAT_SPARK_CUT);
    s.featureFlags = (uint8_t)(PP_FLAG_PERFECT_PASS);
    s.timeoutMs = 0;
    s.startBelowAxis = 0;
    s.rearmBelowAxis = 0;
    s.softZonePercent = 0;
    s.axisFilterShift = 0;
    s.axisAccelLimit = 0xFFFFU;
    s.retardSlewMsPerDeg = 0;

    // RPM page off so the speed page is what the tests read by default.
    ppLaunchGetRpmPageMutable().enable = PP_DISABLED;
    ppLaunchGetReactPageMutable().enable = PP_DISABLED;
  }

  void tick(uint32_t ms = 1U)
  {
    for (uint32_t i = 0U; i < ms; ++i) { ppLaunchTick1ms(current); }
  }

  void launch(uint16_t axisStart = 0U)
  {
    ppLaunchSetAxisForTest(axisStart);
    ppLaunchSetArmForTest(true);
    tick(2U);
    ppLaunchSetArmForTest(false);
    tick(1U);
  }

  void run(uint32_t ms, uint16_t axis)
  {
    ppLaunchSetAxisForTest(axis);
    for (uint32_t i = 0U; i < ms; ++i)
    {
      ppLaunchTick1ms(current);
      ppLaunchUpdate(current, page2);
    }
  }
};

// =====================================================================================
// Layout contract - the tuning-file ABI
// =====================================================================================

void test_all_three_pages_are_whole_tunerstudio_pages(void)
{
  TEST_ASSERT_EQUAL_UINT16(512, sizeof(ppLaunchSpeedConfig));
  TEST_ASSERT_EQUAL_UINT16(512, sizeof(ppLaunchRpmConfig));
  TEST_ASSERT_EQUAL_UINT16(512, sizeof(ppLaunchReactConfig));
  TEST_ASSERT_EQUAL_UINT16(512, PPLAUNCH_PAYLOAD_SIZE);
}

void test_the_table_is_one_axis_and_two_tied_rows(void)
{
  // The structural decision this whole page exists for: ONE 32-point X axis, with the two rows
  // laid directly after it so they share it. Three independently-axised curves cannot be read as
  // a table, which is what an earlier draft got wrong.
  TEST_ASSERT_EQUAL_UINT16(offsetof(ppLaunchSpeedConfig, axisSpeed) + (2U * PP_CURVE_POINTS),
                           offsetof(ppLaunchSpeedConfig, rowRetard));
  TEST_ASSERT_EQUAL_UINT16(offsetof(ppLaunchSpeedConfig, rowRetard) + (2U * PP_CURVE_POINTS),
                           offsetof(ppLaunchSpeedConfig, rowTimeMs));
  TEST_ASSERT_EQUAL_UINT16(offsetof(ppLaunchRpmConfig, axisValue) + (2U * PP_CURVE_POINTS),
                           offsetof(ppLaunchRpmConfig, rowRetard));
  TEST_ASSERT_EQUAL_UINT16(offsetof(ppLaunchRpmConfig, rowRetard) + (2U * PP_CURVE_POINTS),
                           offsetof(ppLaunchRpmConfig, rowTimeMs));
}

void test_there_is_no_second_rpm_or_retard_vs_time_table(void)
{
  // Retard vs race time already exists on page 17, and the RPM page owns the RPM limit. A copy of
  // either here would be a second thing to keep in step.
  TEST_ASSERT_EQUAL_UINT16(512, sizeof(ppLaunchSpeedConfig));
  // The speed page's table region is exactly three 32-point tables; everything past rowTimeMs is
  // reserved, so there is no room for a fourth table by accident.
  const uint16_t tableEnd = offsetof(ppLaunchSpeedConfig, rowTimeMs) + (2U * PP_CURVE_POINTS);
  TEST_ASSERT_EQUAL_UINT16(offsetof(ppLaunchSpeedConfig, reserved1), tableEnd);
}

void test_speed_page_offsets_match_the_ini(void)
{
  TEST_ASSERT_EQUAL_UINT16(0, offsetof(ppLaunchSpeedConfig, enable));
  TEST_ASSERT_EQUAL_UINT16(5, offsetof(ppLaunchSpeedConfig, limiterStrategy));
  TEST_ASSERT_EQUAL_UINT16(14, offsetof(ppLaunchSpeedConfig, softZonePercent));
  TEST_ASSERT_EQUAL_UINT16(24, offsetof(ppLaunchSpeedConfig, label));
  TEST_ASSERT_EQUAL_UINT16(48, offsetof(ppLaunchSpeedConfig, axisSpeed));
  TEST_ASSERT_EQUAL_UINT16(112, offsetof(ppLaunchSpeedConfig, rowRetard));
  TEST_ASSERT_EQUAL_UINT16(176, offsetof(ppLaunchSpeedConfig, rowTimeMs));
}

void test_rpm_page_offsets_match_the_ini(void)
{
  TEST_ASSERT_EQUAL_UINT16(0, offsetof(ppLaunchRpmConfig, enable));
  TEST_ASSERT_EQUAL_UINT16(4, offsetof(ppLaunchRpmConfig, limiterStrategy));
  TEST_ASSERT_EQUAL_UINT16(8, offsetof(ppLaunchRpmConfig, softZonePercent));
  TEST_ASSERT_EQUAL_UINT16(10, offsetof(ppLaunchRpmConfig, label));
  TEST_ASSERT_EQUAL_UINT16(32, offsetof(ppLaunchRpmConfig, axisValue));
  TEST_ASSERT_EQUAL_UINT16(96, offsetof(ppLaunchRpmConfig, rowRetard));
  TEST_ASSERT_EQUAL_UINT16(160, offsetof(ppLaunchRpmConfig, rowTimeMs));
}

void test_reactions_page_offsets_match_the_ini(void)
{
  TEST_ASSERT_EQUAL_UINT16(0, offsetof(ppLaunchReactConfig, enable));
  TEST_ASSERT_EQUAL_UINT16(32, offsetof(ppLaunchReactConfig, axisTimeMs));
  TEST_ASSERT_EQUAL_UINT16(44, offsetof(ppLaunchReactConfig, fuelAddPercent));
  TEST_ASSERT_EQUAL_UINT16(56, offsetof(ppLaunchReactConfig, boostDutyAdj));
  TEST_ASSERT_EQUAL_UINT16(68, offsetof(ppLaunchReactConfig, boostTargetAdj));
  TEST_ASSERT_EQUAL_UINT16(80, offsetof(ppLaunchReactConfig, retardAdj));
}

void test_blocks_do_not_overlap(void)
{
  // 0x1000 LTT, 0x1820 race time, 0x1930 speed, 0x1B40 rpm, 0x1D50 reactions, 0x1F60 boost.
  TEST_ASSERT_EQUAL_UINT16(EEPROM_PPLAUNCH_SPEED_BASE, 0x1930);
  TEST_ASSERT_EQUAL_UINT16(EEPROM_PPLAUNCH_RPM_BASE, 0x1930 + PPLAUNCH_BLOCK_SIZE);
  TEST_ASSERT_EQUAL_UINT16(EEPROM_PPLAUNCH_REACT_BASE, 0x1B40 + PPLAUNCH_BLOCK_SIZE);
  TEST_ASSERT_TRUE((uint32_t)EEPROM_PPLAUNCH_REACT_BASE + PPLAUNCH_BLOCK_SIZE <= 0x5D00UL);
}

void test_each_block_has_a_distinct_magic(void)
{
  TEST_ASSERT_NOT_EQUAL(EEPROM_PPLAUNCH_SPEED_MAGIC, EEPROM_PPLAUNCH_RPM_MAGIC);
  TEST_ASSERT_NOT_EQUAL(EEPROM_PPLAUNCH_SPEED_MAGIC, EEPROM_PPLAUNCH_REACT_MAGIC);
  TEST_ASSERT_NOT_EQUAL(EEPROM_PPLAUNCH_RPM_MAGIC, EEPROM_PPLAUNCH_REACT_MAGIC);
}

// =====================================================================================
// Defaults
// =====================================================================================

void test_all_three_ship_disabled(void)
{
  ppLaunchResetForTest();
  TEST_ASSERT_EQUAL_UINT8(PP_DISABLED, ppLaunchGetSpeedPage().enable);
  TEST_ASSERT_EQUAL_UINT8(PP_DISABLED, ppLaunchGetRpmPage().enable);
  TEST_ASSERT_EQUAL_UINT8(PP_DISABLED, ppLaunchGetReactPage().enable);
}

void test_speed_axis_ships_strictly_ascending(void)
{
  ppLaunchResetForTest();
  const ppLaunchSpeedBlock &s = ppLaunchGetSpeedPage();
  for (uint8_t i = 1U; i < PP_CURVE_POINTS; ++i)
  {
    TEST_ASSERT_GREATER_THAN_UINT16(s.axisSpeed[i - 1U], s.axisSpeed[i]);
  }
  TEST_ASSERT_TRUE(s.rowRetard[0] > 0U);      // a real template, not a flat zero
  TEST_ASSERT_TRUE(s.rowRetard[0] >= s.rowRetard[PP_CURVE_POINTS - 1U]);
}

void test_reaction_axis_is_the_fixed_six_points(void)
{
  ppLaunchResetForTest();
  const ppLaunchReactBlock &r = ppLaunchGetReactPage();
  const uint16_t expected[PP_REACTION_POINTS] = { 0U, 100U, 200U, 300U, 400U, 500U };
  for (uint8_t i = 0U; i < PP_REACTION_POINTS; ++i)
  {
    TEST_ASSERT_EQUAL_UINT16(expected[i], r.axisTimeMs[i]);
    TEST_ASSERT_EQUAL_UINT16(PP_REACTION_TIME_MS[i], r.axisTimeMs[i]);
  }
}

// =====================================================================================
// The linear soft-zone retard - the core behaviour
// =====================================================================================

/** @brief Configure the speed page with a fixed max retard and a known setpoint. */
/**
 * @brief Build a linear axis ending at @p setpointAxis, with a flat max-retard row.
 *
 * The setpoint IS the last axis value: that is the speed at which the launch window closes and
 * therefore the hard limit, and the soft zone is measured back from it.
 */
void setupSoftZone(Fixture &f, uint16_t maxRetardTenths, uint16_t setpointAxis, uint8_t softPct)
{
  ppLaunchSpeedBlock &s = ppLaunchGetSpeedPageMutable();
  for (uint8_t i = 0U; i < PP_CURVE_POINTS; ++i)
  {
    // Linear 0 .. setpointAxis across the 32 points, strictly ascending.
    s.axisSpeed[i] = (uint16_t)(((uint32_t)setpointAxis * i) / (PP_CURVE_POINTS - 1U));
    s.rowRetard[i] = maxRetardTenths;
    s.rowTimeMs[i] = (uint16_t)(i * 100U);
  }
  s.axisSpeed[PP_CURVE_POINTS - 1U] = setpointAxis;
  s.startBelowAxis = 0U;         // no extra gate: the axis top is the window
  s.softZonePercent = softPct;
  s.featureFlags = (uint8_t)(s.featureFlags & (uint8_t)(~PP_FLAG_RETARD_SLEW));
  (void)f;
}

void test_retard_is_zero_at_or_below_the_soft_limit(void)
{
  Fixture f;
  // Setpoint 1000, 10% band = starts at 900.
  setupSoftZone(f, 100U, 1000U, 10U);   // 100 tenths = 10.0 deg at the hard limit

  f.launch(0U);
  f.run(10U, 800U);                     // below the soft limit
  TEST_ASSERT_EQUAL_UINT8(0, ppLaunchRetard());

  f.run(10U, 900U);                     // exactly at the soft limit
  TEST_ASSERT_EQUAL_UINT8(0, ppLaunchRetard());
}

void test_retard_ramps_linearly_from_soft_to_hard_limit(void)
{
  // THIS is the behaviour the feature exists for. A step change in advance reads as a misfire;
  // just tripping the soft limit should pull barely anything.
  Fixture f;
  setupSoftZone(f, 100U, 1000U, 10U);   // 10.0 deg max, band 900..1000

  f.launch(0U);

  f.run(10U, 950U);                     // halfway through the band
  TEST_ASSERT_EQUAL_UINT8(5, ppLaunchRetard());   // ~half of 10 deg

  f.run(10U, 1000U);                    // at the hard limit
  TEST_ASSERT_EQUAL_UINT8(10, ppLaunchRetard());
}

void test_just_tripping_the_soft_limit_pulls_very_little(void)
{
  Fixture f;
  setupSoftZone(f, 100U, 1000U, 10U);

  f.launch(0U);
  f.run(10U, 905U);   // 5% into the band
  // 0.5 deg rounds to 1. The point is that it is nowhere near the full 10.
  TEST_ASSERT_LESS_THAN_UINT8(3, ppLaunchRetard());
}

void test_a_wider_soft_zone_starts_pulling_earlier(void)
{
  Fixture f;
  setupSoftZone(f, 100U, 1000U, 50U);   // band starts at 500
  f.launch(0U);
  f.run(10U, 700U);                     // mid-band with a 50% zone
  TEST_ASSERT_GREATER_THAN_UINT8(0, ppLaunchRetard());
}

void test_the_soft_zone_scales_with_the_setpoint(void)
{
  // A percentage rather than an absolute offset is the whole point: 10% of a high setpoint is a
  // wider band than 10% of a low one, in the same proportion a driver feels.
  Fixture f;
  setupSoftZone(f, 100U, 2000U, 10U);   // band starts at 1800
  f.launch(0U);
  f.run(10U, 1700U);
  TEST_ASSERT_EQUAL_UINT8(0, ppLaunchRetard());   // outside the band
  f.run(10U, 1900U);
  TEST_ASSERT_GREATER_THAN_UINT8(0, ppLaunchRetard());
}

// =====================================================================================
// State machine
// =====================================================================================

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
}

void test_disabling_mid_pass_drops_every_output(void)
{
  Fixture f;
  setupSoftZone(f, 100U, 1000U, 10U);
  f.launch(0U);
  f.run(20U, 1000U);
  TEST_ASSERT_GREATER_THAN_UINT8(0, ppLaunchRetard());

  ppLaunchGetSpeedPageMutable().enable = PP_DISABLED;
  f.run(1U, 1000U);
  TEST_ASSERT_EQUAL_UINT8(PP_STATE_OFF, ppLaunchState());
  TEST_ASSERT_EQUAL_UINT8(0, ppLaunchRetard());
  TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, ppLaunchRpmLimit());
}

// =====================================================================================
// Hard limiter strategy
// =====================================================================================

void test_strategy_bits_are_combined_and_reported(void)
{
  Fixture f;
  setupSoftZone(f, 100U, 1000U, 10U);
  ppLaunchSpeedBlock &s = ppLaunchGetSpeedPageMutable();
  s.limiterStrategy = (uint8_t)(PP_STRAT_BOOST_REDUCE | PP_STRAT_SPARK_CUT);

  f.launch(0U);
  f.run(20U, 500U);   // inside the launch window
  TEST_ASSERT_EQUAL_UINT8((uint8_t)(PP_STRAT_BOOST_REDUCE | PP_STRAT_SPARK_CUT),
                          ppLaunchStrategy());
}

void test_no_strategy_means_no_limit(void)
{
  Fixture f;
  setupSoftZone(f, 100U, 1000U, 10U);
  ppLaunchGetSpeedPageMutable().limiterStrategy = 0U;

  f.launch(0U);
  f.run(20U, 500U);
  TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, ppLaunchRpmLimit());
  TEST_ASSERT_EQUAL_UINT8(0, ppLaunchStrategy());
}

// =====================================================================================
// Both modes running simultaneously
// =====================================================================================

void test_both_modes_run_at_once_and_the_lower_limit_wins(void)
{
  Fixture f;
  setupSoftZone(f, 100U, 4000U, 10U);   // speed page: limit 4000

  ppLaunchRpmBlock &r = ppLaunchGetRpmPageMutable();
  r.enable = PP_ENABLED;
  r.armPin = 0;
  r.armPolarity = 0;
  r.timeoutMs = 0;
  r.softZonePercent = 10U;
  r.limiterStrategy = PP_STRAT_SPARK_CUT;
  // RPM axis 3000..6000, so at 5000 rpm the limit is 5000.
  for (uint8_t i = 0U; i < PP_CURVE_POINTS; ++i)
  {
    r.axisValue[i] = (uint16_t)(3000U + (i * 3000U) / (PP_CURVE_POINTS - 1U));
    r.rowRetard[i] = 50U;
    r.rowTimeMs[i] = (uint16_t)(i * 100U);
  }
  r.axisValue[PP_CURVE_POINTS - 1U] = 6000U;

  f.launch(0U);
  f.current.RPM = 5000U;
  f.run(20U, 500U);

  // Both modes are live: the RPM page says 5000, the speed page says 4000, so 4000 binds.
  TEST_ASSERT_EQUAL_UINT16(4000, ppLaunchRpmLimit());
}

void test_each_mode_keeps_its_own_table(void)
{
  // Neither page carries a copy of the other's table - that is what makes simultaneous operation
  // and independent editing possible.
  ppLaunchSpeedBlock &s = ppLaunchGetSpeedPageMutable();
  ppLaunchRpmBlock &r = ppLaunchGetRpmPageMutable();
  s.rowRetard[3] = 111U;
  r.rowRetard[3] = 222U;
  TEST_ASSERT_EQUAL_UINT16(111U, s.rowRetard[3]);
  TEST_ASSERT_EQUAL_UINT16(222U, r.rowRetard[3]);
}

// =====================================================================================
// Reactions
// =====================================================================================

void test_reactions_do_nothing_when_disabled(void)
{
  Fixture f;
  setupSoftZone(f, 100U, 1000U, 10U);
  f.launch(0U);
  f.run(50U, 1000U);   // at the hard limit for 50 ms
  TEST_ASSERT_EQUAL_INT16(0, ppLaunchFuelAdd());
  TEST_ASSERT_EQUAL_INT16(0, ppLaunchBoostDutyAdj());
}

void test_reactions_ramp_with_sustained_time_at_the_limit(void)
{
  Fixture f;
  setupSoftZone(f, 100U, 1000U, 10U);

  ppLaunchReactBlock &x = ppLaunchGetReactPageMutable();
  x.enable = BL_ENABLED;
  for (uint8_t i = 0U; i < PP_REACTION_POINTS; ++i)
  {
    x.axisTimeMs[i] = PP_REACTION_TIME_MS[i];
    x.fuelAddPercent[i] = (uint16_t)(i * 10U);   // 0,10,20,30,40,50
    x.boostDutyAdj[i] = 0;
    x.boostTargetAdj[i] = 0;
    x.retardAdj[i] = 0;
  }

  // At the hard limit right away, so the hit-timer accumulates.
  f.launch(0U);
  f.run(20U, 1000U);
  TEST_ASSERT_TRUE(ppLaunchIsLimiting());
  TEST_ASSERT_GREATER_THAN_UINT16(0U, ppLaunchLimitHitMs());
  const int16_t early = ppLaunchFuelAdd();

  f.run(300U, 1000U);   // 300+ ms at the limit
  TEST_ASSERT_GREATER_THAN_INT16(early, ppLaunchFuelAdd());
}

void test_reaction_fuel_boost_and_retard_are_all_published(void)
{
  Fixture f;
  setupSoftZone(f, 100U, 1000U, 10U);

  ppLaunchReactBlock &x = ppLaunchGetReactPageMutable();
  x.enable = BL_ENABLED;
  for (uint8_t i = 0U; i < PP_REACTION_POINTS; ++i)
  {
    x.axisTimeMs[i] = PP_REACTION_TIME_MS[i];
    x.fuelAddPercent[i] = 8U;
    x.boostDutyAdj[i] = -12;
    x.boostTargetAdj[i] = -6;
    x.retardAdj[i] = 30U;   // 3.0 deg
  }

  f.launch(0U);
  f.run(60U, 1000U);

  TEST_ASSERT_EQUAL_INT16(8, ppLaunchFuelAdd());
  TEST_ASSERT_EQUAL_INT16(-12, ppLaunchBoostDutyAdj());
  TEST_ASSERT_EQUAL_INT16(-6, ppLaunchBoostTargetAdj());
}

void test_the_hit_timer_resets_when_the_limit_is_left(void)
{
  Fixture f;
  setupSoftZone(f, 100U, 1000U, 10U);
  ppLaunchReactBlock &x = ppLaunchGetReactPageMutable();
  x.enable = BL_ENABLED;

  f.launch(0U);
  f.run(200U, 1000U);          // fighting the limit
  TEST_ASSERT_GREATER_THAN_UINT16(0U, ppLaunchLimitHitMs());

  // Come off the limit (the car has left the launch window) - the episode is over.
  ppLaunchGetSpeedPageMutable().limiterStrategy = 0U;
  f.run(5U, 1000U);
  TEST_ASSERT_EQUAL_UINT16(0, ppLaunchLimitHitMs());
}

// =====================================================================================
// Safety
// =====================================================================================

void test_a_speed_axis_without_a_vss_input_refuses_to_run(void)
{
  Fixture f;
  setupSoftZone(f, 100U, 1000U, 10U);
  f.page2.vssMode = VSS_MODE_OFF;

  f.launch(0U);
  f.run(100U, 1000U);
  TEST_ASSERT_EQUAL_UINT8(0, ppLaunchRetard());
}

void test_retard_never_exceeds_the_scheduler_maximum(void)
{
  Fixture f;
  setupSoftZone(f, 600U, 1000U, 10U);   // 60.0 deg

  ppLaunchReactBlock &x = ppLaunchGetReactPageMutable();
  x.enable = BL_ENABLED;
  for (uint8_t i = 0U; i < PP_REACTION_POINTS; ++i)
  {
    x.axisTimeMs[i] = PP_REACTION_TIME_MS[i];
    x.retardAdj[i] = 600;   // another 60 deg on top
  }

  f.launch(0U);
  f.run(600U, 1000U);
  TEST_ASSERT_TRUE(ppLaunchRetard() <= 60U);
  TEST_ASSERT_TRUE((uint8_t)torqueBusRetard() <= 60U);
}

// =====================================================================================
// Storage
// =====================================================================================

void test_restore_defaults_covers_all_three_pages(void)
{
  ppLaunchGetSpeedPageMutable().rowRetard[0] = 1U;
  ppLaunchGetRpmPageMutable().rowRetard[0] = 1U;
  ppLaunchGetReactPageMutable().fuelAddPercent[0] = 99U;

  ppLaunchRestoreDefaults();

  TEST_ASSERT_EQUAL_UINT16(120U, ppLaunchGetSpeedPage().rowRetard[0]);
  TEST_ASSERT_EQUAL_UINT16(100U, ppLaunchGetRpmPage().rowRetard[0]);
  TEST_ASSERT_EQUAL_UINT16(0U, ppLaunchGetReactPage().fuelAddPercent[0]);
}

void test_the_three_pages_do_not_alias(void)
{
  ppLaunchGetSpeedPageMutable().rowTimeMs[1] = 101U;
  ppLaunchGetRpmPageMutable().rowTimeMs[1] = 202U;
  ppLaunchGetReactPageMutable().axisTimeMs[1] = 303U;

  TEST_ASSERT_EQUAL_UINT16(101U, ppLaunchGetSpeedPage().rowTimeMs[1]);
  TEST_ASSERT_EQUAL_UINT16(202U, ppLaunchGetRpmPage().rowTimeMs[1]);
  TEST_ASSERT_EQUAL_UINT16(303U, ppLaunchGetReactPage().axisTimeMs[1]);
}

// =====================================================================================
// Runner
// =====================================================================================

} // anonymous namespace

void testPpLaunch(void)
{
  UNITY_BEGIN();

  RUN_TEST(test_all_three_pages_are_whole_tunerstudio_pages);
  RUN_TEST(test_the_table_is_one_axis_and_two_tied_rows);
  RUN_TEST(test_there_is_no_second_rpm_or_retard_vs_time_table);
  RUN_TEST(test_speed_page_offsets_match_the_ini);
  RUN_TEST(test_rpm_page_offsets_match_the_ini);
  RUN_TEST(test_reactions_page_offsets_match_the_ini);
  RUN_TEST(test_blocks_do_not_overlap);
  RUN_TEST(test_each_block_has_a_distinct_magic);

  RUN_TEST(test_all_three_ship_disabled);
  RUN_TEST(test_speed_axis_ships_strictly_ascending);
  RUN_TEST(test_reaction_axis_is_the_fixed_six_points);

  RUN_TEST(test_retard_is_zero_at_or_below_the_soft_limit);
  RUN_TEST(test_retard_ramps_linearly_from_soft_to_hard_limit);
  RUN_TEST(test_just_tripping_the_soft_limit_pulls_very_little);
  RUN_TEST(test_a_wider_soft_zone_starts_pulling_earlier);
  RUN_TEST(test_the_soft_zone_scales_with_the_setpoint);

  RUN_TEST(test_release_edge_starts_the_clock);
  RUN_TEST(test_armed_timeout_prevents_an_indefinitely_armed_car);
  RUN_TEST(test_disabling_mid_pass_drops_every_output);

  RUN_TEST(test_strategy_bits_are_combined_and_reported);
  RUN_TEST(test_no_strategy_means_no_limit);

  RUN_TEST(test_both_modes_run_at_once_and_the_lower_limit_wins);
  RUN_TEST(test_each_mode_keeps_its_own_table);

  RUN_TEST(test_reactions_do_nothing_when_disabled);
  RUN_TEST(test_reactions_ramp_with_sustained_time_at_the_limit);
  RUN_TEST(test_reaction_fuel_boost_and_retard_are_all_published);
  RUN_TEST(test_the_hit_timer_resets_when_the_limit_is_left);

  RUN_TEST(test_a_speed_axis_without_a_vss_input_refuses_to_run);
  RUN_TEST(test_retard_never_exceeds_the_scheduler_maximum);

  RUN_TEST(test_restore_defaults_covers_all_three_pages);
  RUN_TEST(test_the_three_pages_do_not_alias);

  UNITY_END();
}
