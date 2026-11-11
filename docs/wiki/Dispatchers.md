Dispatchers are runtime actions exposed by ScrollOverview for controlling the plugin. They let you open, close, or toggle the overview on one or more monitors, move the current selection between windows and workspaces, and select or close windows from inside the overview.

You can call dispatchers directly from your Lua configuration using `hl.plugin.scrolloverview.<dispatcher>(...)`. This makes them suitable for keybinds, submaps, event handlers, and custom Lua functions. The same actions can also be invoked externally over Hyprland IPC with `hyprctl dispatch`, which is useful for shell scripts, status bars, launchers, and other automation.

## Usage examples

### Keybind

Bind `SUPER + g` to toggle the overview on all monitors:

```lua
-- hyprland.lua
hl.bind("SUPER + g", function()
    hl.plugin.scrolloverview.overview("toggle all")
end)
```

A dispatcher does not have to be used only as a standalone keybind action. It can be called from any Lua function and combined with conditions, configuration changes, timers, or other dispatchers as part of a larger script.

For navigation and window-action binds used while the overview is open, see [Keybind submap](Keybind-submap.md).

### CLI

Pass the corresponding Lua dispatcher expression to `hyprctl dispatch`:

```bash
hyprctl dispatch 'hl.plugin.scrolloverview.overview("toggle")'
hyprctl dispatch 'hl.plugin.scrolloverview.navigate("left")'
hyprctl dispatch 'hl.plugin.scrolloverview.window("close")'
```

## Available dispatchers

### `hl.plugin.scrolloverview.overview`

Controls the overview state.

| option | description |
| --- | --- |
| `toggle [monitor\|all]` | toggle the active monitor, a named monitor, or every monitor |
| `select` | select the workspace under the cursor |
| `close [monitor\|all]` | close every overview by default, or only the named monitor |
| `off [monitor\|all]` | same as `close` |
| `open [monitor\|all]` | open on the active monitor, a named monitor, or every monitor |
| `on [monitor\|all]` | same as `open` |

For example, `toggle DP-1` targets only `DP-1`, while `toggle all` opens all missing overviews or closes them when they are all already open. A bare `toggle` still targets only the active monitor, and a bare `close` closes all open overviews.

### `hl.plugin.scrolloverview.navigate`

Moves the overview selection/focus between windows in the current workspace. If there are no more windows in that direction, it selects the next workspace when the direction matches the configured overview layout.

| option | description |
| --- | --- |
| `left` | move selection left |
| `right` | move selection right |
| `up` | move selection up |
| `down` | move selection down |

### `hl.plugin.scrolloverview.window`

Acts on the window under the mouse cursor.

| option | description |
| --- | --- |
| `select` | focus/select the window under the mouse cursor |
| `close` | close the window under the mouse cursor |

### `hl.plugin.scrolloverview.search`

Search windows across ordinary and named workspaces on all enabled monitors.
The traditional equivalent is `scrolloverview:search`.

- `open` (default): enter search, opening an overview if needed.
- `toggle`: enter or cancel search without closing the overview.
- `close`: cancel search only.
- `query <text>`: enter search and replace its UTF-8 query.
  `query` alone clears the query.

Enter reveals and selects the chosen window in its monitor's overview.
Escape cancels only search, including when an overview submap is configured.
For input controls, filtering and safe UTF-8 arguments, see
[Window search](Window-search.md).

[Back to Configuration](Configuration.md)
