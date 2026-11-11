#!/usr/bin/env bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
temporary="$(mktemp -d "${TMPDIR:-/tmp}/workspace-titles.XXXXXX")"
trap 'rm -rf -- "$temporary"' EXIT

# Additional compiler flags can be passed as script arguments, e.g.
# ./scripts/test-workspace-titles.sh -O1 -g -fsanitize=address,undefined
"${CXX:-g++}" -std=c++23 -O2 -Wall -Wextra -Wpedantic -Werror \
    "$@" -I"$root" \
    "$root/WorkspaceTitles.cpp" "$root/tests/workspace-titles.cc" \
    -Wl,--wrap=write -Wl,--wrap=fsync -Wl,--wrap=rename -Wl,--wrap=close \
    -o "$temporary/workspace-titles-tests"

mkdir -- "$temporary/state"
"$temporary/workspace-titles-tests" "$temporary/state"
