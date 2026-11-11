# Crash 4186: input-hook ownership investigation

## Evidence and limits

The user-provided `~/.cache/hyprland/hyprlandCrashReport4186.txt` reports
SIGSEGV in Hyprland 0.57.1, commit
`acc1aa0bfd4bd575dcb0f76c35ccf12b660a1ab9`, with ScrollOverview and hyprbars
loaded. The saved core reports SIGABRT because Hyprland's crash handler
aborted after handling the original segmentation fault.

Offline GDB inspection of that core shows:

```text
xkb_state_update_key(state=nullptr, key=36, direction=XKB_KEY_UP)
IKeyboard::updateXkbStateWithKey(this=0x563e581596c0)
```

At the fault, `rdi=0`; the faulting instruction reads `0x80(%rdi)`.
The caller obtains `m_xkbState` from `this+0x178`.

The keyboard allocation contains allocator free-list links rather than
a valid keyboard vtable. Its allocation size matches `CKeyboard`.
The input manager still retains a pointer to this allocation, while its
control block has zero strong references and null managed data.
The keyboard's XKB pointers are null, consistent with the destructor's
explicit clearing instructions. This is a freed-object lifetime failure,
not merely a missing keymap on an otherwise valid keyboard.

The plugin files available after the crash do not have the core's build ID:

| Artifact | ELF build ID |
| --- | --- |
| Core's plugin | `eeae4d6c839f1ee98bab908f855fb57b4a89c02a` |
| Repository plugin before fix | `45a9f94bf057f8ec2f7683b75cc57c6f952aace1` |
| hyprpm cached plugin inspected | `cf86d16dfcba389ed7091e005b13a2ea2863f0a0` |

The core's detours target plugin offsets `0x29a6ec` and `0x29a94e`, which
match the repository binary's keyboard and pointer wrappers. However,
the core did not dump that plugin's executable pages. Matching offsets
are not proof of identical code. Attribution of this crash to the defect
below remains an inference from the matching destinations, freed-object
state and demonstrated ownership mismatch, not an exact-binary replay.

## Defect in the inspected source and binary

`SearchInput.cpp` previously forwarded both device arguments with:

```cpp
original(manager, event, std::move(device));
```

The keyboard hook replaces an implementation whose caller was optimized
with knowledge of its argument behavior. The installed Hyprland caller
caches a shared-pointer control block and decrements it after return,
without reloading the moved-from argument. The replacement hook moves
that argument into another temporary and releases that ownership itself.
The caller's cached cleanup then releases an ownership count again.

Moving a by-value shared pointer is ordinarily valid C++. It is unsafe
here because a runtime detour changes behavior assumed by an already
optimized caller. The relevant installed executable offsets are:

```text
0x70f4de  adds two temporary strong references
0x70f4eb  calls CInputManager::onKeyboardKey at 0x6f1140
0x70f4f0  decrements the cached control block
0x70f5e0  decrements the cached control block again
```

With the old wrapper, the original-call temporary consumes the incoming
owner rather than acquiring a new one. After context destruction and
caller cleanup, the strong count is one lower than before the event.
The physical-keyboard callback emits the input signal and then updates
XKB state, explaining how destruction during input dispatch can leave
no plugin frames on the eventual crash stack.

The hooks are installed during plugin initialization. The defect does
not require opening search or the overview. Plugin XKB uses inspected
elsewhere are read operations, not state destruction.

## Change and validation

Both hook device parameters are now `const SP<...>` by value, and both
forward by copy. This preserves the incoming owner and gives the original
call its own temporary reference. The keyboard caller's imbalance was
established by disassembly. Applying the same rule to the pointer wrapper
is precautionary; the inspected mouse caller reloads its argument.

The regression test compiles the actual implementation with isolated
substitute dependencies and checks four simultaneous owners inside the
original-call substitute. Restoring either old move-forwarding behavior
in a temporary source copy makes this check fail. This is a behavioral
regression check, not a reproduction of Hyprland's compiler optimization.

Validation completed without loading any plugin into the working desktop:

- `make test`: all workspace-title, window-search and input-hook tests pass.
- Window search: 9 test groups, 1,150,508 checks.
- Input hooks: 131 keyboard calls and 130 pointer calls per passing run.
- Input-hook ASan/UBSan run passes; GCC and Clang runs also pass.
- CMake full plugin build succeeds against installed Hyprland headers.
- Nested-launcher namespace checks pass without starting a compositor.

The newly built artifact is
`.build/cmake-integration/libscrolloverview.so`.
The root `scrolloverview.so` was deliberately not overwritten, and no
installed plugin or Hyprland configuration was modified.

Actual nested input/rendering and physical-input reproduction remain
untested. See [Safe testing](wiki/Safe-testing.md) for the isolated test
procedure. Local private evidence is retained under
`.build/crash-analysis/`, including the extracted core, GDB output,
`ownership.txt`, build log and pre-fix plugin copy. Core dumps may contain
sensitive process memory and are excluded from Git by `.build/`.
