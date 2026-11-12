#!/usr/bin/env bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
temporary="$(mktemp -d "${TMPDIR:-/tmp}/workspace-title-render.XXXXXXXX")"
trap 'rm -rf -- "$temporary"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# Compile the production method bodies unchanged, without their Hyprland includes.
# A small lexical scanner balances braces outside comments/string/char literals.
# Require exactly one standalone definition per signature and reject unsupported
# raw strings/preprocessor directives instead of silently extracting partial code.
# The harness supplies only the external types/services those methods use.
python3 - "$root" "$temporary/workspace-title-render-methods.inc" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])


def code_mask(source, path):
    # Blank comments and literals before finding signatures, not just braces.
    # A commented-out old implementation must never become the tested method.
    result = list(source)
    i = 0
    while i < len(source):
        start = i
        pair = source[i:i + 2]
        if pair == "//":
            end = source.find("\n", i + 2)
            i = len(source) if end < 0 else end
        elif pair == "/*":
            end = source.find("*/", i + 2)
            if end < 0:
                raise SystemExit(f"{path}: unterminated comment")
            i = end + 2
        elif source.startswith('R"', i):
            raise SystemExit(f"{path}: raw strings unsupported by test extractor")
        elif source[i] in ('"', "'"):
            quote = source[i]
            i += 1
            while i < len(source) and source[i] != quote:
                i += 2 if source[i] == "\\" else 1
            if i >= len(source):
                raise SystemExit(f"{path}: unterminated literal")
            i += 1
        else:
            i += 1
            continue
        for j in range(start, i):
            if source[j] != "\n":
                result[j] = " "
    return "".join(result)


def extract(path, signature):
    source = path.read_text(encoding="utf-8")
    code = code_mask(source, path)
    pattern = re.compile(r"^" + re.escape(signature) + r"[ \t]*\{", re.MULTILINE)
    matches = list(pattern.finditer(code))
    if len(matches) != 1:
        raise SystemExit(f"{path}: expected exactly one definition: {signature}")
    match = matches[0]
    depth = 0
    for i in range(match.end() - 1, len(code)):
        if code[i] == "#":
            raise SystemExit(f"{path}: preprocessor directive in {signature}")
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                line = source.count("\n", 0, match.start()) + 1
                return f'#line {line} "{path}"\n' + source[match.start():i + 1]
    raise SystemExit(f"{path}: unterminated definition: {signature}")


parts = [
    "// Generated from production sources; no method-body substitutions.",
    "namespace WorkspaceTitles {",
    extract(root / "WorkspaceTitles.cpp",
            "    std::string formatTitle(std::string_view format, std::string_view workspace, std::string_view title)"),
    "}",
    "namespace OverviewRender {",
    extract(root / "OverviewRender.cpp",
            "void flushPass(Render::CRenderContext& ctx, PHLMONITOR monitor)"),
    "}",
    extract(root / "WorkspaceTitleUI.cpp",
            "CBox CWorkspaceTitleUI::titleBox(size_t index) const"),
    extract(root / "WorkspaceTitleUI.cpp", "void CWorkspaceTitleUI::render()"),
    extract(root / "WorkspaceTitleUI.cpp", "void CWorkspaceTitleUI::renderInputPanel()"),
    extract(root / "WorkspaceTitleUI.cpp", "bool CWorkspaceTitleUI::editing() const"),
    extract(root / "WorkspaceTitleUI.cpp", "void CWorkspaceTitleUI::cancel()"),
]
Path(sys.argv[2]).write_text("\n\n".join(parts) + "\n", encoding="utf-8")
PY

# Extra flags are accepted, e.g. -O1 -g -fsanitize=address,undefined.
"${CXX:-g++}" -std=c++23 -O2 -Wall -Wextra -Wpedantic -Werror \
    "$@" -I"$temporary" "$root/tests/workspace-title-render.cpp" \
    -o "$temporary/workspace-title-render-tests"
"$temporary/workspace-title-render-tests"
