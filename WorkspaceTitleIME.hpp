#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace WorkspaceTitleIME {

struct Key {
    // Fcitx/XKB keysym, XKB keycode (evdev + 8), and Fcitx/XKB modifier mask.
    // Capture text at event time; releases normally have empty text.
    uint32_t sym = 0, code = 0, modifiers = 0, time = 0;
    bool release = false;
    std::string text;
};

struct Candidate {
    std::string label, text;
};

struct Panel {
    std::string preedit, auxiliary;
    // Fcitx's UTF-8 byte cursor, not a character or glyph index.
    int cursor = -1, selected = -1;
    std::vector<Candidate> candidates;
    bool hasPrevious = false, hasNext = false;
};

// Single-threaded, compositor-main-loop transport; no GLib or DBus callbacks.
// The owner must call poll() regularly, including while creation is pending.
// begin() alone may block for dbus_bus_get_private's initial bus registration.
// All subsequent I/O is nonblocking. No automatic service activation/retry.
//
// Callbacks may call end(), begin(), key(), select(), or poll(), but must not
// destroy this plugin-global Client. Recursive poll() is a no-op. end() drops
// queued work and emits one empty Panel if active; it never forwards old keys.
// Calls to key()/select() while inactive are ignored. key() queues snapshots;
// callbacks for fallback keys run from poll(), not from key().
//
// Transport failure clears the panel and falls back for UNSENT keys only.
// An uncertain key already sent to Fcitx is never replayed: its text may have
// committed before the connection failed. A new begin() can reconnect.
// ForwardKey uses the newest matching original snapshot (current or last 64).
// Synthetic/unmatched forwarded keys are ignored: the protocol supplies no
// text/keycode/time from which this transport could reconstruct an XKB event.
// Text formatting and candidate layout hints are not exposed by Panel.
// Reset/FocusOut/DestroyIC on end are best effort; private-connection closure
// also triggers Fcitx's client-disappearance cleanup, without waiting for it.
class Client {
  public:
    Client(std::function<void(const std::string&)> commit,
           std::function<void(const Key&)> unhandled,
           std::function<void(const Panel&)> panel);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) = delete;
    Client& operator=(Client&&) = delete;

    void begin();
    void end();
    void key(Key key);
    void poll();
    // Zero-based candidate index on the current page. Ordered with queued keys.
    void select(int index);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace WorkspaceTitleIME
