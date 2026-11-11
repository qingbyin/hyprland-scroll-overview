#!/bin/sh
set -eu

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/window-search-test.XXXXXXXX")
trap 'rm -rf -- "$build_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
    "$@" -I "$source_dir" \
    "$source_dir/WindowSearch.cpp" \
    "$source_dir/tests/window-search-test.cpp" \
    -o "$build_dir/window-search-test"

"$build_dir/window-search-test"
