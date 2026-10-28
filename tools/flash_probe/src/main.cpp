/*
 * DropBear v2 onboard SPI flash diagnostic
 * ========================================
 *
 * A standalone sketch for proving out (or disproving) the onboard Winbond SPI
 * flash on a DropBear v2 mainboard, independent of the Speeduino firmware.
 *
 * Hardware, from the official hardware repo (DropBear/V2/DropBear.kicad_pcb):
 *
 *   Flash (U3, W25Q32JVSS)      Net        Teensy 4.1
 *   --------------------------  ---------  -------------------------------
 *   pin 1  /CS                  MCU-D6     pin 6
 *   pin 2  DO  / IO1            MISO       pin 12
 *   pin 3  /WP / IO2            +3V3       (tied high, no quad mode possible)
 *   pin 4  GND                  GND
 *   pin 5  DI  / IO0            MOSI       pin 11
 *   pin 6  CLK                  SCLK       pin 13
 *   pin 7  /HOLD / IO3          +3V3       (tied high, no quad mode possible)
 *   pin 8  VCC                  +3V3
 *
 * NOTE: The Teensy 4.1 is at 3.3V so no level shifting is needed. Do NOT apply 5V
 *       to the flash bus.
 *
 * IMPORTANT - this sketch is destructive if you press 'F'. It deliberately talks to
 * raw absolute flash addresses and does not understand the Speeduino wear-levelling
 * layout. Only use it on a board whose tune you have backed up.
 *
 * Answering "does the drive need to be formatted?":
 *   No. There is no filesystem and no format step required by the hardware. A blank
 *   chip reads back 0xFF, which is perfectly valid. Speeduino's EEPROM emulation
 *   writes its own magic numbers per sector on first use; that is an application
 *   concept, not a flash requirement. 'F' below just erases the region so you can
 *   watch it go from 0xFF to written data.
 */

#include <Arduino.h>
#include <SPI.h>

// ---------------------------------------------------------------------------
// Pin configuration - adjust here if your board revision differs
// ---------------------------------------------------------------------------
static constexpr uint8_t PIN_CS   = 6;   // Flash /CS
static constexpr uint8_t PIN_MOSI = 11;
static constexpr uint8_t PIN_MISO = 12;
static constexpr uint8_t PIN_SCK  = 13;

static constexpr uint8_t  PIN_CS_ALT_WRONG = 8;  // What the old firmware wrongly used
static constexpr uint32_t CLOCK_HZ         = 6000000UL; // Match the MC33810s

// Where the Speeduino EEPROM emulation lives (must match board_teensy41.cpp)
static constexpr uint32_t EEPROM_BASE_ADDR = 0x00100000UL;
static constexpr uint32_t SECTOR_SIZE      = 4096UL;

// ---------------------------------------------------------------------------
// Winbond command set (the subset we need)
// ---------------------------------------------------------------------------
static constexpr uint8_t CMD_WRITE_ENABLE  = 0x06;
static constexpr uint8_t CMD_READ_STATUS1  = 0x05;
static constexpr uint8_t CMD_READ_STATUS2  = 0x35;
static constexpr uint8_t CMD_WRITE_STATUS1 = 0x01;
static constexpr uint8_t CMD_READ_DATA     = 0x03;
static constexpr uint8_t CMD_PAGE_PROGRAM  = 0x02;
static constexpr uint8_t CMD_SECTOR_ERASE  = 0x20;
static constexpr uint8_t CMD_CHIP_ERASE    = 0xC7;
static constexpr uint8_t CMD_JEDEC_ID      = 0x9F;
static constexpr uint8_t CMD_RELEASE_PD    = 0xAB;
static constexpr uint8_t CMD_READ_UID      = 0x4B;

static constexpr uint8_t SR1_BUSY = 0x01;
static constexpr uint8_t SR1_WEL  = 0x02;

// Known Winbond JEDEC IDs (capacity byte)
struct JedecInfo { uint8_t capacity; uint32_t bytes; const char *name; };
static const JedecInfo JEDEC_TABLE[] = {
  { 0x14, 1UL * 1024 * 1024, "W25Q80  (1MB)"  },
  { 0x15, 2UL * 1024 * 1024, "W25Q16  (2MB)"  },
  { 0x16, 4UL * 1024 * 1024, "W25Q32  (4MB)"  },
  { 0x17, 8UL * 1024 * 1024, "W25Q64  (8MB)"  },
  { 0x18, 16UL * 1024 * 1024,"W25Q128 (16MB)" },
};

// ---------------------------------------------------------------------------
// Low level helpers
// ---------------------------------------------------------------------------
static void csLow()  { digitalWriteFast(PIN_CS, LOW);  }
// The W25Q requires CS high time (tSHSL) of at least 50ns between commands. On a
// 600MHz Teensy digitalWriteFast toggles in a few nanoseconds, so two back-to-back
// commands (WREN then ERASE) can run together and the second is silently dropped.
// That is exactly what makes erase/program appear to do nothing while reads work.
static void csHigh() { digitalWriteFast(PIN_CS, HIGH); delayMicroseconds(1); }

// Forward declared: clearBlockProtect() is defined before writeEnable()
static void writeEnable();

static uint8_t xfer(uint8_t v) { return SPI.transfer(v); }

static void sendAddr(uint32_t addr)
{
  xfer((uint8_t)(addr >> 16));
  xfer((uint8_t)(addr >> 8));
  xfer((uint8_t)(addr));
}

static uint8_t readStatus1()
{
  csLow();
  xfer(CMD_READ_STATUS1);
  uint8_t v = xfer(0xFF);
  csHigh();
  return v;
}

static bool waitReady(uint32_t timeoutMs = 3000)
{
  uint32_t start = millis();
  // Always allow at least a short settle: a command issued immediately after CS goes
  // high can still read a stale "not busy" status before the chip has started the
  // operation, which makes fast erases look instantaneous.
  delayMicroseconds(50);
  while ((readStatus1() & SR1_BUSY) != 0U)
  {
    if (millis() - start > timeoutMs) { return false; }
    delay(1);
  }
  return true;
}

/** @brief Read status register 2 */
static uint8_t readStatus2()
{
  csLow();
  xfer(CMD_READ_STATUS2);
  uint8_t v = xfer(0xFF);
  csHigh();
  return v;
}

/** @brief Clear the block-protect bits in status register 1 */
static bool clearBlockProtect()
{
  writeEnable();
  csLow();
  xfer(CMD_WRITE_STATUS1);
  xfer(0x00);   // SRP0=0, BP2..0=000, TB=0, SEC=0 -> nothing protected
  csHigh();
  return waitReady(500);
}

static void writeEnable()
{
  csLow();
  xfer(CMD_WRITE_ENABLE);
  csHigh();
}
// ---------------------------------------------------------------------------
// Probe
// ---------------------------------------------------------------------------
struct ProbeResult
{
  bool     responded;
  uint8_t  manufacturer;
  uint8_t  memType;
  uint8_t  capacity;
  uint32_t bytes;
  const char *name;
  bool     wasBusy;
  bool     welSet;      // Write-enable latch unexpectedly set
};

static ProbeResult probe(uint8_t csPin)
{
  ProbeResult r = {};

  digitalWrite(csPin, HIGH);
  pinMode(csPin, OUTPUT);
  delayMicroseconds(10);

  digitalWrite(csPin, LOW);
  xfer(CMD_JEDEC_ID);
  r.manufacturer = xfer(0xFF);
  r.memType      = xfer(0xFF);
  r.capacity     = xfer(0xFF);
  digitalWrite(csPin, HIGH);

  // 0x00 or 0xFF on all three means nothing drove MISO -> no chip / wrong CS
  r.responded = !((r.manufacturer == 0x00 && r.memType == 0x00 && r.capacity == 0x00) ||
                  (r.manufacturer == 0xFF && r.memType == 0xFF && r.capacity == 0xFF));

  r.bytes = 0;
  r.name  = "unknown";
  for (const auto &e : JEDEC_TABLE)
  {
    if (e.capacity == r.capacity) { r.bytes = e.bytes; r.name = e.name; break; }
  }

  if (r.responded)
  {
    uint8_t sr = readStatus1();
    r.wasBusy = (sr & SR1_BUSY) != 0U;
    r.welSet  = (sr & SR1_WEL)  != 0U;
  }
  return r;
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------
static void dumpRegion(uint32_t addr, uint16_t len)
{
  uint8_t buf[16];
  for (uint16_t off = 0; off < len; off += sizeof(buf))
  {
    csLow();
    xfer(CMD_READ_DATA);
    sendAddr(addr + off);
    for (uint8_t i = 0; i < sizeof(buf); i++) { buf[i] = xfer(0xFF); }
    csHigh();

    Serial.printf("%08lX  ", (unsigned long)(addr + off));
    for (uint8_t i = 0; i < sizeof(buf); i++) { Serial.printf("%02X ", buf[i]); }
    Serial.print(" |");
    for (uint8_t i = 0; i < sizeof(buf); i++)
    {
      char c = (char)buf[i];
      Serial.print((c >= 32 && c < 127) ? c : '.');
    }
    Serial.println('|');
  }
}

static void eraseSector(uint32_t addr)
{
  writeEnable();
  csLow();
  xfer(CMD_SECTOR_ERASE);
  sendAddr(addr);
  csHigh();
}

static void programPage(uint32_t addr, const uint8_t *data, uint16_t len)
{
  writeEnable();
  csLow();
  xfer(CMD_PAGE_PROGRAM);
  sendAddr(addr);
  for (uint16_t i = 0; i < len; i++) { xfer(data[i]); }
  csHigh();
}

static bool regionIsErased(uint32_t addr, uint16_t len)
{
  for (uint16_t off = 0; off < len; off++)
  {
    csLow();
    xfer(CMD_READ_DATA);
    sendAddr(addr + off);
    uint8_t v = xfer(0xFF);
    csHigh();
    if (v != 0xFF) { return false; }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Menu actions
// ---------------------------------------------------------------------------
static void printHeader()
{
  Serial.println();
  Serial.println(F("=============================================================="));
  Serial.println(F(" DropBear v2 onboard SPI flash diagnostic"));
  Serial.println(F("=============================================================="));
  Serial.printf(" CS=%u  MOSI=%u  MISO=%u  SCK=%u  clock=%luHz\r\n",
                PIN_CS, PIN_MOSI, PIN_MISO, PIN_SCK, (unsigned long)CLOCK_HZ);
  Serial.printf(" EEPROM region under test: 0x%06lX (+%lu bytes)\r\n",
                (unsigned long)EEPROM_BASE_ADDR, (unsigned long)SECTOR_SIZE);
  Serial.println(F("--------------------------------------------------------------"));
  Serial.println(F(" R = Read JEDEC ID / probe the chip"));
  Serial.println(F(" A = Alternate pin sweep (find which pin truly drives CS)"));
  Serial.println(F(" P = Pin continuity hint (tie MISO high vs low, watch reads)"));
  Serial.println(F(" S = Read status register"));
  Serial.println(F(" D = Dump first 128 bytes of the EEPROM region"));
  Serial.println(F(" W = Write + verify a test pattern to the EEPROM region"));
  Serial.println(F(" E = Erase that sector and confirm it reads 0xFF"));
  Serial.println(F(" T = Time a sector erase (useful for boot-time estimates)"));
  Serial.println(F(" F = FORMAT for Speeduino (all 255 sectors, magic numbers)"));
  Serial.println(F(" C = Clear block protect (if writes are being ignored)"));
  Serial.println(F("--------------------------------------------------------------"));
  Serial.println(F(" NOTE: press F before flashing the firmware. It writes the magic"));
  Serial.println(F(" numbers so the firmware skips its own slow first-boot format."));
  Serial.println(F("--------------------------------------------------------------"));
}

static void doProbe()
{
  Serial.println(F("\r\n-- JEDEC ID probe (CS = pin 6) --"));
  ProbeResult r = probe(PIN_CS);

  Serial.printf("  raw bytes: manuf=0x%02X  type=0x%02X  capacity=0x%02X\r\n",
                r.manufacturer, r.memType, r.capacity);

  if (!r.responded)
  {
    Serial.println(F("  RESULT: no response."));
    Serial.println(F("  All three bytes were 0x00 or 0xFF, so nothing drove MISO."));
    Serial.println(F("  Checks: chip populated? 3V3 present on pin 8?"));
    Serial.println(F("          CS really on pin 6? Try 'A' to sweep pins."));
    return;
  }

  Serial.println(F("  RESULT: chip responded."));
  if (r.manufacturer == 0xEF) { Serial.println(F("  Manufacturer 0xEF = Winbond. Good.")); }
  else { Serial.printf("  WARNING: manufacturer 0x%02X is not Winbond (0xEF).\r\n", r.manufacturer); }

  Serial.printf("  Device: %s, %lu bytes\r\n", r.name, (unsigned long)r.bytes);
  if (r.wasBusy) { Serial.println(F("  WARNING: chip reports BUSY at idle.")); }
  if (r.welSet)  { Serial.println(F("  NOTE: write-enable latch is set (left over from an aborted write?)")); }
}

static void doSweep()
{
  Serial.println(F("\r\n-- CS pin sweep --"));
  Serial.println(F(" Reading JEDEC ID with each candidate pin driven as CS."));
  Serial.println(F(" The real CS should be the only one returning EF 40 xx."));
  Serial.println(F(" (Bus noise can make others look non-zero, so look for 0xEF specifically.)\r\n"));

  // Only pins that are harmless to drive on a DropBear v2 are swept. Deliberately
  // EXCLUDED because they are wired to real hardware in the Speeduino DropBear mapping:
  //   pin 5      = pinFuelPump   -> driving it would run the fuel pump
  //   pin 9, 10  = MC33810 #1/#2 chip selects
  //   pin 7, 8   = Serial2 RX/TX
  // Pin 8 is included below anyway because the firmware used to wrongly treat it as CS,
  // so proving it is NOT the flash is useful. It only breaks secondary serial while
  // this test sketch runs, which does not matter here.
  static const uint8_t candidates[] = { 6, 8, 2, 3, 4 };
  for (uint8_t pin : candidates)
  {
    ProbeResult r = probe(pin);
    Serial.printf("  pin %2u : %02X %02X %02X  %s%s\r\n",
                  pin, r.manufacturer, r.memType, r.capacity,
                  r.responded ? "responded" : "silent",
                  (r.manufacturer == 0xEF) ? "  <== WINBOND" : "");
    digitalWrite(pin, HIGH);
  }
}

static void doStatus()
{
  Serial.println(F("\r\n-- Status registers --"));
  uint8_t sr1 = readStatus1();
  uint8_t sr2 = readStatus2();
  Serial.printf("  SR1 = 0x%02X  busy=%u WEL=%u BP=%u%u%u TB=%u SEC=%u SRP0=%u\r\n",
                sr1,
                (sr1 & 0x01) ? 1 : 0,
                (sr1 & 0x02) ? 1 : 0,
                (sr1 & 0x20) ? 1 : 0, (sr1 & 0x10) ? 1 : 0, (sr1 & 0x08) ? 1 : 0,
                (sr1 & 0x40) ? 1 : 0,
                (sr1 & 0x80) ? 1 : 0,
                (sr1 & 0x80) ? 1 : 0);
  Serial.printf("  SR2 = 0x%02X  SRP1=%u QE=%u WPS=%u CMP=%u SUS=%u\r\n",
                sr2,
                (sr2 & 0x01) ? 1 : 0,
                (sr2 & 0x02) ? 1 : 0,
                (sr2 & 0x04) ? 1 : 0,
                (sr2 & 0x40) ? 1 : 0,
                (sr2 & 0x80) ? 1 : 0);

  uint8_t bp = (sr1 >> 2) & 0x07;
  if (bp != 0u)
  {
    Serial.printf("  WARNING: BP bits = %u%u%u, so part of the chip is BLOCK PROTECTED.\r\n",
                  (bp >> 2) & 1, (bp >> 1) & 1, bp & 1);
    Serial.println(F("  Protected regions silently ignore erase and program commands."));
  }
  else
  {
    Serial.println(F("  No block protection set."));
  }
  if (sr1 == 0xFF || sr1 == 0x00)
  {
    Serial.println(F("  Suspicious: SR1 is all-ones/all-zeros, MISO may be floating."));
  }

  Serial.println(F("\r\n-- Write-enable latch test --"));
  uint8_t before = readStatus1();
  writeEnable();
  delayMicroseconds(50);
  uint8_t after = readStatus1();
  Serial.printf("  WEL before WREN = %u, after WREN = %u -> %s\r\n",
                (before & 0x02) ? 1 : 0, (after & 0x02) ? 1 : 0,
                (after & 0x02) ? "write enable WORKS" : "write enable FAILED (chip is protected)");
}

static void doClearProtect()
{
  Serial.println(F("\r\n-- Clear block protect --"));
  Serial.println(F("  NOTE: if SR1 bit7 (SRP0) was already set, the protection is"));
  Serial.println(F("  permanent until the board is POWER CYCLED. If this reports"));
  Serial.println(F("  failure, power cycle and try again."));

  uint8_t before = readStatus1();
  Serial.printf("  SR1 before = 0x%02X\r\n", before);
  if (!clearBlockProtect()) { Serial.println(F("  write status TIMEOUT")); return; }
  uint8_t after = readStatus1();
  Serial.printf("  SR1 after  = 0x%02X\r\n", after);
  if (((after >> 2) & 0x07) == 0u) { Serial.println(F("  Block protect is now clear.")); }
  else { Serial.println(F("  Still protected. Power cycle the board and retry.")); }
}

static void doDump()
{
  Serial.println(F("\r\n-- Dump of the EEPROM region --"));
  dumpRegion(EEPROM_BASE_ADDR, 128);
  Serial.println(F("  A freshly erased / never written region reads all FF."));
}

static void doWriteTest()
{
  Serial.println(F("\r\n-- Write + verify test pattern --"));

  ProbeResult r = probe(PIN_CS);
  if (!r.responded) { Serial.println(F("  Aborting: no chip on CS pin 6.")); return; }

  uint8_t pattern[32];
  for (uint8_t i = 0; i < sizeof(pattern); i++) { pattern[i] = (uint8_t)(0xA5 ^ i); }

  Serial.print(F("  erasing sector... "));
  uint32_t t0 = micros();
  eraseSector(EEPROM_BASE_ADDR);
  if (!waitReady()) { Serial.println(F("TIMEOUT")); return; }
  Serial.printf("done in %lu us\r\n", (unsigned long)(micros() - t0));

  Serial.print(F("  programming 32 bytes... "));
  programPage(EEPROM_BASE_ADDR, pattern, sizeof(pattern));
  if (!waitReady()) { Serial.println(F("TIMEOUT")); return; }
  Serial.println(F("done"));

  uint8_t readback[32];
  csLow();
  xfer(CMD_READ_DATA);
  sendAddr(EEPROM_BASE_ADDR);
  for (uint8_t i = 0; i < sizeof(readback); i++) { readback[i] = xfer(0xFF); }
  csHigh();

  bool ok = true;
  for (uint8_t i = 0; i < sizeof(pattern); i++)
  {
    if (pattern[i] != readback[i]) { ok = false; }
  }

  if (ok) { Serial.println(F("  RESULT: write and readback MATCH. Flash is writable.")); }
  else
  {
    Serial.println(F("  RESULT: MISMATCH."));
    Serial.print(F("    wrote: "));
    for (uint8_t i = 0; i < sizeof(pattern); i++) { Serial.printf("%02X ", pattern[i]); }
    Serial.println();
    Serial.print(F("    read : "));
    for (uint8_t i = 0; i < sizeof(readback); i++) { Serial.printf("%02X ", readback[i]); }
    Serial.println();
  }
}

static void doErase()
{
  Serial.println(F("\r\n-- Erase + confirm 0xFF --"));
  Serial.print(F("  erasing sector... "));
  eraseSector(EEPROM_BASE_ADDR);
  if (!waitReady()) { Serial.println(F("TIMEOUT")); return; }
  Serial.println(F("done"));

  bool blank = regionIsErased(EEPROM_BASE_ADDR, 64);
  Serial.printf("  first 64 bytes all 0xFF: %s\r\n", blank ? "YES" : "NO");
}

// ---------------------------------------------------------------------------
// Speeduino-compatible format
//
// This replicates exactly what SPI_EEPROM_Class::initialize()/clear() does, so the
// firmware finds a valid device and skips its own format on boot.
//
// Each of the 255 sectors gets the 4 "magic number" bytes at its start:
//     C0 FF EE CC
//   where CC = EEPROM_VERSION (204 = 0xCC for v2.0.4). If Speeduino's
//   EEPROM_VERSION constant changes, FORMAT_MAGIC_4 below must be updated to match,
//   or the firmware will decide the device needs formatting again.
//
// Note the leading offset: the emulated EEPROM lives at EEPROM_BASE_ADDR, so sector
// N's magic numbers are at EEPROM_BASE_ADDR + N*4096.
// ---------------------------------------------------------------------------
static constexpr uint8_t FORMAT_MAGIC_1 = 0xC0;
static constexpr uint8_t FORMAT_MAGIC_2 = 0xFF;
static constexpr uint8_t FORMAT_MAGIC_3 = 0xEE;
static constexpr uint8_t FORMAT_MAGIC_4 = 204;
static constexpr uint32_t FORMAT_SECTORS = 255;
static constexpr uint32_t FORMAT_VERIFY_SECTOR = 132; // must be > 1; never checked before

static void doFormat()
{
  Serial.println(F("\r\n-- Format for Speeduino --"));
  Serial.printf("  Writing magic numbers to %lu sectors at 0x%06lX..0x%06lX\r\n",
                (unsigned long)FORMAT_SECTORS,
                (unsigned long)EEPROM_BASE_ADDR,
                (unsigned long)(EEPROM_BASE_ADDR + (FORMAT_SECTORS - 1) * SECTOR_SIZE));

  ProbeResult r = probe(PIN_CS);
  if (!r.responded) { Serial.println(F("  Aborting: no chip responded.")); return; }

  const uint8_t magic[4] = { FORMAT_MAGIC_1, FORMAT_MAGIC_2, FORMAT_MAGIC_3, FORMAT_MAGIC_4 };
  uint32_t t0 = millis();
  uint32_t failures = 0;

  for (uint32_t sector = 0; sector < FORMAT_SECTORS; sector++)
  {
    uint32_t addr = EEPROM_BASE_ADDR + (sector * SECTOR_SIZE);

    eraseSector(addr);
    if (!waitReady(5000)) { failures++; continue; }

    programPage(addr, magic, sizeof(magic));
    if (!waitReady(5000)) { failures++; continue; }

    if ((sector % 25U) == 0U)
    {
      Serial.printf("  sector %3lu / %lu ...\r\n",
                    (unsigned long)sector, (unsigned long)FORMAT_SECTORS);
    }
  }

  uint32_t elapsed = millis() - t0;
  Serial.printf("  done in %lu ms, %lu failure(s)\r\n",
                (unsigned long)elapsed, (unsigned long)failures);

  // Verify a HIGH sector that the firmware never touched, so "OK" proves this tool
  // actually wrote, rather than merely reading back data that was already there.
  Serial.println(F("  verifying..."));
  bool ok = true;
  const uint32_t verifyList[3] = { 0u, FORMAT_VERIFY_SECTOR, FORMAT_SECTORS - 1u };
  for (uint32_t sector : verifyList)
  {
    uint32_t addr = EEPROM_BASE_ADDR + (sector * SECTOR_SIZE);
    uint8_t buf[4];
    csLow();
    xfer(CMD_READ_DATA);
    sendAddr(addr);
    for (uint8_t i = 0; i < 4; i++) { buf[i] = xfer(0xFF); }
    csHigh();

    bool good = (buf[0] == FORMAT_MAGIC_1) && (buf[1] == FORMAT_MAGIC_2) &&
                (buf[2] == FORMAT_MAGIC_3) && (buf[3] == FORMAT_MAGIC_4);
    Serial.printf("    sector %3lu at 0x%06lX: %02X %02X %02X %02X  %s\r\n",
                  (unsigned long)sector, (unsigned long)addr,
                  buf[0], buf[1], buf[2], buf[3], good ? "OK" : "BAD");
    ok = ok && good;
  }

  if (ok) { Serial.println(F("  RESULT: formatted. The firmware should now boot without formatting.")); }
  else    { Serial.println(F("  RESULT: FAILED. Do not flash the firmware yet; report these bytes.")); }
}

static void doTiming()
{
  Serial.println(F("\r\n-- Sector erase timing --"));
  Serial.println(F("  Measuring worst-case blocking time for one 4KB erase."));
  Serial.println(F("  Speeduino formats ALL sectors on first boot, so multiply by your"));
  Serial.println(F("  sector count (255 in the current config).\r\n"));

  uint32_t total = 0;
  for (uint8_t i = 0; i < 3; i++)
  {
    uint32_t t0 = micros();
    eraseSector(EEPROM_BASE_ADDR);
    bool ok = waitReady(5000);
    uint32_t dt = micros() - t0;
    total += dt;
    Serial.printf("  run %u: %lu us %s\r\n", i + 1, (unsigned long)dt, ok ? "" : "(TIMEOUT)");
  }
  uint32_t avg = total / 3;
  Serial.printf("  average: %lu us\r\n", (unsigned long)avg);
  Serial.printf("  estimate for 255 sectors: %lu ms\r\n", (unsigned long)((avg * 255UL) / 1000UL));
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------
void setup()
{
  Serial.begin(115200);
  uint32_t start = millis();
  while (!Serial && (millis() - start) < 2000) { }

  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);

  SPI.setMOSI(PIN_MOSI);
  SPI.setMISO(PIN_MISO);
  SPI.setSCK(PIN_SCK);
  SPI.begin();
  SPI.beginTransaction(SPISettings(CLOCK_HZ, MSBFIRST, SPI_MODE0));

  printHeader();
  doProbe();
  Serial.println(F("\r\n Press a key from the menu above."));
}

void loop()
{
  if (!Serial.available()) { return; }

  char c = (char)Serial.read();
  switch (toupper(c))
  {
    case 'R': doProbe();     break;
    case 'A': doSweep();     break;
    case 'P':
      Serial.println(F("\r\n-- MISO idle test --"));
      Serial.println(F("  This sketch cannot drive MISO from software. Instead, watch the"));
      Serial.println(F("  status byte from 'S': a floating MISO reads 0xFF, a driven chip"));
      Serial.println(F("  returns sane values (0x00 or 0x02)."));
      doStatus();
      break;
    case 'S': doStatus();    break;
    case 'D': doDump();      break;
    case 'W': doWriteTest(); break;
    case 'E': doErase();     break;
    case 'T': doTiming();    break;
    case 'F': doFormat();    break;
    case 'C': doClearProtect(); break;
    default:
      Serial.println(F("  Unknown key. R/A/P/S/D/W/E/T/F/C."));
      break;
  }
  Serial.println(F("\r\n Press a key from the menu."));
}
