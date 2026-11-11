# Workspace titles

Each preview has a title bar inside its upper edge. Without a custom title,
it displays the native workspace name or number. A custom title changes only
ScrollOverview's display: Hyprland workspace 3 remains workspace 3, including
its native name, dispatchers, rules, and external workspace indicators.

The bar uses the existing workspace preview box in both horizontal and
vertical layouts. Its height and text follow the monitor scale. It does not
require space between cards, so `workspace_gap = 0` is supported. All title
bars are drawn after the window previews, including overflowing, dragged,
and pinned windows. Top/overlay layer surfaces remain above the title bars.

## Editing

Click a title with the primary mouse button to start editing. The primary
button follows the overview's left-handed setting. The insertion point is
shown as a vertical bar. Long text is cropped around that insertion point.

- Enter or keypad Enter saves the title.
- Escape cancels without saving.
- Backspace removes the preceding Unicode codepoint.
- Delete removes the following Unicode codepoint.
- Left/Right move by one Unicode codepoint; Home/End move to either end.
- Saving an empty title removes the override and restores the native label.

Clicking outside the current title cancels editing and consumes that click;
it does not select, close, or drag a window. Clicking a different title on the
same monitor starts editing that title and discards the previous draft.
Clicking the current title preserves the draft; it does not reposition the
cursor. Wheel navigation is suppressed while editing. Existing drag, resize,
pan, or pending submap mouse actions must finish before editing can begin.

Editing also works when a `scrolloverview` submap is configured. Key presses
belonging to the editor and their releases are consumed before the default
overview handlers. Release tracking survives Enter, Escape, switching titles,
and overview destruction; the remaining physical releases are still consumed.
Keys pressed before editing are not claimed retroactively, so their releases
can complete the original input sequence.

Input uses the keyboard's actual XKB state, including its active layout,
Shift, and AltGr, rather than the symbol state used for key bindings. This is
not a complete input-method implementation: there is no IME composition,
clipboard integration, selection, or grapheme-cluster navigation. Combining
marks and joined emoji are edited as individual codepoints. There is no
separate key-repeat timer. The cursor is static rather than blinking.

## Configuration

The defaults are:

```ini
plugin {
    scrolloverview {
        titles {
            enabled = true
            font = sans-serif
            font_size = 14
            height = 30
        }
    }
}
```

`font_size` is the logical pixel font size, from 6 to 64. `height` is the
logical pixel bar height, from 16 to 128. Text is reduced when necessary to
fit a smaller bar. Disabling titles also cancels an active title edit.

The same options can be set globally or per monitor using Lua:

```lua
hl.plugin.scrolloverview.configure({
    output = "DP-1", -- omit for global settings
    titles = {
        enabled = true,
        font = "Noto Sans CJK SC",
        font_size = 14,
        height = 30,
    },
})
```

## Setting UTF-8 titles through Lua

A direct function is available with Hyprland's Lua configuration backend:

```lua
hl.plugin.scrolloverview.set_workspace_title("3", '开发 "项目 A"')
hl.plugin.scrolloverview.set_workspace_title("name:3", "另一个命名工作区")
hl.plugin.scrolloverview.set_workspace_title("3", "") -- clear the override
```

The first argument is the exact canonical workspace selector returned by
Hyprland's `Workspace::selector`: numbered `3` and named `name:3` are separate
keys. No workspace needs to be open when setting a title. Existing titles for
other selectors are preserved. A named workspace's native rename changes its
selector; its old private title remains associated with the old selector.

This function saves immediately; it is not a binding-action factory. It can
also be called from a Lua binding callback. Calling it during configuration
reload writes the specified value again. Arguments are passed as Lua strings
with their explicit lengths. They are never interpolated into Lua source or a
shell command, and no external program or clipboard reader is executed.

Titles support valid UTF-8, spaces, quotes, and backslashes, up to 4096 bytes.
They must be single-line text: ASCII controls, DEL, and Unicode line separators
are rejected. Selectors must be nonempty and satisfy the same validation.
Failures raise a Lua error. Successful calls cancel any open title draft and
refresh the open overviews.

## Persistence and errors

The plugin stores titles in:

```text
$XDG_STATE_HOME/hyprland-scroll-overview/workspace-titles.v1
```

If `XDG_STATE_HOME` is unset, empty, or relative, the fallback is:

```text
$HOME/.local/state/hyprland-scroll-overview/workspace-titles.v1
```

Without either usable absolute base directory, persistence reports an error
rather than writing into the current working directory. Overview object
recreation does not reset the plugin-level store. Reloading the plugin reads
its saved records again.

The versioned binary file uses length-prefixed UTF-8 fields, not executable
configuration or an unescaped line format. It is limited to 4096 records and
8 MiB. Saves reread the existing file to preserve other records, write a
private temporary file in the same directory, synchronize it, and atomically
rename it over the destination. A malformed existing file is not overwritten.
Simultaneous writers from separate compositor processes are not supported.

A failed interactive save keeps the editor and its draft open, colors the
bar red, and displays an error notification. If rename succeeded but directory
synchronization failed, the error explicitly says the file was replaced;
the in-memory store then reflects that file, without claiming durable success.
Escape can discard the remaining draft, or Enter can retry saving it.

## Developer integration and validation

`CScrollOverview` exposes these public methods:

```cpp
void cancelWorkspaceTitleEdit();
bool isWorkspaceTitleEditing() const;
```

A search entry point should cancel title editing before opening its input.
If search shortcuts must work while a title is being edited, register that
shortcut listener before `WorkspaceTitleInput::init()` and consume only its
own shortcut. Title listeners respect `info.cancelled`. Other search handlers
must also respect it, including the release of a title-owned Enter/Escape.
The title release guards remain registered until plugin unload, independent
of the lifetime of any one overview.

`WorkspaceTitles.*` contains the independently testable store and editor;
`WorkspaceTitleUI.*` contains the Hyprland input and rendering integration.
The tests deliberately use `.cc`, outside the CMake/Meson `*.cpp` globs.

```bash
make test-workspace-titles
bash scripts/test-workspace-titles.sh -O1 -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer
make all
```

The standalone tests cover UTF-8 validation and boundaries, selector identity,
round trips, empty-title removal, state-directory fallback, malformed files,
and injected write/sync/rename/close failures. They do not start Hyprland or
change the current desktop.

Runtime checks require a separate, explicitly authorized compositor session:
verify both layouts, fractional monitor scales, zero gaps, overlapping window
previews, long CJK titles, submap bindings, saving/cancelling while keys are
held, closing an editor before key release, and failed saves. Compilation and
the pure tests do not establish that these desktop interactions have passed.
