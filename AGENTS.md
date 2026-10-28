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
| Connectivity | USB only. No SD, RTC or knock module has been validated. |

Other environments (`megaatmega2560`, `black_F407VE-*`, `teensy35`, `native_base`, Bear Cub /
board 56) are **compile-only**. Keep them compiling, but do not test on them, do not add
features for them, and do not "fix" them speculatively. If a change breaks them at compile
time, fix the build; otherwise leave them alone.

`pio` is **not on PATH**. Always use the full path:

```cmd
C:\Users\laptop\.platformio\penv\Scripts\platformio.exe
```

---

## 2. The storage device — this is now permanent

Tune storage is the **onboard Winbond W25Q32JVSS (U3)** SPI flash, 4 MB, accessed as an
emulated EEPROM. **This is a permanent decision, not a temporary workaround.**

- Bytes 0..4095 of the emulated EEPROM are the tune (`STORAGE_END = 0xFFF`).
- Geometry: `EEPROM_Emulation_Config{133, 4096, 31, 0x00100000}` — 133 sectors, 4 KB each,
  31 bytes/sector, based at **1 MB** into the chip.
- Exposed as a `storage_api_t` from `getBoardStorageApi()` in `board_teensy41.cpp`.

**Never revert to the Teensy's internal emulated EEPROM as the primary store.** It is only
4284 bytes and exists purely as a runtime fallback if the flash is absent or unformatted, so
the board still boots. That fallback must stay — but the SPI flash is the intended home for
tune data, and new features should assume its capacity.

Do **not** change the emulation geometry. `_ReadWriteBuffer` is exactly 128 bytes and the
per-byte slot is exactly `4096/(31+1) = 128`; raising `EEPROM_Bytes_Per_Sector` overflows
that buffer. If more capacity is needed, read `docs/future-eeprom-capacity.md` first.

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

5. **Never format on the boot path.** Formatting erases 133 sectors (~6 s) before USB
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

- [ ] `teensy41` builds clean (and other envs still compile if you touched shared code).
- [ ] `verify_storage.py` passes with `backend : SPI Flash`.
- [ ] Data survives a reboot if you changed anything in the write path.
- [ ] `parkSharedSpiDevices()` is still called around every flash access.
- [ ] Nothing writes internal EEPROM at runtime.
- [ ] No debug scaffolding left behind (`slog*`, extra serial commands, test sketches outside
      `tools/`).
- [ ] If you changed behaviour the handoff doc describes, update it.
