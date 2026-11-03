#include "ppLaunch.h"
#include "statuses.h"
#include "storage.h"
#include "globals.h"
#include "maths.h"
#include "torqueRequest.h"
#include "unit_testing.h"
#include "src/pins/pinNumbers_t.h"
#include "src/pins/pinMapping.h"
#include <Arduino.h>
#include <string.h>

/**
 * @file
 * @brief Launch control: state machine, per-mode curve evaluation and persistence.
 *
 * See ppLaunch.h for why the two modes have deliberately different shapes. The properties that
 * are easy to break by accident, restated:
 *
 *  1. Every clock advances only from @ref ppLaunchTick1ms. Anything else that adds to it makes
 *     the elapsed time depend on loop cadence, and every consumer inherits the error.
 *  2. Every output (limit, retard, duty trim) is zero unless the state machine is LAUNCHING.
 *  3. Retard leaves through the torque bus, never by writing `advance`.
 *  4. Nothing here touches storage unless the backend can hold both blocks.
 *
 * Curve evaluation happens on EVERY main-loop pass, deliberately. An earlier draft rate-limited
 * it to 100 Hz "because 32-point interpolations feel expensive"; that is the same mistake this
 * tree already fixed once in launchController, which re-tested a 10 Hz bit and silently capped a
 * drag launch at ~100 ms of lag - longer than the whole first 60 feet. Three interpolations of
 * 32 points is a few hundred comparisons next to a loop that already runs at several kHz.
 */

// =====================================================================================
// The two page payloads
// =====================================================================================

/** @brief Page 18: the speed-based reference-trajectory controller. */
ppLaunchSpeedBlock ppLaunchSpeedData;

/** @brief Page 19: the simple RPM limiter. */
ppLaunchRpmBlock ppLaunchRpmData;

// =====================================================================================
// Runtime state
// =====================================================================================

uint16_t ppLaunchAxisLog = 0U;
int16_t ppLaunchSlipLog = 0;

namespace {

/** @brief Everything that is not configuration, grouped so it resets in one assignment. */
struct RuntimeState {
  uint32_t ms = 0;                 ///< Milliseconds since the launch release. The one clock.
  uint32_t armedMs = 0;            ///< Milliseconds the machine has been armed.
  uint32_t slewMs = 0;             ///< Milliseconds at which the retard slew was last advanced.
  PpState speedState = PP_STATE_OFF; ///< State machine for page 18.
  PpState rpmState = PP_STATE_OFF;   ///< State machine for page 19.
  uint16_t rpmLimit = UINT16_MAX;  ///< Current RPM limit; UINT16_MAX = no limit.
  PpLimitMethod limitMethod = PP_LIMIT_NONE;
  uint16_t retardTenths = 0;       ///< Tenths requested by the curve + slip correction.
  uint16_t retardSlewed = 0;       ///< Tenths actually published after slew limiting.
  uint8_t dutyTrim = 0;            ///< Speed-mode boost duty trim, percent.
  uint16_t axis = 0;               ///< Conditioned axis value (speed mode only).
  int16_t slip = 0;                ///< Signed deviation from the reference wire.
  bool prevArmSpeed = false;       ///< Previous arm level, page 18.
  bool prevArmRpm = false;         ///< Previous arm level, page 19.
  bool limiting = false;           ///< Limiter engaged (with hysteresis applied).
  bool softActive = false;         ///< Soft-retard band engaged (RPM mode).
  bool storageOk = false;          ///< The backend can hold both blocks.
  bool axisSeen = false;           ///< A conditioned axis sample has been taken this attempt.
  uint8_t speedArmPin = 0;         ///< Board pin the page-18 arm input resolved to.
  uint8_t rpmArmPin = 0;           ///< Board pin the page-19 arm input resolved to.
  bool speedPinBound = false;
  bool rpmPinBound = false;
  uint32_t rpmMs = 0;              ///< Page 19's own clock. The two modes arm independently.
  uint32_t rpmArmedMs = 0;         ///< Milliseconds page 19 has been armed.
  uint8_t dirtyMask = 0;           ///< bit0 = speed page, bit1 = rpm page.
  uint8_t initialisedMask = 0;     ///< bit0 = speed block intact at boot, bit1 = rpm block.

  void reset(void)
  {
    ms = 0U;
    armedMs = 0U;
    slewMs = 0U;
    rpmMs = 0U;
    rpmArmedMs = 0U;
    speedState = PP_STATE_OFF;
    rpmState = PP_STATE_OFF;
    rpmLimit = UINT16_MAX;
    limitMethod = PP_LIMIT_NONE;
    retardTenths = 0U;
    retardSlewed = 0U;
    dutyTrim = 0U;
    axis = 0U;
    slip = 0;
    limiting = false;
    softActive = false;
    axisSeen = false;
  }
};

RuntimeState rt;

#if defined(UNIT_TEST)
/// @name Test injection points
/// @{
bool armOverrideActive = false; ///< When set, the arm level comes from armOverrideValue
bool armOverrideValue = false;  ///< The injected arm level
uint16_t axisOverride = 0;      ///< The injected raw axis reading
bool axisOverrideActive = false;///< When set, the axis comes from axisOverride
/// @}
#endif

// =====================================================================================
// Small helpers
// =====================================================================================

bool speedEnabled(void) { return ppLaunchSpeedData.enable == (uint8_t)PP_ENABLED; }
bool rpmEnabled(void) { return ppLaunchRpmData.enable == (uint8_t)PP_ENABLED; }

/** @brief True while the speed page is the one driving the outputs.
 *
 * Both pages are independent features with their own arm inputs, but only one set of outputs
 * exists, so one has to win when both are enabled. The speed page is the more specific
 * controller, so it takes precedence; a tuner who wants only the simple limiter enables only
 * the simple limiter.
 */
bool speedModeActive(void) { return speedEnabled(); }

void markDirty(PpMode mode, bool dirty)
{
  const uint8_t mask = (mode == PP_MODE_RPM) ? 0x02U : 0x01U;
  if (dirty) { rt.dirtyMask = (uint8_t)(rt.dirtyMask | mask); }
  else { rt.dirtyMask = (uint8_t)(rt.dirtyMask & (uint8_t)(~mask)); }
}

bool isDirty(PpMode mode)
{
  return (rt.dirtyMask & ((mode == PP_MODE_RPM) ? 0x02U : 0x01U)) != 0U;
}

uint16_t baseFor(PpMode mode)
{
  return (mode == PP_MODE_RPM) ? EEPROM_PPLAUNCH_RPM_BASE : EEPROM_PPLAUNCH_SPEED_BASE;
}

uint16_t payloadFor(PpMode mode)
{
  return (mode == PP_MODE_RPM) ? EEPROM_PPLAUNCH_RPM_PAYLOAD : EEPROM_PPLAUNCH_SPEED_PAYLOAD;
}

uint32_t magicFor(PpMode mode)
{
  return (mode == PP_MODE_RPM) ? EEPROM_PPLAUNCH_RPM_MAGIC : EEPROM_PPLAUNCH_SPEED_MAGIC;
}

/** @brief km/h x10 to mph x10. 1 mph = 1.609344 km/h, so mph = km/h / 1.609344.
 *
 * The numerator is 32-bit because 160934 does not fit in a uint16_t and truncates to 29862 -
 * which turns a 10 mph threshold into 2 km/h. That compiles cleanly and is simply wrong.
 */
uint16_t kmh10ToMph10(uint16_t kmh10)
{
  return (uint16_t)(((uint32_t)kmh10 * PP_MPH_TO_KMH_DEN) / PP_MPH_TO_KMH_NUM);
}

/** @brief Clamp a tenths-of-a-degree value to the 60 degrees the scheduler can honour. */
uint16_t clampTenths(uint16_t tenths)
{
  return (tenths > 600U) ? 600U : tenths;
}

void clearOutputs(void)
{
  rt.rpmLimit = UINT16_MAX;
  rt.limitMethod = PP_LIMIT_NONE;
  rt.retardTenths = 0U;
  rt.retardSlewed = 0U;
  rt.dutyTrim = 0U;
  rt.limiting = false;
  rt.softActive = false;
}

// =====================================================================================
// Interpolation
// =====================================================================================

/**
 * @brief Linear interpolation with signed delta and round-to-nearest.
 *
 * The delta MUST be signed: a descending curve - retard off the line then give the timing back,
 * or a reference wire that dips - is the common real shape, and an unsigned subtraction there
 * wraps to a huge positive number that then clamps to the curve maximum.
 *
 * Assumes @p axis is ascending in [0, count); the clamps guarantee that for every lookup key.
 */
template <typename TValue>
uint16_t interpolateU16(const uint16_t *axis, const TValue *values, uint8_t count, uint32_t x)
{
  if (count == 0U) { return 0U; }
  if (x <= (uint32_t)axis[0U]) { return (uint16_t)values[0U]; }
  if (x >= (uint32_t)axis[count - 1U]) { return (uint16_t)values[count - 1U]; }

  uint8_t index = 0U;
  for (uint8_t i = 1U; i < count; ++i)
  {
    if ((uint32_t)axis[i] > x) { break; }
    index = i;
  }

  const uint8_t next = (uint8_t)(index + 1U);
  if (next >= count) { return (uint16_t)values[index]; }

  const uint32_t a0 = (uint32_t)axis[index];
  const uint32_t a1 = (uint32_t)axis[next];
  if (a1 <= a0) { return (uint16_t)values[index]; } // never divide by zero

  const int32_t v0 = (int32_t)values[index];
  const int32_t v1 = (int32_t)values[next];
  const int32_t delta = v1 - v0;
  const uint32_t frac = x - a0;
  const uint32_t full = a1 - a0;

  const int64_t scaled = (int64_t)delta * (int64_t)frac;
  const int64_t half = (int64_t)(full >> 1U);
  const int64_t rounded = (scaled >= 0) ? ((scaled + half) / (int64_t)full)
                                        : (-(((-scaled) + half) / (int64_t)full));

  const int32_t result = v0 + (int32_t)rounded;
  if (result <= 0) { return 0U; }
  return (uint16_t)result;
}

/**
 * @brief The reference axis value at the current elapsed time.
 *
 * Past the last breakpoint the last value is held rather than extrapolated: extrapolating a
 * reference forward would keep declaring the car slow forever, and the slip correction would
 * never release.
 */
uint16_t referenceAt(uint32_t ms)
{
  return interpolateU16(ppLaunchSpeedData.curveTime, ppLaunchSpeedData.curveAxis,
                        PP_CURVE_POINTS, ms);
}

/** @brief The axis a speed-mode curve is looked up against, for the configured curve mode. */
const uint16_t *lookupAxis(void)
{
  return (ppLaunchSpeedData.curveMode == (uint8_t)PP_CURVE_TIME)
       ? ppLaunchSpeedData.curveTime
       : ppLaunchSpeedData.curveAxis;
}

/** @brief The lookup key: elapsed time in time mode, the live axis in axis mode. */
uint32_t lookupKey(void)
{
  return (ppLaunchSpeedData.curveMode == (uint8_t)PP_CURVE_TIME) ? rt.ms : (uint32_t)rt.axis;
}

/**
 * @brief Advance the slew-limited retard toward its target, in TENTHS of a degree.
 *
 * The slew is specified as tenths per SECOND and applied against elapsed milliseconds, not per
 * call. That matters because this runs once per main-loop pass and the pass rate is not a
 * constant: a per-call limit would make the real retard rate depend on how busy the loop was, so
 * the same tune would behave differently at idle and at 7000 RPM.
 */
uint16_t advanceRetardSlew(uint16_t targetTenths, uint32_t nowMs, uint16_t slewTenthsPerSec)
{
  if (slewTenthsPerSec == 0U) { return targetTenths; }

  const uint32_t elapsed = nowMs - rt.slewMs;
  if (elapsed == 0U) { return rt.retardSlewed; }   // same millisecond: nothing may move yet
  rt.slewMs = nowMs;

  // Multiply by elapsed BEFORE dividing: the other order truncates a slow slew to zero and it
  // never moves at all.
  const uint32_t budget = ((uint32_t)slewTenthsPerSec * elapsed) / 1000U;

  if (targetTenths > rt.retardSlewed)
  {
    const uint32_t risen = (uint32_t)rt.retardSlewed + budget;
    return (uint16_t)((risen > targetTenths) ? targetTenths : risen);
  }
  if (targetTenths < rt.retardSlewed)
  {
    const uint32_t fall = (uint32_t)rt.retardSlewed - targetTenths;
    return (uint16_t)((fall > budget) ? ((uint32_t)rt.retardSlewed - budget) : targetTenths);
  }
  return targetTenths;
}

} // anonymous namespace

// =====================================================================================
// Defaults and clamps
// =====================================================================================

/**
 * @brief Reset the speed page to a safe, feature-disabled state.
 *
 * These values MUST match the `defaultValue =` entries for page 18 in reference/speeduino.ini.
 */
static void setSpeedDefaults(void)
{
  memset(&ppLaunchSpeedData, 0, sizeof(ppLaunchSpeedData));

  ppLaunchSpeedData.enable = (uint8_t)PP_DISABLED;
  ppLaunchSpeedData.armPin = 35U;          // pin 34 is the multiplexed VSS / cam-3 pin
  ppLaunchSpeedData.armPolarity = 1U;      // active-low trans-brake switch is the common case
  ppLaunchSpeedData.armPullup = 0U;
  ppLaunchSpeedData.axisSource = (uint8_t)PP_AXIS_VSS_MPH10;
  ppLaunchSpeedData.limitMethod = (uint8_t)PP_LIMIT_SPARK;
  ppLaunchSpeedData.curveMode = (uint8_t)PP_CURVE_TIME;
  ppLaunchSpeedData.featureFlags = (uint8_t)(PP_FLAG_PERFECT_PASS | PP_FLAG_RETARD_SLEW
                                           | PP_FLAG_SLIP_CORRECT | PP_FLAG_AXIS_CLAMP);

  ppLaunchSpeedData.timeoutMs = 10000U;    // 10 s armed timeout
  ppLaunchSpeedData.rearmBelowAxis = 0U;
  ppLaunchSpeedData.startBelowAxis = 0U;
  ppLaunchSpeedData.slipDeadband = 20U;    // 2.0 mph of deadband before slip correction acts
  ppLaunchSpeedData.axisAccelLimit = 200U; // 20.0 mph per update - generous, but rejects a spike
  ppLaunchSpeedData.retardSlewTenths = 3000U; ///< 3000 tenths/s = 300 deg/s
  ppLaunchSpeedData.shaftRevPerKph = 0U;
  ppLaunchSpeedData.maxGear = 0U;
  ppLaunchSpeedData.axisFilterShift = 1U;  // light smoothing
  ppLaunchSpeedData.limitHysteresis = 10U;

  const char *label = "speed";
  const size_t len = strlen(label);
  for (size_t i = 0U; (i < len) && (i < (size_t)(PP_LABEL_LEN - 1U)); ++i)
  {
    ppLaunchSpeedData.label[i] = (uint8_t)label[i];
  }

  // 32 points at 100 ms intervals: 0 .. 3100 ms.
  static const uint16_t kTime[PP_CURVE_POINTS] = {
      0U,  100U,  200U,  300U,  400U,  500U,  600U,  700U,
    800U,  900U, 1000U, 1100U, 1200U, 1300U, 1400U, 1500U,
   1600U, 1700U, 1800U, 1900U, 2000U, 2100U, 2200U, 2300U,
   2400U, 2500U, 2600U, 2700U, 2800U, 2900U, 3000U, 3100U };

  // Reference wire, tenths of a mph: 0 -> 71.8 mph in 3.1 s. Deliberately SHAPED rather than
  // flat; a flat default is what you get when nobody thought about it, and a tuner reading a flat
  // wire has no idea what the field is for.
  static const uint16_t kWire[PP_CURVE_POINTS] = {
      0U,  22U,  48U,  80U, 116U, 152U, 188U, 224U,
    258U, 292U, 324U, 356U, 386U, 414U, 442U, 468U,
    492U, 516U, 538U, 558U, 578U, 596U, 612U, 628U,
    642U, 656U, 668U, 680U, 690U, 700U, 710U, 718U };

  // Retard, tenths of a degree: 12.0 deg at the hit, easing out. The shape a turbo drag car
  // actually runs - pull timing to help the tyres live, then give it back.
  static const uint16_t kRetard[PP_CURVE_POINTS] = {
    120U, 118U, 114U, 108U, 100U,  90U,  78U,  66U,
     54U,  44U,  36U,  30U,  24U,  20U,  16U,  13U,
     10U,   8U,   6U,   5U,   4U,   4U,   3U,   3U,
      2U,   2U,   2U,   1U,   1U,   1U,   0U,   0U };

  // Limit, whole RPM (1 RPM resolution, not the 100 RPM steps the classic 2-step byte gives).
  static const uint16_t kLimit[PP_CURVE_POINTS] = {
    6800U, 6800U, 6800U, 6750U, 6700U, 6650U, 6600U, 6550U,
    6500U, 6450U, 6400U, 6400U, 6400U, 6400U, 6400U, 6400U,
    6400U, 6400U, 6400U, 6400U, 6400U, 6400U, 6400U, 6400U,
    6400U, 6400U, 6400U, 6400U, 6400U, 6400U, 6400U, 6400U };

  // Slip response. Boost trim leads on purpose: pulling timing costs power everywhere, trimming
  // boost only costs it where the tyres are actually spinning.
  static const uint16_t kSlipBin[PP_SLIP_POINTS] = { 0U, 10U, 20U, 40U, 80U, 120U, 160U, 240U };
  static const uint16_t kSlipRetard[PP_SLIP_POINTS] = { 0U, 0U, 10U, 20U, 40U, 60U, 80U, 100U };
  static const uint8_t kSlipTrim[PP_SLIP_POINTS] = { 0U, 5U, 10U, 20U, 35U, 50U, 65U, 80U };

  for (uint8_t i = 0U; i < PP_CURVE_POINTS; ++i)
  {
    ppLaunchSpeedData.curveTime[i] = kTime[i];
    ppLaunchSpeedData.curveAxis[i] = kWire[i];
    ppLaunchSpeedData.curveRetard[i] = kRetard[i];
    ppLaunchSpeedData.curveLimit[i] = kLimit[i];
    ppLaunchSpeedData.curveBoostTrim[i] = 0U;   // trim off by default: the limit does the work
  }

  for (uint8_t i = 0U; i < PP_SLIP_POINTS; ++i)
  {
    ppLaunchSpeedData.slipBin[i] = kSlipBin[i];
    ppLaunchSpeedData.slipRetard[i] = kSlipRetard[i];
    ppLaunchSpeedData.slipBoostTrim[i] = kSlipTrim[i];
  }
}

/**
 * @brief Reset the RPM page to a safe, feature-disabled state.
 *
 * These values MUST match the `defaultValue =` entries for page 19 in reference/speeduino.ini.
 */
static void setRpmDefaults(void)
{
  memset(&ppLaunchRpmData, 0, sizeof(ppLaunchRpmData));

  ppLaunchRpmData.enable = (uint8_t)PP_DISABLED;
  ppLaunchRpmData.armPin = 35U;
  ppLaunchRpmData.armPolarity = 1U;
  ppLaunchRpmData.armPullup = 0U;
  ppLaunchRpmData.cutMethod = (uint8_t)PP_LIMIT_SPARK;
  ppLaunchRpmData.timeoutMs = 10000U;
  ppLaunchRpmData.softZoneRpm = 300U;    // soft retard from 300 RPM below the limit
  ppLaunchRpmData.softHystRpm = 100U;    // and it lets go 100 RPM lower still
  ppLaunchRpmData.softRetardTenths = 80U;///< 8.0 degrees
  ppLaunchRpmData.retardSlewTenths = 3000U; ///< 3000 tenths/s = 300 deg/s

  const char *label = "rpm";
  const size_t len = strlen(label);
  for (size_t i = 0U; (i < len) && (i < (size_t)(PP_LABEL_LEN - 1U)); ++i)
  {
    ppLaunchRpmData.label[i] = (uint8_t)label[i];
  }

  // A limiter that ramps: the engine is held lower at the hit, when there is least traction, and
  // allowed to climb as the car puts power down. Flat 6400 after 1.5 s.
  static const uint16_t kTime[PP_RPM_CURVE_POINTS] = {
      0U,  100U,  200U,  300U,  400U,  500U,  600U,  700U,
    800U,  900U, 1000U, 1100U, 1200U, 1300U, 1400U, 1500U,
   1600U, 1700U, 1800U, 1900U, 2000U, 2100U, 2200U, 2300U,
   2400U, 2500U, 2600U, 2700U, 2800U, 2900U, 3000U, 3100U };

  static const uint16_t kLimit[PP_RPM_CURVE_POINTS] = {
    4200U, 4300U, 4400U, 4500U, 4600U, 4700U, 4800U, 4900U,
    5000U, 5100U, 5200U, 5300U, 5400U, 5500U, 5600U, 5700U,
    5800U, 5900U, 6000U, 6100U, 6200U, 6300U, 6400U, 6400U,
    6400U, 6400U, 6400U, 6400U, 6400U, 6400U, 6400U, 6400U };

  for (uint8_t i = 0U; i < PP_RPM_CURVE_POINTS; ++i)
  {
    ppLaunchRpmData.curveTime[i] = kTime[i];
    ppLaunchRpmData.curveLimit[i] = kLimit[i];
  }
}

/** @brief Clamp and repair the speed page. */
static void clampSpeedSettings(void)
{
  ppLaunchSpeedConfig &s = ppLaunchSpeedData;

  if (s.enable > (uint8_t)PP_ENABLED) { s.enable = (uint8_t)PP_DISABLED; }
  if (s.armPolarity > 1U) { s.armPolarity = 0U; }
  if (s.armPullup > 1U) { s.armPullup = 1U; }
  if (s.axisSource > (uint8_t)PP_AXIS_RPM) { s.axisSource = (uint8_t)PP_AXIS_VSS_MPH10; }
  if (s.limitMethod > (uint8_t)PP_LIMIT_ROLLING) { s.limitMethod = (uint8_t)PP_LIMIT_SPARK; }
  if (s.curveMode > (uint8_t)PP_CURVE_AXIS) { s.curveMode = (uint8_t)PP_CURVE_TIME; }
  if (s.maxGear > 6U) { s.maxGear = 6U; }
  if (s.axisFilterShift > 5U) { s.axisFilterShift = 5U; }
  s.featureFlags = (uint8_t)(s.featureFlags & 0x3FU); // only the six defined flags exist

  // The time axis must start at zero and be strictly ascending: it is a lookup key in time mode
  // and the X axis of every curve in the editor.
  s.curveTime[0] = 0U;
  for (uint8_t i = 1U; i < PP_CURVE_POINTS; ++i)
  {
    if (s.curveTime[i] <= s.curveTime[i - 1U])
    {
      s.curveTime[i] = (s.curveTime[i - 1U] < 0xFFFFU)
                     ? (uint16_t)(s.curveTime[i - 1U] + 1U)
                     : s.curveTime[i - 1U];
    }
  }

  // `curveAxis` is only forced monotonic in axis mode, where it IS the lookup key. In time mode
  // it is the reference wire, and a car can legitimately be expected to slow down - forcing the
  // wire to rise would compare the car against a fiction.
  if (s.curveMode == (uint8_t)PP_CURVE_AXIS)
  {
    for (uint8_t i = 1U; i < PP_CURVE_POINTS; ++i)
    {
      if (s.curveAxis[i] <= s.curveAxis[i - 1U])
      {
        s.curveAxis[i] = (s.curveAxis[i - 1U] < 0xFFFFU)
                       ? (uint16_t)(s.curveAxis[i - 1U] + 1U)
                       : s.curveAxis[i - 1U];
      }
    }
  }

  for (uint8_t i = 0U; i < PP_CURVE_POINTS; ++i)
  {
    s.curveRetard[i] = clampTenths(s.curveRetard[i]);
    // A limit of 0 means "no limiter at this point", which is legal. Anything else must be
    // reachable: below 500 RPM it would act as a permanent hard cut.
    if ((s.curveLimit[i] != 0U) && (s.curveLimit[i] < 500U)) { s.curveLimit[i] = 500U; }
    if (s.curveBoostTrim[i] > 100U) { s.curveBoostTrim[i] = 100U; }
  }

  // The slip axis is a lookup key, so it must be strictly ascending.
  s.slipBin[0] = 0U;
  for (uint8_t i = 1U; i < PP_SLIP_POINTS; ++i)
  {
    if (s.slipBin[i] <= s.slipBin[i - 1U])
    {
      s.slipBin[i] = (uint16_t)(s.slipBin[i - 1U] + 1U);
    }
    s.slipRetard[i] = clampTenths(s.slipRetard[i]);
    if (s.slipBoostTrim[i] > 100U) { s.slipBoostTrim[i] = 100U; }
  }

  s.label[PP_LABEL_LEN - 1U] = 0U;
}

/** @brief Clamp and repair the RPM page. */
static void clampRpmSettings(void)
{
  ppLaunchRpmConfig &s = ppLaunchRpmData;

  if (s.enable > (uint8_t)PP_ENABLED) { s.enable = (uint8_t)PP_DISABLED; }
  if (s.armPolarity > 1U) { s.armPolarity = 0U; }
  if (s.armPullup > 1U) { s.armPullup = 1U; }
  if (s.cutMethod > (uint8_t)PP_LIMIT_ROLLING) { s.cutMethod = (uint8_t)PP_LIMIT_SPARK; }

  s.softRetardTenths = clampTenths(s.softRetardTenths);

  // The time axis must start at zero and be strictly ascending: it is the lookup key.
  s.curveTime[0] = 0U;
  for (uint8_t i = 1U; i < PP_RPM_CURVE_POINTS; ++i)
  {
    if (s.curveTime[i] <= s.curveTime[i - 1U])
    {
      s.curveTime[i] = (s.curveTime[i - 1U] < 0xFFFFU)
                     ? (uint16_t)(s.curveTime[i - 1U] + 1U)
                     : s.curveTime[i - 1U];
    }
    // A limit of 0 means "no limiter at this point", which is legal and useful. Anything else
    // must be reachable: below 500 RPM it would act as a permanent hard cut.
    if ((s.curveLimit[i] != 0U) && (s.curveLimit[i] < 500U)) { s.curveLimit[i] = 500U; }
  }

  s.label[PP_LABEL_LEN - 1U] = 0U;
}

// =====================================================================================
// Storage
// =====================================================================================

/** @brief CRC32 (IEEE, reflected) over a payload. Identical to the ltt / raceTime / boostLaunch
 *  implementations - one algorithm, four blocks, no chance of them disagreeing. */
static uint32_t crc32Payload(const byte *pStart, const byte *pEnd)
{
  uint32_t crc = 0xFFFFFFFFU;
  for (const byte *p = pStart; p != pEnd; ++p)
  {
    crc ^= (uint32_t)(*p);
    for (uint8_t bit = 0U; bit < 8U; ++bit)
    {
      const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc = (crc >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

struct ppLaunchBlockHeader
{
  uint32_t magic;
  uint8_t version;
  uint8_t reserved[3];
  uint32_t crc;
  uint8_t trailing[4];
} __attribute__((packed));

static_assert(sizeof(ppLaunchBlockHeader) == EEPROM_PPLAUNCH_HEADER_SIZE,
              "Header size must match the block layout");

/** @brief CRC32 of the live payload for a mode. Both payloads are 512 B, so one helper does. */
static uint32_t payloadCrc(PpMode mode)
{
  const void *pStart = (mode == PP_MODE_RPM)
                     ? (const void *)&ppLaunchRpmData
                     : (const void *)&ppLaunchSpeedData;
  const byte *p = (const byte *)pStart;
  return crc32Payload(p, p + PPLAUNCH_PAYLOAD_SIZE);
}

static bool probeStorage(void)
{
  const uint16_t length = getStorageAPI().length();
  return (uint32_t)length >= ((uint32_t)EEPROM_PPLAUNCH_RPM_BASE + (uint32_t)PPLAUNCH_BLOCK_SIZE);
}

static bool payloadDiffers(PpMode mode)
{
  byte stored[PPLAUNCH_PAYLOAD_SIZE];
  (void)loadBlock(getStorageAPI(), payloadFor(mode), stored, stored + PPLAUNCH_PAYLOAD_SIZE);
  const void *live = (mode == PP_MODE_RPM)
                   ? (const void *)&ppLaunchRpmData
                   : (const void *)&ppLaunchSpeedData;
  return memcmp(stored, live, PPLAUNCH_PAYLOAD_SIZE) != 0;
}

static void writeHeader(PpMode mode)
{
  ppLaunchBlockHeader header;
  header.magic = magicFor(mode);
  header.version = EEPROM_PPLAUNCH_VERSION;
  header.reserved[0] = 0U;
  header.reserved[1] = 0U;
  header.reserved[2] = 0U;
  for (uint8_t i = 0U; i < 4U; ++i) { header.trailing[i] = 0U; }
  header.crc = payloadCrc(mode);
  updateObject(getStorageAPI(), header, baseFor(mode));
}

/**
 * @brief The one place a payload is written. Payload first, header last.
 *
 * BUDGET SEMANTICS - identical to every other page: a nonzero return means the payload is fully
 * stored; zero means the budget ran out and the caller must ask again.
 */
static uint16_t writeBlock(PpMode mode, uint16_t writesRemaining)
{
  const void *live = (mode == PP_MODE_RPM)
                   ? (const void *)&ppLaunchRpmData
                   : (const void *)&ppLaunchSpeedData;
  const byte *pStart = (const byte *)live;

  const uint16_t remaining = updateBlockLimitWriteOps(getStorageAPI(),
                                                     payloadFor(mode),
                                                     pStart,
                                                     pStart + PPLAUNCH_PAYLOAD_SIZE,
                                                     writesRemaining);
  if (remaining == 0U)
  {
    // Budget exhausted, so the payload may be incomplete. Do NOT touch the header: leaving the
    // old CRC in place means a torn save is detected at the next boot rather than trusted.
    return 0U;
  }

  writeHeader(mode);
  markDirty(mode, false);
  return remaining;
}

bool ppLaunchSave(PpMode mode)
{
  if (!rt.storageOk) { return false; }

  // A budget large enough that it can never be exhausted. Using the payload size here would be
  // wrong: when every byte differs the budget is consumed exactly, which looks like "exhausted"
  // and would skip the header write, leaving the block permanently dirty.
  (void)writeBlock(mode, UINT16_MAX);
  return !isDirty(mode);
}

bool ppLaunchNeedsSave(PpMode mode)
{
  if (!rt.storageOk) { return false; }
  return isDirty(mode) || payloadDiffers(mode);
}

uint16_t ppLaunchSaveBlock(PpMode mode, uint16_t writesRemaining)
{
  if (!rt.storageOk)
  {
    // Report the budget untouched rather than 0. Returning 0 would say "come back with more
    // budget", which the burn loop would obey forever - the flash-destroying bug this contract
    // exists to prevent.
    return writesRemaining;
  }

  if (!ppLaunchNeedsSave(mode))
  {
    markDirty(mode, false);
    return writesRemaining;
  }

  return writeBlock(mode, writesRemaining);
}

void ppLaunchMarkDirty(PpMode mode)
{
  markDirty(mode, true);
}

// =====================================================================================
// Arm inputs
// =====================================================================================

/** @brief Bind one mode's arm pin as an input. Safe to call repeatedly. */
static void bindArmPin(PpMode mode)
{
  const uint8_t configured = (mode == PP_MODE_RPM) ? ppLaunchRpmData.armPin
                                                  : ppLaunchSpeedData.armPin;
  const uint8_t pullup = (mode == PP_MODE_RPM) ? ppLaunchRpmData.armPullup
                                               : ppLaunchSpeedData.armPullup;
  uint8_t &boundPin = (mode == PP_MODE_RPM) ? rt.rpmArmPin : rt.speedArmPin;
  bool &bound = (mode == PP_MODE_RPM) ? rt.rpmPinBound : rt.speedPinBound;

  if (configured == 0U)
  {
    boundPin = 0U;
    bound = false;
    return;
  }

  // pinTranslate() maps the TunerStudio pin number to a board pin. Note what is NOT checked:
  // pin 0. Zero is a perfectly valid board pin - board_definition.h defines NOT_A_PIN as
  // UINT8_MAX precisely because zero cannot be used as the "unused" marker. Treating a
  // translated 0 as "untranslatable" refuses pin 0 while accepting every reserved pin it could
  // not actually use - a bug that already shipped once in this tree.
  const uint8_t pin = pinTranslate(configured);
  if (pinIsReserved(pin) || pinIsOutput(pin))
  {
    // Reserved (SD card, serial) or already driven by the ECU: leave the input unbound rather
    // than stealing a pin something else is using.
    boundPin = 0U;
    bound = false;
    return;
  }

  boundPin = pin;
  pinMode(pin, (pullup != 0U) ? INPUT_PULLUP : INPUT);
  bound = true;
}

/** @brief Read one mode's arm input, applying its configured polarity. */
static bool readArmPin(PpMode mode)
{
#if defined(UNIT_TEST)
  if (armOverrideActive) { return armOverrideValue; }
#endif
  const bool bound = (mode == PP_MODE_RPM) ? rt.rpmPinBound : rt.speedPinBound;
  if (!bound) { return false; }

  const uint8_t pin = (mode == PP_MODE_RPM) ? rt.rpmArmPin : rt.speedArmPin;
  const uint8_t polarity = (mode == PP_MODE_RPM) ? ppLaunchRpmData.armPolarity
                                                 : ppLaunchSpeedData.armPolarity;
  const bool high = (digitalRead(pin) == HIGH);
  return (polarity == 0U) ? high : !high;
}

// =====================================================================================
// Axis conditioning (speed mode only)
// =====================================================================================

/** @brief True if the configured axis source needs a working VSS input. */
static bool axisNeedsVss(void)
{
  const uint8_t src = ppLaunchSpeedData.axisSource;
  return (src == (uint8_t)PP_AXIS_VSS_MPH10)
      || (src == (uint8_t)PP_AXIS_VSS_KPH10)
      || (src == (uint8_t)PP_AXIS_SHAFT_RPM);
}

/**
 * @brief Read the raw axis value, in the configured unit.
 *
 * `currentStatus.vss` is whole km/h at 50 Hz, so tenths are produced by multiplying by 10 rather
 * than by inventing resolution.
 */
static uint16_t readRawAxis(const statuses &current)
{
#if defined(UNIT_TEST)
  if (axisOverrideActive) { return axisOverride; }
#endif

  const uint16_t vss10 = (uint16_t)(current.vss * 10U);

  switch ((PpAxisSource)ppLaunchSpeedData.axisSource)
  {
    case PP_AXIS_VSS_KPH10:
      return vss10;

    case PP_AXIS_SHAFT_RPM:
      // shaftRevPerKph is x100 to keep two decimals of a ratio. The intermediate is 32-bit
      // because vss x ratio overflows 16 bits easily.
      return (uint16_t)(((uint32_t)current.vss * (uint32_t)ppLaunchSpeedData.shaftRevPerKph) / 100U);

    case PP_AXIS_RPM:
      return current.RPM;

    case PP_AXIS_VSS_MPH10:
    default:
      return kmh10ToMph10(vss10); // vss is km/h; convert to tenths of a mph
  }
}

/**
 * @brief Condition the raw axis: spike clamp, then IIR smoothing.
 *
 * The spike clamp is the important one. A single noise tooth, or one wheel momentarily losing and
 * regaining signal, produces a reading that crosses every bin on the curve in one update - so the
 * curve's output goes from "leave hard" to "full retard" in a pass. Clamping the change per update
 * turns that into a reading that simply lags reality by a few milliseconds.
 */
static uint16_t conditionAxis(uint16_t raw, bool first)
{
  if (first) { return raw; }

  const ppLaunchSpeedConfig &cfg = ppLaunchSpeedData;
  uint16_t clamped = raw;

  if ((cfg.featureFlags & PP_FLAG_AXIS_CLAMP) != 0U)
  {
    const uint16_t limit = cfg.axisAccelLimit;
    const uint16_t prev = rt.axis;
    if (raw > prev)
    {
      const uint32_t maxStep = (uint32_t)prev + limit;
      if ((uint32_t)raw > maxStep) { clamped = (maxStep > 0xFFFFU) ? 0xFFFFU : (uint16_t)maxStep; }
    }
    else
    {
      const uint16_t minStep = (prev > limit) ? (uint16_t)(prev - limit) : 0U;
      if (raw < minStep) { clamped = minStep; }
    }
  }

  const uint8_t shift = cfg.axisFilterShift;
  if (shift == 0U) { return clamped; }

  // IIR: axis += (raw - axis) / 2^shift. Signed intermediate, because the delta can be negative
  // and an unsigned subtraction would wrap.
  const int32_t delta = (int32_t)clamped - (int32_t)rt.axis;
  const int32_t step = delta / (int32_t)(1U << shift);
  const int32_t next = (int32_t)rt.axis + step;
  if (next <= 0) { return 0U; }
  if (next > 65535) { return 65535U; }
  return (uint16_t)next;
}

/** @brief Slip correction, added to the curve's own retard and duty trim. */
static void applySlipCorrection(uint16_t &retardTenths, uint16_t &dutyTrim)
{
  const ppLaunchSpeedConfig &cfg = ppLaunchSpeedData;
  if ((cfg.featureFlags & PP_FLAG_PERFECT_PASS) == 0U) { return; }
  if ((cfg.featureFlags & PP_FLAG_SLIP_CORRECT) == 0U) { return; }

  // Only positive deviation is "slip". Being behind the wire means the car is slower than the
  // reference - a bog, a soft launch, a lifted throttle - and pulling timing for that would make
  // it slower still. That asymmetry is the difference between a correction and a mistake.
  const int16_t deadband = (int16_t)cfg.slipDeadband;
  if (rt.slip <= deadband) { return; }

  const uint32_t excess = (uint32_t)(rt.slip - deadband);

  const uint16_t extraRetard = interpolateU16(cfg.slipBin, cfg.slipRetard, PP_SLIP_POINTS, excess);
  const uint16_t extraTrim = interpolateU16(cfg.slipBin, cfg.slipBoostTrim, PP_SLIP_POINTS, excess);

  retardTenths = clampTenths((uint16_t)(retardTenths + extraRetard));
  const uint16_t trimSum = (uint16_t)dutyTrim + extraTrim;
  dutyTrim = (trimSum > 100U) ? 100U : trimSum;
}

/** @brief True if the current gear is within the configured gate. */
static bool gearGatePasses(const statuses &current)
{
  const ppLaunchSpeedConfig &cfg = ppLaunchSpeedData;
  if ((cfg.featureFlags & PP_FLAG_GEAR_GATE) == 0U) { return true; }
  if (cfg.maxGear == 0U) { return true; }
  if (current.gear == 0U) { return true; }  // gear unknown: do not block a launch on it
  return current.gear <= cfg.maxGear;
}

// =====================================================================================
// State machines
// =====================================================================================

/** @brief Advance one mode's state machine by a millisecond. Shared by both modes. */
static void tickStateMachine(PpState &state, bool &prevArm, bool arm, uint16_t timeoutMs,
                             uint32_t &armedMs, uint32_t &ms)
{
  switch (state)
  {
    case PP_STATE_OFF:
    case PP_STATE_IDLE:
      // Arming is what starts the sequence.
      if (arm)
      {
        state = PP_STATE_ARMED;
        ms = 0U;
        armedMs = 0U;
      }
      else
      {
        state = PP_STATE_IDLE;
        ms = 0U;
      }
      break;

    case PP_STATE_ARMED:
      if (arm)
      {
        // The timeout is what stops a car that got armed on the highway from sitting armed
        // indefinitely - the same failure MS3 users work around by spoofing the VSS to zero.
        ++armedMs;
        if ((timeoutMs != 0U) && (armedMs >= (uint32_t)timeoutMs))
        {
          state = PP_STATE_TIMEOUT;
        }
        else
        {
          ms = 0U;
        }
      }
      else
      {
        // Release edge: the clock starts here. Release, not assertion - the run begins when the
        // trans-brake or the clutch is let go, which is when the car actually moves.
        state = PP_STATE_LAUNCHING;
        ms = 0U;
        armedMs = 0U;
      }
      break;

    case PP_STATE_LAUNCHING:
      ++ms;
      break;

    case PP_STATE_COMPLETE:
    case PP_STATE_TIMEOUT:
      // Only a fresh assertion re-enters the sequence, so a completed pass is re-run by the driver
      // pressing the arm input again - which is exactly how a second attempt happens.
      if (arm && !prevArm)
      {
        state = PP_STATE_ARMED;
        armedMs = 0U;
        ms = 0U;
      }
      else if (!arm && (state == PP_STATE_TIMEOUT))
      {
        // A timeout is an aborted arming, not a result: releasing the input returns to idle so the
        // driver can simply try again.
        state = PP_STATE_IDLE;
      }
      else
      {
        // COMPLETE latches, deliberately, even though the arm input is released during a pass -
        // the driver let go to launch, so falling back to IDLE would destroy the only record that
        // the run finished. The state is what tells a tuner reading the log why the limiter
        // stopped, and it must survive until the next attempt.
      }
      break;

    default:
      state = PP_STATE_OFF; // MISRA
      break;
  }

  prevArm = arm;
}

void ppLaunchTick1ms(statuses &current)
{
  (void)current;

  // ---- speed page (page 18) ----------------------------------------------------------
  if (speedEnabled())
  {
    const bool arm = readArmPin(PP_MODE_SPEED);
    tickStateMachine(rt.speedState, rt.prevArmSpeed, arm, ppLaunchSpeedData.timeoutMs,
                     rt.armedMs, rt.ms);

    // The speed page's clock is the shared one, and the pass is over when its curve runs out.
    if (rt.speedState == PP_STATE_LAUNCHING)
    {
      const uint16_t lastTime = ppLaunchSpeedData.curveTime[PP_CURVE_POINTS - 1U];
      if ((ppLaunchSpeedData.curveMode == (uint8_t)PP_CURVE_TIME)
          && (lastTime != 0U) && (rt.ms > (uint32_t)lastTime))
      {
        rt.speedState = PP_STATE_COMPLETE;
        clearOutputs();
      }
    }
  }
  else
  {
    rt.speedState = PP_STATE_OFF;
    rt.prevArmSpeed = false;
    if (!rpmEnabled()) { rt.ms = 0U; }
  }

  // ---- rpm page (page 19) ------------------------------------------------------------
  // It keeps its own clock so that enabling both pages does not have them fighting over one
  // counter. The two are armed independently and could legitimately be at different points.
  if (rpmEnabled())
  {
    const bool arm = readArmPin(PP_MODE_RPM);
    tickStateMachine(rt.rpmState, rt.prevArmRpm, arm, ppLaunchRpmData.timeoutMs,
                     rt.rpmArmedMs, rt.rpmMs);

    if (rt.rpmState == PP_STATE_LAUNCHING)
    {
      const uint16_t lastTime = ppLaunchRpmData.curveTime[PP_RPM_CURVE_POINTS - 1U];
      if ((lastTime != 0U) && (rt.rpmMs > (uint32_t)lastTime))
      {
        rt.rpmState = PP_STATE_COMPLETE;
        clearOutputs();
      }
    }
  }
  else
  {
    rt.rpmState = PP_STATE_OFF;
    rt.prevArmRpm = false;
    rt.rpmMs = 0U;
    rt.rpmArmedMs = 0U;
  }
}

// =====================================================================================
// Evaluation
// =====================================================================================

namespace {

/** @brief Evaluate the speed page (page 18). */
void updateSpeedMode(const statuses &current, const config2 &page2)
{
  const ppLaunchSpeedConfig &cfg = ppLaunchSpeedData;

  // A speed-based curve on a car with no VSS configured reads zero forever, which would hold the
  // limiter and the retard engaged for the whole pass with no indication why. Refuse to run
  // rather than applying a curve to a sensor that does not exist.
  const bool vssMissing = axisNeedsVss() && (page2.vssMode == VSS_MODE_OFF);
  if (vssMissing)
  {
    clearOutputs();
    rt.axis = 0U;
    rt.slip = 0;
    rt.axisSeen = false;
    ppLaunchAxisLog = 0U;
    ppLaunchSlipLog = 0;
    return;
  }

  // ---- axis conditioning ---------------------------------------------------------------
  // Only the build-up is treated as a fresh sample. During a pass the axis must be conditioned
  // continuously, or the spike clamp would be bypassed exactly when the car moves fastest.
  rt.axis = conditionAxis(readRawAxis(current), !rt.axisSeen);
  rt.axisSeen = true;
  ppLaunchAxisLog = rt.axis;

  // ---- reference wire and slip ---------------------------------------------------------
  const bool wireValid = (cfg.curveMode == (uint8_t)PP_CURVE_TIME)
                      && ((cfg.featureFlags & PP_FLAG_PERFECT_PASS) != 0U);
  if (wireValid)
  {
    rt.slip = (int16_t)((int32_t)rt.axis - (int32_t)referenceAt(rt.ms));
  }
  else
  {
    // In axis mode there is no time-based wire: the axis IS the reference, so there is nothing to
    // deviate from and slip correction is not meaningful.
    rt.slip = 0;
  }
  ppLaunchSlipLog = rt.slip;

  // ---- re-arm --------------------------------------------------------------------------
  // Evaluated here, not in the tick, because this is where the axis has just been read. A driver
  // who aborts a pass must not have to key off to reset the module.
  if ((rt.speedState == PP_STATE_LAUNCHING) && (cfg.rearmBelowAxis != 0U)
      && ((uint32_t)rt.axis < (uint32_t)cfg.rearmBelowAxis))
  {
    rt.speedState = PP_STATE_IDLE;
    rt.ms = 0U;
    clearOutputs();
  }

  if (rt.speedState != PP_STATE_LAUNCHING)
  {
    clearOutputs();
    return;
  }

  const uint32_t key = lookupKey();
  const uint16_t *axis = lookupAxis();

  // ---- retard --------------------------------------------------------------------------
  uint16_t retardTenths = interpolateU16(axis, cfg.curveRetard, PP_CURVE_POINTS, key);
  uint16_t dutyTrim = interpolateU16(axis, cfg.curveBoostTrim, PP_CURVE_POINTS, key);
  if (wireValid) { applySlipCorrection(retardTenths, dutyTrim); }

  rt.retardTenths = clampTenths(retardTenths);
  const uint16_t slew = ((cfg.featureFlags & PP_FLAG_RETARD_SLEW) != 0U)
                      ? cfg.retardSlewTenths : 0U;
  rt.retardSlewed = advanceRetardSlew(rt.retardTenths, rt.ms, slew);

  // ---- RPM limit -----------------------------------------------------------------------
  const uint16_t curveLimit = interpolateU16(axis, cfg.curveLimit, PP_CURVE_POINTS, key);

  // The start gate is a speed window: the limiter may only engage below it, so a car that has
  // already left does not get caught by the launch limiter on the top end.
  const bool startGateOk = (cfg.startBelowAxis == 0U)
                        || ((uint32_t)rt.axis < (uint32_t)cfg.startBelowAxis);
  const bool wantLimit = (cfg.limitMethod != (uint8_t)PP_LIMIT_NONE)
                      && startGateOk
                      && gearGatePasses(current)
                      && (curveLimit != 0U);

  // Hysteresis on the gate: once engaged the limiter stays engaged until the axis has fallen below
  // the gate by limitHysteresis. Without it a car sitting exactly on the gate chatters the limiter
  // on and off, which sounds like a backfire and ruins the launch.
  if (wantLimit)
  {
    rt.limiting = true;
  }
  else if (!startGateOk && (cfg.startBelowAxis != 0U))
  {
    const uint32_t releaseAt = (cfg.startBelowAxis > cfg.limitHysteresis)
                             ? (uint32_t)(cfg.startBelowAxis - cfg.limitHysteresis) : 0U;
    rt.limiting = ((uint32_t)rt.axis < releaseAt);
  }
  else
  {
    rt.limiting = false;
  }

  if (rt.limiting)
  {
    rt.rpmLimit = curveLimit;
    rt.limitMethod = (PpLimitMethod)cfg.limitMethod;
  }
  else
  {
    rt.rpmLimit = UINT16_MAX;
    rt.limitMethod = PP_LIMIT_NONE;
  }

  rt.dutyTrim = ((cfg.featureFlags & PP_FLAG_DUTY_TRIM) != 0U) ? (uint8_t)dutyTrim : 0U;
}

/** @brief Evaluate the RPM page (page 19).
 *
 * Deliberately simple: an RPM limit that follows a time ramp, and a soft retard that engages
 * within `softZoneRpm` of that limit with hysteresis so it does not flicker at the boundary.
 */
void updateRpmMode(const statuses &current)
{
  const ppLaunchRpmConfig &cfg = ppLaunchRpmData;

  if (rt.rpmState != PP_STATE_LAUNCHING)
  {
    // The soft-retard slew has to keep advancing while ARMED too, so that it has a notion of
    // elapsed time at the moment the pass starts rather than jumping on the first millisecond.
    rt.retardTenths = 0U;
    rt.retardSlewed = 0U;
    rt.slewMs = rt.ms;
    rt.rpmLimit = UINT16_MAX;
    rt.limitMethod = PP_LIMIT_NONE;
    rt.limiting = false;
    rt.softActive = false;
    return;
  }

  // The rpm page's own clock, kept in ppLaunchTick1ms. One definition of "elapsed" for this mode.
  const uint32_t ms = rt.rpmMs;

  const uint16_t limit = interpolateU16(cfg.curveTime, cfg.curveLimit, PP_RPM_CURVE_POINTS, ms);

  const bool haveLimit = (cfg.cutMethod != (uint8_t)PP_LIMIT_NONE) && (limit != 0U);
  rt.limitMethod = haveLimit ? (PpLimitMethod)cfg.cutMethod : PP_LIMIT_NONE;
  rt.rpmLimit = haveLimit ? limit : UINT16_MAX;
  rt.limiting = haveLimit;

  // ---- soft retard band ----------------------------------------------------------------
  // Engages within softZoneRpm BELOW the limit and releases only once the engine has dropped a
  // further softHystRpm. Without the hysteresis the retard flickers on and off at the boundary,
  // which is worse than not having it: the timing jumps about instead of easing.
  // NOTE: gated on the LIMIT VALUE, not on haveLimit. "No cut (retard only)" is an explicit menu
  // option and must still get its soft retard - gating this on a cut method being configured made
  // the most useful combination (retard, no hard cut) silently do nothing.
  const bool haveBand = (cfg.softZoneRpm != 0U) && (limit != 0U);
  if (haveBand)
  {
    const uint32_t engageAt = (limit > cfg.softZoneRpm) ? (uint32_t)(limit - cfg.softZoneRpm) : 0U;
    const uint32_t releaseAt = (engageAt > cfg.softHystRpm)
                             ? (engageAt - cfg.softHystRpm) : 0U;

    if ((uint32_t)current.RPM >= engageAt)
    {
      rt.softActive = true;
    }
    else if (rt.softActive && ((uint32_t)current.RPM < releaseAt))
    {
      rt.softActive = false;
    }
    else
    {
      // Between releaseAt and engageAt: hold whatever the previous state was. That is the
      // hysteresis band doing its job.
    }
  }
  else
  {
    rt.softActive = false;
  }

  rt.retardTenths = rt.softActive ? cfg.softRetardTenths : 0U;
  rt.retardSlewed = advanceRetardSlew(rt.retardTenths, ms, cfg.retardSlewTenths);
}

} // anonymous namespace

void ppLaunchUpdate(statuses &current, const config2 &page2)
{
  if (!ppLaunchIsActive())
  {
    clearOutputs();
    rt.axis = 0U;
    rt.slip = 0;
    ppLaunchAxisLog = 0U;
    ppLaunchSlipLog = 0;
    return;
  }

  if (speedModeActive())
  {
    updateSpeedMode(current, page2);
  }
  else
  {
    updateRpmMode(current);
  }

  // Retard leaves through the torque bus so the arbiter's "largest single request wins" rule can
  // stop it stacking with the race-time curve or a future traction controller. Publishing every
  // pass is required by the bus contract: there is no "cancel", so a state that stops launching
  // simply stops asking.
  const uint8_t retardWhole = (uint8_t)((rt.retardSlewed + 5U) / 10U);
  torqueRequestRetard(TorqueProducer::Launch, (int8_t)retardWhole);

  // Publish the snapshot for logging. Done here rather than in the accessors so a log entry and
  // the applied values can never disagree about which evaluation they belong to.
  current.ppLaunchAxis = rt.axis;
  current.ppLaunchSlip = rt.slip;
  current.ppLaunchLimit = (rt.rpmLimit == UINT16_MAX) ? 0U : rt.rpmLimit;
  current.ppLaunchRetard = retardWhole;
  current.ppLaunchState = (uint8_t)ppLaunchState();
  current.ppLaunchStatus = (uint8_t)(((rt.limiting ? 1U : 0U) << 0U)
                                   | (((rt.dutyTrim != 0U) ? 1U : 0U) << 1U)
                                   | ((uint8_t)torqueBusRetardProducer() << 2U));
}

// =====================================================================================
// Boot
// =====================================================================================

/** @brief Load one block, repairing or defaulting it as required. */
static void loadBlock(PpMode mode)
{
  const uint8_t bit = (mode == PP_MODE_RPM) ? 0x02U : 0x01U;

  ppLaunchBlockHeader header;
  (void)loadObject(getStorageAPI(), baseFor(mode), header);

  const bool magicOk = (header.magic == magicFor(mode))
                    && (header.version == EEPROM_PPLAUNCH_VERSION);

  if (!magicOk)
  {
    // Never initialised by this build, or written by an incompatible version. Start clean and lay
    // down a valid header so the next boot takes the fast path.
    if (mode == PP_MODE_RPM) { setRpmDefaults(); clampRpmSettings(); }
    else { setSpeedDefaults(); clampSpeedSettings(); }
    rt.initialisedMask = (uint8_t)(rt.initialisedMask & (uint8_t)(~bit));
    markDirty(mode, true);
    (void)ppLaunchSave(mode);
    return;
  }

  if (mode == PP_MODE_RPM)
  {
    (void)loadObject(getStorageAPI(), payloadFor(mode), ppLaunchRpmData);
    clampRpmSettings();
  }
  else
  {
    (void)loadObject(getStorageAPI(), payloadFor(mode), ppLaunchSpeedData);
    clampSpeedSettings();
  }

  // Repair FIRST, then verify: the repair is what makes a slightly damaged block usable, but it
  // also changes bytes, so the CRC must be taken against the repaired payload. Discarding a
  // tuner's whole curve because the axis needed one breakpoint nudged would be a worse outcome.
  if (header.crc != payloadCrc(mode))
  {
    rt.initialisedMask = (uint8_t)(rt.initialisedMask & (uint8_t)(~bit));
    markDirty(mode, true);
    (void)ppLaunchSave(mode);
    return;
  }

  rt.initialisedMask = (uint8_t)(rt.initialisedMask | bit);
}

void initialisePpLaunch(statuses &current)
{
  (void)current;

  rt.storageOk = probeStorage();
  rt.dirtyMask = 0U;
  rt.initialisedMask = 0U;
  rt.reset();

  if (!rt.storageOk)
  {
    // Cannot persist anything. Lay down safe defaults so nothing is applied from a block we could
    // not have read.
    setSpeedDefaults();
    clampSpeedSettings();
    setRpmDefaults();
    clampRpmSettings();
    bindArmPin(PP_MODE_SPEED);
    bindArmPin(PP_MODE_RPM);
    return;
  }

  loadBlock(PP_MODE_SPEED);
  loadBlock(PP_MODE_RPM);

  bindArmPin(PP_MODE_SPEED);
  bindArmPin(PP_MODE_RPM);
}

void ppLaunchRestoreDefaults(void)
{
  setSpeedDefaults();
  clampSpeedSettings();
  setRpmDefaults();
  clampRpmSettings();
  markDirty(PP_MODE_SPEED, true);
  markDirty(PP_MODE_RPM, true);
}

// =====================================================================================
// Accessors
// =====================================================================================

PpState ppLaunchState(void)
{
  // The active mode's state is the one that means anything to a consumer.
  return speedModeActive() ? rt.speedState : rt.rpmState;
}

uint32_t ppLaunchElapsedMs(void)
{
  // Zero unless actually launching, so a consumer cannot act on a stale elapsed time from a
  // previous attempt.
  return (rt.speedState == PP_STATE_LAUNCHING) ? rt.ms : 0U;
}

uint16_t ppLaunchRpmLimit(void) { return rt.rpmLimit; }
PpLimitMethod ppLaunchLimitMethod(void) { return rt.limitMethod; }
uint8_t ppLaunchRetard(void) { return (uint8_t)((rt.retardSlewed + 5U) / 10U); }
uint8_t ppLaunchBoostTrim(void) { return rt.dutyTrim; }
PpMode ppLaunchActiveMode(void) { return speedModeActive() ? PP_MODE_SPEED : PP_MODE_RPM; }
uint16_t ppLaunchAxisValue(void) { return rt.axis; }
int16_t ppLaunchSlip(void) { return rt.slip; }
bool ppLaunchIsLimiting(void) { return rt.limiting; }
bool ppLaunchIsAvailable(void) { return rt.storageOk; }

bool ppLaunchIsActive(void)
{
  return speedEnabled() || rpmEnabled();
}

void ppLaunchReset(void)
{
  rt.reset();
  const bool active = ppLaunchIsActive();
  rt.speedState = active ? PP_STATE_IDLE : PP_STATE_OFF;
  rt.rpmState = active ? PP_STATE_IDLE : PP_STATE_OFF;
}

ppLaunchSpeedBlock &ppLaunchGetSpeedPageMutable(void) { return ppLaunchSpeedData; }
ppLaunchRpmBlock &ppLaunchGetRpmPageMutable(void) { return ppLaunchRpmData; }
const ppLaunchSpeedBlock &ppLaunchGetSpeedPage(void) { return ppLaunchSpeedData; }
const ppLaunchRpmBlock &ppLaunchGetRpmPage(void) { return ppLaunchRpmData; }

bool ppLaunchIsInitialised(PpMode mode)
{
  return (rt.initialisedMask & ((mode == PP_MODE_RPM) ? 0x02U : 0x01U)) != 0U;
}

#if defined(UNIT_TEST)
void ppLaunchResetForTest(void)
{
  rt.reset();
  rt.dirtyMask = 0U;
  rt.initialisedMask = 0U;
  rt.storageOk = false;
  rt.speedPinBound = false;
  rt.rpmPinBound = false;
  rt.speedArmPin = 0U;
  rt.rpmArmPin = 0U;
  armOverrideActive = false;
  armOverrideValue = false;
  axisOverride = 0U;
  axisOverrideActive = false;
  setSpeedDefaults();
  clampSpeedSettings();
  setRpmDefaults();
  clampRpmSettings();
  ppLaunchAxisLog = 0U;
  ppLaunchSlipLog = 0;
}

void ppLaunchSetAxisForTest(uint16_t axis)
{
  axisOverride = axis;
  axisOverrideActive = true;
}

void ppLaunchSetArmForTest(bool asserted)
{
  armOverrideValue = asserted;
  armOverrideActive = true;
}
#endif
