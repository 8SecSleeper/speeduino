# Holley-style Fuelling: EGO Authority, Learn Table & Long-Term Trims

Status: **PHASES 0-6 IMPLEMENTED** — see §14 for what was built, what changed against this
plan, and what remains. Learning ships default-OFF.

Scope: three coupled features, all in service of one goal — make fuelling behave like a
Holley Dominator.

1. **EGO authority to ±100%** (Holley allows full authority closed-loop trim).
2. **A realtime learn table** that accumulates closed-loop trim over time.
3. **Long-term fuel trims** that persist across key cycles, one per fuel table.

Nothing here reuses Holley's internals — the mechanism is modelled on what the OEMs do
(long-term/short-term trims), because that is the same thing Holley's Learn table is.

---

## 1. What already exists (and what it is not)

Verified against the tree; line numbers are current.

| Thing | Where | What it actually is |
|---|---|---|
| `egoCorrection` | `statuses.h:215` (`byte`) | Short-term trim. Baseline 100, written only at `corrections.cpp:842`. |
| `corrections` / `gammaEnrich` | `statuses.h:213` (`uint16_t`) | Aggregate of *all* multipliers, hard-clamped to 1500 (`corrections.cpp:867`). |
| `fuelTrimEnabled` | `config_pages.h:521` | Enables the **per-injector** static trim maps, 6×6, up to `INJ_CHANNELS` (8) channels — `trimTables[INJ_CHANNELS]` (`globals.cpp:18`, `pages.cpp:8`, page 8). Static tune data, not learned. |
| VE tables | `fuelTable` (page 2), `fuelTable2` (page 11) | Two independent fuel tables, already selectable via `fuel2Mode` (`speeduino.ini:1280`). |
| AFR target | `afrTable` (page 5) | 16×16, 0.1 AFR. |
| SD/host autotune | `speeduino.ini:6296` | **TunerStudio-only.** No firmware learn engine exists (confirmed by search). |

**So: there is no long-term trim in the firmware at all today.** `egoCorrection` is wiped by
`initialiseCorrections()` (`corrections.cpp:90`) on every stall, so nothing is remembered
across a key cycle. That is the gap.

---

## 2. The measured constraints

These are the things that decide the design. All verified, not assumed.

### 2.1 EGO authority is capped at 16% in three independent places

| Blocker | Evidence |
|---|---|
| INI caps the user setting | `speeduino.ini:673` `egoLimit = scalar, U08, 7, "", 1, 0, 0, 16, 0` — **max 16** |
| Clamp arithmetic wraps | `corrections.cpp:806-808` `(uint8_t)(100 - egoLimit)`, `(uint8_t)(100 + egoLimit)` — above `egoLimit > 155` the bound wraps modulo 256 |
| TS channel is 8-bit | `speeduino.ini:5679` `egoCorrection = scalar, U08, 11, "%", 1.000` |

Also 8-bit: `computeSimpleCorrection` steps `egoCorrection ± 1` and `computePIDCorrection`
returns `uint8_t` (`corrections.cpp:688-722`).

**±100% is not reachable without making `egoCorrection` signed.** The 8-bit width is *not* the
problem — ±100 fits `int8_t` — but the current code is unsigned and centred on 100, which is
what forces the wrap-prone `100 ± egoLimit` arithmetic. The user has
confirmed legacy TS compatibility is not a constraint, so we widen properly.

### 2.2 Storage — the doc's numbers, corrected and confirmed

From `docs/future-eeprom-capacity.md` and `AGENTS.md` §2A, and independently verified:

```
_EEPROM_Emulation_Size = Flash_Sectors_Used × EEPROM_Bytes_Per_Sector   // SPIAsEEPROM.cpp:54
                       = 768 × 31 = 23,808 bytes  (0x0000..0x5CFF)     // board_teensy41.cpp:120
```

> **Correction to an earlier verbal claim of mine.** I twice stated the free space was
> "much larger / ~94 KB" by computing `768 × 4096 / 32 = 98304`. That is wrong. `4096/32`
> is the **flash slot size per emulated byte** (`_Flash_Size_Per_EEPROM_Byte`,
> `SPIAsEEPROM.cpp:52`) — the wear-levelling stride — not the byte count. The emulated
> EEPROM really is 23,808 bytes and the doc's figure is right.

The top of that space is already claimed by the calibration tables, which grow *downward*
from `STORAGE_END = 0xFFF` (`storage.cpp:59-66`):

- O2/CLT/IAT values+bins: `32+64` bytes per table × 3 = **288 bytes**
- `EEPROM_LAST_BARO`: 1 byte
- O2/IAT/CLT CRCs at `3674/3678/3682`: 12 bytes (fixed low addresses)

So `0x1000..0x18000` minus the downward calibration block:

```
23,808 − 4,096 − (288 + 1 + 12) = 19,411 usable bytes  ≈ 19.0 KB
```

**~19 KB is plenty.** Two 16×16 signed trim tables plus header cost 1,152 bytes — about 6%
of the available space. Budget the custom block at 4 KB and leave the rest for later features.
It is ample, but it is not unlimited, so the block still gets a real layout.

Also confirmed: **nothing currently reads or writes above `0x0FFF`.** `loadAllPages()`
(`storage.cpp:486-569`) and `savePage()` (`storage.cpp:286-449`) only touch the page maps, all
of which live below `0x0FFF`. So `0x1000+` is genuinely virgin.

### 2.3 Two hazards specific to this fork

**Overrun.** `read()`/`write()` clamp silently (`SPIAsEEPROM.cpp:85`, `:125`) — an overflow
returns 0 and looks like working code returning dead data. Every custom-block access must be
range-checked. `AGENTS.md` §2A point 3 already requires this.

**The internal-EEPROM fallback is 4,284 bytes** — smaller than `0x1000`. If the SPI flash
fails to initialise, `getBoardStorageApi()` falls through to it (`board_teensy41.cpp:698-710`)
and **every address above 0x10BB is unreadable**. The custom block must detect this and
disable the feature rather than corrupt a tune. Run-time gate, not `#if`: the same binary
must work.

### 2.4 RAM

`trimTables[INJ_CHANNELS]` (8 × 6×6) already costs 288 bytes. Two 16×16 `int16` trim tables =
1,088 bytes. `RAM1 free for local variables: 257504` — 0.4% of free RAM. Not a constraint.

---

## 3. Target pipeline

Current (`fuel_calcs.cpp:142-153`):

```
REQ_FUEL × VE% → MAP mode → [AFR ratio] → [all corrections] → injOpen → AE
```

Target — one new term, in the correct place:

```
REQ_FUEL × VE% → MAP mode → [AFR ratio] → [corrections × long-term-trim] → injOpen → AE
                                              ^^^^^^^^^^^^^^^^^^^^^^^
                                              trim applied ONCE, here
```

**Why here, and only here.** The trim must **not** be applied inside `sumCorrections`, and
must **not** be learned from the aggregate-with-trim-included value. Two reasons:

1. **Loop stability.** If the trim is inside the feedback path that produces it, the learner
   integrates against its own output and the loop diverges. Same reason the OEMs keep STFT
   and LTFT as separate, deliberately frozen-during-learning terms.
2. **Bounded convergence.** Suppose the VE table is 10% off. If EGO has ±100% authority it
   trims 10% on the first pass and the cell is correct; the trim then sits at 10% and would
   be learned as 10%. The correct design learns the value the table *should* have had, so the
   VE table converges to truth and the trims trend to zero rather than absorbing the error
   permanently.

**Concrete invariants:**

- `egoCorrection` (short-term, ±100%) is **never** included in the learned value.
- `longTermTrim` starts at 0% and moves toward the correction that would make EGO read zero error.
- At steady state: `VE × (1 + trim) ≈ VE_truth`, `trim → VE_truth/VE − 1`, and `egoCorrection → 100`.
- The displayed/logged `gammaEnrich` keeps including trim, because that is the real fuel being delivered.

---

## 4. EGO rework

`egoCorrection` becomes a **signed** value so that 0 means no trim — this is what makes
"trim pixels == 0" work for the trim tables.

The authority is then **asymmetric, per-cell**, from a 16×16 authority table:

```c
// statuses.h
int8_t egoCorrection;        // short-term trim, PERCENT, signed. 0 = no trim. Range ±127.

// config: one authority table per fuel table, 16x16, one byte per cell
//   low nibble  = max LEAN trim (positive), 0..15
//   high nibble = max RICH trim (negative), 0..15
// packed 2 cells/byte -> 128 B + 32 B axes = 160 B per table
```

### 4.1 Why asymmetric per-cell authority is the right call

This is the standout idea in the design. A single global `egoLimit` cannot express the thing
that actually matters:

| Operating point | Right authority | Why |
|---|---|---|
| Idle, cruise, light load | Large (±20-100%) | Trims are legitimate and safe; let it learn |
| **WOT / boost** | **Rich ≈ 0. Lean ≈ small** | **Removing fuel at WOT under boost is how engines die.** Adding fuel is far safer than taking it away |

So the requirement is not "how much authority" but "**how much more fuel may I add, and how
much less may I remove, at this cell**". A per-cell pair is exactly that. It also lets a user
blanket-disable learning at WOT by zeroing the cell, which is the simple, inspectable control
you want on a boosted engine.

This replaces the scalar `egoLimit` for the *learn/trim* authority. `egoLimit` stays as the
short-term (STFT) authority, since a transient ±X% of wiggle is a separate concern.

### 4.2 Correction: the packed-nibble part needs a recheck

The instinct to pack is right — it halves the storage and the 0..15 range is a sensible
authority granularity ✅. But **0..15 units maps to 0..15%**, so ±15% is the *maximum*, and the
step is a coarse **1%**. That is probably not what you want for the lean-side limit.

Two independent knobs — pick one:

| | Range | Step | Cells | Size |
|---|---|---|---|---|
| **Packed nibbles** (as proposed) | ±15% | 1% | 2/byte | 160 B/table |
| **One byte per cell, ×1%** | ±255% (cap at ±100) | 1% | 1/byte | 288 B/table |
| **One byte per cell, ×0.1%** | ±12.7% | **0.1%** | 1/byte | 288 B/table |

Recommendation: **one byte per cell at 1% units, clamped to ±100.** It costs 288 B per table
instead of 160 B — **256 bytes more for both tables, i.e. 1.3% of the 19,411 bytes free** — and
buys a 0-100% range at 1% granularity, which covers both "let it learn everything" and the
fine asymmetry you want at WOT. The 256-byte saving is not worth living inside ±15%.

> **TunerStudio constraint (this is why nibbles are awkward).** TS `tableEditor` maps a
> 16×16 grid to 256 consecutive bytes via `zBins`. There is no "4 bits per cell" table shape
> anywhere in this INI — every `zBins` is a `U08` array (`speeduino.ini:493,502,641,642,1382`).
> `bits` definitions with `[0:3]` exist only for scalars (`:60,344,1023`), not tables.
> So packed nibbles cost you a **TS-side grid that is 2× the firmware resolution**, or a
> hand-maintained packing step. A byte per cell edits natively. Worth the 128 bytes.

> **Separate safety, independent of resolution:** cap `egoCorrection` (and any trim) so the
> *total* fuelling multiplier cannot fall below a floor — the existing
> `correctionsFuel()` clamp is `min(1500, sum)` with **no lower bound** (`corrections.cpp:867`).
> A lean-side authority of 100% could multiply fuelling by ~0. Add a floor of e.g. 25%.

### 4.3 `egoCorrection` width

**`int8_t` is sufficient and I'm confident in it** — unlike the trim tables, this one genuinely
does not need 16 bits, because the *range* requirement is ±100 (fits in 127) and the
*resolution* requirement is 1%, which is exactly what `int8_t` percent units already give.
The simple-loop algorithm steps ±1% by design (`corrections.cpp:691,699`), so sub-1%
resolution would be actively wrong for STFT. Do not over-engineer this one.

> Consumers checked: the simple loop's `+1U`/`-1U` literals must drop the `U` (unsigned
> promotion would corrupt negatives), and `comms_legacy.cpp:819` multiplies by 10 → ±1000,
> fine in `int16_t` at the call site.

Changes:

| Site | Change |
|---|---|
| `corrections.cpp:806-808` | clamp to the **per-cell asymmetric authority** `[-richLimit, +leanLimit]` from the authority table, not the scalar `egoLimit` |
| `corrections.cpp:688-702` | simple loop steps `±1` on a signed value |
| `corrections.cpp:714-722` | `computePIDCorrection` returns `int8_t`; the PID computes in `int32_t` (`PID.h:39`), so clamp to ±100 before narrowing |
| `corrections.cpp:74` | `BASELINE_FUEL_CORRECTION` → 0 |
| `corrections.cpp:90` | init to 0 |
| `corrections.cpp:801` | `correction = currentStatus.egoCorrection` (already signed) |
| `config_pages.h:...` `egoLimit` | keep as the **short-term** (STFT) authority only; keep `U08` and raise the INI max to `100` |
| new authority table(s) | 16×16 `uint8`, one per fuel table, long-term trim authority, per-cell asymmetric. Above `0x1000`. |
| `speeduino.ini:673` | max `16` → `100` |
| `speeduino.ini:5679` | TS channel `egoCorrection` stays `U08` for the legacy gauge; **add** a new `S08` channel `egoTrim` carrying the signed value |
| Feed into `sumCorrections` | replace `combineCorrections(sum, currentStatus.egoCorrection)` (`corrections.cpp:843`) with `combineCorrections(sum, (uint16_t)(100 + egoCorrection))`; guard against a negative result |

`logger.cpp:209/401/579` read `egoCorrection` directly — must be offset by +100 or repointed
at the new channel, otherwise logs silently shift by 100.

**This is a TS-visible break.** Accepted per user direction. The old `U08` channel keeps
publishing `100 + egoCorrection` clamped to `[0,255]` so existing dashes/logs don't explode;
the new `S08` channel carries truth.

---

## 5. Learn (realtime trim) table

A working RAM table, 16×16, signed percent per cell. Two of them if the user wants one per
fuel table — see §6.

```c
// src/controllers/fuel/ltt.h
constexpr int16_t LTT_TRIM_LIMIT_UNITS = 1000; // ±100.0%, in 0.1% units
constexpr int16_t LTT_DEADBAND_UNITS   = 2;    // 0.2% — below wideband noise
```

> **Storage type: `int16_t` at 0.1% units. Range and resolution are independent — take both.**
>
> An earlier draft of this plan used `int8_t`. That was wrong for a reason worth recording,
> because the trade-off is easy to get backwards: **in 8 bits, range and resolution compete.
> In 16 bits they do not.**
>
> | Scheme | Range | Resolution | Cells |
> |---|---|---|---|
> | `int8`, 1% units | ±100% (needs 100 of 127) | **1%** | 256 × 1 B |
> | `int8`, 0.1% units | ±12.7% (127 units) | 0.1% | 256 × 1 B |
> | `int16`, 0.1% units | **±100% (1000 units)** | **0.1%** | 256 × 2 B |
>
> The tempting 8-bit compromise — "restrict to ±64% to buy resolution" — is the trade that 8
> bits *forces*. It is unnecessary here: `int16` at 0.1% gives ±100% **and** 0.1% with 32×
> range headroom, for **1,088 bytes, i.e. 5.6% of the 19,411 bytes free**. That is not a
> resource worth trading accuracy against.
>
> **Why 0.1% and not 1%.** A 1% fuelling error is **0.15 AFR at stoich** — larger than a typical
> wideband's ±0.1 AFR accuracy, so a 1%-granular trim would be the dominant error term in the
> loop. Worse, the dead-band must be at least the step size or the learner hunts, so a 1% step
> implies a ≥1% steady-state error. At 0.1% resolution the dead-band can tighten to ~0.2%
> (**0.03 AFR**), comfortably inside sensor noise, and the loop actually settles instead of
> oscillating about a 1% box.

### 5.1 Learning algorithm

Runs on the existing EGO cycle cadence (`AFRnextCycle`, `corrections.cpp:731`), not every loop.

```
on EGO cycle, if isAfrClosedLoopOperational() and learn is allowed:
    cell = table cell at (fuelLoad, RPM)
    error = egoCorrection                    // signed %, what EGO had to add
    step  = clamp(error * LEARN_RATE, ...)   // e.g. 1/8 … 1/2, configurable
    ltt[cell] += step
    // optional: immediately move the trim into the VE table instead (§7)
    ltt[cell] = clamp(ltt[cell], -LTT_TRIM_LIMIT, +LTT_TRIM_LIMIT)
```

`LEARN_RATE` gives the trace time constant. 1/8 per EGO cycle at `egoCount` ignition events
per cycle ≈ tens of seconds to converge — Holley-ish without feeling sluggish.

### 5.2 When learning must refuse to run

This is the part that makes or breaks it. A learn table that samples transients poisons
itself, because the trim cell is indexed by (RPM, load) but the error was caused by something
else. **Refuse to learn unless every one of these holds:**

| Gate | Rationale | Existing predicate |
|---|---|---|
| Wideband only (`egoType == EGO_TYPE_WIDE`) | Narrowband has no lean-both-sides measurement | `configPage6.egoType` |
| Not cranking — `rotationStatus == Running` | Cranking has its own enrichment | `statuses.h:281` |
| Not in ASE | Post-start enrichment is decaying, not steady | `currentStatus.aseIsActive` (`statuses.h:272`) |
| Not in WUE | Warm-up is a programme, not an error | `currentStatus.wueIsActive` (`statuses.h:274`) |
| Not in AE | Tip-in/tip-out is deliberate transient | `currentStatus.AEamount`, `isAcceleratingTPS` (`statuses.h:278`) |
| Not DFCO | Fuel cut is not a mixture | `currentStatus.isDFCOActive` (`statuses.h:234`) |
| Not decelerating | Decel enleanment is deliberate | `currentStatus.isDeceleratingTPS` (`statuses.h:280`) |
| CLT above threshold | Cold engine misfires poison O2 | reuse `page6.egoTemp` |
| MAP within `egoMAPMin/Max` | Outside the operating window | `page9.egoMAPMin/Max` |
| O2 inside `ego_min/max` | Sensor out of range = garbage | `page6.ego_min/max` |
| `runSecs > ego_sdelay` | Sensor not hot yet | `page6.ego_sdelay` |
| Error below a dead-band | Don't chase noise | new `learnDeadband` |
| Not already at the cell's authority limit | Saturated cell learns nothing | new |

Most of these already exist as `isAfrClosedLoopOperational()` (`corrections.cpp:735-746`).
Learning uses a **stricter** superset — reuse that function, then add the transient gates.

### 5.3 Rate limits and slew

- Max learn step per update (prevents a single bad sensor sample from writing ±20%).
- Optional per-cell sample counter: require N clean samples before the cell is allowed to move.
- Trim applied to fuelling is **slewed**, not stepped, so a trim change never appears as a
  pulse-width jump at the injector.

---

## 6. Long-term trims, one per fuel table

The user wants one trim table per fuel table, because `fuelTable` and `fuelTable2` are
selectable via `fuel2Mode` (`speeduino.ini:1280`) and can differ substantially (switch fuel,
switch ignition/fuel maps on the fly).

```
longTermTrim[2]   // index 0 = fuelTable, index 1 = fuelTable2
```

Selected by the *same* predicate that selects the VE table, so a trim never applies to the
wrong table. That predicate must be factored into one function and shared, or the two can
drift.

Naming, to avoid colliding with the existing per-injector `trimTables[]`:
**`lttTable1` / `lttTable2`** in code and INI. Reusing "trim" would be genuinely confusing.

---

## 7. Two learn modes — this matters more than it looks

Holley's Learn table *is* the correction. Speeduino also has a VE table. These are two
different products, and conflating them is the classic mistake.

| Mode | Behaviour | Use |
|---|---|---|
| **Wealth-learn** (Holley parity) | Trim accumulates in `lttTable`; VE table untouched. The trim *is* the learned fuelling. | User wants a tune that self-corrects, or has no interest in a "correct" VE table |
| **VE-learn** (autotune in firmware) | Trim is folded into the VE table and the trim resets to 0. VE converges to truth. | User wants a clean VE table, with trims as diagnostics only |

Recommendation: **ship wealth-learn as the default** (it is what was asked for) and offer
VE-learn as a per-feature flag later. Both share every gate and the storage. Do not build
both at once.

---

## 8. Persistence on RPM drop

Requirement: save trims when RPM falls below ~200 so they survive a key cycle.

**The hook already exists.** `speeduino.ino:221-235` is the stall block ("the time between teeth is too great") where `initialiseCorrections()` (`:231`) and `rotationStatus = Stopped` (`:235`) run. There is also a precedent for
one-shot persistence: `readBaro()` (`sensors.cpp:638-647`) saves once and sets a flag,
explicitly because repeated writes hurt on some targets.

Design:

- Add `BIT_SENSORS_LTT_SAVED` — `statusSensors` bits 0, 2, 3, 4 are free (`sensors.h:29-33`).
- On `RPM < LTT_SAVE_RPM` (default 200, configurable) **and** the trim tables are dirty:
  save once, set the flag, clear the flag when the engine restarts.
- Track dirtiness: a per-table `dirty` bit set on any learn write. Do not burn on every
  key-off if nothing changed — flash wear is real (one sector erase per 128 writes to that
  sector, `future-eeprom-capacity.md` §2.4).
- Write throttle: `getEepromWriteBlockSize()` already returns `maxWrite * 8` when
  `RPM == 0` (`board_teensy41.cpp:678-684`), so the save is already 8× faster at zero RPM.
  Do the burn at zero RPM, not at 200.
- **Order of operations matters:** learn must stop before RPM reaches 0, or the final
  samples are taken at idle decay and never saved. Gate learning on `RPM > LTT_LEARN_MIN_RPM`
  and persist at `RPM == 0`.

A dedicated key-off hook does not exist and is not needed — a stalled engine at 0 RPM with a
dirty flag is a good enough trigger, and it is the pattern already used.

---

## 9. Storage layout (above `0x1000`, per `AGENTS.md` §2A)

```c
// storage.cpp — new block, entirely above the frozen 0x0000-0x0FFF ABI
constexpr uint16_t EEPROM_CUSTOM_BASE      = 0x1000;
constexpr uint8_t  EEPROM_CUSTOM_VERSION   = 1;
constexpr uint32_t EEPROM_CUSTOM_MAGIC     = 0x534C5454; // "SLTT"

struct lttBlockHeader {
  uint32_t magic;      // detect "never initialised" vs "corrupt"
  uint8_t  version;
  uint8_t  reserved[3];
  uint32_t crc;        // over the body
};
```

Ordering inside the block: 16-byte header, then `lttTable1` (values 256 × int16 = 512 B,
axisX 16, axisY 16 = **544 B**), then `lttTable2` (544 B). Total **1,088 B** plus header.
assert at compile time that it fits under `getStorageAPI().length()`.

Optionally each `lttTable` gains a 16×16 `uint8` saturation-hit counters map (+256 B each) so
TunerStudio can highlight cells pinned at their limit — diagnostic only.

Each authority table (`authTable1/2`, 16×16 `uint8` + axes) adds **288 B**, so the full block is

```
 lttTable1 + lttTable2   (int16, 0.1% units) : 1,088 B
 authTable1 + authTable2 (uint8, 1% units)   :   576 B
 header                                      :    16 B
 ------------------------------------------------
 total                                       : 1,680 B  = 8.6% of the 19,411 B free
```

Budget the block at **4 KB** and `static_assert` the real size fits.

Compile-time guards:

```c
static_assert(EEPROM_CUSTOM_BASE + sizeof(lttBlockHeader) <= 0x5CFF, "...");
```

Run-time guard: if `getStorageAPI().length() < EEPROM_CUSTOM_BASE + blockSize`, disable LTT
and surface a status flag rather than reading zeros. This is the internal-EEPROM fallback
case (§2.3).

---

## 10. Phased plan

Each phase lands independently and is testable on its own. Do not start a phase until the
previous one builds and its tests pass on `teensy41`.

| Phase | Deliverable | Risk | Notes |
|---|---|---|---|
| **0** | Storage block: base/version/magic constants, header struct, range + fallback guards, `static_assert`s. No data yet. | Low | Proves the 0x1000 space is writable/readable before anything depends on it |
| **1** | Make `egoCorrection` `int8_t` (signed, 0 = no trim), remove the `100±` wrap, add `egoTrim` TS channel, fix `logger.cpp` consumers | Medium | TS-visible. `egoLimit` INI max → 100 |
| **1b** | **Per-cell asymmetric authority table** (16×16 `uint8`, lean/rich split), applied as the clamp in `correctionAFRClosedLoop()` | Medium | The WOT-safety feature. Needed before Phase 5 or learn can pull fuel at boost |
| **2** | Signed 16×16 `int16` table generator (`table3d16RpmLoadS16`) + `saveTable`/`loadTable` byte-order tests | Low | Required: `table3d_value_t` is `uint8_t`, so the existing generator cannot hold 0.1% units |
| **3** | `lttTable1/2` in RAM (`table3d16RpmLoadS16`), applied once in `fuel_calcs.cpp` after the AFR multiplier. **No learning.** | Low | Verify fuelling shifts by exactly the trim, nothing else |
| **4** | Persistence: dirty tracking, save at RPM 0, load at boot, block CRC | Medium | Bad CRC ⇒ zero the trims, never apply garbage |
| **5** | Producer: the learn algorithm with all §5.2 gates | High | The dangerous one. Land behind a config enable, default off |
| **6** | TunerStudio: tables, gauges, indicators, enable/limits/rate dialogs | Medium | INI linter + `EXPECTED_CONSTANTS_DIGEST` |
| **7** | (Later) VE-learn mode — fold trims into VE table | High | Only after wealth-learn is proven in the field |

Phases 0–4 are mechanical and low risk. **Phase 5 is where this succeeds or fails** — the
gating, dead-band, slew limits and one-shot persistence are the whole feature.

---

## 11. Tests

- **Maths**: trim application is exact at 0% (bit-identical fuelling), linear, and saturates
  at ±100%; EGO clamp cannot wrap at any `egoLimit`.
- **Gates**: a parameterised test asserting learning is refused for *each* condition in the
  §5.2 table independently, and permitted only with all clear. This is the highest-value test
  in the set.
- **Persistence round-trip**: write trims, simulate stall, reload, byte-identical; corrupt
  the CRC and assert trims come back zero.
- **Fallback**: mock a 4,284-byte storage API and assert LTT is disabled rather than overrunning.
- **Convergence**: a closed-loop simulation with a deliberately-detuned VE table; assert
  trims converge and `egoCorrection` returns toward 0.

Follow the existing harness (`test/test_fuel/`, `TESTABLE_INLINE_STATIC`,
`unity_filename_guard_t`). Note the native test host needs a working `g++`; see §12.

---

## 12. Known environment limitation

The full `pio test -e native_base` run currently fails on this machine before reaching any
test, in the **bundled `ArduinoFake` library**:

```
.s: Fatal error: can't write ...\ArduinoFake\FakeMega.o: File too big
```

Pre-existing and unrelated to any change described here, but it means firmware unit tests
cannot be executed locally until it is resolved — CI is the gate of record. Do not treat a
local `native_base` failure as a regression without checking for this signature first.

---

## 13. Open questions

1. **Trim table resolution.** 16×16 matches VE, but Holley uses the same axis. Do trims want
   to be coarser (8×8) so each cell sees more samples and learns faster? Coarser is safer.
2. **`egoLimit` storage.** Widen the existing field (page 6, currently `U08` at offset 7) or
   add a new field above `0x1000`? Widening touches the frozen ABI — but it is a config page,
   not a table, and the user has said legacy compatibility is not a constraint. **Needs a
   decision before Phase 1.**
3. **Two trims vs one.** If `fuel2Mode` is never used, one table is simpler and halves the
   sample rate problem. Gate the second on `fuel2Mode != Off`?
4. **Should trim authority be independent of EGO authority?** Yes — resolved in §4.2: the
   authority table governs long-term trim, `egoLimit` governs short-term. Separate concerns.
   Remaining sub-question: should the *short-term* authority be per-cell too, or is a scalar
   enough now that the long-term side is cell-aware?
5. **Dead-band default.** Chasing ±0.1% is noise. Suggest 0.5%.
6. **`FUEL_TRIM` off-by-one — a real pre-existing bug, found while checking this.** The firmware
   converter is `translate=-127` (`units.h:159`) but the INI declares the same tables
   `translate=-128` (`speeduino.ini:800`). Worked through the arithmetic:

   | TS shows | raw byte | firmware `FUEL_TRIM` reads | error |
   |---|---|---|---|
   | −50% | 78 | −49% | **+1** |
   | 0% | 128 | +1% | **+1** |
   | +50% | 178 | +51% | **+1** |

   A systematic **+1% bias on every per-injector trim**, and at the top of the byte it is far
   worse: raw 255 → 128 → wraps to `int8_t` **−128%**, a sign flip. Since this is the exact
   idiom LTT is meant to copy, **decide whether to fix `units.h:159` to `-128` as part of this
   work** (it is a one-character change but it shifts existing users' trims by 1%, and it is
   outside the stated scope). Recommend fixing it, separately from LTT.

---

## 14. What was actually built (implementation record)

Phases 0-6 are implemented on branch `feature/holley-style-fuelling`. This section records
where the implementation **departed from this plan**, and why — the plan was written before
the code was read closely, and several of its assumptions did not survive contact.

### 14.1 Corrections to this plan's assumptions

| Plan said | Actually true | Consequence |
|---|---|---|
| "`table3d16RpmLoadS16` generator" (§9, phase 2) | There is **no generator system**. `table3d_typedefs.h` defines a single global `table3d_value_t = uint8_t` and grows table *shapes* from `TABLE3D_GENERATOR`. Every table's `values` array is typed `table3d_axis_t`. | No new table *type* was added. The trim tables are flat `int16_t[2][256]` arrays inside one packed page struct, and the interpolator was templated on the value type instead. This is simpler and avoids reworking the byte-oriented TS page layer. |
| "`saveTable` / `loadTable`" (§9, phase 2) | The real functions are `writeTable` / `loadTable`, driven by `savePage()` and an `entityMap[]` in `storage.cpp`. | Persistence is a new `savePage()` case plus `lttInit()` at boot. |
| "TS pages are 15, a 16th is a real change" (§2) | Correct — but the **legacy `P` command reads a single hex digit**, capping pages at 15. TS uses the modern `p` command with a 16-bit page id. | Page 16 works, and the legacy path is simply not used for it. Calibration pseudo-pages move 16/17/18 → 18/19/20 automatically because they derive from `MAX_PAGE_NUM`. |
| "~19 KB free above 0x0FFF" (§2.2) | Confirmed. | The block is 2,080 bytes — 8.7% of it. |

### 14.2 Design decisions taken during implementation

**The empty-base-class trap.** `lttPage`/`lttBlock` derive from `config_page_t`. A *nested*
`lttConfig : config_page_t` member defeated the empty-base optimisation: it reserved a byte,
pushing `settings` off offset 0 and every subsequent field out of alignment with the INI.
Nothing would have failed to compile — TunerStudio would simply have read the wrong values.
Fixed by making `lttConfig` a plain struct, and **every offset is now asserted with
`offsetof()`** in `ltt.h` so this class of bug fails the build.

**Lean/rich naming caused a real sign bug.** "Lean correction" adds fuel (positive trim);
"rich correction" removes it (negative). Naming the authority limits `leanLimit`/`richLimit`
made them trivially swappable, and they *were* swapped in the first cut — the tests caught it.
They are now `maxAddPct` / `maxRemovePct`, named after what they permit.

**Zero authority must be total.** The plan defines a per-cell authority pair for learning. It
is enforced as a bound on *both* learning and the applied trim: a cell with zero authority
applies no trim at all, even if one was learned earlier. Otherwise a trim learned before the
authority was tightened keeps pulling fuel with no authority left to correct it.

**`egoLimit` was moved off the PID output limits.** The PID previously clamped its output to
`±egoLimit`. Left in place, the integral would saturate at `egoLimit` and the per-cell
authority could never be reached. The authority bound is now applied once, in
`correctionAFRClosedLoop()`. `egoLimit` remains the short-term bound and can tighten the
per-cell limits but never widen them. Its INI maximum went 16 → 100.

**A fuelling floor was added.** `correctionsFuel()` previously had only a maximum (1500) and
no minimum, so a lean-side trim of 100% could multiply fuelling by ~0 and stop the engine.
It now floors at `FUEL_MULTIPLIER_FLOOR_PCT` (25), independent of any trim or authority.

**The learn step is counted, not immediate.** A cell must see `minSamples` clean samples
before it may move; the move therefore happens on update `minSamples+1`. This is deliberate
(a cell visited once during a transient never learns) and is asserted in the tests.

**The trim is clamped to the cell's authority three times** — when learning, when stored, and
when applied. Triple enforcement looks redundant but each guards a different entry point:
the learner, a hand-edited table, and interpolation across neighbouring cells.

### 14.3 Units (the easiest thing to get wrong)

| Quantity | Unit | Range |
|---|---|---|
| `trim` table | signed percent, **0.1%** units | −1000 .. +1000 |
| `leanAuth` / `richAuth` | unsigned **whole** percent | 0 .. 100 |
| `egoCorrection` (STFT) | signed **whole** percent | −127 .. +127 |
| `learnDeadband`, `learnMaxStep`, `slewRate` | 0.1% units | 0 .. 255 |

The authority→trim conversion (`× 10`) is applied at every clamp site. It was wrong once
(the applied trim was capped at 1/10th of the declared authority) and is now covered by
`test_authority_clamps_applied_trim`.

### 14.4 Verification performed

| Check | Result |
|---|---|
| `teensy41` build | clean. RAM1 free 253,920 B (was 257,504 — the block costs ~3.6 KB) |
| `test_fuel` | 169/169 pass (40 new LTT tests) |
| `test_pages` | 48/48 — page 16 wired, reported size 2064 |
| `test_tables` | 107/107 — the templated interpolator is bit-identical for `uint8` |
| `test_storage` | 28/28 — no entity overlap, custom block in range |
| `test_loggers`, `test_secondary` | 52/52, 27/27 |
| INI linter | passes; digest updated; all new objects reachable |
| **Real ECU (COM11)** | `backend : SPI Flash`. Page 16 reads 2064 B, defaults are mode=Off / authority 25/25 / all trims zero, write→burn→read-back works, and **a trim survived a real reboot** |

### 14.5 What remains

- **Phase 7, VE-learn** (fold trims into the VE table and reset them) — not started, per plan.
- **`FUEL_TRIM` off-by-one** (§13.6) — **not touched**. It is a separate pre-existing bug in
  the per-injector trims and is deliberately out of scope here.
- **TS gauges/indicators** for trim activity — the tables and the settings dialog exist; no
  gauge was added to the dash.
- **Open question 3** (gate table 2 on `fuel2Mode`) — implemented as: table 2 is only used in
  an exclusive *switch* mode; in MULTIPLY/ADD the running VE is a blend, so table 1's trim is
  used and the learner converges the blend.
- **Dead-band default** is 0.2%, not the 0.5% suggested in §13.5 — the trim resolution is
  0.1%, so 0.2% keeps the loop able to settle.
