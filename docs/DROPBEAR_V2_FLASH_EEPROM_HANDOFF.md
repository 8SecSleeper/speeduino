# DropBear v2 - Onboard SPI Flash as EEPROM: Full Handoff Document

**Branch:** `feature/dropbear-v2-onboard-flash-eeprom`
**Repo:** `C:\Users\laptop\Documents\GitHub\speeduino` (fork: `8SecSleeper/speeduino`, upstream `speeduino/speeduino`)
**Hardware:** Speeduino DropBear v2 mainboard + Teensy 4.1
**Status:** WORKING. Burns complete in ~17 ms and persist across power cycles. Verified on hardware.

---

## 0. TL;DR

The onboard Winbond W25Q32 flash is used as the tune storage. It is detected, formatted,
read, written, and burned. The two bugs that made this take a long time to land:

1. **The burn hung the firmware for >120 s.** Cause: the two MC33810 injector/ignition
   drivers share LPSPI4 with the flash and sit with their chip selects driven **LOW**
   (selected) at idle, so they held MISO and the flash could never answer. Every flash
   access read back all-ones, and `winbondFlashClass::read()` treated a stuck-BUSY chip by
   bailing out. Fixed by parking those selects HIGH around each flash access (§11).
2. **Writes were silently discarded.** `waitWhileBusy()` polled the status register
   immediately after a program/erase, before the chip asserts BUSY, so it returned "ready"
   while the operation was in flight and the next command (a `WREN`) was dropped. Fixed with
   a pre-poll settle (§12).

Both are timing/bus-discipline bugs, invisible to functional testing, which is why the
standalone probe worked while the firmware did not.

---

## 1. Goal

Make the DropBear v2 use its **onboard Winbond SPI flash** as the EEPROM for tune storage,
instead of the Teensy 4.1's internal emulated EEPROM (which only provides 4284 bytes).

---

## 2. Hardware facts (verified from the KiCad sources and by probing)

Source: `github.com/speeduino/Hardware` -> `DropBear/V2/DropBear.kicad_pcb`, footprint **U3**.

### 2.1 The chip

| Source | Part | Notes |
|---|---|---|
| Official BOM v2.0.1 & v2.0.2 | **W25Q32JVSS** | SOIC-8, 4 MB, plain single-bit SPI |
| Probe sketch readback | `EF 40 16` | Confirms **4 MB W25Q32** on the actual board |

### 2.2 Wiring

| Flash pin | Function | Net | Teensy pin |
|---|---|---|---|
| 1 | /CS | `MCU-D6` | **6** |
| 2 | DO / IO1 | `MISO` | 12 |
| 3 | /WP / IO2 | **+3V3** (tied high) | - |
| 4 | GND | GND | - |
| 5 | DI / IO0 | `MOSI` | 11 |
| 6 | CLK | `SCLK` | 13 |
| 7 | /HOLD / IO3 | **+3V3** (tied high) | - |
| 8 | VCC | +3V3 | - |

**Pin-numbering pitfall:** in the KiCad `Teensy4.1` symbol the `(number "N")` field is the
**module pad index**, not the Teensy pin number. Each pin carries a `name` with the real pin
number:

```
pad 8  -> "6_OUT1D"    -> Teensy pin 6   <-- flash /CS lives here
pad 10 -> "8_TX2_IN1"  -> Teensy pin 8
pad 35 -> "13_SCK_LED" -> Teensy pin 13
```

The CS net is named `MCU-D6` -> **pin 6**. An early implementation wrongly used pin 8
(Serial2 TX) and the chip was never selected.

### 2.3 Bus sharing - THE key constraint

The flash shares **LPSPI4** (the global `SPI` object, pins 11/12/13) with **both MC33810**
injector/ignition driver chips:

| Device | CS net | Teensy pin |
|---|---|---|
| U3 flash | `MCU-D6` | 6 |
| U7 MC33810 | `MC33810_2-CS` | 9 |
| U8 MC33810 | `MC33810_1-CS` | 10 |

Two consequences, both of which caused real bugs:

1. **Clock/mode must match.** `acc_mc33810.cpp` calls
   `SPI.beginTransaction(SPISettings(6000000, MSBFIRST, SPI_MODE0))`, and its runtime path
   calls bare `SPI.transfer16()` with no transaction wrapper. On Teensy 4,
   `beginTransaction()` only reprograms the port when the clock actually changes. So the
   flash **must** run at the same 6 MHz / MSB-first / MODE0. This is enforced in
   `board_teensy41.cpp` (`SPI_FLASH_CLOCK_HZ`).

2. **The MC33810s must be deselected, or they own MISO.** This is the root cause of the
   burn hang - see §11.

`/WP` and `/HOLD` are hard-tied to 3V3, so **QSPI/quad mode is physically impossible** even
with a QSPI-capable part. It must be driven as ordinary single-bit SPI.

---

## 3. Firmware architecture

### 3.1 Files changed

| File | Change |
|---|---|
| `platformio.ini` | `-DUSE_SPI_EEPROM` on `[env:teensy41]`; diagnostic env `teensy41-noSPIFlash` |
| `speeduino/board_teensy41.cpp` | Flash backend, detection, `parkSharedSpiDevices()`, `setPinHysteresis` guard, boot trace |
| `speeduino/src/SPIAsEEPROM/SPIAsEEPROM.cpp/.h` | Teensy guard, `detectDevice`/`hasMagicNumbers`/`markAvailable`, bounded busy-waits, tSHSL fix, `isFormatted()` |
| `speeduino/src/SPIAsEEPROM/winbondflash.cpp/.h` | `waitWhileBusy(timeoutMs, settleUs)`, tSHSL delay in `deselect()`, `read()` no longer aborts on busy |
| `speeduino/statuses.h`, `logger.cpp` | `StorageBackend` enum reported in `status5` bits 6-7 |
| `speeduino/init.cpp` | `getBoardDefaultPinMapping()` for blank tunes, boot trace markers |
| `speeduino/board_definition.h` | Declarations for the above |
| `speeduino/board_{avr2560,teensy35,stm32_official,native}.cpp` | `getBoardDefaultPinMapping()` returning 0 |
| `post_extra_script.py` | Survive `AutodetectUploadPort` AssertionError |
| `reference/speeduino.ini` | `storageBackend` field, indicator, log entry |
| `tools/flash_probe/` | Standalone hardware diagnostic sketch |
| `tools/eeprom_readback/` | Standalone read-only magic-number checker |
| `tools/boot_trace/` | Reads boot-progress markers from internal EEPROM |
| `tools/ecm_client/` | PC-side serial client + hardware acceptance test (§13) |
| `docs/future-eeprom-capacity.md` | Plan for growing beyond 7905 bytes |

### 3.2 Emulated EEPROM sizing

```c
static EEPROM_Emulation_Config EmulatedEEPROMMconfig{133UL, 4096UL, 31UL, 0x00100000UL};
// 133 sectors * 31 bytes = 4123 bytes, base 1 MB, sector size 4096
```

- Speeduino only addresses `0..4095` (`STORAGE_END = 0xFFF` in `storage.cpp`), so 4123 is enough.
- **Do NOT raise `EEPROM_Bytes_Per_Sector` (31):** slot size is `4096/(31+1) = 128`, exactly
  `_ReadWriteBuffer[128]`. Raising it overflows the buffer.
- Sector count was reduced 255 -> 133 purely to shorten first-boot format time.

### 3.3 No formatting on the boot path

`initialiseFlashEEPROM()` deliberately does **not** format. It:
1. `detectDevice()` - JEDEC probe only, no writes
2. `hasMagicNumbers()` - read-only check
3. `markAvailable()` - adopt an already-formatted device

Blank/unformatted -> reports `NotDetected` and **falls back to the internal EEPROM** so the
board always boots. Formatting is done from the PC with `tools/flash_probe` key `F`.

Rationale: formatting is ~6 s of blocking and, when it went wrong, produced an unbootable
board that never enumerated USB.

### 3.4 Storage backend indicator

`statuses::StorageBackend`, exposed in `status5` byte **127** bits 6-7:

| Value | Meaning |
|---|---|
| 0 | Internal EEPROM |
| 1 | SPI Flash |
| 2 | Flash Not Found |
| 3 | Flash Fallback |

INI indicator pair:
```
indicator = { storageBackend == 1 }, "Int. EEPROM", "SPI Flash", white, black, green, black
indicator = { (storageBackend == 2) || (storageBackend == 3) }, "Storage OK", "FLASH PROBLEM", white, black, red, black
```
**Decoding tip:** both white => value is `0` (Internal). The label is the *off* state.

---

## 4. Bugs found and fixed (chronological)

| # | Bug | Fix |
|---|---|---|
| 1 | CS pin read as 8 (module pad) instead of 6 (net `MCU-D6`) | `PIN_SPI_FLASH_CS = 6` |
| 2 | `Flash_SPI_Config` held `SPIClass` **by value**; Teensy 4 `SPIClass` has no default ctor -> compile error | Changed to `SPIClass *SPIport` |
| 3 | Unbounded `while(busy())` -> infinite hang, USB starved | `winbondFlashClass::waitWhileBusy(timeoutMs)` |
| 4 | MC33810 CS pins not initialised before flash use -> they could drive MISO | `deselectSharedSpiDevices()` before `SPI.begin()` |
| 5 | **tSHSL violation**: `deselect()` was a bare `digitalWrite(HIGH)`. W25Q needs >=50 ns CS-high between commands, so WREN + ERASE ran together and the erase was **silently dropped** | `delayMicroseconds(1)` in `deselect()` |
| 6 | `read()` re-ran full format on every call when `_EmulatedEEPROMAvailable` stayed false | one-shot `initAttempted` latch |
| 7 | Auto-format on the boot path -> boot loop | Format removed from boot; use `tools/flash_probe` `F` |
| 8 | Blank tune => `pinMapping==255` => forced **board 3 (v0.4 AVR shield)** mapping on a fixed-pinout board | `getBoardDefaultPinMapping()`; Teensy 4.1 returns 55 (DropBear) |
| 9 | **`setPinHysteresis()` out-of-bounds write** with `pin == NOT_A_PIN (255)`, reachable on a blank tune. Fault inside `noInterrupts()` => USB never recovered => **the boot loop** | `if (pin >= CORE_NUM_DIGITAL) return;` |
| 10 | Boot trace wrote internal EEPROM every boot | Gated behind `BOOT_TRACE_ENABLED` (off by default) |
| 11 | **MC33810s hold MISO, flash goes silent => burn hangs >120 s** | `parkSharedSpiDevices()` around every flash access (§11) |
| 12 | **BUSY-poll race** discards the second write of each emulated byte | `waitWhileBusy(timeoutMs, settleUs)` + idle guard before WREN (§12) |

**Bug #9 was the cause of the long-running boot loop.**
**Bug #11 was the cause of "burns don't persist / comms error in TunerStudio".**

---

## 5. Diagnostic tooling (keep these)

All are **separate PlatformIO projects** - flashing them **replaces the Speeduino firmware**.

### 5.1 `tools/flash_probe` - hardware bring-up
```
pio run -d tools/flash_probe -t upload
pio device monitor -d tools/flash_probe -b 115200
```
Keys: `R` JEDEC probe - `A` CS pin sweep (6,8,2,3,4 - **pin 5 is the fuel pump, excluded**) -
`S` status registers + WEL test - `D` dump - `W` write/verify - `E` erase - `T` erase timing -
`F` **format for Speeduino** - `C` clear block protect.

Has a `destructiveOpsAllowed()` interlock: refuses `W`/`F` unless JEDEC capacity is `0x16`.

### 5.2 `tools/eeprom_readback` - read-only magic-number verifier
```
pio run -d tools/eeprom_readback -t upload
```
Checks exactly what firmware `checkForMagicNumbers()` checks (133 sectors from 1 MB).
**Run, power cycle, run again** - if GOOD drops to 0 across a power cycle, writes aren't
persisting.

### 5.3 `tools/boot_trace` - black-box boot recorder
```
pio run -d tools/boot_trace -t upload
```
Reads markers written to internal EEPROM at offset 4220. Requires `BOOT_TRACE_ENABLED`.
Marker map: `0x01` storage entry - `0x10-0x18` flash init - `0x20` flash API returned -
`0x30-0x36` config load / initBoard / timers - `0x40` SD+RTC - `0x41` pin mapping (+ the
mapping ID as the next marker, e.g. `0x37`=55) - `0x42` initCAN - `0x43` aux controllers -
`0x46` trigger/knock interrupts - `0x49` interrupts disabled - `0x4A` buildDecoder -
`0x4B` boardInitPins - `0x4C` startIgnitionSchedulers - `0x4D` startFuelSchedulers -
`0x4E` VSS/FLEX attach + fuel pump - `0x45` initialisation complete.

**This tool found bug #9.** Worth keeping for any future boot-loop work.

### 5.4 `teensy41-noSPIFlash` env
A/B control build. Verified via `arm-none-eabi-nm` to contain **zero** flash-driver symbols.
This is what proved the burn hang was in the flash path: burn latency was 0.00 s on this
build and >120 s on `teensy41`. **Do not use `git stash` as a control** - all work is
committed, so stash is a no-op.

### 5.5 `tools/ecm_client` - PC-side protocol client
See §13.

---

## 6. Verification status

| Item | Status |
|---|---|
| `teensy41` builds | SUCCESS |
| `teensy41-noSPIFlash` builds | SUCCESS |
| `megaatmega2560` builds | SUCCESS |
| `black_F407VE-EEPROM-SPI` builds | SUCCESS |
| `native_base` (unit tests) | cannot build on this machine - no host `g++`. Pre-existing, unrelated. |
| Hardware: flash detected | `EF 40 16` on pin 6 |
| Hardware: flash read/write | verified by probe `W` after tSHSL fix |
| Hardware: format | 255 sectors, magic numbers verified at 0, 132, 254 |
| Hardware: boot using flash | boots, indicator green "SPI Flash" |
| **Hardware: burn completes** | **YES - 17 ms** (was >120 s, no response) |
| **Hardware: burn persists** | **YES - marker survived a power cycle** |
| **Hardware: comms stays alive during burn** | **YES** |

---

## 11. Bug #11 - MC33810s held MISO, so the flash went silent

### 11.1 The mechanism

`acc_mc33810.cpp` drives the two MC33810 chip selects from an RAII guard:

```c
mc33810_active_guard_t(mc33810_t &m) : _mc33810(m) { _mc33810.pin.setPinHigh(); } // ACTIVE
~mc33810_active_guard_t()                          { _mc33810.pin.setPinLow();  } // INACTIVE
```

So "inactive" means the CS pin is driven **LOW**. With both drivers holding their selects
low they remain on LPSPI4 and drive MISO, so the flash can never answer.

This was verified by driving pins 9/10 and probing the flash:

```
A  MC33810 CS high (boot state)  : SR1=0x00 data=C0 FF EE CC  -> responding
B  MC33810 CS low  (after init)  : SR1=0xFF data=0 0 0 0     -> SILENT
C  CS high again                 : SR1=0x00 data=C0 FF EE CC  -> responding
D  CS low again                  : SR1=0xFF data=0 0 0 0     -> SILENT
```

It recovers every time, so nothing is latched - it is purely "who owns MISO right now".

And the exact moment it breaks was captured with per-milestone status samples:

| milestone | SR1 |
|---|---|
| storage API adopted | `0x00` |
| all config pages loaded | `0x00` |
| `doUpdates()` done | `0x00` |
| `initBoard()` done | `0x00` |
| timers done | `0x00` |
| **`startIgnitionSchedulers`** | **`0xFF`** <- silent |
| `startFuelSchedulers` | `0xFF` |
| initialisation complete | `0xFF` |

`startIgnitionSchedulers()` is where `initMC33810()` runs. **That is why the board boots
fine** (the tune is loaded before the MC33810s are configured) **and only fails on a burn.**

Why it looked like a 120 s hang rather than a failure: `read()` on a busy/silent chip used
to bail out, and `FLASH_READ_TIMEOUT` combined with 128 reads per page produced roughly
128 s of stalling before the burn returned.

### 11.2 The fix

`board_teensy41.cpp`:

```c
static inline void parkSharedSpiDevices(void)
{
  digitalWriteFast(PIN_MC33810_1_CS, HIGH);
  digitalWriteFast(PIN_MC33810_2_CS, HIGH);
}
```

called at the top of `readFlashEeprom()` and `writeFlashEeprom()` - i.e. around **every**
flash access. `digitalWriteFast` because these pins are already outputs, so this is two port
writes with no `pinMode` overhead; it runs on every single byte access.

### 11.3 Corroborating report

A Discord report (`pazi88` / `JayBoyd`, #stm32, Aug 2024) describes the same failure shape on
STM32 boards: "winbond flash can be written and read one time if it's empty, once it holds
the tune any change will make the STM32 hang. Reading works fine at boot." Their mechanism
was different - the flash CS pin was left uninitialised so a `digitalWrite` hit a neighbouring
pin - but the symptom class is identical: reads fine at boot, dies when something else takes
the bus. Treat "works at boot, fails later" on a shared SPI bus as a chip-select-ownership
problem first.

---

## 12. Bug #12 - BUSY-poll race silently discarded writes

W25Q does **not** assert BUSY the instant CS rises after `PAGE_PROGRAM` / `SECTOR_ERASE`.
For a short window it still returns status `0x00`. The old code polled immediately:

```c
const uint32_t start = millis();
while (busy())                          // first poll reads stale "idle"
{
  if ((millis() - start) >= timeoutMs) { return false; }
}
return true;                            // claims "ready" while still programming
```

`FLASH_EEPROM_BaseClass::write()` performs **two** separate `writeFlashBytes()` calls per
emulated byte: (1) the value, (2) the address-translation word. Call #1 starts programming;
call #2's `WREN` then landed on a busy chip and was dropped, so its `PAGE_PROGRAM` had no
write-enable latch and was ignored. Net effect: value bytes reached flash but the
translation pointer never advanced, so `read()` saw all-ones and returned `0xFF` forever.

**Fix:** `waitWhileBusy(timeoutMs, settleUs = 0)` with a `delayMicroseconds(settleUs)` before
the first poll, and `FLASH_SETTLE_US = 50` passed after program/erase. 50 us is the same
value `tools/flash_probe` uses and is proven on this hardware. Also added an idle-wait
**before** asserting `WREN`, and `read()` now waits out a busy chip (bounded) instead of
returning a silent zero-length read.

---

## 13. Driving the hardware from a PC (how this was debugged)

`tools/ecm_client/speeduino_client.py` is a minimal Speeduino binary-protocol client. Use it
(Python from the PlatformIO venv has pyserial):

```cmd
cd tools\ecm_client
C:\Users\laptop\.platformio\penv\Scripts\python.exe speeduino_client.py COM11
C:\Users\laptop\.platformio\penv\Scripts\python.exe verify_storage.py COM11
```

Wire format, **both directions**:

```
[len_hi][len_lo][payload ...][crc32 of payload, 4 bytes BIG-endian]
```

Gotchas that cost time:
- The CRC covers the **payload only**, not the two length bytes. Omitting it makes the
  firmware abort with `SERIAL_RC_TIMEOUT (0x80)` after 400 ms and send a *well-formed* error
  frame - so it looks like a protocol mismatch, not a missing CRC.
- 16-bit values *inside* a payload (page, offset, length) are **little-endian**, while the
  frame header and CRC are big-endian.
- Wait for the full response before sending the next command - the receive state machine
  only accepts a new command when `SERIAL_INACTIVE`.
- `'A'` returns nothing; use `'r'` with `SEND_OUTPUT_CHANNELS` (48) for realtime data.
- `storageBackend` is `status5` at realtime byte **127**, bits 6-7.
- **Uploading resets the board even when the firmware is hung.** `pio run -e teensy41 -t upload`
  uses the halfkay bootloader, so it works regardless of firmware state. This makes the board
  fully recoverable without physical access.

`verify_storage.py` is the acceptance test: identity, burn latency, write -> burn -> read
back, and a persistence check. It only writes to padding bytes at the end of config page 2.

---

## 14. Known caveats / debt

- **`teensy41` env is shared with the Bear Cub (board 56).** `getBoardDefaultPinMapping()`
  returns 55 (DropBear), so a blank-tune Bear Cub gets the DropBear mapping. Needs runtime
  board identification to do properly. Not worse than the previous forced board 3.
- **`setPinHysteresis()` unchecked-index pattern may exist on other boards.** Worth auditing
  `board_avr2560.cpp` / STM32 for similar raw pin indexing reachable on a blank tune.
- **The MC33810 CS polarity looks inverted upstream** (`ACTIVE = setPinHigh()`). It is
  long-standing behaviour and was there before the 420b72cc refactor, so it was NOT changed -
  `parkSharedSpiDevices()` works around it instead. If the polarity is ever corrected
  upstream, the parking calls become redundant but harmless. Worth raising upstream, because
  holding a chip select active while idle is wrong regardless of the flash.
- **`parkSharedSpiDevices()` iterates twice per byte** on the boot path (once per
  `readFlashEeprom`). Negligible (2 register writes), but if flash latency ever matters,
  batch it at the `readFlashBytes` level instead.
- **First-boot format is a manual PC step.** Re-enable it only *after* the write path is
  proven, and keep it off the boot path.
- **Boot trace costs internal-EEPROM writes.** Leave `BOOT_TRACE_ENABLED` off in production.
  Note the boot trace and any instrumentation share offset 4220 - never enable two writers.
- **Nothing has been tested beyond `teensy41` on real hardware.** AVR/STM32 changes are
  compile-only and behaviour-preserving.
- **Git history is messy.** Several commits described work that was later reverted. Prefer
  reading the *final* file state over the commit log.

---

## 15. How to build / flash / test

```cmd
cd C:\Users\laptop\Documents\GitHub\speeduino

:: main firmware
C:\Users\laptop\.platformio\penv\Scripts\platformio.exe run -e teensy41 -t upload

:: control build (no external flash)
C:\Users\laptop\.platformio\penv\Scripts\platformio.exe run -e teensy41-noSPIFlash -t upload

:: hardware diagnostic (REPLACES firmware)
C:\Users\laptop\.platformio\penv\Scripts\platformio.exe run -d tools\flash_probe -t upload
C:\Users\laptop\.platformio\penv\Scripts\platformio.exe device monitor -d tools\flash_probe -b 115200

:: serial monitor
C:\Users\laptop\.platformio\penv\Scripts\platformio.exe device monitor -b 115200
```

`pio` is **not on PATH**. Teensy is on **COM11**.

**Reminder:** flashing any `tools/*` sketch overwrites the Speeduino firmware. Reflash
`-e teensy41` afterwards.

---

## 16. Process notes for the next agent

1. **Measure, do not guess.** Five speculative fixes were shipped in an early session and
   wasted the user's time. Everything that actually worked came from a measurement: the
   boot trace, the A/B control build, the per-milestone status samples, the bus drive test.
2. **Build observability early.** The black-box markers found bug #9 and, later,
   `boardFlashStatusSample()` narrowed bug #11 to a single init call.
3. **Bisect with a control build.** `teensy41-noSPIFlash` proved the fault was in the flash
   path in one run after much speculation.
4. **Read the datasheet timings.** tSHSL (>=50 ns CS high) and the BUSY-flag assert delay are
   both invisible to functional testing and both silently dropped commands.
5. **When a diagnostic tool works and the firmware does not, diff the two implementations
   line by line.** The probe had a 50 us settle the library lacked - that one line was the
   entire difference for bug #12.
6. **On a shared SPI bus, check chip-select ownership before anything else.** That was bug
   #11, and it presented as "the flash stopped working".
7. **Do not write internal EEPROM from the hot path.** `EEPROM.update()` erases a 4 KB sector
   and stalls the bus long enough to kill USB CDC, which hangs the board. During init it is
   fine. If you need runtime markers, use the SPI flash.
8. **Trust the user's instincts.** They suggested the tune contents were involved (correct -
   bugs #8, #9) and that another user had the same "works at boot then hangs" symptom
   (correct - bug #11).
