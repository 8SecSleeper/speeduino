#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config_pages.h"
#include "load_source.h"

/**
 * @file
 * @brief FND-05: the race-time framework. One clock, many readers.
 *
 * Drag racing is a time domain: boost vs seconds-from-launch, retard vs seconds,
 * nitrous ramp vs seconds, a perfect-pass reference curve vs driveshaft speed. Before this
 * module the firmware had no time base at all - `runSecs` is a byte of whole seconds, which
 * is useless at 1.2 s into a pass.
 *
 * @section contract Design contract
 *
 *  1. **One clock.** `raceTimeMs` is the single source of elapsed time for every consumer.
 *     A consumer must never start its own timer; if two features each count seconds from the
 *     launch they will disagree about when the launch was.
 *  2. **The clock is the only thing that is time-critical.** It is advanced from the 1 kHz
 *     main-loop gate, so its resolution is 1 ms and it does not depend on any controller's
 *     cadence. Consumers may read it from wherever they run.
 *  3. **Configuration is persisted; the clock is not.** The clock is runtime state. There is
 *     nothing worth storing about a pass that has finished, and storing a running clock would
 *     mean a flash write per millisecond.
 *  4. **Nothing here reads or writes storage unless the backend can hold the block.** The
 *     internal-EEPROM fallback is 4,284 bytes and cannot reach 0x1820 at all - see
 *     @ref raceTimeIsAvailable.
 *  5. **A stale or half-written block is discarded, never trusted.** Payload is written first
 *     and the header (which carries the CRC) last, so a save interrupted by a key-off leaves
 *     a CRC that does not match and the block is reset to defaults on the next boot.
 *
 * @section layout Storage layout
 *
 * Mirrors `ltt.h` exactly, because that is the established convention in this tree for a
 * TunerStudio page above the frozen core tune:
 *
 * | Region | Address | Size |
 * |---|---|---|
 * | Header (magic, version, CRC) | `0x1820` | 16 B |
 * | TunerStudio payload (page 17) | `0x1830` | 256 B |
 *
 * The header is deliberately *outside* the TS page. TunerStudio writes whole pages, so if the
 * magic and CRC shared a page with user data, every burn would overwrite them with whatever
 * the tuning software happened to be holding.
 *
 * @section states The state machine
 *
 *     Off -----> Armed -----> Running -----> Stopped
 *      ^           |             |              |
 *      |           |             +-- false start --+
 *      +-----------+-------------+--------------+
 *                (reset)
 *
 *  - **Armed** is entered when the arm input is asserted (or the arm source is set to
 *    "always armed"). No clock runs yet.
 *  - **Running** begins on a *release* edge of the configured start source. Release, not
 *    assertion: the clock starts when the trans-brake or launch pin is let go, which is when
 *    the car actually moves.
 *  - **Stopped** is entered from Running when any configured stop condition latches. The
 *    final time is preserved until the next reset/re-arm.
 *  - **False start** is flagged when the arm source is still asserted, the race has not
 *    started, and movement is detected above the configured threshold. The clock is not
 *    started; the run is marked invalid so a tuner reading the log sees why.
 */

/// @name Block identity
/// @{
/** @brief Base address of the entire custom block. Above the frozen 0x0000-0x0FFF ABI, and
 *  immediately after the long-term-trim block (0x1000..0x181F). */
constexpr uint16_t EEPROM_RACETIME_BASE = 0x1820;

/** @brief 'RACE'. Detects "never initialised" vs "corrupt". */
constexpr uint32_t EEPROM_RACETIME_MAGIC = 0x52414345;

/**
 * @brief Layout version. Bump whenever the payload layout changes.
 *
 * A stale block whose version does not match is discarded and replaced with defaults.
 *
 * v1 -> v2: the curve gained a stored time axis (`curveTimeMs`, offsets 36..67) and
 * `curveRetard` moved from offset 48 to 68. The payload is still 256 bytes, so a v1 block
 * passes every size assertion and its CRC still matches its own bytes - only the meaning
 * changed. Reading one as a v2 block is exactly what produced 2147483647 in every curve cell
 * and nonsense in the threshold fields.
 *
 * The CRC cannot catch this on its own: it validates integrity, not meaning. The version byte
 * is the only thing that distinguishes "written by an older build" from "corrupt".
 */
constexpr uint8_t EEPROM_RACETIME_VERSION = 2;

/** @brief Size of the block header, in bytes. Must stay a multiple of 2 for alignment. */
constexpr uint16_t EEPROM_RACETIME_HEADER_SIZE = 16;

/** @brief EEPROM address of the TunerStudio-visible payload (immediately after the header). */
constexpr uint16_t EEPROM_RACETIME_PAYLOAD = EEPROM_RACETIME_BASE + EEPROM_RACETIME_HEADER_SIZE;
/// @}

/**
 * @brief Miles per hour to kilometres per hour, expressed as a fraction.
 *
 * 1 mph = 1.609344 km/h exactly. Keeping it as numerator/denominator avoids a float on a 1 kHz
 * path and avoids a rounded integer constant.
 *
 * The numerator MUST be 32-bit: 160934 does not fit in uint16_t and silently truncates to
 * 29862, which turns a 10 mph threshold into 2 km/h. That bug was caught by the threshold
 * boundary tests, not by any build - a narrower type here compiles cleanly and is simply wrong.
 */
constexpr uint32_t MPH_TO_KMH_NUM = 160934U; ///< 1.60934 scaled by 1e5
constexpr uint32_t MPH_TO_KMH_DEN = 100000U;
static_assert(MPH_TO_KMH_NUM > 65535U,
              "MPH_TO_KMH_NUM must be 32-bit; a 16-bit type truncates it silently");

/// @name Payload geometry
/// @{
/** @brief Number of points on the retard-vs-race-time curve. */
constexpr uint8_t RACETIME_CURVE_POINTS = 16;

/** @brief Size of a user-editable axis label, including its NUL terminator.
 *
 * Matches the `prgm_out00Alias` / `AUXin00Alias` string length already used in speeduino.ini,
 * so TunerStudio renders it identically and a tuner needs to learn nothing new. */
constexpr uint8_t RACETIME_LABEL_LEN = 20;

/** @brief Reserved tail bytes. Keeps the payload a round 256 B (a whole TS page).
 *
 * Deliberately reserved rather than omitted: this page is the natural home for the LC-02
 * timed-launch ramp, BST-01 / BST-03 boost-by-time and N2O timers, all of which the roadmap
 * budgets against FND-05. Growing into reserved space costs nothing and does not move the
 * fields above it. */
constexpr uint16_t RACETIME_PAYLOAD_SIZE = 256;
/// @}

/** @brief What causes the clock to start. */
enum RaceTimeStartSource : uint8_t {
  RT_START_LAUNCH_PIN_RELEASE = 0, ///< Release edge of @ref raceTimeConfig::armPin
  RT_START_PROG_OUTPUT_RELEASE = 1,///< Release edge of a programmable output (not yet implemented, see the note in raceTime.cpp)
  RT_START_EXTERNAL = 2,           ///< Only the firmware API - nothing in TunerStudio triggers it
};

/** @brief What freezes the clock once it is running. */
enum RaceTimeStopSource : uint8_t {
  RT_STOP_NONE = 0,        ///< Run until explicitly reset. The normal drag setting.
  RT_STOP_TIME = 1,        ///< Stop after @ref raceTimeConfig::stopTimeMs
  RT_STOP_ARM_PIN = 2,     ///< Stop when the arm input is re-asserted (e.g. trans-brake reapplied)
  RT_STOP_SPEED = 3,       ///< Stop above @ref raceTimeConfig::stopSpeed
};

/** @brief Where "the car is moving" comes from, for false-start detection. */
enum RaceTimeMotionSource : uint8_t {
  RT_MOTION_NONE = 0,        ///< False-start detection disabled
  RT_MOTION_VSS = 1,         ///< `currentStatus.vss`
  RT_MOTION_ARM_PIN = 2,     ///< The arm input itself (a bump-box trigger, for example)
};

/** @brief The state machine state. Reported to TunerStudio so the log explains what happened. */
enum RaceTimeState : uint8_t {
  RT_STATE_OFF = 0,      ///< Disabled, or not yet armed
  RT_STATE_ARMED = 1,    ///< Armed, waiting for the start release
  RT_STATE_RUNNING = 2,  ///< Clock is advancing
  RT_STATE_STOPPED = 3,  ///< Frozen; the final time is in @ref raceTimeMs
  RT_STATE_FALSE_START = 4, ///< Movement detected before the start release; run is invalid
};

/**
 * @brief Persistent settings for the race-time framework.
 *
 * Plain fixed-width fields, no bit-fields: this struct is a raw TunerStudio page, so its
 * offsets are part of the tuning-file ABI and must be obvious by inspection. The
 * `static_assert`s below pin every offset, so a mismatch is a build failure rather than
 * silently wrong values in the tuning software.
 *
 * Values are stored in the units the tuner types. The clamps in raceTime.cpp are the authority
 * on range, because a stored block can be written by an older build or hand-edited.
 */
struct raceTimeConfig : public config_page_t {
  uint8_t enable;           ///< @ref RaceTimeEnable. 0 = the whole framework is inactive.
  uint8_t armPin;           ///< Input pin for arm and (by default) start release. 0 = none.
  uint8_t armPolarity;      ///< 0 = asserted HIGH, 1 = asserted LOW (active-low switches are common).
  uint8_t armPullup;        ///< 0 = plain INPUT, 1 = INPUT_PULLUP.
  uint8_t startSource;      ///< @ref RaceTimeStartSource
  uint8_t stopSource;       ///< @ref RaceTimeStopSource
  uint8_t motionSource;     ///< @ref RaceTimeMotionSource, for false-start detection
  uint8_t curveEnable;      ///< Non-zero enables the retard-vs-race-time consumer.

  uint16_t stopTimeMs;      ///< Stop threshold for @ref RT_STOP_TIME, ms.
  uint16_t stopSpeed;       ///< Stop threshold for @ref RT_STOP_SPEED, in MPH.
  uint16_t falseStartSpeed; ///< Movement above this, in MPH, before the start release is a false start.
  uint16_t reserved00;      ///< Reserved, must be 0.

  uint8_t curveLabel[RACETIME_LABEL_LEN]; ///< User-editable axis label, NUL terminated.

  /// Retard curve breakpoints, ms. Must be strictly ascending - a non-monotonic axis is what
  /// makes a curve unreadable in the tuning software and unpredictable in the lookup.
  uint16_t curveTimeMs[RACETIME_CURVE_POINTS];

  /// Retard in whole degrees at each breakpoint, 0..60.
  uint8_t curveRetard[RACETIME_CURVE_POINTS];

  uint8_t reserved1[RACETIME_PAYLOAD_SIZE - 84U]; ///< Reserved, must be 0.
};

/** @brief `raceTimeConfig::enable` values. */
enum RaceTimeEnable : uint8_t {
  RT_DISABLED = 0, ///< State machine held in @ref RT_STATE_OFF, clock stays 0
  RT_ENABLED = 1,  ///< Normal operation
};

// ---- Layout contract ------------------------------------------------------------------
// TunerStudio addresses this payload as (page 17, byte offset). The offsets are declared in
// reference/speeduino.ini and MUST match offsetof() here exactly - a mismatch produces no
// error at all, just wrong values, which is the worst possible failure mode.
//
//   enable                 1 B  @   0
//   armPin                 1 B  @   1
//   armPolarity            1 B  @   2
//   armPullup              1 B  @   3
//   startSource            1 B  @   4
//   stopSource             1 B  @   5
//   motionSource           1 B  @   6
//   curveEnable            1 B  @   7
//   stopTimeMs             2 B  @   8
//   stopSpeed              2 B  @  10
//   falseStartSpeed        2 B  @  12
//   reserved00             2 B  @  14
//   curveLabel            20 B  @  16
//   curveTimeMs           32 B  @  36   (16 x uint16)
//   curveRetard           16 B  @  68   (16 x uint8)
//   reserved1            172 B  @  84
//                              -------
//   payload              256 B
static_assert(offsetof(raceTimeConfig, enable) == 0,    "raceTimeConfig.enable must be at offset 0");
static_assert(offsetof(raceTimeConfig, armPin) == 1,    "raceTimeConfig.armPin must be at offset 1");
static_assert(offsetof(raceTimeConfig, armPolarity) == 2,"raceTimeConfig.armPolarity must be at offset 2");
static_assert(offsetof(raceTimeConfig, armPullup) == 3, "raceTimeConfig.armPullup must be at offset 3");
static_assert(offsetof(raceTimeConfig, startSource) == 4,"raceTimeConfig.startSource must be at offset 4");
static_assert(offsetof(raceTimeConfig, stopSource) == 5, "raceTimeConfig.stopSource must be at offset 5");
static_assert(offsetof(raceTimeConfig, motionSource) == 6,"raceTimeConfig.motionSource must be at offset 6");
static_assert(offsetof(raceTimeConfig, curveEnable) == 7,"raceTimeConfig.curveEnable must be at offset 7");
static_assert(offsetof(raceTimeConfig, stopTimeMs) == 8,"raceTimeConfig.stopTimeMs must be at offset 8");
static_assert(offsetof(raceTimeConfig, stopSpeed) == 10,"raceTimeConfig.stopSpeed must be at offset 10");
static_assert(offsetof(raceTimeConfig, falseStartSpeed) == 12,"raceTimeConfig.falseStartSpeed must be at offset 12");
static_assert(offsetof(raceTimeConfig, reserved00) == 14,"raceTimeConfig.reserved00 must be at offset 14");
static_assert(offsetof(raceTimeConfig, curveLabel) == 16,"raceTimeConfig.curveLabel must be at offset 16");
static_assert(offsetof(raceTimeConfig, curveTimeMs) == 36,"raceTimeConfig.curveTimeMs must be at offset 36");
static_assert(offsetof(raceTimeConfig, curveRetard) == 68,"raceTimeConfig.curveRetard must be at offset 68");
static_assert(offsetof(raceTimeConfig, reserved1) == 84,"raceTimeConfig.reserved1 must be at offset 84");
static_assert(sizeof(raceTimeConfig) == RACETIME_PAYLOAD_SIZE,
              "raceTimeConfig size changed - update pageSize in speeduino.ini");

/** @brief The page payload is the config struct itself.
 *
 * Deliberately not a wrapper struct deriving from config_page_t: a nested empty base defeats
 * the empty-base optimisation, the compiler reserves a byte for it, and every field after it
 * lands one byte late relative to what TunerStudio reads. `config2`..`config15` all follow the
 * same pattern - the page struct IS the settings struct - and the offsets above are the
 * contract either way.
 */
using raceTimeBlock = raceTimeConfig;

/** @brief Total size of the custom block including its header. */
constexpr uint16_t RACETIME_BLOCK_SIZE = EEPROM_RACETIME_HEADER_SIZE + RACETIME_PAYLOAD_SIZE;

// The emulated EEPROM is 23,808 bytes (0x0000..0x5CFF), see AGENTS.md 2.1. The block must sit
// entirely inside it, above the frozen 0x0000-0x0FFF core tune ABI, and must not overlap the
// long-term-trim block that ends at 0x181F.
static_assert((uint32_t)EEPROM_RACETIME_BASE >= 0x1000U, "Custom data must live above the core tune block");
static_assert((uint32_t)EEPROM_RACETIME_BASE >= (0x1000U + EEPROM_RACETIME_HEADER_SIZE + 2064U),
              "Race-time block must start after the long-term-trim block");
static_assert(((uint32_t)EEPROM_RACETIME_BASE + (uint32_t)RACETIME_BLOCK_SIZE) <= 0x5D00U,
              "Race-time block does not fit in the emulated EEPROM address space");

/** @brief The live page payload.
 *
 * A global rather than a file-static so that pages.cpp can take its address in a
 * `constexpr` page map, exactly as it does for every config page. The page layer needs the
 * address at compile time; a function call cannot provide that.
 */
extern raceTimeBlock raceTimeData;

// ============================== Runtime state ==============================

/**
 * @brief Advance the state machine and the clock by one millisecond.
 *
 * Call exactly once per 1 ms, from the @ref BIT_TIMER_1KHZ branch of the main loop. Calling it
 * at any other rate makes @ref raceTimeMs a lie, which every consumer then inherits.
 *
 * @param current  The live status, for VSS (false start and @ref RT_STOP_SPEED)
 * @param pinHigh  The arm input's asserted state, already polarity-corrected. Tests pass this
 *                 directly; firmware uses @ref raceTimeUpdate.
 */
void raceTimeTick1ms(statuses &current, bool pinHigh);

/** @brief Convenience wrapper that samples the arm pin and then ticks.
 *
 * @param current The live status.
 */
void raceTimeUpdate(statuses &current);

/** @brief Load the block at boot and bind the arm pin. */
void initialiseRaceTime(statuses &current);

/** @brief Restore defaults in RAM only. Called by setTuneToEmpty(). */
void raceTimeRestoreDefaults(void);

/** @brief The elapsed race time, in milliseconds. Zero when the clock is not running. */
uint32_t raceTimeGetMs(void);

/** @brief The state machine state, for logging and for consumers that need to know. */
RaceTimeState raceTimeGetState(void);

/** @brief True while the clock is advancing. */
bool raceTimeIsRunning(void);

/** @brief True if the last completed run was invalidated by a false start. */
bool raceTimeIsFalseStart(void);

/** @brief Force the clock back to zero and the state back to Off. */
void raceTimeReset(void);

/**
 * @brief Ask for an externally-triggered start.
 *
 * The @ref RT_START_EXTERNAL start source exists so a future consumer (a bump box, a CAN
 * message, a programmable-output edge) can start the clock without the firmware needing a new
 * start-source enum. Not reachable from TunerStudio, deliberately: the tuner-facing sources are
 * the two physical ones.
 */
void raceTimeTriggerStart(void);

/**
 * @brief Retard from the retard-vs-race-time curve at the current race time.
 *
 * This is the reference consumer. It exists to prove the clock is usable, and to give the
 * tuner something to see on the first pass rather than a channel that only ever reads zero.
 *
 * @return Retard in whole degrees, 0 when disabled, not running, or the curve is flat.
 */
uint8_t raceTimeCurveRetard(void);

/// @name Test and TunerStudio hooks
/// @{
/** @brief Mutable access to the page payload, for tests and TunerStudio writes. */
raceTimeBlock &raceTimeGetPageMutable(void);

/** @brief Read-only access to the page payload. */
const raceTimeBlock &raceTimeGetPage(void);

/** @brief True if the persisted block was found intact (magic + version + CRC) at boot. */
bool raceTimeIsInitialised(void);

/** @brief True if the storage backend is large enough to hold the block. */
bool raceTimeIsAvailable(void);

/** @brief True if the settings have changed since the last persist. */
bool raceTimeIsDirty(void);

/** @brief Mark the settings dirty so the next stopped-engine poll persists them. */
void raceTimeMarkDirty(void);

/** @brief Force a persist now, regardless of RPM. Used by an explicit TS burn. */
bool raceTimeSave(void);

/**
 * @brief Persist the block, honouring the storage write budget. Called from savePage().
 *
 * This is the *only* correct way for the page-save machinery to persist the block, because the
 * burn loop depends on the budget being reported back accurately. `savePage()` clears the
 * "burn pending" flag when the budget reaches zero, and `speeduino.ino()` re-enters
 * `saveAllPages()` for as long as that flag stays set. A page save that ignores the budget and
 * always reports success makes the loop run forever - a flash-destroying bug that is invisible
 * in a unit test that only checks the data round-trips. See @ref lttSaveBlock.
 *
 * @param writesRemaining Write budget for this pass.
 * @return Writes still remaining. Zero means this page is finished.
 */
uint16_t raceTimeSaveBlock(uint16_t writesRemaining);

/** @brief True if the block differs from what is stored. */
bool raceTimeNeedsSave(void);
/// @}

#if defined(UNIT_TEST)
/** @brief Reset the module to its power-on state. Unit tests only. */
void raceTimeResetForTest(void);
#endif
