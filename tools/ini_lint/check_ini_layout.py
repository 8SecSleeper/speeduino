#!/usr/bin/env python3
"""
check_ini_layout.py - structural / reference-integrity linter for Speeduino's
TunerStudio definition file (reference/speeduino.ini).

The upstream `validate-ini` CI action checks that the INI *parses*. It cannot
know whether our own `[Menu]` entries point at objects we actually defined,
because those objects live in this same file. That is the gap this tool fills.

Checks
------
  MENU-NESTED-GROUP    groupMenu nested inside another groupMenu (spec forbids it)
  MENU-ORPHAN-CHILD    groupChildMenu with no open groupMenu
  MENU-DANGLING        menu target is not a defined object and not a std_* builtin
  MENU-MISSING-COMMA   subMenu target and label ran together (no comma), so
                       the whole thing is parsed as one bogus target name
  MENU-DUPLICATE       same target listed twice in the same menu
  MENU-SEP-EDGE        leading / trailing / doubled std_separator
  MENU-EMPTY           menu with no items
  DLG-DANGLING-PANEL   panel = target is not a defined object
  DLG-DANGLING-GAUGE   gauge = name is not defined in [GaugeConfigurations]
  DLG-BAD-LAYOUT       dialog layout is not one of yAxis/xAxis/border/indexCard
  DLG-STRAY-NUMBER     dialog "layout" slot holds a number (should be a
                       subMenu argument, not a dialog one)
  DLG-NO-TITLE         dialog = with no quoted title argument
  DLG-TRAILING-COMMA   dialog/panel line ends with a comma
  UNREACHABLE          object defined but not reachable from any [Menu] entry
  DUP-DEFINITION       same object name defined more than once

Exit code is 1 if any ERROR is emitted, else 0. Warnings never fail the build.

Usage
-----
    python check_ini_layout.py [path/to/speeduino.ini] [--strict] [-v]
    python check_ini_layout.py --constants-hash      # print [Constants] digest

Notes on preprocessing
-----------------------
#if / #else / #endif are understood only well enough to avoid false "duplicate
definition" reports when the same name is defined in both branches. All
branches are treated as live for reference-resolution purposes, which is the
conservative choice: a target that only exists in one branch still counts as
defined.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from collections import defaultdict

# ---------------------------------------------------------------------------

VALID_LAYOUTS = {"yaxis", "xaxis", "border", "indexcard"}
SEPARATOR = "std_separator"

# sha256 of the [Constants] section, first 16 hex chars, with:
#   - comments and blank lines removed (they carry no offset information)
#   - `<word> = lastOffset` normalised, because lastOffset always resolves to
#     the running byte position, so declaring a locator with it moves nothing.
# Anything else changing this digest means a real offset may have moved, which
# silently invalidates existing .msq files. Update this constant only after
# deliberately confirming that.
EXPECTED_CONSTANTS_DIGEST = "9b3542b62256cc13"

# Why this changed (from 5aa79bb2d9f87b2e):
#   Restructured the launch pages and added page 20. Still an APPEND plus a rework of pages that
#   are new in this branch and have no saved tunes against them:
#
#     - page 19 was REDUCED from a full second copy of the speed page to the simple mode it was
#       meant to be: an RPM limit versus time, a soft retard with a hysteresis band, and a cut
#       method. All of page 19's fields changed, but page 19 is new in this same branch and
#       EEPROM_PPLAUNCH_VERSION was bumped, so no stored block is affected.
#     - page 18 lost its boost TARGET curve (that moved to page 20) and kept only a duty TRIM,
#       freeing 32 bytes that went to reserved. Offsets after curveBoostTrim moved, but again
#       this is a page new in this branch.
#     - page 20 is NEW (launch boost target, in the Boost menu) with a 1024-byte payload: 192
#       bytes used, 832 reserved for the full boost control feature set.
#     - nPages 19 -> 20, pageSize +1024, all seven per-page protocol lists extended to 20.
#
#   No offset on any PRE-EXISTING page (1..17) moved, so every saved .msq keeps its meaning.
#
# Why this changed (from ba58abbf60bc5d46):
#   Added pages 18 and 19 (perfect-pass launch control, speed based and RPM based). This is a
#   pure APPEND: nPages went 17 -> 19, two 512-byte entries were added to pageSize, and two new
#   `page = 18` / `page = 19` constant blocks were added. No existing offset moved, so every
#   saved .msq keeps its meaning and every existing page keeps its layout.
#
#   Deliberately TWO pages rather than one page plus a mode flag. A tuner who has dialled in a
#   speed-based curve must be able to run RPM mode for a pass and come back without re-entering
#   32 rows of data; 1024 bytes of a 17 kB free budget is a good trade for that.
#
#   Also updated in the same change, and each one is a place a previous commit forgot:
#     - all seven per-page protocol lists (pageIdentifier x12/x13 added, and the five command
#       templates extended) - omitting these makes TunerStudio silently fail to burn page 18/19,
#       which is exactly the page-17 data-loss bug this project already shipped once;
#     - [OutputChannels] gained channels 145..155 and ochBlockSize went 146 -> 157, matching
#       LOG_ENTRY_SIZE 145 -> 156 in logger.h and the fsIntIndex entries for the four new U16/S16
#       channels (145, 147, 149, 151 - the following byte of each must NOT be listed);
#     - defaultValue entries for both new pages.
#
# Why this changed (from 24fc7b7d213a1f5f):
#   rtCurveRetard widened from U08 to U16 and its scale changed from whole degrees to tenths of
#   a degree (0.1, max 60.0), so a tuner can shape the retard ramp at finer than 1-degree
#   resolution. It grew from 16 to 32 bytes, so the reserved tail moved from offset 84 to 100
#   (rtUnused84_255 -> rtUnused100_255, 172 -> 156 bytes). Total payload is still 256 bytes.
#   Page 17 is new in this branch and no tunes exist for it, so no stored value is affected;
#   EEPROM_RACETIME_VERSION went 2 -> 3 so any block written by an earlier build is discarded.
#
# Why this changed (from 29cdd08ac19aec9e):
#   Page 17's six spare-bit masks were relabelled from all-"INVALID" to all-"Unused". A 2-bit
#   field with four INVALID options has no valid option for TunerStudio to map a saved value
#   onto, which produces "Failed to set value ... No valid options found". Same bits, same
#   offsets; only the labels changed, so nothing stored is affected.
#
# Why this changed (from 7338d7cc9a4c09c6):
#   The per-page protocol lists (pageIdentifier, pageReadCommand, burnCommand,
#   pageValueWrite, pageChunkWrite, crc32CheckCommand) gained a 17th entry. They had been left
#   at 16 when page 17 was added, so TunerStudio had no burn command for page 17 and every edit
#   to the race-time page silently failed to save. No field OFFSET moved: these are controller
#   protocol tables, not tune constants, so no .msq is affected.
#
# Why this changed (from 179894c5f6cba54c):
#   rtStopSpeed and rtFalseStartSpeed changed UNITS from km/h to mph. Both are new page-17
#   fields with no tunes in existence, so this reinterprets no stored value - but it is still a
#   semantic change to a declared constant, which is exactly what this digest is for. Same
#   offsets (10 and 12), same U16 width, same scales; only the units string differs. The
#   firmware now converts mph -> km/h at the comparison, since currentStatus.vss is km/h.
#
# Why this changed (from c2f00b138a6e456b):
#   rtArmPin changed from `scalar, U08, 1, "", ...` to the same 6-bit dropdown form used by
#   vssPin / knock_pin / n2o_arming_pin: `bits, U08, 1, [0:5], $IO_Pins_no_def` plus a
#   rtArmPinUnused_6_7 mask. This is a TYPE change (scalar -> bits) at the SAME offset 1, in
#   the SAME byte, so no offset moved and no .msq is invalidated: a stored U08 pin number is
#   read identically by a bits [0:5] field, and the new [6:7] mask simply names the padding
#   bits that were already there. The dropdown now offers real board pins (3-53 and A8-A15)
#   instead of a raw 0-255 number.
#   Also added the page-17 `defaultValue =` entries (drag-racing defaults: arm pin 35,
#   active-low, VSS false-start detection, and a descending 12->0 degree retard curve). Those
#   live in [ConstantsExtensions], which is not part of this digest.
#
# Why this changed (from 10308711b0895cd6):
#   rtCurveLabel's declaration was corrected from `string, ASCII, 16, [20], "Axis label"` to
#   `string, ASCII, 16, 20`. The [shape] form is array/bits syntax and made TunerStudio refuse
#   the whole page ("Error loading [Constant] ... Problem at: rtCurveLabel"). This is a SYNTAX
#   fix only: the field is still 20 bytes at page 17 offset 16, so no offset moved and no .msq
#   is invalidated. A CONST-STRING-BAD-LENGTH check was added to this linter so the same
#   mistake cannot reach TunerStudio again - the layout checks alone cannot see field syntax.
#
# Why this changed (from 804fb7f8e41c3bdf, then f287ed6969af9f2e):
#   Added page 17 - the FND-05 race-time block (see speeduino/raceTime.h). This APPENDS a new
#   page after page 16; no existing constant moved and no existing offset changed, so no saved
#   .msq loses meaning. New page 17 constants: rtEnable, rtArmPin, rtArmPolarity, rtArmPullup,
#   rtStartSource, rtStopSource, rtMotionSource, rtCurveEnable, rtStopTimeMs, rtStopSpeed,
#   rtFalseStartSpeed, rtCurveTimeSpan, rtCurveLabel, rtCurveRetard, plus the padding arrays
#   rtUnused*. Page 17's pageSize (256) and the offsetof() assertions in raceTime.h are the
#   contract between this section and the C++ struct - they fail the build if they drift.
# Verified offset-neutral: page 16 ends at lttRichAuth2 (offset 1808 + 16x16), and page 17
#   begins at its own offset 0. Every pre-existing field in [Constants] keeps its offset.
# The [OutputChannels] section also grew (ochBlockSize 139 -> 143, raceTime channels appended
#   at indices 138-141, systemTempRaw moved 138 -> 144). That section is not part of this
#   digest, and appending to it cannot invalidate a .msq because it carries no tune offsets.

# Why this changed (from b9c364fd6c4aede3):
#   egoLimit's INI maximum went 16 -> 100. This is a RANGE change only - the field stays a
#   U08 at offset 7 on page 6, so no offset moved and no saved .msq is invalidated. It is
#   required because egoLimit is now the short-term (STFT) bound and the per-cell authority
#   can legitimately be wider than 16%; leaving the max at 16 would silently cap every
#   per-cell authority at 16% in the tuning software.
# Why this changed (from 2b8fcd041fae7f32):
#   Added page 16 - the long-term fuel trim block (see speeduino/ltt.h). This APPENDS a new
#   page after page 15; no existing constant moved and no existing offset changed, so no
#   saved .msq loses meaning. New page 16 constants: lttMode, lttLearnRateDiv,
#   lttLearnDeadband, lttLearnMinRPM, lttSaveRPM, lttLearnMinSamples, lttLearnMaxStep,
#   lttSlewRate, lttTrim1, lttTrim2, lttLeanAuth1, lttLeanAuth2, lttRichAuth1, lttRichAuth2.
#   Page 16's pageSize (2064) and the firmware's offsetof() assertions in ltt.h are the
#   contract between this section and the C++ struct - they fail the build if they drift.
# Why this changed (from cfb0d92d5e1553bf):
#   onboard_log_file_rate's 4th enum label went "30Hz" -> "100Hz" (the SD logger's
#   highest rate now replaces the old 30 Hz slot rather than adding a 5th option, so the
#   2-bit field is unchanged).
# Verified offset-neutral: the field is still `bits, U08, 116, [4:5]` in both revisions,
# no other field moved, and ochBlockSize is still 139. Only the display text differs, so no
# .msq offset is affected - the digest cannot tell a label edit from an offset move, which
# is why it is pinned and why this note is required.

# Changed again 80afb5c95af0706f -> 3e882d49d285e620 (the TunerStudio #1224 fix):
#   Added iacCLminSteps/iacCLmaxSteps at page 15 offsets 106/107, and shrank the reserved
#   tail Unused15_106_255[150] -> Unused15_108_255[148] to make room. Also inlined the
#   idleRes/idleResMax scale into iacCLminValue/iacCLmaxValue (now a constant 1.0, PWM
#   only) and deleted the two [OutputChannels] helper definitions.
# Verified by diffing every [Constants] field's offset+width between the two revisions:
#   ADDED iacCLminSteps, iacCLmaxSteps, Unused15_108_255
#   REMOVED Unused15_106_255
#   MOVED / RESIZED: none
# The bytes used were already reserved-and-unused, so no .msq offset is affected.

# Changed again 3e882d49d285e620 -> 2b8fcd041fae7f32 (issue #1506, wideband AFR reference):
#   RENAMED unused10_184 -> wbo2AfrRef, same offset 184, same U08/count-1 width. Nothing after
#   it moves, and every field keeps the byte position it had.
# Verified by parsing every [Constants] field's name+definition out of both revisions and
# diffing: exactly one field differs (the rename), and ZERO common fields changed value.
# i.e. no offset moved, no width changed - the reserved byte was simply given a meaning, so
# an existing .msq still maps every field to the same location.
#
# Note this is a *silent* hazard for stored tunes: an old .msq carries whatever junk happened
# to be in that reserved byte. The firmware migration (upgradeV27toV28 in updates.cpp) forces
# the default, so a stale byte can never be interpreted as a real reference.

# Sections that can define referenceable objects.
SECTION_USERDEF = "userdefined"
SECTION_CURVE = "curveeditor"
SECTION_TABLE = "tableeditor"
SECTION_GAUGE = "gaugeconfigurations"
SECTION_MENU = "menu"
SECTION_CONSTANTS = "constants"

# Menus that TunerStudio owns; naming them is how we MERGE into the built-in
# menus rather than creating a new one. Renaming these silently breaks the
# merge, so they are called out explicitly.
TS_RESERVED_MENUS = {"file", "communications", "tools", "help", "datalogging"}

RE_SECTION = re.compile(r"^\s*\[([A-Za-z0-9_]+)\]\s*$")
RE_IF = re.compile(r"^\s*#(if|ifdef|ifndef)\b(.*)$", re.I)
RE_ELSE = re.compile(r"^\s*#(else|elseif)\b", re.I)
RE_ENDIF = re.compile(r"^\s*#endif\b", re.I)

RE_MENUDIALOG = re.compile(r"^\s*menuDialog\s*=\s*(\S+)", re.I)
RE_MENU = re.compile(r"^\s*menu\s*=\s*(.+)$", re.I)
RE_GROUPMENU = re.compile(r"^\s*groupMenu\s*=\s*(.+)$", re.I)
RE_GROUPCHILD = re.compile(r"^\s*groupChildMenu\s*=\s*(.+)$", re.I)
RE_SUBMENU = re.compile(r"^\s*subMenu\s*=\s*(.+)$", re.I)

RE_DIALOG = re.compile(r"^\s*dialog\s*=\s*([^,\s]+)\s*,?\s*(.*)$", re.I)
RE_CURVE = re.compile(r"^\s*curve\s*=\s*([^,\s]+)\s*,\s*(\"[^\"]*\"|\S*)", re.I)
RE_TABLE = re.compile(r"^\s*table\s*=\s*([^,\s]+)\s*,\s*([^,\s]+)", re.I)
RE_PANEL = re.compile(r"^\s*panel\s*=\s*([A-Za-z0-9_.]+)\s*(.*)$", re.I)
RE_GAUGEREF = re.compile(r"^\s*gauge\s*=\s*([A-Za-z0-9_.]+)", re.I)
# `panel = <name>` may legitimately point at an indicatorPanel or a [help] block
# as well as a dialog, so those are collected as definitions too.
RE_INDICATORPANEL = re.compile(r"^\s*indicatorPanel\s*=\s*([A-Za-z0-9_.]+)", re.I)
RE_HELP = re.compile(r"^\s*help\s*=\s*([A-Za-z0-9_.]+)", re.I)

# A "field" in [Constants] - used only for the offset-integrity digest.
RE_CONST_FIELD = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=")

# A full [Constants] declaration: name = class, type, offset, ...
RE_CONST_DECL = re.compile(
    r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(bits|scalar|array|string)\s*,\s*"
    r"([A-Za-z0-9]+)\s*,\s*(.*)$", re.I)

VALID_CONST_TYPES = {"U08", "S08", "U16", "S16", "U32", "S32", "F32", "ASCII"}


class Report:
    def __init__(self) -> None:
        self.items: list[tuple[str, str, int, str]] = []

    def error(self, code: str, msg: str, line: int = 0) -> None:
        self.items.append(("ERROR", code, line, msg))

    def warn(self, code: str, msg: str, line: int = 0) -> None:
        self.items.append(("WARN", code, line, msg))

    def info(self, code: str, msg: str, line: int = 0) -> None:
        self.items.append(("INFO", code, line, msg))

    def count(self, level: str) -> int:
        return sum(1 for lvl, _, _, _ in self.items if lvl == level)

    def dump(self, show_info: bool) -> None:
        order = {"ERROR": 0, "WARN": 1, "INFO": 2}
        rows = [r for r in self.items if show_info or r[0] != "INFO"]
        rows.sort(key=lambda r: (order[r[0]], r[2]))
        if not rows:
            print("  (no findings)")
            return
        for level, code, line, msg in rows:
            loc = f"{line:>6}" if line else "      "
            print(f"  {level:<5} {loc}  {code:<20} {msg}")


def strip_comment(line: str) -> str:
    """Remove a trailing ';' comment, but not one inside a quoted string."""
    out = []
    in_q = False
    for ch in line:
        if ch == '"':
            in_q = not in_q
        if ch == ";" and not in_q:
            break
        out.append(ch)
    return "".join(out)


def split_args(text: str) -> list[str]:
    """Split on commas at nesting depth 0.

    A comma inside a quoted string is NOT a separator, and neither is one
    inside {} or (). This matters: several menu labels contain commas, e.g.
    `"Boost Duty / Target", { expr }`.
    """
    parts, buf = [], []
    in_q = False
    depth = 0
    for ch in text:
        if ch == '"':
            in_q = not in_q
        elif not in_q:
            if ch in "{(":
                depth += 1
            elif ch in "})":
                depth -= 1
            elif ch == "," and depth == 0:
                parts.append("".join(buf))
                buf = []
                continue
        buf.append(ch)
    parts.append("".join(buf))
    return [p.strip() for p in parts]


def _strip_braces(text: str) -> str:
    """Remove balanced {...} spans so expression contents can't confuse us."""
    out, depth = [], 0
    for ch in text:
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth = max(0, depth - 1)
        elif depth == 0:
            out.append(ch)
    return "".join(out)


def missing_comma(text: str) -> bool:
    """True if a target and its quoted label ran together with no comma.

    Catches `subMenu = dwell_tblMap "Dwell Map", { expr }` - a real defect in
    this INI that TunerStudio silently mis-handles, because the target becomes
    the literal string `dwell_tblMap "Dwell Map"`.

    Expression bodies are stripped first: a label followed by
    `{ arrayValue( a, b ) > 0 }` must not be reported just because a quote
    appears after a '('. Quoted groups may legitimately contain commas, so we
    consume the whole "..." span on each opening quote.
    """
    text = _strip_braces(text)
    idx = 0
    n = len(text)
    while idx < n:
        ch = text[idx]
        if ch != '"':
            idx += 1
            continue
        before = text[:idx].rstrip()
        if before and not before.endswith(","):
            return True
        close = text.find('"', idx + 1)
        if close < 0:
            return False
        idx = close + 1
    return False


def unquote(s: str) -> str:
    s = s.strip()
    if len(s) >= 2 and s[0] == '"' and s[-1] == '"':
        return s[1:-1]
    return s


def menu_label(s: str) -> str:
    """Top-level menu name normalised for comparison (strip '&' accelerators)."""
    return unquote(s).replace("&", "").strip().lower()


class Ini:
    def __init__(self, path: str) -> None:
        self.path = path
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            self.raw = fh.read()
        self.lines = self.raw.split("\n")
        self.section_of: list[str] = []
        self.cc_depth: list[int] = []      # preprocessor nesting depth per line
        self._annotate()

    def _annotate(self) -> None:
        """Record the active [Section] and preprocessor depth for every line."""
        section = ""
        depth = 0
        for line in self.lines:
            if RE_SECTION.match(line):
                section = RE_SECTION.match(line).group(1).lower()  # type: ignore[union-attr]
                self.section_of.append(section)
                self.cc_depth.append(depth)
                continue
            if RE_IF.match(line):
                depth += 1
            elif RE_ENDIF.match(line):
                depth = max(0, depth - 1)
            self.section_of.append(section)
            self.cc_depth.append(depth)

    def lineno(self, idx: int) -> int:
        return idx + 1


def check(path: str, strict: bool, verbose: bool) -> int:
    ini = Ini(path)
    rep = Report()

    # ---- collect definitions ------------------------------------------------
    # name -> list of (line_idx, kind, section)
    defs: dict[str, list[tuple[int, str, str]]] = defaultdict(list)

    for i, raw in enumerate(ini.lines):
        sec = ini.section_of[i]
        line = strip_comment(raw)
        if not line.strip():
            continue

        if sec == SECTION_USERDEF:
            for rx, kind in ((RE_DIALOG, "dialog"),
                             (RE_INDICATORPANEL, "indicatorPanel"),
                             (RE_HELP, "help")):
                m = rx.match(line)
                if m:
                    defs[m.group(1)].append((i, kind, sec))
                    break

        elif sec == SECTION_CURVE:
            m = RE_CURVE.match(line)
            if m:
                defs[m.group(1)].append((i, "curve", sec))

        elif sec == SECTION_TABLE:
            m = RE_TABLE.match(line)
            if m:
                defs[m.group(1)].append((i, "table", sec))
                defs[m.group(2)].append((i, "map3d", sec))

        elif sec == SECTION_GAUGE:
            # gauge definitions look like:
            #   oilPressureGauge = oilPressure, "Oil Pressure", "PSI", 0, 100, ...
            # Non-definitions in this section are `gaugeCategory = "Main"`.
            m = re.match(r"^\s*([A-Za-z0-9_.]+)\s*=\s*[^,\s]+\s*,", line)
            if m and m.group(1).lower() != "gaugecategory":
                defs[m.group(1)].append((i, "gauge", sec))

    known = set(defs)

    def resolves(name: str) -> bool:
        if name.lower().startswith("std_"):
            return True
        return name in known

    # ---- menu walk ---------------------------------------------------------
    menu_entry_count: dict[str, int] = defaultdict(int)
    # (menu_label, target) -> first line
    seen_in_menu: dict[tuple[str, str], int] = {}
    menu_targets: list[str] = []          # objects to root reachability from
    current_menu: str | None = None
    group_open = False
    prev_was_separator = False
    menu_items: dict[str, list[str]] = defaultdict(list)

    for i, raw in enumerate(ini.lines):
        if ini.section_of[i] != SECTION_MENU:
            continue
        line = strip_comment(raw)
        s = line.strip()
        if not s:
            continue

        m = RE_MENU.match(line)
        if m and not RE_MENUDIALOG.match(line) and not RE_GROUPMENU.match(line):
            label = menu_label(m.group(1))
            current_menu = label
            group_open = False
            prev_was_separator = False
            if label in TS_RESERVED_MENUS:
                pass  # merged into TS's own menu - expected, not a problem
            continue

        m = RE_MENUDIALOG.match(line)
        if m:
            current_menu = None
            group_open = False
            continue

        m = RE_GROUPMENU.match(line)
        if m:
            if group_open:
                rep.error("MENU-NESTED-GROUP",
                          "groupMenu nested inside another groupMenu "
                          "(spec allows only menu > groupMenu > groupChildMenu)",
                          ini.lineno(i))
            group_open = True
            prev_was_separator = False
            continue

        m = RE_GROUPCHILD.match(line)
        if m:
            args = split_args(m.group(1))
            target = args[0] if args else ""
            if not group_open:
                rep.warn("MENU-ORPHAN-CHILD",
                         f"groupChildMenu '{target}' has no open groupMenu; "
                         "it will attach to the parent menu", ini.lineno(i))
            _check_target(rep, ini, i, target, current_menu, seen_in_menu,
                          menu_items, menu_targets)
            prev_was_separator = False
            continue

        m = RE_SUBMENU.match(line)
        if m:
            args = split_args(m.group(1))
            target = args[0] if args else ""
            if target == SEPARATOR:
                if current_menu is None:
                    rep.warn("MENU-SEP-EDGE", "separator outside any menu",
                             ini.lineno(i))
                elif prev_was_separator:
                    rep.warn("MENU-SEP-EDGE", "doubled std_separator",
                             ini.lineno(i))
                prev_was_separator = True
                # A subMenu of ANY kind - separators included - ends the
                # current group, so a later groupMenu becomes a SIBLING of it
                # rather than nesting inside it.
                group_open = False
                if current_menu:
                    menu_items[current_menu].append(SEPARATOR)
                continue
            group_open = False
            _check_target(rep, ini, i, target, current_menu, seen_in_menu,
                          menu_items, menu_targets)
            prev_was_separator = False

    # separators at the edge of a menu
    for label, items in menu_items.items():
        real = [x for x in items if x != SEPARATOR]
        if not real:
            rep.error("MENU-EMPTY", f"menu '{label}' has no items")
        else:
            if items and items[0] == SEPARATOR:
                rep.warn("MENU-SEP-EDGE",
                         f"menu '{label}' starts with a separator")
            if items and items[-1] == SEPARATOR:
                rep.warn("MENU-SEP-EDGE",
                         f"menu '{label}' ends with a separator")

    # ---- dialog bodies -----------------------------------------------------
    for i, raw in enumerate(ini.lines):
        sec = ini.section_of[i]
        if sec != SECTION_USERDEF:
            continue
        line = strip_comment(raw)
        if not line.strip():
            continue

        m = RE_DIALOG.match(line)
        if m:
            name, rest = m.group(1), m.group(2).strip()
            _check_dialog_header(rep, ini, i, name, rest)

        m = RE_PANEL.match(line)
        if m:
            target = m.group(1)
            if not resolves(target):
                rep.error("DLG-DANGLING-PANEL",
                          f"panel = {target} is not a defined dialog / curve / "
                          "table / gauge", ini.lineno(i))
            if line.rstrip().endswith(","):
                rep.warn("DLG-TRAILING-COMMA",
                         f"panel = {target} ends with a comma", ini.lineno(i))

        m = RE_GAUGEREF.match(line)
        if m:
            g = m.group(1)
            if not resolves(g):
                rep.error("DLG-DANGLING-GAUGE",
                          f"gauge = {g} is not defined in "
                          "[GaugeConfigurations] and not a std_* builtin",
                          ini.lineno(i))

    # ---- duplicate definitions (ignoring #if/#else branches) ---------------
    for name, entries in defs.items():
        if len(entries) < 2:
            continue
        # group by preprocessor depth; same depth twice = real duplicate
        by_depth: dict[int, list[int]] = defaultdict(list)
        for idx, _kind, _sec in entries:
            by_depth[ini.cc_depth[idx]].append(idx)
        for depth, idxs in by_depth.items():
            if len(idxs) > 1:
                where = ", ".join(str(x + 1) for x in idxs)
                if depth == 0:
                    rep.error("DUP-DEFINITION",
                              f"'{name}' defined {len(idxs)}x at top level: "
                              f"lines {where}", idxs[0])
                else:
                    # Sibling #if/#else branches legitimately redefine a name.
                    rep.info("DUP-CONDITIONAL",
                             f"'{name}' defined {len(idxs)}x inside "
                             f"preprocessor branches: lines {where}", idxs[0])

    # ---- reachability ------------------------------------------------------
    # BFS from menu targets through panel = references.
    reachable: set[str] = set()
    queue = [t for t in menu_targets if t in known]
    while queue:
        name = queue.pop()
        if name in reachable:
            continue
        reachable.add(name)
        for idx, kind, sec in defs.get(name, []):
            _ = (kind, sec)
            # scan this object's body for panel references
            base_sec = sec
            j = idx + 1
            while j < len(ini.lines):
                if ini.section_of[j] != base_sec:
                    break
                nxt = strip_comment(ini.lines[j])
                if nxt.strip() and (
                        RE_DIALOG.match(nxt) or RE_CURVE.match(nxt)
                        or RE_TABLE.match(nxt) or RE_INDICATORPANEL.match(nxt)
                        or RE_HELP.match(nxt)):
                    break
                pm = RE_PANEL.match(nxt)
                if pm and pm.group(1) in known:
                    queue.append(pm.group(1))
                j += 1

    unreachable = sorted(n for n in known if n not in reachable)

    # A [TableEditor] table and its map3d id are two keys for ONE object, and a
    # menu entry names only one of them. Treat them as mutually reachable.
    for name in list(known):
        for idx, kind, sec in defs.get(name, []):
            if kind == "map3d":
                for sibling, sentries in defs.items():
                    if any(e[0] == idx and e[1] == "table" for e in sentries):
                        if sibling in reachable or name in reachable:
                            reachable.add(sibling)
                            reachable.add(name)

    if unreachable:
        # Only report objects that look like real UI roots. Most noise here is
        # intentional scaffolding reached via [FrontPage] / dashboards / tools
        # rather than via [Menu].
        for name in sorted(n for n in known if n not in reachable):
            kind = defs[name][0][1]
            idx = defs[name][0][0]
            if kind in ("gauge", "map3d"):
                continue   # gauges live on dashboards, not in menus
            if name.lower().startswith(("blankfield", "invalid")):
                continue
            rep.info("UNREACHABLE",
                     f"{kind} '{name}' is defined but not reachable from "
                     "[Menu]", idx + 1)

    # ---- page protocol tables all agree on page count ----------------------
    # [Constants] declares nPages plus a parallel list per page for pageSize, pageIdentifier,
    # pageReadCommand, burnCommand, pageValueWrite, pageChunkWrite and crc32CheckCommand.
    # If any list is short, TunerStudio has no command for that page - and the symptom is not
    # an error, it is that edits to that page silently never save. That happened when page 17
    # was added and only nPages/pageSize were extended.
    page_lists = ("pageSize", "pageIdentifier", "pageReadCommand", "burnCommand",
                  "pageValueWrite", "pageChunkWrite", "crc32CheckCommand")
    declared_npages = None
    for i, raw in enumerate(ini.lines):
        stripped = strip_comment(raw).strip()
        m = re.match(r"^nPages\s*=\s*(\d+)", stripped, re.I)
        if m:
            declared_npages = int(m.group(1))
            break

    if declared_npages:
        for key in page_lists:
            for i, raw in enumerate(ini.lines):
                stripped = strip_comment(raw).strip()
                m = re.match(r"^" + key + r"\s*=\s*(.+)$", stripped, re.I)
                if not m:
                    continue
                value = m.group(1)
                count = (len(re.findall(r'"(?:[^"\\]|\\.)*"', value))
                         if key != "pageSize"
                         else len([v for v in value.split(",") if v.strip()]))
                if count != declared_npages:
                    rep.error("PAGE-TABLE-SHORT",
                              f"{key} has {count} entries but nPages={declared_npages}. "
                              f"TunerStudio will have no command for page(s) "
                              f"{count + 1}..{declared_npages}, so edits to them silently "
                              f"never save.", i + 1)
                break

    # ---- OutputChannels fit inside ochBlockSize ----------------------------
    # A channel index N is the Nth byte of the live-data payload, and a multi-byte scalar at N
    # occupies N..N+width-1. TunerStudio asks for channels 0..ochBlockSize-1 and refuses to load
    # the ini if any declaration needs a byte beyond that.
    #
    # This shipped broken once: raceTime was split across two U16 scalars, which spans THREE
    # indices (138,139 and 140,141), and the following fields were placed at 142/143 as if only
    # two were consumed. Both of those were past the end of the packet.
    oc_start = None
    och_size = None
    for i, raw in enumerate(ini.lines):
        stripped = strip_comment(raw).strip()
        if stripped == "[OutputChannels]":
            oc_start = i
            continue
        if oc_start is not None and och_size is None:
            m = re.match(r"^ochBlockSize\s*=\s*(\d+)", stripped, re.I)
            if m:
                och_size = int(m.group(1))
            elif stripped.startswith("["):
                break

    if oc_start is not None and och_size:
        widest = 0
        for i in range(oc_start, len(ini.lines)):
            stripped = strip_comment(ini.lines[i]).strip()
            if stripped.startswith("[") and i > oc_start:
                break
            m = re.match(r"^([A-Za-z_]\w*)\s*=\s*(scalar|array|bits)\s*,\s*(\w+)\s*,\s*(\d+)",
                         stripped, re.I)
            if not m:
                continue
            name, cls, typ, off = m.group(1), m.group(2).lower(), m.group(3), int(m.group(4))
            width = {"U08": 1, "S08": 1, "U16": 2, "S16": 2, "U32": 4, "S32": 4}.get(typ.upper(), 1)
            if cls == "bits":
                width = 1
            elif cls == "array":
                shape = re.search(r"\[(\d+)(?:x(\d+))?\]", stripped)
                if shape:
                    width *= int(shape.group(1)) * (int(shape.group(2)) if shape.group(2) else 1)
            top = off + width - 1
            widest = max(widest, top)
            if top > och_size - 1:
                rep.error("OCH-OVERRUN",
                          f"{name} occupies channel(s) {off}..{top}, but ochBlockSize={och_size} "
                          f"means the highest legal channel is {och_size - 1}. TunerStudio will "
                          f"refuse to load this ini.", i + 1)

        # And the converse: a packet far larger than declared is dead weight on every poll.
        if widest < och_size - 1:
            rep.info("OCH-SLACK",
                     f"ochBlockSize={och_size} but the highest declared channel is {widest}; "
                     f"{och_size - 1 - widest} unused byte(s) are sent on every poll", 0)

    # ---- curves reference declared constants, and every page has constants ----
    # Two failures that TunerStudio reports as hard errors and that nothing else here caught:
    #
    #   CURVE-BIN-UNDEFINED  `xBins = foo` where foo is not a [Constants] field. TunerStudio
    #                        aborts the ini with "CurveGraph xBin not a valid Constant".
    #   PAGE-NO-CONSTANTS    a declared page (nPages) with no `page = N` block. Deleting a page
    #                        block leaves every reference to its fields dangling, and the ini
    #                        still parses - so the failure surfaces only in TunerStudio.
    # A curve bin may legitimately name a [Constants] field OR a [PcVariables] array. The latter
    # are the calculated/derived series (wueAFR, wueRecommended, ...) that TunerStudio computes
    # rather than stores, and referencing one is valid.
    curve_bin_names = set()
    page_blocks = set()
    in_bin_section = False
    for raw in ini.lines:
        line = strip_comment(raw).strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
            in_bin_section = section in ("constants", "pcvariables")
            continue
        if in_bin_section:
            m = re.match(r"^([A-Za-z_]\w*)\s*=", line)
            if m:
                curve_bin_names.add(m.group(1))
        m = re.match(r"^page\s*=\s*(\d+)\s*$", line, re.I)
        if m:
            page_blocks.add(int(m.group(1)))

    for i, raw in enumerate(ini.lines):
        line = strip_comment(raw).strip()
        m = re.match(r"^(x|y)Bins\s*=\s*([A-Za-z_]\w*)\s*$", line, re.I)
        if m:
            target = m.group(2)
            if target not in curve_bin_names:
                rep.error("CURVE-BIN-UNDEFINED",
                          f"curve {m.group(1)}Bins references '{target}', which is not declared "
                          f"in [Constants] or [PcVariables]. TunerStudio aborts the ini with "
                          f"\"CurveGraph {m.group(1)}Bin not a valid Constant\".", i + 1)

    # Every page 1..nPages needs its own block. Page 0 is the implicit zero page.
    if declared_npages:
        for pg in range(1, declared_npages + 1):
            if pg not in page_blocks:
                rep.error("PAGE-NO-CONSTANTS",
                          f"pageSize declares {declared_npages} pages but there is no "
                          f"`page = {pg}` block. Every field on that page is undefined, and "
                          f"TunerStudio will report each reference as an invalid Constant.",
                          0)

    # ---- array defaultValue arity ------------------------------------------
    # An array `defaultValue =` must supply exactly as many values as the array's shape. A
    # short list leaves the tail at whatever the page held, which is how a curve ends up partly
    # defaulted and partly garbage.
    for i, raw in enumerate(ini.lines):
        line = strip_comment(raw).strip()
        m = re.match(r"^defaultValue\s*=\s*([A-Za-z_]\w*)\s*,\s*(.*)$", line, re.I)
        if not m:
            continue
        name, values = m.group(1), m.group(2)
        decl = re.search(r"^\s*" + re.escape(name) +
                         r"\s*=\s*array\s*,\s*\w+\s*,\s*\S+\s*,\s*\[(\d+)(?:x(\d+))?\]",
                         "\n".join(ini.lines), re.M)
        if not decl:
            continue
        expected = int(decl.group(1)) * (int(decl.group(2)) if decl.group(2) else 1)
        got = len(re.findall(r"-?\d+(?:\.\d+)?", values))
        if got != expected:
            rep.error("DEFAULT-ARITY",
                      f"{name}: defaultValue supplies {got} value(s) but the array shape "
                      f"needs {expected}. The shortfall is left uninitialised.", i + 1)

    # ---- self-gated menus (unreachable feature) ----------------------------
    # A subMenu gated on a constant that lives INSIDE the dialog it opens can never be
    # entered: the item is greyed out until the setting is on, and that dialog is the only
    # place to switch it on. This shipped once - `subMenu = raceTimeSettings, ..., { rtEnable }`
    # greyed out the very page that holds the enable switch.
    #
    # The established convention in this file is that the dialog holding an enable switch is
    # never gated on it (LaunchControl, lttSettings, fanSettings all follow this).
    def _dialog_owned_fields(dialog_name: str) -> set[str]:
        hits = defs.get(dialog_name)
        if not hits:
            return set()
        start = hits[0][0]
        owned: set[str] = set()
        for j in range(start + 1, len(ini.lines)):
            raw_j = strip_comment(ini.lines[j]).strip()
            if not raw_j:
                continue
            if ini.lines[j].strip().startswith("["):
                break
            if re.match(r"^(dialog|panel|curve|table|indicatorPanel|help|menu|"
                        r"groupMenu|groupChildMenu|subMenu)\s*=", raw_j, re.I):
                break
            fm = re.match(r'^field\s*=\s*"[^"]*"\s*,\s*([A-Za-z_]\w*)', raw_j, re.I)
            if fm:
                owned.add(fm.group(1))
        return owned

    for i, raw in enumerate(ini.lines):
        line = strip_comment(raw).strip()
        m = re.match(r'^subMenu\s*=\s*([A-Za-z0-9_.]+)\s*,\s*"[^"]*"\s*,\s*0\s*,'
                     r'\s*\{([^}]*)\}', line, re.I)
        if not m:
            continue
        target, cond = m.group(1), m.group(2)
        owned = _dialog_owned_fields(target)
        if not owned:
            continue
        named = set(re.findall(r"[A-Za-z_]\w*", cond))
        clash = named & owned
        if clash:
            rep.error("MENU-SELF-GATED",
                      f"'{target}' is hidden by a condition it controls itself "
                      f"({', '.join(sorted(clash))}). The menu item is greyed out until that "
                      f"setting is on, but this dialog is the only place to turn it on - so the "
                      f"feature is unreachable. Drop the gate, as LaunchControl/lttSettings do.",
                      i + 1)

    # ---- [Constants] declaration syntax -------------------------------------
    # The layout checks above deliberately ignore field syntax, so a malformed
    # declaration passes here and only fails when TunerStudio loads the ini - which is
    # exactly what happened with `rtCurveLabel = string, ASCII, 16, [20], "Axis label"`:
    # the string class takes a plain length as its 4th field, not a [shape]. Catching it
    # here costs one regex per line and saves a hardware-in-the-loop trip.
    #
    # These checks are deliberately *structural* only. Trailing arguments are optional in
    # practice (`divider = scalar, U08, 25, "", 1.0, 0.0` is a real, working entry with no
    # lo/hi/digits), so argument counts are not policed - only the things that make
    # TunerStudio reject the page outright.
    # Only the page declaration bodies are field declarations. The [Constants] header
    # (nPages, pageSize, burnCommand, blockingFactor, ...) is controller protocol
    # configuration, not a field, and must not be validated as one.
    in_page_body = False
    for i, raw in enumerate(ini.lines):
        if ini.section_of[i] != SECTION_CONSTANTS:
            in_page_body = False
            continue
        line = strip_comment(raw).strip()
        if re.match(r"^page\s*=", line, re.I):
            in_page_body = True
            continue
        if not in_page_body or not line or line.startswith("#"):
            continue

        m = RE_CONST_DECL.match(line)
        if not m:
            if "=" in line:
                rep.error("CONST-MALFORMED",
                          f"[Constants] line is not a valid declaration: {line[:70]!r}", i + 1)
            continue

        name, cls, ctype, rest = m.group(1), m.group(2).lower(), m.group(3), m.group(4)
        if ctype.upper() not in VALID_CONST_TYPES:
            rep.error("CONST-BAD-TYPE",
                      f"{name}: unknown type {ctype!r} (expected one of "
                      f"{', '.join(sorted(VALID_CONST_TYPES))})", i + 1)

        args = split_args(rest)
        if not args or not args[0]:
            rep.error("CONST-NO-OFFSET", f"{name}: no offset given", i + 1)
            continue

        if cls == "string":
            # Spec: name = string, ASCII, offset, length [, displayColumns]
            if len(args) < 2:
                rep.error("CONST-STRING-NO-LENGTH",
                          f"{name}: string needs a length, e.g. "
                          f"`{name} = string, ASCII, {args[0]}, 20`", i + 1)
            elif not re.fullmatch(r"\d+", args[1]):
                rep.error("CONST-STRING-BAD-LENGTH",
                          f"{name}: string length must be a plain integer, got {args[1]!r}. "
                          f"A [shape] is array/bits syntax and makes TunerStudio reject "
                          f"the page.", i + 1)
            elif len(args) > 3:
                rep.error("CONST-STRING-EXTRA",
                          f"{name}: string takes at most length + displayColumns, got "
                          f"{len(args)} arguments", i + 1)
        elif cls == "scalar":
            if len(args) > 1 and args[1].startswith("["):
                rep.error("CONST-SCALAR-SHAPE",
                          f"{name}: scalar must not take a [shape]; use array", i + 1)
        elif cls == "array":
            if len(args) > 1 and not args[1].startswith("["):
                rep.error("CONST-ARRAY-NO-SHAPE",
                          f"{name}: array needs a [shape] as its second field, got "
                          f"{args[1]!r}", i + 1)
        elif cls == "bits":
            if len(args) > 1 and not args[1].startswith("["):
                rep.error("CONST-BITS-NO-RANGE",
                          f"{name}: bits needs a [lo:hi] range as its second field, got "
                          f"{args[1]!r}", i + 1)

    # ---- constants integrity digest ----------------------------------------
    digest = constants_digest(ini)

    # ---- report ------------------------------------------------------------
    print(f"\nINI layout check - {path}")
    print(f"  sections scanned, {len(known)} referenceable objects defined, "
          f"{len(reachable)} reachable from [Menu]")
    print(f"  [Constants] digest: {digest}")
    print()
    rep.dump(show_info=verbose)
    print()
    n_err, n_warn = rep.count("ERROR"), rep.count("WARN")

    # Offsets are the one thing that silently corrupts existing tunes if moved,
    # so a changed digest is a hard failure unless explicitly acknowledged.
    if digest != EXPECTED_CONSTANTS_DIGEST:
        rep.error("CONSTANTS-DRIFT",
                  f"[Constants] digest changed: expected "
                  f"{EXPECTED_CONSTANTS_DIGEST}, got {digest}. An existing "
                  "offset may have moved, which silently invalidates saved "
                  ".msq files. Confirm the change is intentional, then update "
                  "EXPECTED_CONSTANTS_DIGEST in this file.")
        n_err += 1

    print(f"  {n_err} error(s), {n_warn} warning(s)")
    if n_err:
        print("\n  FAILED")
        return 1
    if strict and n_warn:
        print("\n  FAILED (--strict: warnings are errors)")
        return 1
    print("\n  PASSED")
    return 0


def _check_dialog_header(rep: Report, ini: Ini, i: int,
                         name: str, rest: str) -> None:
    if not rest:
        rep.warn("DLG-NO-TITLE",
                 f"dialog '{name}' has no title argument (it will render with "
                 "no frame when used as a panel)", ini.lineno(i))
        return
    if rest.rstrip().endswith(","):
        rep.warn("DLG-TRAILING-COMMA",
                 f"dialog '{name}' ends with a comma", ini.lineno(i))
    args = split_args(rest)
    if not args or not args[0]:
        rep.warn("DLG-NO-TITLE", f"dialog '{name}' has an empty title slot",
                 ini.lineno(i))
        return
    if unquote(args[0]) == "":
        # "" is the documented way to remove the frame when a dialog is used
        # as a panel - intentional, not a defect.
        rep.info("DLG-EMPTY-TITLE",
                 f"dialog '{name}' has an empty title (no frame; normal for "
                 "layout scaffolding)", ini.lineno(i))
        return
    if len(args) > 1:
        layout = unquote(args[1]).strip()
        if not layout:
            return
        if re.fullmatch(r"\d+", layout):
            rep.error("DLG-STRAY-NUMBER",
                      f"dialog '{name}' has page number '{layout}' in the "
                      "layout slot (page numbers belong on subMenu, not "
                      "dialog)", ini.lineno(i))
        elif layout.lower() not in VALID_LAYOUTS:
            rep.error("DLG-BAD-LAYOUT",
                      f"dialog '{name}' layout '{layout}' is not one of "
                      f"{sorted(VALID_LAYOUTS)}", ini.lineno(i))


def _check_target(rep: Report, ini: Ini, i: int, target: str,
                  current_menu: str | None,
                  seen_in_menu: dict[tuple[str, str], int],
                  menu_items: dict[str, list[str]],
                  menu_targets: list[str]) -> None:
    if not target:
        rep.error("MENU-DANGLING", "empty menu target", ini.lineno(i))
        return
    menu_targets.append(target)
    if not (target.lower().startswith("std_") or _is_known(ini, target)):
        if missing_comma(target):
            first = target.split('"')[0].strip()
            rep.error("MENU-MISSING-COMMA",
                      f"missing comma between target '{first}' and its label "
                      "- TunerStudio will treat the whole string as the "
                      "target name", ini.lineno(i))
        else:
            rep.error("MENU-DANGLING",
                      f"menu target '{target}' is not a defined object "
                      "and not a std_* builtin", ini.lineno(i))
    if current_menu is not None:
        key = (current_menu, target)
        if key in seen_in_menu:
            rep.warn("MENU-DUPLICATE",
                     f"'{target}' listed twice in menu '{current_menu}' "
                     f"(first at line {seen_in_menu[key]})", ini.lineno(i))
        else:
            seen_in_menu[key] = ini.lineno(i)
        menu_items[current_menu].append(target)


_KNOWN_CACHE: dict[str, set[str]] = {}


def _is_known(ini: Ini, name: str) -> bool:
    """Cheap memoised membership test against collected definitions."""
    if ini.path not in _KNOWN_CACHE:
        known: set[str] = set()
        for i, raw in enumerate(ini.lines):
            sec = ini.section_of[i]
            line = strip_comment(raw)
            if not line.strip():
                continue
            if sec == SECTION_USERDEF:
                for rx in (RE_DIALOG, RE_INDICATORPANEL, RE_HELP):
                    m = rx.match(line)
                    if m:
                        known.add(m.group(1))
                        break
            elif sec == SECTION_CURVE:
                m = RE_CURVE.match(line)
                if m:
                    known.add(m.group(1))
            elif sec == SECTION_TABLE:
                m = RE_TABLE.match(line)
                if m:
                    known.add(m.group(1))
                    known.add(m.group(2))
        _KNOWN_CACHE[ini.path] = known
    return name in _KNOWN_CACHE[ini.path]


def constants_digest(ini: Ini) -> str:
    """Digest of [Constants] field names + their textual definition.

    Any change here means tune offsets may have moved. Two INIs with the same
    digest have byte-identical constant declarations.

    Comments and blank lines are stripped because they carry no offset
    information. `lastOffset` is normalised to a constant token: it always
    resolves to the running byte position, so writing `<name> = lastOffset`
    for a new locator cannot move an existing offset.
    """
    buf = []
    inside = False
    for i, raw in enumerate(ini.lines):
        if RE_SECTION.match(raw):
            inside = ini.section_of[i] == SECTION_CONSTANTS
            continue
        if inside:
            line = strip_comment(raw).strip()
            if not line:
                continue
            line = re.sub(r"=\s*lastOffset\b", "= <lastOffset>", line)
            buf.append(" ".join(line.split()))
    return hashlib.sha256("\n".join(buf).encode("utf-8")).hexdigest()[:16]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", nargs="?",
                    default="reference/speeduino.ini",
                    help="path to speeduino.ini (default: %(default)s)")
    ap.add_argument("--strict", action="store_true",
                    help="treat warnings as errors")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="also show INFO findings (unreachable objects)")
    ap.add_argument("--constants-hash", action="store_true",
                    help="print only the [Constants] digest and exit")
    a = ap.parse_args()

    if a.constants_hash:
        print(constants_digest(Ini(a.path)))
        return 0
    return check(a.path, a.strict, a.verbose)


if __name__ == "__main__":
    sys.exit(main())
