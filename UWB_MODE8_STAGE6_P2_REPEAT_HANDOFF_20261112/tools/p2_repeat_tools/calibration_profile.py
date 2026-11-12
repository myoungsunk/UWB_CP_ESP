"""Load an explicit calibration and hardware wiring profile."""
import hashlib
import json
import math

WIRING = {
    "anchor": "esp32-dw1000-anchor-21-22-5",
    "tag": "esp32-dw1000-tag-ab-21-22-17-25-26-27",
}


def load_profile(path):
    payload = path.read_bytes()
    profile = json.loads(payload)
    if not isinstance(profile, dict) or profile.get("schema") != "dual8.calibration.v1":
        raise ValueError("Expected dual8.calibration.v1")
    roles = profile.get("roles")
    if not isinstance(roles, dict) or set(roles) != set(WIRING):
        raise ValueError("Calibration must describe both board roles")
    for role, name in WIRING.items():
        binding = roles[role]
        radios = binding.get("radios", []) if isinstance(binding, dict) else []
        count = 1 if role == "anchor" else 2
        if not isinstance(binding, dict) or binding.get("hardware_profile") != name or len(radios) != count:
            raise ValueError(f"{role}: incorrect wiring profile or radio count")
        for index, cal in enumerate(radios):
            if not isinstance(cal, dict) or type(cal.get("radio")) is not int or cal["radio"] != index:
                raise ValueError(f"{role}: invalid calibration radio")
            for key, maximum in (("id", 0xffffffff), ("tx", 65535), ("rx", 65535)):
                if type(cal.get(key)) is not int or not 0 <= cal[key] <= maximum:
                    raise ValueError(f"{role}: invalid {key}")
            sigma = cal.get("sigma_m")
            if not (sigma is None and cal["id"] == 0) and (
                    type(sigma) not in (int, float) or not math.isfinite(sigma) or not 0 < sigma <= 100):
                raise ValueError(f"{role}: invalid sigma_m")
            quality = cal.get("quality")
            evidence = cal.get("evidence")
            if quality not in ("temporary", "verified") or (quality == "temporary") != (cal["id"] == 0):
                raise ValueError(f"{role}: temporary values require ID 0 and verified values a nonzero ID")
            if quality == "verified" and (not isinstance(evidence, str) or not evidence.strip()):
                raise ValueError(f"{role}: verified calibration requires an evidence reference")
    profile["source_sha256"] = hashlib.sha256(payload).hexdigest()
    return profile


def check_record_calibration(profile, role, records):
    expected = profile["roles"][role]["radios"]
    for record in records:
        reported = record.get("calibration", [])
        if not isinstance(reported, list) or len(reported) != len(expected):
            raise ValueError(f"{role}: missing calibration readback")
        for want, got in zip(expected, reported):
            sigma = got.get("range_sigma_m")
            sigma_ok = (sigma is None if want["sigma_m"] is None else
                        type(sigma) in (int, float) and math.isclose(sigma, want["sigma_m"], rel_tol=1e-7, abs_tol=1e-9))
            if (any(want[key] != got.get(key) for key in ("radio", "id", "tx", "rx")) or not sigma_ok):
                raise ValueError(f"{role}: calibration mismatch in transaction {record.get('txn')}")
