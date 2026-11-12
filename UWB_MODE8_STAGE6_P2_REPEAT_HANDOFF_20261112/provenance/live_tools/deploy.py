from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

OUT = Path(__file__).resolve().parent
PACKAGE = OUT.parents[1]
ARTIFACTS = PACKAGE.parent
WORKSPACE = ARTIFACTS.parent
PI = "slam-robot-tailscale"
TAG_PORT = "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"
SSH_ARGS = ["-F", str(WORKSPACE / ".codex-robot-ssh-config"),
            "-o", "BatchMode=yes", "-o", "ConnectTimeout=8"]
PRIOR = {
    "anchor": ("UWB_MODE8_STAGE6_CONTINUOUS_RX_FEC_RFRETRY_20261111",
               "b4c0e0eeeae965cea145b5935c8813b502e40977cf6f700a11f5a8fa13c60814",
               "28:56:2f:72:e7:18"),
    "tag": ("UWB_MODE8_STAGE6_CONTINUOUS_RX_FEC_PACKED_20261111",
            "dba9fb10a6fda80ed16d1057b66d280ea313a66d9171b1e811087b3a49aa2991",
            "28:56:2f:73:06:54"),
}
PORTS = {"anchor": "COM4", "tag": TAG_PORT}


def write_json(name, value):
    (OUT / name).write_text(json.dumps(value, indent=2), encoding="utf-8")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def invoke(command, log_name, expected_mac=None):
    print(f"{log_name} started", flush=True)
    with (OUT / log_name).open("xb") as log:
        rc = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=90).returncode
    contents = (OUT / log_name).read_text(encoding="utf-8", errors="replace").lower()
    if rc or expected_mac and expected_mac not in contents:
        raise RuntimeError(f"{log_name}: exit {rc}, verify chip identity and result")
    print(f"{log_name} passed", flush=True)


def flash():
    sys.path.insert(0, str(PACKAGE / "tag" / "tools"))
    from offline_preflight import scan
    result = scan(anchor_root=PACKAGE / "anchor", tag_root=PACKAGE / "tag")
    write_json("offline_preflight.json", result)
    if not result["flash_preflight_pass"]:
        raise RuntimeError("Build preflight failed")

    remote = "/tmp/uwb-mode8-reliability-" + OUT.name.removeprefix("stage6_live_").replace("_", "-")
    state = {"started_utc": datetime.now(timezone.utc).isoformat(),
             "remote_staging": remote, "offset": "0x10000", "roles": {}}
    images = {}
    try:
        for role, (baseline, digest, mac) in PRIOR.items():
            filename = role.upper() + "_DUAL_DS_CIR.ino.bin"
            old = ARTIFACTS / baseline / role / "dist" / role / filename
            new = PACKAGE / role / "dist" / role / filename
            if sha(old) != digest:
                raise RuntimeError(f"{role} prior app hash mismatch")
            backup = OUT / (role + "_verified_prior_app.bin")
            with backup.open("xb") as out:
                out.write(old.read_bytes())
            receipt = PACKAGE / role / "dist" / "target_build_receipt.json"
            build = json.loads(receipt.read_bytes())["roles"][role]
            state["roles"][role] = {
                "port": PORTS[role], "mac": mac, "prior_app_sha256": digest, "new_app_sha256": sha(new),
                "build_receipt_sha256": sha(receipt), "source_sha256": build["source_manifest"]["sha256"],
            }
            if sha(backup) != digest or sha(new) != build["artifacts"][filename]["sha256"]:
                raise RuntimeError(f"{role} backup or new app hash mismatch")
            images[role] = (backup, new)

        ssh = ["ssh", "-T", *SSH_ARGS, PI]
        invoke(ssh + ["umask 077; mkdir " + shlex.quote(remote)], "pi_stage.log")
        for path, target in zip(images["tag"], ("prior.bin", "new.bin"), strict=True):
            # A relative local path also avoids scp treating the Windows drive colon as a host.
            relative = str(path.relative_to(WORKSPACE))
            invoke(["scp", *SSH_ARGS, relative, f"{PI}:{remote}/{target}"], f"pi_copy_{target}.log")
        tag = state["roles"]["tag"]
        manifest = f"{tag['prior_app_sha256']}  prior.bin\n{tag['new_app_sha256']}  new.bin"
        check_remote = (f"cd {shlex.quote(remote)} && printf '%s\\n' {shlex.quote(manifest)}"
                        " | sha256sum -c -")
        invoke(ssh + [check_remote], "pi_images_hash.log")

        def board_command(role, action, use_prior=False):
            write_flags = ["--flash-mode", "keep", "--flash-freq", "keep", "--flash-size", "keep"]
            args = ["-m", "esptool", "--chip", "esp32", "--port", PORTS[role], "--baud", "460800",
                    action, *(write_flags if action == "write-flash" else []), "0x10000"]
            if role == "anchor":
                return [sys.executable, *args, str(images[role][0 if use_prior else 1])]
            filename = "prior.bin" if use_prior else "new.bin"
            return ssh + [check_remote + " && " + shlex.join(["python3", *args, filename])]

        for role in PRIOR:
            invoke(board_command(role, "verify-flash", use_prior=True),
                   f"{role}_prior_verify.log", PRIOR[role][2])
            state["roles"][role]["prior_verified_on_device"] = True
            write_json("deployment.json", state)

        for role in PRIOR:
            state["roles"][role]["write_attempted"] = True
            write_json("deployment.json", state)
            invoke(board_command(role, "write-flash"), f"{role}_write.log", PRIOR[role][2])
            state["roles"][role]["write_ok"] = True
            write_json("deployment.json", state)
            invoke(board_command(role, "verify-flash"), f"{role}_verify.log", PRIOR[role][2])
            state["roles"][role]["flash_readback_verified"] = True
            write_json("deployment.json", state)
    except Exception as exc:
        state["error"] = repr(exc)
        raise
    finally:
        state["finished_utc"] = datetime.now(timezone.utc).isoformat()
        write_json("deployment.json", state)


if __name__ == "__main__":
    flash()

