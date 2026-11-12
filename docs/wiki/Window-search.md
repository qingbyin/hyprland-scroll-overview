# Window search

Window search lists eligible windows from every enabled monitor and every
ordinary or named workspace. Candidates come from
`Desktop::windowState()->windows()`, not the current monitor's preview images.
It matches the current window title, appID/class, workspace display/address
name, plugin-private workspace title, and numbered workspace ID. Private
titles are also shown in the result details; native names remain unchanged.

## Opening and setting the query

The traditional dispatcher is `scrolloverview:search`. The Lua equivalent is
`hl.plugin.scrolloverview.search(argument)`. Both accept exactly these actions:

- `open`, or an empty argument: enter search, opening an overview if necessary.
  If search is already open, keep its query and selection.
- `toggle`: enter search, or cancel the existing search.
  Never close an overview.
- `close`: cancel search only. Without an active search, do nothing.
- `query <text>`: enter search and replace the query with the exact text after
  the first space. `query` or `query ` sets an empty query.

An omitted Lua argument defaults to `open`. Search uses the focused monitor's
overview or opens one there. If no monitor has focus, an existing overview can
be used. An existing search remains the target even if another monitor gains
focus. Search is not started during an overview swipe, drag, resize, or
scrolling-pan operation.

Traditional configuration:

```ini
bind = SUPER, f, scrolloverview:search, open

submap = scrolloverview
bind = , slash, scrolloverview:search, open
submap = reset
```

Lua configuration, using an action returned by the factory:

```lua
hl.bind("SUPER + f", hl.plugin.scrolloverview.search("open"))
```

Calling inside a bind callback is also supported:

```lua
hl.bind("SUPER + f", function()
    hl.plugin.scrolloverview.search("query terminal")
end)
```

The traditional IPC dispatcher accepts UTF-8 text without evaluating Lua:

```sh
hyprctl dispatch scrolloverview:search 'query 中文 文档'
hyprctl dispatch scrolloverview:search close
```

Queries are limited to 4096 UTF-8 bytes. Invalid UTF-8, embedded NUL, and C0/C1
control characters are rejected before changing the query. Quotes and
backslashes are ordinary query data. Lua actions capture arguments as data in
a constant-source Lua closure; query text is never inserted into Lua source
or executed through a shell.

## Input and matching

Search has its own modal input handling, whether or not a `scrolloverview`
submap exists. While it is open, ordinary overview binds do not receive search
keystrokes. Already-cancelled events are left alone.

- Type to insert at the caret, using the event's source keyboard and its actual
  XKB state, including the active layout, Shift and Caps Lock.
  Ctrl/Alt/Super combinations are not inserted as text.
  AltGr text is available when XKB resolves it as Level3.
- Left/Right move by one Unicode codepoint. Home/End move to either end.
  Backspace/Delete remove one codepoint. Ctrl+U clears the query.
- Up/Down, Tab/Shift+Tab, and the mouse wheel select results, wrapping at ends.
  Ctrl+J / Ctrl+N select the next result; Ctrl+K / Ctrl+P select the previous
  result, with the same wrapping. These shortcuts leave the query and caret
  unchanged. Additional Alt or Super modifiers do not select results.
  Page Up/Page Down move by one visible page.
- Left-click a result to select it. Press Enter to confirm it.
- Escape cancels only search. It does not select a result or close the overview.
  Clicking outside the panel does nothing.

Captured key and mouse-button releases are tracked per device and consumed even
after Enter or Escape has dismissed search. Holding the confirmation key cannot
fall through into an overview close bind. Releases belonging to presses before
search, including the opening shortcut, still reach Hyprland's state cleanup.
New release/deferred bindings are suppressed for those historical releases;
existing release callbacks can finish actions already started by their press.
ScrollOverview actions are suppressed during that cleanup.
Touch events are suppressed while searching; search selection itself supports
keyboard and mouse, not touch. The plugin's overview
swipe gesture does not close the preview while search is active.

Editing is codepoint-based, not grapheme-based. Combining marks and
multi-codepoint emoji can require several deletion steps.
There is no input-method protocol, preedit, compose-key engine, clipboard paste,
or synthetic key-repeat timer.
The `query` action provides a direct UTF-8 input path for external callers;
complete Chinese IME support is not claimed.

Words separated by Unicode whitespace must all occur, but may match different
fields. ASCII matching ignores case; other Unicode characters are compared
literally, without normalization or Unicode case folding. An empty query shows
all eligible windows. Exact field matches rank before prefixes, then other
substrings; ties prefer title, appID, workspace name, then workspace number.
Numbered workspace ties use numeric order, so 2 precedes 10. Named workspace
ties use natural name order, followed by title, appID and stable window ID.

The panel shows a no-results message when appropriate and scrolls its result
list to keep the selected row visible. Window close, move, title/class changes,
workspace rename, and monitor changes trigger refreshes. Each render and input
action also checks live metadata. Results retain weak window references, not
owning pointers or title-based identities. A vanished or ineligible selection
is not silently confirmed as a different window.

## Appearance

The search panel uses a snapshot of the local fuzzel Catppuccin Mocha theme:
`monospace` text, background `#1e1e2e` with alpha `dd`, text `#cdd6f4`,
secondary text `#bac2de`, muted text `#7f849c`, selection `#585b70`, and
border/caret `#89b4fa`. The border is 1 logical pixel wide and its outer
corner radius is 10 logical pixels. Corners outside the panel are transparent;
monitor scaling applies to both the border and the radius.

The plugin does not read `~/.config/fuzzel/fuzzel.ini` at runtime. Later edits
to that file do not automatically change the plugin's appearance.

## Confirmation stays inside the overview

Enter finds or creates the destination monitor's overview, activates the target
workspace through the monitor API, focuses and selects the target window, and
updates the overview viewport. For a scrolling layout it fits or centers the
target column and recalculates the layout before rendering. If scrolling is
inhibited, confirmation temporarily lifts inhibition and then freezes scrolling
at the new target position; it does not permanently enable free scrolling.
The search panel
closes; both the source and destination overviews remain open. This works
independently of the `cross_monitor_drag` option and does not warp the pointer.
Subsequent overview keyboard navigation uses the destination monitor.

If the destination monitor has an active special workspace, confirming a
normal-workspace result explicitly dismisses that special workspace with
`setSpecialWorkspace(nullptr, true)` before focusing the normal window. Merely
opening, typing in, or cancelling search does not change special-workspace
state. Search does not change group membership, pin state, or fullscreen state.

## Eligibility limits

The search list excludes windows which the preview cannot navigate correctly:

- Special-workspace windows, including inactive special workspaces.
- Pinned windows, including pinned floating windows.
- Non-current group members. A group's current window is searchable by its own
  title/appID; hidden members are not aliases for that card.
- Unmapped windows or windows on missing/disabled monitors, or with inconsistent
  workspace/monitor assignment during a move.
- Other tiled windows hidden by a fullscreen window in a non-scrolling layout,
  or by non-layout-managed fullscreen (including floating fullscreen) in a
  scrolling layout. The fullscreen window and ordinary floating windows remain
  eligible.
- Floating windows whose preview rectangle is entirely outside the monitor at
  the configured overview scale. Off-desktop windows exposed by the overview's
  centered scaling remain eligible.
- Windows with a no-focus rule, non-focusable X11 override-redirect windows,
  and Wayland parents whose modal child blocks focus under the current setting.

Normal tiled and visible, non-pinned floating windows are otherwise included.
These filters are applied both during enumeration and immediately before a
result is used. The renderer's existing handling of other windows is unchanged.

## Implementation and integration

`WindowSearch.*` contains the compositor-independent matching, sorting, UTF-8
validation and editor. `OverviewSearch.*` owns the global modal input listener,
weak result references and cached Pango/Cairo panel. `SearchInput.*` captures
the actual keyboard/pointer device during synchronous input-manager calls and
lets historical releases clean up normally without triggering new bindings.
`scrollOverview.cpp` only adds the lifecycle/render entrances and the navigation
method that needs its existing private workspace/scrolling helpers.

Input source tracking uses two version-specific Hyprland function hooks. If
these cannot be installed, the search dispatcher reports an error rather than
translating text with the wrong keyboard or interfering with release state.
Other overview operations remain available.

The public `CScrollOverview` interfaces for workspace-title integration are:

```cpp
SDispatchResult windowSearchAction(const std::string& argument);
void cancelWindowSearch();
bool isWindowSearchActive() const;
bool revealSearchWindow(PHLWINDOW window);
```

`OverviewSearch::active()` reports modal search on any monitor. Entering
search cancels any title draft across all open overviews. Beginning title
editing cancels search. Search itself captures pointer input until dismissed;
press Escape before clicking a title to edit it.

Search input listeners are installed before title and per-overview listeners.
Both text editors respect cancelled events, track captured releases per device,
and share `SearchInput.*` for source keyboards and historical-release cleanup.
The search panel is drawn after workspace titles and top/overlay layers.

`COverviewSearch::refresh()` includes each plugin-private title in searchable
workspace text. Saving an alias redraws open overviews, invalidating matching
and the cached result panel without renaming any native workspace.

## Offline verification

```sh
make test
make all
```

`make test` runs both title and search suites without a running desktop.
`make test-window-search` runs only `scripts/test-window-search.sh`. It compiles
in a temporary directory and checks all Unicode scalar values, malformed
encodings, control rejection, query limits, codepoint editing, randomized edit
sequences, multi-field matching, relevance and deterministic sorting. CMake
and Meson only enumerate root-level plugin sources, excluding tests and
compiler checks in build directories.

The plugin was built against the local pkg-config Hyprland 0.57.1 headers.
No plugin load, live-desktop input test, or user configuration change is part of
this verification. Cross-monitor focus, rendered appearance, fullscreen and
special-workspace transitions still require a compositor runtime test.
