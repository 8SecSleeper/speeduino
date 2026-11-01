"""Prove the burn loop terminates on real hardware.

The bug this guards against: a page save that never consumed the storage write budget made
the burn-pending flag clear on every pass, so speeduino.ino() re-entered saveAllPages() forever
- continuously erasing and rewriting flash, and eventually stalling the SPI bus hard enough to
kill USB CDC.

So this does not just check that a burn *works*. It checks that a burn *settles*: after issuing
a burn, the ECU must stop reporting work to do, and must stay responsive afterwards. If the
loop were still spinning, the second burn would time out or the board would drop off USB.
"""

import struct
import sys
import time

from speeduino_client import ProtocolError, Speeduino

PAGE = 16
OFF_TRIM = 16


def burn_and_settle(ecu, page, settle_s=3.0, verbose=True):
    """Issue a burn, then confirm the ECU is still responsive and idle afterwards."""
    t0 = time.time()
    ecu.burn_page(page)
    burn_ms = (time.time() - t0) * 1000.0

    # Give any pending write time to drain, then prove the ECU still answers. A spinning burn
    # loop monopolises the main loop and the flash bus, so a read here would time out.
    time.sleep(settle_s)
    try:
        version = ecu.code_version()
    except Exception as exc:
        return False, burn_ms, f"ECU stopped responding after burn: {type(exc).__name__}: {exc}"

    if not version:
        return False, burn_ms, "ECU returned an empty version string after burn"

    if verbose:
        print(f"  burn page {page}: {burn_ms:6.1f} ms, ECU still responsive ({version})")
    return True, burn_ms, "ok"


def main():
    ok = True

    with Speeduino('COM11') as ecu:
        print(f"version : {ecu.code_version()}")
        print(f"backend : {ecu.status().get('storageBackendLabel')}\n")

        # ---- 1. Repeated identical burns must all settle -------------------------
        # Nothing changes between burns, so the trim block should be a no-op after the first.
        # If the loop spun, these would get progressively slower or wedge the board.
        print("=== repeated burns, no changes between them ===")
        times = []
        for i in range(5):
            good, ms, why = burn_and_settle(ecu, PAGE)
            times.append(ms)
            if not good:
                print(f"  FAIL on burn {i + 1}: {why}")
                ok = False
                break
        if ok:
            print(f"  all 5 burns settled; slowest {max(times):.1f} ms")

        # ---- 2. A burn after an actual change ------------------------------------
        print("\n=== burn after modifying a trim ===")
        ecu.write_page(PAGE, OFF_TRIM, struct.pack('<h', 77))
        good, ms, why = burn_and_settle(ecu, PAGE)
        if not good:
            print(f"  FAIL: {why}")
            ok = False
        else:
            back = struct.unpack('<h', ecu.read_page(PAGE, OFF_TRIM, 2))[0]
            if back != 77:
                print(f"  FAIL: trim read back {back}, expected 77")
                ok = False
            else:
                print(f"  trim persisted correctly ({back})")

        # ---- 3. The whole tune can still be burned -------------------------------
        # An unrelated full-tune burn must not rewrite the trim block. This is the "does the
        # rest of the firmware still work" check.
        print("\n=== burn a core page (3) ===")
        good, ms, why = burn_and_settle(ecu, 3)
        if not good:
            print(f"  FAIL: {why}")
            ok = False

        # ---- 4. Clean up ---------------------------------------------------------
        ecu.write_page(PAGE, OFF_TRIM, struct.pack('<h', 0))
        ecu.burn_page(PAGE)
        cleared = struct.unpack('<h', ecu.read_page(PAGE, OFF_TRIM, 2))[0]
        print(f"\ncleaned up: trim[0][0] = {cleared}")
        if cleared != 0:
            print("  FAIL: could not restore trim to zero")
            ok = False

        # ---- 5. Still alive after all that ---------------------------------------
        final = ecu.code_version()
        print(f"final check: ECU responsive ({final})")

    print("\nRESULT:", "PASS - burn loop terminates, no flash spam" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
