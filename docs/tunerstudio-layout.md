# TunerStudio layout

This note records the intended shape of the TunerStudio navigation for this
fork, and the rules to follow when adding to it. It exists because the menu
tree had drifted badly and been re-scattered once already.

The authoritative source is `reference/speeduino.ini`; the `[Menu]` block has
these same rules as a comment at the top. Validate any change with:

```cmd
python tools/ini_lint/check_ini_layout.py reference/speeduino.ini
```

CI runs the same check on any PR touching the INI (`.github/workflows/ini-layout.yml`).
Upstream's `validate-ini.yml` confirms the INI *parses*; it cannot tell whether
our own menu targets and panel references resolve, because both sides live in
the same file. That is the gap the layout linter fills.

---

## 1. The rules

1. **One canonical home per setting.** A dialog is listed exactly once in
   `[Menu]`. If a table is also wanted inside a settings page, embed it with
   `panel =` — do not list the same object twice.
2. **Group by subsystem, not by task type.** Someone whose boost is wrong goes
   to `&Boost` and finds the settings, the table and the closed-loop lookup
   together.
3. **Tables lead their subsystem**, immediately followed by that table's
   settings dialog.
4. **Three levels maximum.** The INI spec allows only
   `menu > groupMenu > groupChildMenu`, and `groupMenu` **cannot** nest inside
   another `groupMenu`. Deeper grouping has to happen inside the dialogs
   (`border` + nested `xAxis`/`yAxis` columns — see §5).
5. **Never rename `Tools`, `Data Logging` or `Help`.** Naming them is how the
   INI *merges* its entries into TunerStudio's own built-in menus instead of
   creating a competing one. Renaming silently breaks that merge.
6. **`std_separator` only between items** — never first or last in a menu.
   Note that a `subMenu` of any kind (separators included) closes the current
   `groupMenu`.
7. **No page numbers in a `dialog` header.** The third argument is the layout
   (`yAxis` / `xAxis` / `border`). Page numbers belong on `subMenu` and only
   matter for `std_*` editors. `indexCard` parses but does not work — see §5.
8. **Dialog height does not scroll, it clips.** Put long content in two
   columns rather than one tall column.

## 2. The menus

Fourteen top-level menus: eleven of ours plus the three that merge into
TunerStudio's own. The `#if enablehardware_test` menu only appears when that
project property is set.

| Menu | Scope |
|---|---|
| `&Engine` | Board, trigger, injectors, I/O summary |
| `&Fuel` | VE table, VE table 2, staging, AFR/lambda target, corrections |
| `&Ignition` | Advance table, advance table 2, spark settings, dwell, corrections |
| `&Warmup` | Cranking, priming, ASE, WUE (**ASE before WUE**) |
| `&Idle` | Idle control (with idle-up and curves inline) |
| `&Boost` | Boost control only — kept clear for the boost rework |
| `&Sensors` | Calibration, filters, auxiliary inputs, CAN, VSS |
| `&Outputs` | Fuel pump, fan, tacho, VVT, A/C, programmable outputs, power adders, launch |
| `&Protection` | Rev limiters, boost cut, oil, AFR, coolant |
| `3D &Maps` | All tables, plain views, for quick access |
| `Tools` | Merged — calibrations |
| `Data Logging` | Merged |
| `Help` | Merged |

`Settings`, `Tuning`, `Spark`, `Startup/Idle` and `Accessories` no longer exist
as top-level menus. Nitrous and water-methanol live under `&Outputs > Power
Adders`, not `&Boost`, deliberately: they are power adders rather than boost
control, and `&Boost` is going to be reworked.

## 3. Where everything went

84 distinct objects are reachable from the menu now, versus 81 before this
reorganisation. Nothing was lost. `idleUpSettings` lost its own menu entry but
is embedded in `idleSettings`; the three tables and `stagingTbl`/`boostTbl`/
`boostDCLupTbl` are reachable as panels as well as from `3D &Maps`.

| Object | Was under | Now under |
|---|---|---|
| `ASE` | Startup/Idle | Warmup |
| `Auxin_config` | Accessories | Sensors > Inputs |
| `CanBcast` | Accessories | Sensors > CAN Bus |
| `Canin_config` | Accessories | Sensors > Inputs |
| `LaunchControl` | Accessories | Outputs |
| `NitrousControl` | Accessories | Outputs > Power Adders |
| `accelEnrichments` | Tuning | Fuel > Corrections |
| `afrProtect` | Tuning > Engine Protection | Protection |
| `afrTable1Tbl` | 3D Tuning Maps / Tuning | 3D Maps / Fuel |
| `airCon` | Accessories | Outputs |
| `airdensity_curve` | Settings | Fuel > Corrections |
| `baroFuel_curve` | Settings | Fuel > Corrections |
| `batCal` | Tools | Sensors > Calibration / Tools |
| `boostCut` | Tuning > Engine Protection | Protection |
| `boostDCLupTbl` | 3D Tuning Maps | 3D Maps |
| `boostLoad` | Accessories | Boost |
| `boostSettings` | Accessories | Boost |
| `boostTbl` | 3D Tuning Maps | 3D Maps |
| `can_serial3IO` | Accessories | Sensors > CAN Bus |
| `clt_advance_curve` | Spark | Ignition > Spark Corrections |
| `crankPW` | Startup/Idle | Warmup |
| `dwellSettings` | Spark | Ignition |
| `dwell_correction_curve` | Spark | Ignition |
| `dwell_map` | 3D Tuning Maps / Spark | 3D Maps / Ignition |
| `egoControl` | Tuning | Fuel |
| `engineProtection` | Tuning > Engine Protection | Protection |
| `engine_constants` | Settings | Engine |
| `fanSettings` | Accessories | Outputs |
| `flexFuel` | Tuning > Engine Protection | Fuel |
| `fuelTable2Dialog` | Tuning | Fuel |
| `fuelTable2Tbl` | 3D Tuning Maps | 3D Maps |
| `fuelTemp_curve` | Tuning | Fuel > Corrections |
| `fuelpump` | Accessories | Outputs |
| `gaugeLimits` | Settings | Engine |
| `helpGeneral` | Help | Help |
| `iacClosedLoop_curve` | Startup/Idle | Idle > Idle Curves |
| `iacPwmCrank_curve` | Startup/Idle | Idle > Idle Curves |
| `iacPwm_curve` | Startup/Idle | Idle > Idle Curves |
| `iacStepCrank_curve` | Startup/Idle | Idle > Idle Curves |
| `iacStep_curve` | Startup/Idle | Idle > Idle Curves |
| `iat_retard_curve` | Spark | Ignition > Spark Corrections |
| `idleAdvanceSettings` | Startup/Idle | Idle |
| `idleSettings` | Startup/Idle | Idle |
| `idleUpSettings` | Startup/Idle | panel-only |
| `injChars` | Settings | Engine > Injectors |
| `inj_trimad` | Tuning | 3D Maps / Engine > Injectors |
| `inj_trimad_B` | Tuning | 3D Maps / Engine > Injectors |
| `io_summary` | Settings | Engine |
| `knockSettings` | Spark | Ignition |
| `lambdaTable1Tbl` | 3D Tuning Maps / Tuning | 3D Maps / Fuel |
| `mapCal` | Tools | Sensors > Calibration / Tools |
| `oilPressureProtection` | Tuning > Engine Protection | Protection |
| `onboard_log_setup` | Data Logging | Data Logging |
| `outputtest1` | Hardware Testing | Hardware Testing |
| `pressureSensors` | Accessories | Sensors |
| `prgm_out_config` | Settings | Outputs |
| `primePW` | Startup/Idle | Warmup |
| `pwmFan` | Accessories | Outputs |
| `reset_control` | Settings | Engine |
| `revLimiterDialog` | Tuning > Engine Protection | Protection |
| `rotary_ignition` | Spark | Ignition |
| `rtc_settings` | Data Logging | Data Logging |
| `sensorFilters` | Tools | Sensors / Tools |
| `spark2Tbl` | 3D Tuning Maps | 3D Maps |
| `sparkSettings` | Spark | Ignition |
| `sparkTable2Dialog` | Tuning | Ignition |
| `sparkTbl` | 3D Tuning Maps / Spark / Tuning | 3D Maps / Ignition |
| `stagingTableDialog` | Tuning | Fuel |
| `stagingTbl` | 3D Tuning Maps | 3D Maps |
| `stm32cmd` | Hardware Testing | Hardware Testing |
| `tacho` | Accessories | Outputs |
| `triggerSettings` | Settings | Engine |
| `veTable1Tbl` | 3D Tuning Maps | 3D Maps |
| `veTableDialog` | Tuning | Fuel |
| `vssSettings` | Accessories | Sensors > Inputs |
| `vvt2Tbl` | 3D Tuning Maps / Accessories | 3D Maps / Outputs > VVT |
| `vvtSettings` | Accessories | Outputs > VVT |
| `vvtTbl` | 3D Tuning Maps / Accessories | 3D Maps / Outputs > VVT |
| `warmup` | Startup/Idle | Warmup |
| `wmiSettings` | Accessories | Outputs > Power Adders |
| `wmiTbl` | 3D Tuning Maps / Accessories | 3D Maps / Outputs > Power Adders |

Two structual points worth noting:

- **Table dialogs carry their own options.** `veTableDialog`,
  `fuelTable2Dialog` and `sparkTable2Dialog` are `border` layouts with the
  table in `Center` and a titled `Options` column in `East`. There are no
  separate "…Options" menu entries any more.
- **Secondary tables sit directly under their parent**, not in a folder:
  `VE Table` → `VE Table 2`, `Ignition Advance Table` → `Ignition Advance
  Table 2`.

`3D &Maps` is the one deliberate exception to the one-canonical-home rule. It
lists the raw TableEditor ids (plain table views, no options) because people are
used to a single maps tab. Only menu *entries* are repeated — every table is
still defined exactly once.

## 4. Deliberately unreachable

| Object | Why |
|---|---|
| `Canout_config` + 8 child dialogs | CAN output configuration chain. Disabled and left disabled; not validated on DropBear. |
| `warmup_afr_curve`, `warmup_analyzer_curve` | Used only by `[WueAnalyze]`, never a UI panel. |

## 5. `indexCard` does not work - do not use it

`indexCard` was trialled on `idleSettings` and **does not render as tabs in
TunerStudio**. All child panels are drawn simultaneously, stacked in a cramped
column, with the dialog broken around them. The layout is listed in the INI
spec revision history (added 3.38) but appears to be unimplemented — consistent
with no shipping INI using it: MS3, MS2 and rusEFI all build complex dialogs
from `border` plus nested `xAxis` / `yAxis` wrappers.

**Use that idiom instead.** The pattern that works here:

```ini
    dialog = someDialog_east, "Options", yAxis
        field = "Thing", thingConst
        panel = someSubPanel

    dialog = someDialog, "Title", border
        panel = theTableOrMainThing, Center
        panel = someDialog_east, East
```

Layout rules learned the hard way:

| Layout | Behaviour |
|---|---|
| `border` + `Center` + `East` | Table in `Center` expands when the window is widened. This is what you want for table dialogs. |
| `border` + `West` | Pins the component to its preferred width and leaves a dead centre. Avoid for tables. |
| `yAxis` | Fields stack top to bottom. Any unconstrained table inside it drags in a huge blank area to its left. |
| `xAxis` | Fields laid left to right. |
| `indexCard` | **Broken — draws everything at once.** |
| `grid`, `tabbed` | Not dialog layouts at all. |

Dialog height is the other trap: if content exceeds it, the dialog does not
scroll, it just clips. `idleSettings` lost the P and I fields that way. Split
content into two columns rather than one long one.

`idleUpSettings` has no menu entry of its own — it is embedded in
`idleSettings`.

## 6. What did not change

- **No `[Constants]` edits at all.** Tune offsets are a frozen ABI
  (see `AGENTS.md` §2A). The `[Constants]` digest is
  `6aedac05452d1188` and must stay that way unless a tune-format break is
  deliberately agreed. The linter prints the digest on every run so a drift is
  obvious.
- No gauge, datalog or `[FrontPage]` changes.
- No firmware changes: this is an INI-only reorganisation.
