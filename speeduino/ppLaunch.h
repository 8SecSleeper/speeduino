#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config_pages.h"
#include "load_source.h"

/**
 * @file
 * @brief Launch control: one shared X axis per mode, and a separate reactions table.
 *
 * This is NOT the classic 2-step. That lives on pages 6 and 10
 * (`src/controllers/launch/launchController.cpp`) and is untouched by this module.
 *
 * @section one-axis One X axis, several rows tied to it
 *
 * The single most important structural decision here: each mode has **ONE array of 32
 * breakpoints**, and every row is evaluated against that same array. The speed page is therefore
 * a 3-row table sharing one speed axis:
 *
 *     row 1  reference time   (ms)   - when the car SHOULD reach this speed
 *     row 2  retard          (deg)   - maximum retard available at this speed
 *     row 3  speed        (mph x10)  - THE X AXIS, shared by both rows above
 *
 * That is the shape a tuner reads: pick a speed column, and the three rows tell you what should
 * be happening there. Three independent curves with three separate axes - which is what an
 * earlier draft of this module did - cannot be read that way at all: the tuner has to line up
 * breakpoints by eye across three plots, and nothing keeps them in step.
 *
 * The rpm page has the same shape with RPM as the X axis instead of speed.
 *
 * @section simultaneous Both modes run at once
 *
 * Speed mode and RPM mode are independent controllers with their own arm input, clock and tables,
 * and they are intended to be run **together**: the speed page shapes the launch, the rpm page
 * provides a hard ceiling. Neither page carries a copy of the other's table, because a tuner
 * editing their speed curve should not have to scroll past an RPM curve they are not using. The
 * hard limit that actually applies is whichever of the two is lower.
 *
 * @section softretard Linear retard from soft limit to hard limit
 *
 * Retard does NOT snap to its full value when the soft limit is crossed. The soft zone is a
 * PERCENT of the setpoint (10% of 6000 rpm = the band starts at 5400), and the retard is
 * interpolated linearly across it:
 *
 *     at the soft limit   -> ~0 degrees
 *     halfway through     -> half the table's retard
 *     at the hard limit   -> the table's full retard
 *
 * A step change in advance is indistinguishable from a misfire, and it makes the car feel like it
 * cut rather than like it is being managed.
 *
 * @section strategy What happens when retard was not enough
 *
 * `limiterStrategy` is a bitmask, so a tuner can combine actions: reduce boost, cut fuel, cut
 * spark, in any combination. Retard is always the first response; the strategy is what takes over
 * at the hard limit.
 *
 * @section reactions Reactions live on their own page
 *
 * What a launch does about a hard limit being hit for a sustained period is a different question
 * from where the limit is, so it gets its own page (21) and its own menu entry - see
 * @ref ppLaunchReactConfig. Piling reactions into the same dialog as the curves is what makes a
 * launch tab unreadable.
 */

// =====================================================================================
// Block identity
// =====================================================================================

/** @brief Base address of the speed-mode block. */
constexpr uint16_t EEPROM_PPLAUNCH_SPEED_BASE = 0x1930;

/** @brief Base address of the RPM-mode block, immediately after the speed-mode block. */
constexpr uint16_t EEPROM_PPLAUNCH_RPM_BASE = 0x1B40;

/** @brief Base address of the reactions block, immediately after the RPM-mode block. */
constexpr uint16_t EEPROM_PPLAUNCH_REACT_BASE = 0x1D50;

/** @brief 'PPLS' - speed mode. */
constexpr uint32_t EEPROM_PPLAUNCH_SPEED_MAGIC = 0x504C5053;

/** @brief 'PPLR' - RPM mode. A separate magic so a block copied between pages is rejected
 *  rather than silently reinterpreted with the other mode's meaning. */
constexpr uint32_t EEPROM_PPLAUNCH_RPM_MAGIC = 0x504C5052;

/** @brief 'PPLX' - reactions. */
constexpr uint32_t EEPROM_PPLAUNCH_REACT_MAGIC = 0x504C504C;

/**
 * @brief Layout version, shared by all three blocks. Bump whenever a payload's layout, a field
 * width, or a field's UNIT changes.
 *
 * The CRC validates integrity, not meaning: a block written by an older build with the same
 * payload size will pass both the size assertions and its own CRC, and only the *meaning* will be
 * wrong. The version byte is the only thing that distinguishes "older build" from "corrupt".
 */
constexpr uint8_t EEPROM_PPLAUNCH_VERSION = 1;

/** @brief Size of a block header, in bytes. Must stay a multiple of 2 for alignment. */
constexpr uint16_t EEPROM_PPLAUNCH_HEADER_SIZE = 16;

/** @brief Number of points on the mode curves. */
constexpr uint8_t PP_CURVE_POINTS = 32;

/** @brief Number of points on the reactions table.
 *
 * Fixed at 6 because the axis is fixed at 0..500 ms in 100 ms steps. A sustained-limit reaction is
 * a short, predictable ramp; making the breakpoints editable would add a second thing to get
 * wrong without adding anthing a tuner would use.
 */
constexpr uint8_t PP_REACTION_POINTS = 6;

/** @brief The fixed reactions time axis, milliseconds. */
constexpr uint16_t PP_REACTION_TIME_MS[PP_REACTION_POINTS] = { 0U, 100U, 200U, 300U, 400U, 500U };

/** @brief Size of the user-editable curve label, including its NUL terminator. */
constexpr uint8_t PP_LABEL_LEN = 20;

/** @brief Payload size, shared by all three pages. A whole TunerStudio page. */
constexpr uint16_t PPLAUNCH_PAYLOAD_SIZE = 512;

/** @brief Total size of one block including its header. */
constexpr uint16_t PPLAUNCH_BLOCK_SIZE = EEPROM_PPLAUNCH_HEADER_SIZE + PPLAUNCH_PAYLOAD_SIZE;

/** @brief EEPROM address of each block's TunerStudio-visible payload. */
constexpr uint16_t EEPROM_PPLAUNCH_SPEED_PAYLOAD =
    EEPROM_PPLAUNCH_SPEED_BASE + EEPROM_PPLAUNCH_HEADER_SIZE;
constexpr uint16_t EEPROM_PPLAUNCH_RPM_PAYLOAD =
    EEPROM_PPLAUNCH_RPM_BASE + EEPROM_PPLAUNCH_HEADER_SIZE;
constexpr uint16_t EEPROM_PPLAUNCH_REACT_PAYLOAD =
    EEPROM_PPLAUNCH_REACT_BASE + EEPROM_PPLAUNCH_HEADER_SIZE;

/** @brief Miles per hour to kilometres per hour, as a fraction (1 mph = 1.609344 km/h exactly).
 *
 * The 32-bit numerator is mandatory: 160934 truncates to 29862 in a uint16_t, and a 10 mph
 * threshold then becomes 2 km/h. That compiles cleanly and is simply wrong.
 */
constexpr uint32_t PP_MPH_TO_KMH_NUM = 160934U;
constexpr uint32_t PP_MPH_TO_KMH_DEN = 100000U;
static_assert(PP_MPH_TO_KMH_NUM > 65535U,
              "PP_MPH_TO_KMH_NUM must be 32-bit; a 16-bit type truncates it silently");

// =====================================================================================
// Enumerations
// =====================================================================================

/** @brief Which physical quantity drives the axis. */
enum PpAxisSource : uint8_t {
  PP_AXIS_VSS_MPH10 = 0, ///< `currentStatus.vss` converted to mph x10 (tenths of a mph)
  PP_AXIS_VSS_KPH10 = 1, ///< `currentStatus.vss` x10, i.e. tenths of a km/h
  PP_AXIS_SHAFT_RPM = 2, ///< Driveshaft RPM, derived from vss x @ref ppLaunchSpeedConfig::shaftRevPerKph
  PP_AXIS_RPM = 3,       ///< Engine RPM
};

/// @name `limiterStrategy` bits - what happens at the HARD limit.
///
/// A bitmask rather than an enum because these combine: a tuner may want boost reduction AND a
/// spark cut, or fuel and spark together. Retard is not in here - it is always the first
/// response, applied progressively across the soft zone, and never optional.
/// @{
constexpr uint8_t PP_STRAT_BOOST_REDUCE = 0x01U; ///< Lower the boost target / trim duty
constexpr uint8_t PP_STRAT_FUEL_CUT = 0x02U;     ///< Cut fuel
constexpr uint8_t PP_STRAT_SPARK_CUT = 0x04U;    ///< Cut spark
constexpr uint8_t PP_STRAT_ROLLING = 0x08U;      ///< Probabilistic rolling cut instead of a full cut
/// @}

/// @name `featureFlags` bits (page 18)
/// @{
constexpr uint8_t PP_FLAG_PERFECT_PASS = 0x01U; ///< Use rowTimeMs as a reference trajectory
constexpr uint8_t PP_FLAG_RETARD_SLEW = 0x02U;  ///< Rate-limit retard changes
constexpr uint8_t PP_FLAG_GEAR_GATE = 0x04U;    ///< Only launch in gears up to `maxGear`
constexpr uint8_t PP_FLAG_AXIS_CLAMP = 0x08U;   ///< Reject a single wild axis reading
/// @}

/** @brief What the X axis means, so the firmware can convert `currentStatus.vss` correctly. */
enum PpCurveMode : uint8_t {
  PP_CURVE_BY_AXIS = 0, ///< Normal: look the tables up by the live axis value
  PP_CURVE_BY_TIME = 1, ///< Look up rowRetard by elapsed time instead (rarely useful)
};

/** @brief The state machine state. Reported to TunerStudio so the log explains what happened -
 *  the single biggest usability gap in competing speed-based launch implementations. */
enum PpState : uint8_t {
  PP_STATE_OFF = 0,       ///< Feature disabled
  PP_STATE_IDLE = 1,      ///< Enabled, waiting for the arm input
  PP_STATE_ARMED = 2,     ///< Armed, clock at zero, waiting for the release edge
  PP_STATE_LAUNCHING = 3, ///< Released; clock running, limits and retard live
  PP_STATE_COMPLETE = 4,  ///< Past the end of the curve; output returned to normal
  PP_STATE_TIMEOUT = 5,   ///< Armed for longer than `timeoutMs` without a launch
};

/** @brief The three pages this module owns, as a selector. */
enum PpMode : uint8_t {
  PP_MODE_SPEED = 0, ///< Page 18
  PP_MODE_RPM = 1,   ///< Page 19
  PP_MODE_REACT = 2, ///< Page 21
};

// =====================================================================================
// Page 18: the speed-based launch controller
// =====================================================================================

/**
 * @brief Persistent settings for the speed-based launch mode.
 *
 * Plain fixed-width fields, no bit-fields. This struct IS a raw TunerStudio page, so its offsets
 * are part of the tuning-file ABI; the `static_assert`s below pin every one of them.
 */
struct ppLaunchSpeedConfig : public config_page_t {
  // ---- 0..15: scalars -----------------------------------------------------------------
  uint8_t enable;           ///< 0 = off, 1 = on
  uint8_t armPin;           ///< Input pin for the arm / launch switch. 0 = none configured.
  uint8_t armPolarity;      ///< 0 = asserted HIGH, 1 = asserted LOW
  uint8_t armPullup;        ///< 0 = plain INPUT, 1 = INPUT_PULLUP
  uint8_t axisSource;       ///< @ref PpAxisSource
  uint8_t limiterStrategy;  ///< @ref PP_STRAT_* bitmask - what happens at the hard limit
  uint8_t curveMode;        ///< @ref PpCurveMode
  uint8_t featureFlags;     ///< @ref PP_FLAG_* bitmask

  uint16_t timeoutMs;       ///< Armed timeout, ms. 0 = never time out.
  uint16_t rearmBelowAxis;  ///< Re-arm once the axis drops below this. 0 = re-arm on arm pin only.
  uint16_t startBelowAxis;  ///< The limiter may only engage while the axis is below this.

  /// Soft zone, as a PERCENT of the setpoint. 10 = the band starts at 90% of the limit.
  ///
  /// A percentage rather than an absolute offset so that it scales: 10% of 6000 rpm is a 600 rpm
  /// band and 10% of 40 mph is a 4 mph band, which is the same relationship a driver feels. An
  /// absolute "400 rpm" figure only makes sense at one operating point.
  uint8_t softZonePercent;

  uint8_t limitHysteresis;  ///< Axis units of hysteresis on the limiter release
  uint8_t maxGear;          ///< Highest gear the limiter may engage in. 0 = no gear gating.
  uint8_t axisFilterShift;  ///< IIR shift for the axis (0 = off, 3 = /8 smoothing)

  uint16_t axisAccelLimit;   ///< Max axis change per update when PP_FLAG_AXIS_CLAMP is set.
  uint16_t retardSlewMsPerDeg; ///< Max retard change, MILLISECONDS PER DEGREE. 0 = no slew.
  uint16_t shaftRevPerKph;   ///< Driveshaft revolutions per km/h x100, for PP_AXIS_SHAFT_RPM.

  // ---- 24..47: label and padding -------------------------------------------------------
  uint8_t label[PP_LABEL_LEN]; ///< Editable X-axis label, NUL terminated
  uint8_t reserved0[4];        ///< Reserved, must be 0. Keeps the tables 2-byte aligned at 48.

  // ---- 48..239: THE TABLE - one X axis, two rows tied to it ---------------------------
  /// Row 3 in the editor: the shared X axis. Speed in the unit selected by `axisSource`
  /// (tenths of a mph, tenths of a km/h, or driveshaft RPM). Forced strictly ascending by
  /// clampSpeedSettings(): it is the lookup key for BOTH rows above it.
  uint16_t axisSpeed[PP_CURVE_POINTS];

  /// Row 2 in the editor: the MAXIMUM retard available at this speed, in tenths of a degree.
  ///
  /// "Maximum" because the actual retard is interpolated across the soft zone: at the soft limit
  /// it is ~0, and it reaches this value only at the hard limit. A tuner reading this row is
  /// answering "how much timing am I prepared to lose if the car gets here".
  uint16_t rowRetard[PP_CURVE_POINTS];

  /// Row 1 in the editor: the reference time at this speed, milliseconds since the release.
  ///
  /// This is the perfect-pass wire: at this speed, the car should have taken this long. The
  /// deviation between actual elapsed time and this reference is the slip score. Must be
  /// non-decreasing for the wire to be sane, but is not a lookup key, so it is repaired rather
  /// than enforced.
  uint16_t rowTimeMs[PP_CURVE_POINTS];

  // ---- 240..511: reserved --------------------------------------------------------------
  uint8_t reserved1[PPLAUNCH_PAYLOAD_SIZE - 240U];
};

using ppLaunchSpeedBlock = ppLaunchSpeedConfig;

/** @brief `enable` values. */
enum PpEnable : uint8_t {
  PP_DISABLED = 0,
  PP_ENABLED = 1,
};

// ---- Page 18 layout contract ----------------------------------------------------------
//   enable                1 B  @   0      axisAccelLimit        2 B  @  16
//   armPin                1 B  @   1      retardSlewMsPerDeg    2 B  @  18
//   armPolarity           1 B  @   2      shaftRevPerKph        2 B  @  20
//   armPullup             1 B  @   3      label                20 B  @  24
//   axisSource            1 B  @   4      reserved0             4 B  @  44
//   limiterStrategy       1 B  @   5      axisSpeed            64 B  @  48   <- the shared X axis
//   curveMode             1 B  @   6      rowRetard            64 B  @ 112   <- row 2
//   featureFlags          1 B  @   7      rowTimeMs            64 B  @ 176   <- row 1
//   timeoutMs             2 B  @   8      reserved1           272 B  @ 240
//   rearmBelowAxis        2 B  @  10                                  -------
//   startBelowAxis        2 B  @  12      payload             512 B
//   softZonePercent       1 B  @  14
//   limitHysteresis       1 B  @  15
//   maxGear               1 B  @  16
//   axisFilterShift       1 B  @  17
static_assert(offsetof(ppLaunchSpeedConfig, enable) == 0, "enable must be at offset 0");
static_assert(offsetof(ppLaunchSpeedConfig, armPin) == 1, "armPin must be at offset 1");
static_assert(offsetof(ppLaunchSpeedConfig, armPolarity) == 2, "armPolarity must be at offset 2");
static_assert(offsetof(ppLaunchSpeedConfig, armPullup) == 3, "armPullup must be at offset 3");
static_assert(offsetof(ppLaunchSpeedConfig, axisSource) == 4, "axisSource must be at offset 4");
static_assert(offsetof(ppLaunchSpeedConfig, limiterStrategy) == 5, "limiterStrategy must be at offset 5");
static_assert(offsetof(ppLaunchSpeedConfig, curveMode) == 6, "curveMode must be at offset 6");
static_assert(offsetof(ppLaunchSpeedConfig, featureFlags) == 7, "featureFlags must be at offset 7");
static_assert(offsetof(ppLaunchSpeedConfig, timeoutMs) == 8, "timeoutMs must be at offset 8");
static_assert(offsetof(ppLaunchSpeedConfig, rearmBelowAxis) == 10, "rearmBelowAxis must be at offset 10");
static_assert(offsetof(ppLaunchSpeedConfig, startBelowAxis) == 12, "startBelowAxis must be at offset 12");
static_assert(offsetof(ppLaunchSpeedConfig, softZonePercent) == 14, "softZonePercent must be at offset 14");
static_assert(offsetof(ppLaunchSpeedConfig, limitHysteresis) == 15, "limitHysteresis must be at offset 15");
static_assert(offsetof(ppLaunchSpeedConfig, maxGear) == 16, "maxGear must be at offset 16");
static_assert(offsetof(ppLaunchSpeedConfig, axisFilterShift) == 17, "axisFilterShift must be at offset 17");
static_assert(offsetof(ppLaunchSpeedConfig, axisAccelLimit) == 18, "axisAccelLimit must be at offset 18");
static_assert(offsetof(ppLaunchSpeedConfig, retardSlewMsPerDeg) == 20, "retardSlewMsPerDeg must be at offset 20");
static_assert(offsetof(ppLaunchSpeedConfig, shaftRevPerKph) == 22, "shaftRevPerKph must be at offset 22");
static_assert(offsetof(ppLaunchSpeedConfig, label) == 24, "label must be at offset 24");
static_assert(offsetof(ppLaunchSpeedConfig, reserved0) == 44, "reserved0 must be at offset 44");
static_assert(offsetof(ppLaunchSpeedConfig, axisSpeed) == 48, "axisSpeed must be at offset 48");
static_assert(offsetof(ppLaunchSpeedConfig, rowRetard) == 112, "rowRetard must be at offset 112");
static_assert(offsetof(ppLaunchSpeedConfig, rowTimeMs) == 176, "rowTimeMs must be at offset 176");
static_assert(offsetof(ppLaunchSpeedConfig, reserved1) == 240, "reserved1 must be at offset 240");
static_assert(sizeof(ppLaunchSpeedConfig) == PPLAUNCH_PAYLOAD_SIZE,
              "ppLaunchSpeedConfig size changed - update pageSize in speeduino.ini");

// =====================================================================================
// Page 19: the RPM-based launch controller - same shape, RPM as the X axis
// =====================================================================================

/**
 * @brief Persistent settings for the RPM-based launch mode.
 *
 * The same 3-row shape as page 18, with engine RPM as the shared X axis. That symmetry is
 * deliberate: a tuner who has read one page has read both, and the only difference is which
 * quantity the columns are.
 *
 * Unlike an earlier draft, this page does NOT have a time axis of its own and does NOT own a
 * standalone RPM limit versus time. The table is limit-versus-rpm, which is what "how much RPM am
 * I allowed once the car is at this RPM" means; the reference-time row supplies the time domain.
 */
struct ppLaunchRpmConfig : public config_page_t {
  // ---- 0..9: scalars -------------------------------------------------------------------
  uint8_t enable;           ///< 0 = off, 1 = on
  uint8_t armPin;           ///< Own arm input, independent of page 18
  uint8_t armPolarity;      ///< 0 = asserted HIGH, 1 = asserted LOW
  uint8_t armPullup;        ///< 0 = plain INPUT, 1 = INPUT_PULLUP
  uint8_t limiterStrategy;  ///< @ref PP_STRAT_* bitmask
  uint8_t reserved0;        ///< Reserved, must be 0.

  uint16_t timeoutMs;       ///< Armed timeout, ms. 0 = never time out.

  uint8_t softZonePercent;  ///< Soft zone as a PERCENT of the setpoint
  uint8_t limitHysteresis;  ///< RPM of hysteresis on the limiter release

  // ---- 10..31: label and padding -------------------------------------------------------
  uint8_t label[PP_LABEL_LEN]; ///< Editable X-axis label, NUL terminated
  uint8_t reserved1[2];        ///< Reserved, must be 0. Keeps the tables 2-byte aligned at 32.

  // ---- 32..223: THE TABLE --------------------------------------------------------------
  /// Row 3 in the editor: the shared X axis, whole engine RPM. Strictly ascending.
  uint16_t axisValue[PP_CURVE_POINTS];

  /// Row 2 in the editor: the MAXIMUM retard at this RPM, tenths of a degree. Interpolated
  /// across the soft zone exactly as on page 18.
  uint16_t rowRetard[PP_CURVE_POINTS];

  /// Row 1 in the editor: the reference time at this RPM, milliseconds since the release.
  uint16_t rowTimeMs[PP_CURVE_POINTS];

  // ---- 224..511: reserved --------------------------------------------------------------
  uint8_t reserved2[PPLAUNCH_PAYLOAD_SIZE - 224U];
};

using ppLaunchRpmBlock = ppLaunchRpmConfig;

// ---- Page 19 layout contract ----------------------------------------------------------
//   enable                1 B  @   0      label                20 B  @  10
//   armPin                1 B  @   1      reserved1             2 B  @  30
//   armPolarity           1 B  @   2      axisValue            64 B  @  32   <- the shared X axis
//   armPullup             1 B  @   3      rowRetard            64 B  @  96   <- row 2
//   limiterStrategy       1 B  @   4      rowTimeMs            64 B  @ 160   <- row 1
//   reserved0             1 B  @   5      reserved2           288 B  @ 224
//   timeoutMs             2 B  @   6                                  -------
//   softZonePercent       1 B  @   8      payload             512 B
//   limitHysteresis       1 B  @   9
static_assert(offsetof(ppLaunchRpmConfig, enable) == 0, "rpm enable must be at offset 0");
static_assert(offsetof(ppLaunchRpmConfig, armPin) == 1, "rpm armPin must be at offset 1");
static_assert(offsetof(ppLaunchRpmConfig, armPolarity) == 2, "rpm armPolarity must be at offset 2");
static_assert(offsetof(ppLaunchRpmConfig, armPullup) == 3, "rpm armPullup must be at offset 3");
static_assert(offsetof(ppLaunchRpmConfig, limiterStrategy) == 4, "rpm limiterStrategy must be at offset 4");
static_assert(offsetof(ppLaunchRpmConfig, timeoutMs) == 6, "rpm timeoutMs must be at offset 6");
static_assert(offsetof(ppLaunchRpmConfig, softZonePercent) == 8, "rpm softZonePercent must be at offset 8");
static_assert(offsetof(ppLaunchRpmConfig, limitHysteresis) == 9, "rpm limitHysteresis must be at offset 9");
static_assert(offsetof(ppLaunchRpmConfig, label) == 10, "rpm label must be at offset 10");
static_assert(offsetof(ppLaunchRpmConfig, reserved1) == 30, "rpm reserved1 must be at offset 30");
static_assert(offsetof(ppLaunchRpmConfig, axisValue) == 32, "rpm axisValue must be at offset 32");
static_assert(offsetof(ppLaunchRpmConfig, rowRetard) == 96, "rpm rowRetard must be at offset 96");
static_assert(offsetof(ppLaunchRpmConfig, rowTimeMs) == 160, "rpm rowTimeMs must be at offset 160");
static_assert(offsetof(ppLaunchRpmConfig, reserved2) == 224, "rpm reserved2 must be at offset 224");
static_assert(sizeof(ppLaunchRpmConfig) == PPLAUNCH_PAYLOAD_SIZE,
              "ppLaunchRpmConfig size changed - update pageSize in speeduino.ini");

// =====================================================================================
// Page 21: reactions - what to do when the hard limit was hit for a sustained time
// =====================================================================================

/**
 * @brief Persistent settings for the launch reactions.
 *
 * A separate page and a separate menu entry, because it answers a different question. Page 18/19
 * decide *where* the limits are; this page decides *what happens* when the limits were not
 * enough. Keeping them in the same dialog is what makes a launch tab unreadable, and it also
 * lets a tuner change their reaction strategy without touching a curve.
 *
 * The axis is FIXED at 0, 100, 200, 300, 400, 500 ms of accumulated time at the hard limit, and
 * every column is a "how much" for that duration. Six fixed points, because a sustained-limit
 * reaction is a short predictable ramp and editable breakpoints would add a second thing to get
 * wrong without adding anything a tuner would use.
 *
 * Every column is deliberately ADDITIVE to the tune rather than absolute: "add 8% pulse width",
 * "pull 6 kPa of target", "trim 12% of duty". An absolute value here would silently override a
 * flex or traction modifier that legitimately moved the base.
 */
struct ppLaunchReactConfig : public config_page_t {
  uint8_t enable;           ///< 0 = off, 1 = on
  uint8_t reserved0[3];     ///< Reserved, must be 0.

  uint8_t label[PP_LABEL_LEN]; ///< Editable table label, NUL terminated
  uint8_t reserved1[8];     ///< Reserved, must be 0. Keeps the tables 2-byte aligned at 32.

  // ---- 32..91: the reaction table -------------------------------------------------------
  /// The time axis, ms of accumulated time at the hard limit. Stored rather than implicit so
  /// the ini and the firmware agree by inspection, and so a future revision can shift it without
  /// a struct change. clampReactSettings() forces it to the fixed PP_REACTION_TIME_MS values.
  uint16_t axisTimeMs[PP_REACTION_POINTS];

  /// Fuel pulse-width adder, PERCENT of the current pulse width. Signed: negative removes fuel.
  uint16_t fuelAddPercent[PP_REACTION_POINTS];

  /// Boost duty adjustment, PERCENT of the current duty. Signed so it can add or remove.
  int16_t boostDutyAdj[PP_REACTION_POINTS];

  /// Boost target adjustment, kPa. Signed so it can add or remove.
  int16_t boostTargetAdj[PP_REACTION_POINTS];

  /// Extra retard, tenths of a degree. Signed, and additive with the launch retard.
  int16_t retardAdj[PP_REACTION_POINTS];

  // ---- 92..511: reserved ---------------------------------------------------------------
  uint8_t reserved2[PPLAUNCH_PAYLOAD_SIZE - 92U];
};

using ppLaunchReactBlock = ppLaunchReactConfig;

// ---- Page 21 layout contract ----------------------------------------------------------
//   enable                1 B  @   0      axisTimeMs           12 B  @  32   (6 x uint16)
//   reserved0             3 B  @   1      fuelAddPercent       12 B  @  44
//   label                20 B  @   4      boostDutyAdj         12 B  @  56
//   reserved1             8 B  @  24      boostTargetAdj       12 B  @  68
//   reserved2           420 B  @  92      retardAdj            12 B  @  80
//                                              -------
//                                          payload             512 B
static_assert(offsetof(ppLaunchReactConfig, enable) == 0, "react enable must be at offset 0");
static_assert(offsetof(ppLaunchReactConfig, reserved0) == 1, "react reserved0 must be at offset 1");
static_assert(offsetof(ppLaunchReactConfig, label) == 4, "react label must be at offset 4");
static_assert(offsetof(ppLaunchReactConfig, reserved1) == 24, "react reserved1 must be at offset 24");
static_assert(offsetof(ppLaunchReactConfig, axisTimeMs) == 32, "react axisTimeMs must be at offset 32");
static_assert(offsetof(ppLaunchReactConfig, fuelAddPercent) == 44, "react fuelAddPercent must be at offset 44");
static_assert(offsetof(ppLaunchReactConfig, boostDutyAdj) == 56, "react boostDutyAdj must be at offset 56");
static_assert(offsetof(ppLaunchReactConfig, boostTargetAdj) == 68, "react boostTargetAdj must be at offset 68");
static_assert(offsetof(ppLaunchReactConfig, retardAdj) == 80, "react retardAdj must be at offset 80");
static_assert(offsetof(ppLaunchReactConfig, reserved2) == 92, "react reserved2 must be at offset 92");
static_assert(sizeof(ppLaunchReactConfig) == PPLAUNCH_PAYLOAD_SIZE,
              "ppLaunchReactConfig size changed - update pageSize in speeduino.ini");

// Block placement. The emulated EEPROM is 23,808 bytes (0x0000..0x5CFF). All three blocks must
// sit inside it, above the frozen 0x0000-0x0FFF core tune ABI, clear of the long-term-trim block
// (0x1000..0x181F) and the race-time block (0x1820..0x192F), and clear of the launch-boost block
// (0x1F60..0x237F), and must not overlap each other.
static_assert(EEPROM_PPLAUNCH_SPEED_BASE >= (0x1000U + 16U + 2064U + 16U + 256U),
              "Speed-mode block must start after the long-term-trim and race-time blocks");
static_assert(EEPROM_PPLAUNCH_RPM_BASE == (EEPROM_PPLAUNCH_SPEED_BASE + PPLAUNCH_BLOCK_SIZE),
              "RPM-mode block must immediately follow the speed-mode block");
static_assert(EEPROM_PPLAUNCH_REACT_BASE == (EEPROM_PPLAUNCH_RPM_BASE + PPLAUNCH_BLOCK_SIZE),
              "Reactions block must immediately follow the RPM-mode block");
static_assert((uint32_t)EEPROM_PPLAUNCH_REACT_BASE + (uint32_t)PPLAUNCH_BLOCK_SIZE <= 0x1F60U,
              "The launch-boost block must still start after all three of these");
static_assert(((uint32_t)EEPROM_PPLAUNCH_REACT_BASE + (uint32_t)PPLAUNCH_BLOCK_SIZE) <= 0x5D00U,
              "PP launch blocks do not fit in the emulated EEPROM address space");

/** @brief The live page payloads.
 *
 * Globals rather than file-statics so that pages.cpp can take their addresses in a `constexpr`
 * page map, exactly as it does for every config page.
 */
extern ppLaunchSpeedBlock ppLaunchSpeedData;
extern ppLaunchRpmBlock ppLaunchRpmData;
extern ppLaunchReactBlock ppLaunchReactData;

// =====================================================================================
// Runtime API
// =====================================================================================

/** @brief Load all three blocks at boot and bind both arm inputs. */
void initialisePpLaunch(statuses &current);

/** @brief Restore defaults in RAM only. Called by setTuneToEmpty(). */
void ppLaunchRestoreDefaults(void);

/** @brief Advance both state machines and clocks. Call exactly once per millisecond. */
void ppLaunchTick1ms(statuses &current);

/**
 * @brief Evaluate both modes and publish retard / limit / reaction requests.
 *
 * Both modes run **simultaneously** and independently; the hard limit that applies is the lower
 * of the two. Cheap and safe to call every main-loop pass.
 */
void ppLaunchUpdate(statuses &current, const config2 &page2);

/// @name Results of the last evaluation
/// @{
/** @brief The RPM limit to apply now, whole RPM. UINT16_MAX when neither mode is limiting. */
uint16_t ppLaunchRpmLimit(void);

/** @brief The strategy bits in force now. Zero when not limiting. */
uint8_t ppLaunchStrategy(void);

/** @brief Retard requested now, whole degrees, 0..60. Zero when nothing is engaging. */
uint8_t ppLaunchRetard(void);

/** @brief Extra fuel, percent of pulse width, from the reactions table. */
int16_t ppLaunchFuelAdd(void);

/** @brief Boost duty adjustment, percent, from the reactions table. */
int16_t ppLaunchBoostDutyAdj(void);

/** @brief Boost target adjustment, kPa, from the reactions table. */
int16_t ppLaunchBoostTargetAdj(void);

/** @brief Which mode is currently driving the limit. */
PpMode ppLaunchActiveMode(void);

/** @brief The speed mode's state machine state. */
PpState ppLaunchState(void);

/** @brief The RPM mode's state machine state. */
PpState ppLaunchRpmState(void);

/** @brief Milliseconds since the speed mode's release, 0 when not launching. */
uint32_t ppLaunchElapsedMs(void);

/** @brief The speed-mode axis value the last evaluation used, in the configured unit. */
uint16_t ppLaunchAxisValue(void);

/** @brief Signed deviation from the reference-time wire, milliseconds.
 *
 * Negative means the car arrived LATE (behind the wire); positive means early. So a positive
 * value is the car being ahead of schedule, which on a speed axis is the signature of wheelspin.
 */
int16_t ppLaunchSlipMs(void);

/** @brief True if either mode is limiting right now. */
bool ppLaunchIsLimiting(void);

/** @brief Milliseconds accumulated at the hard limit on the current pass. */
uint16_t ppLaunchLimitHitMs(void);

/** @brief True if either mode is enabled and loaded. */
bool ppLaunchIsActive(void);

/** @brief Force both state machines back to idle and drop every output. */
void ppLaunchReset(void);
/// @}

/// @name TunerStudio / test hooks
/// @{
ppLaunchSpeedBlock &ppLaunchGetSpeedPageMutable(void);
ppLaunchRpmBlock &ppLaunchGetRpmPageMutable(void);
ppLaunchReactBlock &ppLaunchGetReactPageMutable(void);

const ppLaunchSpeedBlock &ppLaunchGetSpeedPage(void);
const ppLaunchRpmBlock &ppLaunchGetRpmPage(void);
const ppLaunchReactBlock &ppLaunchGetReactPage(void);

/** @brief True if the persisted block for that page was found intact at boot. */
bool ppLaunchIsInitialised(PpMode mode);

/** @brief True if the storage backend is large enough to hold all three blocks. */
bool ppLaunchIsAvailable(void);

/** @brief The conditioned speed-mode axis, for logging. */
extern uint16_t ppLaunchAxisLog;

/** @brief Signed deviation from the reference wire, ms, for logging. */
extern int16_t ppLaunchSlipLog;

/** @brief Mark a page dirty so the next stopped-engine poll persists it. */
void ppLaunchMarkDirty(PpMode mode);

/** @brief Force a persist of one block now, regardless of RPM. Used by an explicit TS burn. */
bool ppLaunchSave(PpMode mode);

/**
 * @brief Persist one block, honouring the storage write budget. Called from savePage().
 *
 * `savePage()` clears the "burn pending" flag when the budget reaches zero, and `speeduino.ino()`
 * re-enters `saveAllPages()` for as long as that flag stays set. A page save that ignores the
 * budget and always reports success makes the loop run forever - a flash-destroying bug that is
 * invisible in a unit test that only checks the data round-trips.
 *
 * @return Writes still remaining. Zero means this page is finished.
 */
uint16_t ppLaunchSaveBlock(PpMode mode, uint16_t writesRemaining);

/** @brief True if a block differs from what is stored. */
bool ppLaunchNeedsSave(PpMode mode);
/// @}

#if defined(UNIT_TEST)
/** @brief Reset the module to its power-on state. Unit tests only. */
void ppLaunchResetForTest(void);

/** @brief Inject the axis value, bypassing the sensor path. Unit tests only. */
void ppLaunchSetAxisForTest(uint16_t axis);

/** @brief Inject the arm input level for both modes. Unit tests only. */
void ppLaunchSetArmForTest(bool asserted);
#endif
