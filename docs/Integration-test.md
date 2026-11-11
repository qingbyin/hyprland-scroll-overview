# Isolated plugin test execution

## Result

Offline tests and compilation pass. Runtime integration is blocked by
EGL initialization in the isolated nested compositor, including the
plugin-free baseline. No successful plugin runtime test is claimed.

No plugin was loaded into the host compositor. The host configuration,
installed plugins and device permissions were not changed. After testing,
the host Hyprland process was still running and no test compositor,
test Bubblewrap process or test D-Bus launcher remained.

## Build and offline checks

```bash
make test
cmake --build .build/cmake-integration -j2
./scripts/test-input-hooks.sh -O1 -g -fsanitize=address,undefined
scripts/test-nested.sh --check-isolation --baseline
```

All commands above passed. Window-search testing performed 1,150,508
checks across nine groups. The input-hook test performed 131 keyboard
calls and 130 pointer calls, including the sanitizer run.

Tested plugin artifact:

```text
.build/cmake-integration/libscrolloverview.so
SHA-256:
af4c477f2bd371aceb253ade7be8086dc6f498d1caae2397b03a240620746e96
```

The plugin snapshot used for the runtime attempt has the same SHA-256.
Hyprland is 0.57.1, commit
`acc1aa0bfd4bd575dcb0f76c35ccf12b660a1ab9`, with Aquamarine 0.16.0.

## Runtime attempts

### Plugin-free baseline

```bash
scripts/test-nested.sh --run --baseline
```

Artifacts: `.build/nested/run.uDzB9KI8/`.
Exit status: 134, SIGABRT.

The private seatd connection failed as intended. Aquamarine selected its
Wayland backend, with no physical session. Parent DMA-BUF feedback led to
NVIDIA `/dev/dri/renderD129`. Hyprland then failed to initialize EGL:

```text
Backend: sessionless
backend: wayland
openRenderNode got drm device /dev/dri/renderD129
EGL: failed to initialize a platform display
at: line 177 in OpenGL.cpp
```

The crash stack points to `Render::GL::CHyprOpenGLImpl` construction,
not the keyboard path from crash 4186.

### Software-rendering baseline attempt

Artifacts: `.build/nested/run.lZqVKgzA/`.
Exit status: 134, SIGABRT.

A second prepared baseline used the same device and namespace isolation,
adding only these environment variables inside Bubblewrap:

```text
LIBGL_ALWAYS_SOFTWARE=1
__EGL_VENDOR_LIBRARY_FILENAMES=
  /usr/share/glvnd/egl_vendor.d/50_mesa.json
```

The vendor path is a single value; the display above wraps it for reading.
The exact command is saved as `software-command.txt` in that directory.
This attempt also failed during EGL initialization. Setting those
variables did not establish a usable software-rendering session.

### Plugin-configured attempt

```bash
scripts/test-nested.sh --run \
  .build/cmake-integration/libscrolloverview.so
```

Artifacts: `.build/nested/run.belIq5u4/`.
Exit status: 134, SIGABRT.

This attempt uses a snapshot of the repaired plugin and the generated
plugin-loading Lua configuration. It failed at the same EGL assertion as
the baseline, before plugin runtime testing. The crash report does not
list a loaded plugin. Consequently this run neither confirms a plugin
startup defect nor demonstrates that the repaired plugin works in a
running compositor.

## Interpretation and remaining coverage

The plugin-free baseline establishes that the current runtime blocker
does not require ScrollOverview. NVIDIA normally needs interfaces beyond
the render node; missing access is a possible explanation, inferred from
the chosen NVIDIA node and the restrictive device mounts, not proven by
a successful comparison with those interfaces exposed.

The test did not expose additional NVIDIA devices, DRM card nodes,
physical input devices or host buses to bypass this failure.

Plugin initialization, ordinary key events through the actual compositor,
overview open/close, search, title editing and unload remain untested in
a running isolated session. Offline ownership tests are not substitutes
for that coverage. The VM and separate-machine options are documented in
[Safe testing](wiki/Safe-testing.md).

Launch summaries and exit statuses are retained under
`.build/integration-test/`. Each run contains its own compositor log and
`cache/hyprland/hyprlandCrashReport5.txt`; PID 5 is namespace-local and
is not the host compositor's PID.
