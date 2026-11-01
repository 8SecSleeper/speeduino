#pragma once
#include <stdint.h>

/**
 * @file
 * @brief FND-09: the torque-reduction request bus.
 *
 * Every drag feature that protects the engine is ultimately asking for the same thing:
 * "remove this many degrees of advance", "cut this fraction of the fuel", "hold for this long".
 * Traction control, launch control, flat shift, the rev limiters, knock retard, nitrous retard
 * and torque management all produce those requests at different times, and before this module
 * they each subtracted from `advance` directly and independently in `correctionsIgn()`.
 *
 * That works until two of them are active at once. Then the retard stacks (20 + 15 = 35
 * degrees becomes a backfire), or one silently overwrites the other depending on call order,
 * and the tuner has no way to see which one did it. The roadmap calls this out explicitly:
 * "Every competitor that got this wrong has a class of bugs where two limiters interact badly.
 * Design it in, not after."
 *
 * @section rule The arbitration rule
 *
 *  - **Retard: the largest single request wins.** Not the sum. Two systems each asking for 10
 *    degrees do not mean 20 - they mean "this engine needs at least 10 removed". Summing is
 *    how you get negative advance at the top of a pass.
 *  - **Cut: the strongest cut wins**, by the same reasoning. A full cut beats a 30% cut.
 *  - **Every request is attributed.** The winning producer is recorded, so the log can say
 *    *why* timing was pulled rather than just reporting a number.
 *
 * @section contract Producer contract
 *
 * 1. Requests are **re-published every evaluation**. There is no "cancel": the bus is cleared
 *    at the start of each main-loop pass and every producer re-states its demand. This makes a
 *    producer that stops running (a disabled feature, a stalled state machine) automatically
 *    stop requesting, with no cleanup path to forget.
 * 2. A producer must never write to `currentStatus.advance` directly. Publish here, and let
 *    the arbiter be the only writer.
 * 3. Requests are clamped at publish time, so a buggy producer cannot ask for 200 degrees.
 */

/** @brief The subsystems that can request torque reduction. */
enum class TorqueProducer : uint8_t {
  None = 0,        ///< No request
  RevLimit = 1,    ///< Soft/hard RPM limiters
  Launch = 2,      ///< Launch control and flat shift
  Knock = 3,       ///< Knock retard
  Nitrous = 4,     ///< Nitrous retard
  RaceTime = 5,    ///< The retard-vs-race-time curve (FND-05's reference consumer)
  Traction = 6,    ///< Traction control (reserved, not implemented)
  TorqueMgmt = 7,  ///< Torque management (reserved, not implemented)
  Protection = 8,  ///< Oil pressure / coolant / AFR / boost protection
  Count
};

/** @brief What kind of cut a request wants. Ordered by severity - higher wins arbitration. */
enum class TorqueCutMode : uint8_t {
  NoCut = 0,    ///< Retard only
  Fuel = 1,     ///< Cut fuel
  Spark = 2,    ///< Cut spark
  Both = 3,     ///< Cut both
};

/** @brief The maximum retard any single producer may request, in whole degrees.
 *
 * Well past anything a real tune uses, but small enough that a runaway producer cannot command
 * the timing to an impossible value. */
constexpr uint8_t TORQUE_RETARD_MAX = 60;

/** @brief The maximum cut percentage a producer may request. */
constexpr uint8_t TORQUE_CUT_PERCENT_MAX = 100;

/**
 * @brief A single torque-reduction request.
 *
 * Kept as a plain struct of bytes so the arbiter can be unit-tested without a fixture, and so
 * the whole bus costs 9 bytes of RAM per producer with nothing hidden.
 */
struct torqueRequest_t {
  int8_t retard = 0;                        ///< Advance to remove, whole degrees, clamped 0..60
  uint8_t cutPercent = 0;                   ///< Fraction of events to cut, 0..100
  TorqueCutMode cutMode = TorqueCutMode::NoCut; ///< Cut fuel / spark / both
};

/** @brief Drop every outstanding request. Call at the top of each main-loop pass. */
void torqueBusReset(void);

/**
 * @brief Publish a request from a producer.
 *
 * Repeated calls from the same producer replace its previous request rather than accumulating,
 * so a producer can call this from inside a loop without having to reason about how often it
 * runs.
 *
 * @param producer The requesting subsystem.
 * @param retard   Advance to remove, whole degrees. Clamped to [0, TORQUE_RETARD_MAX].
 * @param cutMode  The kind of cut wanted.
 * @param cutPercent Fraction of events to cut, 0..100. Ignored when @p cutMode is NoCut.
 */
void torqueRequestRetard(TorqueProducer producer, int8_t retard,
                         TorqueCutMode cutMode = TorqueCutMode::NoCut,
                         uint8_t cutPercent = 0);

/** @brief The winning retard after arbitration, whole degrees. */
int8_t torqueBusRetard(void);

/** @brief The winning cut mode after arbitration. */
TorqueCutMode torqueBusCutMode(void);

/** @brief The winning cut percentage after arbitration. */
uint8_t torqueBusCutPercent(void);

/** @brief Which producer won the retard arbitration. */
TorqueProducer torqueBusRetardProducer(void);

/** @brief Which producer won the cut arbitration. */
TorqueProducer torqueBusCutProducer(void);

/** @brief True if any producer is asking for anything at all. */
bool torqueBusIsActive(void);

/** @brief Read back what one producer asked for, for logging and tests. */
const torqueRequest_t &torqueRequestOf(TorqueProducer producer);

#if defined(UNIT_TEST)
/** @brief Reset the bus to empty. Unit tests only. */
void torqueBusResetForTest(void);
#endif
