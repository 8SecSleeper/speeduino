#include "torqueRequest.h"

/**
 * @file
 * @brief The torque-reduction arbiter. See torqueRequest.h for the contract and the rule.
 *
 * The whole module is deliberately tiny and free of any knowledge of the engine. It exists so
 * that a single, inspectable place decides what the retards actually are, instead of five
 * subsystems each subtracting from `advance` in whatever order `correctionsIgn()` happens to
 * call them.
 */

/** @brief One slot per producer. Indexed by TorqueProducer, so no lookup is needed. */
static torqueRequest_t requests[(uint8_t)TorqueProducer::Count];

static inline uint8_t toIndex(TorqueProducer producer)
{
  const uint8_t index = (uint8_t)producer;
  return (index < (uint8_t)TorqueProducer::Count) ? index : 0U;
}

void torqueBusReset(void)
{
  for (uint8_t i = 0U; i < (uint8_t)TorqueProducer::Count; ++i)
  {
    requests[i] = torqueRequest_t();
  }
}

void torqueRequestRetard(TorqueProducer producer, int8_t retard,
                         TorqueCutMode cutMode, uint8_t cutPercent)
{
  if (producer == TorqueProducer::None)
  {
    // Producer::None is the "no request" slot, not a producer. Writing to it would corrupt the
    // arbiter's empty state and make torqueBusIsActive() permanently true.
    return;
  }

  torqueRequest_t &slot = requests[toIndex(producer)];

  // Clamp rather than reject: a producer asking for 90 degrees still wants as much retard as
  // the ignition system can express, and silently ignoring it would look like a dead feature.
  if (retard < 0) { retard = 0; }
  if (retard > (int8_t)TORQUE_RETARD_MAX) { retard = (int8_t)TORQUE_RETARD_MAX; }

  slot.retard = retard;
  slot.cutMode = cutMode;

  if (cutPercent > TORQUE_CUT_PERCENT_MAX) { cutPercent = TORQUE_CUT_PERCENT_MAX; }
  slot.cutPercent = (cutMode == TorqueCutMode::NoCut) ? 0U : cutPercent;
}

/** @brief The largest single retard request, not the sum. See the rule in torqueRequest.h. */
int8_t torqueBusRetard(void)
{
  int8_t best = 0;
  for (uint8_t i = 0U; i < (uint8_t)TorqueProducer::Count; ++i)
  {
    if (requests[i].retard > best) { best = requests[i].retard; }
  }
  return best;
}

TorqueProducer torqueBusRetardProducer(void)
{
  TorqueProducer winner = TorqueProducer::None;
  int8_t best = 0;
  for (uint8_t i = 0U; i < (uint8_t)TorqueProducer::Count; ++i)
  {
    if (requests[i].retard > best)
    {
      best = requests[i].retard;
      winner = (TorqueProducer)i;
    }
  }
  return winner;
}

/** @brief The strongest cut wins. Ordered by severity, then by percentage within a severity. */
TorqueCutMode torqueBusCutMode(void)
{
  TorqueCutMode best = TorqueCutMode::NoCut;
  for (uint8_t i = 0U; i < (uint8_t)TorqueProducer::Count; ++i)
  {
    if (requests[i].cutMode > best) { best = requests[i].cutMode; }
  }
  return best;
}

uint8_t torqueBusCutPercent(void)
{
  const TorqueCutMode mode = torqueBusCutMode();
  if (mode == TorqueCutMode::NoCut)
  {
    return 0U;
  }

  // Percentage is only comparable between producers asking for the same kind of cut: a 20%
  // spark cut is not weaker than a 100% fuel cut in any sense the arbiter can reason about, so
  // severity is decided first and percentage only breaks ties within it.
  uint8_t best = 0U;
  for (uint8_t i = 0U; i < (uint8_t)TorqueProducer::Count; ++i)
  {
    if (requests[i].cutMode == mode && requests[i].cutPercent > best)
    {
      best = requests[i].cutPercent;
    }
  }
  return best;
}

TorqueProducer torqueBusCutProducer(void)
{
  const TorqueCutMode mode = torqueBusCutMode();
  if (mode == TorqueCutMode::NoCut)
  {
    return TorqueProducer::None;
  }

  TorqueProducer winner = TorqueProducer::None;
  uint8_t best = 0U;
  for (uint8_t i = 0U; i < (uint8_t)TorqueProducer::Count; ++i)
  {
    if (requests[i].cutMode == mode && requests[i].cutPercent >= best)
    {
      best = requests[i].cutPercent;
      winner = (TorqueProducer)i;
    }
  }
  return winner;
}

bool torqueBusIsActive(void)
{
  for (uint8_t i = 0U; i < (uint8_t)TorqueProducer::Count; ++i)
  {
    if (requests[i].retard != 0 || requests[i].cutMode != TorqueCutMode::NoCut)
    {
      return true;
    }
  }
  return false;
}

const torqueRequest_t &torqueRequestOf(TorqueProducer producer)
{
  return requests[toIndex(producer)];
}

#if defined(UNIT_TEST)
void torqueBusResetForTest(void)
{
  torqueBusReset();
}
#endif
