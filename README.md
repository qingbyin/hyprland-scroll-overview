# ScrollOverview

ScrollOverview is an overview plugin like niri.

https://github.com/user-attachments/assets/e5eb1ad2-79bc-492a-82cd-02cd8b960d3e

## Installation

### Using Hyprpm (recommended)

1. Add the plugin repository:
   ```bash
   hyprpm add https://github.com/yayuuu/hyprland-scroll-overview.git
   ```
   If you use a Git build of Hyprland, add the plugin from the `new-release` branch instead:
   ```bash
   hyprpm add https://github.com/yayuuu/hyprland-scroll-overview origin/new-release
   ```
2. Build and fetch dependencies:
   ```bash
   hyprpm update
   ```
3. Enable the plugin:
   ```bash
   hyprpm enable scrolloverview
   ```
4. Configure and Enjoy.

For more installation methods, including building from source and loading the plugin automatically, see the [Installation guide](https://github.com/yayuuu/hyprland-scroll-overview/wiki/Installation).

## Configuration

### Hyprlang

```ini
# .config/hypr/hyprland.conf
plugin {
    scrolloverview {
        gesture_distance = 300 # how far is the "max" for the gesture
        scale = 0.5 # preferred overview scale
        workspace_gap = 100
        layout = vertical # vertical, horizontal, or auto (per-monitor orientation)
        wallpaper = 2 # 0: global only, 1: per-workspace only, 2: both
        wallpaper_layer = auto # auto, background, or bottom
        blur = true # blur only the main overview wallpaper

        shadow {
            enabled = true
            range = 50
        }
    }
}

# Toggle ScrollOverview with SUPER+g
bind = SUPER, g, scrolloverview:overview, "toggle all"
```

### Lua

```lua
-- .config/hypr/hyprland.lua
hl.config({
    plugin = {
        scrolloverview = {
            gesture_distance = 300, -- how far is the "max" for the gesture
            scale = 0.5, -- preferred overview scale
            workspace_gap = 100,
            layout = "vertical", -- vertical, horizontal, or auto (per-monitor orientation)
            wallpaper = 2, -- 0: global only, 1: per-workspace only, 2: both
            wallpaper_layer = "auto", -- auto, background, or bottom
            blur = true, -- blur only the main overview wallpaper

            shadow = {
                enabled = true,
                range = 50,
            },
        },
    },
})

-- Toggle ScrollOverview with SUPER+g
hl.bind("SUPER + g", function()
    hl.plugin.scrolloverview.overview("toggle all")
end)
```

Set `cross_monitor_drag = true` to drag windows between monitors through ScrollOverview and adopt Hyprland move drags when another overview is open. It defaults to `false`. Native adoption uses Hyprland internals; if unavailable, overview-origin cross-monitor dragging still works.

This setting does not affect overview-origin drags with `toggle all` because every overview is already open. Existing multi-monitor commands remain available when it is disabled.

## Workspace titles

Each workspace preview has a centered title bar with Waybar-themed text:
20px DejaVuSansM Nerd Font with FontAwesome 7 Free fallback, Semibold (600),
and `#A7AFD5` text. Font and size remain configurable. The title bar is fully
transparent: neither normal display nor editing draws a background fill.
The edit underline is green (`#16a085`), or red (`#f7768e`) after a failed
save. Text and underlines fade with the workspace. The IME candidate popup
keeps its own background for readability.
The bar is shifted upward by 12 logical pixels for more distance from window
content; `titles:offset` adjusts this shift, and 0 restores its original position.

The `titles:format` string defaults to `$workspace    $title` (four spaces).
In normal display, `$workspace` is Hyprland's native `displayName` (number or
name), and `$title` is the custom title. Variables can be repeated, reordered,
or omitted. `$$` produces one dollar sign; unknown variables stay unchanged.
Replacement is non-recursive, and text is not parsed as Pango markup.
Leading and trailing ASCII spaces are trimmed, while internal spaces and
literal icons remain. An empty title substitutes empty text without removing
icons. An empty format displays no text but keeps the clickable editing area;
only `titles:enabled = false` disables titles.

For example, set this globally inside `plugin:scrolloverview:titles` in
Hyprlang, without string quotes or escaping:

```ini
format = $workspace   $title
```

Do not define Hyprlang configuration variables named `$workspace` or
`$title`, since Hyprlang substitutes them before the plugin sees the format.
Lua strings are not affected by this substitution. Lua supports the same
options globally or per monitor:

```lua
hl.plugin.scrolloverview.configure({
    output = "DP-1", -- omit for global settings
    titles = { format = "$workspace   $title" },
})
```

Click to edit, press Enter to save, or Escape to cancel. Editing always shows
the native name, four spaces, and the editor without applying the format,
so titles remain editable even when `$title` is omitted. Fcitx5 composition
and candidate selection support Chinese input. Titles belong only to
ScrollOverview: workspace 3 can display `3    开发` with the default format
without changing its Hyprland name or number.
Building requires the `dbus-1` development package; Chinese composition requires
a running Fcitx5 instance and a configured Chinese input method.

See [Workspace titles](docs/wiki/Workspace-titles.md) for configuration,
UTF-8 input, persistence, and the direct Lua setting function.

## Window search

`scrolloverview:search open` opens a global window search inside the preview.
Use `hl.plugin.scrolloverview.search("open")` in Lua. Enter selects and reveals
its result without closing the overview; Escape cancels only search.
Search works with or without an overview submap. See
[Window search](docs/wiki/Window-search.md) for controls, UTF-8 queries,
eligibility limits and offline tests.

## Documentation

The complete documentation is available in the [ScrollOverview wiki](https://github.com/yayuuu/hyprland-scroll-overview/wiki). It covers all configuration options, keybinds, submaps, gestures, dispatchers, and advanced Lua examples.

## Development and safe testing

Run `make test` before integration testing. Plugins execute inside Hyprland;
a memory error can terminate the entire graphical session.
See [Safe testing](docs/wiki/Safe-testing.md) for offline regression tests,
an opt-in isolated nested launcher and virtual-machine testing.
The launcher never loads a plugin into the existing host compositor.

## Supported plugins

- `hyprbars`

<br>
<br>

## Star History

<a href="https://www.star-history.com/?repos=yayuuu%2Fhyprland-scroll-overview&type=date&legend=top-left">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=yayuuu/hyprland-scroll-overview&type=date&theme=dark&legend=top-left&sealed_token=dnoTQqdWontVtW1U-Qv5yJAQVYpafoKJ0ytKAwVNsCxYO8lor7ZbsD4oVIRHBW_9LjjRPMV-wHAXTw70cn4SbI_2OIrdFEMbOyjv9GVHi9EOBFIgwzKwvMTGWBhkYg5q1f9oV6mQqQZFgec__fA790Nj_OS5WOyVjeHFwceIJRFBlgSvBWyGMn3WUqym" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=yayuuu/hyprland-scroll-overview&type=date&legend=top-left&sealed_token=dnoTQqdWontVtW1U-Qv5yJAQVYpafoKJ0ytKAwVNsCxYO8lor7ZbsD4oVIRHBW_9LjjRPMV-wHAXTw70cn4SbI_2OIrdFEMbOyjv9GVHi9EOBFIgwzKwvMTGWBhkYg5q1f9oV6mQqQZFgec__fA790Nj_OS5WOyVjeHFwceIJRFBlgSvBWyGMn3WUqym" />
   <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=yayuuu/hyprland-scroll-overview&type=date&legend=top-left&sealed_token=dnoTQqdWontVtW1U-Qv5yJAQVYpafoKJ0ytKAwVNsCxYO8lor7ZbsD4oVIRHBW_9LjjRPMV-wHAXTw70cn4SbI_2OIrdFEMbOyjv9GVHi9EOBFIgwzKwvMTGWBhkYg5q1f9oV6mQqQZFgec__fA790Nj_OS5WOyVjeHFwceIJRFBlgSvBWyGMn3WUqym" />
 </picture>
</a>
