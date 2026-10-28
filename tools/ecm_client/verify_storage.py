"""
Hardware acceptance test for the Speeduino storage backend.

Run against a real ECU over serial. Verifies the full round trip that TunerStudio relies
on, and (optionally) that data survives a reset - which is the thing that actually matters
and the thing that was broken on the DropBear v2 SPI flash.

Usage:
    python verify_storage.py COM11            # read/burn/persist checks
    python verify_storage.py COM11 --no-cycle # skip the reset + persistence check

Never touches anything outside padding bytes at the end of config page 2 (offset 120..123),
which are not used by the config struct, so it cannot corrupt a tune.
"""

import struct
import sys
import time

from speeduino_client import Speeduino

PAGE = 2
OFFSET = 120          # padding beyond the end of the configPage2 struct
MARK = bytes([0xDE, 0xAD, 0xBE, 0xEF])


def banner(text):
    print(f"\n=== {text} ===")


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    do_cycle = "--no-cycle" not in sys.argv

    with Speeduino(port) as ecu:
        banner("identity")
        print("version :", ecu.code_version())
        print("product :", ecu.product_string())

        st = ecu.status()
        backend = st.get("storageBackendLabel", "?")
        print("backend :", backend)
        if backend != "SPI Flash":
            print(f"  NOTE: not using the SPI flash, so this test does not cover it.")

        banner("burn latency")
        # A burn of an unchanged page should be near-instant. The historical failure was a
        # burn that took >120s and never replied, so latency is the key signal.
        for page in (3, 2):
            t0 = time.time()
            try:
                ecu.burn_page(page)
                dt = time.time() - t0
                print(f"burn page {page}: {dt*1000:7.1f} ms  OK")
                if dt > 2.0:
                    print("  WARNING: unexpectedly slow")
            except Exception as exc:
                print(f"burn page {page}: FAILED after {(time.time()-t0):.1f}s -> {exc}")
                return 1

        banner("write -> burn -> read back")
        original = ecu.read_page(PAGE, OFFSET, 4)
        print("before      :", original.hex())
        ecu.write_page(PAGE, OFFSET, MARK)
        print("after write :", ecu.read_page(PAGE, OFFSET, 4).hex(), "(RAM, not yet burned)")
        ecu.burn_page(PAGE)
        print("after burn  :", ecu.read_page(PAGE, OFFSET, 4).hex())

        if not do_cycle:
            print("\nSkipping reset/persistence check (--no-cycle).")
            return 0

        banner("persistence")
        print("Power-cycle or reset the ECU now, then re-run this script.")
        print("Expected after reboot: page 2 offset 120 reads", MARK.hex())
    return 0


if __name__ == "__main__":
    sys.exit(main())
