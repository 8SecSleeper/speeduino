"""
Back up / restore a Speeduino tune over the serial protocol.

Page sizes are NOT uniform. Config pages 2, 3, 5, 8, 11 and 14 are 288 bytes and page 8 is
384 bytes, so reading with a fixed 256-byte window silently truncates them and produces an
incomplete backup. Sizes come from `reference/speeduino.ini` (pageSize=) and are checked
against the firmware's getPageSize() by the write path, which rejects anything out of range.

Writes must also stay within the page: updatePageValues() rejects offsets that run past
getPageSize(pageNum) with SERIAL_RC_RANGE_ERR (0x84). Chunks are therefore clamped to the
declared page size, never a fixed constant.

Usage:
    python tune_backup.py COM11 save    tune_backup.json
    python tune_backup.py COM11 restore tune_backup.json
"""

import json
import sys

from speeduino_client import ProtocolError, Speeduino

# Config pages 1..15. Sizes from reference/speeduino.ini: pageSize= ...
PAGE_SIZES = {
    1: 128, 2: 288, 3: 288, 4: 128, 5: 288, 6: 128, 7: 240, 8: 384,
    9: 192, 10: 192, 11: 288, 12: 192, 13: 128, 14: 288, 15: 256,
}

# Sensor calibration tables (O2, IAT, CLT) at MAX_PAGE_NUM+1..+3, see pages.h.
CALIBRATION_PAGES = {16: 96, 17: 96, 18: 96}

# Keep individual protocol payloads modest so a request stays well inside the firmware's
# serial payload buffer.
CHUNK = 64


def save(ecu, path):
    data = {"pages": {}}

    for page, size in sorted({**PAGE_SIZES, **CALIBRATION_PAGES}.items()):
        try:
            raw = ecu.read_page(page, 0, size)
            if len(raw) != size:
                print(f"  page {page:2}: WARNING expected {size} got {len(raw)}")
            data["pages"][str(page)] = raw.hex()
            blank = (raw == b"\xff" * len(raw))
            print(f"  page {page:2}: {len(raw):3} bytes  {'blank' if blank else 'data'}")
        except ProtocolError as exc:
            print(f"  page {page:2}: FAILED ({exc})")

    with open(path, "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=1)
    print(f"\nSaved to {path}")


def restore(ecu, path):
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)

    for page_str, hexdata in sorted(data["pages"].items(), key=lambda kv: int(kv[0])):
        page = int(page_str)
        raw = bytes.fromhex(hexdata)
        limit = PAGE_SIZES.get(page, CALIBRATION_PAGES.get(page, len(raw)))
        if len(raw) > limit:
            print(f"  page {page:2}: truncating backup {len(raw)} -> {limit} (page size)")
            raw = raw[:limit]

        off = 0
        while off < len(raw):
            chunk = raw[off:off + CHUNK]
            # Never let a chunk run past the page boundary - the firmware rejects it.
            if off + len(chunk) > limit:
                chunk = chunk[:limit - off]
            if not chunk:
                break
            ecu.write_page(page, off, chunk)
            off += len(chunk)
        ecu.burn_page(page)
        print(f"  page {page:2}: restored + burned ({len(raw)} bytes)")

    print("\nRestored. Power-cycle and confirm the tune reads back correctly.")


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    port, action, path = sys.argv[1], sys.argv[2], sys.argv[3]

    with Speeduino(port) as ecu:
        print(f"version: {ecu.code_version()}")
        print(f"backend: {ecu.status().get('storageBackendLabel')}\n")
        if action == "save":
            save(ecu, path)
        elif action == "restore":
            restore(ecu, path)
        else:
            print(f"unknown action {action!r}")
            return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
