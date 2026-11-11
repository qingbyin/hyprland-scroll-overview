#include "WorkspaceTitleIME.hpp"

#include <dbus/dbus.h>

#include <chrono>
#include <cstdlib>
#include <deque>
#include <optional>
#include <utility>

namespace WorkspaceTitleIME {
namespace {
constexpr const char* service = "org.fcitx.Fcitx5";
constexpr const char* imPath = "/org/freedesktop/portal/inputmethod";
constexpr const char* imInterface = "org.fcitx.Fcitx.InputMethod1";
constexpr const char* icInterface = "org.fcitx.Fcitx.InputContext1";
constexpr dbus_uint64_t capabilities = (1ULL << 39) | (1ULL << 37);
constexpr auto timeout = std::chrono::milliseconds(1000);
constexpr unsigned pollBudget = 256;
constexpr unsigned historyLimit = 64;
using Clock = std::chrono::steady_clock;
using Message = std::unique_ptr<DBusMessage, decltype(&dbus_message_unref)>;

Message message(DBusMessage* raw) {
    return Message(raw, &dbus_message_unref);
}

bool equal(const char* value, const char* expected) {
    return value && std::string(value) == expected;
}

// The complete message signature is checked before any iterator is used.
template <typename T> T basic(DBusMessageIter& iter) {
    T value{};
    dbus_message_iter_get_basic(&iter, &value);
    dbus_message_iter_next(&iter);
    return value;
}

std::string formatted(DBusMessageIter& iter) {
    DBusMessageIter array;
    dbus_message_iter_recurse(&iter, &array);
    std::string result;
    while (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_INVALID) {
        DBusMessageIter item;
        dbus_message_iter_recurse(&array, &item);
        result += basic<const char*>(item);
        // Formatting attributes are intentionally not exposed by this API.
        dbus_message_iter_next(&array);
    }
    dbus_message_iter_next(&iter);
    return result;
}

Panel parsePanel(DBusMessage* msg) {
    Panel result;
    DBusMessageIter iter;
    dbus_message_iter_init(msg, &iter);
    result.preedit = formatted(iter);
    result.cursor = basic<dbus_int32_t>(iter);
    result.auxiliary = formatted(iter);
    const auto lower = formatted(iter);
    if (!lower.empty()) {
        if (!result.auxiliary.empty())
            result.auxiliary += '\n';
        result.auxiliary += lower;
    }
    DBusMessageIter array;
    dbus_message_iter_recurse(&iter, &array);
    while (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_INVALID) {
        DBusMessageIter item;
        dbus_message_iter_recurse(&array, &item);
        Candidate candidate;
        candidate.label = basic<const char*>(item);
        candidate.text = basic<const char*>(item);
        result.candidates.push_back(std::move(candidate));
        dbus_message_iter_next(&array);
    }
    dbus_message_iter_next(&iter);
    result.selected = basic<dbus_int32_t>(iter);
    basic<dbus_int32_t>(iter); // Layout hint: the parent chooses its layout.
    result.hasPrevious = basic<dbus_bool_t>(iter);
    result.hasNext = basic<dbus_bool_t>(iter);
    return result;
}
} // namespace

struct Client::Impl {
    enum class State { Inactive, Starting, Ready, Fallback };
    enum class Call { None, Match, Owner, Create, Capability, Focus, Key, Select };
    struct Action {
        std::optional<Key> key;
        int selection = -1;
    };
    struct Snapshot {
        Key key;
        bool delivered = false;
    };

    std::function<void(const std::string&)> commitCallback;
    std::function<void(const Key&)> unhandledCallback;
    std::function<void(const Panel&)> panelCallback;
    DBusConnection* connection = nullptr;
    State state = State::Inactive;
    Call pending = Call::None;
    dbus_uint32_t serial = 0;
    Clock::time_point deadline{};
    uint64_t generation = 0;
    bool polling = false;
    std::string owner, path;
    std::deque<Action> actions;
    std::optional<Snapshot> current;
    std::deque<Snapshot> history;

    Impl(std::function<void(const std::string&)> commit,
         std::function<void(const Key&)> unhandled,
         std::function<void(const Panel&)> panel)
        : commitCallback(std::move(commit)), unhandledCallback(std::move(unhandled)),
          panelCallback(std::move(panel)) {}

    ~Impl() {
        // No user callbacks during destruction/plugin unload.
        close();
    }

    Message method(const char* destination, const char* object,
                   const char* interface, const char* member) {
        auto msg = message(dbus_message_new_method_call(destination, object, interface, member));
        if (msg)
            dbus_message_set_auto_start(msg.get(), false);
        return msg;
    }

    Message contextMethod(const char* member) {
        return method(owner.c_str(), path.c_str(), icInterface, member);
    }

    void clearPanel() {
        const Panel empty;
        if (panelCallback)
            panelCallback(empty);
    }

    void close() {
        if (connection) {
            if (dbus_connection_get_is_connected(connection) && !path.empty()) {
                // Best effort only. Closing the private connection also lets
                // Fcitx's client-owner watcher destroy a context whose creation
                // reply was still pending. Never wait for cleanup replies.
                for (const char* member : {"Reset", "FocusOut", "DestroyIC"}) {
                    auto msg = contextMethod(member);
                    if (msg) {
                        dbus_message_set_no_reply(msg.get(), true);
                        dbus_connection_send(connection, msg.get(), nullptr);
                    }
                }
                dbus_connection_read_write(connection, 0);
            }
            dbus_connection_close(connection);
            dbus_connection_unref(connection);
            connection = nullptr;
        }
        owner.clear();
        path.clear();
        pending = Call::None;
        serial = 0;
        current.reset();
        history.clear();
    }

    void end() {
        const bool active = state != State::Inactive;
        ++generation;
        state = State::Inactive;
        actions.clear();
        close();
        // Change all state before invoking user code. Nested end() is silent.
        if (active)
            clearPanel();
    }

    void fail() {
        // Keep unsent actions. In-flight keys and all composition state are
        // discarded, never retried, even when no CommitString was observed.
        state = State::Fallback;
        close();
        clearPanel();
    }

    bool send(Message msg, Call call) {
        if (!msg || !connection || !dbus_connection_send(connection, msg.get(), &serial)) {
            fail();
            return false;
        }
        pending = call;
        // Creation (including owner lookup, capability and focus) has one
        // overall deadline, not a fresh second for each setup round trip.
        if (state != State::Starting)
            deadline = Clock::now() + timeout;
        return true;
    }

    void begin() {
        const auto previous = generation;
        end();
        if (generation != previous + 1)
            return; // An end-panel callback started or ended another session.
        state = State::Starting;
        DBusError error;
        dbus_error_init(&error);
        connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
        dbus_error_free(&error);
        if (!connection) {
            fail();
            return;
        }
        dbus_connection_set_exit_on_disconnect(connection, false);
        deadline = Clock::now() + timeout;

        // Do not use dbus_bus_add_match/get_name_owner: those helpers wait.
        // Fcitx's context signals are unicast and need no signal match rule.
        auto msg = method(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "AddMatch");
        const char* rule = "type='signal',sender='org.freedesktop.DBus',"
                           "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
                           "path='/org/freedesktop/DBus',arg0='org.fcitx.Fcitx5'";
        if (!msg || !dbus_message_append_args(msg.get(), DBUS_TYPE_STRING, &rule, DBUS_TYPE_INVALID)) {
            fail();
            return;
        }
        send(std::move(msg), Call::Match);
    }

    void getOwner() {
        auto msg = method(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetNameOwner");
        const char* name = service;
        if (!msg || !dbus_message_append_args(msg.get(), DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID)) {
            fail();
            return;
        }
        send(std::move(msg), Call::Owner);
    }

    void create() {
        auto msg = method(owner.c_str(), imPath, imInterface, "CreateInputContext");
        if (!msg) {
            fail();
            return;
        }
        DBusMessageIter iter, array;
        dbus_message_iter_init_append(msg.get(), &iter);
        if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(ss)", &array)) {
            fail();
            return;
        }
        const char* displayEnv = std::getenv("WAYLAND_DISPLAY");
        const std::string display = std::string("wayland:") + (displayEnv ? displayEnv : "");
        const auto appendPair = [&](const char* name, const char* value) {
            DBusMessageIter item;
            if (!dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT, nullptr, &item))
                return false;
            if (!dbus_message_iter_append_basic(&item, DBUS_TYPE_STRING, &name) ||
                !dbus_message_iter_append_basic(&item, DBUS_TYPE_STRING, &value)) {
                dbus_message_iter_abandon_container(&array, &item);
                return false;
            }
            return bool(dbus_message_iter_close_container(&array, &item));
        };
        if (!appendPair("program", "hyprland-scroll-overview") ||
            !appendPair("display", display.c_str())) {
            dbus_message_iter_abandon_container(&iter, &array);
            fail();
            return;
        }
        if (!dbus_message_iter_close_container(&iter, &array)) {
            fail();
            return;
        }
        send(std::move(msg), Call::Create);
    }

    void setCapability() {
        auto msg = contextMethod("SetCapability");
        dbus_uint64_t value = capabilities;
        if (!msg || !dbus_message_append_args(msg.get(), DBUS_TYPE_UINT64, &value, DBUS_TYPE_INVALID)) {
            fail();
            return;
        }
        send(std::move(msg), Call::Capability);
    }

    void deliver(Snapshot& snapshot) {
        if (snapshot.delivered)
            return;
        snapshot.delivered = true;
        // The callback can clear current/history/actions by calling end().
        // Neither its argument nor the calling stack retains a member reference.
        const Key original = snapshot.key;
        if (unhandledCallback)
            unhandledCallback(original);
    }

    void reply(DBusMessage* msg) {
        if (pending == Call::None || dbus_message_get_reply_serial(msg) != serial)
            return;
        const char* sender = dbus_message_get_sender(msg);
        const bool busCall = pending == Call::Match || pending == Call::Owner;
        const bool fromBus = equal(sender, DBUS_SERVICE_DBUS);
        if (!(busCall ? fromBus : equal(sender, owner.c_str())) &&
            !(dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_ERROR && fromBus))
            return;
        if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_ERROR) {
            fail();
            return;
        }
        const Call call = pending;
        pending = Call::None;
        serial = 0;
        const char* signature = "";
        if (call == Call::Owner)
            signature = "s";
        else if (call == Call::Create)
            signature = "oay";
        else if (call == Call::Key)
            signature = "b";
        if (!dbus_message_has_signature(msg, signature)) {
            fail();
            return;
        }
        DBusMessageIter iter;
        dbus_message_iter_init(msg, &iter);
        switch (call) {
            case Call::Match:
                getOwner();
                break;
            case Call::Owner:
                owner = basic<const char*>(iter);
                if (owner.empty() || owner.front() != ':' || !dbus_validate_bus_name(owner.c_str(), nullptr)) {
                    fail();
                    return;
                }
                create();
                break;
            case Call::Create:
                path = basic<const char*>(iter);
                setCapability();
                break;
            case Call::Capability:
                send(contextMethod("FocusIn"), Call::Focus);
                break;
            case Call::Focus:
                state = State::Ready;
                break;
            case Call::Key:
                if (!basic<dbus_bool_t>(iter) && current)
                    deliver(*current);
                break;
            case Call::Select:
            case Call::None:
                break;
        }
    }

    void forward(DBusMessage* msg) {
        DBusMessageIter iter;
        dbus_message_iter_init(msg, &iter);
        const auto sym = basic<dbus_uint32_t>(iter);
        const auto modifiers = basic<dbus_uint32_t>(iter);
        const bool release = basic<dbus_bool_t>(iter);
        const auto matches = [&](const Snapshot& item) {
            return item.key.sym == sym && item.key.modifiers == modifiers && item.key.release == release;
        };
        if (current && matches(*current)) {
            deliver(*current);
            return;
        }
        for (auto item = history.rbegin(); item != history.rend(); ++item) {
            if (matches(*item)) {
                deliver(*item);
                return;
            }
        }
        // ForwardKey has neither keycode, time nor text. Do not invent text
        // from live keyboard state for synthetic or unmatchable forwarded keys.
    }

    void signal(DBusMessage* msg) {
        if (dbus_message_is_signal(msg, DBUS_INTERFACE_DBUS, "NameOwnerChanged") &&
            equal(dbus_message_get_sender(msg), DBUS_SERVICE_DBUS) &&
            equal(dbus_message_get_path(msg), DBUS_PATH_DBUS) &&
            dbus_message_has_signature(msg, "sss")) {
            DBusMessageIter iter;
            dbus_message_iter_init(msg, &iter);
            const auto name = basic<const char*>(iter);
            const auto oldOwner = basic<const char*>(iter);
            const auto newOwner = basic<const char*>(iter);
            if (equal(name, service) && !owner.empty() && owner == oldOwner && owner != newOwner)
                fail();
            return;
        }
        if (path.empty() || owner.empty() ||
            !equal(dbus_message_get_sender(msg), owner.c_str()) ||
            !equal(dbus_message_get_path(msg), path.c_str()) ||
            !equal(dbus_message_get_interface(msg), icInterface))
            return;
        if (dbus_message_is_signal(msg, icInterface, "CommitString") && dbus_message_has_signature(msg, "s")) {
            DBusMessageIter iter;
            dbus_message_iter_init(msg, &iter);
            const std::string text = basic<const char*>(iter);
            if (commitCallback)
                commitCallback(text);
        } else if (dbus_message_is_signal(msg, icInterface, "UpdateClientSideUI") &&
                   dbus_message_has_signature(msg, "a(si)ia(si)a(si)a(ss)iibb")) {
            const Panel panel = parsePanel(msg);
            if (panelCallback)
                panelCallback(panel);
        } else if (dbus_message_is_signal(msg, icInterface, "ForwardKey") &&
                   dbus_message_has_signature(msg, "uub")) {
            forward(msg);
        }
    }

    void drive() {
        const auto session = generation;
        for (unsigned count = 0; count < pollBudget && !actions.empty(); ++count) {
            if (state == State::Inactive || state == State::Starting || pending != Call::None)
                return;
            Action action = std::move(actions.front());
            actions.pop_front();
            if (state == State::Fallback) {
                if (action.key && unhandledCallback)
                    unhandledCallback(*action.key);
                if (generation != session)
                    return;
                continue;
            }
            if (current) {
                history.push_back(std::move(*current));
                current.reset();
                if (history.size() > historyLimit)
                    history.pop_front();
            }
            if (action.key) {
                current = Snapshot{std::move(*action.key), false};
                const auto& key = current->key;
                auto msg = contextMethod("ProcessKeyEvent");
                dbus_uint32_t sym = key.sym, code = key.code, modifiers = key.modifiers, time = key.time;
                dbus_bool_t release = key.release;
                if (!msg || !dbus_message_append_args(msg.get(), DBUS_TYPE_UINT32, &sym,
                                                     DBUS_TYPE_UINT32, &code, DBUS_TYPE_UINT32, &modifiers,
                                                     DBUS_TYPE_BOOLEAN, &release, DBUS_TYPE_UINT32, &time,
                                                     DBUS_TYPE_INVALID)) {
                    // This key was definitely not sent. Preserve its snapshot.
                    actions.push_front({std::move(current->key), -1});
                    fail();
                    return;
                }
                // If send fails, libdbus did not queue the message either.
                // Preserve the key before send() can invoke a reentrant callback.
                Action unsent{current->key, -1};
                dbus_uint32_t nextSerial = 0;
                if (!dbus_connection_send(connection, msg.get(), &nextSerial)) {
                    actions.push_front(std::move(unsent));
                    fail();
                    return;
                }
                serial = nextSerial;
                pending = Call::Key;
                deadline = Clock::now() + timeout;
            } else {
                auto msg = contextMethod("SelectCandidate");
                dbus_int32_t index = action.selection;
                if (!msg || !dbus_message_append_args(msg.get(), DBUS_TYPE_INT32, &index, DBUS_TYPE_INVALID)) {
                    fail();
                    return;
                }
                send(std::move(msg), Call::Select);
            }
            return; // Exactly one key/selection may be in flight.
        }
    }

    void poll() {
        if (polling || state == State::Inactive)
            return;
        polling = true;
        struct PollGuard {
            bool& flag;
            ~PollGuard() { flag = false; }
        } guard{polling};
        const auto session = generation;
        // A timer that ran late must not revive an expired operation just
        // because its reply has since reached the socket.
        if (connection && pending != Call::None && Clock::now() >= deadline) {
            fail();
            if (generation != session)
                return;
        }
        if (connection) {
            // read_write does not dispatch callbacks. pop_message preserves
            // replies AND signals in wire order (including KeyEventOrderFix's
            // signals sent immediately after a ProcessKeyEvent reply).
            dbus_connection_read_write(connection, 0);
            unsigned count = 0;
            for (; count < pollBudget; ++count) {
                auto msg = message(dbus_connection_pop_message(connection));
                if (!msg)
                    break;
                const int type = dbus_message_get_type(msg.get());
                if (type == DBUS_MESSAGE_TYPE_METHOD_RETURN || type == DBUS_MESSAGE_TYPE_ERROR)
                    reply(msg.get());
                else if (type == DBUS_MESSAGE_TYPE_SIGNAL)
                    signal(msg.get());
                if (generation != session)
                    return;
                if (!connection)
                    break;
            }
            if (connection && (!dbus_connection_get_is_connected(connection) ||
                               (pending != Call::None && Clock::now() >= deadline))) {
                fail();
                if (generation != session)
                    return;
            }
            // Drain already-received signals before sending another key. This
            // also bounds compositor work when a peer floods the connection.
            if (connection && count == pollBudget)
                return;
        }
        drive();
    }
};

Client::Client(std::function<void(const std::string&)> commit,
               std::function<void(const Key&)> unhandled,
               std::function<void(const Panel&)> panel)
    : m_impl(std::make_unique<Impl>(std::move(commit), std::move(unhandled), std::move(panel))) {}
Client::~Client() = default;
void Client::begin() { m_impl->begin(); }
void Client::end() { m_impl->end(); }
void Client::key(Key key) {
    if (m_impl->state != Impl::State::Inactive)
        m_impl->actions.push_back({std::move(key), -1});
}
void Client::poll() { m_impl->poll(); }
void Client::select(int index) {
    if (index >= 0 && m_impl->state != Impl::State::Inactive)
        m_impl->actions.push_back({std::nullopt, index});
}

} // namespace WorkspaceTitleIME
