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
   (`indexCard` tabs or `border` + nested columns).
5. **Never rename `Tools`, `Data Logging` or `Help`.** Naming them is how the
   INI *merges* its entries into TunerStudio's own built-in menus instead of
   creating a competing one. Renaming silently breaks that merge.
6. **`std_separator` only between items** — never first or last in a menu.
   Note that a `subMenu` of any kind (separators included) closes the current
   `groupMenu`.
7. **No page numbers in a `dialog` header.** The third argument is the layout
   (`yAxis` / `xAxis` / `border` / `indexCard`). Page numbers belong on
   `subMenu` and only matter for `std_*` editors.

## 2. The menus

Twelve top-level menus, three of which merge into TunerStudio's own.

| Menu | Scope |
|---|---|
| `&Engine` | Board, trigger, injectors, I/O summary |
| `&Fuel` | VE table, AFR/lambda target, corrections, secondary fuel |
| `&Ignition` | Spark table, dwell, spark corrections, secondary spark |
| `&Warmup` | Cranking, priming, WUE, ASE |
| `&Idle` | Idle control, idle curves, idle-up, idle advance |
| `&Boost` | Boost control and tables, nitrous, WMI |
| `&Sensors` | Calibration, filters, auxiliary inputs, CAN, VSS |
| `&Outputs` | Fuel pump, fan, tacho, VVT, A/C, programmable outputs, launch |
| `&Protection` | Rev limiters, boost cut, oil, AFR, coolant |
| `Tools` | Merged — calibrations |
| `Data Logging` | Merged |
| `Help` | Merged |

`Settings`, `Tuning`, `Spark`, `Startup/Idle`, `Accessories` and
`3D &Tuning Maps` no longer exist as top-level menus. `3D &Tuning Maps` was
removed deliberately: every table it held now lives next to its own subsystem,
so it was pure duplication.

## 3. Where everything went

81 distinct objects were reachable before and 81 after — nothing was lost.
76 of them changed location.

| Object | Was under | Now under |
|---|---|---|
| `ASE` | Startup/Idle | Warmup |
| `Auxin_config` | Accessories | Sensors > Inputs |
| `CanBcast` | Accessories | Sensors > CAN Bus |
| `Canin_config` | Accessories | Sensors > Inputs |
| `LaunchControl` | Accessories | Outputs |
| `NitrousControl` | Accessories | Boost > Power Adders |
| `accelEnrichments` | Tuning | Fuel > Corrections |
| `afrProtect` | Tuning > Engine Protection | Protection |
| `afrTable1Tbl` | 3D Tuning Maps / Tuning > Engine Protection | Fuel |
| `airCon` | Accessories | Outputs |
| `airdensity_curve` | Settings | Fuel > Corrections |
| `baroFuel_curve` | Settings | Fuel > Corrections |
| `batCal` | Tools | Sensors > Calibration / Tools |
| `boostCut` | Tuning > Engine Protection | Protection |
| `boostDCLupTbl` | 3D Tuning Maps | panel in Boost Mode and Tables |
| `boostLoad` | Accessories | Boost |
| `boostSettings` | Accessories | Boost |
| `boostTbl` | 3D Tuning Maps | panel in Boost Mode and Tables |
| `can_serial3IO` | Accessories | Sensors > CAN Bus |
| `clt_advance_curve` | Spark | Ignition > Spark Corrections |
| `crankPW` | Startup/Idle | Warmup |
| `dwellSettings` | Spark | Ignition |
| `dwell_correction_curve` | Spark | Ignition |
| `dwell_map` | 3D Tuning Maps / Spark | Ignition |
| `egoControl` | Tuning | Fuel |
| `engineProtection` | Tuning > Engine Protection | Protection |
| `engine_constants` | Settings | Engine |
| `fanSettings` | Accessories | Outputs |
| `flexFuel` | Tuning > Engine Protection | Fuel |
| `fuelTable2Dialog` | Tuning | Fuel > Secondary Fuel |
| `fuelTable2Tbl` | 3D Tuning Maps | Fuel > Secondary Fuel |
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
| `idleUpSettings` | Startup/Idle | Idle |
| `injChars` | Settings | Engine > Injectors |
| `inj_trimad` | Tuning | Engine > Injectors |
| `inj_trimad_B` | Tuning | Engine > Injectors |
| `io_summary` | Settings | Engine |
| `knockSettings` | Spark | Ignition |
| `lambdaTable1Tbl` | 3D Tuning Maps / Tuning > Engine Protection | Fuel |
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
| `spark2Tbl` | 3D Tuning Maps | Ignition > Secondary Spark |
| `sparkSettings` | Spark | Ignition |
| `sparkTable2Dialog` | Tuning | Ignition > Secondary Spark |
| `sparkTbl` | 3D Tuning Maps / Spark / Tuning > Engine Protection | Ignition |
| `stagingTableDialog` | Tuning | Fuel > Secondary Fuel |
| `stagingTbl` | 3D Tuning Maps | panel in Staged Injection |
| `stm32cmd` | Hardware Testing | Hardware Testing |
| `tacho` | Accessories | Outputs |
| `triggerSettings` | Settings | Engine |
| `veTable1Tbl` | 3D Tuning Maps | Fuel |
| `veTableDialog` | Tuning > Engine Protection | Fuel |
| `vssSettings` | Accessories | Sensors > Inputs |
| `vvt2Tbl` | 3D Tuning Maps / Accessories | Outputs > VVT |
| `vvtSettings` | Accessories | Outputs > VVT |
| `vvtTbl` | 3D Tuning Maps / Accessories | Outputs > VVT |
| `warmup` | Startup/Idle | Warmup |
| `wmiSettings` | Accessories | Boost > Power Adders |
| `wmiTbl` | 3D Tuning Maps / Accessories | Boost > Power Adders |

Three tables (`boostTbl`, `boostDCLupTbl`, `stagingTbl`) are no longer listed
in `[Menu]` because they are embedded as `panel =` inside `boostLoad` and
`stagingTableDialog`. That is intentional — the containing dialog also carries
the mode/switch settings, so this is one entry instead of two.

## 4. Deliberately unreachable

| Object | Why |
|---|---|
| `Canout_config` + 8 child dialogs | CAN output configuration chain. Disabled and left disabled; not validated on DropBear. |
| `warmup_afr_curve`, `warmup_analyzer_curve` | Used only by `[WueAnalyze]`, never a UI panel. |

## 5. `indexCard` status

`idleSettings` uses the `indexCard` layout (PWM / Stepper / Closed Loop as
tabs). This is a **pilot**. The layout is documented in the INI spec (added in
iniSpec 3.38, and this file is 3.64) but is used by no shipping INI in the
wild — MS3, MS2 and rusEFI all build complex dialogs from `border` + nested
`xAxis`/`yAxis`. It is confined to this one dialog until tabs are confirmed
rendering in TunerStudio. If the layout is not supported the child panels
stack, which is what the dialog did before, so it degrades to prior behaviour
rather than breaking.

If tabs do not render, the fallback is a `border` layout with nested
`xAxis`/`yAxis` wrapper dialogs — the idiom rusEFI uses everywhere.

## 6. What did not change

- **No `[Constants]` edits at all.** Tune offsets are a frozen ABI
  (see `AGENTS.md` §2A). The `[Constants]` digest is
  `6aedac05452d1188` and must stay that way unless a tune-format break is
  deliberately agreed. The linter prints the digest on every run so a drift is
  obvious.
- No gauge, datalog or `[FrontPage]` changes.
- No firmware changes: this is an INI-only reorganisation.
