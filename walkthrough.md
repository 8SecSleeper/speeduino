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

### The Fuel Calculations view — now 5 wide

```
[Accel] [MAP Accel] [ASE ON] [WUE ON] [DFCO OFF] [O2 Active] [Running]
 VE1           Stoich          Fuel temp cor   Total cor     Duty cycle
 VE2           WUE             Flex cor        AFR target    MAP
 Req fuel      Baro cor        EGO cor         Pulse width   MAP multiply
 Dead time     Air density     Accel enrich    Pulse width 2 Fuel pressure
```

5 columns x 4 gauges.

**Why it was 3 wide before:** `border` layouts only have West / Center / East —
three side-by-side slots. I put the fourth column in `South`, which is a
horizontal band, so its `yAxis` column stacked one gauge per row. That is the
stack you saw. Fixed by nesting an `xAxis` of five columns inside `Center`.

**Also fixed:** the wall showed the same number twice. `totalCor` is a
passthrough of `gammaEnrich`, so `totalCorGauge` and `gammaEnrichGauge` were
both displaying the aggregate. Removed the duplicate; `gammaEnrichGauge` remains.

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
