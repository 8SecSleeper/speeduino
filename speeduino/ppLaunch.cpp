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
 * @brief Launch control: state machines, table evaluation and persistence.
 *
 * See ppLaunch.h for the one-axis-three-rows shape, the simultaneous-modes contract and the
 * linear soft-zone retard. The properties that are easy to break by accident, restated:
 *
 *  1. Every clock advances only from @ref ppLaunchTick1ms. Anything else that adds to it makes
 *     the elapsed time depend on loop cadence, and every consumer inherits the error.
 *  2. Every output is zero unless the relevant state machine is LAUNCHING.
 *  3. Retard leaves through the torque bus, never by writing `advance`.
 *  4. Nothing here touches storage unless the backend can hold all three blocks.
 *
 * Curve evaluation happens on EVERY main-loop pass. An earlier draft rate-limited it to 100 Hz
 * "because 32-point interpolations feel expensive"; that is the same mistake this tree already
 * fixed once in launchController, which re-tested a 10 Hz bit and silently capped a drag launch
 * at ~100 ms of lag - longer than the whole first 60 feet.
 */

// =====================================================================================
// The three page payloads
// =====================================================================================

ppLaunchSpeedBlock ppLaunchSpeedData;
ppLaunchRpmBlock ppLaunchRpmData;
ppLaunchReactBlock ppLaunchReactData;

// =====================================================================================
// Runtime state
// =====================================================================================

uint16_t ppLaunchAxisLog = 0U;
int16_t ppLaunchSlipLog = 0;

namespace {

/** @brief Everything that is not configuration, grouped so it resets in one assignment. */
struct RuntimeState {
  // ---- speed mode ----
  uint32_t ms = 0;                  ///< Milliseconds since the release. The speed mode's clock.
  uint32_t armedMs = 0;             ///< Milliseconds armed.
  uint32_t slewMs = 0;              ///< Milliseconds at which the retard slew last advanced.
  PpState speedState = PP_STATE_OFF;
  uint16_t axis = 0;                ///< Conditioned axis value.
  int16_t slipMs = 0;               ///< Deviation from the reference-time wire.
  uint16_t retardTenths = 0;        ///< Tenths requested.
  uint16_t retardSlewed = 0;        ///< Tenths published after slew limiting.
  uint8_t  speedStrategy = 0;       ///< Strategy bits in force.
  uint16_t speedLimit = UINT16_MAX; ///< Limit from the speed page alone.

  // ---- rpm mode ----
  uint32_t rpmMs = 0;               ///< The rpm mode's own clock.
  uint32_t rpmArmedMs = 0;
  PpState rpmState = PP_STATE_OFF;
  uint16_t rpmRetardTenths = 0;
  uint8_t  rpmStrategy = 0;
  uint16_t rpmLimit = UINT16_MAX;   ///< Limit from the rpm page alone.

  // ---- shared ----
  uint16_t limitHitMs = 0;          ///< Accumulated ms at the hard limit this pass.
  int16_t fuelAdd = 0;              ///< Reaction: percent of pulse width.
  int16_t boostDutyAdj = 0;         ///< Reaction: percent of duty.
  int16_t boostTargetAdj = 0;       ///< Reaction: kPa.
  uint16_t reactionRetardTenths = 0;///< Reaction: tenths of a degree.
  bool limiting = false;
  bool storageOk = false;
  bool axisSeen = false;
  uint8_t speedArmPin = 0;
  uint8_t rpmArmPin = 0;
  bool speedPinBound = false;
  bool rpmPinBound = false;
  uint8_t dirtyMask = 0;            ///< bit per PpMode
  uint8_t initialisedMask = 0;      ///< bit per PpMode

  void reset(void)
  {
    ms = 0U;
    armedMs = 0U;
    slewMs = 0U;
    speedState = PP_STATE_OFF;
    axis = 0U;
    slipMs = 0;
    retardTenths = 0U;
    retardSlewed = 0U;
    speedStrategy = 0U;
    speedLimit = UINT16_MAX;
    rpmMs = 0U;
    rpmArmedMs = 0U;
    rpmState = PP_STATE_OFF;
    rpmRetardTenths = 0U;
    rpmStrategy = 0U;
    rpmLimit = UINT16_MAX;
    limitHitMs = 0U;
    fuelAdd = 0;
    boostDutyAdj = 0;
    boostTargetAdj = 0;
    reactionRetardTenths = 0U;
    limiting = false;
    axisSeen = false;
  }
};

RuntimeState rt;

#if defined(UNIT_TEST)
bool armOverrideActive = false;
bool armOverrideValue = false;
uint16_t axisOverride = 0;
bool axisOverrideActive = false;
#endif

// =====================================================================================
// Small helpers
// =====================================================================================

bool speedEnabled(void) { return ppLaunchSpeedData.enable == (uint8_t)PP_ENABLED; }
bool rpmEnabled(void) { return ppLaunchRpmData.enable == (uint8_t)PP_ENABLED; }

constexpr uint8_t modeBit(PpMode mode) { return (uint8_t)(1U << (uint8_t)mode); }

void markDirty(PpMode mode, bool dirty)
{
  const uint8_t bit = modeBit(mode);
  if (dirty) { rt.dirtyMask = (uint8_t)(rt.dirtyMask | bit); }
  else { rt.dirtyMask = (uint8_t)(rt.dirtyMask & (uint8_t)(~bit)); }
}

bool isDirty(PpMode mode) { return (rt.dirtyMask & modeBit(mode)) != 0U; }

uint16_t baseFor(PpMode mode)
{
  switch (mode)
  {
    case PP_MODE_RPM:   return EEPROM_PPLAUNCH_RPM_BASE;
    case PP_MODE_REACT: return EEPROM_PPLAUNCH_REACT_BASE;
    case PP_MODE_SPEED:
    default:            return EEPROM_PPLAUNCH_SPEED_BASE;
  }
}

uint16_t payloadFor(PpMode mode) { return (uint16_t)(baseFor(mode) + EEPROM_PPLAUNCH_HEADER_SIZE); }

uint32_t magicFor(PpMode mode)
{
  switch (mode)
  {
    case PP_MODE_RPM:   return EEPROM_PPLAUNCH_RPM_MAGIC;
    case PP_MODE_REACT: return EEPROM_PPLAUNCH_REACT_MAGIC;
    case PP_MODE_SPEED:
    default:            return EEPROM_PPLAUNCH_SPEED_MAGIC;
  }
}

void *payloadPtr(PpMode mode)
{
  switch (mode)
  {
    case PP_MODE_RPM:   return (void *)&ppLaunchRpmData;
    case PP_MODE_REACT: return (void *)&ppLaunchReactData;
    case PP_MODE_SPEED:
    default:            return (void *)&ppLaunchSpeedData;
  }
}

/** @brief km/h x10 to mph x10. The 32-bit numerator is mandatory - see the header. */
uint16_t kmh10ToMph10(uint16_t kmh10)
{
  return (uint16_t)(((uint32_t)kmh10 * PP_MPH_TO_KMH_DEN) / PP_MPH_TO_KMH_NUM);
}

uint16_t clampTenths(uint16_t t) { return (t > 600U) ? 600U : t; }

void clearOutputs(void)
{
  rt.speedLimit = UINT16_MAX;
  rt.rpmLimit = UINT16_MAX;
  rt.retardTenths = 0U;
  rt.retardSlewed = 0U;
  rt.rpmRetardTenths = 0U;
  rt.speedStrategy = 0U;
  rt.rpmStrategy = 0U;
  rt.reactionRetardTenths = 0U;
  rt.fuelAdd = 0;
  rt.boostDutyAdj = 0;
  rt.boostTargetAdj = 0;
  rt.limiting = false;
}

// =====================================================================================
// Interpolation
// =====================================================================================

/**
 * @brief Linear interpolation with signed delta and round-to-nearest.
 *
 * The delta MUST be signed: a descending row is legitimate and an unsigned subtraction would wrap
 * to a huge value that then clamps to the maximum.
 */
template <typename TValue>
int32_t interpolate(const uint16_t *axis, const TValue *values, uint8_t count, uint32_t x)
{
  if (count == 0U) { return 0; }
  if (x <= (uint32_t)axis[0U]) { return (int32_t)values[0U]; }
  if (x >= (uint32_t)axis[count - 1U]) { return (int32_t)values[count - 1U]; }

  uint8_t index = 0U;
  for (uint8_t i = 1U; i < count; ++i)
  {
    if ((uint32_t)axis[i] > x) { break; }
    index = i;
  }

  const uint8_t next = (uint8_t)(index + 1U);
  if (next >= count) { return (int32_t)values[index]; }

  const uint32_t a0 = (uint32_t)axis[index];
  const uint32_t a1 = (uint32_t)axis[next];
  if (a1 <= a0) { return (int32_t)values[index]; } // never divide by zero

  const int32_t v0 = (int32_t)values[index];
  const int32_t v1 = (int32_t)values[next];
  const int32_t delta = v1 - v0;
  const uint32_t frac = x - a0;
  const uint32_t full = a1 - a0;

  const int64_t scaled = (int64_t)delta * (int64_t)frac;
  const int64_t half = (int64_t)(full >> 1U);
  const int64_t rounded = (scaled >= 0) ? ((scaled + half) / (int64_t)full)
                                        : (-(((-scaled) + half) / (int64_t)full));
  return (int32_t)(v0 + (int32_t)rounded);
}

/**
 * @brief Advance the slew-limited retard toward its target, in TENTHS of a degree.
 *
 * The slew is specified as tenths per SECOND and applied against elapsed milliseconds, not per
 * call. That matters because this runs once per main-loop pass and the pass rate is not a
 * constant: a per-call limit would make the real retard rate depend on how busy the loop was.
 */
uint16_t advanceRetardSlew(uint16_t targetTenths, uint32_t nowMs, uint16_t msPerDegree,
                           uint16_t currentTenths)
{
  // 0 means "no slew": apply the target immediately. That is the default, because a slew is a
  // refinement and an engine that needs timing pulled should not have to wait for a ramp.
  if (msPerDegree == 0U) { return targetTenths; }

  const uint32_t elapsed = nowMs - rt.slewMs;
  rt.slewMs = nowMs;

  // Allow `elapsed` milliseconds of movement at msPerDegree milliseconds per degree. Expressed in
  // TENTHS so the arithmetic stays integral: one degree = 10 tenths, so the budget in tenths is
  //   elapsed * 10 / msPerDegree
  // Multiply before dividing, or a slow slew truncates to zero and never moves at all.
  const uint32_t budget = ((uint32_t)elapsed * 10U) / (uint32_t)msPerDegree;

  if (targetTenths > currentTenths)
  {
    const uint32_t risen = (uint32_t)currentTenths + budget;
    return (uint16_t)((risen > targetTenths) ? targetTenths : risen);
  }
  if (targetTenths < currentTenths)
  {
    const uint32_t fall = (uint32_t)currentTenths - targetTenths;
    return (uint16_t)((fall > budget) ? ((uint32_t)currentTenths - budget) : targetTenths);
  }
  return targetTenths;
}

/**
 * @brief The progressive retard across the soft zone.
 *
 * This is the heart of the feature and the thing an earlier draft got wrong. Retard does NOT snap
 * to its table value when the soft limit is crossed. It ramps LINEARLY:
 *
 *     at the soft limit   -> 0
 *     halfway through     -> half of maxRetard
 *     at the hard limit   -> maxRetard
 *
 * A step change in advance is indistinguishable from a misfire, and it makes the car feel like it
 * cut rather than like it is being managed. Just tripping the soft limit should pull barely
 * anything; only actually reaching the hard limit should pull the whole value.
 *
 * @param maxRetardTenths The table value: what this speed/rpm can afford at the HARD limit.
 * @param current         The live value being compared (axis or RPM).
 * @param setpoint        The hard limit at this operating point.
 * @param softPercent     Soft zone width as a percent of @p setpoint.
 * @return Retard to apply, tenths of a degree.
 */
uint16_t progressiveRetard(uint16_t maxRetardTenths, uint32_t current, uint32_t setpoint,
                           uint8_t softPercent)
{
  if ((maxRetardTenths == 0U) || (softPercent == 0U) || (setpoint == 0U)) { return 0U; }
  if (current >= setpoint) { return maxRetardTenths; }

  // The band starts this far below the setpoint.
  const uint32_t band = ((uint32_t)setpoint * (uint32_t)softPercent) / 100U;
  if (band == 0U) { return (current >= setpoint) ? maxRetardTenths : 0U; }

  const uint32_t softLimit = (setpoint > band) ? (setpoint - band) : 0U;
  if (current <= softLimit) { return 0U; }

  // Linear ramp: 0 at softLimit, maxRetard at setpoint.
  const uint32_t into = current - softLimit;
  const uint32_t span = setpoint - softLimit;
  const uint32_t scaled = ((uint32_t)maxRetardTenths * into) / span;
  return (uint16_t)scaled;
}

} // anonymous namespace

// =====================================================================================
// Defaults and clamps
// =====================================================================================

/** @brief Reset the speed page. MUST match the page-18 defaultValue entries. */
static void setSpeedDefaults(void)
{
  memset(&ppLaunchSpeedData, 0, sizeof(ppLaunchSpeedData));

  ppLaunchSpeedData.enable = (uint8_t)PP_DISABLED;
  ppLaunchSpeedData.armPin = 35U;
  ppLaunchSpeedData.armPolarity = 1U;
  ppLaunchSpeedData.armPullup = 0U;
  ppLaunchSpeedData.axisSource = (uint8_t)PP_AXIS_VSS_MPH10;
  ppLaunchSpeedData.limiterStrategy = (uint8_t)(PP_STRAT_BOOST_REDUCE | PP_STRAT_SPARK_CUT);
  ppLaunchSpeedData.curveMode = (uint8_t)PP_CURVE_BY_AXIS;
  ppLaunchSpeedData.featureFlags = (uint8_t)(PP_FLAG_PERFECT_PASS | PP_FLAG_RETARD_SLEW
                                           | PP_FLAG_AXIS_CLAMP);
  ppLaunchSpeedData.timeoutMs = 10000U;
  ppLaunchSpeedData.rearmBelowAxis = 0U;
  ppLaunchSpeedData.startBelowAxis = 0U;
  ppLaunchSpeedData.softZonePercent = 10U;    ///< 10% of the setpoint
  ppLaunchSpeedData.limitHysteresis = 10U;

  const char *label = "speed";
  for (size_t i = 0U; (i < strlen(label)) && (i < (size_t)(PP_LABEL_LEN - 1U)); ++i)
  {
    ppLaunchSpeedData.label[i] = (uint8_t)label[i];
  }

  // ONE axis, 32 points, 0 -> ~720 mph x10 (72.0 mph), with a shape that most cars can follow.
  static const uint16_t kAxis[PP_CURVE_POINTS] = {
      0U,  22U,  48U,  80U, 116U, 152U, 188U, 224U,
    258U, 292U, 324U, 356U, 386U, 414U, 442U, 468U,
    492U, 516U, 538U, 558U, 578U, 596U, 612U, 628U,
    642U, 656U, 668U, 680U, 690U, 700U, 710U, 718U };

  // Row 2: max retard at this speed, tenths of a degree. Highest off the line where there is
  // least traction, easing out as the car puts speed down.
  static const uint16_t kRetard[PP_CURVE_POINTS] = {
    120U, 118U, 114U, 108U, 100U,  90U,  78U,  66U,
     54U,  44U,  36U,  30U,  24U,  20U,  16U,  13U,
     10U,   8U,   6U,   5U,   4U,   4U,   3U,   3U,
      2U,   2U,   2U,   1U,   1U,   1U,   0U,   0U };

  // Row 1: reference time at this speed, ms. The inverse shape of the axis, so the wire reads as
  // "this speed should have been reached by now". 71.8 mph by 3.1 s.
  static const uint16_t kTime[PP_CURVE_POINTS] = {
      0U, 100U, 200U, 300U, 400U, 500U, 600U, 700U,
    800U, 900U,1000U,1100U,1200U,1300U,1400U,1500U,
   1600U,1700U,1800U,1900U,2000U,2100U,2200U,2300U,
   2400U,2500U,2600U,2700U,2800U,2900U,3000U,3100U };

  for (uint8_t i = 0U; i < PP_CURVE_POINTS; ++i)
  {
    ppLaunchSpeedData.axisSpeed[i] = kAxis[i];
    ppLaunchSpeedData.rowRetard[i] = kRetard[i];
    ppLaunchSpeedData.rowTimeMs[i] = kTime[i];
  }
}

/** @brief Reset the rpm page. MUST match the page-19 defaultValue entries. */
static void setRpmDefaults(void)
{
  memset(&ppLaunchRpmData, 0, sizeof(ppLaunchRpmData));

  ppLaunchRpmData.enable = (uint8_t)PP_DISABLED;
  ppLaunchRpmData.armPin = 35U;
  ppLaunchRpmData.armPolarity = 1U;
  ppLaunchRpmData.armPullup = 0U;
  ppLaunchRpmData.limiterStrategy = (uint8_t)(PP_STRAT_SPARK_CUT);
  ppLaunchRpmData.timeoutMs = 10000U;
  ppLaunchRpmData.softZonePercent = 10U;
  ppLaunchRpmData.limitHysteresis = 50U;

  const char *label = "rpm";
  for (size_t i = 0U; (i < strlen(label)) && (i < (size_t)(PP_LABEL_LEN - 1U)); ++i)
  {
    ppLaunchRpmData.label[i] = (uint8_t)label[i];
  }

  // One RPM axis, 3800 -> 7000 rpm.
  static const uint16_t kAxis[PP_CURVE_POINTS] = {
    3800U, 3903U, 4006U, 4110U, 4213U, 4316U, 4419U, 4523U,
    4626U, 4729U, 4832U, 4935U, 5039U, 5142U, 5245U, 5348U,
    5452U, 5555U, 5658U, 5761U, 5865U, 5968U, 6071U, 6174U,
    6277U, 6381U, 6484U, 6587U, 6690U, 6794U, 6897U, 7000U };

  static const uint16_t kRetard[PP_CURVE_POINTS] = {
    100U, 100U, 98U, 96U, 94U, 92U, 90U, 86U,
     82U,  78U, 74U, 70U, 66U, 62U, 58U, 54U,
     50U,  46U, 42U, 38U, 34U, 30U, 26U, 22U,
     18U,  14U, 10U,  8U,  6U,  4U,  2U,  0U };

  // Reference time at this RPM: reaching 7000 rpm should take about 3.1 s.
  static const uint16_t kTime[PP_CURVE_POINTS] = {
      0U, 100U, 200U, 300U, 400U, 500U, 600U, 700U,
    800U, 900U,1000U,1100U,1200U,1300U,1400U,1500U,
   1600U,1700U,1800U,1900U,2000U,2100U,2200U,2300U,
   2400U,2500U,2600U,2700U,2800U,2900U,3000U,3100U };

  for (uint8_t i = 0U; i < PP_CURVE_POINTS; ++i)
  {
    ppLaunchRpmData.axisValue[i] = kAxis[i];
    ppLaunchRpmData.rowRetard[i] = kRetard[i];
    ppLaunchRpmData.rowTimeMs[i] = kTime[i];
  }
}

/** @brief Reset the reactions page. MUST match the page-21 defaultValue entries. */
static void setReactDefaults(void)
{
  memset(&ppLaunchReactData, 0, sizeof(ppLaunchReactData));

  ppLaunchReactData.enable = (uint8_t)PP_DISABLED;

  const char *label = "reactions";
  for (size_t i = 0U; (i < strlen(label)) && (i < (size_t)(PP_LABEL_LEN - 1U)); ++i)
  {
    ppLaunchReactData.label[i] = (uint8_t)label[i];
  }

  // The axis is fixed. Stored so the ini and the firmware agree by inspection.
  for (uint8_t i = 0U; i < PP_REACTION_POINTS; ++i)
  {
    ppLaunchReactData.axisTimeMs[i] = PP_REACTION_TIME_MS[i];
  }

  // A plausible reaction: nothing at 0, then progressively more of everything. The car is being
  // held, so add fuel to keep it from going lean on the cut, pull boost duty so it makes less
  // torque, and add a little retard on top of the table's.
  static const uint16_t kFuel[PP_REACTION_POINTS] = { 0U, 2U, 4U, 6U, 8U, 10U };
  static const int16_t  kDuty[PP_REACTION_POINTS] = { 0, -5, -10, -15, -20, -25 };
  static const int16_t  kTgt[PP_REACTION_POINTS]  = { 0, -2, -4, -6, -8, -10 };
  static const int16_t  kRet[PP_REACTION_POINTS]  = { 0, 0, 5, 10, 15, 20 };

  for (uint8_t i = 0U; i < PP_REACTION_POINTS; ++i)
  {
    ppLaunchReactData.fuelAddPercent[i] = kFuel[i];
    ppLaunchReactData.boostDutyAdj[i] = kDuty[i];
    ppLaunchReactData.boostTargetAdj[i] = kTgt[i];
    ppLaunchReactData.retardAdj[i] = kRet[i];
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
  if (s.curveMode > (uint8_t)PP_CURVE_BY_TIME) { s.curveMode = (uint8_t)PP_CURVE_BY_AXIS; }
  if (s.softZonePercent > 90U) { s.softZonePercent = 90U; } // 100% would mean a zero setpoint band
  s.limiterStrategy = (uint8_t)(s.limiterStrategy & 0x0FU);
  s.featureFlags = (uint8_t)(s.featureFlags & 0x0FU);

  // The axis is the lookup key for BOTH rows, so it must be strictly ascending.
  for (uint8_t i = 1U; i < PP_CURVE_POINTS; ++i)
  {
    if (s.axisSpeed[i] <= s.axisSpeed[i - 1U])
    {
      s.axisSpeed[i] = (uint16_t)((s.axisSpeed[i - 1U] < 0xFFFFU)
                                ? (s.axisSpeed[i - 1U] + 1U) : s.axisSpeed[i - 1U]);
    }
  }

  for (uint8_t i = 0U; i < PP_CURVE_POINTS; ++i)
  {
    s.rowRetard[i] = clampTenths(s.rowRetard[i]);
    // The reference-time row is NOT a lookup key, so it is only sanity-bounded. A car can
    // legitimately hold one speed for a while, so equal neighbours are allowed here.
  }

  s.label[PP_LABEL_LEN - 1U] = 0U;
}

/** @brief Clamp and repair the rpm page. */
static void clampRpmSettings(void)
{
  ppLaunchRpmConfig &s = ppLaunchRpmData;

  if (s.enable > (uint8_t)PP_ENABLED) { s.enable = (uint8_t)PP_DISABLED; }
  if (s.armPolarity > 1U) { s.armPolarity = 0U; }
  if (s.armPullup > 1U) { s.armPullup = 1U; }
  if (s.softZonePercent > 90U) { s.softZonePercent = 90U; }
  s.limiterStrategy = (uint8_t)(s.limiterStrategy & 0x0FU);

  for (uint8_t i = 1U; i < PP_CURVE_POINTS; ++i)
  {
    if (s.axisValue[i] <= s.axisValue[i - 1U])
    {
      s.axisValue[i] = (uint16_t)((s.axisValue[i - 1U] < 0xFFFFU)
                                ? (s.axisValue[i - 1U] + 1U) : s.axisValue[i - 1U]);
    }
  }
  for (uint8_t i = 0U; i < PP_CURVE_POINTS; ++i)
  {
    s.rowRetard[i] = clampTenths(s.rowRetard[i]);
  }

  s.label[PP_LABEL_LEN - 1U] = 0U;
}

/** @brief Clamp and repair the reactions page. */
static void clampReactSettings(void)
{
  ppLaunchReactConfig &s = ppLaunchReactData;

  if (s.enable > (uint8_t)PP_ENABLED) { s.enable = (uint8_t)PP_DISABLED; }

  // The axis is fixed. Forcing it here rather than trusting the stored bytes means a hand-edited
  // or truncated block cannot shift the whole reaction ramp.
  for (uint8_t i = 0U; i < PP_REACTION_POINTS; ++i)
  {
    s.axisTimeMs[i] = PP_REACTION_TIME_MS[i];
  }

  for (uint8_t i = 0U; i < PP_REACTION_POINTS; ++i)
  {
    // Fuel add is unsigned in storage but a sensible range is 0..100%.
    if (s.fuelAddPercent[i] > 100U) { s.fuelAddPercent[i] = 100U; }
    // Adjustments are signed; bound them to something a real tune can use.
    if (s.boostDutyAdj[i] > 100) { s.boostDutyAdj[i] = 100; }
    if (s.boostDutyAdj[i] < -100) { s.boostDutyAdj[i] = -100; }
    if (s.boostTargetAdj[i] > 200) { s.boostTargetAdj[i] = 200; }
    if (s.boostTargetAdj[i] < -200) { s.boostTargetAdj[i] = -200; }
    if (s.retardAdj[i] > 600) { s.retardAdj[i] = 600; }
    if (s.retardAdj[i] < 0) { s.retardAdj[i] = 0; }
  }

  s.label[PP_LABEL_LEN - 1U] = 0U;
}

// =====================================================================================
// Storage
// =====================================================================================

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

static uint32_t payloadCrc(PpMode mode)
{
  const byte *p = (const byte *)payloadPtr(mode);
  return crc32Payload(p, p + PPLAUNCH_PAYLOAD_SIZE);
}

static bool probeStorage(void)
{
  const uint16_t length = getStorageAPI().length();
  return (uint32_t)length >= ((uint32_t)EEPROM_PPLAUNCH_REACT_BASE + (uint32_t)PPLAUNCH_BLOCK_SIZE);
}

static bool payloadDiffers(PpMode mode)
{
  byte stored[PPLAUNCH_PAYLOAD_SIZE];
  (void)loadBlock(getStorageAPI(), payloadFor(mode), stored, stored + PPLAUNCH_PAYLOAD_SIZE);
  return memcmp(stored, payloadPtr(mode), PPLAUNCH_PAYLOAD_SIZE) != 0;
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
  const byte *pStart = (const byte *)payloadPtr(mode);
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

void ppLaunchMarkDirty(PpMode mode) { markDirty(mode, true); }

// =====================================================================================
// Arm inputs
// =====================================================================================

static void bindArmPin(PpMode mode)
{
  const bool isRpm = (mode == PP_MODE_RPM);
  const uint8_t configured = isRpm ? ppLaunchRpmData.armPin : ppLaunchSpeedData.armPin;
  const uint8_t pullup = isRpm ? ppLaunchRpmData.armPullup : ppLaunchSpeedData.armPullup;
  uint8_t &boundPin = isRpm ? rt.rpmArmPin : rt.speedArmPin;
  bool &bound = isRpm ? rt.rpmPinBound : rt.speedPinBound;

  if (configured == 0U) { boundPin = 0U; bound = false; return; }

  // Note what is NOT checked: pin 0. Zero is a perfectly valid board pin - board_definition.h
  // defines NOT_A_PIN as UINT8_MAX precisely because zero cannot be the "unused" marker.
  // Treating a translated 0 as "untranslatable" refuses pin 0 while accepting every reserved pin
  // it could not use - a bug that already shipped once in this tree.
  const uint8_t pin = pinTranslate(configured);
  if (pinIsReserved(pin) || pinIsOutput(pin)) { boundPin = 0U; bound = false; return; }

  boundPin = pin;
  pinMode(pin, (pullup != 0U) ? INPUT_PULLUP : INPUT);
  bound = true;
}

static bool readArmPin(PpMode mode)
{
#if defined(UNIT_TEST)
  if (armOverrideActive) { return armOverrideValue; }
#endif
  const bool isRpm = (mode == PP_MODE_RPM);
  if (!(isRpm ? rt.rpmPinBound : rt.speedPinBound)) { return false; }

  const uint8_t pin = isRpm ? rt.rpmArmPin : rt.speedArmPin;
  const uint8_t polarity = isRpm ? ppLaunchRpmData.armPolarity : ppLaunchSpeedData.armPolarity;
  const bool high = (digitalRead(pin) == HIGH);
  return (polarity == 0U) ? high : !high;
}

// =====================================================================================
// Axis (speed mode)
// =====================================================================================

static bool axisNeedsVss(void)
{
  const uint8_t src = ppLaunchSpeedData.axisSource;
  return (src == (uint8_t)PP_AXIS_VSS_MPH10)
      || (src == (uint8_t)PP_AXIS_VSS_KPH10)
      || (src == (uint8_t)PP_AXIS_SHAFT_RPM);
}

static uint16_t readRawAxis(const statuses &current)
{
#if defined(UNIT_TEST)
  if (axisOverrideActive) { return axisOverride; }
#endif
  const uint16_t vss10 = (uint16_t)(current.vss * 10U);

  switch ((PpAxisSource)ppLaunchSpeedData.axisSource)
  {
    case PP_AXIS_VSS_KPH10: return vss10;
    case PP_AXIS_SHAFT_RPM:
      return (uint16_t)(((uint32_t)current.vss * (uint32_t)ppLaunchSpeedData.shaftRevPerKph) / 100U);
    case PP_AXIS_RPM: return current.RPM;
    case PP_AXIS_VSS_MPH10:
    default: return kmh10ToMph10(vss10);
  }
}

/**
 * @brief Condition the raw axis: spike clamp, then IIR smoothing.
 *
 * The spike clamp matters: a single noise tooth crosses every bin on the curve in one update, so
 * the output goes from "leave hard" to "full retard" in a pass. Clamping the change per update
 * turns that into a reading that lags reality by a few milliseconds.
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

  const int32_t delta = (int32_t)clamped - (int32_t)rt.axis;
  const int32_t step = delta / (int32_t)(1U << shift);
  const int32_t next = (int32_t)rt.axis + step;
  if (next <= 0) { return 0U; }
  if (next > 65535) { return 65535U; }
  return (uint16_t)next;
}

// =====================================================================================
// State machines
// =====================================================================================

static void tickStateMachine(PpState &state, bool arm, bool prevArm, uint16_t timeoutMs,
                             uint32_t &armedMs, uint32_t &ms)
{
  switch (state)
  {
    case PP_STATE_OFF:
    case PP_STATE_IDLE:
      if (arm) { state = PP_STATE_ARMED; ms = 0U; armedMs = 0U; }
      else { state = PP_STATE_IDLE; ms = 0U; }
      break;

    case PP_STATE_ARMED:
      if (arm)
      {
        // The timeout stops a car that got armed on the highway from sitting armed indefinitely.
        ++armedMs;
        if ((timeoutMs != 0U) && (armedMs >= (uint32_t)timeoutMs)) { state = PP_STATE_TIMEOUT; }
        else { ms = 0U; }
      }
      else
      {
        // Release edge: the clock starts here. Release, not assertion - the run begins when the
        // trans-brake or clutch is let go, which is when the car actually moves.
        state = PP_STATE_LAUNCHING; ms = 0U; armedMs = 0U;
      }
      break;

    case PP_STATE_LAUNCHING:
      ++ms;
      break;

    case PP_STATE_COMPLETE:
    case PP_STATE_TIMEOUT:
      if (arm && !prevArm) { state = PP_STATE_ARMED; armedMs = 0U; ms = 0U; }
      else if (!arm && (state == PP_STATE_TIMEOUT)) { state = PP_STATE_IDLE; }
      else
      {
        // COMPLETE latches deliberately: the driver released the arm input to launch, so falling
        // back to IDLE would destroy the only record that the run finished.
      }
      break;

    default:
      state = PP_STATE_OFF; // MISRA
      break;
  }
}

void ppLaunchTick1ms(statuses &current)
{
  (void)current;

  static bool prevArmSpeed = false;
  static bool prevArmRpm = false;

  // ---- speed page (page 18) ------------------------------------------------------------
  if (speedEnabled())
  {
    const bool arm = readArmPin(PP_MODE_SPEED);
    tickStateMachine(rt.speedState, arm, prevArmSpeed, ppLaunchSpeedData.timeoutMs,
                     rt.armedMs, rt.ms);
    prevArmSpeed = arm;
  }
  else
  {
    rt.speedState = PP_STATE_OFF;
    prevArmSpeed = false;
    rt.ms = 0U;
    rt.armedMs = 0U;
  }

  // ---- rpm page (page 19) --------------------------------------------------------------
  // Both modes run at once, so they keep separate clocks: enabling both must not have them
  // fighting over one counter. The two are armed independently and could legitimately be at
  // different points in their curves.
  if (rpmEnabled())
  {
    const bool arm = readArmPin(PP_MODE_RPM);
    tickStateMachine(rt.rpmState, arm, prevArmRpm, ppLaunchRpmData.timeoutMs,
                     rt.rpmArmedMs, rt.rpmMs);
    prevArmRpm = arm;
  }
  else
  {
    rt.rpmState = PP_STATE_OFF;
    prevArmRpm = false;
    rt.rpmMs = 0U;
    rt.rpmArmedMs = 0U;
  }
}

// =====================================================================================
// Evaluation
// =====================================================================================

namespace {

/** @brief Accumulate time at the hard limit, saturating at the reaction axis's last point. */
void accumulateLimitHit(bool limiting, uint16_t maxMs)
{
  if (!limiting)
  {
    // Reset only when the limit has genuinely been left alone. This counter is "how long have we
    // been fighting the limit", so a single millisecond off it ends the episode.
    rt.limitHitMs = 0U;
    return;
  }
  if (rt.limitHitMs < maxMs) { ++rt.limitHitMs; }
}

/** @brief Evaluate the reactions table against the accumulated time at the limit. */
void updateReactions(void)
{
  if ((ppLaunchReactData.enable != (uint8_t)PP_ENABLED) || (rt.limitHitMs == 0U))
  {
    rt.fuelAdd = 0;
    rt.boostDutyAdj = 0;
    rt.boostTargetAdj = 0;
    rt.reactionRetardTenths = 0U;
    return;
  }

  const uint32_t ms = rt.limitHitMs;

  rt.fuelAdd = (int16_t)interpolate(ppLaunchReactData.axisTimeMs,
                                    ppLaunchReactData.fuelAddPercent, PP_REACTION_POINTS, ms);
  rt.boostDutyAdj = (int16_t)interpolate(ppLaunchReactData.axisTimeMs,
                                         ppLaunchReactData.boostDutyAdj, PP_REACTION_POINTS, ms);
  rt.boostTargetAdj = (int16_t)interpolate(ppLaunchReactData.axisTimeMs,
                                           ppLaunchReactData.boostTargetAdj, PP_REACTION_POINTS, ms);
  rt.reactionRetardTenths = (uint16_t)interpolate(ppLaunchReactData.axisTimeMs,
                                                  ppLaunchReactData.retardAdj, PP_REACTION_POINTS, ms);
}

/** @brief Evaluate the speed page (page 18). */
void updateSpeedMode(const statuses &current, const config2 &page2)
{
  const ppLaunchSpeedConfig &cfg = ppLaunchSpeedData;

  const bool vssMissing = axisNeedsVss() && (page2.vssMode == VSS_MODE_OFF);
  if (vssMissing)
  {
    rt.axis = 0U;
    rt.slipMs = 0;
    rt.axisSeen = false;
    rt.speedLimit = UINT16_MAX;
    rt.speedStrategy = 0U;
    rt.retardTenths = 0U;
    rt.retardSlewed = 0U;
    ppLaunchAxisLog = 0U;
    ppLaunchSlipLog = 0;
    return;
  }

  rt.axis = conditionAxis(readRawAxis(current), !rt.axisSeen);
  rt.axisSeen = true;
  ppLaunchAxisLog = rt.axis;

  // ---- reference-time wire -------------------------------------------------------------
  // The car is compared against the time it SHOULD have taken to reach this speed. A positive
  // deviation means it got here early, which on a speed axis is the signature of wheelspin.
  if ((cfg.featureFlags & PP_FLAG_PERFECT_PASS) != 0U)
  {
    const int32_t reference = interpolate(cfg.axisSpeed, cfg.rowTimeMs, PP_CURVE_POINTS, rt.axis);
    rt.slipMs = (int16_t)(reference - (int32_t)rt.ms);
  }
  else
  {
    rt.slipMs = 0;
  }
  ppLaunchSlipLog = rt.slipMs;

  // ---- re-arm --------------------------------------------------------------------------
  if ((rt.speedState == PP_STATE_LAUNCHING) && (cfg.rearmBelowAxis != 0U)
      && ((uint32_t)rt.axis < (uint32_t)cfg.rearmBelowAxis))
  {
    rt.speedState = PP_STATE_IDLE;
    rt.ms = 0U;
    rt.speedLimit = UINT16_MAX;
    rt.retardTenths = 0U;
    rt.retardSlewed = 0U;
    return;
  }

  if (rt.speedState != PP_STATE_LAUNCHING)
  {
    rt.speedLimit = UINT16_MAX;
    rt.speedStrategy = 0U;
    rt.retardTenths = 0U;
    rt.retardSlewed = 0U;
    return;
  }

  // ---- the soft-zone retard ------------------------------------------------------------
  // Max retard at this speed, then ramped linearly from zero at the soft limit up to that value
  // at the hard limit. Just tripping the soft limit should pull barely anything.
  const uint16_t maxRetard = (uint16_t)interpolate(cfg.axisSpeed, cfg.rowRetard, PP_CURVE_POINTS, rt.axis);

  // The setpoint the soft zone is measured against. Page 18's own table does not carry an RPM
  // limit (page 19 owns that), so the speed page's soft zone is measured against its HARD speed
  // gate: the point at which the speed limiter is fully engaged.
  // The setpoint for the soft zone is the top of the table's axis: the same figure that closes
  // the launch window and therefore the hard limit. Measuring the band against anything else
  // makes the retard reach its fully value somewhere other than the limit, which is the one
  // thing the ramp must not do.
  const uint32_t setpoint = (uint32_t)cfg.axisSpeed[PP_CURVE_POINTS - 1U];
  rt.retardTenths = (setpoint != 0U)
                  ? progressiveRetard(maxRetard, rt.axis, setpoint, cfg.softZonePercent)
                  : maxRetard;

  rt.retardSlewed = advanceRetardSlew(rt.retardTenths, rt.ms,
                                      ((cfg.featureFlags & PP_FLAG_RETARD_SLEW) != 0U)
                                        ? cfg.retardSlewMsPerDeg : 0U,
                                      rt.retardSlewed);

  // ---- the launch window ---------------------------------------------------------------
  // The limiter engages while the car is still inside the launch window, which ends at the TOP
  // of the table's axis: past that speed the launch is over and normal control resumes.
  //
  // `startBelowAxis` is an OPTIONAL extra gate the tuner can set. It is deliberately NOT the
  // same thing as the setpoint the soft zone measures against - conflating the two meant the
  // limiter released exactly at the point where it should have been at full authority.
  const uint32_t windowTop = (uint32_t)cfg.axisSpeed[PP_CURVE_POINTS - 1U];
  // Inclusive of the top: at exactly the last column the limiter is at FULL authority, which is
  // the whole point of the soft-zone ramp ending there. The launch ends when the car goes past it.
  const bool inWindow = (rt.axis <= windowTop)
                     && ((cfg.startBelowAxis == 0U)
                         || ((uint32_t)rt.axis < (uint32_t)cfg.startBelowAxis));

  const bool gearOk = ((cfg.featureFlags & PP_FLAG_GEAR_GATE) == 0U)
                   || (cfg.maxGear == 0U) || (current.gear == 0U)
                   || (current.gear <= cfg.maxGear);

  if (inWindow && gearOk && (cfg.limiterStrategy != 0U))
  {
    rt.speedStrategy = cfg.limiterStrategy;
    // The limit in force is the speed at which the launch window closes: that is the hard limit
    // this page owns. The RPM limit, if any, comes from page 19 and is arbitrated separately.
    rt.speedLimit = (windowTop != 0U) ? (uint16_t)windowTop : UINT16_MAX;
  }
  else
  {
    rt.speedStrategy = 0U;
    rt.speedLimit = UINT16_MAX;
  }
}

/** @brief Evaluate the rpm page (page 19). */
void updateRpmMode(const statuses &current)
{
  const ppLaunchRpmConfig &cfg = ppLaunchRpmData;

  if (rt.rpmState != PP_STATE_LAUNCHING)
  {
    rt.rpmLimit = UINT16_MAX;
    rt.rpmStrategy = 0U;
    rt.rpmRetardTenths = 0U;
    return;
  }

  // The hard limit at this RPM. The axis IS the RPM, so the limit is the axis value itself: the
  // table says "at 6000 rpm you may have 6000 rpm", which is how a setpoint-versus-rpm table is
  // read. The retard row shapes the approach.
  // The setpoint at this RPM is the RPM itself: the table says "at 6000 rpm the limit is 6000".
  const uint16_t setpoint = current.RPM;
  const uint16_t maxRetard = (uint16_t)interpolate(cfg.axisValue, cfg.rowRetard, PP_CURVE_POINTS,
                                                   current.RPM);

  rt.rpmRetardTenths = progressiveRetard(maxRetard, current.RPM, setpoint, cfg.softZonePercent);

  if (cfg.limiterStrategy != 0U)
  {
    rt.rpmLimit = setpoint;
    rt.rpmStrategy = cfg.limiterStrategy;
  }
  else
  {
    rt.rpmLimit = UINT16_MAX;
    rt.rpmStrategy = 0U;
  }
}

} // anonymous namespace

void ppLaunchUpdate(statuses &current, const config2 &page2)
{
  if (!ppLaunchIsActive())
  {
    clearOutputs();
    rt.axis = 0U;
    rt.slipMs = 0;
    ppLaunchAxisLog = 0U;
    ppLaunchSlipLog = 0;
    return;
  }

  // Both modes are evaluated. They are independent controllers that are meant to run together:
  // the speed page shapes the launch, the rpm page is a hard ceiling.
  if (speedEnabled()) { updateSpeedMode(current, page2); } else { rt.speedLimit = UINT16_MAX; rt.speedStrategy = 0U; }
  if (rpmEnabled())   { updateRpmMode(current); }        else { rt.rpmLimit = UINT16_MAX; rt.rpmStrategy = 0U; }

  // ---- arbitration: the LOWER limit wins ------------------------------------------------
  // The strategy follows whichever mode is actually binding, so the cut method applied is the one
  // the tuner configured on the page that caught the engine.
  if (rt.rpmLimit < rt.speedLimit)
  {
    rt.limiting = (rt.rpmLimit != UINT16_MAX);
    rt.speedStrategy = 0U;
  }
  else
  {
    rt.limiting = (rt.speedLimit != UINT16_MAX);
    rt.rpmStrategy = 0U;
  }

  // ---- reactions, keyed on accumulated time at the hard limit --------------------------
  accumulateLimitHit(rt.limiting, PP_REACTION_TIME_MS[PP_REACTION_POINTS - 1U]);
  updateReactions();

  // ---- retard --------------------------------------------------------------------------
  // The larger of the two modes' retard: both are asking for timing out of the same engine, and
  // the bus would take the largest anyway, but doing it here keeps the published number equal to
  // the number the reactions adjusted.
  uint16_t retardTenths = (rt.rpmRetardTenths > rt.retardSlewed)
                        ? rt.rpmRetardTenths : rt.retardSlewed;
  retardTenths = clampTenths((uint16_t)(retardTenths + rt.reactionRetardTenths));

  const uint8_t retardWhole = (uint8_t)((retardTenths + 5U) / 10U);

  // Retard leaves through the torque bus so the arbiter's "largest single request wins" rule can
  // stop it stacking with the race-time curve or a future traction controller.
  torqueRequestRetard(TorqueProducer::Launch, (int8_t)retardWhole);

  // ---- publish for logging -------------------------------------------------------------
  current.ppLaunchAxis = rt.axis;
  current.ppLaunchSlip = rt.slipMs;
  current.ppLaunchLimit = (ppLaunchRpmLimit() == UINT16_MAX) ? 0U : ppLaunchRpmLimit();
  current.ppLaunchRetard = retardWhole;
  current.ppLaunchState = (uint8_t)ppLaunchState();
  current.ppLaunchStatus = (uint8_t)(((rt.limiting ? 1U : 0U) << 0U)
                                   | (((rt.limitHitMs != 0U) ? 1U : 0U) << 1U)
                                   | ((uint8_t)torqueBusRetardProducer() << 2U));
}

// =====================================================================================
// Boot
// =====================================================================================

static void loadBlock(PpMode mode)
{
  const uint8_t bit = modeBit(mode);

  ppLaunchBlockHeader header;
  (void)loadObject(getStorageAPI(), baseFor(mode), header);

  const bool magicOk = (header.magic == magicFor(mode))
                    && (header.version == EEPROM_PPLAUNCH_VERSION);

  if (!magicOk)
  {
    switch (mode)
    {
      case PP_MODE_RPM:   setRpmDefaults();   clampRpmSettings();   break;
      case PP_MODE_REACT: setReactDefaults(); clampReactSettings(); break;
      case PP_MODE_SPEED:
      default:            setSpeedDefaults(); clampSpeedSettings(); break;
    }
    rt.initialisedMask = (uint8_t)(rt.initialisedMask & (uint8_t)(~bit));
    markDirty(mode, true);
    (void)ppLaunchSave(mode);
    return;
  }

  switch (mode)
  {
    case PP_MODE_RPM:
      (void)loadObject(getStorageAPI(), payloadFor(mode), ppLaunchRpmData);
      clampRpmSettings();
      break;
    case PP_MODE_REACT:
      (void)loadObject(getStorageAPI(), payloadFor(mode), ppLaunchReactData);
      clampReactSettings();
      break;
    case PP_MODE_SPEED:
    default:
      (void)loadObject(getStorageAPI(), payloadFor(mode), ppLaunchSpeedData);
      clampSpeedSettings();
      break;
  }

  // Repair FIRST, then verify: the repair makes a slightly damaged block usable, but changes
  // bytes, so the CRC must be taken against the repaired payload.
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
    setSpeedDefaults(); clampSpeedSettings();
    setRpmDefaults();   clampRpmSettings();
    setReactDefaults(); clampReactSettings();
    bindArmPin(PP_MODE_SPEED);
    bindArmPin(PP_MODE_RPM);
    return;
  }

  loadBlock(PP_MODE_SPEED);
  loadBlock(PP_MODE_RPM);
  loadBlock(PP_MODE_REACT);

  bindArmPin(PP_MODE_SPEED);
  bindArmPin(PP_MODE_RPM);
}

void ppLaunchRestoreDefaults(void)
{
  setSpeedDefaults(); clampSpeedSettings();
  setRpmDefaults();   clampRpmSettings();
  setReactDefaults(); clampReactSettings();
  markDirty(PP_MODE_SPEED, true);
  markDirty(PP_MODE_RPM, true);
  markDirty(PP_MODE_REACT, true);
}

// =====================================================================================
// Accessors
// =====================================================================================

uint16_t ppLaunchRpmLimit(void)
{
  return (rt.rpmLimit < rt.speedLimit) ? rt.rpmLimit : rt.speedLimit;
}

uint8_t ppLaunchStrategy(void)
{
  return (uint8_t)(rt.rpmStrategy | rt.speedStrategy);
}

PpState ppLaunchState(void) { return rt.speedState; }
PpState ppLaunchRpmState(void) { return rt.rpmState; }

uint8_t ppLaunchRetard(void)
{
  uint16_t t = (rt.rpmRetardTenths > rt.retardSlewed) ? rt.rpmRetardTenths : rt.retardSlewed;
  t = clampTenths((uint16_t)(t + rt.reactionRetardTenths));
  return (uint8_t)((t + 5U) / 10U);
}

int16_t ppLaunchFuelAdd(void) { return rt.fuelAdd; }
int16_t ppLaunchBoostDutyAdj(void) { return rt.boostDutyAdj; }
int16_t ppLaunchBoostTargetAdj(void) { return rt.boostTargetAdj; }
uint16_t ppLaunchLimitHitMs(void) { return rt.limitHitMs; }
uint16_t ppLaunchAxisValue(void) { return rt.axis; }
int16_t ppLaunchSlipMs(void) { return rt.slipMs; }
bool ppLaunchIsLimiting(void) { return rt.limiting; }
bool ppLaunchIsAvailable(void) { return rt.storageOk; }

PpMode ppLaunchActiveMode(void)
{
  // Which page is actually binding.
  if (rt.rpmLimit < rt.speedLimit) { return PP_MODE_RPM; }
  return speedEnabled() ? PP_MODE_SPEED : PP_MODE_RPM;
}

uint32_t ppLaunchElapsedMs(void)
{
  return (rt.speedState == PP_STATE_LAUNCHING) ? rt.ms : 0U;
}

bool ppLaunchIsActive(void) { return speedEnabled() || rpmEnabled(); }

void ppLaunchReset(void)
{
  rt.reset();
  const bool active = ppLaunchIsActive();
  rt.speedState = active ? PP_STATE_IDLE : PP_STATE_OFF;
  rt.rpmState = active ? PP_STATE_IDLE : PP_STATE_OFF;
}

ppLaunchSpeedBlock &ppLaunchGetSpeedPageMutable(void) { return ppLaunchSpeedData; }
ppLaunchRpmBlock &ppLaunchGetRpmPageMutable(void) { return ppLaunchRpmData; }
ppLaunchReactBlock &ppLaunchGetReactPageMutable(void) { return ppLaunchReactData; }
const ppLaunchSpeedBlock &ppLaunchGetSpeedPage(void) { return ppLaunchSpeedData; }
const ppLaunchRpmBlock &ppLaunchGetRpmPage(void) { return ppLaunchRpmData; }
const ppLaunchReactBlock &ppLaunchGetReactPage(void) { return ppLaunchReactData; }

bool ppLaunchIsInitialised(PpMode mode)
{
  return (rt.initialisedMask & modeBit(mode)) != 0U;
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
  setSpeedDefaults(); clampSpeedSettings();
  setRpmDefaults();   clampRpmSettings();
  setReactDefaults(); clampReactSettings();
  ppLaunchAxisLog = 0U;
  ppLaunchSlipLog = 0;
}

void ppLaunchSetAxisForTest(uint16_t axis) { axisOverride = axis; axisOverrideActive = true; }
void ppLaunchSetArmForTest(bool asserted) { armOverrideValue = asserted; armOverrideActive = true; }
#endif
