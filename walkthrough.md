# Walkthrough — Accel tab + Fuel Calculations gauge view

INI-only. No firmware change, nothing to flash. Tweaks the gauge INI, so
reload the INI in TunerStudio.

## What changed

| Your request | Done |
|---|---|
| Dedicated Accel Enrichment tab | `&Accel` added, accel moved out of `&Fuel > Corrections` |
| It will grow — plan for that | It is a top-level tab with room to add entries |
| Merge idle/warmup if needed to make room | `&Warmup` folded into `&Idle`, ordered chronologically |
| MS3-style gauge view of fuel calculations | `&Fuel > Fuel Calculations` |

### The Fuel Calculations view

Twenty gauges in four columns, plus an indicator strip:

```
[Accel] [MAP Accel] [ASE ON] [WUE ON] [DFCO OFF] [O2 Active] [Running]
 VE1          WUE              Fuel temp cor    MAP
 VE2          Baro cor         Fuel pressure    Total cor
 Req fuel     Air density cor  Flex cor         Pulse width 1
 Dead time    EGO cor          Accel enrich     Pulse width 2
 Stoich       AFR target       Total cor        Duty cycle
```

### One deliberate difference from MS3

MS3 shows a separate gauge for each correction multiplier — including a
"Fuel Pressure Correction" and a "Long Term Trim Correction" that Speeduino has
no realtime channel for.

I used a single **Total cor** gauge instead. `gammaEnrich` already aggregates
every fuel correction, so this shows the net effect honestly. Inventing the
missing channels would mean deriving numbers in the INI that firmware does not
send — those gauges would display a plausible-looking wrong value, which is
worse than not showing them. Every input to `gammaEnrich` is still individually
visible (WUE, baro, air density, EGO, fuel temp, flex, accel).

If you later add real firmware channels for fuel-pressure or trim corrections,
say so and I will give each its own gauge.

## Please check in TunerStudio

### 1. Accel tab

- [ ] `Accel` appears in the menu bar.
- [ ] It contains Acceleration Enrichment, and the AE graph still draws.
- [ ] `Fuel → Corrections` no longer lists Acceleration Enrichment.

### 2. Fuel Calculations

Open **Fuel → Fuel Calculations**.

- [ ] Gauges render in four columns; nothing is cut off.
- [ ] With the engine off most read 0 — that is expected, not a failure.
- [ ] **`Stoich AFR` matches your configured stoich** (Engine → Engine
      Constants → Stoichiometric ratio, default 14.7). This is the one gauge
      built on a new derived channel, so it is the most likely to show
      something odd. If it reads 0 or garbage, tell me.

Note: with no ECU connected TunerStudio shows 0 for everything. The layout is
still verifiable offline; the values are not.

- [ ] **`Total cor`** should sit near 100% when running at operating
      temperature with no enrichments active.

### 3. Idle tab (was Idle + Warmup)

- [ ] Order reads: Cranking Settings, Priming Pulsewidth | ASE, WUE | Idle
      Control | Idle Curves | Idle Advance Settings.
- [ ] P and I in Idle Control are now reachable (this was fixed in the previous
      round — confirming it still holds).

### 4. Nothing else broke

- [ ] No parse error; saved tune still loads.

## Diagnostics

```cmd
python tools/ini_lint/check_ini_layout.py reference/speeduino.ini
```

This round I extended it because it let two mistakes through, both now caught
automatically:

- **`gauge =` references are validated.** Twenty new gauge references would
  otherwise have been unchecked.
- **The `[Constants]` digest is now an enforced baseline.** It also ignores
  comments and normalises `= lastOffset`, since `lastOffset` always resolves to
  the running byte position and cannot move an existing offset. Any real change
  is a hard error rather than something to eyeball.

Verified `cfb0d92d5e1553bf` is identical across all three commits of this work,
which is the proof that no tune offset moved.

Revert this round with `git revert 78949685`.
