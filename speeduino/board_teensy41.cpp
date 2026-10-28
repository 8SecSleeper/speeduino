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

#if defined(USE_SPI_EEPROM)
/*
***********************************************************************************************************
* Emulated EEPROM in the onboard SPI flash
*
* The DropBear v2 mainboard carries a Winbond W25Q32JVSS (U3) wired to the Teensy 4.1 as a
* normal single-bit SPI device:
*   /CS = pin 8,  DI/IO0 = pin 11 (MOSI),  DO/IO1 = pin 12 (MISO),  CLK = pin 13 (SCK)
* /WP (IO2) and /HOLD (IO3) are hard-tied to +3V3 on the PCB, so quad/QSPI mode cannot be
* used even when a QSPI-capable part is fitted. The existing winbondFlashSPI driver handles it.
*
* IMPORTANT: these pins are the Teensy's LPSPI4 (the global `SPI` object) and are *shared*
* with both MC33810 injector/ignition drivers (U7/U8, CS on pins 9 and 10). The MC33810
* runtime path calls SPI.transfer16() without a transaction wrapper, and on Teensy 4
* beginTransaction() only reprograms the port when the clock actually changes. So the flash
* MUST run at the same clock/mode as the MC33810s, or the two will corrupt each other.
*/
static constexpr uint8_t  PIN_SPI_FLASH_CS   = 8U;       // DropBear v2 U3 /CS
static constexpr uint32_t SPI_FLASH_CLOCK_HZ = 6000000U; // Must match acc_mc33810.cpp

// 255 sectors * 31 bytes = 7905 bytes of emulated EEPROM. Speeduino only addresses
// 0..4095 (see STORAGE_END in storage.cpp), so this comfortably covers the requirement.
// The 1MB base address leaves the start of the chip untouched.
static EEPROM_Emulation_Config EmulatedEEPROMMconfig{255UL, 4096UL, 31UL, 0x00100000UL};
static Flash_SPI_Config SPIconfig{PIN_SPI_FLASH_CS, &SPI};
static SPI_EEPROM_Class flashEEPROM(EmulatedEEPROMMconfig, SPIconfig);

/** @brief Probe and initialise the external flash, once.
 *
 * @return true if the external flash is present and usable as emulated EEPROM.
 *
 * @note This runs from getBoardStorageApi(), which is called before the pin mapping is
 * applied, so the board ID is not yet known. Detection is therefore by probing the chip's
 * JEDEC ID. If the chip is absent the caller falls back to the Teensy's internal EEPROM,
 * so enabling USE_SPI_EEPROM is safe on Teensy 4.1 boards without the external chip.
 */
static bool initialiseFlashEEPROM(void)
{
  static int8_t flashState = -1; // -1 = not yet attempted, 0 = unavailable, 1 = ready
  if (flashState < 0)
  {
    SPI.begin();
    SPI.beginTransaction(SPISettings(SPI_FLASH_CLOCK_HZ, MSBFIRST, SPI_MODE0));
    flashState = flashEEPROM.begin(SPI, PIN_SPI_FLASH_CS);
  }
  return flashState == 1;
}

// storage_api_t adapters for the emulated EEPROM held in the external SPI flash.
// These mirror the shape of board_eeprom_adapter.hpp, but bind to flashEEPROM
// rather than the Teensy's internal EEPROM object.
static byte readFlashEeprom(uint16_t address)
{
  return flashEEPROM.read(address);
}

static void writeFlashEeprom(uint16_t address, byte val)
{
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
  // For use with the onboard Winbond SPI flash. Writes are page-programmed, and only
  // an occasional sector erase + rewrite occurs, so the block size can be lower than
  // for the internal EEPROM but should stay modest to avoid long bus stalls.
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
#if defined(USE_SPI_EEPROM)
  // Prefer the onboard SPI flash when it is present. If the chip cannot be detected
  // (E.g. a Teensy 4.1 board without one fitted) fall through to the internal EEPROM
  // so that the firmware still runs.
  if (initialiseFlashEEPROM())
  {
    return storage_api_t{
      .read = readFlashEeprom,
      .write = writeFlashEeprom,
      .length = lengthFlashEeprom,
      .getMaxWriteBlockSize = getEepromWriteBlockSize,
    };
  }
#endif
  return getEEPROMStorageApi(getEepromWriteBlockSize);
}

/** @brief Get the PWM timer resolution in uS */
uint8_t getPwmTimerResolution(void)
{
  return 2;
}

#endif
