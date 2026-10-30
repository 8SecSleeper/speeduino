#pragma once

#include <stdint.h>
#include "../../../config_pages.h"

/** @brief Initialise the programmable I/O system.
 * @param page13 The tune
 */
void initialiseProgrammableIO(config13& page13);

/** @brief Control function for the programmable I/O system.
 * Should be called regularly (e.g. in main loop) to check the rules and update outputs accordingly.
 * @param page13 The tune
 * @param tickDelays Whether to advance the activation/output delay counters on this call. Those
 *        counters are presented to the user in units of 0.1s, so call this with `true` at 10Hz and
 *        with `false` on any faster evaluation.
 */
void programmableIOControl(const config13& page13, bool tickDelays);

/** @brief Get single I/O data var (from current) for comparison.
 * @param index Field index/number (?)
 */
int16_t programmableIOGetData(uint16_t index);

/** @brief Get the output status of the programmable I/O system.
 * @return The compressed output status.
 */
uint8_t getProgrammableIOOutputStatus(void);
