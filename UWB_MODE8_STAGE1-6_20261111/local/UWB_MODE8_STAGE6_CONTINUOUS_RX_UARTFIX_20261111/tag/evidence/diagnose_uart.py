"""Compare known byte streams on both installed USB serial links at 2 Mbps."""

from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from hashlib import sha256
import argparse
import json
from pathlib import Path
import struct
import time
import zlib

import serial
from capture_tag_rx_status import SwitchablePiSerial, run_split_pair


HEADER = struct.Struct("<4sBBHIIII")
DONE = b'{"event":"UARTTEST_DONE"}'
FRAME_BYTES = 4 * 992 * 4 + 6400
PROFILES = ((0, 0), (512, 400), (96, 160), (64, 160))


def pattern(count):
    return b"".join(
        bytes((j * 73 + (j // 251) * 31 + trial * 17) & 255 for j in range(FRAME_BYTES))
        for trial in range(count)
    )


def send(port, cmd):
    port.write((cmd + "\n").encode("ascii"))
    port.flush()


def take(port, matcher, timeout=10):
    seen = bytearray()
    end = time.monotonic() + timeout
    start = 0
    while time.monotonic() < end:
        seen.extend(port.read(max(1, min(port.in_waiting, 65536))))
        while (start := seen.find(b"RFB8", start)) >= 0:
            if len(seen) - start < HEADER.size:
                break
            _, version, kind, length, _, n, raw, checksum = HEADER.unpack_from(seen, start)
            if (version, kind, length, raw) != (1, 1, HEADER.size, 0) or not 1 <= n <= 768:
                start += 1
                continue
            payload = seen[start + length:start + length + n]
            if len(payload) < n:
                break
            if zlib.crc32(payload) == checksum and matcher(payload):
                return bytes(seen[:start]), json.loads(payload)
            start += 1
        if start < 0:
            start = max(0, len(seen) - 3)
    raise TimeoutError(f"Missing control frame after {len(seen)} bytes")


def hello(port, baud):
    send(port, "HELLO")
    return take(port, lambda x: b'"event":"HELLO"' in x and f'"baud":{baud}'.encode() in x)[1]


def test_role(role, args, expected):
    if role == "anchor":
        port = serial.Serial(port=None, baudrate=921600, timeout=.12, write_timeout=2)
        port.dtr = False
        port.rts = False
        port.port = args.anchor
    else:
        port = SwitchablePiSerial(
            args.out / "pi_ssh_serial.log", run_split_pair.TAG_PORT, 921600, .12, 2
        )
    port.open()
    results = []
    try:
        time.sleep(.6)
        before = hello(port, 921600)
        if before["role"] != role or before["active"] or not before["hw_ok"]:
            raise RuntimeError(f"Unexpected device state: {before}")
        send(port, "BAUD 2000000")
        time.sleep(.2)
        port.baudrate = 2000000
        configured = hello(port, 2000000)
        if configured["boot"] != before["boot"]:
            raise RuntimeError(f"{role} reset during baud change")
        # The baud command and our explicit HELLO can both generate replies.
        while port.read(max(1, min(port.in_waiting, 65536))):
            pass
        for burst, gap in PROFILES:
            started = time.monotonic()
            send(port, f"UARTTEST {args.count} {burst} {gap}")
            payload, _ = take(port, lambda x: x == DONE)
            elapsed = time.monotonic() - started
            name = f"{role}_burst{burst}_gap{gap}.bin"
            (args.out / name).write_bytes(payload)
            results.append({
                "burst_bytes": burst,
                "idle_us": gap,
                "bytes_expected": len(expected),
                "bytes_received": len(payload),
                "length_delta": len(payload) - len(expected),
                "elapsed_s": elapsed,
                "sha256": sha256(payload).hexdigest(),
                "match": payload == expected,
                "path": name,
            })
    finally:
        try:
            if port.baudrate == 2000000:
                send(port, "BAUD 921600")
                time.sleep(.2)
                port.baudrate = 921600
                hello(port, 921600)
        finally:
            port.close()
    return {"identity": before, "results": results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--anchor", default="COM4")
    parser.add_argument("--role", choices=("all", "anchor", "tag"), default="all")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--count", type=int, default=12, choices=range(1, 33))
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    expected = pattern(args.count)
    receipt = {
        "utc": datetime.now(timezone.utc).isoformat(),
        "baud": 2_000_000,
        "repetitions": args.count,
        "expected_sha256": sha256(expected).hexdigest(),
    }
    roles_to_test = ("anchor", "tag") if args.role == "all" else (args.role,)
    with ThreadPoolExecutor(max_workers=len(roles_to_test)) as pool:
        roles = {role: pool.submit(test_role, role, args, expected) for role in roles_to_test}
        for role, task in roles.items():
            try:
                receipt[role] = task.result()
            except Exception as error:
                receipt[role] = {"error": str(error)}
    receipt["production_profile_pass"] = all(
        any(row["burst_bytes"] == 96 and row["idle_us"] == 160 and row["match"]
            for row in receipt[role].get("results", []))
        for role in roles
    )
    data = json.dumps(receipt, indent=2)
    (args.out / "uart_test_receipt.json").write_text(data, encoding="utf-8")
    print(data)
    return 0 if receipt["production_profile_pass"] else 2


if __name__ == "__main__":
    raise SystemExit(main())

