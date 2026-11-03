#include "boostLaunch.h"
#include "ppLaunch.h"
#include "statuses.h"
#include "storage.h"
#include "globals.h"
#include "maths.h"
#include "unit_testing.h"
#include <string.h>

/**
 * @file
 * @brief Launch boost target: the base-target curve and its persistence.
 *
 * See boostLaunch.h for the layering contract (this supplies a BASE target; flex, traction and
 * any future modifier stack on top of it) and for the storage layout. The properties that are
 * easy to break by accident, restated:
 *
 *  1. Outside the launch window this module contributes NOTHING. There is no "hold last value":
 *     a car that has finished launching belongs on its normal boost strategy.
 *  2. A curve value of 0 means "no override", never "command zero boost".
 *  3. The CRC header is written last, so an interrupted save is detected rather than trusted.
 */

// =====================================================================================
// State
// =====================================================================================

/** @brief The live page payload. This is what TunerStudio reads and writes. */
boostLaunchBlock boostLaunchData;

namespace {

struct Runtime {
  uint16_t baseTarget = 0;  ///< The base target to supply, kPa. 0 = contribute nothing.
  uint8_t dutyTrim = 0;     ///< Duty trim to apply, percent.
  bool active = false;      ///< True while this module should supply the base target.
  bool storageOk = false;   ///< The backend can hold the block.
  bool initialised = false; ///< The block was found intact at boot.
  bool dirty = false;       ///< Settings changed since the last persist.
};

Runtime rt;

// =====================================================================================
// Defaults and clamps
// =====================================================================================

/**
 * @brief Reset the payload to a safe, feature-disabled state.
 *
 * These values MUST match the `defaultValue =` entries for page 20 in reference/speeduino.ini.
 * A disagreement is not caught by any test, and shows up as a different curve depending on
 * whether the tuner pressed Burn or the ECU laid the block down itself.
 */
void setPageToDefaults(void)
{
  memset(&boostLaunchData, 0, sizeof(boostLaunchData));

  // Ships DISABLED. A feature that starts substituting the boost target on a car that never asked
  // for it is the worst possible default, and boost is the one thing here that can put a rod
  // through the block.
  boostLaunchData.enable = (uint8_t)BL_DISABLED;
  boostLaunchData.rampInMs = 0U;
  boostLaunchData.rampOutMs = 0U;

  const char *label = "launch boost";
  const size_t len = strlen(label);
  for (size_t i = 0U; (i < len) && (i < (size_t)(sizeof(boostLaunchData.label) - 1U)); ++i)
  {
    boostLaunchData.label[i] = (uint8_t)label[i];
  }

  // 32 points at 100 ms intervals: 0 .. 3100 ms, matching the launch pages so a tuner reads the
  // same time axis on every tab.
  // A "leave soft, then build" shape: a turbo car cannot use full boost at the hit, so the target
  // climbs from a wastegate-ish 180 kPa to 270 kPa over the first second and then holds.
  static const uint16_t kTime[BL_CURVE_POINTS] = {
      0U,  100U,  200U,  300U,  400U,  500U,  600U,  700U,
    800U,  900U, 1000U, 1100U, 1200U, 1300U, 1400U, 1500U,
   1600U, 1700U, 1800U, 1900U, 2000U, 2100U, 2200U, 2300U,
   2400U, 2500U, 2600U, 2700U, 2800U, 2900U, 3000U, 3100U };

  static const uint16_t kTarget[BL_CURVE_POINTS] = {
    180U, 180U, 180U, 180U, 185U, 190U, 195U, 200U,
    205U, 210U, 215U, 220U, 225U, 230U, 235U, 240U,
    245U, 250U, 255U, 260U, 265U, 270U, 270U, 270U,
    270U, 270U, 270U, 270U, 270U, 270U, 270U, 270U };

  for (uint8_t i = 0U; i < BL_CURVE_POINTS; ++i)
  {
    boostLaunchData.curveTime[i] = kTime[i];
    boostLaunchData.curveTarget[i] = kTarget[i];
    boostLaunchData.curveDutyTrim[i] = 0U;   // trim off by default: the target does the work
  }
}

/** @brief Clamp every stored setting into range and repair the time axis. */
void clampSettings(void)
{
  boostLaunchBlock &s = boostLaunchData;

  if (s.enable > (uint8_t)BL_ENABLED) { s.enable = (uint8_t)BL_DISABLED; }

  // The time axis must start at zero and be strictly ascending: it is the lookup key.
  s.curveTime[0] = 0U;
  for (uint8_t i = 1U; i < BL_CURVE_POINTS; ++i)
  {
    if (s.curveTime[i] <= s.curveTime[i - 1U])
    {
      s.curveTime[i] = (s.curveTime[i - 1U] < 0xFFFFU)
                     ? (uint16_t)(s.curveTime[i - 1U] + 1U)
                     : s.curveTime[i - 1U];
    }
  }

  for (uint8_t i = 0U; i < BL_CURVE_POINTS; ++i)
  {
    // 511 kPa is the ceiling getBoostTarget() can represent. 0 is legal and means "no override".
    if (s.curveTarget[i] > BL_TARGET_MAX_KPA) { s.curveTarget[i] = BL_TARGET_MAX_KPA; }
    if (s.curveDutyTrim[i] > 100U) { s.curveDutyTrim[i] = 100U; }
  }

  // The label must be NUL terminated within its own field, or every string consumer reads off
  // the end of the struct.
  s.label[sizeof(s.label) - 1U] = 0U;
}

// =====================================================================================
// Storage
// =====================================================================================

/** @brief CRC32 (IEEE, reflected) over the payload. Identical to the ltt / raceTime / ppLaunch
 *  implementations - one algorithm, four blocks, no chance of them disagreeing. */
uint32_t crc32Payload(const byte *pStart, const byte *pEnd)
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

struct boostLaunchBlockHeader
{
  uint32_t magic;
  uint8_t version;
  uint8_t reserved[3];
  uint32_t crc;
  uint8_t trailing[4];
} __attribute__((packed));

static_assert(sizeof(boostLaunchBlockHeader) == EEPROM_BOOSTLAUNCH_HEADER_SIZE,
              "Header size must match the block layout");

uint32_t payloadCrc(void)
{
  const byte *pStart = (const byte *)&boostLaunchData;
  return crc32Payload(pStart, pStart + sizeof(boostLaunchData));
}

bool probeStorage(void)
{
  const uint16_t length = getStorageAPI().length();
  return (uint32_t)length >= ((uint32_t)EEPROM_BOOSTLAUNCH_BASE + (uint32_t)BOOSTLAUNCH_BLOCK_SIZE);
}

bool payloadDiffers(void)
{
  boostLaunchBlock stored;
  (void)loadObject(getStorageAPI(), EEPROM_BOOSTLAUNCH_PAYLOAD, stored);
  return memcmp(&stored, &boostLaunchData, sizeof(stored)) != 0;
}

void writeHeader(void)
{
  boostLaunchBlockHeader header;
  header.magic = EEPROM_BOOSTLAUNCH_MAGIC;
  header.version = EEPROM_BOOSTLAUNCH_VERSION;
  header.reserved[0] = 0U;
  header.reserved[1] = 0U;
  header.reserved[2] = 0U;
  for (uint8_t i = 0U; i < 4U; ++i) { header.trailing[i] = 0U; }
  header.crc = payloadCrc();
  updateObject(getStorageAPI(), header, EEPROM_BOOSTLAUNCH_BASE);
}

/**
 * @brief The one place the payload is written. Payload first, header last.
 *
 * BUDGET SEMANTICS - identical to every other page: a nonzero return means the payload is fully
 * stored; zero means the budget ran out and the caller must ask again.
 */
uint16_t writeBlock(uint16_t writesRemaining)
{
  const uint16_t remaining = updateBlockLimitWriteOps(getStorageAPI(),
                                                     EEPROM_BOOSTLAUNCH_PAYLOAD,
                                                     (const byte *)&boostLaunchData,
                                                     ((const byte *)&boostLaunchData) + sizeof(boostLaunchData),
                                                     writesRemaining);
  if (remaining == 0U)
  {
    // Budget exhausted. Do NOT touch the header: leaving the old CRC in place means a torn save
    // is detected at the next boot rather than trusted.
    return 0U;
  }

  writeHeader();
  rt.dirty = false;
  return remaining;
}

} // anonymous namespace

bool boostLaunchSave(void)
{
  if (!rt.storageOk) { return false; }

  // A budget large enough that it can never be exhausted. Using the payload size here would be
  // wrong: when every byte differs the budget is consumed exactly, which looks like "exhausted"
  // and would skip the header write, leaving the block permanently dirty.
  (void)writeBlock(UINT16_MAX);
  return !rt.dirty;
}

bool boostLaunchNeedsSave(void)
{
  if (!rt.storageOk) { return false; }
  return rt.dirty || payloadDiffers();
}

uint16_t boostLaunchSaveBlock(uint16_t writesRemaining)
{
  if (!rt.storageOk)
  {
    // Report the budget untouched rather than 0. Returning 0 would say "come back with more
    // budget", which the burn loop would obey forever - the flash-destroying bug this contract
    // exists to prevent.
    return writesRemaining;
  }

  if (!boostLaunchNeedsSave())
  {
    rt.dirty = false;
    return writesRemaining;
  }

  return writeBlock(writesRemaining);
}

void boostLaunchMarkDirty(void)
{
  rt.dirty = true;
}

// =====================================================================================
// Curve evaluation
// =====================================================================================

namespace {

/** @brief Linear interpolation with signed delta and round-to-nearest.
 *
 * Signed because a descending target curve is legitimate (pulling boost back late in a pass) and
 * an unsigned subtraction would wrap to a huge value that then clamps to the maximum.
 */
template <typename TValue>
uint16_t interpolate(const uint16_t *axis, const TValue *values, uint8_t count, uint32_t x)
{
  if (count == 0U) { return 0U; }
  if (x <= (uint32_t)axis[0U]) { return values[0U]; }
  if (x >= (uint32_t)axis[count - 1U]) { return values[count - 1U]; }

  uint8_t index = 0U;
  for (uint8_t i = 1U; i < count; ++i)
  {
    if ((uint32_t)axis[i] > x) { break; }
    index = i;
  }

  const uint8_t next = (uint8_t)(index + 1U);
  if (next >= count) { return values[index]; }

  const uint32_t a0 = (uint32_t)axis[index];
  const uint32_t a1 = (uint32_t)axis[next];
  if (a1 <= a0) { return values[index]; } // guarded by clampSettings(), but never divide by zero

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

} // anonymous namespace

void boostLaunchUpdate(const statuses &current)
{
  // Contribute nothing unless BOTH the feature is on and the launch is actually running. Every
  // early exit clears the outputs rather than leaving a stale value, so a finished pass puts the
  // boost controller straight back on its normal strategy with no "hold the last value" state.
  if ((boostLaunchData.enable != (uint8_t)BL_ENABLED) || (ppLaunchState() != PP_STATE_LAUNCHING))
  {
    rt.baseTarget = 0U;
    rt.dutyTrim = 0U;
    rt.active = false;
    return;
  }

  // The launch clock is the shared one. Two features each counting seconds from "the launch"
  // would disagree about when it was, and a boost target that is a second out is a target for
  // the wrong part of the pass.
  const uint32_t ms = ppLaunchElapsedMs();

  const uint16_t target = interpolate(boostLaunchData.curveTime, boostLaunchData.curveTarget,
                                      BL_CURVE_POINTS, ms);

  // A target of 0 means "no override at this point", not "zero boost". Commanding zero boost
  // because a tuner left a row blank would be a trap that stalls the car.
  rt.baseTarget = target;
  rt.dutyTrim = (uint8_t)interpolate(boostLaunchData.curveTime, boostLaunchData.curveDutyTrim,
                                     BL_CURVE_POINTS, ms);

  // Active means "supply the base target". A target of 0 with a non-zero trim is still active -
  // the trim is applied to whatever target wins, and a car trimming duty without moving the
  // target is a normal way to shape the hit.
  rt.active = (rt.baseTarget != 0U) || (rt.dutyTrim != 0U);

  (void)current;
}

// =====================================================================================
// Boot
// =====================================================================================

void initialiseBoostLaunch(void)
{
  rt.storageOk = probeStorage();
  rt.dirty = false;
  rt.baseTarget = 0U;
  rt.dutyTrim = 0U;
  rt.active = false;
  rt.initialised = false;

  if (!rt.storageOk)
  {
    // Cannot persist anything. Lay down safe defaults so nothing is applied from a block we
    // could not have read.
    setPageToDefaults();
    clampSettings();
    return;
  }

  boostLaunchBlockHeader header;
  (void)loadObject(getStorageAPI(), EEPROM_BOOSTLAUNCH_BASE, header);

  const bool magicOk = (header.magic == EEPROM_BOOSTLAUNCH_MAGIC)
                    && (header.version == EEPROM_BOOSTLAUNCH_VERSION);

  if (!magicOk)
  {
    // Never initialised by this build, or written by an incompatible version.
    setPageToDefaults();
    clampSettings();
    rt.dirty = true;
    (void)boostLaunchSave();
    return;
  }

  (void)loadObject(getStorageAPI(), EEPROM_BOOSTLAUNCH_PAYLOAD, boostLaunchData);

  // Repair FIRST, then verify: the repair is what makes a slightly damaged block usable, but it
  // also changes bytes, so the CRC has to be taken against the repaired payload. Discarding a
  // tuner's whole boost curve because the time axis needed one breakpoint nudged would be a far
  // worse outcome than the repair.
  clampSettings();

  if (header.crc != payloadCrc())
  {
    rt.dirty = true;
    (void)boostLaunchSave();
    return;
  }

  rt.initialised = true;
}

void boostLaunchRestoreDefaults(void)
{
  setPageToDefaults();
  clampSettings();
  rt.dirty = true;
}

// =====================================================================================
// Accessors
// =====================================================================================

uint16_t boostLaunchBaseTarget(void) { return rt.baseTarget; }
uint8_t boostLaunchDutyTrim(void) { return rt.dutyTrim; }
bool boostLaunchIsActive(void) { return rt.active; }
bool boostLaunchIsInitialised(void) { return rt.initialised; }
bool boostLaunchIsAvailable(void) { return rt.storageOk; }
uint16_t boostLaunchTargetLog(void) { return rt.baseTarget; }

boostLaunchBlock &boostLaunchGetPageMutable(void) { return boostLaunchData; }
const boostLaunchBlock &boostLaunchGetPage(void) { return boostLaunchData; }

#if defined(UNIT_TEST)
void boostLaunchResetForTest(void)
{
  rt.baseTarget = 0U;
  rt.dutyTrim = 0U;
  rt.active = false;
  rt.storageOk = false;
  rt.initialised = false;
  rt.dirty = false;
  setPageToDefaults();
  clampSettings();
}
#endif
