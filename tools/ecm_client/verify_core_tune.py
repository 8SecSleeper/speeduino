"""Confirm the existing core tune is intact after adding page 16.

Adding a page and a custom block above 0x1000 must not disturb 0x0000-0x0FFF. This reads a
handful of core tune fields back and prints them, so an accidental offset shift is visible
immediately rather than showing up as mysterious wrong values in TunerStudio.
"""

import sys

from speeduino_client import Speeduino

# configPage2 lives on TS page 1 (veSetPage), NOT page 2. Page 2 is the VE table itself
# (veMapPage). See speeduino/pages.h - the two were swapped in Feb 2019.
PAGE_CONFIG2 = 1

# Offsets within page 1 (configPage2), from reference/speeduino.ini. The field is called
# `pinLayout` in the INI (it was `pinMapping` in older firmwares) and index 55 in
# pinLayouts is "Drop Bear".
OFF_PIN_LAYOUT = 15
OFF_REQFUEL = 24
OFF_NCYLINDERS = 36      # bits [4:7]

DROPPBEAR_PIN_LAYOUT = 55


def main():
    with Speeduino('COM11') as ecu:
        print(f"version : {ecu.code_version()}")
        print(f"backend : {ecu.status().get('storageBackendLabel')}")

        raw = ecu.read_page(PAGE_CONFIG2, 0, 48)
        pin_layout = raw[OFF_PIN_LAYOUT]
        req_fuel = raw[OFF_REQFUEL]
        n_cyl = (raw[OFF_NCYLINDERS] >> 4) & 0x0F

        print(f"\ncore tune (page 1, configPage2):")
        print(f"  pinLayout  : {pin_layout}   (55 == DropBear)")
        print(f"  nCylinders : {n_cyl}")
        print(f"  reqFuel    : {req_fuel}  (x0.1 ms)")

        ok = True
        if pin_layout == 0 or pin_layout == 255:
            print("FAIL: pinLayout is blank - the core tune looks erased")
            ok = False
        elif pin_layout != DROPPBEAR_PIN_LAYOUT:
            print(f"WARN: pinLayout is {pin_layout}, expected {DROPPBEAR_PIN_LAYOUT} (DropBear)")
        if req_fuel == 0 or req_fuel == 255:
            print("FAIL: reqFuel is blank - the core tune looks erased")
            ok = False
        if n_cyl == 0:
            print("FAIL: nCylinders is blank - the core tune looks erased")
            ok = False

    print("\nRESULT:", "PASS - core tune intact" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
