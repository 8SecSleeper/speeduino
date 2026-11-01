#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config_pages.h"
#include "table3d.h"

/**
 * @file
 * @brief Long-term fuel trims (LTFT), the realtime learn table and the EGO authority tables.
 *
 * This is the Holley-style fuelling layer. Three coupled parts, all in service of making
 * closed-loop fuelling behave like a Dominator:
 *
 *  1. **EGO authority** - how much fuel the short-term trim may add or remove at a given
 *     cell. Stored per cell and *asymmetric*, because "may I add fuel here" and "may I take
 *     fuel away here" are different questions with different consequences.
 *  2. **The learn table** - a realtime 16x16 signed trim that accumulates what closed loop
 *     had to do, so the engine stays corrected when the O2 sensor is not reading.
 *  3. **Persistence** - the trims survive a key cycle.
 *
 * @section design Design contract
 *
 * The trim is applied **once**, in fuel_calcs.cpp, after the AFR multiplier and outside
 * sumCorrections(). It is deliberately *not* part of the short-term feedback path:
 *
 *  - If the trim were inside the loop that produces it, the learner would integrate against
 *    its own output and diverge.
 *  - The learner must know the value the VE table *should* have had, so that errors converge
 *    to truth rather than being permanently absorbed by the trim.
 *
 * Invariant: `egoCorrection` (short-term) is NEVER folded into the learned value.
 *
 * @section units Units
 *
 * Every trim is **signed percent in 0.1% units** (so 100 == +10.0%, -1000 == -100.0%).
 * Every authority is **unsigned percent in 1% units** (0..100). Mixing these up is the
 * single easiest way to break the loop, so the names carry the unit: `...Units` for 0.1%
 * and plain values for whole percent.
 */

/// @name Trim limits
/// @{
/** @brief Maximum magnitude of a learned trim, in 0.1% units (1000 == 100.0%). */
constexpr int16_t LTT_TRIM_LIMIT_UNITS = 1000;

/** @brief Default learn dead-band, in 0.1% units (2 == 0.2%).
 *
 * Chasing below this is chasing wideband noise: 0.2% of fuelling is roughly 0.03 AFR at
 * stoich, comfortably inside a typical controller's accuracy. */
constexpr int16_t LTT_DEADBAND_DEFAULT_UNITS = 2;

/** @brief Hard floor on the total fuelling multiplier, in whole percent.
 *
 * Independent of any trim, and independent of `egoLimit`. The existing corrections clamp
 * (`min(1500, sum)` in correctionsFuel()) has no lower bound at all, so a lean-side trim
 * of 100% could otherwise multiply fuelling by ~0. This is the backstop that makes a
 * mis-set authority table survivable. */
constexpr uint16_t FUEL_MULTIPLIER_FLOOR_PCT = 25;

/// @}
/// @name Authority defaults
/// @{
/** @brief Default per-cell authority, whole percent, for both lean and rich.
 *
 * Generous compared to the 16% the scalar `egoLimit` used to cap at, because full-authority
 * closed loop is the point of this feature. It is deliberately *not* 100: a fresh tune
 * should not be able to swing fuelling by a factor of two before anyone has looked at it.
 * Cells at WOT/boost are expected to be tightened by the user - that is the safety control.
 */
constexpr uint8_t LTT_AUTHORITY_DEFAULT_PCT = 25;
/// @}

/// @name Block identity
/// @{
/** @brief Base address of the entire custom block. Above the frozen 0x0000-0x0FFF ABI. */
constexpr uint16_t EEPROM_LTT_BASE = 0x1000;

/** @brief 'SLTT' - "Speeduino Long Term Trim". Detects "never initialised" vs "corrupt". */
constexpr uint32_t EEPROM_LTT_MAGIC = 0x534C5454;

/** @brief Layout version. Bump when the block layout changes; older data is then discarded. */
constexpr uint8_t EEPROM_LTT_VERSION = 1;

/** @brief Size of the block header, in bytes. Must stay a multiple of 2 for int16 alignment. */
constexpr uint16_t EEPROM_LTT_HEADER_SIZE = 16;

/** @brief EEPROM address of the TunerStudio-visible payload (immediately after the header).
 *
 * The header is deliberately *outside* the TS page: TunerStudio writes whole pages, so if
 * the magic/CRC shared a page with user data, any TS burn would overwrite them with whatever
 * the tuning software happened to be holding. */
constexpr uint16_t EEPROM_LTT_PAYLOAD = EEPROM_LTT_BASE + EEPROM_LTT_HEADER_SIZE;
/// @}

/** @brief Number of fuel tables that carry their own trim. Mirrors fuelTable/fuelTable2. */
constexpr uint8_t LTT_TABLE_COUNT = 2;

/** @brief Side length of a trim / authority table. Matches the VE table so cells line up 1:1. */
constexpr uint8_t LTT_DIM = 16;

/** @brief Number of cells in a trim / authority table. */
constexpr uint16_t LTT_CELL_COUNT = LTT_DIM * LTT_DIM;

/** @brief Index of a working mode / enable flag. */
enum LttMode : uint8_t {
  LTT_MODE_OFF = 0,   ///< Learning disabled. Trims are still applied if stored.
  LTT_MODE_LEARN = 1, ///< Wealth-learn: accumulate the trim; the VE table is untouched.
};

/** @brief Persistent settings for the long-term trim system.
 *
 * Plain bytes, no bit-fields: this struct is a raw TunerStudio page, so its offsets are part
 * of the tuning-file ABI and must be obvious by inspection. Keep it 16 bytes.
 *
 * Deliberately *not* derived from config_page_t. It is a member of lttBlock, which is itself
 * derived from config_page_t, and a nested empty base defeats the empty-base optimisation:
 * the compiler reserves a byte for it, which pushes every subsequent field out of alignment
 * with the offsets TunerStudio reads. The assertions below catch exactly that.
 */
struct lttConfig {
  uint8_t mode;           ///< @ref LttMode. 0 = off (default), 1 = learn.
  uint8_t learnRateDiv;   ///< Learn step = error >> this. 3 == 1/8 per update. Clamped 1..6.
  uint8_t deadbandUnits;  ///< Learn dead-band, in 0.1% units.
  uint8_t minRPMDiv100;   ///< Do not learn below this RPM/100.
  uint8_t saveRPMDiv100;  ///< Persist the trims once RPM falls below this RPM/100.
  uint8_t minSamples;     ///< Clean samples required before a cell may move. 0 = no gate.
  uint8_t maxStepUnits;   ///< Maximum learn step per update, 0.1% units. 0 = unlimited.
  uint8_t slewUnitsPerSec;///< Maximum trim change applied per second, 0.1% units. 0 = no slew.
  uint16_t reserved0;     ///< Reserved, must be 0.
  uint8_t reserved1[6];   ///< Reserved, must be 0.
} __attribute__((packed));

static_assert(sizeof(lttConfig) == EEPROM_LTT_HEADER_SIZE, "lttConfig must stay 16 bytes");

/** @brief Everything TunerStudio can see, in page-offset order.
 *
 * This is one contiguous, little-endian, packed block so page offsets map linearly. The
 * int16 arrays therefore appear to TunerStudio as plain `S16` arrays - no byte-swapping and
 * no special table3d handling is needed on the page path.
 *
 * The trim/authority tables share the VE table's axes rather than storing their own. That
 * is deliberate: duplicated axes can drift from the table they are meant to correct, and the
 * whole point of aligning 1:1 with VE is that cell (n,m) means the same operating point.
 * TunerStudio points `xBins`/`yBins` at the VE bins, so the grid still shows real values.
 */
struct lttBlock : public config_page_t {
  lttConfig settings;                    ///< Block settings, offset 0
  int16_t trim[LTT_TABLE_COUNT][LTT_CELL_COUNT]; ///< Learned trims, 0.1% units
  uint8_t leanAuth[LTT_TABLE_COUNT][LTT_CELL_COUNT]; ///< Max fuel REMOVAL, whole percent
  uint8_t richAuth[LTT_TABLE_COUNT][LTT_CELL_COUNT]; ///< Max fuel ADDITION, whole percent
} __attribute__((packed, aligned(__alignof__(uint16_t))));

/** @brief Size of the TunerStudio-visible payload, in bytes. */
constexpr uint16_t LTT_PAYLOAD_SIZE = sizeof(lttBlock);

/** @brief Total size of the custom block including its header. */
constexpr uint16_t LTT_BLOCK_SIZE = EEPROM_LTT_HEADER_SIZE + LTT_PAYLOAD_SIZE;

// ---- Size contract -------------------------------------------------------------------
// These are not decoration: the payload size is written into reference/speeduino.ini as
// page 16's `pageSize`, and the block offsets are asserted against the emulated EEPROM
// ceiling below. If any of these fail, the INI and the firmware have diverged and
// TunerStudio would read garbage - so fail the build rather than ship that.
//
//   settings       16 B
//   trim         2 x 256 x 2 B = 1024 B
//   leanAuth     2 x 256 x 1 B =  512 B
//   richAuth     2 x 256 x 1 B =  512 B
//                             ----------
//   payload                     2064 B
// ---- Layout contract ------------------------------------------------------------------
// TunerStudio addresses this page as (page 16, byte offset). The offsets are declared in
// reference/speeduino.ini and MUST match offsetof() here exactly - a mismatch produces no
// error at all, just wrong values, which is the worst possible failure mode. So assert every
// offset rather than trusting the struct's natural layout.
//
//   settings       16 B   @    0
//   trim          1024 B   @   16   (2 x 256 x int16, 0.1% units)
//   leanAuth       512 B   @ 1040   (2 x 256 x uint8, whole percent)
//   richAuth       512 B   @ 1552   (2 x 256 x uint8, whole percent)
//                        ----------
//   payload       2064 B
static_assert(offsetof(lttBlock, settings) == 0,    "lttBlock.settings must be at offset 0");
static_assert(offsetof(lttBlock, trim) == 16,       "lttBlock.trim must be at offset 16");
static_assert(offsetof(lttBlock, leanAuth) == 1040, "lttBlock.leanAuth must be at offset 1040");
static_assert(offsetof(lttBlock, richAuth) == 1552, "lttBlock.richAuth must be at offset 1552");
static_assert(sizeof(lttBlock) == 2064, "lttBlock size changed - update pageSize in speeduino.ini");

// The emulated EEPROM is 23,808 bytes (0x0000..0x5CFF), see AGENTS.md 2.1. The block must
// sit entirely inside it, above the frozen 0x0000-0x0FFF core tune ABI.
static_assert((uint32_t)EEPROM_LTT_BASE >= 0x1000U, "Custom data must live above the core tune block");
static_assert(((uint32_t)EEPROM_LTT_BASE + (uint32_t)LTT_BLOCK_SIZE) <= 0x5D00U,
              "LTT block does not fit in the emulated EEPROM address space");

/** @brief The live page payload.
 *
 * A global rather than a file-static so that pages.cpp can take its address in a
 * `constexpr` page map, exactly as it does for every config page. The page layer needs the
 * address at compile time; a function call cannot provide that.
 */
extern lttBlock lttData;

// ============================== Runtime state ==============================

/** @brief True if the storage backend is large enough to hold the block.
 *
 * The internal-EEPROM fallback is only 4,284 bytes, which cannot reach 0x1000 at all. On
 * that backend the feature is disabled outright rather than reading/writing past the end -
 * `read()`/`write()` clamp silently, so an overrun would look like working code returning
 * dead data.
 */
bool lttIsAvailable(void);

/** @brief True if the persisted block was found intact (magic + version + CRC) at boot. */
bool lttIsInitialised(void);

/** @brief Total fuel multiplier floor, in whole percent. Never returns 0. */
uint16_t lttFuelMultiplierFloorPct(void);

/**
 * @brief Look up the learned trim for an operating point.
 *
 * @param tableIndex   0 = fuelTable, 1 = fuelTable2
 * @param axisSource   The VE table whose axes this trim shares
 * @param load         Load (y axis)
 * @param rpm          Engine speed (x axis)
 * @return The interpolated trim in 0.1% units. Returns 0 when the feature is disabled.
 */
int16_t lttGetTrimUnits(uint8_t tableIndex, const table3d16RpmLoad &axisSource, uint16_t load, uint16_t rpm);

/**
 * @brief Interpolate the *slewed* trim - the value actually applied to fuelling.
 *
 * Deliberately separate from lttGetTrimUnits(): the stored trim is the target the learner
 * drives toward, and the slewed trim is what reaches the injector. A trim change therefore
 * cannot appear as a step change in pulse width.
 *
 * @param tableIndex 0 = fuelTable, 1 = fuelTable2
 * @param axisSource The VE table whose axes this trim shares
 * @param load       Load (y axis)
 * @param rpm        Engine speed (x axis)
 * @return The interpolated, rate-limited trim in 0.1% units.
 */
int16_t lttGetSlewedTrimUnits(uint8_t tableIndex, const table3d16RpmLoad &axisSource, uint16_t load, uint16_t rpm);

/**
 * @brief Advance the trim slew. Call on a fixed cadence from the main loop.
 * @param elapsedMs Milliseconds since the previous call.
 */
void lttUpdateSlew(uint16_t elapsedMs);

/**
 * @brief Look up the asymmetric per-cell EGO/trim authority.
 *
 * @param tableIndex 0 = fuelTable, 1 = fuelTable2
 * @param axisSource The VE table whose axes this authority shares
 * @param load       Load (y axis)
 * @param rpm        Engine speed (x axis)
 * @param[out] leanRemovalPct Most fuel that may be REMOVED at this cell, whole percent (>= 0)
 * @param[out] richAdditionPct Most fuel that may be ADDED at this cell, whole percent (>= 0)
 */
void lttGetAuthority(uint8_t tableIndex, const table3d16RpmLoad &axisSource, uint16_t load, uint16_t rpm,
                     uint8_t &leanRemovalPct, uint8_t &richAdditionPct);

/**
 * @brief Which trim table applies, given the secondary-fuel-table configuration.
 *
 * Factored into one place so the trim and the VE lookup can never disagree about which
 * table is in force. Only an exclusive switch mode makes table 2 the active table; in
 * MULTIPLY/ADD the running VE is a blend, so table 1's trim is applied instead (the learner
 * then converges the blend, which is the only honest single answer available).
 */
uint8_t lttActiveTableIndex(const config10 &page10, const statuses &current);

/** @brief Convert a signed trim in 0.1% units to a whole-percent multiplier offset. */
int16_t lttUnitsToPercent(int16_t units);

/** @brief Apply a trim in 0.1% units to an integer intermediate, rounded to nearest.
 *
 * Exactness matters: a trim of 0 must return @p intermediate bit-identically, so that
 * "learning enabled but nothing learned yet" cannot perturb fuelling by even one count.
 */
uint32_t lttApplyTrim(uint32_t intermediate, int16_t trimUnits);

/** @brief Run one learn step. Called from the EGO cycle, never from the main loop. */
void lttLearn(void);

/** @brief Persist the trims if they are dirty and the engine has stopped. */
void lttSaveIfRequired(void);

/** @brief Load the block at boot. Safe to call before the rest of the tune is loaded. */
void lttInit(void);

/**
 * @brief Restore the block to its power-on defaults, in RAM only.
 *
 * Called by setTuneToEmpty() when a board is being set up for the first time. That function
 * zeroes every page, which for this block would mean zero authority - silently disabling
 * closed loop entirely rather than merely starting with no trims. A fresh board should get
 * "no trims learned yet, but closed loop works", which is what this provides.
 *
 * Does not write storage; the caller's normal save path handles persistence.
 */
void lttRestoreDefaults(void);

/// @name Test and TunerStudio hooks
/// @{
/** @brief Mark the trims dirty so the next stopped-engine poll persists them. */
void lttMarkDirty(void);

/** @brief Force a persist now, regardless of RPM. Used by an explicit TS burn. */
bool lttSave(void);

/**
 * @brief Persist the block, honouring the storage write budget. Called from savePage().
 *
 * This is the *only* correct way for the page-save machinery to persist the block, because
 * the burn loop depends on the budget being reported back accurately. `savePage()` clears the
 * "burn pending" flag when the budget reaches zero, and `speeduino.ino()` re-enters
 * `saveAllPages()` for as long as that flag stays cleared:
 *
 *     if (isEepromWritePending() && ...) { saveAllPages(); }
 *
 * So a page save that ignores the budget and always reports success makes the loop run
 * forever - a flash-destroying bug that is invisible in a unit test that only checks the data
 * round-trips.
 *
 * The budget applies to PAYLOAD bytes only. The header is always rewritten, because it is
 * small, must always match the payload, and is written last so a torn save is detectable.
 *
 * @param writesRemaining Write budget for this pass.
 * @return Writes still remaining. Zero means this page is finished.
 */
uint16_t lttSaveBlock(uint16_t writesRemaining);

/**
 * @brief True if the block differs from what is stored.
 *
 * Used by the burn path to skip writing entirely when nothing has changed. Without this, any
 * full-tune burn would rewrite the whole 2 kB block even though the trims had not moved,
 * which costs flash life for nothing. `update()` already skips unchanged bytes, but the cost
 * we are avoiding here is the sector erase, not the byte writes.
 */
bool lttNeedsSave(void);

/** @brief Read-only access to the page payload. */
const lttBlock &lttGetPage(void);

/** @brief Mutable access to the page payload, for tests and TunerStudio writes. */
lttBlock &lttGetPageMutable(void);

/** @brief True if the trims have changed since the last persist. */
bool lttIsDirty(void);
/// @}

#if defined(UNIT_TEST)
/** @brief Reset the module to its power-on state. Unit tests only. */
void lttResetForTest(void);

/** @brief Set every cell's authority to the same value, in whole percent. Unit tests only. */
void setAllLttAuthority(uint8_t percent);
#endif
