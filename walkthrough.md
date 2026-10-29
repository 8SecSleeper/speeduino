# Walkthrough — TunerStudio layout reorganization

Everything below is **INI-only**. No firmware change, so nothing needs
flashing, and no EEPROM block was touched.

## What was done

| Phase | Change | Commit |
|---|---|---|
| 0 | Fixed 9 malformed dialog/menu statements; re-enabled the knock-window panel | `404a47de` |
| 1 | Replaced the `[Menu]` block with 12 subsystem menus | `404a47de` |
| 3 | Added `tools/ini_lint/check_ini_layout.py` + CI workflow | `404a47de` |
| 2 | `indexCard` tab pilot on idle settings; fixed 3 malformed note rows | `404a47de` |

Files touched:

```
reference/speeduino.ini              (the work)
tools/ini_lint/check_ini_layout.py   (new validator)
.github/workflows/ini-layout.yml     (new CI job)
docs/tunerstudio-layout.md           (new: rules + old→new mapping)
```

## The headline numbers

- **81** distinct objects reachable from the menu before, **81** after. Nothing lost.
- **76** of them moved to a new subsystem menu.
- Top-level menus: `Settings`, `Tuning`, `Spark`, `Startup/Idle`, `Accessories`,
  `3D &Tuning Maps` are gone; `&Engine`, `&Fuel`, `&Ignition`, `&Warmup`, `&Idle`,
  `&Boost`, `&Sensors`, `&Outputs`, `&Protection` replace them.
- **0** errors and **0** warnings from the linter.
- `[Constants]` digest `6aedac05452d1188` — unchanged, so every existing `.msq` still loads.

## Real bugs fixed (not style)

These were all silently wrong before:

1. **Six menu entries in `3D &Tuning Maps` were missing a comma** between target
   and label — `subMenu = dwell_tblMap "Dwell Map"`, and five more just like it.
   TunerStudio parsed the whole string as the target name, so those entries were
   almost certainly dead. Now `dwell_map`, `stagingTableDialog`, `boostDCLupTbl`,
   `vvtTbl`, `vvt2Tbl`, `wmiTbl`.
2. **Five dialogs had a page number where the layout goes** —
   `dialog = sparkSettings,"Spark Settings",4`. Not a valid layout.
3. **Seven dialogs declared with no title argument.**
4. **A dialog named `selectionOfEdianness.`** — trailing period, and "Edianness"
   misspelt. Renamed to `selectionOfEndianness`, reference updated.
5. **`knock_windows` panel was commented out**, so the entire knock-window UI
   was unreachable even though the settings dialog referred to it. Re-enabled.
6. **Three note rows** used `field = "text", , {}, {...}, {...}` — an empty
   constant slot plus a duplicated visibility expression.
7. **`Data Logging` was effectively empty on this fork** because its entries were
   gated on `arrayValue(array.boardHasRTC, pinLayout) > 0`, which is always false
   on DropBear. Gate removed.
8. **`rtc_setup`** was a dead duplicate of `rtc_settings` with zero references. Removed.

## What you need to check (I cannot run TunerStudio)

### 1. Load the INI — most important

Point TunerStudio at `reference/speeduino.ini` and open a project. Confirm:

- [ ] No parse error / "error in ini" dialog.
- [ ] The top menu bar shows approximately: **Engine, Fuel, Ignition, Warmup,
      Idle, Boost, Sensors, Outputs, Protection**, plus TS's own
      **Tools, Data Logging, Help**.
- [ ] An existing saved tune still opens with no signature mismatch and values
      look unchanged.

If TunerStudio rejects the INI, stop here and tell me the exact message — that is
the one thing my linter cannot prove.

### 2. The `indexCard` pilot — decides Phase 2's direction

Open **Idle → Idle Control**.

- [ ] Do you see **tabs** (PWM Idle / Stepper Idle / Closed loop Idle)?
- [ ] Or do the three sections just stack vertically (old behaviour)?
- [ ] Or does the dialog fail to open?

All three answers are useful; the first enables tabbed consolidation everywhere,
the second is the safe fallback and I'll revert this one dialog.

### 3. Spot-check the reorganization

- [ ] **Fuel** — VE Table, VE Table Options, AFR + Lambda target tables, then a
      `Corrections` group, then `Secondary Fuel`. They used to be in three
      different menus.
- [ ] **Ignition** — Spark table, Spark Settings, Dwell, then `Spark Corrections`,
      then `Secondary Spark`, then Knock, then Rotary (only when `sparkMode == 4`).
- [ ] **Boost** — `Boost Control`, then `Boost Mode and Tables`, then
      `Power Adders` (Nitrous / WMI).
- [ ] **Sensors → Calibration** — the four calibration entries now live here
      instead of under `Tools`. They are still offered under `Tools` as well;
      say so if you'd rather I remove one of the two.
- [ ] **Protection** — all six rev/boost/oil/AFR entries together.
- [ ] **Idle → Idle Curves** — the five curves, each appearing only for the idle
      algorithm that uses it. Change idle control type and confirm they swap.

### 4. Conditions still hide things correctly

These are the ones most likely to look "missing" if an expression is wrong:

- [ ] `Flex Fuel` only appears in **Fuel** when flex is enabled.
- [ ] `Dwell Map` only appears in **Ignition** when `useDwellMap`.
- [ ] `VVT2` table only when both VVT and VVT2 are enabled.
- [ ] The WMI table only when WMI is on and `wmiMode > 1`.
- [ ] Boost entries only when boost control is enabled.

## If something is wrong

Every change is in one commit and the previous state is intact, so a single
revert is safe:

```cmd
git revert 404a47de
```

I deliberately did **not** restructure `boostSettings`, `NitrousControl`,
`aircon` or `vvtSettings`: inspection showed they are already well composed from
sub-panels. The fragmentation was in the *menu layer*, which is what changed.

## Deliberately left alone

- `Canout_config` and its 8 dialogs stay commented out (per the agreed default;
  CAN output is not validated on DropBear).
- `Hardware Testing` stays hidden behind `#unset enablehardware_test`.
- Settings that appear in more than one dialog — `CrankAng` (`sparkSettings` and
  `crankingIgnOptions`), `fpPrime` (`fuelpump` and `crankingOptions`),
  `vvtFreq`/`vvt2Pin` (`vvtSettings` and `wmiSettings`) — were **not** removed.
  They are shown under different conditions in different workflows, so removing
  one side is a judgement call about how you tune. Flag it and I'll consolidate.
