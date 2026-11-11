#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
exe=$(mktemp /tmp/uwb_anchor_code_audit.XXXXXXXX)
trap 'rm -f -- "$exe"' EXIT
g++ --version | sed -n '1p'
g++ -std=c++11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fno-pie -no-pie \
    -fsanitize=address,undefined -DESP32 -DARDUINO \
    -Ishared -Itests/stubs -Ivendor/anchor/DW1000Ng/src \
    evidence/anchor_firmware_code_audit_20261111/repro.cpp -o "$exe"
"$exe"
g++ -std=c++11 -Wall -Wextra -Wpedantic -Werror -Ishared \
    -fsyntax-only tests/test_pll_startup.cpp
printf '%s\n' 'PLL startup policy checks passed'
g++ -std=c++11 -DESP32 -DARDUINO -Wall -Wextra -Wpedantic -Werror \
    -Itests/stubs -Ivendor/anchor/DW1000Ng/src -fsyntax-only firmware/ANCHOR_DUAL_DS_CIR/app.cpp
printf '%s\n' 'Anchor application syntax check passed'
fixture_dir=$(mktemp -d /tmp/uwb_anchor_core.XXXXXXXX)
trap 'rm -f -- "$exe" "$fixture_dir/synthetic_tag_record.json" "$fixture_dir/SYNTHETIC_CIR_i16.raw"; rmdir -- "$fixture_dir"' EXIT
g++ -std=c++11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fno-pie -no-pie \
    -fsanitize=address,undefined -Ishared tests/test_core.cpp -o "$exe"
"$exe" "$fixture_dir"
