# Future Plan — Enlarging Speeduino's EEPROM address space

Status: **PARTIALLY DONE.** The emulated EEPROM has been grown to 768 sectors = 23,808 bytes
(`0x0000..0x5CFF`), which is the physical maximum for the W25Q32. `STORAGE_END` remains
`0xFFF` so the core tune layout is unchanged and existing tunes stay compatible. The space
`0x1000..0x5CFF` is available for custom tables; see `AGENTS.md` §2A for the policy.

> **Standing policy — see `AGENTS.md` §2A.** All new/custom tables go at **`0x1000` and
> above**; `0x0000–0x0FFF` is a frozen ABI and is never modified. This fork does not support
> the AVR Mega or any device without onboard flash, so the 4 KB limit is a compatibility
> boundary, not a hardware constraint. Design new features for the 23,808-byte ceiling.

---

## 1. Where the limit actually lives

The 7905 bytes is **not** the real constraint. The real constraint is the address
space Speeduino is allowed to address:

```c
// speeduino/storage.cpp:59
constexpr uint16_t STORAGE_END = 0xFFF;   // 4095
```

Everything downstream is typed `uint16_t` and derives from `STORAGE_END`
(`storage.cpp:59-66`, `145-146`):

```c
constexpr uint16_t EEPROM_CALIBRATION_CLT_VALUES = STORAGE_END - ...;
constexpr uint16_t EEPROM_LAST_BARO          = EEPROM_CALIBRATION_O2_BINS - 1;
uint16_t MAX_PAGE_ADDRESS = EEPROM_LAST_BARO - sizeof(uint8_t);
uint16_t STORAGE_SIZE     = STORAGE_END;
```

So **4 KB is a firmware-wide constant**, not a flash-chip property. The emulated
EEPROM is already sized at 7905 bytes — almost twice what is addressable — so the
flash has spare capacity *today*; it is the `0xFFF` ceiling that binds.

### Why 4 KB exists

- AVR Mega2560 has exactly 4096 bytes of real EEPROM (`board_avr2560`), and it is the
  reference platform. Speeduino keeps one layout across all boards so a tune file is
  portable.
- `uint16_t` addressing above 4 KB starts colliding with other uses of the high byte
  only if code assumes a 12-bit mask — need to audit, not just bump the constant.
- TunerStudio's `.msq` format and the `speeduino.ini` definition encode EEPROM offsets
  for every setting; the offsets come from the firmware layout.

---

## 2. What must change, in order of blast radius

### 2.1 The hard blocker: tune-file / INI compatibility

Offsets in `reference/speeduino.ini` (TunerStudio project definition) are tied to the
Eeprom addresses emitted by the firmware. Growing the space is only *safe* if new
data goes **above** the existing 4096-byte block, because:

- existing `.msq` tunes carry `ochGetCommand`/`ochBlockSize` and per-setting offsets;
- TunerStudio writes by absolute offset;
- if you move anything below 4096, every saved tune is silently mis-read.

**Rule: append, never reorder.** Treat `0x0000-0x0FFF` as a frozen ABI.

### 2.2 Widen the constants

```c
// storage.cpp
constexpr uint16_t STORAGE_END = 0x1FFF;   // e.g. 8192 bytes
```

Then audit every `uint16_t address` / `uint16_t eepromStartAddress` (already `uint16_t`,
so it holds up to 65535 — OK) and every place that assumes `0xFFF`:

- `storage.cpp` address arithmetic (`EEPROM_*` constants are `uint16_t`, fine)
- `page_crc.cpp` — CRC spans must widen with the pages
- `storage_api_t::read/write` signatures: already `uint16_t address` — OK
- `getMaxWriteBlockSize` and the burn loop — unaffected
- `updateBlockLimitWriteOps` — check for `uint16_t` overflow at the new size

### 2.3 Enlarge the emulated EEPROM **and** the write granularity

Current config: `{255 sectors, 4096 bytes/sector, 31 bytes/sector, base}` = 7905 bytes.

`EEPROM_Bytes_Per_Sector` **must** stay small enough that
`Flash_Sector_Size / (EEPROM_Bytes_Per_Sector + 1)` fits `_ReadWriteBuffer[128]`
(`SPIAsEEPROM.h`). With a 4 KB sector the per-EEPROM-byte slot is
`4096/(31+1) = 128` bytes exactly — right at the buffer limit. To grow the address
space you multiply **sectors**, not bytes-per-sector:

| Target space | Sectors | B/sector | Emulated bytes | Slot size |
|---|---|---|---|---|
| 4 KB (core) | 133 | 31 | 4123 | 128 B |
| 8 KB | 265 | 31 | 8215 | 128 B |
| 15 KB | 511 | 31 | 15841 | 128 B |
| 23 KB (max, current) | 768 | 31 | 23808 | 128 B |

Keep `B/sector = 31` and only raise the sector count — that keeps slot size at the
128-byte buffer limit and keeps wear-levelling behaviour identical.

**Correction (verified against the W25Q32 datasheet and the firmware config):** an earlier
version of this table listed 1023 and 4095 sectors. Those are **impossible on this part**.
The chip is 4 MB, the emulation base is 1 MB (leaving 3 MB usable), and each sector is 4 KB
— so 3 MB / 4 KB = **768 sectors** is the hard ceiling. The 4095-sector row overflowed the
chip by 13 MB. Do not plan above 768 sectors without moving the base address down.

### 2.4 The wear-levelling cost (the real reason not to go huge)

Each sector holds `31` emulated bytes with `4096/32 = 128` write slots each. A sector
erase + rewrite of all 31 values happens every 128 writes to *any* byte in that sector.
With the burn loop issuing `20 writes/iteration` (`maxWrite`, `board_teensy41.cpp`),
and TunerStudio writing whole pages, growth means:

- **more sectors to erase on `clear()`** — `initialize()` formats every sector on first
  boot. At 4095 sectors × 4 KB = 16 MB and a ~50-100 ms sector erase, first-boot
  formatting would take **minutes**. This is the single biggest practical obstacle.
- Mitigation options:
  1. **Lazy formatting** — only `checkForMagicNumbers()` the sector being accessed and
     format on demand. Changes `initialize()` semantics; needs care so a genuine CRC
     failure isn't confused with "not yet formatted".
  2. **Format-in-background** — format one sector per main-loop pass, gated on
     `RPM == 0`.
  3. **Larger `Flash_Sector_Size`** — use 64 KB block erase (`erase64kBlock`) so fewer
     erase operations are needed for the same capacity. Requires a much bigger
     `_ReadWriteBuffer` (64 KB / 32 = 2048 bytes) — likely too much RAM on some boards,
     but the Teensy 4.1 has 512 KB so it is viable *for this board only*.

### 2.5 Per-board divergence

If only the Teensy 4.1 / DropBear gets the larger space, the layout becomes
board-dependent, which breaks tune portability. Options:

- **(a) Uniform growth** — raise `STORAGE_END` for every board, but only boards with
  ≥ 8 KB of physical EEPROM can actually persist it. AVR (4 KB) and the Teensy's
  internal EEPROM (4284 B) cannot. Not viable unless those boards are dropped.
- **(b) Board-conditional layout** — `#if` on the board, with the tuner INI selecting a
  matching project. Speeduino already ships board-specific INI sections, so this is the
  realistic path.
- **(c) Keep the 4 KB core + a separate "extended" table block** — new/large tables live
  in their own address range and their own INI section, so the core stays ABI-frozen and
  boards without the flash simply hide those tables.

**(c) is the recommended direction** — it is the only option that does not force a
tune-format break, and it degrades gracefully on AVR.

---

## 3. Suggested implementation order (when the time comes)

1. Confirm the external flash actually works on hardware with the current 7905-byte
   config (this branch). Do not scale before it is proven.
2. Add a `storage_size_t` typedef and replace bare `uint16_t` on storage paths.
3. Add CRC + version coverage for any *new* extended block.
4. Implement lazy/background sector formatting, since `initialize()` cost scales with
   capacity and is the first thing that will bite.
5. Raise the sector count in `board_teensy41.cpp` only, behind a board-conditional
   constant.
6. Extend `reference/speeduino.ini` with the extended-block offsets.
7. Only then consider raising `STORAGE_END` for the core block — last, and probably
   never.

## 4. Open measurements needed before choosing numbers

- Actual sector-erase time on the W25Q32JVSS at 6 MHz (datasheet: ~45 ms typ for 4 KB;
  page program ~0.7 ms). Measured value drives how aggressive `maxWrite` can be.
- Whether `_ReadWriteBuffer[128]` can be raised on the Teensy 4.1 (it can — 512 KB RAM).
- TunerStudio's tolerance for a larger `ochBlockSize`.
