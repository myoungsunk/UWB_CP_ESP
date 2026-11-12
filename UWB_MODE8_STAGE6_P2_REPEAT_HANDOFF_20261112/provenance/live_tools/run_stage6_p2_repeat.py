"""Run Stage 6 with both P2 repeat builds and their matching audit tools."""

from importlib import import_module
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
TOOLS = HERE / "p2_repeat_tools"


def main():
    # Load the matched collector before live_pair initializes its Link subclass.
    sys.path.insert(0, str(TOOLS))
    collector = import_module("run_pair")
    if Path(collector.__file__).resolve() != TOOLS / "run_pair.py":
        raise RuntimeError("Stage 6 P2 repeat collector import mismatch")
    runner = import_module("run_anchor_rx_edges_tag_queue_fix")
    return runner.main(
        anchor_root=HERE / "anchor_p2_repeat",
        anchor_firmware="D8_ANCHOR_STAGE6_P2_REPEAT_20261112",
        tag_root=HERE / "tag_p2_repeat",
        tag_firmware="D8_TAG_STAGE6_P2_REPEAT_20261112",
        analysis_root=TOOLS,
    )


if __name__ == "__main__":
    raise SystemExit(main())
