# Implementation Plan — Use DropBear v2 onboard SPI flash as EEPROM

## 1. Request

Switch the DropBear v2 (Teensy 4.1) build from the **Teensy's internal emulated EEPROM**
(4284 bytes in the top of the T4.1 program flash) to the **onboard Winbond SPI flash chip
on the DropBear v2 mainboard**.

---

## 2. Hardware verification (done against the official hardware repo)

Source: `github.com/speeduino/Hardware` → `DropBear/V2/`

### 2.1 Which chip is actually on the board

`DropBear/V2/BOM/DropBear v2.0.1.csv` **and** `v2.0.2.csv` both list:

| Ref | Value | Footprint | Mfr P/N |
|-----|-------|-----------|---------|
| **U3** | **W25Q32JVSS** | `SOIC-8_5.23x5.23mm_P1.27mm` | Winbond **W25Q32JVSSIQ** |

So the *published* v2 design uses a **W25Q32JV (32 Mbit / 4 MB), plain SOIC-8, single-bit SPI** part.

⚠️ **You said W25Q64JVXGIM.** That is a different package (XSON-8) and a different density
(64 Mbit / 8 MB). This matters only for *sizing*, not for the code — the driver auto-detects
both. See §7 Q1.

### 2.2 Actual wiring (extracted from `DropBear.kicad_pcb` netlist, footprint U3)

| Flash pin | Function | Net | Teensy 4.1 module pad | Teensy pin |
|-----------|----------|-----|----------------------|-----------|
| 1 | /CS | `MCU-D6` | 8 (`6_OUT1D`) | **8** |
| 2 | DO / IO1 | `MISO` | 14 (`12_MISO_MQSL`) | 12 |
| 3 | /WP / IO2 | **+3V3** (tied high) | — | — |
| 4 | GND | `GND` | — | — |
| 5 | DI / IO0 | `MOSI` | 13 (`11_MOSI_CTX1`) | 11 |
| 6 | CLK | `SCLK` | 35 (`13_SCK_LED`) | 13 |
| 7 | /HOLD / IO3 | **+3V3** (tied high) | — | — |
| 8 | VCC | `+3V3` | — | — |

**Conclusion: the chip is wired for single-bit SPI only.** `/WP` and `/HOLD` are hard-tied to
3V3, so **QSPI / quad mode cannot be used** even if a QSPI-capable part (W25Q64JVXGIM) is fitted.
It must be driven as ordinary SPI. This is exactly what the existing `winbondFlashSPI` driver does.

### 2.3 Bus sharing — the critical constraint

Netlist cross-reference shows the flash **shares MOSI / MISO / SCLK with both MC33810
injector/ignition driver chips**:

| Ref | Part | CS net | CS (Teensy pin) |
|-----|------|--------|-----------------|
| U3 | W25Q32JVSS | `MCU-D6` | **8** |
| U7 | MC33810 | `MC33810_2-CS` | 11 (module pad 11 → `9_OUT1C`) |
| U8 | MC33810 | `MC33810_1-CS` | 10 (module pad 12 → `10_CS_MQSR`) |

All three are on **LPSPI4**, i.e. the Teensyduino global `SPI` object. Pins 11/12/13 are the
`SPI` MOSI/MISO/SCK, and the DropBear pin mapping already marks them as injector pins
"so that nothing else will set them as inputs" (`pinMapping.cpp`, `getDropBearMapping()`).

**Consequence:** the flash must never leave LPSPI4 in a different clock/mode than the MC33810s
expect. MC33810 init calls `SPI.beginTransaction(SPISettings(6000000, MSBFIRST, SPI_MODE0))`
(`acc_mc33810.cpp:103`) and its runtime ISR path calls bare `SPI.transfer16()` **with no
transaction wrapper**. On Teensy 4, `beginTransaction()` only reprograms the port when the
requested clock *differs* from the cached `_clock`, and `endTransaction()` does **not** restore
the previous settings.

➡️ **Design rule adopted: run the flash at the same 6 MHz / MSB-first / MODE0 as the MC33810s
and never change it.** That makes contention impossible without touching ISR code.

---

## 3. Existing infrastructure (reused, not reinvented)

- `speeduino/src/SPIAsEEPROM/SPIAsEEPROM.{h,cpp}` — wear-levelled EEPROM emulation over flash
  (log-structured, address-translation bitmap, sector erase + rewrite). Already used by the
  STM32 boards via `USE_SPI_EEPROM`.
- `speeduino/src/SPIAsEEPROM/winbondflash.{h,cpp}` — `winbondFlashSPI` driver. **Already knows
  W25Q32 (`0x4016`) and W25Q64 (`0x4017`)** and supports `autoDetect`. No driver changes needed.
- `speeduino/board_eeprom_adapter.hpp` — `getEEPROMStorageApi()`, adapts the global `EEPROM`
  object to `storage_api_t`.
- `speeduino/storage_api.h` — `storage_api_t { read, write, length, getMaxWriteBlockSize }`.
- Precedent for the flag: `board_teensy35.cpp` already uses `#ifdef USE_SPI_EEPROM` as a boolean.

Constraint check: Speeduino needs a **4096-byte** address space (`storage.cpp:59`,
`STORAGE_END = 0xFFF`). The config used by the STM32 SPI path,
`{255UL, 4096UL, 31, base}` → 255 × 31 = **7905 emulated bytes**, satisfies this.

---

## 4. Proposed changes

### 4.1 `platformio.ini` — enable the flag for the T4.1 env

```ini
[env:teensy41]
build_flags = -DTEENSY_INIT_USB_DELAY_AFTER=40 -DUSE_SPI_EEPROM
```

### 4.2 `speeduino/src/SPIAsEEPROM/SPIAsEEPROM.cpp` — make the SPI class portable

Line 250: broaden the guard so `SPI_EEPROM_Class` compiles on Teensy as well as STM32.

```cpp
#if defined(ARDUINO_ARCH_STM32) || defined(CORE_TEENSY)
```
(matching `#endif` at line 294.)

Nothing else in that block is STM32-specific — it only uses `SPIClass`, `SPISettings`,
`pinMode`/`digitalWrite` and `winbondFlashSPI`.

Also change the lazy-init clock in `SPI_EEPROM_Class::read()` (line 260) from
`22500000` → `6000000` to match §2.3, so an accidental lazy init can never desynchronise
the shared bus.

### 4.3 `speeduino/board_teensy41.cpp` — new storage backend + safe fallback

**a) Includes** — replace the unconditional `#include <EEPROM.h>` with a branch:

```cpp
#if defined(USE_SPI_EEPROM)
  #include "src/SPIAsEEPROM/SPIAsEEPROM.h"
#else
  #include <EEPROM.h>
#endif
```

**b) Pin / config constants** (new, near the top of the file):

```cpp
static constexpr uint8_t  PIN_SPI_FLASH_CS   = 8;       // DropBear v2 U3 /CS = Teensy pin 8
static constexpr uint32_t SPI_FLASH_CLOCK_HZ = 6000000; // Match MC33810: shared LPSPI4 bus
```

**c) Backend object**

```cpp
#if defined(USE_SPI_EEPROM)
// Emulated EEPROM in the DropBear v2 onboard Winbond W25Q32/W25Q64 SPI flash (U3).
EEPROM_Emulation_Config EmulatedEEPROMMconfig{
  255UL,        // sectors used
  4096UL,       // flash sector size
  31UL,         // emulated EEPROM bytes per sector  -> 255*31 = 7905 bytes
  0x00100000UL  // 1 MB into the chip: leaves the first sectors free
};
Flash_SPI_Config SPIconfig{PIN_SPI_FLASH_CS, SPI};
SPI_EEPROM_Class EEPROM(EmulatedEEPROMMconfig, SPIconfig);
#endif
```

**d) One-shot init with graceful fallback** — called from `getBoardStorageApi()`
(which runs before pin mapping, so the board ID isn't known yet; detection is therefore
done by probing the chip's JEDEC ID, not by board ID):

```cpp
#if defined(USE_SPI_EEPROM)
static bool initialiseFlashEeprom(void)
{
  static int8_t flashState = -1;         // -1 = not tried, 0 = unavailable, 1 = ready
  if (flashState < 0)
  {
    SPI.begin();
    // Same settings the MC33810s use - see the note above initialiseFlashEeprom()
    SPI.beginTransaction(SPISettings(SPI_FLASH_CLOCK_HZ, MSBFIRST, SPI_MODE0));
    flashState = EEPROM.begin(SPI, PIN_SPI_FLASH_CS);
  }
  return flashState == 1;
}
#endif
```

**e) Storage API selection**

```cpp
storage_api_t getBoardStorageApi(void)
{
#if defined(USE_SPI_EEPROM)
  if (initialiseFlashEeprom() && EEPROM._EmulatedEEPROMAvailable)
  {
    return { EEPROM.read, EEPROM.write, EEPROM.length, getEepromWriteBlockSize };
  }
#endif
  return getEEPROMStorageApi(getEepromWriteBlockSize); // Teensy internal EEPROM fallback
}
```

**f) Write-throughput policy** — `getEepromWriteBlockSize()` gains the SPI case,
mirroring `board_teensy35.cpp` / `board_stm32_official.cpp`:

```cpp
#if defined(USE_SPI_EEPROM)
  uint16_t maxWrite = 20U;   // SPI flash: page-program + rare sector erase
#else
  uint16_t maxWrite = 64U;
#endif
```

---

## 5. Verification plan

I cannot run the hardware, so verification is layered:

1. **Compile** the `teensy41` env (`pio run -e teensy41`) — must build clean, and the map
   must show `SPI_EEPROM_Class` symbols were linked in. *(Requires the Teensy toolchain;
   I'll attempt it and report if it isn't installed.)*
2. **Compile** a non-`USE_SPI_EEPROM` build to prove the fallback path still compiles
   (temporarily remove the flag, or build `teensy36`/`teensy35` which share the file pattern).
3. **Static self-review** of the changed regions: symbol collisions (`EEPROM` name clash),
   include guards, `_EmulatedEEPROMAvailable` visibility, signed/unsigned on `length()`
   (`uint16_t` vs the 7905 size), and that `pinMode(8, OUTPUT)` is set by
   `SPI_EEPROM_Class::begin()` before any CS toggle.
4. **On-target smoke test (you)** — flash, connect TunerStudio:
   - board reports as DropBear, tune loads/saves, `Tools → Burns` persists across power cycle
   - existing Teensy-internal tune is *not* migrated (different device) → reload the tune
   - verify injectors/ignition still fire (proves the 6 MHz bus sharing is sound)

## 6. Risks / notes

- **Different storage device ⇒ no automatic migration.** Settings currently in the T4.1's
  internal EEPROM will read back as blank. TunerStudio must re-write the tune once.
  That is inherent to the request, not a bug.
- **Bench-test the chip first**: the JEDEC ID probe determines which backend is used. If the
  chip isn't detected the firmware silently falls back to internal EEPROM, which is safe but
  may look like "nothing changed". The plan therefore adds nothing that could brick a board.
- **Sector-erase stall**: filling a sector (128 writes) triggers a 4 KB erase plus a rewrite
  of 31 bytes — a multi-ms blocking event, same as the existing STM32 SPI boards. Mitigated by
  the write-block-size limit and the `RPM==0` boost.
- No use of QSPI/quad mode is possible because `/WP` and `/HOLD` are hard-tied to 3V3 (§2.2).

---

## 7. Open questions — please confirm before I implement

**Q1 — Chip density.** The official v2 BOM says *W25Q32JVSS (4 MB, SOIC-8)*; you said
*W25Q64JVXGIM (8 MB, XSON-8)*. The code auto-detects either, so this is not a blocker —
but it changes the numbers used in the plan's comments and the region I reserve.
Shall I write it chip-agnostic (auto-detect, 1 MB base offset, no hard-coded density), or
pin it to W25Q64?

**Q2 — Flag scope.** `-DUSE_SPI_EEPROM` on the shared `[env:teensy41]` affects *all* T4.1
builds (DropBear v2 **and** the Bear Cub mapping). With the runtime fallback in §4.3 that is
safe — a board without the chip just uses internal EEPROM. Prefer that, or should I add a
separate `[env:teensy41_dropbear]` env so the stock `teensy41` build stays byte-identical?

**Q3 — Emulated EEPROM size.** Keep the proven 7905-byte layout (`255 × 31`, same as the
STM32 SPI boards, only 4 KB actually used), or enlarge it to use more of the 4/8 MB?
Speeduino only ever addresses `0..4095`, so enlarging buys nothing without changing
`STORAGE_END` in `storage.cpp` — which would break tune compatibility. I recommend
**keep 7905**.

**Q4 — Burn while running.** Should flash writes be blocked while the engine is running
(safest, avoids a 4 KB erase stall mid-cycle), or keep the existing "20 writes/loop, ×8 when
RPM==0" behaviour for consistency with the other boards? I recommend **keep existing
behaviour**.

I'll implement as soon as you approve (and answer Q1–Q4).
