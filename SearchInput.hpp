#pragma once

#include <hyprland/src/helpers/memory/Memory.hpp>

class IKeyboard;
class IPointer;

namespace OverviewSearchInput {
// Installs source-context hooks only; does not change input configuration.
// A failed initialization removes any hooks created during that attempt.
bool initialize();
bool available();
void shutdown();

// The device belonging to the current synchronous input-manager call.
// Outside that call, or for the other input kind, returns an empty pointer.
SP<IKeyboard> keyboard();
SP<IPointer> pointer();

// True when the current event's device/code has a keybind press record.
// Does not consult physical key state or the input manager's seat key list.
bool hasPressRecord();

// Call for an uncaptured release while a text editor is active. Suppresses new
// bind actions, preserving release callbacks and normal release cleanup.
// A keyboard release without a record gets a fully suppressed temporary one;
// wrapper exit removes it if an upstream early return leaves it behind.
// No actual submap or seat press is changed. Existing records are not removed.
// The historical-release flag lasts until the current wrapper returns.
void suppressHistoricalReleaseBinds();
bool passingHistoricalRelease();
}
