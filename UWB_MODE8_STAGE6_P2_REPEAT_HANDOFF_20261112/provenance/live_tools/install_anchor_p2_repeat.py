"""Install the Stage 6 P2 repeat build on the pinned COM4 anchor."""

import install_anchor_diag as installer


if __name__ == "__main__":
    installer.PACKAGE = installer.HERE / "anchor_p2_repeat"
    installer.FIRMWARE = "D8_ANCHOR_STAGE6_P2_REPEAT_20261112"
    installer.PRIOR_SHA256 = 'fb52914ac7eb0783b801fab6fbaa5fc7c359c5870c05397e7a712934be2c1850'
    raise SystemExit(installer.main())
