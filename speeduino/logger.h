/** \file logger.h
 * @brief File for generating log files and meta data
 * @author Josh Stewart
 * 
 * This file contains functions for creating a log file for use with by TunerStudio directly or to be written to an SD card
 * 
 */

#ifndef LOGGER_H
#define LOGGER_H

#include "statuses.h"

/**
 * @brief Number of live-data channels (bytes) the firmware will serve.
 *
 * This is NOT the byte count on the wire: generateLiveValues() prepends one reply-code byte, so
 * the wire length is LOG_ENTRY_SIZE + 1. In the ini that is exactly `ochBlockSize`, and the
 * highest legal channel index is LOG_ENTRY_SIZE - 1.
 *
 * FND-05 moved this from 138 to 145. The added channels are:
 *   138..141  raceTime (32-bit, published as two U16 halves)
 *   142       raceTimeState
 *   143       raceTimeStatus
 *   144       systemTemp  (previously declared at 138 and therefore never actually sent)
 *
 * Perfect-pass launch control (pages 18/19) moved it from 145 to 156. The added channels are:
 *   145..146  ppLaunchAxis        (U16, tenths of mph/kph, shaft RPM, or engine RPM)
 *   147..148  ppLaunchSlip        (S16, signed deviation from the reference wire)
 *   149..150  ppLaunchLimit       (U16, RPM limit in force, whole RPM)
 *   151..152  ppLaunchBoostTarget (U16, kPa, 0 = no override)
 *   153       ppLaunchRetard      (U08, whole degrees)
 *   154       ppLaunchState       (U08, PpState)
 *   155       ppLaunchStatus      (U08, bitfield)
 *
 * Both constants MUST agree with reference/speeduino.ini. If they drift, TunerStudio rejects the
 * ini outright: ochBlockSize-1 is the highest channel it will ask for.
 */
constexpr uint8_t LOG_ENTRY_SIZE = 156;

byte getTSLogEntry(uint16_t byteNum);

/** @brief The main-loop rate to report over the serial protocol.
 *
 * @ref statuses.loopsPerSecond is 32 bit because the loop can exceed 65535/s, but the
 * TunerStudio field (`loopsPerSecond` in speeduino.ini) is a U16. Clamping here rather than
 * writing the clamp back into currentStatus keeps the measured value intact for internal use -
 * the old code did `currentStatus.loopsPerSecond = 60000U`, destroying the measurement.
 *
 * @return The rate, saturated to UINT16_MAX.
 */
uint16_t loopCountForReporting(void);
int16_t getReadableLogEntry(uint16_t logIndex);
float getReadableFloatLogEntry(uint16_t logIndex);
uint8_t getLegacySecondarySerialLogEntry(uint16_t byteNum);
bool is2ByteEntry(uint8_t key);

void startToothLogger(void);
void stopToothLogger(void);

void startCompositeLogger(void);
void stopCompositeLogger(void);

void startCompositeLoggerTertiary(void);
void stopCompositeLoggerTertiary(void);

void startCompositeLoggerCams(void);
void stopCompositeLoggerCams(void);

/** @brief Build the TunerStudio engine status byte from the current status */
byte buildEngineStatus(const statuses &current);

/** @brief Build the TunerStudio SD card status byte from the current status */
byte buildSdCardStatus(const statuses &current);

#endif
