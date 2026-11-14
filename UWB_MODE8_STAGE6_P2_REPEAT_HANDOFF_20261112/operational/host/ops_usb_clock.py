"""Fit tag DW1000 clocks to Pi system time using validated USB round-trip bounds."""
import argparse
import json
from pathlib import Path
from ops_pose_align import AlignmentHold, fit_domain, unwrap


def _cross_bounds(stamp):
    before, after, wall = (stamp[k] for k in ("mono_before_ns", "mono_after_ns", "realtime_ns"))
    if any(type(x) is not int for x in (before, after, wall)) or not 0 <= before <= after or wall <= 0:
        raise AlignmentHold("invalid Pi monotonic/realtime bracket")
    return wall - after, wall - before


def fit_usb_markers(rows, boot, session, *, max_bound_ns=2_000_000, max_ppm=200.0):
    """Return A/B models only within each measured span and stated USB bound."""
    if not 0 < max_bound_ns <= 2_000_000 or not 0 < max_ppm <= 200:
        raise AlignmentHold("invalid USB acceptance thresholds")
    selected = {0: [], 1: []}
    seen = set()
    offsets = []
    for row in rows:
        if row.get("status") != "ok":
            continue
        event = row["event"]
        for field in ("marker", "radio", "boot", "session", "sys_time", "esp_before_us", "esp_after_us"):
            if type(row.get(field)) is not int or type(event.get(field)) is not int or row[field] != event[field]:
                raise AlignmentHold(f"USB response mismatch for {field}")
        if (row["marker"] in seen or not 0 < row["marker"] < 1 << 32 or row["boot"] != boot or
                row["rf_session"] != session or row["radio"] not in selected or
                event.get("role") != "tag" or event.get("event") != "SYNC_MARK"):
            raise AlignmentHold("invalid USB marker identity")
        phase = row["phase"]
        if (phase not in ("before", "during", "after") or event.get("active") is not (phase == "during")
                or phase in ("during", "after") and row["session"] != session):
            raise AlignmentHold("USB marker does not match capture phase")
        seen.add(row["marker"])
        send, recv = row["send_mono_ns"], row["recv_mono_ns"]
        start, end = _cross_bounds(row["send_clock"]), _cross_bounds(row["recv_clock"])
        if (type(send) is not int or type(recv) is not int or
                not row["send_clock"]["mono_after_ns"] <= send < recv <= row["recv_clock"]["mono_before_ns"] or
                not 0 <= row["esp_before_us"] <= row["esp_after_us"] or
                (row["esp_after_us"] - row["esp_before_us"]) * 1000 > recv - send or
                not 0 <= row["sys_time"] < 1 << 40 or row["sys_time"] & 511):
            raise AlignmentHold("inconsistent USB or DW1000 clock bracket")
        offsets.extend((start, end))
        local_span = row["recv_clock"]["mono_after_ns"] - row["send_clock"]["mono_before_ns"]
        local_drift = (local_span + 1999) // 2000
        lo = max(start[0], end[0]) - local_drift
        hi = min(start[1], end[1]) + local_drift
        if lo > hi:
            raise AlignmentHold("Pi realtime offset changed within a USB sample")
        selected[row["radio"]].append((send, recv, row["sys_time"], phase, lo, hi))
    if not offsets:
        raise AlignmentHold("no validated USB markers")
    # Allow clock discipline of at most 500 ppm relative to the first stamp.
    sends = [send for samples in selected.values() for send, *_ in samples]
    receives = [recv for samples in selected.values() for _, recv, *_ in samples]
    drift = (max(receives) - min(sends) + 1999) // 2000
    offset_lo = max(lo for lo, _ in offsets) - drift
    offset_hi = min(hi for _, hi in offsets) + drift
    if offset_lo > offset_hi:
        raise AlignmentHold("Pi realtime offset changed beyond allowed clock discipline")
    models = {}
    for radio, points in selected.items():
        ordered = sorted(points)
        if {phase for _, _, _, phase, _, _ in ordered} != {"before", "during", "after"}:
            raise AlignmentHold(f"missing bracket phase on tag {'AB'[radio]}")
        bounds = [(send+lo, recv+hi, ticks) for send, recv, ticks, _, lo, hi in ordered]
        radius = max((hi-lo+1)//2 for lo, hi, _ in bounds)
        if radius > max_bound_ns:
            raise AlignmentHold(f"USB asymmetry bound exceeds {max_bound_ns} ns on tag {'AB'[radio]}")
        sync = [{"radio_ticks": ticks, "ros_ns": (lo+hi)//2} for lo, hi, ticks in bounds]
        model = fit_domain(sync, max_bound_ns, max_ppm)
        # Every measured causal interval must contain its modeled timestamp.
        for x, (lo, hi, _) in zip(unwrap([ticks for _, _, ticks in bounds]), bounds):
            prediction = model["mean_ns"] + model["slope_ns_per_tick"]*(x-model["mean_x"])
            if not lo <= prediction <= hi:
                raise AlignmentHold(f"USB model outside measured interval on tag {'AB'[radio]}")
        models[f"tag{'AB'[radio]}"] = model | {"max_sample_bound_ns": radius}
    return models


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("markers", type=Path)
    cli.add_argument("--boot", required=True, type=int)
    cli.add_argument("--session", required=True, type=int)
    args = cli.parse_args()
    try:
        with args.markers.open(encoding="utf-8") as source:
            models = fit_usb_markers((json.loads(line) for line in source), args.boot, args.session)
    except (OSError, KeyError, TypeError, ValueError) as error:
        cli.error(f"TIMESYNC_HOLD: {error}")
    print(json.dumps({"status": "USB_CLOCK_MODEL_VALIDATED", "models": models,
                      "ros_system_clock_required": True, "anchor_usb_clock_sampled": False}, indent=2))


if __name__ == "__main__":
    main()
