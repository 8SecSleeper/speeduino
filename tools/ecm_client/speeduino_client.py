"""
Minimal Speeduino binary-protocol client, for driving real hardware from the PC.

Wire format, both directions:
    [len_hi][len_lo][payload ... ][crc32 of payload, 4 bytes BIG-endian]

Notes learned the hard way:
  * The CRC covers the PAYLOAD ONLY, not the 2 length bytes.
  * Everything multi-byte is big-endian on the wire.
  * The whole request (len + payload + crc) must arrive within 400 ms of its first
    byte, or the firmware aborts the receive and replies SERIAL_RC_TIMEOUT (0x80).
  * Do not send a new command until the previous response is fully read - the
    receive state machine only accepts a new command when SERIAL_INACTIVE.

Usage:
    from speeduino_client import Speeduino
    ecu = Speeduino("COM11")
    print(ecu.status())          # storage backend, RPM, etc.
    ecu.close()
"""

import struct
import time
import zlib

import serial

# Serial return codes
RC_OK = 0x00
RC_REALTIME = 0x01
RC_PAGE = 0x02
RC_BURN_OK = 0x04
RC_TIMEOUT = 0x80
RC_CRC_ERR = 0x82
RC_UKWN_ERR = 0x83
RC_RANGE_ERR = 0x84
RC_BUSY_ERR = 0x85

# 'r' sub-command for "send output channels" (SEND_OUTPUT_CHANNELS in comms.cpp)
SEND_OUTPUT_CHANNELS = 48

RC_NAMES = {
    RC_OK: "OK",
    RC_REALTIME: "REALTIME",
    RC_PAGE: "PAGE",
    RC_BURN_OK: "BURN_OK",
    RC_TIMEOUT: "TIMEOUT",
    RC_CRC_ERR: "CRC_ERR",
    RC_UKWN_ERR: "UNKNOWN_CMD",
    RC_RANGE_ERR: "RANGE_ERR",
    RC_BUSY_ERR: "BUSY_ERR",
}

# statuses::StorageBackend, exposed in status5 byte 127 bits 6-7
STORAGE_BACKEND = {
    0: "Internal EEPROM",
    1: "SPI Flash",
    2: "Flash Not Found",
    3: "Flash Fallback",
}


def frame(payload: bytes) -> bytes:
    """Encode a request frame."""
    return (
        struct.pack(">H", len(payload))
        + payload
        + struct.pack(">I", zlib.crc32(payload) & 0xFFFFFFFF)
    )


class ProtocolError(Exception):
    pass


class Speeduino:
    def __init__(self, port="COM11", baud=115200, verbose=False):
        self.port = port
        self.verbose = verbose
        # Small read timeout: we parse precisely, so we can afford to be strict.
        self.ser = serial.Serial(port, baud, timeout=2.0)
        time.sleep(0.2)
        self.ser.reset_input_buffer()

    # ------------------------------------------------------------------ low level

    def _drain(self, settle=0.05):
        time.sleep(settle)
        self.ser.reset_input_buffer()

    def _read_exact(self, n, timeout=2.0):
        end = time.time() + timeout
        buf = b""
        while len(buf) < n:
            if time.time() > end:
                raise ProtocolError(f"timeout reading {n} bytes (got {len(buf)})")
            chunk = self.ser.read(n - len(buf))
            if chunk:
                buf += chunk
        return buf

    def request(self, payload: bytes, timeout=2.0):
        """Send a framed request and read one framed response. Returns payload bytes."""
        self.ser.reset_input_buffer()
        self.ser.write(frame(payload))
        self.ser.flush()

        hdr = self._read_exact(2, timeout)
        length = struct.unpack(">H", hdr)[0]
        if length > 4200:
            raise ProtocolError(f"implausible response length {length}")
        body = self._read_exact(length, timeout) if length else b""
        crc = self._read_exact(4, timeout)

        want = zlib.crc32(body) & 0xFFFFFFFF
        got = struct.unpack(">I", crc)[0]
        if want != got:
            raise ProtocolError(
                f"response CRC mismatch: got {got:08x} want {want:08x} (body={body.hex()})"
            )
        if self.verbose:
            print(f"  > {payload!r}  < {body.hex()}")
        return body

    def request_rc(self, payload: bytes, timeout=2.0):
        """Send a request that returns a single return-code payload. Raises on non-OK."""
        body = self.request(payload, timeout)
        if len(body) != 1:
            raise ProtocolError(f"expected 1-byte return code, got {body.hex()}")
        rc = body[0]
        if rc not in (RC_OK, RC_BURN_OK):
            raise ProtocolError(f"return code {rc:#04x} ({RC_NAMES.get(rc, '?')})")
        return rc

    # ------------------------------------------------------------------ commands

    def test_comms(self):
        """'C' - the handshake TunerStudio uses to detect an ECU."""
        return self.request(b"C")

    def code_version(self):
        """'Q' - version string."""
        return self.request(b"Q")[1:].decode("ascii", "replace")

    def product_string(self):
        """'S' - product string."""
        return self.request(b"S")[1:].decode("ascii", "replace")

    def send_capabilities(self):
        """'f' - serial capability details."""
        b = self.request(b"f")
        return {
            "proto_version": b[1],
            "blocking_factor": (b[2] << 8) | b[3],
            "table_blocking_factor": (b[4] << 8) | b[5],
        }

    def page_crc(self, page):
        """'d' - CRC32 of a stored page, as 4 raw bytes."""
        return self.request(bytes([ord("d"), 0, page]))

    def read_page(self, page, offset=0, length=128):
        """'p' - read from a config page.

        Payload layout (see comms.cpp case 'p'):
            [0]='p' [1]=? [2]=page [3..4]=offset (LE) [5..6]=length (LE)

        Returns the page bytes (RC byte stripped). Raises on a non-OK return code.
        """
        req = (
            bytes([ord("p"), 0, page])
            + struct.pack("<H", offset)
            + struct.pack("<H", length)
        )
        body = self.request(req)
        if body[0] != RC_OK:
            raise ProtocolError(f"page read rc={body[0]:#04x}")
        return body[1:]

    def write_page(self, page, offset, data):
        """'M' - write to a config page (RAM copy only; needs a burn to persist).

        Payload layout (see comms.cpp case 'M'):
            [0]='M' [1]=? [2]=page [3..4]=offset (LE) [5..6]=length (LE) [7..]=data
        """
        req = (
            bytes([ord("M"), 0, page])
            + struct.pack("<H", offset)
            + struct.pack("<H", len(data))
            + bytes(data)
        )
        return self.request_rc(req)

    def burn_page(self, page):
        """'b' - burn (write) a single page to EEPROM."""
        return self.request_rc(bytes([ord("b"), 0, page]))

    def realtime_raw(self, offset=0, length=128):
        """'r' with SEND_OUTPUT_CHANNELS - the realtime/output-channel block."""
        req = (
            bytes([ord("r"), 0, SEND_OUTPUT_CHANNELS])
            + struct.pack("<H", offset)
            + struct.pack("<H", length)
        )
        body = self.request(req)
        if body[0] != RC_OK:
            raise ProtocolError(f"realtime rc={body[0]:#04x}")
        return body[1:]

    # ------------------------------------------------------------------ decoding

    def status(self):
        """Decode the useful parts of the realtime block.

        Byte offsets come from reference/speeduino.ini output channel definitions.
        status5 is at offset 127; storageBackend is bits 6-7 of it.
        """
        need = 128
        rt = self.realtime_raw(0, need)
        out = {"_len": len(rt), "rpm": rt[0] | (rt[1] << 8)}

        if len(rt) > 127:
            s5 = rt[127]
            out["status5"] = s5
            out["storageBackend"] = (s5 >> 6) & 0x03
            out["storageBackendLabel"] = STORAGE_BACKEND.get(out["storageBackend"], "?")
        else:
            out["storageBackendLabel"] = f"<realtime only {len(rt)} bytes>"
        return out

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


if __name__ == "__main__":
    import sys

    port = sys.argv[1] if len(sys.argv) > 1 else "COM11"
    with Speeduino(port, verbose=True) as ecu:
        print("test_comms  :", ecu.test_comms().hex())
        print("code version:", ecu.code_version())
        print("product     :", ecu.product_string())
        print("capability  :", ecu.send_capabilities())
