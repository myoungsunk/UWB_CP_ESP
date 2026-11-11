"""Verify each available Stage 6 grid, preserving losses in the 1,000-run denominator."""
from hashlib import sha256
from pathlib import Path
import json
import sys

import numpy as np

capture = Path(__file__).resolve().parent
evidence = capture.parent
sys.path.insert(0, str(evidence))
from verify_stage6 import check_probe, MOD, TICKS_PER_SECOND
from analyze import air_frame, packet_arrays, same_identity
from rfb8 import Decoder, read_file
from uart_fragments import FragmentDecoder, parity_count


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def ref(path):
    return {"path": path.as_posix(), "sha256": sha256(path.read_bytes()).hexdigest()}


class InspectFragments(FragmentDecoder):
    def __init__(self):
        super().__init__()
        self.failures = []

    def _finalize(self):
        frame = self.frame
        packet = super()._finalize()
        if not packet:
            missing_data = sorted(set(range(frame.data_count)) - frame.parts.keys())
            parity = sorted(i for i in frame.parts if i >= frame.data_count)
            self.failures.append({
                "sequence": frame.sequence, "data_required": frame.data_count,
                "missing_data_indices": missing_data, "valid_parity_indices": parity,
                "parity_sent": parity_count(frame.data_count),
                "insufficient_valid_fragments": len(missing_data) > len(parity),
            })
        return packet


def replay_wire(path):
    wire, decoder = InspectFragments(), Decoder()
    packets = []
    with path.open("rb") as raw:
        while chunk := raw.read(65536):
            packets.extend(p for p in decoder.feed(wire.feed(chunk)) if p.kind == 2)
    return wire, decoder.statistics(), packets


def csv_rows(rows):
    return {(row["txn"], row["slot"]): {key: "" if val is None else str(val)
            for key, val in row.items()} for row in rows}


def has_grid(arrays, probe):
    grid = arrays[f"common_p{probe}_delay_s"]
    power = arrays[f"common_p{probe}_power_norm"]
    return bool(grid.size and power.shape == (2, grid.size)
                and np.isfinite(grid).all() and np.isfinite(power).all(axis=0).any())


def main():
    receipt = read_json(capture / "capture_receipt.json")
    manager = read_json(capture / "collector_process_receipt.json")
    count, session = receipt["requested_count"], receipt["session"]
    assert count == 1000 and receipt["stage"] == 6
    records, endpoints, references = {}, {}, {}
    assert receipt["shutdown"]["pass"] is True
    for role in ("anchor", "tag"):
        rfb8 = capture / f"{role}.rfb8"
        uart = capture / f"{role}.uart"
        status_file = capture / f"{role}_post_status.json"
        restore_file = capture / f"{role}_baud_restored.json"
        decoder = Decoder()
        all_packets = list(read_file(rfb8, decoder))
        stats = receipt[f"{role}_capture"]
        identity = receipt[f"{role}_identity"]
        status = read_json(status_file)
        restore = read_json(restore_file)
        assert status["boot"] == identity["boot"] == restore["boot"]
        assert identity["baud"] == status["baud"] == 2000000
        assert status["period_us"] == 200000 and status["active"] is False
        assert restore["baud"] == 921600 and restore["active"] is False
        assert any(p.kind == 1 and p.meta.get("event") == "SELFTEST" and p.meta.get("pass") is True
                   and p.meta.get("dev_a") == 0xDECA0130
                   and (role == "anchor" or p.meta.get("dev_b") == 0xDECA0130)
                   for p in all_packets)
        kept = [p for p in all_packets if p.kind == 2]
        txns = [p.meta["txn"] for p in kept]
        assert txns == sorted(set(txns)) and set(txns) <= set(range(count))
        assert all(p.meta["session"] == session and p.meta["role"] == role
                   and p.meta["boot"] == identity["boot"] and p.meta["stage"] == 6
                   and p.meta["firmware"] == identity["firmware"] and p.meta["period_us"] == 200000
                   and p.meta["peer_boot"] == receipt[f'{"tag" if role=="anchor" else "anchor"}_identity']["boot"]
                   for p in kept)
        assert len(kept) == stats["records"]
        wire, wire_rfb8_stats, replayed = replay_wire(uart)
        assert replayed == kept
        assert wire.counts["unrepairable_records"] == stats["uart_fragment_transport"]["unrepairable_records"]
        records[role] = {p.meta["txn"]: p for p in kept}
        sequences = {p.sequence for p in kept}
        missing = sorted(set(range(count)) - set(txns))
        endpoints[role] = {
            "rfb8_records": len(kept), "rfb8_missing": len(missing), "missing_txns": missing,
            "complete_records": sum(p.meta["complete"] for p in kept),
            "incomplete_txns": [p.meta["txn"] for p in kept if not p.meta["complete"]],
            "sent_delta": status["sent"] - identity["sent"], "drops_delta": status["drops"] - identity["drops"],
            "period_miss": status["period_miss"], "writer_max_us": status["writer_max_us"],
            "fault_events": stats["fault_events"], "capture_rfb8_decoder": stats["capture_decoder"],
            "full_rfb8_decoder": decoder.statistics(), "wire_replay_rfb8_decoder": wire_rfb8_stats,
            "outer_usb_fragment_crc_errors": stats["uart_fragment_transport"]["fragment_crc_errors"],
            "unrecoverable_rfb8_records": stats["uart_fragment_transport"]["unrepairable_records"],
            "repaired_measurement_records": sum(r["sequence"] in sequences for r in wire.repaired),
            "unrecoverable_fragments": wire.failures,
            "sequence_gaps": [[a.sequence,b.sequence,a.meta.get("txn"),b.meta.get("txn")]
                              for a,b in zip(all_packets,all_packets[1:])
                              if b.sequence != ((a.sequence+1) & 0xffffffff)],
        }
        references[role] = [ref(p) for p in (rfb8,uart,status_file,restore_file)]

    trials, ticks = [], []
    for txn in range(count):
        a, t = records["anchor"].get(txn), records["tag"].get(txn)
        row = {"txn": txn, "anchor_available": a is not None, "tag_available": t is not None}
        if t is None:
            trials.append(row)
            ticks.append(a.meta["initial_poll_tx"] if a else None)
            continue
        m = t.meta
        poll = air_frame(m["frames"][0])
        if a:
            tick = a.meta["initial_poll_tx"]
            assert tick == m["initial_poll_tx"]
        elif same_identity(poll, m, peer=True) and poll["type"] == 1 and poll["tx_ok_mask"] == 0:
            tick = poll["poll_tx"]
            assert tick == m["initial_poll_tx"]
        else:
            tick = None
        ticks.append(tick)
        row.update(tag_complete=m["complete"], p1_retry=m["p1_retry"], p2_retry=m["p2_retry"],
                   tag_errors=m["errors"],
                   invalid_cir_slots=[i for i, slot in enumerate(m["cir"]) if not slot["valid"]])
        try:
            arrays, observations = packet_arrays(m, t.raw, a.meta if a else None)
            unpaired, unpaired_rows = packet_arrays(m, t.raw)
        except (ValueError, KeyError) as error:
            row["analysis_error"] = str(error)
            trials.append(row)
            continue
        for probe in (1,2):
            has = has_grid(arrays, probe)
            details = {"axis_and_power_generated": has, "both_endpoints_verified": False}
            if has and a:
                try:
                    result = check_probe(a.meta, m, t.raw, probe, arrays, unpaired,
                                         csv_rows(observations), csv_rows(unpaired_rows))
                    details["both_endpoints_verified"] = result["pass"]
                    details["check"] = result
                except (ValueError, KeyError, AssertionError) as error:
                    details["verification_error"] = str(error)[:500]
            row[f"P{probe}"] = details
        trials.append(row)

    intervals = [((b-a) % MOD) * 1e6 / TICKS_PER_SECOND for a,b in zip(ticks,ticks[1:])
                 if a is not None and b is not None]
    summary = {}
    for probe in ("P1","P2"):
        summary[probe] = {
            "axis_and_power_generated":sum(x.get(probe,{}).get("axis_and_power_generated",False) for x in trials),
            "both_endpoints_verified":sum(x.get(probe,{}).get("both_endpoints_verified",False) for x in trials),
            "not_generated_txns":[x["txn"] for x in trials if not x.get(probe,{}).get("axis_and_power_generated")],
            "generated_without_both_endpoint_verification":[x["txn"] for x in trials
               if x.get(probe,{}).get("axis_and_power_generated") and not x[probe]["both_endpoints_verified"]],
        }
    result = {
        "question_id":"MODE8-STAGE6-200MS-2MBPS-1000","session":session,"requested_count":count,
        "period_us":200000,"baud":2000000,"pass":False,"collector_pass":receipt["pass"],
        "collector_exit_code":manager["processes"][0]["exit_code"],"collector_error":receipt["error"],
        "stop_confirmed":receipt["shutdown"]["pass"],"grids":summary,
        "retry_txns":{p:[x["txn"] for x in trials if x.get(p.lower()+"_retry")] for p in ("P1","P2")},
        "cadence":{"interval_count":len(intervals),"min_us":min(intervals),"max_us":max(intervals),
                   "max_abs_error_us":max(abs(i-200000) for i in intervals),
                   "source":"Anchor initial POLL timestamps, with a tag RF-validated POLL for a missing anchor record."},
        "endpoints":endpoints,
        "input_hashes":{"streams":references,"capture":ref(capture/"capture_receipt.json"),
             "contract":ref(Path(str(capture)+".contract.json")), "script":ref(Path(__file__)),
             "pair_checker":ref(evidence/"verify_stage6.py"), "process":ref(capture/"collector_process_receipt.json")},
        "transactions":trials,
    }
    with (capture/"stage6_1000_verdict.json").open("x",encoding="utf-8") as file:
        json.dump(result,file,indent=2,allow_nan=False)
    print(json.dumps({k:v for k,v in result.items() if k not in ("transactions","input_hashes")},indent=2))


if __name__ == "__main__":
    main()

