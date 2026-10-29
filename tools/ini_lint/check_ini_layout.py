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
RE_INDICATORPANEL = re.compile(r"^\s*indicatorPanel\s*=\s*([A-Za-z0-9_.]+)", re.I)
RE_HELP = re.compile(r"^\s*help\s*=\s*([A-Za-z0-9_.]+)", re.I)

# A "field" in [Constants] - used only for the offset-integrity digest.
RE_CONST_FIELD = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=")


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
            if g not in known and not g.lower().startswith("std_"):
                rep.error("DLG-DANGLING-GAUGE",
                          f"gauge = {g} not defined in [GaugeConfigurations]",
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
    """
    buf = []
    inside = False
    for i, raw in enumerate(ini.lines):
        if RE_SECTION.match(raw):
            inside = ini.section_of[i] == SECTION_CONSTANTS
            continue
        if inside:
            line = strip_comment(raw).rstrip()
            if line.strip():
                buf.append(line)
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
