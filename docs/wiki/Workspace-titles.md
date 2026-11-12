# Workspace titles

Each preview has a centered title bar near its upper edge, with `#A7AFD5`
text by default, configurable through `titles:color`.
The bar is fully transparent: neither normal display nor editing draws a
background fill. The default font is DejaVuSansM
Nerd Font with FontAwesome 7 Free as fallback, at Semibold weight (600) and
20 logical pixels. The bar is shifted upward by 12 logical pixels to separate
it from window content without moving or resizing the windows.

Normal display uses the `titles:format` string, which defaults to
`$workspace    $title` with four spaces between the variables. With this
default, the native workspace name or number and custom title are centered
together; without a custom title, only the native label is shown. A custom
title changes only ScrollOverview's display: Hyprland workspace 3 remains
workspace 3, including its native name, dispatchers, rules, and external
workspace indicators.

The bar uses the existing workspace preview box in both horizontal and
vertical layouts. Its height, upward offset and text follow the monitor scale.
The offset does not reserve space between cards: `workspace_gap = 0` is still
supported, but larger offsets can overlap the preceding preview in vertical
layouts. Titles outside the monitor are clipped rather than moved back.
All title bars are drawn after the window previews, including overflowing,
dragged, and pinned windows. Top/overlay layer surfaces remain above the
title bars.

## Editing

Click a title with the primary mouse button to start editing. The primary
button follows the overview's left-handed setting. The insertion point is
shown as a vertical bar. Long text is cropped around that insertion point.
Editing always shows the native name, four spaces, and the editor; it does
not apply `titles:format`. The native name is read-only, and the custom title
remains editable even if the format omits `$title` or is empty. Editing a
workspace without an existing custom title starts with an empty draft.
Long native names are shortened while editing to leave room for the cursor.
A thin green (`#16a085`) underline indicates the active editor; a failed save
changes it to red (`#f7768e`). No title-bar background fill is drawn in either
state. The configured text opacity is multiplied by the workspace overview
alpha; underlines use the overview alpha directly. Both fade with the preview.

- Enter or keypad Enter saves the title.
- Escape cancels without saving.
- Backspace removes the preceding Unicode codepoint.
- Delete removes the following Unicode codepoint.
- Left/Right move by one Unicode codepoint; Home/End move to either end.
- Saving an empty title removes the override; `$title` then substitutes empty
  text. With the default format, this leaves only the native label.

Clicking outside the current title cancels editing and consumes that click;
it does not select, close, or drag a window. Clicking a different title on the
same monitor starts editing that title and discards the previous draft.
Clicking the current title preserves the draft; it does not reposition the
cursor. Wheel navigation is suppressed while editing. Existing drag, resize,
pan, or pending submap mouse actions must finish before editing can begin.

Editing also works when a `scrolloverview` submap is configured. Key presses
belonging to the editor and their releases are consumed before the default
overview handlers. Release tracking is per input device and survives Enter,
Escape, switching titles, and overview destruction. Keys pressed before
editing are not claimed retroactively: their releases complete Hyprland's
state cleanup, with new release/deferred bindings suppressed.

### Fcitx5 Chinese input

When Fcitx5 is running on the session D-Bus, title editing creates a private
input context. Use the input-method switch configured in Fcitx, such as
Ctrl+Space, and type normally. A popup near the title shows Fcitx's composition
text and candidates. Space, number keys, and paging keys are processed by
Fcitx; clicking a candidate also commits it. Enter and Escape reach the title
editor only when Fcitx has not consumed them, so accepting or cancelling a
composition does not prematurely save or discard the title.

The popup is rendered by the plugin using Fcitx's client-side input-panel API.
The overview is compositor-owned and has no application surface to which a
native Wayland input-method popup could attach. The candidate popup retains
its own background and selection colors for readability, independent of the
transparent title bar. The popup uses the configured title font and weight,
not Fcitx's native popup theme. No clipboard, external text-entry application,
or shell command is involved.

Fcitx key processing is asynchronous and ordered, with a one-second timeout.
Without Fcitx, direct XKB input remains available. After a connection failure,
unsent keys fall back to XKB; a key already sent to Fcitx is not replayed,
because it may already have committed text. Reopening the editor reconnects.
Closing or switching titles discards pending composition and old callbacks.

Input snapshots use the keyboard's actual XKB state, including its layout,
Shift, and AltGr, rather than the symbol state used for key bindings. There is
no clipboard integration, selection, or grapheme-cluster navigation. Combining
marks and joined emoji are edited as individual codepoints. There is no
separate key-repeat timer. The cursor is static rather than blinking.

## Configuration

The defaults are:

```ini
plugin {
    scrolloverview {
        titles {
            enabled = true
            format = $workspace    $title
            font = DejaVuSansM Nerd Font,FontAwesome 7 Free Semibold
            font_size = 20
            color = rgba(a7afd5ff)
            height = 40
            offset = 12
        }
    }
}
```

`font` accepts a Pango font description, not a CSS font-family declaration.
Separate fallback families with commas, without quoting individual families,
and put the weight after the family list. The default description above
parses as two families with weight 600. Custom descriptions such as
`Noto Sans CJK SC` or `Noto Sans CJK SC Bold` retain their own weight; the
renderer does not force Semibold on user overrides. The underline colors
remain fixed; the title bar has no background fill.

`color` controls all title-bar text, including the workspace name, custom
title, literal icons, and the draft and insertion cursor while editing.
It accepts Hyprland color syntax: `rgb(ffcc66)` is opaque, while
`rgba(ffcc6680)` uses approximately 50% opacity. The default is
`rgba(a7afd5ff)` (`#A7AFD5`, fully opaque). The eight hexadecimal digits in
`rgba(...)` are red, green, blue, then alpha; `00` is transparent and `ff`
is opaque. The configured opacity is multiplied by the workspace overview
fade. Changing RGB refreshes the text texture; changing only opacity reuses
it. Fully transparent text still leaves the title area clickable.

The color applies globally or per monitor through Lua, like the other title
options. It does not change edit underlines or the input-method popup's text,
background, or selection colors.

`font_size` is the logical pixel font size, from 6 to 64. `height` is the
logical pixel bar height, from 16 to 128. Text is reduced when necessary to
fit a smaller bar. `offset` shifts the entire bar upward from the preview's
top edge, from 0 to 128 logical pixels; 0 restores the original position.
It is a position adjustment, not a guaranteed gap from every window, since
window positions and decorations vary. The clickable editing area and input
method popup follow the shifted bar. `enabled = false` disables titles and
cancels an active title edit. An empty `format` only hides the normal display
text; it does not disable the bar's clickable editing area.

### Display format

`titles:format` is a string used only for normal display. `$workspace`
substitutes the native Hyprland `displayName`, which can be a number or name;
`$title` substitutes the custom title, or empty text when no title is set.
The default is `$workspace    $title`, with four ASCII spaces between the
variables. The resulting text is centered as a whole.

Variables may be repeated, reordered, or omitted. For example,
`$title / $workspace / $title` repeats the title, while `$workspace` omits it.
`$$` produces a single literal dollar sign, and unknown variables such as
`$unknown` are preserved unchanged. Substitution is non-recursive: dollar
signs or variable-like text inside a native name or custom title are not
expanded again. The output is plain text, not Pango markup; markup-like text
is displayed literally. Pango font descriptions still configure fonts as
described above.

After substitution, leading and trailing ASCII spaces (U+0020) are removed.
Internal spaces and literal icons are preserved. An empty custom title
replaces only `$title` with empty text; it does not automatically remove an
adjacent icon or separator. For workspace 3 with title `开发`, the format
`$workspace   $title` displays `3   开发`. Without a custom title, it displays
`3 `, keeping the icon while trimming the trailing spaces.

An empty format displays empty text but leaves the title bar clickable.
Clicking still opens the native-name-plus-four-spaces editor, regardless of
whether the format includes `$title`. Only `titles:enabled = false` disables
titles; formatting does not change saved titles or native workspace names.

Set the format globally in Hyprlang without string quotes or escaping:

```ini
plugin {
    scrolloverview {
        titles {
            format = $workspace   $title
        }
    }
}
```

Hyprlang 0.6.8 passes this format through unchanged unless a matching
configuration variable is defined. Do not define Hyprlang variables named
`$workspace` or `$title`: Hyprlang substitutes matching variables before the
plugin receives the format. Lua strings are not affected by this Hyprlang
variable substitution.

The same options, including `format`, can be set globally or per monitor
using Lua. Lua string values use quotes:

```lua
hl.plugin.scrolloverview.configure({
    output = "DP-1", -- omit for global settings
    titles = {
        enabled = true,
        format = "$workspace   $title",
        font = "Noto Sans CJK SC",
        font_size = 22,
        color = "rgba(ffcc66ff)",
        height = 40,
        offset = 12,
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
other selectors are preserved. Recreating a named workspace under a different
address uses a different selector; aliases are not migrated automatically.

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
underline red (`#f7768e`), and displays an error notification. If rename
succeeded but directory synchronization failed, the error explicitly says
the file was replaced; the in-memory store then reflects that file, without
claiming durable success.
Escape can discard the remaining draft, or Enter can retry saving it.

## Developer integration and validation

`CScrollOverview` exposes these public methods:

```cpp
void cancelWorkspaceTitleEdit();
bool isWorkspaceTitleEditing() const;
```

Title editing and window search are mutually exclusive across monitors.
Entering search cancels the current title draft. While either editor owns
input, normal overview shortcuts do not run; Escape leaves that editor without
closing the preview. Custom titles are included in search and result details.

Both editors use `SearchInput.*` to obtain the actual source keyboard and
finish historical releases without triggering new bindings. If those
version-specific hooks cannot be installed, clicking a title reports an input
error; the direct Lua setter and display of persisted titles remain available.
Title release guards stay registered until plugin unload.

`WorkspaceTitles.*` contains the independently testable store and editor;
`WorkspaceTitleUI.*` contains the Hyprland input and rendering integration.
`WorkspaceTitleIME.*` contains the standalone Fcitx5 D-Bus transport.
Building the plugin requires the `dbus-1` development package. Fcitx5 and a
Chinese input method such as Pinyin are runtime requirements for Chinese
composition, not compile-time dependencies.
All test sources are excluded from the CMake/Meson root-level source lists.

```bash
make test-workspace-titles
make test-workspace-title-render
make test-workspace-title-ime
make test-workspace-title-ime-real # optional: requires Fcitx5 and Pinyin
bash scripts/test-workspace-titles.sh -O1 -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer
make all
```

The standalone tests cover format substitution and escaping, UTF-8 validation
and boundaries, selector identity, round trips, empty-title removal,
state-directory fallback, malformed files, and injected write/sync/rename/close
failures. They do not start Hyprland or change the current desktop.

The rendering regression test compiles the actual `titleBox()`, `render()`,
and `flushPass()` method bodies against lightweight substitute dependencies.
It also extracts the real input-panel renderer and cancellation methods.
It checks registered font, format and color defaults, formatted and centered
labels, transparent title backgrounds, configurable text color and opacity,
RGB cache invalidation, alpha-only texture reuse, green/red edit underlines,
alpha multiplication, prefix spacing, candidate popups and hitboxes, nonempty draw
regions, flushing before overlays, clipping, scaling, configuration guards,
empty formats, per-monitor formats and colors, and texture caching.
It loads the system Pango library (`libpango-1.0.so.0`) to verify the fallback
family list
and weight, including custom font overrides; layout and DPI remain test
substitutes. It does not test OpenGL, glyph pixels or Hyprland's occlusion
logic.

The IME tests use a fake Fcitx service on private D-Bus sessions. They require
`dbus-run-session`, Python `dbus-python`, and PyGObject. Tests cover ordering,
press/release snapshots, deadlines, malformed or forged messages, owner loss,
bus disconnects, stale responses, and callback reentrancy. The optional real
Fcitx test starts an isolated Pinyin instance, types `nihao`, checks the
candidate panel, and commits `你好`. Both modes use temporary HOME/XDG paths
and do not connect to the desktop's Fcitx instance.

Titles must be queued as pass elements and flushed before top/overlay layers.
In Hyprland 0.57.1, `draw(context, data, {})` draws immediately with empty
damage; it does not enqueue the element or infer a full-screen draw region.
Using that call for title rectangles and textures skips their drawing.

Runtime checks require a separate, explicitly authorized compositor session:
verify both layouts, fractional monitor scales, zero gaps, overlapping window
previews, long CJK titles, submap bindings, saving/cancelling while keys are
held, closing an editor before key release, and failed saves. Check that title
bars draw no background fill in normal or editing states, while edit
underlines and the IME popup background remain visible.

Format checks include the default, repeated/reordered/omitted variables,
`$$`, unknown variables, non-recursive replacement, literal markup, outer
ASCII-space trimming, preserved internal spaces, and icons with empty titles.
Check global and per-monitor formats, including an empty format: the bar
must remain clickable and editing must retain the native name, four spaces,
and editor. Verify that `enabled = false` disables titles instead.
Compilation and the pure tests do not establish that these desktop
interactions have passed.
