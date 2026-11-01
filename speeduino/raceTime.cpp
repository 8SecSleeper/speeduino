#include "raceTime.h"
#include "statuses.h"
#include "storage.h"
#include "globals.h"
#include "maths.h"
#include "unit_testing.h"
#include "src/pins/pinNumbers_t.h"
#include "src/pins/pinMapping.h"
#include <Arduino.h>
#include <string.h>

/**
 * @file
 * @brief Race-time framework: the clock, its state machine and its persistence.
 *
 * See raceTime.h for the design contract and the storage layout. The properties that are easy
 * to break by accident, restated:
 *
 *  1. The clock advances only from @ref raceTimeTick1ms, and only while
 *     @ref RaceTimeState::RT_STATE_RUNNING. Anything else that adds to it makes the time
 *     depend on loop cadence.
 *  2. A disable (or a config burn) resets the clock. A tuner who turns the feature off must not
 *     be left with a stale 47 s "race time" reading that some future consumer acts on.
 *  3. Nothing here touches storage unless the backend can hold the block.
 */

// =====================================================================================
// State
// =====================================================================================

/** @brief The live page payload. This is what TunerStudio reads and writes. */
raceTimeBlock raceTimeData;

/** @brief The elapsed race time in milliseconds. The one clock. */
static uint32_t raceTimeMs = 0;

/** @brief Current state machine state. */
static RaceTimeState raceTimeState = RT_STATE_OFF;

/** @brief Set when a movement before the start release invalidated the run. */
static bool raceTimeFalseStart = false;

/** @brief Previous (polarity-corrected) arm-pin level, for release-edge detection. */
static bool raceTimePrevPinHigh = false;

/** @brief Set while the clock is running and the stop source is @ref RT_STOP_TIME. */
static uint32_t raceTimeStopThreshold = 0;

/** @brief True once the block has been loaded or defaulted at boot. */
static bool raceTimeReady = false;

/** @brief True if the storage backend is big enough to hold the whole block. */
static bool raceTimeStorageOk = false;

/** @brief Set when the settings change. Cleared on a successful save. */
static bool raceTimeDirty = false;

/** @brief True while the arm pin has actually been configured as an input. */
static bool raceTimePinBound = false;

/** @brief The board pin the arm input resolved to, or 0 when unbound. */
static uint8_t raceTimeArmPin = 0;

// =====================================================================================
// Small helpers
// =====================================================================================

/** @brief Reset the payload to a safe, feature-disabled state. */
static void setPageToDefaults(void)
{
  memset(&raceTimeData, 0, sizeof(raceTimeData));

  // These MUST match the `defaultValue =` entries for page 17 in reference/speeduino.ini. A
  // disagreement is not caught by any test, and shows up as a different curve depending on
  // whether the tuner pressed Burn or the ECU laid the block down itself.
  //
  // The feature ships DISABLED, but otherwise configured for a real drag car, so enabling it is
  // one switch rather than an empty page to fill in from scratch.
  raceTimeData.enable = RT_DISABLED;
  raceTimeData.armPin = 35U;                 // pin 34 is the multiplexed VSS/cam-3 pin
  raceTimeData.armPolarity = 1U;             // active-low trans-brake switch is the common case
  raceTimeData.armPullup = 0U;               // external switch pulls to ground; no internal pull-up
  raceTimeData.startSource = (uint8_t)RT_START_LAUNCH_PIN_RELEASE;
  raceTimeData.stopSource = (uint8_t)RT_STOP_NONE;
  raceTimeData.motionSource = (uint8_t)RT_MOTION_VSS;
  raceTimeData.curveEnable = 1U;

  raceTimeData.stopTimeMs = 0U;
  raceTimeData.stopSpeed = 0U;
  raceTimeData.falseStartSpeed = 2U;

  // 16 points over 6 s. Retard off the line for traction, then give it back: 12 degrees of pull
  // at the hit, easing to 0 by 3.2 s. This is the shape a turbo drag car actually runs, not a
  // flat line - a flat line is what you get when nobody thought about the defaults.
  {
    static const uint16_t kDefaultTimes[RACETIME_CURVE_POINTS] =
      { 0U, 400U, 800U, 1200U, 1600U, 2000U, 2400U, 2800U,
        3200U, 3600U, 4000U, 4400U, 4800U, 5200U, 5600U, 6000U };
    static const uint8_t kDefaultRetard[RACETIME_CURVE_POINTS] =
      { 12U, 11U, 10U, 9U, 8U, 7U, 6U, 4U, 2U, 1U, 0U, 0U, 0U, 0U, 0U, 0U };

    for (uint8_t i = 0; i < RACETIME_CURVE_POINTS; ++i)
    {
      raceTimeData.curveTimeMs[i] = kDefaultTimes[i];
      raceTimeData.curveRetard[i] = kDefaultRetard[i];
    }
  }

  // A blank label is useless in the tuning software, so ship a readable one. The tuner can
  // rename it from TunerStudio; the point of the field is that they do not have to.
  static const char kDefaultLabel[] = "raceTime";
  (void)memcpy(raceTimeData.curveLabel, kDefaultLabel, sizeof(kDefaultLabel));

  // Retard defaults to zero everywhere: an unconfigured curve must be a no-op, not a guess.
}

/**
 * @brief CRC32 (IEEE, reflected, as used by the tuning protocol) over the payload.
 *
 * Implemented here rather than reusing FastCRC so that the stored value is computed by the
 * same function that validates it, with no dependency on how a third-party library happens to
 * be compiled. This is deliberately identical to the ltt implementation - one algorithm, two
 * blocks, no chance of them disagreeing about what "corrupt" means.
 */
static uint32_t crc32Payload(const byte *pStart, const byte *pEnd)
{
  uint32_t crc = 0xFFFFFFFFU;
  for (const byte *p = pStart; p != pEnd; ++p)
  {
    crc ^= (uint32_t)(*p);
    for (uint8_t bit = 0; bit < 8U; ++bit)
    {
      // Reflected polynomial 0xEDB88320
      const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc = (crc >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

/** @brief The header as it is laid out in storage. */
struct raceTimeBlockHeader
{
  uint32_t magic;      ///< @ref EEPROM_RACETIME_MAGIC
  uint8_t version;     ///< @ref EEPROM_RACETIME_VERSION
  uint8_t reserved[3]; ///< Padding, written as 0
  uint32_t crc;        ///< CRC32 of the payload that follows
  uint8_t trailing[4]; ///< Reserved for future use, written as 0
} __attribute__((packed));

static_assert(sizeof(raceTimeBlockHeader) == EEPROM_RACETIME_HEADER_SIZE,
              "Header size must match the block layout");

static uint32_t payloadCrc(void)
{
  const byte *pStart = (const byte *)&raceTimeData;
  return crc32Payload(pStart, pStart + sizeof(raceTimeData));
}

/**
 * @brief Work out whether the storage backend can hold the block at all.
 *
 * Done against the live API rather than a compile-time constant because the Teensy falls back
 * to its 4,284-byte internal EEPROM when the SPI flash is absent or unformatted. On that
 * backend every address above ~0x10BB is unreadable, and `read()`/`write()` clamp silently -
 * so an overrun would return plausible-looking dead data instead of failing.
 */
static bool probeStorage(void)
{
  const uint16_t length = getStorageAPI().length();
  return (uint32_t)length >= ((uint32_t)EEPROM_RACETIME_BASE + (uint32_t)RACETIME_BLOCK_SIZE);
}

/** @brief Reset the clock and state machine, keeping the configuration untouched. */
static void resetClock(void)
{
  raceTimeMs = 0U;
  raceTimeState = RT_STATE_OFF;
  raceTimeFalseStart = false;
  raceTimeStopThreshold = 0U;
}

/**
 * @brief Clamp every stored setting into range.
 *
 * A stored block can hold out-of-range values if it was written by an older build or
 * hand-edited, and a nonsense value here reaches an ignition retard directly.
 */
static void clampSettings(void)
{
  raceTimeConfig &s = raceTimeData;

  if (s.enable > (uint8_t)RT_ENABLED) { s.enable = (uint8_t)RT_DISABLED; }
  if (s.armPolarity > 1U) { s.armPolarity = 0U; }
  if (s.armPullup > 1U) { s.armPullup = 1U; }
  if (s.startSource > (uint8_t)RT_START_EXTERNAL) { s.startSource = (uint8_t)RT_START_LAUNCH_PIN_RELEASE; }
  if (s.stopSource > (uint8_t)RT_STOP_SPEED) { s.stopSource = (uint8_t)RT_STOP_NONE; }
  if (s.motionSource > (uint8_t)RT_MOTION_ARM_PIN) { s.motionSource = (uint8_t)RT_MOTION_NONE; }

  // The curve axis must be strictly ascending. A hand-edited or truncated block can easily
  // hold a non-monotonic axis, and the lookup assumes ordering - so repair it here rather than
  // producing a curve that silently reads the wrong breakpoint.
  s.curveTimeMs[0] = 0U;
  for (uint8_t i = 1; i < RACETIME_CURVE_POINTS; ++i)
  {
    if (s.curveTimeMs[i] <= s.curveTimeMs[i - 1U])
    {
      s.curveTimeMs[i] = (s.curveTimeMs[i - 1U] < 0xFFFFU) ? (uint16_t)(s.curveTimeMs[i - 1U] + 1U)
                                                         : s.curveTimeMs[i - 1U];
    }
  }

  // Retard beyond 60 degrees cannot be honoured by the scheduler and is not a real tune.
  for (uint8_t i = 0; i < RACETIME_CURVE_POINTS; ++i)
  {
    if (s.curveRetard[i] > 60U) { s.curveRetard[i] = 60U; }
  }

  // The label must be NUL terminated within its own field, or TunerStudio and every string
  // consumer reads off the end of the struct.
  s.curveLabel[RACETIME_LABEL_LEN - 1U] = 0U;
}

// =====================================================================================
// Storage
// =====================================================================================

bool raceTimeIsAvailable(void) { return raceTimeStorageOk; }
bool raceTimeIsInitialised(void) { return raceTimeReady; }
bool raceTimeIsDirty(void) { return raceTimeDirty; }
bool raceTimeIsFalseStart(void) { return raceTimeFalseStart; }
void raceTimeMarkDirty(void) { raceTimeDirty = true; }

const raceTimeBlock &raceTimeGetPage(void) { return raceTimeData; }
raceTimeBlock &raceTimeGetPageMutable(void) { return raceTimeData; }

void raceTimeRestoreDefaults(void)
{
  // RAM only. setTuneToEmpty() zeroes every page, which for this block would leave enable=0 -
  // correct here, unlike the trim block, because "race time off" is a sane fresh-board state.
  // Persistence is the caller's normal save path, so this deliberately does not touch storage.
  setPageToDefaults();
  clampSettings();
  resetClock();
  raceTimeReady = true;
}

/** @brief Does the payload in RAM differ from what is already stored? */
static bool payloadDiffersFromStorage(void)
{
  if (!raceTimeStorageOk)
  {
    return false;
  }

  const storage_api_t &api = getStorageAPI();
  const byte *p = (const byte *)&raceTimeData;
  const byte *pEnd = p + sizeof(raceTimeData);

  for (uint16_t address = EEPROM_RACETIME_PAYLOAD; p != pEnd; ++p, ++address)
  {
    if (api.read(address) != *p)
    {
      return true;
    }
  }
  return false;
}

bool raceTimeNeedsSave(void)
{
  if (!raceTimeStorageOk)
  {
    return false;
  }
  return raceTimeDirty || payloadDiffersFromStorage();
}

/** @brief Write the block header. Always written, so it always matches the payload. */
static void writeHeader(void)
{
  raceTimeBlockHeader header;
  header.magic = EEPROM_RACETIME_MAGIC;
  header.version = EEPROM_RACETIME_VERSION;
  header.reserved[0] = 0U;
  header.reserved[1] = 0U;
  header.reserved[2] = 0U;
  for (uint8_t i = 0; i < 4U; ++i) { header.trailing[i] = 0U; }
  header.crc = payloadCrc();
  updateObject(getStorageAPI(), header, EEPROM_RACETIME_BASE);
}

/**
 * @brief The one place the payload is written.
 *
 * Payload first, header last. That ordering is the whole crash-safety design: if power is lost
 * mid-save the stored CRC will not match the stored payload, and raceTimeInit then treats the
 * block as corrupt and discards it. Losing a set of defaults is recoverable; applying half a
 * curve to the ignition timing is not.
 *
 * BUDGET SEMANTICS - this matches every other page:
 *
 *   @p writesRemaining is a budget of *byte writes*. `updateBlockLimitWriteOps()` returns what
 *   is left of it, and consumes one per byte that actually needed changing. So:
 *     - nonzero return  => the payload is fully stored
 *     - zero return     => the budget ran out, and the caller must ask again
 *
 * @return Writes remaining. Zero means "budget exhausted, come back for more".
 */
static uint16_t writeBlock(uint16_t writesRemaining)
{
  const uint16_t remaining = updateBlockLimitWriteOps(getStorageAPI(),
                                                     EEPROM_RACETIME_PAYLOAD,
                                                     (const byte *)&raceTimeData,
                                                     ((const byte *)&raceTimeData) + sizeof(raceTimeData),
                                                     writesRemaining);
  if (remaining == 0U)
  {
    // Budget exhausted, so the payload may be incomplete. Do NOT touch the header: leaving the
    // old CRC in place means a torn save is detected at the next boot rather than trusted.
    return 0U;
  }

  // The payload is fully stored, so the header can now be made to describe it.
  writeHeader();
  raceTimeDirty = false;

  // Preserve the caller's remaining budget so the rest of the pages still get to use it.
  return remaining;
}

bool raceTimeSave(void)
{
  if (!raceTimeStorageOk)
  {
    return false;
  }

  // A budget large enough that it can never be exhausted. Using RACETIME_PAYLOAD_SIZE here
  // would be wrong: when every byte differs the budget is consumed exactly, which looks like
  // "exhausted" and would skip the header write, leaving the block permanently dirty.
  (void)writeBlock(UINT16_MAX);
  return !raceTimeDirty;
}

uint16_t raceTimeSaveBlock(uint16_t writesRemaining)
{
  if (!raceTimeStorageOk)
  {
    // Report the budget untouched rather than 0. Returning 0 would say "come back with more
    // budget", which the burn loop would obey forever - the flash-destroying bug this contract
    // exists to prevent.
    return writesRemaining;
  }

  if (!raceTimeNeedsSave())
  {
    raceTimeDirty = false;
    return writesRemaining;
  }

  return writeBlock(writesRemaining);
}

/**
 * @brief Bind the configured arm pin as an input. Safe to call repeatedly.
 *
 * Pin 0 means "not configured" and is not an error: the framework still runs, driven by
 * @ref raceTimeTriggerStart, which is what makes it unit-testable without real hardware.
 */
static void bindArmPin(void)
{
  const uint8_t configured = raceTimeData.armPin;

  if (configured == 0U)
  {
    raceTimeArmPin = 0U;
    raceTimePinBound = false;
    return;
  }

  // pinTranslate() maps the TunerStudio pin number to a board pin, exactly as it does for the
  // launch, VSS and fan inputs. A pin that cannot be translated (reserved for the SD card, or
  // an analog-only pin) resolves to 0 and leaves the input unbound rather than stealing it.
  const uint8_t pin = pinTranslate(configured);
  if ((pin == 0U) || pinIsReserved(pin))
  {
    raceTimeArmPin = 0U;
    raceTimePinBound = false;
    return;
  }

  raceTimeArmPin = pin;
  if (raceTimeData.armPullup != 0U)
  {
    pinMode(pin, INPUT_PULLUP);
  }
  else
  {
    pinMode(pin, INPUT);
  }
  raceTimePinBound = true;
}

/** @brief Read the arm input, applying the configured polarity. False when unbound. */
static bool readArmPin(void)
{
  if (!raceTimePinBound)
  {
    return false;
  }
  const bool high = (digitalRead(raceTimeArmPin) == HIGH);
  return (raceTimeData.armPolarity == 0U) ? high : !high;
}

// =====================================================================================
// Boot
// =====================================================================================

/**
 * @brief Read the block at boot.
 *
 * On a blank or corrupt block the payload is left at its defaults rather than being
 * half-loaded: applying garbage ignition retard is far worse than applying none. A magic
 * mismatch is "never initialised"; a CRC mismatch on a matching magic is "corrupt". Both end
 * up at defaults, but only the first one also rewrites the header.
 */
void initialiseRaceTime(statuses &current)
{
  (void)current;

  raceTimeStorageOk = probeStorage();
  raceTimeDirty = false;
  resetClock();

  if (!raceTimeStorageOk)
  {
    // Cannot persist anything. Run with the feature disabled so nothing is applied.
    setPageToDefaults();
    clampSettings();
    raceTimeReady = true;
    bindArmPin();
    return;
  }

  const storage_api_t &api = getStorageAPI();

  raceTimeBlockHeader header;
  (void)loadObject(api, EEPROM_RACETIME_BASE, header);

  const bool magicOk = (header.magic == EEPROM_RACETIME_MAGIC)
                    && (header.version == EEPROM_RACETIME_VERSION);

  if (!magicOk)
  {
    // Never initialised (or written by an incompatible version). Start clean and lay down a
    // valid header so the next boot takes the fast path.
    setPageToDefaults();
    clampSettings();
    raceTimeReady = true;
    (void)raceTimeSave();
    bindArmPin();
    return;
  }

  (void)loadObject(api, EEPROM_RACETIME_PAYLOAD, raceTimeData);

  if (header.crc != payloadCrc())
  {
    // Corrupt payload. Discard it entirely - never apply a partial curve.
    setPageToDefaults();
    clampSettings();
    raceTimeReady = true;
    (void)raceTimeSave();
    bindArmPin();
    return;
  }

  clampSettings();
  raceTimeReady = true;
  bindArmPin();
}

// =====================================================================================
// Runtime
// =====================================================================================

uint32_t raceTimeGetMs(void) { return raceTimeMs; }
RaceTimeState raceTimeGetState(void) { return raceTimeState; }

bool raceTimeIsRunning(void)
{
  return raceTimeState == RT_STATE_RUNNING;
}

void raceTimeReset(void)
{
  resetClock();
}

void raceTimeTriggerStart(void)
{
  if (raceTimeData.enable == (uint8_t)RT_DISABLED)
  {
    return;
  }
  // An explicit trigger overrides "armed": the caller has decided the run has begun. This is
  // what a bump box or a CAN message will use, and what the external start source means.
  raceTimeMs = 0U;
  raceTimeFalseStart = false;
  raceTimeState = RT_STATE_RUNNING;
  raceTimeStopThreshold = raceTimeData.stopTimeMs;
}

/** @brief Is the configured motion source reporting movement? */
static bool motionDetected(const statuses &current)
{
  const uint8_t source = raceTimeData.motionSource;

  if (source == (uint8_t)RT_MOTION_VSS)
  {
    // A threshold of 0 with a source selected means "any non-zero speed counts". That is the
    // useful default: a staged car is stationary, so the smallest reading is already a fault.
    const uint16_t threshold = raceTimeData.falseStartSpeed;
    return current.vss > threshold;
  }

  if (source == (uint8_t)RT_MOTION_ARM_PIN)
  {
    // "Movement" is the trans-brake being released while the run has not started - i.e. the
    // driver let go early and the arm input dropped without a valid start condition.
    return !raceTimePrevPinHigh;
  }

  return false;
}

void raceTimeTick1ms(statuses &current, bool pinHigh)
{
  if (raceTimeData.enable == (uint8_t)RT_DISABLED)
  {
    // Turning the feature off must also clear the clock. Leaving a stale elapsed time behind
    // would let a consumer act on a run that no longer exists.
    if (raceTimeState != RT_STATE_OFF)
    {
      resetClock();
    }
    raceTimePrevPinHigh = pinHigh;
    return;
  }

  raceTimePrevPinHigh = pinHigh;

  switch (raceTimeState)
  {
    case RT_STATE_OFF:
    case RT_STATE_STOPPED:
    case RT_STATE_FALSE_START:
      // Arming is the only transition out of these. A stopped or false-started run is only
      // re-entered by the driver releasing the arm input and pressing it again, which is
      // exactly how a real second attempt happens.
      if (pinHigh)
      {
        raceTimeMs = 0U;
        raceTimeFalseStart = false;
        raceTimeState = RT_STATE_ARMED;
        raceTimeStopThreshold = raceTimeData.stopTimeMs;
      }
      break;

    case RT_STATE_ARMED:
      if (!pinHigh)
      {
        // The arm input dropped. Either this is the start release, or the driver let go
        // before the car was staged - which is a false start if we can see movement.
        if (motionDetected(current))
        {
          raceTimeFalseStart = true;
          raceTimeMs = 0U;
          raceTimeState = RT_STATE_FALSE_START;
        }
        else
        {
          raceTimeMs = 0U;
          raceTimeState = RT_STATE_RUNNING;
          raceTimeStopThreshold = raceTimeData.stopTimeMs;
        }
      }
      else if (motionDetected(current))
      {
        // Still armed, but the car is moving. Flag it without starting the clock.
        raceTimeFalseStart = true;
        raceTimeMs = 0U;
        raceTimeState = RT_STATE_FALSE_START;
      }
      else
      {
        // Waiting. Nothing to do.
      }
      break;

    case RT_STATE_RUNNING:
      // Advance first, then test the stop conditions, so a stop at exactly the threshold
      // records the threshold rather than one millisecond short of it.
      ++raceTimeMs;

      switch ((RaceTimeStopSource)raceTimeData.stopSource)
      {
        case RT_STOP_TIME:
          if (raceTimeMs >= raceTimeStopThreshold) { raceTimeState = RT_STATE_STOPPED; }
          break;

        case RT_STOP_ARM_PIN:
          if (pinHigh) { raceTimeState = RT_STATE_STOPPED; }
          break;

        case RT_STOP_SPEED:
          if (current.vss >= raceTimeData.stopSpeed) { raceTimeState = RT_STATE_STOPPED; }
          break;

        case RT_STOP_NONE:
        default:
          // Runs until an explicit reset. A drag pass is timed by the driver, not the ECU.
          break;
      }
      break;

    default:
      raceTimeState = RT_STATE_OFF; // MISRA
      break;
  }

  // 'motionDetected' is consulted only while armed, so a running pass is never invalidated by
  // the car legitimately moving.
}

#if defined(UNIT_TEST)
void raceTimeResetForTest(void)
{
  // Order matters: the clock first, then the configuration, because resetClock() also clears
  // the flags that clampSettings() reads.
  resetClock();
  raceTimePrevPinHigh = false;
  setPageToDefaults();
  clampSettings();
  raceTimeStorageOk = false;
  raceTimeDirty = false;
  raceTimeReady = false;
  raceTimePinBound = false;
  raceTimeArmPin = 0U;
}
#endif

void raceTimeUpdate(statuses &current)
{
  raceTimeTick1ms(current, readArmPin());
}

uint8_t raceTimeCurveRetard(void)
{
  if (raceTimeData.enable == (uint8_t)RT_DISABLED) { return 0U; }
  if (raceTimeData.curveEnable == 0U) { return 0U; }
  if (raceTimeState != RT_STATE_RUNNING) { return 0U; }

  const uint16_t *times = raceTimeData.curveTimeMs;
  const uint8_t *points = raceTimeData.curveRetard;

  // Before the first breakpoint (normally t=0) hold the first value. clampSettings() forces
  // times[0] to 0, so this only fires for a block that has never been loaded.
  if (raceTimeMs <= (uint32_t)times[0]) { return points[0]; }

  // Past the last breakpoint hold the last value rather than extrapolating. Extrapolation off
  // the end of a retard curve would keep pulling timing forever, which is not what a tuner
  // draws, and at 60 degrees it would make the engine undriveable rather than merely slow.
  if (raceTimeMs >= (uint32_t)times[RACETIME_CURVE_POINTS - 1U])
  {
    return points[RACETIME_CURVE_POINTS - 1U];
  }

  // Find the bracketing pair. A linear scan is correct and cheap here: 16 points, and this
  // runs once per main-loop pass at most. A binary search would be more code for less clarity,
  // and the axis is guaranteed ascending by clampSettings().
  uint8_t index = 0U;
  for (uint8_t i = 1U; i < RACETIME_CURVE_POINTS; ++i)
  {
    if ((uint32_t)times[i] > raceTimeMs) { break; }
    index = i;
  }

  const uint8_t next = (uint8_t)(index + 1U);
  if (next >= RACETIME_CURVE_POINTS) { return points[index]; }

  const uint32_t t0 = (uint32_t)times[index];
  const uint32_t t1 = (uint32_t)times[next];
  if (t1 <= t0) { return points[index]; } // guarded by clampSettings(), but never divide by 0

  // Linear interpolation, rounded to nearest whole degree. The delta is signed: a descending
  // curve (retard off the line, then give the timing back) is the common real shape, and an
  // unsigned subtraction there would wrap to a huge positive number.
  const int32_t a = (int32_t)points[index];
  const int32_t b = (int32_t)points[next];
  const int32_t delta = b - a;
  const uint32_t frac = raceTimeMs - t0;
  const uint32_t full = t1 - t0;

  const int64_t scaled = (int64_t)delta * (int64_t)frac;
  const int64_t half = (int64_t)(full >> 1U);
  const int64_t rounded = (scaled >= 0) ? ((scaled + half) / (int64_t)full)
                                        : (-(((-scaled) + half) / (int64_t)full));

  const int32_t result = a + (int32_t)rounded;
  if (result <= 0) { return 0U; }
  if (result >= 255) { return 255U; }
  return (uint8_t)result;
}
