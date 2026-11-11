#!/usr/bin/env bash
set -euo pipefail

# Dependencies: g++, pkg-config, libdbus development headers, dbus-run-session,
# and /usr/bin/python3 with dbus-python and PyGObject (gi/GLib).
# Only a temporary standalone executable is built; no plugin build is invoked.
# Extra compiler/linker flags are accepted, e.g. -O1 -g -fsanitize=address,undefined.
# Optional --real runs only the real Fcitx5/Pinyin smoke in temporary homes.
mode=fake
test_source=workspace-title-ime.cpp
if [[ "${1:-}" == --real ]]; then
    mode=real
    test_source=workspace-title-ime-real.cpp
    shift
fi
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
temporary="$(mktemp -d "${TMPDIR:-/tmp}/workspace-title-ime.XXXXXX")"
trap 'rm -rf -- "$temporary"' EXIT
# Deliberately independent of the invoking desktop's WAYLAND_DISPLAY.
export WAYLAND_DISPLAY=workspace-title-ime-test-0

for command in "${CXX:-g++}" pkg-config dbus-run-session timeout; do
    command -v "$command" >/dev/null || {
        printf 'Missing dependency: %s\n' "$command" >&2
        exit 2
    }
done
if [[ "$mode" == real ]]; then
    command -v fcitx5 >/dev/null || { echo 'Missing optional dependency: fcitx5' >&2; exit 2; }
else
    /usr/bin/python3 -c 'import dbus; import gi; from gi.repository import GLib'
fi
pkg-config --exists dbus-1
for source in WorkspaceTitleIME.hpp WorkspaceTitleIME.cpp; do
    [[ -f "$root/$source" ]] || {
        printf 'Missing implementation: %s\n' "$root/$source" >&2
        exit 2
    }
done
read -r -a dbus_cflags <<< "$(pkg-config --cflags dbus-1)"
read -r -a dbus_libs <<< "$(pkg-config --libs dbus-1)"
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic \
    "${dbus_cflags[@]}" -I"$root" "$@" \
    "$root/WorkspaceTitleIME.cpp" "$root/tests/$test_source" \
    "${dbus_libs[@]}" -pthread -o "$temporary/workspace-title-ime-tests"

if [[ "$mode" == real ]]; then
    mkdir -p "$temporary"/{home,config/fcitx5,data,cache,runtime,state,config-dirs}
    chmod 700 "$temporary/runtime"
    cat >"$temporary/config/fcitx5/profile" <<'PROFILE'
[Groups/0]
Name=Default
Default Layout=us
DefaultIM=pinyin

[Groups/0/Items/0]
Name=keyboard-us
Layout=

[Groups/0/Items/1]
Name=pinyin
Layout=

[GroupOrder]
0=Default
PROFILE
    # A clean environment removes DISPLAY, WAYLAND_DISPLAY, WAYLAND_SOCKET,
    # DBUS_STARTER_ADDRESS/BUS_TYPE and all inherited session/addon overrides.
    # System resources remain readable; all writable homes are temporary.
    timeout 30s env -i PATH=/usr/bin:/bin LC_ALL=C.UTF-8 \
        HOME="$temporary/home" XDG_CONFIG_HOME="$temporary/config" \
        XDG_DATA_HOME="$temporary/data" XDG_CACHE_HOME="$temporary/cache" \
        XDG_RUNTIME_DIR="$temporary/runtime" XDG_STATE_HOME="$temporary/state" \
        XDG_CONFIG_DIRS="$temporary/config-dirs" XDG_DATA_DIRS=/usr/share \
        DBUS_SYSTEM_BUS_ADDRESS="unix:path=$temporary/no-system-bus" \
        dbus-run-session -- bash -s -- "$temporary" <<'REAL'
set -euo pipefail
temporary="$1"
fcitx_pid=''
cleanup() {
    if [[ -n "$fcitx_pid" ]]; then
        kill "$fcitx_pid" 2>/dev/null || true
        wait "$fcitx_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT
# --disable all also disables required punctuation; enable it explicitly.
fcitx5 --disable all --enable dbus,dbusfrontend,keyboard,pinyin,punctuation --keep \
    >"$temporary/fcitx5.log" 2>&1 &
fcitx_pid=$!
status=0
"$temporary/workspace-title-ime-tests" || status=$?
if ((status != 0)); then cat "$temporary/fcitx5.log" >&2; fi
exit "$status"
REAL
    exit $?
fi

# Each phase gets its own bus. The second phase deliberately kills only its
# private daemon; the normal suite and the user's desktop bus are untouched.
# The outer timeout also bounds hangs inside a supposedly asynchronous call.
for phase in normal disconnect; do
mkdir -p "$temporary/$phase"/{home,config,data,cache,runtime,state,config-dirs}
chmod 700 "$temporary/$phase/runtime"
timeout 90s env -i PATH=/usr/bin:/bin LC_ALL=C.UTF-8 \
    HOME="$temporary/$phase/home" XDG_CONFIG_HOME="$temporary/$phase/config" \
    XDG_DATA_HOME="$temporary/$phase/data" XDG_CACHE_HOME="$temporary/$phase/cache" \
    XDG_RUNTIME_DIR="$temporary/$phase/runtime" XDG_STATE_HOME="$temporary/$phase/state" \
    XDG_CONFIG_DIRS="$temporary/$phase/config-dirs" XDG_DATA_DIRS=/usr/share \
    DBUS_SYSTEM_BUS_ADDRESS="unix:path=$temporary/no-system-bus" \
    WAYLAND_DISPLAY=workspace-title-ime-test-0 \
    dbus-run-session -- bash -s -- "$root" "$temporary" "$phase" <<'SESSION'
set -euo pipefail
root="$1"
executable="$2/workspace-title-ime-tests"
phase="$3"
temporary="$2/$phase"
service_pid=''
cleanup() {
    if [[ -n "$service_pid" ]]; then
        kill "$service_pid" 2>/dev/null || true
        wait "$service_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT
/usr/bin/python3 "$root/tests/workspace-title-ime-service.py" \
    "$temporary/ready" >"$temporary/service.log" 2>&1 &
service_pid=$!
for ((attempt = 0; attempt < 200; ++attempt)); do
    [[ -f "$temporary/ready" ]] && break
    if ! kill -0 "$service_pid" 2>/dev/null; then
        cat "$temporary/service.log" >&2
        exit 2
    fi
    sleep 0.01
done
if [[ ! -f "$temporary/ready" ]]; then
    printf 'Fake Fcitx service did not become ready\n' >&2
    cat "$temporary/service.log" >&2
    exit 2
fi
status=0
if [[ "$phase" == disconnect ]]; then
    # Ask this freshly-created bus for its own PID, never use a host session PID.
    bus_pid="$(/usr/bin/python3 - <<'PY'
import dbus
bus = dbus.SessionBus(private=True)
print(bus.call_blocking(
    'org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
    'GetConnectionUnixProcessID', 's', ('org.freedesktop.DBus',)))
PY
)"
    "$executable" --disconnect "$bus_pid" || status=$?
else
    "$executable" || status=$?
fi
if [[ -s "$temporary/service.log" ]]; then
    printf '\nFake service output:\n' >&2
    cat "$temporary/service.log" >&2
fi
exit "$status"
SESSION
done
