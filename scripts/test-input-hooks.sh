#!/bin/sh
set -eu

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/input-hook-test.XXXXXXXX")
trap 'rm -rf -- "$build_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Compile the actual implementation with isolated fake dependencies. No
# Hyprland libraries, plugin binary, running compositor, or desktop IPC.
# Extra flags are accepted, e.g. -O1 -g -fsanitize=address,undefined.
"${CXX:-g++}" -std=c++23 -O2 -Wall -Wextra -Wpedantic -Werror \
    "$@" -I "$source_dir/tests/input-hook-fakes" -I "$source_dir" \
    "$source_dir/SearchInput.cpp" \
    "$source_dir/tests/input-hook-ownership.cpp" \
    -o "$build_dir/input-hook-test"

"$build_dir/input-hook-test"
