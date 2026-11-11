"""Copy the verified Stage 6 START-to-STOP RFB8 bytes for strict offline analysis."""
import argparse
from hashlib import sha256
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tag" / "tools"))
from rfb8 import Decoder, HEADER, MAGIC


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def frame_spans(data, packets):
    offset = 0
    for packet in packets:
        while (start := data.find(MAGIC, offset)) >= 0:
            if start + HEADER.size > len(data):
                break
            _, version, kind, hlen, seq, nj, nr, _ = HEADER.unpack_from(data, start)
            end = start + hlen + nj + nr
            if ((version, kind, hlen, seq, nr) ==
                    (1, packet.kind, HEADER.size, packet.sequence, len(packet.raw))
                    and end <= len(data) and Decoder().feed(data[start:end]) == [packet]):
                yield start, end
                offset = end
                break
            offset = start + 1
        else:
            raise ValueError(f"frame {packet.sequence} missing")
        require(start + HEADER.size <= len(data), "truncated frame locator")


def scope_stream(path, role, session, count, baud):
    data = path.read_bytes()
    full = Decoder()
    packets = full.feed(data)
    stats = full.statistics()
    require(not any(stats[key] for key in ("crc_errors", "schema_errors", "trailing_bytes")),
            f"{role} whole-file RFB8 integrity")
    measurements = [(p.meta.get("session"), p.meta.get("txn"), p.meta.get("role"))
                    for p in packets if p.kind == 2]
    require(measurements == [(session, txn, role) for txn in range(count)],
            f"{role} whole-file measurement completeness and identity")
    first = next(i for i, p in enumerate(packets) if p.kind == 2)
    last = max(i for i, p in enumerate(packets) if p.kind == 2)
    begin, = [i for i, p in enumerate(packets[:first])
              if p.kind == 1 and p.meta.get("event") == "HELLO" and p.meta.get("active") is True
              and p.meta.get("stage") == 6 and p.meta.get("role") == role and p.meta.get("baud") == baud]
    stop = next(i for i in range(last + 1, len(packets))
                if packets[i].kind == 1 and packets[i].meta.get("event") == "STOPPED"
                and packets[i].meta.get("session") == session and packets[i].meta.get("role") == role)
    gaps = []
    for i, (a, b) in enumerate(zip(packets, packets[1:])):
        if b.sequence == (a.sequence + 1) & 0xffffffff:
            continue
        require(i + 1 < begin and baud == 2000000 and (b.sequence - a.sequence) % (1 << 32) == 2
                and a.kind == b.kind == 1
                and a.meta.get("event") == b.meta.get("event") == "HELLO"
                and a.meta.get("active") is b.meta.get("active") is False
                and (a.meta.get("baud"), b.meta.get("baud")) == (921600, baud),
                f"{role} RFB8 sequence gap outside inactive baud transition")
        gaps.append([a.sequence, b.sequence])
    require(len(gaps) == stats["sequence_gap_events"] and len(gaps) <= 1,
            f"{role} whole-file sequence accounting")
    spans = list(frame_spans(data, packets))
    start, end = spans[begin][0], spans[stop][1]
    scoped = data[start:end]
    clean = Decoder()
    checked = clean.feed(scoped)
    require(checked == packets[begin:stop + 1] and not any(clean.statistics().values()),
            f"{role} START-to-STOP integrity")
    return scoped, {
        "source_sha256": sha256(data).hexdigest(), "scope_sha256": sha256(scoped).hexdigest(),
        "source_bytes": len(data), "source_byte_range": [start, end], "range_end_exclusive": True,
        "sequence_range": [checked[0].sequence, checked[-1].sequence], "measurement_count": count,
        "source_decoder": stats, "scope_decoder": clean.statistics(),
        "pre_start_baud_transition_sequence_gaps": gaps,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--baud", type=int, choices=(921600, 2000000), required=True)
    args = parser.parse_args()
    capture = args.capture.resolve()
    receipt = json.loads((capture / "capture_receipt.json").read_bytes())
    require(receipt["stage"] == 6 and receipt["pass"] is True
            and receipt["audit"]["pass"] is True and receipt["shutdown"]["pass"] is True,
            "Stage 6 collector and shutdown gates")
    session, count = receipt["session"], receipt["requested_count"]
    output = capture / "measurement_scope"
    scoped = {role: scope_stream(capture / f"{role}.rfb8", role, session, count, args.baud)
              for role in ("anchor", "tag")}
    output.mkdir()
    for role, (data, _) in scoped.items():
        (output / f"{role}.rfb8").write_bytes(data)
    result = {"session": session, "baud": args.baud, "requested_count": count,
              "streams": {role: info for role, (_, info) in scoped.items()}}
    (output / "scope_receipt.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    main()

