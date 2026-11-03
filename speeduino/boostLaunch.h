#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config_pages.h"
#include "load_source.h"

/**
 * @file
 * @brief Launch boost target: a base-target curve versus time, for the boost menu.
 *
 * @section why Why this is its own page and its own module
 *
 * Boost is a separate job from launch. Tying the two together would mean a tuner experimenting
 * with launch timing could not help but disturb their boost strategy, and the launch curve would
 * be buried in a menu nobody looking for boost would open. So this lives under Boost, with its
 * own page (20), its own EEPROM block and its own CRC header, exactly like every other feature
 * that owns persisted state.
 *
 * @section layering What "base target" means - the important part
 *
 * This module supplies a **base target**, never a final one. The boost controller's chain is:
 *
 *     base target  (this module while the launch window is open, otherwise boostTable)
 *        + flexBoostCorrection      <- flex fuel ethanol adder
 *        + any future target modifier <- traction control, scramble, boost-by-speed...
 *        = currentStatus.boostTarget
 *        -> PID + feed-forward -> duty
 *        - duty trim (launch / slip)
 *        = currentStatus.boostDuty
 *
 * The distinction matters. If this module overrode `currentStatus.boostTarget` after the
 * modifiers had run, launch boost would silently disable flex-fuel compensation and every future
 * traction feature - the tuner would see a boost target that ignored half their tune, with
 * nothing in the log explaining why. Substituting the BASE and letting the existing modifiers
 * stack on top is the only formulation that composes.
 *
 * @section window When it is active
 *
 * While `enable` is set and the launch state machine is between the release edge and the end of
 * the time axis, the base target follows the curve. Before and after that window - and whenever
 * the toggle is off - this module contributes nothing at all: `boostLaunchIsActive()` returns
 * false and the boost controller behaves exactly as if the feature did not exist. There is no
 * "hold the last value" state, because a car that has finished launching should be on its normal
 * boost strategy, not stuck on a launch number.
 *
 * @section reserved Room to grow
 *
 * The payload is 1024 bytes: 512 used, 512 reserved. The next round of work on this page adds a
 * full set of boost control options (per-gear targets, boost-by-speed, spool/scramble, PID
 * shaping, dome control), and none of it should have to move an existing offset or bump a page
 * size. Growing into reserved space costs nothing and does not move the fields above it.
 */

// =====================================================================================
// Block identity
// =====================================================================================

/** @brief Base address of the launch-boost block. Immediately after the two launch blocks. */
constexpr uint16_t EEPROM_BOOSTLAUNCH_BASE = 0x1F60;

/** @brief 'BLCH'. Detects "never initialised" against "corrupt". */
constexpr uint32_t EEPROM_BOOSTLAUNCH_MAGIC = 0x424C4348;

/**
 * @brief Layout version. Bump whenever the payload layout, a field width, or a field's UNIT
 * changes. The CRC validates integrity, not meaning - see the note in ppLaunch.h.
 */
constexpr uint8_t EEPROM_BOOSTLAUNCH_VERSION = 1;

/** @brief Size of the block header, in bytes. Must stay a multiple of 2 for alignment. */
constexpr uint16_t EEPROM_BOOSTLAUNCH_HEADER_SIZE = 16;

/** @brief Number of points on the launch boost target curve. */
constexpr uint8_t BL_CURVE_POINTS = 32;

/** @brief Payload size: 1024 bytes, of which half is reserved for the boost overhaul.
 *
 * Deliberately twice a normal page. This page is the future home of the full boost-control
 * feature set, and reserving the space now is free - it is inside the ~17 kB of EEPROM that
 * nothing else is using.
 */
constexpr uint16_t BOOSTLAUNCH_PAYLOAD_SIZE = 1024;

/** @brief Total size of the block including its header. */
constexpr uint16_t BOOSTLAUNCH_BLOCK_SIZE = EEPROM_BOOSTLAUNCH_HEADER_SIZE + BOOSTLAUNCH_PAYLOAD_SIZE;

/** @brief EEPROM address of the TunerStudio-visible payload (immediately after the header). */
constexpr uint16_t EEPROM_BOOSTLAUNCH_PAYLOAD =
    EEPROM_BOOSTLAUNCH_BASE + EEPROM_BOOSTLAUNCH_HEADER_SIZE;

/** @brief Highest boost target representable, kPa absolute.
 *
 * Matches the clamp in `getBoostTarget()` (boostController.cpp), which limits
 * `currentStatus.boostTarget` to 0..511. A target above this would silently do nothing. */
constexpr uint16_t BL_TARGET_MAX_KPA = 511U;

/**
 * @brief Persistent settings for the launch boost target.
 *
 * Plain fixed-width fields, no bit-fields: this struct IS a raw TunerStudio page, so its offsets
 * are part of the tuning-file ABI.
 */
struct boostLaunchConfig : public config_page_t {
  // ---- 0..31: scalars -----------------------------------------------------------------
  uint8_t enable;              ///< 0 = off (module contributes nothing), 1 = on
  uint8_t reserved00;          ///< Reserved, must be 0.
  uint16_t reserved01;         ///< Reserved, must be 0.

  uint16_t rampInMs;           ///< Reserved for the boost overhaul; unused today.
  uint16_t rampOutMs;          ///< Reserved for the boost overhaul; unused today.

  uint8_t label[20];           ///< Editable axis label, NUL terminated ("launch boost")

  uint8_t reserved02[4];       ///< Reserved, must be 0. Keeps the tables 2-byte aligned.

  // ---- 32..95: time axis --------------------------------------------------------------
  /// Milliseconds since the launch release. Forced to start at 0 and strictly ascending by
  /// clampBoostSettings(); a non-monotonic axis makes the curve unreadable and the lookup
  /// unpredictable.
  uint16_t curveTime[BL_CURVE_POINTS];

  // ---- 96..159: boost target ----------------------------------------------------------
  /// Base boost target at each point, kPa absolute.
  ///
  /// This is a BASE, not a final target: flex-fuel compensation, traction control and any future
  /// modifier are added on top of it. A value of 0 means "no override at this point", which
  /// leaves the tune's normal target in place - never "command zero boost", which would stall
  /// the car because a tuner left a row blank.
  uint16_t curveTarget[BL_CURVE_POINTS];

  // ---- 160..191: launch duty trim -----------------------------------------------------
  /// Optional duty trim at each point, percent (0..100). Zero everywhere leaves duty alone.
  ///
  /// Separate from the target because they act in different places: the target moves the PID
  /// setpoint, the trim scales the solenoid output. A car that overboosts on the hit is usually
  /// better fixed with trim than with a lower target, because the target also moves the
  /// feed-forward table.
  uint8_t curveDutyTrim[BL_CURVE_POINTS];

  // ---- 192..1023: reserved for the full boost control overhaul -------------------------
  /// 832 bytes. This is where per-gear targets, boost-by-speed, spool/scramble, PID shaping and
  /// dome control will go. Reserved rather than omitted so that adding them moves nothing.
  uint8_t reserved03[BOOSTLAUNCH_PAYLOAD_SIZE - 192U];
};

using boostLaunchBlock = boostLaunchConfig;

/** @brief `boostLaunchConfig::enable` values. */
enum BoostLaunchEnable : uint8_t {
  BL_DISABLED = 0,
  BL_ENABLED = 1,
};

// ---- Layout contract ------------------------------------------------------------------
// TunerStudio addresses this payload as (page 20, byte offset). The offsets below are asserted,
// not trusted: a single added field shifts everything after it and TunerStudio reads by absolute
// offset, so the failure mode is silent wrong values.
//
//   enable                 1 B  @   0      curveTarget           64 B  @  96
//   reserved00             1 B  @   1      curveDutyTrim         32 B  @ 160
//   reserved01             2 B  @   2      reserved03           832 B  @ 192
//   rampInMs               2 B  @   4                                  -------
//   rampOutMs              2 B  @   6      payload             1024 B
//   label                 20 B  @   8
//   reserved02             4 B  @  28
//   curveTime             64 B  @  32
static_assert(offsetof(boostLaunchConfig, enable) == 0, "enable must be at offset 0");
static_assert(offsetof(boostLaunchConfig, reserved00) == 1, "reserved00 must be at offset 1");
static_assert(offsetof(boostLaunchConfig, reserved01) == 2, "reserved01 must be at offset 2");
static_assert(offsetof(boostLaunchConfig, rampInMs) == 4, "rampInMs must be at offset 4");
static_assert(offsetof(boostLaunchConfig, rampOutMs) == 6, "rampOutMs must be at offset 6");
static_assert(offsetof(boostLaunchConfig, label) == 8, "label must be at offset 8");
static_assert(offsetof(boostLaunchConfig, reserved02) == 28, "reserved02 must be at offset 28");
static_assert(offsetof(boostLaunchConfig, curveTime) == 32, "curveTime must be at offset 32");
static_assert(offsetof(boostLaunchConfig, curveTarget) == 96, "curveTarget must be at offset 96");
static_assert(offsetof(boostLaunchConfig, curveDutyTrim) == 160, "curveDutyTrim must be at offset 160");
static_assert(offsetof(boostLaunchConfig, reserved03) == 192, "reserved03 must be at offset 192");
static_assert(sizeof(boostLaunchConfig) == BOOSTLAUNCH_PAYLOAD_SIZE,
              "boostLaunchConfig size changed - update pageSize in speeduino.ini");

// Block placement: inside the 23,808-byte emulated EEPROM (0x0000..0x5CFF), above the frozen
// 0x0000-0x0FFF core tune ABI, clear of LTT (0x1000..0x181F), race time (0x1820..0x192F) and the
// three launch blocks (0x1930..0x1F5F).
static_assert(EEPROM_BOOSTLAUNCH_BASE >= (0x1930U + 2U * 528U),   // = 0x1D50
              "Launch-boost block must start after both launch blocks");
static_assert(((uint32_t)EEPROM_BOOSTLAUNCH_BASE + (uint32_t)BOOSTLAUNCH_BLOCK_SIZE) <= 0x5D00U,
              "Launch-boost block does not fit in the emulated EEPROM address space");

/** @brief The live page payload.
 *
 * A global rather than a file-static so that pages.cpp can take its address in a `constexpr`
 * page map, exactly as it does for every config page.
 */
extern boostLaunchBlock boostLaunchData;

// =====================================================================================
// Runtime API
// =====================================================================================

/** @brief Load the block at boot, defaulting it if missing, stale or corrupt. */
void initialiseBoostLaunch(void);

/** @brief Restore defaults in RAM only. Called by setTuneToEmpty(). */
void boostLaunchRestoreDefaults(void);

/**
 * @brief Evaluate the launch boost curve. Call every main-loop pass.
 *
 * Cheap and safe: returns immediately when the feature is disabled or the launch window is
 * closed.
 *
 * @param current The live status (for the launch state).
 */
void boostLaunchUpdate(const statuses &current);

/**
 * @brief The base boost target this module wants, kPa absolute.
 *
 * Only meaningful when @ref boostLaunchIsActive() is true. The caller must ADD its target
 * modifiers (flex, traction, ...) on top of this rather than treating it as final.
 */
uint16_t boostLaunchBaseTarget(void);

/** @brief The duty trim this module wants, percent 0..100. Applied after the PID. */
uint8_t boostLaunchDutyTrim(void);

/**
 * @brief True while this module should supply the base target.
 *
 * False when the toggle is off, when launch is not running, or when the time axis has been
 * passed. When false the caller must use its normal target: this module contributes nothing,
 * there is no "hold the last value" state.
 */
bool boostLaunchIsActive(void);

/** @brief True if the persisted block was found intact (magic + version + CRC) at boot. */
bool boostLaunchIsInitialised(void);

/** @brief True if the storage backend is large enough to hold the block. */
bool boostLaunchIsAvailable(void);

/** @brief Mark the settings dirty so the next stopped-engine poll persists them. */
void boostLaunchMarkDirty(void);

/** @brief Force a persist now, regardless of RPM. Used by an explicit TS burn. */
bool boostLaunchSave(void);

/**
 * @brief Persist the block, honouring the storage write budget. Called from savePage().
 *
 * Must report the budget back accurately - see the warning on @ref ppLaunchSaveBlock.
 *
 * @return Writes still remaining. Zero means this page is finished.
 */
uint16_t boostLaunchSaveBlock(uint16_t writesRemaining);

/** @brief True if the block differs from what is stored. */
bool boostLaunchNeedsSave(void);

/** @brief Mutable access to the page payload, for tests and TunerStudio writes. */
boostLaunchBlock &boostLaunchGetPageMutable(void);

/** @brief Read-only access to the page payload. */
const boostLaunchBlock &boostLaunchGetPage(void);

/** @brief The curve value the last evaluation used, kPa. 0 when inactive. For logging. */
uint16_t boostLaunchTargetLog(void);

#if defined(UNIT_TEST)
/** @brief Reset the module to its power-on state. Unit tests only. */
void boostLaunchResetForTest(void);
#endif
