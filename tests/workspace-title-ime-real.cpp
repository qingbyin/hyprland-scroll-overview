// Optional real Fcitx/Pinyin smoke test. The runner supplies a private bus and
// temporary HOME/XDG directories; never run this against a desktop session.
#include "WorkspaceTitleIME.hpp"
#include <dbus/dbus.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

int main() {
    try {
        DBusError error;
        dbus_error_init(&error);
        DBusConnection* bus = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
        if (!bus) {
            const std::string detail = error.message ? error.message : "no bus";
            dbus_error_free(&error);
            throw std::runtime_error(detail);
        }
        dbus_connection_set_exit_on_disconnect(bus, false);
        bool owned = false;
        const auto startup = Clock::now() + 5s;
        while (Clock::now() < startup) {
            owned = dbus_bus_name_has_owner(bus, "org.fcitx.Fcitx5", &error);
            if (owned || dbus_error_is_set(&error)) break;
            std::this_thread::sleep_for(20ms);
        }
        dbus_error_free(&error);
        dbus_connection_close(bus);
        dbus_connection_unref(bus);
        if (!owned) throw std::runtime_error("real Fcitx did not acquire its bus name");

        std::vector<std::string> commits;
        std::vector<WorkspaceTitleIME::Key> fallback;
        WorkspaceTitleIME::Panel panel;
        bool sawPreedit = false;
        WorkspaceTitleIME::Client client(
            [&](const std::string& text) { commits.push_back(text); },
            [&](const WorkspaceTitleIME::Key& value) { fallback.push_back(value); },
            [&](const WorkspaceTitleIME::Panel& value) {
                panel = value;
                sawPreedit = sawPreedit || !value.preedit.empty();
            });
        auto wait = [&](const std::function<bool()>& done, const char* description) {
            const auto deadline = Clock::now() + 5s;
            while (Clock::now() < deadline) {
                client.poll();
                if (done()) return;
                std::this_thread::sleep_for(2ms);
            }
            throw std::runtime_error(std::string(description) + "; preedit=" + panel.preedit +
                ", candidates=" + std::to_string(panel.candidates.size()) +
                ", fallback=" + std::to_string(fallback.size()));
        };
        uint32_t time = 1;
        auto stroke = [&](uint32_t sym, uint32_t code, uint32_t modifiers, const char* text) {
            WorkspaceTitleIME::Key value;
            value.sym = sym;
            value.code = code;
            value.modifiers = modifiers;
            value.time = time++;
            value.text = text;
            client.key(value);
            value.release = true;
            value.time = time++;
            value.text.clear();
            client.key(value);
        };
        client.begin();
        // The profile starts in keyboard-us; Control+Space activates Pinyin.
        // Keys intentionally queue during asynchronous context creation.
        stroke(0x20, 65, 4, "");
        stroke('n', 57, 0, "n");
        stroke('i', 31, 0, "i");
        stroke('h', 43, 0, "h");
        stroke('a', 38, 0, "a");
        stroke('o', 32, 0, "o");
        auto candidate = [&] {
            return std::find_if(panel.candidates.begin(), panel.candidates.end(),
                [](const auto& value) { return value.text == "你好"; });
        };
        wait([&] {
            // Pinyin may suggest 你好 as early as "ni h". Wait for every queued
            // letter before choosing an index from the final candidate list.
            std::string letters;
            for (char ch : panel.preedit)
                if (ch >= 'a' && ch <= 'z') letters += ch;
            return letters == "nihao" && candidate() != panel.candidates.end();
        }, "real Pinyin did not produce final nihao candidate 你好");
        if (!sawPreedit) throw std::runtime_error("missing real client-side preedit");
        const auto index = static_cast<int>(candidate() - panel.candidates.begin());
        std::cout << "PASS real-pinyin-client-side-panel: 你好 at candidate " << index
                  << ", preedit=" << panel.preedit << std::endl;
        client.select(index);
        wait([&] { return !commits.empty(); }, "real Pinyin candidate did not commit");
        if (commits != std::vector<std::string>{"你好"})
            throw std::runtime_error("unexpected real Pinyin commit payload/count");
        client.end();
        std::cout << "PASS real-pinyin-select-commit: 你好" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL real-pinyin-smoke: " << error.what() << std::endl;
        return 1;
    }
}
