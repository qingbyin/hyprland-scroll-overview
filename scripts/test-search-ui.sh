#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
temporary="$(mktemp -d "${TMPDIR:-/tmp}/search-ui.XXXXXXXX")"
trap 'rm -rf -- "$temporary"' EXIT

# These methods have no column-zero closing braces except their own. Keep the
# body unchanged so the regression test exercises the production key routing.
python3 - "$root" "$temporary/search-input-methods.inc" <<'PY'
from pathlib import Path
import re
import sys
root = Path(sys.argv[1])
source = (root / "OverviewSearch.cpp").read_text()
parts = []
for signature in (
    "void COverviewSearch::key(const IKeyboard::SKeyEvent& event)",
    "void COverviewSearch::moveSelection(int delta)",
):
    matches = list(re.finditer(
        "^" + re.escape(signature) + r" \{\n.*?^\}", source, re.M | re.S))
    if len(matches) != 1:
        raise SystemExit(f"Expected exactly one definition: {signature}")
    parts.append(matches[0].group())
Path(sys.argv[2]).write_text("\n\n".join(parts) + "\n")
PY

"${CXX:-g++}" -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror \
    "$@" -I"$root" -I"$temporary" \
    "$root/tests/search-ui.cpp" "$root/WindowSearch.cpp" \
    $(pkg-config --cflags --libs pangocairo xkbcommon) \
    -o "$temporary/search-ui-tests"
"$temporary/search-ui-tests"
