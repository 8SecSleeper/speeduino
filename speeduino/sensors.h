#ifndef SENSORS_H
#define SENSORS_H

#include "timers.h"
#include "config_pages.h"
#include "statuses.h"
#include "table2d.h"

// The following are alpha values for the ADC filters.
// Their values are from 0 to 240, with 0 being no filtering and 240 being maximum
#define ADCFILTER_TPS_DEFAULT   50U
#define ADCFILTER_CLT_DEFAULT  180U
#define ADCFILTER_IAT_DEFAULT  180U
#define ADCFILTER_O2_DEFAULT   128U
#define ADCFILTER_BAT_DEFAULT  128U
#define ADCFILTER_MAP_DEFAULT   20U //This is only used on Instantaneous MAP readings and is intentionally very weak to allow for faster response
#define ADCFILTER_BARO_DEFAULT  64U

#define ADCFILTER_PSI_DEFAULT  150U //not currently configurable at runtime, used for misc pressure sensors, oil, fuel, etc.

#define FILTER_FLEX_DEFAULT     75U

#define VSS_GEAR_HYSTERESIS 10U
#define VSS_SAMPLES         4U //Must be a power of 2 and smaller than 255

extern volatile uint8_t flexCounter;
extern volatile uint32_t flexPulseWidth;

#define BIT_SENSORS_UNUSED1         0
#define BIT_SENSORS_BARO_SAVED      1
#define BIT_SENSORS_UNUSED2         2
#define BIT_SENSORS_UNUSED3         3
#define BIT_SENSORS_UNUSED4         4
#define BIT_SENSORS_UNUSED5         5
#define BIT_SENSORS_UNUSED6         6
#define BIT_SENSORS_UNUSED7         7

void initialiseADC(void);
void flexPulse(void);
void initialiseFlexSensor(config2 &page2, statuses &current, uint8_t pin);
void knockPulse(void);
uint32_t vssGetPulseGap(byte toothHistoryIndex);
void vssPulse(void);
uint16_t readAnalogSensor(uint8_t pin);

/** @brief Initial reading of the TPS sensor, primarily to detect flood clear state */
void initialiseTPS(void);

/** @brief Initial reading of the coolant sensor, primarily to make sure the priming pulsewidth is correct */
void initialiseCLT(void);

/** @brief How often the TPS is sampled, in Hz. Valid values are 15, 30 or 200. */
#define TPS_READ_FREQUENCY  200

/** @brief Define the TPS sensor read frequency.
 *
 * @note These must be numeric literals, not the BIT_TIMER_* constants: those are `constexpr` and are
 * invisible to the preprocessor, so `#if` cannot test them. If a BIT_TIMER_* bit is ever renumbered,
 * update the mapping below by hand - the `#error` branch exists so a forgotten update fails loudly.
 */
#if TPS_READ_FREQUENCY==15
#define TPS_READ_TIMER_BIT BIT_TIMER_15HZ
#elif TPS_READ_FREQUENCY==30
#define TPS_READ_TIMER_BIT BIT_TIMER_30HZ
#elif TPS_READ_FREQUENCY==200
#define TPS_READ_TIMER_BIT BIT_TIMER_200HZ
#else
#error "TPS_READ_FREQUENCY must be 15, 30 or 200 (Hz)"
#endif

/** @brief Define the coolant sensor read frequency.
 *  Thermal mass means this only needs to track WUE, idle target, fan and protection. */
#define CLT_READ_TIMER_BIT BIT_TIMER_10HZ

/** @brief Define the IAT sensor read frequency. Feeds the air-density correction. */
#define IAT_READ_TIMER_BIT BIT_TIMER_10HZ

/** @brief Define the O2 sensor read frequency. Drives closed-loop lambda, AFR protection, autotune.
 *  The wideband controller itself is the limit here, not the ECU. */
#define O2_READ_TIMER_BIT BIT_TIMER_50HZ

/** @brief Define the battery sensor read frequency. Drives injector dead-time and dwell correction,
 *  so an alternator load step should be visible rather than 250ms late. */
#define BAT_READ_TIMER_BIT BIT_TIMER_10HZ

/** @brief Define the baro sensor read frequency. Only needs to track weather. */
#define BARO_READ_TIMER_BIT BIT_TIMER_4HZ

/** @brief Define the MAP sensor read frequency. Drives fuel load and MAPdot AE. Already correct. */
#define MAP_READ_TIMER_BIT BIT_TIMER_1KHZ

/** @brief The rate at which TPS *rate of change* (tpsDOT) is evaluated, in Hz.
 *
 * tpsDOT now uses real elapsed time (see getTPSDeltaTime()), so it is correct at any sample rate.
 * This therefore simply follows the TPS sample rate - differentiated over one sample interval, which
 * at 200Hz is 5ms and gives the tightest possible tip-in response.
 *
 * The one thing that *does* need care is noise: tpsDOT divides by tpsDeltaTime, so back-to-back
 * reads would divide by a near-zero interval. TPS_DOT_MIN_PERIOD_US floors that divisor. 1ms is well
 * below the 5ms interval produced by a 200Hz TPS read, so the floor never distorts a normal sample -
 * it only bounds the pathological case.
 */
#define TPS_DOT_TIMER_BIT TPS_READ_TIMER_BIT
#define TPS_DOT_MIN_PERIOD_US 1000U

/** @brief How often the analog knock input is sampled.
 *
 * Knock detection is inherently a high-rate measurement - it is looking for a short mechanical
 * ringing event, and a 33Hz sample interval (the previous rate) is far too coarse to catch it
 * reliably. 200Hz is the fastest rate available from the main loop without perturbing the
 * injector/ignition schedulers. A genuinely fast knock channel would need ISR-level sampling with a
 * per-cylinder window, which is separate work (KNO-01/KNO-02 in the backlog).
 */
#define KNOCK_READ_TIMER_BIT BIT_TIMER_200HZ

/** @brief Define the VSS (vehicle speed) read frequency. Feeds traction control, boost-by-speed and
 *  gear detection. The edge capture is in an ISR; this is only the derived value. */
#define VSS_READ_TIMER_BIT BIT_TIMER_50HZ

/** @brief Define the gear-detection read frequency. Must be usable as a table axis, so it cannot
 *  chatter the way a 10Hz update does. */
#define GEAR_READ_TIMER_BIT BIT_TIMER_50HZ

/** @brief Define the fuel-pressure read frequency. This is a protection input; 250ms is far too slow
 *  to catch a failing pump under load. */
#define FUEL_PRESSURE_READ_TIMER_BIT BIT_TIMER_20HZ

/** @brief Define the oil-pressure read frequency. Protection input - must not be as slow as a
 *  coolant sensor. */
#define OIL_PRESSURE_READ_TIMER_BIT BIT_TIMER_20HZ

/** @brief Read the sensors that are polled at every loop. This includes the TPS, MAP, CLT, IAT and O2 sensors */
void readPolledSensors(loop_timer_t loopTimer);

/** @brief Get the time in µS between the last 2 TPS readings. Feeds tpsDOT. */
uint32_t getTPSDeltaTime(void);

#if defined(UNIT_TEST)
/** @brief Override the measured TPS sample interval. Unit tests only - see sensors.cpp. */
void setTPSSampleIntervalUs(uint32_t interval);
#endif

/** @brief Initialize the MAP calculation & Baro values */
void initialiseMAPBaro(void);
void resetMAPcycleAndEvent(void);

uint8_t getAnalogKnock(void);

/** @brief Get the MAP change between the last 2 readings */
int16_t getMAPDelta(void);

/** @brief Get the time in µS between the last 2 MAP readings */
uint32_t getMAPDeltaTime(void);

extern table2D_u16_u8_32 cltCalibrationTable;
extern table2D_u16_u8_32 iatCalibrationTable;
extern table2D_u16_u8_32 o2CalibrationTable; 

#endif // SENSORS_H
