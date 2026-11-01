/*
All functions in the gamma file return

*/
#ifndef CORRECTIONS_H
#define CORRECTIONS_H

void initialiseCorrections(void);
uint16_t correctionsFuel(void);
uint8_t calculateAfrTarget(table3d16RpmLoad &afrLookUpTable, const statuses &current, const config2 &page2, const config6 &page6);

/** @brief Present the signed short-term trim on the legacy 0..255, 100 == no-change scale.
 *
 * `currentStatus.egoCorrection` is signed about zero, which is what the fuelling code and the
 * learner need. Every *external* consumer - the status byte stream, the legacy serial
 * protocol, saved logs - was built around 100 == no change, so converting at the boundary
 * keeps them correct without either side having to know about the other's convention.
 *
 * Saturates rather than wrapping: a -100% trim reports 0, not 156.
 */
static inline uint8_t buildEgoCorrectionChannel(const statuses &current)
{
  const int16_t scaled = (int16_t)100 + (int16_t)current.egoCorrection;
  return (uint8_t)clamp(scaled, (int16_t)0, (int16_t)255);
}

int8_t correctionsIgn(int8_t advance);
int8_t correctionFixedTiming(int8_t advance);
int8_t correctionCrankingFixedTiming(int8_t advance);

uint16_t correctionsDwell(uint16_t dwell);


#endif // CORRECTIONS_H
