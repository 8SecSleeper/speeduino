#include "ltt.h"
#include "storage.h"
#include "maths.h"
#include "globals.h"
#include "unit_testing.h"
#include "secondaryTables.h"
#include "units.h"
#include <string.h>

/**
 * @file
 * @brief Long-term fuel trims: storage, lookup, application, learning and persistence.
 *
 * See ltt.h for the design contract. The important properties, restated because they are
 * easy to break by accident:
 *
 *  1. A trim of 0 is bit-exact - lttApplyTrim() returns its input unchanged. A freshly
 *     formatted ECU therefore fuels identically to one without this feature compiled in.
 *  2. The trim is never inside the loop that produces it. What is learned is what EGO had
 *     to add on top of the *already trimmed* fuel, so the loop converges instead of diverging.
 *  3. Nothing here reads or writes storage unless the backend can actually hold the block.
 *     The internal-EEPROM fallback is 4,284 bytes and cannot reach 0x1000 at all.
 */

// =====================================================================================
// State
// =====================================================================================

/** @brief The live page payload. This is what TunerStudio reads and writes. */
lttBlock lttData;

/** @brief True once the block has been loaded or defaulted at boot. */
static bool lttReady = false;

/** @brief True if the storage backend is big enough to hold the whole block. */
static bool lttStorageOk = false;

/** @brief Set when a learn step changes a trim. Cleared on a successful save. */
static bool lttDirty = false;

/**
 * @brief Per-cell count of consecutive clean learn samples.
 *
 * A cell is not allowed to move until it has been seen @ref lttConfig::minSamples times in
 * a row without a gate firing. This is what stops a transient that slipped through the
 * gates from writing a large trim into a cell that has only ever been visited once.
 */
static uint8_t lttSampleCount[LTT_TABLE_COUNT][LTT_CELL_COUNT];

/**
 * @brief Slewed trim actually applied to fuelling, in 0.1% units.
 *
 * The trim in @ref lttData is the target. This is what reaches the pulse width, and it moves
 * toward the target at no more than @ref lttConfig::slewUnitsPerSec, so a trim change can
 * never appear as an instantaneous pulse-width step at the injector.
 */
static int16_t lttSlewedUnits[LTT_TABLE_COUNT][LTT_CELL_COUNT];

// =====================================================================================
// Small helpers
// =====================================================================================

static inline uint16_t clampCellIndex(uint16_t index)
{
  return (index < LTT_CELL_COUNT) ? index : (LTT_CELL_COUNT - 1U);
}

/** @brief Reset the payload to a sane, non-trimming, default-authority state. */
static void setPageToDefaults(void)
{
  memset(&lttData, 0, sizeof(lttData));

  lttData.settings.mode = LTT_MODE_OFF;
  lttData.settings.learnRateDiv = 3U;                     // 1/8 per update
  lttData.settings.deadbandUnits = (uint8_t)LTT_DEADBAND_DEFAULT_UNITS;
  lttData.settings.minRPMDiv100 = 10U;                    // 1000 RPM
  lttData.settings.saveRPMDiv100 = 2U;                    // 200 RPM
  lttData.settings.minSamples = 5U;
  lttData.settings.maxStepUnits = 50U;                    // 5.0% per update, worst case
  lttData.settings.slewUnitsPerSec = 100U;                // 10.0% per second

  // A trim of 0 is "no trim", and 0 is also the natural default of the array, so the
  // trims need no explicit initialisation beyond the memset above.

  for (uint8_t table = 0; table < LTT_TABLE_COUNT; ++table)
  {
    for (uint16_t cell = 0; cell < LTT_CELL_COUNT; ++cell)
    {
      lttData.leanAuth[table][cell] = LTT_AUTHORITY_DEFAULT_PCT;
      lttData.richAuth[table][cell] = LTT_AUTHORITY_DEFAULT_PCT;
    }
  }

  memset(lttSampleCount, 0, sizeof(lttSampleCount));
  memset(lttSlewedUnits, 0, sizeof(lttSlewedUnits));
}

/**
 * @brief CRC32 (IEEE, reflected, as used by the tuning protocol) over the payload.
 *
 * Implemented here rather than reusing FastCRC so that the stored value is computed by the
 * same function that validates it, with no dependency on how a third-party library happens
 * to be compiled.
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

/** @brief The header as it is laid out in storage.
 *
 * Fixed 16 bytes: magic (4), version (1), reserved (3), CRC (4), then 4 further reserved
 * bytes kept explicit so the struct size is obvious by inspection rather than by arithmetic.
 */
struct lttBlockHeader
{
  uint32_t magic;      ///< @ref EEPROM_LTT_MAGIC
  uint8_t version;     ///< @ref EEPROM_LTT_VERSION
  uint8_t reserved[3]; ///< Padding, written as 0
  uint32_t crc;        ///< CRC32 of the payload that follows
  uint8_t trailing[4]; ///< Reserved for future use, written as 0
} __attribute__((packed));

static_assert(sizeof(lttBlockHeader) == EEPROM_LTT_HEADER_SIZE, "Header size must match the block layout");

static uint32_t payloadCrc(void)
{
  const byte *pStart = (const byte *)&lttData;
  return crc32Payload(pStart, pStart + sizeof(lttData));
}

void lttRestoreDefaults(void)
{
  // RAM only. setTuneToEmpty() zeroes every page, which for this block would leave zero
  // authority and silently disable closed loop on a fresh board. Persistence is the caller's
  // normal save path, so this deliberately does not touch storage.
  setPageToDefaults();
  lttReady = true;
}

// =====================================================================================
// Storage
// =====================================================================================

/**
 * @brief Work out whether the storage backend can hold the block at all.
 *
 * The check is done against the live API rather than a compile-time constant because the
 * Teensy falls back to its 4,284-byte internal EEPROM when the SPI flash is absent or
 * unformatted. On that backend every address above 0x10BB is unreadable, and
 * `read()`/`write()` clamp silently - so an overrun would return plausible-looking dead
 * data instead of failing. This is the gate that prevents that.
 */
static bool probeStorage(void)
{
  const uint16_t length = getStorageAPI().length();
  return (uint32_t)length >= ((uint32_t)EEPROM_LTT_BASE + (uint32_t)LTT_BLOCK_SIZE);
}

bool lttIsAvailable(void)
{
  return lttStorageOk;
}

bool lttIsInitialised(void)
{
  return lttReady;
}

bool lttIsDirty(void)
{
  return lttDirty;
}

void lttMarkDirty(void)
{
  lttDirty = true;
}

const lttBlock &lttGetPage(void)
{
  return lttData;
}

lttBlock &lttGetPageMutable(void)
{
  return lttData;
}

/**
 * @brief Read the block at boot.
 *
 * On a blank or corrupt block the payload is left at its defaults rather than being
 * half-loaded: applying garbage trims is far worse than applying none. A magic mismatch is
 * "never initialised"; a CRC mismatch on a matching magic is "corrupt". Both end up at
 * defaults, but only the first one also rewrites the header.
 */
void lttInit(void)
{
  lttStorageOk = probeStorage();
  lttDirty = false;

  if (!lttStorageOk)
  {
    // Cannot persist anything. Run with no trims and zero authority so nothing is applied.
    setPageToDefaults();
    lttReady = true;
    return;
  }

  const storage_api_t &api = getStorageAPI();

  lttBlockHeader header;
  (void)loadObject(api, EEPROM_LTT_BASE, header);

  const bool magicOk = (header.magic == EEPROM_LTT_MAGIC)
                    && (header.version == EEPROM_LTT_VERSION);

  if (!magicOk)
  {
    // Never initialised (or written by an incompatible version). Start clean and lay down
    // a valid header so the next boot takes the fast path.
    setPageToDefaults();
    lttReady = true;
    (void)lttSave();
    return;
  }

  (void)loadObject(api, EEPROM_LTT_PAYLOAD, lttData);

  if (header.crc != payloadCrc())
  {
    // Corrupt payload. Discard it entirely - never apply a partial or wrong trim.
    setPageToDefaults();
    lttReady = true;
    (void)lttSave();
    return;
  }

  // A stored block can still hold out-of-range values if it was written by an older build
  // or hand-edited. Clamp defensively on the way in.
  for (uint8_t table = 0; table < LTT_TABLE_COUNT; ++table)
  {
    for (uint16_t cell = 0; cell < LTT_CELL_COUNT; ++cell)
    {
      lttData.trim[table][cell] = (int16_t)clamp((int32_t)lttData.trim[table][cell],
                                                (int32_t)-LTT_TRIM_LIMIT_UNITS,
                                                (int32_t)LTT_TRIM_LIMIT_UNITS);
      if (lttData.leanAuth[table][cell] > 100U) { lttData.leanAuth[table][cell] = 100U; }
      if (lttData.richAuth[table][cell] > 100U) { lttData.richAuth[table][cell] = 100U; }
    }
  }

  // The applied trims start at the stored values: a key-on should not re-slew from zero
  // and briefly under/over-fuel while the slew catches up.
  memcpy(lttSlewedUnits, lttData.trim, sizeof(lttSlewedUnits));

  // Keep the settings themselves in range before anything reads them.
  if (lttData.settings.learnRateDiv < 1U) { lttData.settings.learnRateDiv = 1U; }
  if (lttData.settings.learnRateDiv > 6U) { lttData.settings.learnRateDiv = 6U; }

  lttReady = true;
}

/**
 * @brief Does the payload in RAM differ from what is already stored?
 *
 * Used to skip writing when nothing has changed, so that an unrelated full-tune burn does not
 * rewrite 2 kB of untouched trims. The stored CRC is checked first: if it does not match the
 * RAM payload then the header's separate data version forces a rewrite anyway, and the
 * per-byte loop would only be reporting what the CRC already told us.
 *
 * @return true if any payload byte differs from storage.
 */
static bool payloadDiffersFromStorage(void)
{
  if (!lttStorageOk)
  {
    return false;
  }

  const storage_api_t &api = getStorageAPI();
  const byte *p = (const byte *)&lttData;
  const byte *pEnd = p + sizeof(lttData);

  for (uint16_t address = EEPROM_LTT_PAYLOAD; p != pEnd; ++p, ++address)
  {
    if (api.read(address) != *p)
    {
      return true;
    }
  }
  return false;
}

/** @brief Write the block header. Always written, so it always matches the payload. */
static void writeHeader(void)
{
  lttBlockHeader header;
  header.magic = EEPROM_LTT_MAGIC;
  header.version = EEPROM_LTT_VERSION;
  header.reserved[0] = 0U;
  header.reserved[1] = 0U;
  header.reserved[2] = 0U;
  for (uint8_t i = 0; i < 4U; ++i) { header.trailing[i] = 0U; }
  header.crc = payloadCrc();
  updateObject(getStorageAPI(), header, EEPROM_LTT_BASE);
}

/**
 * @brief The one place the payload is written.
 *
 * Payload first, header last. That ordering is the whole crash-safety design: if power is lost
 * mid-save the stored CRC will not match the stored payload, and lttInit() then treats the
 * block as corrupt and discards it. Losing one learn session is recoverable; applying half of
 * one is not.
 *
 * BUDGET SEMANTICS - this matches every other page, and getting it wrong is what caused the
 * burn-loop bug:
 *
 *   @p writesRemaining is a budget of *byte writes*. `updateBlockLimitWriteOps()` returns what
 *   is left of it, and consumes one per byte that actually needed changing. So:
 *     - nonzero return  => the burn happened to have budget left over (not "not finished")
 *     - zero return     => the budget ran out, and the caller must ask again
 *
 *   `savePage()` clears the "burn pending" flag only when the budget hits zero, so a page that
 *   cannot finish must return zero. The header is written only once the payload is completely
 *   stored, which is what preserves the torn-save guarantee.
 *
 * @return Writes remaining. Zero means "budget exhausted, come back for more".
 */
static uint16_t writeBlock(uint16_t writesRemaining)
{
  const uint16_t remaining = updateBlockLimitWriteOps(getStorageAPI(),
                                                     EEPROM_LTT_PAYLOAD,
                                                     (const byte *)&lttData,
                                                     ((const byte *)&lttData) + sizeof(lttData),
                                                     writesRemaining);
  if (remaining == 0U)
  {
    // Budget exhausted, so the payload may be incomplete. Do NOT touch the header: leaving the
    // old CRC in place means a torn save is detected at the next boot rather than trusted.
    return 0U;
  }

  // The payload is fully stored, so the header can now be made to describe it.
  writeHeader();
  lttDirty = false;

  // Preserve the caller's remaining budget so the rest of the pages still get to use it.
  return remaining;
}

/**
 * @brief Persist the block immediately, ignoring the write budget.
 *
 * Safe for the key-off path (`lttSaveIfRequired()`), which runs at RPM 0 outside the burn loop
 * and is not competing with a protocol-driven burn. The page-save path must use
 * lttSaveBlock() instead - see the warning in ltt.h.
 *
 * @return true if storage is usable and the write completed.
 */
bool lttSave(void)
{
  if (!lttStorageOk)
  {
    return false;
  }

  // A budget large enough that it can never be exhausted, so every differing byte is written
  // in this pass and the header is then written. Using LTT_PAYLOAD_SIZE here would be wrong:
  // when every byte differs the budget is consumed exactly, which looks like "exhausted" and
  // would skip the header write, leaving the block permanently dirty.
  (void)writeBlock(UINT16_MAX);
  return !lttDirty;
}

uint16_t lttSaveBlock(uint16_t writesRemaining)
{
  // Contract (matches every other page in savePage()): return 0 to mean "budget exhausted,
  // call me again", and a nonzero value to mean "this page is done". savePage() clears the
  // burn-pending flag only on a zero return.
  //
  // That makes the two "nothing to write" cases below the subtle ones. Returning 0 for them
  // would say "come back with more budget", which the burn loop would obey forever - the exact
  // bug this function exists to avoid. They must report the budget untouched instead.

  if (!lttStorageOk)
  {
    // Cannot store anything at all (e.g. the 4,284-byte internal-EEPROM fallback, which cannot
    // reach 0x1000). Report "done" so the burn terminates rather than retrying forever.
    return writesRemaining;
  }

  if (!lttDirty && !payloadDiffersFromStorage())
  {
    // Nothing has changed. Report "done" and consume no budget, so an unrelated full-tune burn
    // does not rewrite 2 kB of untouched trims.
    return writesRemaining;
  }

  return writeBlock(writesRemaining);
}

bool lttNeedsSave(void)
{
  return lttStorageOk && (lttDirty || payloadDiffersFromStorage());
}

/**
 * @brief Persist the trims once the engine has stopped.
 *
 * Deliberately behind two gates: the trims must be dirty (so a key-off that changed nothing
 * costs no flash wear at all) and the engine must have stopped. Writing at 0 RPM also gets
 * the 8x write-block-size boost the storage layer applies at zero RPM, so the save is
 * shorter than it would be at idle.
 */
void lttSaveIfRequired(void)
{
  if (!lttStorageOk || !lttDirty)
  {
    return;
  }

  if (currentStatus.RPM != 0U)
  {
    return;
  }

  (void)lttSave();
}

// =====================================================================================
// Lookup
// =====================================================================================

/** @brief Translate a VE-axis operating point into a cell index. */
static uint16_t cellIndexFor(const table3d16RpmLoad &axisSource, uint16_t load, uint16_t rpm)
{
  // The trim table shares the VE axes, so find the bin the same way the VE lookup does and
  // land on the same cell. Reusing findCachedBin against the VE axes is what guarantees the
  // trim is never interpolated across a different cell boundary than the VE value it corrects.
  auto xBin = _table2d_detail::findCachedBin(1U, std::begin(axisSource.axisX), std::end(axisSource.axisX),
                                            (table3d_dim_t)div_round_closest_u16<100U>(rpm));
  auto yBin = _table2d_detail::findCachedBin(1U, std::begin(axisSource.axisY), std::end(axisSource.axisY),
                                            (table3d_dim_t)div_round_closest_u16<2U>(load));

  // Values are stored row-major with the upper index of each bin selecting the cell.
  const uint16_t row = (uint16_t)yBin.upperIndex;
  const uint16_t col = (uint16_t)xBin.upperIndex;
  return clampCellIndex((row * LTT_DIM) + col);
}

int16_t lttGetTrimUnits(uint8_t tableIndex, const table3d16RpmLoad &axisSource, uint16_t load, uint16_t rpm)
{
  if (!lttStorageOk || (tableIndex >= LTT_TABLE_COUNT))
  {
    return 0;
  }

  // Interpolate across the 4 surrounding cells rather than snapping to one, so the applied
  // trim is continuous as the engine moves between cells. A flat region (the common case for
  // an untouched table) short-circuits inside interpolate_3d_value_s16().
  const xy_pair_t lookup = { rpm, load };

  const int16_t *pValues = &lttData.trim[tableIndex][0];
  auto xBin = _table2d_detail::findCachedBin(1U, std::begin(axisSource.axisX), std::end(axisSource.axisX),
                                            (table3d_dim_t)div_round_closest_u16<100U>(rpm));
  auto yBin = _table2d_detail::findCachedBin(1U, std::begin(axisSource.axisY), std::end(axisSource.axisY),
                                            (table3d_dim_t)div_round_closest_u16<2U>(load));

  return interpolate_3d_value_s16(lookup, LTT_DIM, pValues, xBin, 100U, yBin, 2U);
}

void lttGetAuthority(uint8_t tableIndex, const table3d16RpmLoad &axisSource, uint16_t load, uint16_t rpm,
                     uint8_t &leanRemovalPct, uint8_t &richAdditionPct)
{
  if (!lttStorageOk || (tableIndex >= LTT_TABLE_COUNT))
  {
    // Feature unavailable: no authority at all, in either direction. This is the safe
    // failure mode - the engine simply runs open loop on the VE table.
    leanRemovalPct = 0U;
    richAdditionPct = 0U;
    return;
  }

  const uint16_t cell = cellIndexFor(axisSource, load, rpm);
  leanRemovalPct = lttData.leanAuth[tableIndex][cell];
  richAdditionPct = lttData.richAuth[tableIndex][cell];
}

// =====================================================================================
// Application
// =====================================================================================

uint8_t lttActiveTableIndex(const config10 &page10, const statuses &current)
{
  // Only an exclusive switch makes fuel table 2 *the* table. In MULTIPLY and ADD the
  // running VE is a blend of both, so there is no single correct trim table; table 1 is
  // used and the learner converges the blend.
  if ((page10.fuel2Mode == FUEL2_MODE_CONDITIONAL_SWITCH) || (page10.fuel2Mode == FUEL2_MODE_INPUT_SWITCH))
  {
    if (current.secondFuelTableActive)
    {
      return 1U;
    }
  }
  return 0U;
}

int16_t lttUnitsToPercent(int16_t units)
{
  // 0.1% units -> whole percent, rounded to nearest, half away from zero.
  if (units >= 0)
  {
    return (int16_t)(((int32_t)units + 5) / 10);
  }
  return (int16_t)(-((((int32_t)-units) + 5) / 10));
}

uint32_t lttApplyTrim(uint32_t intermediate, int16_t trimUnits)
{
  if (trimUnits == 0)
  {
    // Exact: no trim must be bit-identical to no trim, or every fuel calculation shifts
    // by a rounding count for no reason.
    return intermediate;
  }

  // multiplier = 1000 + trimUnits, applied with a divisor of 1000.
  // Range check: trimUnits is bounded to +/-1000, so multiplier is 0..2000. intermediate is
  // bounded by the caller at UINT16_MAX, so intermediate * 2000 fits comfortably in uint32_t.
  const int32_t multiplier = (int32_t)1000 + (int32_t)trimUnits;

  if (multiplier <= 0)
  {
    // A -100% trim can only happen if the limits are bypassed. Return zero fuelling rather
    // than letting a negative intermediate wrap into nonsense; the floor in fuel_calcs
    // catches this before it reaches the injector.
    return 0U;
  }

  const int64_t scaled = (int64_t)intermediate * (int64_t)multiplier;
  const int64_t rounded = (scaled + 500) / 1000;

  if (rounded <= 0)
  {
    return 0U;
  }
  return (uint32_t)rounded;
}

uint16_t lttFuelMultiplierFloorPct(void)
{
  return FUEL_MULTIPLIER_FLOOR_PCT;
}

// =====================================================================================
// Learning
// =====================================================================================

/**
 * @brief Should the learner run for this cycle?
 *
 * This is the strict superset of isAfrClosedLoopOperational() described in the design. Each
 * condition is here for a specific reason, recorded so it is not "simplified" away later:
 *
 *  - Wideband only: a narrowband sensor cannot tell rich from very rich, so there is no
 *    usable error signal to integrate.
 *  - Not cranking: cranking has its own enrichment and the VE table is not the model.
 *  - Not in ASE / WUE: both are programmes that decay on purpose. Learning from them would
 *    bake a transient into a permanent trim.
 *  - Not in AE (tip-in/tip-out) or decel: deliberate transients, same reason.
 *  - Not DFCO: fuel cut is not a mixture at all.
 *  - Coolant above threshold and O2 warm: a cold engine misfires, which reads as a false lean.
 *  - MAP and O2 inside their configured windows: outside them the reading is not trustworthy.
 *  - Above a minimum RPM: below it the engine is stopping, and those samples are worthless.
 *
 * @param[out] cell Receives the cell being evaluated, when learning is permitted.
 * @return true if the learner may update a trim this cycle.
 */
static bool isLearnPermitted(const config6 &page6, const config9 &page9, const config10 &page10,
                            const lttConfig &settings, const statuses &current, uint16_t &cell)
{
  if (settings.mode != LTT_MODE_LEARN)
  {
    return false;
  }

  const uint16_t minRPM = (uint16_t)settings.minRPMDiv100 * 100U;

  const bool conditionsClear =
         (page6.egoType == EGO_TYPE_WIDE)
      && (current.rotationStatus == EngineRotationStatus::Running)
      && (!current.aseIsActive)
      && (!current.wueIsActive)
      && (!current.isAcceleratingTPS)
      && (!current.isDeceleratingTPS)
      && (!current.isDFCOActive)
      && (current.AEamount <= 100U)
      && (current.coolant > temperatureRemoveOffset(page6.egoTemp))
      && (current.runSecs > page6.ego_sdelay)
      && (current.O2 < page6.ego_max)
      && (current.O2 > page6.ego_min)
      && (current.MAP <= (uint16_t)((uint16_t)page9.egoMAPMax * 2U))
      && (current.MAP >= (uint16_t)((uint16_t)page9.egoMAPMin * 2U))
      && (current.RPM > minRPM);

  if (!conditionsClear)
  {
    return false;
  }

  cell = clampCellIndex(cellIndexFor(fuelTable, current.fuelLoad, current.RPM));
  return true;
}

/**
 * @brief Run one learn step against the current cell.
 *
 * Called only from the EGO cycle, so the update rate is already tied to how often the
 * closed-loop controller actually produces a new correction.
 */
void lttLearn(void)
{
  if (!lttStorageOk || !lttReady)
  {
    return;
  }

  const lttConfig &settings = lttData.settings;
  uint16_t cell;
  if (!isLearnPermitted(configPage6, configPage9, configPage10, settings, currentStatus, cell))
  {
    return;
  }

  const uint8_t table = lttActiveTableIndex(configPage10, currentStatus);
  if (table >= LTT_TABLE_COUNT)
  {
    return;
  }

  // The error is what short-term trim had to do, in whole percent, signed about zero.
  const int16_t errorPct = (int16_t)currentStatus.egoCorrection;

  // The trim is bounded by this cell's authority, not just by the global limit. If a cell is
  // allowed to add fuel but not remove it, a sustained lean error must not be able to drive
  // the trim negative past zero. Without this, the authority table would bound the short-term
  // trim while the long-term trim quietly walked past it - which is precisely the WOT
  // protection this feature exists to provide.
  //
  // Named after what they permit, matching corrections.cpp: "lean correction" ADDS fuel, so
  // maxAddPct is positive and maxRemovePct is negative. Naming these lean/rich made them
  // trivially swappable, and they were swapped once already.
  const int16_t maxRemovePct = -(int16_t)lttData.leanAuth[table][cell];
  const int16_t maxAddPct = (int16_t)lttData.richAuth[table][cell];

  if (errorPct < maxRemovePct)
  {
    // The short-term trim is being clamped by this cell's remove-fuel authority, so any
    // further learning would be reacting to a limit rather than to the engine.
    return;
  }
  if (errorPct > maxAddPct)
  {
    return;
  }

  // Convert to 0.1% units so it can be compared against the dead-band and added to the trim.
  const int32_t errorUnits = (int32_t)errorPct * 10;

  if (errorUnits > -(int32_t)settings.deadbandUnits && errorUnits < (int32_t)settings.deadbandUnits)
  {
    // Inside the noise floor. Count it as a clean sample but do not move the cell.
    if (lttSampleCount[table][cell] < 255U) { ++lttSampleCount[table][cell]; }
    return;
  }

  // A cell must be seen clean repeatedly before it is allowed to move.
  if (lttSampleCount[table][cell] < settings.minSamples)
  {
    ++lttSampleCount[table][cell];
    return;
  }

  // Step is a fraction of the error, so the trim converges geometrically instead of
  // oscillating. learnRateDiv of 3 means 1/8 of the error per update.
  const uint8_t rateDiv = (uint8_t)clamp((int32_t)settings.learnRateDiv, (int32_t)1, (int32_t)6);
  int32_t step = errorUnits >> rateDiv;

  // A genuinely tiny step would never accumulate; give it a floor of one unit.
  if (step == 0)
  {
    step = (errorUnits > 0) ? 1 : -1;
  }

  // Rate limit: a single bad sample must not be able to write a large trim.
  if (settings.maxStepUnits != 0U)
  {
    step = (int32_t)clamp(step,
                         -(int32_t)settings.maxStepUnits,
                          (int32_t)settings.maxStepUnits);
  }

  const int32_t target = (int32_t)lttData.trim[table][cell] + step;

  // Clamp to this cell's authority as well as the global limit, so a trim can never accumulate
  // beyond what the authority table says may be applied at that cell. The applied trim is what
  // matters, and letting the stored value exceed it would make the stored table a lie - and
  // would silently start applying the excess the moment a user widened the authority.
  const int32_t cellLower = (maxRemovePct * 10 < -LTT_TRIM_LIMIT_UNITS) ? -LTT_TRIM_LIMIT_UNITS : (maxRemovePct * 10);
  const int32_t cellUpper = (maxAddPct * 10 > LTT_TRIM_LIMIT_UNITS) ? LTT_TRIM_LIMIT_UNITS : (maxAddPct * 10);

  const int16_t clamped = (int16_t)clamp(target, cellLower, cellUpper);

  if (clamped != lttData.trim[table][cell])
  {
    lttData.trim[table][cell] = clamped;
    lttDirty = true;
  }
}

// =====================================================================================
// Slew
// =====================================================================================

/**
 * @brief Move the applied trim toward the target trim, rate limited.
 *
 * Called on a fixed cadence from the main loop. The slew is per cell rather than global
 * because two cells can hold very different trims and moving between them should not
 * produce a step.
 *
 * @param elapsedMs Milliseconds since the last call.
 */
void lttUpdateSlew(uint16_t elapsedMs)
{
  if (!lttStorageOk || !lttReady)
  {
    return;
  }

  const uint8_t rate = lttData.settings.slewUnitsPerSec;
  if (rate == 0U)
  {
    // No slew configured: the applied trim tracks the target exactly.
    memcpy(lttSlewedUnits, lttData.trim, sizeof(lttSlewedUnits));
    return;
  }

  // Maximum movement this call, in 0.1% units. Rate is in units/second.
  int32_t maxStep = ((int32_t)rate * (int32_t)elapsedMs) / 1000;
  if (maxStep < 1)
  {
    maxStep = 1;
  }

  for (uint8_t table = 0; table < LTT_TABLE_COUNT; ++table)
  {
    for (uint16_t cell = 0; cell < LTT_CELL_COUNT; ++cell)
    {
      const int32_t target = lttData.trim[table][cell];
      int32_t applied = lttSlewedUnits[table][cell];
      const int32_t delta = target - applied;

      if (delta > maxStep)       { applied += maxStep; }
      else if (delta < -maxStep) { applied -= maxStep; }
      else                       { applied = target; }

      lttSlewedUnits[table][cell] = (int16_t)applied;
    }
  }
}

int16_t lttGetSlewedTrimUnits(uint8_t tableIndex, const table3d16RpmLoad &axisSource, uint16_t load, uint16_t rpm)
{
  if (!lttStorageOk || (tableIndex >= LTT_TABLE_COUNT))
  {
    return 0;
  }

  // A cell with zero authority is a cell the user has said "do not touch" - typically WOT
  // under boost. Honouring that must mean *no trim at all*, not "no new trim": a trim learned
  // earlier would otherwise keep pulling fuel at that operating point with no remaining
  // authority to correct it if it turned out to be wrong. Zeroing authority is therefore the
  // single, inspectable "hands off here" control, and it is total.
  const uint16_t cell = cellIndexFor(axisSource, load, rpm);
  const uint8_t leanPct = lttData.leanAuth[tableIndex][cell];
  const uint8_t richPct = lttData.richAuth[tableIndex][cell];

  if ((leanPct == 0U) && (richPct == 0U))
  {
    return 0;
  }

  const xy_pair_t lookup = { rpm, load };

  const int16_t *pValues = &lttSlewedUnits[tableIndex][0];
  auto xBin = _table2d_detail::findCachedBin(1U, std::begin(axisSource.axisX), std::end(axisSource.axisX),
                                            (table3d_dim_t)div_round_closest_u16<100U>(rpm));
  auto yBin = _table2d_detail::findCachedBin(1U, std::begin(axisSource.axisY), std::end(axisSource.axisY),
                                            (table3d_dim_t)div_round_closest_u16<2U>(load));

  const int16_t trim = interpolate_3d_value_s16(lookup, LTT_DIM, pValues, xBin, 100U, yBin, 2U);

  // Clamp the *applied* trim to this cell's authority. The stored value is held within
  // authority by the learner, but the tables can also be edited directly in TunerStudio, and
  // a hand-edited trim must not be able to exceed the authority the user declared for that
  // cell. Interpolation across cells can also produce a value slightly outside either
  // neighbour's limit, so the clamp is on the result, not the inputs.
  //
  // UNITS: authority is whole percent, trim is 0.1% units, so the limits are scaled by 10
  // here. Getting this wrong silently caps every trim at 1/10th of the declared authority.
  const int16_t lower = -(int16_t)((uint16_t)leanPct * 10U);
  const int16_t upper = (int16_t)((uint16_t)richPct * 10U);
  return (int16_t)clamp(trim, lower, upper);
}

#if defined(UNIT_TEST)
void setAllLttAuthority(uint8_t percent)
{
  for (uint8_t table = 0; table < LTT_TABLE_COUNT; ++table)
  {
    for (uint16_t cell = 0; cell < LTT_CELL_COUNT; ++cell)
    {
      lttData.leanAuth[table][cell] = percent;
      lttData.richAuth[table][cell] = percent;
    }
  }
}
#endif

#if defined(UNIT_TEST)
void lttResetForTest(void)
{
  setPageToDefaults();
  // Respect the installed storage API's actual size, so a test that deliberately installs a
  // too-small backend (to prove the feature disables itself) sees the real behaviour. When the
  // native test host has no storage API at all, `length()` returns 0 - in that case force the
  // feature on, because the closed-loop tests need real authority and would otherwise be
  // testing "clamped to zero" rather than the logic under test.
  const uint16_t length = getStorageAPI().length();
  lttStorageOk = (length == 0U) || probeStorage();
  lttReady = true;
  lttDirty = false;
}
#endif
