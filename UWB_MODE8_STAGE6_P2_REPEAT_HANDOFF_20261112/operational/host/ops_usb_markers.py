"""Bounded Pi-side USB clock samples for Mode 8, using existing serial capture."""
from collections import Counter
import json
import os
from pathlib import Path
import secrets
from threading import Lock
import time


SAMPLES_PER_RADIO = 6
MAX_DURING_MARKERS = 24
TIMEOUT_NS = 500_000_000


def cross_stamp():
    """Bracket CLOCK_REALTIME with CLOCK_MONOTONIC on the same Pi."""
    before = time.monotonic_ns()
    realtime = time.time_ns()
    after = time.monotonic_ns()
    return {"mono_before_ns": before, "realtime_ns": realtime, "mono_after_ns": after}


def uint(value, bits):
    return type(value) is int and 0 <= value < 1 << bits


def phase_receipt(path, row):
    tmp = path.with_suffix(".tmp")
    with tmp.open("x", encoding="utf-8") as out:
        json.dump(row, out, indent=2)
        out.write("\n")
    os.replace(tmp, path)


class SyncSampler:
    """One outstanding marker, with validated responses timestamped by the reader.

    pump() and control() run on the Pi bridge's control thread. observe() runs
    immediately after RFB8 decoding. tag_persisted() runs on the durable writer.
    Marker logging must stay out of both the incoming-data and ACK write paths.
    """
    def __init__(self, directory):
        self.directory = Path(directory)
        self.out = (self.directory / "sync_markers.jsonl").open("x", encoding="utf-8")
        self.lock = Lock()
        self.markers = set()
        self.pending = None
        self.completed = []
        self.jobs = {}
        self.job = None
        self.last_persisted = None

    def control(self, command):
        parts = command.split()
        if len(parts) < 2 or parts[0] != "\0SYNC":
            raise ValueError("Invalid Pi clock sync control")
        phase = parts[1].lower()
        if phase not in {"idle", "before", "during", "after"}:
            raise ValueError(f"Unknown marker phase {phase}")
        if len(parts) != (4 if phase == "idle" else 5):
            raise ValueError(f"Incorrect identity for marker phase {phase}")
        boot, session = map(int, parts[2:4])
        rf_session = int(parts[4]) if len(parts) == 5 else None
        if not uint(boot, 32) or not boot or not uint(session, 32):
            raise ValueError("Invalid board identity for marker request")
        if rf_session is not None and (not uint(rf_session, 32) or not rf_session):
            raise ValueError("Invalid paired RF session for marker request")
        if phase in {"during", "after"} and rf_session != session:
            raise ValueError("Invalid during/after marker session")
        with self.lock:
            if phase in self.jobs:
                raise ValueError(f"Marker phase {phase} was already requested")
            if self.job and self.job["phase"] != "during":
                raise ValueError("Current marker phase must finish before changing phases")
            if self.job and phase != "after":
                raise ValueError("Only after-markers may follow active markers")
            if self.job:
                self._finish_pending_locked("cancelled")
                self._finish_job_locked()
            self.job = {
                "phase": phase, "boot": boot, "session": session, "rf_session": rf_session,
                "requested_mono_ns": time.monotonic_ns(), "starts": 0, "outcomes": [],
                "last_trigger_txn": -1,
            }
            self.jobs[phase] = self.job

    def should_coalesce(self):
        with self.lock:
            return self.pending is None

    def tag_persisted(self, meta):
        with self.lock:
            self.last_persisted = (meta.get("boot"), meta.get("session"), meta.get("txn"))

    def _finish_pending_locked(self, status, event=None, sequence=None):
        if self.pending is None:
            return
        row = self.pending
        self.pending = None
        if event is not None:
            row["event"] = event
            row["sequence"] = sequence
            row["recv_mono_ns"] = time.monotonic_ns()
            row["recv_clock"] = cross_stamp()
            if status == "ok" and row["recv_mono_ns"] - row["send_mono_ns"] >= TIMEOUT_NS:
                status = "late"
            for key in ("sys_time", "esp_before_us", "esp_after_us", "txn", "active"):
                if key in event:
                    row[key] = event[key]
        row["status"] = status
        self.job["outcomes"].append((row["radio"], status))
        self.completed.append(row)

    def observe(self, packet):
        if packet.kind != 1:
            return
        event = packet.meta
        if event.get("event") not in {"SYNC_MARK", "SYNC_MARK_REJECT"}:
            return
        with self.lock:
            if self.pending is None or event.get("marker") != self.pending["marker"]:
                self.completed.append({
                    "status": "unmatched", "event": event, "sequence": packet.sequence,
                    "recv_mono_ns": time.monotonic_ns(), "recv_clock": cross_stamp(),
                })
                return
            row = self.pending
            identity = all(
                type(event.get(key)) is int and event[key] == row[key]
                for key in ("marker", "radio", "boot", "session")
            ) and event.get("role") == "tag"
            if not identity:
                self._finish_pending_locked("invalid", event, packet.sequence)
                return
            if event["event"] == "SYNC_MARK_REJECT":
                self._finish_pending_locked("rejected", event, packet.sequence)
                return
            good_clock = (
                uint(event.get("sys_time"), 40)
                and uint(event.get("esp_before_us"), 64)
                and uint(event.get("esp_after_us"), 64)
                and event["esp_before_us"] <= event["esp_after_us"]
            )
            proper_gap = uint(event.get("txn"), 32) and event.get("active") is (row["phase"] == "during")
            if proper_gap and row["trigger_txn"] is not None:
                proper_gap = event["txn"] > row["trigger_txn"]
            self._finish_pending_locked(
                "ok" if good_clock and proper_gap else "invalid",
                event,
                packet.sequence,
            )

    def _finish_job_locked(self):
        job = self.job
        if job is None:
            return
        if "ended_mono_ns" not in job:
            job["ended_mono_ns"] = time.monotonic_ns()
        self.job = None

    def _eligible_locked(self):
        job = self.job
        if not job or self.pending:
            return False
        phase = job["phase"]
        if phase != "during":
            return job["starts"] < 2*SAMPLES_PER_RADIO
        if job["starts"] >= MAX_DURING_MARKERS or self.last_persisted is None:
            return False
        boot, session, txn = self.last_persisted
        return (
            (boot, session) == (job["boot"], job["session"])
            and uint(txn, 32)
            and txn >= job["last_trigger_txn"] + (1 if not job["starts"] else 2)
        )

    def _send(self, link):
        with link.write_lock:
            with self.lock:
                if not self._eligible_locked():
                    return
                job = self.job
                marker = secrets.randbelow(0xffffffff)+1
                while marker in self.markers:
                    marker = secrets.randbelow(0xffffffff)+1
                self.markers.add(marker)
                radio = job["starts"] % 2
                job["starts"] += 1
                trigger = None
                if job["phase"] == "during":
                    trigger = self.last_persisted[2]
                    job["last_trigger_txn"] = trigger
                wire = f"SYNC_MARK {marker} {'AB'[radio]}\n".encode("ascii")
                stamp = cross_stamp()
                self.pending = {
                    "phase": job["phase"], "marker": marker, "radio": radio,
                    "boot": job["boot"], "session": job["session"],
                    "rf_session": job["rf_session"], "trigger_txn": trigger,
                    "request_ascii": wire.decode("ascii").rstrip("\n"),
                    "send_clock": stamp, "send_mono_ns": time.monotonic_ns(),
                }
            try:
                if link.serial.write(wire) != len(wire):
                    raise OSError("Incomplete marker command write")
            except (OSError, ValueError) as error:
                with self.lock:
                    if self.pending and self.pending["marker"] == marker:
                        self.pending["send_error"] = str(error)
                        self._finish_pending_locked("send_error")

    def _flush(self):
        with self.lock:
            rows, self.completed = self.completed, []
            jobs = [job for job in self.jobs.values() if "ended_mono_ns" in job and not job.get("published")]
            for job in jobs:
                job["published"] = True
        for row in rows:
            self.out.write(json.dumps(row) + "\n")
        if rows:
            self.out.flush()
        for job in jobs:
            results = Counter(status for _, status in job["outcomes"])
            accepted = Counter(radio for radio, status in job["outcomes"] if status == "ok")
            detail = {k: v for k, v in job.items() if k not in {"outcomes", "published"}}
            detail["outcomes"] = dict(results)
            detail["ok_by_radio"] = {str(r): accepted[r] for r in range(2)}
            phase_receipt(self.directory / f"sync_{job['phase']}.json", detail)

    def pump(self, link):
        with self.lock:
            if self.pending and time.monotonic_ns() >= self.pending["send_mono_ns"] + TIMEOUT_NS:
                self._finish_pending_locked("timeout")
            if self.job and not self.pending:
                limit = MAX_DURING_MARKERS if self.job["phase"] == "during" else 2*SAMPLES_PER_RADIO
                if self.job["starts"] >= limit:
                    self._finish_job_locked()
        self._flush()
        self._send(link)

    def close(self):
        with self.lock:
            self._finish_pending_locked("cancelled")
            self._finish_job_locked()
        self._flush()
        os.fsync(self.out.fileno())
        self.out.close()
