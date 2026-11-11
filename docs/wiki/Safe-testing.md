# Safe plugin testing

Hyprland plugins execute inside the compositor process. A plugin memory
error can terminate the whole graphical session, including its clients.
A version/hash check does not protect against ownership errors or invalid
function hooks. Do not use a working desktop as the first integration test.

## Offline regression tests

```bash
make test
./scripts/test-input-hooks.sh -O1 -g -fsanitize=address,undefined
```

The input-hook test compiles the actual `SearchInput.cpp` with substitute
external dependencies. It checks copy ownership during forwarding,
repeated press/release events, multiple devices, nested dispatch, exception
cleanup and hook lifecycle. It does not reproduce Hyprland's optimized
caller, physical input stack, rendering or plugin ABI.

Build a separate artifact without installing or loading it:

```bash
cmake -S . -B .build/safe-test -DCMAKE_BUILD_TYPE=Debug
cmake --build .build/safe-test -j2
```

The resulting plugin is `.build/safe-test/libscrolloverview.so`.
Building does not replace a plugin already loaded in memory.

## Isolated nested session

`scripts/test-nested.sh` is an opt-in launcher for Linux with a merged
`/usr` layout, Bubblewrap, `dbus-run-session` and Hyprland 0.57 Lua support.
Run it from an existing compatible Wayland session. It never invokes
`hyprctl`, `hyprpm`, or a host service manager.

Preparation and namespace checks do not start a compositor:

```bash
scripts/test-nested.sh --prepare .build/safe-test/libscrolloverview.so
scripts/test-nested.sh --check-isolation --baseline
```

Explicitly start a plugin-free baseline first, then a plugin test:

```bash
scripts/test-nested.sh --run --baseline
scripts/test-nested.sh --run .build/safe-test/libscrolloverview.so
```

Each invocation has a fresh configuration, home, runtime directory,
cache, state directory and private D-Bus session. It clears the inherited
instance signature and display/session variables before supplying only
the parent Wayland socket. It disables Hyprland's systemd environment
updates, target activation, notifications and realtime-priority requests.
Xwayland and built-in news/donation prompts are disabled.

Bubblewrap creates separate process, network and other namespaces.
The host home and runtime directory are not mounted. A fresh `/dev`
contains no physical input devices, DRM card nodes or host virtual
terminals. The launcher exposes `/dev/dri/renderD*` for nested rendering.
It forces libseat to use an absent private seatd socket, so physical-seat
acquisition fails instead of falling through to a host session manager.
There is no automatic unsandboxed retry if startup fails.

Inside the nested window, `ALT+Return` launches Kitty if installed,
`ALT+g` toggles the overview, `ALT+f` opens search, and `ALT+Escape` exits.
Host shortcuts can still intercept keys. The nested process is limited
to 180 seconds, followed by a five-second forced-termination interval.
Timeout status 124 is not a successful test result.

Artifacts remain under `.build/nested/run.*`: the configuration, a plugin
copy and SHA-256, version information, launch command, logs and private
application state. Crash reports should appear under that run's cache.
These directories are writable by the test; they are not tamper-proof.
They can contain typed test data. Do not enter secrets into test clients.

**Verified locally:** shell syntax and namespace isolation on Hyprland
0.57.1 / Aquamarine 0.16.0. Actual nested startup was attempted with and
without the plugin. Both exited with status 134 during EGL initialization
on NVIDIA `renderD129`, before plugin runtime testing. A separate baseline
attempt forcing Mesa software rendering failed at the same stage.
No device permissions were expanded and no host plugin was loaded.

Plugin loading, overview rendering, interactive input and timed shutdown
remain unverified. See [the execution record](../Integration-test.md).
A passing namespace check is not a passing integration test.

### What remains shared

This protects the main compositor from ordinary plugin-process faults,
not every possible system failure. The kernel, GPU driver and parent
Wayland server remain shared. Binding a socket read-only does not restrict
its protocol requests. The plugin can use both exposed render nodes on a
multi-GPU machine. A GPU hang or driver defect can still affect the host.
Host `/usr`, `/etc` and `/sys` are visible read-only. This launcher is for
trusted development code, not for containing an actively malicious plugin.

If nested rendering fails, retain the log rather than exposing DRM card
nodes or removing the namespace restrictions. NVIDIA configurations can
require device access not granted by this launcher; compatibility is not
guaranteed on every driver setup.

## Virtual machine for stronger system isolation

For compositor startup, reload/unload, input-device lifecycle and crash
reproduction, use a disposable VM with its own Hyprland session and disk
snapshot. Install the matching Hyprland version and headers in the guest,
build there, and load the plugin only into the guest compositor.
Do not pass the host GPU or physical input devices through to the guest.
A virtual GPU is sufficient for many functional tests, but does not
reproduce a particular physical GPU's behavior. Software rendering, where
supported, reduces shared hardware-acceleration exposure further.

The guest has its own kernel and session. Guest compositor faults do not
terminate the host desktop. Hypervisor and accelerated virtual-GPU code
remain possible shared failure surfaces; a VM is not an absolute guarantee.
Use a separate test machine for exact physical GPU/DRM/VT behavior when
failure of the working machine is unacceptable.

A second TTY or a different user account is not equivalent to a VM: it
still shares the host kernel, driver and physical display devices.

## Integration test sequence

1. Establish a plugin-free baseline in the isolated session.
2. Load only ScrollOverview. Type and release ordinary keys before opening
   any overview; its input hooks are already active at that point.
3. Exercise overview open/close, search, title editing, cancellation,
   repeated keys and mouse press/release pairs with test windows.
4. Test reload and unload, then test with hyprbars enabled separately.
5. In a VM or separate machine, test keyboard attach/detach, multiple
   layouts and the physical-input path. Nested input is not identical to
   the physical Aquamarine/libinput path from the reported crash.

Record the exact executable and plugin hashes with every failure. A plugin
version string based on the Git commit alone cannot identify its bytes.
For debugger work, start or attach GDB only inside the test environment,
not to the working compositor. ASan integration is best done with both
the guest compositor and plugin instrumented; the offline ASan test above
only covers the substitute-dependency test process.

## Backend limitations in this installed version

The inspected Hyprland build constructs a mandatory headless backend,
an optional DRM backend and a fallback Wayland backend. Merely setting
`WAYLAND_DISPLAY` does not rule out physical-seat acquisition.
`AQ_BACKEND=headless` and `WLR_BACKENDS=headless` are not backend selectors
in the inspected sources. `--safe-mode` is a recovery configuration,
not a process or device sandbox.

The headless backend alone does not supply the render device required
by this Aquamarine version. A simple GPU-free headless command is therefore
not provided here. A headless Weston parent is not a verified substitute:
Aquamarine 0.16 requests protocol versions above those advertised by
Weston 15.0.1, as well as usable DMA-BUF feedback.

Source references for the version-specific behavior:

- [Hyprland backend and session initialization][hypr-compositor]
- [Aquamarine backend initialization][aq-backend]
- [Aquamarine Wayland protocol bindings][aq-wayland]
- [Aquamarine headless implementation][aq-headless]

[hypr-compositor]: https://github.com/hyprwm/Hyprland/blob/acc1aa0bfd4bd575dcb0f76c35ccf12b660a1ab9/src/Compositor.cpp
[aq-backend]: https://github.com/hyprwm/aquamarine/blob/v0.16.0/src/backend/Backend.cpp
[aq-wayland]: https://github.com/hyprwm/aquamarine/blob/v0.16.0/src/backend/Wayland.cpp
[aq-headless]: https://github.com/hyprwm/aquamarine/blob/v0.16.0/src/backend/Headless.cpp
