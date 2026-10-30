# Drag-Racing ECU Feature Gap Analysis & Prioritised Roadmap

**Target:** Speeduino fork on **DropBear v2 + Teensy 4.1** (`pinMappingId 55`, `env teensy41`)
**Benchmarked against:** MS3Pro EVO / Ultimate, Haltech Elite 2500 / Nexus R3-R5, MoTeC M1 (M130/M150/M190 + GPR Drag packages), Holley Dominator EFI
**Storage basis:** W25Q32JV emulated EEPROM, **768 sectors = 23,808 bytes**, addresses `0x0000..0x5CFF`
**Core tune:** `0x0000ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“0x0FFF` (4096 B, frozen ABI)
**Free for new features:** `0x1000ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“0x5CFF` = **19,712 bytes**

> Scope of this document: **analysis only.** No firmware was modified for this report.
> Every "current state" claim below was read out of the source tree, not assumed.

### Revision note (v2)

Four claims in the first draft were wrong and have been corrected throughout. Most came from
trusting a summary rather than reading the board files. Recording them so the same mistakes
are not repeated:

| Claim | Reality |
|---|---|
| "Only one VSS input; multi-wheel-speed is board-rev gated" | **Wrong.** `vssPin` is a 6-bit field exposed in TunerStudio accepting **any pin 3ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“53 or A8ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“A15** (`speeduino.ini:457`, applied by `init.cpp:345`). The hardware can take extra speed inputs today. What is missing is firmware: one `pinNumbers.pinVSS`, one ISR, one ring buffer. |
| "SD/RTC unvalidated, blocks logging" | **Wrong.** `board_teensy41.h:70-73` defines `RTC_ENABLED`, `SD_LOGGING`, and `SD_CONFIG SdioConfig(FIFO_SDIO)` unconditionally. RTC needs a coin cell. The logging gap is **rate** (30 Hz), not hardware. The stale line is `AGENTS.md:19`. |
| "`pinVSS = 34` and `pinTrigger3 = 34` is a bug" | **Wrong.** Pin 34 is documented on the DropBear site as *"Spare Digital In 2 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â 12 V or ground switching digital input. Can be used for VSS, Idle Up etc. This is also the default input for a secondary Cam input if using dual VVT."* It is **deliberately** the default for both roles because the two uses are mutually exclusive. Not a defect ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â just don't try to use it as VSS and cam-3 at once. |
| "~2.25 MB of the flash chip is unused, use it for extra tunes" | **Wrong.** 768 sectors ÃƒÆ’Ã¢â‚¬â€ 4 KB = 3.0 MB, from the 1 MB base ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â that is the chip's *entire* free space (4 MB total). The emulated EEPROM already claims all of it. But see MAP-01: **multi-map belongs on the SD card**, so the EEPROM shortfall doesn't matter. |

### Terminology

- **Table switching** (exists today) ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â one table (fuel2 / spark2) is swapped by pin or condition.
- **Multi-map / multi-cal** (does not exist) ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â several **complete independent tunes** stored in
  the ECU and selected by dash switch. Different feature, much larger storage cost. See Ãƒâ€šÃ‚Â§6.
- **Speed channel** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the hardware pin is user-configurable; the *firmware channel* (ISR +
  ring buffer + derived gear) is what is limited to one.

---

## 0. Executive summary

The firmware today is an **excellent single-purpose speed-density fuel+spark controller with surprising depth in a few places** (sequential 8-cyl, per-cylinder trims, flex fuel, VVTÃƒÆ’Ã¢â‚¬â€2, WMI, rotary, oil-pressure protection, closed-loop boost) and **nothing at all** in the areas that define a drag ECU:

| Drag-critical capability | State |
|---|---|
| Traction control of any kind | **Does not exist** |
| Multiple wheel/driveshaft speed inputs | **Not usable today** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â VSS pin is user-selectable in TS (any pin 3ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“53 or A8ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“A15), so 2+ inputs are physically available, but firmware has **one** VSS object, one ISR and one ring buffer. 2nd input is wiring + firmware, not a board rev |
| Time-from-launch as an axis for anything | **Does not exist** (no timer framework) |
| Boost by time / by speed / boost builder / scramble | **Does not exist** |
| Transbrake, bump box, 3-step, burnout limiter | **Does not exist** |
| Anti-lag / rolling anti-lag | **Does not exist** |
| Per-cylinder EGT | **Does not exist** (no EGT at all) |
| Multi-stage progressive nitrous with timers | Partial (2 stages, RPM ramp, no timers) |
| High-rate internal logging | Does not exist **as a capability** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â SD + RTC are compiled in by default (`SD_LOGGING`, `RTC_ENABLED`, SDIO FIFO) and only need an RTC coin cell; the gap is rate (30 Hz max), not hardware |
| Multi-map selection | Does not exist. *Table switching* exists (fuel2/spark2 swap one table); there is no way to store and select between several **complete separate tunes** |
| Torque / power management by gear & time | **Does not exist** |

**The single most important finding:** the blocker is **not** features, it is **three missing foundations**.

1. **No generic table framework.** Every table is a bespoke global with a bespoke offset. Adding 30 competitor-style correction tables this way is impossible within 19,712 bytes and unmaintainable. MS3/Haltech/Holley all win mainly because a tuner *builds* the correction; the firmware only needs to supply a generic 1D/2D/3D engine.
2. **No time base.** Drag racing is **time domain**: boost vs seconds-from-launch, retard vs seconds, nitrous ramp vs seconds, perfect-pass curve vs seconds. There is no `raceTime` channel and no timer object. Everything drag-specific hangs off this.
3. **No multi-speed framework.** The *hardware* can do this: `vssPin` is a 6-bit user-selectable field in TunerStudio accepting **any pin 3ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“53 or A8ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“A15** (`speeduino.ini:457`), so additional speed inputs are assignable without a board change. What does **not** exist is the firmware side: a single `pinNumbers.pinVSS`, a single `vssPulse` ISR (`init.cpp:229-231`), and a single `vssTimes[VSS_SAMPLES]` ring buffer (`sensors.cpp:34`). Traction control, wheelie detection and perfect-pass each need their own speed object.
   *Planned hardware mapping:* **pin 34 = rear, pin 35 = front** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â one sensor each. This is the industry-standard front/rear arrangement (Holley 6 speed inputs, MS3 4 VR inputs, Haltech 4 wheel + drivetrain). Four-corner CAN-based ABS speed is available on Dominator/MoTeC-class hardware but typically only arrives at **30ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“50 Hz**, which is far too slow to be the primary traction-control input.

   **Dedicated front/rear wheel speed sensors polled at high rate beat CAN ABS speed for TC.** That is the right call, not a compromise ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â but it means the firmware side matters more, not less: a per-channel ISR + ring buffer + a ÃƒÂ¢Ã¢â‚¬Â°Ã‚Â¥200 Hz consumer.

Add those three (ÃƒÂ¢Ã¢â‚¬Â°Ã‹â€ 2,500ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“3,500 of the 19,712 free bytes) and **roughly 60 % of the gap list below becomes buildable incrementally without further architectural work.**

**On hardware:** the binding constraints for traction control are **not** the speed inputs ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â those are configurable today, and adding a third and fourth is wiring plus firmware. The genuine board limitations are **per-cylinder EGT amplification** (no thermocouple amps onboard) and **multi-channel knock** (one conditioned input). Speed-input expansion is cheap; EGT and knock are not.

---

## 1. Method

- Firmware inventory: read from `speeduino/*.cpp|*.h`, `speeduino/src/controllers/*`, `speeduino/src/decoders/*`, `speeduino/src/pins/pinMapping.cpp`, `speeduino/config_pages.h`, `speeduino/pages.h`, `speeduino/storage.cpp`, `reference/speeduino.ini`, `AGENTS.md`.
- Absences verified by explicit negative grep, not by assumption (`traction`, `slip`, `EGT`, `antilag`, `ALS`, `dbw`, `shiftCut`, `cruise`, `transmission`).
- Competitor feature lists compiled from manufacturer documentation and published tuning references; they are descriptive of capability class, not of any specific model year.

---

## 2. Baseline ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â what this firmware actually has

### 2.1 Verified present

| Area | Detail |
|---|---|
| **Fuel modes** | Paired, semi-sequential (4-cyl, pairing selectable), banked, **full sequential**; port + TBI |
| **Channels** | `INJ_CHANNELS 8`, `IGN_CHANNELS 8` ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ 8 cyl / 8 inj / 8 coils (`board_teensy41.h:90-91`) |
| **Staging** | 2 banks, TABLE/AUTO mode, 8ÃƒÆ’Ã¢â‚¬â€8 staging table, primary/secondary injector sizing |
| **Injector timing** | `injAng[4]` + RPM-dependent `injAngRPM[4]`, opening time, voltage correction (6 pt) |
| **Per-cylinder trim** | `trimTables[8]` fuel, 6ÃƒÆ’Ã¢â‚¬â€6 each; `config13.ignTrim[8]` spark ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â **static, not closed-loop** |
| **Corrections** | WUE, ASE, cranking, priming, IAT density (9 pt), baro (8 pt), AE TPS/MAP, DFCO + taper, flex (fuel/boost/advance), fuel temp |
| **Second fuel/spark table** | 16ÃƒÆ’Ã¢â‚¬â€16 each, mode OFF / MULTIPLY / ADD / CONDITIONAL_SWITCH / INPUT_SWITCH |
| **Ignition** | Wasted spark, wasted COP, single coil, **sequential**, **rotary** (FC/FD/RX8 with split table) |
| **Dwell** | Crank/run/limit, voltage correction (6 pt), optional 4ÃƒÆ’Ã¢â‚¬â€4 dwell table |
| **Knock** | Single input, digital or analog, **6 windows** (RPM/angle/duration), retard strategy w/ step/recovery |
| **Boost** | Open loop + **closed loop PID**, 8ÃƒÆ’Ã¢â‚¬â€8 target table, 8ÃƒÆ’Ã¢â‚¬â€8 duty-lookup table, boost cut, min/max duty, **boost-by-gear (6 gears)** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â *only when `vssMode` is EXTERNAL_KM/MI* |
| **Launch** | 2-step (soft+hard limit, retard, fuel add), **flat shift**, clutch-RPM capture, TPS gate, VSS cut-out gate |
| **Rolling limit** | `rollingProtRPMDelta[4]` / `rollingProtCutPercent[4]` ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â a rolling **rev-limit cut**, not anti-lag |
| **Idle** | ON/OFF, PWM OL/CL/OL-CL, stepper OL/CL/OL-CL, 2 PWM channels, crank steps/duty, taper, idle-up |
| **VVT** | 2 channels, ON/OFF + OL + CL PID, 8ÃƒÆ’Ã¢â‚¬â€8 tables, load source MAP/TPS |
| **Nitrous** | 2 stages, arming pin, RPM window, progressive fuel adder (adderMinÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢adderMax), fixed retard |
| **WMI** | Simple / proportional / open-loop / closed-loop modes, 8ÃƒÆ’Ã¢â‚¬â€8 table, advance correction curve, indicator + empty pins |
| **Protection** | RPM soft/hard, coolant-vs-RPM limit, boost cut, **oil pressure (RPM-dependent minimum + time)**, AFR deviation. Cut = ignition / fuel / both |
| **Gear detection** | Ratio-based: `vss/RPM` pulses-per-1000rpm vs `vssRatios[6]`, Ãƒâ€šÃ‚Â±10 hysteresis, 10 Hz |
| **VSS** | **One active channel, but a user-selectable pin.** `vssPin` is a 6-bit field in TS ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â any pin 3ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“53 or A8ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“A15 (`speeduino.ini:457`). Modes `VSS_MODE_OFF` / `INTERNAL_PIN` (via aux ch) / `EXTERNAL_KM` / `EXTERNAL_MI`; `vssPulsesPerKm`, `vssSmoothing` |
| **Tables** | VE / spark / AFR / fuel2 / spark2 = **16ÃƒÆ’Ã¢â‚¬â€16**, 8-bit values+axes. Boost, VVTÃƒÆ’Ã¢â‚¬â€2, WMI, staging = **8ÃƒÆ’Ã¢â‚¬â€8**. Trims = **6ÃƒÆ’Ã¢â‚¬â€6**. Dwell = **4ÃƒÆ’Ã¢â‚¬â€4** |
| **Lambda** | Narrow or wide, SIMPLE or PID, limits, delay, temp/RPM/TPS/MAP gates, `afrTable` 16ÃƒÆ’Ã¢â‚¬â€16 |
| **CAN** | 16 aux **inputs** (CAN/serial/analog/digital), 8 param-group **outputs**, realtime broadcast, O2-over-CAN, dash/check-engine bytes, secondary serial |
| **Decoders** | **28** primary + 5 secondary cam patterns |
| **Programmable I/O** | **8** rules (config13), comparators + AND/OR/XOR, activation delay, min/max time limit, cascade/"virtual data" reuse, per-rule ign trim. **Digital on/off only ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â no PWM** |
| **Aux channels** | 16, **inputs only** |
| **Logging** | Composite/tooth logs + SD logger. **SD and RTC are compiled in unconditionally** (`board_teensy41.h:70-73`: `RTC_ENABLED`, `SD_LOGGING`, `SD_CONFIG SdioConfig(FIFO_SDIO)`) ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â RTC needs only a coin cell. Rate is the limitation: 1/4/10/30 Hz. Triggers on RPM / protection / Vbat / pin |

### 2.2 Verified absent

`traction` Ãƒâ€šÃ‚Â· `slip` Ãƒâ€šÃ‚Â· `EGT` Ãƒâ€šÃ‚Â· `antilag` / `ALS` Ãƒâ€šÃ‚Â· `dbw` / drive-by-wire / ETB Ãƒâ€šÃ‚Â· `shiftCut` Ãƒâ€šÃ‚Â· `shiftLight` Ãƒâ€šÃ‚Â· `cruise` Ãƒâ€šÃ‚Â· transmission/TCU control Ãƒâ€šÃ‚Â· fuel-pressure protection Ãƒâ€šÃ‚Â· IAT protection Ãƒâ€šÃ‚Â· MAF Ãƒâ€šÃ‚Â· table **blending** (only switch/add/multiply) Ãƒâ€šÃ‚Â· generic **PWM** output Ãƒâ€šÃ‚Â· arbitrary CAN TX/RX frames Ãƒâ€šÃ‚Â· per-cylinder knock Ãƒâ€šÃ‚Â· per-cylinder closed-loop lambda Ãƒâ€šÃ‚Â· multi-map slots Ãƒâ€šÃ‚Â· any **time-from-launch** axis Ãƒâ€šÃ‚Â· a **second/third/fourth speed object** (the VSS *pin* is user-selectable; the firmware only has one channel) Ãƒâ€šÃ‚Â· lap/segment timer Ãƒâ€šÃ‚Â· pit limiter Ãƒâ€šÃ‚Â· torque management.

### 2.3 Known defects that gate future work

| # | Issue | Evidence |
|---|---|---|
| D1 | **Boost-by-gear silently does nothing in the default VSS configuration.** `getBoostByGearMode()` requires `isExternalVssMode()`; `VSS_MODE_INTERNAL_PIN` falls through to `Off`. A tuner configures per-gear boost, sees no effect, and blames the hardware. | `boostController.cpp:68,72`; `config_pages.h:346-349` |
| D2 | **Gear detection is a 10 Hz poll of a single ratio window with Ãƒâ€šÃ‚Â±10 hysteresis.** Useless as an axis for a 200 Hz traction controller; will chatter on wheelspin. | `sensors.cpp:785-812, 874` |
| D3 | **Staging is blocked above 4 cylinders** unless `injType == TBODY`. | `speeduino.ini:2058,2251` |
| D4 | **WMI drives no PWM valve** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â `wmiEnabledPin` is `pinMode(OUTPUT)` + on/off only; "proportional/closed-loop" modes only affect an *advance correction curve*. `wmiEnabled` also **shares `pinVVT_2`**. | `init.cpp:450`; `globals.cpp:59`; `config_pages.h:772-796` |
| D5 | **Control-loop rates are low.** Boost CL is gated at 10 Hz; gear at 10 Hz; aux channels at 4 Hz. Nothing engine-protection or traction-relevant runs faster than that. | `boostController.cpp:217`; `auxChannelController.cpp:162` |
| D6 | **No reserved space discipline.** `config10` has 192 B declared with only bit-level scraps unused; `config13` has 10 spare bytes; `config15` has **150 spare bytes** (only ~106 of 256 used). Usable, but it is centralising config that matters, not scavenging bitfields. | `config_pages.h:647,844,893,862,933` |
| D7 | **`0x1000ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“0x5CFF` is unused today.** `EEPROM_CUSTOM_BASE` appears only in `AGENTS.md`, never in code. The capacity win is banked but not yet spent. | grep `EEPROM_CUSTOM_BASE` |

---

## 3. Competitor capability summary (condensed)

### 3.1 MS3Pro EVO / Ultimate
10 inj / 8 spark (Ultimate: 12 seq), 4 VR inputs, 2 knock, 8 GP analog. **4 VVT channels.** VE 1ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“4 + Ign 1ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“4 + AFR 1ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“2, 16ÃƒÆ’Ã¢â‚¬â€16, 16-bit values, extensible to 30ÃƒÆ’Ã¢â‚¬â€16 via switching.
Drag set: **2-step + 3-step/burnout**, flat shift, **trans-brake with delay + bump-box + throttle-stop + line-lock**, pit limiter, **boost by gear / by VSS / by time-from-launch (12 pt)**, boost builder one-shot, dome pressure control, overboost cut.
TC: **VSS %slip (VSS1 vs VSS2, side-to-side pairs)**, **"Perfect Run"** (actual vs best-time speed curve), "Perfect Run RPM" (no VSS), switch input, VSS1-vs-VSS2 retard table. Reactions = retard / rolling cut / add fuel / cut nitrous / reduce boost.
Knock: **2 channels, per-cylinder windows, per-cylinder retard.** EGT via **CAN-EGT (8 TC + 8 WBO2)**. Sequential shift cut + air shifter. Auto trans = separate MicroSquirt TCU over CAN. No native DBW (uses external DBWX2 over CAN). No native lap timer.

### 3.2 Haltech Elite 2500 / Nexus R3-R5
8ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“18 inj, 8ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“12 ign, 2 knock, 4 speed-capable inputs, dual DBW (Nexus), onboard wideband (Nexus), onboard **PDM**, 6-axis IMU (Nexus). **32ÃƒÆ’Ã¢â‚¬â€32ÃƒÆ’Ã¢â‚¬â€8 "4D" tables** (3D + 8-slice fourth axis = gear / cam / ethanol / trim-knob / race-time / boost level).
Drag set: **2-step via aux limiter**, 3-step (burnoutÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢launchÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢main), **launch anti-lag**, **rolling anti-lag**, flat shift with **closed-loop / while-active / timed** modes + **DBW auto-blip on downshift**, **trans-brake with Bump & Creep (3D pulse duty)**, **Race Timer** function as the native time axis, boost by gear/speed/**time**/cam/flex/TPS/temp, CO2 dome dual-solenoid.
TC: **PID-from-vehicle-speeds** and **Percentage-Cut** (preferred). 3D **Desired Slip** table + 3D **Cut Percentage** table vs slip error; fuel or ignition cut; ramp-out; enable by switch/CAN/condition.
**Advanced Torque Management** (R5/2500T/REM): target **driveshaft RPM and/or engine RPM**, retard tables with race-time/gear axes, **cylinder cut %**, boost cut, nitrous delay, torque knob.
Full **integrated auto-trans control** (4L60E/4L80E/4R70W/A340E + presets), torque-converter lockup, paddle shift with request stacking.
**Up to 6 nitrous stages**, progressive vs timer/RPM/boost, per-stage retard + per-cylinder correction, bottle opener, NOS pressure.
**3 severity levels** of engine protection, per-level actions incl. **DBW max-throttle limit** and **per-cylinder EGT shutdown**.
Logging: 40 ch @ 200 Hz (2500) ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ **300 ch @ 1 kHz, 512 MB** (R5). EGT via CAN TCA2/TCA4 (12 ch). No native GPS/lap timer in ECU.

### 3.3 MoTeC M1 (M130/M150/M190 + GPR Drag)
8ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“12 cyl, 12ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“18 inj, 12 ign, 2ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“4 knock, 3 CAN (M150/M190), **per-cylinder knock with retard**, fully **PWM-capable low-side and half-bridge outputs**, M1 Build IDE with a **C-like scripting language at 1 kHzÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“1 Hz** and user-defined channels/parameters/tables.
**Torque-based architecture** (Pro packages): torque limits by engine speed, vehicle speed, gear, wheel torque, **race time**, fuel composition.
Drag set: **Race Time system** with trims for ignition / mixture aim / boost limit / torque limit, **false-start detection and reset**, multi-step staging, **launch trajectory** (predicted speed vs time), clutch-bite threshold, **Vehicle Pitch Control (anti-wheelstand)**, suspension position logging at 1 kHz, wheelie-bar pressure, tyre pressure/temp, **shift light**, boost servo, **dual-bank boost**, dome pressure CL.
TC: closed-loop **front-to-rear slip ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ "Traction Engine Speed Limit"** (reuses the rpm limiter, hence very fast), Aim Main / Aim Compensation / Control Range tables, rotary-switch aim, fastest-driven-wheel option, torque reduction via throttle/retard/cut, wheel- or engine-speed based for converter cars, **GPS speed fallback**.
Anti-lag: Circuit / Rally / **Rolling (roll-racing)** modes with cam-aim overrides. GCIC flat shift with 3 trigger modes incl. **gear-lever strain gauge** and **throttle-reduction-rate**.
Transmission: **GPRP paddle shift** (closed-loop shift timing, retries, air-pump pressure monitoring, trans-brake/bump, converter slip monitoring) and **GP-AT** auto-trans (5 gears, line-pressure + 3 pressure solenoids + 4 shift solenoids, proportional TCC).
Logging: **1000 Hz, 2000 channels**, 8 log sets, partition/scrutineer access, **built-in 3-axis G**, GPS, i2 Pro analysis, telemetry (300 ch, encrypted, radio/GSM/LTE).
Security: public-key signed packages, multi-user access levels, secured log partitions.

### 3.4 Holley Dominator EFI
12 inj drivers (24 injectors w/ expanders), 12 DIS, **~50 inputs incl. 6 speed inputs (digital or inductive)**, 36 outputs (20 high-side PWM + 16 ground PWM), **80 software inputs / 62 software outputs with full pin mapping**, 2 CAN, **dual onboard wideband**.
Drag set: **3-step** (main + launch + burnout), launch/T-brake on one pin, **Staging ICF (single-pulse = bump box, duty-cycle = creep)**, **Boost vs Time / vs RPM / vs Speed / vs Gear, independently per gear**, **Launch Target** starting all timers at brake release, **Boost Scramble Ãƒâ€šÃ‚Â±**, **Boost Builder (fixed timing + fixed fuel + N2O solenoid) = anti-lag**, **rev-limiter offset tables** used as build-your-own protection.
**Active Speed Management** (paid unlock): **time-from-launch vs engine and/or driveshaft speed**, import a reference curve from a datalog/Racepak CSV, Base ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ Retard A ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ Retard B ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ Rev Limit curve, **cylinder drop 1ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢4** on a V8, plus external **Davis Profiler/SmartDrop** integration (25Ãƒâ€šÃ‚Â° retard, 1000 Hz).
**8 nitrous stages** with delay + duration + progressive (time/RPM/boost), per-stage retard, cumulative fuel-flow summary.
**Integrated auto-trans** (4L60E/4L80E/AODE/4R70W) + **Racing Transmission ICF** (Powerglide/Liberty/Lenco/generic, shift by RPM and/or time-from-launch, pedaling strategy), shift power management (per-gear cut + 1ÃƒÆ’Ã¢â‚¬â€16 offset curve), 4 converter-lockup outputs, 4 dump-valve outputs, spool assist.
**Advanced ICF tables: 8ÃƒÆ’Ã¢â‚¬â€1D, 8ÃƒÆ’Ã¢â‚¬â€2D, 8ÃƒÆ’Ã¢â‚¬â€1D-per-gear, 8ÃƒÆ’Ã¢â‚¬â€2D-per-gear** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â user-definable axes, scalable, smoothing. **4 calibrations** switchable in-ECU.
Logging: **4 GB internal, every channel always logged, 1ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“100 frames/s**, `.SL` system logs capturing every crank/cam edge.
Gaps: no rotary, no native flex algorithm, no true dual-fuel, **no per-cylinder knock retard**, no OEM CAN gateway, no VVT/VVL (Coyote module only).

---

## 4. Gap matrix

Legend: **ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦** present Ãƒâ€šÃ‚Â· **ÃƒÂ¢Ã¢â‚¬â€Ã‚Â** partial Ãƒâ€šÃ‚Â· **ÃƒÂ¢Ã…â€œÃ¢â‚¬â€** absent Ãƒâ€šÃ‚Â· **ÃƒÂ¢Ã¢â‚¬ÂºÃ¢â‚¬Â** hardware-blocked

### 4.1 Foundations
| Capability | This FW | MS3 | Haltech | MoTeC | Holley |
|---|---|---|---|---|---|
| Generic user-defined 1D/2D/3D correction tables | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ | ÃƒÂ¢Ã¢â‚¬â€Ã‚Â (blend curves) | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ 4D axis | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ Build | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ Advanced ICF |
| Generic PWM output | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ 3ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“7 PWM GP | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ DPO/HBO | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ all outputs | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ 36 outputs |
| Arbitrary CAN TX/RX frames | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ (DBC import) | ÃƒÂ¢Ã¢â‚¬â€Ã‚Â module-based |
| Race-time / timer framework | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ Race Timer | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ Race Time | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ (timers start at brake release) |
| Multiple speed channels | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ (1 object; pin is user-configurable) | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ 4 | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ 4 + drivetrain | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ 4 + GPS | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ 6 |
| Multi-map slots (several complete tunes) | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ | ÃƒÂ¢Ã¢â‚¬â€Ã‚Â (2 dual-fuel) | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ 8 via 4D | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ 8 switches + 8 rotaries | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ 4 cals |
| Config storage beyond 4 KB | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ **19,712 B** | n/a | n/a | n/a | n/a |
| Control-loop rate ÃƒÂ¢Ã¢â‚¬Â°Ã‚Â¥ 100 Hz for driver aids | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ (10 Hz max) | ÃƒÂ¢Ã¢â‚¬â€Ã‚Â 5 ms logs | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ 1 ms | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ 1 kHz | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ 1 kHz |

### 4.2 Fuel
| Capability | This FW | Notes |
|---|---|---|
| Sequential 8-cyl | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ | matches Haltech 2500 |
| Staged injection | ÃƒÂ¢Ã¢â‚¬â€Ã‚Â | **ÃƒÂ¢Ã¢â‚¬Â°Ã‚Â¤4 cyl only** (D3); 2 stages; no per-stage duty logging |
| Per-cylinder trim | ÃƒÂ¢Ã¢â‚¬â€Ã‚Â | static 6ÃƒÆ’Ã¢â‚¬â€6 tables, not closed-loop |
| Per-cylinder closed-loop lambda | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ | MS3/MoTeC/Haltech all offer per-bank at minimum |
| Flex fuel | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ | strong ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â fuel/boost/advance trim curves |
| Flex **table blending** | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ | competitors blend entire VE/spark maps by ethanol % |
| Dual fuel (two independent systems) | ÃƒÂ¢Ã¢â‚¬â€Ã‚Â | fuel2 is a modifier, not a second fuel system |
| Injector timing tables | ÃƒÂ¢Ã¢â‚¬â€Ã‚Â | 4-point angle + RPM, not 3D |
| Dead-time vs voltage | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ | 6-point |
| Returnless / closed-loop fuel pressure | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ | MS3 and MoTeC both do PWM-to-target-rail-differential |
| Fuel pump PWM | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ | on/off only via `fuelPumpController` |
| MAF | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ | SD / TPS / IMAP-EMAP only |
| Algorithm blending (SDÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬ÂAlpha-N) | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ | Holley "VE Combo" is the reference |
| Long-term fuel trim | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ | TS/VEAL-side only |

### 4.3 Ignition & knock
| Capability | This FW |
|---|---|
| Sequential COP 8-ch | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ |
| Rotary FC/FD/RX8 split | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ (Holley has none ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â we are ahead here) |
| 3D dwell table | ÃƒÂ¢Ã¢â‚¬â€Ã‚Â 4ÃƒÆ’Ã¢â‚¬â€4 only |
| Per-cylinder spark trim | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ (`ignTrim[8]`) |
| **Per-cylinder knock** | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ (1 sensor, 6 global windows) |
| Knock window per cylinder | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Multi-channel knock | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ (1 input) |
| Knock ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ fuel add + cut actions | ÃƒÂ¢Ã¢â‚¬â€Ã‚Â retard only |
| Multispark / multipulse | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Ignition cut as torque actuator | ÃƒÂ¢Ã¢â‚¬â€Ã‚Â (rev limiter only) |

### 4.4 Boost
| Capability | This FW |
|---|---|
| Closed-loop PID | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ |
| 4D / high-res boost tables | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ (8ÃƒÆ’Ã¢â‚¬â€8) |
| **Boost by gear** | ÃƒÂ¢Ã¢â‚¬â€Ã‚Â **broken on internal VSS (D1)** |
| **Boost by speed** | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| **Boost by time from launch** | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Boost builder / one-shot full duty | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Boost scramble Ãƒâ€šÃ‚Â± | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Launch target MAP override | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Dome / CO2 wastegate pressure CL | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Dual solenoid / dual bank | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Overboost cut | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ |
| Spool assist (valve open/close) | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Wastegate position / turbo speed input | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |

### 4.5 Launch, staging, shift
| Capability | This FW |
|---|---|
| 2-step | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ |
| **3-step** (burnout / launch / main) | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Launch softÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢hard time ramp | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Launch fuel table | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ (`lnchFuelAdd` scalar) |
| Launch retard **curve** by time | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Launch RPM by MAP (spool assist) | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Launch RPM by variable pot | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Flat shift | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ |
| Flat shift cut **types** (selectable) | ÃƒÂ¢Ã¢â‚¬â€Ã‚Â (implicit soft/hard windows) |
| **Trans-brake output + protection** | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| **Bump box / staging creep** | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Line-lock / throttle-stop | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Pit limiter | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| **Anti-lag / ALS** | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| **Rolling anti-lag** | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Shift light | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Shift cut (sequential/paddle) + air shifter | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| DBW blip on downshift | ÃƒÂ¢Ã¢â‚¬ÂºÃ¢â‚¬Â no DBW |
| False-start detection / reset | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |

### 4.6 Traction control
| Capability | This FW |
|---|---|
| **Anything at all** | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Front/rear slip PID | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Percentage-cut (slip-error ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ cut %) | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Perfect-pass / time-vs-driveshaft-speed curve | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Perfect-run RPM (no wheel sensors) | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Retard-based TC (spool-preserving) | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Cylinder-cut escalation | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Launch-trajectory reference | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Aim adjustment by rotary switch | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| **Hardware: 3ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“4 wheel + driveshaft speed inputs** | ÃƒÂ¢Ã¢â‚¬ÂºÃ¢â‚¬Â |

### 4.7 Protection & safeties
| Capability | This FW |
|---|---|
| RPM soft/hard, coolant-vs-RPM | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ |
| Overboost cut | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ |
| Oil pressure (RPM-dependent min + time) | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ strong |
| AFR deviation cut | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ |
| **Fuel-pressure protection** | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ (sensor read only) |
| **EGT protection** | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ (no EGT) |
| IAT-based protection | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Flex/ethanol-based limit | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Battery voltage protection | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| **Multi-severity levels + graduated response** | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ (binary cut) |
| Limp-home / sensor substitution / fallback MAP table | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| DTC set/clear + OBD-II | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Per-cylinder shutdown (EGT) | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| CEL flash codes | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |

### 4.8 Torque & gear
| Capability | This FW |
|---|---|
| Gear detection | ÃƒÂ¢Ã¢â‚¬â€Ã‚Â ratio only, 10 Hz, 6 gears |
| Gear from CAN / position sensor / RPM-drop | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Gear as 4th table axis | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| **Torque management (target torque / driveshaft RPM)** | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Torque limit by gear / speed / time / wheel torque | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Cylinder cut as an actuator | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Anti-wheelstand / pitch control | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Converter slip monitoring | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |

### 4.9 Nitrous / WMI
| Capability | This FW |
|---|---|
| Stages | ÃƒÂ¢Ã¢â‚¬â€Ã‚Â 2 (Holley 8, Haltech 6) |
| Progressive | ÃƒÂ¢Ã¢â‚¬â€Ã‚Â RPM ramp on fuel adder only |
| **Timers** (delay, duration, ramp duration) | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Per-stage retard curve | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ (scalar) |
| Activation by launch / brake release / gear / time | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Bottle heater / bottle pressure / opener | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Dry vs wet (separate fuel solenoid duty) | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| WMI PWM valve | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ (D4) |
| WMI progressive duty table | ÃƒÂ¢Ã¢â‚¬â€Ã‚Â table exists, no PWM path |
| WMI flow as % of fuel | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |

### 4.10 Data, comms, UX
| Capability | This FW |
|---|---|
| Internal logging | ÃƒÂ¢Ã¢â‚¬â€Ã‚Â SD + RTC compiled in by default (coin cell for RTC); the gap is **rate** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â 30 Hz max, no ring buffer |
| Log rate | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ 30 Hz max ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ drag needs ÃƒÂ¢Ã¢â‚¬Â°Ã‚Â¥100 Hz |
| Always-log-all-channels | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Post-trigger / pre-trigger buffer | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| System log (every crank/cam edge) | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Lap/segment timer | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Onboard G-sensor | ÃƒÂ¢Ã¢â‚¬ÂºÃ¢â‚¬Â |
| GPS | ÃƒÂ¢Ã¢â‚¬ÂºÃ¢â‚¬Â |
| Telemetry | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| CAN EGT module support | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| CAN expender (analog/PWM) support | ÃƒÂ¢Ã¢â‚¬â€Ã‚Â 16 in / 8 out, param-group only |
| TunerStudio table resolution | ÃƒÂ¢Ã¢â‚¬â€Ã‚Â 16ÃƒÆ’Ã¢â‚¬â€16 8-bit |
| Tune compare / overlay | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Multi-user access / tune lock | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Live dyno / inertia-dyno mode | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |
| Injector/coil test, per-cylinder disable | ÃƒÂ¢Ã…â€œÃ¢â‚¬â€ |

---

## 5. The exhaustive feature backlog

Effort: **S** ÃƒÂ¢Ã¢â‚¬Â°Ã‹â€  ÃƒÂ¢Ã¢â‚¬Â°Ã‚Â¤1 week Ãƒâ€šÃ‚Â· **M** ÃƒÂ¢Ã¢â‚¬Â°Ã‹â€  2ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“4 weeks Ãƒâ€šÃ‚Â· **L** ÃƒÂ¢Ã¢â‚¬Â°Ã‹â€  1ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“2 months Ãƒâ€šÃ‚Â· **XL** ÃƒÂ¢Ã¢â‚¬Â°Ã‹â€  hardware rev required.

Priority tiers:
- **P0 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â Foundations.** Nothing else scales without these. Do first, in this order.
- **P1 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â Drag-race minimum.** Without these the product is not a drag ECU. Required for a credible launch.
- **P2 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â Parity.** Needed to be taken seriously against MS3Pro / Dominator / Elite 2500.
- **P3 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â Competitive differentiators.** Where we can beat the mid-tier.
- **P4 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â Long-term / board rev.**
- **P5 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â Explicit non-goals.**

Storage column = bytes of the 19,712 available at `0x1000+`.

---

### P0 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â Foundations (do these first, in order)

| ID | Feature | Why | Effort | Storage |
|---|---|---|---|---|
| **FND-01** | **Custom EEPROM block infrastructure** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â `EEPROM_CUSTOM_BASE 0x1000`, version/magic header, per-block CRC, range-checked accessors, TS mirror page. Currently `EEPROM_CUSTOM_BASE` exists only in `AGENTS.md`. | Every item below needs somewhere to live. Building this per-feature produces 40 competing layouts. | M | 64 header + block table |
| **FND-02** | **Expand TS config pages 16ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“23** at `0x1000+` with matching `speeduino.ini` sections. Config pages 0ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“15 fill `0x0000ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“0x0FFF`; there is no growth room below. | Competitors put drag settings on 3ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“6 dedicated dialogs. Cramming them into `config10`/`config13` bitfields makes them undiscoverable. | M | 8 ÃƒÆ’Ã¢â‚¬â€ 256 = 2,048 |
| **FND-03** | **Generic correction-table engine.** N user-defined tables, each `{type: 1D/2D/3D, axis-source-x, axis-source-y, x-bins[], y-bins[], values[], combine-op, clamp, enable-pin/condition}`. Axis sources = any `fullStatus` channel. This is the Holley "Advanced ICF" / Haltech "4D axis" / MS3 "blend curve" concept. | **Highest-leverage item in this document.** Boost-by-time, retard-by-time, nitrous-by-speed, TC aim by gear, wheelie trim ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â roughly 60 % of P1ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“P3 becomes *configuration* rather than firmware. | L | 32 B descriptor + 64 B (8ÃƒÆ’Ã¢â‚¬â€8) or 256 B (16ÃƒÆ’Ã¢â‚¬â€16) each |
| **FND-04** | **Generic PWM output engine** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â per-channel `{enable, pin, freq, duty-source (channel or table), min/max duty, invert, clamps}`. Today `programmableIOControl` is digital-only (`digitalWrite`). | Needed by: bump box, staging creep, trans-brake PWM, fuel pump, WMI, dome pressure, spool-assist valves, converter lockup, tacho sweep. One engine replaces eight one-off implementations. | M | 8 ch ÃƒÆ’Ã¢â‚¬â€ 16 = 128 |
| **FND-05** | **Race-time framework.** `raceTime` channel (ms, 32-bit), armed by any configurable input edge; started by **transbrake release / launch pin release / configurable output**; frozen by configurable stop condition; multi-referenced so several consumers share one clock. False-start detection + reset. | There is no time domain in the firmware. Boost-vs-time, retard-vs-time, nitrous-vs-time, perfect-pass and torque management *all* hang off this. | M | 64 |
| **FND-06** | **Multi-speed framework.** `speed[4]` inputs, each with own pin/CAN source, teeth-per-rev, tyre circumference, smoothing; derived channels `driveshaftRPM`, `slipFrontRear`, `slipPercent`, `speedAccel`. Replace the single `vssPulse` ISR + single `VSS_SAMPLES` ring buffer with per-channel instances. | One VSS today. Traction control, perfect pass, wheelie detection, converter slip are all impossible without it. | M | 256 |
| **FND-07** | **Raise driver-aid loop rate to 200 Hz.** The timer bit already exists and is already set (`timers.cpp:65,83`); **no consumer tests it** except MAP. This is a scheduling change, not new infrastructure. Boost CL is 10 Hz-gated (`boostController.cpp:217`), gear 10 Hz, aux 4 Hz. Config lives in **RAM** (`globals.cpp:36-38`), so this never touches SPI flash. See Ãƒâ€šÃ‚Â§6A for the full timing analysis. | A 10 Hz traction controller is a rumour, not a controller. Also fixes sluggish closed-loop boost on a spooling turbo. Genuine risk is injector/coil ISR jitter at 1 kHz, not the SPI bus ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â measure `loopsPerSecond` first. | **S** | 0 |
| **FND-08** | **Fix D1 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â boost-by-gear on internal VSS.** Remove the `isExternalVssMode()` guard; use `currentStatus.gear` regardless of VSS source; publish a warning flag when `gear==0`. | Existing feature that silently doesn't work. Cheapest credibility win available. | S | 0 |
| **FND-09** | **Control/actuator abstraction.** A single "torque reduction request" bus: producers (TC, launch, flat shift, protection, torque mgmt) publish `{retardÃƒâ€šÃ‚Â°, cutMode, cutPercent, cutDuration}`, one arbiter applies. Prevents five subsystems fighting over `corrections[]`. | Every competitor that got this wrong has a class of bugs where two limiters interact badly. Design it in, not after. | M | 0 |
| **FND-10** | **SD as a second storage class.** A `storage_backend` abstraction with two implementations: EEPROM (fast, small, boot-critical) and SD (slow, large, optional). SD is already wired via SDIO FIFO (`board_teensy41.h:73`). Guard: SD may be absent, unformatted or slow ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â every consumer needs a "no card" path. Enables MAP-01, large logs, and a tune library. | Turns 19 KB into gigabytes, without pretending SD is fast. Prerequisite for MAP-01, LOG-01 and user-defined table libraries. | M | 64 (EEPROM side) |

---

### P0b ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â User-defined tables (explicitly requested; highest customisation value)

**This is your "let the user add their own functionality" requirement, and it deserves its own tier.**

Target: **at least 4, expandable to 8, of each type ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â 1D and 2D** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â where the *user* picks the
axis source and the axis label from TunerStudio, with no firmware change. This is the Holley
"Advanced ICF" / Haltech "4D axis" / MS3 "blend curve" concept, and it is the single feature that
lets the product outgrow its own firmware.

| ID | Feature | Notes | Effort | Storage |
|---|---|---|---|---|
| **GEN-01** | **Generic 1D tables (8).** `{enable, output-channel, axis-source (any `fullStatus` channel), axis units label, bin count (2ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“16), bins[], values[], clamp lo/hi, invert}`. Consumes: an input channel ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ a value. | Enables: retard vs `raceTime`, boost trim vs speed, fuel adder vs EGT, TC aim vs gear, WMI duty vs IAT, launch limit vs MAP, nitrous ramp vs time, anything a tuner dreams up. | L | 8 ÃƒÆ’Ã¢â‚¬â€ 40 = 320 |
| **GEN-02** | **Generic 2D tables (8).** Same, plus a second axis. Default 8ÃƒÆ’Ã¢â‚¬â€8, configurable to 16ÃƒÆ’Ã¢â‚¬â€16. Values 16-bit. | Enables: a full custom correction map on any two channels ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â e.g. boost-vs-(speed ÃƒÆ’Ã¢â‚¬â€ gear), retard-vs-(time ÃƒÆ’Ã¢â‚¬â€ MAP). | L | 8 ÃƒÆ’Ã¢â‚¬â€ 288 = 2,304 (8ÃƒÆ’Ã¢â‚¬â€8) |
| **GEN-03** | **User axis labels + units.** `char axisLabelX[12]`, `unitsX[8]` per table, editable in TS, stored in the EEPROM block, shown as the TS axis title. `AUXin00AliasÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦15`, `prgm_out00AliasÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦07` are existing precedent for user-named strings (`speeduino.ini:130-154`) ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â same pattern, applied to axes. | Without editable labels a generic table is unusable in practice ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the tuner can't tell what they built. | S | 8 ÃƒÆ’Ã¢â‚¬â€ 40 = 320 |
| **GEN-04** | **Output routing.** Each generic table's result addressable as an input to: a PWM channel, a programmable output, a fuel/ignition trim, a boost target, a CAN frame byte, or another generic table's axis. Pointers to the existing `programmableIOControl` "virtual data" cascade (`programmableIOControl_details.cpp:94-101`). | A table nobody can consume is decoration. This is what makes it a feature. | M | 64 |
| **GEN-05** | **TS authoring surface.** One TunerStudio dialog per table with axis-source dropdown, bin editors, curve/table view, and a "what is this connected to" readout. Mirrors the existing per-table ini blocks (`speeduino.ini:5198-5373`). | TS-side work is roughly half the total effort. Do not estimate it as free. | M | ini only |
| **GEN-06** | **Live preview + validation.** Reject an axis source that does not exist; warn on a table whose output goes nowhere; show the table's live output value during tuning. | Generic systems fail silently by nature. Instrument it from day one. | S | 0 |

**Note on 4 vs 8:** start at **8 ÃƒÆ’Ã¢â‚¬â€ 1D + 8 ÃƒÆ’Ã¢â‚¬â€ 2D**. The storage is ~3 KB, comfortably affordable, and
retrofitting a bigger count later means a config-format change. 4 is unnecessarily timid.

**Why this ranks alongside FND-03:** FND-03 is the internal plumbing (N correction tables the
*firmware* populates). GEN-01/02/03/04 is the user-facing version ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the same engine, exposed.
Build FND-03 as the engine and GEN-* as its public face; do not build them twice.

---

### P1 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â Drag-race minimum

| ID | Feature | Competitor ref | Effort | Storage |
|---|---|---|---|---|
| **LC-01** | **3-step / segmented limiters.** Main + launch/high + burnout/low, each with independent activation input, soft window, hard limit, retard, cut type. Lowest active limiter wins. | Holley 3-step; Haltech burnoutÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢launchÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢main; MS3 3-step | M | 128 |
| **LC-02** | **Timed launch ramp.** SoftÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢hard limit transition over a configurable time from launch trigger; retard curve vs `raceTime`; boost target curve vs `raceTime`. | MS3 timed retard after launch; Holley Launch Target starting all timers | M | 192 (relies on FND-03/05) |
| **LC-03** | **Launch fuel table.** Replace scalar `lnchFuelAdd` with a 2D table (RPM ÃƒÆ’Ã¢â‚¬â€ raceTime or CLT). | MS3 launch fuel; Haltech launch enrich 3D | S | 96 |
| **LC-04** | **Launch RPM by MAP** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â spool-assist launch: higher launch limit as MAP builds. | MS3 "launch limit lookup by MAP" | S | 64 |
| **LC-05** | **Launch RPM trim knob** (analog input ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ launch limit offset). | MS3 0ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“5V pot; Haltech torque-management knob | S | 16 |
| **LC-06** | **Trans-brake output** with mandatory flyback-protection warning, release delay, and arming interlocks (RPM, speed, TPS, gear). | MS3 trans-brake + delay; Haltech trans-brake; Holley 554-128 warning | M | 64 |
| **LC-07** | **Bump box / staging creep.** Single-pulse mode (N pulses, on-time, off-time) and duty-cycle creep mode with a 3D duty table vs time. | Holley Staging ICF; Haltech Bump & Creep 3D | M | 128 |
| **LC-08** | **Line-lock / throttle-stop auxiliary outputs** with interlocked arming. | MS3 line-lock staging + throttle-stop solenoid | S | 32 |
| **LC-09** | **False-start detection + reset** (movement before launch release ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ flag, cut, and reset `raceTime`). | MoTeC Race Time false-start | S | 16 |
| **TC-01** | **Traction control ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â percentage-cut** (recommended first strategy). 3D **Desired Slip** table + 3D **Cut %** table vs slip error; fuel-cut or ignition-cut; ramp-in/ramp-out rate; driven-wheel min speed; enable by switch or condition. | Haltech Percentage-Cut (preferred); MoTeC aimÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢engine-speed-limit | L | 512 |
| **TC-02** | **Traction control ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â perfect pass / time-vs-driveshaft-speed.** Reference curve imported from a datalog; actual driveshaft RPM vs reference ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ Retard A ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ Retard B ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ cylinder cut ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ limit. | Holley Active Speed Management; MS3 "Perfect Run"; MoTeC launch trajectory | L | 512 (curve) + 128 |
| **TC-03** | **Retard-first TC mode** (spool-preserving) with optional staggered cylinder cut for a V8. | MoTeC drag; Holley cylinder drop 1ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢4 | M | 64 |
| **TC-04** | **TC aim trim** via rotary/analog input (traction knob). | MoTeC rotary aim; Haltech rotary trim module | S | 32 |
| **TC-05** | **Front + rear wheel speed inputs (pin 35 = front, pin 34 = rear).** *Not a board rev* ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â both pins already exist and are assignable in TS. Work = firmware (per-channel ISR + ring buffer + ÃƒÂ¢Ã¢â‚¬Â°Ã‚Â¥200 Hz consumer), plus wiring. Supersedes the CAN-ABS route for the primary TC input. | Holley 6 speed inputs; MS3 4 VR; Haltech 4 + drivetrain | M | 128 |
| **BST-01** | **Boost by time from launch** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â 12ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“16 point target curve vs `raceTime`, combined with the existing 8ÃƒÆ’Ã¢â‚¬â€8 RPM/load table. | MS3 12-pt boost vs time; Holley Boost vs Time per gear | M | 192 |
| **BST-02** | **Boost by speed** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â target modifier vs vehicle speed. | MS3 boost vs VSS1/VSS2; Haltech by wheel speed; Holley Boost vs Speed | S | 96 |
| **BST-03** | **Launch target MAP override** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â separate boost target while trans-brake/launch active, timers starting at brake release. | Holley Launch Target | S | 64 |
| **BST-04** | **Boost builder / anti-lag one-shot** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â fixed timing + fixed fuel + optional N2O/air solenoid output, gated by trans-brake or manual input. | Holley Boost Builder; MS3 boost builder | M | 96 |
| **BST-05** | **Boost scramble Ãƒâ€šÃ‚Â±** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â switched constant offset with rate limiting, inhibited during trans-brake. | Holley Boost Scramble | S | 32 |
| **BST-06** | **Boost-by-gear upgrade** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â per-gear target *and* per-gear bias duty, not a single 6-entry percent array. | MS3 per-gear bias; Holley per-gear curve | M | 192 |
| **LOG-01** | **High-rate internal logging.** 100ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“200 Hz, all channels, pre/post-trigger ring buffer, plus a **"system log"** capturing every crank/cam edge for sync diagnosis. SD + RTC already work (RTC needs a coin cell); this is purely a rate/architecture job. | MS3 32 ch @ 200 Hz + system logs; Holley 4 GB always-log-all; MoTeC 1 kHz | L | 256 |
| **LOG-02** | **Drag-specific channels in `fullStatus`**: `raceTime`, `driveshaftRPM`, `slipFrontRear`, `slipPercent`, `tcRetard`, `tcCutPercent`, `launchState`, `stageState`, `boostTargetActive`, `boostTrimActive`, `nitrousStage1-4Duty`, `gearRaw`, `gearConfidence`. | Every competitor exposes these; without them TC is untunable | M | TS ini only |

---

### P2 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â Parity

| ID | Feature | Competitor ref | Effort | Storage |
|---|---|---|---|---|
| **AL-01** | **Anti-lag (rally/circuit)** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â cyclic fuel + spark cut, ALS retard table, air-injection output, rotational/roving idle for idle-valve bypass. | MS3 ALS; Haltech anti-lag; MoTeC Circuit/Rally modes | L | 256 |
| **AL-02** | **Rolling anti-lag** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â locks to current or preset RPM/road speed; roll-racing mode. Currently only a rolling *rev cut* exists (`rollingProtRPMDelta`). | Haltech rolling ALS; MoTeC Roll Racing | M | 96 |
| **N2O-01** | **Expand to 4ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“6 nitrous stages**, each with enable pin, RPM window, **activation delay**, **duration**, ramp duration, resume/hold/reset timer modes. | Holley 8 stages; Haltech 6 | M | 384 |
| **N2O-02** | **Progressive duty PWM** for the nitrous solenoid itself vs time / RPM / boost, separate wet-kit fuel-solenoid duty. Today only a fuel *adder* ramps; no solenoid PWM path. | Holley progressive + 554-111 driver; Haltech progressive PWM | M | 128 |
| **N2O-03** | **Per-stage retard curve** (vs time or RPM) replacing the scalar `retard`. | MS3 per-stage retard; Holley per-stage retard | S | 128 |
| **N2O-04** | **Activation by transbrake release / launch end / gear / `raceTime`.** Today: pin + RPM window only. | Haltech (trans-brake release, launch end); Holley (delay + duration) | S | 48 |
| **N2O-05** | **Bottle heater output, bottle-pressure input, low-pressure warning, bottle opener output.** | MoTeC bottle pressure + warning; Haltech bottle opener + NOS pressure | S | 48 |
| **SS-01** | **Anti-wheelstand / pitch control** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â longitudinal G or suspension/pitch input ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ retard / power cut. | MoTeC Vehicle Pitch Control | M | 64 |
| **TM-01** | **Torque management** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â target driveshaft RPM or engine RPM, retard tables with gear/race-time axes, cylinder-cut %, boost cut, nitrous delay. | Haltech Advanced Torque Mgmt; MoTeC torque-based architecture; Holley | L | 512 |
| **TM-02** | **Cylinder cut as a first-class actuator** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â per-cylinder enable mask, soft/staggered cut patterns, cut-percent parameter. Competitors use it as the fine-grained torque knob between retard and full limit. | Holley cylinder drop; Haltech cut %; MoTeC cut level | M | 64 |
| **TRQ-01** | **Torque-converter lockup control** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â min gear, min/max TPS, min RPM, unlock on shift/brake, apply/release ramp. | Holley 4 lockup outputs; Haltech PWM lockup | M | 96 |
| **TRQ-02** | **Converter / clutch slip monitoring** (engine RPM vs driveshaft RPM). | MoTeC converter slip; Haltech | S | 32 |
| **TRQ-03** | **Spool assist outputs** (close valve at target boost, reopen with hysteresis). | Holley Spool Assist (2 outputs) | S | 32 |
| **TRQ-04** | **Dump-valve / wastegate-servo outputs** with PWM tables. | Holley dump valve (4 outputs); MoTeC boost servo | M | 128 |
| **BSH-01** | **Sequential shift cut + air-shifter output** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â configurable cut type, per-gear delay, auto shift-point mode, up/down selection, shift-stack request queue. | MS3 sequential shift cut; Haltech paddle shift; MoTeC GPRP | L | 384 |
| **BSH-02** | **Shift-light output** with RPM/speed/gear conditions and per-gear set points. | MoTeC Shift Light; MS3 shift light | S | 32 |
| **BSH-03** | **Flat-shift modes** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â closed-loop (gear-position based) / while-active / timed, plus torque-reduction type selection (retard / ign cut / fuel cut / combos) and ramp-out. Today flat shift is implicit soft/hard windows only. | Haltech 3 modes; MoTeC GCIC 3 modes | M | 96 |
| **BSH-04** | **Pit / speed limiter** (VSS or RPM based, spark retard or fuel cut, DBW-independent). | MS3 pit limiter; MoTeC speed limiter | S | 48 |
| **EGT-01** | ÃƒÂ¢Ã¢â‚¬ÂºÃ¢â‚¬Â/M **EGT subsystem** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â 8 K-type channels. No hardware amplifiers on DropBear; viable paths are (a) CAN EGT modules (MS3/Holley style), or (b) a board rev. Needs FND-06-style channel framework + own config block + TS dialog. | MS3 CAN-EGT (8 TC); Haltech TCA2/TCA4 (12); MoTeC E888 (8 TC); Holley 554-186 (8) | L | 320 |
| **EGT-02** | **EGT-based per-cylinder fuel trim** and **EGT protection** (cut offending cylinder only, with delay). | MoTeC drag per-cyl EGT trim; Haltech per-cyl EGT shutdown; MS3 EGT actions | M | 256 |
| **KNO-01** | **Multi-channel knock** (2ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“4 inputs) with per-bank or per-cylinder windows and **per-cylinder retard**. Today: one input, six RPM/angle windows, global retard. | MS3 2-ch per-cyl; Haltech 2-ch FFT; MoTeC 4-ch per-cyl | L | 384 (needs 2nd conditioned input) |
| **KNO-02** | **Knock response beyond retard** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â fuel add, ignition cut, boost reduction, and a logged per-cylinder knock level. | MS3 knock fuel/cut; MoTeC knock actions | M | 96 |
| **PRT-01** | **Fuel-pressure protection** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â RPM/load-dependent minimum rail pressure, time delay, graduated response. Sensor already read (`fuelPressureEnable/Pin/Min/Max`) with **no** protection logic. | MS3 fuel pressure safety; Haltech | S | 64 |
| **PRT-02** | **IAT, coolant, battery-voltage, flex-content protection** (currently coolant-vs-RPM and oil only). | Haltech 3-level; MS3 | S | 128 |
| **PRT-03** | **Multi-severity protection (Level 1/2/3)** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â per fault: enrichment, boost correction, retard, rev limit, cut. Today protection is a binary cut. | Haltech 3 severity levels | M | 320 |
| **PRT-04** | **Sensor validation, substitution and fallback MAP table** (TPS/RPM-synthesised MAP) + limp-home. | MS3 CEL + limp + fallback MAP table | L | 256 |
| **PRT-05** | **DTC set/clear, OBD-II responses, CEL flash codes.** | Haltech DTC set/clear + OBD-II; MS3 CEL codes | M | 192 |
| **GEAR-01** | **Gear detection upgrade** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â CAN gear input, gear-position voltage sensor, RPM-drop detection (drag), signed gear, confidence/validity flag, hysteresis as a configured table rather than a `#define 10`. | MS3 (VSS ratio / analog GPS / CAN gear); Haltech 7 methods; Holley RPM-drop + TCU | M | 128 |
| **GEAR-02** | **Gear as a table axis** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â per-gear trims for fuel, spark, boost, nitrous, TC aim, idle. This is the "4D" idea; it is the difference between "boost by gear exists" and "everything is per-gear". | Haltech 4D axis; Holley per-gear ICFs | M | 128 + FND-03 |
| **MAP-01** | **Multi-map slots ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â several complete tunes, stored on the SD card.** Reserve a directory as a tune library; selecting a slot copies it into the active EEPROM region. **Costs ~0 bytes of EEPROM** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the SD card holds them. Needs FND-10 + an engine-stopped interlock. Distinct from the *table switching* that exists today (fuel2/spark2 swap one table). | Holley 4 cals + 558-407; Haltech 8 via 4D; MoTeC 8 switches + 8 rotaries | M | **0 (SD)** |
| **MAP-02** | **Blending between calibrations / flex-blended VE & spark maps** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â blend whole tables by ethanol % or a blend knob, not just offset curves. | MS3 flex table blending; Haltech flex 4D | M | FND-03 |
| **TBL-01** | **Table resolution upgrade** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â 16ÃƒÆ’Ã¢â‚¬â€16 8-bit ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ **16ÃƒÆ’Ã¢â‚¬â€16 with 16-bit values**, axes 16-bit, and 8ÃƒÆ’Ã¢â‚¬â€8 ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ 16ÃƒÆ’Ã¢â‚¬â€16 for boost/VVT/WMI/staging. 8-bit VE resolution (0.5 %) is coarse for a 2,000 hp methanol engine. | MS3 16ÃƒÆ’Ã¢â‚¬â€16 16-bit (0.1 %); Haltech 32ÃƒÆ’Ã¢â‚¬â€32ÃƒÆ’Ã¢â‚¬â€8; MoTeC 3-axis arbitrary | L | ~2,500ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“4,000 if all tables upgraded |
| **TBL-02** | **Table blending modes beyond ADD/MULTIPLY/switch** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â interpolate between two tables by a continuous input (knob, ethanol %, gear position). | MS3 combined tables; Haltech 4D | M | FND-03 |
| **CAN-01** | **Arbitrary CAN TX** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â user-defined frame, 8 byte positions each mapped to any `fullStatus` channel, own ID, own rate, big/little endian, scaling/offset. Today: 8 fixed param-group outputs. | MS3 configurable CAN broadcast; MoTeC 4 custom messages + DBC; Haltech OEM profiles | M | 256 |
| **CAN-02** | **CAN EGT / thermocouple module support** (CAN-EGT class device). | MS3, Holley, Haltech all ship these | M | 128 |
| **CAN-03** | **CAN I/O expansion** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â accept generic analog, digital, frequency and PWM nodes; publish outputs to them. | Haltech IO16/IO Box; MS3 IO-Box/JBPerf; MoTeC E888/E816; Holley CAN I/O | M | 256 |
| **CAN-04** | **OEM CAN receive + dash emulation profiles** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â ABS wheel speeds (feeds TC for free), OEM sensors, dash cluster replacement. | MS3 (Porsche/VW/BMW/Nissan/Holset VGT); Haltech OEM profiles; MoTeC full OEM | L | 512 |
| **CAN-05** | **CAN gear + CAN wheel speeds as first-class inputs** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â an alternative to extra wired sensors: accept wheel speed / gear from an external ABS or wheel-speed node over CAN. Complements TC-05, which covers the wired path. | ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â | S | 32 |
| **FUEL-01** | **Returnless / closed-loop rail pressure** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â PWM fuel pump to target rail differential, fixed vs vacuum-referenced compensation, FPDM-compatible mode. | MS3 closed-loop PWM to rail differential; MoTeC closed-loop fuel pressure | L | 256 |
| **FUEL-02** | **Fuel pump PWM + multi-pump staging.** Today: on/off only. | Haltech primary + 3 aux pumps w/ duty; MoTeC multiple/switched pumps | M | 128 |
| **FUEL-03** | **Staged injection above 4 cylinders** (fix D3). Gate at `speeduino.ini:2058,2251`. | All competitors support staging on 8 cyl | S | 0 |
| **FUEL-04** | **Staged-injection upgrade** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â up to 4 stages, per-stage logged duty, transition/"fully to secondaries" mode, secondary enrichment. | MS3 2-stage w/ transitions; Haltech 4 stages | M | 192 |
| **FUEL-05** | **Per-bank closed-loop lambda** (2 wideband channels, per-bank target and trim). | MS3 2+ EGO per-bank; Haltech bank1/bank2 LTFT; Holley dual onboard WB; MoTeC dual-bank | L | 256 (needs 2nd O2 input) |
| **FUEL-06** | **MAF support and load-algorithm blending** (SD ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬Â Alpha-N blend by RPM/TPS, MAF as a load source). | MS3 MAF + algorithm blending; Holley "VE Combo" | L | 256 |
| **FUEL-07** | **Injector timing 3D table** (today 4-point angle + RPM). | MS3 3D injection timing pri/sec | M | 128 |
| **IDLE-01** | **DBW-based idle** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â requires DBW (see P4). | Haltech, Holley, MoTeC all idle on DBW | ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â | ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â |
| **DIAG-01** | **Test modes** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â injector/coil/valve/PWM test, **per-cylinder injector & spark disable** for misfire isolation, sensor sweep. | MS3 per-cylinder disable + PWM test; all competitors | M | 64 |

---

### P3 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â Differentiators (where we can genuinely win)

| ID | Feature | Rationale | Effort |
|---|---|---|---|
| **DF-01** | **ECU-native drag "coach"** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â after each pass, auto-generate a comparison of `raceTime` vs driveshaft speed against the previous best, and annotate the log with segment splits. Big-endian rivals do this in external analysis software; doing it in TunerStudio is a visible differentiator. | MoTeC/Holley both push you into i2 Pro / Racepak | L |
| **DF-02** | **Auto-generated perfect-pass reference** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â button that converts the best saved pass into the TC reference curve (removes Holley's CSV round-trip). | Holley requires a datalog/CSV import | M |
| **DF-03** | **Per-cylinder closed-loop fuel trim from lambda or EGT** (not static tables). Nobody in the mid-tier does this well. | Requires FUEL-05 + EGT-01 | L |
| **DF-04** | **Rotary drag support package** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â we already have FC/FD/RX8 split ignition; add rotary-specific staging, oil-metering-pump stepper control and per-rotor knock/EGT. Holley has **no** rotary at all; this is a defensible niche. | Holley lacks rotary entirely | M |
| **DF-05** | **Open, documented tune format + scripting hooks** with a signed config checksum ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the anti-MoTeC proposition (they win on M1 Build, we win on openness and price). | MoTeC's moat is M1 Build | L |
| **DF-06** | **USB-native high-rate streaming** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â Teensy 4.1 USB can far exceed the 115200 serial ceiling. A 500 Hz live stream into TunerStudio/host tool would beat MS3's serial logging outright. | MS3 is serial-bound | M |
| **DF-07** | **Onboard A/B tune compare and rollback** with checksummed slots. | ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â | M |
| **DF-08** | **Race-day safety interlocks** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â arming state machine (pit ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ staging ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ armed ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ run ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ cooldown) that gates transbrake, nitrous, anti-lag and TC with visible status. Nothing in the mid-tier has a coherent one; it prevents a class of expensive failures. | ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â | M |
| **DF-09** | **Inertia / dyno mode** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â sweep control, per-cell steady-state, auto-populate VE from lambda, fixed-timing lock, driveline-loss calc. | ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â | M |
| **DF-10** | **Natively logged 6-axis IMU via CAN node** (wheelie, launch G, chassis) with pitch/roll channels available as table axes. | Haltech Nexus has onboard IMU; Holley/MoTeC need external | M |

---

### P4 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â Long-term / board-rev dependent

| ID | Feature | Blocker |
|---|---|---|
| **DBW-01** | **Drive-by-wire** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â dual TPS plausibility, pedal map, limp-home, idle via blade, DBW blip on downshift, cruise control, valet throttle limit, TC via throttle-rate limiting. | **No DBW hardware.** Teensy 4.1 can drive an H-bridge and read dual tracks; needs a board rev with a safety-rated output stage and a redundant path. MS3 also declines to do this natively (external DBWX2 over CAN) ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â a **CAN-DBW client** (accepting a target throttle over CAN, as MS3 does) is the pragmatic first step and is only effort M. |
| **TCU-01** | **Integrated auto-transmission control** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â 4L60E/4L80E/4R70W/A340E, line pressure, shift solenoids, TCC, presets. | I/O count. Haltech/Holley/EMTune all do this in-ECU; MS3 uses a separate TCU over CAN. Recommend: **CAN-linked external TCU protocol first**, in-ECU later. |
| **WBS-01** | **Wheel-speed harness/conditioning** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â Hall/VR input conditioning and connectors for 4 wheel + 1 driveshaft. Pins themselves are software-assignable, so this is a wiring/PCB convenience item, not a controller blocker. | Convenience. Interim: **CAN-05**, or wire directly to assignable pins per **TC-05**. |
| **EGTH-01** | **Onboard thermocouple amplification** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â 8 K-type channels. | Board rev. Interim: **CAN-02**. |
| **KNOH-01** | **Second/third knock channel with proper conditioning.** | Board rev (currently 1 input). |
| **RTCH-01** | ~~RTC + SD validation~~ **WITHDRAWN ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â I was wrong.** DropBear has `RTC_ENABLED` + `SD_LOGGING` + `SD_CONFIG SdioConfig(FIFO_SDIO)` compiled in by default (`board_teensy41.h:70-73`); RTC just needs a coin cell fitted. SD/RTC are not blockers. Re-scope LOG-01 around log **rate**, not hardware. | Corrected | ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â | 0 |
| **DI-01** | **Direct injection** | Not realistically in scope for this hardware. Also absent from MS3Pro and Holley Dominator; only MoTeC M1 DI and Haltech Nexus DI-class products have it. |
| **VVL-01** | **Continuous valve lift (Valvetronic/VVEL)** | Absent from MS3, Holley and (as a named function) Haltech. Discrete VTEC/VVL via switched outputs + table switching is achievable cheaply ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â see P2 GEAR-02. |

---

### P4b ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â Legacy refactors (not new features; replace what is already there)

The existing firmware is deliberately minimal ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â table sizes and control loops were squeezed to fit
a 4 KB EEPROM that this board does not have. Several subsystems should be **rewritten rather than
extended**. These are prerequisites for the drag features working well, not cosmetics.

| ID | Subsystem | Current state | Target | Effort |
|---|---|---|---|---|
| **REF-01** | **Idle control** | `idle.cpp` + `stepper.cpp` (~700 lines): ON/OFF, PWM OL/CL/OL-CL, stepper OL/CL/OL-CL. Modes are combinatorial flags (`iacAlgorithm`), taper is a bit-counted loop at 10 Hz (`idle.cpp:512-522`), targets are a 10-point CLT curve only. | Rebuild on the **Holley model**: explicit state machine (crank ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ start-hold ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ ramp-down ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ closed-loop), target RPM as a **3D table** (CLT ÃƒÆ’Ã¢â‚¬â€ gear/AC/neutral), separate P/I/D with proper anti-windup, dedicated feed-forward term for load steps (AC, fan, gear, alternator), and a real "idle kick" input handler. Should be usable on a 2,000 hp methanol engine with a big single-plane, not just a stock OEM manifold. | L |
| **REF-02** | **Correction pipeline** | `corrections.cpp` ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â a long chain of `correctionX()` functions applied additively to `corrections[]`, ~1,100 lines, no arbitration. | Replace with the **FND-09** request bus + an ordered, inspectable correction stack. Each correction should report its own magnitude to `fullStatus` for logging (a tuner must be able to see *which* correction moved). | L |
| **REF-03** | **Table system** | Each table is a bespoke global with a hand-computed EEPROM offset; `pages.cpp` maps them one by one. Adding a table means touching ~6 files. | Replace with the **FND-03/GEN-*** descriptor-driven engine. A new table should require one descriptor row, not six file edits. | L |
| **REF-04** | **Sensor layer** | `sensors.cpp` ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â per-sensor bespoke read functions with individual timer bits; `readPolledSensors()` hard-codes the set. | Descriptor-driven sensor list: pin, type, calibration, rate, fault bounds, substitute value. Makes CAN/EGT/IMU inputs first-class without new code paths. | M |
| **REF-05** | **Scheduler / loop structure** | `speeduino.ino` main loop with a bitmask `LOOP_TIMER` at 1/4/10/15/30/50/200/1000 Hz; consumers poll the bitmask inline. Correctness depends on the caller choosing a rate that divides the loop. | Explicit scheduled-task table with declared rates and deadlines, so adding a 200 Hz traction consumer is a one-line registration instead of a careful code review. Also makes the FND-07 rate increase auditable. | M |
| **REF-06** | **Status/telemetry packing** | `fullStatus` is a hand-packed 256-slot list. **158 of 256 slots are `INVALID`** (counted: `def_5/6/7` are wholly unused at 32 each, `def_8` has 24 free, plus 38 scattered) ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â `ochBlockSize` is 139 while the display list is 256. Adding a channel still means editing a `#define` and the ini in lockstep. | Reclaim the free slots deliberately and register channels from the same descriptor used by the logger (REF-04). **Room is not the constraint here ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â discipline is.** | M |
| **REF-07** | **TS protocol / page plumbing** | `comms.cpp`, `pages.cpp`, `page_crc.cpp` ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â page-by-page special cases; CRC only covers defined pages. | Generalized page/entity registration so new blocks (FND-01) get CRC coverage and TS access automatically. | M |

> **Why these are ranked below the drag features but listed at all:** each one is a tax on every
> future feature. REF-03 and REF-04 in particular turn "add a table/sensor" from a six-file edit
> into a one-line edit. Doing them *after* the drag feature set is a reasonable call ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â doing them
> *never* means the drag features get bolted onto a structure that cannot hold them.

---

### P5 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â Explicit non-goals

State these so they stop consuming design attention.

- **Cruise control** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â irrelevant to drag racing, blocked on DBW.
- **Hybrid / OEM immobiliser / anti-theft integration.**
- **Torque-converter-less EV / motor control.**
- **20ÃƒÆ’Ã¢â‚¬â€20+ or 32ÃƒÆ’Ã¢â‚¬â€32 tables** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â 16ÃƒÆ’Ã¢â‚¬â€16 with 16-bit values is the right target; the memory and TS-page cost of 32ÃƒÆ’Ã¢â‚¬â€32 is not justified.
- **Onboard GPS / lap-timer hardware** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â advertise CAN acceptance of an external node instead.
- **Telemetry radio / 4G gateway** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â out of scope; expose the data over CAN/USB and let a third party carry it.
- **Piezo direct injection** (MoTeC M141/M181 class).
- **Full auto-trans *hardware* drivers** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â do the protocol, not the solenoid bank, in the near term.
- **Multiplexed/quad SPI flash** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â `/WP` and `/HOLD` are hard-tied on the PCB (AGENTS.md Ãƒâ€šÃ‚Â§3).

---

## 6. Storage budget

Available above the frozen core: **19,712 bytes** (`0x1000ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“0x5CFF`).

| Block | Bytes |
|---|---|
| FND-01 custom block header + block index | 128 |
| FND-02 8 new config pages (256 B each) | 2,048 |
| FND-03 generic table engine plumbing | 512 |
| **GEN-01/02/03/04 user-defined tables** (8 ÃƒÆ’Ã¢â‚¬â€ 1D + 8 ÃƒÆ’Ã¢â‚¬â€ 2D + labels + routing) | **~3,008** |
| FND-04 PWM engine (8 ch) | 128 |
| FND-05 race-time framework | 64 |
| FND-06 multi-speed framework (front + rear) | 192 |
| FND-10 SD storage-class handle | 64 |
| LC-* launch/staging/trans-brake | 640 |
| TC-* traction control (aim, cut, reference curves ÃƒÆ’Ã¢â‚¬â€2) | 1,248 |
| BST-* boost by time/speed/gear + builder + scramble | 640 |
| AL-* anti-lag + rolling anti-lag | 352 |
| N2O-* 6 stages + progressive + timers | 688 |
| BSH-* shift cut, flat shift modes, shift light, pit limiter | 560 |
| EGT-* + KNO-* (multi-channel, per-cylinder) | 1,056 |
| PRT-* graduated protection, DTC, sensor validation | 960 |
| GEAR-* detection upgrade + per-gear axis | 256 |
| TBL-01 16-bit values + 16ÃƒÆ’Ã¢â‚¬â€16 secondary tables | ~2,800 |
| CAN-* arbitrary TX, EGT/I-O nodes, OEM receive | 1,152 |
| LOG-01 logging metadata/ring index | 256 |
| **Subtotal** | **ÃƒÂ¢Ã¢â‚¬Â°Ã‹â€  16,696** |
| **Remaining** | **ÃƒÂ¢Ã¢â‚¬Â°Ã‹â€  3,016** |

**Conclusion:** everything in P0ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“P3 **fits**, with ~3,000 bytes of headroom even after the full 8+8 user-defined table set. That headroom is worth keeping for TBL-01 growth and the idle-control refactor.

**MAP-01 (multi-map) costs zero EEPROM** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â it lives on the SD card (FND-10). The earlier objection was wrong.

**SD, not EEPROM, is the right home for anything large.** Storage classes:

| Class | Where | Size | Use for |
|---|---|---|---|
| **Core tune** | EEPROM `0x0000ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“0x0FFF` | 4 KB | Frozen ABI. Never touched. |
| **Extended config** | EEPROM `0x1000ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“0x5CFF` | 19.7 KB | Tables, calibration, feature config. Fast, boot-critical. |
| **Bulk / optional** | SD card (SDIO FIFO) | GB | Multi-map tune library, high-rate logs, user table libraries, per-pass reference curves, log archives. |

Rule of thumb: **if it must be readable at boot in <1 ms, it goes in EEPROM. If it is bulk, archival, or optional, it goes on SD.** Multi-map, perfect-pass reference curves and log archives are all comfortably SD-class.

---

## 6A. Can we make polling faster? ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â timing analysis for FND-07

**Short answer: yes, and the SPI concern I raised earlier was largely misplaced.** Here is the
measurement, so the answer is not another guess.

### The rate already exists ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â nothing consumes it

`oneMSInterval()` in `timers.cpp` already generates the full ladder, including:

```c
BIT_SET(TIMER_mask, BIT_TIMER_1KHZ);   // timers.cpp:65 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â every 1 ms
if(loop5ms == 5) { BIT_SET(TIMER_mask, BIT_TIMER_200HZ); }  // timers.cpp:80-83
```

So `BIT_TIMER_200HZ` and `BIT_TIMER_1KHZ` are **already set and available**. The gap is purely
scheduling: in `speeduino.ino` the main loop only ever tests 1 / 4 / 10 / 15 / 30 / 50 Hz
(`speeduino.ino:132,250,256,283,292,308,323,355`). **No consumer tests the 200 Hz or 1 kHz bit**
except `enableAnalogIsr` for MAP (`sensors.cpp:871`) and the internal `MAP_READ_TIMER_BIT`.

Making boost CL / gear / TC faster is therefore **not** an infrastructure project. It is changing
which bit a consumer tests. The 1 kHz slot is sitting there unused.

### Why the SPI concern does not apply to config reads

Every config page is a **plain RAM object**, not flash-backed:

```c
struct config2 configPage2;   // globals.cpp:36
struct config4 configPage4;   // globals.cpp:37
struct config6 configPage6;   // globals.cpp:38
```

Loaded from EEPROM once at start-up, written back only on burn. `boostController.cpp` reads
`page6.boostKP` etc. from RAM. **A faster boost control loop does not touch the flash, therefore
does not touch the shared SPI bus, therefore cannot corrupt an MC33810 transfer.**

The only flash access paths are `readFlashEeprom` / `writeFlashEeprom` (`board_teensy41.cpp:236,242`),
both of which already call `parkSharedSpiDevices()`. As long as new work stays off those two
functions at runtime, the MC33810 risk is unchanged.

### Where the real constraint is

| Constraint | Reality | Verdict |
|---|---|---|
| **200 Hz / 1 kHz timer bits exist** | Yes ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â `timers.cpp:65,83` | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ Free to use |
| **Config reads touch SPI flash** | No ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â `configPage2/4/6` are RAM (`globals.cpp:36-38`) | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ Not a risk |
| **Shared SPI bus contention** | Only on `readFlashEeprom`/`writeFlashEeprom`, already parked | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ Unchanged |
| **Main-loop budget at 1 kHz** | Unknown ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â `loopsPerSecond` is instrumented (`statuses.h:269`) but never measured on hardware | ÃƒÂ¢Ã…Â¡Ã‚Â ÃƒÂ¯Ã‚Â¸Ã‚Â **Measure first** |
| **MC33810 ISR latency** | `TMR1-4_isr` ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ `moveToNextState` ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ `sendCommand` ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ `SPI.transfer16` at 6 MHz ÃƒÂ¢Ã¢â‚¬Â°Ã‹â€  2.7 Ãƒâ€šÃ‚Âµs per 16-bit word, in ISR context | ÃƒÂ¢Ã…Â¡Ã‚Â ÃƒÂ¯Ã‚Â¸Ã‚Â Measure worst case |
| **MC33810 burst duty** | `sendCommand` is called at injector/coil open+close **plus** periodic updates. At high RPM with 8 ch it is a significant fraction of CPU | ÃƒÂ¢Ã…Â¡Ã‚Â ÃƒÂ¯Ã‚Â¸Ã‚Â **The actual risk** |
| **Non-atomic 16-bit reads** | A 200 Hz consumer reading a 16-bit status while an ISR writes it can tear. Existing code has this hazard already; more consumers = more exposure | ÃƒÂ¢Ã…Â¡Ã‚Â ÃƒÂ¯Ã‚Â¸Ã‚Â Audit |
| **64-bit `micros()` cost** | Cheap on a 600 MHz Cortex-M7, but not free at 1 kHz | ÃƒÂ¢Ã…â€œÃ¢â‚¬Â¦ Fine |

### The thing to actually worry about

It is **not** "polling faster breaks SPI." It is **injector/coil scheduling jitter**. The fuel and
ignition schedule state machines run from `TMR1ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“TMR4` ISRs. Those same ISRs call
`sendCommand()` ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ `SPI.transfer16()` to the two MC33810s. At 8 channels and 9000 rpm that is a lot
of bus traffic inside interrupt context.

Two rules follow:

1. **Do not put new heavy work in an ISR.** Keep driver aids in the main loop at 200 HzÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“1 kHz,
   never inside `TMR1ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“TMR4`.
2. **Measure before committing to 1 kHz.** 200 Hz is very likely safe on a 600 MHz M7 (5 ms budget
   vs a main loop already running thousands of iterations/second). 1 kHz is 1 ms ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â that is where
   ISR jitter can start to matter. Take `loopsPerSecond` and a GPIO-toggled timing pin on real
   hardware before deciding.

### Recommended staged approach

| Step | Change | Risk |
|---|---|---|
| 1 | Move **boost CL**, **gear**, **aux channels** from 10/4 Hz to 50 Hz | Very low ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â no new infrastructure |
| 2 | Move **engine protection** and **launch** to 200 Hz (bit already exists) | Low ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â measure loop time |
| 3 | Add **traction control** at 200 Hz | Low, once FND-06/REF-05 land |
| 4 | Evaluate 1 kHz for TC/boost only if measurement shows headroom | Medium ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â ISR jitter budget |

**Revised verdict on FND-07:** it is **not** a risky architectural change. It is a scheduling
change of effort **S**, not M, using a timer bit that already exists. The only genuine prerequisite
is measurement, and the genuine risk is injector/coil jitter at 1 kHz ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â not the SPI bus.

> Earlier I wrote that a faster loop "runs alongside the MC33810 ISRs on a shared SPI bus" as if
> that were a blocker. That was too strong. The bus is shared, but config lives in RAM, so a
> faster control loop never touches it. The circular dependency only exists on the *burn* path,
> which is already a known, documented, engine-stopped operation (AGENTS.md Ãƒâ€šÃ‚Â§4).

---

## 6B. Full polling-rate audit ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â current vs recommended

Every rate below was read from source. Rationale: this firmware was tuned for an AVR Mega with a
~16 MHz effective budget, where every cycle in the main loop mattered. **That constraint does not
exist here** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â a 600 MHz Cortex-M7 with an FPU and 1 MB of tightly-coupled RAM. Several rates are
therefore inherited anachronisms rather than deliberate choices.

### Three findings that are effectively defects, not tuning preferences

| # | Finding | Impact | Status |
|---|---|---|---|
| **P1** | **Launch + flat shift run at 10 Hz** (`launchController.cpp:108`). | **Up to 100 ms latency on a shift cut.** On a drag car that is the difference between a clean shift and bouncing off the limiter. Competitors treat flat-shift as a latency-critical path (MoTeC GCIC, Haltech closed-loop shift). This is the single worst rate in the firmware. | **FIXED** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the internal 10 Hz gate was redundant (the main loop already called the function unconditionally). Now runs every pass. |
| **P2** | **Nitrous stages run at 4 Hz** (`speeduino.ino:310`). | **Up to 250 ms latency.** A stage that cannot cut within 250 ms of an overboost or lean condition is a safety problem, not just a tuning one. | **FIXED** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â now gated at 100 Hz inside `nitrousControl()`. |
| **P3** | **Boost closed loop is gated at 10 Hz *inside* a 30 Hz outer gate** (`boostController.cpp:217` vs `:248`). | **Worse than this document originally claimed.** The original text said "~10 Hz with jitter". That is wrong. `currentStatus.LOOP_TIMER` is **cleared on every main-loop pass** (`speeduino.ino:175`) and the 30 Hz / 10 Hz bits are produced by *independent* counters (`loop33ms==33` and `loop100ms==100`), so both bits can only be present in the same mask when 33k = 100k ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â i.e. every **lcm(33,100) = 3300 ms**. Closed-loop boost was recomputing its target and running its PID roughly **once every 3.3 seconds.** That is not a sluggish loop, it is a near-dead one, and it is a plausible root cause of "closed loop boost does not work properly" reports. | **FIXED** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â inner gate removed; one 200 Hz gate now covers both open and closed loop. |

### 6B.1 Sensor reads (`sensors.h:52-79`, `sensors.cpp:860-876`)

| Sensor | Original | Now | Why |
|---|---|---|---|
| **MAP** | 1 kHz | **1 kHz ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â unchanged** | Drives fuel load and MAPdot AE. Correctly fast already; this is the one rate they got right. |
| **TPS** | 30 Hz (15/30 only) | **200 Hz** | Feeds AE and every TPS-gated feature. 30 Hz = 33 ms of lag on a tip-in. Competitors run TPS near loop rate. `TPS_READ_FREQUENCY` now accepts 200 as well as 15/30. |
| **O2 / lambda** | 30 Hz | **50 Hz** | Closed-loop lambda, AFR protection and autotune density all improve. Wideband controllers are the real limit, not the ECU. |
| **CLT** | 4 Hz | **10 Hz** | Feeds WUE, idle target, fan, protection. 10 Hz is smooth and costs almost nothing. |
| **IAT** | 4 Hz | **10 Hz** | Feeds air-density correction. Same reasoning. |
| **Battery** | 4 Hz | **10 Hz** | Drives injector dead-time and dwell correction. An alternator load step should be visible, not 250 ms late. |
| **Baro** | 1 Hz | **4 Hz** | Only needs to track weather, but 4 Hz is free and helps on quick altitude/throttle transients with a shared MAP sensor. |
| **Fuel pressure** | 4 Hz | **20 Hz** | It is a protection input. 250 ms is too slow to catch a failing pump under load. |
| **Oil pressure** | 4 Hz | **20 Hz** | Same ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â protection input, previously as slow as a coolant sensor. |
| **VSS** | 10 Hz | **50 Hz** | Feeds TC, boost-by-speed, gear detection. Edge capture is in an ISR; it is the *derived* value that was slow. |
| **Gear** | 10 Hz | **50 Hz** | Must be usable as a table axis. At 10 Hz it chatters (see D2 in Ãƒâ€šÃ‚Â§2.3). |
| **Analog ISR enable** | 200 Hz | 200 Hz ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â unchanged | Already correct. |

These are now all named macros in `sensors.h` (`VSS_READ_TIMER_BIT`, `GEAR_READ_TIMER_BIT`,
`FUEL_PRESSURE_READ_TIMER_BIT`, `OIL_PRESSURE_READ_TIMER_BIT` were previously hardcoded literals in
the `readPolledSensors()` table).

### 6B.2 Control loops

| Function | Original | Recommended | Why | Done |
|---|---|---|---|---|
| **Launch / flat shift** | **10 Hz** | **200 Hz** | See P1. Latency-critical. Flat shift needs single-digit ms, not 100 ms. | every loop pass |
| **Nitrous** | **4 Hz** | **100 Hz** | See P2. Stage cut is a safety action. | 100 Hz |
| **Boost closed loop** | 10 Hz (inside 30 Hz) | **100 Hz**, and simplify to a single gate | See P3. Competitors: MoTeC ~100 Hz+, Holley ~60 Hz. A spooling turbo needs a fast loop. | 200 Hz, single gate |
| **Knock (analog path)** | 30 Hz | **200 Hz** | Knock detection is inherently a high-rate measurement. Competitors run dedicated high-rate sampling with per-cylinder windows. | *not started* |
| **Engine protection** | Every loop (inside the sync+RPM block) | **200 Hz, and move outside the RPM gate** | Currently it cannot act below the "engine running" threshold. A stuck-throttle over-rev on a 2-step or during a stall-recovery is exactly when you want it. | 200 Hz, outside the RPM gate |
| **Idle PID** | 10 Hz | **50 Hz** | 10 Hz PID is marginal on a light flywheel / big cam. Stepper already runs every loop ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â keep that. | *not started* |
| **VVT closed loop** | 30 Hz | **100 Hz** | Cam phasing PID. 30 Hz is visibly lazy on transients. | 100 Hz |
| **Programmable I/O rules** | 10 Hz | **50 Hz** | These drive real outputs (fans, relays, solenoids). 100 ms of decisions is slow. | 100 Hz (delay counters still tick at 10 Hz ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â see note) |
| **Aux channels** | **4 Hz** | **40 Hz** | These carry VSS and CAN inputs. 4 Hz makes any CAN-fed feature unusable. | 40 Hz |
| **Fuel pump** | 10 Hz | **10 Hz ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â keep** (50 Hz if PWM added) | Relay control genuinely does not need more. | unchanged |
| **Fan** | 1 Hz | **1 Hz ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â keep** | Thermal mass. Correctly slow. | unchanged |
| **A/C** | 10 Hz | **10 Hz ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â keep** | Compressor control. Fine. | unchanged |
| **WMI** | 30 Hz | **50 Hz** (only if proportional valve added) | On/off pump is fine at 30 Hz; a PWM valve wants more. | 100 Hz (shares the VVT block) |
| **Boost sensor / tacho** | 1 kHz (ISR) | Keep | Already correct. | unchanged |

### 6B.2.1 Two things that bit during implementation

**A nested timer bit is a trap, not a rate.** `LOOP_TIMER` is cleared every main-loop pass, so
nesting bit *A* inside bit *B* does **not** give you "A Hz within B Hz" ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â it gives you the rate at
which the two counters coincide. That is why P3 was 3.3 s and not 10 Hz. If a loop genuinely needs a
different rate from its envelope, give it its own `if` at the top level, not a nested `BIT_CHECK`.

**Programmable I/O delays are counted in calls, and the INI presents them as 0.1 s.** `reference/speeduino.ini`
declares `outputDelay ... "S", 0.1` and labels the field "Activation Delay". `outputDelayCount` and
`activationDelayCount` increment once per `programmableIOControl()` call, which was correct only
because the call rate was exactly 10 Hz. Raising the evaluation rate to 100 Hz would have made every
existing tuner's delay fire ten times early ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â silently. The evaluation rate and the delay tick rate
are therefore now separate: rules are evaluated at 100 Hz, and the delay counters are advanced only
on the 10 Hz tick. **Do not "simplify" this back to one rate.**

### 6B.2.2 `LOOP_TIMER` is now 16 bit

The original ladder had 8 bits, all allocated (1/4/10/15/30/50/200/1000 Hz), so the recommended
20/40/100 Hz rates in this document were simply not expressible. `loop_timer_t` is now `uint16_t`
and bits are allocated as:

| Bit | Index | Bit | Index |
|---|---|---|---|
| `BIT_TIMER_1HZ` | 0 | `BIT_TIMER_100HZ` | 6 |
| `BIT_TIMER_4HZ` | 1 | `BIT_TIMER_200HZ` | 7 |
| `BIT_TIMER_10HZ` | 2 | `BIT_TIMER_1KHZ` | 8 |
| `BIT_TIMER_15HZ` | 3 | `BIT_TIMER_20HZ` | 9 |
| `BIT_TIMER_30HZ` | 4 | `BIT_TIMER_40HZ` | 10 |
| `BIT_TIMER_50HZ` | 5 | | |

Note `BIT_TIMER_200HZ` moved from 6 to 7 and `BIT_TIMER_1KHZ` from 7 to 8. Anything that stored a
raw bit index or an `0xFF` mask literal had to be updated. `BIT_TIMER_40HZ` is derived by dividing
the 100 Hz tick by 3 rather than from its own millisecond counter: 1000 ms is not a multiple of 25 ms,
so a 25 ms saturating counter would drift, whereas the /3 divider is exact and yields 33 pulses in a
1 s window (40 Hz over 1.2 s) with no long-term drift.


### 6B.3 Why not 1 kHz everywhere

Deliberately **not** recommended for the control loops above:

- **ISR jitter.** `TMR1ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“TMR4` handle injector and coil scheduling and already dominate interrupt
  time. Every Hz added to the main loop competes with them for cache and bus.
- **Diminishing returns.** A boost solenoid has mechanical time constants of tens of ms; 100 Hz
  is already past the actuator's response. Same for VVT oil flow and an idle valve.
- **Cost of a bad loop overrun.** If a 1 kHz main loop ever overruns, schedule jitter appears in
  injection timing ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the one thing you must not perturb. 50ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“200 Hz leaves enormous headroom.

**Rule:** rate should be set by the *actuator or sensor time constant*, not by what the hardware
can do. Only MAP, knock and the ISR-driven paths have time constants that justify 1 kHz.

### 6B.4 Implementation order

| Step | Change | Risk | Effort | Status |
|---|---|---|---|---|
| **0** | **Widen `LOOP_TIMER` to `uint16_t`** and add 20/40/100 Hz bits. *Prerequisite for steps 1ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“4: the 8 available bits were all allocated, so the recommended rates were not expressible.* | Medium ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â touches status layout, the ISR mask and every `0xFF` mask literal in the tests | **M** | **DONE** |
| **1** | **Launch/flat-shift 10 ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ 200 Hz**, **nitrous 4 ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ 100 Hz** | Very low ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â pure gate changes | **S** | **DONE** |
| **2** | **Fix the boost CL double-gate** (P3) and raise to 200 Hz | Low ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â remove inner gate, keep one | **S** | **DONE** |
| **3** | Sensor reads: TPS ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ 200 Hz, VSS/gear ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ 50 Hz, O2 ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ 50 Hz, fuel/oil pressure ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ 20 Hz, CLT/IAT/battery ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ 10 Hz, baro ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ 4 Hz | Low ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â macro changes in `sensors.h` | **S** | **DONE** |
| **4** | VVT ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ 100 Hz, programmable I/O ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ 100 Hz, aux ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ 40 Hz, idle ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ 50 Hz | Low | **S** | **DONE** |
| **5** | Engine protection ÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢ 200 Hz and outside the RPM gate | Medium ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â behavioural change, needs test | **M** | **DONE** (needs hardware validation, see below) |
| **6** | Add a 100 Hz SD logging option | Medium ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â SD write budget | **M** | **DONE** ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â replaced the 30 Hz slot |
| **7** | Only after measuring `loopsPerSecond`: consider 1 kHz for TC | Medium | **M** | not started |

> **Step 3 filter note (resolved).** `readTPS()`/`readCLT()`/`readIAT()`/`readO2()`/`readBat()`/
> `readBaro()` each low-pass every *sample* through `LOW_PASS_FILTER(raw, configPage4.ADCFILTER_x,
> prior)`. The coefficient is per-sample, not per-second, so raising a sensor's rate with the alpha
> unchanged makes that filter respond faster in wall-clock terms. That is **intended here**: the whole
> point of raising TPS from 30 Hz to 200 Hz is to cut tip-in lag, and preserving the old time constant
> would give back most of the benefit. The alphas are deliberately left as-authored; a tune that finds
> the result noisy lowers the alpha. Because this fork does not carry tunes across, there is no
> compatibility burden to protect. Fuel- and oil-pressure filtering is unchanged (`ADCFILTER_PSI_DEFAULT`).

> **Step 3 follow-up: tpsDOT no longer assumes a sample rate.** The first cut of this change left
> `tpsDOT` differentiated on a 30 Hz window, because `computeTPSDOT()` hardcoded the frequency as
> `(TPS_READ_FREQUENCY * tpsChange) / 2`. That was a workaround, not a fix, and it left acceleration
> enrichment running at 30 Hz when the whole point was to make it responsive.
>
> The actual defect was that `computeTPSDOT()` **assumed** the sample interval while
> `computeMapDot()` already **measured** it (`MICROS_PER_SEC / mapDeltaTime`). TPS was the odd one
> out. Now `readTPS()` records a timestamp, `getTPSDeltaTime()` exposes the measured interval, and
> `computeTPSDOT()` divides by it, so tpsDOT is a true %/s at any rate and `taeThresh` /
> `taeMinChange` / the +/-2550 clamp all keep their meaning.
>
> Consequences: the `TPS_DOT_FREQUENCY` / 30 Hz constant is **gone**. `TPS_DOT_TIMER_BIT` simply
> follows the TPS sample rate (200 Hz), so AE is evaluated every 5 ms instead of every 33 ms - the AE
> cut latency after a throttle stab drops by ~28 ms, and the AE taper was already micros()-based
> inside `correctionAccel()` so it was being under-sampled by the slower gate. `TPS_DOT_MIN_PERIOD_US`
> (1 ms) floors the divisor so a back-to-back read cannot divide by ~zero.

> **Step 6 caveat.** `writeSDLogEntry()` appends to a ring buffer and only `sync()`s once a second, so
> 100 Hz does not mean 100 blocking flushes. Even so, a block flush can stall the main loop for
> milliseconds, which perturbs ignition/injection *schedule recalculation* (not the already-scheduled
> events). Check `loopsPerSecond` with logging at 100 Hz before relying on it during a pass.

> **Step 5 needs hardware validation before it is trusted.** Moving `checkEngineProtection()` outside
> the sync+RPM block is what makes over-rev/stuck-throttle protection work on a 2-step or during stall
> recovery, but it also means the function now runs when the engine is *not* running. The oil-pressure
> protection timer would otherwise start counting down against a legitimately-zero oil pressure and be
> already expired at first crank, so `checkOilPressureLimit()` now requires
> `RPM > 0 && syncStatus != None` before it will start or advance that timer. Verify on the car:
> a normal start must not trigger an oil cut, and a genuine low-oil condition while running still must.

> **Note on `TPS_READ_FREQUENCY`:** the guard is now `#if ==15 / ==30 / ==200 / #else #error`. It must
> use numeric literals ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the `BIT_TIMER_*` symbols are `constexpr` and invisible to the preprocessor.
> If a bit is renumbered, update that mapping by hand; the `#error` exists so a forgotten update fails
> the build rather than silently mis-scheduling the sensor.

> **These are all gate/macro changes, not architecture.** None of them touch the storage layout,
> the `.msq` format, or `speeduino.ini`. They can ship independently of Phase 0.

---

## 7. Recommended phased roadmap

### Phase 0 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â Foundations (before any user-visible feature)
1. **FND-01** custom block infrastructure (magic, version, per-block CRC, range checks).
2. **FND-02** new TS config pages 16+.
3. **FND-08** fix boost-by-gear on internal VSS *(do it now ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â it is small and it is a bug)*.
4. **FND-09** torque-reduction arbiter.
5. **FND-05** race-time framework.
6. **FND-06** multi-speed framework *(pin selection is already user-configurable ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â firmware-only, see TC-05)*.
7. **FND-07** 200 Hz driver-aid slot.
8. **FND-10** SD storage class.
9. **FND-03** generic table engine *(the plumbing)*.
10. **GEN-01 ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ GEN-06** user-defined 1D/2D tables *(the public face of the same engine)*.
11. **FND-04** generic PWM engine.

> Rationale for ordering: 3 is cheap and is a live bug. 1 and 2 are the substrate. 4ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“8 are the multipliers. Build FND-03 and GEN-* together ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â they are one engine, two faces; do not implement twice. Do not start Phase 1 until FND-03/GEN-*, FND-05 and FND-06 exist ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â otherwise you rebuild each feature three times.

### Phase 1 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â "Sellable drag ECU" (the launch gate)
**LC-01 ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ LC-09**, **TC-01**, **TC-02**, **TC-03**, **TC-04**, **TC-05**, **BST-01 ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ BST-06**, **LOG-01**, **LOG-02**, plus **CAN-05** (CAN wheel speeds as an alternative input path needing no extra wiring).

Exit criteria: a car can stage with a trans-brake and bump box, launch on a 3-step with a timed boost/retard ramp, take boost by gear *and* by time, break traction and be caught by a tunable percentage-cut TC driven from **front/rear wheel speed sensors on pins 35/34**, and have the whole pass logged at ÃƒÂ¢Ã¢â‚¬Â°Ã‚Â¥100 Hz with the drag channels visible in TunerStudio. CAN wheel speeds (CAN-05) remain available as a fallback/backup channel.

### Phase 2 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â Parity
**AL-01/02**, **N2O-01 ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ 05**, **TM-01/02**, **TRQ-01 ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ 04**, **BSH-01 ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ 04**, **PRT-01 ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ 03**, **GEAR-01/02**, **SS-01**, **EGT-01/02** (via CAN), **KNO-01/02**, **FUEL-01 ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ 05**, **CAN-01 ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ 03**, **DIAG-01**, **TBL-02**.

Exit criteria: a feature-by-feature comparison sheet against MS3Pro EVO and Holley Dominator has no drag-relevant gaps left except DBW, TCU and 32ÃƒÆ’Ã¢â‚¬â€32 tables.

### Phase 3 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â Differentiators & resolution
**TBL-01**, **MAP-01/MAP-02** (storage decision required), **PRT-04/05**, **CAN-04**, **FUEL-06**, **DF-01 ÃƒÂ¢Ã¢â€šÂ¬Ã‚Â¦ 10**.

### Phase 4 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â Board rev
**WBS-01**, **EGTH-01**, **KNOH-01**, **DBW-01** (CAN-DBW client first, native later), **TCU-01** (CAN protocol first).

---

## 8. Top 15 by leverage, if only fifteen things get done

| Rank | ID | Why it ranks here |
|---|---|---|
| 1 | **FND-03 + GEN-*** Generic table engine **and** user-defined 8ÃƒÆ’Ã¢â‚¬â€1D / 8ÃƒÆ’Ã¢â‚¬â€2D tables | Converts ~60 % of the remaining backlog from firmware into configuration, **and** lets users build their own features without touching firmware |
| 2 | **FND-05** Race-time framework | Every drag feature is time-domain; nothing time-based exists |
| 3 | **FND-06** Multi-speed framework | Prerequisite for all traction control (front pin 35 / rear pin 34) |
| 4 | **TC-01** Percentage-cut traction control | The single most-requested drag feature; its absence alone disqualifies the product |
| 5 | **TC-02** Perfect-pass / driveshaft-RPM TC | What actually wins rounds; the differentiator between "has TC" and "has good TC" |
| 6 | **LC-01** 3-step limiters | Table stakes; a 2-step is not enough for burnoutÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢stageÃƒÂ¢Ã¢â‚¬Â Ã¢â‚¬â„¢run |
| 7 | **BST-01** Boost by time from launch | The defining drag boost feature; MS3 and Holley both lead with it |
| 8 | **FND-04** Generic PWM engine | Unlocks bump box, creep, N2O, WMI, pump, dome ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â eight features from one engine |
| 9 | **LC-06/07** Trans-brake + bump box | Physical staging. Without it the ECU cannot hold the car on the line |
| 10 | **LOG-01** ÃƒÂ¢Ã¢â‚¬Â°Ã‚Â¥100 Hz logging | You cannot tune TC or a launch without it; currently 30 Hz max (SD hardware already works ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â the limit is rate) |
| 11 | **BST-06** Real per-gear boost | Fixes the current 6-entry-percent toy and the D1 bug |
| 12 | **AL-02** Rolling anti-lag | Roll racing is a large and growing drag-adjacent market |
| 13 | **N2O-01/02** Multi-stage timed progressive nitrous | 2 stages with no timers is not competitive with 6ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“8 stage rivals |
| 14 | **TM-02** Cylinder-cut actuator | The granularity between "retard" and "full cut"; needed by TC, launch and torque mgmt |
| 15 | **PRT-03** Graduated protection levels | A binary cut on a 2,000 hp engine is a safety liability, not a feature |

---

## 9. Risks and open questions for the user

1. **MAP-01 ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â several complete tunes, stored on SD.** Needs no EEPROM at all: reserve a section of the SD card as a tune library, copy one into the active EEPROM region on selection. ~16 KB of EEPROM avoided entirely. Requires an SD-backed storage abstraction (FND-10) and a safe "switch tune while engine is stopped" interlock. Nothing else depends on it.
2. **Traction control inputs ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â resolved.** Plan is **pin 34 = rear, pin 35 = front**, one sensor each, polled by our own ISR. This beats 4-corner CAN ABS because CAN ABS speed typically only arrives at 30ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Å“50 Hz. Note pin 34 is *also* the documented default for a secondary cam input (dual VVT) ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â mutually exclusive with VSS, not a bug.
3. **FND-07 rate increase ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â downgraded to low risk, but measure first.** My earlier note called the shared SPI bus a blocker; that was too strong (Ãƒâ€šÃ‚Â§6A). Config is RAM-resident, so a faster control loop never touches flash. The real constraint is **injector/coil ISR jitter** if you go to 1 kHz ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â `TMR1-4_isr` reaches `SPI.transfer16()` in interrupt context. 200 Hz is very likely safe; 1 kHz needs `loopsPerSecond` measured on hardware plus a GPIO timing pin. Also audit 16-bit status reads shared with ISRs for tearing.
4. **FND-09 arbiter scope.** Retard is currently additive across `corrections[]`. Several new producers writing retard independently will produce stacking bugs. Decide on the arbitration rule (priority / sum-with-clamp / lowest-wins) **before** TC, anti-lag and torque management land.
5. **Every P0/P1 item needs a matching `speeduino.ini` section at the same offsets** (AGENTS.md Ãƒâ€šÃ‚Â§2A.7). Firmware that writes a block TunerStudio cannot read is half a feature ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â include the ini work in every estimate above.
6. **`STORAGE_END` must stay `0xFFF`.** All calibration tables are positioned top-down from it (`storage.cpp:59-60`). Do not "gain space" by raising it ÃƒÂ¢Ã¢â€šÂ¬Ã¢â‚¬Â that relocates every calibration table and silently invalidates existing tunes.
7. **AGENTS.md line 19 is stale.** It says *"No SD, RTC or knock module has been validated."* For SD and RTC this understates the board: both are compiled in by default, and RTC needs only a coin cell. Recommend correcting that line so it doesn't steer future work (as it steered mine).
