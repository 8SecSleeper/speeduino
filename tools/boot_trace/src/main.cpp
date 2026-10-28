/*
 * Boot-trace reader for the Teensy 4.1 internal EEPROM
 * ===================================================
 *
 * The boot loop resets the CPU before USB enumerates, so there is no serial output to
 * see where it died. The firmware writes a progress marker into the Teensy's internal
 * EEPROM at each milestone instead; this sketch reads those markers back.
 *
 * Markers written by board_teensy41.cpp:
 *   0xFF  (or nothing) -> never even reached getBoardStorageApi()
 *   0x01  getBoardStorageApi() entered
 *   0x10  entering flash init
 *   0x11  MC33810 CS pins parked
 *   0x12  SPI.begin() done
 *   0x13  SPI transaction started
 *   0x14  flash detected
 *   0x15  flash NOT detected
 *   0x16  magic numbers OK -> using external flash
 *   0x17  detected but NOT formatted
 *   0x18  flash init complete
 *   0x20  returning SPI-flash storage API
 *   0x21  returning internal-EEPROM storage API
 *
 * The LAST non-0xFF marker is how far the board got before it reset.
 *
 * Usage:
 *   pio run -d tools/boot_trace -t upload
 *   then READ THE OUTPUT, and afterwards reflash the firmware:
 *   pio run -e teensy41 -t upload
 *
 * This sketch only READS the trace. It does not modify the tune.
 */

#include <Arduino.h>
#include <EEPROM.h>

static constexpr uint16_t BOOT_TRACE_BASE = 4220U;
static constexpr uint16_t BOOT_TRACE_LEN  = 48U;

static const char *describe(uint8_t v)
{
  switch (v)
  {
    case 0x01: return "getBoardStorageApi() entered";
    case 0x10: return "entering flash init";
    case 0x11: return "MC33810 CS pins parked";
    case 0x12: return "SPI.begin() done";
    case 0x13: return "SPI transaction started";
    case 0x14: return "flash DETECTED";
    case 0x15: return "flash NOT detected";
    case 0x16: return "magic numbers OK -> using external flash";
    case 0x17: return "detected but NOT formatted";
    case 0x18: return "flash init complete";
    case 0x20: return "returning SPI-flash storage API";
    case 0x21: return "returning internal-EEPROM storage API";
    default:   return "?";
  }
}

static void setup()
{
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 3000) { }

  Serial.println();
  Serial.println(F("=============================================="));
  Serial.println(F(" Speeduino boot trace (read only)"));
  Serial.println(F("=============================================="));

  uint8_t last = 0xFF;
  uint16_t count = 0;

  Serial.println(F(" raw markers:"));
  Serial.print(F("  "));
  for (uint16_t i = 0; i < BOOT_TRACE_LEN; i++)
  {
    uint8_t v = EEPROM.read(BOOT_TRACE_BASE + i);
    if (v == 0xFF) { break; }
    Serial.printf("%02X ", v);
    last = v;
    count++;
  }
  Serial.println();

  if (count == 0)
  {
    Serial.println(F(" No markers found."));
    Serial.println(F(" Either the firmware never reached getBoardStorageApi(),"));
    Serial.println(F(" or it did not run at all since the last trace reset."));
    Serial.println(F(" Make sure the firmware was flashed and the board power cycled."));
  }
  else
  {
    Serial.println();
    Serial.println(F(" decoded:"));
    for (uint16_t i = 0; i < count; i++)
    {
      uint8_t v = EEPROM.read(BOOT_TRACE_BASE + i);
      Serial.printf("   %2u. 0x%02X  %s\r\n", i + 1, v, describe(v));
    }
    Serial.println();
    Serial.printf(" LAST MARKER: 0x%02X = %s\r\n", last, describe(last));
    Serial.println(F(" The board reset at or just after this point."));
  }

  Serial.println(F("=============================================="));
  Serial.println(F(" Next: reflash the firmware with"));
  Serial.println(F("   pio run -e teensy41 -t upload"));
  Serial.println(F(" Then run this sketch again to see the new trace."));
  Serial.println(F("=============================================="));
}

void loop() { }
