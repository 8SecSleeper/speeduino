# Fixing TunerStudio #1224 — `iacCLminValue` / `iacCLmaxValue` multiply by 3 on every project load

Upstream issue: **TunerStudio MS changing values randomly #1224** (open since 2024, still
being reported).

Symptom, as reported and reproduced:

```
tune contains:  iacCLminValue = 33.0   iacCLmaxValue = 90.0
open project:   iacCLminValue = 99.0   iacCLmaxValue = 270.0     (x3)
open again:     iacCLminValue = 297.0  iacCLmaxValue = 810.0     (x9)
```

Happens with **no controller connected**, on **every** project open, and **writes the
inflated value back into the tune**.

---

## Root cause

`reference/speeduino.ini` declares both values with a **conditional display scale**:

```ini
; [OutputChannels]
idleRes      = { (iacAlgorithm == 5 || iacAlgorithm == 7) ? 3.0 : 1.0 }
idleResMax   = { (iacAlgorithm == 5 || iacAlgorithm == 7) ? (iacStepHome-3) : 100.0 }

; page 1
iacCLminValue = scalar, U08, 61, "% / Steps", {idleRes}, 0.0, 0.0, {idleResMax}, 0
iacCLmaxValue = scalar, U08, 62, "% / Steps", {idleRes}, 0.0, 0.0, {idleResMax}, 0
```

`iacAlgorithm` lives on **page 6**. `iacCLminValue` / `iacCLmaxValue` live on **page 1**.

When TunerStudio loads a project it evaluates the scale **before page 6 has been read**.
`iacAlgorithm` is therefore 0, the condition is false, and the scale resolves to `1.0`.
The saved display value (e.g. 33) is then treated as a raw byte, re-rendered at the
correct stepper scale of `3.0`, and written back as 99. Next open repeats it.

### In-session switching is correct — this is the proof

Switching the idle algorithm in a live session shows the value drop as expected:

| Mode | Display | ÷ scale | Raw byte |
|---|---|---|---|
| Stepper (5/7) | 3 | ÷ 3.0 | **1** |
| PWM (3/6) | 1 | ÷ 1.0 | **1** |

Same byte, only the display changes. So the *scale expression itself* is right; the bug
is purely that it cannot be resolved at load time. That is what confirms a **constant**
scale is a valid fix rather than a workaround.

### Why only these two fields

Every conditional scale in the ini was audited (33 of them). The rest are safe because
their dependency is either on the **same page** (`fuel2SwitchValue`,
`spark2SwitchValue`, `loadBins*`, `mapBins*`) or a **global constant** (`stoich`).
`idleRes` is the only one that reaches **across pages**. That is why this is the only
field pair that ratchets.

---

## The fix

Make the scale constant, keeping the 3.0 factor (which is genuinely required — see
below), so the read and write paths can no longer disagree:

```ini
; [OutputChannels]
idleRes      = 3.0
idleResMax   = 765.0
```

No byte layout changes: `iacCLminValue`/`iacCLmaxValue` stay at page 1 offsets 61/62, and
`ochBlockSize` is untouched. Existing `.msq` files remain valid.

### Why 3.0 and not 1.0

The byte genuinely cannot hold a full step count — `iacStepHome` defaults to 279 and a
`U08` maxes at 255. The firmware therefore stores **steps ÷ 3** and multiplies back up:

```cpp
// units.h
static constexpr conversionFactor<uint16_t, uint8_t> IAC_STEPS = { .scale=3U, .translate=0U };

// idle.cpp — stepper path applies the x3 and the <<2 itself
configureIdlePID(configPage6,
                 (configPage2.iacCLminValue * 3)<<2,
                 (configPage2.iacCLmaxValue * 3)<<2, ...);
```

Removing the factor would make a 270-step limit be commanded as 810 — real over-travel.
So the factor must stay; only its *conditionality* is the bug.

### Accepted trade-off

PWM idle (algorithms 3 and 6) now also renders the field at 3×, so a 30 % duty is
entered as 90. That is cosmetic, not corruption, and strictly better than a tune that
inflates itself on every open. The dialog labels were updated to say so:

```ini
field = "Minimum valve value / Steps",   iacCLminValue, { ... }
displayOnlyField = "#  PWM modes: these are shown at 3x (30% duty = 90)"
```

---

## The cleaner long-term fix (needs a page-1 byte)

Two purpose-built fields would remove the overload entirely:

| Field | Offset | Type | Scale | Used by |
|---|---|---|---|---|
| `iacCLminPwm` / `iacCLmaxPwm` | new | U08 `%` | 1.0 | PWM, algorithms 3 & 6 |
| `iacCLminSteps` / `iacCLmaxSteps` | new | U08 steps | 3.0 constant | Stepper, 5 & 7 |

Firmware selects the pair by `iacAlgorithm`; TS never resolves a conditional.

**Blocker:** page 1 is **completely full — 128 of 128 bytes used** (verified by mapping
every `U08`/`S08` as 1 byte and every `U16`/`S16` as 2). There is no spare byte, so this
variant is a page-1 growth / tune-format change and must be a deliberate decision. The
constant-scale fix above needs no such change and is safe to ship now.

---

## Which TunerStudio behaviour is arguably still the bug

Two upstream changes would make the ini-side workaround unnecessary:

1. **Resolve dependent expressions at use time, not load time.** The scale depends on a
   different page; TS should either defer evaluation until that page is available, or
   refuse to write a value it could not resolve.
2. **Never write back a value that the user did not edit during a read.** A project open
   should be read-only for untouched constants. This alone stops the ratchet.

Either would fix #1224 for every ini that uses cross-page conditional scales.

---

## How this was validated

```cmd
python tools/ini_lint/check_ini_layout.py reference/speeduino.ini
```
`PASSED — 0 errors, 0 warnings`, and the `[Constants]` digest is **unchanged**
(`80afb5c95af0706f`), proving no offset moved.
