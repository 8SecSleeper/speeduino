# AGENTS.md — read this before changing anything

Guidance for AI coding agents working in this repository. Read this file first, then
`docs/DROPBEAR_V2_FLASH_EEPROM_HANDOFF.md` for the full history of the storage work.

---

## 1. Target platform — one target only

This fork exists to run Speeduino on **one** board. Do not spend effort on other targets.

| Item | Value |
|---|---|
| Mainboard | **Speeduino DropBear v2** |
| MCU board | **Teensy 4.1** (i.MX RT1062, 600 MHz) |
| PlatformIO env | `teensy41` |
| Serial port | `COM11` @ 115200 |
| Pin mapping ID | **55** (DropBear) |
| Connectivity | USB. **SD and RTC are compiled in by default** — see §1A. Knock: one conditioned input. |

**No other environment is a target.** `megaatmega2560`, `black_F407VE-*`, `teensy35`, Bear Cub /
board 56 and the rest are **not supported and are not required to compile**. Do not spend effort on
them, do not gate new code behind `#if defined(...)` checks for them, and do not treat a break in
them as a build failure to fix. If a change breaks one of them, that is acceptable and expected.

The single exception is `native_base` / `native_code_coverage`, which are the **unit-test host**
environments, not target boards. They are worth keeping buildable so `test/` stays usable, but they
are not a release gate and they need a host `gcc`/`g++` that is not installed on every dev machine.

**The only build that must pass is `teensy41`.**

---

## 1A. Board capabilities — do not under-state these

An earlier version of this file said *"No SD, RTC or knock module has been validated."* That was
misleading and caused wasted work. The facts:

| Capability | Reality | Evidence |
|---|---|---|
| **RTC** | **Compiled in.** Needs a coin cell fitted on the board. | `board_teensy41.h:70` `#define RTC_ENABLED`; `:72` `RTC_LIB_H "TimeLib.h"` |
| **SD card** | **Compiled in, via SDIO not SPI.** Fastest Teensy mode. | `board_teensy41.h:71` `#define SD_LOGGING`; `:73` `SD_CONFIG SdioConfig(FIFO_SDIO)` |
| **Knock** | One conditioned analog/digital input. No multi-channel. | `config10.knock_mode`, single `knock_pin` |

**Consequences:**

- Do not treat "logging" as absent. The limitation is **rate** (max 30 Hz, `config13.onboard_log_file_rate`), not hardware.
- SD is a legitimate home for bulk data — multi-tune libraries, high-rate logs, reference curves. It is **not** a substitute for EEPROM on the boot path (SD can be absent, unformatted, or slow).
- Pins **42–47 are reserved for the SD card** (`board_teensy41.h:78-88` `pinIsReserved`). Do not assign them.

### 1A.1 Pin 34 is multiplexed by design, not buggy

DropBear documents pin 34 ("Spare Digital In 2") as *"12 V or ground switching digital input. Can be
used for VSS, Idle Up etc. This is also the default input for a secondary Cam input if using dual
VVT."* It therefore appears as **both** `pinVSS` and `pinTrigger3` in the DropBear mapping
(`pinMapping.cpp:1231,1238`). The two uses are **mutually exclusive** — that is intentional. Do not
"fix" it. Pin 35 is likewise `pinWMIEmpty` by default and is available as a second speed input.

### 1A.2 Speed inputs are user-assignable

`vssPin` is a 6-bit field exposed in TunerStudio accepting **any pin 3–53 or A8–A15**
(`speeduino.ini:457`, applied at `init.cpp:345`). Adding a second or third speed input is
**firmware work, not a board rev**: the limitation is that `sensors.cpp` has one `vssPulse` ISR and
one `vssTimes[VSS_SAMPLES]` ring buffer. Planned arrangement: **pin 34 = rear, pin 35 = front**.

### 1A.3 The CPU is not the constraint the original code assumes

The firmware was written for AVR where main-loop cycles were scarce, and many control loops are
polled at 4–10 Hz to save them. On a 600 MHz Cortex-M7 that is an anachronism. `timers.cpp` already
generates **200 Hz and 1 kHz** timer bits (`:65`, `:83`) that almost nothing consumes — the main
loop only tests 1/4/10/15/30/50 Hz.

**Important:** config pages (`configPage2/4/6`, …) are **plain RAM** (`globals.cpp:36-38`), loaded
from EEPROM at boot and written only on burn. A faster control loop therefore **never touches the
shared SPI bus** and cannot corrupt an MC33810 transfer. The only flash paths are
`readFlashEeprom`/`writeFlashEeprom` (`board_teensy41.cpp:236,242`), both already parked.

The real limit on raising loop rates is **injector/coil ISR jitter** — `TMR1–TMR4` reach
`SPI.transfer16()` in interrupt context. Set rates from actuator/sensor time constants, not from
CPU headroom, and measure `loopsPerSecond` (`statuses.h:269`, already instrumented) before going
above ~200 Hz.

See `docs/DRAG_ECU_FEATURE_GAP_AND_ROADMAP.md` §6A–§6B for the full timing analysis and a
per-sensor / per-loop rate audit.

`pio` is **not on PATH**. Always use the full path:

```cmd
C:\Users\laptop\.platformio\penv\Scripts\platformio.exe
```

---

## 2. The storage device — this is now permanent

Tune storage is the **onboard Winbond W25Q32JVSS (U3)** SPI flash, 4 MB, accessed as an
emulated EEPROM. **This is a permanent decision, not a temporary workaround.**

- Bytes 0..4095 of the emulated EEPROM are the tune (`STORAGE_END = 0xFFF`).
- Geometry: `EEPROM_Emulation_Config{768, 4096, 31, 0x00100000}` — **768 sectors**, 4 KB
  each, 31 bytes/sector, based at **1 MB** into the chip. That yields **23,808 emulated
  bytes, addresses `0x0000..0x5CFF`** — the physical maximum for this part.
- `0x0000–0x0FFF` is the core tune (frozen ABI, see §2A). **`0x1000–0x5CFF` is free for
  custom tables** — 19,713 bytes.
- Exposed as a `storage_api_t` from `getBoardStorageApi()` in `board_teensy41.cpp`.

**Never revert to the Teensy's internal emulated EEPROM as the primary store.** It is only
4284 bytes and exists purely as a runtime fallback if the flash is absent or unformatted, so
the board still boots. That fallback must stay — but the SPI flash is the intended home for
tune data, and new features should assume its capacity.

**Geometry rules:**

- `EEPROM_Bytes_Per_Sector` **must stay 31**. The per-byte slot is exactly
  `4096/(31+1) = 128`, which is the size of `_ReadWriteBuffer`. Raising it overflows that
  buffer.
- To gain capacity, raise the **sector count** — that is the sanctioned mechanism (see §2.1
  and `docs/future-eeprom-capacity.md`).

### 2.1 Capacity is a fixed physical fact — check it before promising anything

The W25Q32 is **4 MB**. The emulation base is **1 MB** (`0x00100000`), which leaves
**3 MB** usable. Per sector is 4 KB, and `EEPROM_Bytes_Per_Sector` must stay at **31**
(because the per-byte slot is exactly `4096/(31+1) = 128`, the size of `_ReadWriteBuffer`).

| Sectors | Emulated bytes | Highest address | Physical end | Verdict |
|---|---|---|---|---|
| 133 (old) | 4,123 | `0x101A` | `0x00185000` | was current |
| 265 | 8,215 | `0x2016` | `0x00209000` | fits |
| 511 | 15,841 | `0x3DE0` | `0x002FF000` | fits |
| **768 (current)** | **23,808** | **`0x5CFF`** | `0x00400000` | **hard ceiling** |
| 1023 | 31,713 | — | overflows chip | **impossible** |
| 4095 | 126,945 | — | overflows chip by 13 MB | **impossible** |

**Hard ceiling: 768 sectors = 23,808 emulated bytes, highest address `0x5CFF`.**

`docs/future-eeprom-capacity.md` previously claimed 1023 and 4095 sectors were viable. Those
rows were wrong — both run past the end of the chip — and have been corrected. Use the table
above as the authoritative one.

### 2.2 The format cost

Formatting is a manual PC step (`tools/flash_probe`). The probe's `FORMAT_SECTORS` **must**
match `EmulatedEEPROMMconfig` in `board_teensy41.cpp` (both 768 today). If the firmware count
is higher than the probe's, the board under-formats, `hasMagicNumbers()` fails, and it
silently falls back to the internal EEPROM.

`tools/eeprom_readback` has `SECTORS_TO_CHECK` which must match too.

At ~45 ms per sector erase: 768 sectors ≈ **35 s**. Acceptable as an attended PC operation,
never on the boot path.

### 2.3 Growing the sector count without losing tune data

Use `tools/flash_probe` key **`K`** ("format but keep a tune"). It writes magic numbers only
into sectors that do not already have them and leaves every formatted sector untouched.

**Do not use `F` (full format) on a board holding a tune** — it erases all sectors.

This distinction matters and was learned the hard way: an earlier version of `K` decided
"if any sector lacks magic numbers, rewrite the whole range", on the reasoning that an
incomplete format means the firmware rejects the chip so there is no tune to lose. That is
false when *growing* the range — the old lower sectors hold a perfectly good tune. It wiped
a working tune when going 133 → 768. The current implementation is per-sector and is safe.

---

## 2A. Policy: all new data goes ABOVE the core EEPROM block

**This is a standing architectural decision. Apply it to every new feature from now on.**

### The rule

Treat `0x0000–0x0FFF` (4096 bytes) as a **frozen ABI**. It is the Speeduino core tune
block, its offsets are encoded in `reference/speeduino.ini` and in every saved `.msq`, and
nothing in it may be moved, resized, or reordered — ever. Existing users' tunes depend on
those absolute offsets byte for byte.

**Every new custom table, calibration, or user data block belongs at `0x1000` and above.**

- `0x1000` is the **base address for all new/custom data.**
- Never insert or extend a table inside `0x0000–0x0FFF`. Append above it.
- Never relocate existing core data to "make room". Add a new block instead.
- A feature that requires resizing a core table is a **tune-format break** — stop and raise
  it with the user, do not do it silently.

### Why (short version)

- Existing `.msq` files and `speeduino.ini` store **absolute** offsets. Anything moved below
  4096 is silently mis-read by TunerStudio — no error, just wrong values.
- The upstream reason for the 4 KB limit was the AVR Mega's 4096-byte physical EEPROM. **We
  do not support that.**
- This fork targets a board with 4 MB of flash. The internal-EEPROM fallback (4284 bytes) is
  a boot-safety net only — it is **not** a size constraint and must not shape feature design.

### Support scope — do not design around devices we do not support

| | |
|---|---|
| **Supported, tested** | DropBear v2 + Teensy 4.1, SPI flash storage |
| **Not supported** | Arduino Mega / any 4 KB EEPROM board, devices without onboard flash |
| **Compile-only** | None. Other envs are not built and are not a support target |

Do not dilute a feature with `#if defined(ARDUINO_AVR_MEGA2560)` guards, reduced table sizes,
or "small flash mode" workarounds to accommodate boards we have chosen not to support. Do not
constrain behaviour for other boards at all — if a change breaks their build, that is accepted.

### What this means in practice

1. **Define the block.** Give it a base constant, a size, and a version byte. Keep it in one
   place, not scattered magic numbers:

   ```c
   // above STORAGE_END (0xFFF)
   constexpr uint16_t EEPROM_CUSTOM_BASE    = 0x1000;
   constexpr uint16_t EEPROM_CUSTOM_VERSION = 1;
   ```

2. **Add a magic/version header** at the block base so you can detect "not yet initialised"
   versus "corrupt". Never assume 0xFF means valid.

3. **Range-check every access** against both the block bounds and
   `flashEEPROM.length()`. `FLASH_EEPROM_BaseClass::read()`/`write()` clamp silently — an
   overflow looks like working code returning 0xFF.

4. **Widen the addressable range first.** `STORAGE_END` in `storage.cpp:59` is still `0xFFF`
   and **must stay there**. The calibration tables are positioned *top-down from it*
   (`EEPROM_CALIBRATION_CLT_VALUES = STORAGE_END - size`, and each successive table subtracts
   again), so raising it would relocate every calibration table and silently invalidate
   existing tunes. The emulated EEPROM is now 23,808 bytes, so `write()` accepts addresses
   above 4095 — that is the space to use. Do not confuse "more flash" with "raise
   `STORAGE_END`"; they are separate decisions and only the first one is done.

5. **Re-format after changing geometry.** Bumping sectors invalidates the old format. Three
   places must agree — if they drift you get a board that silently falls back to internal
   EEPROM, which looks like "the flash stopped working":

   | Where | Constant | Today |
   |---|---|---|
   | `board_teensy41.cpp` | `EmulatedEEPROMMconfig` sectors | **768** (`:120`) |
   | `tools/flash_probe` | `FORMAT_SECTORS` | **768** (`src/main.cpp:527`) |
   | `tools/eeprom_readback` | `SECTORS_TO_CHECK` | **768** (`src/main.cpp:34`) |

   All three currently agree at 768. If they drift you under-format, `hasMagicNumbers()` fails,
   and the board silently runs on internal EEPROM. If you raise the firmware above the probe's
   value you will under-format. Update all three, re-run `K` (never `F` on a board holding a
   tune), then confirm `verify_storage.py` reports `backend : SPI Flash`.

6. **`uint16_t` is fine** up to 65,535 addresses, so it spans the full 23,808-byte ceiling.
   What breaks is code that *assumes* `0xFFF`:
   - `storage.cpp:59` `STORAGE_END`, and `STORAGE_SIZE` / `MAX_PAGE_ADDRESS` derived from it.
   - `page_crc.cpp` computes a CRC per *page* over `getPageSize(pageNum)`. A new block that
     is not part of a defined page gets **no CRC coverage at all** — so a corrupted custom
     table would never be detected. Give new blocks their own checksum.
   - `updateBlockLimitWriteOps` — check for `uint16_t` overflow arithmetic at larger sizes.
   - `storage_api_t::read/write` are already `uint16_t` — no change needed.

7. **TunerStudio mirror.** New tables need a matching `reference/speeduino.ini` section at
   the same offsets. Firmware that writes a block TS cannot read is half a feature.

### Do not

- Put new data in internal RAM as a substitute — it does not persist, and this fork stores
  tunes in flash.
- Grow `EEPROM_Bytes_Per_Sector` above 31 (overflows `_ReadWriteBuffer[128]`).
- Format on the boot path.
- Assume `0xFF` is a safe "unused" sentinel without a version header.
- Add new blocks below `0x1000`, even if there is space.

---

## 3. Wiring and the shared bus — the thing that bites everyone

The flash is on **LPSPI4** (the global `SPI` object, MOSI 11 / MISO 12 / SCK 13). **Two
MC33810 injector/ignition drivers share that same bus.**

| Device | Chip select |
|---|---|
| U3 flash | **pin 6** |
| U8 MC33810 (#1) | pin 10 |
| U7 MC33810 (#2) | pin 9 |

### Non-negotiable constraints

1. **Clock and mode must stay `6 MHz, MSBFIRST, SPI_MODE0`.** `acc_mc33810.cpp` configures
   the port at exactly that, and its runtime path calls bare `SPI.transfer16()` with no
   transaction wrapper. On Teensy 4, `beginTransaction()` only reprograms the port when the
   clock actually *changes*, so a different speed silently corrupts one device or the other.

2. **Park the MC33810 selects HIGH around every flash access.** This is already done by
   `parkSharedSpiDevices()` in `readFlashEeprom()` / `writeFlashEeprom()`. Do not remove it.
   Their idle state is CS driven **LOW** (see `mc33810_active_guard_t` in
   `acc_mc33810.cpp`), which leaves both drivers on the bus driving MISO — the flash then
   reads back `0xFF` and every access fails. This cost an entire debugging session; see §11
   of the handoff doc.

3. **Quad/QSPI is physically impossible.** `/WP` and `/HOLD` are hard-tied to +3V3 on the PCB.
   Single-bit SPI only.

4. **Never write the Teensy's internal EEPROM from the hot path.** `EEPROM.update()` erases a
   4 KB sector and stalls the bus long enough to kill USB CDC, which hangs the board and
   requires a reflash to recover. It is safe during init. For runtime diagnostics, use the
   SPI flash or serial.

5. **Never format on the boot path.** Formatting erases 768 sectors (~35 s at ~45 ms each) before USB
   enumerates. Formatting is a manual PC step: `tools/flash_probe`, key `F`.

---

## 4. Known-unfixed risk: burning while the engine is running

**Flag this to the user before shipping anything that encourages burning with the engine
running.**

`parkSharedSpiDevices()` only owns the CS pins at the *start* of a flash access. It cannot
stop a timer ISR from preempting mid-transfer. Injector and coil outputs fire from
`TMR*_isr()` handlers in `board_teensy41.cpp`, and those reach `SPI.transfer16()` via
`mc33810_t::sendCommand()`. A flash command can therefore be interleaved with an MC33810
transfer or a sector erase can be cut short.

Burning while stopped is verified working. Burning while the engine is running is **not
verified** and should be treated as unsafe until someone closes this gap. The likely fix is to
hold the flash access inside `noInterrupts()`/`interrupts()` (or a proper transaction lock)
for the duration of each page program/erase. That has real-time implications for the
injector/ignition schedulers and needs its own design pass — do not bolt it on casually.

---

## 5. Build, flash, verify

```cmd
cd C:\Users\laptop\Documents\GitHub\speeduino

:: build + flash the firmware (this is the normal loop)
C:\Users\laptop\.platformio\penv\Scripts\platformio.exe run -e teensy41 -t upload

:: A/B control build with the flash driver absent (diagnostic only)
C:\Users\laptop\.platformio\penv\Scripts\platformio.exe run -e teensy41-noSPIFlash -t upload

:: serial monitor
C:\Users\laptop\.platformio\penv\Scripts\platformio.exe device monitor -b 115200
```

Builds take **~60-130 s** and will exceed a 60 s tool timeout. Run them in the background and
poll, or expect the timeout and re-read the log rather than assuming failure.

**Uploading resets the board even if the firmware is completely hung**, because it uses the
halfkay bootloader. You never need the user to physically power cycle — reflash to recover.

**Flashing anything in `tools/` overwrites the Speeduino firmware.** Reflash `-e teensy41`
afterwards.

---

## 6. Tools available to you

### Drive the ECU from the PC

`tools/ecm_client/` — a minimal Speeduino binary-protocol client. Use PlatformIO's Python
(plain `python` has no pyserial):

```cmd
cd tools\ecm_client
C:\Users\laptop\.platformio\penv\Scripts\python.exe speeduino_client.py COM11
C:\Users\laptop\.platformio\penv\Scripts\python.exe verify_storage.py COM11
```

`verify_storage.py` is the storage acceptance test: identity, burn latency, write → burn →
read back, persistence. Run it after any storage change. It only touches padding at the end of
config page 2, so it cannot corrupt a tune.

Wire format is `[len_hi][len_lo][payload][crc32 of payload, 4 bytes BE]`. The CRC covers the
payload **only**, and omitting it yields a well-formed `SERIAL_RC_TIMEOUT (0x80)` reply that
looks like a protocol mismatch rather than a missing CRC. See §13 of the handoff doc for the
rest of the gotchas.

### Hardware diagnostics (each replaces the firmware)

| Tool | Purpose |
|---|---|
| `tools/flash_probe` | JEDEC probe, CS pin sweep, dump, write/verify, erase, **`F` = format for Speeduino** |
| `tools/eeprom_readback` | Read-only magic-number check. Run, power cycle, run again to test persistence |
| `tools/boot_trace` | Black-box boot markers from internal EEPROM. Requires `BOOT_TRACE_ENABLED` |

---

## 7. Debugging rules that earned their place

1. **Measure before you change code.** Several speculative fixes were shipped early on and
   wasted the user's time. Every real fix came from a measurement: the boot trace, the A/B
   control build, per-milestone status samples, or a two-point bus drive test.
2. **Bisect with the control build, not with reasoning.** `teensy41-noSPIFlash` has zero flash
   driver symbols. `git stash` is NOT a valid control here — the work is committed.
3. **On a shared SPI bus, check chip-select ownership first.** "Works at boot, dies later" is
   almost always another device holding MISO.
4. **Read the datasheet timings.** tSHSL (≥50 ns CS-high between commands) and the delay before
   the chip asserts BUSY are both invisible to functional testing and both silently drop
   commands.
5. **When a diagnostic tool works and the firmware doesn't, diff the two implementations line
   by line.** A missing 50 µs settle in `waitWhileBusy()` was the entire difference for one bug.
6. **Never trust a green "success" indication alone.** `storageBackend` in `status5` bit 6-7
   tells you which backend is actually in use: 0 internal, 1 SPI flash, 2 not found,
   3 fallback. If it isn't `1`, your storage test is void.

---

## 8. Before you call it done

- [ ] `teensy41` builds clean. Other environments are **not** a gate — do not chase their build
      errors.
- [ ] `verify_storage.py` passes with `backend : SPI Flash`.
- [ ] Data survives a reboot if you changed anything in the write path.
- [ ] `parkSharedSpiDevices()` is still called around every flash access.
- [ ] Nothing writes internal EEPROM at runtime.
- [ ] If you added storage, it is **at `0x1000` or above** and nothing in `0x0000–0x0FFF`
      moved or changed size.
- [ ] If you changed the sector count, `FORMAT_SECTORS` in `tools/flash_probe` **and**
      `SECTORS_TO_CHECK` in `tools/eeprom_readback` were updated to match, and the chip was
      re-formatted with `K` (never `F` on a board holding a tune).
- [ ] `STORAGE_END` in `storage.cpp` is still `0xFFF` unless a tune-format break was agreed.
- [ ] New blocks have a version/magic header and their own CRC coverage.
- [ ] Matching `reference/speeduino.ini` offsets added for anything TunerStudio must see.
- [ ] No debug scaffolding left behind (`slog*`, extra serial commands, test sketches outside
      `tools/`).
- [ ] If you changed behaviour the handoff doc describes, update it.
