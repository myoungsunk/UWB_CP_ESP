"""Exercise two Mode 8 audit counterexamples without connecting a serial port."""

import copy
import json
from pathlib import Path
from queue import Empty, Queue
import sys
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest.mock import patch
import zlib


PACKAGE = Path(__file__).parent / "UWB_DUAL_DW1000_MODE8_20261111"
sys.path.insert(0, str(PACKAGE / "tools"))

from analyze import air_frame, packet_arrays
from rfb8 import encode, RAW_BYTES
import run_pair


def reproduce_p2_conflict():
    evidence = PACKAGE / "evidence" / "host"
    meta = json.loads((evidence / "synthetic_tag_record.json").read_bytes())
    raw = (evidence / "SYNTHETIC_CIR_i16.raw").read_bytes()
    accepted_p2 = air_frame(meta["frames"][2])
    conflicting_p2 = bytearray.fromhex(meta["frames"][2])
    # Change B's reported anchor RESPONSE RX by 256 DTU while keeping the
    # transaction IDs, P2 TX timestamp and a valid over-air CRC.
    altered_b_response = accepted_p2["resp_rx"][1] + 256
    conflicting_p2[42:47] = altered_b_response.to_bytes(5, "little")
    conflicting_p2[92:96] = zlib.crc32(conflicting_p2[:92]).to_bytes(4, "little")
    meta["probe_rx"][3]["air"] = conflicting_p2.hex()
    assert air_frame(meta["probe_rx"][3]["air"])["resp_rx"][1] == altered_b_response
    # These are the flags that waitPair and finalizeRanges emit on conflict.
    meta.update(metadata_conflict=True, p1_tx_confirmed=False, complete=False, errors=64)
    for result in meta["ranges"]:
        result.update(math_valid=False, d_m=None, six_ts=[0] * 6)

    anchor = {
        "session": meta["session"], "txn": meta["txn"], "config": meta["config"],
        "boot": meta["peer_boot"], "peer_boot": meta["boot"],
        "tx": [{}, {}, {"ok": True, "actual": accepted_p2["probe_tx"]}],
        "calibration": [{"id": 0}],
    }
    _, rows = packet_arrays(meta, raw, anchor)
    evidence = {
        "metadata_conflict": meta["metadata_conflict"],
        "p2_b_frame_matches_shared_frame": meta["probe_rx"][3]["air"] == meta["frames"][2],
        "p2_b_axis_estimate_available": rows[3]["axis_estimate_available"],
        "p2_b_range_m": rows[3]["range_m"],
        "p2_b_method": rows[3]["axis_method"],
    }
    assert evidence["p2_b_axis_estimate_available"], evidence
    return evidence


class InMemorySerial:
    def __init__(self, **_kwargs):
        self.port = None
        self.incoming = Queue()
        self.sequence = 0
        self.stage = 5
        self.period = 300000

    @property
    def in_waiting(self):
        return 65536

    def open(self):
        pass

    def close(self):
        pass

    def flush(self):
        pass

    def read(self, _size):
        try:
            return self.incoming.get(timeout=0.02)
        except Empty:
            return b""

    def push(self, meta, *, kind=1, raw=b""):
        self.incoming.put(encode(meta, raw, kind=kind, sequence=self.sequence))
        self.sequence += 1

    def hello(self, *, active=False):
        self.push({
            "event": "HELLO", "role": self.port, "hw_ok": True,
            "boot": 101 if self.port == "anchor" else 202, "config_id": 42,
            "active": active, "stage": self.stage, "period_us": self.period,
        })

    def write(self, data):
        tokens = data.decode("ascii").split()
        match tokens:
            case ["STOP"]:
                self.push({"event": "STOPPED", "role": self.port})
            case ["HELLO"]:
                self.hello()
            case ["SELFTEST"]:
                self.push({"event": "SELFTEST", "role": self.port, "pass": True})
            case ["SET", "STAGE", stage]:
                self.stage = int(stage)
                self.hello()
            case ["SET", "PERIOD_US", period]:
                self.period = int(period)
                self.hello()
            case ["START", "77", "2"]:
                self.hello(active=True)
                raw = bytes(RAW_BYTES) if self.port == "tag" else b""
                record = {
                    "schema": "dual8.record.v1", "mode": 8, "role": self.port,
                    "session": 76, "txn": 0, "boot": 999, "config": 42,
                    "complete": True,
                }
                self.push(record, kind=2, raw=raw)
                self.sequence += 1
                self.push(copy.copy(record), kind=2, raw=raw)
            case _:
                raise AssertionError(tokens)
        return len(data)


def reproduce_capture_false_pass():
    with TemporaryDirectory(prefix="mode8-audit-") as tmp:
        argv = ["run_pair.py", "--anchor", "anchor", "--tag", "tag",
                "--out", tmp, "--count", "2"]
        with (patch.object(sys, "argv", argv),
              patch.dict(sys.modules, {"serial": SimpleNamespace(Serial=InMemorySerial)}),
              patch.object(run_pair.secrets, "randbelow", return_value=76),
              patch("builtins.print")):
            exit_code = run_pair.main()
        result = json.loads((Path(tmp) / "capture_receipt.json").read_bytes())
    evidence = {
        "exit_code": exit_code, "reported_pass": result["pass"],
        "requested_session": result["session"], "injected_session": 76,
        "injected_txns_per_role": [0, 0], "injected_boot": 999,
        "anchor_sequence_gap_events": result["anchor_capture"]["decoder"]["sequence_gap_events"],
        "tag_sequence_gap_events": result["tag_capture"]["decoder"]["sequence_gap_events"],
    }
    assert exit_code == 0 and result["pass"], evidence
    assert evidence["anchor_sequence_gap_events"] and evidence["tag_sequence_gap_events"], evidence
    return evidence


if __name__ == "__main__":
    print(json.dumps({"p2_conflict": reproduce_p2_conflict(),
                      "capture_false_pass": reproduce_capture_false_pass()}, indent=2))
