// Standalone libdbus transport tests; run via scripts/test-workspace-title-ime.sh.
// No compositor, GLib client loop, installed Fcitx, or user session bus is used.
#include "WorkspaceTitleIME.hpp"

#include <dbus/dbus.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using WorkspaceTitleIME::Client;
using WorkspaceTitleIME::Key;
using WorkspaceTitleIME::Panel;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {
constexpr const char* controlName = "org.example.WorkspaceTitleIMETest";
constexpr const char* controlPath = "/org/example/WorkspaceTitleIMETest";

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct MessageDeleter {
    void operator()(DBusMessage* message) const { dbus_message_unref(message); }
};
using Message = std::unique_ptr<DBusMessage, MessageDeleter>;

class Control {
  public:
    Control() {
        DBusError error;
        dbus_error_init(&error);
        connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
        const std::string detail = error.message ? error.message : "unknown error";
        dbus_error_free(&error);
        require(connection != nullptr, "control connection: " + detail);
        dbus_connection_set_exit_on_disconnect(connection, false);
    }
    ~Control() {
        dbus_connection_close(connection);
        dbus_connection_unref(connection);
    }
    Message call(const char* method, const std::function<void(DBusMessage*)>& fill = {}) {
        Message request(dbus_message_new_method_call(
            controlName, controlPath, controlName, method));
        require(bool(request), "allocate control request");
        if (fill)
            fill(request.get());
        DBusError error;
        dbus_error_init(&error);
        Message reply(dbus_connection_send_with_reply_and_block(
            connection, request.get(), 2000, &error));
        const std::string detail = error.message ? error.message : "no reply";
        dbus_error_free(&error);
        require(bool(reply), std::string(method) + ": " + detail);
        return reply;
    }
    void configure(const char* mode = "handled", int create = 0, int key = 0,
                   int startup = 0, int select = 0) {
        call("Configure", [&](DBusMessage* request) {
            dbus_int32_t c = create, k = key, s = startup, selection = select;
            require(dbus_message_append_args(
                request, DBUS_TYPE_STRING, &mode, DBUS_TYPE_INT32, &c,
                DBUS_TYPE_INT32, &k, DBUS_TYPE_INT32, &s,
                DBUS_TYPE_INT32, &selection, DBUS_TYPE_INVALID), "configure arguments");
        });
    }
    void emit(const char* kind) {
        call("Emit", [&](DBusMessage* request) {
            require(dbus_message_append_args(request, DBUS_TYPE_STRING, &kind,
                                             DBUS_TYPE_INVALID), "emit arguments");
        });
    }
    std::vector<std::string> events() {
        auto reply = call("Events");
        DBusMessageIter iter, array;
        require(dbus_message_iter_init(reply.get(), &iter) &&
                dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY,
                "Events must return an array");
        dbus_message_iter_recurse(&iter, &array);
        std::vector<std::string> result;
        while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRING) {
            const char* value = nullptr;
            dbus_message_iter_get_basic(&array, &value);
            result.emplace_back(value);
            dbus_message_iter_next(&array);
        }
        return result;
    }
  private:
    DBusConnection* connection = nullptr;
};

Key key(uint32_t sym = 97, bool release = false, uint32_t time = 101) {
    Key result{};
    result.sym = sym;
    result.code = sym + 19;
    result.modifiers = 5;
    result.time = time;
    result.release = release;
    result.text = release ? "original release snapshot" : "原始 input snapshot";
    return result;
}
bool same(const Key& a, const Key& b) {
    return a.sym == b.sym && a.code == b.code && a.modifiers == b.modifiers &&
           a.time == b.time && a.release == b.release && a.text == b.text;
}
bool empty(const Panel& p) {
    return p.preedit.empty() && p.auxiliary.empty() && p.cursor == -1 &&
           p.selected == -1 && p.candidates.empty() && !p.hasPrevious && !p.hasNext;
}
std::string wireKey(const Key& k) {
    return "key:" + std::to_string(k.sym) + ":" + std::to_string(k.code) + ":" +
           std::to_string(k.modifiers) + ":" + std::to_string(k.release) + ":" +
           std::to_string(k.time);
}

struct Harness {
    std::vector<std::string> commits;
    std::vector<Key> fallback;
    std::vector<Panel> panels;
    std::function<void()> onCommit, onUnhandled, onPanel;
    Client client;
    Harness() : client(
        [this](const std::string& text) {
            commits.push_back(text);
            if (onCommit) onCommit();
        },
        [this](const Key& value) {
            fallback.push_back(value);
            if (onUnhandled) onUnhandled();
        },
        [this](const Panel& panel) {
            panels.push_back(panel);
            if (onPanel) onPanel();
        }) {}
    ~Harness() {
        onCommit = onUnhandled = onPanel = {};
        client.end();
    }
    void poll() {
        const auto start = Clock::now();
        client.poll();
        require(Clock::now() - start < 250ms, "poll blocked for >=250ms");
    }
    void pump(std::chrono::milliseconds duration) {
        const auto deadline = Clock::now() + duration;
        do {
            poll();
            std::this_thread::sleep_for(2ms);
        } while (Clock::now() < deadline);
    }
    void wait(const std::function<bool()>& predicate, const std::string& description,
              std::chrono::milliseconds timeout = 2200ms) {
        const auto deadline = Clock::now() + timeout;
        while (Clock::now() < deadline) {
            poll();
            if (predicate()) return;
            std::this_thread::sleep_for(2ms);
        }
        throw std::runtime_error("timed out waiting for " + description);
    }
    void ready(Control& control) {
        client.begin();
        wait([&] {
            const auto events = control.events();
            return std::find(events.begin(), events.end(), "focus-in") != events.end();
        }, "FocusIn");
        pump(30ms); // Allow the asynchronous startup reply to arrive.
    }
};

std::vector<std::string> operations(Control& control) {
    std::vector<std::string> result;
    for (const auto& event : control.events())
        if (event.rfind("key:", 0) == 0 || event.rfind("select:", 0) == 0)
            result.push_back(event);
    return result;
}
void exactFallback(const Harness& h, const std::vector<Key>& expected) {
    require(h.fallback.size() == expected.size(), "wrong fallback count: " +
            std::to_string(h.fallback.size()) + ", expected " + std::to_string(expected.size()));
    for (size_t i = 0; i < expected.size(); ++i)
        require(same(h.fallback[i], expected[i]), "fallback snapshot/order mismatch at " +
                std::to_string(i));
}
void checkPanel(const Panel& p) {
    require(p.preedit == "你ab", "preedit segments were not concatenated");
    require(p.cursor == 5, "Fcitx byte cursor was changed");
    require(p.auxiliary == "up\ndown", "auxiliary text must join with newline");
    require(p.selected == 1 && p.hasPrevious && !p.hasNext, "panel selection/page flags");
    require(p.candidates.size() == 2 && p.candidates[0].label == "1" &&
            p.candidates[0].text == "候選" && p.candidates[1].label == "2" &&
            p.candidates[1].text == "次", "candidate parsing");
}

void disconnectedBus(Control& control, pid_t busPid) {
    control.configure("commit", 0, 2000);
    Harness h;
    h.ready(control);
    control.emit("panel");
    h.wait([&] { return !h.panels.empty() && !empty(h.panels.back()); },
           "panel before bus disconnect");
    const auto sent = key(), a = key(98), b = key(98, true, 211);
    h.client.key(sent);
    h.client.key(a);
    h.client.key(b);
    h.wait([&] { return operations(control).size() == 1; }, "key before bus disconnect");
    const auto panels = h.panels.size();
    // The runner owns this dedicated daemon. Never kill the normal suite bus.
    require(busPid > 1 && kill(busPid, SIGTERM) == 0, "terminate dedicated test bus");
    // No control calls can be made after this point. Fail before the key timeout
    // so this tests actual disconnect detection rather than eventual expiration.
    h.wait([&] { return h.fallback.size() >= 2; }, "bus-disconnect fallback", 500ms);
    exactFallback(h, {a, b});
    require(h.commits.empty() && h.panels.size() == panels + 1 && empty(h.panels.back()),
            "disconnect must drop sent key and clear panel once");
    h.pump(100ms);
    exactFallback(h, {a, b});
    require(h.commits.empty() && h.panels.size() == panels + 1,
            "callbacks survived bus disconnect");
    std::cout << "PASS actual private bus disconnect" << std::endl;
}
} // namespace

int main(int argc, char** argv) {
    try {
        Control control;
        if (argc == 3 && std::string(argv[1]) == "--disconnect") {
            disconnectedBus(control, static_cast<pid_t>(std::stol(argv[2])));
            return 0;
        }
        require(argc == 1, "usage: test [--disconnect dedicated-bus-pid]");
        int failures = 0;
        auto test = [&](const char* name, const std::function<void()>& body) {
            try {
                body();
                std::cout << "PASS " << name << std::endl;
            } catch (const std::exception& error) {
                ++failures;
                std::cerr << "FAIL " << name << ": " << error.what() << std::endl;
            }
        };

        test("absent service preserves original press/release", [&] {
            control.call("ReleaseName");
            {
                Harness h;
                h.client.begin();
                const auto press = key(), release = key(97, true, 102);
                h.client.key(press);
                h.client.key(release);
                h.wait([&] { return h.fallback.size() >= 2; }, "absent-service fallback");
                exactFallback(h, {press, release});
            }
            control.call("AcquireName");
        });
        // Ensure an earlier assertion cannot leave later tests without an owner.
        control.call("AcquireName");

        test("exact CreateInputContext program and display options", [&] {
            control.configure();
            Harness h;
            h.ready(control);
            std::vector<std::string> options;
            for (const auto& event : control.events())
                if (event.rfind("option:", 0) == 0) options.push_back(event);
            std::sort(options.begin(), options.end());
            require(options == std::vector<std::string>{
                        "option:display=wayland:workspace-title-ime-test-0",
                        "option:program=hyprland-scroll-overview"},
                    "CreateInputContext options differ from exact program/display pair");
        });

        for (bool endInCallback : {false, true}) {
            test(endInCallback ? "direct fallback recursive poll and reentrant end" :
                                 "direct fallback recursive poll preserves queue order", [&] {
                control.configure("create-error");
                Harness h;
                h.client.begin();
                h.wait([&] { return !h.panels.empty(); }, "direct fallback state");
                const auto a = key(), b = key(98), c = key(98, true, 212);
                int depth = 0;
                h.onUnhandled = [&] {
                    require(++depth == 1, "recursive poll reentered fallback callback");
                    const auto count = h.fallback.size();
                    h.client.poll();
                    require(h.fallback.size() == count, "recursive poll drained fallback queue");
                    --depth;
                    if (endInCallback) h.client.end();
                };
                h.client.key(a);
                h.client.key(b);
                h.client.key(c);
                require(h.fallback.empty(), "key delivered direct fallback outside poll");
                h.pump(100ms);
                exactFallback(h, endInCallback ? std::vector<Key>{a} : std::vector<Key>{a, b, c});
                require(operations(control).empty(), "direct fallback unexpectedly sent keys");
            });
        }

        test("startup capabilities and unhandled snapshots", [&] {
            control.configure("unhandled");
            Harness h;
            h.ready(control);
            bool foundCapability = false;
            for (const auto& event : control.events()) {
                if (event.rfind("cap:", 0) != 0) continue;
                foundCapability = true;
                const auto caps = std::stoull(event.substr(4));
                require((caps & ((1ULL << 39) | (1ULL << 37))) ==
                        ((1ULL << 39) | (1ULL << 37)), "client-side UI capability bits");
                require(!(caps & (1ULL << 1)), "must not advertise Preedit capability");
            }
            require(foundCapability, "missing SetCapability");
            const auto press = key(), release = key(97, true, 102);
            h.client.key(press);
            h.client.key(release);
            h.wait([&] { return h.fallback.size() >= 2; }, "unhandled press/release");
            exactFallback(h, {press, release});
            require(operations(control) == std::vector<std::string>{wireKey(press), wireKey(release)},
                    "ProcessKeyEvent wire arguments/order");
        });

        test("handled and committed keys are not replayed", [&] {
            control.configure("commit");
            Harness h;
            h.ready(control);
            h.client.key(key());
            h.wait([&] { return !h.commits.empty(); }, "CommitString");
            h.pump(80ms);
            require(h.commits == std::vector<std::string>{"確定"}, "commit payload/count");
            exactFallback(h, {});
        });

        for (const char* mode : {"forward", "forward-false", "commit-forward", "forward-after-reply"}) {
            test(mode, [&] {
                control.configure(mode);
                Harness h;
                h.ready(control);
                const auto press = key(), release = key(97, true, 102);
                h.client.key(press);
                h.client.key(release);
                h.wait([&] { return h.fallback.size() >= 2; }, "ForwardKey snapshots");
                h.pump(100ms);
                exactFallback(h, {press, release});
                require(h.commits.size() == (std::string(mode) == "commit-forward" ? 2U : 0U),
                        "unexpected commits while forwarding");
            });
        }

        test("panel parsing and forged signal rejection", [&] {
            control.configure();
            Harness h;
            h.ready(control);
            // Establish an original snapshot an attacker might try to replay.
            h.client.key(key());
            h.wait([&] { return operations(control).size() == 1; }, "handled key");
            h.pump(50ms);
            const auto before = h.panels.size();
            control.emit("foreign");
            control.emit("wrong-path");
            h.pump(100ms);
            require(h.panels.size() == before && h.commits.empty() && h.fallback.empty(),
                    "accepted forged sender/path signal");
            control.emit("panel");
            h.wait([&] { return h.panels.size() > before; }, "authentic panel");
            checkPanel(h.panels.back());
        });

        test("malformed authentic IC signals are ignored", [&] {
            control.configure();
            Harness h;
            h.ready(control);
            h.client.key(key());
            h.wait([&] { return operations(control).size() == 1; }, "snapshot before malformed signals");
            control.emit("panel");
            h.wait([&] { return !h.panels.empty() && !empty(h.panels.back()); }, "baseline panel");
            const auto panels = h.panels.size();
            control.emit("malformed");
            h.pump(100ms);
            require(h.panels.size() == panels && h.commits.empty() && h.fallback.empty(),
                    "malformed authentic signal triggered callback");
            checkPanel(h.panels.back());
            control.emit("commit");
            h.wait([&] { return h.commits.size() == 1; }, "valid signal after malformed input");
            require(h.commits.front() == "確定", "malformed input damaged signal processing");
        });

        test("wrong serial and foreign sender replies cannot complete key", [&] {
            control.configure("spoof-reply", 0, 300);
            Harness h;
            h.ready(control);
            const auto a = key(), b = key(98);
            h.client.key(a);
            h.client.key(b);
            h.wait([&] { return operations(control).size() == 1; }, "key before forged replies");
            h.pump(100ms);
            require(h.fallback.empty() && operations(control).size() == 1,
                    "forged reply completed in-flight key");
            h.wait([&] { return operations(control).size() == 2; }, "genuine reply advances queue");
            h.pump(400ms);
            exactFallback(h, {});
            // The session bus may itself reject unsolicited method returns;
            // this is an end-to-end check, not proof of client filter coverage.
        });

        test("delayed creation queues keys without blocking", [&] {
            control.configure("unhandled", 220);
            Harness h;
            const auto start = Clock::now();
            h.client.begin();
            require(Clock::now() - start < 150ms, "begin blocked on CreateInputContext");
            const auto a = key(), b = key(98, true, 203);
            h.client.key(a);
            h.client.key(b);
            h.pump(100ms);
            require(h.fallback.empty() && operations(control).empty(), "keys escaped creation queue");
            h.wait([&] { return h.fallback.size() >= 2; }, "creation queue drain");
            exactFallback(h, {a, b});
        });

        for (const char* mode : {"create-error", "startup-error"}) {
            test(mode, [&] {
                control.configure(mode);
                Harness h;
                h.client.begin();
                const auto a = key(), b = key(98, true, 204);
                h.client.key(a);
                h.client.key(b);
                h.wait([&] { return h.fallback.size() >= 2; }, "startup failure fallback");
                exactFallback(h, {a, b});
                require(operations(control).empty(), "keys sent after failed startup");
            });
        }

        test("creation timeout ignores its late successful reply", [&] {
            control.configure("handled", 1450);
            Harness h;
            h.client.begin();
            const auto a = key(), b = key(98, true, 205);
            h.client.key(a);
            h.client.key(b);
            h.wait([&] { return h.fallback.size() >= 2; }, "creation deadline", 1350ms);
            exactFallback(h, {a, b});
            const auto count = h.panels.size();
            h.pump(600ms);
            require(operations(control).empty() && h.panels.size() == count,
                    "late creation revived expired session");
        });

        test("startup uses one total deadline", [&] {
            control.configure("handled", 650, 0, 650);
            Harness h;
            h.client.begin();
            const auto original = key();
            h.client.key(original);
            h.wait([&] { return !h.fallback.empty(); }, "total startup deadline", 1250ms);
            exactFallback(h, {original});
            require(operations(control).empty(), "startup exceeded total deadline and sent key");
            h.pump(500ms);
            exactFallback(h, {original});
        });

        test("keys and selections share one ordered queue", [&] {
            control.configure("unhandled", 0, 180, 0, 180);
            Harness h;
            h.ready(control);
            const auto a = key(), b = key(98, false, 206);
            h.client.key(a);
            h.client.select(1);
            h.client.key(b);
            h.wait([&] { return !operations(control).empty(); }, "first queued operation");
            h.pump(60ms);
            require(operations(control) == std::vector<std::string>{wireKey(a)},
                    "selection or second key overtook first reply");
            h.wait([&] { return operations(control).size() >= 2; }, "selection dispatch");
            h.pump(60ms);
            require(operations(control).size() == 2, "key overtook selection reply");
            h.wait([&] { return h.fallback.size() >= 2; }, "ordered queue drain");
            exactFallback(h, {a, b});
            require(operations(control) == std::vector<std::string>{wireKey(a), "select:1", wireKey(b)},
                    "selection/key ordering");
        });

        for (const char* mode : {"handled", "commit", "commit-before-timeout"}) {
            test((std::string("uncertain timeout drops sent key: ") + mode).c_str(), [&] {
                control.configure(mode, 0, 1450);
                Harness h;
                h.ready(control);
                control.emit("panel");
                h.wait([&] { return !h.panels.empty() && !empty(h.panels.back()); }, "initial panel");
                const auto sent = key(), a = key(98, false, 207), b = key(98, true, 208);
                h.client.key(sent);
                h.client.key(a);
                h.client.key(b);
                h.wait([&] { return h.fallback.size() >= 2; }, "key timeout fallback", 1350ms);
                exactFallback(h, {a, b});
                require(!h.panels.empty() && empty(h.panels.back()), "timeout did not clear panel");
                require(operations(control) == std::vector<std::string>{wireKey(sent)},
                        "queued keys sent after uncertain timeout");
                const auto panels = h.panels.size();
                h.pump(650ms);
                exactFallback(h, {a, b});
                const size_t expectedCommits = std::string(mode) == "commit-before-timeout" ? 1 : 0;
                require(h.commits.size() == expectedCommits && h.panels.size() == panels,
                        "late timeout reply/signal produced callbacks");
            });
        }

        test("selection timeout falls back queued unsent keys", [&] {
            control.configure("handled", 0, 0, 0, 1450);
            Harness h;
            h.ready(control);
            h.client.select(0);
            const auto original = key();
            h.client.key(original);
            h.wait([&] { return !h.fallback.empty(); }, "selection deadline", 1350ms);
            exactFallback(h, {original});
            require(operations(control) == std::vector<std::string>{"select:0"},
                    "key sent during stalled selection");
            h.pump(600ms);
            exactFallback(h, {original});
        });

        test("expired key reply buffered before poll is discarded", [&] {
            control.configure("unhandled", 0, 100);
            Harness h;
            h.ready(control);
            const auto sent = key(), queued = key(98, true, 209);
            h.client.key(sent);
            h.client.key(queued);
            h.wait([&] { return operations(control).size() == 1; }, "key before paused poll");
            std::this_thread::sleep_for(1100ms);
            h.wait([&] { return !h.fallback.empty(); }, "deadline before buffered reply");
            h.pump(80ms);
            exactFallback(h, {queued});
            require(operations(control).size() == 1, "late reply resumed key queue");
        });

        test("expired startup reply buffered before poll is discarded", [&] {
            control.configure("handled", 100);
            Harness h;
            h.client.begin();
            const auto original = key();
            h.client.key(original);
            h.wait([&] {
                const auto events = control.events();
                return std::find(events.begin(), events.end(), "create") != events.end();
            }, "create before paused poll");
            std::this_thread::sleep_for(1100ms);
            h.wait([&] { return !h.fallback.empty(); }, "startup deadline before buffered reply");
            exactFallback(h, {original});
            require(operations(control).empty(), "expired startup dispatched key");
        });

        test("key error drops uncertain key and forwards only unsent queue", [&] {
            control.configure("key-error", 0, 50);
            Harness h;
            h.ready(control);
            const auto sent = key(), a = key(98), b = key(98, true, 210);
            h.client.key(sent);
            h.client.key(a);
            h.client.key(b);
            h.wait([&] { return h.fallback.size() >= 2; }, "key error fallback");
            exactFallback(h, {a, b});
            require(operations(control).size() == 1, "key error sent queued keys");
        });

        test("service owner loss invalidates pending work", [&] {
            control.configure("commit", 0, 350);
            Harness h;
            h.ready(control);
            const auto sent = key(), queued = key(98);
            h.client.key(sent);
            h.client.key(queued);
            h.wait([&] { return operations(control).size() == 1; }, "key before owner loss");
            control.call("ReleaseName");
            h.wait([&] { return !h.fallback.empty(); }, "owner-loss fallback", 500ms);
            exactFallback(h, {queued});
            h.pump(450ms);
            require(h.commits.empty(), "old owner delivered late commit");
        });
        control.call("AcquireName");

        test("end during creation discards all queued snapshots", [&] {
            control.configure("unhandled", 250);
            Harness h;
            h.client.begin();
            h.client.key(key());
            h.pump(50ms);
            const auto count = h.panels.size();
            h.client.end();
            h.client.end();
            h.pump(350ms);
            require(h.panels.size() == count + 1 && empty(h.panels.back()),
                    "repeated end emitted extra empty panel");
            require(h.fallback.empty() && h.commits.empty() && operations(control).empty(),
                    "late creation revived ended session");
        });

        test("end discards pending work and clears panel once", [&] {
            control.configure("commit", 0, 250);
            Harness h;
            h.ready(control);
            control.emit("panel");
            h.wait([&] { return !h.panels.empty() && !empty(h.panels.back()); }, "panel before end");
            h.client.key(key());
            h.client.key(key(98));
            h.wait([&] { return !operations(control).empty(); }, "in-flight key before end");
            const auto count = h.panels.size();
            h.client.end();
            require(h.panels.size() == count + 1 && empty(h.panels.back()), "end must emit one empty panel");
            h.pump(400ms);
            require(h.commits.empty() && h.fallback.empty() && h.panels.size() == count + 1,
                    "callbacks survived end");
            require(operations(control).size() == 1, "end sent queued key");
            // Cleanup calls are best effort; their delivery is not required.
        });

        test("new session rejects signals from old context", [&] {
            control.configure();
            Harness h;
            h.ready(control);
            h.client.end();
            control.configure();
            h.ready(control);
            const auto count = h.panels.size();
            control.emit("stale");
            h.pump(100ms);
            require(h.commits.empty() && h.fallback.empty() && h.panels.size() == count,
                    "old-context signal entered new session");
            control.emit("commit");
            h.wait([&] { return h.commits.size() == 1; }, "new context commit");
        });

        for (const char* callback : {"commit", "unhandled", "panel"}) {
            test((std::string("reentrant end/begin from ") + callback).c_str(), [&] {
                control.configure(callback);
                Harness h;
                h.ready(control);
                bool restarted = false;
                auto restart = [&] {
                    if (restarted) return;
                    restarted = true; // Guard the empty panel emitted by end().
                    h.client.end();
                    h.client.begin();
                };
                if (std::string(callback) == "commit") h.onCommit = restart;
                if (std::string(callback) == "unhandled") h.onUnhandled = restart;
                if (std::string(callback) == "panel") h.onPanel = restart;
                h.client.key(key());
                h.client.key(key(98)); // Must be discarded when callback ends the session.
                h.wait([&] {
                    const auto events = control.events();
                    return restarted && std::count(events.begin(), events.end(), "focus-in") == 2;
                }, "reentrant new session");
                h.pump(100ms);
                require(operations(control).size() == 1, "old queued key survived callback restart");
                control.emit("commit");
                h.wait([&] { return h.commits.size() == (std::string(callback) == "commit" ? 2U : 1U); },
                       "commit after callback restart");
            });
        }

        test("failure-panel callback restart discards old fallback queue", [&] {
            control.configure("key-error", 0, 50);
            Harness h;
            h.ready(control);
            bool restarted = false;
            h.onPanel = [&] {
                if (restarted || !empty(h.panels.back())) return;
                restarted = true;
                h.client.end();
                h.client.begin();
            };
            h.client.key(key());
            h.client.key(key(98));
            h.wait([&] {
                const auto events = control.events();
                return restarted && std::count(events.begin(), events.end(), "focus-in") == 2;
            }, "restart from failure panel");
            h.pump(80ms);
            require(h.fallback.empty() && operations(control).size() == 1,
                    "failure callback restart retained old work");
        });

        std::cout << (failures ? "FAILED: " : "All transport tests passed; failures: ")
                  << failures << std::endl;
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "Transport test setup failed: " << error.what() << std::endl;
        return 2;
    }
}
