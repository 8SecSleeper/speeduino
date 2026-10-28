#include "board_definition.h"

#if defined(CORE_TEENSY) && defined(__IMXRT1062__)
// The Teensy's internal EEPROM is kept available even when USE_SPI_EEPROM is set, so
// that the board can fall back to it at runtime if the external flash is not detected.
#include <EEPROM.h>
#if defined(USE_SPI_EEPROM)
  #include <SPI.h>
  #include "src/SPIAsEEPROM/SPIAsEEPROM.h"
#endif
#include "src/controllers/vvt/vvtController.h"
#include "src/controllers/idle/idle.h"
#include "timers.h"
#include "comms_secondary.h"
#include <InternalTemperature.h>
#include RTC_LIB_H
#include "board_eeprom_adapter.hpp"
#include "scheduler_ignition_controller.h"
#include "scheduler_fuel_controller.h"
#include "src/pins/pinNumbers_t.h"
#include "src/controllers/boost/boostController.h"
#include "globals.h"

/*
***********************************************************************************************************
* Boot trace ("black box")
*
* The failure mode we are chasing resets the CPU before USB enumerates, so there is no
* way to print where it died. The Teensy's internal EEPROM DOES work and survives a
* reset, so write a marker at each milestone. Read them back afterwards with
* tools/boot_trace - no serial needed during the failure.
*
* Stored above the 4096 bytes Speeduino uses for the tune, so it cannot corrupt a tune.
*/
static constexpr uint16_t BOOT_TRACE_BASE = 4220U;
static constexpr uint16_t BOOT_TRACE_LEN  = 48U;
static uint8_t bootTraceIndex = 0;

/** @brief Clear the trace. Call first, so stale markers are not mistaken for progress. */
static void bootTraceReset(void)
{
#if defined(BOOT_TRACE_ENABLED)
  bootTraceIndex = 0;
  for (uint16_t i = 0; i < BOOT_TRACE_LEN; i++) { EEPROM.update(BOOT_TRACE_BASE + i, 0xFF); }
#endif
}

/** @brief Record progress. Markers are small codes, see boot_trace sketch for the list. */
static void bootTrace(uint8_t marker)
{
#if defined(BOOT_TRACE_ENABLED)
  if (bootTraceIndex < BOOT_TRACE_LEN)
  {
    EEPROM.update(BOOT_TRACE_BASE + bootTraceIndex, marker);
    ++bootTraceIndex;
  }
#else
  UNUSED(marker);
#endif
}

/** @brief Public wrapper so init.cpp can drop markers along the shared boot path. */
void boardBootTrace(uint8_t marker) { bootTrace(marker); }

#if defined(USE_SPI_EEPROM)
/*
***********************************************************************************************************
* Emulated EEPROM in the onboard SPI flash
*
* The DropBear v2 mainboard carries a Winbond W25Q32JVSS (U3) wired to the Teensy 4.1 as a
* normal single-bit SPI device:
*   /CS = pin 6,  DI/IO0 = pin 11 (MOSI),  DO/IO1 = pin 12 (MISO),  CLK = pin 13 (SCK)
* /WP (IO2) and /HOLD (IO3) are hard-tied to +3V3 on the PCB, so quad/QSPI mode cannot be
* used even when a QSPI-capable part is fitted. The existing winbondFlashSPI driver handles it.
*
* NOTE on the CS pin: in the KiCad sources the CS net is "MCU-D6", which lands on U1 pad 8.
* In the Teensy4.1 schematic symbol the (number "N") field is the module *pad index*, not the
* Teensy pin number. Pad 8 is named "6_OUT1D", i.e. Teensy pin 6. (Teensy pin 8 is pad 10,
* "8_TX2_IN1" / GPIO_B1_00.) So CS is Teensy pin 6.
*
* IMPORTANT: these pins are the Teensy's LPSPI4 (the global `SPI` object) and are *shared*
* with both MC33810 injector/ignition drivers (U7/U8, CS on pins 9 and 10). The MC33810
* runtime path calls SPI.transfer16() without a transaction wrapper, and on Teensy 4
* beginTransaction() only reprograms the port when the clock actually changes. So the flash
* MUST run at the same clock/mode as the MC33810s, or the two will corrupt each other.
*/
static constexpr uint8_t  PIN_SPI_FLASH_CS   = 6U;       // DropBear v2 U3 /CS (net MCU-D6)
static constexpr uint32_t SPI_FLASH_CLOCK_HZ = 6000000U; // Must match acc_mc33810.cpp

// The MC33810 chip selects are on the same LPSPI4 bus as the flash. They are only
// configured later, in initMC33810(), which is after this file's flash init runs. Until
// then those pins sit as inputs in an undefined state, so the driver ICs can drive MISO
// while the flash is being formatted and corrupt the status reads. Driving both high
// (deselected) before touching the flash prevents that. Kept in sync with
// getDropBearMapping() in pinMapping.cpp.
static constexpr uint8_t PIN_MC33810_1_CS = 10U;
static constexpr uint8_t PIN_MC33810_2_CS = 9U;

// 133 sectors * 31 bytes = 4123 bytes of emulated EEPROM. Speeduino only addresses
// 0..4095 (see STORAGE_END in storage.cpp), so this covers the requirement with the
// fewest sectors, which matters because initialize() formats every sector on first
// boot and that cost is linear in the sector count.
//
// NOTE: _ReadWriteBuffer is 128 bytes and holds one whole per-byte slot. Slot size is
// Flash_Sector_Size/(EEPROM_Bytes_Per_Sector+1) = 4096/32 = 128, so this is exactly at
// the limit. Do NOT raise EEPROM_Bytes_Per_Sector - that would shrink the slot below
// what the buffer needs and overflow it.
// The 1MB base address leaves the start of the chip untouched.
static EEPROM_Emulation_Config EmulatedEEPROMMconfig{133UL, 4096UL, 31UL, 0x00100000UL};
static Flash_SPI_Config SPIconfig{PIN_SPI_FLASH_CS, &SPI};
static SPI_EEPROM_Class flashEEPROM(EmulatedEEPROMMconfig, SPIconfig);

/** @brief Park every other device on the shared SPI bus in the deselected state.
 *
 * Without this the MC33810s can drive MISO during flash access, which corrupts the
 * flash's status register reads.
 */
static void deselectSharedSpiDevices(void)
{
  pinMode(PIN_MC33810_1_CS, OUTPUT);
  digitalWrite(PIN_MC33810_1_CS, HIGH);
  pinMode(PIN_MC33810_2_CS, OUTPUT);
  digitalWrite(PIN_MC33810_2_CS, HIGH);
}

#endif // USE_SPI_EEPROM

#if defined(USE_SPI_EEPROM)

/** @brief Prepare the emulated EEPROM in the onboard SPI flash.
 *
 * @return true if the flash is detected AND already formatted, so it is safe to use.
 *
 * @note This deliberately does NOT format the chip. Formatting is 133 sector erases
 * (~6s) or, if anything goes wrong, far longer - all of it on the boot path before USB
 * is even set up, so the board never enumerates and appears to reset forever. That is
 * the boot loop this board exhibited.
 *
 * If the chip is detected but not formatted, we report NotDetected and fall back to the
 * Teensy's internal EEPROM, which is a working board. The chip is then formatted from
 * the PC using tools/flash_probe ('F' key), which writes the same magic numbers. That
 * keeps multi-second blocking work off the boot path entirely.
 */
static bool initialiseFlashEEPROM(void)
{
  static bool initialised = false; // Only probe the chip once
  if (!initialised)
  {
    initialised = true;
    bootTrace(0x10); // entering flash init

    // Do this BEFORE SPI.begin() so nothing else can answer on MISO.
    deselectSharedSpiDevices();
    bootTrace(0x11); // MC33810 CS parked

    SPI.begin();
    bootTrace(0x12); // SPI begun

    SPI.beginTransaction(SPISettings(SPI_FLASH_CLOCK_HZ, MSBFIRST, SPI_MODE0));
    bootTrace(0x13); // transaction started

    // Detect the chip first. This is only a JEDEC ID read - no writes.
    bool detected = flashEEPROM.detectDevice(SPI, PIN_SPI_FLASH_CS);
    bootTrace(detected ? 0x14 : 0x15); // 0x14 detected, 0x15 not detected

    if (!detected)
    {
      currentStatus.storageBackend = statuses::StorageBackend::NotDetected;
    }
    else if (flashEEPROM.hasMagicNumbers())
    {
      // Already formatted (normally by tools/flash_probe). Safe to use.
      flashEEPROM.markAvailable();
      currentStatus.storageBackend = statuses::StorageBackend::ExternalFlash;
      bootTrace(0x16); // magic numbers OK, using flash
    }
    else
    {
      // Present but blank. Do NOT format here. Run tools/flash_probe and press 'F'.
      currentStatus.storageBackend = statuses::StorageBackend::NotDetected;
      bootTrace(0x17); // detected but NOT formatted
    }
    bootTrace(0x18); // flash init complete
  }
  return currentStatus.storageBackend == statuses::StorageBackend::ExternalFlash;
}

#if defined(USE_SPI_EEPROM)
/** @brief Take the MC33810 chip selects off the shared SPI bus.
 *
 * This is the fix for "reads work at boot then the burn hangs forever".
 *
 * The two MC33810 injector/ignition drivers (U7/U8) sit on the same LPSPI4 bus as the
 * flash. acc_mc33810.cpp drives their chip selects from an RAII guard that leaves the pins
 * LOW when the chip is idle:
 *
 *     mc33810_active_guard_t(m) : _m(m) { _m.pin.setPinHigh(); }  // ACTIVE
 *     ~mc33810_active_guard_t()         { _m.pin.setPinLow();  }  // INACTIVE
 *
 * i.e. "inactive" is driven LOW. With both drivers holding their selects low they stay on
 * the bus and drive MISO, so the flash can never answer: every access reads back 0xFF for
 * the status register and all-ones for data.
 *
 * Measured on hardware by driving the two selects and probing the flash:
 *   CS high (as at boot)  -> SR1=0x00, magic numbers read back correctly
 *   CS low  (after init)  -> SR1=0xFF, data all zeros      <- the failure
 *   CS high again         -> SR1=0x00, recovered
 *
 * During boot this did not show up because initMC33810() runs later in the init sequence
 * than the tune is loaded. That is why the board boots fine and only fails on a burn.
 */
static inline void parkSharedSpiDevices(void)
{
  // Cheapest possible form - these are already configured as outputs by
  // deselectSharedSpiDevices()/initMC33810(), so this is two port writes with no pinMode
  // overhead, which matters because this runs around every single flash access.
  digitalWriteFast(PIN_MC33810_1_CS, HIGH);
  digitalWriteFast(PIN_MC33810_2_CS, HIGH);
}
#endif

// storage_api_t adapters for the emulated EEPROM held in the external SPI flash.
// These mirror the shape of board_eeprom_adapter.hpp, but bind to flashEEPROM
// rather than the Teensy's internal EEPROM object.
static byte readFlashEeprom(uint16_t address)
{
  parkSharedSpiDevices();
  return flashEEPROM.read(address);
}

static void writeFlashEeprom(uint16_t address, byte val)
{
  parkSharedSpiDevices();
  (void)flashEEPROM.write(address, val);
}

static uint16_t lengthFlashEeprom(void)
{
  return flashEEPROM.length();
}
#endif

static void PIT_isr();
static void TMR1_isr(void);
static void TMR2_isr(void);
static void TMR3_isr(void);
static void TMR4_isr(void);

/*
* The default Teensy41 serial.begin() has a timeout of 750ms, which is both too long to wait on startup and longer than it needs to be
* This function is a copy of the default serial.begin() but with the timeout lowered to 100ms
*/
static void serialBegin()
{
  uint32_t millis_begin = systick_millis_count;
  while (!Serial) 
  {
    uint32_t elapsed = systick_millis_count - millis_begin;
    //Wait up to 100ms for this. 
    if (elapsed > 100) { break; }
    yield();
  }
}

void initBoard(uint32_t /*baudRate*/)
{
    /*
    ***********************************************************************************************************
    * General
    */
   pSecondarySerial = &Serial2;

    /*
    Idle + Boost + VVT use the PIT timer. THIS IS ALSO USED BY THE INTERVAL TIMER THAT CALLS THE 1MS LOW RES TIMER!
    This has 4 channels that don't have compare registers, but will run for a period of time and then fire an interrupt
    The clock for these is set to 24Mhz and a prescale of 48 is used to give a 2uS tick time
    Set Prescaler
      * This is ideally too fast, but appears to be the slowest that the PIT can be set
      * 24Mhz = PER_clock
      * 48 * 1000000uS / PER_clock = 2uS
    */
    CCM_CSCMR1 |= CCM_CSCMR1_PERCLK_CLK_SEL; // 24MHz
    CCM_CSCMR1 |= CCM_CSCMR1_PERCLK_PODF(0b101111); //Prescale to 48

    attachInterruptVector(IRQ_PIT, PIT_isr);
    NVIC_ENABLE_IRQ(IRQ_PIT);
    NVIC_SET_PRIORITY(IRQ_PIT,255);

    /*
    ***********************************************************************************************************
    * Idle
    */
    if (isPwmIac(configPage6))
    {
      PIT_TCTRL0 = 0;
      PIT_TCTRL0 |= PIT_TCTRL_TIE; // enable Timer 1 interrupts
      PIT_TCTRL0 |= PIT_TCTRL_TEN; // start Timer 1
      PIT_LDVAL0 = 1; //1 * 2uS = 2uS
    }

    /*
    ***********************************************************************************************************
    * Timers
    */
    //Uses the PIT timer channel 4 on Teensy 4.1.
    PIT_TCTRL3 = 0;
    PIT_TCTRL3 |= PIT_TCTRL_TIE; // enable Timer 2 interrupts
    PIT_TCTRL3 |= PIT_TCTRL_TEN; // start Timer 2
    PIT_LDVAL3 = 500; //500 * 2uS = 1ms

    //TODO: Configure timers here

    /*
    ***********************************************************************************************************
    * Auxiliaries
    */
    if (configPage6.boostEnabled == 1)
    {
      PIT_TCTRL1 = 0;
      PIT_TCTRL1 |= PIT_TCTRL_TIE; // enable Timer 2 interrupts
      PIT_TCTRL1 |= PIT_TCTRL_TEN; // start Timer 2
      PIT_LDVAL1 = 1; //1 * 2uS = 2uS
    }
    if (configPage6.vvtEnabled == 1)
    {
      PIT_TCTRL2 = 0;
      PIT_TCTRL2 |= PIT_TCTRL_TIE; // enable Timer 3 interrupts
      PIT_TCTRL2 |= PIT_TCTRL_TEN; // start Timer 3
      PIT_LDVAL2 = 1; //1 * 2uS = 2uS
    }

    //TODO: Configure timers here

    /*
    ***********************************************************************************************************
    * Schedules
    */
    //Use the Quad timer
    //Uses the BUS clock speed, which is 1/4 of the CPU clock. Maximum prescaler of 128 is used to give a 0.853333uS tick time @ 600Mhz
    //TMR1 - Fuel 1-4
    //0
    TMR1_CTRL0 = 0;
    TMR1_CSCTRL0 = 0;
    TMR1_LOAD0 = 0; /* Reset load register */
    TMR1_CTRL0 |= TMR_CTRL_PCS(0b1111); //Set the prescaler to 128
    TMR1_CTRL0 |= TMR_CTRL_CM(1); //Start the timer
    //1
    TMR1_CTRL1 = 0;
    TMR1_CSCTRL1 = 0;
    TMR1_LOAD1 = 0; /* Reset load register */
    TMR1_CTRL1 |= TMR_CTRL_PCS(0b1111); //Set the prescaler to 128
    TMR1_CTRL1 |= TMR_CTRL_CM(1); //Start the timer
    //2
    TMR1_CTRL2 = 0;
    TMR1_CSCTRL2 = 0;
    TMR1_LOAD2 = 0; /* Reset load register */
    TMR1_CTRL2 |= TMR_CTRL_PCS(0b1111); //Set the prescaler to 128
    TMR1_CTRL2 |= TMR_CTRL_CM(1); //Start the timer
    //3
    TMR1_CTRL3 = 0;
    TMR1_CSCTRL3 = 0;
    TMR1_LOAD3 = 0; /* Reset load register */
    TMR1_CTRL3 |= TMR_CTRL_PCS(0b1111); //Set the prescaler to 128
    TMR1_CTRL3 |= TMR_CTRL_CM(1); //Start the timer
    //TMR2 - Ign 1-4
    //0
    TMR2_CTRL0 = 0;
    TMR2_CSCTRL0 = 0;
    TMR2_LOAD0 = 0; /* Reset load register */
    TMR2_CTRL0 |= TMR_CTRL_PCS(0b1111); //Set the prescaler to 128
    TMR2_CTRL0 |= TMR_CTRL_CM(1); //Start the timer
    //1
    TMR2_CTRL1 = 0;
    TMR2_CSCTRL1 = 0;
    TMR2_LOAD1 = 0; /* Reset load register */
    TMR2_CTRL1 |= TMR_CTRL_PCS(0b1111); //Set the prescaler to 128
    TMR2_CTRL1 |= TMR_CTRL_CM(1); //Start the timer
    //2
    TMR2_CTRL2 = 0;
    TMR2_CSCTRL2 = 0;
    TMR2_LOAD2 = 0; /* Reset load register */
    TMR2_CTRL2 |= TMR_CTRL_PCS(0b1111); //Set the prescaler to 128
    TMR2_CTRL2 |= TMR_CTRL_CM(1); //Start the timer
    //3
    TMR2_CTRL3 = 0;
    TMR2_CSCTRL3 = 0;
    TMR2_LOAD3 = 0; /* Reset load register */
    TMR2_CTRL3 |= TMR_CTRL_PCS(0b1111); //Set the prescaler to 128
    TMR2_CTRL3 |= TMR_CTRL_CM(1); //Start the timer
    //TMR3 - Fuel 5-8
    //0
    TMR3_CTRL0 = 0;
    TMR3_CSCTRL0 = 0;
    TMR3_LOAD0 = 0; /* Reset load register */
    TMR3_CTRL0 |= TMR_CTRL_PCS(0b1111); //Set the prescaler to 128
    TMR3_CTRL0 |= TMR_CTRL_CM(1); //Start the timer
    //1
    TMR3_CTRL1 = 0;
    TMR3_CSCTRL1 = 0;
    TMR3_LOAD1 = 0; /* Reset load register */
    TMR3_CTRL1 |= TMR_CTRL_PCS(0b1111); //Set the prescaler to 128
    TMR3_CTRL1 |= TMR_CTRL_CM(1); //Start the timer
    //2
    TMR3_CTRL2 = 0;
    TMR3_CSCTRL2 = 0;
    TMR3_LOAD2 = 0; /* Reset load register */
    TMR3_CTRL2 |= TMR_CTRL_PCS(0b1111); //Set the prescaler to 128
    TMR3_CTRL2 |= TMR_CTRL_CM(1); //Start the timer
    //3
    TMR3_CTRL3 = 0;
    TMR3_CSCTRL3 = 0;
    TMR3_LOAD3 = 0; /* Reset load register */
    TMR3_CTRL3 |= TMR_CTRL_PCS(0b1111); //Set the prescaler to 128
    TMR3_CTRL3 |= TMR_CTRL_CM(1); //Start the timer
    //TMR4 - IGN 5-8
    //0
    TMR4_CTRL0 = 0;
    TMR4_CSCTRL0 = 0;
    TMR4_LOAD0 = 0; /* Reset load register */
    TMR4_CTRL0 |= TMR_CTRL_PCS(0b1111); //Set the prescaler to 128
    TMR4_CTRL0 |= TMR_CTRL_CM(1); //Start the timer
    //1
    TMR4_CTRL1 = 0;
    TMR4_CSCTRL1 = 0;
    TMR4_LOAD1 = 0; /* Reset load register */
    TMR4_CTRL1 |= TMR_CTRL_PCS(0b1111); //Set the prescaler to 128
    TMR4_CTRL1 |= TMR_CTRL_CM(1); //Start the timer
    //2
    TMR4_CTRL2 = 0;
    TMR4_CSCTRL2 = 0;
    TMR4_LOAD2 = 0; /* Reset load register */
    TMR4_CTRL2 |= TMR_CTRL_PCS(0b1111); //Set the prescaler to 128
    TMR4_CTRL2 |= TMR_CTRL_CM(1); //Start the timer
    //3
    TMR4_CTRL3 = 0;
    TMR4_CSCTRL3 = 0;
    TMR4_LOAD3 = 0; /* Reset load register */
    TMR4_CTRL3 |= TMR_CTRL_PCS(0b1111); //Set the prescaler to 128
    TMR4_CTRL3 |= TMR_CTRL_CM(1); //Start the timer

    attachInterruptVector(IRQ_QTIMER1, TMR1_isr);
    NVIC_ENABLE_IRQ(IRQ_QTIMER1);
    attachInterruptVector(IRQ_QTIMER2, TMR2_isr);
    NVIC_ENABLE_IRQ(IRQ_QTIMER2);
    attachInterruptVector(IRQ_QTIMER3, TMR3_isr);
    NVIC_ENABLE_IRQ(IRQ_QTIMER3);
    attachInterruptVector(IRQ_QTIMER4, TMR4_isr);
    NVIC_ENABLE_IRQ(IRQ_QTIMER4);

    //Teensy 4.1 does not require .begin() to be called. This introduces a 700ms delay on startup time whilst USB is enumerated if it is called
    serialBegin();
}

void PIT_isr()
{
  bool interrupt1 = (PIT_TFLG0 & PIT_TFLG_TIF);
  bool interrupt2 = (PIT_TFLG1 & PIT_TFLG_TIF);
  bool interrupt3 = (PIT_TFLG2 & PIT_TFLG_TIF);
  bool interrupt4 = (PIT_TFLG3 & PIT_TFLG_TIF);

  if(interrupt1)      { PIT_TFLG0 = 1; idleInterrupt();  }
  else if(interrupt2) { PIT_TFLG1 = 1; boostInterrupt(); }
  else if(interrupt3) { PIT_TFLG2 = 1; vvtInterrupt();   }
  else if(interrupt4) { PIT_TFLG3 = 1; oneMSInterval();  }
  asm volatile ("dsb") ;
}

void TMR1_isr(void)
{
  //TMR1 is fuel channels 1-4
  bool interrupt1 = (TMR1_CSCTRL0 & TMR_CSCTRL_TCF1);
  bool interrupt2 = (TMR1_CSCTRL1 & TMR_CSCTRL_TCF1);
  bool interrupt3 = (TMR1_CSCTRL2 & TMR_CSCTRL_TCF1);
  bool interrupt4 = (TMR1_CSCTRL3 & TMR_CSCTRL_TCF1);

  if(interrupt1)      { TMR1_CSCTRL0 &= ~TMR_CSCTRL_TCF1; moveToNextState(fuelSchedule1); }
  else if(interrupt2) { TMR1_CSCTRL1 &= ~TMR_CSCTRL_TCF1; moveToNextState(fuelSchedule2); }
  else if(interrupt3) { TMR1_CSCTRL2 &= ~TMR_CSCTRL_TCF1; moveToNextState(fuelSchedule3); }
  else if(interrupt4) { TMR1_CSCTRL3 &= ~TMR_CSCTRL_TCF1; moveToNextState(fuelSchedule4); }
}
void TMR2_isr(void)
{
  //TMR2 is IGN channels 1-4
  bool interrupt1 = (TMR2_CSCTRL0 & TMR_CSCTRL_TCF1);
  bool interrupt2 = (TMR2_CSCTRL1 & TMR_CSCTRL_TCF1);
  bool interrupt3 = (TMR2_CSCTRL2 & TMR_CSCTRL_TCF1);
  bool interrupt4 = (TMR2_CSCTRL3 & TMR_CSCTRL_TCF1);

  if(interrupt1)      { TMR2_CSCTRL0 &= ~TMR_CSCTRL_TCF1; moveToNextState(ignitionSchedule1); }
  else if(interrupt2) { TMR2_CSCTRL1 &= ~TMR_CSCTRL_TCF1; moveToNextState(ignitionSchedule2); }
  else if(interrupt3) { TMR2_CSCTRL2 &= ~TMR_CSCTRL_TCF1; moveToNextState(ignitionSchedule3); }
  else if(interrupt4) { TMR2_CSCTRL3 &= ~TMR_CSCTRL_TCF1; moveToNextState(ignitionSchedule4); }
}
void TMR3_isr(void)
{
  //TMR3 is Fuel  channels 5-8
  bool interrupt1 = (TMR3_CSCTRL0 & TMR_CSCTRL_TCF1);
  bool interrupt2 = (TMR3_CSCTRL1 & TMR_CSCTRL_TCF1);
  bool interrupt3 = (TMR3_CSCTRL2 & TMR_CSCTRL_TCF1);
  bool interrupt4 = (TMR3_CSCTRL3 & TMR_CSCTRL_TCF1);

  if(interrupt1)      { TMR3_CSCTRL0 &= ~TMR_CSCTRL_TCF1; moveToNextState(fuelSchedule5); }
  else if(interrupt2) { TMR3_CSCTRL1 &= ~TMR_CSCTRL_TCF1; moveToNextState(fuelSchedule6); }
  else if(interrupt3) { TMR3_CSCTRL2 &= ~TMR_CSCTRL_TCF1; moveToNextState(fuelSchedule7); }
  else if(interrupt4) { TMR3_CSCTRL3 &= ~TMR_CSCTRL_TCF1; moveToNextState(fuelSchedule8); }
}
void TMR4_isr(void)
{
  //TMR4 is IGN channels 5-8
  bool interrupt1 = (TMR4_CSCTRL0 & TMR_CSCTRL_TCF1);
  bool interrupt2 = (TMR4_CSCTRL1 & TMR_CSCTRL_TCF1);
  bool interrupt3 = (TMR4_CSCTRL2 & TMR_CSCTRL_TCF1);
  bool interrupt4 = (TMR4_CSCTRL3 & TMR_CSCTRL_TCF1);

  if(interrupt1)      { TMR4_CSCTRL0 &= ~TMR_CSCTRL_TCF1; moveToNextState(ignitionSchedule5); }
  else if(interrupt2) { TMR4_CSCTRL1 &= ~TMR_CSCTRL_TCF1; moveToNextState(ignitionSchedule6); }
  else if(interrupt3) { TMR4_CSCTRL2 &= ~TMR_CSCTRL_TCF1; moveToNextState(ignitionSchedule7); }
  else if(interrupt4) { TMR4_CSCTRL3 &= ~TMR_CSCTRL_TCF1; moveToNextState(ignitionSchedule8); }
}

uint16_t freeRam()
{
    uint32_t stackTop;
    uint32_t heapTop;

    // current position of the stack.
    stackTop = (uint32_t) &stackTop;

    // current position of heap.
    void* hTop = malloc(1);
    heapTop = (uint32_t) hTop;
    free(hTop);

    // The difference is the free, available ram.
    return (uint16_t)stackTop - heapTop;
}

//This function is used for attempting to set the RTC time during compile
static  time_t getTeensy3Time()
{
  return Teensy3Clock.get();
}

void doSystemReset() { return; }
void jumpToBootloader() { return; }

//Checks if the request pin is being used for rx/tx on secondary serial. Primary (USB) serial does not need to be checked as it is not broken out to an IO on Teensy
//The Secondary serial that it checks against is based on that one set by the pinMapping
bool pinIsSerial(uint8_t pin)
{
  bool isSerial = false;

  if(&secondarySerial == &Serial1)
  {
    if( (pin == 0) || (pin == 1) ) { isSerial = true; }
  }
  else if(&secondarySerial == &Serial2)
  {
    if( (pin == 7) || (pin == 8) ) { isSerial = true; }
  }
  else if(&secondarySerial == &Serial3)
  {
    if( (pin == 14) || (pin == 15) ) { isSerial = true; }
  }
  else if(&secondarySerial == &Serial4)
  {
    if( (pin == 16) || (pin == 17) ) { isSerial = true; }
  }
  else if(&secondarySerial == &Serial5)
  {
    if( (pin == 20) || (pin == 21) ) { isSerial = true; }
  }
  else if(&secondarySerial == &Serial6)
  {
    if( (pin == 24) || (pin == 25) ) { isSerial = true; }
  }
  else if(&secondarySerial == &Serial7)
  {
    if( (pin == 28) || (pin == 29) ) { isSerial = true; }
  }
  else if(&secondarySerial == &Serial8)
  {
    if( (pin == 34) || (pin == 35) ) { isSerial = true; }
  }

  return isSerial;
}

static void setPinHysteresis(uint8_t pin)
{
  // Must reject invalid pins. digital_pin_to_info_PGM + pin is unchecked pointer
  // arithmetic, so NOT_A_PIN (255) or any pin beyond the table would index far outside
  // it and the writes below would corrupt memory or hard fault.
  //
  // This is not theoretical: a decoder that does not use the secondary/tertiary trigger
  // makes buildDecoder() set pinTrigger2/pinTrigger3 to NOT_A_PIN, and on a blank tune
  // (missing-tooth decoder) those paths are reached. With interrupts already disabled at
  // that point, the resulting fault resets the CPU before USB enumerates - which is the
  // boot loop that a freshly formatted external EEPROM produced.
  if (pin >= CORE_NUM_DIGITAL)
  {
    return;
  }

  //Refer to digital.c in the Teensyduino core for the following code
  //Refer also to Pgs 382 and 950 of the iMXRT1060 Reference Manual
  const struct digital_pin_bitband_and_config_table_struct *p;
  const uint32_t padConfig = IOMUXC_PAD_DSE(1) | IOMUXC_PAD_PKE | IOMUXC_PAD_PUE | IOMUXC_PAD_SPEED(0) | IOMUXC_PAD_HYS;

  p = digital_pin_to_info_PGM + pin;
  *(p->reg + 1) &= ~(p->mask); // TODO: atomic
  *(p->pad) = padConfig;
  *(p->mux) = 5 | 0x10;
}

uint8_t getSystemTemp(void)
{
  return trunc(InternalTemperature.readTemperatureC());
}


void boardInitRTC(void)
{
    setSyncProvider(getTeensy3Time);
}


void boardInitPins(uint8_t, pinNumbers_t &pins)
{
  //Primary trigger
  setPinHysteresis(pins.pinTrigger);
  //Secondary trigger
  setPinHysteresis(pins.pinTrigger2);
  //Tertiary trigger
  setPinHysteresis(pins.pinTrigger3);

  if(configPage2.flexEnabled > 0) { setPinHysteresis(pins.pinFlex); }
  if(configPage2.vssMode > 1) { setPinHysteresis(pins.pinVSS); }// VSS modes 2 and 3 are interrupt drive (Mode 1 is CAN)
  if(configPage10.knock_mode == KNOCK_MODE_DIGITAL) { setPinHysteresis(configPage10.knock_pin); }
}

static uint16_t getEepromWriteBlockSize(const statuses &current)
{
#if defined(USE_SPI_EEPROM)
  // Each written byte costs a WRITE_ENABLE + PAGE_PROGRAM + busy-wait poll, and a
  // W25Q page program is ~0.7ms typical / ~3ms worst case.
  //
  // IMPORTANT: this value is load-bearing for TunerStudio's burn protocol.
  // storage.cpp does `setEepromWritePending(writesRemaining==0U)` at the end of
  // savePage(), so if the budget is exhausted the page is marked as still pending and
  // the write is deferred to the main loop's saveAllPages() retry. That retry only runs
  // when storageWriteTimeoutExpired() is true, i.e. ~1 second later.
  //
  // So a budget that is too low doesn't just slow the burn down, it makes every page
  // write take a second to resume, and TS's per-page 'b' command never gets a completed
  // burn - data silently never lands. This must be high enough that a config page
  // (roughly 64-128 bytes) completes in a single savePage() call.
  //
  // 160 (20*8 with RPM==0) matches what the working STM32 SPI-flash boards use. The
  // earlier comms drop was a *different* problem - see the MC33810/transaction notes -
  // not this budget.
  uint16_t maxWrite = 20;
#else
  uint16_t maxWrite = 64;
#endif

  // Write to EEPROM more aggressively if the engine is not running
  if(current.RPM==0U)
  { 
    return maxWrite * 8U;
  } 

  return maxWrite;
}

/** @brief Get the EEPROM storage API for the board */
storage_api_t getBoardStorageApi(void)
{
  // Clear any stale markers from a previous boot, then leave a trail. If the board
  // reset-loops, whatever markers are present afterwards show how far it got.
  bootTraceReset();
  bootTrace(0x01); // getBoardStorageApi() entered
#if defined(USE_SPI_EEPROM)
  // Prefer the onboard SPI flash when it is present. If the chip cannot be detected
  // (E.g. a Teensy 4.1 board without one fitted) fall through to the internal EEPROM
  // so that the firmware still runs.
  if (initialiseFlashEEPROM())
  {
    bootTrace(0x20); // returning the SPI-flash API
    return storage_api_t{
      .read = readFlashEeprom,
      .write = writeFlashEeprom,
      .length = lengthFlashEeprom,
      .getMaxWriteBlockSize = getEepromWriteBlockSize,
    };
  }
#endif
  bootTrace(0x21); // returning the internal EEPROM API
  return getEEPROMStorageApi(getEepromWriteBlockSize);
}

/** @brief Get the PWM timer resolution in uS */
uint8_t getPwmTimerResolution(void)
{
  return 2;
}

/** @brief Default pin mapping for a blank tune.
 *
 * A freshly formatted EEPROM reads back as 0xFF, so configPage2.pinMapping is 255 and
 * the generic fallback would select board 3 (Speeduino v0.4). That is an AVR shield
 * mapping and sets up completely wrong pins on a DropBear v2, whose pinout is fixed.
 * Initialising with it put the injector/ignition outputs, including the MC33810 chip
 * selects on the shared SPI bus, onto unrelated pins - which is what made a first boot
 * on a blank external flash reset-loop.
 *
 * Board 55 is the DropBear in getPinMapping(). The Teensy 4.1 is also used by the Bear
 * Cub (56), so this is not ideal, but 55 is the mapping this firmware has always used
 * for the T4.1 target and choosing it is no worse than the previous forced 3.
 */
uint8_t getBoardDefaultPinMapping(void)
{
  return 55U; // DropBear
}

#endif
