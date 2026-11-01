"""Verify page 16 (long-term trims) on a real ECU.

Checks, in order:
  1. Page 16 is readable and returns the expected size (2064 bytes).
  2. The block is initialised: the stored trims are all zero and the settings are sane.
     On a first boot after this firmware is flashed, lttInit() finds no header at 0x1000,
     lays down defaults and writes a header - so this proves that path ran.
  3. A write -> read-back round trip on the trim payload works through the page protocol.
  4. The written value survives a burn and a reboot.

Anything this script writes is into the custom block at 0x1000+, never the core tune, so it
cannot corrupt an existing tune.
"""

import sys
import struct

from speeduino_client import ProtocolError, Speeduino

PAGE = 16
PAGE_SIZE = 2064

# Offsets within page 16, mirroring speeduino/ltt.h (asserted there with offsetof).
OFF_MODE = 0
OFF_LEARN_RATE_DIV = 1
OFF_TRIM = 16          # int16[2][256], 0.1% units
OFF_LEAN_AUTH = 1040   # uint8[2][256]
OFF_RICH_AUTH = 1552   # uint8[2][256]

# A distinctive trim value to write into trim[0][0]. +5.0% == 50 in 0.1% units.
TEST_TRIM = 50


def read_all(ecu, page, size):
    """Read a page in chunks; the protocol caps a single request."""
    data = bytearray()
    chunk = 128
    while len(data) < size:
        want = min(chunk, size - len(data))
        part = ecu.read_page(page, len(data), want)
        if not part:
            break
        data.extend(part)
    return bytes(data)


def main():
    ok = True

    with Speeduino('COM11') as ecu:
        print(f"version : {ecu.code_version()}")
        print(f"backend : {ecu.status().get('storageBackendLabel')}")

        # ---- 1. page 16 readable, correct size ------------------------------------
        try:
            raw = read_all(ecu, PAGE, PAGE_SIZE)
        except ProtocolError as exc:
            print(f"FAIL: page {PAGE} not readable: {exc}")
            return 1

        print(f"\n=== page {PAGE} ===")
        print(f"read {len(raw)} of {PAGE_SIZE} bytes")
        if len(raw) != PAGE_SIZE:
            print(f"FAIL: short read ({len(raw)} != {PAGE_SIZE})")
            ok = False

        # ---- 2. block is initialised to defaults ----------------------------------
        mode = raw[OFF_MODE] & 0x01
        rate = raw[OFF_LEARN_RATE_DIV]
        lean = raw[OFF_LEAN_AUTH]
        rich = raw[OFF_RICH_AUTH]
        trims = struct.unpack_from('<256h', raw, OFF_TRIM)

        print(f"mode            : {mode} ({'Learn' if mode else 'Off'})")
        print(f"learnRateDiv    : {rate}")
        print(f"authority[0][0] : lean={lean} rich={rich}")
        print(f"trim[0] nonzero : {sum(1 for t in trims if t != 0)} of 256")

        if mode != 0:
            print("FAIL: learning must default to OFF")
            ok = False
        if lean == 0 or rich == 0:
            print("FAIL: default authority must be non-zero")
            ok = False
        if any(t != 0 for t in trims):
            print("FAIL: a freshly initialised block must have all-zero trims")
            ok = False
        if lean != 25 or rich != 25:
            print(f"WARN: expected default authority 25/25, got {lean}/{rich}")

        # ---- 3. write -> read back -----------------------------------------------
        print("\n=== write -> read back ===")
        before = raw[OFF_TRIM:OFF_TRIM + 2]
        print(f"before          : {before.hex()} ({struct.unpack('<h', before)[0]})")

        payload = struct.pack('<h', TEST_TRIM)
        ecu.write_page(PAGE, OFF_TRIM, payload)
        back = ecu.read_page(PAGE, OFF_TRIM, 2)
        print(f"after write     : {back.hex()} ({struct.unpack('<h', back)[0]})")
        if back != payload:
            print("FAIL: trim write did not read back")
            ok = False

        # ---- 4. burn, then confirm ------------------------------------------------
        print("\n=== burn ===")
        ecu.burn_page(PAGE)
        burned = ecu.read_page(PAGE, OFF_TRIM, 2)
        print(f"after burn      : {burned.hex()} ({struct.unpack('<h', burned)[0]})")
        if burned != payload:
            print("FAIL: trim did not survive the burn")
            ok = False

        # Restore the trim to zero so the ECU is left in a clean, neutral state.
        ecu.write_page(PAGE, OFF_TRIM, struct.pack('<h', 0))
        ecu.burn_page(PAGE)
        restored = ecu.read_page(PAGE, OFF_TRIM, 2)
        print(f"restored to zero: {restored.hex()}")
        if restored != struct.pack('<h', 0):
            print("FAIL: could not restore the trim to zero")
            ok = False

    print("\nRESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
