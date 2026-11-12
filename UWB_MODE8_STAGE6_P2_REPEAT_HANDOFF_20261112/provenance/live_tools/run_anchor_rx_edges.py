"""Collect anchor RX on/off samples with the pinned Stage 6 diagnostic runner."""

import argparse
from importlib import import_module
import json
from pathlib import Path
import sys
import time

import run_anchor_diag as base


FIRMWARE = "D8_ANCHOR_STAGE6_RX_EDGES_20261112"
SUMMARY = "ANCHOR_RX_EDGES"
EDGE = "ANCHOR_RX_EDGE"


class EdgesLink(base.CurrentStatusLink):
    queries = []
    ACK_RETRY_INTERVAL_S = 0.08
    ACK_RETRY_LIMIT = 2

    def __init__(self, name, device, directory):
        self.ack_retries = {}
        self.acks_sent = 0
        self.ack_retries_sent = 0
        super().__init__(name, device, directory)
        self.source = directory.name
        self.boot = None
        self.idle = False
        self.started = False
        self.stopped = False
        self.queried = False

    def send(self, command):
        result = super().send(command)
        if command.startswith("START "):
            self.started = True
        if command.startswith("REC_ACK ") and self.replay_identity:
            # The collector only sends this after identity, CIR CRC and fsync.
            self.acks_sent += 1
            self.ack_retries[command] = (time.monotonic() + self.ACK_RETRY_INTERVAL_S, 0)
        return result

    def begin_capture(self, boot=None, session=None):
        with self.condition:
            self.ack_retries.clear()
            self.acks_sent = self.ack_retries_sent = 0
            super().begin_capture(boot, session)

    def disable_replay(self):
        with self.condition:
            self.ack_retries.clear()
            super().disable_replay()

    def filter_wire(self, data):
        restored = super().filter_wire(data)
        if self.replay_identity:
            now = time.monotonic()
            for command, (due, retries) in list(self.ack_retries.items()):
                if now < due:
                    continue
                # ACKs are idempotent at the writer. Resend even on a quiet link
                # so a missing ACK can release a slot after all slots fill.
                super().send(command)
                self.ack_retries_sent += 1
                if retries + 1 == self.ACK_RETRY_LIMIT:
                    del self.ack_retries[command]
                else:
                    self.ack_retries[command] = (now + self.ACK_RETRY_INTERVAL_S, retries + 1)
        return restored

    def stats(self):
        with self.condition:
            stats = super().stats()
            stats["transport"]["durable_acks_sent"] = self.acks_sent
            stats["transport"]["durable_ack_retries_sent"] = self.ack_retries_sent
            return stats

    def wait(self, *args, **kwargs):
        row = super().wait(*args, **kwargs)
        event = row.get("event")
        if event == "HELLO":
            self.boot = row.get("boot")
            self.idle = row.get("active") is False
        if event == "STOPPED" and self.started and row.get("stop_request"):
            self.stopped = True
        if self.name == "anchor" and self.idle and self.boot is not None and not self.queried:
            if (self.source == "usb" and event == "SELFTEST") or (self.stopped and event == "HELLO"):
                self.read_cached_edges()
        return row

    def read_cached_edges(self):
        self.queried = True
        outcome = {"source": self.source, "boot": self.boot, "received_indices": []}
        self.queries.append(outcome)
        try:
            # Previous responses are already persisted by the reader. Match fresh query responses here.
            with self.condition:
                for row in list(self.events):
                    if row.get("event") in {SUMMARY, EDGE}:
                        self.events.remove(row)
                self.send("RXEDGES")
            summary = outcome["summary"] = super().wait(
                lambda row: row.get("event") == SUMMARY and row.get("boot") == self.boot,
                timeout=3, check_faults=False)
            count = summary.get("count")
            if type(count) is not int or not 0 <= count <= 6:
                raise ValueError(f"Invalid RXEDGES sample count: {count!r}")

            remaining = set(range(count))
            def matches(entry):
                return (entry.get("event") == EDGE and entry.get("boot") == self.boot
                        and entry.get("scope") == summary.get("scope")
                        and entry.get("session") == summary.get("session"))

            deadline = time.monotonic() + 3
            try:
                while remaining:
                    row = super().wait(lambda entry: matches(entry) and entry.get("index") in remaining,
                                       timeout=max(0.0, deadline - time.monotonic()), check_faults=False)
                    remaining.remove(row["index"])
                    outcome["received_indices"].append(row["index"])
            except TimeoutError:
                outcome["indexed_queries"] = sorted(remaining)
                for index in outcome["indexed_queries"]:
                    self.send(f"RXEDGES {index}")
                    try:
                        super().wait(lambda entry: matches(entry) and entry.get("index") == index,
                                     timeout=1, check_faults=False)
                    except TimeoutError:
                        continue
                    remaining.remove(index)
                    outcome["received_indices"].append(index)
            outcome["missing_indices"] = sorted(remaining)
        except (OSError, RuntimeError, ValueError) as exc:
            outcome["error"] = str(exc)


def edge_events(output):
    for source in ("usb", "capture"):
        path = output / source / "anchor.rfb8"
        if path.is_file():
            for packet in base.live.run_pair.read_file(path):
                if packet.kind == 1 and packet.meta.get("event") in {SUMMARY, EDGE}:
                    yield {"source": source, "sequence": packet.sequence, "event": packet.meta}


def pin_analysis_tools(tools=None):
    tools = (tools or base.RELIABILITY / "tag" / "tools").resolve()
    # The Pi serial bridge adds the legacy R5 tools directory during import.
    # The Stage 6 collector loads its analyzer lazily when building the grids.
    sys.path.insert(0, str(tools))
    inputs = []
    for name in ("run_pair", "rfb8", "capture_audit", "analyze", "uart_fragments", "calibration_profile"):
        expected = tools / (name + ".py")
        actual = Path(import_module(name).__file__).resolve()
        if actual != expected:
            raise RuntimeError(f"Expected Stage 6 {name} from {expected}, got {actual}")
        inputs.append(actual)
    return inputs


def main(*, anchor_root=base.HERE / "anchor_rx_edges", anchor_firmware=FIRMWARE,
         analysis_root=None):
    analysis_inputs = pin_analysis_tools(analysis_root)
    # The previous runner still owns the CLI, deployment checks, capture and STOP.
    base.ANCHOR_ROOT = anchor_root
    base.FIRMWARE = {**base.FIRMWARE, "anchor": anchor_firmware}
    base.CurrentStatusLink = EdgesLink
    EdgesLink.queries.clear()
    result = base.main()

    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--out", type=Path, required=True)
    args, _ = parser.parse_known_args()
    output = args.out.resolve()
    destination = output / "anchor_rx_edges.json"
    events = list(edge_events(output))
    base.write_json(destination, {"events": events, "queries": EdgesLink.queries})
    execution = output / "execution.json"
    record = json.loads(execution.read_bytes())
    wrapper = Path(__file__).resolve()
    for path in [wrapper, *analysis_inputs]:
        record["input_sha256"][str(path)] = base.live.sha(path)
    record["rx_edges"] = {"path": str(destination), "sha256": base.live.sha(destination),
                          "event_count": len(events)}
    base.write_json(execution, record)
    print(json.dumps(record["rx_edges"]), flush=True)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
