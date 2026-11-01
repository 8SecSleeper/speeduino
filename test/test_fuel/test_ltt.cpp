#include <unity.h>
#include <stdio.h>
#include "../test_utils.h"
#include "globals.h"
#include "ltt.h"
#include "units.h"

/**
 * @file
 * @brief Tests for the long-term fuel trim subsystem (speeduino/ltt.h / ltt.cpp).
 *
 * The value of these tests is concentrated in three places, because those are the places
 * where a bug is silent and expensive:
 *
 *  1. **The trim maths.** A trim of 0 must be bit-exact, and the trim must be exactly
 *     linear in between. If it is not, then merely compiling this feature in changes
 *     fuelling on every engine in the field.
 *  2. **The authority clamp.** A cell with no authority must apply no trim and must not
 *     learn. That is the WOT/boost safety control, and it failing open is how an engine
 *     gets hurt.
 *  3. **The learn gates.** Each gate is a reason not to trust a sample. The highest-value
 *     test in this file is that every gate independently suppresses learning.
 */

// =====================================================================================
// Trim application maths
// =====================================================================================

static void test_applyTrim_zero_is_bit_exact(void) {
  // The single most important property in the feature: enabling learning but having
  // learned nothing yet must not perturb fuelling by even one count.
  const uint32_t inputs[] = {0U, 1U, 2U, 37U, 100U, 999U, 1234U, 25000U, 65025U, UINT16_MAX};
  for (uint32_t input : inputs) {
    TEST_ASSERT_EQUAL_UINT32(input, lttApplyTrim(input, 0));
  }
}

static void test_applyTrim_positive_is_linear(void) {
  TEST_ASSERT_EQUAL_UINT32(1100U, lttApplyTrim(1000U, 100));   // +10.0%
  TEST_ASSERT_EQUAL_UINT32(1500U, lttApplyTrim(1000U, 500));   // +50.0%
  TEST_ASSERT_EQUAL_UINT32(2000U, lttApplyTrim(1000U, 1000));  // +100.0%
  TEST_ASSERT_EQUAL_UINT32(1001U, lttApplyTrim(1000U, 1));     // +0.1%
}

static void test_applyTrim_negative_is_linear(void) {
  TEST_ASSERT_EQUAL_UINT32(900U, lttApplyTrim(1000U, -100));   // -10.0%
  TEST_ASSERT_EQUAL_UINT32(500U, lttApplyTrim(1000U, -500));   // -50.0%
  TEST_ASSERT_EQUAL_UINT32(0U, lttApplyTrim(1000U, -1000));    // -100.0%
  TEST_ASSERT_EQUAL_UINT32(999U, lttApplyTrim(1000U, -1));     // -0.1%
}

static void test_applyTrim_rounds_to_nearest(void) {
  TEST_ASSERT_EQUAL_UINT32(333U, lttApplyTrim(333U, 1));   // 333.333 -> 333
  TEST_ASSERT_EQUAL_UINT32(501U, lttApplyTrim(500U, 1));   // 500.5   -> 501
  TEST_ASSERT_EQUAL_UINT32(504U, lttApplyTrim(505U, -1));  // 504.495 -> 504
}

static void test_applyTrim_never_returns_negative_or_wraps(void) {
  TEST_ASSERT_EQUAL_UINT32(0U, lttApplyTrim(UINT16_MAX, -1000));
}

static void test_unitsToPercent(void) {
  TEST_ASSERT_EQUAL_INT16(0, lttUnitsToPercent(0));
  TEST_ASSERT_EQUAL_INT16(1, lttUnitsToPercent(10));
  TEST_ASSERT_EQUAL_INT16(1, lttUnitsToPercent(5));
  TEST_ASSERT_EQUAL_INT16(0, lttUnitsToPercent(4));
  TEST_ASSERT_EQUAL_INT16(-1, lttUnitsToPercent(-5));
  TEST_ASSERT_EQUAL_INT16(10, lttUnitsToPercent(100));
  TEST_ASSERT_EQUAL_INT16(-100, lttUnitsToPercent(-1000));
}

static void test_fuelMultiplierFloor(void) {
  TEST_ASSERT_GREATER_THAN(0, lttFuelMultiplierFloorPct());
}

// =====================================================================================
// Authority
// =====================================================================================

static void test_defaults_are_zero_trim_and_bounded_authority(void) {
  lttResetForTest();
  const lttBlock &page = lttGetPage();

  TEST_ASSERT_EQUAL(LTT_MODE_OFF, page.settings.mode);
  for (uint8_t t = 0; t < LTT_TABLE_COUNT; ++t) {
    for (uint16_t c = 0; c < LTT_CELL_COUNT; ++c) {
      TEST_ASSERT_EQUAL_INT16(0, page.trim[t][c]);
    }
  }

  uint8_t leanPct = 0, richPct = 0;
  lttGetAuthority(0, fuelTable, 100U, 3000U, leanPct, richPct);
  TEST_ASSERT_GREATER_THAN(0, leanPct);
  TEST_ASSERT_GREATER_THAN(0, richPct);
  TEST_ASSERT_LESS_THAN(100, leanPct);
  TEST_ASSERT_LESS_THAN(100, richPct);
}

/** @brief Give the VE table real, monotonically increasing axes.
 *
 * The trim/authority tables share the VE axes, and by default those axes are all zeroes.
 * A degenerate axis sends every lookup to the same cell, which makes it impossible to test
 * that different operating points resolve to different cells. Populating a realistic ramp
 * makes the cell mapping well-defined.
 *
 * Axis units are compressed: RPM bins are stored /100 and load bins /2 (see table3d_axes.h).
 */
static void populate_ve_axes(void) {
  for (uint8_t i = 0; i < LTT_DIM; ++i) {
    fuelTable.axisX[i] = (table3d_axis_t)(10U + (i * 15U));  // 1000..23500 RPM
    fuelTable.axisY[i] = (table3d_axis_t)(10U + (i * 12U));  // 20..380 kPa
  }
  invalidate_cache(&fuelTable.get_value_cache);
}

static void test_authority_is_asymmetric_per_cell(void) {
  lttResetForTest();
  populate_ve_axes();
  lttBlock &page = lttGetPageMutable();

  // Make every cell symmetric, then make exactly one cell asymmetric.
  setAllLttAuthority(30U);
  const uint16_t asymCell = 37U;
  page.leanAuth[0][asymCell] = 5U;   // may remove only 5%
  page.richAuth[0][asymCell] = 80U;  // may add up to 80%

  bool foundAsymmetry = false;
  bool foundSymmetric = false;
  for (uint16_t loadBin = 0; loadBin < LTT_DIM; ++loadBin) {
    for (uint16_t rpmBin = 0; rpmBin < LTT_DIM; ++rpmBin) {
      uint8_t leanPct = 0, richPct = 0;
      lttGetAuthority(0, fuelTable,
                      (uint16_t)fuelTable.axisY[loadBin] * 2U,
                      (uint16_t)fuelTable.axisX[rpmBin] * 100U,
                      leanPct, richPct);
      if ((leanPct == 5U) && (richPct == 80U)) { foundAsymmetry = true; }
      if ((leanPct == 30U) && (richPct == 30U)) { foundSymmetric = true; }
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(foundAsymmetry, "asymmetric lean/rich pair was never returned");
  TEST_ASSERT_TRUE_MESSAGE(foundSymmetric, "symmetric default was never returned");
}

static void test_zero_authority_applies_no_trim(void) {
  lttResetForTest();
  lttBlock &page = lttGetPageMutable();

  for (uint8_t t = 0; t < LTT_TABLE_COUNT; ++t) {
    for (uint16_t c = 0; c < LTT_CELL_COUNT; ++c) {
      page.trim[t][c] = 500; // +50%
    }
  }
  setAllLttAuthority(0U);

  // No authority means NO TRIM AT ALL, not merely "no new trim".
  TEST_ASSERT_EQUAL_INT16(0, lttGetSlewedTrimUnits(0, fuelTable, 100U, 3000U));
}

static void test_authority_clamps_applied_trim(void) {
  lttResetForTest();
  lttBlock &page = lttGetPageMutable();

  // A hand-edited trim larger than the declared authority must be clamped on the way out.
  for (uint16_t c = 0; c < LTT_CELL_COUNT; ++c) {
    page.trim[0][c] = 900; // +90%
  }
  setAllLttAuthority(25U);

  const int16_t applied = lttGetSlewedTrimUnits(0, fuelTable, 100U, 3000U);
  TEST_ASSERT_LESS_OR_EQUAL(250, applied);   // 25 percent => 250 in 0.1% units
  TEST_ASSERT_GREATER_OR_EQUAL(-250, applied);
}

// =====================================================================================
// Slew
// =====================================================================================

static void test_slew_is_rate_limited(void) {
  lttResetForTest();
  lttBlock &page = lttGetPageMutable();

  setAllLttAuthority(100U);
  page.settings.slewUnitsPerSec = 100U; // 10.0% per second

  for (uint16_t c = 0; c < LTT_CELL_COUNT; ++c) {
    page.trim[0][c] = 1000; // +100%
  }
  lttUpdateSlew(100U);

  // 10.0%/s for 0.1s => at most 1.0% = 10 units of movement.
  const int16_t applied = lttGetSlewedTrimUnits(0, fuelTable, 100U, 3000U);
  TEST_ASSERT_LESS_OR_EQUAL(10, applied);
  TEST_ASSERT_GREATER_THAN(0, applied);
}

static void test_slew_disabled_tracks_target(void) {
  lttResetForTest();
  lttBlock &page = lttGetPageMutable();

  setAllLttAuthority(100U);
  page.settings.slewUnitsPerSec = 0U; // no slew => apply immediately

  for (uint16_t c = 0; c < LTT_CELL_COUNT; ++c) {
    page.trim[0][c] = 200; // +20.0%
  }
  lttUpdateSlew(10U);

  // Every cell shares the same value, so the interpolated result is exactly that value.
  TEST_ASSERT_EQUAL_INT16(200, lttGetSlewedTrimUnits(0, fuelTable, 100U, 3000U));
}

// =====================================================================================
// Learn gates
// =====================================================================================

/** @brief Put the status into a state where every learn gate is clear. */
static void setup_learn_permitted(void) {
  lttResetForTest();
  populate_ve_axes();
  lttBlock &page = lttGetPageMutable();
  page.settings.mode = LTT_MODE_LEARN;
  page.settings.learnRateDiv = 1U;
  page.settings.deadbandUnits = 2U;
  page.settings.minRPMDiv100 = 10U;
  page.settings.minSamples = 0U;
  page.settings.maxStepUnits = 0U;
  setAllLttAuthority(100U);

  configPage6.egoType = EGO_TYPE_WIDE;
  configPage6.egoTemp = 40U;
  configPage6.ego_sdelay = 2U;
  configPage6.ego_min = 50U;
  configPage6.ego_max = 200U;
  configPage9.egoMAPMin = 0U;
  configPage9.egoMAPMax = 200U;
  configPage10.fuel2Mode = FUEL2_MODE_OFF;

  currentStatus.rotationStatus = EngineRotationStatus::Running;
  currentStatus.aseIsActive = false;
  currentStatus.wueIsActive = false;
  currentStatus.isAcceleratingTPS = false;
  currentStatus.isDeceleratingTPS = false;
  currentStatus.isDFCOActive = false;
  currentStatus.AEamount = 100U;
  currentStatus.coolant = temperatureRemoveOffset(configPage6.egoTemp) + 10;
  currentStatus.runSecs = configPage6.ego_sdelay + 10U;
  currentStatus.O2 = 100U;
  currentStatus.afrTarget = 100U;
  currentStatus.MAP = 100U;
  currentStatus.setRpm(3000U);
  currentStatus.fuelLoad = 100U;
  currentStatus.secondFuelTableActive = false;

  currentStatus.egoCorrection = 10; // a clear lean error, well outside the dead-band
}

/** @brief The largest trim anywhere in table 1.
 *
 * Tests assert on the maximum rather than on a specific cell, because which cell a given
 * (RPM, load) resolves to depends on the VE table's bin values, which this suite does not
 * control. "Some cell moved" / "no cell moved" is the property under test anyway.
 */
static int16_t maxTrim(void) {
  int16_t result = 0;
  for (uint16_t c = 0; c < LTT_CELL_COUNT; ++c) {
    if (lttGetPage().trim[0][c] > result) {
      result = lttGetPage().trim[0][c];
    }
  }
  return result;
}

static void test_learn_runs_when_every_gate_is_clear(void) {
  setup_learn_permitted();
  lttLearn();
  TEST_ASSERT_GREATER_THAN(0, maxTrim());
}

static void test_learn_refused_when_mode_off(void) {
  setup_learn_permitted();
  lttGetPageMutable().settings.mode = LTT_MODE_OFF;
  lttLearn();
  TEST_ASSERT_EQUAL_INT16(0, maxTrim());
}

static void test_learn_refused_narrowband(void) {
  setup_learn_permitted();
  configPage6.egoType = EGO_TYPE_NARROW;
  lttLearn();
  TEST_ASSERT_EQUAL_INT16(0, maxTrim());
}

static void test_learn_refused_not_running(void) {
  setup_learn_permitted();
  currentStatus.rotationStatus = EngineRotationStatus::Cranking;
  lttLearn();
  TEST_ASSERT_EQUAL_INT16(0, maxTrim());
}

static void test_learn_refused_during_ase(void) {
  setup_learn_permitted();
  currentStatus.aseIsActive = true;
  lttLearn();
  TEST_ASSERT_EQUAL_INT16(0, maxTrim());
}

static void test_learn_refused_during_wue(void) {
  setup_learn_permitted();
  currentStatus.wueIsActive = true;
  lttLearn();
  TEST_ASSERT_EQUAL_INT16(0, maxTrim());
}

static void test_learn_refused_while_accelerating(void) {
  setup_learn_permitted();
  currentStatus.isAcceleratingTPS = true;
  lttLearn();
  TEST_ASSERT_EQUAL_INT16(0, maxTrim());
}

static void test_learn_refused_while_decelerating(void) {
  setup_learn_permitted();
  currentStatus.isDeceleratingTPS = true;
  lttLearn();
  TEST_ASSERT_EQUAL_INT16(0, maxTrim());
}

static void test_learn_refused_during_dfco(void) {
  setup_learn_permitted();
  currentStatus.isDFCOActive = true;
  lttLearn();
  TEST_ASSERT_EQUAL_INT16(0, maxTrim());
}

static void test_learn_refused_during_accel_enrichment(void) {
  setup_learn_permitted();
  currentStatus.AEamount = 150U; // above the no-enrichment value of 100
  lttLearn();
  TEST_ASSERT_EQUAL_INT16(0, maxTrim());
}

static void test_learn_refused_when_cold(void) {
  setup_learn_permitted();
  currentStatus.coolant = temperatureRemoveOffset(configPage6.egoTemp) - 10;
  lttLearn();
  TEST_ASSERT_EQUAL_INT16(0, maxTrim());
}

static void test_learn_refused_before_ego_delay(void) {
  setup_learn_permitted();
  currentStatus.runSecs = configPage6.ego_sdelay - 1U;
  lttLearn();
  TEST_ASSERT_EQUAL_INT16(0, maxTrim());
}

static void test_learn_refused_when_O2_out_of_range_high(void) {
  setup_learn_permitted();
  currentStatus.O2 = configPage6.ego_max + 1U;
  lttLearn();
  TEST_ASSERT_EQUAL_INT16(0, maxTrim());
}

static void test_learn_refused_when_O2_out_of_range_low(void) {
  setup_learn_permitted();
  currentStatus.O2 = configPage6.ego_min - 1U;
  lttLearn();
  TEST_ASSERT_EQUAL_INT16(0, maxTrim());
}

static void test_learn_refused_when_MAP_out_of_range_high(void) {
  setup_learn_permitted();
  currentStatus.MAP = (uint16_t)(configPage9.egoMAPMax * 2U) + 10U;
  lttLearn();
  TEST_ASSERT_EQUAL_INT16(0, maxTrim());
}

static void test_learn_refused_when_MAP_out_of_range_low(void) {
  setup_learn_permitted();
  configPage9.egoMAPMin = 50U; // 100 kPa after the x2 scaling
  currentStatus.MAP = 10U;
  lttLearn();
  TEST_ASSERT_EQUAL_INT16(0, maxTrim());
}

static void test_learn_refused_below_min_rpm(void) {
  setup_learn_permitted();
  currentStatus.setRpm(500U); // below the 1000 RPM default floor
  lttLearn();
  TEST_ASSERT_EQUAL_INT16(0, maxTrim());
}

static void test_learn_refused_inside_deadband(void) {
  setup_learn_permitted();
  // The smallest representable error is 1 whole percent = 10 units, so widen the dead-band
  // rather than trying to express a sub-unit error.
  lttGetPageMutable().settings.deadbandUnits = 100U;
  currentStatus.egoCorrection = 5; // 50 units, inside a 100-unit dead-band
  lttLearn();
  TEST_ASSERT_EQUAL_INT16(0, maxTrim());
}

static void test_learn_respects_min_samples(void) {
  setup_learn_permitted();
  lttGetPageMutable().settings.minSamples = 5U;

  // A cell may not move until it has been seen `minSamples` times. The counter increments on
  // each refused step, so the move happens on update minSamples+1 - not minSamples.
  for (uint8_t i = 0; i < 5U; ++i) {
    lttLearn();
    TEST_ASSERT_EQUAL_INT16_MESSAGE(0, maxTrim(), "cell moved before minSamples was reached");
  }
  lttLearn();
  TEST_ASSERT_GREATER_THAN(0, maxTrim());
}

static void test_learn_respects_max_step(void) {
  setup_learn_permitted();
  lttBlock &page = lttGetPageMutable();
  page.settings.learnRateDiv = 1U;  // 1/2 of the error
  page.settings.maxStepUnits = 20U; // cap at 2.0% per update

  currentStatus.egoCorrection = 100; // 1000 units; half of that would be 500 uncapped
  lttLearn();

  TEST_ASSERT_LESS_OR_EQUAL(20, maxTrim());
  TEST_ASSERT_GREATER_THAN(0, maxTrim());
}

static void test_learn_never_exceeds_cell_authority(void) {
  setup_learn_permitted();
  lttGetPageMutable().settings.maxStepUnits = 0U;

  setAllLttAuthority(5U); // only 5% of addition allowed anywhere

  for (uint8_t i = 0; i < 50U; ++i) {
    lttLearn();
  }

  TEST_ASSERT_LESS_OR_EQUAL(50, maxTrim()); // 5.0% in 0.1% units
}

static void test_learn_marks_dirty(void) {
  setup_learn_permitted();
  TEST_ASSERT_FALSE(lttIsDirty());
  lttLearn();
  TEST_ASSERT_TRUE(lttIsDirty());
}

// =====================================================================================
// Persistence
// =====================================================================================

static void test_save_clears_dirty(void) {
  setup_learn_permitted();
  lttLearn();
  TEST_ASSERT_TRUE(lttIsDirty());

  TEST_ASSERT_TRUE(lttSave());
  TEST_ASSERT_FALSE(lttIsDirty());
}

static void test_saveIfRequired_needs_zero_rpm(void) {
  setup_learn_permitted();
  lttLearn();
  TEST_ASSERT_TRUE(lttIsDirty());

  currentStatus.RPM = 3000U;
  lttSaveIfRequired();
  TEST_ASSERT_TRUE(lttIsDirty());

  currentStatus.RPM = 0U;
  lttSaveIfRequired();
  TEST_ASSERT_FALSE(lttIsDirty());
}

static void test_saveIfRequired_does_nothing_when_clean(void) {
  setup_learn_permitted();
  TEST_ASSERT_FALSE(lttIsDirty());
  currentStatus.RPM = 0U;

  lttSaveIfRequired();
  TEST_ASSERT_FALSE(lttIsDirty());
}

// =====================================================================================
// Block layout contract
// =====================================================================================

static void test_block_fits_in_storage(void) {
  TEST_ASSERT_GREATER_OR_EQUAL(0x1000, EEPROM_LTT_BASE);
  TEST_ASSERT_LESS_OR_EQUAL(0x5D00U, (uint32_t)EEPROM_LTT_BASE + (uint32_t)LTT_BLOCK_SIZE);
}

static void test_page_layout_matches_reported_size(void) {
  TEST_ASSERT_EQUAL(LTT_PAYLOAD_SIZE, sizeof(lttBlock));
  TEST_ASSERT_EQUAL(2064, LTT_PAYLOAD_SIZE);
}

void testLtt(void)
{
  SET_UNITY_FILENAME() {
    RUN_TEST(test_applyTrim_zero_is_bit_exact);
    RUN_TEST(test_applyTrim_positive_is_linear);
    RUN_TEST(test_applyTrim_negative_is_linear);
    RUN_TEST(test_applyTrim_rounds_to_nearest);
    RUN_TEST(test_applyTrim_never_returns_negative_or_wraps);
    RUN_TEST(test_unitsToPercent);
    RUN_TEST(test_fuelMultiplierFloor);
    RUN_TEST(test_defaults_are_zero_trim_and_bounded_authority);
    RUN_TEST(test_authority_is_asymmetric_per_cell);
    RUN_TEST(test_zero_authority_applies_no_trim);
    RUN_TEST(test_authority_clamps_applied_trim);
    RUN_TEST(test_slew_is_rate_limited);
    RUN_TEST(test_slew_disabled_tracks_target);
    RUN_TEST(test_learn_runs_when_every_gate_is_clear);
    RUN_TEST(test_learn_refused_when_mode_off);
    RUN_TEST(test_learn_refused_narrowband);
    RUN_TEST(test_learn_refused_not_running);
    RUN_TEST(test_learn_refused_during_ase);
    RUN_TEST(test_learn_refused_during_wue);
    RUN_TEST(test_learn_refused_while_accelerating);
    RUN_TEST(test_learn_refused_while_decelerating);
    RUN_TEST(test_learn_refused_during_dfco);
    RUN_TEST(test_learn_refused_during_accel_enrichment);
    RUN_TEST(test_learn_refused_when_cold);
    RUN_TEST(test_learn_refused_before_ego_delay);
    RUN_TEST(test_learn_refused_when_O2_out_of_range_high);
    RUN_TEST(test_learn_refused_when_O2_out_of_range_low);
    RUN_TEST(test_learn_refused_when_MAP_out_of_range_high);
    RUN_TEST(test_learn_refused_when_MAP_out_of_range_low);
    RUN_TEST(test_learn_refused_below_min_rpm);
    RUN_TEST(test_learn_refused_inside_deadband);
    RUN_TEST(test_learn_respects_min_samples);
    RUN_TEST(test_learn_respects_max_step);
    RUN_TEST(test_learn_never_exceeds_cell_authority);
    RUN_TEST(test_learn_marks_dirty);
    RUN_TEST(test_save_clears_dirty);
    RUN_TEST(test_saveIfRequired_needs_zero_rpm);
    RUN_TEST(test_saveIfRequired_does_nothing_when_clean);
    RUN_TEST(test_block_fits_in_storage);
    RUN_TEST(test_page_layout_matches_reported_size);
  }
}
