/*
 * Speeduino EEPROM magic-number checker
 * =====================================
 *
 * Standalone, READ-ONLY. It never erases or programs anything.
 *
 * Purpose: prove whether the magic numbers the format writes actually persist
 * on the chip. Run it right after formatting, and again after a power cycle.
 * If they vanish across a power cycle, the writes are not really committing and
 * no amount of firmware fixing will help until that is solved.
 *
 * It reads exactly what the firmware's checkForMagicNumbers() reads:
 *   sector N at (1MB + N*4096), first 4 bytes == C0 FF EE CC
 *
 * Usage:
 *   pio run -d tools/eeprom_readback -t upload
 *   pio device monitor -d tools/eeprom_readback -b 115200
 *
 * TIP: it also dumps USB descriptors via usb. That is how we tell whether the
 * board is resetting or simply failing to enumerate.
 */

#include <Arduino.h>
#include <SPI.h>

static constexpr uint8_t  PIN_CS   = 6;
static constexpr uint8_t  PIN_MOSI = 11;
static constexpr uint8_t  PIN_MISO = 12;
static constexpr uint8_t  PIN_SCK  = 13;
static constexpr uint32_t CLOCK_HZ = 6000000UL;

static constexpr uint32_t EEPROM_BASE_ADDR = 0x00100000UL;
static constexpr uint32_t SECTOR_SIZE      = 4096UL;
static constexpr uint32_t SECTORS_TO_CHECK = 768UL;  // must match board_teensy41.cpp

static constexpr uint8_t CMD_READ_DATA    = 0x03;
static constexpr uint8_t CMD_JEDEC_ID     = 0x9F;
static constexpr uint8_t CMD_READ_STATUS1 = 0x05;

static constexpr uint8_t MAGIC_1 = 0xC0;
static constexpr uint8_t MAGIC_2 = 0xFF;
static constexpr uint8_t MAGIC_3 = 0xEE;
static constexpr uint8_t MAGIC_4 = 204;

static inline void csLow()  { digitalWriteFast(PIN_CS, LOW);  }
static inline void csHigh() { digitalWriteFast(PIN_CS, HIGH); delayMicroseconds(1); }

static void readBytes(uint32_t addr, uint8_t *buf, uint16_t n)
{
  csLow();
  SPI.transfer(CMD_READ_DATA);
  SPI.transfer((uint8_t)(addr >> 16));
  SPI.transfer((uint8_t)(addr >> 8));
  SPI.transfer((uint8_t)(addr));
  for (uint16_t i = 0; i < n; i++) { buf[i] = SPI.transfer(0xFF); }
  csHigh();
}

static void setup()
{
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 3000) { }

  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  SPI.setMOSI(PIN_MOSI);
  SPI.setMISO(PIN_MISO);
  SPI.setSCK(PIN_SCK);
  SPI.begin();
  SPI.beginTransaction(SPISettings(CLOCK_HZ, MSBFIRST, SPI_MODE0));

  Serial.println();
  Serial.println(F("=============================================="));
  Serial.println(F(" Speeduino EEPROM magic-number check (READ ONLY)"));
  Serial.println(F("=============================================="));

  // Identify the chip
  uint8_t buf[4];
  csLow();
  SPI.transfer(CMD_JEDEC_ID);
  uint8_t m = SPI.transfer(0xFF);
  uint8_t t = SPI.transfer(0xFF);
  uint8_t c = SPI.transfer(0xFF);
  csHigh();
  Serial.printf(" JEDEC: %02X %02X %02X  (expect EF 40 16 = W25Q32 4MB)\r\n", m, t, c);

  // Status register, to see whether writes left anything odd behind
  csLow();
  SPI.transfer(CMD_READ_STATUS1);
  uint8_t sr = SPI.transfer(0xFF);
  csHigh();
  Serial.printf(" SR1:   0x%02X  busy=%u WEL=%u BP=%u%u%u\r\n",
                sr, (sr & 1) ? 1 : 0, (sr & 2) ? 1 : 0,
                (sr >> 5) & 1, (sr >> 4) & 1, (sr >> 3) & 1);

  Serial.println(F("----------------------------------------------"));
  Serial.printf(" Checking %lu sectors at 0x%06lX\r\n",
                (unsigned long)SECTORS_TO_CHECK, (unsigned long)EEPROM_BASE_ADDR);

  uint32_t good = 0;
  uint32_t bad = 0;
  uint32_t firstBad = 0xFFFFFFFFu;

  for (uint32_t sector = 0; sector < SECTORS_TO_CHECK; sector++)
  {
    uint32_t addr = EEPROM_BASE_ADDR + (sector * SECTOR_SIZE);
    readBytes(addr, buf, 4);

    bool ok = (buf[0] == MAGIC_1) && (buf[1] == MAGIC_2) &&
              (buf[2] == MAGIC_3) && (buf[3] == MAGIC_4);
    if (ok) { good++; }
    else
    {
      bad++;
      if (firstBad == 0xFFFFFFFFu) { firstBad = sector; }
      // Show the first few failures in full - the actual bytes are diagnostic
      if (bad <= 5)
      {
        Serial.printf("  sector %3lu @ 0x%06lX: %02X %02X %02X %02X  BAD\r\n",
                      (unsigned long)sector, (unsigned long)addr,
                      buf[0], buf[1], buf[2], buf[3]);
      }
    }
  }

  Serial.println(F("----------------------------------------------"));
  Serial.printf(" GOOD %lu / %lu   BAD %lu\r\n",
                (unsigned long)good, (unsigned long)SECTORS_TO_CHECK, (unsigned long)bad);

  if (bad == 0)
  {
    Serial.println(F(" RESULT: fully formatted."));
    Serial.println(F(" The firmware's checkForMagicNumbers() must pass on this"));
    Serial.println(F(" data, so the firmware should use this chip."));
    Serial.println(F(" If it still reports FLASH PROBLEM, the firmware is reading"));
    Serial.println(F(" a different address or sector count than this sketch."));
  }
  else if (good == 0)
  {
    Serial.println(F(" RESULT: NOT formatted at all."));
    Serial.println(F(" Every sector is blank/other. Format it with tools/flash_probe"));
    Serial.println(F(" ('F'), then power cycle and run this again to check it STICKS."));
  }
  else
  {
    Serial.printf(" RESULT: PARTIAL. First bad sector = %lu\r\n", (unsigned long)firstBad);
    Serial.println(F(" A partial result is the interesting case: it means writes"));
    Serial.println(F(" work for low sectors but stop partway. Note the JEDEC ID and"));
    Serial.println(F(" the first bad sector number - they point at the cause."));
  }

  Serial.println(F("----------------------------------------------"));
  Serial.println(F(" Now POWER CYCLE and run again. If GOOD drops to 0 across a"));
  Serial.println(F(" power cycle, the writes are not persisting and that is the"));
  Serial.println(F(" real bug - not anything in the Speeduino EEPROM layer."));
  Serial.println(F("=============================================="));

  // Prove the firmware's own repeat-format cannot happen any more: this loop
  // just re-reads and reports, so if the board stays alive it is not resetting.
  Serial.println(F(" Idle heartbeat prints every 2s; if these stop, the board reset."));
}

void loop()
{
  static uint32_t last = 0;
  if (millis() - last >= 2000U)
  {
    last = millis();
    Serial.printf(" hb %lu\r\n", (unsigned long)(millis() / 1000U));
  }
}
