# Walkthrough — TunerStudio layout, second pass

INI-only. No firmware change, no EEPROM change, nothing to flash.

## What changed in this pass

| Your feedback | Done |
|---|---|
| Secondary fuel/spark should sit directly below their parent tables, `VE Table` → `VE Table 2` | Done — folder gone, secondary tables sit right under their parents |
| `VE Table` + `VE Table Options` should be one entry, options inside the table | Done for VE, VE Table 2 and Advance Table 2 |
| Move Power Adders out of Boost, keep Boost clean for the rework | Done — Nitrous / WMI now under `&Outputs > Power Adders`; `&Boost` is two entries |
| ASE before WUE in Warmup | Done |
| Put back a tab with all the 3D maps | Done — `3D &Maps`, plain table views |
| Idle settings: P and I unreachable, hard to read | Reworked — see below |

### `indexCard` failed — worth recording

The idle pilot confirmed `indexCard` is **not implemented**. What you saw is
exactly its failure mode: all three child panels drawn at once in a cramped
right column with the dialog broken around them. That matches the research
finding that no shipping INI uses it.

So idle now uses the idiom MS3 and rusEFI actually use — `border` + columns:

```
Idle Settings
├── North  : idle control type, crank taper, idle-up input/output
└── Center : [ PWM Idle ] [ Stepper Idle ] [ Closed loop Idle ]   side by side
```

P and I were unreachable because the content exceeded the dialog height and the
title row pushed it down — TunerStudio clips rather than scrolls. Three columns
side by side roughly a third the previous height. **If P and I are still cut
off, tell me**: the next lever is dropping the `#Fast Idle` row and the
"1.0 means 100%" banner, then reducing further.

`Idle Up Settings` no longer has its own menu entry — it is inside Idle Control
now.

## Please check

### 1. Idle Control — the one most likely to need another pass

Open **Idle → Idle Control**.

- [ ] P, I and D are all visible and reachable.
- [ ] The three columns sit side by side rather than stacked.
- [ ] Idle-up settings appear underneath.
- [ ] Nothing is clipped at the bottom.

### 2. Table dialogs — verify the `Center`/`East` split works

Open **Fuel → VE Table**.

- [ ] The table is on the left and **grows** when you widen the window.
- [ ] An `Options` column sits on the right with *Multiply VE value by MAP
      ratio* and the legacy options.
- [ ] There is no separate `VE Table Options` entry in the Fuel menu any more.

Then **Fuel → VE Table 2** (only shows if secondary fuel is enabled) and
**Ignition → Ignition Advance Table 2**: same shape, options on the right.

### 3. Menu order

- [ ] **Fuel**: `VE Table`, `VE Table 2`, `Staged Injection` | AFR/Lambda/EGO | Corrections | Flex Fuel
- [ ] **Ignition**: `Ignition Advance Table`, `Ignition Advance Table 2` | Spark Settings | Dwell | Spark Corrections | Sequential Trim | Knock | Rotary
- [ ] **Warmup**: Cranking, Priming | **ASE, then WUE**
- [ ] **Boost**: only `Boost Control` and `Boost Mode and Tables`
- [ ] **Outputs → Power Adders**: Nitrous, Water-Methanol, WMI table
- [ ] **3D &Maps**: all the tables as plain views, no options columns

### 4. Still fine?

- [ ] No parse error, menu bar looks right, saved tune unchanged.

## Known trade-off I made

`3D &Maps` lists tables that also appear in their subsystem menu. That is a
deliberate exception to the one-canonical-home rule, because you asked for the
combined maps tab back and it is a common way to work. Only menu *entries*
repeat — every table is still defined once, so nothing can drift. If it ever
becomes confusing I can gate the per-subsystem copies behind a setting.

## Diagnostics, if something looks wrong

```cmd
python tools/ini_lint/check_ini_layout.py reference/speeduino.ini
```

Reports dangling menu targets, dangling `panel =` references, nested
`groupMenu`, duplicates within a menu, malformed dialog headers, and anything
that became unreachable. The `[Constants]` digest printed on every run must stay
`6aedac05452d1188` — anything else means tune offsets moved, which would
invalidate existing `.msq` files.

Revert this pass with `git revert f0509c95`.
