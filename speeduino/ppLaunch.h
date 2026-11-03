#pragma once
#include <stdint.h>
#include <stddef.h>
#include "config_pages.h"
#include "load_source.h"

/**
 * @file
 * @brief Launch control: two independent modes, deliberately of different complexity.
 *
 * This is NOT the classic 2-step. That lives on pages 6 and 10
 * (`src/controllers/launch/launchController.cpp`) and is untouched by this module: it answers
 * "hold the engine at X RPM until the clutch is released". This module answers
 * "the car should be at Y mph at T milliseconds after the hit; how far is it from that, and what
 * should we do about it".
 *
 * @section two-modes Two modes, two pages, two levels of ambition
 *
 * Both modes get a **whole page to themselves** (18 and 19) so that switching between them never
 * costs a tuner the table they already built. Memory is the cheap resource here: 512 bytes per
 * page against ~17 kB of free EEPROM, on an ECU whose entire purpose is drag racing.
 *
 * But they are NOT the same feature twice, and the pages deliberately have different shapes:
 *
 *  - **Speed mode (page 18)** is the full reference-trajectory controller. Every point carries
 *    time, a reference speed, retard, an RPM limit and a boost duty trim. The car is compared
 *    against the reference "wire" and the deviation drives the correction curves.
 *  - **RPM mode (page 19)** is the simple one, and it is simple on purpose: an **RPM limit versus
 *    time**, a **soft timing retard** that engages at a hysteresis band below that limit, and a
 *    choice of **power cut method**. There is no reference wire, no slip scoring, no boost curve
 *    and no gear gate on this page. It is the thing you reach for when you want a believable
 *    launch limiter that follows a time ramp rather than a flat value.
 *
 * The boost *target* curve is deliberately not here at all - see @ref boostLaunch.h. Boost is a
 * separate menu with a separate page because it is a separate job, and because tying the two
 * together would mean a tuner experimenting with launch timing could not help but disturb their
 * boost strategy.
 *
 * @section contract Design contract
 *
 *  1. **Every clock advances only at 1 kHz.** The elapsed time is incremented in
 *     @ref ppLaunchTick1ms and nowhere else, so it is a real millisecond base and not a function
 *     of loop cadence. It deliberately does NOT read `currentStatus.raceTimeMs`: that clock is
 *     gated on the race-time feature's own `enable` flag and is held at zero when FND-05 is
 *     switched off, so borrowing it would make launch control silently dead for any tuner who
 *     never enabled the race timer.
 *  2. **Nothing is applied unless the state machine says so.** Limits and retard are zero in
 *     every state except LAUNCHING. A disabled or completed module must never leave timing on
 *     the table.
 *  3. **Retard leaves through the torque bus.** It is published as @c TorqueProducer::Launch so
 *     the arbiter's "largest single request wins" rule prevents it summing with the race-time
 *     curve or a future traction controller. Writing `currentStatus.advance` directly is what
 *     makes two retard features add up to negative advance at the top of a pass.
 *  4. **A stale or half-written block is discarded, never trusted.** Payload first, header (with
 *     the CRC) last - identical to `ltt` and `raceTime`.
 *  5. **Every lookup axis is repaired, never assumed monotonic.** A hand-edited or truncated
 *     block can hold a non-monotonic axis, and the interpolation bakes that assumption in.
 */

// =====================================================================================
// Block identity
// =====================================================================================

/** @brief Base address of the speed-mode block. First free address above the race-time block. */
constexpr uint16_t EEPROM_PPLAUNCH_SPEED_BASE = 0x1930;

/** @brief Base address of the RPM-mode block, immediately after the speed-mode block. */
constexpr uint16_t EEPROM_PPLAUNCH_RPM_BASE = 0x1B40;

/** @brief 'PPLS' - speed mode. Detects "never initialised" against "corrupt". */
constexpr uint32_t EEPROM_PPLAUNCH_SPEED_MAGIC = 0x504C5053;

/** @brief 'PPLR' - RPM mode. A separate magic so a block copied between pages is rejected
 *  rather than silently reinterpreted with the other mode's meaning. */
constexpr uint32_t EEPROM_PPLAUNCH_RPM_MAGIC = 0x504C5052;

/**
 * @brief Layout version, shared by both pages. Bump whenever either payload's layout, a field
 * width, or a field's UNIT changes.
 *
 * The CRC validates integrity, not meaning: a block written by an older build with the same
 * payload size will pass both the size assertions and its own CRC, and only the *meaning* will
 * be wrong. The version byte is the only thing that distinguishes "older build" from "corrupt".
 */
constexpr uint8_t EEPROM_PPLAUNCH_VERSION = 1;

/** @brief Size of the block header, in bytes. Must stay a multiple of 2 for alignment. */
constexpr uint16_t EEPROM_PPLAUNCH_HEADER_SIZE = 16;

/** @brief Number of points on the speed-mode curve. */
constexpr uint8_t PP_CURVE_POINTS = 32;

/** @brief Number of points on the RPM-mode limit-vs-time curve. */
constexpr uint8_t PP_RPM_CURVE_POINTS = 32;

/** @brief Number of points on the speed-mode slip-correction curve. Shorter on purpose: slip
 *  correction is a response, not a shape, and 8 breakpoints is more than any tuner will use. */
constexpr uint8_t PP_SLIP_POINTS = 8;

/** @brief Size of the user-editable curve label, including its NUL terminator.
 *
 * Matches `prgm_out00Alias` / `curveLabel` so TunerStudio renders it identically. */
constexpr uint8_t PP_LABEL_LEN = 20;

/** @brief Payload size, shared by both pages.
 *
 * 512 B is a whole TunerStudio page. The speed page uses most of it for its five 32-point tables;
 * the RPM page uses a fraction and reserves the rest, so that both pages stay the same size and a
 * future addition to the simple page costs nothing.
 */
constexpr uint16_t PPLAUNCH_PAYLOAD_SIZE = 512;

/** @brief Total size of one block including its header. */
constexpr uint16_t PPLAUNCH_BLOCK_SIZE = EEPROM_PPLAUNCH_HEADER_SIZE + PPLAUNCH_PAYLOAD_SIZE;

/** @brief EEPROM address of each mode's TunerStudio-visible payload. */
constexpr uint16_t EEPROM_PPLAUNCH_SPEED_PAYLOAD =
    EEPROM_PPLAUNCH_SPEED_BASE + EEPROM_PPLAUNCH_HEADER_SIZE;
constexpr uint16_t EEPROM_PPLAUNCH_RPM_PAYLOAD =
    EEPROM_PPLAUNCH_RPM_BASE + EEPROM_PPLAUNCH_HEADER_SIZE;

/** @brief Miles per hour to kilometres per hour, as a fraction (1 mph = 1.609344 km/h exactly).
 *
 * Duplicated from raceTime.h rather than shared, so that this module has no dependency on that
 * one - they are independent features that happen to use the same conversion. The 32-bit
 * numerator is mandatory: 160934 truncates to 29862 in a uint16_t, and a 10 mph threshold then
 * becomes 2 km/h. That compiles cleanly and is simply wrong.
 */
constexpr uint32_t PP_MPH_TO_KMH_NUM = 160934U;
constexpr uint32_t PP_MPH_TO_KMH_DEN = 100000U;
static_assert(PP_MPH_TO_KMH_NUM > 65535U,
              "PP_MPH_TO_KMH_NUM must be 32-bit; a 16-bit type truncates it silently");

// =====================================================================================
// Enumerations
// =====================================================================================

/** @brief Which physical quantity drives the speed-mode axis. */
enum PpAxisSource : uint8_t {
  PP_AXIS_VSS_MPH10 = 0, ///< `currentStatus.vss` converted to mph x10 (tenths of a mph)
  PP_AXIS_VSS_KPH10 = 1, ///< `currentStatus.vss` x10, i.e. tenths of a km/h
  PP_AXIS_SHAFT_RPM = 2, ///< Driveshaft RPM, derived from vss x @ref ppLaunchSpeedConfig::shaftRevPerKph
  PP_AXIS_RPM = 3,       ///< Engine RPM, if the tuner wants the speed PAGE indexed by RPM
};

/** @brief Which limiter is used while launching.
 *
 * Ordered to match `PROTECT_CUT_*` where the meanings overlap, so the channel-cut machinery can
 * be reused, but stored per page so a launch limiter can cut differently from the general
 * engine-protection limiter. */
enum PpLimitMethod : uint8_t {
  PP_LIMIT_NONE = 0,   ///< No rev limiter; retard only
  PP_LIMIT_SPARK = 1,  ///< Spark cut
  PP_LIMIT_FUEL = 2,   ///< Fuel cut
  PP_LIMIT_BOTH = 3,   ///< Spark and fuel cut
  PP_LIMIT_ROLLING = 4,///< Rolling (probabilistic, progressive) cut - smoothest for spooling a turbo
};

/** @brief What the X axis of the speed-mode curve is. */
enum PpCurveMode : uint8_t {
  PP_CURVE_TIME = 0, ///< Look up by elapsed milliseconds. `curveAxis` is then the reference wire.
  PP_CURVE_AXIS = 1, ///< Look up by the live axis value. `curveTime` is unused for lookup.
};

/// @name `ppLaunchSpeedConfig::featureFlags` bits
/// @{
constexpr uint8_t PP_FLAG_PERFECT_PASS = 0x01U; ///< Use `curveAxis` as a reference wire and score deviation
constexpr uint8_t PP_FLAG_RETARD_SLEW = 0x02U;  ///< Rate-limit retard changes
constexpr uint8_t PP_FLAG_SLIP_CORRECT = 0x04U; ///< Apply the slip-correction curve
constexpr uint8_t PP_FLAG_GEAR_GATE = 0x08U;    ///< Only launch in gears up to `maxGear`
constexpr uint8_t PP_FLAG_AXIS_CLAMP = 0x10U;   ///< Slew-limit the axis so one noise spike cannot cross every bin
constexpr uint8_t PP_FLAG_DUTY_TRIM = 0x20U;    ///< Apply `curveBoostTrim` and the slip duty trim
/// @}

/** @brief The state machine state. Reported to TunerStudio so the log explains what happened -
 *  the single biggest usability gap in competing speed-based launch implementations, where the
 *  tuner cannot see *why* the limiter engaged. */
enum PpState : uint8_t {
  PP_STATE_OFF = 0,       ///< Feature disabled
  PP_STATE_IDLE = 1,      ///< Enabled, waiting for the arm input
  PP_STATE_ARMED = 2,     ///< Armed, clock at zero, waiting for the release edge
  PP_STATE_LAUNCHING = 3, ///< Released; clock running, limits and retard live
  PP_STATE_COMPLETE = 4,  ///< Past the end of the curve; output returned to normal
  PP_STATE_TIMEOUT = 5,   ///< Armed for longer than `timeoutMs` without a launch
};

/** @brief The two pages this module owns, as a selector. */
enum PpMode : uint8_t {
  PP_MODE_SPEED = 0, ///< Page 18
  PP_MODE_RPM = 1,   ///< Page 19
};

// =====================================================================================
// Page 18: the speed-based reference-trajectory controller
// =====================================================================================

/**
 * @brief Persistent settings for the speed-based launch mode.
 *
 * Plain fixed-width fields, no bit-fields. This struct IS a raw TunerStudio page, so its offsets
 * are part of the tuning-file ABI and must be obvious by inspection; the `static_assert`s below
 * pin every one of them. A mismatch produces no error at all - just wrong values, which is the
 * worst failure mode available.
 */
struct ppLaunchSpeedConfig : public config_page_t {
  // ---- 0..31: scalars -----------------------------------------------------------------
  uint8_t enable;           ///< 0 = off, 1 = on.
  uint8_t armPin;           ///< Input pin for the arm / launch switch. 0 = none configured.
  uint8_t armPolarity;      ///< 0 = asserted HIGH, 1 = asserted LOW (active-low switches are common)
  uint8_t armPullup;        ///< 0 = plain INPUT, 1 = INPUT_PULLUP
  uint8_t axisSource;       ///< @ref PpAxisSource
  uint8_t limitMethod;      ///< @ref PpLimitMethod
  uint8_t curveMode;        ///< @ref PpCurveMode
  uint8_t featureFlags;     ///< @ref PP_FLAG_* bitmask

  uint16_t timeoutMs;       ///< Armed timeout, ms. 0 = never time out. Max 65535 ms (65.5 s).
  uint16_t rearmBelowAxis;  ///< Re-arm once the axis drops below this. 0 = re-arm on arm pin only.
  uint16_t startBelowAxis;  ///< The limiter may only engage while the axis is below this.
  uint16_t slipDeadband;    ///< Deviation from the wire tolerated before slip correction acts.
  uint16_t axisAccelLimit;  ///< Maximum axis change per update when PP_FLAG_AXIS_CLAMP is set.
  uint16_t retardSlewTenths;///< Max retard change per SECOND (tenths of a degree).
  uint16_t shaftRevPerKph;  ///< Driveshaft revolutions per km/h x100, for PP_AXIS_SHAFT_RPM.

  uint8_t  maxGear;         ///< Highest gear the limiter may engage in. 0 = no gear gating.
  uint8_t  axisFilterShift; ///< IIR shift for the axis (0 = off, 3 = /8 smoothing)
  uint8_t  limitHysteresis; ///< Axis units of hysteresis on the limiter release, prevents chatter
  uint8_t  reserved18;      ///< Reserved, must be 0. Pads to the uint32_t below.

  uint32_t reserved0;       ///< Reserved, must be 0.

  // ---- 32..51: user label -------------------------------------------------------------
  uint8_t label[PP_LABEL_LEN]; ///< Editable curve label, NUL terminated ("speed")

  uint8_t reserved1[4];     ///< Reserved, must be 0. Keeps the tables 2-byte aligned.

  // ---- 56..119: time axis -------------------------------------------------------------
  /// Milliseconds since the launch trigger. Forced to start at 0 and strictly ascending by
  /// clampSpeedSettings(); a non-monotonic axis is what makes a curve unreadable in the software
  /// and unpredictable in the lookup.
  uint16_t curveTime[PP_CURVE_POINTS];

  // ---- 120..183: reference axis -------------------------------------------------------
  /// The reference trajectory: what the axis *should* read at `curveTime[i]`.
  ///
  /// In @ref PP_CURVE_TIME mode this is only used for slip scoring. In @ref PP_CURVE_AXIS mode it
  /// is the lookup axis and `curveTime` is unused. Never forced monotonic: a car can be expected
  /// to slow down (a lifted throttle, a bog), and forcing the wire to rise would compare the car
  /// against a fiction.
  uint16_t curveAxis[PP_CURVE_POINTS];

  // ---- 184..247: retard ---------------------------------------------------------------
  /// Ignition retard at each point, in TENTHS of a degree (0..600 = 0.0..60.0).
  uint16_t curveRetard[PP_CURVE_POINTS];

  // ---- 248..311: RPM limit ------------------------------------------------------------
  /// RPM hard limit at each point. Whole RPM, u16 - not the `rpm/100` byte the classic 2-step
  /// uses, because 100 RPM steps are visible when you are trying to hold a launch within 50 RPM.
  /// 0 disables the limiter at that point.
  uint16_t curveLimit[PP_CURVE_POINTS];

  // ---- 312..375: boost duty trim ------------------------------------------------------
  /// Boost solenoid duty reduction at each point, percent (0..100).
  ///
  /// This is a DUTY TRIM, not a target: the boost *target* curve lives on page 20 with the rest of
  /// boost control. Keeping the trim here means the launch page can still pull boost back where
  /// the car is making too much, without owning the boost strategy.
  uint8_t curveBoostTrim[PP_CURVE_POINTS];

  // ---- 344..375: reserved -------------------------------------------------------------
  uint8_t reserved2[32];

  // ---- 376..415: slip correction ------------------------------------------------------
  /// Slip breakpoints, in axis units of deviation from the reference wire.
  uint16_t slipBin[PP_SLIP_POINTS];
  /// Additional retard per slip bin, tenths of a degree. Additive with @ref curveRetard.
  uint16_t slipRetard[PP_SLIP_POINTS];
  /// Additional boost duty reduction per slip bin, percent. Additive with @ref curveBoostTrim.
  uint8_t slipBoostTrim[PP_SLIP_POINTS];

  // ---- 416..511: reserved -------------------------------------------------------------
  uint8_t reserved3[PPLAUNCH_PAYLOAD_SIZE - 416U];
};

/** @brief `ppLaunchSpeedConfig::enable` values. */
enum PpEnable : uint8_t {
  PP_DISABLED = 0,
  PP_ENABLED = 1,
};

// ---- Page 18 layout contract ----------------------------------------------------------
// TunerStudio addresses this payload as (page 18, byte offset). The offsets are declared in
// reference/speeduino.ini and MUST match offsetof() here exactly.
//
//   enable                 1 B  @   0      curveRetard           64 B  @ 184
//   armPin                 1 B  @   1      curveLimit            64 B  @ 248
//   armPolarity            1 B  @   2      curveBoostTrim        32 B  @ 312
//   armPullup              1 B  @   3      reserved2             32 B  @ 344
//   axisSource             1 B  @   4      slipBin               16 B  @ 376
//   limitMethod            1 B  @   5      slipRetard            16 B  @ 392
//   curveMode              1 B  @   6      slipBoostTrim          8 B  @ 408
//   featureFlags           1 B  @   7      reserved3             96 B  @ 416
//   timeoutMs              2 B  @   8                                  -------
//   rearmBelowAxis         2 B  @  10      payload              512 B
//   startBelowAxis         2 B  @  12
//   slipDeadband           2 B  @  14   (the 2-byte pad before reserved0 at 28 is what a
//   axisAccelLimit         2 B  @  16    uint32_t aligned to 4 costs; do not "fix" it, the
//   retardSlewTenths       2 B  @  18    ini offsets are written against it)
//   shaftRevPerKph         2 B  @  20
//   maxGear                1 B  @  22
//   axisFilterShift        1 B  @  23
//   limitHysteresis        1 B  @  24
//   reserved18             1 B  @  25
//   reserved0              4 B  @  28
//   label                 20 B  @  32
//   reserved1              4 B  @  52
//   curveTime             64 B  @  56   (32 x uint16, milliseconds)
//   curveAxis             64 B  @ 120   (32 x uint16, mph x10 / rpm / shaft rpm)
static_assert(offsetof(ppLaunchSpeedConfig, enable) == 0,    "enable must be at offset 0");
static_assert(offsetof(ppLaunchSpeedConfig, armPin) == 1,    "armPin must be at offset 1");
static_assert(offsetof(ppLaunchSpeedConfig, armPolarity) == 2, "armPolarity must be at offset 2");
static_assert(offsetof(ppLaunchSpeedConfig, armPullup) == 3, "armPullup must be at offset 3");
static_assert(offsetof(ppLaunchSpeedConfig, axisSource) == 4, "axisSource must be at offset 4");
static_assert(offsetof(ppLaunchSpeedConfig, limitMethod) == 5, "limitMethod must be at offset 5");
static_assert(offsetof(ppLaunchSpeedConfig, curveMode) == 6, "curveMode must be at offset 6");
static_assert(offsetof(ppLaunchSpeedConfig, featureFlags) == 7, "featureFlags must be at offset 7");
static_assert(offsetof(ppLaunchSpeedConfig, timeoutMs) == 8, "timeoutMs must be at offset 8");
static_assert(offsetof(ppLaunchSpeedConfig, rearmBelowAxis) == 10, "rearmBelowAxis must be at offset 10");
static_assert(offsetof(ppLaunchSpeedConfig, startBelowAxis) == 12, "startBelowAxis must be at offset 12");
static_assert(offsetof(ppLaunchSpeedConfig, slipDeadband) == 14, "slipDeadband must be at offset 14");
static_assert(offsetof(ppLaunchSpeedConfig, axisAccelLimit) == 16, "axisAccelLimit must be at offset 16");
static_assert(offsetof(ppLaunchSpeedConfig, retardSlewTenths) == 18, "retardSlewTenths must be at offset 18");
static_assert(offsetof(ppLaunchSpeedConfig, shaftRevPerKph) == 20, "shaftRevPerKph must be at offset 20");
static_assert(offsetof(ppLaunchSpeedConfig, maxGear) == 22, "maxGear must be at offset 22");
static_assert(offsetof(ppLaunchSpeedConfig, axisFilterShift) == 23, "axisFilterShift must be at offset 23");
static_assert(offsetof(ppLaunchSpeedConfig, limitHysteresis) == 24, "limitHysteresis must be at offset 24");
static_assert(offsetof(ppLaunchSpeedConfig, reserved0) == 28, "reserved0 must be at offset 28");
static_assert(offsetof(ppLaunchSpeedConfig, label) == 32, "label must be at offset 32");
static_assert(offsetof(ppLaunchSpeedConfig, reserved1) == 52, "reserved1 must be at offset 52");
static_assert(offsetof(ppLaunchSpeedConfig, curveTime) == 56, "curveTime must be at offset 56");
static_assert(offsetof(ppLaunchSpeedConfig, curveAxis) == 120, "curveAxis must be at offset 120");
static_assert(offsetof(ppLaunchSpeedConfig, curveRetard) == 184, "curveRetard must be at offset 184");
static_assert(offsetof(ppLaunchSpeedConfig, curveLimit) == 248, "curveLimit must be at offset 248");
static_assert(offsetof(ppLaunchSpeedConfig, curveBoostTrim) == 312, "curveBoostTrim must be at offset 312");
static_assert(offsetof(ppLaunchSpeedConfig, reserved2) == 344, "reserved2 must be at offset 344");
static_assert(offsetof(ppLaunchSpeedConfig, slipBin) == 376, "slipBin must be at offset 376");
static_assert(offsetof(ppLaunchSpeedConfig, slipRetard) == 392, "slipRetard must be at offset 392");
static_assert(offsetof(ppLaunchSpeedConfig, slipBoostTrim) == 408, "slipBoostTrim must be at offset 408");
static_assert(offsetof(ppLaunchSpeedConfig, reserved3) == 416, "reserved3 must be at offset 416");
static_assert(sizeof(ppLaunchSpeedConfig) == PPLAUNCH_PAYLOAD_SIZE,
              "ppLaunchSpeedConfig size changed - update pageSize in speeduino.ini");

/** @brief The page payload is the config struct itself.
 *
 * Deliberately not a wrapper deriving from config_page_t: a nested empty base defeats the
 * empty-base optimisation, the compiler reserves a byte for it, and every field after it lands
 * one byte late relative to what TunerStudio reads.
 */
using ppLaunchSpeedBlock = ppLaunchSpeedConfig;

// =====================================================================================
// Page 19: the simple RPM limiter
// =====================================================================================

/**
 * @brief Persistent settings for the RPM-based launch mode.
 *
 * This page is intentionally much smaller than page 18, and everything on it is required:
 *
 *   - an **RPM limit versus time** curve, so the limiter can ramp rather than sit at one value;
 *   - a **soft timing retard** that engages while the engine is within `softZoneRpm` of the
 *     limit, with its own hysteresis so it does not flicker on and off at the boundary;
 *   - a **power cut method**, so the hard limit can cut spark, fuel, both, or roll.
 *
 * There is no reference wire, no slip scoring, no gear gate and no boost trim here. Those belong
 * to the speed page or to the boost page, and duplicating them would be exactly the "full
 * secondary setup" that this page exists to avoid needing.
 *
 * Most of the payload is reserved so that future additions to the simple path cost nothing.
 */
struct ppLaunchRpmConfig : public config_page_t {
  // ---- 0..15: enable and arming --------------------------------------------------------
  uint8_t enable;        ///< 0 = off, 1 = on.
  uint8_t armPin;        ///< Input pin for the arm / launch switch. 0 = none configured.
  uint8_t armPolarity;   ///< 0 = asserted HIGH, 1 = asserted LOW
  uint8_t armPullup;     ///< 0 = plain INPUT, 1 = INPUT_PULLUP
  uint8_t cutMethod;     ///< @ref PpLimitMethod, applied at the hard limit
  uint8_t reserved0;     ///< Reserved, must be 0.

  uint16_t timeoutMs;    ///< Armed timeout, ms. 0 = never time out.
  uint16_t softZoneRpm;  ///< Soft retard engages within this many RPM BELOW the limit. 0 = off.
  uint16_t softHystRpm;  ///< Hysteresis, RPM. Once engaged, disengages only below
                         ///< (limit - softZone - softHyst). Guards against flicker at the edge.
  uint16_t reserved1;    ///< Reserved, must be 0.

  // ---- 16..35: soft retard -------------------------------------------------------------
  /// Retard applied while in the soft zone, in TENTHS of a degree (0..600 = 0.0..60.0).
  ///
  /// A flat value rather than a curve on purpose: this page is the simple one, and "pull 8
  /// degrees when we get close to the limit" is the whole of what a soft launch retard needs to
  /// be. A tuner who wants a shaped retard ramp wants the speed page.
  uint16_t softRetardTenths;
  /// Rate limit for the soft retard, tenths of a degree per SECOND. 0 = apply immediately.
  uint16_t retardSlewTenths;
  uint8_t  reserved2[18]; ///< Reserved, must be 0. Pads the label to a 2-byte boundary.

  // ---- 36..55: label -------------------------------------------------------------------
  uint8_t label[PP_LABEL_LEN]; ///< Editable curve label, NUL terminated ("rpm")

  // ---- 56..119: time axis --------------------------------------------------------------
  /// Milliseconds since the launch release. Forced to start at 0 and strictly ascending.
  uint16_t curveTime[PP_RPM_CURVE_POINTS];

  // ---- 120..183: RPM limit -------------------------------------------------------------
  /// The RPM hard limit at each point. Whole RPM; 0 disables the limiter at that point.
  uint16_t curveLimit[PP_RPM_CURVE_POINTS];

  // ---- 184..511: reserved for future additions -----------------------------------------
  /// Deliberately 328 bytes of headroom. This page is deliberately sparse, and a future
  /// addition to the simple path (a per-point soft zone, a cut-duty ramp, whatever the next
  /// round of tuning asks for) should cost nothing and move nothing.
  uint8_t reserved3[PPLAUNCH_PAYLOAD_SIZE - 184U];
};

using ppLaunchRpmBlock = ppLaunchRpmConfig;

// ---- Page 19 layout contract ----------------------------------------------------------
//   enable                 1 B  @   0      curveTime             64 B  @  56
//   armPin                 1 B  @   1      curveLimit            64 B  @ 120
//   armPolarity            1 B  @   2      reserved3            328 B  @ 184
//   armPullup              1 B  @   3                                  -------
//   cutMethod              1 B  @   4      payload              512 B
//   reserved0              1 B  @   5
//   timeoutMs              2 B  @   6
//   softZoneRpm            2 B  @   8
//   softHystRpm            2 B  @  10
//   reserved1              2 B  @  12
//   softRetardTenths       2 B  @  14
//   retardSlewTenths       2 B  @  16
//   reserved2             18 B  @  18
//   label                 20 B  @  36
static_assert(offsetof(ppLaunchRpmConfig, enable) == 0, "rpm enable must be at offset 0");
static_assert(offsetof(ppLaunchRpmConfig, armPin) == 1, "rpm armPin must be at offset 1");
static_assert(offsetof(ppLaunchRpmConfig, armPolarity) == 2, "rpm armPolarity must be at offset 2");
static_assert(offsetof(ppLaunchRpmConfig, armPullup) == 3, "rpm armPullup must be at offset 3");
static_assert(offsetof(ppLaunchRpmConfig, cutMethod) == 4, "rpm cutMethod must be at offset 4");
static_assert(offsetof(ppLaunchRpmConfig, timeoutMs) == 6, "rpm timeoutMs must be at offset 6");
static_assert(offsetof(ppLaunchRpmConfig, softZoneRpm) == 8, "rpm softZoneRpm must be at offset 8");
static_assert(offsetof(ppLaunchRpmConfig, softHystRpm) == 10, "rpm softHystRpm must be at offset 10");
static_assert(offsetof(ppLaunchRpmConfig, softRetardTenths) == 14, "rpm softRetardTenths must be at offset 14");
static_assert(offsetof(ppLaunchRpmConfig, retardSlewTenths) == 16, "rpm retardSlewTenths must be at offset 16");
static_assert(offsetof(ppLaunchRpmConfig, reserved2) == 18, "rpm reserved2 must be at offset 18");
static_assert(offsetof(ppLaunchRpmConfig, label) == 36, "rpm label must be at offset 36");
static_assert(offsetof(ppLaunchRpmConfig, curveTime) == 56, "rpm curveTime must be at offset 56");
static_assert(offsetof(ppLaunchRpmConfig, curveLimit) == 120, "rpm curveLimit must be at offset 120");
static_assert(offsetof(ppLaunchRpmConfig, reserved3) == 184, "rpm reserved3 must be at offset 184");
static_assert(sizeof(ppLaunchRpmConfig) == PPLAUNCH_PAYLOAD_SIZE,
              "ppLaunchRpmConfig size changed - update pageSize in speeduino.ini");

// The block locations. The emulated EEPROM is 23,808 bytes (0x0000..0x5CFF), see AGENTS.md 2.1.
// Both blocks must sit entirely inside it, above the frozen 0x0000-0x0FFF core tune ABI, must
// not overlap the long-term-trim block (0x1000..0x181F), the race-time block (0x1820..0x192F) or
// the launch-boost block (0x1D50..0x215F), and must not overlap each other.
static_assert(EEPROM_PPLAUNCH_SPEED_BASE >= 0x1000U, "Custom data must live above the core tune block");
static_assert(EEPROM_PPLAUNCH_SPEED_BASE >= (0x1000U + 16U + 2064U + 16U + 256U),
              "Speed-mode block must start after the long-term-trim and race-time blocks");
static_assert(EEPROM_PPLAUNCH_RPM_BASE == (EEPROM_PPLAUNCH_SPEED_BASE + PPLAUNCH_BLOCK_SIZE),
              "RPM-mode block must immediately follow the speed-mode block");
static_assert((uint32_t)EEPROM_PPLAUNCH_RPM_BASE + (uint32_t)PPLAUNCH_BLOCK_SIZE <= 0x1D50U,
              "Launch-boost block must start after both launch blocks");
static_assert(((uint32_t)EEPROM_PPLAUNCH_RPM_BASE + (uint32_t)PPLAUNCH_BLOCK_SIZE) <= 0x5D00U,
              "PP launch blocks do not fit in the emulated EEPROM address space");

/** @brief The live page payloads.
 *
 * Globals rather than file-statics so that pages.cpp can take their addresses in a `constexpr`
 * page map, exactly as it does for every config page. The page layer needs the address at
 * compile time; a function call cannot provide it.
 */
extern ppLaunchSpeedBlock ppLaunchSpeedData;
extern ppLaunchRpmBlock ppLaunchRpmData;

// =====================================================================================
// Runtime API
// =====================================================================================

/** @brief Load both blocks at boot and bind the arm inputs. */
void initialisePpLaunch(statuses &current);

/** @brief Restore defaults in RAM only, for both modes. Called by setTuneToEmpty(). */
void ppLaunchRestoreDefaults(void);

/** @brief Advance the state machine and both clocks. Call exactly once per millisecond. */
void ppLaunchTick1ms(statuses &current);

/**
 * @brief Evaluate the active mode and publish retard / limit requests.
 *
 * Cheap and safe to call every main-loop pass. The retard is republished on every call because
 * the torque bus is cleared once per pass and a producer that only published occasionally would
 * have its request flicker to zero in between.
 *
 * @param current The live status.
 * @param page2   Base config, for the VSS mode.
 */
void ppLaunchUpdate(statuses &current, const config2 &page2);

/// @name Results of the last evaluation
/// @{
/** @brief The RPM limit to apply now, in whole RPM. UINT16_MAX when the limiter is not engaged. */
uint16_t ppLaunchRpmLimit(void);

/** @brief The limiter method to apply now. Equals the configured method only while limiting. */
PpLimitMethod ppLaunchLimitMethod(void);

/** @brief Retard requested now, in whole degrees, 0..60. Zero when not launching. */
uint8_t ppLaunchRetard(void);

/** @brief Speed-mode boost DUTY trim, percent 0..100. Zero when not launching. */
uint8_t ppLaunchBoostTrim(void);

/** @brief Which page is currently driving the outputs. */
PpMode ppLaunchActiveMode(void);

/** @brief The state machine state, for logging and for consumers that need to know. */
PpState ppLaunchState(void);

/**
 * @brief Milliseconds since the launch release, as the launch module counts them.
 *
 * Exposed so a consumer that must be in step with the launch - the boost target curve, for one -
 * reads the SAME clock rather than starting its own. Two features each counting seconds from
 * "the launch" will disagree about when it was, and a boost target one second out is a target
 * for the wrong part of the pass.
 *
 * Zero whenever the module is not LAUNCHING, so a consumer cannot accidentally act on a stale
 * elapsed time from a previous attempt.
 */
uint32_t ppLaunchElapsedMs(void);

/** @brief The speed-mode axis value the last evaluation used (filters and clamps applied). */
uint16_t ppLaunchAxisValue(void);

/** @brief Signed deviation from the reference wire, axis units. Negative means behind the wire. */
int16_t ppLaunchSlip(void);

/** @brief True if the limiter is engaged right now. */
bool ppLaunchIsLimiting(void);

/** @brief True if either mode is enabled and loaded. */
bool ppLaunchIsActive(void);

/** @brief Force the state machine back to idle and drop every output. */
void ppLaunchReset(void);
/// @}

/// @name TunerStudio / test hooks
/// @{
/** @brief Mutable access to a page payload. */
ppLaunchSpeedBlock &ppLaunchGetSpeedPageMutable(void);
ppLaunchRpmBlock &ppLaunchGetRpmPageMutable(void);

/** @brief Read-only access to a page payload. */
const ppLaunchSpeedBlock &ppLaunchGetSpeedPage(void);
const ppLaunchRpmBlock &ppLaunchGetRpmPage(void);

/** @brief True if the persisted block for that mode was found intact (magic + version + CRC). */
bool ppLaunchIsInitialised(PpMode mode);

/** @brief True if the storage backend is large enough to hold both blocks. */
bool ppLaunchIsAvailable(void);

/** @brief The conditioned axis value, in the configured unit, for logging. */
extern uint16_t ppLaunchAxisLog;

/** @brief Signed deviation from the reference wire on the last evaluation, axis units. */
extern int16_t ppLaunchSlipLog;

/** @brief Mark a mode's settings dirty so the next stopped-engine poll persists them. */
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

/** @brief Inject the arm input level. Unit tests only. */
void ppLaunchSetArmForTest(bool asserted);
#endif
