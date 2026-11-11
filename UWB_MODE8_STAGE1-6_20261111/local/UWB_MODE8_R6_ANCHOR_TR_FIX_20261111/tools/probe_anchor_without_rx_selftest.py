"""Isolate the anchor RX self-test path with one paired Stage 2 transaction."""

import json
from pathlib import Path
from secrets import randbelow
import sys
from time import sleep

from contextlib import ExitStack

sys.path.insert(0, r"D:/SLAM_bot/artifacts/UWB_MODE8_R5_PAIR_RF_20261111")
from run_split_pair import SplitLink, TAG_PORT


def command(link, text, event, predicate=lambda _: True):
    link.send(text)
    return link.wait(lambda value: value.get("event") == event and predicate(value),
                     timeout=5, check_faults=False)


def main(directory):
    directory.mkdir()
    session = randbelow(2**31 - 1) + 1
    stop_id = randbelow(2**31 - 1) + 1
    report = {"session": session, "scope": "one Stage 2 transaction without anchor RX SELFTEST"}
    with ExitStack() as opened:
        anchor = SplitLink("anchor", "COM4", directory)
        opened.callback(anchor.close)
        tag = SplitLink("tag", TAG_PORT, directory)
        opened.callback(tag.close)
        sleep(0.4)
        try:
            for link in (anchor, tag):
                command(link, f"STOP {stop_id}", "STOPPED", lambda m: m.get("stop_request") == stop_id)
            command(anchor, "RESET", "HELLO", lambda m: not m.get("active"))
            for link in (anchor, tag):
                info = command(link, "SET STAGE 2", "HELLO", lambda m: m.get("stage") == 2)
                if info.get("role") != link.name or info.get("mode") != 8 or not info.get("hw_ok"):
                    raise RuntimeError(f"Unexpected board state {info}")
                report[f"{link.name}_hello"] = info
            if report["anchor_hello"]["config_id"] != report["tag_hello"]["config_id"]:
                raise RuntimeError("Configuration mismatch")
            report["anchor_pll_before"] = command(anchor, "PLLSTATUS", "PLLSTATUS")
            if not report["anchor_pll_before"].get("ready"):
                raise RuntimeError("Anchor PLL guard did not settle")
            report["tag_selftest"] = command(tag, "SELFTEST", "SELFTEST")
            if not report["tag_selftest"].get("pass"):
                raise RuntimeError("Tag self-test failed")
            for link in (anchor, tag):
                link.begin_capture()
            command(tag, f"START {session} 1", "HELLO", lambda m: m.get("active"))
            command(anchor, f"START {session} 1", "HELLO", lambda m: m.get("active"))
            try:
                anchor.wait(lambda m: m.get("event") == "STOPPED" and m.get("session") == session,
                            timeout=2)
                tag.wait(lambda m: m.get("event") == "STOPPED" and m.get("session") == session,
                         timeout=2)
            except (RuntimeError, TimeoutError) as err:
                report["exchange_error"] = str(err)
        except (RuntimeError, TimeoutError) as err:
            report["setup_error"] = str(err)
        finally:
            stop_id += 1
            report["stop"] = {}
            for link in (anchor, tag):
                try:
                    report["stop"][link.name] = command(
                        link, f"STOP {stop_id}", "STOPPED", lambda m: m.get("stop_request") == stop_id)
                except (RuntimeError, TimeoutError, OSError) as err:
                    report["stop"][link.name] = str(err)
            report["anchor_pll_after"] = command(anchor, "PLLSTATUS", "PLLSTATUS")
            report["stats"] = {link.name: link.stats() for link in (anchor, tag)}
    with (directory / "diagnosis.json").open("x", encoding="utf-8") as output:
        json.dump(report, output, indent=2)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main(Path(sys.argv[1]))
